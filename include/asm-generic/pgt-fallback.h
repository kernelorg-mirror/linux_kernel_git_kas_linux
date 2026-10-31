/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pgt fallback: the architecture contract on top of the legacy per-level
 * helpers.
 *
 * Level n uses the helpers of the n-th family counted from the leaf: pte,
 * pmd, pud, p4d, pgd. A level the architecture folds, at build time or at
 * run time, is not a pgt level: the root is the highest level the mm does
 * not fold, reached through the folded pXd_offset() steps.
 *
 * Everything here mirrors what the legacy helpers do on the architecture,
 * including where they disagree with the kinds model; see
 * Documentation/mm/pgt.rst.
 */
#ifndef _ASM_GENERIC_PGT_FALLBACK_H
#define _ASM_GENERIC_PGT_FALLBACK_H

#ifdef CONFIG_HIGHPTE
#include <linux/highmem.h>
#endif
#include <linux/pgtable.h>

#define __pgt_pte(v)	__pte((v).val)
#define __pgt_pmd(v)	__pmd((v).val)
#define __pgt_pud(v)	__pud((v).val)
#define __pgt_p4d(v)	__p4d((v).val)
#define __pgt_pgd(v)	__pgd((v).val)
#define __pgtv(x, l)	((pgtv_t){ .val = (x), .lvl = (l) })

/* The PUD leaf helpers exist only with THP on some architectures (riscv) */
#if defined(CONFIG_TRANSPARENT_HUGEPAGE) && \
	defined(CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD)
#define __PGT_PUD_LEAVES
#endif

/* Pointers */

/*
 * Levels folded at run time are always the topmost ones: x86 without 5-level
 * paging, riscv, arm64 with LPA2, s390. The root is the highest level that
 * is not folded, so the fallback has the same levels as a native
 * implementation and nothing below the root is folded.
 */
#define pgt_arch_root_level pgt_arch_root_level
static __always_inline unsigned int pgt_arch_root_level(struct mm_struct *mm)
{
	unsigned int lvl = PGT_MAX_LEVEL;

	if (lvl == 4 && mm_p4d_folded(mm))
		lvl = 3;
	if (lvl == 3 && mm_pud_folded(mm))
		lvl = 2;
	if (lvl == 2 && mm_pmd_folded(mm))
		lvl = 1;
	return lvl;
}

/*
 * The legacy offset helpers step through a folded level without reading
 * memory, except on s390, which decides from the type of the entry. The
 * entry they return is the parent entry itself (x86, riscv, s390) or the
 * entry for @addr in the same table (arm64).
 */
#define pgt_arch_root pgt_arch_root
static __always_inline pgtp_t pgt_arch_root(struct mm_struct *mm,
					    unsigned long addr)
{
	unsigned int lvl = pgt_arch_root_level(mm);
	pgd_t *pgd = pgd_offset(mm, addr);
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;

	if (lvl == 4)
		return pgtp_make(pgd, 4);
	p4d = p4d_offset(pgd, addr);
	if (lvl == 3)
		return pgtp_make(p4d, 3);
	pud = pud_offset(p4d, addr);
	if (lvl == 2)
		return pgtp_make(pud, 2);
	pmd = pmd_offset(pud, addr);
	return pgtp_make(pmd, 1);
}

/*
 * The top level of the configuration holds PGD entries. With fewer than
 * five levels it is a family folded at build time, the PGD seen through
 * another type: its pXd_addr_end() does not clip the range, and on 2-level
 * arm its type is half the size of a PGD entry. Use the PGD's geometry
 * there.
 */
#define pgt_arch_entry_order pgt_arch_entry_order
static __always_inline unsigned int pgt_arch_entry_order(unsigned int lvl)
{
	if (lvl == PGT_MAX_LEVEL)
		return ilog2(sizeof(pgd_t));
	switch (lvl) {
	case 0:
		return ilog2(sizeof(pte_t));
	case 1:
		return ilog2(sizeof(pmd_t));
	case 2:
		return ilog2(sizeof(pud_t));
	case 3:
		return ilog2(sizeof(p4d_t));
	default:
		return ilog2(sizeof(pgd_t));
	}
}

#define pgt_arch_shift pgt_arch_shift
static __always_inline unsigned int pgt_arch_shift(unsigned int lvl)
{
	switch (lvl) {
	case 0:
		return PAGE_SHIFT;
	case 1:
		return PMD_SHIFT;
	case 2:
		return PUD_SHIFT;
	case 3:
		return P4D_SHIFT;
	default:
		return PGDIR_SHIFT;
	}
}

/*
 * The child entry is computed from a stack copy of the snapshot, so the
 * entry is not read twice. Nothing below the root is folded; a folded level
 * would return the address of the copy.
 */
