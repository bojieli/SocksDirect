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

## Function-by-function compatibility

<!-- AUTO-TABLE-BEGIN -->

_The table below is generated from [tests/conformance/coverage.toml](../tests/conformance/coverage.toml). Edit that file (and the matching case under `tests/conformance/cases/`) instead of editing this section by hand. CI fails on drift._

### Sockets

| Function | Status | Notes |
|---|---|---|
| `accept` | ACCELERATED |  |
| `accept4` | ACCELERATED |  |
| `bind` | ACCELERATED |  |
| `connect` | ACCELERATED |  |
| `getpeername` | ACCELERATED |  |
| `getsockname` | ACCELERATED |  |
| `listen` | ACCELERATED |  |
| `recv` | ACCELERATED |  |
| `recvmsg` | PARTIAL | **Phase 3 fix.** |
| `send` | ACCELERATED |  |
| `sendmsg` | PARTIAL | ancillary data passthrough; payload accelerated **Phase 3 fix.** |
| `shutdown` | UNSUPPORTED | **Phase 3 fix.** |
| `socket` | ACCELERATED | AF_INET/SOCK_STREAM is fast-path; others passthrough |

### Multiplexing

| Function | Status | Notes |
|---|---|---|
| `epoll_create` | ACCELERATED |  |
| `epoll_create1` | ACCELERATED |  |
| `epoll_ctl` | ACCELERATED |  |
| `epoll_wait` | ACCELERATED |  |

### File descriptors

| Function | Status | Notes |
|---|---|---|
| `close` | ACCELERATED |  |
| `dup` | UNSUPPORTED | **Phase 3 fix.** |
| `dup2` | UNSUPPORTED | **Phase 3 fix.** |
| `dup3` | UNSUPPORTED | **Phase 3 fix.** |
| `fcntl_F_DUPFD` | UNSUPPORTED | **Phase 3 fix.** |
| `fcntl_F_GETFL` | ACCELERATED |  |
| `fcntl_F_SETFL` | ACCELERATED |  |

### Process lifecycle

| Function | Status | Notes |
|---|---|---|
| `clone` | UNSUPPORTED | **Phase 3 fix.** |
| `fork` | ACCELERATED |  |
| `vfork` | UNSUPPORTED | **Phase 3 fix.** |

<!-- AUTO-TABLE-END -->

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

