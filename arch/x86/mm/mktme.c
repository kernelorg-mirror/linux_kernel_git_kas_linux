#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/rmap.h>
#include <asm/mktme.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>

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

	sync_direct_mapping(PAGE_OFFSET, PAGE_OFFSET + direct_mapping_size);
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

/* Set the encryption keyid bits in a VMA */
void mprotect_set_encrypt(struct vm_area_struct *vma, int newkeyid,
			  unsigned long start, unsigned long end)
{
	int oldkeyid = vma_keyid(vma);
	pgprotval_t newprot;

	/* Unmap pages with old KeyID if there's any. */
	zap_page_range(vma, start, end - start);

	if (oldkeyid == newkeyid)
		return;
	vma_put_encrypt_ref(vma);
	newprot = pgprot_val(vma->vm_page_prot);
	newprot &= ~mktme_keyid_mask();
	newprot |= (unsigned long)newkeyid << mktme_keyid_shift();
	vma->vm_page_prot = __pgprot(newprot);
	vma_get_encrypt_ref(vma);

	/*
	 * The VMA doesn't have any inherited pages.
	 * Start anon VMA tree from scratch.
	 */
	unlink_anon_vmas(vma);
}

void vma_get_encrypt_ref(struct vm_area_struct *vma)
{
	if (vma_keyid(vma))
		percpu_ref_get(&encrypt_count[vma_keyid(vma)]);
}

void vma_put_encrypt_ref(struct vm_area_struct *vma)
{
	if (vma_keyid(vma))
		percpu_ref_put(&encrypt_count[vma_keyid(vma)]);
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

	/*
	 * Make sure the KeyID cannot be freed until the last page that
	 * uses the KeyID is gone.
	 *
	 * This is required because the page may live longer than VMA it
	 * is mapped into (i.e. in get_user_pages() case) and having
	 * refcounting per-VMA is not enough.
	 *
	 * Taking a reference per-4K helps in case if the page will be
	 * split after the allocation. free_encrypted_page() will balance
	 * out the refcount even if the page was split and freed as bunch
	 * of 4K pages.
	 */

	percpu_ref_get_many(&encrypt_count[keyid], 1 << order);
}

/*
 * Handles freeing of encrypted page.
 * Called from page allocator on freeing encrypted page.
 */
void free_encrypted_page(struct page *page, int order)
{
	int i, keyid;

	keyid = page_keyid(page);

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

	percpu_ref_put_many(&encrypt_count[keyid], 1 << order);
}

static int sync_direct_mapping_pte(unsigned long keyid,
		pmd_t *dst_pmd, pmd_t *src_pmd,
		unsigned long addr, unsigned long end)
{
	pte_t *src_pte, *dst_pte;
	pte_t *new_pte = NULL;
	bool remove_pte;

	/*
	 * We want to unmap and free the page table if the source is empty and
	 * the range covers whole page table.
	 */
	remove_pte = !src_pmd && PAGE_ALIGNED(addr) && PAGE_ALIGNED(end);

	/*
	 * PMD page got split into page table.
	 * Clear PMD mapping. Page table will be established instead.
	 */
	if (pmd_large(*dst_pmd)) {
		spin_lock(&init_mm.page_table_lock);
		pmd_clear(dst_pmd);
		spin_unlock(&init_mm.page_table_lock);
	}

	/* Allocate a new page table if needed. */
	if (pmd_none(*dst_pmd)) {
		new_pte = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
		if (!new_pte)
			return -ENOMEM;
		dst_pte = new_pte + pte_index(addr + keyid * direct_mapping_size);
	} else {
		dst_pte = pte_offset_map(dst_pmd, addr + keyid * direct_mapping_size);
	}
	src_pte = src_pmd ? pte_offset_map(src_pmd, addr) : NULL;

	spin_lock(&init_mm.page_table_lock);

	do {
		pteval_t val;

		if (!src_pte || pte_none(*src_pte)) {
			set_pte(dst_pte, __pte(0));
			goto next;
		}

		if (!pte_none(*dst_pte)) {
			/*
			 * Sanity check: PFNs must match between source
			 * and destination even if the rest doesn't.
			 */
			BUG_ON(pte_pfn(*dst_pte) != pte_pfn(*src_pte));
		}

		/* Copy entry, but set KeyID. */
		val = pte_val(*src_pte) | keyid << mktme_keyid_shift();
		val &= __supported_pte_mask;
		set_pte(dst_pte, __pte(val));
next:
		addr += PAGE_SIZE;
		dst_pte++;
		if (src_pte)
			src_pte++;
	} while (addr != end);

	if (new_pte)
		pmd_populate_kernel(&init_mm, dst_pmd, new_pte);

	if (remove_pte) {
		__free_page(pmd_page(*dst_pmd));
		pmd_clear(dst_pmd);
	}

	spin_unlock(&init_mm.page_table_lock);

	return 0;
}

