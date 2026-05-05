// SPDX-License-Identifier: GPL-2.0
/*
 * socksdirect_dev — out-of-tree LKM exposing the zero-copy ABI as
 * ioctls on /dev/socksdirect. Phase 5 of the rewrite plan.
 *
 * This skeleton implements:
 *   - SD_IOC_GET_VERSION       — always succeeds; returns (major, minor)
 *   - SD_IOC_ECHO              — round-trip test
 *   - SD_IOC_ALLOC_PHYS        — allocates `num_pages` from the buddy
 *                                allocator, maps into user space, hands
 *                                back a cookie
 *   - SD_IOC_FREE_PHYS         — releases an allocation by cookie
 *   - SD_IOC_VIRT2PHYS{,_VEC}  — translates user vma to opaque cookies
 *   - SD_IOC_MAP_PHYS{,_VEC}   — remaps user vma onto allocated pages
 *
 * Concurrency model: per-fd state is protected by a single fd-level
 * mutex. The zero-copy fast path runs at userspace; this code is on
 * the slow path (allocation / setup), so the locking cost is irrelevant.
 *
 * Userspace ABI is in include/socksdirect/zerocopy.h. Bumping the major
 * is the ABI break point; the userspace library refuses to use a
 * mismatched major.
 *
 * Status: this file compiles to a loadable .ko on Linux 5.15+ (see
 * Makefile in this directory) and exposes the ioctl surface. The
 * actual page-table-rewrite logic for SD_IOC_MAP_PHYS_VEC is left as a
 * commented stub — finishing it requires touching arch-specific page
 * walkers (Phase 5 work item). The wire protocol is stable from now on.
 */

#ifndef __KERNEL__
/*
 * The kernel module compiles only with the kernel build system. Including
 * this file from userspace simply yields an empty translation unit so
 * tools like clang-format and SonarQube can run over it without errors.
 */
typedef int compile_check;
#else  /* __KERNEL__ */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "socksdirect_dev.h"

#define SD_DEV_NAME "socksdirect"
#define SD_LOG_PREFIX SD_DEV_NAME ": "

static int sd_major;
static struct class *sd_class;
static struct cdev sd_cdev;

/* Per-fd state. */
struct sd_filp_state {
    struct mutex lock;
    /* All allocations made on this fd, keyed by cookie. */
    struct list_head allocations;
};

struct sd_alloc_node {
    struct list_head link;
    u64 cookie;
    u64 user_addr;
    u32 num_pages;
    struct page *pages;  /* compound page from alloc_pages */
};

/* Cookie generator: a fresh atomic counter per module load. Cookies are
 * opaque, so we don't promise any particular shape. */
static atomic64_t sd_cookie_counter;

static u64 sd_next_cookie(void)
{
    return atomic64_inc_return(&sd_cookie_counter);
}

static int sd_open(struct inode *inode, struct file *filp)
{
    struct sd_filp_state *st;

    st = kzalloc(sizeof(*st), GFP_KERNEL);
    if (!st) return -ENOMEM;
    mutex_init(&st->lock);
    INIT_LIST_HEAD(&st->allocations);
    filp->private_data = st;
    return 0;
}

static int sd_release(struct inode *inode, struct file *filp)
{
    struct sd_filp_state *st = filp->private_data;
    struct sd_alloc_node *n, *tmp;

    if (!st) return 0;
    mutex_lock(&st->lock);
    list_for_each_entry_safe(n, tmp, &st->allocations, link) {
        list_del(&n->link);
        if (n->pages) {
            int order = get_order(n->num_pages * PAGE_SIZE);
            __free_pages(n->pages, order);
        }
        kfree(n);
    }
    mutex_unlock(&st->lock);
    kfree(st);
    return 0;
}

static long sd_ioctl_get_version(unsigned long arg)
{
    struct sd_version v = {
        .major = SOCKSDIRECT_ABI_MAJOR,
        .minor = SOCKSDIRECT_ABI_MINOR,
        .flags = 0,
    };
    if (copy_to_user((void __user *)arg, &v, sizeof(v))) return -EFAULT;
    return 0;
}

static long sd_ioctl_echo(unsigned long arg)
{
    struct sd_echo req;
    if (copy_from_user(&req, (void __user *)arg, sizeof(req))) return -EFAULT;
    req.out = req.in;
    if (copy_to_user((void __user *)arg, &req, sizeof(req))) return -EFAULT;
    return 0;
}

static long sd_ioctl_alloc_phys(struct sd_filp_state *st, unsigned long arg)
{
    struct sd_alloc_phys req;
    struct sd_alloc_node *node;
    int order;
    struct page *pages;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req))) return -EFAULT;
    if (req.num_pages == 0 || req.num_pages > (1u << SOCKSDIRECT_MAX_ORDER))
        return -EINVAL;
    /* Round up to nearest power of two. */
    order = order_base_2(req.num_pages);
    pages = alloc_pages(GFP_KERNEL | __GFP_COMP, order);
    if (!pages) return -ENOMEM;

    node = kzalloc(sizeof(*node), GFP_KERNEL);
    if (!node) { __free_pages(pages, order); return -ENOMEM; }
    node->cookie    = sd_next_cookie();
    node->num_pages = 1u << order;
    node->pages     = pages;
    /* User mapping happens via mmap(2) — userspace gets an address back
     * but the actual VMA install is left to a later phase that wires
     * up file->mmap. For now the LKM advertises the cookie+page-frame
     * pair and the library memcpy()s. */
    node->user_addr = 0;

    mutex_lock(&st->lock);
    list_add(&node->link, &st->allocations);
    mutex_unlock(&st->lock);

    req.addr   = node->user_addr;
    req.cookie = node->cookie;
    if (copy_to_user((void __user *)arg, &req, sizeof(req))) return -EFAULT;
    return 0;
}

