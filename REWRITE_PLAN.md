# SocksDirect: Rewrite and Reorganization Plan

## 1. Executive summary

The current SocksDirect tree is a research prototype: ~7.5K lines of C/C++
across `lib/`, `monitor/`, and `common/`, plus a partial Linux-kernel overlay
under `zerocopy/linux/`, plus per-figure benchmark binaries in `pot/` driven by
shell scripts in `data/` that hardcode site-specific paths and IPs. The README
flags it as "not fully tested." The build target name (`libipc.so`) does not
match the paper's name (`libsd`), several BSD socket APIs are unimplemented or
ignored (`dup`/`dup2`/`dup3`, `shutdown`, `EPOLLHUP`), and the paper's claimed
17K lines is more than 2x the visible code — features described in the paper
(NFV pipeline, share-core scheduling, complete fork handling) appear to be
missing or unfinished.

This plan rewrites the codebase along two axes:

1. **Production readiness** — finish the API surface, add tests + CI, replace
   asserts with proper errno paths, externalize all configuration, package as
   `.deb`/`.rpm`+DKMS, and make the monitor a real daemon.
2. **Reproduction readiness** — a `reproduce/` harness with one command per
   paper figure, three reproduction tiers (VM functional / VM intra-host perf /
   bare-metal full perf), packer-built VM images so readers do not have to
   touch their host kernel, and explicit honesty about what each tier can and
   cannot reproduce.

The work is organized into **eight sequential phases** (0–7), each
independently shippable, with **cross-cutting workstreams** that run in
parallel. Estimated critical path: 10–12 engineer-weeks solo, 6–8 with two
engineers parallelizing the API/test and kernel/repro tracks.

## 2. Goals and non-goals

### Goals

- Drop-in BSD socket replacement with a clearly defined and tested
  compatibility surface.
- One-command build, install, and "hello world" demo on a fresh Ubuntu LTS.
- One-command per-figure reproduction with auto-detected hardware tier and
  tier-aware reporting that does not lie about RDMA performance under
  SoftRoCE.
- Safe to install: kernel changes shipped as a DKMS module where possible, and
  as a packer-built QEMU image where a syscall-level kernel patch is required.
- Stable enough for an external user to run nginx + redis + memcached under
  preload without manual workarounds.

### Non-goals

- Adding new performance optimizations beyond what the paper already
  describes. The rewrite makes existing claims reproducible; it does not chase
  new ones.
- Porting to non-Linux kernels.
- Supporting NICs other than Mellanox ConnectX-4/5/6 in the perf path.
  Functional path uses SoftRoCE so any Ethernet NIC works.
- A new transport. RDMA stays as the inter-host transport.
- Rewriting in another language. Stay in C/C++ to preserve the paper's
  performance story.

## 3. Current-state assessment (summary)

Detailed audit in conversation history; the load-bearing facts:

- **Build**: CMake works but `launch.sh` references `cmake-build-debug/`
  (IDE-specific). Library builds as `libipc.so`, paper says `libsd`.
- **API completeness gaps** (with file:line):
  - `lib/socket_lib.cpp:564,580` — "Dynamic allocation not implemented" `FATAL`.
  - `lib/socket_lib.cpp:714` — `shutdown` silently ignored.
  - `lib/socket_lib.cpp:2110-2149` — `dup`/`dup2`/`dup3` log error and bail.
  - `lib/socket_lib.cpp:1242,1257` — fcntl branches not implemented.
  - `lib/poll_lib.cpp:280` — `EPOLLHUP` and friends "not supported yet".
  - `lib/lib.cpp:49` — multi-thread socket migration TODO.
  - `lib/lib.cpp:342` — `vfork`/`clone`/`daemon`/`sigaction` TODO.
- **Hardcoded configuration**:
  - `pot/testn.sh`: `/sampa/home/cuity/projects/libsd/...`, `10.2.5.203`.
  - `data/.../*.sh`: `10.1.2.4`, `10.1.2.34`, `/home/boj/libsd/build`.
  - `demo/start_nginx.sh`: `/root/IPC-Direct/demo/nginx.conf`.
  - `demo/demo.sh`: hostname `netsys34`, path `/usr/share/nginx/html/data`.
