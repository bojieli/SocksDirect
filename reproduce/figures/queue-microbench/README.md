# queue-microbench — SHM queue microbenchmark

Standalone benchmark of the underlying lockless SHM queue. Runs entirely
on one host (no SSH, no RDMA, no kernel module). Useful as a smoke
check that the harness is wired up correctly.

## What it measures

- Throughput of `locklessqueue_t_v3<int, 256>` (single-producer / single-consumer)
- Round-trip latency p50 / p99 / p99.9 (ping-pong via two queue pairs)

## How to interpret

The numbers represent the **floor** of overhead the SocksDirect socket
layer can achieve — anything slower in the full intra-host figures is
attributable to socket_lib, not the queue itself. Used during Phase 3
work to bisect performance regressions.

This is **not** a paper figure. The paper's intra-host throughput
figures (`msgsize-ipc-tput`, `corenum-IPC-tput`) measure the full socket
path, not the raw queue.

## Output

`result.csv` columns: `iter, mode, throughput_mps, p50_ns, p99_ns, p999_ns`.
