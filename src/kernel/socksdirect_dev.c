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

/* Look up the cookie at a user virtual address by walking the caller's
 * mm. We resolve the VMA, find the struct page backing it, and report
 * the PFN as a 64-bit cookie. The userspace ABI doesn't require the
 * cookie to be a PFN — but a PFN is the cheapest stable identifier. */
static long sd_resolve_virt(unsigned long virt, u64 *cookie_out)
{
    struct mm_struct *mm = current->mm;
    struct vm_area_struct *vma;
    struct page *page = NULL;
    int got;

    if (virt & (PAGE_SIZE - 1)) return -EINVAL;
    if (!mm) return -ESRCH;

    mmap_read_lock(mm);
    vma = find_vma(mm, virt);
    if (!vma || virt < vma->vm_start) {
        mmap_read_unlock(mm);
        return -EFAULT;
    }
    /* Pin and walk one page. */
    got = get_user_pages_remote(mm, virt, 1, FOLL_GET, &page, NULL, NULL);
    mmap_read_unlock(mm);
    if (got != 1 || !page) return -EFAULT;
    *cookie_out = (u64)page_to_pfn(page);
    put_page(page);
    return 0;
}

static long sd_ioctl_virt2phys(unsigned long arg)
{
    struct sd_virt2phys req;
    long rc;
    if (copy_from_user(&req, (void __user *)arg, sizeof(req))) return -EFAULT;
    rc = sd_resolve_virt((unsigned long)req.virt, &req.cookie);
    if (rc) return rc;
    if (copy_to_user((void __user *)arg, &req, sizeof(req))) return -EFAULT;
    return 0;
}

static long sd_ioctl_virt2phys_vec(unsigned long arg)
{
    struct sd_virt2phys_vec req;
    u64 *cookies;
    unsigned long virt;
    long rc = 0;
    u32 i;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req))) return -EFAULT;
    if (req.npages == 0 || req.npages > (1u << SOCKSDIRECT_MAX_ORDER))
        return -EINVAL;
    cookies = kmalloc_array(req.npages, sizeof(u64), GFP_KERNEL);
    if (!cookies) return -ENOMEM;
    virt = (unsigned long)req.virt;
    for (i = 0; i < req.npages; ++i) {
        rc = sd_resolve_virt(virt + (unsigned long)i * PAGE_SIZE, &cookies[i]);
        if (rc) goto out;
    }
    if (copy_to_user((void __user *)(unsigned long)req.cookies,
                     cookies, sizeof(u64) * req.npages))
        rc = -EFAULT;
out:
    kfree(cookies);
    return rc;
}

/* Find the allocation node holding `cookie` (the alloc-time cookie,
 * NOT the PFN cookie) on the open fd. */
static struct sd_alloc_node *sd_find_alloc_locked(struct sd_filp_state *st,
                                                  u64 cookie)
{
    struct sd_alloc_node *n;
    list_for_each_entry(n, &st->allocations, link) {
        if (n->cookie == cookie)
            return n;
    }
    return NULL;
}

/* Replace the page at user-vaddr `virt` with the first page of the
 * allocation identified by `cookie`. We use vm_insert_page() which is
 * the supported way for an LKM to install a kernel-allocated page into
 * a user VMA without touching arch-specific page-table code directly. */
static long sd_map_one(struct sd_filp_state *st, unsigned long virt, u64 cookie,
                       u64 *old_cookie_out)
{
    struct mm_struct *mm = current->mm;
    struct vm_area_struct *vma;
    struct sd_alloc_node *node;
    struct page *page = NULL;
    long rc;

    if (virt & (PAGE_SIZE - 1)) return -EINVAL;
    if (!mm) return -ESRCH;

    mutex_lock(&st->lock);
    node = sd_find_alloc_locked(st, cookie);
    mutex_unlock(&st->lock);
    if (!node) return -EINVAL;

    mmap_write_lock(mm);
    vma = find_vma(mm, virt);
    if (!vma || virt < vma->vm_start) {
        mmap_write_unlock(mm);
        return -EFAULT;
    }
    /* Capture the previous PFN if the caller asked for it. */
    if (old_cookie_out) {
        rc = get_user_pages_remote(mm, virt, 1, FOLL_GET, &page, NULL, NULL);
        if (rc == 1 && page) {
            *old_cookie_out = (u64)page_to_pfn(page);
            put_page(page);
        } else {
            *old_cookie_out = 0;
        }
    }
    /* vm_insert_page requires VM_MIXEDMAP on the VMA. The userspace
     * library is expected to allocate the target VMA via
     * mmap(/dev/socksdirect, ...) which sets VM_MIXEDMAP from
     * sd_mmap() below. If the caller used a plain anonymous mapping,
     * vm_insert_page returns -EINVAL and the caller falls back to
     * copy mode. */
    rc = vm_insert_page(vma, virt, node->pages);
    mmap_write_unlock(mm);
    return rc;
}