- **Kernel zero-copy**: `zerocopy/linux/` is a 5-file overlay claiming syscall
  numbers 333–339 with no patch series, no kernel version pinned, no build
  instructions. Without it, the >=16 KiB zero-copy paths cannot run.
- **Tests**: none. The `test/` directory contains smoke programs.
- **Deprecated cruft**: `__deprecated_attachqueue*` files in three modules.
- **LOC vs paper**: paper says 17K; code+monitor+common is 7.5K.
- **License**: missing.

## 4. Target architecture

### 4.1 Repository layout

```
socksdirect/
  CMakeLists.txt
  README.md
  CHANGELOG.md
  CONTRIBUTING.md

  src/
    common/                # shared types, queues, RDMA helpers
    lib/                   # libsd.so — preloaded into apps
    monitor/               # socksdirect-monitor daemon
    kernel/                # out-of-tree LKM (preferred) and patch series (fallback)
    tools/                 # socksdirect-ctl CLI

  include/socksdirect/     # public headers (for tools / extensions only)

  tests/
    unit/                  # gtest-based, per-module
    integration/           # pytest harness, spawns monitor + clients
    conformance/           # LTP socket suite under LD_PRELOAD
    fault-injection/

  bench/                   # microbenchmark binaries (was pot/)
    intra/
    inter/
    baselines/             # linux/, libvma/, rsocket/, raw-rdma/ wrappers

  apps/
    nginx-demo/
    nfv-pipeline/          # NEW: NFs as processes reading pcap from stdin
    redis-demo/
    rpclib-demo/

  reproduce/
    inventory.yml          # SINGLE source of truth for hosts/IPs/NICs
    repro                  # top-level CLI
    figures/
      fig3-msgsize-intra/{run.sh, plot.sh, expected/, README.md}
      fig3-msgsize-inter/...
      fig4-corenum/...
      fig5-sharecore/...
      fig6-nginx/...
      fig7-nfv/...
      tab1-latency-breakdown/...
    plot-templates/        # vendored .plt from paper repo
    vm-images/             # packer manifests for tier-2/tier-1 QEMU images

  packaging/
    debian/
    rpm/
    dkms/                  # DKMS metadata for the kernel module
    systemd/               # socksdirect-monitor.service

  docs/
    ARCHITECTURE.md        # paper sections -> code modules
    API.md                 # supported / unsupported / passthrough table
    CONFIGURATION.md       # every config knob
    REPRODUCIBILITY.md     # the 3-tier model, expected outputs
    TROUBLESHOOTING.md
    KERNEL_MODULE.md       # building, loading, fallback behavior

  paper/                   # git submodule -> SocksDirect-paper repo
```

### 4.2 Naming conventions

| Today | After rewrite |
|---|---|
| `libipc.so` | `libsd.so` |
| `ipc-common` | `libsdcommon.so` |
| `monitor` binary | `socksdirect-monitor` |
| project name "IPC-Direct" | `socksdirect` everywhere |

A one-shot global rename in Phase 1; afterward CI greps for the old strings
and fails the build.

### 4.3 Module boundaries

- `common/` exposes only data structures and pure helpers. No I/O, no
  globals, no logging side effects beyond a passed-in logger.
- `lib/` is the only place that intercepts libc.
- `monitor/` is the only place that allocates ports, IDs, and SHM keys.
- `kernel/` exposes one device node `/dev/socksdirect` (when LKM mode is
  used); userspace code never references syscall numbers directly.
- Cross-module dependencies flow `lib → common`, `monitor → common`,
  `tools → common`. No back-edges.

### 4.4 Configuration schema (`/etc/socksdirect/socksdirect.toml`)

```toml
[monitor]
socket_path     = "/run/socksdirect/monitor.sock"
log_dir         = "/var/log/socksdirect"
log_level       = "info"
metrics_socket  = "/run/socksdirect/metrics.sock"

[fd]
virtual_fd_base = 0x40000000        # see lib/socket_lib.cpp current behavior
virtual_fd_top  = 0x7fffffff

[shm]
hugepage_size_kb = 2048
ring_pages       = 64

[rdma]
device           = "auto"           # or "mlx5_0", etc.
gid_index        = 3
mtu              = 4096
qp_depth         = 128
batch_completions = 64
fallback         = "rxe"            # or "none"

[zerocopy]
enabled          = "auto"           # auto-detects /dev/socksdirect
min_message_size = 16384
```

