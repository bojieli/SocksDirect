# SocksDirect

User-space, drop-in BSD socket replacement that bypasses the kernel for
intra-host TCP and uses RDMA for inter-host. The original research
prototype is described in the SOCKSDIRECT paper (`paper/`); this tree
is the in-progress rewrite that turns the prototype into a system you
can install, reproduce, and contribute to. See `REWRITE_PLAN.md` for
the multi-phase plan and `CHANGELOG.md` for what has landed so far.

> Status: rewrite in progress. The user-facing tooling (`socksdirect-ctl`,
> `reproduce/repro`, microbenchmarks, kernel-module skeleton) and tests
> are in place. The fast-path library and monitor daemon still build
> from the legacy `lib/`/`monitor/` trees and are being migrated under
> `src/lib/`+`src/monitor/` per Phase 1–3 of the plan.

## Quick start (no RDMA, ~5 minutes on stock Ubuntu)

```bash
sudo apt-get install -y cmake build-essential libgtest-dev googletest python3-pytest
git clone <this repo> socksdirect && cd socksdirect

cmake -S . -B build -DSOCKSDIRECT_WITH_RDMA=OFF
cmake --build build -j
ctest --test-dir build --output-on-failure
```

You should see 16/16 tests pass: unit tests for every header in
`include/socksdirect/`, plus integration tests for `socksdirect-ctl`,
the `reproduce/repro` harness, the `perf_regression.py` script, the
microbench JSON contract, and the kernel/userspace ABI consistency
check.

## Quick start (RDMA, full library)

You need a Mellanox ConnectX-4 or newer, libibverbs, and Mellanox OFED
for the HERD helper. See `docs/TROUBLESHOOTING.md` for the build flags.

```bash
sudo apt-get install -y libibverbs-dev rdma-core libmemcached-dev libnuma-dev
cmake -S . -B build -DSOCKSDIRECT_WITH_RDMA=ON -DSOCKSDIRECT_WITH_HERD=ON
cmake --build build -j
```

This builds `libsd.so` (the preload library) and `socksdirect-monitor`
(the per-host daemon). With `LD_PRELOAD=/path/to/libsd.so` your TCP
application will route intra-host traffic over shared memory and
inter-host traffic over RDMA. The supported API surface is documented
in `docs/API.md`.

## Reproducing the paper

```bash
cp reproduce/inventory.example.yml reproduce/inventory.yml
$EDITOR reproduce/inventory.yml         # set hostnames / NICs / IPs
./reproduce/repro check                 # auto-detect tier
./reproduce/repro all                   # run every figure for that tier
./reproduce/repro report                # produce results/summary.md
```

Three reproduction tiers, auto-detected from the inventory + your
hardware:

- **Tier 1** — any Linux box. SoftRoCE for inter-host transport;
  inter-host *perf* numbers are explicitly excluded from the report.
  Intra-host numbers are functional only.
- **Tier 2** — one Linux box with KVM, hugepages, ≥8 cores. Reproduces
  every intra-host figure within ~10 % of the paper.
- **Tier 3** — two bare-metal hosts with Mellanox ConnectX-4 (or newer)
  and a 100 GbE switch. Reproduces every figure.

See `docs/REPRODUCIBILITY.md` for which figures land at which tier and
what to check if your numbers diverge.

## Operating the monitor

```bash
sudo cp packaging/systemd/socksdirect-monitor.service /etc/systemd/system/
sudo systemctl enable --now socksdirect-monitor

# Check status / connections / config from any user with access to the
# control socket:
socksdirect-ctl status
socksdirect-ctl connections
socksdirect-ctl reload
```

`socksdirect-ctl` is a thin wrapper around the newline-delimited JSON
protocol in `include/socksdirect/monitor_ipc.hpp`. Its op surface is
extensible without bumping the binary.

## Layout

```
include/socksdirect/    Public, header-only stop-gap APIs (config, log,
                        metrics, monitor_ipc, fd_remap, zerocopy).
src/kernel/             Out-of-tree LKM exposing /dev/socksdirect.
tools/                  socksdirect-ctl + Python tooling
                        (perf_regression.py, check_kernel_abi.py).
bench/microbench/       Standalone microbenchmarks. JSON output;
                        consumed by tools/perf_regression.py.
tests/unit/             gtest-based unit tests for every header.
tests/integration/      pytest-based integration suites
                        (ctl, repro CLI, perf gate, ABI drift, bench
                        smoke).
reproduce/              Reproduction harness + per-figure scripts.
packaging/              Debian rules, DKMS metadata, systemd unit,
                        example config.
docs/                   ARCHITECTURE / API / CONFIGURATION /
                        REPRODUCIBILITY / TROUBLESHOOTING /
                        KERNEL_MODULE.
common/, lib/, monitor/ Legacy trees from the research prototype.
                        Phase 1–3 of the rewrite migrates these into
                        src/. Don't add new code here.
```

## Documentation

| Document | Purpose |
|---|---|
| [`REWRITE_PLAN.md`](REWRITE_PLAN.md) | The multi-phase plan and exit criteria for each phase. |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Paper sections → code modules. |
| [`docs/API.md`](docs/API.md) | Which libc functions are accelerated, passthrough, or unsupported. |
| [`docs/CONFIGURATION.md`](docs/CONFIGURATION.md) | Every config knob, env-var override, and default. |
| [`docs/REPRODUCIBILITY.md`](docs/REPRODUCIBILITY.md) | Reproduction tiers, per-figure expectations. |
| [`docs/KERNEL_MODULE.md`](docs/KERNEL_MODULE.md) | LKM build, load, and security model. |
| [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) | Build/runtime diagnostics. |
| [`docs/MISSING_FEATURES.md`](docs/MISSING_FEATURES.md) | What the paper describes that this tree doesn't yet implement. |
| [`docs/THIRD_PARTY.md`](docs/THIRD_PARTY.md) | Vendored / fetched dependencies and their licenses. |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | What we accept; how to run the test matrix. |
| [`CHANGELOG.md`](CHANGELOG.md) | User-facing changes. |

## License

See the project root.
