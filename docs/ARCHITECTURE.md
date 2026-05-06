# SocksDirect Architecture

This document maps the paper's design sections to the code modules.
Read the SOCKSDIRECT paper (gitignored as `SocksDirect-paper/`; the
plan adds it as a submodule under `paper/` in a follow-up) first;
this is a navigation aid, not a substitute for it.

## Components

```
┌─────────────────────┐         ┌────────────────────────────┐
│  Application        │         │  Application               │
│  (any libc-based)   │         │                            │
│                     │         │                            │
│  LD_PRELOAD libsd ──┼────┐    │  LD_PRELOAD libsd ─────────┼─┐
└─────────────────────┘    │    └────────────────────────────┘ │
                           │                                   │
                  ┌────────▼─────────┐               ┌─────────▼────────┐
                  │ libsd.so         │               │ libsd.so         │
                  │  src/lib/        │ ◀── SHM ──▶  │                  │
                  │   ├ socket.cpp   │  ring          │                  │
                  │   ├ poll.cpp     │  buffers       │                  │
                  │   ├ fork.cpp     │                │                  │
                  │   └ rdma.cpp     │                │                  │
                  └────────┬─────────┘                └─────────┬────────┘
                           │ Unix-socket control plane          │
                           ▼                                    ▼
                  ┌──────────────────────────────────────────────────────┐
                  │ socksdirect-monitor (one per host)                  │
                  │   src/monitor/                                       │
                  │     ├ main.cpp              process+conn lifecycle   │
                  │     ├ sock_monitor.cpp     port/conn allocation     │
                  │     └ rdma_monitor.cpp     RDMA QP setup            │
                  └──────────────────────────────────────────────────────┘
                           │
                           ▼
                  ┌──────────────────┐
                  │ /dev/socksdirect │  src/kernel/  — page-remap zero-copy
                  └──────────────────┘
```

## Paper section → code

| Paper                                          | Code                                           |
|------------------------------------------------|------------------------------------------------|
| §3 Design — fast path                          | `src/lib/socket.cpp` send/recv hot path       |
| §3 Design — control plane via monitor          | `src/monitor/` and `src/lib/setup_sock_lib.cpp` |
| §3 Design — fd remapping (kernel vs userspace) | `include/socksdirect/fd_remap.hpp`             |
| §4 Optimizations — lockless ring buffer        | `common/locklessq_v3.hpp`                      |
| §4 Optimizations — variable-size SHM ring      | `common/locklessq_v2.hpp`                      |
| §4 Optimizations — adjacency list per fd       | `common/adjlist_t.hpp`, `common/darray.hpp`   |
| §4 Optimizations — page-remap zero copy        | `src/kernel/` (LKM) + `include/socksdirect/zerocopy.h` |
| §4 Optimizations — fork handling               | `src/lib/fork.cpp`, `src/monitor/process.cpp` |
| §4 Optimizations — RDMA inter-host transport   | `src/lib/rdma.cpp`, `common/rdma.cpp`,         |
|                                                | `common/rdma_locklessqueue_n.hpp`              |
| §5 Implementation — preload entry              | `src/lib/preload.cpp` (constructor)            |
| §5 Implementation — kernel-event multiplexing  | `src/lib/poll.cpp` (epoll thread)              |

## How libsd intercepts libc

The preload library replaces a small set of libc functions via
`LD_PRELOAD`. Each hook follows the same shape:

1. **Cache the next-symbol pointer.** `DECLARE_REAL(int, socket, ...)`
   and the matching `REAL(socket)` accessor in `src/lib/intercept.hpp`
   wrap `dlsym(RTLD_NEXT, "socket")` in a function-static, so we pay
   the lookup once per symbol per process.
2. **Short-circuit during bootstrap.** Until the constructor finishes
   (`g_active.load(memory_order_acquire)`) every hook just calls
   `REAL(name)`. This breaks the chicken-and-egg between
   `dlsym → libc → maybe-our-hook` during init.
3. **Re-entrancy guard.** A `thread_local g_in_hook` flag set via
   `ScopedReentrancyGuard` prevents a hook from triggering another
   hook (e.g. `LOG_INFO` writing to stderr while the program is
   inside `write()`).
4. **Bookkeep + forward.** The actual hook body does whatever
   tracking work is needed (FdRemapTable updates, metric
   increments) and forwards to `REAL(name)`.

Hooks are `extern "C"` so the dynamic linker matches them; everything
else in `src/lib/` is `-fvisibility=hidden`. The library has no
exported symbols beyond the libc replacements.

State that survives across hooks lives in `src/lib/state.hpp`. The
constructor allocates a `State` containing a `Config`, an
`FdRemapTable`, the cached metric handles, and the
`monitor_reachable` flag. A magic-static (`State& state()`) gives all
hooks a thread-safe accessor.

## Monitor lifecycle

`src/monitor/main.cpp` is a single-threaded `poll()` event loop:

1. **Startup.** Load config, initialize the structured logger, open
   the control Unix socket (`/run/socksdirect/control.sock` by
   default), allocate the metrics registry, set up `signalfd` for
   SIGTERM/SIGHUP, write the PID file, emit `READY=1` to systemd.
2. **Steady state.** `poll()` watches the accept socket, the
   signalfd, and every active control connection. Each accepted
   connection is `O_NONBLOCK | O_CLOEXEC`; the loop reads
   newline-delimited JSON, dispatches via the op map, writes the
   response, and stays connected for further requests.
3. **SIGHUP.** Triggers `op_reload` — re-read config, reset log
   level/sink. Other in-flight requests are not interrupted.
4. **SIGTERM / SIGINT.** Sets `running = false`. The loop exits
   normally, closes every connection, removes the control socket
   and PID file, emits `STOPPING=1`, returns 0. systemd's
   `TimeoutStopSec=10` provides the upper bound.

Op dispatch is in `dispatch()`. Adding a new op is one entry in the
switch + the corresponding handler. Wire format is in
[`include/socksdirect/monitor_ipc.hpp`](../include/socksdirect/monitor_ipc.hpp).

## Cross-module rules

- `common/` has no I/O, no globals, no logging side effects beyond a
  passed-in logger. Anything testable in isolation belongs here.
- `src/lib/` is the **only** place that intercepts libc functions.
- `src/monitor/` is the **only** place that allocates ports, FDs, and
  shared-memory keys.
- `src/kernel/` exposes `/dev/socksdirect`. Userspace code never refers
  to syscall numbers directly; it goes through
  `include/socksdirect/zerocopy.h`.
- Dependency direction: `lib → common`, `monitor → common`,
  `tools → common`. No back-edges. CI checks via `clang-tidy`.

## Trust boundary

The monitor is the trust root: it is the only process that allocates
port numbers, holds the master fd-remap state, and brokers the SHM
keys two peers share. A misbehaving preloaded application can corrupt
its own state but cannot forge connections it didn't open or read SHM
it wasn't granted access to. The systemd unit constrains the monitor's
syscall surface (see `packaging/systemd/`).

## What's deliberately not here

- A new transport. RDMA stays as the inter-host fast path; the rewrite
  doesn't introduce alternatives.
- A new IPC primitive between the lib and the monitor. The Unix-socket
  control plane is sufficient for the control rate; SHM rings carry
  the data rate.
- Multi-tenant isolation beyond per-uid SHM key derivation. SocksDirect
  is intended for trusted-coresident workloads (containers in the same
  pod, microservices on the same host); for hostile multi-tenancy use
  the kernel.
