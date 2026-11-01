/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Level-generic page table API: the architecture contract.
 *
 * An implementation of the contract ("provider") is either native,
 * <asm/pgt.h>, or the fallback, <asm-generic/pgt-fallback.h>, which every
 * architecture gets. CONFIG_PGT_FALLBACK selects the fallback.
 *
 * The contract is the set of pgt_arch_*() entry points below. A native
 * provider describes its entry format in a struct pgt_fmt table; the
 * default of every entry point is then derived from the table, in
 * <asm-generic/pgt-fmt.h>, and the provider overrides only the hooks the
 * table cannot express. The fallback has no table and overrides every
 * entry point; it is described in Documentation/mm/pgt-legacy.rst.
 *
 * Overriding follows the usual pattern: the provider defines the function
 * and "#define pgt_arch_foo pgt_arch_foo" before the default here is seen.
 * Inline entry points are __always_inline: callers know the level as a
 * constant, and it only folds away once the entry point is inlined.
 *
 * Only this header and the providers read pgtv_t.val.
 *
 * A provider makes pgprot_t and softleaf_t visible: the consumer API uses
 * them after including it, and <linux/pgt.h> does not include anything that
 * defines them.
 */
#ifndef _ASM_GENERIC_PGT_CONTRACT_H
#define _ASM_GENERIC_PGT_CONTRACT_H

#ifndef _LINUX_PGT_H
#error "include <linux/pgt.h> instead"
#endif

/**
 * enum pgt_flag - Software-visible flags of an entry.
 * @PGT_F_VALID: the hardware may use the entry.
 * @PGT_F_WRITE: writable.
 * @PGT_F_DIRTY: hardware dirty.
 * @PGT_F_YOUNG: accessed.
 * @PGT_F_USER: user-accessible.
 * @PGT_F_NOEXEC: not executable.
 * @PGT_F_SPECIAL: no refcounted struct page behind the mapping.
 * @PGT_F_SOFT_DIRTY: soft-dirty, in a LEAF.
 * @PGT_F_UFFD_WP: userfaultfd write-protected, in a LEAF.
 * @PGT_F_CONT: part of a contiguous-hint run.
 * @PGT_F_GLOBAL: not flushed on address space switch.
 * @PGT_F_PROTNONE: PROT_NONE leaf.
 * @PGT_F_SAVED_DIRTY: dirty state saved while the hardware bit is clear.
 * @PGT_F_SWP_SOFT_DIRTY: soft-dirty, in a SOFTLEAF.
 * @PGT_F_SWP_UFFD_WP: userfaultfd write-protected, in a SOFTLEAF.
 * @PGT_F_SWP_EXCLUSIVE: exclusive anonymous page, in a SOFTLEAF.
 * @PGT_NR_FLAGS: number of flags.
 */
enum pgt_flag {
	PGT_F_VALID,
	PGT_F_WRITE,
	PGT_F_DIRTY,
	PGT_F_YOUNG,
	PGT_F_USER,
	PGT_F_NOEXEC,
	PGT_F_SPECIAL,
	PGT_F_SOFT_DIRTY,
	PGT_F_UFFD_WP,
	PGT_F_CONT,
	PGT_F_GLOBAL,
	PGT_F_PROTNONE,
	PGT_F_SAVED_DIRTY,
	PGT_F_SWP_SOFT_DIRTY,
	PGT_F_SWP_UFFD_WP,
	PGT_F_SWP_EXCLUSIVE,
	PGT_NR_FLAGS,
};

/**
 * struct pgt_level_fmt - Entry format of one level.
 * @shift: log2 of the size an entry maps.
 * @entry_order: log2 of the size of an entry.
 * @entries: entries per table.
 * @none_mask: with @none_val, the NONE test.
 * @none_val: see @none_mask.
 * @leaf_mask: with @leaf_val, the LEAF test above level 0; zero if the
 *             level cannot hold a leaf.
 * @leaf_val: see @leaf_mask.
 * @present_mask: Linux-present bits; a value with none of them is not a
 *                LEAF or TABLE.
 * @table_mask: with @table_val, the pattern of a valid TABLE entry.
 * @table_val: see @table_mask.
 * @table_flags: bits a TABLE entry carries next to the table address.
 * @mbz: bits that make a non-leaf entry BAD.
 * @table_pfn_mask: PFN field of a TABLE entry; zero if the level holds no
 *                  TABLE.
 * @leaf_pfn_mask: PFN field of a LEAF; zero if the level cannot hold a
 *                 LEAF.
 * @pfn_shift: shift of the PFN fields.
 * @pfn_hi_mask: high part of a split PFN field, zero if unused.
 * @pfn_hi_shift: shift of the high part.
 * @bit: position of each enum pgt_flag, zero if the level lacks it.
 * @cont_entries: length of a contiguous-hint run, zero if none.
 *
 * Generic classification is described in Documentation/mm/pgt.rst.
 */
