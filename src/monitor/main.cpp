// SPDX-License-Identifier: Apache-2.0
//
// socksdirect-monitor — production daemon (Phase 2 of the rewrite).
//
// Responsibilities:
//   - Load configuration via socksdirect::Config.
//   - Initialize the structured Logger; rotate via SIGHUP.
//   - Open the control-plane Unix socket and serve socksdirect-ctl
//     (status, connections, dump-state, dump-config, reload, drain,
//     metrics).
//   - Track a small set of operational metrics in MetricsRegistry.
//   - Handle SIGTERM / SIGINT for graceful shutdown.
//   - Optional sd_notify(READY=1) when systemd's Type=notify is in use
//     (compile-out gracefully when libsystemd isn't linked).
//
// Things this daemon does NOT do yet (deferred to Phase 3):
//   - Actually accept libsd preloaded clients on the data plane. The
//     legacy monitor at monitor/main.cpp still owns that path during
//     the migration; the client-listener side of this new daemon is a
//     skeleton with the right control-plane wiring, ready to absorb
//     the data-plane logic when Phase 3 lands.
//
// Concurrency: single-threaded poll() loop. Control-plane traffic is
// low-rate; threading would only add bug surface.

#include "socksdirect/config.hpp"
#include "socksdirect/log.hpp"
#include "socksdirect/metrics.hpp"
#include "socksdirect/monitor_ipc.hpp"
#include "socksdirect/shm_handshake.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <memory>
#include <poll.h>
#include <string>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_set>
#include <vector>

namespace sd = socksdirect;

namespace {

// ---------------------------------------------------------------------------
// State shared across the event loop. One instance per daemon process.
// ---------------------------------------------------------------------------
struct Daemon {
    sd::Config config;
    std::string control_socket_path;
    std::string pid_file_path;
    int control_fd = -1;
    int signal_fd  = -1;
    bool draining  = false;
    std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();

    // Per-connection ctl state. Keys are fds; values hold accumulated
    // bytes (NDJSON requests are line-delimited).
    struct CtlConn {
        std::string buf;
    };
    std::map<int, CtlConn> ctl_conns;

    // SHM intra-host pair registry (Phase 3 scaffold). The full ring
    // semantics live in src/lib/; the monitor only brokers keys.
    sd::ShmHandshakeRegistry shm_registry;

    // Cached counter handles to avoid map lookups in the hot path.
    sd::Counter* m_ctl_requests = nullptr;
    sd::Counter* m_ctl_errors   = nullptr;
    sd::Gauge*   m_ctl_active   = nullptr;
};

// Notify systemd if we're under Type=notify. We avoid linking libsystemd
// by speaking the protocol manually: write a NOTIFY_SOCKET datagram with
// "READY=1\nSTATUS=...\n". If $NOTIFY_SOCKET is unset we silently skip.
void sd_notify_ready(const std::string& status_msg = "") {
    const char* sock = std::getenv("NOTIFY_SOCKET");
    if (!sock || !*sock) return;
    int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    // Abstract socket support: a leading '@' becomes a NUL.
    if (sock[0] == '@') {
        a.sun_path[0] = '\0';
        std::strncpy(a.sun_path + 1, sock + 1, sizeof(a.sun_path) - 2);
    } else {
        std::strncpy(a.sun_path, sock, sizeof(a.sun_path) - 1);
    }
    std::string body = "READY=1\n";
    if (!status_msg.empty()) body += "STATUS=" + status_msg + "\n";
    ::sendto(fd, body.data(), body.size(), 0,
             reinterpret_cast<sockaddr*>(&a), sizeof(a));
    ::close(fd);
}

void sd_notify_stopping() {
    const char* sock = std::getenv("NOTIFY_SOCKET");
    if (!sock || !*sock) return;
    int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    if (sock[0] == '@') {
        a.sun_path[0] = '\0';
        std::strncpy(a.sun_path + 1, sock + 1, sizeof(a.sun_path) - 2);
    } else {
        std::strncpy(a.sun_path, sock, sizeof(a.sun_path) - 1);
    }
    const char body[] = "STOPPING=1\n";
    ::sendto(fd, body, sizeof(body) - 1, 0,
             reinterpret_cast<sockaddr*>(&a), sizeof(a));
    ::close(fd);
}

// ---------------------------------------------------------------------------
// PID file
// ---------------------------------------------------------------------------
bool write_pid_file(const std::string& path) {
    if (path.empty()) return true;
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        LOG_WARN("cannot open pid file %s: %s", path.c_str(), std::strerror(errno));
        return false;
    }
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "%d\n", static_cast<int>(getpid()));
    bool ok = ::write(fd, buf, n) == n;
    ::close(fd);
    return ok;
}

