# SocksDirect kernel module

The page-remap zero-copy fast path described in §4 of the paper requires
kernel support: SocksDirect needs to allocate physical pages, look up
virtual-to-physical mappings, and remap pages between processes. This
document describes how the kernel side is shipped and what to do when
it's not available.

## Two delivery options

Per Phase 0 of the rewrite plan, the kernel side ships in **one** of
two ways. The current tree commits to Option A (LKM); the userspace
ABI in `include/socksdirect/zerocopy.h` is locked-in and the kernel
skeleton in `src/kernel/` builds against any modern kernel via DKMS.

### Option A (selected): out-of-tree LKM

A loadable kernel module exposes `/dev/socksdirect` with ioctls that
implement the same primitives the prototype defined as syscalls 333–339.
The userspace library uses `include/socksdirect/zerocopy.h` and never
references syscall numbers directly. The userspace shim
(`include/socksdirect/zerocopy_client.hpp`) auto-detects the device,
falls back to copy mode if absent, and refuses to use a mismatched ABI
major version.

Distribution: `socksdirect-dkms` package. Installs the module source
under `/usr/src/socksdirect-<version>` and registers it with DKMS so
the module is rebuilt automatically on kernel upgrades.

```bash
sudo apt install socksdirect-dkms
sudo modprobe socksdirect
ls /dev/socksdirect
```

Tested kernels: 5.15 LTS (Ubuntu 22.04), 6.8 (Ubuntu 24.04). The build
system uses standard `linux/cdev.h` + `linux/ioctl.h` interfaces; any
LTS kernel from 5.10 onward is expected to work. The
`tools/check_kernel_abi.py` CI gate fails if the kernel-side header
(`src/kernel/socksdirect_dev.h`) drifts from the userspace header
(`include/socksdirect/zerocopy.h`).

### Option B (fallback): syscall patch series

If the LKM approach hits a wall (e.g. one of the operations genuinely
needs syscall semantics), the kernel side ships as a `git format-patch`
series under `src/kernel/patches/v5.15/`. You apply it to a stock
5.15 LTS kernel, build, install, reboot.

Distribution: `socksdirect-kernel-source` package + `make kernel`.

This option locks SocksDirect to a single kernel version forever and
is therefore the fallback. It also makes the Tier-2 reproduction story
require a packer-built QEMU image with the patched kernel
pre-installed, since asking readers to rebuild their host kernel is
not acceptable.

## Detecting whether the module is loaded

The library auto-detects at startup:

```c
// in libsd's constructor, simplified
int fd = open("/dev/socksdirect", O_RDWR);
zerocopy_enabled = (fd >= 0);
```

If `/dev/socksdirect` is absent, libsd falls back to `memcpy` mode for
messages >= `[zerocopy] min_message_size` (default 16 KiB) and emits a
one-time `WARN`:

```
[WARN] socksdirect: /dev/socksdirect not present; falling back to memcpy
       for large messages. See docs/KERNEL_MODULE.md to enable zero-copy.
```

The reproduction harness reads this state and **skips** zero-copy
figures with a clear "skipped — kernel module not loaded" tag. It does
NOT silently produce slower numbers.

## Why this matters

The paper's intra-host latency claim for 1 MiB messages (1/13 of Linux,
26x throughput) depends entirely on page-remap zero-copy. Without the
module:

- Intra-host figures for sizes < 16 KiB are unaffected.
- Intra-host figures for sizes ≥ 16 KiB will look 5–10x worse than the
  paper claims, because libsd is doing two memcpys instead of two page
  remaps.
- Inter-host RDMA figures use a different zero-copy mechanism (RDMA
  scatter-gather) and are not affected.

## Security

The LKM exposes `/dev/socksdirect` with mode `0660` and group
`socksdirect`. Members of that group can request physical-page
allocations and per-page virtual-address remaps. **Anyone in this
group can effectively read/write arbitrary pages**, so:

- Add only the user the monitor runs as.
- Do not add applications or random users.
- The systemd unit (`packaging/systemd/`) confines the monitor's
  syscall surface so a compromise of the monitor process cannot
  trivially escalate via the LKM.

This is the same trust model as DPDK / VFIO / other userspace
hardware-access frameworks.

## Building from source

```bash
cd src/kernel
make            # builds against /lib/modules/$(uname -r)/build
sudo insmod socksdirect.ko
ls /dev/socksdirect
sudo dmesg | tail
```

For the syscall-patch fallback, see `src/kernel/patches/v5.15/README.md`.
