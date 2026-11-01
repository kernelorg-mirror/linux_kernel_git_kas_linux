// SPDX-License-Identifier: GPL-2.0
/*
 * pgt: the out-of-line defaults of the architecture contract, derived from
 * the format table of a native provider. See <asm-generic/pgt-fmt.h>.
 */
#include <linux/hugetlb.h>
#include <linux/leafops.h>
#include <linux/mm.h>
#include <linux/pgt.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>

#ifdef CONFIG_PGT_FALLBACK
#error "the format table defaults are for a native provider"
#endif

/*
 * An invalidated leaf is neither valid nor PROT_NONE. The new value is
 * computed from a snapshot; the exchange returns the value the hardware
 * last saw, with every accessed and dirty bit it set until then.
 */
#ifndef pgt_arch_invalidate
pgtval_t pgt_arch_invalidate(struct vm_area_struct *vma, unsigned long addr,
			     void *ptr, unsigned int lvl)
{
	unsigned long size = 1UL << pgt_arch_shift(lvl);
	unsigned long start = addr & ~(size - 1);
	pgtv_t old = __pgtv(pgt_arch_load(ptr, lvl), lvl);
	pgtv_t new = pgt_arch_clear_flag(old, PGT_F_VALID);
	pgtval_t val;

	new = pgt_arch_clear_flag(new, PGT_F_PROTNONE);
	new.val = __pgt_fmt_reinvert(old.val, new.val, lvl);
	val = pgt_arch_xchg(vma->vm_mm, addr, ptr, new.val, lvl);
	flush_tlb_range(vma, start, start + size);
	return val;
}
#endif

#ifndef pgt_arch_lockptr
spinlock_t *pgt_arch_lockptr(struct mm_struct *mm, pgtp_t p)
{
#ifdef CONFIG_SPLIT_PTE_PTLOCKS
	unsigned int lvl = pgtp_level(p);
	unsigned long size, base;

	if (pgt_fmt.split_lock & BIT(lvl)) {
		size = pgt_fmt.lvl[lvl].entries << pgt_arch_entry_order(lvl);
		base = (unsigned long)pgtp_ptr(p) & ~(size - 1);
		return ptlock_ptr(virt_to_ptdesc((void *)base));
	}
#endif
	return &mm->page_table_lock;
}
#endif

#ifndef pgt_arch_table_ctor
bool pgt_arch_table_ctor(struct mm_struct *mm, struct ptdesc *table,
			 unsigned int lvl)
{
	switch (lvl) {
	case 0:
		return pagetable_pte_ctor(mm, table);
	case 1:
		return pagetable_pmd_ctor(mm, table);
	case 2:
		pagetable_pud_ctor(table);
		return true;
	case 3:
		pagetable_p4d_ctor(table);
		return true;
	default:
		pagetable_pgd_ctor(table);
		return true;
	}
}
#endif

#ifndef pgt_arch_table_dtor
void pgt_arch_table_dtor(struct ptdesc *table, unsigned int lvl)
{
	pagetable_dtor(table);
}
#endif

#ifndef pgt_arch_table_alloc
struct ptdesc *pgt_arch_table_alloc(struct mm_struct *mm, unsigned int lvl)
{
	gfp_t gfp = mm == &init_mm ? GFP_PGTABLE_KERNEL : GFP_PGTABLE_USER;
	unsigned long entries = pgt_fmt.lvl[lvl].entries;
	unsigned long size = entries << pgt_arch_entry_order(lvl);
	pgtval_t none = pgt_fmt.lvl[lvl].none_val;
	struct ptdesc *table;
	unsigned int i;
	void *ptr;

	table = pagetable_alloc(gfp, get_order(size));
	if (!table)
		return NULL;
	if (!pgt_arch_table_ctor(mm, table, lvl)) {
		pagetable_free(table);
		return NULL;
	}
	if (!none)
		return table;
	ptr = ptdesc_address(table);
	for (i = 0; i < pgt_fmt.lvl[lvl].entries; i++) {
		__pgt_fmt_write(ptr, none, lvl);
		ptr += 1U << pgt_arch_entry_order(lvl);
	}
	return table;
}
#endif

#ifndef pgt_arch_table_free
void pgt_arch_table_free(struct ptdesc *table, unsigned int lvl)
{
	pgt_arch_table_dtor(table, lvl);
	pagetable_free(table);
}
#endif

#ifndef pgt_arch_clear_bad
void pgt_arch_clear_bad(pgtp_t p)
{
	unsigned int lvl = pgtp_level(p);
	pgtv_t v = pgtp_get(p);

	pr_err("pgt: bad level %u entry %p(%016llx)\n", lvl, pgtp_ptr(p),
	       (u64)v.val);
	pgt_arch_clear(NULL, 0, pgtp_ptr(p), lvl);
}
#endif

