# msgsize-intra — intra-host throughput vs message size

Reproduces the intra-host curve in paper Figure 3-left (msg/s as a
function of message size, single host, single core).

## Tier

Requires **Tier 2** (libsd built; intra-host SHM fast path active).
Skips cleanly with a sentinel `SKIPPED.txt` otherwise.

## What it measures

Round-trip throughput for a sweep of message sizes from 8 B to 1 MiB.
Compare against `loopback-baseline` for the Linux reference number on
the same hardware.

## Output

- `raw.jsonl` — one JSON object per (size, mode) tuple.
- `result.csv` — flat CSV; columns `iter,bench,submode,mode,msg_size,throughput_mps,p50_ns`.
- `result.pdf` — line chart (gnuplot; only if installed).

## Customization

Override sizes via env: `SIZES="8 64 4096" ./reproduce/repro msgsize-intra`.
