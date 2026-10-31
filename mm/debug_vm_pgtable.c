// SPDX-License-Identifier: GPL-2.0-only
/*
 * This kernel test validates architecture page table helpers and
 * accessors and helps in verifying their continued compliance with
 * expected generic MM semantics.
 *
 * Copyright (C) 2019 ARM Ltd.
 *
 * Author: Anshuman Khandual <anshuman.khandual@arm.com>
 */
#define pr_fmt(fmt) "debug_vm_pgtable: [%-25s]: " fmt, __func__

#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/hugetlb.h>
#include <linux/kernel.h>
#include <linux/kconfig.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mm_types.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/pgtable.h>
#include <linux/random.h>
#include <linux/spinlock.h>
#include <linux/swap.h>
#include <linux/leafops.h>
#include <linux/start_kernel.h>
#include <linux/sched/mm.h>
#include <linux/io.h>
#include <linux/vmalloc.h>
#include <linux/pgalloc.h>
#include <linux/pgt.h>
#include <linux/pgt-walk.h>

#include <asm/cacheflush.h>
#include <asm/tlbflush.h>

/*
 * Please refer Documentation/mm/arch_pgtable_helpers.rst for the semantics
 * expectations that are being validated here. All future changes in here
 * or the documentation need to be in sync.
 */
#define RANDOM_NZVALUE	GENMASK(7, 0)

struct pgtable_debug_args {
	struct mm_struct	*mm;
	struct vm_area_struct	*vma;

	pgd_t			*pgdp;
	p4d_t			*p4dp;
	pud_t			*pudp;
	pmd_t			*pmdp;
	pte_t			*ptep;

	p4d_t			*start_p4dp;
	pud_t			*start_pudp;
	pmd_t			*start_pmdp;
	pgtable_t		start_ptep;

	unsigned long		vaddr;
	pgprot_t		page_prot;
	pgprot_t		page_prot_none;

	bool			is_contiguous_page;
	unsigned long		pud_pfn;
	unsigned long		pmd_pfn;
	unsigned long		pte_pfn;

	unsigned long		fixed_alignment;
	unsigned long		fixed_pgd_pfn;
	unsigned long		fixed_p4d_pfn;
	unsigned long		fixed_pud_pfn;
	unsigned long		fixed_pmd_pfn;
	unsigned long		fixed_pte_pfn;

	swp_entry_t		swp_entry;
	swp_entry_t		leaf_entry;
};

static void __init pte_basic_tests(struct pgtable_debug_args *args, int idx)
{
	pgprot_t prot = vm_get_page_prot(idx);
	pte_t pte = pfn_pte(args->fixed_pte_pfn, prot);
	unsigned long val = idx, *ptr = &val;

	pr_debug("Validating PTE basic (%pGv)\n", ptr);

	/*
	 * This test needs to be executed after the given page table entry
	 * is created with pfn_pte() to make sure that vm_get_page_prot(idx)
	 * does not have the dirty bit enabled from the beginning. This is
	 * important for platforms like arm64 where (!PTE_RDONLY) indicate
	 * dirty bit being set.
	 */
	WARN_ON(pte_dirty(pte_wrprotect(pte)));

	WARN_ON(!pte_same(pte, pte));
	WARN_ON(!pte_young(pte_mkyoung(pte_mkold(pte))));
	WARN_ON(!pte_dirty(pte_mkdirty(pte_mkclean(pte))));
	WARN_ON(!pte_write(pte_mkwrite(pte_wrprotect(pte), args->vma)));
	WARN_ON(pte_young(pte_mkold(pte_mkyoung(pte))));
	WARN_ON(pte_dirty(pte_mkclean(pte_mkdirty(pte))));
	WARN_ON(pte_write(pte_wrprotect(pte_mkwrite(pte, args->vma))));
	WARN_ON(pte_dirty(pte_wrprotect(pte_mkclean(pte))));
	WARN_ON(!pte_dirty(pte_wrprotect(pte_mkdirty(pte))));

	WARN_ON(!pte_dirty(pte_mkwrite_novma(pte_mkdirty(pte))));
	WARN_ON(pte_dirty(pte_mkwrite_novma(pte_mkclean(pte))));
	WARN_ON(!pte_write(pte_mkdirty(pte_mkwrite_novma(pte))));
	WARN_ON(!pte_write(pte_mkwrite_novma(pte_wrprotect(pte))));
	WARN_ON(pte_write(pte_wrprotect(pte_mkwrite_novma(pte))));
}