#define pgt_arch_child pgt_arch_child
static __always_inline pgtp_t pgt_arch_child(pgtv_t v, unsigned long addr)
{
	switch (v.lvl) {
	case 1: {
		pmd_t pmd = __pgt_pmd(v);
		pte_t *c = __pte_map(&pmd, addr);

		return pgtp_make(c, 0);
	}
#if CONFIG_PGTABLE_LEVELS > 2
	case 2: {
		pud_t pud = __pgt_pud(v);
		pmd_t *c = pmd_offset(&pud, addr);

		VM_WARN_ON_ONCE((void *)c == (void *)&pud);
		return pgtp_make(c, 1);
	}
#endif
#if CONFIG_PGTABLE_LEVELS > 3
	case 3: {
		p4d_t p4d = __pgt_p4d(v);
		pud_t *c = pud_offset(&p4d, addr);

		VM_WARN_ON_ONCE((void *)c == (void *)&p4d);
		return pgtp_make(c, 2);
	}
#endif
#if CONFIG_PGTABLE_LEVELS > 4
	case 4: {
		pgd_t pgd = __pgt_pgd(v);
		p4d_t *c = p4d_offset(&pgd, addr);

		VM_WARN_ON_ONCE((void *)c == (void *)&pgd);
		return pgtp_make(c, 3);
	}
#endif
	default:
		WARN_ON_ONCE(1);
		return PGTP_NULL;
	}
}

/*
 * Does the TABLE value @v link the table that contains @p? Tables may be
 * smaller than a page, so compare the position of @p against the table @v
 * links rather than the struct page.
 */
#define pgt_arch_links pgt_arch_links
static __always_inline bool pgt_arch_links(pgtv_t v, pgtp_t p)
{
	unsigned long ptr = (unsigned long)pgtp_ptr(p);
	unsigned long size, base;

	switch (v.lvl) {
	case 1: {
		pmd_t pmd = __pgt_pmd(v);
#ifdef CONFIG_HIGHPTE
		struct page *page = kmap_to_page((void *)ptr);

		return pmd_pfn(pmd) == page_to_pfn(page);
#else

		base = (unsigned long)pte_offset_kernel(&pmd, 0);
		size = PTRS_PER_PTE * sizeof(pte_t);
		break;
#endif
	}
#if CONFIG_PGTABLE_LEVELS > 2
	case 2: {
		pud_t pud = __pgt_pud(v);

		base = (unsigned long)pmd_offset(&pud, 0);
		size = PTRS_PER_PMD * sizeof(pmd_t);
		break;
	}
#endif
#if CONFIG_PGTABLE_LEVELS > 3
	case 3: {
		p4d_t p4d = __pgt_p4d(v);

		base = (unsigned long)pud_offset(&p4d, 0);
		size = PTRS_PER_PUD * sizeof(pud_t);
		break;
	}
#endif
#if CONFIG_PGTABLE_LEVELS > 4
	case 4: {
		pgd_t pgd = __pgt_pgd(v);

		base = (unsigned long)p4d_offset(&pgd, 0);
		size = PTRS_PER_P4D * sizeof(p4d_t);
		break;
	}
#endif
	default:
		WARN_ON_ONCE(1);
		return false;
	}
	return ptr - base < size;
}

#define pgt_arch_addr_end pgt_arch_addr_end
static __always_inline unsigned long pgt_arch_addr_end(pgtp_t p,
						       unsigned long addr,
						       unsigned long end)
{
	if (pgtp_level(p) == PGT_MAX_LEVEL)
		return pgd_addr_end(addr, end);
	switch (pgtp_level(p)) {
	case 0:
		return addr + PAGE_SIZE;
	case 1:
		return pmd_addr_end(addr, end);
	case 2:
		return pud_addr_end(addr, end);
	case 3:
		return p4d_addr_end(addr, end);
	default:
		return pgd_addr_end(addr, end);
	}
}

/* Out of line, so that this header does not need <linux/mm.h> */
#define pgt_arch_lockptr pgt_arch_lockptr
spinlock_t *pgt_arch_lockptr(struct mm_struct *mm, pgtp_t p);

/* Reads and stores */

#define pgt_arch_load pgt_arch_load
static __always_inline pgtval_t pgt_arch_load(const void *ptr, unsigned int lvl)
{
	switch (lvl) {
	case 0:
		return pte_val(ptep_get((pte_t *)ptr));
	case 1:
		return pmd_val(pmdp_get((pmd_t *)ptr));
#if CONFIG_PGTABLE_LEVELS > 2
	case 2:
		return pud_val(pudp_get((pud_t *)ptr));
#endif
#if CONFIG_PGTABLE_LEVELS > 3
	case 3:
		return p4d_val(p4dp_get((p4d_t *)ptr));
#endif
#if CONFIG_PGTABLE_LEVELS > 4
	case 4: {
		pgd_t *pgdp = (pgd_t *)ptr;

		return pgd_val(pgdp_get(pgdp));
	}
#endif
	default:
		WARN_ON_ONCE(1);
		return 0;
	}
}

/*
 * The lockless getters differ from the plain ones where an entry can be
 * read torn (64-bit entries in two halves) or spans a contiguous block
 * whose accessed and dirty bits must be gathered consistently (arm64).
 * There are none above level 1.
 */
