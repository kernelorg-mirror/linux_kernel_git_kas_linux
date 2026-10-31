/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Level-generic page table API: the consumer API.
 *
 * One entry pointer type, pgtp_t, and one entry value type, pgtv_t, carry
 * the page table level as data. Level 0 is the leaf table; the root of an
 * mm is at pgt_root_level(). Every value is one of five kinds (enum
 * pgt_kind); generic code acts on kinds and never looks inside a value.
 * A value known to be a LEAF, SOFTLEAF or TABLE has a type of its own,
 * pgt_leaf_t, pgt_softleaf_t or pgt_table_t, and only the operations of
 * that kind take it.
 *
 * Everything here is built on the architecture contract in
 * <asm-generic/pgt-contract.h>. Documentation/mm/pgt.rst describes the
 * semantics, the lock protocol and the lifetime rules.
 */
#ifndef _LINUX_PGT_H
#define _LINUX_PGT_H

#include <linux/build_bug.h>
#include <linux/mmdebug.h>
#include <linux/pgt_types.h>
#include <linux/spinlock_types.h>

struct folio;
struct mm_struct;
struct mmu_gather;
struct ptdesc;
struct vm_area_struct;

/* Pointers */

/**
 * pgtp_make() - Build an entry pointer.
 * @ptr: address of the entry.
 * @lvl: level of the table @ptr points into.
 *
 * Return: the entry pointer.
 */
static __always_inline pgtp_t pgtp_make(void *ptr, unsigned int lvl)
{
	return (pgtp_t){ .ptr = ptr, .lvl = lvl };
}

/**
 * pgtp_level() - Level of the table an entry pointer points into.
 * @p: entry pointer.
 *
 * Return: the level.
 */
static __always_inline unsigned int pgtp_level(pgtp_t p)
{
	return p.lvl;
}

/**
 * pgtp_assume_level() - Tell the compiler the level of an entry pointer.
 * @p: entry pointer.
 * @lvl: the level of @p, normally a constant.
 *
 * Every operation reads the level from @p. Code that knows it as a
 * constant states it here, and the operations then compile for that level
 * alone. With CONFIG_DEBUG_VM the claim is checked rather than assumed.
 */
static __always_inline void pgtp_assume_level(pgtp_t p, unsigned int lvl)
{
	if (IS_ENABLED(CONFIG_DEBUG_VM)) {
		VM_WARN_ON_ONCE(pgtp_level(p) != lvl);
		return;
	}
	__assume(p.lvl == lvl);
}

/**
 * pgtp_ptr() - Address of an entry.
 * @p: entry pointer.
 *
 * Return: the entry address.
 */
static __always_inline void *pgtp_ptr(pgtp_t p)
{
	return p.ptr;
}

/**
 * pgtp_is_null() - Check for PGTP_NULL.
 * @p: entry pointer.
 *
 * Return: true if @p is PGTP_NULL.
 */
static __always_inline bool pgtp_is_null(pgtp_t p)
{
	return !p.ptr;
}

/**
 * pgtp_table() - Table that contains an entry.
 * @p: entry pointer.
 *
 * Return: the ptdesc of the page table @p points into.
 */
struct ptdesc *pgtp_table(pgtp_t p);

#include <asm-generic/pgt-contract.h>

/**
 * pgt_root_level() - Level of the root table of an mm.
 * @mm: the mm.
 *
 * Return: the root level; at most PGT_MAX_LEVEL.
 */
static __always_inline unsigned int pgt_root_level(struct mm_struct *mm)
{
	return pgt_arch_root_level(mm);
}

/**
 * pgtp_root() - Root entry for an address.
 * @mm: the mm.
 * @addr: user address.
 *
 * Return: pointer to the entry for @addr in the root table of @mm.
 */
static __always_inline pgtp_t pgtp_root(struct mm_struct *mm,
					unsigned long addr)
{
	return pgt_arch_root(mm, addr);
}

/**
 * pgtv_kind() - Classify a value.
 * @v: value.
 *
 * Return: the kind of @v.
 */
static __always_inline enum pgt_kind pgtv_kind(pgtv_t v)
{
	return pgt_arch_kind(v);
}

/**
 * pgtv_to_leaf() - Narrow a value to a leaf.
 * @v: value; pgtv_kind() must have returned PGT_LEAF for it.
 *
 * Return: @v as a pgt_leaf_t.
 */
static __always_inline pgt_leaf_t pgtv_to_leaf(pgtv_t v)
{
	VM_WARN_ON_ONCE(pgtv_kind(v) != PGT_LEAF);
	return (pgt_leaf_t){ .v = v };
}

