// SPDX-License-Identifier: GPL-2.0
/*
 * Xen PV dom0 nested under Hyper-V.
 *
 * When Xen itself runs as a guest of Hyper-V (e.g. a nested VM on Azure), its
 * PV dom0 must drive the L0 host's synthetic (VMBus) devices for storage and
 * networking.  Xen exposes the Hyper-V enlightenments to dom0 and proxies the
 * hypercalls / synthetic interrupts, so here we detect that situation, bring up
 * the (otherwise Hyper-V-only) VMBus stack while remaining a Xen guest, and
 * route the relayed VMBus interrupt (delivered by Xen as VIRQ_HYPERV_VMBUS)
 * into the VMBus ISR.
 */
#include <linux/init.h>

#if IS_ENABLED(CONFIG_HYPERV)

#include <linux/cpuhotplug.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/printk.h>

#include <asm/cpuid/api.h>
#include <asm/mshyperv.h>
#include <asm/xen/page.h>

#include <xen/xen.h>
#include <xen/events.h>
#include <xen/interface/xen.h>

/*
 * Translate a guest pseudo-physical frame / address to the machine
 * (L1-physical) frame the underlying Hyper-V host must use.  For a Xen PV dom0
 * the two differ, so every host-facing VMBus / SynIC / hypercall address must
 * be run through these.
 */
unsigned long hv_nested_hostpfn(unsigned long pfn)
{
	return hyperv_nested_on_xen ? pfn_to_mfn(pfn) : pfn;
}
EXPORT_SYMBOL_GPL(hv_nested_hostpfn);

u64 hv_nested_hostpa(void *va)
{
	unsigned long pfn = virt_to_pfn(va);

	if (hyperv_nested_on_xen)
		pfn = pfn_to_mfn(pfn);

	return ((u64)pfn << PAGE_SHIFT) | offset_in_page(va);
}
EXPORT_SYMBOL_GPL(hv_nested_hostpa);

static bool __init xen_detect_hyperv_l0(void)
{
	u32 eax, ebx, ecx, edx;

	if (!xen_pv_domain())
		return false;

	/*
	 * Xen presents the underlying host's "Microsoft Hv" leaves at
	 * 0x40000000 (its own XenVMMXenVMM leaves are relocated to 0x40000100).
	 */
	cpuid(HYPERV_CPUID_VENDOR_AND_MAX_FUNCTIONS, &eax, &ebx, &ecx, &edx);

	return ebx == 0x7263694d &&	/* "Micr" */
	       ecx == 0x666f736f &&	/* "osof" */
	       edx == 0x76482074;	/* "t Hv" */
}

static int __init xen_hyperv_init(void)
{
	if (!xen_detect_hyperv_l0())
		return 0;

	pr_info("Xen: nested under Hyper-V, enabling VMBus passthrough\n");
	hyperv_nested_on_xen = true;
	hyperv_init_nested_on_xen();

	return 0;
}
arch_initcall(xen_hyperv_init);

/*
 * VMBus interrupt bridge.  A PV dom0 receives interrupts as Xen event channels,
 * not as the native SINT vector VMBus programs, so Xen relays each VMBus
 * message via the per-vcpu VIRQ_HYPERV_VMBUS.  Bind it on every cpu and drive
 * the VMBus ISR from its handler.
 */
static void (*xen_vmbus_handler)(void);
static DEFINE_PER_CPU(int, xen_vmbus_irq);

static irqreturn_t xen_hyperv_vmbus_isr(int irq, void *dev_id)
{
	if (xen_vmbus_handler)
		xen_vmbus_handler();

	return IRQ_HANDLED;
}

static int xen_hyperv_vmbus_cpu_up(unsigned int cpu)
{
	int irq;

	irq = bind_virq_to_irqhandler(VIRQ_HYPERV_VMBUS, cpu,
				      xen_hyperv_vmbus_isr,
				      IRQF_PERCPU | IRQF_NOBALANCING,
				      "hyperv-vmbus", NULL);
	if (irq < 0)
		return irq;

	per_cpu(xen_vmbus_irq, cpu) = irq;

	return 0;
}

static int xen_hyperv_vmbus_cpu_down(unsigned int cpu)
{
	int irq = per_cpu(xen_vmbus_irq, cpu);

	if (irq > 0) {
		unbind_from_irqhandler(irq, NULL);
		per_cpu(xen_vmbus_irq, cpu) = 0;
	}

	return 0;
}

int hyperv_setup_xen_vmbus_irq(void (*isr)(void))
{
	int ret;

	if (!hyperv_nested_on_xen)
		return -ENODEV;

	xen_vmbus_handler = isr;

	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "xen/hyperv-vmbus:online",
				xen_hyperv_vmbus_cpu_up, xen_hyperv_vmbus_cpu_down);

	return ret < 0 ? ret : 0;
}

#endif /* CONFIG_HYPERV */
