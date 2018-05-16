#include <linux/linkage.h>
#include <linux/kernel.h>

#include <asm/processor.h>
#include <asm/cpu.h>
#include <asm/cpu_device_id.h>
#include <asm/intel-family.h>
#include <asm/fpu/internal.h>

#include "cpu.h"

static const __initconst struct x86_cpu_id cpu_no_speculation[] = {
	{ X86_VENDOR_INTEL,	6, INTEL_FAM6_ATOM_CEDARVIEW,	X86_FEATURE_ANY },
	{ X86_VENDOR_INTEL,	6, INTEL_FAM6_ATOM_CLOVERVIEW,	X86_FEATURE_ANY },
	{ X86_VENDOR_INTEL,	6, INTEL_FAM6_ATOM_LINCROFT,	X86_FEATURE_ANY },
	{ X86_VENDOR_INTEL,	6, INTEL_FAM6_ATOM_PENWELL,	X86_FEATURE_ANY },
	{ X86_VENDOR_INTEL,	6, INTEL_FAM6_ATOM_PINEVIEW,	X86_FEATURE_ANY },
	{ X86_VENDOR_CENTAUR,	5 },
	{ X86_VENDOR_INTEL,	5 },
	{ X86_VENDOR_NSC,	5 },
	{ X86_VENDOR_ANY,	4 },
	{}
};

static const __initconst struct x86_cpu_id cpu_no_meltdown[] = {
	{ X86_VENDOR_AMD },
	{}
};

static bool __init cpu_vulnerable_to_meltdown(struct cpuinfo_x86 *c)
{
	u64 ia32_cap = 0;

	if (x86_match_cpu(cpu_no_meltdown))
		return false;

	if (cpu_has(c, X86_FEATURE_ARCH_CAPABILITIES))
		rdmsrl(MSR_IA32_ARCH_CAPABILITIES, ia32_cap);

	/* Rogue Data Cache Load? No! */
	if (ia32_cap & ARCH_CAP_RDCL_NO)
		return false;

	return true;
}

/*
 * Do minimum CPU detection early.
 * Fields really needed: vendor, cpuid_level, family, model, mask,
 * cache alignment.
 * The others are not touched to avoid unwanted side effects.
 *
 * WARNING: this function is only called on the boot CPU.  Don't add code
 * here that is supposed to run on all CPUs.
 */
static void __init early_identify_cpu(struct cpuinfo_x86 *c)
{
#ifdef CONFIG_X86_64
	c->x86_clflush_size = 64;
	c->x86_phys_bits = 36;
	c->x86_virt_bits = 48;
#else
	c->x86_clflush_size = 32;
	c->x86_phys_bits = 32;
	c->x86_virt_bits = 32;
#endif
	c->x86_cache_alignment = c->x86_clflush_size;

	memset(&c->x86_capability, 0, sizeof c->x86_capability);
	c->extended_cpuid_level = 0;

	/* cyrix could have cpuid enabled via c_identify()*/
	if (have_cpuid_p()) {
		cpu_detect(c);
		get_cpu_vendor(c);
		get_cpu_cap(c);
		get_cpu_address_sizes(c);
		setup_force_cpu_cap(X86_FEATURE_CPUID);

		if (this_cpu_dev->c_early_init)
			this_cpu_dev->c_early_init(c);

		c->cpu_index = 0;
		filter_cpuid_features(c, false);

		if (this_cpu_dev->c_bsp_init)
			this_cpu_dev->c_bsp_init(c);
	} else {
		identify_cpu_without_cpuid(c);
		setup_clear_cpu_cap(X86_FEATURE_CPUID);
	}

	setup_force_cpu_cap(X86_FEATURE_ALWAYS);

	if (!x86_match_cpu(cpu_no_speculation)) {
		if (cpu_vulnerable_to_meltdown(c))
			setup_force_cpu_bug(X86_BUG_CPU_MELTDOWN);
		setup_force_cpu_bug(X86_BUG_SPECTRE_V1);
		setup_force_cpu_bug(X86_BUG_SPECTRE_V2);
	}

	fpu__init_system(c);

#ifdef CONFIG_X86_32
	/*
	 * Regardless of whether PCID is enumerated, the SDM says
	 * that it can't be enabled in 32-bit mode.
	 */
	setup_clear_cpu_cap(X86_FEATURE_PCID);
#endif

	/*
	 * Later in the boot process pgtable_l5_enabled() relies on
	 * cpu_feature_enabled(X86_FEATURE_LA57). If 5-level paging is not
	 * enabled by this point we need to clear the feature bit to avoid
	 * false-positives at the later stage.
	 *
	 * pgtable_l5_enabled() can be false here for several reasons:
	 *  - 5-level paging is disabled compile-time;
	 *  - it's 32-bit kernel;
	 *  - machine doesn't support 5-level paging;
	 *  - user specified 'no5lvl' in kernel command line.
	 */
	if (!pgtable_l5_enabled())
		setup_clear_cpu_cap(X86_FEATURE_LA57);
}

void __init early_cpu_init(void)
{
	const struct cpu_dev *const *cdev;
	int count = 0;

#ifdef CONFIG_PROCESSOR_SELECT
	pr_info("KERNEL supported cpus:\n");
#endif

	for (cdev = __x86_cpu_dev_start; cdev < __x86_cpu_dev_end; cdev++) {
		const struct cpu_dev *cpudev = *cdev;

		if (count >= X86_VENDOR_NUM)
			break;
		cpu_devs[count] = cpudev;
		count++;

#ifdef CONFIG_PROCESSOR_SELECT
		{
			unsigned int j;

			for (j = 0; j < 2; j++) {
				if (!cpudev->c_ident[j])
					continue;
				pr_info("  %s %s\n", cpudev->c_vendor,
					cpudev->c_ident[j]);
			}
		}
#endif
	}
	early_identify_cpu(&boot_cpu_data);
}
