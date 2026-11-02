// SPDX-License-Identifier: GPL-2.0-only
/*
 * Zap: remove the page table entries that map a range of a VMA, for
 * munmap(), MADV_DONTNEED, exit and the OOM reaper, and those that map a
 * range of a file in every VMA, for truncation and hole punching.
 *
 * One pgt level walker visits every table. A leaf takes the same path at
 * every level: a run of level-0 entries under the table lock, or one leaf
 * above level 0, which the range covers whole, under its entry lock. What
 * differs by level is a constant: the size of a leaf, uffd-wp markers and
 * delayed rmaps at level 0, the deposited table of a PMD leaf. A leaf the
 * range covers in part is split first. hugetlb VMAs are handed to
 * __unmap_hugepage_range().
 */
#include <linux/export.h>
#include <linux/hugetlb.h>
#include <linux/ksm.h>
#include <linux/leafops.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/mmu_notifier.h>
#include <linux/pgalloc.h>
#include <linux/pgt-walk.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/uprobes.h>
#include <linux/userfaultfd_k.h>

#include <asm/tlb.h>

#include "internal.h"

/* Whether we should zap all COWed (private) pages too */
static inline bool should_zap_cows(struct zap_details *details)
{
	/* By default, zap all pages */
	if (!details)
		return true;

	VM_WARN_ON_ONCE(details->skip_cows && details->reclaim_pt);

	/* Or, we zap COWed pages only if the caller wants to */
	return !details->skip_cows;
}

/* Decides whether we should zap this folio with the folio pointer specified */
static inline bool should_zap_folio(struct zap_details *details,
				    struct folio *folio)
{
	/* If we can make a decision without *folio.. */
	if (should_zap_cows(details))
		return true;

	/* Otherwise we should only zap non-anon folios */
	return !folio_test_anon(folio);
}

static inline bool zap_drop_markers(struct zap_details *details)
{
	if (!details)
		return false;

	return details->zap_flags & ZAP_FLAG_DROP_MARKER;
}

/*
 * Get the "struct page" a pgt leaf maps, at any level. See
 * __vm_normal_page(). pgt level n is the n-th level counted from the leaf,
 * as enum pgtable_level counts.
 */
static __always_inline struct page *
vm_normal_page_pgt(struct vm_area_struct *vma, unsigned long addr,
		   pgt_leaf_t l)
{
	pgtv_t v = pgtv_from_leaf(l);
	enum pgtable_level level = pgtv_level(v);
	unsigned long pfn = pgt_leaf_pfn(l);
	bool special = pgt_leaf_special(l);
	const void *entry;
	size_t size;

	entry = pgtv_raw(&v, &size);
	return __vm_normal_page(vma, addr, pfn, special, entry, size, level);
}

/*
 * Leave uffd-wp markers behind entries that were write-protected by
 * userfaultfd and have just been cleared, as cond_install_uffd_wp_ptes()
 * does. Called with the table lock held.
 *
 * Returns true if markers were installed.
 */
static bool zap_install_uffd_wp(struct vm_area_struct *vma, unsigned long addr,
				pgtp_t p, unsigned int nr,
				struct zap_details *details, bool uffd_wp)
{
	pgt_softleaf_t marker;
	softleaf_t entry;

	pgtp_assume_level(p, 0);
	if (!uffd_wp || zap_drop_markers(details) || !uffd_supports_wp_marker())
		return false;
	/* The entry should be "cleared" before calling */
	WARN_ON_ONCE(pgtv_kind(pgtp_get(p)) != PGT_NONE);
	if (vma_is_anonymous(vma) || !userfaultfd_wp(vma))
		return false;

	entry = make_pte_marker_entry(PTE_MARKER_UFFD_WP);
	marker = pgt_softleaf_mk(entry, 0);
	for (;;) {
		pgtp_set_softleaf(vma->vm_mm, addr, p, marker);
		if (--nr == 0)
			break;
		p = pgtp_next(p);
		addr += PAGE_SIZE;
	}
	return true;
}

/* As print_bad_pte(), for a leaf of any level */
static void zap_print_bad(struct vm_area_struct *vma, unsigned long addr,
			  pgt_leaf_t l, struct page *page)
{
	pgtv_t v = pgtv_from_leaf(l);
	enum pgtable_level level = pgtv_level(v);
	const void *entry;
	size_t size;

	entry = pgtv_raw(&v, &size);
	print_bad_page_map(vma, addr, entry, size, page, level);
}

