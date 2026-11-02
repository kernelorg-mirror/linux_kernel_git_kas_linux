/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Native x86-64 provider of the pgt architecture contract: the entry format
 * of the five levels, and overrides for the rules the format table cannot
 * express. See "x86-64 overrides" and "x86-64 format rules" in
 * Documentation/mm/pgt.rst.
 *
 * The levels are the paging structures of the Intel SDM, from the page
 * table up: PT, PD, PDPT, PML4 and PML5. A PDE or PDPTE with PS set maps a
 * 2M or 1G page. Without 5-level paging the root is the PML4 table and
 * there is no PML5 level.
 *
 * The rules shared with the per-level helpers are in <asm/pgt-rules.h>.
 * The per-level paravirt operations, the page table check and the TLB
 * gathering helpers are the only per-level interfaces used here.
 */
#ifndef _ASM_X86_PGT_H
#define _ASM_X86_PGT_H

#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/mem_encrypt.h>
#include <linux/page_table_check.h>
#include <asm/cpufeature.h>
#include <asm/page.h>
#include <asm/pgt-rules.h>
#ifdef CONFIG_PARAVIRT_XXL
#include <asm/paravirt.h>
#endif

/**
 * enum pgt_x86_level - pgt levels by the paging structure their entries are in.
 * @PGT_X86_PT: page table.
 * @PGT_X86_PD: page directory.
 * @PGT_X86_PDPT: page-directory-pointer table.
 * @PGT_X86_PML4: PML4 table.
 * @PGT_X86_PML5: PML5 table, with 5-level paging.
 */
enum pgt_x86_level {
	PGT_X86_PT,
	PGT_X86_PD,
	PGT_X86_PDPT,
	PGT_X86_PML4,
	PGT_X86_PML5,
};

/*
 * Values known only at boot are read as constants patched into the code
 * once init_mem_mapping() has run, see pgt_x86_init(); modules cannot be
 * patched and read the variables.
 */
#ifdef MODULE
#define __pgt_x86_runtime(sym)	(sym)
#else
#include <asm/runtime-const.h>
#define __pgt_x86_runtime(sym)	runtime_const_ptr(sym)
#endif

static __always_inline pgtval_t __pgt_x86_physical_mask(void)
{
#ifdef CONFIG_DYNAMIC_PHYSICAL_MASK
	return __pgt_x86_runtime(physical_mask);
#else
	return __PHYSICAL_MASK;
#endif
}

static __always_inline pgtval_t __pgt_x86_enc(void)
{
#ifdef CONFIG_AMD_MEM_ENCRYPT
	return __pgt_x86_runtime(sme_me_mask);
#else
	return 0;
#endif
}

static __always_inline pgtval_t __pgt_x86_supported(void)
{
	return __pgt_x86_runtime(__supported_pte_mask);
}

/* The physical address field of every level, before the boot-time mask */
#define PGT_X86_PA_FIELD GENMASK_ULL(__PHYSICAL_MASK_SHIFT - 1, PAGE_SHIFT)

/* The size an entry at level @l maps, as a shift; 512 entries per table */
#define PGT_X86_SHIFT(l)	(PAGE_SHIFT + 9 * (l))

/* The PFN field of a leaf at level @l; in a PDE or PDPTE bit 12 is PAT */
#define __PGT_X86_LEAF_PFN(l)						\
	(PGT_X86_PA_FIELD & ~(BIT_ULL(PGT_X86_SHIFT(l)) - 1))

/*
 * A table entry: the flags other than user and accessed equal the kernel
 * table flags; a PML5 entry requires accessed, as pgd_bad() does. Page
 * table isolation sets NX in the kernel copy of user top-level entries,
 * PML4 or PML5. Bits of the address field above the physical address mask
 * are flags too (memory encryption), and are added at boot.
 */
#define __PGT_X86_TABLE_IGNORE	(_PAGE_USER | _PAGE_ACCESSED)
#ifdef CONFIG_MITIGATION_PAGE_TABLE_ISOLATION
#define __PGT_X86_PTI_IGNORE	_PAGE_NX
#else
#define __PGT_X86_PTI_IGNORE	0
#endif
#define __PGT_X86_TABLE_MASK (~(PGT_X86_PA_FIELD | __PGT_X86_TABLE_IGNORE))
#define __PGT_X86_PML4_MASK (__PGT_X86_TABLE_MASK & ~__PGT_X86_PTI_IGNORE)
#define __PGT_X86_PML5_MASK						\
	(~(PGT_X86_PA_FIELD | _PAGE_USER | __PGT_X86_PTI_IGNORE))
