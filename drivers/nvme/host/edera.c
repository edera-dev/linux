// SPDX-License-Identifier: GPL-2.0
/*
 * Edera enlightened mode for emulated NVMe controllers on Xen.
 *
 * An Edera backend advertises a vendor-specific PCI capability with a
 * mailbox. Once negotiated, every DMA address the controller sees is a grant
 * address in a region the driver registered, and queues can signal through
 * event channels instead of MSI-X and MMIO doorbells. The protocol is
 * specified in edera-devices/docs/enlightened-nvme.md.
 */
#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/dma-map-ops.h>
#include <linux/highmem.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <xen/xen.h>
#include <xen/events.h>
#include <xen/grant_table.h>
#include <xen/page.h>
#include <xen/interface/event_channel.h>
#include <asm/xen/hypercall.h>

#include "edera.h"

#define EDERA_CAP_TYPE_MAILBOX	0x01
#define EDERA_CAP_MAGIC		0x41524445	/* "EDRA" */

#define EDERA_CAP_LEN_OFF	0x02	/* u8 */
#define EDERA_CAP_TYPE_OFF	0x03	/* u8 */
#define EDERA_CAP_MAGIC_OFF	0x04
#define EDERA_CAP_VERSION	0x08	/* u16 major, then u16 minor */
#define EDERA_CAP_CLASS		0x0c	/* u16 class, then u16 backend domid */
#define EDERA_CAP_DEV_FEAT_LO	0x10
#define EDERA_CAP_DEV_FEAT_HI	0x14
#define EDERA_CAP_LIMITS	0x18	/* u32 max region pages */
#define EDERA_CAP_LIMITS2	0x1c	/* u16 max regions, then u16 max queues */
#define EDERA_CAP_MBOX_CMD	0x20
#define EDERA_CAP_MBOX_STATUS	0x24
#define EDERA_CAP_MBOX_ARG0	0x28
#define EDERA_CAP_MBOX_ARG1	0x2c
#define EDERA_CAP_MBOX_ARG2	0x30
#define EDERA_CAP_MBOX_RESULT	0x34
#define EDERA_CAP_MIN_LEN	0x38

#define EDERA_CLASS_NVME	0x0001

#define EDERA_VERSION_MAJOR	1
#define EDERA_VERSION_MINOR	0

#define EDERA_F_GRANT_ADDRESSING	BIT_ULL(0)
#define EDERA_F_EVTCHN_QUEUES		BIT_ULL(1)
#define EDERA_F_EVTCHN_DOORBELL		BIT_ULL(2)
#define EDERA_F_GRANT_ONLY_LOCK		BIT_ULL(3)
#define EDERA_F_GRANT_DOORBELLS		BIT_ULL(4)
#define EDERA_F_GRANT_COPY		BIT_ULL(5)
#define EDERA_F_DRIVER_SUPPORTED	(EDERA_F_GRANT_ADDRESSING | \
					 EDERA_F_EVTCHN_QUEUES | \
					 EDERA_F_EVTCHN_DOORBELL | \
					 EDERA_F_GRANT_ONLY_LOCK | \
					 EDERA_F_GRANT_DOORBELLS | \
					 EDERA_F_GRANT_COPY)

enum edera_cmd {
	EDERA_CMD_NEGOTIATE	= 0x01,
	EDERA_CMD_REGION_ADD	= 0x02,
	EDERA_CMD_REGION_REMOVE	= 0x03,
	EDERA_CMD_ACTIVATE	= 0x04,
	EDERA_CMD_QUEUE_BIND	= 0x05,
	EDERA_CMD_QUEUE_UNBIND	= 0x06,
	EDERA_CMD_RESET		= 0x07,
	EDERA_CMD_LOCK		= 0x08,
	EDERA_CMD_DOORBELL_PAGE	= 0x09,
};

enum edera_status {
	EDERA_ST_OK		= 0x00,
	EDERA_ST_BUSY		= 0x01,
	EDERA_ST_UNKNOWN_CMD	= 0x10,
	EDERA_ST_BAD_STATE	= 0x11,
	EDERA_ST_INVALID_ARG	= 0x12,
	EDERA_ST_VERSION	= 0x13,
	EDERA_ST_FEATURES	= 0x14,
	EDERA_ST_LIMIT		= 0x15,
	EDERA_ST_GRANT		= 0x16,
	EDERA_ST_NOT_FOUND	= 0x17,
	EDERA_ST_EVTCHN		= 0x18,
	EDERA_ST_POLICY		= 0x19,
	EDERA_ST_IN_USE		= 0x1a,
};

/* Bit 63 marks a grant address, bits 12-43 carry the grant reference. */
#define EDERA_GADDR_FLAG	BIT_ULL(63)
#define EDERA_GADDR_GREF(a)	((grant_ref_t)(((a) >> XEN_PAGE_SHIFT) & U32_MAX))

#define EDERA_MAX_MAPPING	SZ_128K
#define EDERA_MAX_MAPPING_PAGES	(EDERA_MAX_MAPPING / XEN_PAGE_SIZE + 1)
#define EDERA_MBOX_POLL_US	10
#define EDERA_MBOX_POLLS	100000