struct zap_walk {
	struct mmu_gather *tlb;
	struct vm_area_struct *vma;
	struct zap_details *details;
	int rss[NR_MM_COUNTERS];
	bool force_flush;
	bool force_break;
	bool any_skipped;
};

/*
 * Above level 0 only a PMD leaf keeps a table, deposited for its split:
 * where the architecture deposits one for every leaf, for an anonymous
 * folio, and for the huge zero folio outside DAX. It goes with the leaf.
 * @l is the leaf as it was, or NULL for a software entry.
 */
static __always_inline void zap_withdraw_table(struct zap_walk *zw, pgtp_t p,
					       const pgt_leaf_t *l,
					       struct folio *folio,
					       const unsigned int lvl)
{
	bool deposit;

	if (!IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE) || lvl != 1)
		return;
	if (arch_needs_pgtable_deposit())
		deposit = true;
	else if (l && is_huge_zero_pfn(pgt_leaf_pfn(*l)))
		deposit = !vma_is_dax(zw->vma);
	else
		deposit = folio && folio_test_anon(folio);
	if (deposit)
		zap_deposited_table(zw->tlb->mm, pgtp_ptr(p));
}

static __always_inline void zap_present_folio(struct zap_walk *zw,
		struct folio *folio, struct page *page, pgtp_t p,
		pgt_leaf_t l, unsigned int nr, unsigned long addr,
		const unsigned int lvl)
{
	unsigned long pages = nr * (pgtp_size(p) >> PAGE_SHIFT);
	struct mmu_gather *tlb = zw->tlb;
	struct vm_area_struct *vma = zw->vma;
	bool delay_rmap = false;

	if (!folio_test_anon(folio)) {
		l = pgtp_get_and_clear_leaves(vma, addr, p, nr, tlb->fullmm);
		if (pgt_leaf_dirty(l)) {
			folio_mark_dirty(folio);
			/* mmu_gather removes the delayed rmaps of PTEs only */
			if (!lvl && tlb_delay_rmap(tlb)) {
				delay_rmap = true;
				zw->force_flush = true;
			}
		}
		if (pgt_leaf_young(l) && likely(vma_has_recency(vma)))
			folio_mark_accessed(folio);
		zw->rss[mm_counter(folio)] -= pages;
	} else {
		/* We don't need up-to-date accessed/dirty bits */
		pgtp_clear_leaves(vma, addr, p, nr, tlb->fullmm);
		zw->rss[MM_ANONPAGES] -= pages;
	}
	/* Checking a single leaf of a batch is sufficient */
	pgt_leaf_check_zapped(vma, l);
	tlb_remove_pgt_entries(tlb, p, nr, addr);
	if (!lvl && unlikely(userfaultfd_wp(vma) && pgt_leaf_uffd_wp(l)))
		zw->any_skipped = zap_install_uffd_wp(vma, addr, p, nr,
						      zw->details, true);
	zap_withdraw_table(zw, p, &l, folio, lvl);

	if (!delay_rmap) {
		folio_remove_rmap(folio, page, pages, vma, lvl);

		if (unlikely(folio_mapcount(folio) < 0))
			zap_print_bad(vma, addr, l, page);
	}
	if (unlikely(tlb_remove_pgt_pages(tlb, p, page, nr, delay_rmap))) {
		zw->force_flush = true;
		zw->force_break = true;
	}
}

/*
 * Zap or skip at least one leaf, batching the following ones that map
 * consecutive pages of the same folio.
 *
 * Returns the number of processed (skipped or zapped) entries (at least 1).
 */
static __always_inline unsigned int zap_present(struct zap_walk *zw, pgtp_t p,
						pgt_leaf_t l,
						unsigned int max_nr,
						unsigned long addr,
						const unsigned int lvl)
{
	struct mmu_gather *tlb = zw->tlb;
	struct vm_area_struct *vma = zw->vma;
	struct folio *folio;
	struct page *page;
	unsigned int nr;

	page = vm_normal_page_pgt(vma, addr, l);
	if (!page) {
		/* We don't need up-to-date accessed/dirty bits */
		pgtp_clear_leaves(vma, addr, p, 1, tlb->fullmm);
		pgt_leaf_check_zapped(vma, l);
		tlb_remove_pgt_entry(tlb, p, addr);
		if (!lvl && userfaultfd_wp(vma) && pgt_leaf_uffd_wp(l))
			zw->any_skipped = zap_install_uffd_wp(vma, addr, p, 1,
							      zw->details,
							      true);
		zap_withdraw_table(zw, p, &l, NULL, lvl);
		ksm_might_unmap_zero_pfn(tlb->mm, pgt_leaf_pfn(l),
					 pgt_leaf_dirty(l));
		return 1;
	}

	folio = page_folio(page);
	if (unlikely(!should_zap_folio(zw->details, folio))) {
		zw->any_skipped = true;
		return 1;
	}

	/*
	 * Make sure that the common "small folio" case is as fast as possible
	 * by keeping the batching logic separate.
	 */
	if (unlikely(folio_test_large(folio) && max_nr != 1)) {
		nr = pgt_leaf_batch(folio, p, l, max_nr);
		zap_present_folio(zw, folio, page, p, l, nr, addr, lvl);
		return nr;
	}
	zap_present_folio(zw, folio, page, p, l, 1, addr, lvl);
	return 1;
}

