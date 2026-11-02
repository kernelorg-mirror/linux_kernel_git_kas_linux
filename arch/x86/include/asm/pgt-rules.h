/* SPDX-License-Identifier: GPL-2.0 */
/*
 * x86 page table entry rules that depend on more than one bit, on a raw
 * entry value of any level. The pgt provider, <asm/pgt.h>, and the
 * per-level helpers of <asm/pgtable.h> both build on them.
 */
#ifndef _ASM_X86_PGT_RULES_H
#define _ASM_X86_PGT_RULES_H

#ifndef __ASSEMBLER__

#include <asm/pgtable_types.h>
#include <asm/pkru.h>

/*
 * L1TF: a non-present, non-zero entry stores its PFN inverted, so that a
 * speculative walk does not reach the memory it names. A cleared entry is
 * not inverted. Even a protection-only value is never zero, since PAGE_NONE
 * has _PAGE_PROTNONE | _PAGE_ACCESSED set, so zero really means cleared.
 */
static inline bool pgt_x86_pfn_inverted(u64 val)
{
	return val && !(val & _PAGE_PRESENT);
}

/* The mask to XOR an entry with to read or write its PFN field */
static inline u64 pgt_x86_invert_mask(u64 val)
{
	return pgt_x86_pfn_inverted(val) ? ~0ull : 0;
}

/* Flip the PFN field in @mask when a change moves an entry in or out of inversion */
static inline u64 pgt_x86_reinvert(u64 oldval, u64 val, u64 mask)
{
	if (pgt_x86_pfn_inverted(oldval) != pgt_x86_pfn_inverted(val))
		val = (val & ~mask) | (~val & mask);
	return val;
}

/*
 * Shadow stack: Write=0, Dirty=1 marks a shadow stack entry. Making a dirty
 * entry read-only would create one by accident, so the dirty state moves to
 * the software bit _PAGE_SAVED_DIRTY while the entry is read-only, and back
 * when it becomes writable.
 */
static inline u64 pgt_x86_mksaveddirty(u64 v)
{
	u64 cond = (~v >> _PAGE_BIT_RW) & 1;

	v |= ((v >> _PAGE_BIT_DIRTY) & cond) << _PAGE_BIT_SAVED_DIRTY;
	v &= ~(cond << _PAGE_BIT_DIRTY);
	return v;
}

static inline u64 pgt_x86_clear_saveddirty(u64 v)
{
	u64 cond = (v >> _PAGE_BIT_RW) & 1;

	v |= ((v >> _PAGE_BIT_SAVED_DIRTY) & cond) << _PAGE_BIT_DIRTY;
	v &= ~(cond << _PAGE_BIT_SAVED_DIRTY);
	return v;
}

/* Protection keys: the key of a leaf, and whether PKRU allows the access */
static inline u16 pgt_x86_pkey(u64 val)
{
#ifdef CONFIG_X86_INTEL_MEMORY_PROTECTION_KEYS
	/* ifdef to avoid doing 59-bit shift on 32-bit values */
	return (val & _PAGE_PKEY_MASK) >> _PAGE_BIT_PKEY_BIT0;
#else
	return 0;
#endif
}

static inline bool pgt_x86_pkru_allows(u16 pkey, bool write)
{
	u32 pkru = read_pkru();

	if (!__pkru_allows_read(pkru, pkey))
		return false;
	if (write && !__pkru_allows_write(pkru, pkey))
		return false;
	return true;
}

/*
 * Whether the hardware allows a user access through a leaf: present, user
 * and, for a write, writable. A shadow stack entry has Write=0 and is
 * refused for a write as well.
 */
static inline bool pgt_x86_access_permitted(u64 val, bool write)
{
	u64 need = _PAGE_USER;

	/* On its own, so that the compiler knows the PFN is not inverted */
	if (!(val & _PAGE_PRESENT))
		return false;
	if (write)
		need |= _PAGE_RW;
	if ((val & need) != need)
		return false;
	return pgt_x86_pkru_allows(pgt_x86_pkey(val), write);
}

/*
 * Page table isolation: a store of a top-level entry also updates the user
 * copy of the top-level table. Returns the value to store in the kernel
 * copy.
 */
#ifdef CONFIG_MITIGATION_PAGE_TABLE_ISOLATION
u64 __pgt_x86_pti_set_user(void *ptr, u64 val);

static inline u64 pgt_x86_pti_set_user(void *ptr, u64 val)
{
	if (!cpu_feature_enabled(X86_FEATURE_PTI))
		return val;
	return __pgt_x86_pti_set_user(ptr, val);
}
#else
static inline u64 pgt_x86_pti_set_user(void *ptr, u64 val)
{
	return val;
}
#endif

#endif /* __ASSEMBLER__ */

#endif /* _ASM_X86_PGT_RULES_H */
