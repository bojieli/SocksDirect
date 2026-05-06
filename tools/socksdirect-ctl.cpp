// SPDX-License-Identifier: Apache-2.0
//
// socksdirect-ctl — CLI for the socksdirect-monitor daemon.
//
// Usage:
//   socksdirect-ctl [--socket PATH] <op> [args...]
//
// Available ops are documented in include/socksdirect/monitor_ipc.hpp:
//   status        – monitor lifecycle / pid / uptime
//   connections   – per-connection info
//   dump-state    – verbose internal state for debugging
//   reload        – re-read config without restart
//   drain         – stop accepting new clients; finish in-flight
//
// Exit codes:
//   0  the request was answered with ok=true
//   1  the request was answered with ok=false (response printed to stderr)
//   2  invalid arguments
//   3  could not connect to the monitor socket
//   4  malformed response from the monitor
//
// This binary contains no business logic — it serializes a CtlRequest,
// writes it to the monitor's control socket, and prints lines from the
// CtlResponse. The op-to-action mapping lives in the daemon. Keeping the
// CLI minimal makes the protocol surface easy to evolve without touching
// every release of the tool.

#include "socksdirect/log.hpp"
#include "socksdirect/monitor_ipc.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void print_usage(FILE* out) {
    std::fprintf(out,
        "usage: socksdirect-ctl [--socket PATH] <op> [args...]\n"
        "\n"
        "Common ops:\n"
        "  status                 monitor pid, uptime, transport summary\n"
        "  connections            list active intercepted connections\n"
        "  dump-state             verbose internal state\n"
        "  reload                 re-read /etc/socksdirect/socksdirect.conf\n"
        "  drain                  stop accepting new clients\n"
        "\n"
        "Options:\n"
        "  --socket PATH          override default control socket path\n"
        "  --help                 show this help\n");
}

int run_request(const std::string& sock_path,
                const socksdirect::CtlRequest& req) {
    int fd = socksdirect::connect_unix(sock_path);
    if (fd < 0) {
        std::fprintf(stderr,
            "socksdirect-ctl: cannot connect to %s: %s\n",
            sock_path.c_str(), std::strerror(errno));
        std::fprintf(stderr,
            "  Is socksdirect-monitor running?  Set SOCKSDIRECT_CTL_SOCKET\n"
            "  or pass --socket to point at the right path.\n");
        return 3;
    }

    if (!socksdirect::write_all(fd, socksdirect::encode_request(req))) {
        std::fprintf(stderr, "socksdirect-ctl: write failed: %s\n",
                     std::strerror(errno));
        ::close(fd);
        return 4;
    }

    std::string line = socksdirect::read_line(fd);
    ::close(fd);
    if (line.empty()) {
        std::fprintf(stderr,
            "socksdirect-ctl: monitor closed the connection without responding\n");
        return 4;
    }

    socksdirect::CtlResponse resp;
    if (!socksdirect::decode_response(line, resp)) {
        std::fprintf(stderr,
            "socksdirect-ctl: malformed response from monitor: %s\n",
            line.c_str());
        return 4;
    }

    if (!resp.ok) {
        std::fprintf(stderr, "%s\n", resp.error.c_str());
        return 1;
    }
    for (const auto& l : resp.lines) {
        std::printf("%s\n", l.c_str());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // Ignore SIGPIPE so write() to a peer that closed early returns
    // EPIPE instead of killing us. The daemon may have closed the
    // connection (e.g. drain mode) before our write reached it.
    std::signal(SIGPIPE, SIG_IGN);

    std::string sock_path =
        std::getenv("SOCKSDIRECT_CTL_SOCKET")
            ? std::getenv("SOCKSDIRECT_CTL_SOCKET")
            : socksdirect::kMonitorCtlSocketDefault;

    socksdirect::CtlRequest req;
    int i = 1;
    for (; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            print_usage(stdout);
            return 0;
        }
        if (a == "--socket") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "socksdirect-ctl: --socket needs an argument\n");
                return 2;
            }
            sock_path = argv[++i];
            continue;
        }
        if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "socksdirect-ctl: unknown flag: %s\n", a.c_str());
            print_usage(stderr);
            return 2;
        }
        // First positional is the op; the rest are its args.
        if (req.op.empty()) {
            req.op = a;
        } else {
            req.args.push_back(a);
        }
    }

    if (req.op.empty()) {
        print_usage(stderr);
        return 2;
    }

    return run_request(sock_path, req);
}
