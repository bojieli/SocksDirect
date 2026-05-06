// SPDX-License-Identifier: Apache-2.0
//
// libsd preload library — process bootstrap.
//
// Constructor responsibilities (in order):
//   1. Initialize the Logger from env (level + sink).
//   2. Load Config from $SOCKSDIRECT_CONFIG or the default path.
//   3. Allocate the per-process FdRemapTable.
//   4. Connect to the monitor's control socket if available, register
//      ourselves, and stash the connection on a long-lived fd.
//      Failure here is non-fatal (logged at WARN); standalone mode is
//      a supported configuration.
//   5. Mark the active flag so hooks start interposing.
//
// Destructor responsibilities:
//   - Best-effort flush of metrics + log sinks.
//   - Close the monitor connection.
//
// fork()/exec*() handlers in src/lib/process_api.cpp coordinate with
// this scaffold so child processes start in a sane state without
// double-initializing.

#include "src/lib/intercept.hpp"
#include "src/lib/metrics_dump.hpp"
#include "src/lib/state.hpp"
#include "socksdirect/config.hpp"
#include "socksdirect/fd_remap.hpp"
#include "socksdirect/log.hpp"
#include "socksdirect/metrics.hpp"
#include "socksdirect/monitor_ipc.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace socksdirect {
namespace preload {

std::atomic<bool> g_active{false};
thread_local bool g_in_hook = false;

State& state() {
    // Magic-static; thread-safe under the C++11 model.
    static State s;
    return s;
}

}  // namespace preload
}  // namespace socksdirect

namespace sd  = socksdirect;
namespace sdp = socksdirect::preload;

// ---------------------------------------------------------------------------
// Bootstrap
// ---------------------------------------------------------------------------

static void preload_init() __attribute__((constructor(101)));
static void preload_fini() __attribute__((destructor(101)));

static void preload_init() {
    // Logger: read SOCKSDIRECT_LOG, etc.
    sd::Logger::instance();

    // Config: best-effort load from env-or-default.
    sdp::state().config = sd::Config::load_default();

    // Per-process metrics for the data path.
    auto& mr = sd::MetricsRegistry::instance();
    sdp::state().m_socket_total =
        &mr.counter("socksdirect_lib_socket_total",
                    "socket(2) calls intercepted by libsd");
    sdp::state().m_close_total =
        &mr.counter("socksdirect_lib_close_total",
                    "close(2) calls on libsd-tracked fds");
    sdp::state().m_dup_total =
        &mr.counter("socksdirect_lib_dup_total",
                    "dup family calls on libsd-tracked fds");
    sdp::state().m_unsupported_total =
        &mr.counter("socksdirect_lib_unsupported_total",
                    "calls returning ENOSYS due to UNSUPPORTED status");
    sdp::state().m_passthrough_total =
        &mr.counter("socksdirect_lib_passthrough_total",
                    "calls passed through to glibc unchanged");

    // Try to register with the monitor. Failure is non-fatal.
    std::string sock = sdp::state().config.get_string(
        "monitor", "control_socket", sd::kMonitorCtlSocketDefault);
    int fd = sd::connect_unix(sock);
    if (fd >= 0) {
        // Ping the monitor so it logs our pid.
        sd::CtlRequest req;
        req.op = "ping";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "lib pid=%d", static_cast<int>(getpid()));
        req.args.push_back(buf);
        sd::write_all(fd, sd::encode_request(req));
        // Drain the response so the daemon doesn't get a SIGPIPE on
        // close. We don't care about the contents.
        (void)sd::read_line(fd);
        ::close(fd);
        sdp::state().monitor_reachable = true;
        LOG_INFO("libsd preloaded; monitor at %s", sock.c_str());
    } else {
        sdp::state().monitor_reachable = false;
        LOG_INFO("libsd preloaded; monitor at %s unreachable (%s) — standalone mode",
                 sock.c_str(), std::strerror(errno));
    }

    // Metrics export: SOCKSDIRECT_LIB_METRICS_DIR overrides the
    // config; default is /run/socksdirect/lib-metrics/. Empty path
    // disables export.
    const char* mdir_env = std::getenv("SOCKSDIRECT_LIB_METRICS_DIR");
    sdp::state().metrics_dir = mdir_env
        ? std::string(mdir_env)
        : sdp::state().config.get_string(
              "monitor", "lib_metrics_dir",
              "/run/socksdirect/lib-metrics");
    sdp::ensure_metrics_dumper_started();

    sdp::g_active.store(true, std::memory_order_release);
}

static void preload_fini() {
    // We don't dump-on-exit here: the static-destructor order for
    // the function-static ConnRegistry vs. the State is not guaranteed
    // relative to this destructor, and dereferencing either after its
    // destruction would crash. Instead, the close() hook calls
    // dump_lib_metrics_once() right after folding the conn's counters
    // into State, so the on-disk snapshot reflects the latest state
    // for every closed connection. The periodic dumper (1 s interval)
    // covers the remaining live counters.
    //
    // Per-pid file is left in place for the monitor's lib-metrics op
    // to scrub on its next scan (it detects the dead pid via
    // kill(pid, 0)).
    sdp::g_active.store(false, std::memory_order_release);
    LOG_TRACE("libsd unloading; %zu live fds in remap table",
              sdp::state().fd_table.live_count());
}