static long sd_ioctl_map_phys(struct sd_filp_state *st, unsigned long arg)
{
    struct sd_map_phys req;
    long rc;
    if (copy_from_user(&req, (void __user *)arg, sizeof(req))) return -EFAULT;
    rc = sd_map_one(st, (unsigned long)req.virt, req.cookie, &req.old_cookie);
    if (rc) return rc;
    if (copy_to_user((void __user *)arg, &req, sizeof(req))) return -EFAULT;
    return 0;
}

static long sd_ioctl_map_phys_vec(struct sd_filp_state *st, unsigned long arg)
{
    struct sd_map_phys_vec req;
    u64 *cookies = NULL;
    u64 *olds    = NULL;
    long rc = 0;
    u32 i;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req))) return -EFAULT;
    if (req.npages == 0 || req.npages > (1u << SOCKSDIRECT_MAX_ORDER))
        return -EINVAL;
    cookies = kmalloc_array(req.npages, sizeof(u64), GFP_KERNEL);
    olds    = kmalloc_array(req.npages, sizeof(u64), GFP_KERNEL);
    if (!cookies || !olds) { rc = -ENOMEM; goto out; }
    if (copy_from_user(cookies, (void __user *)(unsigned long)req.cookies,
                       sizeof(u64) * req.npages)) { rc = -EFAULT; goto out; }
    for (i = 0; i < req.npages; ++i) {
        rc = sd_map_one(st, (unsigned long)req.virt + (unsigned long)i * PAGE_SIZE,
                        cookies[i], &olds[i]);
        if (rc) goto out;
    }
    if (copy_to_user((void __user *)(unsigned long)req.old_cookies,
                     olds, sizeof(u64) * req.npages))
        rc = -EFAULT;
out:
    kfree(cookies);
    kfree(olds);
    return rc;
}

/* mmap on /dev/socksdirect — sets up a VMA that map_phys can later
 * install pages into. The mmap call itself doesn't have a cookie; it
 * just allocates address space and marks the VMA VM_MIXEDMAP so
 * vm_insert_page() works. The userspace library then follows up with
 * SD_IOC_MAP_PHYS to point each page at a specific allocation. */
static int sd_mmap(struct file *filp, struct vm_area_struct *vma)
{
    (void)filp;
    if ((vma->vm_end - vma->vm_start) > (1UL << (SOCKSDIRECT_MAX_ORDER + PAGE_SHIFT)))
        return -EINVAL;
    /* VM_MIXEDMAP lets vm_insert_page work; VM_DONTEXPAND prevents
     * mremap from breaking our invariants; VM_DONTDUMP keeps the
     * region out of core dumps (it's IPC scratch). */
    vma->vm_flags |= VM_MIXEDMAP | VM_DONTEXPAND | VM_DONTDUMP;
    return 0;
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
    case SD_IOC_MAP_PHYS:       return sd_ioctl_map_phys(st, arg);
    case SD_IOC_MAP_PHYS_VEC:   return sd_ioctl_map_phys_vec(st, arg);
    default:                    return -ENOTTY;
    }
}

static const struct file_operations sd_fops = {
    .owner          = THIS_MODULE,
    .open           = sd_open,
    .release        = sd_release,
    .unlocked_ioctl = sd_unlocked_ioctl,
    .compat_ioctl   = sd_unlocked_ioctl,
    .mmap           = sd_mmap,
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