#define pgt_arch_load_lockless pgt_arch_load_lockless
static __always_inline pgtval_t pgt_arch_load_lockless(const void *ptr,
						       unsigned int lvl)
{
	switch (lvl) {
	case 0:
		return pte_val(ptep_get_lockless((pte_t *)ptr));
	case 1:
		return pmd_val(pmdp_get_lockless((pmd_t *)ptr));
	default:
		return pgt_arch_load(ptr, lvl);
	}
}

#define pgt_arch_clear pgt_arch_clear
static __always_inline void pgt_arch_clear(struct mm_struct *mm,
					   unsigned long addr, void *ptr,
					   unsigned int lvl)
{
	switch (lvl) {
	case 0:
		pte_clear(mm, addr, (pte_t *)ptr);
		break;
	case 1: {
		pmd_t *pmdp = ptr;

		pmd_clear(pmdp);
		break;
	}
#if CONFIG_PGTABLE_LEVELS > 2
	case 2:
		pud_clear((pud_t *)ptr);
		break;
#endif
#if CONFIG_PGTABLE_LEVELS > 3
	case 3:
		p4d_clear((p4d_t *)ptr);
		break;
#endif
#if CONFIG_PGTABLE_LEVELS > 4
	case 4:
		pgd_clear((pgd_t *)ptr);
		break;
#endif
	default:
		WARN_ON_ONCE(1);
		break;
	}
}

/*
 * Leaves and software entries go through the set_pXd_at() family (page
 * table check, arch hooks). There is no common legacy setter for a table
 * entry (powerpc has no set_pmd(), two-level arm none at all): tables are
 * linked by pgt_alloc_child() only.
 */
#define pgt_arch_store_leaf pgt_arch_store_leaf
static __always_inline void pgt_arch_store_leaf(struct mm_struct *mm,
						unsigned long addr, void *ptr,
						pgtval_t val, unsigned int lvl,
						unsigned int nr)
{
	switch (lvl) {
	case 0:
		set_ptes(mm, addr, (pte_t *)ptr, __pte(val), nr);
		break;
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	case 1:
		set_pmd_at(mm, addr, (pmd_t *)ptr, __pmd(val));
		break;
#endif
#ifdef __PGT_PUD_LEAVES
	case 2:
		set_pud_at(mm, addr, (pud_t *)ptr, __pud(val));
		break;
#endif
	default:
		WARN_ON_ONCE(1);
		break;
	}
}

#define pgt_arch_store_softleaf pgt_arch_store_softleaf
static __always_inline void pgt_arch_store_softleaf(struct mm_struct *mm,
						    unsigned long addr,
						    void *ptr, pgtval_t val,
						    unsigned int lvl)
{
	switch (lvl) {
	case 0:
		set_pte_at(mm, addr, (pte_t *)ptr, __pte(val));
		break;
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	case 1:
		set_pmd_at(mm, addr, (pmd_t *)ptr, __pmd(val));
		break;
#endif
	default:
		WARN_ON_ONCE(1);
		break;
	}
}

#define pgt_arch_invalidate pgt_arch_invalidate
static __always_inline pgtval_t pgt_arch_invalidate(struct vm_area_struct *vma,
						    unsigned long addr,
						    void *ptr, unsigned int lvl)
{
	switch (lvl) {
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	case 1:
		return pmd_val(pmdp_invalidate(vma, addr, (pmd_t *)ptr));
#endif
#ifdef __PGT_PUD_LEAVES
	case 2:
		return pud_val(pudp_invalidate(vma, addr, (pud_t *)ptr));
#endif
	default:
		WARN_ON_ONCE(1);
		return pgt_arch_load(ptr, lvl);
	}
}

/* Classification */

#define pgt_arch_can_leaf pgt_arch_can_leaf
static __always_inline bool pgt_arch_can_leaf(unsigned int lvl)
{
	return !lvl || (lvl <= 2 && IS_ENABLED(CONFIG_PGTABLE_HAS_HUGE_LEAVES));
}

#define pgt_arch_can_softleaf pgt_arch_can_softleaf
static __always_inline bool pgt_arch_can_softleaf(unsigned int lvl)
{
	return !lvl || (lvl == 1 && IS_ENABLED(CONFIG_ARCH_HAS_PMD_SOFTLEAVES));
}

/*
 * pXd_leaf() is only defined for present entries (powerpc software
 * entries carry the leaf bit), so test present first. A non-present
 * entry at a level without software entries is a leaf if pXd_leaf() says
 * so: x86 pud_present() does not accept PROT_NONE. A macro, so that a
 * predicate is evaluated only when the order reaches it: a leaf must not
 * pay for pXd_bad().
 */