/**
 * pgtv_to_softleaf() - Narrow a value to a software entry.
 * @v: value; pgtv_kind() must have returned PGT_SOFTLEAF for it.
 *
 * Return: @v as a pgt_softleaf_t.
 */
static __always_inline pgt_softleaf_t pgtv_to_softleaf(pgtv_t v)
{
	VM_WARN_ON_ONCE(pgtv_kind(v) != PGT_SOFTLEAF);
	return (pgt_softleaf_t){ .v = v };
}

/**
 * pgtv_to_table() - Narrow a value to a table entry.
 * @v: value; pgtv_kind() must have returned PGT_TABLE for it.
 *
 * Return: @v as a pgt_table_t.
 */
static __always_inline pgt_table_t pgtv_to_table(pgtv_t v)
{
	VM_WARN_ON_ONCE(pgtv_kind(v) != PGT_TABLE);
	return (pgt_table_t){ .v = v };
}

/**
 * pgtv_from_leaf() - A leaf as a value of any kind.
 * @l: leaf.
 *
 * For the operations that take any kind, pgtp_xchg() and pgtv_same().
 *
 * Return: @l as a pgtv_t.
 */
static __always_inline pgtv_t pgtv_from_leaf(pgt_leaf_t l)
{
	return l.v;
}

/**
 * pgtv_from_softleaf() - A software entry as a value of any kind.
 * @s: software entry.
 *
 * Return: @s as a pgtv_t.
 */
static __always_inline pgtv_t pgtv_from_softleaf(pgt_softleaf_t s)
{
	return s.v;
}

/**
 * pgtv_from_table() - A table entry as a value of any kind.
 * @t: table entry.
 *
 * Return: @t as a pgtv_t.
 */
static __always_inline pgtv_t pgtv_from_table(pgt_table_t t)
{
	return t.v;
}

/**
 * pgt_table_child() - Entry for an address in the table a TABLE links.
 * @t: TABLE snapshot of an entry.
 * @addr: address inside the range of that entry.
 *
 * The child table is found from @t; the entry it was read from is not read
 * again. With CONFIG_HIGHPTE a level-0 child is mapped here and must go to
 * pgtp_lock_table() or pgtp_map() before use.
 *
 * Return: pointer to the entry for @addr one level below @t.
 */
static __always_inline pgtp_t pgt_table_child(pgt_table_t t, unsigned long addr)
{
	return pgt_arch_child(t.v, addr);
}

/**
 * pgtp_advance() - Entry @n places further in the same table.
 * @p: entry pointer.
 * @n: number of entries to skip.
 *
 * Return: pointer to the entry @n entries after @p.
 */
static __always_inline pgtp_t pgtp_advance(pgtp_t p, unsigned int n)
{
	unsigned int lvl = pgtp_level(p);
	unsigned long size = 1UL << pgt_arch_entry_order(lvl);
	void *ptr = pgtp_ptr(p) + n * size;

	return pgtp_make(ptr, lvl);
}

/**
 * pgtp_next() - Next entry in the same table.
 * @p: entry pointer.
 *
 * Return: pointer to the entry after @p.
 */
static __always_inline pgtp_t pgtp_next(pgtp_t p)
{
	return pgtp_advance(p, 1);
}

/**
 * pgtp_addr_end() - End of the range an entry covers, clipped.
 * @p: entry pointer.
 * @addr: address inside the range of @p; page aligned at level 0.
 * @end: end of the walk; page aligned at level 0, so that a level-0 entry
 *       never passes it.
 *
 * Return: the end of @p's range or @end, whichever comes first.
 */
static __always_inline unsigned long pgtp_addr_end(pgtp_t p, unsigned long addr,
						   unsigned long end)
{
	VM_WARN_ON_ONCE(!pgtp_level(p) && ((addr | end) & ~PAGE_MASK));
	return pgt_arch_addr_end(p, addr, end);
}

/**
 * pgtp_size() - Size of the range an entry covers.
 * @p: entry pointer.
 *
 * Return: the size in bytes, PAGE_SIZE at level 0.
 */
static __always_inline unsigned long pgtp_size(pgtp_t p)
{
	return 1UL << pgt_arch_shift(pgtp_level(p));
}

/* Reads and stores */