struct pgt_level_fmt {
	u8 shift;
	u8 entry_order;
	u16 entries;
	pgtval_t none_mask, none_val;
	pgtval_t leaf_mask, leaf_val;
	pgtval_t present_mask;
	pgtval_t table_mask, table_val;
	pgtval_t table_flags;
	pgtval_t mbz;
	pgtval_t table_pfn_mask;
	pgtval_t leaf_pfn_mask;
	u8 pfn_shift;
	pgtval_t pfn_hi_mask;
	u8 pfn_hi_shift;
	pgtval_t bit[PGT_NR_FLAGS];
	u16 cont_entries;
};

/**
 * struct pgt_fmt - Entry format of all levels.
 * @lvl: per-level formats.
 * @can_leaf: levels that can hold a LEAF, by bit.
 * @can_softleaf: levels that can hold a SOFTLEAF, by bit.
 * @split_lock: levels whose entries are protected by a lock in the table's
 *              ptdesc, by bit; mm->page_table_lock otherwise.
 * @softleaf: layout of a software entry.
 * @softleaf_mbz: bits that must be clear in a software entry.
 *
 * A provider defines the table as "static const struct pgt_fmt pgt_fmt",
 * so that reads at a constant level fold to constants. A field that is
 * known only at boot is left out of the table and supplied by overriding
 * its accessor in <asm-generic/pgt-fmt.h>: pgt_fmt_table_pfn_mask(),
 * pgt_fmt_leaf_pfn_mask(), pgt_fmt_table_mask(), pgt_fmt_table_val() or
 * pgt_fmt_table_flags().
 */
struct pgt_fmt {
	struct pgt_level_fmt lvl[PGT_MAX_LEVEL + 1];
	unsigned long can_leaf, can_softleaf, split_lock;
	struct {
		u8 type_shift, type_bits, offset_shift, offset_bits;
		bool invert_offset;
	} softleaf;
	pgtval_t softleaf_mbz;
};

#define __pgtv(x, l)	((pgtv_t){ .val = (x), .lvl = (l) })

#ifdef CONFIG_PGT_FALLBACK
#include <asm-generic/pgt-fallback.h>
#else
#include <asm/pgt.h>
#include <asm-generic/pgt-fmt.h>
#endif

/*
 * Hooks: behaviour the format table cannot express. Each has a generic
 * default.
 */

#ifndef pgt_arch_load
/**
 * pgt_arch_load() - Read an entry once.
 * @ptr: entry address.
 * @lvl: level of the entry.
 *
 * The caller holds the entry's lock or knows it cannot change.
 *
 * Default: READ_ONCE() of the entry width.
 *
 * Return: the raw entry.
 */
static __always_inline pgtval_t pgt_arch_load(const void *ptr,
					      unsigned int lvl);
#endif

#ifndef pgt_arch_load_lockless
/**
 * pgt_arch_load_lockless() - Read an entry that may change concurrently.
 * @ptr: entry address.
 * @lvl: level of the entry.
 *
 * Default: pgt_arch_load(). Overridden where a plain read can be torn
 * (64-bit entries read in two halves) or has to gather a contiguous block
 * consistently.
 *
 * Return: the raw entry.
 */
static __always_inline pgtval_t pgt_arch_load_lockless(const void *ptr,
						       unsigned int lvl);
#endif

#ifndef pgt_arch_store_leaf
/**
 * pgt_arch_store_leaf() - Store leaves.
 * @mm: the mm.
 * @addr: address the first entry maps.
 * @ptr: address of the first entry.
 * @val: raw LEAF value of the first entry.
 * @lvl: level of the entries.
 * @nr: number of entries; each next one maps the following memory.
 *
 * Default: plain stores.
 */
static __always_inline void pgt_arch_store_leaf(struct mm_struct *mm,
						unsigned long addr, void *ptr,
						pgtval_t val, unsigned int lvl,
						unsigned int nr);
#endif

#ifndef pgt_arch_store_softleaf
/**
 * pgt_arch_store_softleaf() - Store a software entry.
 * @mm: the mm.
 * @addr: address the entry maps.
 * @ptr: entry address.
 * @val: raw SOFTLEAF value.
 * @lvl: level of the entry.
 *
 * Default: plain store.
 */