// ---------------------------------------------------------------------------
// Op dispatch
// ---------------------------------------------------------------------------
sd::CtlResponse op_status(const Daemon& d, const sd::CtlRequest&) {
    sd::CtlResponse r;
    r.ok = true;
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - d.start_time).count();
    char buf[256];
    std::snprintf(buf, sizeof(buf), "pid %d", static_cast<int>(getpid()));
    r.lines.push_back(buf);
    std::snprintf(buf, sizeof(buf), "uptime_sec %lld", static_cast<long long>(uptime));
    r.lines.push_back(buf);
    r.lines.push_back(std::string("control_socket ") + d.control_socket_path);
    r.lines.push_back(std::string("draining ") + (d.draining ? "true" : "false"));
    return r;
}

sd::CtlResponse op_connections(const Daemon& d, const sd::CtlRequest&) {
    sd::CtlResponse r;
    r.ok = true;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "ctl_active %zu", d.ctl_conns.size());
    r.lines.push_back(buf);
    // The libsd data-plane connections aren't tracked in this skeleton;
    // they'll show up here when Phase 3 wires the client-listener in.
    r.lines.push_back("data_plane_connections 0  (data plane not yet active in src/monitor)");
    return r;
}

sd::CtlResponse op_dump_state(Daemon& d, const sd::CtlRequest&) {
    sd::CtlResponse r;
    r.ok = true;
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - d.start_time).count();
    char buf[256];
    std::snprintf(buf, sizeof(buf), "pid=%d uptime=%llds draining=%s",
                  static_cast<int>(getpid()),
                  static_cast<long long>(uptime),
                  d.draining ? "true" : "false");
    r.lines.push_back(buf);
    r.lines.push_back(std::string("control_socket=") + d.control_socket_path);
    r.lines.push_back(std::string("pid_file=") + d.pid_file_path);
    std::snprintf(buf, sizeof(buf), "ctl_requests=%.0f errors=%.0f",
                  d.m_ctl_requests->value(), d.m_ctl_errors->value());
    r.lines.push_back(buf);
    // Dump every config key with the source we resolved it from.
    for (const auto& kv : d.config.all()) {
        r.lines.push_back("config " + kv.first + "=" + kv.second);
    }
    return r;
}

sd::CtlResponse op_dump_config(const Daemon& d, const sd::CtlRequest&) {
    sd::CtlResponse r;
    r.ok = true;
    for (const auto& kv : d.config.all()) {
        r.lines.push_back(kv.first + "=" + kv.second);
    }
    if (r.lines.empty()) {
        r.lines.push_back("(no config file loaded; all values defaulted)");
    }
    return r;
}

sd::CtlResponse op_metrics(const Daemon&, const sd::CtlRequest&) {
    sd::CtlResponse r;
    r.ok = true;
    std::string text = sd::MetricsRegistry::instance().render();
    // Split into lines so the CtlResponse list maps 1:1 to scrape format.
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t eol = text.find('\n', start);
        if (eol == std::string::npos) eol = text.size();
        if (eol > start) r.lines.emplace_back(text.substr(start, eol - start));
        start = eol + 1;
    }
    return r;
}

