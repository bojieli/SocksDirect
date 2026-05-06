// SPDX-License-Identifier: Apache-2.0
//
// Per-process state for libsd's preload runtime.

#ifndef SOCKSDIRECT_LIB_STATE_HPP_
#define SOCKSDIRECT_LIB_STATE_HPP_

#include "socksdirect/config.hpp"
#include "socksdirect/fd_remap.hpp"
#include "socksdirect/metrics.hpp"

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
};

State& state();

}  // namespace preload
}  // namespace socksdirect

#endif  // SOCKSDIRECT_LIB_STATE_HPP_
