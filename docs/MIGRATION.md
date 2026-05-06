# Migrating from the research prototype

If you've used the original `libsd` (research-prototype) release,
this document tells you what's different in the post-rewrite tree
and what you need to change.

The `REWRITE_PLAN.md` at the repo root is the multi-phase blueprint;
this page is the user-facing summary.

## Names

| Old | New |
|---|---|
| `libipc.so` | `libsd.so` |
| `ipc-common` library | `sdcommon` |
| `monitor` binary | `socksdirect-monitor` |
| Project name "IPC-Direct" | `socksdirect` |

CI fails on any of the old strings appearing in the post-rewrite
tree (`src/`, `include/`, `tests/`, `tools/`, `bench/`, `reproduce/`,
`packaging/`, `docs/`, `apps/`).

## Build flags

| Old | New |
|---|---|
| `-DSOCKSDIRECT_WITH_RDMA=ON` (default) | `OFF` is now the default. |
| `cmake-build-debug/` (IDE) | `build/` (standard out-of-source). |
| Single hardcoded `launch.sh` | Replaced by `install` + systemd. |

A clean post-rewrite build looks like:

```bash
cmake -S . -B build -DSOCKSDIRECT_WITH_RDMA=OFF
cmake --build build -j
ctest --test-dir build
```

The legacy library still exists when you opt into RDMA + HERD; it's
named `sd-legacy` to avoid colliding with the post-rewrite `sd`
target.

## Configuration

The prototype hardcoded paths and IPs in `pot/testn.sh`,
`data/.../*.sh`, `demo/start_nginx.sh`. The post-rewrite tree reads
all of that from one place.

| Old | New |
|---|---|
| `/sampa/home/cuity/...` hardcoded | `/etc/socksdirect/socksdirect.conf` (or `$SOCKSDIRECT_CONFIG`) |
| `10.1.2.4` / `10.2.5.203` | `reproduce/inventory.yml` per host |
| `ssh user@netsys34 ...` in scripts | Ansible playbook (`reproduce/orchestration/`) |
| `cmake-build-debug/...` | `build/` (or `$REPRO_REPO`) |
| Per-script `LD_PRELOAD=...` | One config + env-var override |

See [`docs/CONFIGURATION`](CONFIGURATION.md) for the full schema.

## API surface

The prototype `lib/socket_lib.cpp` had several `FATAL`/return-`-1`
paths for libc functions it didn't implement:

| Function | Old behavior | New behavior |
|---|---|---|
| `dup` / `dup2` / `dup3` (sockets) | log error, return -1 | tracked via FdRemapTable refcounts; works |
| `shutdown` | silently ignored | proper half-close (passthrough today; SHM-aware in Phase 3+) |
| `vfork` | not intercepted, broke fork bookkeeping | passthrough; child gets a fresh libsd init |
| `clone` | not intercepted | passthrough |
| `EPOLLHUP` / `EPOLLRDHUP` | dropped silently | passed through |
| `socket(AF_INET, SOCK_STREAM)` "dynamic alloc" | `FATAL` | tracked + forwarded |

Every entry in [`tests/conformance/coverage.toml`](../tests/conformance/coverage.toml)
gets a runnable case under `tests/conformance/cases/` and is
exercised under preload by `integration-libsd-preload`. See
[`docs/API`](API.md) (auto-generated from `coverage.toml`).

## Reproduction

| Old workflow | New workflow |
|---|---|
| Hand-edit shell scripts in `data/<scenario>/` | Edit `reproduce/inventory.yml` once |
| `bash data/socksdirect/intra/test_*.sh` | `./reproduce/repro msgsize-intra` |
| Hand-collected CSVs / plot in PowerPoint | `./reproduce/repro report` writes `summary.md` |
| Inter-host figures with no tier guard | Tier-aware harness; SoftRoCE results tagged + excluded from comparison |

The harness has graceful skips so a Tier-1 reader doesn't error out
on a Tier-2/3 figure. See [`docs/REPRODUCIBILITY`](REPRODUCIBILITY.md).

## Kernel module

The prototype shipped a partial `zerocopy/linux/` overlay that
hardcoded syscall numbers 333–339, with no patch series and no
kernel pinned. It wasn't safe to apply.

The post-rewrite tree ships an out-of-tree LKM under `src/kernel/`
that exposes the same operations as ioctls on `/dev/socksdirect`.
DKMS-installable; no host-kernel rebuild required. See
[`docs/KERNEL_MODULE`](KERNEL_MODULE.md).

If you have hosts running the old syscall-overlay kernel: no action
needed if you're only running post-rewrite code (libsd auto-detects
`/dev/socksdirect` and falls back to copy mode if absent). If you
want zero-copy, install `socksdirect-dkms`.

## Tests

The prototype's `test/` directory contains smoke programs you ran
manually. The post-rewrite tree has:

- **`tests/unit/`** — gtest unit tests for every header in
  `include/socksdirect/`.
- **`tests/integration/`** — pytest scenarios driving real
  binaries (the monitor, `socksdirect-ctl`, the conformance suite,
  the repro CLI).
- **`tests/conformance/`** — libc-API conformance suite, runs
  in baseline (no preload) and `--preload PATH` modes.

Existing smoke programs under `test/` are kept as
`tests/integration/legacy/` for archaeology.

## Operating

| Old | New |
|---|---|
| `nohup ./monitor &` | `systemctl enable --now socksdirect-monitor` |
| Kill with `pkill monitor` | `systemctl stop socksdirect-monitor` (graceful drain on SIGTERM) |
| No metrics | `socksdirect-ctl metrics` (Prometheus text) |
| No control surface | `socksdirect-ctl status / connections / dump-state / reload / drain` |
| Logs to stderr only | `/var/log/socksdirect/<pid>.log`, level via env or config |

See [`docs/OPERATIONS`](OPERATIONS.md).

## Things that haven't moved yet

- The actual **SHM data plane** (the part of the prototype that
  delivers the paper's perf claims). The new `src/lib/` is
  instrumented passthrough today; the SHM ring + page-remap zero
  copy port lands in a follow-up Phase 3 PR.
- The legacy RDMA inter-host transport still uses HERD experimental
  verbs and ships as `sd-legacy.so`. The Phase 3 port replaces this
  with stock `rdma-core`.
- The benchmark drivers `pot_eval_*` are still in the legacy tree;
  they're the binaries the figure scripts shell out to. They'll be
  rewritten in Phase 6 once the post-rewrite data plane is up.

If your existing setup depends on these, run with the legacy library
(`-DSOCKSDIRECT_WITH_RDMA=ON -DSOCKSDIRECT_WITH_HERD=ON`) for now —
both libraries coexist in the build.

## Migration checklist

For an existing deployment:

1. Run your existing smoke tests against the new tree with the old
   build flags. If they break, that's a regression — file an issue.
2. Switch your build to the new flags (`-DSOCKSDIRECT_WITH_RDMA=OFF`
   for control-plane / passthrough, with the legacy build kept as a
   fallback for the data-plane bits).
3. Replace `monitor &` with the systemd unit.
4. Replace your hand-rolled benchmark scripts with the
   `reproduce/repro <figure>` invocation.
5. Move per-host config out of shell scripts into
   `inventory.yml` + `socksdirect.conf`.

When Phase 3 lands and the post-rewrite data plane goes live, you
can drop `-DSOCKSDIRECT_WITH_HERD=ON` entirely.