static void __init pte_advanced_tests(struct pgtable_debug_args *args)
{
	struct page *page;
	pte_t pte;

	/*
	 * Architectures optimize set_pte_at by avoiding TLB flush.
	 * This requires set_pte_at to be not used to update an
	 * existing pte entry. Clear pte before we do set_pte_at
	 *
	 * flush_dcache_page() is called after set_pte_at() to clear
	 * PG_arch_1 for the page on ARM64. The page flag isn't cleared
	 * when it's released and page allocation check will fail when
	 * the page is allocated again. For architectures other than ARM64,
	 * the unexpected overhead of cache flushing is acceptable.
	 */
	page = (args->pte_pfn != ULONG_MAX) ? pfn_to_page(args->pte_pfn) : NULL;
	if (!page)
		return;

	pr_debug("Validating PTE advanced\n");
	if (WARN_ON(!args->ptep))
		return;

	pte = pfn_pte(args->pte_pfn, args->page_prot);
	set_pte_at(args->mm, args->vaddr, args->ptep, pte);
	flush_dcache_page(page);
	ptep_set_wrprotect(args->mm, args->vaddr, args->ptep);
	pte = ptep_get(args->ptep);
	WARN_ON(pte_write(pte));
	ptep_get_and_clear(args->mm, args->vaddr, args->ptep);
	pte = ptep_get(args->ptep);
	WARN_ON(!pte_none(pte));

	pte = pfn_pte(args->pte_pfn, args->page_prot);
	pte = pte_wrprotect(pte);
	pte = pte_mkclean(pte);
	set_pte_at(args->mm, args->vaddr, args->ptep, pte);
	flush_dcache_page(page);
	pte = pte_mkwrite(pte, args->vma);
	pte = pte_mkdirty(pte);
	ptep_set_access_flags(args->vma, args->vaddr, args->ptep, pte, 1);
	pte = ptep_get(args->ptep);
	WARN_ON(!(pte_write(pte) && pte_dirty(pte)));
	ptep_get_and_clear_full(args->mm, args->vaddr, args->ptep, 1);
	pte = ptep_get(args->ptep);
	WARN_ON(!pte_none(pte));

	pte = pfn_pte(args->pte_pfn, args->page_prot);
	pte = pte_mkyoung(pte);
	set_pte_at(args->mm, args->vaddr, args->ptep, pte);
	flush_dcache_page(page);
	ptep_test_and_clear_young(args->vma, args->vaddr, args->ptep);
	pte = ptep_get(args->ptep);
	WARN_ON(pte_young(pte));

	ptep_get_and_clear_full(args->mm, args->vaddr, args->ptep, 1);
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static void __init pmd_basic_tests(struct pgtable_debug_args *args, int idx)
{
	pgprot_t prot = vm_get_page_prot(idx);
	unsigned long val = idx, *ptr = &val;
	pmd_t pmd;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD basic (%pGv)\n", ptr);
	pmd = pfn_pmd(args->fixed_pmd_pfn, prot);

	/*
	 * This test needs to be executed after the given page table entry
	 * is created with pfn_pmd() to make sure that vm_get_page_prot(idx)
	 * does not have the dirty bit enabled from the beginning. This is
	 * important for platforms like arm64 where (!PTE_RDONLY) indicate
	 * dirty bit being set.
	 */
	WARN_ON(pmd_dirty(pmd_wrprotect(pmd)));


	WARN_ON(!pmd_same(pmd, pmd));
	WARN_ON(!pmd_young(pmd_mkyoung(pmd_mkold(pmd))));
	WARN_ON(!pmd_dirty(pmd_mkdirty(pmd_mkclean(pmd))));
	WARN_ON(!pmd_write(pmd_mkwrite(pmd_wrprotect(pmd), args->vma)));
	WARN_ON(pmd_young(pmd_mkold(pmd_mkyoung(pmd))));
	WARN_ON(pmd_dirty(pmd_mkclean(pmd_mkdirty(pmd))));
	WARN_ON(pmd_write(pmd_wrprotect(pmd_mkwrite(pmd, args->vma))));
	WARN_ON(pmd_dirty(pmd_wrprotect(pmd_mkclean(pmd))));
	WARN_ON(!pmd_dirty(pmd_wrprotect(pmd_mkdirty(pmd))));

	WARN_ON(!pmd_dirty(pmd_mkwrite_novma(pmd_mkdirty(pmd))));
	WARN_ON(pmd_dirty(pmd_mkwrite_novma(pmd_mkclean(pmd))));
	WARN_ON(!pmd_write(pmd_mkdirty(pmd_mkwrite_novma(pmd))));
	WARN_ON(!pmd_write(pmd_mkwrite_novma(pmd_wrprotect(pmd))));
	WARN_ON(pmd_write(pmd_wrprotect(pmd_mkwrite_novma(pmd))));

	/*
	 * A huge page does not point to next level page table
	 * entry. Hence this must qualify as pmd_bad().
	 */
	WARN_ON(!pmd_bad(pmd_mkhuge(pmd)));
}

static void __init pmd_advanced_tests(struct pgtable_debug_args *args)
{
	struct page *page;
	pmd_t pmd;
	unsigned long vaddr = args->vaddr;

	if (!has_transparent_hugepage())
		return;

	page = (args->pmd_pfn != ULONG_MAX) ? pfn_to_page(args->pmd_pfn) : NULL;
	if (!page)
		return;

	/*
	 * flush_dcache_page() is called after set_pmd_at() to clear
	 * PG_arch_1 for the page on ARM64. The page flag isn't cleared
	 * when it's released and page allocation check will fail when
	 * the page is allocated again. For architectures other than ARM64,
	 * the unexpected overhead of cache flushing is acceptable.
	 */
	pr_debug("Validating PMD advanced\n");
	/* Align the address wrt HPAGE_PMD_SIZE */
	vaddr &= HPAGE_PMD_MASK;

	pgtable_trans_huge_deposit(args->mm, args->pmdp, args->start_ptep);

	pmd = pfn_pmd(args->pmd_pfn, args->page_prot);
	set_pmd_at(args->mm, vaddr, args->pmdp, pmd);
	flush_dcache_page(page);
	pmdp_set_wrprotect(args->mm, vaddr, args->pmdp);
	pmd = pmdp_get(args->pmdp);
	WARN_ON(pmd_write(pmd));
	pmdp_huge_get_and_clear(args->mm, vaddr, args->pmdp);
	pmd = pmdp_get(args->pmdp);
	WARN_ON(!pmd_none(pmd));

	pmd = pfn_pmd(args->pmd_pfn, args->page_prot);
	pmd = pmd_wrprotect(pmd);
	pmd = pmd_mkclean(pmd);
	set_pmd_at(args->mm, vaddr, args->pmdp, pmd);
	flush_dcache_page(page);
	pmd = pmd_mkwrite(pmd, args->vma);
	pmd = pmd_mkdirty(pmd);
	pmdp_set_access_flags(args->vma, vaddr, args->pmdp, pmd, 1);
	pmd = pmdp_get(args->pmdp);
	WARN_ON(!(pmd_write(pmd) && pmd_dirty(pmd)));
	pmdp_huge_get_and_clear_full(args->vma, vaddr, args->pmdp, 1);
	pmd = pmdp_get(args->pmdp);
	WARN_ON(!pmd_none(pmd));

	pmd = pmd_mkhuge(pfn_pmd(args->pmd_pfn, args->page_prot));
	pmd = pmd_mkyoung(pmd);
	set_pmd_at(args->mm, vaddr, args->pmdp, pmd);
	flush_dcache_page(page);
	pmdp_test_and_clear_young(args->vma, vaddr, args->pmdp);
	pmd = pmdp_get(args->pmdp);
	WARN_ON(pmd_young(pmd));

	/*  Clear the pte entries  */
	pmdp_huge_get_and_clear(args->mm, vaddr, args->pmdp);
	pgtable_trans_huge_withdraw(args->mm, args->pmdp);
}

static void __init pmd_leaf_tests(struct pgtable_debug_args *args)
{
	pmd_t pmd;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD leaf\n");
	pmd = pfn_pmd(args->fixed_pmd_pfn, args->page_prot);

	/*
	 * PMD based THP is a leaf entry.
	 */
	pmd = pmd_mkhuge(pmd);
	WARN_ON(!pmd_leaf(pmd));
}

#ifdef CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD
static void __init pud_basic_tests(struct pgtable_debug_args *args, int idx)
{
	pgprot_t prot = vm_get_page_prot(idx);
	unsigned long val = idx, *ptr = &val;
	pud_t pud;

	if (!has_transparent_pud_hugepage())
		return;

	pr_debug("Validating PUD basic (%pGv)\n", ptr);
	pud = pfn_pud(args->fixed_pud_pfn, prot);

	/*
	 * This test needs to be executed after the given page table entry
	 * is created with pfn_pud() to make sure that vm_get_page_prot(idx)
	 * does not have the dirty bit enabled from the beginning. This is
	 * important for platforms like arm64 where (!PTE_RDONLY) indicate
	 * dirty bit being set.
	 */
	WARN_ON(pud_dirty(pud_wrprotect(pud)));

	WARN_ON(!pud_same(pud, pud));
	WARN_ON(!pud_young(pud_mkyoung(pud_mkold(pud))));
	WARN_ON(!pud_dirty(pud_mkdirty(pud_mkclean(pud))));
	WARN_ON(pud_dirty(pud_mkclean(pud_mkdirty(pud))));
	WARN_ON(!pud_write(pud_mkwrite(pud_wrprotect(pud))));
	WARN_ON(pud_write(pud_wrprotect(pud_mkwrite(pud))));
	WARN_ON(pud_young(pud_mkold(pud_mkyoung(pud))));
	WARN_ON(pud_dirty(pud_wrprotect(pud_mkclean(pud))));
	WARN_ON(!pud_dirty(pud_wrprotect(pud_mkdirty(pud))));

	if (mm_pmd_folded(args->mm))
		return;

	/*
	 * A huge page does not point to next level page table
	 * entry. Hence this must qualify as pud_bad().
	 */
	WARN_ON(!pud_bad(pud_mkhuge(pud)));
}

static void __init pud_advanced_tests(struct pgtable_debug_args *args)
{
	struct page *page;
	unsigned long vaddr = args->vaddr;
	pud_t pud;

	if (!has_transparent_pud_hugepage())
		return;

	page = (args->pud_pfn != ULONG_MAX) ? pfn_to_page(args->pud_pfn) : NULL;
	if (!page)
		return;

	/*
	 * flush_dcache_page() is called after set_pud_at() to clear
	 * PG_arch_1 for the page on ARM64. The page flag isn't cleared
	 * when it's released and page allocation check will fail when
	 * the page is allocated again. For architectures other than ARM64,
	 * the unexpected overhead of cache flushing is acceptable.
	 */
	pr_debug("Validating PUD advanced\n");
	/* Align the address wrt HPAGE_PUD_SIZE */
	vaddr &= HPAGE_PUD_MASK;

	pud = pfn_pud(args->pud_pfn, args->page_prot);
	set_pud_at(args->mm, vaddr, args->pudp, pud);
	flush_dcache_page(page);
	pudp_set_wrprotect(args->mm, vaddr, args->pudp);
	pud = pudp_get(args->pudp);
	WARN_ON(pud_write(pud));

#ifndef __PAGETABLE_PMD_FOLDED
	pudp_huge_get_and_clear(args->mm, vaddr, args->pudp);
	pud = pudp_get(args->pudp);
	WARN_ON(!pud_none(pud));
#endif /* __PAGETABLE_PMD_FOLDED */
	pud = pfn_pud(args->pud_pfn, args->page_prot);
	pud = pud_wrprotect(pud);
	pud = pud_mkclean(pud);
	set_pud_at(args->mm, vaddr, args->pudp, pud);
	flush_dcache_page(page);
	pud = pud_mkwrite(pud);
	pud = pud_mkdirty(pud);
	pudp_set_access_flags(args->vma, vaddr, args->pudp, pud, 1);
	pud = pudp_get(args->pudp);
	WARN_ON(!(pud_write(pud) && pud_dirty(pud)));

#ifndef __PAGETABLE_PMD_FOLDED
	pudp_huge_get_and_clear_full(args->vma, vaddr, args->pudp, 1);
	pud = pudp_get(args->pudp);
	WARN_ON(!pud_none(pud));
#endif /* __PAGETABLE_PMD_FOLDED */

	pud = pfn_pud(args->pud_pfn, args->page_prot);
	pud = pud_mkyoung(pud);
	set_pud_at(args->mm, vaddr, args->pudp, pud);
	flush_dcache_page(page);
	pudp_test_and_clear_young(args->vma, vaddr, args->pudp);
	pud = pudp_get(args->pudp);
	WARN_ON(pud_young(pud));

	pudp_huge_get_and_clear(args->mm, vaddr, args->pudp);
}

static void __init pud_leaf_tests(struct pgtable_debug_args *args)
{
	pud_t pud;

	if (!has_transparent_pud_hugepage())
		return;

	pr_debug("Validating PUD leaf\n");
	pud = pfn_pud(args->fixed_pud_pfn, args->page_prot);
	/*
	 * PUD based THP is a leaf entry.
	 */
	pud = pud_mkhuge(pud);
	WARN_ON(!pud_leaf(pud));
}
#else  /* !CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD */
static void __init pud_basic_tests(struct pgtable_debug_args *args, int idx) { }
static void __init pud_advanced_tests(struct pgtable_debug_args *args) { }
static void __init pud_leaf_tests(struct pgtable_debug_args *args) { }
#endif /* CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD */
#else  /* !CONFIG_TRANSPARENT_HUGEPAGE */
static void __init pmd_basic_tests(struct pgtable_debug_args *args, int idx) { }
static void __init pud_basic_tests(struct pgtable_debug_args *args, int idx) { }
static void __init pmd_advanced_tests(struct pgtable_debug_args *args) { }
static void __init pud_advanced_tests(struct pgtable_debug_args *args) { }
static void __init pmd_leaf_tests(struct pgtable_debug_args *args) { }
static void __init pud_leaf_tests(struct pgtable_debug_args *args) { }
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

#ifdef CONFIG_HAVE_ARCH_HUGE_VMAP
static void __init pmd_huge_tests(struct pgtable_debug_args *args)
{
	pmd_t pmd;

	if (!arch_vmap_pmd_supported(args->page_prot) ||
	    args->fixed_alignment < PMD_SIZE)
		return;

	pr_debug("Validating PMD huge\n");
	/*
	 * X86 defined pmd_set_huge() verifies that the given
	 * PMD is not a populated non-leaf entry.
	 */
	pmd_clear(args->pmdp);
	WARN_ON(!pmd_set_huge(args->pmdp, __pfn_to_phys(args->fixed_pmd_pfn), args->page_prot));
	WARN_ON(!pmd_clear_huge(args->pmdp));
	pmd = pmdp_get(args->pmdp);
	WARN_ON(!pmd_none(pmd));
}

static void __init pud_huge_tests(struct pgtable_debug_args *args)
{
	pud_t pud;

	if (!arch_vmap_pud_supported(args->page_prot) ||
	    args->fixed_alignment < PUD_SIZE)
		return;

	pr_debug("Validating PUD huge\n");
	/*
	 * X86 defined pud_set_huge() verifies that the given
	 * PUD is not a populated non-leaf entry.
	 */
	pud_clear(args->pudp);
	WARN_ON(!pud_set_huge(args->pudp, __pfn_to_phys(args->fixed_pud_pfn), args->page_prot));
	WARN_ON(!pud_clear_huge(args->pudp));
	pud = pudp_get(args->pudp);
	WARN_ON(!pud_none(pud));
}
#else /* !CONFIG_HAVE_ARCH_HUGE_VMAP */
static void __init pmd_huge_tests(struct pgtable_debug_args *args) { }
static void __init pud_huge_tests(struct pgtable_debug_args *args) { }
#endif /* CONFIG_HAVE_ARCH_HUGE_VMAP */

static void __init p4d_basic_tests(struct pgtable_debug_args *args)
{
	p4d_t p4d;

	pr_debug("Validating P4D basic\n");
	memset(&p4d, RANDOM_NZVALUE, sizeof(p4d_t));
	WARN_ON(!p4d_same(p4d, p4d));
}

static void __init pgd_basic_tests(struct pgtable_debug_args *args)
{
	pgd_t pgd;

	pr_debug("Validating PGD basic\n");
	memset(&pgd, RANDOM_NZVALUE, sizeof(pgd_t));
	WARN_ON(!pgd_same(pgd, pgd));
}

#ifndef __PAGETABLE_PUD_FOLDED
static void __init pud_clear_tests(struct pgtable_debug_args *args)
{
	pud_t pud = pudp_get(args->pudp);

	if (mm_pmd_folded(args->mm))
		return;

	pr_debug("Validating PUD clear\n");
	WARN_ON(pud_none(pud));
	pud_clear(args->pudp);
	pud = pudp_get(args->pudp);
	WARN_ON(!pud_none(pud));
}

static void __init pud_populate_tests(struct pgtable_debug_args *args)
{
	pud_t pud;

	if (mm_pmd_folded(args->mm))
		return;

	pr_debug("Validating PUD populate\n");
	/*
	 * This entry points to next level page table page.
	 * Hence this must not qualify as pud_bad().
	 */
	pud_populate(args->mm, args->pudp, args->start_pmdp);
	pud = pudp_get(args->pudp);
	WARN_ON(pud_bad(pud));
}
#else  /* !__PAGETABLE_PUD_FOLDED */
static void __init pud_clear_tests(struct pgtable_debug_args *args) { }
static void __init pud_populate_tests(struct pgtable_debug_args *args) { }
#endif /* PAGETABLE_PUD_FOLDED */

#ifndef __PAGETABLE_P4D_FOLDED
static void __init p4d_clear_tests(struct pgtable_debug_args *args)
{
	p4d_t p4d = p4dp_get(args->p4dp);

	if (mm_pud_folded(args->mm))
		return;

	pr_debug("Validating P4D clear\n");
	WARN_ON(p4d_none(p4d));
	p4d_clear(args->p4dp);
	p4d = p4dp_get(args->p4dp);
	WARN_ON(!p4d_none(p4d));
}

static void __init p4d_populate_tests(struct pgtable_debug_args *args)
{
	p4d_t p4d;

	if (mm_pud_folded(args->mm))
		return;

	pr_debug("Validating P4D populate\n");
	/*
	 * This entry points to next level page table page.
	 * Hence this must not qualify as p4d_bad().
	 */
	pud_clear(args->pudp);
	p4d_clear(args->p4dp);
	p4d_populate(args->mm, args->p4dp, args->start_pudp);
	p4d = p4dp_get(args->p4dp);
	WARN_ON(p4d_bad(p4d));
}

static void __init pgd_clear_tests(struct pgtable_debug_args *args)
{
	pgd_t pgd = pgdp_get(args->pgdp);

	if (mm_p4d_folded(args->mm))
		return;

	pr_debug("Validating PGD clear\n");
	WARN_ON(pgd_none(pgd));
	pgd_clear(args->pgdp);
	pgd = pgdp_get(args->pgdp);
	WARN_ON(!pgd_none(pgd));
}

static void __init pgd_populate_tests(struct pgtable_debug_args *args)
{
	pgd_t pgd;

	if (mm_p4d_folded(args->mm))
		return;

	pr_debug("Validating PGD populate\n");
	/*
	 * This entry points to next level page table page.
	 * Hence this must not qualify as pgd_bad().
	 */
	p4d_clear(args->p4dp);
	pgd_clear(args->pgdp);
	pgd_populate(args->mm, args->pgdp, args->start_p4dp);
	pgd = pgdp_get(args->pgdp);
	WARN_ON(pgd_bad(pgd));
}
#else  /* !__PAGETABLE_P4D_FOLDED */
static void __init p4d_clear_tests(struct pgtable_debug_args *args) { }
static void __init pgd_clear_tests(struct pgtable_debug_args *args) { }
static void __init p4d_populate_tests(struct pgtable_debug_args *args) { }
static void __init pgd_populate_tests(struct pgtable_debug_args *args) { }
#endif /* PAGETABLE_P4D_FOLDED */

static void __init pte_clear_tests(struct pgtable_debug_args *args)
{
	struct page *page;
	pte_t pte = pfn_pte(args->pte_pfn, args->page_prot);

	page = (args->pte_pfn != ULONG_MAX) ? pfn_to_page(args->pte_pfn) : NULL;
	if (!page)
		return;

	/*
	 * flush_dcache_page() is called after set_pte_at() to clear
	 * PG_arch_1 for the page on ARM64. The page flag isn't cleared
	 * when it's released and page allocation check will fail when
	 * the page is allocated again. For architectures other than ARM64,
	 * the unexpected overhead of cache flushing is acceptable.
	 */
	pr_debug("Validating PTE clear\n");
	if (WARN_ON(!args->ptep))
		return;

	set_pte_at(args->mm, args->vaddr, args->ptep, pte);
	WARN_ON(pte_none(pte));
	flush_dcache_page(page);
	barrier();
	ptep_clear(args->mm, args->vaddr, args->ptep);
	pte = ptep_get(args->ptep);
	WARN_ON(!pte_none(pte));
}

static void __init pmd_clear_tests(struct pgtable_debug_args *args)
{
	pmd_t pmd = pmdp_get(args->pmdp);

	pr_debug("Validating PMD clear\n");
	WARN_ON(pmd_none(pmd));
	pmd_clear(args->pmdp);
	pmd = pmdp_get(args->pmdp);
	WARN_ON(!pmd_none(pmd));
}

static void __init pmd_populate_tests(struct pgtable_debug_args *args)
{
	pmd_t pmd;

	pr_debug("Validating PMD populate\n");
	/*
	 * This entry points to next level page table page.
	 * Hence this must not qualify as pmd_bad().
	 */
	pmd_populate(args->mm, args->pmdp, args->start_ptep);
	pmd = pmdp_get(args->pmdp);
	WARN_ON(pmd_bad(pmd));
}

static void __init pte_special_tests(struct pgtable_debug_args *args)
{
	pte_t pte = pfn_pte(args->fixed_pte_pfn, args->page_prot);

	if (!IS_ENABLED(CONFIG_ARCH_HAS_PTE_SPECIAL))
		return;

	pr_debug("Validating PTE special\n");
	WARN_ON(!pte_special(pte_mkspecial(pte)));
}

static void __init pte_protnone_tests(struct pgtable_debug_args *args)
{
	pte_t pte = pfn_pte(args->fixed_pte_pfn, args->page_prot_none);

	if (!IS_ENABLED(CONFIG_ARCH_HAS_PTE_PROTNONE))
		return;

	pr_debug("Validating PTE protnone\n");
	WARN_ON(!pte_protnone(pte));
	WARN_ON(!pte_present(pte));
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static void __init pmd_protnone_tests(struct pgtable_debug_args *args)
{
	pmd_t pmd;

	if (!IS_ENABLED(CONFIG_ARCH_HAS_PTE_PROTNONE))
		return;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD protnone\n");
	pmd = pmd_mkhuge(pfn_pmd(args->fixed_pmd_pfn, args->page_prot_none));
	WARN_ON(!pmd_protnone(pmd));
	WARN_ON(!pmd_present(pmd));
}
#else  /* !CONFIG_TRANSPARENT_HUGEPAGE */
static void __init pmd_protnone_tests(struct pgtable_debug_args *args) { }
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

static void __init pte_soft_dirty_tests(struct pgtable_debug_args *args)
{
	pte_t pte = pfn_pte(args->fixed_pte_pfn, args->page_prot);

	if (!pgtable_supports_soft_dirty())
		return;

	pr_debug("Validating PTE soft dirty\n");
	WARN_ON(!pte_soft_dirty(pte_mksoft_dirty(pte)));
	WARN_ON(pte_soft_dirty(pte_clear_soft_dirty(pte)));
}

static void __init pte_swap_soft_dirty_tests(struct pgtable_debug_args *args)
{
	pte_t pte;
	softleaf_t entry;

	if (!pgtable_supports_soft_dirty())
		return;

	pr_debug("Validating PTE swap soft dirty\n");
	pte = swp_entry_to_pte(args->swp_entry);
	entry = softleaf_from_pte(pte);

	WARN_ON(!softleaf_is_swap(entry));
	WARN_ON(!pte_swp_soft_dirty(pte_swp_mksoft_dirty(pte)));
	WARN_ON(pte_swp_soft_dirty(pte_swp_clear_soft_dirty(pte)));
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static void __init pmd_soft_dirty_tests(struct pgtable_debug_args *args)
{
	pmd_t pmd;

	if (!pgtable_supports_soft_dirty())
		return;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD soft dirty\n");
	pmd = pfn_pmd(args->fixed_pmd_pfn, args->page_prot);
	WARN_ON(!pmd_soft_dirty(pmd_mksoft_dirty(pmd)));
	WARN_ON(pmd_soft_dirty(pmd_clear_soft_dirty(pmd)));
}

static void __init pmd_leaf_soft_dirty_tests(struct pgtable_debug_args *args)
{
	pmd_t pmd;

	if (!pgtable_supports_soft_dirty() ||
	    !IS_ENABLED(CONFIG_ARCH_HAS_PMD_SOFTLEAVES))
		return;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD swap soft dirty\n");
	pmd = softleaf_to_pmd(args->leaf_entry);
	WARN_ON(!pmd_is_huge(pmd));
	WARN_ON(!pmd_is_valid_softleaf(pmd));

	WARN_ON(!pmd_swp_soft_dirty(pmd_swp_mksoft_dirty(pmd)));
	WARN_ON(pmd_swp_soft_dirty(pmd_swp_clear_soft_dirty(pmd)));
}
#else  /* !CONFIG_TRANSPARENT_HUGEPAGE */
static void __init pmd_soft_dirty_tests(struct pgtable_debug_args *args) { }
static void __init pmd_leaf_soft_dirty_tests(struct pgtable_debug_args *args) { }
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

static void __init pte_swap_exclusive_tests(struct pgtable_debug_args *args)
{
	swp_entry_t entry;
	softleaf_t softleaf;
	pte_t pte;

	pr_debug("Validating PTE swap exclusive\n");
	entry = args->swp_entry;

	pte = swp_entry_to_pte(entry);
	softleaf = softleaf_from_pte(pte);

	WARN_ON(pte_swp_exclusive(pte));
	WARN_ON(!softleaf_is_swap(softleaf));
	WARN_ON(memcmp(&entry, &softleaf, sizeof(entry)));

	pte = pte_swp_mkexclusive(pte);
	softleaf = softleaf_from_pte(pte);

	WARN_ON(!pte_swp_exclusive(pte));
	WARN_ON(!softleaf_is_swap(softleaf));
	WARN_ON(pte_swp_soft_dirty(pte));
	WARN_ON(memcmp(&entry, &softleaf, sizeof(entry)));

	pte = pte_swp_clear_exclusive(pte);
	softleaf = softleaf_from_pte(pte);

	WARN_ON(pte_swp_exclusive(pte));
	WARN_ON(!softleaf_is_swap(softleaf));
	WARN_ON(memcmp(&entry, &softleaf, sizeof(entry)));
}

static void __init pte_swap_tests(struct pgtable_debug_args *args)
{
	swp_entry_t arch_entry;
	softleaf_t entry;
	pte_t pte1, pte2;

	pr_debug("Validating PTE swap\n");
	pte1 = swp_entry_to_pte(args->swp_entry);
	entry = softleaf_from_pte(pte1);

	WARN_ON(!softleaf_is_swap(entry));

	arch_entry = __pte_to_swp_entry(pte1);
	pte2 = __swp_entry_to_pte(arch_entry);
	WARN_ON(memcmp(&pte1, &pte2, sizeof(pte1)));
}

#ifdef CONFIG_ARCH_HAS_PMD_SOFTLEAVES
static void __init pmd_softleaf_tests(struct pgtable_debug_args *args)
{
	swp_entry_t arch_entry;
	pmd_t pmd1, pmd2;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD swap\n");
	pmd1 = softleaf_to_pmd(args->leaf_entry);
	WARN_ON(!pmd_is_huge(pmd1));
	WARN_ON(!pmd_is_valid_softleaf(pmd1));

	arch_entry = __pmd_to_swp_entry(pmd1);
	pmd2 = __swp_entry_to_pmd(arch_entry);
	WARN_ON(memcmp(&pmd1, &pmd2, sizeof(pmd1)));
}
#else  /* !CONFIG_ARCH_HAS_PMD_SOFTLEAVES */
static void __init pmd_softleaf_tests(struct pgtable_debug_args *args) { }
#endif /* CONFIG_ARCH_HAS_PMD_SOFTLEAVES */

static void __init swap_migration_tests(struct pgtable_debug_args *args)
{
	struct page *page;
	softleaf_t entry;

	if (!IS_ENABLED(CONFIG_MIGRATION))
		return;

	/*
	 * swap_migration_tests() requires a dedicated page as it needs to
	 * be locked before creating a migration entry from it. Locking the
	 * page that actually maps kernel text ('start_kernel') can be real
	 * problematic. Lets use the allocated page explicitly for this
	 * purpose.
	 */
	page = (args->pte_pfn != ULONG_MAX) ? pfn_to_page(args->pte_pfn) : NULL;
	if (!page)
		return;

	pr_debug("Validating swap migration\n");

	/*
	 * make_[readable|writable]_migration_entry() expects given page to
	 * be locked, otherwise it stumbles upon a BUG_ON().
	 */
	__SetPageLocked(page);
	entry = make_writable_migration_entry(page_to_pfn(page));
	WARN_ON(!softleaf_is_migration(entry));
	WARN_ON(!softleaf_is_migration_write(entry));

	entry = make_readable_migration_entry(swp_offset(entry));
	WARN_ON(!softleaf_is_migration(entry));
	WARN_ON(softleaf_is_migration_write(entry));

	entry = make_readable_migration_entry(page_to_pfn(page));
	WARN_ON(!softleaf_is_migration(entry));
	WARN_ON(softleaf_is_migration_write(entry));
	__ClearPageLocked(page);
}

#ifdef CONFIG_HUGETLB_PAGE
static void __init hugetlb_basic_tests(struct pgtable_debug_args *args)
{
	pte_t pte;

	pr_debug("Validating HugeTLB basic\n");
	pte = pfn_pte(args->fixed_pmd_pfn, args->page_prot);
	pte = arch_make_huge_pte(pte, PMD_SHIFT, VM_ACCESS_FLAGS);

#ifdef CONFIG_ARCH_WANT_GENERAL_HUGETLB
	WARN_ON(!pte_huge(pte));
#endif
	WARN_ON(!huge_pte_dirty(huge_pte_mkdirty(pte)));
	WARN_ON(!huge_pte_write(huge_pte_mkwrite(huge_pte_wrprotect(pte))));
	WARN_ON(huge_pte_write(huge_pte_wrprotect(huge_pte_mkwrite(pte))));
}
#else  /* !CONFIG_HUGETLB_PAGE */
static void __init hugetlb_basic_tests(struct pgtable_debug_args *args) { }
#endif /* CONFIG_HUGETLB_PAGE */

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static void __init pmd_thp_tests(struct pgtable_debug_args *args)
{
	pmd_t pmd;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD based THP\n");
	/*
	 * pmd_trans_huge() and pmd_present() must return positive after
	 * MMU invalidation with pmd_mkinvalid(). This behavior is an
	 * optimization for transparent huge page. pmd_trans_huge() must
	 * be true if pmd_page() returns a valid THP to avoid taking the
	 * pmd_lock when others walk over non transhuge pmds (i.e. there
	 * are no THP allocated). Especially when splitting a THP and
	 * removing the present bit from the pmd, pmd_trans_huge() still
	 * needs to return true. pmd_present() should be true whenever
	 * pmd_trans_huge() returns true.
	 */
	pmd = pfn_pmd(args->fixed_pmd_pfn, args->page_prot);
	WARN_ON(!pmd_trans_huge(pmd_mkhuge(pmd)));

#ifndef __HAVE_ARCH_PMDP_INVALIDATE
	WARN_ON(!pmd_trans_huge(pmd_mkinvalid(pmd_mkhuge(pmd))));
	WARN_ON(!pmd_present(pmd_mkinvalid(pmd_mkhuge(pmd))));
	WARN_ON(!pmd_leaf(pmd_mkinvalid(pmd_mkhuge(pmd))));
#endif /* __HAVE_ARCH_PMDP_INVALIDATE */
}

#ifdef CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD
static void __init pud_thp_tests(struct pgtable_debug_args *args)
{
	pud_t pud;

	if (!has_transparent_pud_hugepage())
		return;

	pr_debug("Validating PUD based THP\n");
	pud = pfn_pud(args->fixed_pud_pfn, args->page_prot);
	WARN_ON(!pud_trans_huge(pud_mkhuge(pud)));

	/*
	 * pud_mkinvalid() has been dropped for now. Enable back
	 * these tests when it comes back with a modified pud_present().
	 *
	 * WARN_ON(!pud_trans_huge(pud_mkinvalid(pud_mkhuge(pud))));
	 * WARN_ON(!pud_present(pud_mkinvalid(pud_mkhuge(pud))));
	 */
}
#else  /* !CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD */
static void __init pud_thp_tests(struct pgtable_debug_args *args) { }
#endif /* CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD */
#else  /* !CONFIG_TRANSPARENT_HUGEPAGE */
static void __init pmd_thp_tests(struct pgtable_debug_args *args) { }
static void __init pud_thp_tests(struct pgtable_debug_args *args) { }
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

static unsigned long __init get_random_vaddr(void)
{
	unsigned long random_vaddr, random_pages, total_user_pages;

	total_user_pages = (TASK_SIZE - FIRST_USER_ADDRESS) / PAGE_SIZE;

	random_pages = get_random_long() % total_user_pages;
	random_vaddr = FIRST_USER_ADDRESS + random_pages * PAGE_SIZE;

	return random_vaddr;
}

static void __init
debug_vm_pgtable_free_huge_page(struct pgtable_debug_args *args,
		unsigned long pfn, int order)
{
#ifdef CONFIG_CONTIG_ALLOC
	if (args->is_contiguous_page) {
		free_contig_range(pfn, 1 << order);
		return;
	}
#endif
	__free_pages(pfn_to_page(pfn), order);
}

static void __init destroy_args(struct pgtable_debug_args *args)
{
	/* Free (huge) page */
	if (IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE) &&
	    has_transparent_pud_hugepage() &&
	    args->pud_pfn != ULONG_MAX) {
		debug_vm_pgtable_free_huge_page(args, args->pud_pfn, HPAGE_PUD_ORDER);
		args->pud_pfn = ULONG_MAX;
		args->pmd_pfn = ULONG_MAX;
		args->pte_pfn = ULONG_MAX;
	}

	if (IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE) &&
	    has_transparent_hugepage() &&
	    args->pmd_pfn != ULONG_MAX) {
		debug_vm_pgtable_free_huge_page(args, args->pmd_pfn, HPAGE_PMD_ORDER);
		args->pmd_pfn = ULONG_MAX;
		args->pte_pfn = ULONG_MAX;
	}

	if (args->pte_pfn != ULONG_MAX) {
		__free_page(pfn_to_page(args->pte_pfn));

		args->pte_pfn = ULONG_MAX;
	}

	/* Free page table entries */
	if (args->start_ptep) {
		pmd_clear(args->pmdp);
		pte_free(args->mm, args->start_ptep);
		mm_dec_nr_ptes(args->mm);
	}

	if (args->start_pmdp) {
		pud_clear(args->pudp);
		pmd_free(args->mm, args->start_pmdp);
		mm_dec_nr_pmds(args->mm);
	}

	if (args->start_pudp) {
		p4d_clear(args->p4dp);
		pud_free(args->mm, args->start_pudp);
		mm_dec_nr_puds(args->mm);
	}

	if (args->start_p4dp) {
		pgd_clear(args->pgdp);
		p4d_free(args->mm, args->start_p4dp);
	}

	/* Free vma and mm struct */
	if (args->vma)
		vm_area_free(args->vma);

	if (args->mm)
		mmput(args->mm);
}

static struct page * __init
debug_vm_pgtable_alloc_huge_page(struct pgtable_debug_args *args, int order)
{
	struct page *page = NULL;

#ifdef CONFIG_CONTIG_ALLOC
	if (order > MAX_PAGE_ORDER) {
		page = alloc_contig_pages((1 << order), GFP_KERNEL,
					  first_online_node, NULL);
		if (page) {
			args->is_contiguous_page = true;
			return page;
		}
	}
#endif

	if (order <= MAX_PAGE_ORDER)
		page = alloc_pages(GFP_KERNEL, order);

	return page;
}

/*
 * Check if a physical memory range described by <pstart, pend> contains
 * an area that is of size psize, and aligned to psize.
 *
 * Don't use address 0, an all-zeroes physical address might mask bugs, and
 * it's not used on x86.
 */
static void  __init phys_align_check(phys_addr_t pstart,
				     phys_addr_t pend, unsigned long psize,
				     phys_addr_t *physp, unsigned long *alignp)
{
	phys_addr_t aligned_start, aligned_end;

	if (pstart == 0)
		pstart = PAGE_SIZE;

	aligned_start = ALIGN(pstart, psize);
	aligned_end = aligned_start + psize;

	if (aligned_end > aligned_start && aligned_end <= pend) {
		*alignp = psize;
		*physp = aligned_start;
	}
}

static void __init init_fixed_pfns(struct pgtable_debug_args *args)
{
	u64 idx;
	phys_addr_t phys, pstart, pend;

	/*
	 * Initialize the fixed pfns. To do this, try to find a
	 * valid physical range, preferably aligned to PUD_SIZE,
	 * but settling for aligned to PMD_SIZE as a fallback. If
	 * neither of those is found, use the physical address of
	 * the start_kernel symbol.
	 *
	 * The memory doesn't need to be allocated, it just needs to exist
	 * as usable memory. It won't be touched.
	 *
	 * The alignment is recorded, and can be checked to see if we
	 * can run the tests that require an actual valid physical
	 * address range on some architectures ({pmd,pud}_huge_test
	 * on x86).
	 */

	phys = __pa_symbol(&start_kernel);
	args->fixed_alignment = PAGE_SIZE;

	for_each_mem_range(idx, &pstart, &pend) {
		/* First check for a PUD-aligned area */
		phys_align_check(pstart, pend, PUD_SIZE, &phys,
				 &args->fixed_alignment);

		/* If a PUD-aligned area is found, we're done */
		if (args->fixed_alignment == PUD_SIZE)
			break;

		/*
		 * If no PMD-aligned area found yet, check for one,
		 * but continue the loop to look for a PUD-aligned area.
		 */
		if (args->fixed_alignment < PMD_SIZE)
			phys_align_check(pstart, pend, PMD_SIZE, &phys,
					 &args->fixed_alignment);
	}

	args->fixed_pgd_pfn = __phys_to_pfn(phys & PGDIR_MASK);
	args->fixed_p4d_pfn = __phys_to_pfn(phys & P4D_MASK);
	args->fixed_pud_pfn = __phys_to_pfn(phys & PUD_MASK);
	args->fixed_pmd_pfn = __phys_to_pfn(phys & PMD_MASK);
	args->fixed_pte_pfn = __phys_to_pfn(phys & PAGE_MASK);
	WARN_ON(!pfn_valid(args->fixed_pte_pfn));
}


static int __init init_args(struct pgtable_debug_args *args)
{
	unsigned long max_swap_offset;
	struct page *page = NULL;
	int ret = 0;

	/*
	 * Initialize the debugging data.
	 *
	 * vm_get_page_prot(VM_NONE) or vm_get_page_prot(VM_SHARED|VM_NONE)
	 * will help create page table entries with PROT_NONE permission as
	 * required for pxx_protnone_tests().
	 */
	memset(args, 0, sizeof(*args));
	args->vaddr              = get_random_vaddr();
	args->page_prot          = vm_get_page_prot(VM_ACCESS_FLAGS);
	args->page_prot_none     = vm_get_page_prot(VM_NONE);
	args->is_contiguous_page = false;
	args->pud_pfn            = ULONG_MAX;
	args->pmd_pfn            = ULONG_MAX;
	args->pte_pfn            = ULONG_MAX;
	args->fixed_pgd_pfn      = ULONG_MAX;
	args->fixed_p4d_pfn      = ULONG_MAX;
	args->fixed_pud_pfn      = ULONG_MAX;
	args->fixed_pmd_pfn      = ULONG_MAX;
	args->fixed_pte_pfn      = ULONG_MAX;

	/* Allocate mm and vma */
	args->mm = mm_alloc();
	if (!args->mm) {
		pr_err("Failed to allocate mm struct\n");
		ret = -ENOMEM;
		goto error;
	}

	args->vma = vm_area_alloc(args->mm);
	if (!args->vma) {
		pr_err("Failed to allocate vma\n");
		ret = -ENOMEM;
		goto error;
	}

	/*
	 * Allocate page table entries. They will be modified in the tests.
	 * Lets save the page table entries so that they can be released
	 * when the tests are completed.
	 */
	args->pgdp = pgd_offset(args->mm, args->vaddr);
	args->p4dp = p4d_alloc(args->mm, args->pgdp, args->vaddr);
	if (!args->p4dp) {
		pr_err("Failed to allocate p4d entries\n");
		ret = -ENOMEM;
		goto error;
	}
	args->start_p4dp = p4d_offset(args->pgdp, 0UL);
	WARN_ON(!args->start_p4dp);

	args->pudp = pud_alloc(args->mm, args->p4dp, args->vaddr);
	if (!args->pudp) {
		pr_err("Failed to allocate pud entries\n");
		ret = -ENOMEM;
		goto error;
	}
	args->start_pudp = pud_offset(args->p4dp, 0UL);
	WARN_ON(!args->start_pudp);

	args->pmdp = pmd_alloc(args->mm, args->pudp, args->vaddr);
	if (!args->pmdp) {
		pr_err("Failed to allocate pmd entries\n");
		ret = -ENOMEM;
		goto error;
	}
	args->start_pmdp = pmd_offset(args->pudp, 0UL);
	WARN_ON(!args->start_pmdp);

	if (pte_alloc(args->mm, args->pmdp)) {
		pr_err("Failed to allocate pte entries\n");
		ret = -ENOMEM;
		goto error;
	}
	args->start_ptep = pmd_pgtable(pmdp_get(args->pmdp));
	WARN_ON(!args->start_ptep);

	init_fixed_pfns(args);

	/* See generic_max_swapfile_size(): probe the maximum offset */
	max_swap_offset = swp_offset(softleaf_from_pte(softleaf_to_pte(swp_entry(0, ~0UL))));
	/* Create a swp entry with all possible bits set while still being swap. */
	args->swp_entry = swp_entry(MAX_SWAPFILES - 1, max_swap_offset);
	/* Create a non-present migration entry. */
	args->leaf_entry = make_writable_migration_entry(~0UL);

	/*
	 * Allocate (huge) pages because some of the tests need to access
	 * the data in the pages. The corresponding tests will be skipped
	 * if we fail to allocate (huge) pages.
	 */
	if (IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE) &&
	    has_transparent_pud_hugepage()) {
		page = debug_vm_pgtable_alloc_huge_page(args, HPAGE_PUD_ORDER);
		if (page) {
			args->pud_pfn = page_to_pfn(page);
			args->pmd_pfn = args->pud_pfn;
			args->pte_pfn = args->pud_pfn;
			return 0;
		}
	}

	if (IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE) &&
	    has_transparent_hugepage()) {
		page = debug_vm_pgtable_alloc_huge_page(args, HPAGE_PMD_ORDER);
		if (page) {
			args->pmd_pfn = page_to_pfn(page);
			args->pte_pfn = args->pmd_pfn;
			return 0;
		}
	}

	page = alloc_page(GFP_KERNEL);
	if (page)
		args->pte_pfn = page_to_pfn(page);

	return 0;