/**
 * pgtp_get() - Snapshot an entry that does not change under the caller.
 * @p: entry pointer.
 *
 * The caller holds the lock of @p, or nothing else can change *@p. Reads
 * the entry once; on some architectures that gathers the accessed and
 * dirty bits of a contiguous block.
 *
 * Return: the value, at the level of @p.
 */
static __always_inline pgtv_t pgtp_get(pgtp_t p)
{
	unsigned int lvl = pgtp_level(p);

	return (pgtv_t){ .val = pgt_arch_load(pgtp_ptr(p), lvl), .lvl = lvl };
}

/**
 * pgtp_get_lockless() - Snapshot an entry that may change concurrently.
 * @p: entry pointer.
 *
 * For walkers that do not hold the lock of @p: GUP-fast, RCU-only walks,
 * and the entries a walker descends through above the level it locks.
 * The value is one the entry held at some point, not a mix of two. With
 * 32-bit entries split in two halves, a level-1 read only guarantees that
 * with interrupts disabled.
 *
 * Return: the value, at the level of @p.
 */
static __always_inline pgtv_t pgtp_get_lockless(pgtp_t p)
{
	unsigned int lvl = pgtp_level(p);

	return (pgtv_t){
		.val = pgt_arch_load_lockless(pgtp_ptr(p), lvl),
		.lvl = lvl,
	};
}

/**
 * pgtp_set_leaf() - Store a leaf.
 * @mm: the mm.
 * @addr: address the entry maps.
 * @p: entry pointer.
 * @l: leaf; must be at the level of @p.
 *
 * The caller holds the lock of @p.
 */
static __always_inline void pgtp_set_leaf(struct mm_struct *mm,
					  unsigned long addr, pgtp_t p,
					  pgt_leaf_t l)
{
	VM_WARN_ON_ONCE(l.v.lvl != pgtp_level(p));
	pgt_arch_store_leaf(mm, addr, pgtp_ptr(p), l.v.val, pgtp_level(p), 1);
}

/**
 * pgtp_set_leaf_nr() - Store consecutive leaves.
 * @mm: the mm.
 * @addr: address the first entry maps.
 * @p: pointer to the first entry.
 * @l: leaf of the first entry; must be at the level of @p.
 * @nr: number of entries; above one only at level 0.
 *
 * Entry i maps the memory of @l advanced i times by pgt_leaf_next(). The
 * caller holds the lock of @p.
 */
static __always_inline void pgtp_set_leaf_nr(struct mm_struct *mm,
					     unsigned long addr, pgtp_t p,
					     pgt_leaf_t l, unsigned int nr)
{
	VM_WARN_ON_ONCE(l.v.lvl != pgtp_level(p));
	VM_WARN_ON_ONCE(l.v.lvl && nr != 1);
	pgt_arch_store_leaf(mm, addr, pgtp_ptr(p), l.v.val, pgtp_level(p),
			    nr);
}

/**
 * pgtp_set_softleaf() - Store a software entry.
 * @mm: the mm.
 * @addr: address the entry maps.
 * @p: entry pointer.
 * @s: software entry; must be at the level of @p.
 *
 * The caller holds the lock of @p.
 */
static __always_inline void pgtp_set_softleaf(struct mm_struct *mm,
					      unsigned long addr, pgtp_t p,
					      pgt_softleaf_t s)
{
	VM_WARN_ON_ONCE(s.v.lvl != pgtp_level(p));
	pgt_arch_store_softleaf(mm, addr, pgtp_ptr(p), s.v.val,
				pgtp_level(p));
}

/**
 * pgtp_xchg() - Exchange an entry.
 * @mm: the mm.
 * @addr: address the entry maps.
 * @p: entry pointer.
 * @v: new value of any kind; must be at the level of @p.
 *
 * The caller holds the lock of @p.
 *
 * Return: the old value.
 */
static __always_inline pgtv_t pgtp_xchg(struct mm_struct *mm,
					unsigned long addr, pgtp_t p, pgtv_t v)
{
	unsigned int lvl = pgtp_level(p);

	VM_WARN_ON_ONCE(v.lvl != lvl);
	return (pgtv_t){
		.val = pgt_arch_xchg(mm, addr, pgtp_ptr(p), v.val, lvl),
		.lvl = lvl,
	};
}

/**
 * pgtp_clear() - Clear an entry.
 * @mm: the mm.
 * @addr: address the entry maps.
 * @p: entry pointer.
 *
 * The caller holds the lock of @p.
 */
static __always_inline void pgtp_clear(struct mm_struct *mm, unsigned long addr,
				       pgtp_t p)
{
	pgt_arch_clear(mm, addr, pgtp_ptr(p), pgtp_level(p));
}