#define __PGT_X86_TABLE_VAL	(_KERNPG_TABLE_NOENC & ~_PAGE_ACCESSED)
#define __PGT_X86_PRESENT	(_PAGE_PRESENT | _PAGE_PROTNONE)

#ifdef CONFIG_ARCH_HAS_PMD_SOFTLEAVES
#define __PGT_X86_CAN_SOFTLEAF	(BIT(PGT_X86_PT) | BIT(PGT_X86_PD))
#else
#define __PGT_X86_CAN_SOFTLEAF	BIT(PGT_X86_PT)
#endif

#if defined(CONFIG_SPLIT_PMD_PTLOCKS)
#define __PGT_X86_SPLIT_LOCK	(BIT(PGT_X86_PT) | BIT(PGT_X86_PD))
#elif defined(CONFIG_SPLIT_PTE_PTLOCKS)
#define __PGT_X86_SPLIT_LOCK	BIT(PGT_X86_PT)
#else
#define __PGT_X86_SPLIT_LOCK	0
#endif

#define pgt_fmt_table_pfn_mask(l)					\
	(pgt_fmt.lvl[l].table_pfn_mask & __pgt_x86_physical_mask())
#define pgt_fmt_leaf_pfn_mask(l)					\
	(pgt_fmt.lvl[l].leaf_pfn_mask & __pgt_x86_physical_mask())
#define pgt_fmt_table_mask(l)						\
	(pgt_fmt.lvl[l].table_mask |					\
	 (PGT_X86_PA_FIELD & ~__pgt_x86_physical_mask()))
#define pgt_fmt_table_val(l)						\
	(pgt_fmt.lvl[l].table_val | __pgt_x86_enc())
#define pgt_fmt_table_flags(l)						\
	(pgt_fmt.lvl[l].table_flags | __pgt_x86_enc())

#define __PGT_X86_BITS(lvl)						\
	{								\
		[PGT_F_VALID] = _PAGE_PRESENT,				\
		[PGT_F_WRITE] = _PAGE_RW,				\
		[PGT_F_DIRTY] = _PAGE_DIRTY,				\
		[PGT_F_YOUNG] = _PAGE_ACCESSED,				\
		[PGT_F_USER] = _PAGE_USER,				\
		[PGT_F_NOEXEC] = _PAGE_NX,				\
		[PGT_F_SPECIAL] = _PAGE_SPECIAL,			\
		[PGT_F_SOFT_DIRTY] = _PAGE_SOFT_DIRTY,			\
		[PGT_F_UFFD_WP] = (lvl) <= PGT_X86_PD ? _PAGE_UFFD : 0,	\
		[PGT_F_GLOBAL] = _PAGE_GLOBAL,				\
		[PGT_F_PROTNONE] = _PAGE_PROTNONE,			\
		[PGT_F_SAVED_DIRTY] = _PAGE_SAVED_DIRTY,		\
		[PGT_F_SWP_SOFT_DIRTY] =				\
			(lvl) <= PGT_X86_PD ? _PAGE_SWP_SOFT_DIRTY : 0,	\
		[PGT_F_SWP_UFFD_WP] =					\
			(lvl) <= PGT_X86_PD ? _PAGE_SWP_UFFD : 0,	\
		[PGT_F_SWP_EXCLUSIVE] =					\
			(lvl) == PGT_X86_PT ? _PAGE_SWP_EXCLUSIVE : 0,	\
	}

