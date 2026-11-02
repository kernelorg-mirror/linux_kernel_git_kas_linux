/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pgt: the defaults of the architecture contract, derived from the format
 * table of a native provider.
 *
 * <asm/pgt.h> defines "static const struct pgt_fmt pgt_fmt" and overrides
 * the entry points whose behaviour the table cannot express; everything
 * else is computed here from the table. The out-of-line defaults are in
 * mm/pgt-fmt.c.
 *
 * A field known only at boot is read through its accessor below, which the
 * provider overrides; every other field is read from the table directly,
 * so that it folds to a constant at a constant level.
 */
#ifndef _ASM_GENERIC_PGT_FMT_H
#define _ASM_GENERIC_PGT_FMT_H

#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/mm_types.h>
#include <linux/pfn.h>
#include <asm/page.h>

#ifdef CONFIG_HIGHPTE
#error "the pgt format table defaults do not map level-0 tables"
#endif

#ifndef pgt_fmt_table_pfn_mask
#define pgt_fmt_table_pfn_mask(l)	(pgt_fmt.lvl[l].table_pfn_mask)
#endif
#ifndef pgt_fmt_leaf_pfn_mask
#define pgt_fmt_leaf_pfn_mask(l)	(pgt_fmt.lvl[l].leaf_pfn_mask)
#endif
#ifndef pgt_fmt_table_mask
#define pgt_fmt_table_mask(l)		(pgt_fmt.lvl[l].table_mask)
#endif
#ifndef pgt_fmt_table_val
#define pgt_fmt_table_val(l)		(pgt_fmt.lvl[l].table_val)
#endif
#ifndef pgt_fmt_table_flags
#define pgt_fmt_table_flags(l)	(pgt_fmt.lvl[l].table_flags)
#endif

static __always_inline pgtval_t __pgt_fmt_bit(unsigned int lvl,
					      enum pgt_flag f)
{
	return pgt_fmt.lvl[lvl].bit[f];
}

/* Physical address of the table a TABLE value links; never inverted */
static __always_inline phys_addr_t __pgt_fmt_table_pa(pgtv_t v)
{
	pgtval_t field = v.val & pgt_fmt_table_pfn_mask(v.lvl);
	unsigned long pfn = field >> pgt_fmt.lvl[v.lvl].pfn_shift;

	return PFN_PHYS(pfn);
}

/* Hooks */

#ifndef pgt_arch_load
#define pgt_arch_load pgt_arch_load
static __always_inline pgtval_t pgt_arch_load(const void *ptr,
					      unsigned int lvl)
{
	if (pgt_fmt.lvl[lvl].entry_order == 3)
		return READ_ONCE(*(const u64 *)ptr);
	return READ_ONCE(*(const u32 *)ptr);
}
#endif

#ifndef pgt_arch_load_lockless
#define pgt_arch_load_lockless pgt_arch_load_lockless
static __always_inline pgtval_t pgt_arch_load_lockless(const void *ptr,
						       unsigned int lvl)
{
	return pgt_arch_load(ptr, lvl);
}
#endif

static __always_inline void __pgt_fmt_write(void *ptr, pgtval_t val,
					    unsigned int lvl)
{
	if (pgt_fmt.lvl[lvl].entry_order == 3)
		WRITE_ONCE(*(u64 *)ptr, val);
	else
		WRITE_ONCE(*(u32 *)ptr, val);
}

#ifndef pgt_arch_pfn_inverted
#define pgt_arch_pfn_inverted pgt_arch_pfn_inverted
static __always_inline bool pgt_arch_pfn_inverted(pgtval_t val,
						  unsigned int lvl)
{
	return false;
}
#endif

static __always_inline pgtval_t __pgt_fmt_inv(pgtval_t val, unsigned int lvl)
{
	return pgt_arch_pfn_inverted(val, lvl) ? ~(pgtval_t)0 : 0;
}

/*
 * A change of protection can change whether the PFN is stored inverted;
 * flip the field when it does.
 */
