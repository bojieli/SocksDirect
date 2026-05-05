# control-plane-overhead — config / fd-remap / metrics overhead

Measures the cost of the control-plane primitives that sit on the
socket setup hot path. Three benches:

- `bench_fd_remap` — virtual-fd alloc/lookup/free throughput, plus a
  multi-thread lookup throughput run.
- `bench_metrics` — atomic counter inc throughput (single + multi
  thread) and rendering throughput.
- `bench_config` — INI lookup throughput (hits, misses, int parse).

## How to interpret

These are floor-level numbers: every accepted / closed connection pays
some fraction of these costs in `socket_lib`. The intra-host
throughput figures in the paper will visibly slow down if any of these
regress more than ~10%.

This is **not** a paper figure. It's a CI-friendly figure for
attributing overhead in the intra-host benchmark. Use the queue
microbenchmark in `queue-microbench/` together with this one to
pinpoint where a regression lives.

## Requirements

Builds without RDMA — works on any Linux box with the `bench` targets
compiled (default).

## Output

`result.csv` columns: `iter, bench, submode, mode, throughput_mps,
p50_ns`. Rows per bench/submode/mode tuple, three iterations each.
