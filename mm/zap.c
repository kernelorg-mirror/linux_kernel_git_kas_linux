// SPDX-License-Identifier: GPL-2.0-only
/*
 * Zap: remove the page table entries that map a range of a VMA, for
 * munmap(), MADV_DONTNEED, exit and the OOM reaper, and those that map a
 * range of a file in every VMA, for truncation and hole punching.
 */
#include <linux/export.h>
#include <linux/hugetlb.h>
#include <linux/ksm.h>
#include <linux/leafops.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/mmu_notifier.h>
#include <linux/pgalloc.h>
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
 * This function makes sure that we'll replace the none pte with an uffd-wp
 * swap special pte marker when necessary. Must be with the pgtable lock held.
 *
 * Returns true if uffd-wp ptes was installed, false otherwise.
 */
static inline bool
zap_install_uffd_wp_if_needed(struct vm_area_struct *vma,
			      unsigned long addr, pte_t *pte, int nr,
			      struct zap_details *details, pte_t pteval)
{
	if (zap_drop_markers(details))
		return false;

	return cond_install_uffd_wp_ptes(vma, addr, pte, pteval, nr);
}

static __always_inline void zap_present_folio_ptes(struct mmu_gather *tlb,
		struct vm_area_struct *vma, struct folio *folio,
		struct page *page, pte_t *pte, pte_t ptent, unsigned int nr,
		unsigned long addr, struct zap_details *details, int *rss,
		bool *force_flush, bool *force_break, bool *any_skipped)
{
	struct mm_struct *mm = tlb->mm;
	bool delay_rmap = false;

	if (!folio_test_anon(folio)) {
		ptent = get_and_clear_full_ptes(mm, addr, pte, nr, tlb->fullmm);
		if (pte_dirty(ptent)) {
			folio_mark_dirty(folio);
			if (tlb_delay_rmap(tlb)) {
				delay_rmap = true;
				*force_flush = true;
			}
		}
		if (pte_young(ptent) && likely(vma_has_recency(vma)))
			folio_mark_accessed(folio);
		rss[mm_counter(folio)] -= nr;
	} else {
		/* We don't need up-to-date accessed/dirty bits. */
		clear_full_ptes(mm, addr, pte, nr, tlb->fullmm);
		rss[MM_ANONPAGES] -= nr;
	}
	/* Checking a single PTE in a batch is sufficient. */
	arch_check_zapped_pte(vma, ptent);
	tlb_remove_tlb_entries(tlb, pte, nr, addr);
	if (unlikely(userfaultfd_pte_wp(vma, ptent)))
		*any_skipped = zap_install_uffd_wp_if_needed(vma, addr, pte,
							     nr, details, ptent);

	if (!delay_rmap) {
		folio_remove_rmap_ptes(folio, page, nr, vma);

		if (unlikely(folio_mapcount(folio) < 0))
			print_bad_pte(vma, addr, ptent, page);
	}
	if (unlikely(__tlb_remove_folio_pages(tlb, page, nr, delay_rmap))) {
		*force_flush = true;
		*force_break = true;
	}
}

/*
 * Zap or skip at least one present PTE, trying to batch-process subsequent
 * PTEs that map consecutive pages of the same folio.
 *
 * Returns the number of processed (skipped or zapped) PTEs (at least 1).
 */
