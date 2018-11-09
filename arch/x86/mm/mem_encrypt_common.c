#include <linux/mm.h>
#include <linux/mem_encrypt.h>
#include <linux/dma-mapping.h>
#include <asm/mktme.h>

/*
 * Encryption bits need to be set and cleared for both Intel MKTME and
 * AMD SME when converting between DMA address and physical address.
 */
dma_addr_t __mem_encrypt_dma_set(dma_addr_t daddr, phys_addr_t paddr)
{
	unsigned long keyid;

	if (sme_active())
		return __sme_set(daddr);
	keyid = page_keyid(pfn_to_page(__phys_to_pfn(paddr)));

	return (daddr & ~mktme_keyid_mask()) | (keyid << mktme_keyid_shift());
}

phys_addr_t __mem_encrypt_dma_clear(phys_addr_t paddr)
{
	if (sme_active())
		return __sme_clr(paddr);

	return paddr & ~mktme_keyid_mask();
}

/* Override for DMA direct allocation check - ARCH_HAS_FORCE_DMA_UNENCRYPTED */
bool force_dma_unencrypted(struct device *dev)
{
	u64 dma_enc_mask, dma_dev_mask;

	/*
	 * For SEV, all DMA must be to unencrypted addresses.
	 */
	if (sev_active())
		return true;

	/*
	 * For SME and MKTME, all DMA must be to unencrypted addresses if the
	 * device does not support DMA to addresses that include the encryption
	 * mask.
	 */
	if (!sme_active() && !mktme_enabled())
		return false;

	dma_enc_mask = sme_me_mask | mktme_keyid_mask();
	dma_dev_mask = min_not_zero(dev->coherent_dma_mask, dev->bus_dma_mask);

	return (dma_dev_mask & dma_enc_mask) != dma_enc_mask;
}
