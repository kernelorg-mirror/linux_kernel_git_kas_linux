// SPDX-License-Identifier: GPL-2.0
/*
 * pgt fallback: the out-of-line parts of the architecture contract on top
 * of the legacy per-level helpers. See <asm-generic/pgt-fallback.h>.
 */
#include <linux/highmem.h>
#include <linux/leafops.h>
#include <linux/mm.h>
#include <linux/pgt.h>

#include "internal.h"

/* The fallback copies pXd_val() into pgtval_t, which must not lose bits */
static_assert(sizeof(pgtval_t) >= sizeof(pte_val(*(pte_t *)0)));
static_assert(sizeof(pgtval_t) >= sizeof(pmd_val(*(pmd_t *)0)));
#if CONFIG_PGTABLE_LEVELS > 2
static_assert(sizeof(pgtval_t) >= sizeof(pud_val(*(pud_t *)0)));
#endif
#if CONFIG_PGTABLE_LEVELS > 3
static_assert(sizeof(pgtval_t) >= sizeof(p4d_val(*(p4d_t *)0)));
#endif
#if CONFIG_PGTABLE_LEVELS > 4
static_assert(sizeof(pgtval_t) >= sizeof(pgd_val(*(pgd_t *)0)));
#endif

#if defined(CONFIG_HIGHPTE) && defined(CONFIG_SPLIT_PTE_PTLOCKS)
/* ptep_lockptr() does not take a kmapped pointer */
static spinlock_t *pgt_kmapped_ptlock(pgtp_t p)
{
	struct page *page = kmap_to_page(pgtp_ptr(p));

	return ptlock_ptr(page_ptdesc(page));
}
#endif

spinlock_t *pgt_arch_lockptr(struct mm_struct *mm, pgtp_t p)
{
	switch (pgtp_level(p)) {
	case 0:
#if defined(CONFIG_HIGHPTE) && defined(CONFIG_SPLIT_PTE_PTLOCKS)
		return pgt_kmapped_ptlock(p);
#else
		return ptep_lockptr(mm, pgtp_ptr(p));
#endif
	case 1:
		return pmd_lockptr(mm, (pmd_t *)pgtp_ptr(p));
	default:
		return &mm->page_table_lock;
	}
}

softleaf_t pgt_arch_to_softleaf(pgtv_t v)
{
	switch (v.lvl) {
	case 0:
		return softleaf_from_pte(__pgt_pte(v));
	case 1:
		return softleaf_from_pmd(__pgt_pmd(v));
	default:
		return softleaf_mk_none();
	}
}

pgtv_t pgt_arch_from_softleaf(softleaf_t e, unsigned int lvl)
{
	switch (lvl) {
	case 0: {
		pte_t pte = softleaf_to_pte(e);

		return __pgtv(pte_val(pte), 0);
	}
#ifdef CONFIG_ARCH_HAS_PMD_SOFTLEAVES
	case 1: {
		pmd_t pmd = softleaf_to_pmd(e);

		return __pgtv(pmd_val(pmd), 1);
	}
#endif
	default:
		WARN_ON_ONCE(1);
		return pgt_arch_none_mk(lvl);
	}
}

/* Swap entries are batched at level 0 only */
pgtv_t pgt_arch_softleaf_next(pgtv_t v)
{
	pte_t pte;

	if (WARN_ON_ONCE(v.lvl))
		return v;
	pte = pte_next_swp_offset(__pgt_pte(v));
	return __pgtv(pte_val(pte), 0);
}

/*
 * The legacy __pXd_alloc() helpers allocate, take the parent's lock,
 * recheck and populate or free. The address they take is only passed to
 * pXd_alloc_one(), which no architecture uses.
 */
int pgt_arch_alloc_child(struct mm_struct *mm, pgtp_t p)
{
	void *ptr = pgtp_ptr(p);

	switch (pgtp_level(p)) {
	case 1:
		return __pte_alloc(mm, (pmd_t *)ptr);
#if CONFIG_PGTABLE_LEVELS > 2
	case 2:
		return __pmd_alloc(mm, (pud_t *)ptr, 0);
#endif
#if CONFIG_PGTABLE_LEVELS > 3
	case 3:
		return __pud_alloc(mm, (p4d_t *)ptr, 0);
#endif
#if CONFIG_PGTABLE_LEVELS > 4
	case 4:
		return __p4d_alloc(mm, (pgd_t *)ptr, 0);
#endif
	default:
		WARN_ON_ONCE(1);
		return -EINVAL;
	}
}

unsigned int pgt_arch_leaf_batch(struct folio *folio, pgtp_t p, pgtv_t v,
				 unsigned int max_nr)
{
	pte_t pte = __pgt_pte(v);

	return folio_pte_batch(folio, pgtp_ptr(p), pte, max_nr);
}