/**
 * pgtp_invalidate() - Make a leaf unusable by the hardware.
 * @vma: the VMA.
 * @addr: address the entry maps.
 * @p: pointer to a LEAF entry above level 0.
 *
 * The entry stays a LEAF, the hardware can no longer use it, and the TLB
 * is flushed for its range. The caller holds the lock of @p.
 *
 * Return: the leaf as it was when the hardware lost access, with every
 * accessed and dirty bit the hardware set until then.
 */
static __always_inline pgt_leaf_t pgtp_invalidate(struct vm_area_struct *vma,
						  unsigned long addr, pgtp_t p)
{
	unsigned int lvl = pgtp_level(p);
	pgtv_t old;

	/* Level 0 has no split to protect; clearing is enough there */
	VM_WARN_ON_ONCE(!lvl || !pgt_arch_can_leaf(lvl));
	old = (pgtv_t){
		.val = pgt_arch_invalidate(vma, addr, pgtp_ptr(p), lvl),
		.lvl = lvl,
	};
	return pgtv_to_leaf(old);
}

/**
 * pgt_alloc_child() - Link a new table below an empty entry.
 * @mm: the mm.
 * @p: entry pointer at level 1 or above.
 * @v: out: a fresh snapshot of *@p.
 *
 * Allocates a table for the level below @p, takes the lock of @p and links
 * the table if *@p is still NONE, otherwise frees it. May sleep.
 *
 * Return: 0, or -ENOMEM.
 */
int pgt_alloc_child(struct mm_struct *mm, pgtp_t p, pgtv_t *v);

/**
 * pgtp_clear_bad() - Report and clear a BAD entry.
 * @p: entry pointer.
 *
 * Needs no lock: nothing else writes a BAD entry.
 */
void pgtp_clear_bad(pgtp_t p);

/* Values of any kind */

/**
 * pgtv_same() - Compare two values.
 * @a: value.
 * @b: value at the same level as @a.
 *
 * As pte_same(), bits that only the architecture's own bookkeeping changes
 * may be ignored.
 *
 * Return: true if the entries are identical.
 */
static __always_inline bool pgtv_same(pgtv_t a, pgtv_t b)
{
	VM_WARN_ON_ONCE(a.lvl != b.lvl);
	return pgt_arch_same(a, b);
}

/**
 * pgt_none_mk() - Build a NONE value.
 * @lvl: level.
 *
 * Return: the empty entry at @lvl.
 */
static __always_inline pgtv_t pgt_none_mk(unsigned int lvl)
{
	return pgt_arch_none_mk(lvl);
}

/* Leaves */

/**
 * pgt_leaf_mk() - Build a leaf.
 * @pfn: first page frame.
 * @prot: protection in the canonical (level-0) layout.
 * @lvl: level of the leaf; must be able to hold a leaf.
 *
 * Return: a leaf mapping @pfn at @lvl.
 */
static __always_inline pgt_leaf_t pgt_leaf_mk(unsigned long pfn, pgprot_t prot,
					      unsigned int lvl)
{
	pgt_leaf_t l;

	VM_WARN_ON_ONCE(!pgt_arch_can_leaf(lvl));
	l.v = pgt_arch_mk(pfn, prot, lvl);
	return l;
}

/**
 * pgt_leaf_accessible() - Check whether the hardware may use a leaf.
 * @l: leaf.
 * @write: also check for write access.
 *
 * False for PROT_NONE and invalidated leaves and where protection keys
 * forbid the access. This is the test GUP-fast makes.
 *
 * Return: true if user access of the given type is allowed.
 */
static __always_inline bool pgt_leaf_accessible(pgt_leaf_t l, bool write)
{
	return pgt_arch_accessible(l.v, write);
}

/**
 * pgt_leaf_pfn() - Page frame a leaf maps.
 * @l: leaf.
 *
 * Return: the first PFN of the memory @l maps.
 */
static __always_inline unsigned long pgt_leaf_pfn(pgt_leaf_t l)
{
	return pgt_arch_pfn(l.v);
}

/**
 * pgt_leaf_shift() - Size of the memory a leaf maps, as a shift.
 * @l: leaf.
 *
 * Return: log2 of the size, PAGE_SHIFT at level 0.
 */
static __always_inline unsigned int pgt_leaf_shift(pgt_leaf_t l)
{
	return pgt_arch_shift(l.v.lvl);
}

/**
 * pgt_leaf_size() - Size of the memory a leaf maps.
 * @l: leaf.
 *
 * Return: the size in bytes.
 */
