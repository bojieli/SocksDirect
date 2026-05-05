# loopback-baseline — Linux TCP loopback ping-pong

The Linux baseline that intra-host figures compare against.

## What it measures

For message sizes `64, 1024, 4096, 16384, 65536` bytes:
- Round-trip throughput (msg/s, counting each direction).
- Half-RTT latency p50 / p99 / p99.9 (ns).

Runs entirely on `127.0.0.1`. No SSH, no RDMA, no preload, no kernel
module. This figure can produce numbers at every tier.

## Why this exists

Every claim "SocksDirect is Nx faster than Linux at message size M"
needs a Linux number measured on the same hardware. Producing it via
the harness means a reader can reproduce the *comparison context*
even at Tier 1, where they cannot reproduce SocksDirect's own
numbers.

## Output

`result.csv` columns: `iter, bench, submode, mode, msg_size,
throughput_mps, p50_ns, p99_ns, p999_ns`. Three iterations per
size for noise.

## How to interpret

The numbers are noisy on a shared CPU — for tight numbers, pin to
isolated cores (`taskset`) and disable Turbo Boost.
