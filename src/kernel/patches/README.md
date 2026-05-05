# Kernel patches (fallback path)

The current tree commits to the LKM delivery model: the zero-copy
ABI is exposed as ioctls on `/dev/socksdirect`, built out-of-tree
via DKMS. See `../socksdirect_dev.{c,h}` and `../Makefile`.

This directory exists as a placeholder for the **fallback** path
described in `REWRITE_PLAN.md` Phase 5: a `git format-patch`
series against a stock kernel for cases where syscall semantics
turn out to be required after all.

## When this directory gets populated

Only if a future LKM-blocking discovery surfaces — concretely, an
operation that needs:

- A new ABI for an existing syscall (vs. an ioctl on a chrdev).
- Direct access to a non-exported kernel symbol that an LKM
  cannot reach.

The Phase 5 spike concluded none of `alloc_phys`/`virt2phys`/`map_phys`
needs syscall semantics; ioctls suffice. So today this directory
should stay empty.

## What would land here, if needed

- `v5.15/`, `v6.8/`, …: per-kernel-version directories.
- Each contains `0001-*.patch` … `NNNN-*.patch`, the same series
  the prototype's `zerocopy/linux/` overlay implied.
- `apply.sh` driving `git am` against the kernel source.
- `README.md` per-version: which line numbers, which `Kconfig`
  options to toggle.

## Why we don't ship the prototype's overlay verbatim

`zerocopy/linux/` in the legacy tree is a 5-file partial overlay
that hardcodes syscall numbers 333..339 with no kernel pinned and
no patch series. It's not buildable as-is and not safe to apply
to a host kernel. The LKM in `../socksdirect_dev.c` replaces it
end-to-end.