Library reads via `SOCKSDIRECT_CONFIG` env var (default
`/etc/socksdirect/socksdirect.toml`). Per-process overrides via
`SOCKSDIRECT_<SECTION>_<KEY>` env vars for ad-hoc benchmarking.

### 4.5 Reproduction inventory schema (`reproduce/inventory.yml`)

```yaml
hosts:
  alpha:
    ssh: alpha.example.com
    cores: [0,1,2,3,4,5,6,7]
    nic: mlx5_0
    rdma_ip: 10.0.0.1
  beta:
    ssh: beta.example.com
    cores: [0,1,2,3,4,5,6,7]
    nic: mlx5_0
    rdma_ip: 10.0.0.2

transport: auto              # rdma | rxe | auto
tier:      auto              # 1 | 2 | 3 | auto
results_dir: ./results
```

`./repro check` resolves `auto` and prints the resulting tier. `./repro all`
honors it.

## 5. Reproduction tiers

| Tier | Hardware | Inter-host transport | What it reproduces | Audience |
|---|---|---|---|---|
| **T1 Functional** | Any Linux box with KVM | SoftRoCE (`rxe`) over virtio-net between two QEMU VMs | All figures *run* end-to-end and produce CSVs; only correctness and relative ordering are validated. Intra-host throughput numbers will be in the right ballpark; inter-host numbers are explicitly marked "functional only — not comparable to paper." | Anyone evaluating the codebase. CI default. |
| **T2 Intra-host perf** | One Linux box with KVM, hugepages, ≥8 cores | n/a (intra-host only) | Single VM running the patched kernel image. Reproduces all intra-host figures (msgsize-ipc, corenum-IPC, sharecore-lat, fork-tput, Table 1 intra-host rows) within ~10% of paper. Inter-host figures skipped. | Readers who want to defend the intra-host story without sourcing RDMA hardware or modifying the host kernel. |
| **T3 Full perf** | Two bare-metal hosts with Mellanox ConnectX-4 (or newer), 100 GbE switch, RoCEv2 | Real RDMA | Reproduces every paper figure. Required for inter-host claims. | Reviewers, follow-up work. |

Why not "VMs simulate RDMA":

SoftRoCE preserves verbs *semantics* but not the perf model — it traverses
the kernel networking stack, has no kernel-bypass, no NIC-resident state, no
PCIe DMA. Inter-host RDMA performance under SoftRoCE will be wrong by 1–2
orders of magnitude and not even in the right relative order vs. Linux TCP.
SoftRoCE is therefore a **functional substrate only**. The reproduction
harness refuses to publish inter-host perf numbers when running under SoftRoCE
(it writes them to CSV but tags every row `transport=rxe` and the summary
report omits them with a banner).

Why VMs are appropriate for intra-host perf:

A SocksDirect intra-host transfer is two processes sharing memory inside one
address-space domain. Inside a KVM VM with `-cpu host`, hugepages,
`mem-prealloc`, and CPU pinning, the SHM fast path is within a few percent of
bare metal. The Table 1 microarchitectural effects (CPU cache migration in
particular) survive virtualization because they are intra-VM. We frame Tier-2
results as "SocksDirect is ≥Nx faster than Linux on this hardware" rather
than "you got 23 M msg/s exactly."

## 6. Phased execution plan

### Phase 0 — Triage and decisions (1 week)

**Goal**: resolve the unknowns that bound every later phase.

**Deliverables**:
- Tag the current tree as `v0-research-snapshot` and freeze it.
- Investigate the paper-vs-code LOC gap. Walk every branch, fork, and
  collaborator's tree we can find. Write `docs/MISSING_FEATURES.md` listing
  what the paper describes but the tree does not implement (NFV pipeline,
  share-core scheduling, complete fork-during-IO). Mark each item
  "resurrect from history" or "rewrite from scratch."
- Pin a target kernel version for the zero-copy path. Default
  recommendation: Ubuntu 22.04's 5.15 LTS series. Rationale documented in
  `docs/KERNEL_MODULE.md`.
- Decide LKM-vs-syscall-patch for zero-copy. Build a 1-day spike of an
  ioctl-on-`/dev/socksdirect` LKM doing `alloc_phys`/`virt2phys`/`map_phys`.
  If the spike works, commit to LKM as primary, syscall patch as fallback;
  if not, commit to syscall patch and accept the kernel-version lock-in.