static const struct pgt_fmt pgt_fmt __maybe_unused = {
	.lvl[PGT_X86_PT] = {
		.shift		= PGT_X86_SHIFT(PGT_X86_PT),
		.entry_order	= 3,
		.entries	= 512,
		.none_mask	= ~_PAGE_KNL_ERRATUM_MASK,
		.present_mask	= __PGT_X86_PRESENT,
		.leaf_pfn_mask	= __PGT_X86_LEAF_PFN(PGT_X86_PT),
		.pfn_shift	= PAGE_SHIFT,
		.bit		= __PGT_X86_BITS(PGT_X86_PT),
	},
	.lvl[PGT_X86_PD] = {
		.shift		= PGT_X86_SHIFT(PGT_X86_PD),
		.entry_order	= 3,
		.entries	= 512,
		.none_mask	= ~_PAGE_KNL_ERRATUM_MASK,
		.leaf_mask	= _PAGE_PSE,
		.leaf_val	= _PAGE_PSE,
		.present_mask	= __PGT_X86_PRESENT,
		.table_mask	= __PGT_X86_TABLE_MASK,
		.table_val	= __PGT_X86_TABLE_VAL,
		.table_flags	= _PAGE_TABLE_NOENC,
		.table_pfn_mask	= PGT_X86_PA_FIELD,
		.leaf_pfn_mask	= __PGT_X86_LEAF_PFN(PGT_X86_PD),
		.pfn_shift	= PAGE_SHIFT,
		.bit		= __PGT_X86_BITS(PGT_X86_PD),
	},
	.lvl[PGT_X86_PDPT] = {
		.shift		= PGT_X86_SHIFT(PGT_X86_PDPT),
		.entry_order	= 3,
		.entries	= 512,
		.none_mask	= ~_PAGE_KNL_ERRATUM_MASK,
		.leaf_mask	= _PAGE_PSE,
		.leaf_val	= _PAGE_PSE,
		.present_mask	= __PGT_X86_PRESENT,
		.table_mask	= __PGT_X86_TABLE_MASK,
		.table_val	= __PGT_X86_TABLE_VAL,
		.table_flags	= _PAGE_TABLE_NOENC,
		.table_pfn_mask	= PGT_X86_PA_FIELD,
		.leaf_pfn_mask	= __PGT_X86_LEAF_PFN(PGT_X86_PDPT),
		.pfn_shift	= PAGE_SHIFT,
		.bit		= __PGT_X86_BITS(PGT_X86_PDPT),
	},
	.lvl[PGT_X86_PML4] = {
		.shift		= PGT_X86_SHIFT(PGT_X86_PML4),
		.entry_order	= 3,
		.entries	= 512,
		.none_mask	= ~_PAGE_KNL_ERRATUM_MASK,
		.present_mask	= __PGT_X86_PRESENT,
		.table_mask	= __PGT_X86_PML4_MASK,
		.table_val	= __PGT_X86_TABLE_VAL,
		.table_flags	= _PAGE_TABLE_NOENC,
		.mbz		= _PAGE_PSE,
		.table_pfn_mask	= PGT_X86_PA_FIELD,
		.pfn_shift	= PAGE_SHIFT,
	},
	.lvl[PGT_X86_PML5] = {
		.shift		= PGT_X86_SHIFT(PGT_X86_PML5),
		.entry_order	= 3,
		.entries	= 512,
		.none_mask	= ~(pgtval_t)0,
		.present_mask	= __PGT_X86_PRESENT,
		.table_mask	= __PGT_X86_PML5_MASK,
		.table_val	= _KERNPG_TABLE_NOENC,
		.table_flags	= _PAGE_TABLE_NOENC,
		.mbz		= _PAGE_PSE,
		.table_pfn_mask	= PGT_X86_PA_FIELD,
		.pfn_shift	= PAGE_SHIFT,
	},
	.can_leaf	= BIT(PGT_X86_PT) | BIT(PGT_X86_PD) |
			  BIT(PGT_X86_PDPT),
	.can_softleaf	= __PGT_X86_CAN_SOFTLEAF,
	.split_lock	= __PGT_X86_SPLIT_LOCK,
	.softleaf = {
		.type_shift	= 64 - SWP_TYPE_BITS,
		.type_bits	= SWP_TYPE_BITS,
		.offset_shift	= SWP_OFFSET_FIRST_BIT,
		.offset_bits	= 64 - SWP_TYPE_BITS - SWP_OFFSET_FIRST_BIT,
		.invert_offset	= true,
	},
	.softleaf_mbz	= _PAGE_PRESENT | _PAGE_PSE | _PAGE_PROTNONE,
};

/* Pointers */

static __always_inline unsigned int __pgt_x86_root_level(void)
{
	return pgtable_l5_enabled() ? PGT_X86_PML5 : PGT_X86_PML4;
}

#define pgt_arch_root_level pgt_arch_root_level
static __always_inline unsigned int pgt_arch_root_level(struct mm_struct *mm)
{
	return __pgt_x86_root_level();
}

