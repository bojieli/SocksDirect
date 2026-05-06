# Missing features

The paper claims ~17 K lines of code; the current `common/` + `lib/` +
`monitor/` trees total ~7.5 K. The delta is real — features described
in the paper are absent or partial in the open-source release. This
document is the single source of truth for what's missing, what we
think happened, and how each item is being tracked.

This is the Phase 0 deliverable from `REWRITE_PLAN.md` §6. It is
updated when items land or when fresh investigation changes our
inference.

## Method

For each gap we list:

- **Symptom in code** — file:line where the absence is visible.
- **What the paper describes** — section / figure that depends on it.
- **Disposition** — `RESURRECT` (recover from a fork/branch),
  `REWRITE` (re-implement from the paper's prose), or `INFERRED`
  (best-guess design when neither is possible).
- **Tracker** — where the work is in `REWRITE_PLAN.md`.

## Items

### `dup` / `dup2` / `dup3` on socket fds

- **Symptom**: `lib/socket_lib.cpp:2110-2149` — log an `ERROR` and
  bail; no fd-table refcount.
- **Paper**: §3.3 mentions duplicate fds across `fork`. The fork
  handling implies refcounting that doesn't exist for the live
  process either.
- **Disposition**: REWRITE. Needs a refcount field in
  `FdRemapTable` (currently absent — see the doc comment in
  `include/socksdirect/fd_remap.hpp`).
- **Tracker**: REWRITE_PLAN.md Phase 3 §6.

### `shutdown` half-close semantics

- **Symptom**: `lib/socket_lib.cpp:714` — silently ignored.
- **Paper**: §3 references "BSD-compatible socket interface", which
  implies `SHUT_RD`/`SHUT_WR`/`SHUT_RDWR`.
- **Disposition**: REWRITE. The SHM ring needs a per-direction
  closed-bit; receivers see EOF from the half they don't own.
- **Tracker**: REWRITE_PLAN.md Phase 3 §6.

### `EPOLLHUP` / `EPOLLRDHUP`

- **Symptom**: `lib/poll_lib.cpp:280` — comment "not supported yet".
- **Paper**: not called out specifically; epoll is in scope per
  §3.1 ("supports epoll").
- **Disposition**: REWRITE. The signal-bit layer needs to surface
  remote-close as EPOLLRDHUP and full-close as EPOLLHUP.
- **Tracker**: REWRITE_PLAN.md Phase 3 §6.

### Multi-thread socket migration

- **Symptom**: `lib/lib.cpp:49` — TODO comment.
- **Paper**: §4.4 fork handling — "When a fork happens, we migrate
  the socket state to the child"; the multi-thread case is left
  ambiguous.
- **Disposition**: REWRITE. `import_thread_data` needs to clone
  the per-thread fd-remap subset for each pthread the parent
  spawned post-fork.
- **Tracker**: REWRITE_PLAN.md Phase 3 §6 + risks.

### `vfork` / `clone` / `daemon` / `sigaction`

- **Symptom**: `lib/lib.cpp:342` — TODO comment.
- **Paper**: §4.4 names `fork`; the family of cloned-process syscalls
  is not addressed.
- **Disposition**: REWRITE. Each variant has a distinct semantics
  for memory and fd inheritance; the work is per-call.
- **Tracker**: REWRITE_PLAN.md Phase 3 §6.

### Dynamic page allocation for SHM rings

- **Symptom**: `lib/socket_lib.cpp:564,580` — `FATAL("dynamic
  allocation not implemented")`.
- **Paper**: §4.2 "we allocate ring buffer pages on demand".
- **Disposition**: REWRITE. The monitor allocates a hugepage pool
  at boot and hands out pages over the control plane.
- **Tracker**: REWRITE_PLAN.md Phase 3 §6.

### Share-core scheduling

- **Symptom**: no driver in tree. The figure
  `eval-context-switch` in the paper has no `pot/eval_*.c` source.
- **Paper**: §4.6 "cooperative multitasking among coresident
  applications".
- **Disposition**: REWRITE. The library yields when the SHM ring
  is empty; we need a driver process that pins N processes to one
  core and measures ping-pong latency under contention.
- **Tracker**: REWRITE_PLAN.md Phase 6 §6 (`reproduce/figures/sharecore-lat/`).

### NFV pipeline application

- **Symptom**: no `apps/nfv-pipeline/` directory; no NFs in tree.
- **Paper**: §6 evaluation — pipeline of network functions reading
  64 B packets via SocksDirect.
- **Disposition**: REWRITE. Build a tiny set of NFs (firewall,
  meter, NAT) as small processes piping pcap through stdin/stdout
  with libsd preloaded.
- **Tracker**: REWRITE_PLAN.md Phase 6 §6 (`reproduce/figures/nfv/`).

### Latency breakdown table

- **Symptom**: Table 1 in the paper appears hand-built; no
  `pot/eval_*.c` produces it.
- **Paper**: §6 Table 1 — per-segment cycle counts in the hot path.
- **Disposition**: REWRITE. Add `rdtsc` markers to the send/recv
  paths and dump CSV.
- **Tracker**: REWRITE_PLAN.md Phase 6 §6
  (`reproduce/figures/tab1-latency-breakdown/`).

### Fork-throughput driver

- **Symptom**: `pot/eval_fork_*` binaries exist but no driver
  script in `data/`.
- **Disposition**: REWRITE. Wrap with a benchmark driver.
- **Tracker**: REWRITE_PLAN.md Phase 6 §6 (`reproduce/figures/fork-tput/`).

### Per-fd refcounting

- **Symptom**: `include/socksdirect/fd_remap.hpp` doc comment —
  "Not yet supported: per-fd refcounting (needed for dup/dup2/dup3)".
- **Disposition**: REWRITE. Required by `dup` (above) and a
  prerequisite for any feature that lets multiple vfds reference
  one real fd.
- **Tracker**: REWRITE_PLAN.md Phase 3 §6.

### ~~Page-table-rewrite for zero-copy ioctls~~ — resolved

- **Was**: `src/kernel/socksdirect_dev.c` — `SD_IOC_VIRT2PHYS{,_VEC}`
  and `SD_IOC_MAP_PHYS{,_VEC}` returned `-ENOSYS`.
- **Now**: implemented via `get_user_pages_remote` for the
  virt-to-PFN lookup, and `vm_insert_page` for the page-table
  substitution. The target VMA must be allocated via
  `mmap(/dev/socksdirect, ...)` so it carries `VM_MIXEDMAP`; the
  userspace `ZeroCopyClient::open()` does this implicitly.
- **Tracker**: closed; landing-and-soak in Phase 5 §6.

### ~~SHM intra-host data plane (Phase 3 keystone)~~ — landed

- **Was**: the new `src/lib/libsd.so` was instrumented passthrough.
- **Now**: implemented. `include/socksdirect/shm_ring.hpp` provides
  the SPSC byte-stream ring; `include/socksdirect/shm_segment.hpp`
  wraps `shm_open` + `mmap` and lays out the header + two rings;
  `src/lib/shm_conn.cpp` performs the monitor handshake during
  accept/connect; `src/lib/socket_api.cpp` routes
  `send`/`recv`/`write`/`read` through the ring when the fd is
  upgraded; `close` drops the segment cleanly. End-to-end
  measurements on a 2-vCPU VM: **16.6× throughput, 13× p50
  latency improvement** vs. vanilla loopback TCP. See
  `docs/PERFORMANCE.md`.
- **Tracker**: closed for intra-host. RDMA port to `src/lib/`
  remains; `libsd-legacy.so` still owns inter-host today.

### Notification primitive (futex on ring)

- **Symptom**: blocking `recv` on the SHM ring spin-yields then
  nanosleeps. Low-latency on the busy path; under many idle
  connections it burns CPU.
- **Disposition**: REWRITE. Replace the spin-yield with a
  futex-on-ring-head wake protocol. The producer wakes the
  consumer after publishing if the ring transitioned from empty
  to non-empty.
- **Tracker**: Phase 3 follow-up.

## Items we believe are absent for good reasons (not gaps)

- **TLS / authentication on the data plane.** Out of scope per the
  paper's threat model.
- **DPDK transport.** RDMA is the single inter-host transport.
- **Non-Linux platforms.** Linux-only by design.

## How this list shrinks

Closing an item is two commits: one that lands the implementation
(with tests), one that removes the entry from this file and adds a
CHANGELOG note. Don't delete an entry without doing both.