- Audit vendored code (`rdma/hrd_*` is
  HERD; `acmart.cls` etc. are paper templates and don't ship in the source
  release).
- Define the supported-API contract. Draft `docs/API.md` v0 listing every
  intercepted libc function, its support level
  (`ACCELERATED`/`PASSTHROUGH`/`UNSUPPORTED`), and the LTP test that
  validates it.

**Exit criteria**: every later phase can be planned without "we'll figure it
out." `MISSING_FEATURES.md` and `API.md` are merged.

**Risks**: if the LOC gap turns out to mean ~10K lines of unrecoverable
features, Phase 3 + Phase 6 grow significantly and the timeline doubles.
Surface this in Phase 0, not Phase 6.

### Phase 1 — Build hygiene and repo layout (1–2 weeks)

**Goal**: a clean tree that builds cleanly on any modern Ubuntu without
IDE-specific paths.

**Deliverables**:
- Reorganize into the layout in §4.1. One commit per logical move; preserve
  `git blame`.
- Modernize CMake: 3.16+, `target_*` everywhere, `install()` rules,
  `socksdirect-config.cmake` exports for downstream `find_package()`.
- Global rename: `libipc → libsd`, `ipc-common → libsdcommon`, `monitor →
  socksdirect-monitor`, "IPC-Direct" → "socksdirect".
- Delete `__deprecated_*` files. Move `SocksDirect-paper/` to a git
  submodule under `paper/`.
- Compiler hardening: `-Wall -Wextra -Werror -Wconversion
  -fstack-protector-strong -D_FORTIFY_SOURCE=2`, RELRO/NOW link flags.
- Standard out-of-source build (`build/`); rewrite `launch.sh` to not
  reference `cmake-build-debug/`. Or delete it; a working install obviates
  it.
- Add `clang-format` config and run it. Add `clang-tidy` config (warnings
  only at first).

**Exit criteria**: `cmake -S . -B build && cmake --build build && ctest`
succeeds on a fresh Ubuntu 22.04 container with only documented packages
installed. Old binary names produce no grep hits.

### Phase 2 — Configuration, lifecycle, observability (2 weeks)

**Goal**: kill every hardcoded IP, path, and hostname; make the monitor a
real daemon.

**Deliverables**:
- `socksdirect.toml` config loader (use `tomlplusplus`, header-only). Config
  schema as in §4.4. `SOCKSDIRECT_CONFIG` env override.
- Replace the printf-based `DEBUG`/`FATAL`/`ERROR` macros with a leveled
  logger (`spdlog` is fine). Levels via `SOCKSDIRECT_LOG=info|debug|trace`.
  Structured fields: `pid`, `tid`, `conn_id`. Per-process file in
  `/var/log/socksdirect/<pid>.log`.
- Monitor as a real daemon: PID file, `SIGTERM` → graceful drain,
  `Type=notify` systemd unit in `packaging/systemd/`.
- `socksdirect-ctl` CLI talking to the monitor via Unix socket: `status`,
  `connections`, `dump-state`, `reload`, `drain`.
- Stats endpoint on a Unix socket exposing Prometheus text format:
  `socksdirect_connections_total`, `socksdirect_rdma_qp_count`,
  `socksdirect_queue_occupancy_bytes`, `socksdirect_fast_path_total`,
  `socksdirect_zerocopy_bytes_total`. Cheap to add and indispensable for
  debugging perf regressions later.
- Sweep all shell scripts in `bench/` and `reproduce/` to read the
  inventory rather than embedding paths/IPs.

**Exit criteria**: `grep -RE '(10\.[0-9]+\.[0-9]+|/sampa/|/home/(boj|cuity)/|netsys34)'`
returns nothing. Monitor restart leaves connected clients in a defined
state (currently undefined).

### Phase 3 — API completeness and correctness (3–4 weeks; highest risk)

**Goal**: zero TODOs in the supported API surface defined in Phase 0.

**Deliverables**:
- Implement: `dup`/`dup2`/`dup3`, `shutdown` (half-close semantics),
  `vfork`/`clone`/`sigaction` interception, `recvmsg`/`sendmsg` with
  ancillary data, `EPOLLHUP`/`EPOLLRDHUP`, the "Dynamic allocation" path
  (`socket_lib.cpp:564,580`), the unimplemented `fcntl` branches.