/*
 * Reads and stores. Without CONFIG_PARAVIRT_XXL an entry is read and
 * written as it is, at every level, and a store at the root level also
 * updates the page table isolation copy of the top-level table.
 *
 * With CONFIG_PARAVIRT_XXL an entry is translated on every read and write
 * (Xen PV keeps machine frames in the tables), and a store goes through the
 * paravirt operation of its level, which Xen implements per level.
 */
#ifdef CONFIG_PARAVIRT_XXL
static __always_inline pgtval_t __pgt_x86_from_raw(pgtval_t raw,
						   unsigned int lvl)
{
	switch (lvl) {
	case PGT_X86_PT:
		return pte_val(native_make_pte(raw));
	case PGT_X86_PD:
		return pmd_val(native_make_pmd(raw));
	case PGT_X86_PDPT:
		return pud_val(native_make_pud(raw));
	case PGT_X86_PML4:
		return p4d_val(native_make_p4d(raw));
	default:
		return pgd_val(native_make_pgd(raw));
	}
}

static __always_inline pgtval_t __pgt_x86_to_raw(pgtval_t val,
						 unsigned int lvl)
{
	switch (lvl) {
	case PGT_X86_PT:
		return native_pte_val(__pte(val));
	case PGT_X86_PD:
		return native_pmd_val(__pmd(val));
	case PGT_X86_PDPT:
		return native_pud_val(__pud(val));
	case PGT_X86_PML4:
		return native_p4d_val(__p4d(val));
	default:
		return native_pgd_val(__pgd(val));
	}
}

static __always_inline void __pgt_x86_set(void *ptr, pgtval_t val,
					  unsigned int lvl)
{
	switch (lvl) {
	case PGT_X86_PT:
		set_pte(ptr, __pte(val));
		break;
	case PGT_X86_PD:
		set_pmd(ptr, __pmd(val));
		break;
	case PGT_X86_PDPT:
		set_pud(ptr, __pud(val));
		break;
	case PGT_X86_PML4:
		set_p4d(ptr, __p4d(val));
		break;
	default:
		set_pgd(ptr, __pgd(val));
		break;
	}
}
#else
static __always_inline pgtval_t __pgt_x86_from_raw(pgtval_t raw,
						   unsigned int lvl)
{
	return raw;
}

static __always_inline pgtval_t __pgt_x86_to_raw(pgtval_t val,
						 unsigned int lvl)
{
	return val;
}

static __always_inline void __pgt_x86_set(void *ptr, pgtval_t val,
					  unsigned int lvl)
{
	if (lvl == __pgt_x86_root_level())
		val = pgt_x86_pti_set_user(ptr, val);
	WRITE_ONCE(*(pgtval_t *)ptr, val);
}
#endif

#define pgt_arch_load pgt_arch_load
static __always_inline pgtval_t pgt_arch_load(const void *ptr,
					      unsigned int lvl)
{
	pgtval_t raw = READ_ONCE(*(const pgtval_t *)ptr);

	return __pgt_x86_from_raw(raw, lvl);
}

/*
 * The page table check takes values of the level's own type until it has a
 * level-generic interface. Setting a value checks it against the entry it
 * overwrites; clearing reports the old value.
 */
static __always_inline void __pgt_x86_check_set(struct mm_struct *mm,
						unsigned long addr, void *ptr,
						pgtval_t val, unsigned int lvl,
						unsigned int nr)
{
	pgtval_t raw;

	if (!IS_ENABLED(CONFIG_PAGE_TABLE_CHECK))
		return;
	raw = __pgt_x86_to_raw(val, lvl);
	switch (lvl) {
	case PGT_X86_PT:
		page_table_check_ptes_set(mm, addr, ptr, native_make_pte(raw),
					  nr);
		break;
	case PGT_X86_PD:
		page_table_check_pmd_set(mm, addr, ptr, native_make_pmd(raw));
		break;
	default:
		page_table_check_pud_set(mm, addr, ptr, native_make_pud(raw));
		break;
	}
}