static __always_inline unsigned int zap_nonpresent(struct zap_walk *zw,
						   pgtp_t p,
						   pgt_softleaf_t s,
						   unsigned int max_nr,
						   unsigned long addr,
						   const unsigned int lvl)
{
	unsigned long pages = pgtp_size(p) >> PAGE_SHIFT;
	struct zap_details *details = zw->details;
	struct vm_area_struct *vma = zw->vma;
	softleaf_t entry = softleaf_from_pgt(s);
	struct folio *folio = NULL;
	unsigned int nr = 1;
	bool uffd_wp;

	zw->any_skipped = true;
	if (softleaf_is_device_private(entry) ||
	    softleaf_is_device_exclusive(entry)) {
		struct page *page = softleaf_to_page(entry);

		folio = page_folio(page);
		if (unlikely(!should_zap_folio(details, folio)))
			return 1;
		/*
		 * Both device private/exclusive mappings should only
		 * work with anonymous page so far, so we don't need to
		 * consider uffd-wp bit when zap. For more information,
		 * see zap_install_uffd_wp().
		 */
		WARN_ON_ONCE(!folio_test_anon(folio));
		zw->rss[mm_counter(folio)] -= pages;
		folio_remove_rmap(folio, page, pages, vma, lvl);
		folio_put(folio);
	} else if (softleaf_is_swap(entry)) {
		/* Genuine swap entries, hence a private anon pages */
		if (!should_zap_cows(details))
			return 1;

		nr = pgt_softleaf_batch(p, s, max_nr);
		zw->rss[MM_SWAPENTS] -= nr;
		swap_put_entries_direct(entry, nr);
	} else if (softleaf_is_migration(entry)) {
		folio = softleaf_to_folio(entry);
		if (!should_zap_folio(details, folio))
			return 1;
		zw->rss[mm_counter(folio)] -= pages;
	} else if (softleaf_is_uffd_wp_marker(entry)) {
		/*
		 * For anon: always drop the marker; for file: only
		 * drop the marker if explicitly requested.
		 */
		if (!vma_is_anonymous(vma) && !zap_drop_markers(details))
			return 1;
	} else if (softleaf_is_guard_marker(entry)) {
		/*
		 * Ordinary zapping should not remove guard PTE
		 * markers. Only do so if we should remove PTE markers
		 * in general.
		 */
		if (!zap_drop_markers(details))
			return 1;
	} else if (softleaf_is_hwpoison(entry) ||
		   softleaf_is_poison_marker(entry)) {
		if (!should_zap_cows(details))
			return 1;
	} else {
		/* We should have covered all the swap entry types */
		pr_alert("unrecognized swap entry 0x%lx\n", entry.val);
		WARN_ON_ONCE(1);
	}
	pgtp_clear_softleaves(vma, addr, p, nr, zw->tlb->fullmm);
	zap_withdraw_table(zw, p, NULL, folio, lvl);
	if (!lvl) {
		/* A uffd-wp software entry, or a uffd-wp marker itself */
		uffd_wp = pgt_softleaf_uffd_wp(s) ||
			  softleaf_is_uffd_wp_marker(entry);
		zw->any_skipped = zap_install_uffd_wp(vma, addr, p, nr,
						      details, uffd_wp);
	}
	return nr;
}

