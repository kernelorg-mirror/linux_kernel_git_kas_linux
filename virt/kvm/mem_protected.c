#include <linux/kvm_host.h>
#include <linux/mm.h>
#include <linux/rmap.h>

static DEFINE_XARRAY(kvm_pfn_map);

static bool gfn_is_shared(struct kvm *kvm, unsigned long gfn)
{
	bool ret = false;
	int i;

	spin_lock(&kvm->mmu_lock);
	for (i = 0; i < kvm->nr_shared_ranges; i++) {
		if (gfn < kvm->shared_ranges[i].start)
			continue;
		if (gfn >= kvm->shared_ranges[i].end)
			continue;

		ret = true;
		break;
	}
	spin_unlock(&kvm->mmu_lock);

	return ret;
}

bool kvm_protect_pfn(struct kvm *kvm, gfn_t gfn, kvm_pfn_t pfn)
{
	struct page *page = pfn_to_page(pfn);
	bool ret = true;

	if (gfn_is_shared(kvm, gfn))
		return true;

	if (is_zero_pfn(pfn))
		return true;

	/* Only anonymous and shmem/tmpfs pages are supported */
	if (!PageSwapBacked(page))
		return false;

	lock_page(page);

	/* Recheck gfn_is_shared() under page lock */
	if (gfn_is_shared(kvm, gfn))
		goto out;

	if (!TestSetPageHWPoison(page)) {
		try_to_unmap(page, TTU_IGNORE_MLOCK);
		xa_store(&kvm_pfn_map, pfn, kvm->id, GFP_KERNEL);
	} else if (xa_load(&kvm_pfn_map, pfn) != kvm->id) {
		ret = false;
	}
out:
	unlock_page(page);
	return ret;
}
EXPORT_SYMBOL_GPL(kvm_protect_pfn);

void __kvm_share_memory(struct kvm *kvm,
			unsigned long start, unsigned long end)
{
	/*
	 * Out of slots.
	 * Still worth to proceed: the new range may merge with an existing
	 * one.
	 */
	WARN_ON_ONCE(kvm->nr_shared_ranges == ARRAY_SIZE(kvm->shared_ranges));

	spin_lock(&kvm->mmu_lock);
	kvm->nr_shared_ranges = add_range_with_merge(kvm->shared_ranges,
						ARRAY_SIZE(kvm->shared_ranges),
						kvm->nr_shared_ranges,
						start, end);
	kvm->nr_shared_ranges = clean_sort_range(kvm->shared_ranges,
					    ARRAY_SIZE(kvm->shared_ranges));
	spin_unlock(&kvm->mmu_lock);
}
EXPORT_SYMBOL(__kvm_share_memory);

void kvm_share_pfn(struct kvm *kvm, kvm_pfn_t pfn)
{
	struct page *page = pfn_to_page(pfn);

	lock_page(page);
	if (xa_load(&kvm_pfn_map, pfn) == kvm->id) {
		xa_erase(&kvm_pfn_map, pfn);
		ClearPageHWPoison(page);
	}
	unlock_page(page);
}
EXPORT_SYMBOL(kvm_share_pfn);

void kvm_unpoison_page(struct page *page)
{
	unsigned long pfn = page_to_pfn(page);

	if (xa_erase(&kvm_pfn_map, pfn))
		ClearPageHWPoison(page);
}

bool kvm_page_allowed(struct kvm *kvm, struct page *page)
{
	unsigned long pfn = page_to_pfn(page);

	if (!PageHWPoison(page))
		return true;

	return xa_load(&kvm_pfn_map, pfn) == kvm->id;
}
