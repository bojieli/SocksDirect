// SPDX-License-Identifier: Apache-2.0
//
// libsd-side metrics export — write a Prometheus-text snapshot of
// the current process's SHM data-path counters to a per-pid file.
// The monitor's `lib-metrics` ctl op aggregates files in the
// configured directory.

#ifndef SOCKSDIRECT_LIB_METRICS_DUMP_HPP_
#define SOCKSDIRECT_LIB_METRICS_DUMP_HPP_

#include <string>

namespace socksdirect {
namespace preload {

// Render the per-pid snapshot to a string. Sums per-conn live
// counters (from the conn registry) with the cumulative-on-close
// counters (in State).
std::string render_lib_metrics();

// Atomic-rename write of the snapshot to
// <metrics_dir>/<pid>.prom. Returns false on failure (e.g. dir
// doesn't exist or isn't writable). Best-effort; we never crash
// the host process on dump failure.
bool dump_lib_metrics_once();

// Idempotently start a background thread that calls
// dump_lib_metrics_once() every interval_ms (default 1000). The
// thread is detached and runs for the life of the process.
void ensure_metrics_dumper_started(int interval_ms = 1000);

// Best-effort: remove the per-pid file at process exit. Safe to
// call multiple times.
void remove_lib_metrics_file();

}  // namespace preload
}  // namespace socksdirect

#endif  // SOCKSDIRECT_LIB_METRICS_DUMP_HPP_