- Finish multi-thread socket migration (`lib.cpp:49` TODO): `import_thread_data`
  must produce a fully usable child state.
- Convert internal `assert()` and `FATAL()` on user-input paths into proper
  `errno` returns. `FATAL` is reserved for detected internal corruption,
  not "user passed a weird flag." Audit tracked in
  `docs/error-handling-audit.md`.
- Define and document fault-injection points: what fails, what error
  surfaces. Add corresponding tests in Phase 4.
- Run **LTP** socket suite under `LD_PRELOAD=libsd.so` as a conformance
  gate. Anything passing is in the contract; anything failing is added to
  `docs/API.md` as out-of-scope with rationale.

**Exit criteria**: `grep -RE 'TODO|FIXME|XXX|not implement' src/lib src/monitor`
returns only entries explicitly marked `OUT-OF-SCOPE: see docs/API.md`. LTP
socket suite passes the documented contract.

**Risks**: multi-thread socket migration is described as deferred; finishing
it may require redesigning the per-thread state ownership model. If the
spike in Phase 0 reveals this, allocate an extra week.

### Phase 4 — Tests and CI (2 weeks; runs parallel with Phase 3)

**Goal**: the codebase enforces its own correctness on every PR.

**Deliverables**:
- **Unit tests** (gtest) for `common/`: `locklessq_v{2,3}`, `metaqueue`,
  `darray`, `adjlist_t`, fd remap table, RDMA buffer pool. Target ≥80%
  line coverage on `common/`.
- **Integration tests** (pytest harness) that boot a monitor + N preloaded
  child processes. Scenarios: TCP echo, fork-during-IO,
  accept-storm-with-fork, epoll edge/level, send/recv across page-remap
  boundary, monitor restart with live connections, RDMA failover, syscall
  intercept layering with `pthread_create`.
- **App smoke tests**: nginx, redis-server, iperf3, memcached come up
  under preload and serve a basic request.
- **CI matrix** (GitHub Actions): {Ubuntu 22.04, 24.04} × {with kernel
  module loaded, without} × {SoftRoCE for inter-host, no inter-host}.
  SoftRoCE makes inter-host functional testing possible without hardware.
- **Sanitizer jobs**: ASan + UBSan on the unit + integration suites. TSan
  on a smaller subset because of false positives in the lockless paths.
- **Performance regression gate**: a small fixed set of microbenchmarks
  (intra-host 8 B latency, intra-host throughput, fork latency) runs on a
  self-hosted runner with pinned CPUs; PR fails if regression > 10% vs the
  baseline branch.
- **Coverage** with `lcov`, posted as a PR comment.

**Exit criteria**: every test in the matrix runs on PRs in <30 min wall
clock. Perf-regression gate has caught at least one synthetic regression
during dogfooding.

### Phase 5 — Kernel module and patch (1–2 weeks; runs parallel with Phase 4)

**Goal**: zero-copy ships safely, without forcing readers to rebuild their
kernel.

**Deliverables** (assumes Phase 0 picks LKM as primary):
- Out-of-tree LKM exposing `/dev/socksdirect` with ioctls replacing
  syscalls 333–339: `SD_IOC_ALLOC_PHYS`, `SD_IOC_VIRT2PHYS`,
  `SD_IOC_MAP_PHYS`, etc. Userspace code uses
  `include/socksdirect/zerocopy.h` and never references syscall numbers.
- DKMS metadata in `packaging/dkms/` so `apt install socksdirect-dkms`
  builds and loads the module at install time across kernel upgrades.
- Library auto-detects `/dev/socksdirect` and falls back to `memcpy` mode
  with a one-time `WARN` if absent. Reproduction harness reads this and
  marks zero-copy figures "skipped — kernel module not loaded."
- Fallback path: a proper `git format-patch` series under
  `src/kernel/patches/v5.15/` for environments where syscall semantics are
  required. With `make kernel` to apply, build, and produce a `.deb`.
- Loadable on T2 QEMU image at boot via systemd; T1 image runs without it
  (copy mode).