static __always_inline pgtval_t __pgt_fmt_reinvert(pgtval_t old, pgtval_t val,
						   unsigned int lvl)
{
	pgtval_t mask = pgt_fmt_leaf_pfn_mask(lvl);

	if (pgt_arch_pfn_inverted(old, lvl) != pgt_arch_pfn_inverted(val, lvl))
		val ^= mask;
	return val;
}

#ifndef pgt_arch_root_table
#define pgt_arch_root_table pgt_arch_root_table
static __always_inline void *pgt_arch_root_table(struct mm_struct *mm,
						 unsigned long addr)
{
	return mm->pgd;
}
#endif

#ifndef pgt_arch_root_level
#define pgt_arch_root_level pgt_arch_root_level
static __always_inline unsigned int pgt_arch_root_level(struct mm_struct *mm)
{
	return PGT_MAX_LEVEL;
}
#endif

/* Geometry */

#ifndef pgt_arch_entry_order
#define pgt_arch_entry_order pgt_arch_entry_order
static __always_inline unsigned int pgt_arch_entry_order(unsigned int lvl)
{
	return pgt_fmt.lvl[lvl].entry_order;
}
#endif

#ifndef pgt_arch_shift
#define pgt_arch_shift pgt_arch_shift
static __always_inline unsigned int pgt_arch_shift(unsigned int lvl)
{
	return pgt_fmt.lvl[lvl].shift;
}
#endif

static __always_inline void *__pgt_fmt_index(void *table, unsigned long addr,
					     unsigned int lvl)
{
	unsigned long idx = addr >> pgt_arch_shift(lvl);

	idx &= pgt_fmt.lvl[lvl].entries - 1;
	return table + (idx << pgt_arch_entry_order(lvl));
}

#ifndef pgt_arch_root
#define pgt_arch_root pgt_arch_root
static __always_inline pgtp_t pgt_arch_root(struct mm_struct *mm,
					    unsigned long addr)
{
	unsigned int lvl = pgt_arch_root_level(mm);
	void *table = pgt_arch_root_table(mm, addr);

	return pgtp_make(__pgt_fmt_index(table, addr, lvl), lvl);
}
#endif

#ifndef pgt_arch_child
#define pgt_arch_child pgt_arch_child
static __always_inline pgtp_t pgt_arch_child(pgtv_t v, unsigned long addr)
{
	unsigned int lvl = v.lvl - 1;
	void *table = __va(__pgt_fmt_table_pa(v));

	return pgtp_make(__pgt_fmt_index(table, addr, lvl), lvl);
}
#endif

#ifndef pgt_arch_links
#define pgt_arch_links pgt_arch_links
static __always_inline bool pgt_arch_links(pgtv_t v, pgtp_t p)
{
	unsigned int lvl = v.lvl - 1;
	unsigned long base = (unsigned long)__va(__pgt_fmt_table_pa(v));
	unsigned long entries = pgt_fmt.lvl[lvl].entries;
	unsigned long size = entries << pgt_arch_entry_order(lvl);

	return (unsigned long)pgtp_ptr(p) - base < size;
}
#endif

#ifndef pgt_arch_addr_end
#define pgt_arch_addr_end pgt_arch_addr_end
static __always_inline unsigned long pgt_arch_addr_end(pgtp_t p,
						       unsigned long addr,
						       unsigned long end)
{
	unsigned int lvl = pgtp_level(p);
	unsigned long size, boundary;

	if (!lvl)
		return addr + PAGE_SIZE;
	size = 1UL << pgt_arch_shift(lvl);
	boundary = (addr + size) & ~(size - 1);
	return boundary - 1 < end - 1 ? boundary : end;
}
#endif

/* Classification */

#ifndef pgt_arch_can_leaf
#define pgt_arch_can_leaf pgt_arch_can_leaf
static __always_inline bool pgt_arch_can_leaf(unsigned int lvl)
{
	return pgt_fmt.can_leaf & BIT(lvl);
}
#endif