static bool edera_enlighten = true;
module_param(edera_enlighten, bool, 0444);
MODULE_PARM_DESC(edera_enlighten,
		 "use the Edera enlightened mode when the controller offers it");

static unsigned int edera_bounce_kb = 16384;
module_param(edera_bounce_kb, uint, 0444);
MODULE_PARM_DESC(edera_bounce_kb,
		 "size in KiB of the persistent granted data pool per controller");

static bool edera_fill_reads;
module_param(edera_fill_reads, bool, 0444);
MODULE_PARM_DESC(edera_fill_reads,
		 "copy a read's pages into its pool range before the device writes it, so a short device write cannot return another request's data");

static unsigned int edera_db_page = 1;
module_param(edera_db_page, uint, 0444);
MODULE_PARM_DESC(edera_db_page,
		 "write doorbells to a granted page that the backend polls: 1 with MSI-X interrupts, 2 with event channel interrupts, 0 for event channels in both directions");

static unsigned int edera_copy = 2;
module_param(edera_copy, uint, 0444);
MODULE_PARM_DESC(edera_copy,
		 "grant a mapping's own pages for the backend to copy, instead of copying through the data pool: 1 for reads, 2 for reads and writes, 0 never");

static bool edera_grant_only;
module_param(edera_grant_only, bool, 0444);
MODULE_PARM_DESC(edera_grant_only,
		 "ask the backend to refuse non-grant DMA for the rest of the guest's life");

struct edera_region {
	struct list_head node;
	void *vaddr;
	grant_ref_t first;
	unsigned int npages;
};

struct edera_slot {
	phys_addr_t orig;
	unsigned int npages;
};

/* The largest physically contiguous allocation, and so the largest pool part. */
#define EDERA_POOL_PAGES	(1U << MAX_PAGE_ORDER)
#define EDERA_MAX_POOLS		16

/*
 * One physically contiguous, persistently granted part of the data pool. Each
 * has its own lock, and a CPU starts at its own part, so mappings on different
 * CPUs rarely contend. A copy window has the same layout but no pages: its
 * references grant the caller's own pages for one mapping, and the backend
 * reaches them only with grant copy.
 */
struct edera_pool {
	/* Protects map and hint. */
	spinlock_t lock;
	struct edera_region *region;
	grant_ref_t first;
	unsigned int npages;
	bool copy;
	unsigned long *map;
	struct edera_slot *slots;
	/* Next-fit start, so a search does not rescan the busy front each time. */
	unsigned int hint;
};

struct edera_nvme {
	/* A per-device copy, so the callbacks can find their controller. */
	struct dma_map_ops ops;
	struct pci_dev *pdev;
	int cap;
	domid_t backend;
	u64 features;
	u32 max_region_pages;
	u16 max_regions;
	u16 max_queues;

	/* Regions are registered with the backend while @active. */
	bool active;
	bool ever_active;
	bool fallback;

	/* Serialises the mailbox; region allocation can run in atomic context. */
	spinlock_t mbox_lock;

	/* Protects the region list and makes registration atomic with replay. */
	spinlock_t region_lock;
	struct list_head regions;
	unsigned int nr_regions;
	unsigned int region_pages;

	struct edera_pool pools[EDERA_MAX_POOLS];
	unsigned int nr_pools;
	struct edera_pool wins[EDERA_MAX_POOLS];
	unsigned int nr_wins;

	/* The doorbell array, in a granted page, with EDERA_F_GRANT_DOORBELLS. */
	struct edera_region *db;
};

static struct edera_nvme *edera_from_dev(struct device *dev)
{
	return container_of(get_dma_ops(dev), struct edera_nvme, ops);
}

static inline dma_addr_t edera_gaddr(grant_ref_t gref, unsigned int off)
{
	return EDERA_GADDR_FLAG | ((u64)gref << XEN_PAGE_SHIFT) | off;
}

static int edera_status_errno(u32 status)
{
	switch (status) {
	case EDERA_ST_OK:
		return 0;
	case EDERA_ST_NOT_FOUND:
		return -ENOENT;
	case EDERA_ST_BAD_STATE:
		return -EBUSY;
	case EDERA_ST_LIMIT:
		return -ENOSPC;
	case EDERA_ST_IN_USE:
		return -EADDRINUSE;
	case EDERA_ST_UNKNOWN_CMD:
	case EDERA_ST_VERSION:
	case EDERA_ST_FEATURES:
	case EDERA_ST_POLICY:
		return -EOPNOTSUPP;
	default:
		return -EIO;
	}
}

static int edera_mbox(struct edera_nvme *e, u32 cmd, u32 a0, u32 a1, u32 a2,
		      u32 *result)
{
	struct pci_dev *pdev = e->pdev;
	unsigned long flags;
	u32 status = EDERA_ST_BUSY;
	unsigned int i;

	spin_lock_irqsave(&e->mbox_lock, flags);
	pci_write_config_dword(pdev, e->cap + EDERA_CAP_MBOX_ARG0, a0);
	pci_write_config_dword(pdev, e->cap + EDERA_CAP_MBOX_ARG1, a1);
	pci_write_config_dword(pdev, e->cap + EDERA_CAP_MBOX_ARG2, a2);
	pci_write_config_dword(pdev, e->cap + EDERA_CAP_MBOX_CMD, cmd);
	for (i = 0; i < EDERA_MBOX_POLLS; i++) {
		pci_read_config_dword(pdev, e->cap + EDERA_CAP_MBOX_STATUS,
				      &status);
		if (status != EDERA_ST_BUSY)
			break;
		udelay(EDERA_MBOX_POLL_US);
	}
	if (result)
		pci_read_config_dword(pdev, e->cap + EDERA_CAP_MBOX_RESULT,
				      result);
	spin_unlock_irqrestore(&e->mbox_lock, flags);

	if (status != EDERA_ST_OK)
		dev_dbg(&pdev->dev, "edera: command %#x failed, status %#x\n",
			cmd, status);
	return edera_status_errno(status);
}