static int sync_direct_mapping_pmd(unsigned long keyid,
		pud_t *dst_pud, pud_t *src_pud,
		unsigned long addr, unsigned long end)
{
	pmd_t *src_pmd, *dst_pmd;
	pmd_t *new_pmd = NULL;
	bool remove_pmd = false;
	unsigned long next;
	int ret = 0;

	/*
	 * We want to unmap and free the page table if the source is empty and
	 * the range covers whole page table.
	 */
	remove_pmd = !src_pud && IS_ALIGNED(addr, PUD_SIZE) && IS_ALIGNED(end, PUD_SIZE);

	/*
	 * PUD page got split into page table.
	 * Clear PUD mapping. Page table will be established instead.
	 */
	if (pud_large(*dst_pud)) {
		spin_lock(&init_mm.page_table_lock);
		pud_clear(dst_pud);
		spin_unlock(&init_mm.page_table_lock);
	}

	/* Allocate a new page table if needed. */
	if (pud_none(*dst_pud)) {
		new_pmd = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
		if (!new_pmd)
			return -ENOMEM;
		dst_pmd = new_pmd + pmd_index(addr + keyid * direct_mapping_size);
	} else {
		dst_pmd = pmd_offset(dst_pud, addr + keyid * direct_mapping_size);
	}
	src_pmd = src_pud ? pmd_offset(src_pud, addr) : NULL;

	do {
		pmd_t *__src_pmd = src_pmd;

		next = pmd_addr_end(addr, end);
		if (!__src_pmd || pmd_none(*__src_pmd)) {
			if (pmd_none(*dst_pmd))
				goto next;
			if (pmd_large(*dst_pmd)) {
				spin_lock(&init_mm.page_table_lock);
				set_pmd(dst_pmd, __pmd(0));
				spin_unlock(&init_mm.page_table_lock);
				goto next;
			}
			__src_pmd = NULL;
		}

		if (__src_pmd && pmd_large(*__src_pmd)) {
			pmdval_t val;

			if (pmd_large(*dst_pmd)) {
				/*
				 * Sanity check: PFNs must match between source
				 * and destination even if the rest doesn't.
				 */
				BUG_ON(pmd_pfn(*dst_pmd) != pmd_pfn(*__src_pmd));
			} else if (!pmd_none(*dst_pmd)) {
				/*
				 * Page table is replaced with a PMD page.
				 * Free and unmap the page table.
				 */
				__free_page(pmd_page(*dst_pmd));
				spin_lock(&init_mm.page_table_lock);
				pmd_clear(dst_pmd);
				spin_unlock(&init_mm.page_table_lock);
			}

			/* Copy entry, but set KeyID. */
			val = pmd_val(*__src_pmd) | keyid << mktme_keyid_shift();
			val &= __supported_pte_mask;
			spin_lock(&init_mm.page_table_lock);
			set_pmd(dst_pmd, __pmd(val));
			spin_unlock(&init_mm.page_table_lock);
			goto next;
		}

		ret = sync_direct_mapping_pte(keyid, dst_pmd, __src_pmd,
				addr, next);
next:
		addr = next;
		dst_pmd++;
		if (src_pmd)
			src_pmd++;
	} while (addr != end && !ret);

	if (new_pmd) {
		spin_lock(&init_mm.page_table_lock);
		pud_populate(&init_mm, dst_pud, new_pmd);
		spin_unlock(&init_mm.page_table_lock);
	}

	if (remove_pmd) {
		spin_lock(&init_mm.page_table_lock);
		__free_page(pud_page(*dst_pud));
		pud_clear(dst_pud);
		spin_unlock(&init_mm.page_table_lock);
	}

	return ret;
}