static __always_inline void pgt_arch_store_softleaf(struct mm_struct *mm,
						    unsigned long addr,
						    void *ptr, pgtval_t val,
						    unsigned int lvl);
#endif

#ifndef pgt_arch_xchg
/**
 * pgt_arch_xchg() - Exchange an entry.
 * @mm: the mm.
 * @addr: address the entry maps.
 * @ptr: entry address.
 * @val: new raw value.
 * @lvl: level of the entry.
 *
 * Default: xchg().
 *
 * Return: the old raw value.
 */
static __always_inline pgtval_t pgt_arch_xchg(struct mm_struct *mm,
					      unsigned long addr, void *ptr,
					      pgtval_t val, unsigned int lvl);
#endif

#ifndef pgt_arch_invalidate
/**
 * pgt_arch_invalidate() - Make a leaf unusable by the hardware.
 * @vma: the VMA.
 * @addr: address the entry maps.
 * @ptr: address of a LEAF entry above level 0.
 * @lvl: level of the entry.
 *
 * Default: exchange with the value with PGT_F_VALID cleared, then flush
 * the TLB for the range.
 *
 * Return: the old raw value.
 */
pgtval_t pgt_arch_invalidate(struct vm_area_struct *vma, unsigned long addr,
			     void *ptr, unsigned int lvl);
#endif

#ifndef pgt_arch_populate
/**
 * pgt_arch_populate() - Link a new table below an entry.
 * @mm: the mm.
 * @ptr: entry address.
 * @table: the table to link.
 * @lvl: level of the entry.
 *
 * Default: store pgt_arch_table_entry(@table, @lvl).
 */
static __always_inline void pgt_arch_populate(struct mm_struct *mm, void *ptr,
					      struct ptdesc *table,
					      unsigned int lvl);
#endif

#ifndef pgt_arch_root_table
/**
 * pgt_arch_root_table() - Root table of an mm.
 * @mm: the mm.
 * @addr: user address, for architectures with more than one root.
 *
 * Default: mm->pgd.
 *
 * Return: the root table.
 */
static __always_inline void *pgt_arch_root_table(struct mm_struct *mm,
						 unsigned long addr);
#endif

#ifndef pgt_arch_root_level
/**
 * pgt_arch_root_level() - Level of the root table of an mm.
 * @mm: the mm.
 *
 * Default: PGT_MAX_LEVEL.
 *
 * Return: the root level.
 */
static __always_inline unsigned int pgt_arch_root_level(struct mm_struct *mm);
#endif

#ifndef pgt_arch_mkwrite
/**
 * pgt_arch_mkwrite() - Make a leaf writable.
 * @v: LEAF value.
 * @vma: the VMA, or NULL for pgt_leaf_mkwrite_novma().
 *
 * Default: set PGT_F_WRITE and move PGT_F_SAVED_DIRTY to PGT_F_DIRTY.
 *
 * Return: the modified value.
 */
static __always_inline pgtv_t pgt_arch_mkwrite(pgtv_t v,
					       struct vm_area_struct *vma);
#endif

#ifndef pgt_arch_mkdirty
/**
 * pgt_arch_mkdirty() - Mark a leaf dirty.
 * @v: LEAF value.
 *
 * Default: set PGT_F_DIRTY and PGT_F_SOFT_DIRTY.
 *
 * Return: the modified value.
 */
static __always_inline pgtv_t pgt_arch_mkdirty(pgtv_t v);
#endif

#ifndef pgt_arch_wrprotect
/**
 * pgt_arch_wrprotect() - Make a leaf read-only.
 * @v: LEAF value.
 *
 * Default: clear PGT_F_WRITE.
 *
 * Return: the modified value.
 */
static __always_inline pgtv_t pgt_arch_wrprotect(pgtv_t v);
#endif

#ifndef pgt_arch_write
/**
 * pgt_arch_write() - Check whether a leaf is writable.
 * @v: LEAF value.
 *
 * Default: PGT_F_WRITE.
 *
 * Return: true if writable.
 */
static __always_inline bool pgt_arch_write(pgtv_t v);
#endif

#ifndef pgt_arch_accessible
/**
 * pgt_arch_accessible() - Check whether the hardware allows a user access.
 * @v: LEAF value.
 * @write: check for write access.
 *
 * Default: PGT_F_VALID, PGT_F_USER where the level has it, and
 * pgt_arch_write() for a write.
 *
 * Return: true if allowed.
 */