static int edera_region_register(struct edera_nvme *e, struct edera_region *r)
{
	return edera_mbox(e, EDERA_CMD_REGION_ADD, r->first, r->npages, 0,
			  NULL);
}

static struct edera_region *edera_region_alloc(struct edera_nvme *e,
					       size_t size, gfp_t gfp)
{
	unsigned int npages = PAGE_ALIGN(size) >> XEN_PAGE_SHIFT;
	struct edera_region *r;
	unsigned long flags;
	unsigned int i;
	int ret;

	r = kzalloc(sizeof(*r), gfp);
	if (!r)
		return NULL;
	r->npages = npages;
	r->vaddr = alloc_pages_exact(npages << XEN_PAGE_SHIFT, gfp | __GFP_ZERO);
	if (!r->vaddr)
		goto free_region;
	if (gnttab_alloc_grant_reference_seq(npages, &r->first))
		goto free_pages;
	for (i = 0; i < npages; i++) {
		struct page *page = virt_to_page(r->vaddr + i * XEN_PAGE_SIZE);

		gnttab_grant_foreign_access_ref(r->first + i, e->backend,
						xen_page_to_gfn(page), 0);
	}

	spin_lock_irqsave(&e->region_lock, flags);
	if (e->nr_regions >= e->max_regions ||
	    e->region_pages + npages > e->max_region_pages) {
		ret = -ENOSPC;
	} else {
		ret = e->active ? edera_region_register(e, r) : 0;
		/* No session: the next enable replays the region. */
		if (ret == -EBUSY)
			ret = 0;
	}
	if (!ret) {
		list_add_tail(&r->node, &e->regions);
		e->nr_regions++;
		e->region_pages += npages;
	}
	spin_unlock_irqrestore(&e->region_lock, flags);
	if (ret)
		goto end_grants;
	return r;

end_grants:
	/* The backend never mapped these grants, so ending them cannot fail. */
	for (i = 0; i < npages; i++)
		gnttab_end_foreign_access_ref(r->first + i);
	gnttab_free_grant_reference_seq(r->first, npages);
free_pages:
	free_pages_exact(r->vaddr, npages << XEN_PAGE_SHIFT);
free_region:
	kfree(r);
	return NULL;
}

static void edera_region_free(struct edera_nvme *e, struct edera_region *r)
{
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&e->region_lock, flags);
	list_del(&r->node);
	e->nr_regions--;
	e->region_pages -= r->npages;
	/* Without a session the backend has already unmapped everything. */
	if (e->active)
		edera_mbox(e, EDERA_CMD_REGION_REMOVE, r->first, 0, 0, NULL);
	spin_unlock_irqrestore(&e->region_lock, flags);

	for (i = 0; i < r->npages; i++) {
		if (!gnttab_end_foreign_access_ref(r->first + i)) {
			dev_alert(&e->pdev->dev,
				  "edera: grant %u still mapped by the backend, leaking region\n",
				  r->first + i);
			return;
		}
	}
	gnttab_free_grant_reference_seq(r->first, r->npages);
	free_pages_exact(r->vaddr, r->npages << XEN_PAGE_SHIFT);
	kfree(r);
}

static void *edera_dma_alloc(struct device *dev, size_t size,
			     dma_addr_t *dma_handle, gfp_t gfp,
			     unsigned long attrs)
{
	struct edera_nvme *e = edera_from_dev(dev);
	struct edera_region *r;

	r = edera_region_alloc(e, size, gfp);
	if (!r)
		return NULL;
	*dma_handle = edera_gaddr(r->first, 0);
	return r->vaddr;
}

static void edera_dma_free(struct device *dev, size_t size, void *vaddr,
			   dma_addr_t dma_handle, unsigned long attrs)
{
	struct edera_nvme *e = edera_from_dev(dev);
	grant_ref_t first = EDERA_GADDR_GREF(dma_handle);
	struct edera_region *r, *found = NULL;
	unsigned long flags;

	spin_lock_irqsave(&e->region_lock, flags);
	list_for_each_entry(r, &e->regions, node) {
		if (r->first == first && r->vaddr == vaddr) {
			found = r;
			break;
		}
	}
	spin_unlock_irqrestore(&e->region_lock, flags);

	if (WARN_ON_ONCE(!found))
		return;
	edera_region_free(e, found);
}

