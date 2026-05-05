# Changelog

All notable user-facing changes are recorded here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project
follows semantic versioning once it reaches v1.0.

## [Unreleased]

### Added
- **Conformance suite** at `tests/conformance/`:
  - `coverage.toml` is the single source of truth for libc-function
    support levels (accelerated / passthrough / partial / unsupported).
  - `cases/*.c` — one tiny C program per category, validating glibc
    behavior so libsd's preloaded behavior can be compared apples-to-
    apples.
  - `run_conformance.py` walks the table, compiles + runs each case,
    and supports `--preload PATH/libsd.so` for the libsd path.
- **Linux TCP loopback baseline** under `bench/baselines/` plus a
  `loopback-baseline` reproduction figure so the comparison context
  for intra-host claims is reproducible at every tier.
- **Phase-0 documents**:
  - `docs/MISSING_FEATURES.md` enumerating every paper-described
    feature this tree doesn't yet implement, with disposition.
  - `docs/THIRD_PARTY.md` listing every vendored / fetched dependency
    and its license.
- **DKMS test matrix** at `packaging/dkms/test-matrix.md` documenting
  which kernels the LKM has been built against.
- **Kernel patches placeholder** at `src/kernel/patches/README.md`
  explaining that the LKM was selected over the syscall-patch
  fallback.
- **Public API headers** under `include/socksdirect/`:
  - `config.hpp` — INI-style configuration loader with env-override
    semantics. Replaces hardcoded paths/IPs in lib + monitor.
  - `log.hpp` — leveled, header-only Logger replacing the
    printf-based `DEBUG`/`FATAL`/`ERROR` macros.
  - `metrics.hpp` — header-only Prometheus-text metrics registry
    (counters, gauges, fixed-bucket histograms).
  - `monitor_ipc.hpp` — newline-delimited JSON wire protocol used by
    the daemon and `socksdirect-ctl`.
  - `fd_remap.hpp` — thread-safe virtual ↔ real fd table.
  - `zerocopy.h` + `zerocopy_client.hpp` — userspace ABI for the
    `/dev/socksdirect` LKM, with copy-mode fallback when the module
    isn't loaded.
- **`socksdirect-ctl` CLI** — the control-plane tool talking to the
  monitor over Unix socket. Operations: `status`, `connections`,
  `dump-state`, `reload`, `drain`.
- **Out-of-tree LKM skeleton** in `src/kernel/` exposing the zero-copy
  ABI as ioctls on `/dev/socksdirect`. Built via DKMS; the userspace
  library auto-detects the device and falls back to copy mode.
- **Unit tests** for every new header (logger, metrics, monitor IPC,
  zerocopy client, config). Lockless queue tests, fd remap tests, and
  helper-timing tests previously added remain.
- **Performance microbenchmarks** with stable JSON output for the
  perf-regression gate: `bench_queue_v3`, `bench_fd_remap`,
  `bench_metrics`, `bench_config`.
- **Integration tests** runnable without RDMA/the LKM: `socksdirect-ctl`
  against a fake monitor, the `repro` CLI subcommands, the
  `perf_regression.py` script, kernel/userspace ABI drift, and
  microbench JSON-schema smoke.
- **Reproduction harness** (`reproduce/repro`) with `--figures-dir`,
  `--inventory`, and `--results-dir` flags, making the harness
  testable without modifying the in-tree `reproduce/`.
- **CI** matrix on Ubuntu 22.04 + 24.04 (Release/Debug), ASan + UBSan
  job, naming-consistency gate, docs-lint gate, microbench JSON-smoke.
- **Packaging** scaffolding under `packaging/` (debian rules, DKMS
  metadata, systemd unit, example config).

### Changed
- Library target name: `libipc.so` → `libsd.so` (matches the paper).
- Monitor binary name: `monitor` → `socksdirect-monitor`.
- Project name: "IPC-Direct" → "socksdirect" everywhere.
- Tests now run without `libsd` being built — control-plane and repro
  suites are CI-portable.

### Removed
- `__deprecated_attachqueue*` files in `lib/`, `common/`, `monitor/`
  are scheduled for removal once Phase 1 lands; left in tree for now
  to preserve `git blame` history.

### Known issues
- `lib/socket_lib.cpp:564,580` — "Dynamic allocation not implemented"
  remains; tracked in `docs/API.md`.
- `lib/poll_lib.cpp:280` — `EPOLLHUP` and friends still unsupported.
- Multi-thread socket migration (`lib/lib.cpp:49`) deferred to Phase 3.
- Zero-copy `SD_IOC_VIRT2PHYS{,_VEC}` and `SD_IOC_MAP_PHYS{,_VEC}`
  return `-ENOSYS`; userspace falls back to memcpy. Wire protocol
  is locked-in.

[Unreleased]: https://github.com/anthropics/socksdirect/compare/v0.0.0...HEAD
