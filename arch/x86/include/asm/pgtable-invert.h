/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_PGTABLE_INVERT_H
#define _ASM_PGTABLE_INVERT_H 1

#ifndef __ASSEMBLER__

#include <asm/pgt-rules.h>

/* The L1TF rules are in <asm/pgt-rules.h> */
static inline bool __pte_needs_invert(u64 val)
{
	return pgt_x86_pfn_inverted(val);
}

static inline u64 protnone_mask(u64 val)
{
	return pgt_x86_invert_mask(val);
}

static inline u64 flip_protnone_guard(u64 oldval, u64 val, u64 mask)
{
	return pgt_x86_reinvert(oldval, val, mask);
}

#endif /* __ASSEMBLER__ */

#endif
