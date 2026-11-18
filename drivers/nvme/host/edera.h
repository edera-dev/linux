/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Edera enlightened mode for emulated NVMe controllers on Xen.
 *
 * The protocol is specified in edera-devices/docs/enlightened-nvme.md.
 */
#ifndef _NVME_EDERA_H
#define _NVME_EDERA_H

#include <linux/interrupt.h>
#include <linux/pci.h>

#ifdef CONFIG_NVME_EDERA
#include <xen/events.h>
#endif

struct edera_nvme;

/* Per-queue event channel binding, embedded in struct nvme_queue. */
struct edera_nvme_qbind {
	struct edera_nvme *edera;
	irq_handler_t handler;
	void *data;
	char *name;
	int irq;
	unsigned int port;
	u16 qid;
	bool doorbell;
};

#ifdef CONFIG_NVME_EDERA
struct edera_nvme *edera_nvme_probe(struct pci_dev *pdev);
void edera_nvme_release(struct edera_nvme *e);
int edera_nvme_enable(struct edera_nvme *e);
int edera_nvme_bind_queue(struct edera_nvme *e, struct edera_nvme_qbind *b,
			  u16 qid, irq_handler_t handler, void *data,
			  const struct cpumask *affinity, int instance);
void edera_nvme_unbind_queue(struct edera_nvme_qbind *b);
void __iomem *edera_nvme_dbs(struct edera_nvme *e, void __iomem *dbs);

/*
 * An event channel carries no value, so it can only replace a doorbell write
 * whose value the controller reads from the shadow doorbell buffer.
 */
static inline bool edera_nvme_notify(struct edera_nvme_qbind *b, bool shadowed)
{
	if (!shadowed || !b->doorbell)
		return false;
	notify_remote_via_evtchn(b->port);
	return true;
}
#else
static inline struct edera_nvme *edera_nvme_probe(struct pci_dev *pdev)
{
	return NULL;
}

static inline void edera_nvme_release(struct edera_nvme *e)
{
}

static inline int edera_nvme_enable(struct edera_nvme *e)
{
	return 0;
}

static inline int edera_nvme_bind_queue(struct edera_nvme *e,
					struct edera_nvme_qbind *b, u16 qid,
					irq_handler_t handler, void *data,
					const struct cpumask *affinity,
					int instance)
{
	return -EOPNOTSUPP;
}

static inline void edera_nvme_unbind_queue(struct edera_nvme_qbind *b)
{
}

static inline bool edera_nvme_notify(struct edera_nvme_qbind *b, bool shadowed)
{
	return false;
}

static inline void __iomem *edera_nvme_dbs(struct edera_nvme *e,
					   void __iomem *dbs)
{
	return dbs;
}
#endif

#endif /* _NVME_EDERA_H */