static inline int zap_present_ptes(struct mmu_gather *tlb,
		struct vm_area_struct *vma, pte_t *pte, pte_t ptent,
		unsigned int max_nr, unsigned long addr,
		struct zap_details *details, int *rss, bool *force_flush,
		bool *force_break, bool *any_skipped)
{
	struct mm_struct *mm = tlb->mm;
	struct folio *folio;
	struct page *page;
	int nr;

	page = vm_normal_page(vma, addr, ptent);
	if (!page) {
		/* We don't need up-to-date accessed/dirty bits. */
		ptep_get_and_clear_full(mm, addr, pte, tlb->fullmm);
		arch_check_zapped_pte(vma, ptent);
		tlb_remove_tlb_entry(tlb, pte, addr);
		if (userfaultfd_pte_wp(vma, ptent))
			*any_skipped = zap_install_uffd_wp_if_needed(vma, addr,
						pte, 1, details, ptent);
		ksm_might_unmap_zero_page(mm, ptent);
		return 1;
	}

	folio = page_folio(page);
	if (unlikely(!should_zap_folio(details, folio))) {
		*any_skipped = true;
		return 1;
	}

	/*
	 * Make sure that the common "small folio" case is as fast as possible
	 * by keeping the batching logic separate.
	 */
	if (unlikely(folio_test_large(folio) && max_nr != 1)) {
		nr = folio_pte_batch(folio, pte, ptent, max_nr);
		zap_present_folio_ptes(tlb, vma, folio, page, pte, ptent, nr,
				       addr, details, rss, force_flush,
				       force_break, any_skipped);
		return nr;
	}
	zap_present_folio_ptes(tlb, vma, folio, page, pte, ptent, 1, addr,
			       details, rss, force_flush, force_break, any_skipped);
	return 1;
}

static inline int zap_nonpresent_ptes(struct mmu_gather *tlb,
		struct vm_area_struct *vma, pte_t *pte, pte_t ptent,
		unsigned int max_nr, unsigned long addr,
		struct zap_details *details, int *rss, bool *any_skipped)
{
	softleaf_t entry;
	int nr = 1;