static int sync_direct_mapping_pud(unsigned long keyid,
		p4d_t *dst_p4d, p4d_t *src_p4d,
		unsigned long addr, unsigned long end)
{
	pud_t *src_pud, *dst_pud;
	pud_t *new_pud = NULL;
	bool remove_pud = false;
	unsigned long next;
	int ret = 0;

	/*
	 * We want to unmap and free the page table if the source is empty and
	 * the range covers whole page table.
	 */
	remove_pud = !src_p4d && IS_ALIGNED(addr, P4D_SIZE) && IS_ALIGNED(end, P4D_SIZE);

	/*
	 * P4D page got split into page table.
	 * Clear P4D mapping. Page table will be established instead.
	 */
	if (p4d_large(*dst_p4d)) {
		spin_lock(&init_mm.page_table_lock);
		p4d_clear(dst_p4d);
		spin_unlock(&init_mm.page_table_lock);
	}

	/* Allocate a new page table if needed. */
	if (p4d_none(*dst_p4d)) {
		new_pud = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
		if (!new_pud)
			return -ENOMEM;
		dst_pud = new_pud + pud_index(addr + keyid * direct_mapping_size);
	} else {
		dst_pud = pud_offset(dst_p4d, addr + keyid * direct_mapping_size);
	}
	src_pud = src_p4d ? pud_offset(src_p4d, addr) : NULL;

	do {
		pud_t *__src_pud = src_pud;

		next = pud_addr_end(addr, end);
		if (!__src_pud || pud_none(*__src_pud)) {
			if (pud_none(*dst_pud))
				goto next;
			if (pud_large(*dst_pud)) {
				spin_lock(&init_mm.page_table_lock);
				set_pud(dst_pud, __pud(0));
				spin_unlock(&init_mm.page_table_lock);
				goto next;
			}
			__src_pud = NULL;
		}

		if (__src_pud && pud_large(*__src_pud)) {
			pudval_t val;

			if (pud_large(*dst_pud)) {
				/*
				 * Sanity check: PFNs must match between source
				 * and destination even if the rest doesn't.
				 */
				BUG_ON(pud_pfn(*dst_pud) != pud_pfn(*__src_pud));
			} else if (!pud_none(*dst_pud)) {
				/*
				 * Page table is replaced with a pud page.
				 * Free and unmap the page table.
				 */
				__free_page(pud_page(*dst_pud));
				spin_lock(&init_mm.page_table_lock);
				pud_clear(dst_pud);
				spin_unlock(&init_mm.page_table_lock);
			}

			/* Copy entry, but set KeyID. */
			val = pud_val(*__src_pud) | keyid << mktme_keyid_shift();
			val &= __supported_pte_mask;
			spin_lock(&init_mm.page_table_lock);
			set_pud(dst_pud, __pud(val));
			spin_unlock(&init_mm.page_table_lock);
			goto next;
		}

		ret = sync_direct_mapping_pmd(keyid, dst_pud, __src_pud,
				addr, next);
next:
		addr = next;
		dst_pud++;
		if (src_pud)
			src_pud++;
	} while (addr != end && !ret);

	if (new_pud) {
		spin_lock(&init_mm.page_table_lock);
		p4d_populate(&init_mm, dst_p4d, new_pud);
		spin_unlock(&init_mm.page_table_lock);
	}

	if (remove_pud) {
		spin_lock(&init_mm.page_table_lock);
		__free_page(p4d_page(*dst_p4d));
		p4d_clear(dst_p4d);
		spin_unlock(&init_mm.page_table_lock);
	}

	return ret;
}