#ifndef pgt_arch_can_softleaf
#define pgt_arch_can_softleaf pgt_arch_can_softleaf
static __always_inline bool pgt_arch_can_softleaf(unsigned int lvl)
{
	return pgt_fmt.can_softleaf & BIT(lvl);
}
#endif

/* The order is the one Documentation/mm/pgt.rst gives */
#ifndef pgt_arch_kind
#define pgt_arch_kind pgt_arch_kind
static __always_inline enum pgt_kind pgt_arch_kind(pgtv_t v)
{
	const struct pgt_level_fmt *f = &pgt_fmt.lvl[v.lvl];
	pgtval_t val = v.val;

	if ((val & f->none_mask) == f->none_val)
		return PGT_NONE;
	if (!v.lvl)
		return val & f->present_mask ? PGT_LEAF : PGT_SOFTLEAF;
	if (f->leaf_mask && (val & f->leaf_mask) == f->leaf_val)
		return PGT_LEAF;
	if (!(val & f->present_mask))
		return pgt_arch_can_softleaf(v.lvl) ? PGT_SOFTLEAF : PGT_BAD;
	if ((val & pgt_fmt_table_mask(v.lvl)) != pgt_fmt_table_val(v.lvl))
		return PGT_BAD;
	return val & f->mbz ? PGT_BAD : PGT_TABLE;
}
#endif

/* Values */

#ifndef pgt_arch_same
#define pgt_arch_same pgt_arch_same
static __always_inline bool pgt_arch_same(pgtv_t a, pgtv_t b)
{
	return a.val == b.val;
}
#endif

#ifndef pgt_arch_none_mk
#define pgt_arch_none_mk pgt_arch_none_mk
static __always_inline pgtv_t pgt_arch_none_mk(unsigned int lvl)
{
	return __pgtv(pgt_fmt.lvl[lvl].none_val, lvl);
}
#endif

#ifndef pgt_arch_table_entry
#define pgt_arch_table_entry pgt_arch_table_entry
static __always_inline pgtv_t pgt_arch_table_entry(struct ptdesc *table,
						   unsigned int lvl)
{
	pgtval_t pfn = page_to_pfn(ptdesc_page(table));
	pgtval_t field = pfn << pgt_fmt.lvl[lvl].pfn_shift;

	field &= pgt_fmt_table_pfn_mask(lvl);
	return __pgtv(field | pgt_fmt_table_flags(lvl), lvl);
}
#endif

/* Flags */

/*
 * A PROT_NONE leaf is one with the PROT_NONE bit and without the valid
 * bit: formats that have the bit reuse it in valid entries.
 */
#ifndef pgt_arch_test_flag
#define pgt_arch_test_flag pgt_arch_test_flag
static __always_inline bool pgt_arch_test_flag(pgtv_t v, enum pgt_flag f)
{
	pgtval_t bit = __pgt_fmt_bit(v.lvl, f);

	if (f == PGT_F_PROTNONE && (v.val & __pgt_fmt_bit(v.lvl, PGT_F_VALID)))
		return false;
	return v.val & bit;
}
#endif

#ifndef pgt_arch_set_flag
#define pgt_arch_set_flag pgt_arch_set_flag
static __always_inline pgtv_t pgt_arch_set_flag(pgtv_t v, enum pgt_flag f)
{
	v.val |= __pgt_fmt_bit(v.lvl, f);
	return v;
}
#endif

#ifndef pgt_arch_clear_flag
#define pgt_arch_clear_flag pgt_arch_clear_flag
static __always_inline pgtv_t pgt_arch_clear_flag(pgtv_t v, enum pgt_flag f)
{
	v.val &= ~__pgt_fmt_bit(v.lvl, f);
	return v;
}
#endif

#ifndef pgt_arch_write
#define pgt_arch_write pgt_arch_write
static __always_inline bool pgt_arch_write(pgtv_t v)
{
	return pgt_arch_test_flag(v, PGT_F_WRITE);
}
#endif

