// SPDX-License-Identifier: GPL-2.0
/*
 * Native x86-64 provider of the pgt architecture contract: the out-of-line
 * parts. See <asm/pgt.h>.
 */
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/pgt.h>
#include <asm/runtime-const.h>
#include <asm/setup.h>

/*
 * The boot-time values <asm/pgt.h> reads as patched constants: the memory
 * encryption setup (sme_enable(), tdx_early_init(), hv_vtom_init()) and
 * probe_page_size_mask() have settled them once init_mem_mapping() has run.
 * Nothing walks a user page table before that.
 */
void __init pgt_x86_init(void)
{
	runtime_const_init(ptr, __supported_pte_mask);
#ifdef CONFIG_DYNAMIC_PHYSICAL_MASK
	runtime_const_init(ptr, physical_mask);
#endif
#ifdef CONFIG_AMD_MEM_ENCRYPT
	runtime_const_init(ptr, sme_me_mask);
#endif
}

#ifndef CONFIG_PGT_FALLBACK
pgtval_t pgt_x86_mkwrite_vma(pgtval_t val, struct vm_area_struct *vma)
{
	if (vma->vm_flags & VM_SHADOW_STACK)
		return (val & ~_PAGE_RW) | _PAGE_DIRTY;
	return pgt_x86_clear_saveddirty(val | _PAGE_RW);
}

/* Xen PV pins a table before it is linked, with an operation per level */
static void pgt_x86_pv_alloc(struct mm_struct *mm, struct ptdesc *table,
			     unsigned int lvl)
{
#ifdef CONFIG_PARAVIRT_XXL
	unsigned long pfn = page_to_pfn(ptdesc_page(table));

	switch (lvl) {
	case PGT_X86_PD:
		paravirt_alloc_pte(mm, pfn);
		break;
	case PGT_X86_PDPT:
		paravirt_alloc_pmd(mm, pfn);
		break;
	case PGT_X86_PML4:
		paravirt_alloc_pud(mm, pfn);
		break;
	default:
		paravirt_alloc_p4d(mm, pfn);
		break;
	}
#endif
}

void pgt_arch_populate(struct mm_struct *mm, void *ptr, struct ptdesc *table,
		       unsigned int lvl)
{
	pgtv_t v = pgt_arch_table_entry(table, lvl);

	pgt_x86_pv_alloc(mm, table, lvl);
	__pgt_x86_set(ptr, v.val, lvl);
}

#ifdef CONFIG_DEBUG_VM
static int __init pgt_x86_check_runtime(void)
{
	WARN_ON(__pgt_x86_supported() != __supported_pte_mask);
	WARN_ON(__pgt_x86_physical_mask() != __PHYSICAL_MASK);
	WARN_ON(__pgt_x86_enc() != sme_me_mask);
	return 0;
}
late_initcall(pgt_x86_check_runtime);
#endif
#endif /* !CONFIG_PGT_FALLBACK */