error:
	destroy_args(args);
	return ret;
}

#ifdef CONFIG_PGT
/*
 * Conformance tests for the level-generic page table API: every kind at
 * every level, agreement with the legacy helpers of the level on
 * constructed and random values, and the lock protocol on the tables
 * init_args() built. See Documentation/mm/pgt.rst.
 */
static unsigned long pgt_checks __initdata;
static unsigned long pgt_failures __initdata;
static unsigned long pgt_gaps __initdata;

static const char * const pgt_kind_name[] __initconst = {
	[PGT_NONE] = "NONE",
	[PGT_TABLE] = "TABLE",
	[PGT_LEAF] = "LEAF",
	[PGT_SOFTLEAF] = "SOFTLEAF",
	[PGT_BAD] = "BAD",
};

#define PGT_CHECK(cond, fmt, ...)					\
do {									\
	pgt_checks++;							\
	if (unlikely(!(cond)) && pgt_failures++ < 32)			\
		pr_err("pgt: line %d: %s: " fmt "\n", __LINE__, #cond,	\
		       ##__VA_ARGS__);					\
} while (0)

/*
 * A property the contract promises but the fallback cannot give, because
 * the legacy helpers it mirrors do not: listed in Documentation/mm/pgt.rst.
 * On the fallback a miss is reported as a gap, not a failure.
 */
#define PGT_CHECK_NATIVE(cond, fmt, ...)				\
do {									\
	if (!IS_ENABLED(CONFIG_PGT_FALLBACK)) {				\
		PGT_CHECK(cond, fmt, ##__VA_ARGS__);			\
	} else if (unlikely(!(cond)) && pgt_gaps++ < 32) {		\
		pr_info("pgt: fallback gap at line %d: %s: " fmt "\n",	\
			__LINE__, #cond, ##__VA_ARGS__);		\
	}								\
} while (0)

#define PGT_EXPECT_KIND(v, k)						\
do {									\
	pgtv_t __v = (v);						\
	enum pgt_kind __k = pgtv_kind(__v);				\
									\
	PGT_CHECK(__k == (k), "lvl %u val %#llx kind %s",		\
		  __v.lvl, (u64)__v.val, pgt_kind_name[__k]);		\
} while (0)

/* The bits of a typed value are of its kind */
#define PGT_EXPECT_LEAF(l)						\
	PGT_EXPECT_KIND(pgtv_from_leaf(l), PGT_LEAF)
#define PGT_EXPECT_SOFTLEAF(s)						\
	PGT_EXPECT_KIND(pgtv_from_softleaf(s), PGT_SOFTLEAF)

/* Descend from the root to @lvl at @addr; the level-0 table is mapped */
static pgtp_t __init pgt_find(struct mm_struct *mm, unsigned long addr,
			      unsigned int lvl, pgtp_t *pp)
{
	pgtp_t p = pgtp_root(mm, addr);
	pgtv_t v;

	*pp = PGTP_NULL;
	while (pgtp_level(p) > lvl) {
		v = pgtp_get_lockless(p);
		if (pgtv_kind(v) != PGT_TABLE)
			return PGTP_NULL;
		*pp = p;
		p = pgt_table_child(pgtv_to_table(v), addr);
	}
	return p;
}

static unsigned long __init pgt_level_pfn(struct pgtable_debug_args *args,
					  unsigned int lvl)
{
	switch (lvl) {
	case 0:
		return args->fixed_pte_pfn;
	case 1:
		return args->fixed_pmd_pfn;
	default:
		return args->fixed_pud_pfn;
	}
}

/*
 * Every flag modifier keeps a leaf a leaf, and the flags the level has
 * read back as set.
 */
static void __init pgt_leaf_flag_tests(struct pgtable_debug_args *args,
				       pgt_leaf_t l)
{
	unsigned int lvl = l.v.lvl;
	bool thp = !lvl || (lvl == 1 && has_transparent_hugepage()) ||
		   (lvl == 2 && has_transparent_hugepage() &&
		    has_transparent_pud_hugepage());
	pgt_leaf_t m;

	m = pgt_leaf_mkwrite(l, args->vma);
	PGT_EXPECT_LEAF(m);
	PGT_CHECK(!thp || pgt_leaf_write(m), "lvl %u", lvl);
	m = pgt_leaf_wrprotect(m);
	PGT_EXPECT_LEAF(m);
	PGT_CHECK(!pgt_leaf_write(m), "lvl %u", lvl);
	m = pgt_leaf_mkdirty(l);
	PGT_EXPECT_LEAF(m);
	PGT_CHECK(!thp || pgt_leaf_dirty(m), "lvl %u", lvl);
	m = pgt_leaf_mkclean(m);
	PGT_EXPECT_LEAF(m);
	PGT_CHECK(!pgt_leaf_dirty(m), "lvl %u", lvl);
	m = pgt_leaf_mkyoung(l);
	PGT_EXPECT_LEAF(m);
	PGT_CHECK(!thp || pgt_leaf_young(m), "lvl %u", lvl);
	m = pgt_leaf_mkold(m);
	PGT_EXPECT_LEAF(m);
	PGT_CHECK(!pgt_leaf_young(m), "lvl %u", lvl);
	PGT_EXPECT_LEAF(pgt_leaf_mkspecial(l));
	if (lvl < 2 && IS_ENABLED(CONFIG_MEM_SOFT_DIRTY)) {
		m = pgt_leaf_mksoft_dirty(l);
		PGT_EXPECT_LEAF(m);
		PGT_CHECK(!thp || pgt_leaf_soft_dirty(m), "lvl %u", lvl);
		m = pgt_leaf_clear_soft_dirty(m);
		PGT_CHECK(!pgt_leaf_soft_dirty(m), "lvl %u", lvl);
	}
	if (lvl < 2 && IS_ENABLED(CONFIG_HAVE_ARCH_USERFAULTFD_WP)) {
		m = pgt_leaf_mkuffd_wp(l);
		PGT_EXPECT_LEAF(m);
		PGT_CHECK(!thp || (pgt_leaf_uffd_wp(m) && !pgt_leaf_write(m)),
			  "lvl %u", lvl);
		m = pgt_leaf_clear_uffd_wp(m);
		PGT_CHECK(!pgt_leaf_uffd_wp(m), "lvl %u", lvl);
	}
}

static void __init pgt_kind_tests(struct pgtable_debug_args *args)
{
	unsigned int lvl, root = pgt_root_level(args->mm);
	pgt_softleaf_t s;
	pgt_leaf_t l, m;
	softleaf_t e;
	pgtp_t p, pp;

	PGT_CHECK(root == pgtp_level(pgtp_root(args->mm, args->vaddr)),
		  "root %u", root);
	/* At the top level of the configuration, the root steps as the PGD */
	if (root == PGT_MAX_LEVEL) {
		unsigned long end;

		p = pgtp_root(args->mm, args->vaddr);
		end = args->vaddr + 2 * pgtp_size(p);
		PGT_CHECK((unsigned long)pgtp_ptr(pgtp_next(p)) -
			  (unsigned long)pgtp_ptr(p) == sizeof(pgd_t), "");
		PGT_CHECK(pgtp_addr_end(p, args->vaddr, end) ==
			  pgd_addr_end(args->vaddr, end), "%#lx", args->vaddr);
	}
	for (lvl = 0; lvl <= root; lvl++)
		PGT_EXPECT_KIND(pgt_none_mk(lvl), PGT_NONE);

	/* init_args() linked a table at every level above 0 */
	for (lvl = root; lvl >= 1; lvl--) {
		p = pgt_find(args->mm, args->vaddr, lvl, &pp);
		PGT_CHECK(!pgtp_is_null(p), "lvl %u", lvl);
		if (!pgtp_is_null(p))
			PGT_EXPECT_KIND(pgtp_get_lockless(p), PGT_TABLE);
	}

	for (lvl = 0; lvl <= min(2, PGT_MAX_LEVEL); lvl++) {
		if (!pgt_arch_can_leaf(lvl))
			continue;
		if (lvl == 1 && !has_transparent_hugepage())
			continue;
		if (lvl == 2 && !(has_transparent_hugepage() &&
				      has_transparent_pud_hugepage()))
			continue;
		l = pgt_leaf_mk(pgt_level_pfn(args, lvl), args->page_prot, lvl);
		PGT_EXPECT_LEAF(l);
		PGT_CHECK(pgt_leaf_pfn(l) == pgt_level_pfn(args, lvl),
			  "lvl %u", lvl);
		PGT_CHECK(pgt_leaf_shift(l) == pgt_arch_shift(lvl), "lvl %u",
			  lvl);
		PGT_CHECK_NATIVE(pgprot_val(pgt_leaf_pgprot(l)) ==
				 pgprot_val(args->page_prot),
				 "lvl %u %#llx %#llx", lvl,
				 (u64)pgprot_val(pgt_leaf_pgprot(l)),
				 (u64)pgprot_val(args->page_prot));
		pgt_leaf_flag_tests(args, l);

		/* PROT_NONE is a leaf the hardware cannot use */
		m = pgt_leaf_modify(l, args->page_prot_none);
		PGT_EXPECT_LEAF(m);
		if (IS_ENABLED(CONFIG_ARCH_HAS_PTE_PROTNONE) && lvl < 2)
			PGT_CHECK(!pgt_leaf_accessible(m, false), "lvl %u",
				  lvl);
		PGT_CHECK(pgt_leaf_pfn(m) == pgt_level_pfn(args, lvl),
			  "lvl %u", lvl);
	}

	/* Software entries */
	PGT_EXPECT_SOFTLEAF(pgt_softleaf_mk(args->swp_entry, 0));
	e = make_readable_migration_entry(args->fixed_pte_pfn);
	PGT_EXPECT_SOFTLEAF(pgt_softleaf_mk(e, 0));
	e = make_writable_migration_entry(args->fixed_pte_pfn);
	s = pgt_softleaf_mk(e, 0);
	PGT_EXPECT_SOFTLEAF(s);
	PGT_CHECK(softleaf_from_pgt(s).val == e.val, "%#lx", e.val);
	e = make_pte_marker_entry(PTE_MARKER_GUARD);
	PGT_EXPECT_SOFTLEAF(pgt_softleaf_mk(e, 0));
	if (IS_ENABLED(CONFIG_MEM_SOFT_DIRTY)) {
		s = pgt_softleaf_mk(args->swp_entry, 0);
		s = pgt_softleaf_mksoft_dirty(s);
		PGT_EXPECT_SOFTLEAF(s);
		PGT_CHECK(pgt_softleaf_soft_dirty(s), "");
		PGT_CHECK(softleaf_from_pgt(s).val == args->swp_entry.val, "");
	}
	if (pgt_arch_can_softleaf(1)) {
		e = make_readable_migration_entry(args->fixed_pmd_pfn);
		PGT_EXPECT_SOFTLEAF(pgt_softleaf_mk(e, 1));
		e = make_writable_migration_entry(args->fixed_pmd_pfn);
		s = pgt_softleaf_mk(e, 1);
		PGT_EXPECT_SOFTLEAF(s);
		PGT_CHECK(softleaf_from_pgt(s).val == e.val, "%#lx", e.val);
	}
}

/*
 * pgt agrees with the legacy helpers of the level. On the fallback this
 * checks the level mapping and the classification order; on a native
 * implementation it checks the format table.
 */
static void __init pgt_legacy_check(pgtv_t v)
{
	enum pgt_kind k = pgtv_kind(v);

	if (!pgt_arch_can_leaf(v.lvl))
		PGT_CHECK(k != PGT_LEAF, "lvl %u val %#llx", v.lvl,
			  (u64)v.val);

	switch (v.lvl) {
	case 0: {
		pte_t pte = __pte(v.val);

		PGT_CHECK((k == PGT_NONE) == !!pte_none(pte), "%#llx",
			  (u64)v.val);
		if (k == PGT_NONE)
			break;
		PGT_CHECK((k == PGT_LEAF) == !!pte_present(pte), "%#llx",
			  (u64)v.val);
		PGT_CHECK(k == PGT_LEAF || k == PGT_SOFTLEAF, "%#llx",
			  (u64)v.val);
		if (k == PGT_LEAF) {
			pgt_leaf_t l = pgtv_to_leaf(v);

			PGT_CHECK(pgt_leaf_pfn(l) == pte_pfn(pte), "%#llx",
				  (u64)v.val);
			PGT_CHECK(pgt_leaf_write(l) == !!pte_write(pte), "");
			PGT_CHECK(pgt_leaf_dirty(l) == !!pte_dirty(pte), "");
			PGT_CHECK(pgt_leaf_young(l) == !!pte_young(pte), "");
			PGT_CHECK(pgt_leaf_special(l) == !!pte_special(pte),
				  "");
			PGT_CHECK(pgt_leaf_soft_dirty(l) ==
				  !!pte_soft_dirty(pte), "");
			PGT_CHECK(pgt_leaf_uffd_wp(l) == !!pte_uffd(pte), "");
		} else {
			pgt_softleaf_t s = pgtv_to_softleaf(v);

			PGT_CHECK(softleaf_from_pgt(s).val ==
				  softleaf_from_pte(pte).val, "%#llx",
				  (u64)v.val);
			PGT_CHECK(pgt_softleaf_soft_dirty(s) ==
				  !!pte_swp_soft_dirty(pte), "");
			PGT_CHECK(pgt_softleaf_uffd_wp(s) ==
				  !!pte_swp_uffd(pte), "");
		}
		break;
	}
	case 1: {
		pmd_t pmd = __pmd(v.val);

		PGT_CHECK((k == PGT_NONE) == !!pmd_none(pmd), "%#llx",
			  (u64)v.val);
		if (k == PGT_NONE)
			break;
		if (pmd_present(pmd)) {
			PGT_CHECK((k == PGT_LEAF) == !!pmd_leaf(pmd), "%#llx",
				  (u64)v.val);
			if (!pmd_leaf(pmd))
				PGT_CHECK((k == PGT_BAD) == !!pmd_bad(pmd),
					  "%#llx", (u64)v.val);
		} else {
			PGT_CHECK(k == (pgt_arch_can_softleaf(1) ?
					PGT_SOFTLEAF : pmd_leaf(pmd) ?
					PGT_LEAF : PGT_BAD),
				  "%#llx", (u64)v.val);
		}
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
		if (k == PGT_LEAF) {
			pgt_leaf_t l = pgtv_to_leaf(v);

			PGT_CHECK(pgt_leaf_pfn(l) == pmd_pfn(pmd), "%#llx",
				  (u64)v.val);
			PGT_CHECK(pgt_leaf_write(l) == !!pmd_write(pmd), "");
			PGT_CHECK(pgt_leaf_dirty(l) == !!pmd_dirty(pmd), "");
			PGT_CHECK(pgt_leaf_young(l) == !!pmd_young(pmd), "");
			PGT_CHECK(pgt_leaf_soft_dirty(l) ==
				  !!pmd_soft_dirty(pmd), "");
		}
#endif
		break;
	}
#if CONFIG_PGTABLE_LEVELS > 2
	case 2: {
		pud_t pud = __pud(v.val);

		PGT_CHECK((k == PGT_NONE) == !!pud_none(pud), "%#llx",
			  (u64)v.val);
		if (k == PGT_NONE)
			break;
		if (pud_present(pud)) {
			PGT_CHECK((k == PGT_LEAF) == !!pud_leaf(pud), "%#llx",
				  (u64)v.val);
			if (!pud_leaf(pud))
				PGT_CHECK((k == PGT_BAD) == !!pud_bad(pud),
					  "%#llx", (u64)v.val);
		} else {
			PGT_CHECK(k == (pud_leaf(pud) ? PGT_LEAF : PGT_BAD),
				  "%#llx", (u64)v.val);
		}
#if defined(CONFIG_TRANSPARENT_HUGEPAGE) && \
	defined(CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD)
		if (k == PGT_LEAF) {
			pgt_leaf_t l = pgtv_to_leaf(v);

			PGT_CHECK(pgt_leaf_pfn(l) == pud_pfn(pud), "%#llx",
				  (u64)v.val);
			PGT_CHECK(pgt_leaf_write(l) == !!pud_write(pud), "");
			PGT_CHECK(pgt_leaf_dirty(l) == !!pud_dirty(pud), "");
			PGT_CHECK(pgt_leaf_young(l) == !!pud_young(pud), "");
		}
#endif
		break;
	}
#endif
#if CONFIG_PGTABLE_LEVELS > 3
	case 3: {
		p4d_t p4d = __p4d(v.val);

		PGT_CHECK((k == PGT_NONE) == !!p4d_none(p4d), "%#llx",
			  (u64)v.val);
		if (k != PGT_NONE && p4d_present(p4d) && !p4d_leaf(p4d))
			PGT_CHECK((k == PGT_BAD) == !!p4d_bad(p4d), "%#llx",
				  (u64)v.val);
		break;
	}
#endif
#if CONFIG_PGTABLE_LEVELS > 4
	case 4: {
		pgd_t pgd = __pgd(v.val);

		PGT_CHECK((k == PGT_NONE) == !!pgd_none(pgd), "%#llx",
			  (u64)v.val);
		if (k != PGT_NONE && pgd_present(pgd) && !pgd_leaf(pgd))
			PGT_CHECK((k == PGT_BAD) == !!pgd_bad(pgd), "%#llx",
				  (u64)v.val);
		break;
	}
#endif
	default:
		break;
	}
}

static void __init pgt_legacy_tests(struct pgtable_debug_args *args)
{
	unsigned int lvl, i, root = pgt_root_level(args->mm);
	pgt_softleaf_t s;
	pgt_leaf_t l, m;
	pgtp_t p, pp;
	pte_t pte;

	/* pgt_leaf_mk() builds what the legacy constructors build */
	l = pgt_leaf_mk(args->fixed_pte_pfn, args->page_prot, 0);
	pte = pfn_pte(args->fixed_pte_pfn, args->page_prot);
	PGT_CHECK(l.v.val == pte_val(pte), "");
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	if (has_transparent_hugepage()) {
		pmd_t pmd = pfn_pmd(args->fixed_pmd_pfn, args->page_prot);

		l = pgt_leaf_mk(args->fixed_pmd_pfn, args->page_prot, 1);
		PGT_CHECK(l.v.val == pmd_val(pmd_mkhuge(pmd)), "");
	}
#endif
#if defined(CONFIG_TRANSPARENT_HUGEPAGE) && \
	defined(CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD)
	if (has_transparent_pud_hugepage()) {
		pud_t pud = pfn_pud(args->fixed_pud_pfn, args->page_prot);

		l = pgt_leaf_mk(args->fixed_pud_pfn, args->page_prot, 2);
		PGT_CHECK(l.v.val == pud_val(pud_mkhuge(pud)), "");
	}
#endif

	/* Constructed values */
	for (lvl = 0; lvl <= root; lvl++) {
		pgt_legacy_check(pgt_none_mk(lvl));
		if (!lvl)
			continue;
		p = pgt_find(args->mm, args->vaddr, lvl, &pp);
		if (!pgtp_is_null(p))
			pgt_legacy_check(pgtp_get_lockless(p));
	}
	for (lvl = 0; lvl <= min(2, PGT_MAX_LEVEL); lvl++) {
		if (!pgt_arch_can_leaf(lvl) ||
		    (lvl == 1 && !has_transparent_hugepage()) ||
		    (lvl == 2 && !(has_transparent_hugepage() &&
				      has_transparent_pud_hugepage())))
			continue;
		l = pgt_leaf_mk(pgt_level_pfn(args, lvl), args->page_prot, lvl);
		pgt_legacy_check(pgtv_from_leaf(l));
		m = pgt_leaf_modify(l, args->page_prot_none);
		pgt_legacy_check(pgtv_from_leaf(m));
		m = pgt_leaf_wrprotect(pgt_leaf_mkdirty(l));
		pgt_legacy_check(pgtv_from_leaf(m));
		m = pgt_leaf_mkwrite(pgt_leaf_mkold(l), args->vma);
		pgt_legacy_check(pgtv_from_leaf(m));
	}
	s = pgt_softleaf_mk(args->swp_entry, 0);
	pgt_legacy_check(pgtv_from_softleaf(s));
	s = pgt_softleaf_mk(args->leaf_entry, 0);
	pgt_legacy_check(pgtv_from_softleaf(s));
	if (pgt_arch_can_softleaf(1)) {
		s = pgt_softleaf_mk(args->leaf_entry, 1);
		pgt_legacy_check(pgtv_from_softleaf(s));
	}

	/* Random values */
	for (lvl = 0; lvl <= root; lvl++) {
		for (i = 0; i < 4096; i++) {
			pgtv_t r = { .val = (pgtval_t)get_random_u64(),
				     .lvl = lvl };

			pgt_legacy_check(r);
		}
	}
}

/*
 * Install a leaf in the empty level-1 entry next to the one init_args()
 * populated, invalidate it, and clear it again.
 */
static void __init pgt_invalidate_tests(struct pgtable_debug_args *args)
{
	unsigned long vaddr = (args->vaddr & PMD_MASK) ^ PMD_SIZE;
	pgt_leaf_t l, old;
	spinlock_t *ptl;
	pgtp_t p, pp;
	pgtv_t v;

	if (!has_transparent_hugepage() || args->pmd_pfn == ULONG_MAX)
		return;
	p = pgt_find(args->mm, vaddr, 1, &pp);
	if (pgtp_is_null(p) || pgtv_kind(pgtp_get_lockless(p)) != PGT_NONE)
		return;
	ptl = pgtp_lock_entry(args->mm, p);
	l = pgt_leaf_mk(args->pmd_pfn, args->page_prot, 1);
	pgtp_set_leaf(args->mm, vaddr, p, l);
	v = pgtp_get(p);
	PGT_EXPECT_KIND(v, PGT_LEAF);
	PGT_CHECK(pgt_leaf_accessible(pgtv_to_leaf(v), false), "%#llx",
		  (u64)v.val);
	old = pgtp_invalidate(args->vma, vaddr, p);
	PGT_CHECK(pgtv_same(pgtv_from_leaf(old), pgtv_from_leaf(l)),
		  "%#llx %#llx", (u64)old.v.val, (u64)l.v.val);
	v = pgtp_get(p);
	PGT_EXPECT_KIND(v, PGT_LEAF);
	l = pgtv_to_leaf(v);
	PGT_CHECK(!pgt_leaf_accessible(l, false), "%#llx", (u64)v.val);
	PGT_CHECK(pgt_leaf_pfn(l) == args->pmd_pfn, "%#lx", pgt_leaf_pfn(l));
	v = pgtp_xchg(args->mm, vaddr, p, pgt_none_mk(1));
	PGT_EXPECT_KIND(v, PGT_LEAF);
	PGT_EXPECT_KIND(pgtp_get(p), PGT_NONE);
	pgtp_unlock(p, ptl);
}

/*
 * Every level locks. A level-0 table locks with the right parent and is
 * refused under one that does not link it: the neighbouring entry in the
 * parent's table.
 */
static void __init pgt_lock_tests(struct pgtable_debug_args *args)
{
	unsigned int lvl, root = pgt_root_level(args->mm);
	unsigned long addr = args->vaddr, other;
	pgtp_t p, pp, q, qp;
	spinlock_t *ptl;
	pgtv_t v;

	for (lvl = 0; lvl <= root; lvl++) {
		p = pgt_find(args->mm, addr, lvl, &pp);
		if (pgtp_is_null(p))
			continue;
		ptl = lvl ? pgtp_lock_entry(args->mm, p) :
			    pgtp_lock_table(args->mm, p, pp);
		PGT_CHECK(ptl, "lvl %u", lvl);
		if (!ptl)
			continue;
		pgtp_assert_locked(args->mm, p);
		PGT_CHECK(pgtv_same(pgtp_get(p), pgtp_get_lockless(p)),
			  "lvl %u", lvl);
		pgtp_unlock(p, ptl);
		if (lvl)
			continue;

		/* An address that differs only in the parent's index */
		other = addr ^ (1UL << pgt_arch_shift(lvl + 1));
		q = pgt_find(args->mm, other, lvl + 1, &qp);
		if (pgtp_is_null(q) || pgtp_ptr(q) == pgtp_ptr(pp))
			continue;
		p = pgt_find(args->mm, addr, lvl, &pp);
		ptl = pgtp_lock_table(args->mm, p, q);
		PGT_CHECK(!ptl, "lvl %u", lvl);
		if (ptl)
			pgtp_unlock(p, ptl);
	}

	p = pgt_find(args->mm, addr, 0, &pp);
	PGT_CHECK(!pgtp_is_null(p) && pgtp_map(p, pp), "");
	if (!pgtp_is_null(p))
		pgtp_unmap(p);

	/* Link a level-0 table at the neighbouring level-1 entry and undo it */
	other = addr ^ PMD_SIZE;
	p = pgt_find(args->mm, other, 1, &pp);
	if (pgtp_is_null(p) || pgtv_kind(pgtp_get_lockless(p)) != PGT_NONE)
		return;
	PGT_CHECK(!pgt_alloc_child(args->mm, p, &v), "");
	PGT_EXPECT_KIND(v, PGT_TABLE);
	PGT_CHECK(!pgt_alloc_child(args->mm, p, &v), "");
	PGT_EXPECT_KIND(v, PGT_TABLE);
	ptl = pgtp_lock_entry(args->mm, p);
	v = pgtp_xchg(args->mm, other, p, pgt_none_mk(1));
	pgtp_unlock(p, ptl);
	PGT_EXPECT_KIND(v, PGT_TABLE);
	pte_free(args->mm, pmd_pgtable(__pmd(v.val)));
	mm_dec_nr_ptes(args->mm);
}

struct pgt_count {
	struct mm_struct *mm;
	unsigned long entries, bytes;
};

PGT_DEFINE_LEVEL_WALKER(pgt_count_walk, pgt_count_body, struct pgt_count *);

static __always_inline unsigned long
pgt_count_body(struct pgt_count *c, pgtp_t p, pgtp_t pp, unsigned long addr,
	       unsigned long end, const unsigned int lvl)
{
	unsigned long next, ret;
	spinlock_t *ptl = NULL;
	pgtp_t first = p, child;
	pgtv_t v;

	if (!lvl) {
		ptl = pgtp_lock_table(c->mm, p, pp);
		if (!ptl)
			return addr;
	}
	do {
		next = pgtp_addr_end(p, addr, end);
		v = lvl ? pgtp_get_lockless(p) : pgtp_get(p);
		if (lvl && pgtv_kind(v) == PGT_TABLE) {
			child = pgt_table_child(pgtv_to_table(v), addr);
			ret = pgt_count_walk(c, child, p, addr, next, lvl - 1);
			if (ret != next)
				return ret;
		} else {
			c->entries++;
			c->bytes += next - addr;
		}
	} while (p = pgtp_next(p), addr = next, addr != end);
	if (ptl)
		pgtp_unlock(first, ptl);
	return end;
}

/*
 * The iterator, in both modes, and a level walker see the same entries
 * over the range of the level-2 entry init_args() populated.
 */
static void __init pgt_walk_tests(struct pgtable_debug_args *args)
{
	unsigned long start, end, n, bytes;
	struct pgt_count c = { .mm = args->mm };
	unsigned int mode;
	pgtp_t root;
	pgtv_t v;

	if (PGT_MAX_LEVEL < 2)
		return;
	start = args->vaddr & PUD_MASK;
	end = start + PUD_SIZE;

	for (mode = 0; mode < 2; mode++) {
		PGT_ITER(it, args->mm, start, end,
			 mode ? PGT_ITER_LOCKED : PGT_ITER_RO);

		n = 0;
		bytes = 0;
		for (v = pgt_iter_first(&it); !pgt_iter_done(&it);
		     v = pgt_iter_next(&it)) {
			PGT_CHECK(pgtv_kind(v) != PGT_TABLE, "");
			PGT_CHECK(v.lvl == pgtp_level(pgt_iter_ptp(&it)), "");
			n++;
			bytes += pgt_iter_size(&it);
		}
		PGT_CHECK(n == PTRS_PER_PTE + PTRS_PER_PMD - 1, "mode %u n %lu",
			  mode, n);
		PGT_CHECK(bytes == PUD_SIZE, "mode %u bytes %#lx", mode, bytes);
	}

	root = pgtp_root(args->mm, start);
	PGT_CHECK(pgt_count_walk(&c, root, PGTP_NULL, start, end,
				 pgtp_level(root)) == end, "");
	PGT_CHECK(c.entries == PTRS_PER_PTE + PTRS_PER_PMD - 1, "%lu",
		  c.entries);
	PGT_CHECK(c.bytes == PUD_SIZE, "%#lx", c.bytes);
}

static void __init pgt_tests(struct pgtable_debug_args *args)
{
	pr_info("Validating level-generic page table API\n");
	pgt_kind_tests(args);
	pgt_legacy_tests(args);
	pgt_invalidate_tests(args);
	pgt_lock_tests(args);
	pgt_walk_tests(args);
	pr_info("pgt: %lu checks, %lu failures, %lu fallback gaps\n",
		pgt_checks, pgt_failures, pgt_gaps);
}
#else  /* !CONFIG_PGT */
static void __init pgt_tests(struct pgtable_debug_args *args) { }
#endif /* CONFIG_PGT */

static int __init debug_vm_pgtable(void)
{
	struct pgtable_debug_args args;
	spinlock_t *ptl = NULL;
	int idx, ret;

	pr_info("Validating architecture page table helpers\n");
	ret = init_args(&args);
	if (ret)
		return ret;

	/*
	 * Iterate over each possible vm_flags to make sure that all
	 * the basic page table transformation validations just hold
	 * true irrespective of the starting protection value for a
	 * given page table entry.
	 *
	 * Protection based vm_flags combinations are always linear
	 * and increasing i.e starting from VM_NONE and going up to
	 * (VM_SHARED | READ | WRITE | EXEC).
	 */
#define VM_FLAGS_START	(VM_NONE)
#define VM_FLAGS_END	(VM_SHARED | VM_EXEC | VM_WRITE | VM_READ)

	for (idx = VM_FLAGS_START; idx <= VM_FLAGS_END; idx++) {
		pte_basic_tests(&args, idx);
		pmd_basic_tests(&args, idx);
		pud_basic_tests(&args, idx);
	}

	/*
	 * Both P4D and PGD level tests are very basic which do not
	 * involve creating page table entries from the protection
	 * value and the given pfn. Hence just keep them out from
	 * the above iteration for now to save some test execution
	 * time.
	 */
	p4d_basic_tests(&args);
	pgd_basic_tests(&args);

	pmd_leaf_tests(&args);
	pud_leaf_tests(&args);

	pte_special_tests(&args);
	pte_protnone_tests(&args);
	pmd_protnone_tests(&args);

	pte_soft_dirty_tests(&args);
	pmd_soft_dirty_tests(&args);
	pte_swap_soft_dirty_tests(&args);
	pmd_leaf_soft_dirty_tests(&args);

	pte_swap_exclusive_tests(&args);

	pte_swap_tests(&args);
	pmd_softleaf_tests(&args);

	swap_migration_tests(&args);

	pmd_thp_tests(&args);
	pud_thp_tests(&args);

	hugetlb_basic_tests(&args);

	pgt_tests(&args);

	/*
	 * Page table modifying tests. They need to hold
	 * proper page table lock.
	 */

	args.ptep = pte_offset_map_lock(args.mm, args.pmdp, args.vaddr, &ptl);
	pte_clear_tests(&args);
	pte_advanced_tests(&args);
	if (args.ptep)
		pte_unmap_unlock(args.ptep, ptl);

	ptl = pmd_lock(args.mm, args.pmdp);
	pmd_clear_tests(&args);
	pmd_advanced_tests(&args);
	pmd_huge_tests(&args);
	pmd_populate_tests(&args);
	spin_unlock(ptl);

	ptl = pud_lock(args.mm, args.pudp);
	pud_clear_tests(&args);
	pud_advanced_tests(&args);
	pud_huge_tests(&args);
	pud_populate_tests(&args);
	spin_unlock(ptl);

	spin_lock(&(args.mm->page_table_lock));
	p4d_clear_tests(&args);
	pgd_clear_tests(&args);
	p4d_populate_tests(&args);
	pgd_populate_tests(&args);
	spin_unlock(&(args.mm->page_table_lock));

	destroy_args(&args);
	return 0;
}
late_initcall(debug_vm_pgtable);