#ifndef pgt_arch_accessible
#define pgt_arch_accessible pgt_arch_accessible
static __always_inline bool pgt_arch_accessible(pgtv_t v, bool write)
{
	if (!pgt_arch_test_flag(v, PGT_F_VALID))
		return false;
	if (__pgt_fmt_bit(v.lvl, PGT_F_USER) &&
	    !pgt_arch_test_flag(v, PGT_F_USER))
		return false;
	return !write || pgt_arch_write(v);
}
#endif

#ifndef pgt_arch_mkwrite
#define pgt_arch_mkwrite pgt_arch_mkwrite
static __always_inline pgtv_t pgt_arch_mkwrite(pgtv_t v,
					       struct vm_area_struct *vma)
{
	v = pgt_arch_set_flag(v, PGT_F_WRITE);
	if (!pgt_arch_test_flag(v, PGT_F_SAVED_DIRTY))
		return v;
	v = pgt_arch_clear_flag(v, PGT_F_SAVED_DIRTY);
	return pgt_arch_set_flag(v, PGT_F_DIRTY);
}
#endif

#ifndef pgt_arch_mkdirty
#define pgt_arch_mkdirty pgt_arch_mkdirty
static __always_inline pgtv_t pgt_arch_mkdirty(pgtv_t v)
{
	v = pgt_arch_set_flag(v, PGT_F_DIRTY);
	return pgt_arch_set_flag(v, PGT_F_SOFT_DIRTY);
}
#endif

#ifndef pgt_arch_wrprotect
#define pgt_arch_wrprotect pgt_arch_wrprotect
static __always_inline pgtv_t pgt_arch_wrprotect(pgtv_t v)
{
	return pgt_arch_clear_flag(v, PGT_F_WRITE);
}
#endif

/* Leaves */

#ifndef pgt_arch_pfn
#define pgt_arch_pfn pgt_arch_pfn
static __always_inline unsigned long pgt_arch_pfn(pgtv_t v)
{
	pgtval_t val = v.val ^ __pgt_fmt_inv(v.val, v.lvl);

	val &= pgt_fmt_leaf_pfn_mask(v.lvl);
	return val >> pgt_fmt.lvl[v.lvl].pfn_shift;
}
#endif

/* Whether the PFN is stored inverted is decided by the protection alone */
#ifndef pgt_arch_mk
#define pgt_arch_mk pgt_arch_mk
static __always_inline pgtv_t pgt_arch_mk(unsigned long pfn, pgprot_t prot,
					  unsigned int lvl)
{
	pgtval_t protval = pgprot_val(prot);
	pgtval_t field = (pgtval_t)pfn << pgt_fmt.lvl[lvl].pfn_shift;

	field ^= __pgt_fmt_inv(protval, lvl);
	field &= pgt_fmt_leaf_pfn_mask(lvl);
	if (lvl)
		protval |= pgt_fmt.lvl[lvl].leaf_val;
	return __pgtv(field | protval, lvl);
}
#endif

#ifndef pgt_arch_pgprot
#define pgt_arch_pgprot pgt_arch_pgprot
static __always_inline pgprot_t pgt_arch_pgprot(pgtv_t v)
{
	pgtval_t protval = v.val & ~pgt_fmt_leaf_pfn_mask(v.lvl);

	if (v.lvl)
		protval &= ~pgt_fmt.lvl[v.lvl].leaf_mask;
	return __pgprot(protval);
}
#endif