static __always_inline unsigned long pgt_leaf_size(pgt_leaf_t l)
{
	return 1UL << pgt_leaf_shift(l);
}

/**
 * pgt_leaf_write() - Check whether a leaf is writable.
 * @l: leaf.
 *
 * True also for an x86 shadow stack entry.
 *
 * Return: true if writable.
 */
static __always_inline bool pgt_leaf_write(pgt_leaf_t l)
{
	return pgt_arch_write(l.v);
}

/**
 * pgt_leaf_dirty() - Check whether a leaf is dirty.
 * @l: leaf.
 *
 * True for a hardware-dirty and for a saved-dirty entry.
 *
 * Return: true if dirty.
 */
static __always_inline bool pgt_leaf_dirty(pgt_leaf_t l)
{
	return pgt_arch_test_flag(l.v, PGT_F_DIRTY) ||
	       pgt_arch_test_flag(l.v, PGT_F_SAVED_DIRTY);
}

/**
 * pgt_leaf_young() - Check whether a leaf is young.
 * @l: leaf.
 *
 * Return: true if accessed.
 */
static __always_inline bool pgt_leaf_young(pgt_leaf_t l)
{
	return pgt_arch_test_flag(l.v, PGT_F_YOUNG);
}

/**
 * pgt_leaf_special() - Check whether a leaf is special.
 * @l: leaf.
 *
 * Return: true if @l maps memory without a refcounted struct page.
 */
static __always_inline bool pgt_leaf_special(pgt_leaf_t l)
{
	return pgt_arch_test_flag(l.v, PGT_F_SPECIAL);
}

/**
 * pgt_leaf_soft_dirty() - Check the soft-dirty state.
 * @l: leaf.
 *
 * Return: true if soft-dirty.
 */
static __always_inline bool pgt_leaf_soft_dirty(pgt_leaf_t l)
{
	return pgt_arch_test_flag(l.v, PGT_F_SOFT_DIRTY);
}

/**
 * pgt_leaf_uffd_wp() - Check the userfaultfd write-protect state.
 * @l: leaf.
 *
 * Return: true if write-protected by userfaultfd.
 */
static __always_inline bool pgt_leaf_uffd_wp(pgt_leaf_t l)
{
	return pgt_arch_test_flag(l.v, PGT_F_UFFD_WP);
}

/**
 * pgt_leaf_exec() - Check whether a leaf is executable.
 * @l: leaf.
 *
 * On the fallback every leaf reads as executable.
 *
 * Return: true if executable.
 */
static __always_inline bool pgt_leaf_exec(pgt_leaf_t l)
{
	return !pgt_arch_test_flag(l.v, PGT_F_NOEXEC);
}

/**
 * pgt_leaf_user() - Check whether a leaf is user-accessible.
 * @l: leaf.
 *
 * Return: true if the user bit is set; false where the format has none,
 * and always on the fallback.
 */
static __always_inline bool pgt_leaf_user(pgt_leaf_t l)
{
	return pgt_arch_test_flag(l.v, PGT_F_USER);
}

/**
 * pgt_leaf_cont() - Check the contiguous hint.
 * @l: leaf.
 *
 * Return: true if @l is part of a contiguous-hint run.
 */
static __always_inline bool pgt_leaf_cont(pgt_leaf_t l)
{
	return pgt_arch_test_flag(l.v, PGT_F_CONT);
}

/**
 * pgt_leaf_modify() - Change the protection of a leaf.
 * @l: leaf.
 * @prot: new protection in the canonical layout.
 *
 * Takes the access permissions from @prot and keeps the PFN and the young,
 * dirty, soft-dirty, special and uffd-wp state. The memory type is kept on
 * x86 and taken from @prot on arm64, riscv and powerpc.
 *
 * Return: the modified value.
 */
static __always_inline pgt_leaf_t pgt_leaf_modify(pgt_leaf_t l, pgprot_t prot)
{
	l.v = pgt_arch_modify(l.v, prot);
	return l;
}

/**
 * pgt_leaf_pgprot() - Protection of a leaf.
 * @l: leaf.
 *
 * Return: the protection in the canonical layout.
 */
static __always_inline pgprot_t pgt_leaf_pgprot(pgt_leaf_t l)
{
	return pgt_arch_pgprot(l.v);
}

/**
 * pgt_leaf_mkwrite() - Make a leaf writable.
 * @l: leaf.
 * @vma: the VMA; decides between a normal and a shadow stack entry.
 *
 * Return: the modified value.
 */
