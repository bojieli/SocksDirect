/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0
 *
 * include/socksdirect/zerocopy.h — userspace ABI for the SocksDirect
 * zero-copy kernel module.
 *
 * Phase 5 of the rewrite plan picks LKM as the primary delivery channel
 * for the zero-copy operations. The legacy lib/zerocopy.h shipped with
 * the research prototype hardcoded syscall numbers 333..339, which is
 * unsafe across kernels and unportable. This header defines the same
 * operations in terms of ioctls on /dev/socksdirect, so the kernel
 * module can be loaded into any stock kernel the user already runs.
 *
 * Wire format conventions:
 *   - All structs are __packed and use fixed-width types.
 *   - Lengths are bytes; addresses are user virtual addresses.
 *   - "physical page number" historically meant kernel pfn; the LKM
 *     instead returns an opaque 64-bit cookie that callers must keep
 *     and hand back when remapping. Userspace must NEVER interpret it.
 *
 * Compatibility:
 *   - The LKM advertises a major/minor version via SD_IOC_GET_VERSION.
 *     The library refuses to use the device if the major mismatches.
 *   - Falling back to copy-mode is the responsibility of the library,
 *     not the kernel.
 *
 * This file is included from C, C++, and the kernel module itself; it
 * therefore avoids C++-isms and any libc functions.
 */

#ifndef SOCKSDIRECT_ZEROCOPY_H_
#define SOCKSDIRECT_ZEROCOPY_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOCKSDIRECT_DEV_PATH    "/dev/socksdirect"
#define SOCKSDIRECT_ABI_MAJOR   1
#define SOCKSDIRECT_ABI_MINOR   0

/* Stable ioctl group ('S' has too many collisions; we use a private code). */
#define SD_IOC_MAGIC            0xCD

/* SD_IOC_GET_VERSION
 *   - direction: read from kernel
 *   - argument:  struct sd_version*
 *   - returns:   0 on success, -EINVAL if struct version is unsupported
 */
struct sd_version {
    uint16_t major;
    uint16_t minor;
    uint32_t flags;
} __attribute__((packed));

#define SD_IOC_GET_VERSION       _IOR(SD_IOC_MAGIC, 0x01, struct sd_version)

/* SD_IOC_ALLOC_PHYS
 *   Allocate a physically-contiguous chunk of `num_pages` pages and map
 *   it into the calling process. Returns the user-space virtual address
 *   in `addr`. The `cookie` is opaque and must be passed back to
 *   SD_IOC_FREE_PHYS / SD_IOC_MAP_PHYS to identify the allocation.
 *   `num_pages` must be a power of two, <= (1 << MAX_ORDER).
 *
 *   Errors: -ENOMEM, -EINVAL.
 */
struct sd_alloc_phys {
    /* in: */
    uint32_t num_pages;
    uint32_t reserved;
    /* out: */
    uint64_t addr;
    uint64_t cookie;
} __attribute__((packed));

#define SD_IOC_ALLOC_PHYS        _IOWR(SD_IOC_MAGIC, 0x02, struct sd_alloc_phys)

/* SD_IOC_FREE_PHYS
 *   Release an allocation previously obtained from SD_IOC_ALLOC_PHYS.
 *   Errors: -EINVAL if cookie unknown.
 */
struct sd_free_phys {
    uint64_t cookie;
} __attribute__((packed));

#define SD_IOC_FREE_PHYS         _IOW(SD_IOC_MAGIC, 0x03, struct sd_free_phys)

/* SD_IOC_VIRT2PHYS
 *   Look up the physical-page cookie backing a single user virtual
 *   address. The address must be page-aligned. Returns -EINVAL otherwise.
 */
struct sd_virt2phys {
    /* in:  */ uint64_t virt;
    /* out: */ uint64_t cookie;
} __attribute__((packed));

#define SD_IOC_VIRT2PHYS         _IOWR(SD_IOC_MAGIC, 0x04, struct sd_virt2phys)

/* SD_IOC_VIRT2PHYS_VEC
 *   Vectored variant: writes `npages` cookies into the user buffer at
 *   `cookies`. The buffer must hold `npages * sizeof(uint64_t)` bytes.
 */
struct sd_virt2phys_vec {
    /* in:  */ uint64_t virt;
    /* in:  */ uint64_t cookies;   /* user pointer to uint64_t[] */
    /* in:  */ uint32_t npages;
    /* in:  */ uint32_t flags;
} __attribute__((packed));

#define SD_IOC_VIRT2PHYS_VEC     _IOWR(SD_IOC_MAGIC, 0x05, struct sd_virt2phys_vec)

/* SD_IOC_MAP_PHYS
 *   Remap the page at `virt` to point at the physical page identified
 *   by `cookie`. Returns the *previous* cookie at that address in
 *   `old_cookie`, so the caller can restore later. The address must be
 *   page-aligned.
 */
struct sd_map_phys {
    /* in:  */ uint64_t virt;
    /* in:  */ uint64_t cookie;
    /* out: */ uint64_t old_cookie;
} __attribute__((packed));

#define SD_IOC_MAP_PHYS          _IOWR(SD_IOC_MAGIC, 0x06, struct sd_map_phys)

/* SD_IOC_MAP_PHYS_VEC
 *   Vectored map: remaps `npages` pages starting at `virt`. The
 *   `cookies` and `old_cookies` user pointers each hold `npages` entries.
 */
struct sd_map_phys_vec {
    /* in:  */ uint64_t virt;
    /* in:  */ uint64_t cookies;        /* uint64_t[npages] */
    /* in:  */ uint64_t old_cookies;    /* uint64_t[npages] (out) */
    /* in:  */ uint32_t npages;
    /* in:  */ uint32_t flags;
} __attribute__((packed));

#define SD_IOC_MAP_PHYS_VEC      _IOWR(SD_IOC_MAGIC, 0x07, struct sd_map_phys_vec)

/* SD_IOC_ECHO
 *   Test ioctl: returns the input value. Used by the library at startup
 *   to verify the kernel module is loaded and responsive.
 */
struct sd_echo {
    uint64_t in;
    uint64_t out;
} __attribute__((packed));

#define SD_IOC_ECHO              _IOWR(SD_IOC_MAGIC, 0x08, struct sd_echo)

#define SOCKSDIRECT_MAX_ORDER    11

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* SOCKSDIRECT_ZEROCOPY_H_ */