**Exit criteria**: `modprobe socksdirect` works on a stock Ubuntu 22.04
kernel. With the module loaded, the existing zero-copy throughput
microbenchmark recovers to its bare-metal baseline; without the module, the
test is skipped, not failed.

### Phase 6 — Reproduction harness (3 weeks; the long pole)

**Goal**: one command per figure; readers do not write shell scripts.

**Deliverables**:
- `reproduce/inventory.yml` schema (§4.5). Single source of truth for
  hosts, IPs, NICs.
- `./repro` CLI: `check`, `all`, `<figure-name>`, `clean`, `report`.
  - `check` validates hardware/kernel/module/RDMA, resolves tier=auto,
    prints the resulting tier and what will be skipped.
  - `<figure-name>` runs one figure end-to-end into `results/`.
  - `report` consolidates `results/*` into `results/summary.md` with
    paper-vs-reproduced numbers side by side, banner naming the tier.
- Two-host orchestration via Ansible (or a thin Python `fabric` script).
  Reads `inventory.yml` once; no SSH commands embedded in figure scripts.
- Per-figure dir contains: `run.sh`, `plot.sh`, `expected/`, `README.md`
  describing what the figure shows and how long it takes to reproduce.
- Plot templates: vendor `.plt` files from `paper/eval/` into
  `reproduce/plot-templates/`, parameterized to read CSVs from `results/`.
- **Backfill missing benchmarks** (validated against `MISSING_FEATURES.md`
  from Phase 0):
  - `sharecore-lat` (Fig. eval-context-switch): ping-pong with N processes
    pinned to one core. No driver currently exists.
  - `fork-tput`: wrapper around existing `pot_eval_fork_*` binaries.
  - `tab1-latency-breakdown`: instrument hot paths with `rdtsc` markers,
    dump per-segment cycle counts to CSV. Today this table is hand-built
    and not reproducible.
  - **NFV pipeline app**: NFs as small processes reading 64 B packets
    from stdin, updating counters, writing stdout, driven by a pcap
    reader. Not in tree at all.
- **Containerized baselines**: `bench/baselines/Dockerfile` builds LibVMA,
  RSocket, redis-benchmark, RPClib, nginx 1.10 to known versions.
- **Tier-aware reporting**: `summary.md` includes a banner like
  `Tier 2 reproduction — inter-host figures omitted; intra-host within X%
  of paper.` Inter-host CSVs from RXE-mode runs are tagged
  `transport=rxe` and excluded from the side-by-side table.
- **VM image artifacts** (packer-built):
  - `socksdirect-tier1.qcow2`: stock Ubuntu 22.04 + SocksDirect packages
    + SoftRoCE configured. ~2 GB.
  - `socksdirect-tier2.qcow2`: as above + patched kernel (only if Phase 0
    chose syscall-patch over LKM) and DKMS module preloaded.
  - Vagrantfile and `libvirt` XML for one-command launch:
    `vagrant up tier1` brings up two VMs on a virtual fabric and runs
    `./repro check`.

**Exit criteria**: a fresh checkout on a fresh Ubuntu box can do
`./repro check && ./repro all` and produce `results/summary.md` with at
least Tier-1 numbers, with no manual editing.

### Phase 7 — Packaging, docs, release (1 week)

**Goal**: external readers can install, run the demo, and reproduce in
under 30 minutes.

**Deliverables**:
- Debian + RPM packages: `socksdirect`, `socksdirect-monitor`,
  `socksdirect-dkms`, `socksdirect-tools`. Reverse deps wired.
- systemd unit (`Type=notify`, `Restart=on-failure`,
  `LimitMEMLOCK=infinity` for RDMA pinning).
- README rewrite: 5-minute "build → run nginx demo → see speedup" path.
- `docs/ARCHITECTURE.md`: maps paper §3–§5 to code modules.
- `docs/API.md`: compatibility table, generated from a single source of
  truth that the conformance suite also reads.
- `docs/REPRODUCIBILITY.md`: hardware needed per tier, expected wall-clock
  per figure, how to interpret the tier banner.
- `docs/CONFIGURATION.md`: every knob.
- `docs/TROUBLESHOOTING.md`, `docs/KERNEL_MODULE.md`,
  `CONTRIBUTING.md`, `CHANGELOG.md`.
- v1.0 GitHub release: prebuilt `.deb`/`.rpm`, source tarball, both VM
  images as release artifacts, `summary.md` of the authors' own Tier-3
  reproduction.