static __always_inline void __pgt_x86_check_clear(struct mm_struct *mm,
						  unsigned long addr,
						  pgtval_t old,
						  unsigned int lvl)
{
	pgtval_t raw;

	if (!IS_ENABLED(CONFIG_PAGE_TABLE_CHECK))
		return;
	raw = __pgt_x86_to_raw(old, lvl);
	switch (lvl) {
	case PGT_X86_PT:
		page_table_check_pte_clear(mm, addr, native_make_pte(raw));
		break;
	case PGT_X86_PD:
		page_table_check_pmd_clear(mm, addr, native_make_pmd(raw));
		break;
	default:
		page_table_check_pud_clear(mm, addr, native_make_pud(raw));
		break;
	}
}

/* The leaf for the next page: the PFN field is stored inverted or not */
static __always_inline pgtval_t __pgt_x86_next_page(pgtval_t val)
{
	if (pgt_x86_pfn_inverted(val))
		return val - PAGE_SIZE;
	return val + PAGE_SIZE;
}

#define pgt_arch_store_leaf pgt_arch_store_leaf
static __always_inline void pgt_arch_store_leaf(struct mm_struct *mm,
						unsigned long addr, void *ptr,
						pgtval_t val, unsigned int lvl,
						unsigned int nr)
{
	__pgt_x86_check_set(mm, addr, ptr, val, lvl, nr);
	for (;;) {
		__pgt_x86_set(ptr, val, lvl);
		if (--nr == 0)
			break;
		ptr += sizeof(pgtval_t);
		val = __pgt_x86_next_page(val);
	}
}

#define pgt_arch_store_softleaf pgt_arch_store_softleaf
static __always_inline void pgt_arch_store_softleaf(struct mm_struct *mm,
						    unsigned long addr,
						    void *ptr, pgtval_t val,
						    unsigned int lvl)
{
	__pgt_x86_check_set(mm, addr, ptr, val, lvl, 1);
	__pgt_x86_set(ptr, val, lvl);
}

#define pgt_arch_clear pgt_arch_clear
static __always_inline void pgt_arch_clear(struct mm_struct *mm,
					   unsigned long addr, void *ptr,
					   unsigned int lvl)
{
	__pgt_x86_set(ptr, 0, lvl);
}

/*
 * PML4 and PML5 entries are only ever table entries. They are stored rather
 * than exchanged, so that a root-level store updates the page table
 * isolation copy.
 */
#define pgt_arch_xchg pgt_arch_xchg
static __always_inline pgtval_t pgt_arch_xchg(struct mm_struct *mm,
					      unsigned long addr, void *ptr,
					      pgtval_t val, unsigned int lvl)
{
	pgtval_t raw, old;

	if (lvl >= PGT_X86_PML4) {
		old = pgt_arch_load(ptr, lvl);
		__pgt_x86_set(ptr, val, lvl);
		return old;
	}
	if (val)
		__pgt_x86_check_set(mm, addr, ptr, val, lvl, 1);
	raw = __pgt_x86_to_raw(val, lvl);
	old = xchg((pgtval_t *)ptr, raw);
	old = __pgt_x86_from_raw(old, lvl);
	if (!val)
		__pgt_x86_check_clear(mm, addr, old, lvl);
	return old;
}

/*
 * Tearing down a whole mm needs no atomic exchange of a PTE: nothing else
 * uses it, and the paravirt operations do not care, as with
 * ptep_get_and_clear_full().
 */
#define pgt_arch_get_and_clear pgt_arch_get_and_clear
static __always_inline pgtval_t pgt_arch_get_and_clear(struct mm_struct *mm,
						       unsigned long addr,
						       void *ptr,
						       unsigned int lvl,
						       bool full)
{
	pgtval_t old;

	if (!full || lvl != PGT_X86_PT)
		return pgt_arch_xchg(mm, addr, ptr, 0, lvl);
	old = READ_ONCE(*(pgtval_t *)ptr);
	WRITE_ONCE(*(pgtval_t *)ptr, 0);
	old = __pgt_x86_from_raw(old, lvl);
	__pgt_x86_check_clear(mm, addr, old, lvl);
	return old;
}

/* Out of line: it builds the entry with the default pgt_arch_table_entry() */
#define pgt_arch_populate pgt_arch_populate
void pgt_arch_populate(struct mm_struct *mm, void *ptr, struct ptdesc *table,
		       unsigned int lvl);

/* Leaves */

/* L1TF: a non-present, non-zero entry stores its PFN inverted */
#define pgt_arch_pfn_inverted pgt_arch_pfn_inverted
static __always_inline bool pgt_arch_pfn_inverted(pgtval_t val,
						  unsigned int lvl)
{
	return pgt_x86_pfn_inverted(val);
}

