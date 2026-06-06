// SPDX-License-Identifier: GPL-2.0
/*
 * arm64 SDEI-based cross-CPU NMI service.
 *
 * Delivering an "NMI-shaped" event to an EL1 context that has locally
 * masked interrupts, on silicon without FEAT_NMI, can be done two ways:
 *
 *   - pseudo-NMI: mask "interrupts" via the GIC priority register
 *     (ICC_PMR_EL1) instead of PSTATE.DAIF, leaving a high-priority band
 *     deliverable. Functionally this works -- but it reimplements every
 *     local_irq_disable()/enable() and exception entry/exit as a PMR
 *     write plus synchronisation, a cost paid on that hot path forever,
 *     whether or not an NMI is ever delivered.
 *
 *   - SDEI: leave interrupt masking as the cheap PSTATE.DAIF operation
 *     and have the firmware bounce an EL3-routed Group-0 SGI back to
 *     NS-EL1 as an event callback. The cost is a firmware round-trip,
 *     but only at the rare moment delivery is actually needed.
 *
 * This driver takes the second path: it keeps the IRQ-mask hot path
 * free and pays only when it fires, which is what makes cross-CPU NMI
 * affordable on hardware where the pseudo-NMI tax isn't, until FEAT_NMI
 * makes NMI masking cheap in the architecture itself.
 *
 * Capabilities provided:
 *
 *   - sdei_nmi_trigger_cpumask_backtrace() — override for arm64's
 *     arch_trigger_cpumask_backtrace(), so sysrq-l, RCU stall dumps,
 *     hardlockup_all_cpu_backtrace, soft-lockup/hung-task secondary
 *     dumps all reach interrupt-masked CPUs.
 *
 *   - sdei_nmi_stop_cpus() — the last rung of smp_send_stop()'s
 *     escalation (reboot/halt and the panic/kdump crash stop alike),
 *     reaching CPUs that ignored the stop IPIs; on the kdump path the
 *     wedged context is captured into the vmcore before the CPU parks.
 *
 * Delivery uses the standard SDEI software-signalled event (event 0) and
 * SDEI_EVENT_SIGNAL. We register a handler for event 0, enable it, and
 * poke a target CPU with sdei_event_signal(0, mpidr): firmware makes
 * event 0 pending on that PE and dispatches the handler NMI-like,
 * regardless of the target's DAIF.
 * Availability is simply whether event 0 registers and enables -- if SDEI
 * and its software-signalled event are present we use it, otherwise the
 * driver stays inert.
 */

#define pr_fmt(fmt) "sdei_nmi: " fmt

#include <linux/arm_sdei.h>
#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/nmi.h>
#include <linux/printk.h>
#include <linux/ptrace.h>
#include <linux/smp.h>
#include <linux/types.h>

#include <asm/nmi.h>
#include <asm/smp_plat.h>

static bool sdei_nmi_available;

#define SDEI_NMI_EVENT			0

/*
 * Stop-request dispatch lives on the same SDEI event 0 as everything
 * else. The requesting CPU sets each target's bit in sdei_nmi_stop_mask
 * before signalling event 0; the target's handler test-and-clears its
 * bit and hands the CPU to arm64_nmi_cpu_stop(), which saves crash
 * state when the stop is a kdump crash-stop, marks the CPU offline
 * (which is what the requester polls for) and parks it.
 *
 * This mirrors the cpumask the framework's nmi_cpu_backtrace() consults
 * just below, and a shared mask rather than a separate SDEI event avoids
 * extra registrations from firmware.
 */
static cpumask_t sdei_nmi_stop_mask;

static int sdei_nmi_handler(u32 event, struct pt_regs *regs, void *arg)
{
	int cpu = smp_processor_id();

	if (cpumask_test_and_clear_cpu(cpu, &sdei_nmi_stop_mask)) {
		/*
		 * Saves crash state when this is a kdump crash stop, marks
		 * the CPU offline (the requester's ack), masks this PE and
		 * flags the CPU for parking. The park itself happens on the
		 * SDEI exit path, *after* firmware completed this event:
		 * returning to the interrupted (wedged) context is not an
		 * option, so do_sdei_event() redirects the completion via
		 * SDEI_EVENT_COMPLETE_AND_RESUME into a stub that powers the
		 * CPU off. Completing the event first is what makes CPU_OFF
		 * legal (it can't be issued mid-event), and powering off lets
		 * an SMP capture kernel reclaim the CPU with CPU_ON.
		 */
		arm64_nmi_cpu_stop(regs);
		return SDEI_EV_HANDLED;
	}

	/*
	 * nmi_cpu_backtrace() no-ops unless this CPU's bit is set in the
	 * global backtrace mask (driven by nmi_trigger_cpumask_backtrace()),
	 * so a fire that reaches a CPU not being backtraced is harmless.
	 */
	nmi_cpu_backtrace(regs);
	return SDEI_EV_HANDLED;
}
NOKPROBE_SYMBOL(sdei_nmi_handler);

