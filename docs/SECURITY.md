# Security model

What SocksDirect protects against, what it doesn't, and how to reason
about deploying it.

## TL;DR

- SocksDirect is for **trusted-coresident** workloads on a single
  host: containers in the same pod, microservices on the same VM,
  processes that already share the same security boundary.
- It is **not** a multi-tenant isolation layer. A malicious
  preloaded application can reach into other libsd-using processes
  in ways the kernel TCP stack would prevent.
- The kernel module gives `socksdirect`-group members effectively
  arbitrary page-mapping privileges on their own VMA. **Treat
  membership as you would VFIO or DPDK group membership.**

## Trust boundary

```
                       trusted
                          │
   ┌──────────────┐       │       ┌──────────────┐
   │ Application  │       │       │ Application  │
   │ (process A)  │       │       │ (process B)  │
   │   libsd.so ──┤       │       ├── libsd.so   │
   └──────┬───────┘       │       └───────┬──────┘
          │ unix socket   │   unix socket │
          ▼               │               ▼
   ┌──────────────────────┴──────────────────────┐
   │  socksdirect-monitor (root or "socksdirect")│
   │  - allocates port numbers                   │
   │  - holds master fd-remap state              │
   │  - brokers SHM keys between peers           │
   └─────────────────────────────────────────────┘
                          │
                          ▼
                   /dev/socksdirect
                   (LKM, kernel space)
```

**Trusted**: the monitor daemon, the kernel module, the system
administrator. The monitor is the only process with authoritative
state; libsd-side clients are clients.

**Untrusted**: applications. They link `libsd.so` and run with their
own uid. A compromised application can do at most what the kernel
TCP stack allowed it to do *plus* what the libsd accelerated path
adds — and the latter is the surface this document describes.

## What SocksDirect protects

1. **Cross-process SHM key collisions**. Per-uid key derivation
   prevents two unrelated apps from accidentally connecting to each
   other's rings. (Defense-in-depth, not a security guarantee.)
2. **Monitor-state spoofing**. Clients communicate with the monitor
   over a 0660-mode Unix socket; only members of the `socksdirect`
   group can connect. The protocol does not allow a client to
   forge state on behalf of another client.
3. **Kernel-module ABI break**. The userspace shim enforces a
   major-version match with the LKM and refuses to use a mismatched
   module. Prevents accidental data corruption from running an
   incompatible userspace against a stale kernel module.
4. **Resource exhaustion via SHM ring growth**. Rings are
   pre-allocated from a bounded hugepage pool; alloc failures fall
   back to copy mode rather than spinning up unbounded memory.

## What SocksDirect does NOT protect

1. **Hostile multi-tenancy**. If two uids both have access to
   `/dev/socksdirect` (for performance), one can in principle
   manipulate the other's page mappings via `SD_IOC_MAP_PHYS`. Don't
   add untrusted users to the `socksdirect` group.
2. **Confidentiality of intra-host data**. Messages on the SHM rings
   are not encrypted. Anyone with read access to the SHM region can
   sniff. (The kernel TCP stack has the same property over the
   loopback interface.)
3. **Denial-of-service**. A misbehaving application can saturate the
   SHM ring or hold connections open indefinitely. The monitor logs
   but does not enforce per-client quotas. If you need quotas, run
   inside cgroups; libsd respects the kernel limits.
4. **Side channels through shared CPU caches**. `share-core`
   scheduling intentionally co-locates senders and receivers on the
   same CPU; if one side is malicious, this gives it richer
   timing-channel signal than vanilla Linux. Don't run mutually
   distrusting workloads on the same physical core *for performance
   reasons*.
5. **Bypass via the legacy non-libsd path**. Applications that don't
   `LD_PRELOAD libsd.so` use the kernel TCP stack directly and are
   unaffected by anything here.

## Kernel module risks

The LKM exposes `/dev/socksdirect` mode 0660 group `socksdirect`. A
process with this fd open can:

- Allocate physically-contiguous pages.
- Look up the PFN of any user-virtual page in *its own* address
  space.
- Replace a page in *its own* VMA with a previously-allocated page.

The module does **not** allow:

- Reading or writing arbitrary kernel memory.
- Looking up another process's VMA contents (the
  `get_user_pages_remote` calls operate on `current->mm`).
- Mapping pages outside a `VM_MIXEDMAP` VMA established via
  `mmap(/dev/socksdirect, ...)`.

But — anyone in the `socksdirect` group can call `SD_IOC_MAP_PHYS`
on their own pages, which means they can do everything `mmap()` lets
them do plus a few page-walking primitives. **This is the same trust
model as DPDK / VFIO / io_uring**: powerful, but inappropriate for
hostile multi-tenancy.

If you're worried, you have three options:

1. Don't install the DKMS module. libsd falls back to copy mode for
   ≥16 KiB messages. You lose some peak throughput on the largest
   transfers but keep the kernel surface vanilla.
2. Set `min_message_size` higher in `[zerocopy]` so the LKM is used
   on a smaller surface.
3. Restrict `socksdirect` group membership to specific service
   accounts (the systemd unit's `User=` would be a natural place).

## Monitor permissions

The monitor's control socket is 0660 owned by `socksdirect:socksdirect`
by default. The systemd unit applies:

```
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
ProtectKernelTunables=true
ProtectKernelModules=true
ProtectControlGroups=true
PrivateTmp=true
RestrictSUIDSGID=true
RestrictNamespaces=true
LockPersonality=true
RestrictRealtime=true
SystemCallArchitectures=native
SystemCallFilter=@system-service
SystemCallFilter=~@privileged @mount @raw-io @reboot @swap @cpu-emulation @debug @keyring @module
CapabilityBoundingSet=CAP_IPC_LOCK CAP_NET_RAW CAP_NET_ADMIN
AmbientCapabilities=CAP_IPC_LOCK
```

`CAP_IPC_LOCK` is needed for `ibv_reg_mr` (RDMA memory pinning).
`CAP_NET_RAW` and `CAP_NET_ADMIN` come from the legacy monitor's
RDMA QP setup; the new daemon doesn't strictly need them and we'll
narrow the bounding set as Phase 3 lands.

## Reporting a vulnerability

For issues you believe affect downstream users:

1. **Don't open a public issue.** Email the project maintainer:
   **`bojieli@gmail.com`** (Bojie Li). PGP not currently
   available; use the address for initial contact and we'll
   move to a side channel if the report is sensitive.
2. Include a reproducer, the package versions, and the kernel
   version (`uname -a`).
3. We follow a 90-day coordinated-disclosure window unless the
   issue is being actively exploited.

For issues that are clearly low-risk (e.g. a `WARN` log instead of
an `INFO` log), a regular GitHub issue is fine.

## Threat model the rewrite plan adds

`REWRITE_PLAN.md` §7 (cross-cutting workstreams) calls for tighter
seccomp + capability sets on the systemd unit and an audit of SHM
key derivation. Phase 3 completes the seccomp set; later work
hardens the per-uid SHM key collision-resistance proof.