sd::CtlResponse op_reload(Daemon& d, const sd::CtlRequest&) {
    sd::CtlResponse r;
    // Reload config from the same path; reset log level if it changed.
    d.config = sd::Config::load_default();
    auto level = sd::parse_level(
        d.config.get_string("monitor", "log_level", "info").c_str(),
        sd::kLogInfo);
    sd::Logger::instance().set_level(level);
    auto sink = d.config.get_string("monitor", "log_file", "");
    if (!sink.empty()) sd::Logger::instance().set_sink_file(sink);
    LOG_INFO("reloaded config; level=%s sink=%s",
             sd::level_name(level),
             sink.empty() ? "stderr" : sink.c_str());
    r.ok = true;
    r.lines.push_back("reloaded");
    return r;
}

sd::CtlResponse op_drain(Daemon& d, const sd::CtlRequest&) {
    sd::CtlResponse r;
    d.draining = true;
    LOG_INFO("draining: no new ctl clients accepted");
    r.ok = true;
    r.lines.push_back("draining");
    return r;
}

sd::CtlResponse op_ping(const Daemon&, const sd::CtlRequest& req) {
    sd::CtlResponse r;
    r.ok = true;
    r.lines.push_back(req.args.empty() ? std::string("pong") : req.args[0]);
    return r;
}

sd::CtlResponse op_help(const Daemon&, const sd::CtlRequest&) {
    sd::CtlResponse r;
    r.ok = true;
    r.lines.push_back("status         daemon pid / uptime / control socket / drain flag");
    r.lines.push_back("connections    counts of active connections");
    r.lines.push_back("dump-state     verbose internal state");
    r.lines.push_back("dump-config    every resolved config key=value");
    r.lines.push_back("metrics        Prometheus text format");
    r.lines.push_back("reload         re-read config; reopen log sink");
    r.lines.push_back("drain          stop accepting new ctl clients");
    r.lines.push_back("ping [msg]     echo (defaults to 'pong')");
    r.lines.push_back("shm-register A B PID   broker SHM key for the (A,B) pair");
    r.lines.push_back("shm-unregister A B     drop the SHM key for (A,B)");
    r.lines.push_back("help           this list");
    return r;
}

// shm-register <endpoint_a> <endpoint_b> <pid> — Phase 3 scaffold
// for the SHM intra-host fast path. Both peers call this with the
// same canonical (a,b) pair; the monitor returns a shared key and
// indicates whether the caller is the creator or joiner. The actual
// SHM ring is allocated + mapped by libsd using the returned key.
sd::CtlResponse op_shm_register(Daemon& d, const sd::CtlRequest& req) {
    sd::CtlResponse r;
    if (req.args.size() < 3) {
        r.ok = false;
        r.error = "usage: shm-register <endpoint_a> <endpoint_b> <pid>";
        return r;
    }
    int pid = std::atoi(req.args[2].c_str());
    if (pid <= 0) {
        r.ok = false;
        r.error = "shm-register: bad pid";
        return r;
    }
    auto id = sd::ShmHandshakeRegistry::make(req.args[0], req.args[1]);
    auto res = d.shm_registry.register_endpoint(id, pid);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "shm_key=%llu",
                  static_cast<unsigned long long>(res.key));
    r.lines.push_back(buf);
    r.lines.push_back(res.role == sd::ShmHandshakeRegistry::Role::kCreator
                          ? "role=creator" : "role=joiner");
    std::snprintf(buf, sizeof(buf), "pid_a=%d pid_b=%d",
                  res.pid_a, res.pid_b);
    r.lines.push_back(buf);
    r.ok = true;
    return r;
}

sd::CtlResponse op_shm_unregister(Daemon& d, const sd::CtlRequest& req) {
    sd::CtlResponse r;
    if (req.args.size() < 2) {
        r.ok = false;
        r.error = "usage: shm-unregister <endpoint_a> <endpoint_b>";
        return r;
    }
    auto id = sd::ShmHandshakeRegistry::make(req.args[0], req.args[1]);
    bool removed = d.shm_registry.unregister_endpoint(id);
    r.ok = true;
    r.lines.push_back(removed ? "removed" : "not_present");
    return r;
}