static __always_inline bool pgt_arch_accessible(pgtv_t v, bool write);
#endif

#ifndef pgt_arch_pfn_inverted
/**
 * pgt_arch_pfn_inverted() - Check whether an entry stores its PFN inverted.
 * @val: raw entry.
 * @lvl: level of the entry.
 *
 * Default: false.
 *
 * Return: true if the PFN field of @val is stored inverted.
 */
static __always_inline bool pgt_arch_pfn_inverted(pgtval_t val,
						  unsigned int lvl);
#endif

#ifndef pgt_arch_table_alloc
/**
 * pgt_arch_table_alloc() - Allocate a table.
 * @mm: the mm.
 * @lvl: level of the entries of the new table.
 *
 * Default: pagetable_alloc() and pgt_arch_table_ctor(); entries filled with
 * the level's none_val.
 *
 * Return: the table, or NULL.
 */
struct ptdesc *pgt_arch_table_alloc(struct mm_struct *mm, unsigned int lvl);
#endif

#ifndef pgt_arch_table_free
/**
 * pgt_arch_table_free() - Free a table that was never linked.
 * @table: the table.
 * @lvl: level of its entries.
 *
 * Default: pgt_arch_table_dtor() and pagetable_free().
 */
void pgt_arch_table_free(struct ptdesc *table, unsigned int lvl);
#endif

#ifndef pgt_arch_table_ctor
/**
 * pgt_arch_table_ctor() - Initialise the ptdesc of a new table.
 * @mm: the mm.
 * @table: the table.
 * @lvl: level of its entries.
 *
 * Default: pagetable_pte_ctor(), pagetable_pmd_ctor() or the plain
 * constructor of the level.
 *
 * Return: false if the split lock could not be allocated.
 */
bool pgt_arch_table_ctor(struct mm_struct *mm, struct ptdesc *table,
			 unsigned int lvl);
#endif

#ifndef pgt_arch_table_dtor
/**
 * pgt_arch_table_dtor() - Tear down the ptdesc of a table.
 * @table: the table.
 * @lvl: level of its entries.
 *
 * Default: pagetable_dtor().
 */
void pgt_arch_table_dtor(struct ptdesc *table, unsigned int lvl);
#endif

#ifndef pgt_arch_deposit
/**
 * pgt_arch_deposit() - Deposit a table for a later split of a leaf.
 * @mm: the mm.
 * @p: the LEAF entry the table is deposited for.
 * @table: the table.
 *
 * Default: pgtable_trans_huge_deposit() at level 1.
 */
static __always_inline void pgt_arch_deposit(struct mm_struct *mm, pgtp_t p,
					     struct ptdesc *table);
#endif

#ifndef pgt_arch_withdraw
/**
 * pgt_arch_withdraw() - Take back a table deposited for a leaf.
 * @mm: the mm.
 * @p: the LEAF entry.
 *
 * Default: pgtable_trans_huge_withdraw() at level 1.
 *
 * Return: the table.
 */
static __always_inline struct ptdesc *pgt_arch_withdraw(struct mm_struct *mm,
							pgtp_t p);
#endif

/*
 * Derived operations: defaults computed from the format table. The
 * fallback overrides all of them.
 */

#ifndef pgt_arch_root
/**
 * pgt_arch_root() - Root entry for an address.
 * @mm: the mm.
 * @addr: user address.
 *
 * Default: pgt_arch_root_table() indexed at pgt_arch_root_level().
 *
 * Return: pointer to the root entry.
 */
static __always_inline pgtp_t pgt_arch_root(struct mm_struct *mm,
					    unsigned long addr);
#endif

#ifndef pgt_arch_entry_order
/**
 * pgt_arch_entry_order() - Size of an entry of a level, as a shift.
 * @lvl: level.
 *
 * Return: log2 of the entry size in bytes.
 */
static __always_inline unsigned int pgt_arch_entry_order(unsigned int lvl);
#endif

#ifndef pgt_arch_links
/**
 * pgt_arch_links() - Check that a TABLE value links the table of an entry.
 * @v: TABLE snapshot of an entry.
 * @p: entry pointer one level below @v.
 *
 * Return: true if @v links the table that contains @p.
 */
static __always_inline bool pgt_arch_links(pgtv_t v, pgtp_t p);
#endif

#ifndef pgt_arch_clear
/**
 * pgt_arch_clear() - Empty an entry.
 * @mm: the mm.
 * @addr: address the entry maps.
 * @ptr: entry address.
 * @lvl: level of the entry.
 *
 * Default: store the level's none_val.
 */