	*any_skipped = true;
	entry = softleaf_from_pte(ptent);
	if (softleaf_is_device_private(entry) ||
	    softleaf_is_device_exclusive(entry)) {
		struct page *page = softleaf_to_page(entry);
		struct folio *folio = page_folio(page);

		if (unlikely(!should_zap_folio(details, folio)))
			return 1;
		/*
		 * Both device private/exclusive mappings should only
		 * work with anonymous page so far, so we don't need to
		 * consider uffd-wp bit when zap. For more information,
		 * see zap_install_uffd_wp_if_needed().
		 */
		WARN_ON_ONCE(!folio_test_anon(folio));
		rss[mm_counter(folio)]--;
		folio_remove_rmap_pte(folio, page, vma);
		folio_put(folio);
	} else if (softleaf_is_swap(entry)) {
		/* Genuine swap entries, hence a private anon pages */
		if (!should_zap_cows(details))
			return 1;

		nr = swap_pte_batch(pte, max_nr, ptent);
		rss[MM_SWAPENTS] -= nr;
		swap_put_entries_direct(entry, nr);
	} else if (softleaf_is_migration(entry)) {
		struct folio *folio = softleaf_to_folio(entry);

		if (!should_zap_folio(details, folio))
			return 1;
		rss[mm_counter(folio)]--;
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
	clear_nonpresent_ptes(vma->vm_mm, addr, pte, nr);
	*any_skipped = zap_install_uffd_wp_if_needed(vma, addr, pte, nr, details, ptent);

	return nr;
}

static inline int do_zap_pte_range(struct mmu_gather *tlb,
				   struct vm_area_struct *vma, pte_t *pte,
				   unsigned long addr, unsigned long end,
				   struct zap_details *details, int *rss,
				   bool *force_flush, bool *force_break,
				   bool *any_skipped)
{
	pte_t ptent = ptep_get(pte);
	int max_nr = (end - addr) / PAGE_SIZE;
	int nr = 0;

	/* Skip all consecutive none ptes */
	if (pte_none(ptent)) {
		for (nr = 1; nr < max_nr; nr++) {
			ptent = ptep_get(pte + nr);
			if (!pte_none(ptent))
				break;
		}
		max_nr -= nr;
		if (!max_nr)
			return nr;
		pte += nr;
		addr += nr * PAGE_SIZE;
	}

	if (pte_present(ptent))
		nr += zap_present_ptes(tlb, vma, pte, ptent, max_nr, addr,
				       details, rss, force_flush, force_break,
				       any_skipped);
	else
		nr += zap_nonpresent_ptes(tlb, vma, pte, ptent, max_nr, addr,
					  details, rss, any_skipped);

	return nr;
}

static bool pte_table_reclaim_possible(unsigned long start, unsigned long end,
		struct zap_details *details)
{
	if (!IS_ENABLED(CONFIG_PT_RECLAIM))
		return false;
	/* Only zap if we are allowed to and cover the full page table. */
	return details && details->reclaim_pt && (end - start >= PMD_SIZE);
}

static bool zap_empty_pte_table(struct mm_struct *mm, pmd_t *pmd,
		spinlock_t *ptl, pmd_t *pmdval)
{
	spinlock_t *pml = pmd_lockptr(mm, pmd);

	if (ptl != pml && !spin_trylock(pml))
		return false;

	*pmdval = pmdp_get(pmd);
	pmd_clear(pmd);
	if (ptl != pml)
		spin_unlock(pml);
	return true;
}

static bool zap_pte_table_if_empty(struct mm_struct *mm, pmd_t *pmd,
		unsigned long addr, pmd_t *pmdval)
{
	spinlock_t *pml, *ptl = NULL;
	pte_t *start_pte, *pte;
	int i;

	pml = pmd_lock(mm, pmd);
	start_pte = pte_offset_map_rw_nolock(mm, pmd, addr, pmdval, &ptl);
	if (!start_pte)
		goto out_ptl;
	if (ptl != pml)
		spin_lock_nested(ptl, SINGLE_DEPTH_NESTING);

	for (i = 0, pte = start_pte; i < PTRS_PER_PTE; i++, pte++) {
		if (!pte_none(ptep_get(pte)))
			goto out_ptl;
	}
	pte_unmap(start_pte);

	pmd_clear(pmd);

	if (ptl != pml)
		spin_unlock(ptl);
	spin_unlock(pml);
	return true;
out_ptl:
	if (start_pte)
		pte_unmap_unlock(start_pte, ptl);
	if (ptl != pml)
		spin_unlock(pml);
	return false;
}

static unsigned long zap_pte_range(struct mmu_gather *tlb,
				struct vm_area_struct *vma, pmd_t *pmd,
				unsigned long addr, unsigned long end,
				struct zap_details *details)
{
	bool can_reclaim_pt = pte_table_reclaim_possible(addr, end, details);
	bool force_flush = false, force_break = false;
	struct mm_struct *mm = tlb->mm;
	int rss[NR_MM_COUNTERS];
	spinlock_t *ptl;
	pte_t *start_pte;
	pte_t *pte;
	pmd_t pmdval;
	unsigned long start = addr;
	bool direct_reclaim = true;
	int nr;

retry:
	tlb_change_page_size(tlb, PAGE_SIZE);
	init_rss_vec(rss);
	start_pte = pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
	if (!pte)
		return addr;

	flush_tlb_batched_pending(mm);
	lazy_mmu_mode_enable();
	do {
		bool any_skipped = false;

		if (need_resched()) {
			direct_reclaim = false;
			break;
		}

		nr = do_zap_pte_range(tlb, vma, pte, addr, end, details, rss,
				      &force_flush, &force_break, &any_skipped);
		if (any_skipped)
			can_reclaim_pt = false;
		if (unlikely(force_break)) {
			addr += nr * PAGE_SIZE;
			direct_reclaim = false;
			break;
		}
	} while (pte += nr, addr += PAGE_SIZE * nr, addr != end);

	/*
	 * Fast path: try to hold the pmd lock and unmap the PTE page.
	 *
	 * If the pte lock was released midway (retry case), or if the attempt
	 * to hold the pmd lock failed, then we need to recheck all pte entries
	 * to ensure they are still none, thereby preventing the pte entries
	 * from being repopulated by another thread.
	 */
	if (can_reclaim_pt && direct_reclaim && addr == end)
		direct_reclaim = zap_empty_pte_table(mm, pmd, ptl, &pmdval);

	add_mm_rss_vec(mm, rss);
	lazy_mmu_mode_disable();