static __always_inline pgt_leaf_t pgt_leaf_mkwrite(pgt_leaf_t l,
						   struct vm_area_struct *vma)
{
	l.v = pgt_arch_mkwrite(l.v, vma);
	return l;
}

/**
 * pgt_leaf_mkwrite_novma() - Make a leaf writable outside a shadow stack VMA.
 * @l: leaf.
 *
 * Return: the modified value.
 */
static __always_inline pgt_leaf_t pgt_leaf_mkwrite_novma(pgt_leaf_t l)
{
	l.v = pgt_arch_mkwrite(l.v, NULL);
	return l;
}

/**
 * pgt_leaf_wrprotect() - Make a leaf read-only.
 * @l: leaf.
 *
 * Return: the modified value.
 */
static __always_inline pgt_leaf_t pgt_leaf_wrprotect(pgt_leaf_t l)
{
	l.v = pgt_arch_wrprotect(l.v);
	return l;
}

/**
 * pgt_leaf_mkdirty() - Mark a leaf dirty.
 * @l: leaf.
 *
 * Also sets soft-dirty.
 *
 * Return: the modified value.
 */
static __always_inline pgt_leaf_t pgt_leaf_mkdirty(pgt_leaf_t l)
{
	l.v = pgt_arch_mkdirty(l.v);
	return l;
}

/**
 * pgt_leaf_mkclean() - Clear the dirty state of a leaf.
 * @l: leaf.
 *
 * Return: the modified value.
 */
static __always_inline pgt_leaf_t pgt_leaf_mkclean(pgt_leaf_t l)
{
	l.v = pgt_arch_clear_flag(l.v, PGT_F_DIRTY);
	l.v = pgt_arch_clear_flag(l.v, PGT_F_SAVED_DIRTY);
	return l;
}

/**
 * pgt_leaf_mkyoung() - Mark a leaf young.
 * @l: leaf.
 *
 * Return: the modified value.
 */
static __always_inline pgt_leaf_t pgt_leaf_mkyoung(pgt_leaf_t l)
{
	l.v = pgt_arch_set_flag(l.v, PGT_F_YOUNG);
	return l;
}

/**
 * pgt_leaf_mkold() - Mark a leaf old.
 * @l: leaf.
 *
 * Return: the modified value.
 */
static __always_inline pgt_leaf_t pgt_leaf_mkold(pgt_leaf_t l)
{
	l.v = pgt_arch_clear_flag(l.v, PGT_F_YOUNG);
	return l;
}

/**
 * pgt_leaf_mkspecial() - Mark a leaf special.
 * @l: leaf.
 *
 * Return: the modified value.
 */
static __always_inline pgt_leaf_t pgt_leaf_mkspecial(pgt_leaf_t l)
{
	l.v = pgt_arch_set_flag(l.v, PGT_F_SPECIAL);
	return l;
}

/**
 * pgt_leaf_mksoft_dirty() - Set soft-dirty.
 * @l: leaf.
 *
 * Return: the modified value.
 */
static __always_inline pgt_leaf_t pgt_leaf_mksoft_dirty(pgt_leaf_t l)
{
	l.v = pgt_arch_set_flag(l.v, PGT_F_SOFT_DIRTY);
	return l;
}

/**
 * pgt_leaf_clear_soft_dirty() - Clear soft-dirty.
 * @l: leaf.
 *
 * Return: the modified value.
 */
static __always_inline pgt_leaf_t pgt_leaf_clear_soft_dirty(pgt_leaf_t l)
{
	l.v = pgt_arch_clear_flag(l.v, PGT_F_SOFT_DIRTY);
	return l;
}

/**
 * pgt_leaf_mkuffd_wp() - Set userfaultfd write-protect.
 * @l: leaf.
 *
 * Return: the modified value, also write-protected.
 */
static __always_inline pgt_leaf_t pgt_leaf_mkuffd_wp(pgt_leaf_t l)
{
	l.v = pgt_arch_set_flag(l.v, PGT_F_UFFD_WP);
	return pgt_leaf_wrprotect(l);
}

/**
 * pgt_leaf_clear_uffd_wp() - Clear userfaultfd write-protect.
 * @l: leaf.
 *
 * Return: the modified value.
 */
static __always_inline pgt_leaf_t pgt_leaf_clear_uffd_wp(pgt_leaf_t l)
{
	l.v = pgt_arch_clear_flag(l.v, PGT_F_UFFD_WP);
	return l;
}