static __always_inline unsigned int zap_entries(struct zap_walk *zw, pgtp_t p,
						unsigned long addr,
						unsigned long end,
						const unsigned int lvl)
{
	unsigned long size = pgtp_size(p);
	unsigned int max_nr = (end - addr) / size;
	pgtv_t v = pgtp_get(p);
	unsigned int nr = 0;

	/* Skip all consecutive NONE entries */
	if (pgtv_kind(v) == PGT_NONE) {
		for (nr = 1; nr < max_nr; nr++) {
			v = pgtp_get(pgtp_advance(p, nr));
			if (pgtv_kind(v) != PGT_NONE)
				break;
		}
		max_nr -= nr;
		if (!max_nr)
			return nr;
		p = pgtp_advance(p, nr);
		addr += nr * size;
	}

	if (pgtv_kind(v) == PGT_LEAF)
		nr += zap_present(zw, p, pgtv_to_leaf(v), max_nr, addr, lvl);
	else
		nr += zap_nonpresent(zw, p, pgtv_to_softleaf(v), max_nr, addr,
				     lvl);
	return nr;
}

/* Only reclaim a table if allowed to and the zap covers all of it */
static bool zap_table_reclaim_possible(unsigned long start, unsigned long end,
				       pgtp_t pp, struct zap_details *details)
{
	if (!IS_ENABLED(CONFIG_PT_RECLAIM))
		return false;
	return details && details->reclaim_pt && end - start >= pgtp_size(pp);
}

/* Unlink the level-0 table, empty now, with its lock held */
static bool zap_unlink_empty_table(struct mm_struct *mm, pgtp_t pp,
				   spinlock_t *ptl, unsigned long addr,
				   pgt_table_t *t)
{
	spinlock_t *pml = pgtp_lockptr(mm, pp);

	if (ptl != pml && !spin_trylock(pml))
		return false;
	*t = pgtv_to_table(pgtp_get(pp));
	pgtp_clear(mm, addr, pp);
	if (ptl != pml)
		spin_unlock(pml);
	return true;
}

/* Unlink the level-0 table at @addr if no entry was refilled meanwhile */
static bool zap_unlink_table_if_empty(struct mm_struct *mm, pgtp_t pp,
				      unsigned long addr, pgt_table_t *t)
{
	unsigned int i, nr = pgtp_size(pp) >> PAGE_SHIFT;
	spinlock_t *pml, *ptl;
	bool empty = false;
	pgtp_t first, p;
	pgtv_t v;

	pml = pgtp_lock_entry(mm, pp);
	v = pgtp_get(pp);
	if (pgtv_kind(v) != PGT_TABLE)
		goto out;
	*t = pgtv_to_table(v);
	first = p = pgt_table_child(*t, addr);
	if (!pgtp_map(first, pp))
		goto out;
	ptl = pgtp_lockptr(mm, first);
	if (ptl != pml)
		spin_lock_nested(ptl, SINGLE_DEPTH_NESTING);

	for (i = 0; i < nr; i++, p = pgtp_next(p)) {
		if (pgtv_kind(pgtp_get(p)) != PGT_NONE)
			break;
	}
	empty = i == nr;
	pgtp_unmap(first);
	if (empty)
		pgtp_clear(mm, addr, pp);
	if (ptl != pml)
		spin_unlock(ptl);
out:
	pgtp_unlock(pp, pml);
	return empty;
}

/*
 * Lock the leaves from @addr: the table at level 0, the entry above it,
 * which must still hold a leaf. NULL means the caller looks at the entry
 * again.
 */
static __always_inline spinlock_t *zap_lock(struct mm_struct *mm, pgtp_t p,
					    pgtp_t pp, const unsigned int lvl)
{
	enum pgt_kind kind;
	spinlock_t *ptl;

	if (!lvl)
		return pgtp_lock_table(mm, p, pp);
	ptl = pgtp_lock_entry(mm, p);
	kind = pgtv_kind(pgtp_get(p));
	if (kind == PGT_LEAF || kind == PGT_SOFTLEAF)
		return ptl;
	pgtp_unlock(p, ptl);
	return NULL;
}

/*
 * Zap the leaves from @addr to @end under one lock: a run of entries of a
 * level-0 table, or one leaf above level 0. Returns where it stopped if the
 * table or the leaf went away.
 */