static __always_inline void pgt_arch_clear(struct mm_struct *mm,
					   unsigned long addr, void *ptr,
					   unsigned int lvl);
#endif

#ifndef pgt_arch_clear_bad
/**
 * pgt_arch_clear_bad() - Report and empty a BAD entry.
 * @p: entry pointer at level 1 or above.
 */
void pgt_arch_clear_bad(pgtp_t p);
#endif

#ifndef pgt_arch_alloc_child
/**
 * pgt_arch_alloc_child() - Link a new table below an entry if it is empty.
 * @mm: the mm.
 * @p: entry pointer at level 1 or above.
 *
 * Default: pgt_arch_table_alloc(), then under the lock of @p
 * pgt_arch_populate() if *@p is NONE, pgt_arch_table_free() otherwise.
 *
 * Return: 0, or -ENOMEM.
 */
int pgt_arch_alloc_child(struct mm_struct *mm, pgtp_t p);
#endif

#ifndef pgt_arch_kind
/**
 * pgt_arch_kind() - Classify a value.
 * @v: value.
 *
 * Return: the kind of @v.
 */
static __always_inline enum pgt_kind pgt_arch_kind(pgtv_t v);
#endif

#ifndef pgt_arch_can_leaf
/**
 * pgt_arch_can_leaf() - Check whether a level can hold a LEAF.
 * @lvl: level.
 *
 * Return: true if it can.
 */
static __always_inline bool pgt_arch_can_leaf(unsigned int lvl);
#endif

#ifndef pgt_arch_can_softleaf
/**
 * pgt_arch_can_softleaf() - Check whether a level can hold a SOFTLEAF.
 * @lvl: level.
 *
 * Return: true if it can.
 */
static __always_inline bool pgt_arch_can_softleaf(unsigned int lvl);
#endif

#ifndef pgt_arch_shift
/**
 * pgt_arch_shift() - Size an entry of a level maps, as a shift.
 * @lvl: level.
 *
 * Return: log2 of the size.
 */
static __always_inline unsigned int pgt_arch_shift(unsigned int lvl);
#endif

#ifndef pgt_arch_pfn
/**
 * pgt_arch_pfn() - PFN of a leaf.
 * @v: LEAF value.
 *
 * Return: the PFN.
 */
static __always_inline unsigned long pgt_arch_pfn(pgtv_t v);
#endif

#ifndef pgt_arch_mk
/**
 * pgt_arch_mk() - Build a leaf.
 * @pfn: first page frame.
 * @prot: protection in the canonical layout.
 * @lvl: level.
 *
 * Return: the LEAF value.
 */
static __always_inline pgtv_t pgt_arch_mk(unsigned long pfn, pgprot_t prot,
					  unsigned int lvl);
#endif

#ifndef pgt_arch_modify
/**
 * pgt_arch_modify() - Change the protection of a leaf.
 * @v: LEAF value.
 * @prot: protection in the canonical layout.
 *
 * Return: the modified value.
 */
static __always_inline pgtv_t pgt_arch_modify(pgtv_t v, pgprot_t prot);
#endif

#ifndef pgt_arch_pgprot
/**
 * pgt_arch_pgprot() - Protection of a leaf.
 * @v: LEAF value.
 *
 * Return: the protection in the canonical layout.
 */
static __always_inline pgprot_t pgt_arch_pgprot(pgtv_t v);
#endif

#ifndef pgt_arch_test_flag
/**
 * pgt_arch_test_flag() - Test a flag.
 * @v: LEAF or SOFTLEAF value.
 * @f: flag.
 *
 * Return: true if @f is set; false if the level lacks it.
 */
static __always_inline bool pgt_arch_test_flag(pgtv_t v, enum pgt_flag f);
#endif

#ifndef pgt_arch_set_flag
/**
 * pgt_arch_set_flag() - Set a flag.
 * @v: LEAF or SOFTLEAF value.
 * @f: flag.
 *
 * Return: the modified value; @v if the level lacks @f.
 */
static __always_inline pgtv_t pgt_arch_set_flag(pgtv_t v, enum pgt_flag f);
#endif

#ifndef pgt_arch_clear_flag
/**
 * pgt_arch_clear_flag() - Clear a flag.
 * @v: LEAF or SOFTLEAF value.
 * @f: flag.
 *
 * Return: the modified value.
 */
