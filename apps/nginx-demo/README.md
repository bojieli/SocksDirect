# nginx-demo

Run nginx under `LD_PRELOAD=libsd.so` to demonstrate intra-host
acceleration of HTTP traffic.

## Quick start

```bash
cmake -S /path/to/repo -B build && cmake --build build -j
./run-demo.sh
# In another terminal:
./client-demo.sh                   # uses curl
```

The demo binds to 127.0.0.1:58080 and serves a small file. You should
see lower per-request latency under preload than without; for a real
benchmark use `reproduce/figures/nginx/`.

## Inventory-driven variant

For the figure-style harness:

```bash
PORT=58080 ./reproduce/repro nginx
```

That writes `result.csv` under `reproduce/results/nginx/`.