#ifndef pgt_arch_alloc_child
/* The page table accounting of a table linked below an entry at @lvl */
static void pgt_fmt_account(struct mm_struct *mm, unsigned int lvl)
{
	switch (lvl) {
	case 1:
		mm_inc_nr_ptes(mm);
		break;
	case 2:
		mm_inc_nr_pmds(mm);
		break;
	case 3:
		mm_inc_nr_puds(mm);
		break;
	default:
		break;
	}
}

/*
 * As the legacy __pXd_alloc() helpers: allocate, take the entry's lock,
 * recheck, and link or free.
 */
int pgt_arch_alloc_child(struct mm_struct *mm, pgtp_t p)
{
	unsigned int lvl = pgtp_level(p);
	struct ptdesc *table;
	spinlock_t *ptl;
	pgtv_t v;

	table = pgt_arch_table_alloc(mm, lvl - 1);
	if (!table)
		return -ENOMEM;
	ptl = pgtp_lock_entry(mm, p);
	v = pgtp_get(p);
	if (pgtv_kind(v) == PGT_NONE) {
		pgt_fmt_account(mm, lvl);
		/* See the comment in pmd_install() */
		smp_wmb();
		pgt_arch_populate(mm, pgtp_ptr(p), table, lvl);
		table = NULL;
	}
	pgtp_unlock(p, ptl);
	if (table)
		pgt_arch_table_free(table, lvl - 1);
	return 0;
}
#endif

#ifndef pgt_arch_to_softleaf
softleaf_t pgt_arch_to_softleaf(pgtv_t v)
{
	unsigned long type_mask = BIT(pgt_fmt.softleaf.type_bits) - 1;
	pgtval_t off_mask = BIT_ULL(pgt_fmt.softleaf.offset_bits) - 1;
	pgtval_t val = v.val;
	unsigned long type;
	pgoff_t off;

	if (pgt_arch_kind(v) != PGT_SOFTLEAF)
		return softleaf_mk_none();
	type = (val >> pgt_fmt.softleaf.type_shift) & type_mask;
	if (pgt_fmt.softleaf.invert_offset)
		val = ~val;
	off = (val >> pgt_fmt.softleaf.offset_shift) & off_mask;
	return swp_entry(type, off);
}
#endif

#ifndef pgt_arch_from_softleaf
pgtv_t pgt_arch_from_softleaf(softleaf_t e, unsigned int lvl)
{
	unsigned long type_mask = BIT(pgt_fmt.softleaf.type_bits) - 1;
	pgtval_t off_mask = BIT_ULL(pgt_fmt.softleaf.offset_bits) - 1;
	pgtval_t off = swp_offset(e);
	pgtval_t type = swp_type(e) & type_mask;
	pgtval_t val;

	if (pgt_fmt.softleaf.invert_offset)
		off = ~off;
	val = (off & off_mask) << pgt_fmt.softleaf.offset_shift;
	val |= type << pgt_fmt.softleaf.type_shift;
	VM_WARN_ON_ONCE(val & pgt_fmt.softleaf_mbz);
	return __pgtv(val, lvl);
}
#endif

#ifndef pgt_arch_leaf_batch
/*
 * As folio_pte_batch(): the following entries must be leaves of the same
 * folio and equal the expected value once the accessed, writable, dirty
 * and soft-dirty state is cleared from both.
 */
static pgtv_t pgt_fmt_batch_clean(pgtv_t v)
{
	v = pgt_arch_clear_flag(v, PGT_F_DIRTY);
	v = pgt_arch_clear_flag(v, PGT_F_SAVED_DIRTY);
	v = pgt_arch_clear_flag(v, PGT_F_SOFT_DIRTY);
	v = pgt_arch_wrprotect(v);
	return pgt_arch_clear_flag(v, PGT_F_YOUNG);
}

unsigned int pgt_arch_leaf_batch(struct folio *folio, pgtp_t p, pgtv_t v,
				 unsigned int max_nr)
{
	unsigned long end_pfn = folio_pfn(folio) + folio_nr_pages(folio);
	unsigned long room = end_pfn - pgt_arch_pfn(v);
	pgtv_t expected = pgt_fmt_batch_clean(v);
	unsigned int nr;

	if (room < max_nr)
		max_nr = room;
	for (nr = 1; nr < max_nr; nr++) {
		pgtv_t cur;

		p = pgtp_next(p);
		cur = pgtp_get(p);
		expected = pgt_arch_next(expected);
		if (pgtv_kind(cur) != PGT_LEAF)
			break;
		if (!pgt_arch_same(pgt_fmt_batch_clean(cur), expected))
			break;
	}
	return nr;
}
#endif
