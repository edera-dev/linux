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
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/rculist.h>
#include <linux/slab.h>

#include <asm/cpuid/api.h>
#include <asm/mshyperv.h>
#include <asm/xen/hypercall.h>
#include <asm/xen/page.h>

#include <xen/xen.h>
#include <xen/events.h>
#include <xen/interface/physdev.h>
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

/*
 * vPCI (VMBus-assigned PCI device, e.g. NVMe or a MANA VF) interrupt bridge.
 *
 * The host composes these MSIs itself and delivers them to the VP, which under
 * Xen belongs to Xen and not to us: the vPCI protocol messages that request them
 * travel in a VMBus ring buffer Xen does not inspect, so it cannot rewrite the
 * vector we ask for.  Xen therefore owns one vector for all of them, tells us
 * which (PHYSDEVOP_hyperv_vpci_vector), and relays each delivery to the vcpu
 * that received it as VIRQ_HYPERV_VPCI.  We compose every vPCI MSI with that
 * vector and demultiplex here, since Xen cannot tell the devices apart.
 *
 * That also means x86_vector_domain cannot be the parent of the vPCI MSI domain:
 * its vectors are meaningless to a PV guest (and acking one reaches
 * xen_apic_eoi(), which warns).  We supply a minimal parent domain instead - the
 * host owns masking and affinity, and Xen has already EOId the vector by the
 * time we run.
 */
static u32 xen_hv_vpci_vector;
static struct irq_domain *xen_hv_vpci_domain;
static DEFINE_PER_CPU(int, xen_vpci_irq);

/* The vPCI interrupts to consider on each relayed VIRQ. */
struct xen_hv_vpci_irq {
	struct list_head node;
	struct rcu_head rcu;
	unsigned int irq;
};

static LIST_HEAD(xen_hv_vpci_irqs);
static DEFINE_SPINLOCK(xen_hv_vpci_irqs_lock);

static int xen_hv_vpci_irq_add(unsigned int irq)
{
	struct xen_hv_vpci_irq *entry;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->irq = irq;

	spin_lock(&xen_hv_vpci_irqs_lock);
	list_add_rcu(&entry->node, &xen_hv_vpci_irqs);
	spin_unlock(&xen_hv_vpci_irqs_lock);

	return 0;
}

static void xen_hv_vpci_irq_del(unsigned int irq)
{
	struct xen_hv_vpci_irq *entry;

	spin_lock(&xen_hv_vpci_irqs_lock);
	list_for_each_entry(entry, &xen_hv_vpci_irqs, node) {
		if (entry->irq == irq) {
			list_del_rcu(&entry->node);
			kfree_rcu(entry, rcu);
			break;
		}
	}
	spin_unlock(&xen_hv_vpci_irqs_lock);
}

static irqreturn_t xen_hyperv_vpci_isr(int irq, void *dev_id)
{
	struct xen_hv_vpci_irq *entry;

	/*
	 * One or more of these devices interrupted; which one is not
	 * recoverable from the relay, so run them all.  Device handlers are
	 * required to tolerate a spurious call.
	 */
	rcu_read_lock();
	list_for_each_entry_rcu(entry, &xen_hv_vpci_irqs, node)
		generic_handle_irq(entry->irq);
	rcu_read_unlock();

	return IRQ_HANDLED;
}

static int xen_hyperv_vpci_cpu_up(unsigned int cpu)
{
	int irq;

	irq = bind_virq_to_irqhandler(VIRQ_HYPERV_VPCI, cpu,
				      xen_hyperv_vpci_isr,
				      IRQF_PERCPU | IRQF_NOBALANCING,
				      "hyperv-vpci", NULL);
	if (irq < 0)
		return irq;

	per_cpu(xen_vpci_irq, cpu) = irq;

	return 0;
}

static int xen_hyperv_vpci_cpu_down(unsigned int cpu)
{
	int irq = per_cpu(xen_vpci_irq, cpu);

	if (irq > 0) {
		unbind_from_irqhandler(irq, NULL);
		per_cpu(xen_vpci_irq, cpu) = 0;
	}

	return 0;
}

/*
 * Xen acknowledged the host's vector before relaying, so there is nothing to ack
 * here beyond completing a pending affinity change - the same thing
 * apic_ack_edge() does for a native MSI.
 */
static void xen_hv_vpci_irq_ack(struct irq_data *data)
{
	irq_move_irq(data);
}

static void xen_hv_vpci_irq_noop(struct irq_data *data)
{
}

/*
 * Record where the interrupt should land; the vPCI irq_chip above us turns this
 * into a HVCALL_RETARGET_INTERRUPT (proxied by Xen) when it unmasks, which is
 * what actually moves it.
 */
