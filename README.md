# SocksDirect

User-space, drop-in BSD socket replacement that bypasses the kernel for
intra-host TCP and uses RDMA for inter-host. The research prototype is
described in the SOCKSDIRECT paper (`paper/`); this repository is the
production rewrite that takes the prototype and turns it into a system
you can install, reproduce, monitor, and contribute to.

> **Status (May 2026)**: rewrite phases 1–7 land in `master` and the
> Phase 3 SHM intra-host data plane is live. Two preloaded processes
> talking over `127.0.0.1` exchange messages through a SHM ring
> brokered by the monitor; on this VM that's **16.6× higher
> throughput and 13× lower p50 latency than vanilla loopback TCP**.
> Inter-host RDMA still requires the legacy `libsd-legacy.so` build
> while the RDMA port to `src/lib/` is in progress. See
> [`docs/PERFORMANCE`](docs/PERFORMANCE.md) for the full numbers.

## Documentation map

Start at [`docs/README`](docs/README.md). It points you at the right
document for what you're trying to do (try it / reproduce / deploy /
embed / understand what's missing).

## 5-minute quick start (no RDMA needed)

On a fresh Ubuntu 22.04 / 24.04 box:

```bash
# Install build deps.
sudo apt-get install -y cmake build-essential libgtest-dev \
                        googletest python3-pytest python3-yaml

# Clone + build.
git clone https://github.com/bojieli/SocksDirect.git
cd SocksDirect
cmake -S . -B build -DSOCKSDIRECT_WITH_RDMA=OFF
cmake --build build -j

# Run the full test matrix.
ctest --test-dir build --output-on-failure
```

Expected output:

```
100% tests passed, 0 tests failed out of 23
```

Twenty-three tests: gtest unit tests for every header in
`include/socksdirect/`, plus integration suites for
`socksdirect-monitor`, `socksdirect-ctl`, the `reproduce/repro`
harness, the LTP conformance gate, the microbenchmark JSON schema,
the kernel/userspace ABI gate, and the libsd preload scaffold.

## 5-minute hello world: monitor + ctl + libsd preload

```bash
# Boot the daemon on a temp socket so we don't need root.
./build/socksdirect-monitor --control-socket /tmp/sd.sock --log-level warn &
MON=$!
sleep 0.5

# Talk to it.
SOCKSDIRECT_CTL_SOCKET=/tmp/sd.sock ./build/socksdirect-ctl status
# pid 12345
# uptime_sec 0
# control_socket /tmp/sd.sock
# draining false

SOCKSDIRECT_CTL_SOCKET=/tmp/sd.sock ./build/socksdirect-ctl metrics \
    | grep socksdirect_ctl_requests_total
# socksdirect_ctl_requests_total 2

# Run any program with libsd preloaded; it pings the monitor on startup.
LD_PRELOAD=$(pwd)/build/libsd.so SOCKSDIRECT_LOG=info /bin/echo hello
# hello
SOCKSDIRECT_CTL_SOCKET=/tmp/sd.sock ./build/socksdirect-ctl metrics \
    | grep socksdirect_ctl_requests_total
# socksdirect_ctl_requests_total 4    # the lib's ping is in there

kill -TERM $MON; wait $MON 2>/dev/null
```

## RDMA build (data plane fast path)

To build the legacy data-plane library (`libsd-legacy.so`) for
inter-host RDMA + paper reproduction:

```bash
sudo apt-get install -y libibverbs-dev rdma-core libmemcached-dev libnuma-dev
cmake -S . -B build -DSOCKSDIRECT_WITH_RDMA=ON -DSOCKSDIRECT_WITH_HERD=ON
cmake --build build -j
```

The HERD helper requires Mellanox OFED's experimental verbs; if you
don't have OFED, drop `-DSOCKSDIRECT_WITH_HERD=ON` and the legacy
library is skipped (the new instrumented-passthrough `libsd.so`
still builds).

## Reproducing the paper

```bash
cp reproduce/inventory.example.yml reproduce/inventory.yml
$EDITOR reproduce/inventory.yml         # set hostnames / NICs / IPs
./reproduce/repro check                 # auto-detect tier
./reproduce/repro all                   # run every figure
./reproduce/repro report                # produce results/summary.md
```

Three reproduction tiers, auto-detected:

- **Tier 1** — any Linux box. Functional figures only.
  Inter-host *perf* numbers are excluded from comparison (SoftRoCE
  isn't representative).
- **Tier 2** — one Linux box with KVM, hugepages, ≥8 cores.
  Reproduces every intra-host figure.
- **Tier 3** — two bare-metal hosts with Mellanox ConnectX-4 (or
  newer) and a 100 GbE switch. Reproduces every figure.

See [`docs/REPRODUCIBILITY`](docs/REPRODUCIBILITY.md) for what each
tier can and cannot prove.

## Production install

```bash
# Debian / Ubuntu
sudo apt install ./socksdirect-monitor_*.deb \
                 ./socksdirect_*.deb \
                 ./socksdirect-tools_*.deb

# RHEL / Rocky / Fedora
sudo dnf install ./socksdirect-monitor-*.rpm \
                 ./socksdirect-*.rpm \
                 ./socksdirect-tools-*.rpm

# Optional zero-copy LKM
sudo apt install ./socksdirect-dkms_*.deb
sudo modprobe socksdirect
```

The `postinst` creates the `socksdirect` user/group and starts the
systemd unit. Verify:

```bash
systemctl status socksdirect-monitor
socksdirect-ctl status
```

Full deploy guide: [`docs/OPERATIONS`](docs/OPERATIONS.md).

## Layout

```
include/socksdirect/    Public, header-only stop-gap APIs
                        (config, log, metrics, monitor_ipc,
                        fd_remap, zerocopy{,_client}).
src/lib/                libsd preload library.
src/monitor/            socksdirect-monitor daemon.
src/kernel/             /dev/socksdirect LKM + ABI mirror.
tools/                  socksdirect-ctl source +
                        Python helpers (perf_regression,
                        check_kernel_abi, render_api_doc,
                        scan_error_handling, repro_dry_run).
bench/microbench/       Standalone microbenchmarks (JSON output).
bench/baselines/        Linux reference benchmarks.
tests/unit/             gtest unit tests.
tests/integration/      pytest integration scenarios.
tests/conformance/      libc-API conformance suite +
                        case files + LTP runner.
reproduce/              Paper-figure reproduction harness.
reproduce/figures/      Per-figure run.sh + README.
reproduce/orchestration/ Ansible playbook for two-host runs.
reproduce/vm-images/    Packer manifests + Vagrantfile.
apps/                   Demo / reproduction binaries.
packaging/              .deb, .rpm, DKMS, systemd, docker.
docs/                   Architecture / API / Configuration /
                        Reproducibility / Operations / Security /
                        Migration / Performance / FAQ /
                        Troubleshooting / Kernel module / etc.

common/, lib/, monitor/ Legacy research-prototype trees. Phase 1+
                        of the rewrite migrates these into src/.
                        Don't add new code here.
```

## License

See the project root.
