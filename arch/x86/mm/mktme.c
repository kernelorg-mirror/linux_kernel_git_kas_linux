#include <linux/mm.h>
#include <linux/highmem.h>
#include <asm/mktme.h>

/* Mask to extract KeyID from physical address. */
phys_addr_t __mktme_keyid_mask;
phys_addr_t mktme_keyid_mask(void)
{
	return __mktme_keyid_mask;
}
EXPORT_SYMBOL_GPL(mktme_keyid_mask);

/* Shift of KeyID within physical address. */
int __mktme_keyid_shift;
int mktme_keyid_shift(void)
{
	return __mktme_keyid_shift;
}
EXPORT_SYMBOL_GPL(mktme_keyid_shift);

/*
 * Number of KeyIDs available for MKTME.
 * Excludes KeyID-0 which used by TME. MKTME KeyIDs start from 1.
 */
int __mktme_nr_keyids;
int mktme_nr_keyids(void)
{
	return __mktme_nr_keyids;
}

unsigned int mktme_algs;

DEFINE_STATIC_KEY_FALSE(mktme_enabled_key);
EXPORT_SYMBOL_GPL(mktme_enabled_key);

void mktme_disable(void)
{
	physical_mask = (1ULL << __PHYSICAL_MASK_SHIFT) - 1;
	__mktme_keyid_mask = 0;
	__mktme_keyid_shift = 0;
	__mktme_nr_keyids = 0;
	if (mktme_enabled())
		static_branch_disable(&mktme_enabled_key);
}

static bool need_page_mktme(void)
{
	/* Make sure keyid doesn't collide with extended page flags */
	BUILD_BUG_ON(__NR_PAGE_EXT_FLAGS > 16);

	return !!mktme_nr_keyids();
}

static void init_page_mktme(void)
{
	static_branch_enable(&mktme_enabled_key);
}

struct page_ext_operations page_mktme_ops = {
	.need = need_page_mktme,
	.init = init_page_mktme,
};

int __vma_keyid(struct vm_area_struct *vma)
{
	pgprotval_t prot = pgprot_val(vma->vm_page_prot);
	return (prot & mktme_keyid_mask()) >> mktme_keyid_shift();
}

/* Prepare page to be used for encryption. Called from page allocator. */
void __prep_encrypted_page(struct page *page, int order, int keyid, bool zero)
{
	int i;

	/*
	 * The hardware/CPU does not enforce coherency between mappings
	 * of the same physical page with different KeyIDs or
	 * encryption keys. We are responsible for cache management.
	 *
	 * Flush cache lines with KeyID-0. page_address() returns virtual
	 * address of the page mapping with the current (zero) KeyID.
	 */
	clflush_cache_range(page_address(page), PAGE_SIZE * (1UL << order));

	for (i = 0; i < (1 << order); i++) {
		/* All pages coming out of the allocator should have KeyID 0 */
		WARN_ON_ONCE(lookup_page_ext(page)->keyid);

		/*
		 * Change KeyID. From now on page_address() will return address
		 * of the page mapping with the new KeyID.
		 *
		 * We don't need barrier() before the KeyID change because
		 * clflush_cache_range() above stops compiler from reordring
		 * past the point with mb().
		 *
		 * And we don't need a barrier() after the assignment because
		 * any future reference of KeyID (i.e. from page_address())
		 * will create address dependency and compiler is not allow to
		 * mess with this.
		 */
		lookup_page_ext(page)->keyid = keyid;

		/* Clear the page after the KeyID is set. */
		if (zero)
			clear_highpage(page);

		page++;
	}
}

/*
 * Handles freeing of encrypted page.
 * Called from page allocator on freeing encrypted page.
 */
void free_encrypted_page(struct page *page, int order)
{
	int i;

	/*
	 * The hardware/CPU does not enforce coherency between mappings
	 * of the same physical page with different KeyIDs or
	 * encryption keys. We are responsible for cache management.
	 *
	 * Flush cache lines with non-0 KeyID. page_address() returns virtual
	 * address of the page mapping with the current (non-zero) KeyID.
	 */
	clflush_cache_range(page_address(page), PAGE_SIZE * (1UL << order));

	for (i = 0; i < (1 << order); i++) {
		/* Check if the page has reasonable KeyID */
		WARN_ON_ONCE(!lookup_page_ext(page)->keyid);
		WARN_ON_ONCE(lookup_page_ext(page)->keyid > mktme_nr_keyids());

		/*
		 * Switch the page back to zero KeyID.
		 *
		 * We don't need barrier() before the KeyID change because
		 * clflush_cache_range() above stops compiler from reordring
		 * past the point with mb().
		 *
		 * And we don't need a barrier() after the assignment because
		 * any future reference of KeyID (i.e. from page_address())
		 * will create address dependency and compiler is not allow to
		 * mess with this.
		 */
		lookup_page_ext(page)->keyid = 0;
		page++;
	}
}