static __always_inline unsigned long zap_leaves(struct zap_walk *zw,
						pgtp_t p, pgtp_t pp,
						unsigned long addr,
						unsigned long end,
						const unsigned int lvl)
{
	struct mmu_gather *tlb = zw->tlb;
	struct vm_area_struct *vma = zw->vma;
	unsigned long size = pgtp_size(p);
	struct mm_struct *mm = tlb->mm;
	unsigned long start = addr;
	bool direct_reclaim = true;
	bool can_reclaim_pt;
	pgt_table_t table;
	spinlock_t *ptl;
	unsigned int nr;
	pgtp_t locked;
	pgtv_t v;

	/* Only a level-0 table that the zap empties is reclaimed */
	can_reclaim_pt = !lvl && zap_table_reclaim_possible(addr, end, pp,
							    zw->details);
retry:
	tlb_change_page_size(tlb, size);
	init_rss_vec(zw->rss);
	zw->force_flush = false;
	zw->force_break = false;
	ptl = zap_lock(mm, p, pp, lvl);
	if (!ptl)
		return addr;
	locked = p;

	if (!lvl) {
		flush_tlb_batched_pending(mm);
		lazy_mmu_mode_enable();
	}
	do {
		zw->any_skipped = false;

		/* A leaf above level 0 is zapped whole */
		if (!lvl && need_resched()) {
			direct_reclaim = false;
			break;
		}

		nr = zap_entries(zw, p, addr, end, lvl);
		if (zw->any_skipped)
			can_reclaim_pt = false;
		if (unlikely(zw->force_break)) {
			addr += nr * size;
			direct_reclaim = false;
			break;
		}
	} while (p = pgtp_advance(p, nr), addr += nr * size, addr != end);

	/*
	 * Fast path: try to hold the parent's lock and unlink the table.
	 *
	 * If the table lock was released midway (retry case), or if the
	 * attempt to take the parent's lock failed, then we need to recheck
	 * all entries to ensure they are still none, thereby preventing them
	 * from being repopulated by another thread.
	 */
	if (can_reclaim_pt && direct_reclaim && addr == end)
		direct_reclaim = zap_unlink_empty_table(mm, pp, ptl, start,
							&table);

	add_mm_rss_vec(mm, zw->rss);
	if (!lvl)
		lazy_mmu_mode_disable();

	/* Do the actual TLB flush before dropping ptl */
	if (zw->force_flush) {
		tlb_flush_mmu_tlbonly(tlb);
		tlb_flush_rmaps(tlb, vma);
	}
	pgtp_unlock(locked, ptl);

	/*
	 * If we forced a TLB flush (either due to running out of
	 * batch buffers or because we needed to flush dirty TLB
	 * entries before releasing the ptl), free the batched
	 * memory too. Come back again if we didn't do everything.
	 */
	if (zw->force_flush)
		tlb_flush_mmu(tlb);

	/* Only level 0 stops early: a leaf above it is zapped whole */
	if (!lvl && addr != end) {
		cond_resched();
		/* The unlock unmapped the table: find it again */
		v = pgtp_get_lockless(pp);
		if (pgtv_kind(v) != PGT_TABLE)
			return addr;
		p = pgt_table_child(pgtv_to_table(v), addr);
		goto retry;
	}

	if (can_reclaim_pt) {
		if (direct_reclaim ||
		    zap_unlink_table_if_empty(mm, pp, start, &table))
			tlb_remove_pgt_table(tlb, table, start);
	}

	return addr;
}

/*
 * The levels where zap meets leaves: THP at level 1, PUD-sized THP and DAX
 * at level 2. hugetlb VMAs are zapped elsewhere.
 */
static __always_inline bool zap_has_leaves(const unsigned int lvl)
{
	if (!lvl)
		return true;
	if (!IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE))
		return false;
	return lvl == 1 ||
	       (lvl == 2 && IS_ENABLED(CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD));
}

PGT_DEFINE_LEVEL_WALKER(zap_level, zap_body, struct zap_walk *);

/*
 * One table above level 0. A leaf that the range covers whole goes through
 * the same path as the entries of a level-0 table; one it covers in part
 * is split first. A BAD entry is reported and cleared.
 */
