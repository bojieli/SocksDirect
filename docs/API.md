# SocksDirect API compatibility

SocksDirect intercepts libc functions via `LD_PRELOAD`. Each
intercepted function is in one of three categories:

- **ACCELERATED** — handled by libsd's fast path; the kernel is bypassed.
- **PASSTHROUGH** — libsd recognizes the call but forwards it to glibc
  unchanged. Listed here so behavior is predictable and tested.
- **UNSUPPORTED** — calling this function on a libsd-managed fd returns
  an error or is a no-op. Documented so users don't ship surprises.

The conformance suite at `tests/conformance/` (Phase 4) will run the
relevant LTP tests against each function under preload and gate
membership in the ACCELERATED column. The unit + integration matrix
under `tests/unit/` and `tests/integration/` is in place today; the LTP
runner is the next addition (see `REWRITE_PLAN.md` Phase 4).

## Sockets

| Function | Status | Notes |
|---|---|---|
| `socket(AF_INET, SOCK_STREAM, ...)`  | ACCELERATED | Other family/type combos pass through |
| `socket(AF_INET6, ...)`              | PASSTHROUGH | IPv6 not yet on the fast path |
| `socket(AF_UNIX, ...)`               | PASSTHROUGH | |
| `bind` / `listen` / `accept` / `accept4` | ACCELERATED | |
| `connect`                            | ACCELERATED | Local destinations short-circuit through SHM |
| `send` / `recv`                      | ACCELERATED | |
| `sendto` / `recvfrom`                | ACCELERATED for connected sockets; PASSTHROUGH otherwise |
| `sendmsg` / `recvmsg`                | PARTIAL | Ancillary data passes through; payload uses fast path. **Phase 3 fix.** |
| `sendfile`                           | PASSTHROUGH | |
| `read` / `write` / `readv` / `writev` | ACCELERATED on socket fds; PASSTHROUGH on others |
| `shutdown`                           | UNSUPPORTED | Currently silently ignored. **Phase 3 fix.** |
| `getsockopt` / `setsockopt`          | PARTIAL | TCP_NODELAY, SO_REUSEADDR, SO_REUSEPORT, SO_KEEPALIVE supported; others passthrough |
| `getsockname` / `getpeername`        | ACCELERATED | |
| `socketpair`                         | PASSTHROUGH | |

## Multiplexing

| Function | Status | Notes |
|---|---|---|
| `epoll_create` / `epoll_create1`   | ACCELERATED |  |
| `epoll_ctl`                        | ACCELERATED | Mixed kernel + user fds via per-process epoll thread |
| `epoll_wait` / `epoll_pwait`       | ACCELERATED |  |
| `EPOLLIN` / `EPOLLOUT` / `EPOLLERR` | ACCELERATED | |
| `EPOLLHUP` / `EPOLLRDHUP`          | UNSUPPORTED | **Phase 3 fix.** |
| `select` / `pselect`               | PASSTHROUGH | Use epoll for accelerated paths |
| `poll` / `ppoll`                   | PARTIAL | Slow path; use epoll for performance |

## File descriptors

| Function | Status | Notes |
|---|---|---|
| `close`                            | ACCELERATED | |
| `dup` / `dup2` / `dup3`            | UNSUPPORTED on socket fds | **Phase 3 fix.** Returns -1 + ERROR log |
| `fcntl(F_GETFL/F_SETFL/F_GETFD/F_SETFD)` | ACCELERATED | |
| `fcntl(F_DUPFD)`                   | UNSUPPORTED | **Phase 3 fix.** |
| `fcntl(F_NOTIFY/F_GETLEASE/...)`   | PASSTHROUGH | |
| `ioctl(FIONBIO/FIONREAD/SIOCGIFADDR)` | ACCELERATED | |
| `ioctl(other)`                     | PASSTHROUGH | |

## Process lifecycle

| Function | Status | Notes |
|---|---|---|
| `fork`                             | ACCELERATED | Per paper §4.4 |
| `vfork`                            | UNSUPPORTED | **Phase 3 fix.** |
| `clone`                            | UNSUPPORTED | **Phase 3 fix.** |
| `pthread_create`                   | ACCELERATED | Child thread inherits remap table |
| `exec*`                            | PASSTHROUGH | libsd is reloaded in the new image |
| `daemon`                           | UNSUPPORTED | **Phase 3 fix.** |
| `sigaction`                        | PASSTHROUGH | (until Phase 3) |

## Files

Files are not the SocksDirect target, but the preload runtime needs to
dispatch correctly between socket fds and file fds.

| Function | Status | Notes |
|---|---|---|
| `open` / `openat` / `creat`        | PASSTHROUGH | |
| `read` / `write` / `pread` / `pwrite` (on file fds) | PASSTHROUGH | |
| `mmap` / `munmap`                  | PASSTHROUGH | |

## How to read this table

- **PARTIAL** is the dangerous category — the function returns success
  but doesn't necessarily do what userland expects. Phase 3 audits
  every PARTIAL entry and converts it to ACCELERATED or UNSUPPORTED.
- **UNSUPPORTED** is loud — it logs an error to libsd's log
  (`/var/log/socksdirect/<pid>.log`) and returns -1 with `errno=ENOSYS`
  or sets `errno=EOPNOTSUPP`.
- The conformance suite is the source of truth. Today the table is
  hand-maintained against `tests/conformance/coverage.toml`; the
  doc-generator script (Phase 4) is forthcoming. The CI job
  `integration-conformance` validates that every entry in
  `coverage.toml` has a runnable case under `tests/conformance/cases/`
  and that the baseline (no-preload) path still works.

## Reporting a missing function

If your application uses a syscall not on this list and breaks under
preload, file an issue with:

1. The libsd log up to the failure.
2. `strace -f -e trace=network,file <your_app>` output (compares with
   and without preload).
3. The application name and version.