#define __pgt_kind(pfx, x, lvl)						\
({									\
	enum pgt_kind __k;						\
									\
	if (pfx##_none(x))						\
		__k = PGT_NONE;						\
	else if (pfx##_present(x))					\
		__k = pfx##_leaf(x) ? PGT_LEAF :			\
		      pfx##_bad(x) ? PGT_BAD : PGT_TABLE;		\
	else if (pgt_arch_can_softleaf(lvl))				\
		__k = PGT_SOFTLEAF;					\
	else								\
		__k = pfx##_leaf(x) ? PGT_LEAF : PGT_BAD;		\
	__k;								\
})

#define pgt_arch_kind pgt_arch_kind
static __always_inline enum pgt_kind pgt_arch_kind(pgtv_t v)
{
	switch (v.lvl) {
	case 0: {
		pte_t pte = __pgt_pte(v);

		if (pte_none(pte))
			return PGT_NONE;
		return pte_present(pte) ? PGT_LEAF : PGT_SOFTLEAF;
	}
	case 1: {
		pmd_t x = __pgt_pmd(v);

		return __pgt_kind(pmd, x, 1);
	}
#if CONFIG_PGTABLE_LEVELS > 2
	case 2: {
		pud_t x = __pgt_pud(v);

		return __pgt_kind(pud, x, 2);
	}
#endif
#if CONFIG_PGTABLE_LEVELS > 3
	case 3: {
		p4d_t x = __pgt_p4d(v);

		return __pgt_kind(p4d, x, 3);
	}
#endif
#if CONFIG_PGTABLE_LEVELS > 4
	case 4: {
		pgd_t x = __pgt_pgd(v);

		return __pgt_kind(pgd, x, 4);
	}
#endif
	default:
		WARN_ON_ONCE(1);
		return PGT_BAD;
	}
}

/*
 * The legacy helpers only exchange with an empty entry. Any other new
 * value is stored after the old one is taken out, under the entry's lock.
 */
#define pgt_arch_xchg pgt_arch_xchg
static __always_inline pgtval_t pgt_arch_xchg(struct mm_struct *mm,
					      unsigned long addr, void *ptr,
					      pgtval_t val, unsigned int lvl)
{
	pgtv_t new = __pgtv(val, lvl);
	enum pgt_kind kind;
	pgtval_t old;

	switch (lvl) {
	case 0:
		old = pte_val(ptep_get_and_clear(mm, addr, (pte_t *)ptr));
		break;
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	case 1:
		old = pmd_val(pmdp_huge_get_and_clear(mm, addr, (pmd_t *)ptr));
		break;
#if CONFIG_PGTABLE_LEVELS > 2
	case 2:
		old = pud_val(pudp_huge_get_and_clear(mm, addr, (pud_t *)ptr));
		break;
#endif
#endif
	default:
		old = pgt_arch_load(ptr, lvl);
		pgt_arch_clear(mm, addr, ptr, lvl);
		break;
	}
	kind = pgt_arch_kind(new);
	if (kind == PGT_LEAF)
		pgt_arch_store_leaf(mm, addr, ptr, val, lvl, 1);
	else if (kind == PGT_SOFTLEAF)
		pgt_arch_store_softleaf(mm, addr, ptr, val, lvl);
	else if (kind != PGT_NONE)
		WARN_ON_ONCE(1);
	return old;
}

/* Values */

#define pgt_arch_same pgt_arch_same
static __always_inline bool pgt_arch_same(pgtv_t a, pgtv_t b)
{
	switch (a.lvl) {
	case 0:
		return pte_same(__pgt_pte(a), __pgt_pte(b));
	case 1:
		return pmd_same(__pgt_pmd(a), __pgt_pmd(b));
#if CONFIG_PGTABLE_LEVELS > 2
	case 2:
		return pud_same(__pgt_pud(a), __pgt_pud(b));
#endif
#if CONFIG_PGTABLE_LEVELS > 3
	case 3:
		return p4d_same(__pgt_p4d(a), __pgt_p4d(b));
#endif
#if CONFIG_PGTABLE_LEVELS > 4
	case 4:
		return pgd_same(__pgt_pgd(a), __pgt_pgd(b));
#endif
	default:
		return a.val == b.val;
	}
}

/*
 * The legacy helpers have no way to name the empty value of a level, only
 * to test for it and to clear an entry. Zero is the empty value on every
 * architecture the fallback runs on; s390 is the known exception.
 */
#define pgt_arch_none_mk pgt_arch_none_mk
static __always_inline pgtv_t pgt_arch_none_mk(unsigned int lvl)
{
	return __pgtv(0, lvl);
}

/*
 * pXd_populate() both builds a table entry and stores it, with side
 * effects (paravirt pinning); there is no legacy way to build one alone.
 */
#define pgt_arch_table_entry pgt_arch_table_entry
static __always_inline pgtv_t pgt_arch_table_entry(struct ptdesc *table,
						   unsigned int lvl)
{
	WARN_ON_ONCE(1);
	return pgt_arch_none_mk(lvl);
}

#define pgt_arch_pfn pgt_arch_pfn
static __always_inline unsigned long pgt_arch_pfn(pgtv_t v)
{
	switch (v.lvl) {
	case 0:
		return pte_pfn(__pgt_pte(v));
	case 1:
		return pmd_pfn(__pgt_pmd(v));
#if CONFIG_PGTABLE_LEVELS > 2
	case 2:
		return pud_pfn(__pgt_pud(v));
#endif
	default:
		WARN_ON_ONCE(1);
		return 0;
	}
}

#define pgt_arch_pgprot pgt_arch_pgprot
static __always_inline pgprot_t pgt_arch_pgprot(pgtv_t v)
{
	switch (v.lvl) {
	case 0:
		return pte_pgprot(__pgt_pte(v));
	case 1:
		return pmd_pgprot(__pgt_pmd(v));
#if CONFIG_PGTABLE_LEVELS > 2
	case 2:
		return pud_pgprot(__pgt_pud(v));
#endif
	default:
		WARN_ON_ONCE(1);
		return __pgprot(0);
	}
}

#define pgt_arch_mk pgt_arch_mk
static __always_inline pgtv_t pgt_arch_mk(unsigned long pfn, pgprot_t prot,
					  unsigned int lvl)
{
	switch (lvl) {
	case 0: {
		pte_t pte = pfn_pte(pfn, prot);

		return __pgtv(pte_val(pte), 0);
	}
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	case 1: {
		pmd_t pmd = pfn_pmd(pfn, prot);

		pmd = pmd_mkhuge(pmd);
		return __pgtv(pmd_val(pmd), 1);
	}
#endif
#ifdef __PGT_PUD_LEAVES
	case 2: {
		pud_t pud = pfn_pud(pfn, prot);

		pud = pud_mkhuge(pud);
		return __pgtv(pud_val(pud), 2);
	}
#endif
	default:
		WARN_ON_ONCE(1);
		return pgt_arch_none_mk(lvl);
	}
}

#define pgt_arch_modify pgt_arch_modify
static __always_inline pgtv_t pgt_arch_modify(pgtv_t v, pgprot_t prot)
{
	switch (v.lvl) {
	case 0: {
		pte_t pte = pte_modify(__pgt_pte(v), prot);

		return __pgtv(pte_val(pte), 0);
	}
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	case 1: {
		pmd_t pmd = pmd_modify(__pgt_pmd(v), prot);

		return __pgtv(pmd_val(pmd), 1);
	}
#endif
#ifdef __PGT_PUD_LEAVES
	case 2: {
		pud_t pud = pud_modify(__pgt_pud(v), prot);

		return __pgtv(pud_val(pud), 2);
	}
#endif
	default:
		WARN_ON_ONCE(1);
		return v;
	}
}

#define pgt_arch_next pgt_arch_next
static __always_inline pgtv_t pgt_arch_next(pgtv_t v)
{
	unsigned long pfn, nr;
	pgprot_t prot;

	if (!v.lvl) {
		pte_t pte = pte_advance_pfn(__pgt_pte(v), 1);

		return __pgtv(pte_val(pte), 0);
	}
	nr = 1UL << (pgt_arch_shift(v.lvl) - PAGE_SHIFT);
	pfn = pgt_arch_pfn(v) + nr;
	prot = pgt_arch_pgprot(v);
	return pgt_arch_mk(pfn, prot, v.lvl);
}


/* Flags; a flag without a legacy helper at the level reads as clear */

static __always_inline bool __pgt_pte_test(pte_t pte, enum pgt_flag f)
{
	switch (f) {
	case PGT_F_VALID:
		return pte_present(pte) && !pte_protnone(pte);
	case PGT_F_WRITE:
		return pte_write(pte);
	case PGT_F_DIRTY:
		return pte_dirty(pte);
	case PGT_F_YOUNG:
		return pte_young(pte);
	case PGT_F_SPECIAL:
		return pte_special(pte);
	case PGT_F_SOFT_DIRTY:
		return pte_soft_dirty(pte);
	case PGT_F_UFFD_WP:
		return pte_uffd(pte);
	case PGT_F_PROTNONE:
		return pte_protnone(pte);
	case PGT_F_SWP_SOFT_DIRTY:
		return pte_swp_soft_dirty(pte);
	case PGT_F_SWP_UFFD_WP:
		return pte_swp_uffd(pte);
	case PGT_F_SWP_EXCLUSIVE:
		return pte_swp_exclusive(pte);
	default:
		return false;
	}
}

static __always_inline pte_t __pgt_pte_set(pte_t pte, enum pgt_flag f)
{
	switch (f) {
	case PGT_F_WRITE:
		return pte_mkwrite_novma(pte);
	case PGT_F_DIRTY:
		return pte_mkdirty(pte);
	case PGT_F_YOUNG:
		return pte_mkyoung(pte);
	case PGT_F_SPECIAL:
		return pte_mkspecial(pte);
	case PGT_F_SOFT_DIRTY:
		return pte_mksoft_dirty(pte);
	case PGT_F_UFFD_WP:
		return pte_mkuffd(pte);
	case PGT_F_SWP_SOFT_DIRTY:
		return pte_swp_mksoft_dirty(pte);
	case PGT_F_SWP_UFFD_WP:
		return pte_swp_mkuffd(pte);
	case PGT_F_SWP_EXCLUSIVE:
		return pte_swp_mkexclusive(pte);
	default:
		return pte;
	}
}

static __always_inline pte_t __pgt_pte_clear(pte_t pte, enum pgt_flag f)
{
	switch (f) {
	case PGT_F_WRITE:
		return pte_wrprotect(pte);
	case PGT_F_DIRTY:
		return pte_mkclean(pte);
	case PGT_F_YOUNG:
		return pte_mkold(pte);
	case PGT_F_SOFT_DIRTY:
		return pte_clear_soft_dirty(pte);
	case PGT_F_UFFD_WP:
		return pte_clear_uffd(pte);
	case PGT_F_SWP_SOFT_DIRTY:
		return pte_swp_clear_soft_dirty(pte);
	case PGT_F_SWP_UFFD_WP:
		return pte_swp_clear_uffd(pte);
	case PGT_F_SWP_EXCLUSIVE:
		return pte_swp_clear_exclusive(pte);
	default:
		return pte;
	}
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static __always_inline bool __pgt_pmd_test(pmd_t pmd, enum pgt_flag f)
{
	switch (f) {
	case PGT_F_VALID:
		return pmd_present(pmd) && !pmd_protnone(pmd) &&
		       pmd_access_permitted(pmd, false);
	case PGT_F_WRITE:
		return pmd_write(pmd);
	case PGT_F_DIRTY:
		return pmd_dirty(pmd);
	case PGT_F_YOUNG:
		return pmd_young(pmd);
	case PGT_F_SPECIAL:
		return pmd_special(pmd);
	case PGT_F_SOFT_DIRTY:
		return pmd_soft_dirty(pmd);
	case PGT_F_UFFD_WP:
		return pmd_uffd(pmd);
	case PGT_F_PROTNONE:
		return pmd_protnone(pmd);
	case PGT_F_SWP_SOFT_DIRTY:
		return pmd_swp_soft_dirty(pmd);
	case PGT_F_SWP_UFFD_WP:
		return pmd_swp_uffd(pmd);
	default:
		return false;
	}
}

static __always_inline pmd_t __pgt_pmd_set(pmd_t pmd, enum pgt_flag f)
{
	switch (f) {
	case PGT_F_WRITE:
		return pmd_mkwrite_novma(pmd);
	case PGT_F_DIRTY:
		return pmd_mkdirty(pmd);
	case PGT_F_YOUNG:
		return pmd_mkyoung(pmd);
	case PGT_F_SPECIAL:
		return pmd_mkspecial(pmd);
	case PGT_F_SOFT_DIRTY:
		return pmd_mksoft_dirty(pmd);
	case PGT_F_UFFD_WP:
		return pmd_mkuffd(pmd);
	case PGT_F_SWP_SOFT_DIRTY:
		return pmd_swp_mksoft_dirty(pmd);
	case PGT_F_SWP_UFFD_WP:
		return pmd_swp_mkuffd(pmd);
	default:
		return pmd;
	}
}

static __always_inline pmd_t __pgt_pmd_clear(pmd_t pmd, enum pgt_flag f)
{
	switch (f) {
	case PGT_F_WRITE:
		return pmd_wrprotect(pmd);
	case PGT_F_DIRTY:
		return pmd_mkclean(pmd);
	case PGT_F_YOUNG:
		return pmd_mkold(pmd);
	case PGT_F_SOFT_DIRTY:
		return pmd_clear_soft_dirty(pmd);
	case PGT_F_UFFD_WP:
		return pmd_clear_uffd(pmd);
	case PGT_F_SWP_SOFT_DIRTY:
		return pmd_swp_clear_soft_dirty(pmd);
	case PGT_F_SWP_UFFD_WP:
		return pmd_swp_clear_uffd(pmd);
	default:
		return pmd;
	}
}
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

#ifdef __PGT_PUD_LEAVES
static __always_inline bool __pgt_pud_test(pud_t pud, enum pgt_flag f)
{
	switch (f) {
	case PGT_F_VALID:
		return pud_present(pud) && pud_access_permitted(pud, false);
	case PGT_F_WRITE:
		return pud_write(pud);
	case PGT_F_DIRTY:
		return pud_dirty(pud);
	case PGT_F_YOUNG:
		return pud_young(pud);
	case PGT_F_SPECIAL:
		return pud_special(pud);
	default:
		return false;
	}
}

static __always_inline pud_t __pgt_pud_set(pud_t pud, enum pgt_flag f)
{
	switch (f) {
	case PGT_F_WRITE:
		return pud_mkwrite(pud);
	case PGT_F_DIRTY:
		return pud_mkdirty(pud);
	case PGT_F_YOUNG:
		return pud_mkyoung(pud);
	case PGT_F_SPECIAL:
		return pud_mkspecial(pud);
	default:
		return pud;
	}
}

static __always_inline pud_t __pgt_pud_clear(pud_t pud, enum pgt_flag f)
{
	switch (f) {
	case PGT_F_WRITE:
		return pud_wrprotect(pud);
	case PGT_F_DIRTY:
		return pud_mkclean(pud);
	case PGT_F_YOUNG:
		return pud_mkold(pud);
	default:
		return pud;
	}
}
#endif /* __PGT_PUD_LEAVES */

/*
 * Without the THP helpers, a leaf at levels 1 and 2 is a hugetlb leaf. Only
 * the write bit is known there, as legacy GUP-fast reads it.
 */
#define pgt_arch_test_flag pgt_arch_test_flag
static __always_inline bool pgt_arch_test_flag(pgtv_t v, enum pgt_flag f)
{
	switch (v.lvl) {
	case 0:
		return __pgt_pte_test(__pgt_pte(v), f);
	case 1:
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
		return __pgt_pmd_test(__pgt_pmd(v), f);
#elif defined(CONFIG_HUGETLB_PAGE)
		return f == PGT_F_WRITE && pmd_write(__pgt_pmd(v));
#else
		return false;
#endif
	case 2:
#ifdef __PGT_PUD_LEAVES
		return __pgt_pud_test(__pgt_pud(v), f);
#elif defined(CONFIG_HUGETLB_PAGE)
		return f == PGT_F_WRITE && pud_write(__pgt_pud(v));
#else
		return false;
#endif
	default:
		return false;
	}
}

#define pgt_arch_set_flag pgt_arch_set_flag
static __always_inline pgtv_t pgt_arch_set_flag(pgtv_t v, enum pgt_flag f)
{
	switch (v.lvl) {
	case 0: {
		pte_t pte = __pgt_pte_set(__pgt_pte(v), f);

		return __pgtv(pte_val(pte), 0);
	}
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	case 1: {
		pmd_t pmd = __pgt_pmd_set(__pgt_pmd(v), f);

		return __pgtv(pmd_val(pmd), 1);
	}
#endif
#ifdef __PGT_PUD_LEAVES
	case 2: {
		pud_t pud = __pgt_pud_set(__pgt_pud(v), f);

		return __pgtv(pud_val(pud), 2);
	}
#endif
	default:
		return v;
	}
}

#define pgt_arch_clear_flag pgt_arch_clear_flag
static __always_inline pgtv_t pgt_arch_clear_flag(pgtv_t v, enum pgt_flag f)
{
	switch (v.lvl) {
	case 0: {
		pte_t pte = __pgt_pte_clear(__pgt_pte(v), f);

		return __pgtv(pte_val(pte), 0);
	}
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	case 1: {
		pmd_t pmd = __pgt_pmd_clear(__pgt_pmd(v), f);

		return __pgtv(pmd_val(pmd), 1);
	}
#endif
#ifdef __PGT_PUD_LEAVES
	case 2: {
		pud_t pud = __pgt_pud_clear(__pgt_pud(v), f);

		return __pgtv(pud_val(pud), 2);
	}
#endif
	default:
		return v;
	}
}

#define pgt_arch_write pgt_arch_write
static __always_inline bool pgt_arch_write(pgtv_t v)
{
	return pgt_arch_test_flag(v, PGT_F_WRITE);
}

#define pgt_arch_accessible pgt_arch_accessible
static __always_inline bool pgt_arch_accessible(pgtv_t v, bool write)
{
	switch (v.lvl) {
	case 0: {
		pte_t pte = __pgt_pte(v);

		return !pte_protnone(pte) && pte_access_permitted(pte, write);
	}
	case 1: {
		pmd_t pmd = __pgt_pmd(v);

		return !pmd_protnone(pmd) && pmd_access_permitted(pmd, write);
	}
#if CONFIG_PGTABLE_LEVELS > 2
	case 2:
		return pud_access_permitted(__pgt_pud(v), write);
#endif
	default:
		return false;
	}
}

#define pgt_arch_mkwrite pgt_arch_mkwrite
static __always_inline pgtv_t pgt_arch_mkwrite(pgtv_t v,
					       struct vm_area_struct *vma)
{
	switch (v.lvl) {
	case 0: {
		pte_t pte = __pgt_pte(v);

		pte = vma ? pte_mkwrite(pte, vma) : pte_mkwrite_novma(pte);
		return __pgtv(pte_val(pte), 0);
	}
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	case 1: {
		pmd_t pmd = __pgt_pmd(v);

		pmd = vma ? pmd_mkwrite(pmd, vma) : pmd_mkwrite_novma(pmd);
		return __pgtv(pmd_val(pmd), 1);
	}
#endif
	default:
		return pgt_arch_set_flag(v, PGT_F_WRITE);
	}
}

#define pgt_arch_mkdirty pgt_arch_mkdirty
static __always_inline pgtv_t pgt_arch_mkdirty(pgtv_t v)
{
	return pgt_arch_set_flag(v, PGT_F_DIRTY);
}

#define pgt_arch_wrprotect pgt_arch_wrprotect
static __always_inline pgtv_t pgt_arch_wrprotect(pgtv_t v)
{
	return pgt_arch_clear_flag(v, PGT_F_WRITE);
}

/* Software entries; out of line, so this header needs no <linux/leafops.h> */

#define pgt_arch_to_softleaf pgt_arch_to_softleaf
softleaf_t pgt_arch_to_softleaf(pgtv_t v);

#define pgt_arch_from_softleaf pgt_arch_from_softleaf
pgtv_t pgt_arch_from_softleaf(softleaf_t e, unsigned int lvl);

/* Tables */

#define pgt_arch_clear_bad pgt_arch_clear_bad
static __always_inline void pgt_arch_clear_bad(pgtp_t p)
{
	void *ptr = pgtp_ptr(p);

	switch (pgtp_level(p)) {
	case 1:
		pmd_clear_bad((pmd_t *)ptr);
		break;
#if CONFIG_PGTABLE_LEVELS > 2
	case 2:
		pud_clear_bad((pud_t *)ptr);
		break;
#endif
#if CONFIG_PGTABLE_LEVELS > 3
	case 3:
		p4d_clear_bad((p4d_t *)ptr);
		break;
#endif
#if CONFIG_PGTABLE_LEVELS > 4
	case 4:
		pgd_clear_bad((pgd_t *)ptr);
		break;
#endif
	default:
		WARN_ON_ONCE(1);
		break;
	}
}

#define pgt_arch_alloc_child pgt_arch_alloc_child
int pgt_arch_alloc_child(struct mm_struct *mm, pgtp_t p);

#define pgt_arch_leaf_batch pgt_arch_leaf_batch
unsigned int pgt_arch_leaf_batch(struct folio *folio, pgtp_t p, pgtv_t v,
				 unsigned int max_nr);

/* TLB; macros, since they expand to the <asm/tlb.h> helpers */

/*
 * Without huge leaves nothing above level 0 is a leaf, and the helpers for
 * one do not build: HPAGE_PMD_SIZE is a BUILD_BUG() there
 */
#ifdef CONFIG_PGTABLE_HAS_HUGE_LEAVES
#define __pgt_tlb_remove_pmd_entry(tlb, p, addr)			\
	tlb_remove_pmd_tlb_entry(tlb, (pmd_t *)pgtp_ptr(p), addr)
#define __pgt_tlb_remove_pud_entry(tlb, p, addr)			\
	tlb_remove_pud_tlb_entry(tlb, (pud_t *)pgtp_ptr(p), addr)
#else
#define __pgt_tlb_remove_pmd_entry(tlb, p, addr)	WARN_ON_ONCE(1)
#define __pgt_tlb_remove_pud_entry(tlb, p, addr)	WARN_ON_ONCE(1)
#endif

#define pgt_arch_tlb_remove_entry(tlb, p, addr)				\
do {									\
	struct mmu_gather *__tlb = (tlb);				\
	unsigned long __addr = (addr);					\
	pgtp_t __p = (p);						\
									\
	switch (pgtp_level(__p)) {					\
	case 0:								\
		tlb_remove_tlb_entry(__tlb, (pte_t *)pgtp_ptr(__p),	\
				     __addr);				\
		break;							\
	case 1:								\
		__pgt_tlb_remove_pmd_entry(__tlb, __p, __addr);		\
		break;							\
	default:							\
		__pgt_tlb_remove_pud_entry(__tlb, __p, __addr);		\
		break;							\
	}								\
} while (0)

/*
 * Not every architecture has pud_pgtable() and p4d_pgtable(); the child
 * table is found with pXd_offset() at address 0, as pgt_arch_child() does.
 */
#if CONFIG_PGTABLE_LEVELS > 2
#define __pgt_tlb_remove_pmd_table(tlb, v, addr)			\
do {									\
	pud_t __pgt_pudv = __pgt_pud(v);				\
	pmd_t *__pgt_pmdp = pmd_offset(&__pgt_pudv, 0);			\
									\
	pmd_free_tlb(tlb, __pgt_pmdp, addr);				\
	mm_dec_nr_pmds((tlb)->mm);					\
} while (0)
#else
#define __pgt_tlb_remove_pmd_table(tlb, v, addr)	WARN_ON_ONCE(1)
#endif

#if CONFIG_PGTABLE_LEVELS > 3
#define __pgt_tlb_remove_pud_table(tlb, v, addr)			\
do {									\
	p4d_t __pgt_p4dv = __pgt_p4d(v);				\
	pud_t *__pgt_pudp = pud_offset(&__pgt_p4dv, 0);			\
									\
	pud_free_tlb(tlb, __pgt_pudp, addr);				\
	mm_dec_nr_puds((tlb)->mm);					\
} while (0)
#else
#define __pgt_tlb_remove_pud_table(tlb, v, addr)	WARN_ON_ONCE(1)
#endif

#define pgt_arch_tlb_remove_table(tlb, v, addr)				\
do {									\
	struct mmu_gather *__tlb = (tlb);				\
	unsigned long __addr = (addr);					\
	pgtv_t __v = (v);						\
									\
	switch (__v.lvl) {						\
	case 1: {							\
		pmd_t __pgt_pmdv = __pgt_pmd(__v);			\
		pgtable_t __pgt_table = pmd_pgtable(__pgt_pmdv);	\
									\
		pte_free_tlb(__tlb, __pgt_table, __addr);		\
		mm_dec_nr_ptes(__tlb->mm);				\
		break;							\
	}								\
	case 2:								\
		__pgt_tlb_remove_pmd_table(__tlb, __v, __addr);		\
		break;							\
	case 3:								\
		__pgt_tlb_remove_pud_table(__tlb, __v, __addr);		\
		break;							\
	default:							\
		WARN_ON_ONCE(1);					\
		break;							\
	}								\
} while (0)

#endif /* _ASM_GENERIC_PGT_FALLBACK_H */
