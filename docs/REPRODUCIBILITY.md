# Reproducing the SocksDirect paper

This document tells a reader how to reproduce the figures and tables
from the SocksDirect paper, on whatever hardware they have. It is
honest about what is and is not reproducible at each tier.

## TL;DR

```bash
git clone <this repo>
cd socksdirect
cmake -S . -B build && cmake --build build -j
cp reproduce/inventory.example.yml reproduce/inventory.yml
$EDITOR reproduce/inventory.yml         # set hostnames/NICs/IPs
./reproduce/repro check                 # auto-detect tier
./reproduce/repro all                   # run every figure for that tier
./reproduce/repro report                # produce results/summary.md
```

## Reproduction tiers

The harness runs at one of three tiers, auto-detected by `./repro check`:

| Tier | Hardware                                          | Inter-host transport       | Faithfulness                               |
|------|---------------------------------------------------|----------------------------|--------------------------------------------|
| **1** | Any Linux box with KVM                            | SoftRoCE (`rxe`) over virtio-net | All figures *run*; only correctness and relative shape are validated. Inter-host *perf* numbers are excluded from the side-by-side comparison because SoftRoCE traverses the kernel stack. |
| **2** | One Linux box with KVM, hugepages, ≥8 cores       | n/a                         | Reproduces all intra-host figures (msgsize-ipc, corenum-IPC, sharecore-lat, fork-tput, Table 1 intra-host rows) within ~10% of paper. Inter-host figures skipped. |
| **3** | Two bare-metal hosts, Mellanox ConnectX-4 (or newer), 100 GbE switch, RoCEv2 | Real RDMA | Reproduces every paper figure. Required for inter-host claims. |

You don't have to pick the tier; `./repro check` resolves
`tier: auto` against your inventory and prints what it chose.

## Why VMs are the default — and what they can't do

Installing the SocksDirect kernel module on a host kernel risks a
crash if the module misbehaves, and it requires either a DKMS-friendly
LKM (Phase 5) or a syscall-patched kernel rebuild. Neither is something
you should impose on a reader. So Tier 1 and Tier 2 use a packer-built
QEMU image with the patched kernel pre-installed.

For inter-host RDMA, a common temptation is to "simulate" with two VMs
over virtio-net plus SoftRoCE (`rxe`). **Don't compare those numbers
against the paper.** SoftRoCE preserves verbs *semantics* but not the
perf model — it goes through the kernel networking stack, has no
kernel-bypass, no NIC-resident state, no PCIe DMA. Inter-host RDMA
performance under SoftRoCE will be wrong by 1–2 orders of magnitude
and not even in the right relative order vs. Linux TCP. The harness
will run your inter-host figures, write CSVs tagged `transport=rxe`,
and **exclude them** from the comparison report. Use SoftRoCE for
correctness testing, not perf.

For intra-host SHM, KVM with `-cpu host`, hugepages, `mem-prealloc`
and CPU pinning is within a few percent of bare metal. Tier 2 results
are valid for intra-host claims.

## Per-figure expectations

Each figure under `reproduce/figures/<name>/` has its own README with
expected wall-clock duration, what it measures, and how the result
maps to the paper. The summary report aggregates them.

| Figure                       | Tier required | Expected wall-clock | Status |
|------------------------------|----------------|---------------------|--------|
| `queue-microbench`           | 1              | ~30 s               | landed |
| `control-plane-overhead`     | 1              | ~30 s               | landed |
| `loopback-baseline`          | 1              | ~1 min              | landed |
| `msgsize-intra`              | 2              | ~3 min              | landed (driver script; needs libsd build) |
| `msgsize-inter`              | 3              | ~4 min              | landed (driver script; needs libsd + 2 hosts) |
| `corenum-intra`              | 2              | ~5 min              | landed |
| `corenum-inter`              | 3              | ~5 min              | scaffold (driver TODO) |
| `sharecore-lat`              | 2              | ~2 min              | landed (proxy: queue-only) |
| `fork-tput`                  | 2              | ~3 min              | landed |
| `tab1-latency-breakdown`     | 2              | ~10 min             | landed (partial; queue segment only) |
| `nginx`                      | 2              | ~10 min             | landed |
| `nfv`                        | 2              | ~5 min              | landed |

The two CI-runnable figures are useful as harness smoke checks. The
remaining figures land per Phase 6 of the rewrite plan; `./repro list`
prints whatever is available in your tree.

## What's missing from the paper artifact

The original release does not contain enough to reproduce every figure
even on the right hardware. The full inventory is in
[`docs/MISSING_FEATURES.md`](MISSING_FEATURES.md); the headline items
the rewrite needs to backfill are:

- Share-core latency benchmark (Figure eval-context-switch).
- NFV pipeline application (Figure eval-tun-tput).
- Latency breakdown table instrumentation (Table 1) — the
  paper's table is hand-built from per-component timings; the
  rewrite adds `rdtsc` markers in the hot path.
- Fork-throughput benchmark driver (the binaries exist; no
  driver script).

`./repro check` warns when a figure's reproduction script has not yet
landed.

## Interpreting results

The summary report (`results/summary.md`) shows each figure with
your numbers next to the paper's claimed numbers, **flagged with the
tier you ran at**. The success criterion at Tier 2 is "SocksDirect
is ≥Nx faster than Linux on this hardware" rather than "you got
exactly the same nanoseconds." Absolute numbers depend on CPU,
hugepages, NUMA, kernel version, and how busy the host is.

If your numbers diverge from the paper, before filing an issue:

1. Run `./repro check` and verify the resolved tier matches what you
   intended.
2. Check `dmesg` for kernel module errors.
3. Verify `cat /proc/sys/kernel/perf_event_paranoid` < 2 (some
   measurements need PMU access).
4. Disable Turbo Boost / SMT for stable numbers
   (`echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo`).
5. Confirm hugepages are mounted: `grep Huge /proc/meminfo`.
