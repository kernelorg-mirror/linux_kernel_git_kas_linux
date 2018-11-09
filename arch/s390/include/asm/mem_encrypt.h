/* SPDX-License-Identifier: GPL-2.0 */
#ifndef S390_MEM_ENCRYPT_H__
#define S390_MEM_ENCRYPT_H__

#ifndef __ASSEMBLY__

#define sme_me_mask	0ULL

static inline bool sme_active(void) { return false; }
extern bool sev_active(void);

int set_memory_encrypted(unsigned long addr, int numpages);
int set_memory_decrypted(unsigned long addr, int numpages);

static inline dma_addr_t __mem_encrypt_dma_set(dma_addr_t daddr, phys_addr_t paddr)
{
	return daddr;
}

static inline phys_addr_t __mem_encrypt_dma_clear(phys_addr_t paddr)
{
	return paddr;
}

#endif	/* __ASSEMBLY__ */

#endif	/* S390_MEM_ENCRYPT_H__ */
