// SPDX-License-Identifier: GPL-2.0
/*
 *	mm/mremap.c
 *
 *	(C) Copyright 1996 Linus Torvalds
 *
 *	Address space accounting code	<alan@lxorguk.ukuu.org.uk>
 *	(C) Copyright 2002 Red Hat Inc, All Rights Reserved
 */

#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/shm.h>
#include <linux/ksm.h>
#include <linux/mman.h>
#include <linux/swap.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/swapops.h>
#include <linux/highmem.h>
#include <linux/security.h>
#include <linux/syscalls.h>
#include <linux/mmu_notifier.h>
#include <linux/uaccess.h>
#include <linux/mm-arch-hooks.h>
#include <linux/userfaultfd_k.h>

#include <asm/cacheflush.h>
#include <asm/tlbflush.h>

SYSCALL_DEFINE1(alloc_phys, unsigned long, npages_order)
{
	unsigned long pfn;
	struct page *p = alloc_pages(GFP_KERNEL, npages_order);
	if (p == NULL)
		return -1;

	pfn = page_to_pfn(p);
	return pfn;
}


SYSCALL_DEFINE1(virt2phys, unsigned long, virt_addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	unsigned long pfn;
	struct mm_struct *mm = current->mm;

	pgd = pgd_offset(mm, virt_addr);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return -1;

	p4d = p4d_offset(pgd, virt_addr);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return -2;

	pud = pud_offset(p4d, virt_addr);
	if (pud_none(*pud) || pud_bad(*pud))
		return -3;

	pmd = pmd_offset(pud, virt_addr);
	if (pmd_none(*pmd) || pmd_bad(*pmd))
		return -4;

	pte = pte_offset_map(pmd, virt_addr);
	if (!pte)
		return -5;

	pfn = pte->pte & PTE_PFN_MASK;
	pte_unmap(pte);
	
	return pfn;
}

inline static int do_map_phys(unsigned long virt_addr, unsigned long new_pfn, unsigned long *old_pfn)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	struct mm_struct *mm = current->mm;

	if (new_pfn == 0)
		return -1;

	pgd = pgd_offset(mm, virt_addr);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return -2;

	p4d = p4d_offset(pgd, virt_addr);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return -3;

	pud = pud_offset(p4d, virt_addr);
	if (pud_none(*pud) || pud_bad(*pud))
		return -4;

	pmd = pmd_offset(pud, virt_addr);
	if (pmd_none(*pmd) || pmd_bad(*pmd))
		return -5;

	pte = pte_offset_map(pmd, virt_addr);
	if (!pte)
		return -6;

	*old_pfn = pte->pte & PTE_PFN_MASK;
	pte->pte = (pte->pte & (~PTE_PFN_MASK)) | (new_pfn & PTE_PFN_MASK);
	pte_unmap(pte);
	
	return 0;
}

SYSCALL_DEFINE2(map_phys, unsigned long, virt_addr, unsigned long, phys_addr)
{
	unsigned long old_pfn;
	long ret = do_map_phys(virt_addr, phys_addr, &old_pfn);
	if (ret == 0) {
		__flush_tlb();
		return old_pfn;
	}
	else
		return ret;
}

SYSCALL_DEFINE4(map_physv, unsigned long, virt_addr, unsigned long __user *, phys_addr, unsigned long __user *, old_phys_addr, unsigned long, npages)
{
	unsigned long *buf = kmalloc(GFP_KERNEL, sizeof(unsigned long) * npages * 2);
	unsigned long *old_pfn = buf;
	unsigned long *new_pfn = buf + npages;
	long ret = 0;
	int i;

	if (buf == NULL)
		return -10;

	copy_from_user(old_pfn, phys_addr, sizeof(unsigned long) * npages);

	for (i=0; i<npages; i++)
	{
		ret = do_map_phys(virt_addr, old_pfn[i], &new_pfn[i]);
		if (ret != 0)
			goto out;
		virt_addr += PAGE_SIZE;
	}
	
	__flush_tlb();
	copy_to_user(old_phys_addr, new_pfn, sizeof(unsigned long) * npages);

out:
	kfree(buf);
	return ret;
}

SYSCALL_DEFINE1(socksdirect, unsigned long, echo)
{
	return echo;
}