static __always_inline unsigned long zap_body(struct zap_walk *zw, pgtp_t p,
					      pgtp_t pp, unsigned long addr,
					      unsigned long end,
					      const unsigned int lvl)
{
	struct zap_details *details = zw->details;
	unsigned long next, ret;
	pgt_table_t t;
	pgtp_t child;
	pgtv_t v;

	if (!lvl)
		return zap_leaves(zw, p, pp, addr, end, lvl);

	do {
		next = pgtp_addr_end(p, addr, end);
again:
		v = pgtp_get_lockless(p);
		if (zap_has_leaves(lvl) && (pgtv_kind(v) == PGT_LEAF ||
					    pgtv_kind(v) == PGT_SOFTLEAF)) {
			if (next - addr != pgtp_size(p))
				pgt_split_leaf(zw->vma, p, addr);
			else if (zap_leaves(zw, p, pp, addr, next, lvl) == next)
				goto next;
			v = pgtp_get_lockless(p);
		} else if (lvl == 1 && pgtv_kind(v) == PGT_NONE && details &&
			   details->single_folio &&
			   folio_test_pmd_mappable(details->single_folio) &&
			   next - addr == pgtp_size(p)) {
			spinlock_t *ptl = pgtp_lock_entry(zw->tlb->mm, p);

			/* Wait for a concurrent zap of the folio's leaf */
			pgtp_unlock(p, ptl);
			v = pgtp_get_lockless(p);
		}
		if (pgtv_kind(v) == PGT_BAD)
			pgtp_clear_bad(p);
		if (pgtv_kind(v) != PGT_TABLE)
			goto next;

		t = pgtv_to_table(v);
		child = pgt_table_child(t, addr);
		ret = zap_level(zw, child, p, addr, next, lvl - 1);
		if (ret != next) {
			addr = ret;
			cond_resched();
			goto again;
		}
next:
		if (lvl <= 2)
			cond_resched();
	} while (p = pgtp_next(p), addr = next, addr != end);

	return end;
}

static void __zap_vma_range(struct mmu_gather *tlb, struct vm_area_struct *vma,
		unsigned long start, unsigned long end,
		struct zap_details *details)
{
	const bool reaping = details && details->reaping;

	VM_WARN_ON_ONCE(start >= end || !range_in_vma(vma, start, end));

	/* uprobe_munmap() might sleep, so skip it when reaping. */
	if (vma->vm_file && !reaping)
		uprobe_munmap(vma, start, end);

	if (unlikely(is_vm_hugetlb_page(vma))) {
		zap_flags_t zap_flags = details ? details->zap_flags : 0;

		VM_WARN_ON_ONCE(reaping);
		/*
		 * vm_file will be NULL when we fail early while instantiating
		 * a new mapping. In this case, no pages were mapped yet and
		 * there is nothing to do.
		 */
		if (!vma->vm_file)
			return;
		__unmap_hugepage_range(tlb, vma, start, end, NULL, zap_flags);
	} else {
		struct zap_walk zw = {
			.tlb = tlb,
			.vma = vma,
			.details = details,
		};
		pgtp_t root = pgtp_root(vma->vm_mm, start);

		tlb_start_vma(tlb, vma);
		zap_level(&zw, root, PGTP_NULL, start, end, pgtp_level(root));
		tlb_end_vma(tlb, vma);
	}
}

/**
 * zap_vma_for_reaping - zap all page table entries in the vma without blocking
 * @vma: The vma to zap.
 *
 * Zap all page table entries in the vma without blocking for use by the oom
 * killer. Hugetlb vmas are not supported.
 *
 * Returns: 0 on success, -EBUSY if we would have to block.
 */
int zap_vma_for_reaping(struct vm_area_struct *vma)
{
	struct zap_details details = {
		.reaping = true,
	};
	struct mmu_notifier_range range;
	struct mmu_gather tlb;

	mmu_notifier_range_init(&range, MMU_NOTIFY_CLEAR, 0, vma->vm_mm,
				vma->vm_start, vma->vm_end);
	tlb_gather_mmu(&tlb, vma->vm_mm);
	if (mmu_notifier_invalidate_range_start_nonblock(&range)) {
		tlb_finish_mmu(&tlb);
		return -EBUSY;
	}
	__zap_vma_range(&tlb, vma, range.start, range.end, &details);
	mmu_notifier_invalidate_range_end(&range);
	tlb_finish_mmu(&tlb);
	return 0;
}

/**
 * unmap_vmas - unmap a range of memory covered by a list of vma's
 * @tlb: address of the caller's struct mmu_gather
 * @unmap: The unmap_desc
 *
 * Unmap all pages in the vma list.
 *
 * Only addresses between `start' and `end' will be unmapped.
 *
 * The VMA list must be sorted in ascending virtual address order.
 *
 * unmap_vmas() assumes that the caller will flush the whole unmapped address
 * range after unmap_vmas() returns.  So the only responsibility here is to
 * ensure that any thus-far unmapped pages are flushed before unmap_vmas()
 * drops the lock and schedules.
 */