// lib-metrics: aggregate per-pid Prometheus-text snapshots written
// by libsd-preloaded processes into the configured lib_metrics_dir.
// Stale files (pid no longer alive) are scrubbed during the scan.
sd::CtlResponse op_lib_metrics(Daemon& d, const sd::CtlRequest&) {
    sd::CtlResponse r;
    std::string dir = d.config.get_string(
        "monitor", "lib_metrics_dir",
        "/run/socksdirect/lib-metrics");
    DIR* dp = ::opendir(dir.c_str());
    if (!dp) {
        r.ok = true;
        r.lines.push_back("# no lib-metrics directory: " + dir);
        return r;
    }
    struct dirent* de;
    while ((de = ::readdir(dp)) != nullptr) {
        std::string name = de->d_name;
        if (name.size() < 6 || name.substr(name.size() - 5) != ".prom") continue;
        std::string pid_str = name.substr(0, name.size() - 5);
        char* end = nullptr;
        long pid = std::strtol(pid_str.c_str(), &end, 10);
        std::string path = dir + "/" + name;
        // Always emit the file's contents (the dead-pid snapshot
        // is still useful — it's the final state of that process).
        // Then, if the pid is no longer alive, unlink so the dir
        // doesn't accumulate stale snapshots indefinitely.
        FILE* f = std::fopen(path.c_str(), "r");
        if (f) {
            char buf[1024];
            while (std::fgets(buf, sizeof(buf), f)) {
                std::string line(buf);
                if (!line.empty() && line.back() == '\n') line.pop_back();
                r.lines.push_back(line);
            }
            std::fclose(f);
        }
        if (pid > 0 && end && *end == '\0'
            && ::kill(static_cast<pid_t>(pid), 0) < 0 && errno == ESRCH) {
            ::unlink(path.c_str());
        }
    }
    ::closedir(dp);
    r.ok = true;
    return r;
}

sd::CtlResponse dispatch(Daemon& d, const sd::CtlRequest& req) {
    d.m_ctl_requests->inc();
    if (req.op == "status")      return op_status(d, req);
    if (req.op == "connections") return op_connections(d, req);
    if (req.op == "dump-state")  return op_dump_state(d, req);
    if (req.op == "dump-config") return op_dump_config(d, req);
    if (req.op == "metrics")     return op_metrics(d, req);
    if (req.op == "reload")      return op_reload(d, req);
    if (req.op == "drain")       return op_drain(d, req);
    if (req.op == "ping")           return op_ping(d, req);
    if (req.op == "help")           return op_help(d, req);
    if (req.op == "shm-register")   return op_shm_register(d, req);
    if (req.op == "shm-unregister") return op_shm_unregister(d, req);
    if (req.op == "lib-metrics")    return op_lib_metrics(d, req);
    sd::CtlResponse r;
    r.ok = false;
    r.error = std::string("unknown op: ") + req.op + " (try 'help')";
    d.m_ctl_errors->inc();
    return r;
}

// ---------------------------------------------------------------------------
// Per-connection I/O. Reads bytes into the connection's buffer, splits on
// '\n', dispatches each request, writes the response. Returns false when
// the connection should be closed.
// ---------------------------------------------------------------------------
bool drain_ctl_conn(Daemon& d, int fd) {
    auto& conn = d.ctl_conns[fd];
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;  // peer closed
        conn.buf.append(buf, buf + n);
        // Cap accumulated buffer to defend against a wedged client.
        if (conn.buf.size() > (1u << 20)) {
            LOG_WARN("ctl client exceeded 1 MiB buffer; closing");
            return false;
        }
        for (;;) {
            std::size_t eol = conn.buf.find('\n');
            if (eol == std::string::npos) break;
            std::string line = conn.buf.substr(0, eol);
            conn.buf.erase(0, eol + 1);
            sd::CtlRequest req;
            sd::CtlResponse resp;
            if (!sd::decode_request(line, req)) {
                resp.ok = false;
                resp.error = "malformed request";
                d.m_ctl_errors->inc();
            } else {
                resp = dispatch(d, req);
            }
            if (!sd::write_all(fd, sd::encode_response(resp))) return false;
        }
    }
}