static void sdei_nmi_fire(unsigned int target_cpu)
{
	int err = sdei_event_signal(SDEI_NMI_EVENT, cpu_logical_map(target_cpu));

	if (err)
		pr_warn("SDEI_EVENT_SIGNAL to CPU %u failed: %d\n",
			target_cpu, err);
}

/*
 * Raise callback for nmi_trigger_cpumask_backtrace(): signal event 0
 * at every CPU still pending in @mask. The framework excludes the local
 * CPU from @mask before calling us.
 */
static void sdei_nmi_raise_backtrace(cpumask_t *mask)
{
	unsigned int cpu;

	for_each_cpu(cpu, mask)
		sdei_nmi_fire(cpu);
}

/*
 * Override hook for arch_trigger_cpumask_backtrace() (see
 * arch/arm64/kernel/smp.c). Returns true when SDEI handled the request,
 * which is the case whenever SDEI is active; on a false return the arch
 * falls back to its regular-IRQ (or pseudo-NMI, if enabled) IPI.
 *
 * On a kernel built without paying the pseudo-NMI hot-path cost (the
 * usual case for this driver's target), the IPI can't reach a CPU that
 * has interrupts masked -- so the backtrace of the one CPU you care
 * about comes back empty. SDEI is dispatched out of EL3 and lands
 * regardless of the target's DAIF, without taxing the IRQ-mask path.
 */
bool sdei_nmi_trigger_cpumask_backtrace(const cpumask_t *mask, int exclude_cpu)
{
	if (!sdei_nmi_available)
		return false;

	nmi_trigger_cpumask_backtrace(mask, exclude_cpu,
				      sdei_nmi_raise_backtrace);
	return true;
}

/*
 * Last rung of the stop escalation in smp_send_stop() (see
 * arch/arm64/kernel/smp.c). The caller runs the regular stop IPI (and
 * the pseudo-NMI stop IPI, where available) first; @mask holds whatever
 * stayed online through those -- typically CPUs wedged with interrupts
 * masked, unreachable by an IPI. Set each target's stop-request flag and
 * signal event 0 at it; a target acks by marking itself offline, which
 * the caller polls for.
 *
 * Returns false when SDEI isn't active, so the caller can skip the wait.
 */
bool sdei_nmi_stop_cpus(const cpumask_t *mask)
{
	unsigned int cpu;

	if (!sdei_nmi_available)
		return false;

	cpumask_or(&sdei_nmi_stop_mask, &sdei_nmi_stop_mask, mask);

	/* Publish the mask before the SMCs read it on the target side. */
	smp_wmb();

	for_each_cpu(cpu, mask)
		sdei_nmi_fire(cpu);

	return true;
}

/*
 * device_initcall (after arch_initcall(sdei_init), so the SDEI subsystem
 * is up): probe the firmware, register the event, and turn on the
 * cross-CPU service. If the probe fails the driver stays inert and the
 * override hooks decline, leaving the arch's own paths in place.
 */
static int __init sdei_nmi_init(void)
{
	int err;

	err = sdei_event_register(SDEI_NMI_EVENT, sdei_nmi_handler, NULL);
	if (err) {
		pr_err("sdei_event_register(%u) failed: %d\n",
		       SDEI_NMI_EVENT, err);
		return 0;
	}

	err = sdei_event_enable(SDEI_NMI_EVENT);
	if (err) {
		pr_err("sdei_event_enable(%u) failed: %d\n",
		       SDEI_NMI_EVENT, err);
		sdei_event_unregister(SDEI_NMI_EVENT);
		return 0;
	}

	sdei_nmi_available = true;
	pr_info("using SDEI cross-CPU NMI (SDEI_EVENT_SIGNAL, event %u)\n",
		SDEI_NMI_EVENT);

	return 0;
}
device_initcall(sdei_nmi_init);