void unmap_vmas(struct mmu_gather *tlb, struct unmap_desc *unmap)
{
	struct vm_area_struct *vma;
	struct mmu_notifier_range range;
	struct zap_details details = {
		.zap_flags = ZAP_FLAG_DROP_MARKER | ZAP_FLAG_UNMAP,
	};

	vma = unmap->first;
	mmu_notifier_range_init(&range, MMU_NOTIFY_UNMAP, 0, vma->vm_mm,
				unmap->vma_start, unmap->vma_end);
	mmu_notifier_invalidate_range_start(&range);
	do {
		unsigned long start = max(vma->vm_start, unmap->vma_start);
		unsigned long end = min(vma->vm_end, unmap->vma_end);

		hugetlb_zap_begin(vma, &start, &end);
		__zap_vma_range(tlb, vma, start, end, &details);
		hugetlb_zap_end(vma, &details);
		vma = mas_find(unmap->mas, unmap->tree_end - 1);
	} while (vma);
	mmu_notifier_invalidate_range_end(&range);
}

/**
 * zap_vma_range_batched - zap page table entries in a vma range
 * @tlb: pointer to the caller's struct mmu_gather
 * @vma: the vma covering the range to zap
 * @address: starting address of the range to zap
 * @size: number of bytes to zap
 * @details: details specifying zapping behavior
 *
 * @tlb must not be NULL. The provided address range must be fully
 * contained within @vma. If @vma is for hugetlb, @tlb is flushed and
 * re-initialized by this function.
 *
 * If @details is NULL, this function will zap all page table entries.
 */
void zap_vma_range_batched(struct mmu_gather *tlb,
		struct vm_area_struct *vma, unsigned long address,
		unsigned long size, struct zap_details *details)
{
	const unsigned long end = address + size;
	struct mmu_notifier_range range;

	VM_WARN_ON_ONCE(!tlb || tlb->mm != vma->vm_mm);

	if (unlikely(!size))
		return;

	mmu_notifier_range_init(&range, MMU_NOTIFY_CLEAR, 0, vma->vm_mm,
				address, end);
	hugetlb_zap_begin(vma, &range.start, &range.end);
	update_hiwater_rss(vma->vm_mm);
	mmu_notifier_invalidate_range_start(&range);
	/*
	 * unmap 'address-end' not 'range.start-range.end' as range
	 * could have been expanded for hugetlb pmd sharing.
	 */
	__zap_vma_range(tlb, vma, address, end, details);
	mmu_notifier_invalidate_range_end(&range);
	if (is_vm_hugetlb_page(vma)) {
		/*
		 * flush tlb and free resources before hugetlb_zap_end(), to
		 * avoid concurrent page faults' allocation failure.
		 */
		tlb_finish_mmu(tlb);
		hugetlb_zap_end(vma, details);
		tlb_gather_mmu(tlb, vma->vm_mm);
	}
}

/**
 * zap_vma_range - zap all page table entries in a vma range
 * @vma: the vma covering the range to zap
 * @address: starting address of the range to zap
 * @size: number of bytes to zap
 *
 * The provided address range must be fully contained within @vma.
 */
void zap_vma_range(struct vm_area_struct *vma, unsigned long address,
		unsigned long size)
{
	struct mmu_gather tlb;

	tlb_gather_mmu(&tlb, vma->vm_mm);
	zap_vma_range_batched(&tlb, vma, address, size, NULL);
	tlb_finish_mmu(&tlb);
}

/**
 * zap_special_vma_range - zap all page table entries in a special vma range
 * @vma: the vma covering the range to zap
 * @address: starting address of the range to zap
 * @size: number of bytes to zap
 *
 * This function does nothing when the provided address range is not fully
 * contained in @vma, or when the @vma is not VM_PFNMAP or VM_MIXEDMAP.
 */
void zap_special_vma_range(struct vm_area_struct *vma, unsigned long address,
		unsigned long size)
{
	if (!range_in_vma(vma, address, address + size) ||
	   !(vma->vm_flags & (VM_PFNMAP | VM_MIXEDMAP)))
		return;

	zap_vma_range(vma, address, size);
}
EXPORT_SYMBOL_GPL(zap_special_vma_range);

static inline void unmap_mapping_range_tree(struct address_space *mapping,
					    pgoff_t first_index,
					    pgoff_t last_index,
					    struct zap_details *details)
{
	struct vm_area_struct *vma;
	struct mmu_gather tlb;