static int xen_hv_vpci_set_affinity(struct irq_data *data,
				    const struct cpumask *mask, bool force)
{
	unsigned int cpu = cpumask_first_and(mask, cpu_online_mask);

	if (cpu >= nr_cpu_ids)
		return -EINVAL;

	irq_data_update_effective_affinity(data, cpumask_of(cpu));

	return IRQ_SET_MASK_OK;
}

static struct irq_chip xen_hv_vpci_irq_chip = {
	.name			= "XEN-HV-vPCI",
	.irq_ack		= xen_hv_vpci_irq_ack,
	.irq_eoi		= xen_hv_vpci_irq_noop,
	.irq_mask		= xen_hv_vpci_irq_noop,
	.irq_unmask		= xen_hv_vpci_irq_noop,
	.irq_set_affinity	= xen_hv_vpci_set_affinity,
};

static int xen_hv_vpci_domain_alloc(struct irq_domain *domain, unsigned int virq,
				    unsigned int nr_irqs, void *arg)
{
	unsigned int i;
	int ret;

	for (i = 0; i < nr_irqs; i++) {
		ret = xen_hv_vpci_irq_add(virq + i);
		if (ret)
			goto err;

		irq_domain_set_info(domain, virq + i, virq + i,
				    &xen_hv_vpci_irq_chip, NULL,
				    handle_edge_irq, NULL, NULL);

		/*
		 * Every handler runs on every relayed interrupt, so most calls
		 * return IRQ_NONE by design.  Tell the spurious-interrupt
		 * detector this line is polled, or it would eventually decide a
		 * busy device's interrupt is stuck and disable it.
		 */
		irq_set_status_flags(virq + i, IRQ_IS_POLLED);

		/*
		 * The host has to be given a VP to deliver to, and
		 * hv_compose_msi_msg() takes it from the effective affinity, so
		 * that must never be empty.
		 */
		irq_data_update_effective_affinity(irq_domain_get_irq_data(domain,
									  virq + i),
						   cpumask_of(0));
	}

	return 0;

err:
	while (i--)
		xen_hv_vpci_irq_del(virq + i);

	return ret;
}

static void xen_hv_vpci_domain_free(struct irq_domain *domain, unsigned int virq,
				    unsigned int nr_irqs)
{
	unsigned int i;

	for (i = 0; i < nr_irqs; i++) {
		xen_hv_vpci_irq_del(virq + i);
		irq_domain_reset_irq_data(irq_domain_get_irq_data(domain, virq + i));
	}
}

static const struct irq_domain_ops xen_hv_vpci_domain_ops = {
	.alloc	= xen_hv_vpci_domain_alloc,
	.free	= xen_hv_vpci_domain_free,
};

/* The vector every vPCI MSI must be composed with. */
unsigned int hyperv_xen_vpci_vector(void)
{
	return xen_hv_vpci_vector;
}
EXPORT_SYMBOL_GPL(hyperv_xen_vpci_vector);

/*
 * Parent irq domain for vPCI MSIs, brought up on first use.  Returns NULL if Xen
 * is not relaying vPCI interrupts, which leaves the vPCI bus unusable - the
 * devices on it cannot deliver an interrupt any other way.
 */
struct irq_domain *hyperv_xen_vpci_root_domain(void)
{
	static DEFINE_MUTEX(setup_lock);
	struct physdev_hyperv_vpci_vector out = {};
	struct fwnode_handle *fn;
	int ret;

	if (!hyperv_nested_on_xen)
		return NULL;

	guard(mutex)(&setup_lock);

	if (xen_hv_vpci_domain)
		return xen_hv_vpci_domain;

	ret = HYPERVISOR_physdev_op(PHYSDEVOP_hyperv_vpci_vector, &out);
	if (ret) {
		pr_err("Xen: no Hyper-V vPCI interrupt vector: %d\n", ret);
		return NULL;
	}
	xen_hv_vpci_vector = out.vector;

	fn = irq_domain_alloc_named_fwnode("XEN-HV-vPCI");
	if (!fn)
		return NULL;

	xen_hv_vpci_domain = irq_domain_create_tree(fn, &xen_hv_vpci_domain_ops,
						    NULL);
	if (!xen_hv_vpci_domain) {
		irq_domain_free_fwnode(fn);
		return NULL;
	}

	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "xen/hyperv-vpci:online",
				xen_hyperv_vpci_cpu_up, xen_hyperv_vpci_cpu_down);
	if (ret < 0) {
		pr_err("Xen: can't bind the Hyper-V vPCI VIRQ: %d\n", ret);
		irq_domain_remove(xen_hv_vpci_domain);
		xen_hv_vpci_domain = NULL;
		return NULL;
	}

	pr_info("Xen: Hyper-V vPCI interrupts relayed on vector 0x%x\n",
		xen_hv_vpci_vector);

	return xen_hv_vpci_domain;
}
EXPORT_SYMBOL_GPL(hyperv_xen_vpci_root_domain);

#endif /* CONFIG_HYPERV */
