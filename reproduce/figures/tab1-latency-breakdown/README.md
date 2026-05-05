# tab1-latency-breakdown — paper Table 1

Per-segment cycle counts in the hot path (queue alloc, ring write,
SHM barrier, ring read, queue free, ...). The paper's Table 1 was
hand-built; the rewrite adds `rdtsc` markers in `src/lib/` (Phase 3
follow-up) so the breakdown is reproducible.

This script currently publishes only the queue-segment portion via
`bench_queue_v3`. The full breakdown lands when the lib-side markers
are wired.