/* Copies between a bounce slot range and the original pages it shadows. */
static void edera_bounce_copy(struct edera_pool *p, unsigned int slot,
			      unsigned int off, size_t size, bool to_bounce)
{
	void *bounce = p->region->vaddr + slot * XEN_PAGE_SIZE + off;
	phys_addr_t orig = p->slots[slot].orig + off;

	while (size) {
		size_t chunk = min_t(size_t, size,
				     PAGE_SIZE - offset_in_page(orig));
		void *va = kmap_local_pfn(PHYS_PFN(orig)) + offset_in_page(orig);

		if (to_bounce)
			memcpy(bounce, va, chunk);
		else
			memcpy(va, bounce, chunk);
		kunmap_local(va);
		bounce += chunk;
		orig += chunk;
		size -= chunk;
	}
}

static struct edera_pool *edera_pool_of(struct edera_nvme *e, dma_addr_t addr,
					unsigned int *slot)
{
	grant_ref_t gref = EDERA_GADDR_GREF(addr);
	unsigned int i;

	if (WARN_ON_ONCE(!(addr & EDERA_GADDR_FLAG)))
		return NULL;
	for (i = 0; i < e->nr_pools + e->nr_wins; i++) {
		struct edera_pool *p = i < e->nr_pools ? &e->pools[i] :
				       &e->wins[i - e->nr_pools];

		if (gref >= p->first && gref - p->first < p->npages) {
			*slot = gref - p->first;
			return p;
		}
	}
	WARN_ON_ONCE(1);
	return NULL;
}

/* Takes @npages consecutive pages of one of @nr parts, this CPU's first. */
static struct edera_pool *edera_pool_get(struct edera_pool *pools,
					 unsigned int nr, unsigned int npages,
					 unsigned int *slot)
{
	unsigned int first = raw_smp_processor_id() % nr;
	unsigned long flags, s;
	unsigned int i;

	for (i = 0; i < nr; i++) {
		struct edera_pool *p = &pools[(first + i) % nr];
		unsigned int size = p->npages;

		spin_lock_irqsave(&p->lock, flags);
		s = bitmap_find_next_zero_area(p->map, size, p->hint, npages, 0);
		if (s >= size)
			s = bitmap_find_next_zero_area(p->map, size, 0, npages,
						       0);
		if (s < size) {
			bitmap_set(p->map, s, npages);
			p->hint = s + npages;
			spin_unlock_irqrestore(&p->lock, flags);
			*slot = s;
			return p;
		}
		spin_unlock_irqrestore(&p->lock, flags);
	}
	return NULL;
}

/* Shadows @size bytes at @phys with pool pages from @slot on. */
static dma_addr_t edera_map_into(struct edera_pool *p, unsigned int slot,
				 phys_addr_t phys, size_t size,
				 enum dma_data_direction dir,
				 unsigned long attrs)
{
	unsigned int off = offset_in_page(phys);
	unsigned int npages = DIV_ROUND_UP(off + size, XEN_PAGE_SIZE);
	unsigned int i;

	for (i = 0; i < npages; i++)
		p->slots[slot + i].orig = (phys & PAGE_MASK) + i * PAGE_SIZE;
	p->slots[slot].npages = npages;

	/*
	 * Fill the slot even for DMA_FROM_DEVICE, so a short device write
	 * cannot copy another request's stale data back to the caller.
	 */
	if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC) &&
	    (dir != DMA_FROM_DEVICE || edera_fill_reads))
		edera_bounce_copy(p, slot, off, size, true);
	return edera_gaddr(p->region->first + slot, off);
}

/* Grants the backend the pages of @size bytes at @phys under window @slot on. */
static dma_addr_t edera_grant_into(struct edera_nvme *e, struct edera_pool *p,
				   unsigned int slot, phys_addr_t phys,
				   size_t size, enum dma_data_direction dir)
{
	unsigned int off = offset_in_page(phys);
	unsigned int npages = DIV_ROUND_UP(off + size, XEN_PAGE_SIZE);
	unsigned int i;

	for (i = 0; i < npages; i++)
		gnttab_grant_foreign_access_ref(p->first + slot + i, e->backend,
						pfn_to_gfn(PHYS_PFN(phys) + i),
						dir == DMA_TO_DEVICE);
	p->slots[slot].npages = npages;
	return edera_gaddr(p->first + slot, off);
}

/* Ends the grants of a window mapping, or leaks it while the backend holds one. */
static void edera_grant_end(struct edera_nvme *e, struct edera_pool *p,
			    unsigned int slot)
{
	unsigned int npages = p->slots[slot].npages;
	unsigned long flags;
	unsigned int i;

	for (i = 0; i < npages; i++) {
		if (!gnttab_end_foreign_access_ref(p->first + slot + i)) {
			dev_alert(&e->pdev->dev,
				  "edera: grant %u still in use by the backend, leaking it\n",
				  p->first + slot + i);
			return;
		}
	}
	spin_lock_irqsave(&p->lock, flags);
	bitmap_clear(p->map, slot, npages);
	spin_unlock_irqrestore(&p->lock, flags);
}