/**
 * pgt_leaf_next() - Advance a leaf to the memory that follows it.
 * @l: leaf.
 *
 * Return: @l with its PFN advanced by pgt_leaf_size(@l) >> PAGE_SHIFT.
 */
static __always_inline pgt_leaf_t pgt_leaf_next(pgt_leaf_t l)
{
	l.v = pgt_arch_next(l.v);
	return l;
}

/* Software entries */

/**
 * pgt_softleaf_mk() - Encode a software entry.
 * @e: the software entry.
 * @lvl: level; must be able to hold a SOFTLEAF.
 *
 * Return: @e encoded at @lvl.
 */
static __always_inline pgt_softleaf_t pgt_softleaf_mk(softleaf_t e,
						      unsigned int lvl)
{
	pgt_softleaf_t s;

	VM_WARN_ON_ONCE(!pgt_arch_can_softleaf(lvl));
	s.v = pgt_arch_from_softleaf(e, lvl);
	return s;
}

/**
 * softleaf_from_pgt() - Decode a software entry.
 * @s: software entry.
 *
 * Return: the softleaf_t @s encodes.
 */
static __always_inline softleaf_t softleaf_from_pgt(pgt_softleaf_t s)
{
	return pgt_arch_to_softleaf(s.v);
}

/**
 * pgt_softleaf_soft_dirty() - Check the soft-dirty state.
 * @s: software entry.
 *
 * Return: true if soft-dirty.
 */
static __always_inline bool pgt_softleaf_soft_dirty(pgt_softleaf_t s)
{
	return pgt_arch_test_flag(s.v, PGT_F_SWP_SOFT_DIRTY);
}

/**
 * pgt_softleaf_uffd_wp() - Check the userfaultfd write-protect state.
 * @s: software entry.
 *
 * Return: true if write-protected by userfaultfd.
 */
static __always_inline bool pgt_softleaf_uffd_wp(pgt_softleaf_t s)
{
	return pgt_arch_test_flag(s.v, PGT_F_SWP_UFFD_WP);
}

/**
 * pgt_softleaf_mksoft_dirty() - Set soft-dirty.
 * @s: software entry.
 *
 * Return: the modified value.
 */
static __always_inline pgt_softleaf_t
pgt_softleaf_mksoft_dirty(pgt_softleaf_t s)
{
	s.v = pgt_arch_set_flag(s.v, PGT_F_SWP_SOFT_DIRTY);
	return s;
}

/**
 * pgt_softleaf_clear_soft_dirty() - Clear soft-dirty.
 * @s: software entry.
 *
 * Return: the modified value.
 */
static __always_inline pgt_softleaf_t
pgt_softleaf_clear_soft_dirty(pgt_softleaf_t s)
{
	s.v = pgt_arch_clear_flag(s.v, PGT_F_SWP_SOFT_DIRTY);
	return s;
}

/**
 * pgt_softleaf_mkuffd_wp() - Set userfaultfd write-protect.
 * @s: software entry.
 *
 * Return: the modified value.
 */
static __always_inline pgt_softleaf_t pgt_softleaf_mkuffd_wp(pgt_softleaf_t s)
{
	s.v = pgt_arch_set_flag(s.v, PGT_F_SWP_UFFD_WP);
	return s;
}

/**
 * pgt_softleaf_clear_uffd_wp() - Clear userfaultfd write-protect.
 * @s: software entry.
 *
 * Return: the modified value.
 */
static __always_inline pgt_softleaf_t
pgt_softleaf_clear_uffd_wp(pgt_softleaf_t s)
{
	s.v = pgt_arch_clear_flag(s.v, PGT_F_SWP_UFFD_WP);
	return s;
}

/* Tables */

/**
 * pgt_table_mk() - Build a table entry.
 * @table: the table to link; its level is @lvl - 1.
 * @lvl: level of the entry; at least 1.
 *
 * The fallback cannot build a table entry without storing it and warns;
 * link tables with pgt_alloc_child().
 *
 * Return: a table entry at @lvl linking @table.
 */
static __always_inline pgt_table_t pgt_table_mk(struct ptdesc *table,
						unsigned int lvl)
{
	pgt_table_t t;

	t.v = pgt_arch_table_entry(table, lvl);
	return t;
}

/* Locking */

/**
 * pgtp_lockptr() - Lock that protects an entry.
 * @mm: the mm.
 * @p: entry pointer.
 *
 * The split lock of the table containing @p for levels that have one,
 * &mm->page_table_lock otherwise. Reads the table's ptdesc: the table must
 * be known to be alive.
 *
 * Return: the lock.
 */
