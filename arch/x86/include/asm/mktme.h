#ifndef	_ASM_X86_MKTME_H
#define	_ASM_X86_MKTME_H

#include <linux/types.h>
#include <linux/page_ext.h>
#include <linux/jump_label.h>

#ifdef CONFIG_X86_INTEL_MKTME
extern phys_addr_t __mktme_keyid_mask;
extern phys_addr_t mktme_keyid_mask(void);
extern int __mktme_keyid_shift;
extern int mktme_keyid_shift(void);
extern int __mktme_nr_keyids;
extern int mktme_nr_keyids(void);
extern unsigned int mktme_algs;

DECLARE_STATIC_KEY_FALSE(mktme_enabled_key);
static inline bool mktme_enabled(void)
{
	return static_branch_unlikely(&mktme_enabled_key);
}

extern struct page_ext_operations page_mktme_ops;

#define page_keyid page_keyid
static inline int page_keyid(const struct page *page)
{
	if (!mktme_enabled())
		return 0;

	return lookup_page_ext(page)->keyid;
}

#else
#define mktme_keyid_mask()	((phys_addr_t)0)
#define mktme_nr_keyids()	0
#define mktme_keyid_shift()	0

#define page_keyid(page) 0

static inline bool mktme_enabled(void)
{
	return false;
}
#endif

#endif