/* The protection of a present leaf is filtered as check_pgprot() does */
static __always_inline pgtval_t __pgt_x86_massage(pgtval_t protval)
{
	pgtval_t massaged = protval;

	if (massaged & _PAGE_PRESENT)
		massaged &= __pgt_x86_supported();
	VM_WARN_ON_ONCE(massaged != protval);
	return massaged;
}

/* The canonical PAT bit is bit 7, as in a PTE; a PDE or PDPTE has it at 12 */
#define pgt_arch_mk pgt_arch_mk
static __always_inline pgtv_t pgt_arch_mk(unsigned long pfn, pgprot_t prot,
					  unsigned int lvl)
{
	pgtval_t protval = __pgt_x86_massage(pgprot_val(prot));
	pgtval_t field = (pgtval_t)pfn << PAGE_SHIFT;

	field ^= pgt_x86_invert_mask(pgprot_val(prot));
	field &= pgt_fmt_leaf_pfn_mask(lvl);
	if (lvl != PGT_X86_PT)
		protval = protval_4k_2_large(protval) | _PAGE_PSE;
	return __pgtv(field | protval, lvl);
}

#define pgt_arch_pgprot pgt_arch_pgprot
static __always_inline pgprot_t pgt_arch_pgprot(pgtv_t v)
{
	pgtval_t protval = v.val & ~pgt_fmt_leaf_pfn_mask(v.lvl);

	if (v.lvl != PGT_X86_PT)
		protval = protval_large_2_4k(protval & ~_PAGE_PSE);
	return __pgprot(protval);
}

/*
 * As pte_modify(), pmd_modify() and pud_modify(): the memory type is kept.
 * The whole address field is kept, which covers the memory encryption bits
 * _PAGE_CC stands for in the legacy masks.
 */
#define __PGT_X86_CHG_MASK						\
	(PGT_X86_PA_FIELD | _PAGE_PCD | _PAGE_PWT | _PAGE_SPECIAL |	\
	 _PAGE_ACCESSED | _PAGE_DIRTY_BITS | _PAGE_SOFT_DIRTY | _PAGE_UFFD)

#define pgt_arch_modify pgt_arch_modify
static __always_inline pgtv_t pgt_arch_modify(pgtv_t v, pgprot_t prot)
{
	pgtval_t protval = __pgt_x86_massage(pgprot_val(prot));
	pgtval_t chg = __PGT_X86_CHG_MASK | _PAGE_PAT;
	pgtval_t old = v.val;
	pgtval_t mask = pgt_fmt_leaf_pfn_mask(v.lvl);

	if (v.lvl != PGT_X86_PT) {
		chg = __PGT_X86_CHG_MASK | _PAGE_PSE | _PAGE_PAT_LARGE;
		protval = protval_4k_2_large(protval);
	}
	v.val = (old & chg) | (protval & ~chg);
	v.val = pgt_x86_reinvert(old, v.val, mask);
	if (old & _PAGE_RW)
		v.val = pgt_x86_mksaveddirty(v.val);
	else
		v.val = pgt_x86_clear_saveddirty(v.val);
	return v;
}

/*
 * Shadow stack: a PTE or PDE with Write=0 and Dirty=1 is a shadow stack
 * entry. Clearing the write bit or setting the dirty bit moves
 * the dirty state to _PAGE_SAVED_DIRTY so that none is made by accident;
 * the rule is in <asm/pgt-rules.h>.
 */
#define pgt_arch_write pgt_arch_write
static __always_inline bool pgt_arch_write(pgtv_t v)
{
	if (v.val & _PAGE_RW)
		return true;
	if (v.lvl > PGT_X86_PD || !cpu_feature_enabled(X86_FEATURE_SHSTK))
		return false;
	return (v.val & (_PAGE_RW | _PAGE_DIRTY)) == _PAGE_DIRTY;
}

/* Out of line: the shadow stack tests need the VMA flags of <linux/mm.h> */
pgtval_t pgt_x86_mkwrite_vma(pgtval_t val, struct vm_area_struct *vma);
void pgt_x86_check_zapped(struct vm_area_struct *vma, pgtv_t v);