	/* Do the actual TLB flush before dropping ptl */
	if (force_flush) {
		tlb_flush_mmu_tlbonly(tlb);
		tlb_flush_rmaps(tlb, vma);
	}
	pte_unmap_unlock(start_pte, ptl);

	/*
	 * If we forced a TLB flush (either due to running out of
	 * batch buffers or because we needed to flush dirty TLB
	 * entries before releasing the ptl), free the batched
	 * memory too. Come back again if we didn't do everything.
	 */
	if (force_flush)
		tlb_flush_mmu(tlb);

	if (addr != end) {
		cond_resched();
		force_flush = false;
		force_break = false;
		goto retry;
	}

	if (can_reclaim_pt) {
		if (direct_reclaim || zap_pte_table_if_empty(mm, pmd, start, &pmdval)) {
			pte_free_tlb(tlb, pmd_pgtable(pmdval), start);
			mm_dec_nr_ptes(mm);
		}
	}

	return addr;
}

static inline unsigned long zap_pmd_range(struct mmu_gather *tlb,
				struct vm_area_struct *vma, pud_t *pud,
				unsigned long addr, unsigned long end,
				struct zap_details *details)
{
	pmd_t *pmd;
	unsigned long next;

	pmd = pmd_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);
		if (pmd_is_huge(*pmd)) {
			if (next - addr != HPAGE_PMD_SIZE)
				__split_huge_pmd(vma, pmd, addr, false);
			else if (zap_huge_pmd(tlb, vma, pmd, addr)) {
				addr = next;
				continue;
			}
			/* fall through */
		} else if (details && details->single_folio &&
			   folio_test_pmd_mappable(details->single_folio) &&
			   next - addr == HPAGE_PMD_SIZE && pmd_none(*pmd)) {
			sync_with_folio_pmd_zap(tlb->mm, pmd);
		}
		if (pmd_none(*pmd)) {
			addr = next;
			continue;
		}
		addr = zap_pte_range(tlb, vma, pmd, addr, next, details);
		if (addr != next)
			pmd--;
	} while (pmd++, cond_resched(), addr != end);

	return addr;
}

static inline unsigned long zap_pud_range(struct mmu_gather *tlb,
				struct vm_area_struct *vma, p4d_t *p4d,
				unsigned long addr, unsigned long end,
				struct zap_details *details)
{
	pud_t *pud;
	unsigned long next;

	pud = pud_offset(p4d, addr);
	do {
		next = pud_addr_end(addr, end);
		if (pud_trans_huge(*pud)) {
			if (next - addr != HPAGE_PUD_SIZE)
				split_huge_pud(vma, pud, addr);
			else if (zap_huge_pud(tlb, vma, pud, addr))
				goto next;
			/* fall through */
		}
		if (pud_none_or_clear_bad(pud))
			continue;
		next = zap_pmd_range(tlb, vma, pud, addr, next, details);
next:
		cond_resched();
	} while (pud++, addr = next, addr != end);

	return addr;
}

static inline unsigned long zap_p4d_range(struct mmu_gather *tlb,
				struct vm_area_struct *vma, pgd_t *pgd,
				unsigned long addr, unsigned long end,
				struct zap_details *details)
{
	p4d_t *p4d;
	unsigned long next;

	p4d = p4d_offset(pgd, addr);
	do {
		next = p4d_addr_end(addr, end);
		if (p4d_none_or_clear_bad(p4d))
			continue;
		next = zap_pud_range(tlb, vma, p4d, addr, next, details);
	} while (p4d++, addr = next, addr != end);

	return addr;
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
		unsigned long next, addr = start;
		pgd_t *pgd;

		tlb_start_vma(tlb, vma);
		pgd = pgd_offset(vma->vm_mm, addr);
		do {
			next = pgd_addr_end(addr, end);
			if (pgd_none_or_clear_bad(pgd))
				continue;
			next = zap_p4d_range(tlb, vma, pgd, addr, next, details);
		} while (pgd++, addr = next, addr != end);
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