/* Kept from the old value: the PFN and the state the hardware or mm set */
static __always_inline pgtval_t __pgt_fmt_modify_keep(unsigned int lvl)
{
	pgtval_t keep = pgt_fmt_leaf_pfn_mask(lvl);

	keep |= __pgt_fmt_bit(lvl, PGT_F_YOUNG);
	keep |= __pgt_fmt_bit(lvl, PGT_F_DIRTY);
	keep |= __pgt_fmt_bit(lvl, PGT_F_SAVED_DIRTY);
	keep |= __pgt_fmt_bit(lvl, PGT_F_SOFT_DIRTY);
	keep |= __pgt_fmt_bit(lvl, PGT_F_SPECIAL);
	keep |= __pgt_fmt_bit(lvl, PGT_F_UFFD_WP);
	if (lvl)
		keep |= pgt_fmt.lvl[lvl].leaf_mask;
	return keep;
}

#ifndef pgt_arch_modify
#define pgt_arch_modify pgt_arch_modify
static __always_inline pgtv_t pgt_arch_modify(pgtv_t v, pgprot_t prot)
{
	pgtval_t keep = __pgt_fmt_modify_keep(v.lvl);
	pgtval_t val = (v.val & keep) | (pgprot_val(prot) & ~keep);

	return __pgtv(__pgt_fmt_reinvert(v.val, val, v.lvl), v.lvl);
}
#endif

#ifndef pgt_arch_next
#define pgt_arch_next pgt_arch_next
static __always_inline pgtv_t pgt_arch_next(pgtv_t v)
{
	unsigned int order = pgt_arch_shift(v.lvl) - PAGE_SHIFT;
	pgtval_t mask = pgt_fmt_leaf_pfn_mask(v.lvl);
	pgtval_t pfn = pgt_arch_pfn(v) + (1UL << order);
	pgtval_t field = pfn << pgt_fmt.lvl[v.lvl].pfn_shift;

	field ^= __pgt_fmt_inv(v.val, v.lvl);
	v.val = (v.val & ~mask) | (field & mask);
	return v;
}
#endif

/* Reads and stores; plain memory accesses unless the provider overrides */

#ifndef pgt_arch_store_leaf
#define pgt_arch_store_leaf pgt_arch_store_leaf
static __always_inline void pgt_arch_store_leaf(struct mm_struct *mm,
						unsigned long addr, void *ptr,
						pgtval_t val, unsigned int lvl,
						unsigned int nr)
{
	pgtv_t v = __pgtv(val, lvl);
	unsigned int size = 1U << pgt_arch_entry_order(lvl);

	for (;;) {
		__pgt_fmt_write(ptr, v.val, lvl);
		if (--nr == 0)
			break;
		ptr += size;
		v = pgt_arch_next(v);
	}
}
#endif

#ifndef pgt_arch_store_softleaf
#define pgt_arch_store_softleaf pgt_arch_store_softleaf
static __always_inline void pgt_arch_store_softleaf(struct mm_struct *mm,
						    unsigned long addr,
						    void *ptr, pgtval_t val,
						    unsigned int lvl)
{
	__pgt_fmt_write(ptr, val, lvl);
}
#endif

#ifndef pgt_arch_xchg
#define pgt_arch_xchg pgt_arch_xchg
static __always_inline pgtval_t pgt_arch_xchg(struct mm_struct *mm,
					      unsigned long addr, void *ptr,
					      pgtval_t val, unsigned int lvl)
{
	if (pgt_fmt.lvl[lvl].entry_order == 3)
		return xchg((u64 *)ptr, val);
	return xchg((u32 *)ptr, val);
}
#endif

#ifndef pgt_arch_get_and_clear
#define pgt_arch_get_and_clear pgt_arch_get_and_clear
static __always_inline pgtval_t pgt_arch_get_and_clear(struct mm_struct *mm,
						       unsigned long addr,
						       void *ptr,
						       unsigned int lvl,
						       bool full)
{
	pgtv_t none = pgt_arch_none_mk(lvl);

	return pgt_arch_xchg(mm, addr, ptr, none.val, lvl);
}
#endif