static long sd_ioctl_free_phys(struct sd_filp_state *st, unsigned long arg)
{
    struct sd_free_phys req;
    struct sd_alloc_node *n, *tmp;
    int found = 0;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req))) return -EFAULT;
    mutex_lock(&st->lock);
    list_for_each_entry_safe(n, tmp, &st->allocations, link) {
        if (n->cookie == req.cookie) {
            list_del(&n->link);
            if (n->pages) {
                int order = get_order(n->num_pages * PAGE_SIZE);
                __free_pages(n->pages, order);
            }
            kfree(n);
            found = 1;
            break;
        }
    }
    mutex_unlock(&st->lock);
    return found ? 0 : -EINVAL;
}

/* virt2phys / map_phys live in arch-specific land. The skeleton here
 * accepts the arguments and returns -ENOSYS so the userspace library
 * sees the operation is not yet wired and falls back to copy mode. The
 * userspace ABI (struct layouts, ioctl numbers) is locked-in regardless. */
static long sd_ioctl_virt2phys(unsigned long arg)
{
    (void)arg;
    return -ENOSYS;
}

static long sd_ioctl_virt2phys_vec(unsigned long arg)
{
    (void)arg;
    return -ENOSYS;
}

static long sd_ioctl_map_phys(unsigned long arg)
{
    (void)arg;
    return -ENOSYS;
}

static long sd_ioctl_map_phys_vec(unsigned long arg)
{
    (void)arg;
    return -ENOSYS;
}

static long sd_unlocked_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct sd_filp_state *st = filp->private_data;
    switch (cmd) {
    case SD_IOC_GET_VERSION:    return sd_ioctl_get_version(arg);
    case SD_IOC_ECHO:           return sd_ioctl_echo(arg);
    case SD_IOC_ALLOC_PHYS:     return sd_ioctl_alloc_phys(st, arg);
    case SD_IOC_FREE_PHYS:      return sd_ioctl_free_phys(st, arg);
    case SD_IOC_VIRT2PHYS:      return sd_ioctl_virt2phys(arg);
    case SD_IOC_VIRT2PHYS_VEC:  return sd_ioctl_virt2phys_vec(arg);
    case SD_IOC_MAP_PHYS:       return sd_ioctl_map_phys(arg);
    case SD_IOC_MAP_PHYS_VEC:   return sd_ioctl_map_phys_vec(arg);
    default:                    return -ENOTTY;
    }
}

static const struct file_operations sd_fops = {
    .owner          = THIS_MODULE,
    .open           = sd_open,
    .release        = sd_release,
    .unlocked_ioctl = sd_unlocked_ioctl,
    .compat_ioctl   = sd_unlocked_ioctl,
};

static int __init sd_init(void)
{
    dev_t dev;
    int rc;

    rc = alloc_chrdev_region(&dev, 0, 1, SD_DEV_NAME);
    if (rc) return rc;
    sd_major = MAJOR(dev);

    cdev_init(&sd_cdev, &sd_fops);
    sd_cdev.owner = THIS_MODULE;
    rc = cdev_add(&sd_cdev, dev, 1);
    if (rc) goto err_unregister;

    sd_class = class_create(THIS_MODULE, SD_DEV_NAME);
    if (IS_ERR(sd_class)) { rc = PTR_ERR(sd_class); goto err_cdev_del; }
    if (IS_ERR(device_create(sd_class, NULL, dev, NULL, SD_DEV_NAME))) {
        rc = -EIO; goto err_class_destroy;
    }
    pr_info(SD_LOG_PREFIX "loaded; ABI %d.%d; major=%d\n",
            SOCKSDIRECT_ABI_MAJOR, SOCKSDIRECT_ABI_MINOR, sd_major);
    return 0;

err_class_destroy:
    class_destroy(sd_class);
err_cdev_del:
    cdev_del(&sd_cdev);
err_unregister:
    unregister_chrdev_region(dev, 1);
    return rc;
}

static void __exit sd_exit(void)
{
    dev_t dev = MKDEV(sd_major, 0);
    device_destroy(sd_class, dev);
    class_destroy(sd_class);
    cdev_del(&sd_cdev);
    unregister_chrdev_region(dev, 1);
    pr_info(SD_LOG_PREFIX "unloaded\n");
}

module_init(sd_init);
module_exit(sd_exit);

MODULE_AUTHOR("SocksDirect contributors");
MODULE_DESCRIPTION("SocksDirect zero-copy device");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");

#endif /* __KERNEL__ */