/* As arch_check_zapped_pte(): a debug check only */
#define pgt_arch_check_zapped pgt_arch_check_zapped
static __always_inline void pgt_arch_check_zapped(struct vm_area_struct *vma,
						  pgtv_t v)
{
	if (IS_ENABLED(CONFIG_DEBUG_VM))
		pgt_x86_check_zapped(vma, v);
}

#define pgt_arch_mkwrite pgt_arch_mkwrite
static __always_inline pgtv_t pgt_arch_mkwrite(pgtv_t v,
					       struct vm_area_struct *vma)
{
	if (v.lvl == PGT_X86_PDPT)
		v.val = pgt_x86_clear_saveddirty(v.val | _PAGE_RW);
	else if (vma)
		v.val = pgt_x86_mkwrite_vma(v.val, vma);
	else
		v.val |= _PAGE_RW;
	return v;
}

#define pgt_arch_mkdirty pgt_arch_mkdirty
static __always_inline pgtv_t pgt_arch_mkdirty(pgtv_t v)
{
	v.val = pgt_x86_mksaveddirty(v.val | _PAGE_DIRTY | _PAGE_SOFT_DIRTY);
	return v;
}

#define pgt_arch_wrprotect pgt_arch_wrprotect
static __always_inline pgtv_t pgt_arch_wrprotect(pgtv_t v)
{
	v.val = pgt_x86_mksaveddirty(v.val & ~_PAGE_RW);
	return v;
}

/* Present, user and, for a write, writable; protection keys permitting */
#define pgt_arch_accessible pgt_arch_accessible
static __always_inline bool pgt_arch_accessible(pgtv_t v, bool write)
{
	return pgt_x86_access_permitted(v.val, write);
}

/*
 * TLB; macros, since they expand to the <asm/tlb.h> helpers. The gathering
 * helpers are per level until mmu_gather has a level-generic interface.
 */

#define pgt_arch_tlb_remove_entry(tlb, p, addr)				\
do {									\
	struct mmu_gather *__tlb = (tlb);				\
	unsigned long __addr = (addr);					\
	pgtp_t __p = (p);						\
									\
	switch (pgtp_level(__p)) {					\
	case PGT_X86_PT:						\
		tlb_remove_tlb_entry(__tlb, (pte_t *)pgtp_ptr(__p),	\
				     __addr);				\
		break;							\
	case PGT_X86_PD:						\
		tlb_remove_pmd_tlb_entry(__tlb, (pmd_t *)pgtp_ptr(__p),	\
					 __addr);			\
		break;							\
	default:							\
		tlb_remove_pud_tlb_entry(__tlb, (pud_t *)pgtp_ptr(__p),	\
					 __addr);			\
		break;							\
	}								\
} while (0)

#define pgt_arch_tlb_remove_entries(tlb, p, nr, addr)			\
do {									\
	struct mmu_gather *__tlbe = (tlb);				\
	unsigned long __addre = (addr);					\
	pgtp_t __pe = (p);						\
									\
	if (pgtp_level(__pe) == PGT_X86_PT)				\
		tlb_remove_tlb_entries(__tlbe, (pte_t *)pgtp_ptr(__pe),	\
				       nr, __addre);			\
	else								\
		pgt_arch_tlb_remove_entry(__tlbe, __pe, __addre);	\
} while (0)

#define pgt_arch_tlb_remove_table(tlb, v, addr)				\
do {									\
	struct mmu_gather *__tlb = (tlb);				\
	unsigned long __addr = (addr);					\
	pgtv_t __v = (v);						\
	void *__t = pgtp_ptr(pgt_arch_child(__v, 0));			\
									\
	switch (__v.lvl) {						\
	case PGT_X86_PD:						\
		pte_free_tlb(__tlb, virt_to_page(__t), __addr);		\
		mm_dec_nr_ptes(__tlb->mm);				\
		break;							\
	case PGT_X86_PDPT:						\
		pmd_free_tlb(__tlb, (pmd_t *)__t, __addr);		\
		mm_dec_nr_pmds(__tlb->mm);				\
		break;							\
	case PGT_X86_PML4:						\
		pud_free_tlb(__tlb, (pud_t *)__t, __addr);		\
		mm_dec_nr_puds(__tlb->mm);				\
		break;							\
	default:							\
		p4d_free_tlb(__tlb, (p4d_t *)__t, __addr);		\
		break;							\
	}								\
} while (0)

#endif /* _ASM_X86_PGT_H */