static dma_addr_t edera_dma_map_page(struct device *dev, struct page *page,
				     unsigned long offset, size_t size,
				     enum dma_data_direction dir,
				     unsigned long attrs)
{
	struct edera_nvme *e = edera_from_dev(dev);
	phys_addr_t phys = page_to_phys(page) + offset;
	unsigned int npages = DIV_ROUND_UP(offset_in_page(phys) + size,
					   XEN_PAGE_SIZE);
	struct edera_pool *p;
	unsigned int slot;

	if (!size || size > EDERA_MAX_MAPPING)
		return DMA_MAPPING_ERROR;
	/*
	 * Writes stay in the pool: the backend limits the write bytes in
	 * flight, and a copy inside that limit costs more than one before it.
	 */
	if (e->nr_wins && (dir == DMA_FROM_DEVICE || edera_copy > 1)) {
		p = edera_pool_get(e->wins, e->nr_wins, npages, &slot);
		if (p)
			return edera_grant_into(e, p, slot, phys, size, dir);
	}
	p = edera_pool_get(e->pools, e->nr_pools, npages, &slot);
	if (!p)
		return DMA_MAPPING_ERROR;
	return edera_map_into(p, slot, phys, size, dir, attrs);
}

static void edera_dma_unmap_page(struct device *dev, dma_addr_t addr,
				 size_t size, enum dma_data_direction dir,
				 unsigned long attrs)
{
	struct edera_nvme *e = edera_from_dev(dev);
	unsigned int off = addr & ~XEN_PAGE_MASK;
	struct edera_pool *p;
	unsigned long flags;
	unsigned int slot;

	p = edera_pool_of(e, addr, &slot);
	if (!p)
		return;
	if (p->copy) {
		edera_grant_end(e, p, slot);
		return;
	}
	if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC) &&
	    (dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL))
		edera_bounce_copy(p, slot, off, size, false);

	spin_lock_irqsave(&p->lock, flags);
	bitmap_clear(p->map, slot, p->slots[slot].npages);
	spin_unlock_irqrestore(&p->lock, flags);
}

static void edera_dma_sync_for_cpu(struct device *dev, dma_addr_t addr,
				   size_t size, enum dma_data_direction dir)
{
	struct edera_nvme *e = edera_from_dev(dev);
	struct edera_pool *p;
	unsigned int slot;

	if (dir == DMA_TO_DEVICE)
		return;
	p = edera_pool_of(e, addr, &slot);
	if (p && !p->copy)
		edera_bounce_copy(p, slot, addr & ~XEN_PAGE_MASK, size, false);
}

static void edera_dma_sync_for_device(struct device *dev, dma_addr_t addr,
				      size_t size, enum dma_data_direction dir)
{
	struct edera_nvme *e = edera_from_dev(dev);
	struct edera_pool *p;
	unsigned int slot;

	p = edera_pool_of(e, addr, &slot);
	if (p && !p->copy)
		edera_bounce_copy(p, slot, addr & ~XEN_PAGE_MASK, size, true);
}

static void edera_dma_unmap_sg(struct device *dev, struct scatterlist *sgl,
			       int nents, enum dma_data_direction dir,
			       unsigned long attrs)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i)
		edera_dma_unmap_page(dev, sg->dma_address, sg_dma_len(sg), dir,
				     attrs);
}

static int edera_dma_map_sg(struct device *dev, struct scatterlist *sgl,
			    int nents, enum dma_data_direction dir,
			    unsigned long attrs)
{
	struct edera_nvme *e = edera_from_dev(dev);
	unsigned int total = 0, slot;
	struct scatterlist *sg;
	struct edera_pool *p;
	int i;

	/* One pool range for the whole list: one lock instead of one per segment. */
	for_each_sg(sgl, sg, nents, i) {
		if (!sg->length || sg->length > EDERA_MAX_MAPPING) {
			total = 0;
			break;
		}
		total += DIV_ROUND_UP(offset_in_page(sg_phys(sg)) + sg->length,
				      XEN_PAGE_SIZE);
	}
	p = total ? edera_pool_get(e->pools, e->nr_pools, total, &slot) : NULL;
	if (p) {
		for_each_sg(sgl, sg, nents, i) {
			phys_addr_t phys = sg_phys(sg);

			sg->dma_address = edera_map_into(p, slot, phys,
							 sg->length, dir, attrs);
			sg_dma_len(sg) = sg->length;
			slot += DIV_ROUND_UP(offset_in_page(phys) + sg->length,
					     XEN_PAGE_SIZE);
		}
		return nents;
	}

	for_each_sg(sgl, sg, nents, i) {
		sg->dma_address = edera_dma_map_page(dev, sg_page(sg),
						     sg->offset, sg->length,
						     dir, attrs);
		if (sg->dma_address == DMA_MAPPING_ERROR) {
			edera_dma_unmap_sg(dev, sgl, i, dir,
					   attrs | DMA_ATTR_SKIP_CPU_SYNC);
			return -ENOMEM;
		}
		sg_dma_len(sg) = sg->length;
	}
	return nents;
}

static void edera_dma_sync_sg_for_cpu(struct device *dev,
				      struct scatterlist *sgl, int nents,
				      enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i)
		edera_dma_sync_for_cpu(dev, sg->dma_address, sg_dma_len(sg),
				       dir);
}

static void edera_dma_sync_sg_for_device(struct device *dev,
					 struct scatterlist *sgl, int nents,
					 enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i)
		edera_dma_sync_for_device(dev, sg->dma_address,
					  sg_dma_len(sg), dir);
}

