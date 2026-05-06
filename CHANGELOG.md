# Changelog

All notable user-facing changes are recorded here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project
follows semantic versioning once it reaches v1.0.

## [Unreleased]

### Added — Phase 3 robustness + observability
- **Watchdog peer-crash detection** (`src/lib/shm_conn.cpp`):
  per-process background thread scans the ConnRegistry every 50 ms
  and calls `kill(peer_pid, 0)` on each conn's stored peer pid. On
  `ESRCH`, marks the local inbound ring closed, futex-wakes any
  parked recv, and `shm_unlink`s the segment (the dead peer never
  decremented its refcount). The previous xfail on
  `integration-shm-peer-died::test_crashed_peer_leaves_server_clean`
  is removed; the test passes 5/5 stress runs.
- **Per-pid SHM data-path metrics**
  (`src/lib/metrics_dump.{cpp,hpp}`): every libsd process writes a
  Prometheus-text snapshot to `$SOCKSDIRECT_LIB_METRICS_DIR/<pid>.prom`
  once per second (background thread) and synchronously after every
  `close()` that drops a SHM connection. Exported counters:
  `socksdirect_lib_shm_bytes_sent_total`,
  `socksdirect_lib_shm_bytes_recv_total`,
  `socksdirect_lib_shm_ring_full_blocks_total`,
  `socksdirect_lib_shm_ring_empty_blocks_total`,
  `socksdirect_lib_shm_conns_total`,
  `socksdirect_lib_shm_conn_closed_total`,
  `socksdirect_lib_shm_conns_live` — each labeled by pid.
- **`lib-metrics` ctl op** on the monitor: aggregates all per-pid
  `.prom` files in the configured directory. Stale files (whose
  pid is no longer alive) are read-then-unlinked, so a scraper
  running just after a process exits still sees that process's
  last-known counters.
- **`integration-lib-metrics`** (3 cases): real-workload
  round-trip via the ctl op asserting ≥ 1.2 MB sent across both
  peers; stale-file scrub; missing-directory handling.

