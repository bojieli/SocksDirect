// SPDX-License-Identifier: Apache-2.0
//
// Per-process state for libsd's preload runtime.

#ifndef SOCKSDIRECT_LIB_STATE_HPP_
#define SOCKSDIRECT_LIB_STATE_HPP_

#include "socksdirect/config.hpp"
#include "socksdirect/fd_remap.hpp"
#include "socksdirect/metrics.hpp"

#include <atomic>
#include <cstdint>
#include <string>

namespace socksdirect {
namespace preload {

struct State {
    // Loaded once at constructor time; can be reloaded via SIGHUP
    // (Phase 3 follow-up; for now it's static after init).
    Config config;

    FdRemapTable fd_table;

    // True if the monitor's control socket was reachable at boot.
    bool monitor_reachable = false;

    // Cached metric handles.
    Counter* m_socket_total       = nullptr;
    Counter* m_close_total        = nullptr;
    Counter* m_dup_total          = nullptr;
    Counter* m_unsupported_total  = nullptr;
    Counter* m_passthrough_total  = nullptr;

    // Cumulative SHM data-path counters from closed connections.
    // Per-conn live counters (in ShmConn) get folded in on close so
    // the Prometheus snapshot reflects the total bytes ever flowed
    // through this libsd instance.
    std::atomic<std::uint64_t> shm_bytes_sent_closed{0};
    std::atomic<std::uint64_t> shm_bytes_recv_closed{0};
    std::atomic<std::uint64_t> shm_ring_full_closed{0};
    std::atomic<std::uint64_t> shm_ring_empty_closed{0};
    std::atomic<std::uint64_t> shm_conns_total{0};
    std::atomic<std::uint64_t> shm_conn_closed_total{0};

    // Where to drop per-pid Prometheus-text snapshots. Empty means
    // metrics export is disabled for this process.
    std::string metrics_dir;
};

State& state();

}  // namespace preload
}  // namespace socksdirect

#endif  // SOCKSDIRECT_LIB_STATE_HPP_