static int edera_dma_supported(struct device *dev, u64 mask)
{
	return mask == DMA_BIT_MASK(64);
}

static size_t edera_dma_max_mapping_size(struct device *dev)
{
	return EDERA_MAX_MAPPING;
}

static const struct dma_map_ops edera_dma_ops = {
	.alloc			= edera_dma_alloc,
	.free			= edera_dma_free,
	.map_page		= edera_dma_map_page,
	.unmap_page		= edera_dma_unmap_page,
	.map_sg			= edera_dma_map_sg,
	.unmap_sg		= edera_dma_unmap_sg,
	.sync_single_for_cpu	= edera_dma_sync_for_cpu,
	.sync_single_for_device	= edera_dma_sync_for_device,
	.sync_sg_for_cpu	= edera_dma_sync_sg_for_cpu,
	.sync_sg_for_device	= edera_dma_sync_sg_for_device,
	.dma_supported		= edera_dma_supported,
	.max_mapping_size	= edera_dma_max_mapping_size,
};

static int edera_find_cap(struct pci_dev *pdev)
{
	int pos;

	for (pos = pci_find_capability(pdev, PCI_CAP_ID_VNDR); pos;
	     pos = pci_find_next_capability(pdev, pos, PCI_CAP_ID_VNDR)) {
		u8 len, type;
		u32 magic;

		pci_read_config_byte(pdev, pos + EDERA_CAP_LEN_OFF, &len);
		pci_read_config_byte(pdev, pos + EDERA_CAP_TYPE_OFF, &type);
		pci_read_config_dword(pdev, pos + EDERA_CAP_MAGIC_OFF, &magic);
		if (len >= EDERA_CAP_MIN_LEN && type == EDERA_CAP_TYPE_MAILBOX &&
		    magic == EDERA_CAP_MAGIC)
			return pos;
	}
	return 0;
}

static void edera_free_bounce(struct edera_nvme *e)
{
	unsigned int i;

	if (e->db)
		edera_region_free(e, e->db);
	e->db = NULL;
	for (i = 0; i < EDERA_MAX_POOLS; i++) {
		struct edera_pool *p = &e->pools[i];

		if (p->region)
			edera_region_free(e, p->region);
		p->region = NULL;
		bitmap_free(p->map);
		p->map = NULL;
		kvfree(p->slots);
		p->slots = NULL;
	}
	e->nr_pools = 0;
	for (i = 0; i < e->nr_wins; i++) {
		struct edera_pool *w = &e->wins[i];

		/* Every mapping has ended its grants, so the references are free. */
		gnttab_free_grant_reference_seq(w->first, w->npages);
		bitmap_free(w->map);
		w->map = NULL;
		kvfree(w->slots);
		w->slots = NULL;
	}
	e->nr_wins = 0;
}

/* One copy window per pool part, as large as the part. */
static void edera_alloc_wins(struct edera_nvme *e)
{
	unsigned int i;

	for (i = 0; i < e->nr_pools; i++) {
		struct edera_pool *w = &e->wins[i];
		unsigned int n = e->pools[i].npages;

		spin_lock_init(&w->lock);
		w->copy = true;
		w->npages = n;
		w->map = bitmap_zalloc(n, GFP_KERNEL);
		w->slots = kvcalloc(n, sizeof(*w->slots), GFP_KERNEL);
		if (!w->map || !w->slots ||
		    gnttab_alloc_grant_reference_seq(n, &w->first)) {
			bitmap_free(w->map);
			w->map = NULL;
			kvfree(w->slots);
			w->slots = NULL;
			break;
		}
		e->nr_wins++;
	}
	if (!e->nr_wins)
		e->features &= ~EDERA_F_GRANT_COPY;
}

/* Returns NULL, and so stock emulated mode, unless enlightenment is usable. */
struct edera_nvme *edera_nvme_probe(struct pci_dev *pdev)
{
	unsigned int bounce_pages, per_pool, nr, i;
	struct edera_nvme *e;
	u32 version, class, lo, hi, limits, limits2;
	int cap;

	BUILD_BUG_ON(PAGE_SIZE != XEN_PAGE_SIZE);

	cap = edera_find_cap(pdev);
	if (!cap) {
		dev_dbg(&pdev->dev, "edera: no enlightenment capability\n");
		return NULL;
	}
	if (!edera_enlighten || !xen_hvm_domain()) {
		dev_info(&pdev->dev,
			 "edera: enlightenment disabled or not on Xen, using emulated mode\n");
		return NULL;
	}

	pci_read_config_dword(pdev, cap + EDERA_CAP_VERSION, &version);
	pci_read_config_dword(pdev, cap + EDERA_CAP_CLASS, &class);
	pci_read_config_dword(pdev, cap + EDERA_CAP_DEV_FEAT_LO, &lo);
	pci_read_config_dword(pdev, cap + EDERA_CAP_DEV_FEAT_HI, &hi);
	pci_read_config_dword(pdev, cap + EDERA_CAP_LIMITS, &limits);
	pci_read_config_dword(pdev, cap + EDERA_CAP_LIMITS2, &limits2);

