// SPDX-License-Identifier: Apache-2.0

#include "src/lib/metrics_dump.hpp"
#include "src/lib/shm_conn.hpp"
#include "src/lib/state.hpp"

#include "socksdirect/log.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace socksdirect {
namespace preload {

namespace {

std::string current_path() {
    if (state().metrics_dir.empty()) return {};
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/%d.prom",
                  static_cast<int>(::getpid()));
    return state().metrics_dir + buf;
}

// Render a Prometheus counter line with one label.
void emit_counter(std::string& out, const char* name, const char* help,
                  std::uint64_t value, int pid) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "# HELP %s %s\n"
        "# TYPE %s counter\n"
        "%s{pid=\"%d\"} %llu\n",
        name, help, name, name, pid,
        static_cast<unsigned long long>(value));
    out += buf;
}

void emit_gauge(std::string& out, const char* name, const char* help,
                std::uint64_t value, int pid) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "# HELP %s %s\n"
        "# TYPE %s gauge\n"
        "%s{pid=\"%d\"} %llu\n",
        name, help, name, name, pid,
        static_cast<unsigned long long>(value));
    out += buf;
}

}  // namespace

std::string render_lib_metrics() {
    int pid = static_cast<int>(::getpid());
    std::string out;
    out.reserve(2048);

    // Sum live counters across the registry.
    std::uint64_t live_sent = 0, live_recv = 0;
    std::uint64_t live_full = 0, live_empty = 0;
    std::size_t   live_conns = 0;
    for (auto& c : conn_registry().snapshot()) {
        if (!c) continue;
        ++live_conns;
        live_sent  += c->bytes_sent.load(std::memory_order_relaxed);
        live_recv  += c->bytes_recv.load(std::memory_order_relaxed);
        live_full  += c->ring_full_blocks.load(std::memory_order_relaxed);
        live_empty += c->ring_empty_blocks.load(std::memory_order_relaxed);
    }

    auto& s = state();
    std::uint64_t total_sent  = live_sent  + s.shm_bytes_sent_closed.load();
    std::uint64_t total_recv  = live_recv  + s.shm_bytes_recv_closed.load();
    std::uint64_t total_full  = live_full  + s.shm_ring_full_closed.load();
    std::uint64_t total_empty = live_empty + s.shm_ring_empty_closed.load();

    emit_counter(out, "socksdirect_lib_shm_bytes_sent_total",
                 "bytes sent through the SHM data plane",
                 total_sent, pid);
    emit_counter(out, "socksdirect_lib_shm_bytes_recv_total",
                 "bytes received through the SHM data plane",
                 total_recv, pid);
    emit_counter(out, "socksdirect_lib_shm_ring_full_blocks_total",
                 "send blocked because the SHM ring was full",
                 total_full, pid);
    emit_counter(out, "socksdirect_lib_shm_ring_empty_blocks_total",
                 "recv blocked because the SHM ring was empty",
                 total_empty, pid);
    emit_counter(out, "socksdirect_lib_shm_conns_total",
                 "SHM connections opened by this process",
                 s.shm_conns_total.load(), pid);
    emit_counter(out, "socksdirect_lib_shm_conn_closed_total",
                 "SHM connections closed by this process",
                 s.shm_conn_closed_total.load(), pid);
    emit_gauge(out, "socksdirect_lib_shm_conns_live",
               "currently-open SHM connections",
               static_cast<std::uint64_t>(live_conns), pid);
    return out;
}

bool dump_lib_metrics_once() {
    std::string path = current_path();
    if (path.empty()) return false;

    std::string body = render_lib_metrics();
    // Atomic write via tmp-file + rename so a scraper never sees a
    // half-written file.
    std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    ssize_t total = 0;
    while (static_cast<std::size_t>(total) < body.size()) {
        ssize_t n = ::write(fd, body.data() + total, body.size() - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        total += n;
    }
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) < 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

namespace {
std::atomic<bool> g_dumper_started{false};
}

void ensure_metrics_dumper_started(int interval_ms) {
    if (state().metrics_dir.empty()) return;
    if (g_dumper_started.exchange(true, std::memory_order_acq_rel)) return;
    // Try to create the dir; ignore EEXIST.
    ::mkdir(state().metrics_dir.c_str(), 0755);
    std::thread([interval_ms]() {
        ::pthread_setname_np(::pthread_self(), "sd-metrics");
        for (;;) {
            dump_lib_metrics_once();
            struct timespec ts{interval_ms / 1000,
                               (interval_ms % 1000) * 1000L * 1000L};
            ::nanosleep(&ts, nullptr);
        }
    }).detach();
}

void remove_lib_metrics_file() {
    std::string path = current_path();
    if (path.empty()) return;
    ::unlink(path.c_str());
}

}  // namespace preload
}  // namespace socksdirect