void close_ctl_conn(Daemon& d, int fd, std::vector<pollfd>* polls) {
    ::close(fd);
    d.ctl_conns.erase(fd);
    if (polls) {
        for (auto it = polls->begin(); it != polls->end(); ++it) {
            if (it->fd == fd) { polls->erase(it); break; }
        }
    }
    d.m_ctl_active->set(static_cast<double>(d.ctl_conns.size()));
}

// ---------------------------------------------------------------------------
// Signal handling via signalfd (so we don't race the poll loop).
// ---------------------------------------------------------------------------
int setup_signalfd() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGPIPE);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    int fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (fd < 0) {
        LOG_ERROR("signalfd failed: %s", std::strerror(errno));
    }
    return fd;
}

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------
struct Args {
    std::string config_path;
    std::string control_socket;
    std::string pid_file;
    std::string log_file;
    std::string log_level;
    bool foreground = true;  // Type=notify systemd doesn't fork
    bool show_help = false;
};

void print_usage(FILE* out) {
    std::fprintf(out,
        "usage: socksdirect-monitor [options]\n"
        "\n"
        "  --config PATH           override $SOCKSDIRECT_CONFIG / default\n"
        "  --control-socket PATH   override [monitor].control_socket\n"
        "  --pid-file PATH         write our pid to PATH (and unlink at exit)\n"
        "  --log-file PATH         override [monitor].log_file\n"
        "  --log-level LEVEL       trace|debug|info|warn|error (default info)\n"
        "  --help                  print this help\n"
        "\n"
        "Use socksdirect-ctl(1) to talk to a running daemon.\n");
}

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", s.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (s == "--help" || s == "-h") { a.show_help = true; }
        else if (s == "--config")          { a.config_path    = next(); }
        else if (s == "--control-socket")  { a.control_socket = next(); }
        else if (s == "--pid-file")        { a.pid_file       = next(); }
        else if (s == "--log-file")        { a.log_file       = next(); }
        else if (s == "--log-level")       { a.log_level      = next(); }
        else {
            std::fprintf(stderr, "unknown argument: %s\n", s.c_str());
            print_usage(stderr);
            std::exit(2);
        }
    }
    return a;
}

}  // namespace

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);
    if (args.show_help) { print_usage(stdout); return 0; }

    Daemon d;

    // Config: --config wins over $SOCKSDIRECT_CONFIG wins over default path.
    if (!args.config_path.empty()) {
        ::setenv("SOCKSDIRECT_CONFIG", args.config_path.c_str(), 1);
    }
    d.config = sd::Config::load_default();

    // Logger: --log-* > config > env vars (Logger reads them on first use).
    auto level_str = !args.log_level.empty()
        ? args.log_level
        : d.config.get_string("monitor", "log_level", "info");
    sd::Logger::instance().set_level(sd::parse_level(level_str.c_str(), sd::kLogInfo));
    auto log_file = !args.log_file.empty()
        ? args.log_file
        : d.config.get_string("monitor", "log_file", "");
    if (!log_file.empty()) sd::Logger::instance().set_sink_file(log_file);

    // Control socket path.
    d.control_socket_path = !args.control_socket.empty()
        ? args.control_socket
        : d.config.get_string("monitor", "control_socket",
                              sd::kMonitorCtlSocketDefault);

    // Make the parent directory if it doesn't exist (handle /run/socksdirect/).
    {
        std::string dir = d.control_socket_path;
        auto pos = dir.find_last_of('/');
        if (pos != std::string::npos) {
            std::string parent = dir.substr(0, pos);
            if (!parent.empty()) ::mkdir(parent.c_str(), 0755);
        }
    }

    d.pid_file_path = !args.pid_file.empty()
        ? args.pid_file
        : d.config.get_string("monitor", "pid_file", "");

    // Metrics — register the small core set.
    auto& mr = sd::MetricsRegistry::instance();
    d.m_ctl_requests = &mr.counter("socksdirect_ctl_requests_total",
                                   "control-plane requests received");
    d.m_ctl_errors   = &mr.counter("socksdirect_ctl_errors_total",
                                   "control-plane requests that returned ok=false");
    d.m_ctl_active   = &mr.gauge("socksdirect_ctl_connections",
                                 "currently-open ctl connections");

    // Open the control socket.
    d.control_fd = sd::listen_unix(d.control_socket_path, /*backlog=*/16);
    if (d.control_fd < 0) {
        LOG_ERROR("cannot listen on %s: %s",
                  d.control_socket_path.c_str(), std::strerror(errno));
        return 1;
    }
    // Permissions: 0660 so root + members of the daemon's group can talk.
    ::chmod(d.control_socket_path.c_str(), 0660);
    // Non-blocking accept so the poll loop never blocks.
    int flags = ::fcntl(d.control_fd, F_GETFL, 0);
    ::fcntl(d.control_fd, F_SETFL, flags | O_NONBLOCK);

    d.signal_fd = setup_signalfd();
    if (d.signal_fd < 0) return 1;

    write_pid_file(d.pid_file_path);

    LOG_INFO("socksdirect-monitor started: pid=%d control=%s log_level=%s",
             static_cast<int>(getpid()),
             d.control_socket_path.c_str(),
             sd::level_name(sd::Logger::instance().level()));
    sd_notify_ready("ready");

    // ---- event loop ----
    std::vector<pollfd> polls;
    polls.push_back({d.control_fd, POLLIN, 0});
    polls.push_back({d.signal_fd,  POLLIN, 0});
    bool running = true;

    while (running) {
        for (auto& p : polls) p.revents = 0;
        int rc = ::poll(polls.data(), polls.size(), 60000);
        if (rc < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("poll failed: %s", std::strerror(errno));
            break;
        }

        for (size_t i = 0; i < polls.size(); ++i) {
            if (polls[i].revents == 0) continue;
            int fd = polls[i].fd;

            if (fd == d.control_fd) {
                // Accept loop (could be many at once).
                for (;;) {
                    int c = ::accept4(d.control_fd, nullptr, nullptr,
                                      SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (c < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        LOG_WARN("accept failed: %s", std::strerror(errno));
                        break;
                    }
                    if (d.draining) {
                        sd::CtlResponse r;
                        r.ok = false;
                        r.error = "draining";
                        sd::write_all(c, sd::encode_response(r));
                        ::close(c);
                        continue;
                    }
                    d.ctl_conns[c] = Daemon::CtlConn{};
                    polls.push_back({c, POLLIN, 0});
                    d.m_ctl_active->set(static_cast<double>(d.ctl_conns.size()));
                }
                continue;
            }

            if (fd == d.signal_fd) {
                signalfd_siginfo si{};
                while (::read(d.signal_fd, &si, sizeof(si)) == sizeof(si)) {
                    if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT) {
                        LOG_INFO("received signal %u; shutting down", si.ssi_signo);
                        running = false;
                    } else if (si.ssi_signo == SIGHUP) {
                        LOG_INFO("received SIGHUP; reloading config");
                        sd::CtlRequest req;
                        req.op = "reload";
                        op_reload(d, req);
                    } else if (si.ssi_signo == SIGPIPE) {
                        // ignored; write paths handle EPIPE
                    }
                }
                continue;
            }

            // Otherwise: a ctl-connection fd.
            if (!drain_ctl_conn(d, fd)) {
                close_ctl_conn(d, fd, &polls);
                --i;  // we just removed an entry from polls
            }
        }
    }

    sd_notify_stopping();
    LOG_INFO("graceful shutdown: closing %zu ctl connections", d.ctl_conns.size());
    for (auto& kv : d.ctl_conns) ::close(kv.first);
    d.ctl_conns.clear();
    if (d.control_fd >= 0) {
        ::close(d.control_fd);
        ::unlink(d.control_socket_path.c_str());
    }
    if (d.signal_fd >= 0) ::close(d.signal_fd);
    if (!d.pid_file_path.empty()) ::unlink(d.pid_file_path.c_str());
    LOG_INFO("socksdirect-monitor stopped");
    return 0;
}