	if ((version & 0xffff) != EDERA_VERSION_MAJOR ||
	    (class & 0xffff) != EDERA_CLASS_NVME ||
	    (class >> 16) >= DOMID_FIRST_RESERVED) {
		dev_info(&pdev->dev,
			 "edera: unsupported capability (version %#x, class %#x), using emulated mode\n",
			 version, class);
		return NULL;
	}
	if (!(lo & EDERA_F_GRANT_ADDRESSING)) {
		dev_info(&pdev->dev,
			 "edera: backend lacks grant addressing, using emulated mode\n");
		return NULL;
	}

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return NULL;
	e->pdev = pdev;
	e->cap = cap;
	e->backend = class >> 16;
	e->features = (((u64)hi << 32) | lo) & EDERA_F_DRIVER_SUPPORTED;
	e->max_region_pages = limits;
	e->max_regions = limits2 & 0xffff;
	e->max_queues = limits2 >> 16;
	e->ops = edera_dma_ops;
	spin_lock_init(&e->mbox_lock);
	spin_lock_init(&e->region_lock);
	INIT_LIST_HEAD(&e->regions);

	/*
	 * Leave at least half of the backend's page and region budgets for
	 * queues. Each pool part is one physically contiguous allocation.
	 */
	bounce_pages = min(edera_bounce_kb / (XEN_PAGE_SIZE / SZ_1K),
			   e->max_region_pages / 2);
	nr = min3(DIV_ROUND_UP(bounce_pages, EDERA_POOL_PAGES),
		  (unsigned int)EDERA_MAX_POOLS, e->max_regions / 2U);
	per_pool = nr ? min(bounce_pages / nr, EDERA_POOL_PAGES) : 0;
	if (per_pool < 4 * EDERA_MAX_MAPPING_PAGES) {
		dev_info(&pdev->dev,
			 "edera: data pool of %u pages is too small, using emulated mode\n",
			 bounce_pages);
		goto free;
	}
	for (i = 0; i < nr; i++) {
		struct edera_pool *p = &e->pools[i];

		spin_lock_init(&p->lock);
		p->map = bitmap_zalloc(per_pool, GFP_KERNEL);
		p->slots = kvcalloc(per_pool, sizeof(*p->slots), GFP_KERNEL);
		if (!p->map || !p->slots)
			goto free_bounce;
		p->region = edera_region_alloc(e, per_pool << XEN_PAGE_SHIFT,
					       GFP_KERNEL);
		if (!p->region)
			goto free_bounce;
		p->first = p->region->first;
		p->npages = p->region->npages;
		e->nr_pools++;
	}

	if (edera_copy && (e->features & EDERA_F_GRANT_COPY))
		edera_alloc_wins(e);
	else
		e->features &= ~EDERA_F_GRANT_COPY;

	if (edera_db_page && (e->features & EDERA_F_GRANT_DOORBELLS)) {
		e->features &= ~EDERA_F_EVTCHN_DOORBELL;
		if (edera_db_page == 1)
			e->features &= ~EDERA_F_EVTCHN_QUEUES;
		e->db = edera_region_alloc(e, XEN_PAGE_SIZE, GFP_KERNEL);
		if (!e->db)
			goto free_bounce;
	} else {
		e->features &= ~EDERA_F_GRANT_DOORBELLS;
	}

	set_dma_ops(&pdev->dev, &e->ops);
	dev_info(&pdev->dev,
		 "edera: capability v%u.%u, backend domain %u, features %#llx, data pool %u x %u pages, %u copy windows\n",
		 version & 0xffff, version >> 16, e->backend, e->features,
		 nr, per_pool, e->nr_wins);
	return e;

free_bounce:
	edera_free_bounce(e);
free:
	kfree(e);
	return NULL;
}

static void edera_fall_back(struct edera_nvme *e, int err)
{
	dev_warn(&e->pdev->dev,
		 "edera: negotiation failed (%d), using emulated mode\n", err);
	edera_mbox(e, EDERA_CMD_RESET, 0, 0, 0, NULL);
	set_dma_ops(&e->pdev->dev, NULL);
	edera_free_bounce(e);
	e->fallback = true;
}

/*
 * Called with the controller disabled and bus mastering on, before the admin
 * queue is set up. Clearing bus mastering ends the backend session, so every
 * enable negotiates again and replays the regions that are still allocated.
 */
