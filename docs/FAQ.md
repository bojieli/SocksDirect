# FAQ

## "Will SocksDirect make my application faster right now?"

Probably not yet. The new `src/lib/` libsd that ships in this tree
is **instrumented passthrough** — it intercepts the API surface
correctly but forwards every call to glibc. The SHM data plane that
delivers the paper's perf claims is being ported onto this scaffold
in a follow-up. See [`docs/PERFORMANCE`](PERFORMANCE.md) for the
honest answer.

If you need the paper's numbers today, build with
`-DSOCKSDIRECT_WITH_RDMA=ON -DSOCKSDIRECT_WITH_HERD=ON` and use
`libsd-legacy.so` (the prototype data-plane). It still works.

## "What's the difference between `libsd.so` and `libsd-legacy.so`?"

| | `libsd.so` (new) | `libsd-legacy.so` |
|---|---|---|
| Built by default | yes | only if `-DSOCKSDIRECT_WITH_HERD=ON` |
| Needs Mellanox OFED | no | yes |
| Correct API surface (dup/shutdown/EPOLLHUP/...) | yes | partial |
| Includes the SHM data plane | no (yet) | yes |
| Recommended for new code | yes | no, only for paper reproduction |

When the data-plane port from prototype to `src/lib/` lands,
`libsd-legacy.so` is removed and `libsd.so` becomes the only library.

## "Do I need the kernel module?"

Only if you want zero-copy on messages ≥ 16 KiB. Without it, libsd
falls back to memcpy mode and emits a one-time WARN. Smaller-message
performance is unaffected.

The module ships as `socksdirect-dkms` — it rebuilds itself on
kernel upgrades and doesn't require you to maintain a custom kernel
tree.

## "Can I run it without RDMA hardware?"

Yes for intra-host (the SHM fast path uses memcpy or zero-copy
locally). For inter-host, you need RDMA on both ends or you'll see
the SoftRoCE numbers, which are explicitly excluded from the
comparison report (they're not representative).

## "Does SocksDirect work in containers?"

Yes; it's tested in Docker and Kubernetes pods. The monitor needs
either a host bind-mount of `/run/socksdirect/` or to run in the
container's PID namespace. SHM rings are namespace-aware.

## "Does it work over `127.0.0.1` between two Docker containers?"

Yes — that's an intra-host scenario as far as libsd is concerned.
Both containers need libsd preloaded and access to the same monitor
(usually via a shared bind-mount of `/run/socksdirect/`).

## "Does it work for IPv6?"

Today: passthrough only (the fast path is IPv4-only). IPv6 fast-path
support is in the Phase 3+ backlog; no scheduled date.

## "Does it work for AF_UNIX sockets?"

No. AF_UNIX is passthrough by design — the kernel already does the
fast intra-host job for that domain.

## "Does it work for UDP?"

No. UDP isn't on the rewrite plan; the paper is TCP-only.

## "Why does my preloaded application show similar performance to vanilla?"

Today, the new libsd is instrumented passthrough — that's expected.
Once the SHM data-plane port lands, intra-host TCP between two
preloaded apps will see the paper's speedups. Inter-host RDMA needs
the legacy build today.

## "How do I know libsd actually loaded?"

```bash
$ socksdirect-ctl metrics | grep socksdirect_lib_socket_total
socksdirect_lib_socket_total 17
```

If that counter is non-zero, libsd is hooking real applications.
You can also check per-process logs at `/var/log/socksdirect/<pid>.log`.

## "What happens if the monitor crashes?"

In-flight applications keep running with their existing SHM rings
intact — the data plane doesn't depend on the monitor for forwarded
traffic. New connection setup (anything that needs the monitor for
port allocation or shared-memory keying) blocks until the monitor
comes back. systemd restarts the daemon automatically.

## "Is there a TUI / web UI?"

Not yet. The CLI is `socksdirect-ctl`; everything is on top of the
NDJSON wire protocol so a web UI would be a small project on top.
Not on the rewrite plan.

## "How do I run multiple monitors on the same host?"

Pass `--control-socket` to each instance:

```bash
sudo systemctl edit socksdirect-monitor.service
# add:
[Service]
ExecStart=
ExecStart=/usr/sbin/socksdirect-monitor --control-socket /run/socksdirect/m1.sock
```

Each preloaded application picks its monitor via
`SOCKSDIRECT_MONITOR_CONTROL_SOCKET=...`.

In production this is rarely useful — one monitor per host scales
fine. The flexibility is mostly for tests.

## "What about Windows / macOS?"

Linux-only by design. The fast paths depend on shared memory
semantics + epoll + RDMA verbs. Porting to BSD-flavored kernels
isn't on the plan.

## "How do I contribute?"

[`CONTRIBUTING.md`](../CONTRIBUTING.md). The short version: each PR
should pass the full test matrix locally (`ctest --test-dir build`
+ `ctest --test-dir build-asan`), and changes to public APIs in
`include/socksdirect/` need a CHANGELOG note.

## "What if my libc function isn't on the support list?"

Then libsd doesn't intercept it; glibc handles it normally. To
*request* support, file an issue with:

1. The function name and the libc man page.
2. A test case in `tests/conformance/cases/` style.
3. Whether you'd like it ACCELERATED, PASSTHROUGH-with-tracking,
   or just PASSTHROUGH.

The conformance suite makes adding these cheap.

## "Why isn't the kernel module loaded by default?"

Loading kernel modules at install time has historically caused
issues — wrong kernel headers, transient build failures from
incompatible toolchains, packaging bugs that make hosts unbootable.
DKMS handles most of that, but the `apt install` flow doesn't load
the module automatically; you do `sudo modprobe socksdirect`. We
think the explicit-load step is worth the (small) friction.

## "Is the codebase under active development?"

Yes; see the [CHANGELOG](../CHANGELOG.md) for what's landed
recently. The [`REWRITE_PLAN.md`](../REWRITE_PLAN.md) at the repo
root tracks what's planned and the phase ordering.

## "I see a `Phase 3 fix` note next to a libc function in API.md. What does that mean?"

It means the function was UNSUPPORTED or PARTIAL in the prototype
and Phase 3 of the rewrite plan converts it to ACCELERATED. The
note disappears from the auto-generated table once the conformance
suite confirms the conversion landed.
