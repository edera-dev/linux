/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __XEN_PCI_H__
#define __XEN_PCI_H__

#include <linux/errno.h>
#include <linux/types.h>

struct pci_dev;

#if defined(CONFIG_XEN_PCIDEV_FRONTEND)
int pcifront_machine_sbdf(const struct pci_dev *dev, u32 *sbdf);
#else
static inline int pcifront_machine_sbdf(const struct pci_dev *dev, u32 *sbdf)
{
	return -ENODEV;
}
#endif

#if defined(CONFIG_XEN_DOM0)
int xen_reset_device(const struct pci_dev *dev);
int xen_find_device_domain_owner(struct pci_dev *dev);
int xen_register_device_domain_owner(struct pci_dev *dev, uint16_t domain);
int xen_unregister_device_domain_owner(struct pci_dev *dev);
#else
static inline int xen_reset_device(const struct pci_dev *dev)
{
	return -1;
}

static inline int xen_find_device_domain_owner(struct pci_dev *dev)
{
	return -1;
}

static inline int xen_register_device_domain_owner(struct pci_dev *dev,
						   uint16_t domain)
{
	return -1;
}

static inline int xen_unregister_device_domain_owner(struct pci_dev *dev)
{
	return -1;
}
#endif

#endif