### Fixed — `maybe_upgrade_to_shm` double-counted refs
On the connect path, `socket()` already registered the fd in the
FdRemapTable; the subsequent `maybe_upgrade_to_shm` was alloc-ing
a second time, double-bumping the per-(type, fd) refcount. The
close hook then decremented from 2 to 1 and never reached the
SHM-drop branch — the conn registry kept stale entries until
process exit, and metrics dumps under-counted. Now we check the
table before re-allocating. Discovered while writing the metrics
test (one peer's bytes_sent showed zero).

### Added — Phase 3 follow-ups: epoll, futex, sendmsg, RDMA scaffold
- **Per-conn send/recv mutexes** in `ShmConn` so multi-threaded
  applications can safely call `send()` from N threads on the same
  fd without corrupting the SPSC ring. 8-thread × 50 K-iter
  concurrent-send integration test (`integration-shm-concurrent-send`)
  validates byte accounting per-thread.
- **epoll integration with SHM rings** — `epoll_ctl(EPOLL_CTL_ADD)`
  on a libsd-tracked fd now creates an `eventfd` per connection,
  registers it with the kernel epoll set, and a per-process
  `ShmPoller` thread writes to it whenever the inbound ring has
  data or `peer_closed`. `epoll_wait` substitutes the application's
  fd via `data.fd` so callers see the fd they registered. Tested
  end-to-end (`integration-shm-epoll`).
- **`sendmsg` / `recvmsg`** route through SHM when the fd is
  upgraded; iovec entries are walked in order. Ancillary data
  (`msg_controllen > 0`) returns `EOPNOTSUPP` cleanly so callers
  fall back to TCP for SCM_RIGHTS-style use.
- **Futex-on-ring notification** (replaces spin-yield in `recv`):
  producer bumps + `FUTEX_WAKE`s a 32-bit counter in the SHM
  segment header after publishing; consumer `FUTEX_WAIT`s on the
  same address with a 100 ms ceiling. Idle servers consume **0 ms
  of CPU per 600 ms idle window** in the test (previously burned
  ~100% of one core). `integration-shm-futex` asserts the budget.
- **`ShmSegment::wait_for_peer` barrier** — both sides must have
  set their pid in the segment header before either side returns
  from `try_attach`. Without this, an early-closing creator could
  unlink the segment before the joiner ever opens it, leaving the
  joiner asymmetrically on TCP while the creator was on SHM.
- **`mark_closed` now wakes the futex** so the consumer detects
  EOF in microseconds, not at the next 100 ms timeout. Closes the
  segment-leak-on-rapid-close race.
- **Peer-died detection** in `shm_recv` — when a futex_wait
  doesn't make progress, poll the underlying TCP fd for POLLHUP /
  POLLERR / POLLNVAL. If hung up, mark the ring closed locally so
  recv returns EOF instead of hanging forever. (Detection latency
  on this VM is variable; the dedicated SIGKILL test is xfail
  pending a more robust signal — tracked in MISSING_FEATURES.)
- **RDMA inter-host data plane scaffold** at
  `include/socksdirect/rdma/{qp_info,conn}.hpp` +
  `src/lib/rdma/conn.cpp`. QpInfo wire codec (64 B packed, hex-
  encoded for the NDJSON ctl protocol; 5 unit tests). Verbs +
  Conn wrap libibverbs QP setup (INIT/RTR/RTS); compiles with
  `-DSOCKSDIRECT_WITH_RDMA=ON`, falls back to a no-op stub
  otherwise. Send/recv ring data path is sketched but full ring-
  over-RDMA implementation needs Mellanox hardware to verify.
  Tracked in MISSING_FEATURES.
- **Stale-segment robustness** — leftover `/dev/shm/sd-*` from a
  crashed prior run doesn't block fresh connections; `try_attach`
  generates a random per-connection key, so collisions are
  vanishingly unlikely. New `integration-shm-peer-died` test
  validates the behavior.

### Added — Phase 3 keystone: SHM intra-host data plane is live
- **`include/socksdirect/shm_ring.hpp`** — process-shared SPSC
  byte-stream ring. Cache-line padded head/tail, power-of-two
  buffer, blocking + non-blocking send/recv, peer-closed flag.
  10 unit tests including a 1 MB-through-4 KiB SPSC stress run.
- **`include/socksdirect/shm_segment.hpp`** — `shm_open` /
  `ftruncate` / `mmap` lifecycle. Lays out a 64-byte header + two
  ring buffers. Refcount in the header so the last side to close
  unlinks. 7 unit tests including refcount transitions and
  stale-segment recovery.
- **`src/lib/shm_conn.{hpp,cpp}`** — per-connection state and the
  monitor handshake during accept/connect. `try_attach` is a
  no-op for non-loopback peers so RDMA-bound traffic still hits
  the legacy library.
- **`src/lib/socket_api.cpp` rewrite** — `accept`/`accept4`/
  `connect` call `maybe_upgrade_to_shm`. `send` / `recv` /
  `write` / `read` route through the ring when the fd is
  upgraded; fall back to glibc otherwise.
- **`SOCKSDIRECT_HOOK` macro** now carries
  `__attribute__((visibility("default")))`. Without it,
  `-fvisibility=hidden` was hiding hooks from the dynamic symbol
  table — LD_PRELOAD interception only fired on libsd's
  *internal* calls. (Spotted via "why isn't the SHM upgrade
  firing?" debugging.)
- **5 new integration tests** in
  `tests/integration/test_shm_data_plane.py`. 20/20 stress runs
  clean.
- **Joiner-shm_open retry**: the monitor can answer "you're the
  joiner" before the creator's `shm_open(O_CREAT)` has happened.
  The joiner now retries on `ENOENT` for up to 5 seconds.

### Headline performance (this VM, 2 vCPUs, no RDMA)
64-byte ping-pong via `apps/rpclib-demo`, 200 000 round-trips:

| Path | Throughput | p50 / p99 / p99.9 latency |
|---|---|---|
| Vanilla TCP loopback     |   118 K msg/s | 6.4 µs / 13.2 µs / 26.6 µs |
| **libsd preloaded (SHM)** | **1.97 M msg/s** | **488 ns / 621 ns / 3.4 µs** |
| Speedup                  | **16.6×** | **13× / 21× / 7.8×** |

Squarely in the paper's claimed 7-20× throughput / 17-35× latency
range.

### Added — pre-release
- **`LICENSE`** — Apache License, Version 2.0. Hard prerequisite
  for any public release; copied from OpenClickNP and retagged
  "Copyright 2026 The SocksDirect Authors".
- **`NOTICE`** — Apache-2.0 §4(d) attribution file. Lists
  HERD-derived helpers, googletest, and the project's own
  license.
- **`RELEASE_NOTES.md`** — explicit `v0.1.0-preview` release
  notes with the tag command queued (not pushed; user-action
  required).
- **`include/socksdirect/zerocopy_mock.hpp`** + 8 unit tests —
  in-process fake of `/dev/socksdirect` so `ZeroCopyClient` is
  exercised end-to-end without loading the real kernel module.
  Closes the "LKM page-mapping has no functional test on this
  VM" soft blocker.
- **`include/socksdirect/shm_handshake.hpp`** + 7 unit tests +
  3 monitor-daemon integration tests — Phase 3 keystone scaffold.
  Two preloaded peers register with the monitor; the registry
  hands back the same SHM key to both sides regardless of who
  calls first. The monitor exposes `shm-register` and
  `shm-unregister` ops. The actual ring semantics (allocation,
  mmap, send/recv replacement, fallback) are the next Phase 3 PR.
- **FATAL→errno conversions in legacy `lib/socket_lib.cpp`**:
  - The `AF_INET` family check now returns `-1 + EAFNOSUPPORT`.
  - The `inet_aton` failure path now returns `-1 + EINVAL`.
  - 113 sites remain in the legacy tree;
    `docs/error-handling-audit.md` documents why the rest are
    deferred to the Phase 3 lib rewrite rather than retrofit on
    the about-to-be-retired code.
- **Maintainer email** pinned in `docs/SECURITY.md` for vuln
  reports (`bojieli@gmail.com`).

### Changed
- **`docs/THIRD_PARTY.md`** — corrected the HERD entry. Upstream
  declares no license as of 2026-05-06; vendored files
  (`rdma/hrd_*`) are excluded from the source tarball produced
  by `release.yml` unless the builder has an out-of-band
  redistribution grant.
- **`reproduce/vm-images/{tier1,tier2}.pkr.hcl`** — annotated
  the default cloud-init credentials (`ubuntu`/`ubuntu`) with a
  security note pointing at a "Hardening before redistribution"
  section in the README.

### Documentation expansion (in this release cycle)
- **Documentation expansion**:
  - `docs/README.md` — entry-point doc index. Tells newcomers
    which file to open based on their goal (try / reproduce /
    deploy / embed / debug).
  - `docs/OPERATIONS.md` — production deploy guide: install,
    systemd, metrics scraping, oncall checklist, capacity,
    restart safety, upgrade procedure, known caveats.
  - `docs/SECURITY.md` — explicit trust + threat model.
  - `docs/MIGRATION.md` — what's different between the prototype
    and the post-rewrite tree.
  - `docs/PERFORMANCE.md` — honest answer to "is it fast yet?"
    including microbench numbers and the
    libsd vs libsd-legacy split.
  - `docs/FAQ.md` — consolidated recurring questions.
  - `docs/ARCHITECTURE.md` extended with prose on the libsd
    interception model (dlsym caching, bootstrap short-circuit,
    re-entrancy guard) and the monitor lifecycle (poll loop,
    SIGHUP/SIGTERM handling, op dispatch).
  - README rewritten with a working hello-world recipe.
  - `integration-docs-links` CI gate validates every internal
    markdown link in docs/, README, CHANGELOG, and CONTRIBUTING.
    Plus docs/README.md is required to mention every doc under
    docs/.
  - Docs-lint job extended to require all 14 reference docs.
- **Phase 3 keystone — new src/lib/ libsd preload library**:
  - `src/lib/preload.cpp` — constructor (logger init, config load,
    FdRemapTable allocation, optional monitor handshake), destructor,
    `g_active`/`g_in_hook` re-entrancy guards.
  - `src/lib/socket_api.cpp` — full socket family with proper
    semantics for ACCELERATED / PASSTHROUGH / PARTIAL entries.
  - `src/lib/file_api.cpp` — close/dup/dup2/dup3/fcntl using
    FdRemapTable's refcount machinery (retiring the prototype's
    UNSUPPORTED tag for the dup family).
  - `src/lib/poll_api.cpp` — epoll family.
  - `src/lib/process_api.cpp` — fork/vfork/sigaction.
  - Built unconditionally; no RDMA/HERD dep; visibility=hidden so
    only the SOCKSDIRECT_HOOK extern "C" symbols escape.
  - 14 new pytest cases verify LD_PRELOAD survives `true`/`echo`/
    `ls`/`curl`/forked TCP echo; the conformance suite runs green
    under preload; libsd registers with the monitor when present
    and falls back to standalone mode otherwise.
- **`tests/conformance/run_conformance.py --preload`** wires every
  coverage.toml entry through the new libsd; each status class is
  honored.
- **LTP socket conformance** at `tests/conformance/ltp.py` plus
  `integration-ltp` ctest. Skips cleanly when LTP isn't installed.
- **Release workflow** at `.github/workflows/release.yml` — on tag
  push, builds source-tarball, .deb, .rpm, uploads to a GitHub
  release with SHA256SUMS. `packaging/docker/Dockerfile.{deb,rpm}-builder`
  for local artifact builds.
- **Self-hosted perf-regression workflow** at
  `.github/workflows/perf-regression.yml` (`[self-hosted, perf]`,
  10% threshold, `tools/perf-baselines/` storage,
  `workflow_dispatch -f refresh_baseline=true`).
- **Packer validation** in main CI — `packer validate -syntax-only`
  on tier1/tier2 manifests; doesn't need KVM.
- **Repro dry-run sweep** at `tools/repro_dry_run.sh` plus the
  `repro-dry-run` CI job. Every figure script either runs end-to-end
  at Tier 1 or emits a sentinel artifact, so script bit-rot is
  caught without RDMA hardware.
- **Production monitor daemon** at `src/monitor/main.cpp` — single
  threaded poll() event loop using the new Logger / Metrics /
  MonitorIpc / Config; ops `status`, `connections`, `dump-state`,
  `dump-config`, `metrics`, `reload`, `drain`, `ping`, `help`;
  signalfd-based SIGTERM/SIGHUP; sd_notify(READY=1) when systemd
  Type=notify is in use; PID file lifecycle; graceful socket cleanup
  on exit. End-to-end pytest suite (`integration-monitor-daemon`)
  exercises every op against the real binary.
- **FdRemapTable refcounting**: `dup()` / `dup_to()` (dup2 semantics)
  / `refcount()` / refcount-aware `free()` returning the post-decrement
  count. Phase 3 prerequisite for socket-fd dup/dup2/dup3 in libsd.
- **`tools/render_api_doc.py`**: regenerates `docs/API.md` from
  `tests/conformance/coverage.toml`. CI gate
  (`integration-api-doc-drift`) fails on divergence.
- **`tools/scan_error_handling.py`**: enumerates every `FATAL/assert/
  abort` site in the legacy tree and emits
  `docs/error-handling-audit.md`. CI gate
  (`integration-error-handling-audit`) fails on drift.
- **Phase 5 LKM page mapping**: `SD_IOC_VIRT2PHYS{,_VEC}` and
  `SD_IOC_MAP_PHYS{,_VEC}` are now functional via
  `get_user_pages_remote` + `vm_insert_page`. The kernel module
  exposes `mmap()` on `/dev/socksdirect` to install a `VM_MIXEDMAP`
  VMA the userspace library can later substitute pages into.
- **All Phase 6 figures** present in `reproduce/figures/`:
  `msgsize-{intra,inter}`, `corenum-{intra,inter}`, `sharecore-lat`,
  `fork-tput`, `tab1-latency-breakdown`, `nginx`, `nfv` plus the
  shared helper at `reproduce/figures/_lib.sh` and gnuplot templates
  under `reproduce/plot-templates/`. Each figure gracefully skips
  with a sentinel `SKIPPED.txt` when its prerequisites aren't met.
- **`apps/`** demo programs with `LD_PRELOAD`-friendly entry points:
  `nfv-pipeline/` (5 tiny Python NFs piped together), `nginx-demo/`,
  `redis-demo/`, `rpclib-demo/` (C client+server, ~87 K msg/s on
  loopback in this VM).
- **Two-host orchestration** via Ansible:
  `reproduce/orchestration/playbook.yml` plus
  `tools/inv-to-ansible.py` to render the harness inventory as an
  Ansible inventory.
- **Packer manifests** for the Tier-1 / Tier-2 VM images at
  `reproduce/vm-images/{tier1,tier2}.pkr.hcl`, with cloud-init
  autoinstall config under `http/` and a Vagrantfile.
- **RPM spec** (`packaging/rpm/socksdirect.spec`) plus full Debian
  maintainer scripts: `postinst` creates the `socksdirect` user/
  group and enables the systemd unit; `prerm`/`postrm` handle the
  uninstall path.
- **Hardened systemd unit**: `Type=notify`, `NotifyAccess=main`,
  `SystemCallFilter=@system-service`, `~@privileged @mount @raw-io ...`.
- **Packaging-lint integration test** validates that every
  `debian/*.install` references a real path and that the spec/unit/
  packer/playbook files parse.

### Changed
- `common/locklessq_v2.hpp`: `atomic_copy16` and `atomic_copy8` now
  carry the `"memory"` clobber on their inline asm (Phase 1 fix for
  the stale-read bug under -O2/-O3).
- `common/locklessq_v3.hpp`: added a destructor that frees the two
  block descriptors `init_ptr` allocates, fixing the leak previously
  suppressed via `tests/asan.supp`. Also added the `"memory"` clobber
  to the matching `atomic_copy16` here.
- Deleted: `__deprecated_attachqueue_*` files in `common/`, `lib/`,
  and `monitor/`. Naming gate enforces they stay deleted.
- Naming gate widened to include `apps/` and to require the
  `__deprecated_*` files stay deleted.
- `tests/asan.supp` now near-empty (the locklessq_v3 leak was the
  only remaining suppression).

### Added — also
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