int edera_nvme_enable(struct edera_nvme *e)
{
	u64 want;
	struct edera_region *r;
	unsigned long flags;
	u32 result = 0;
	int ret;

	if (!e || e->fallback)
		return 0;

	e->active = false;
	want = e->features;
	ret = edera_mbox(e, EDERA_CMD_NEGOTIATE,
			 EDERA_VERSION_MAJOR | EDERA_VERSION_MINOR << 16,
			 lower_32_bits(want), upper_32_bits(want), &result);
	if (ret)
		goto fail;

	spin_lock_irqsave(&e->region_lock, flags);
	list_for_each_entry(r, &e->regions, node) {
		ret = edera_region_register(e, r);
		if (ret)
			break;
	}
	spin_unlock_irqrestore(&e->region_lock, flags);
	if (ret)
		goto fail;

	ret = edera_mbox(e, EDERA_CMD_ACTIVATE, 0, 0, 0, NULL);
	if (ret)
		goto fail;

	if (e->db) {
		ret = edera_mbox(e, EDERA_CMD_DOORBELL_PAGE, e->db->first, 0, 0,
				 NULL);
		if (ret)
			goto fail;
	}

	if (edera_grant_only) {
		if (!(want & EDERA_F_GRANT_ONLY_LOCK) ||
		    edera_mbox(e, EDERA_CMD_LOCK, 0, 0, 0, NULL))
			dev_warn(&e->pdev->dev,
				 "edera: backend refused the grant-only lock\n");
	}

	e->active = true;
	if (!e->ever_active)
		dev_info(&e->pdev->dev, "edera: enlightened mode active (v%u.%u)\n",
			 result & 0xffff, result >> 16);
	e->ever_active = true;
	return 0;

fail:
	/*
	 * Queue memory already lives in grant space after the first success,
	 * so only a controller that never went live can return to emulation.
	 */
	if (!e->ever_active && e->nr_regions == e->nr_pools + !!e->db) {
		edera_fall_back(e, ret);
		return 0;
	}
	edera_mbox(e, EDERA_CMD_RESET, 0, 0, 0, NULL);
	dev_err(&e->pdev->dev, "edera: renegotiation failed (%d)\n", ret);
	return ret;
}

/* Where the driver writes doorbells: the granted page while a session uses one. */
void __iomem *edera_nvme_dbs(struct edera_nvme *e, void __iomem *dbs)
{
	if (!e || !e->active || !e->db)
		return dbs;
	return (void __iomem *)e->db->vaddr;
}

void edera_nvme_release(struct edera_nvme *e)
{
	if (!e)
		return;
	if (!e->fallback) {
		if (e->active)
			edera_mbox(e, EDERA_CMD_RESET, 0, 0, 0, NULL);
		e->active = false;
		edera_free_bounce(e);
		WARN_ON(e->nr_regions);
		set_dma_ops(&e->pdev->dev, NULL);
	}
	kfree(e);
}

static irqreturn_t edera_nvme_evtchn_irq(int irq, void *dev_id)
{
	struct edera_nvme_qbind *b = dev_id;
	irqreturn_t ret = b->handler(irq, b->data);

	/* A late EOI throttles a backend that sends events with no work. */
	xen_irq_lateeoi(irq, ret == IRQ_NONE ? XEN_EOI_FLAG_SPURIOUS : 0);
	return IRQ_HANDLED;
}

static void edera_evtchn_close(evtchn_port_t port)
{
	struct evtchn_close close = { .port = port };

	HYPERVISOR_event_channel_op(EVTCHNOP_close, &close);
}

/* Returns -EOPNOTSUPP when the caller should use its MSI-X vector instead. */
int edera_nvme_bind_queue(struct edera_nvme *e, struct edera_nvme_qbind *b,
			  u16 qid, irq_handler_t handler, void *data,
			  const struct cpumask *affinity, int instance)
{
	struct evtchn_alloc_unbound alloc = {
		.dom = DOMID_SELF,
	};
	int irq, ret;

	if (!e || !e->active || !(e->features & EDERA_F_EVTCHN_QUEUES) ||
	    qid >= e->max_queues)
		return -EOPNOTSUPP;

	alloc.remote_dom = e->backend;
	ret = HYPERVISOR_event_channel_op(EVTCHNOP_alloc_unbound, &alloc);
	if (ret)
		return -EOPNOTSUPP;

	b->name = kasprintf(GFP_KERNEL, "nvme%dq%d", instance, qid);
	if (!b->name) {
		edera_evtchn_close(alloc.port);
		return -ENOMEM;
	}
	b->edera = e;
	b->handler = handler;
	b->data = data;
	b->qid = qid;
	b->port = alloc.port;
	b->doorbell = false;

	irq = bind_evtchn_to_irqhandler_lateeoi(alloc.port,
						edera_nvme_evtchn_irq, 0,
						b->name, b);
	if (irq < 0) {
		edera_evtchn_close(alloc.port);
		ret = irq;
		goto free_name;
	}
	if (affinity)
		irq_set_affinity_and_hint(irq, affinity);

	ret = edera_mbox(e, EDERA_CMD_QUEUE_BIND, qid, alloc.port, 0, NULL);
	if (ret) {
		irq_set_affinity_and_hint(irq, NULL);
		unbind_from_irqhandler(irq, b);
		dev_warn(&e->pdev->dev,
			 "edera: queue %u bind failed (%d), using MSI-X\n",
			 qid, ret);
		ret = -EOPNOTSUPP;
		goto free_name;
	}
	b->irq = irq;
	b->doorbell = e->features & EDERA_F_EVTCHN_DOORBELL;
	return 0;

free_name:
	kfree(b->name);
	b->name = NULL;
	return ret;
}

void edera_nvme_unbind_queue(struct edera_nvme_qbind *b)
{
	struct edera_nvme *e = b->edera;

	if (!b->irq)
		return;
	b->doorbell = false;
	/* The backend has already dropped the binding if the controller reset. */
	if (e->active)
		edera_mbox(e, EDERA_CMD_QUEUE_UNBIND, b->qid, 0, 0, NULL);
	irq_set_affinity_and_hint(b->irq, NULL);
	unbind_from_irqhandler(b->irq, b);
	b->irq = 0;
	kfree(b->name);
	b->name = NULL;
}