static int sync_direct_mapping_p4d(unsigned long keyid,
		pgd_t *dst_pgd, pgd_t *src_pgd,
		unsigned long addr, unsigned long end)
{
	p4d_t *src_p4d, *dst_p4d;
	p4d_t *new_p4d_1 = NULL, *new_p4d_2 = NULL;
	bool remove_p4d = false;
	unsigned long next;
	int ret = 0;

	/*
	 * We want to unmap and free the page table if the source is empty and
	 * the range covers whole page table.
	 */
	remove_p4d = !src_pgd && IS_ALIGNED(addr, PGDIR_SIZE) && IS_ALIGNED(end, PGDIR_SIZE);

	/* Allocate a new page table if needed. */
	if (pgd_none(*dst_pgd)) {
		new_p4d_1 = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
		if (!new_p4d_1)
			return -ENOMEM;
		dst_p4d = new_p4d_1 + p4d_index(addr + keyid * direct_mapping_size);
	} else {
		dst_p4d = p4d_offset(dst_pgd, addr + keyid * direct_mapping_size);
	}
	src_p4d = src_pgd ? p4d_offset(src_pgd, addr) : NULL;

	do {
		p4d_t *__src_p4d = src_p4d;

		next = p4d_addr_end(addr, end);
		if (!__src_p4d || p4d_none(*__src_p4d)) {
			if (p4d_none(*dst_p4d))
				goto next;
			__src_p4d = NULL;
		}

		ret = sync_direct_mapping_pud(keyid, dst_p4d, __src_p4d,
				addr, next);
next:
		addr = next;
		dst_p4d++;

		/*
		 * Direct mappings are 1TiB-aligned. With 5-level paging it
		 * means that on PGD level there can be misalignment between
		 * source and distiantion.
		 *
		 * Allocate the new page table if dst_p4d crosses page table
		 * boundary.
		 */
		if (!((unsigned long)dst_p4d & ~PAGE_MASK) && addr != end) {
			if (pgd_none(dst_pgd[1])) {
				new_p4d_2 = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
				if (!new_p4d_2)
					ret = -ENOMEM;
				dst_p4d = new_p4d_2;
			} else {
				dst_p4d = p4d_offset(dst_pgd + 1, 0);
			}
		}
		if (src_p4d)
			src_p4d++;
	} while (addr != end && !ret);

	if (new_p4d_1 || new_p4d_2) {
		spin_lock(&init_mm.page_table_lock);
		if (new_p4d_1)
			pgd_populate(&init_mm, dst_pgd, new_p4d_1);
		if (new_p4d_2)
			pgd_populate(&init_mm, dst_pgd + 1, new_p4d_2);
		spin_unlock(&init_mm.page_table_lock);
	}

	if (remove_p4d) {
		spin_lock(&init_mm.page_table_lock);
		__free_page(pgd_page(*dst_pgd));
		pgd_clear(dst_pgd);
		spin_unlock(&init_mm.page_table_lock);
	}

	return ret;
}

static int sync_direct_mapping_keyid(unsigned long keyid,
		unsigned long addr, unsigned long end)
{
	pgd_t *src_pgd, *dst_pgd;
	unsigned long next;
	int ret = 0;

	dst_pgd = pgd_offset_k(addr + keyid * direct_mapping_size);
	src_pgd = pgd_offset_k(addr);

	do {
		pgd_t *__src_pgd = src_pgd;

		next = pgd_addr_end(addr, end);
		if (pgd_none(*__src_pgd)) {
			if (pgd_none(*dst_pgd))
				continue;
			__src_pgd = NULL;
		}

		ret = sync_direct_mapping_p4d(keyid, dst_pgd, __src_pgd,
				addr, next);
	} while (dst_pgd++, src_pgd++, addr = next, addr != end && !ret);

	return ret;
}

/*
 * For MKTME we maintain per-KeyID direct mappings. This allows kernel to have
 * access to encrypted memory.
 *
 * sync_direct_mapping() sync per-KeyID direct mappings with a canonical
 * one -- KeyID-0.
 *
 * The function tracks changes in the canonical mapping:
 *  - creating or removing chunks of the translation tree;
 *  - changes in mapping flags (i.e. protection bits);
 *  - splitting huge page mapping into a page table;
 *  - replacing page table with a huge page mapping;
 *
 * The function need to be called on every change to the direct mapping:
 * hotplug, hotremove, changes in permissions bits, etc.
 *
 * The function is nop until MKTME is enabled.
 */
int sync_direct_mapping(unsigned long start, unsigned long end)
{
	int i, ret = 0;

	if (!mktme_enabled())
		return 0;

	for (i = 1; !ret && i <= mktme_nr_keyids(); i++)
		ret = sync_direct_mapping_keyid(i, start, end);

	flush_tlb_all();

	return ret;
}