	mapping_rmap_tree_foreach(vma, mapping, first_index, last_index) {
		const pgoff_t start_idx = max(first_index, vma_start_pgoff(vma));
		const pgoff_t end_idx = min(last_index, vma_last_pgoff(vma)) + 1;
		const pgoff_t offset = start_idx - vma_start_pgoff(vma);
		const unsigned long offset_bytes = offset << PAGE_SHIFT;
		const unsigned long start = vma->vm_start + offset_bytes;
		const unsigned long size = (end_idx - start_idx) << PAGE_SHIFT;

		tlb_gather_mmu(&tlb, vma->vm_mm);
		zap_vma_range_batched(&tlb, vma, start, size, details);
		tlb_finish_mmu(&tlb);
	}
}

/**
 * unmap_mapping_folio() - Unmap single folio from processes.
 * @folio: The locked folio to be unmapped.
 *
 * Unmap this folio from any userspace process which still has it mmaped.
 * Typically, for efficiency, the range of nearby pages has already been
 * unmapped by unmap_mapping_pages() or unmap_mapping_range().  But once
 * truncation or invalidation holds the lock on a folio, it may find that
 * the page has been remapped again: and then uses unmap_mapping_folio()
 * to unmap it finally.
 */
void unmap_mapping_folio(struct folio *folio)
{
	struct address_space *mapping = folio->mapping;
	struct zap_details details = { };
	pgoff_t	first_index;
	pgoff_t	last_index;

	VM_BUG_ON(!folio_test_locked(folio));

	first_index = folio->index;
	last_index = folio_next_index(folio) - 1;

	details.skip_cows = true;
	details.single_folio = folio;
	details.zap_flags = ZAP_FLAG_DROP_MARKER;

	i_mmap_lock_read(mapping);
	if (unlikely(mapping_mapped(mapping)))
		unmap_mapping_range_tree(mapping, first_index,
					 last_index, &details);
	i_mmap_unlock_read(mapping);
}

/**
 * unmap_mapping_pages() - Unmap pages from processes.
 * @mapping: The address space containing pages to be unmapped.
 * @start: Index of first page to be unmapped.
 * @nr: Number of pages to be unmapped.  0 to unmap to end of file.
 * @even_cows: Whether to unmap even private COWed pages.
 *
 * Unmap the pages in this address space from any userspace process which
 * has them mmaped.  Generally, you want to remove COWed pages as well when
 * a file is being truncated, but not when invalidating pages from the page
 * cache.
 */
void unmap_mapping_pages(struct address_space *mapping, pgoff_t start,
		pgoff_t nr, bool even_cows)
{
	struct zap_details details = { };
	pgoff_t	first_index = start;
	pgoff_t	last_index = start + nr - 1;

	details.skip_cows = !even_cows;
	if (last_index < first_index)
		last_index = ULONG_MAX;

	i_mmap_lock_read(mapping);
	if (unlikely(mapping_mapped(mapping)))
		unmap_mapping_range_tree(mapping, first_index,
					 last_index, &details);
	i_mmap_unlock_read(mapping);
}
EXPORT_SYMBOL_GPL(unmap_mapping_pages);

/**
 * unmap_mapping_range - unmap the portion of all mmaps in the specified
 * address_space corresponding to the specified byte range in the underlying
 * file.
 *
 * @mapping: the address space containing mmaps to be unmapped.
 * @holebegin: byte in first page to unmap, relative to the start of
 * the underlying file.  This will be rounded down to a PAGE_SIZE
 * boundary.  Note that this is different from truncate_pagecache(), which
 * must keep the partial page.  In contrast, we must get rid of
 * partial pages.
 * @holelen: size of prospective hole in bytes.  This will be rounded
 * up to a PAGE_SIZE boundary.  A holelen of zero truncates to the
 * end of the file.
 * @even_cows: 1 when truncating a file, unmap even private COWed pages;
 * but 0 when invalidating pagecache, don't throw away private data.
 */
void unmap_mapping_range(struct address_space *mapping,
		loff_t const holebegin, loff_t const holelen, int even_cows)
{
	pgoff_t hba = (pgoff_t)(holebegin) >> PAGE_SHIFT;
	pgoff_t hlen = ((pgoff_t)(holelen) + PAGE_SIZE - 1) >> PAGE_SHIFT;

	/* Check for overflow. */
	if (sizeof(holelen) > sizeof(hlen)) {
		long long holeend =
			(holebegin + holelen + PAGE_SIZE - 1) >> PAGE_SHIFT;
		if (holeend & ~(long long)ULONG_MAX)
			hlen = ULONG_MAX - hba + 1;
	}

	unmap_mapping_pages(mapping, hba, hlen, even_cows);
}
EXPORT_SYMBOL(unmap_mapping_range);
