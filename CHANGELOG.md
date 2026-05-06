# Changelog

All notable user-facing changes are recorded here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project
follows semantic versioning once it reaches v1.0.

## [Unreleased]

### Added
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
