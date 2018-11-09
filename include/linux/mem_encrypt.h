/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * AMD Memory Encryption Support
 *
 * Copyright (C) 2016 Advanced Micro Devices, Inc.
 *
 * Author: Tom Lendacky <thomas.lendacky@amd.com>
 */

#ifndef __MEM_ENCRYPT_H__
#define __MEM_ENCRYPT_H__

#ifndef __ASSEMBLY__

#ifdef CONFIG_ARCH_HAS_MEM_ENCRYPT

#include <asm/mem_encrypt.h>

#else	/* !CONFIG_ARCH_HAS_MEM_ENCRYPT */

#define sme_me_mask	0ULL

static inline bool sme_active(void) { return false; }
static inline bool sev_active(void) { return false; }

static inline dma_addr_t __mem_encrypt_dma_set(dma_addr_t daddr, phys_addr_t paddr)
{
	return daddr;
}

static inline phys_addr_t __mem_encrypt_dma_clear(phys_addr_t paddr)
{
	return paddr;
}

#endif	/* CONFIG_ARCH_HAS_MEM_ENCRYPT */

static inline bool mem_encrypt_active(void)
{
	return sme_me_mask;
}

static inline u64 sme_get_me_mask(void)
{
	return sme_me_mask;
}

#endif	/* __ASSEMBLY__ */

#endif	/* __MEM_ENCRYPT_H__ */
