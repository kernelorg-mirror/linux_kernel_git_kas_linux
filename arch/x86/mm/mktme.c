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
