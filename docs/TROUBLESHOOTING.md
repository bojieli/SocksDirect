# Troubleshooting

## Build problems

### `fatal error: infiniband/verbs.h: No such file or directory`

Install RDMA development headers:

```bash
sudo apt install libibverbs-dev rdma-core
```

Or build without RDMA:

```bash
cmake -S . -B build -DSOCKSDIRECT_WITH_RDMA=OFF
```

This is the right choice for development on a laptop — you get the
intra-host fast path and the unit/integration tests, without needing
RDMA drivers.

### `error: 'ibv_exp_*' was not declared`

The HERD helper library uses Mellanox OFED experimental verbs that
aren't in stock `rdma-core`. Either install MOFED, or skip HERD:

```bash
cmake -S . -B build -DSOCKSDIRECT_WITH_HERD=OFF
```

The non-HERD inter-host path is being added in Phase 1 of the rewrite.
Until it lands, `-DSOCKSDIRECT_WITH_HERD=OFF` disables `pot_eval_rdma_*`
and `rdma_throughput`/`rdma_latency`.

### `gtest not found`

The CMake test target fetches googletest via `FetchContent` if the
system package is absent. If your build environment can't reach
`github.com`, install the system package instead:

```bash
sudo apt install libgtest-dev googletest
```

## Runtime problems

### Application crashes immediately after `LD_PRELOAD`

Check the libsd log first:

```bash
tail /var/log/socksdirect/$(pgrep -n yourapp).log
```

The most common causes:

1. **Monitor isn't running.** `systemctl status socksdirect-monitor`
   should show "active (running)". The library waits 3 s for the
   monitor's accept socket and then aborts.
2. **Stale monitor socket.** `rm /run/socksdirect/monitor.sock` and
   restart the monitor.
3. **Application uses an unsupported syscall on a socket fd** (see
   `docs/API.md`). Look for `ERROR ... not implemented` in the log.

### `epoll_wait` never returns

If you mix kernel fds (e.g. inotify, signalfd, file fds) with libsd
socket fds in the same epoll set, the per-process epoll thread is
responsible for forwarding kernel events to libsd's signal layer. The
forwarding has known limitations — see the EPOLLHUP entry in
`docs/API.md`.

Workaround: split the epoll set so kernel fds and libsd fds are in
different sets.

### Monitor refuses to start: `Failed to open the shared memory`

The monitor opens a System V SHM region via `shmget`. If a previous
monitor crashed, the region may persist. Clean up with:

```bash
sudo ipcrm -a
sudo systemctl restart socksdirect-monitor
```

(`ipcrm -a` removes ALL System V IPC objects on the host. Use only
when you know nothing else relies on them.)

### Performance much lower than expected

1. Run `./reproduce/repro check` and verify the resolved tier.
   Tier 1 numbers are functional only.
2. Check that hugepages are mounted and have free pages:
   `grep Huge /proc/meminfo`.
3. Disable Turbo Boost / SMT for stable numbers
   (`echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo`).
4. Pin processes to specific cores (`taskset -c 2 ...`).
5. Confirm `/dev/socksdirect` exists if your message size is ≥16 KiB
   — without the kernel module, libsd silently falls back to memcpy
   mode.

## Test failures

### `test_locklessq_v2` fails under `-O3` only

Historical bug in `common/locklessq_v2.hpp`: `atomic_copy16` lacked a
`"memory"` clobber on its inline asm, so GCC at -O2/-O3 could hoist
reads across the SSE asm. **Fixed in Phase 1**; the test's
`compiler_memory_fence()` helper is now functionally redundant but
kept for grep-ability.

### `test_locklessq_v3` reports leaks under ASan

Was a known leak: `locklessq_v3::init_ptr` allocated two block
descriptors with `new` and the class had no destructor. **Fixed in
Phase 1** by adding a destructor that frees the block pair.
`tests/asan.supp` no longer suppresses it.

### TSan crashes with "unexpected memory mapping"

Known interaction between TSan and PIE binaries on this glibc/kernel
combo. The CI uses ASan + UBSan for sanitizer coverage; TSan is
opt-in only via `-DSOCKSDIRECT_WITH_TSAN=ON` and may need
`-no-pie`.

## Reporting issues

Include in your report:

- `./reproduce/repro check` output
- Output of `cmake -S . -B build` (configuration summary)
- The libsd log (`/var/log/socksdirect/<pid>.log`)
- `dmesg | tail` if the monitor crashed
- Kernel version (`uname -r`), distro (`lsb_release -a`)
