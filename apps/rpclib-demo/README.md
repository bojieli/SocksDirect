# rpclib-demo

Two C programs (`server.c`, `client.c`) implementing a tiny RPC
echo protocol over TCP. Build:

```bash
cmake -S /path/to/repo -B build && cmake --build build -j -- rpclib_server rpclib_client
```

Run:

```bash
LD_PRELOAD=$REPO/build/libsd.so ./build/apps/rpclib-demo/rpclib_server &
LD_PRELOAD=$REPO/build/libsd.so ./build/apps/rpclib-demo/rpclib_client 100000
```

The client prints messages-per-second and median latency. The figure
`reproduce/figures/queue-microbench` measures the queue floor; this
demo measures the lib + queue stack end-to-end.
