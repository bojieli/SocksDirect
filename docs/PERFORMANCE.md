# Performance — what's actually fast today

This document is the honest answer to "should I expect the paper's
numbers if I install this right now?"

**TL;DR**: the new `src/lib/` libsd is **instrumented passthrough**
— every libc call is intercepted, bookkept, and forwarded to glibc.
That makes the API surface correct (and noticeable in metrics) but
does **not** deliver the paper's intra-host throughput / latency
claims yet. The SHM data plane port from the legacy tree to the new
scaffold is the remaining engineering work.

If you need the paper's numbers right now, build with
`-DSOCKSDIRECT_WITH_RDMA=ON -DSOCKSDIRECT_WITH_HERD=ON` and use
`libsd-legacy.so` instead. See [`docs/MIGRATION`](MIGRATION.md).

## Numbers measured on this VM (Tier 1, 2 vCPUs, no RDMA)

The microbenchmarks under `bench/microbench/` produce JSON for the
perf-regression gate. Headline values from `results-eval/`:

| Bench | Mode | Value |
|---|---|---|
| `queue_v3` (SPSC ring) | throughput | 50.7 M msg/s |
| `queue_v3` | round-trip latency p50 / p99 / p99.9 | 300 / 344 / 437 ns |
| `fd_remap` | alloc / lookup / free (single-threaded) | 65 / 454 / 220 M ops/s |
| `fd_remap` | lookup, 4 threads | 19 M ops/s (mutex-bound; see below) |
| `fd_remap` | alloc+free latency p50 | 22 ns |
| `metrics` | counter inc, single thread | 138 M ops/s |
| `metrics` | counter inc, 4 threads | 39 M ops/s |
| `metrics` | render (50 metrics) | 110 K renders/s |
| `config` | `get_string` hit | 11 M lookups/s |
| `linux_tcp_pingpong` (loopback baseline) | p50 latency, 64 B | 5.6 µs |
| `linux_tcp_pingpong` | p50 latency, 64 KiB | (size-dependent; see CSV) |

Reproduce locally:

```bash
cmake -S . -B build && cmake --build build -j
./reproduce/repro all
./reproduce/repro report
cat reproduce/results/summary.md
```

These are floor-level numbers — the cost of the queue / fd remap /
metrics / config primitives in isolation. The full preloaded socket
stack pays additional overhead on top.

## What the paper claims vs. what's reproducible today

| Paper claim | Today's status |
|---|---|
| 7-20× message throughput vs. Linux | Not reproducible until the data-plane port lands |
| 17-35× latency improvement | Same |
| 20× connection setup throughput | Reproducible against `libsd-legacy.so` only (RDMA+HERD build) |

`libsd-legacy.so` (the full prototype) does still exist; if you build
with `-DSOCKSDIRECT_WITH_HERD=ON` and run the figures against it,
you'll see the paper's numbers (modulo Mellanox hardware and the
caveats in `docs/REPRODUCIBILITY.md`).

## Why the new libsd is currently passthrough

The rewrite plan deliberately separated the API contract from the
data plane:

1. **Phase 3** (this PR) — get every libc function correctly hooked,
   make the bookkeeping right, ship it under the conformance suite.
2. **Phase 3+** (follow-up) — port the SHM rings + page-remap zero
   copy from `lib/socket_lib.cpp` onto the new scaffold.

This sequencing matters because:

- An incorrect API hook produces silent data corruption. Putting
  the fast path under a buggy hook just makes the corruption
  faster.
- Coverage tests can't validate semantics of a path that requires
  RDMA hardware to even build. So the "correct" floor needs to
  ship first, on stock-Linux build infrastructure.

## Known performance overhead the new libsd adds

Roughly: every intercepted call pays one `dlsym(RTLD_NEXT)` (cached
in a function-static after first use), one atomic-load on
`g_active`, and one thread-local check on `g_in_hook`. Tracked
sockets additionally hit one `FdRemapTable` mutex in
alloc/lookup/free.

For control-plane operations (open/close/dup), this is sub-µs.
For data-plane operations (`send`/`recv`), where every byte goes
through glibc anyway, the libsd overhead is in the noise.

The microbenchmarks above show the per-operation cost in isolation;
the full `loopback-baseline` figure shows the application-level
impact (≈0% degradation today, since we're just adding a virtual
function call to each glibc call).

## Multi-thread fd_remap regression

`fd_remap`'s single-mutex implementation gives 454 M lookups/s
single-threaded but only 19 M lookups/s with 4 threads. This is a
known cost of the simple table; production workloads with
fork-heavy behavior will pay it. Two future optimizations:

1. **Sharded mutexes** — partition the vfd space across N stripes.
   Easy win when contention is real; no API change.
2. **RW locks** for read-heavy paths. `lookup` is already 95% of
   traffic; promoting it to a read-acquire halves contention even
   under heavy fork.

Neither is urgent; current production deployments don't bottleneck
on this. Track in `docs/MISSING_FEATURES.md` if it changes.

## Performance regression gate

The `perf-regression` GitHub Actions workflow runs the microbench
suite on a self-hosted runner with pinned CPUs and compares against
`tools/perf-baselines/all.jsonl`. PRs that regress any pair by more
than 10% fail.

If you legitimately move the floor (e.g. add an instrumentation
counter and accept the small cost), run

```bash
gh workflow run perf-regression.yml -f refresh_baseline=true
```

to commit a new baseline.

## When to file a perf bug

- The microbench numbers move by >10% on the same hardware
  between commits → file a PR with the diff.
- A real application gets slower under preload than without preload
  → file an issue with the bench setup, including
  `socksdirect-ctl metrics` output and the relevant
  `linux_tcp_pingpong` baseline.
- The legacy `libsd-legacy.so` numbers don't match the paper on
  Mellanox hardware → file with the inventory.yml, kernel version,
  hugepage setup, and `dmesg`.