#ifndef pgt_arch_get_and_clear_leaves
#define pgt_arch_get_and_clear_leaves pgt_arch_get_and_clear_leaves
static __always_inline pgtval_t
pgt_arch_get_and_clear_leaves(struct vm_area_struct *vma, unsigned long addr,
			      void *ptr, unsigned int lvl, unsigned int nr,
			      bool full)
{
	unsigned int step = 1U << pgt_arch_entry_order(lvl);
	struct mm_struct *mm = vma->vm_mm;
	pgtval_t val = pgt_arch_get_and_clear(mm, addr, ptr, lvl, full);
	pgtv_t first = __pgtv(val, lvl);
	pgtv_t cur;

	while (--nr) {
		ptr += step;
		addr += PAGE_SIZE;
		val = pgt_arch_get_and_clear(mm, addr, ptr, lvl, full);
		cur = __pgtv(val, lvl);
		if (pgt_arch_test_flag(cur, PGT_F_DIRTY) ||
		    pgt_arch_test_flag(cur, PGT_F_SAVED_DIRTY))
			first = pgt_arch_mkdirty(first);
		if (pgt_arch_test_flag(cur, PGT_F_YOUNG))
			first = pgt_arch_set_flag(first, PGT_F_YOUNG);
	}
	return first.val;
}
#endif

#ifndef pgt_arch_clear_leaves
#define pgt_arch_clear_leaves pgt_arch_clear_leaves
static __always_inline void pgt_arch_clear_leaves(struct vm_area_struct *vma,
						  unsigned long addr,
						  void *ptr, unsigned int lvl,
						  unsigned int nr, bool full)
{
	unsigned int step = 1U << pgt_arch_entry_order(lvl);

	for (;;) {
		pgt_arch_get_and_clear(vma->vm_mm, addr, ptr, lvl, full);
		if (--nr == 0)
			break;
		ptr += step;
		addr += PAGE_SIZE;
	}
}
#endif

#ifndef pgt_arch_clear_softleaves
#define pgt_arch_clear_softleaves pgt_arch_clear_softleaves
static __always_inline void
pgt_arch_clear_softleaves(struct vm_area_struct *vma, unsigned long addr,
			  void *ptr, unsigned int lvl, unsigned int nr,
			  bool full)
{
	unsigned int step = 1U << pgt_arch_entry_order(lvl);

	for (;;) {
		pgt_arch_clear(vma->vm_mm, addr, ptr, lvl);
		if (--nr == 0)
			break;
		ptr += step;
		addr += PAGE_SIZE;
	}
}
#endif

#ifndef pgt_arch_check_zapped
#define pgt_arch_check_zapped pgt_arch_check_zapped
static __always_inline void pgt_arch_check_zapped(struct vm_area_struct *vma,
						  pgtv_t v)
{
}
#endif

/* The swap offset field moves by one; it may be stored inverted */
#ifndef pgt_arch_softleaf_next
#define pgt_arch_softleaf_next pgt_arch_softleaf_next
static __always_inline pgtv_t pgt_arch_softleaf_next(pgtv_t v)
{
	pgtval_t one = (pgtval_t)1 << pgt_fmt.softleaf.offset_shift;

	if (pgt_fmt.softleaf.invert_offset)
		v.val -= one;
	else
		v.val += one;
	return v;
}
#endif

#ifndef pgt_arch_clear
#define pgt_arch_clear pgt_arch_clear
static __always_inline void pgt_arch_clear(struct mm_struct *mm,
					   unsigned long addr, void *ptr,
					   unsigned int lvl)
{
	__pgt_fmt_write(ptr, pgt_fmt.lvl[lvl].none_val, lvl);
}
#endif

#ifndef pgt_arch_populate
#define pgt_arch_populate pgt_arch_populate
static __always_inline void pgt_arch_populate(struct mm_struct *mm, void *ptr,
					      struct ptdesc *table,
					      unsigned int lvl)
{
	pgtv_t v = pgt_arch_table_entry(table, lvl);

	__pgt_fmt_write(ptr, v.val, lvl);
}
#endif

#endif /* _ASM_GENERIC_PGT_FMT_H */