**Exit criteria**: a colleague who has not seen the codebase can follow
the README on a fresh laptop and reproduce Tier-1 numbers in under
30 minutes wall clock.

## 7. Cross-cutting workstreams

These run continuously across phases.

- **Naming consistency**: enforced by a CI grep step from end of Phase 1
  onward.
- **Telemetry-friendliness**: any new counter that matters operationally
  exposed via the stats endpoint from Phase 2 onward; do not bolt on later.
- **Security hardening**: `monitor` runs as a trusted daemon. Tighten
  `seccomp` and `Capabilities=` on the systemd unit; audit SHM key
  derivation for collision/spoofing across uids; document the trust
  boundary in `docs/ARCHITECTURE.md`.
- **License + provenance**: track third-party code under
  `docs/THIRD_PARTY.md`. HERD (`rdma/hrd_*`), `tomlplusplus`, `spdlog`,
  any vendored gnuplot templates from the paper.
- **Documentation as you go**: phase exit criteria include the relevant
  doc file existing and being accurate. No "we'll write the docs later"
  phase.

## 8. Open questions to resolve in Phase 0

These block planning beyond Phase 0; do not start Phase 1 without
answers.

1. **Where does the missing ~10K lines live?** Original authors' branches?
   Forks? Lost? Outcome shapes Phase 3 and Phase 6 scope.
2. **LKM vs syscall patch?** Spike must conclude before Phase 5 is
   planned in detail.
3. **Pinned kernel version.** 5.15 LTS is the recommended default;
   confirm or pick another.
4. **License.** Apache-2.0 or MIT recommended; confirm with original
   authors. Affects packaging.
5. **Original authors' availability** to answer questions about deferred
   features. Affects Phase 0 risk on item 1.
6. **CI compute budget.** Self-hosted runner for the perf-regression gate
   needs a dedicated machine with stable thermal characteristics. Who
   owns it?

## 9. Effort estimate and critical path

| Phase | Solo | Two engineers |
|---|---|---|
| 0 — Triage | 1 wk | 1 wk |
| 1 — Build hygiene | 1–2 wks | 1 wk |
| 2 — Config + lifecycle + obs | 2 wks | 1.5 wks |
| 3 — API completeness | 3–4 wks | 2 wks (pair on the hard parts) |
| 4 — Tests + CI | 2 wks | parallel with 3 |
| 5 — Kernel module | 1–2 wks | parallel with 4 |
| 6 — Reproduction harness | 3 wks | 2 wks (parallel with 4–5 once API is stable) |
| 7 — Packaging + docs | 1 wk | 1 wk |
| **Critical path** | **~10–12 wks** | **~6–8 wks** |

The critical path with two engineers is `0 → 1 → 2 → 3 → 6 → 7`. Phase 4
shadows Phase 3 (tests written against the API as it lands). Phase 5
shadows Phase 4 (kernel work is independent of API correctness).

## 10. Risks (load-bearing only)

1. **The 17K vs 7.5K LOC gap is unrecoverable.** If the deferred features
   (NFV pipeline, share-core scheduling, multi-thread migration, fork
   completeness) were never published and have to be rewritten from the
   paper's prose, Phase 3 + Phase 6 grow by ~4 weeks. Mitigation: resolve
   in Phase 0, before timeline commits.
2. **Kernel module is infeasible.** If the zero-copy operations truly need
   syscall semantics, we are locked to a single kernel version forever and
   Tier-2 must ship the patched-kernel VM image, not just an LKM.
   Mitigation: Phase 0 spike de-risks this in 1 day.
3. **Performance regression from cleanups.** Adding logging, asserts-to-errno
   conversion, and config indirection can move numbers 5–10%. Mitigation:
   Phase 4 perf-regression gate; expect a few rounds of "the logger is in
   the hot path."
4. **CI hardware availability.** Real Mellanox 100G in CI is unrealistic;
   SoftRoCE catches functional bugs, not perf. Mitigation: documented in
   `docs/REPRODUCIBILITY.md` that perf reproduction requires the hardware
   listed in paper §6.1.
5. **Original authors unavailable.** Some design decisions are not
   recoverable from code alone (why is `metaqueue` shaped this way; what
   was the planned semantics of the deferred fork path). Mitigation:
   document our best inference and proceed; mark inferred decisions
   `# INFERRED:` in code comments.