static __always_inline spinlock_t *pgtp_lockptr(struct mm_struct *mm, pgtp_t p)
{
	return pgt_arch_lockptr(mm, p);
}

/**
 * pgtp_lock_table() - Lock a level-0 table.
 * @mm: the mm.
 * @p: entry pointer at level 0.
 * @pp: the entry @p was reached through, or PGTP_NULL for a root entry.
 *
 * Enters an RCU read-side section, checks that *@pp still links the table
 * containing @p, takes the table lock and checks again.
 *
 * Return: the lock held, or NULL if the table is gone and the caller must
 * walk again from this address.
 */
spinlock_t *pgtp_lock_table(struct mm_struct *mm, pgtp_t p, pgtp_t pp);

/**
 * pgtp_lock_entry() - Lock an entry at level 1 or above.
 * @mm: the mm.
 * @p: entry pointer at level 1 or above.
 *
 * Takes the lock that covers the entry. A table above level 0 is freed only
 * by free_pgtables(), after its VMA is unlinked, so it cannot go away under
 * the caller and there is nothing to check again.
 *
 * Return: the lock held.
 */
spinlock_t *pgtp_lock_entry(struct mm_struct *mm, pgtp_t p);

/**
 * pgtp_unlock() - Release a lock taken by pgtp_lock_table() or
 * pgtp_lock_entry().
 * @p: the entry pointer passed to the lock function.
 * @ptl: the lock it returned.
 *
 * Drops the lock and, for a level-0 table, the RCU read-side section and,
 * with CONFIG_HIGHPTE, the mapping.
 */
void pgtp_unlock(pgtp_t p, spinlock_t *ptl);

/**
 * pgtp_map() - Access a table read-only, without its lock.
 * @p: entry pointer.
 * @pp: the entry @p was reached through, or PGTP_NULL for a root entry.
 *
 * Enters an RCU read-side section and checks that *@pp still links the
 * table containing @p. Entries read this way may change at any time.
 *
 * Return: true on success; false if the caller must walk again.
 */
bool pgtp_map(pgtp_t p, pgtp_t pp);

/**
 * pgtp_unmap() - End access started by pgtp_map().
 * @p: the entry pointer passed to pgtp_map().
 */
void pgtp_unmap(pgtp_t p);

/**
 * pgtp_assert_locked() - Debug check that the lock of an entry is held.
 * @mm: the mm.
 * @p: entry pointer.
 *
 * A macro, so that this header does not need <linux/lockdep.h>; without
 * lockdep it compiles to nothing.
 */
#define pgtp_assert_locked(mm, p)					\
do {									\
	if (IS_ENABLED(CONFIG_LOCKDEP))					\
		lockdep_assert_held(pgtp_lockptr(mm, p));		\
} while (0)

/* Batching */

/**
 * pgt_leaf_batch() - Count leaves that map consecutive pages of one folio.
 * @folio: the large folio @l maps.
 * @p: pointer to the first entry, at level 0.
 * @l: snapshot of *@p.
 * @max_nr: maximum number of entries to look at; must not cross the end
 *          of the VMA or of the table.
 *
 * The entries of a batch map consecutive pages of @folio and differ from
 * @l only in the PFN and the accessed, writable, dirty and soft-dirty
 * state.
 *
 * Return: the number of entries in the batch, at least one.
 */
unsigned int pgt_leaf_batch(struct folio *folio, pgtp_t p, pgt_leaf_t l,
			    unsigned int max_nr);

/* TLB; macros because they expand to the <asm/tlb.h> helpers */

/**
 * tlb_remove_pgt_entry() - Record a cleared leaf for the TLB flush.
 * @tlb: the mmu_gather.
 * @p: pointer to the cleared entry.
 * @addr: address the entry mapped.
 */
#define tlb_remove_pgt_entry(tlb, p, addr)				\
	pgt_arch_tlb_remove_entry(tlb, p, addr)

/**
 * tlb_remove_pgt_table() - Free an unlinked table after the TLB flush.
 * @tlb: the mmu_gather.
 * @t: the table entry that linked the table, as it was before the entry
 *     was cleared.
 * @addr: an address the table covered.
 *
 * Also drops the mm's page table accounting for the table.
 */
#define tlb_remove_pgt_table(tlb, t, addr)				\
	pgt_arch_tlb_remove_table(tlb, pgtv_from_table(t), addr)

#endif /* _LINUX_PGT_H */
