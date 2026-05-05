/* SPDX-License-Identifier: GPL-2.0
 *
 * Kernel-side mirror of the userspace ABI. We keep this in sync with
 * include/socksdirect/zerocopy.h by hand. The DKMS build system can't
 * easily reach into ../include/, so the kernel needs its own copy.
 *
 * If you change one, change the other. The unit-test
 * tests/unit/test_zerocopy_client.cpp pins the user-visible struct sizes
 * and ioctl numbers so a divergence is caught immediately on the
 * userspace side.
 */

#ifndef SOCKSDIRECT_DEV_H_
#define SOCKSDIRECT_DEV_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#define SOCKSDIRECT_ABI_MAJOR    1
#define SOCKSDIRECT_ABI_MINOR    0
#define SOCKSDIRECT_MAX_ORDER    11

#define SD_IOC_MAGIC             0xCD

struct sd_version {
    __u16 major;
    __u16 minor;
    __u32 flags;
} __attribute__((packed));

struct sd_alloc_phys {
    __u32 num_pages;
    __u32 reserved;
    __u64 addr;
    __u64 cookie;
} __attribute__((packed));

struct sd_free_phys {
    __u64 cookie;
} __attribute__((packed));

struct sd_virt2phys {
    __u64 virt;
    __u64 cookie;
} __attribute__((packed));

struct sd_virt2phys_vec {
    __u64 virt;
    __u64 cookies;
    __u32 npages;
    __u32 flags;
} __attribute__((packed));

struct sd_map_phys {
    __u64 virt;
    __u64 cookie;
    __u64 old_cookie;
} __attribute__((packed));

struct sd_map_phys_vec {
    __u64 virt;
    __u64 cookies;
    __u64 old_cookies;
    __u32 npages;
    __u32 flags;
} __attribute__((packed));

struct sd_echo {
    __u64 in;
    __u64 out;
} __attribute__((packed));

#define SD_IOC_GET_VERSION       _IOR(SD_IOC_MAGIC,  0x01, struct sd_version)
#define SD_IOC_ALLOC_PHYS        _IOWR(SD_IOC_MAGIC, 0x02, struct sd_alloc_phys)
#define SD_IOC_FREE_PHYS         _IOW(SD_IOC_MAGIC,  0x03, struct sd_free_phys)
#define SD_IOC_VIRT2PHYS         _IOWR(SD_IOC_MAGIC, 0x04, struct sd_virt2phys)
#define SD_IOC_VIRT2PHYS_VEC     _IOWR(SD_IOC_MAGIC, 0x05, struct sd_virt2phys_vec)
#define SD_IOC_MAP_PHYS          _IOWR(SD_IOC_MAGIC, 0x06, struct sd_map_phys)
#define SD_IOC_MAP_PHYS_VEC      _IOWR(SD_IOC_MAGIC, 0x07, struct sd_map_phys_vec)
#define SD_IOC_ECHO              _IOWR(SD_IOC_MAGIC, 0x08, struct sd_echo)

#endif  /* SOCKSDIRECT_DEV_H_ */