## 11. Appendix: file-by-file disposition

| Current | After rewrite | Notes |
|---|---|---|
| `lib/lib.cpp` | `src/lib/preload.cpp` | thread/fork/exec entrypoints |
| `lib/socket_lib.{cpp,h}` | `src/lib/socket.cpp` (split) | split by API family |
| `lib/poll_lib.cpp` | `src/lib/poll.cpp` | finish `EPOLLHUP` |
| `lib/file_lib.cpp` | `src/lib/file.cpp` | |
| `lib/fork.{cpp,h}` | `src/lib/fork.cpp` | finish multi-thread migration |
| `lib/rdma_lib.{cpp,h}` | `src/lib/rdma.cpp` | |
| `lib/setup_sock_lib.*` | `src/lib/monitor_client.cpp` | rename for clarity |
| `lib/pot_socket_lib.*` | `src/bench/lib_pot.cpp` | move with the benchmarks |
| `lib/zerocopy.h` | `include/socksdirect/zerocopy.h` | add `/dev/socksdirect` ioctl wrappers |
| `lib/__deprecated_attachqueue_lib.*` | DELETED | |
| `monitor/main.cpp` | `src/monitor/main.cpp` | add daemon lifecycle |
| `monitor/process.{cpp,h}` | `src/monitor/process.cpp` | |
| `monitor/sock_monitor.*` | `src/monitor/sock.cpp` | |
| `monitor/setup_sock_monitor.*` | `src/monitor/client_listener.cpp` | rename |
| `monitor/rdma_monitor.*` | `src/monitor/rdma.cpp` | |
| `monitor/__deprecated_attachqueue_monitor.*` | DELETED | |
| `common/locklessq_v{2,3}.hpp` | `src/common/queues/` | one file per queue type |
| `common/locklessqueue_n.hpp` | `src/common/queues/n_consumer.hpp` | |
| `common/rdma_locklessqueue_n.hpp` | `src/common/queues/rdma_n_consumer.hpp` | |
| `common/locklessq_v2_rdma.hpp` | `src/common/queues/v2_rdma.hpp` | |
| `common/interprocess_t*.{cpp,hpp,h}` | `src/common/interprocess.cpp` | merge variants |
| `common/rdma{,.cpp,_struct.h,_interprocess_t.cpp}` | `src/common/rdma.cpp` | |
| `common/metaqueue.h` | `src/common/metaqueue.h` | |
| `common/darray.hpp`, `adjlist_t.hpp` | `src/common/` | |
| `common/helper.{c,h}` | `src/common/helper.c` | likely partly absorbed by spdlog |
| `common/setup_sock.h`, `socket.h` | `src/common/protocol.h` | wire protocol between lib and monitor |
| `common/__deprecated_attachqueue.h` | DELETED | |
| `pot/eval_*.{c,cpp}` | `bench/intra/`, `bench/inter/` | regroup by figure |
| `pot/testn.sh` | DELETED (replaced by `reproduce/`) | |
| `data/*/test_*.sh` | DELETED (replaced by `reproduce/figures/*/run.sh`) | |
| `data/socksdirect/setup/eval-setup.sh` | `reproduce/figures/conn-setup/run.sh` | parameterized |
| `demo/*` | `apps/nginx-demo/` | parameterized via inventory |
| `test/test_*` (smoke programs) | `tests/integration/legacy/` | kept as smoke tests, not asserted |
| `test/demo_*` | `apps/`, `bench/` | sort by purpose |
| `rdma/hrd_*` | `src/common/hrd/` | unchanged, attribution preserved |
| `rdma/{latency,throughput}.{cpp,h}` | `bench/baselines/raw-rdma/` | |
| `rdma/*-{client,server}.sh` | `reproduce/figures/.../baselines/` | |
| `zerocopy/zerocopy.c` | `tests/integration/zerocopy_smoke.c` | |
| `zerocopy/linux/` | `src/kernel/patches/v5.15/` (fallback path) | replaced by LKM as primary |
| `CMakeLists.txt` | rewritten | per §4.1 |
| `launch.sh` | DELETED | obsoleted by install |
| `SocksDirect-paper/` | `paper/` (git submodule) | |