static __always_inline pgtv_t pgt_arch_clear_flag(pgtv_t v, enum pgt_flag f);
#endif

#ifndef pgt_arch_next
/**
 * pgt_arch_next() - Advance a leaf to the memory that follows it.
 * @v: LEAF value.
 *
 * Return: the advanced value.
 */
static __always_inline pgtv_t pgt_arch_next(pgtv_t v);
#endif

#ifndef pgt_arch_same
/**
 * pgt_arch_same() - Compare two values at the same level.
 * @a: value.
 * @b: value.
 *
 * Default: bit for bit. The fallback uses pte_same() and kin, which ignore
 * the hash bits on powerpc.
 *
 * Return: true if identical.
 */
static __always_inline bool pgt_arch_same(pgtv_t a, pgtv_t b);
#endif

#ifndef pgt_arch_table_entry
/**
 * pgt_arch_table_entry() - Build a TABLE value.
 * @table: the table to link.
 * @lvl: level of the entry.
 *
 * Return: the TABLE value.
 */
static __always_inline pgtv_t pgt_arch_table_entry(struct ptdesc *table,
						   unsigned int lvl);
#endif

#ifndef pgt_arch_none_mk
/**
 * pgt_arch_none_mk() - Build a NONE value.
 * @lvl: level.
 *
 * Return: the NONE value.
 */
static __always_inline pgtv_t pgt_arch_none_mk(unsigned int lvl);
#endif

#ifndef pgt_arch_to_softleaf
/**
 * pgt_arch_to_softleaf() - Decode a software entry.
 * @v: value.
 *
 * Return: the software entry of a SOFTLEAF value, the empty one
 * (softleaf_mk_none()) for any other kind.
 */
softleaf_t pgt_arch_to_softleaf(pgtv_t v);
#endif

#ifndef pgt_arch_from_softleaf
/**
 * pgt_arch_from_softleaf() - Encode a software entry.
 * @e: software entry.
 * @lvl: level.
 *
 * Return: the SOFTLEAF value.
 */
pgtv_t pgt_arch_from_softleaf(softleaf_t e, unsigned int lvl);
#endif

#ifndef pgt_arch_child
/**
 * pgt_arch_child() - Entry for an address one level down.
 * @v: TABLE snapshot of an entry.
 * @addr: address.
 *
 * Return: pointer to the child entry.
 */
static __always_inline pgtp_t pgt_arch_child(pgtv_t v, unsigned long addr);
#endif

#ifndef pgt_arch_addr_end
/**
 * pgt_arch_addr_end() - End of an entry's range, clipped.
 * @p: entry pointer.
 * @addr: address inside the range of @p; page aligned at level 0.
 * @end: end of the walk; page aligned at level 0.
 *
 * A level-0 entry never passes @end, and its end is @addr + PAGE_SIZE.
 *
 * Return: the clipped end.
 */
static __always_inline unsigned long pgt_arch_addr_end(pgtp_t p,
						       unsigned long addr,
						       unsigned long end);
#endif

#ifndef pgt_arch_lockptr
/**
 * pgt_arch_lockptr() - Lock that protects an entry.
 * @mm: the mm.
 * @p: entry pointer.
 *
 * Return: the lock.
 */
spinlock_t *pgt_arch_lockptr(struct mm_struct *mm, pgtp_t p);
#endif

#ifndef pgt_arch_leaf_batch
/**
 * pgt_arch_leaf_batch() - Count leaves that map consecutive pages of a folio.
 * @folio: the large folio @v maps.
 * @p: pointer to the first entry, at level 0.
 * @v: LEAF snapshot of *@p.
 * @max_nr: maximum number of entries to look at.
 *
 * Default: compare each following entry with @v advanced by pgt_arch_next(),
 * ignoring the accessed, writable, dirty and soft-dirty state.
 *
 * Return: the number of entries in the batch, at least one.
 */
unsigned int pgt_arch_leaf_batch(struct folio *folio, pgtp_t p, pgtv_t v,
				 unsigned int max_nr);
#endif

/*
 * The TLB entry points are macros, since they expand to the <asm/tlb.h>
 * helpers, and have no generic default:
 *
 * pgt_arch_tlb_remove_entry(tlb, p, addr) records the leaf cleared at @p
 * for the TLB flush.
 *
 * pgt_arch_tlb_remove_table(tlb, v, addr) frees, after the TLB flush, the
 * table that the TABLE value @v linked, and drops the mm's accounting for
 * it.
 */

#endif /* _ASM_GENERIC_PGT_CONTRACT_H */
