// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Copyright 2010
 *  by Konrad Rzeszutek Wilk <konrad.wilk@oracle.com>
 *
 * This code provides a IOMMU for Xen PV guests with PCI passthrough.
 *
 * PV guests under Xen are running in an non-contiguous memory architecture.
 *
 * When PCI pass-through is utilized, this necessitates an IOMMU for
 * translating bus (DMA) to virtual and vice-versa and also providing a
 * mechanism to have contiguous pages for device drivers operations (say DMA
 * operations).
 *
 * Specifically, under Xen the Linux idea of pages is an illusion. It
 * assumes that pages start at zero and go up to the available memory. To
 * help with that, the Linux Xen MMU provides a lookup mechanism to
 * translate the page frame numbers (PFN) to machine frame numbers (MFN)
 * and vice-versa. The MFN are the "real" frame numbers. Furthermore
 * memory is not contiguous. Xen hypervisor stitches memory for guests
 * from different pools, which means there is no guarantee that PFN==MFN
 * and PFN+1==MFN+1. Lastly with Xen 4.0, pages (in debug mode) are
 * allocated in descending order (high to low), meaning the guest might
 * never get any MFN's under the 4GB mark.
 */

#define pr_fmt(fmt) "xen:" KBUILD_MODNAME ": " fmt

#include <linux/memblock.h>
#include <linux/dma-direct.h>
#include <linux/dma-map-ops.h>
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/vmalloc.h>
#include <xen/swiotlb-xen.h>
#include <xen/page.h>
#include <xen/xen-ops.h>
#include <xen/hvc-console.h>

#include <asm/dma-mapping.h>

#include <trace/events/swiotlb.h>
#define MAX_DMA_BITS 32

/*
 * Quick lookup value of the bus address of the IOTLB.
 */

static inline phys_addr_t xen_phys_to_bus(struct device *dev, phys_addr_t paddr)
{
	unsigned long bfn = pfn_to_bfn(XEN_PFN_DOWN(paddr));
	phys_addr_t baddr = (phys_addr_t)bfn << XEN_PAGE_SHIFT;

	baddr |= paddr & ~XEN_PAGE_MASK;
	return baddr;
}

static inline dma_addr_t xen_phys_to_dma(struct device *dev, phys_addr_t paddr)
{
	return phys_to_dma(dev, xen_phys_to_bus(dev, paddr));
}

static inline phys_addr_t xen_bus_to_phys(struct device *dev,
					  phys_addr_t baddr)
{
	unsigned long xen_pfn = bfn_to_pfn(XEN_PFN_DOWN(baddr));
	phys_addr_t paddr = (xen_pfn << XEN_PAGE_SHIFT) |
			    (baddr & ~XEN_PAGE_MASK);

	return paddr;
}

static inline phys_addr_t xen_dma_to_phys(struct device *dev,
					  dma_addr_t dma_addr)
{
	return xen_bus_to_phys(dev, dma_to_phys(dev, dma_addr));
}

static inline bool range_requires_alignment(phys_addr_t p, size_t size)
{
	phys_addr_t algn = 1ULL << (get_order(size) + PAGE_SHIFT);
	phys_addr_t bus_addr = pfn_to_bfn(XEN_PFN_DOWN(p)) << XEN_PAGE_SHIFT;

	return IS_ALIGNED(p, algn) && !IS_ALIGNED(bus_addr, algn);
}

static inline int range_straddles_page_boundary(phys_addr_t p, size_t size)
{
	unsigned long next_bfn, xen_pfn = XEN_PFN_DOWN(p);
	unsigned int i, nr_pages = XEN_PFN_UP(xen_offset_in_page(p) + size);

	next_bfn = pfn_to_bfn(xen_pfn);

	for (i = 1; i < nr_pages; i++)
		if (pfn_to_bfn(++xen_pfn) != ++next_bfn)
			return 1;

	return 0;
}

static struct io_tlb_pool *xen_swiotlb_find_pool(struct device *dev,
						 dma_addr_t dma_addr)
{
	unsigned long bfn = XEN_PFN_DOWN(dma_to_phys(dev, dma_addr));
	unsigned long xen_pfn = bfn_to_local_pfn(bfn);
	phys_addr_t paddr = (phys_addr_t)xen_pfn << XEN_PAGE_SHIFT;

	/* If the address is outside our domain, it CAN
	 * have the same virtual address as another address
	 * in our domain. Therefore _only_ check address within our domain.
	 */
	if (pfn_valid(PFN_DOWN(paddr)))
		return swiotlb_find_pool(dev, paddr);
	return NULL;
}

#ifdef CONFIG_X86
int __init xen_swiotlb_fixup(void *buf, unsigned long nslabs)
{
	int rc;
	unsigned int order = get_order(IO_TLB_SEGSIZE << IO_TLB_SHIFT);
	unsigned int i, dma_bits = order + PAGE_SHIFT;
	dma_addr_t dma_handle;
	phys_addr_t p = virt_to_phys(buf);

	BUILD_BUG_ON(IO_TLB_SEGSIZE & (IO_TLB_SEGSIZE - 1));
	BUG_ON(nslabs % IO_TLB_SEGSIZE);

	i = 0;
	do {
		do {
			rc = xen_create_contiguous_region(
				p + (i << IO_TLB_SHIFT), order,
				dma_bits, &dma_handle);
		} while (rc && dma_bits++ < MAX_DMA_BITS);
		if (rc)
			return rc;

		i += IO_TLB_SEGSIZE;
	} while (i < nslabs);
	return 0;
}

static void *
xen_swiotlb_alloc_coherent(struct device *dev, size_t size,
		dma_addr_t *dma_handle, gfp_t flags, unsigned long attrs)
{
	u64 dma_mask = dev->coherent_dma_mask;
	int order = get_order(size);
	phys_addr_t phys;
	void *ret;

	/* Align the allocation to the Xen page size */
	size = ALIGN(size, XEN_PAGE_SIZE);

	ret = (void *)__get_free_pages(flags, get_order(size));
	if (!ret)
		return ret;
	phys = virt_to_phys(ret);

	*dma_handle = xen_phys_to_dma(dev, phys);
	if (*dma_handle + size - 1 > dma_mask ||
	    range_straddles_page_boundary(phys, size) ||
	    range_requires_alignment(phys, size)) {
		if (xen_create_contiguous_region(phys, order, fls64(dma_mask),
				dma_handle) != 0)
			goto out_free_pages;
		SetPageXenRemapped(virt_to_page(ret));
	}

	memset(ret, 0, size);
	return ret;

out_free_pages:
	free_pages((unsigned long)ret, get_order(size));
	return NULL;
}

static void
xen_swiotlb_free_coherent(struct device *dev, size_t size, void *vaddr,
		dma_addr_t dma_handle, unsigned long attrs)
{
	phys_addr_t phys = virt_to_phys(vaddr);
	int order = get_order(size);

	/* Convert the size to actually allocated. */
	size = ALIGN(size, XEN_PAGE_SIZE);

	if (WARN_ON_ONCE(dma_handle + size - 1 > dev->coherent_dma_mask) ||
	    WARN_ON_ONCE(range_straddles_page_boundary(phys, size) ||
			 range_requires_alignment(phys, size)))
	    	return;

	if (TestClearPageXenRemapped(virt_to_page(vaddr)))
		xen_destroy_contiguous_region(phys, order);
	free_pages((unsigned long)vaddr, get_order(size));
}
#endif /* CONFIG_X86 */

#if defined(CONFIG_X86) && defined(CONFIG_CONTIG_ALLOC)
/*
 * swiotlb hands out bounce buffers from fixed IO_TLB_SEGSIZE segments, so it
 * cannot serve a mapping larger than swiotlb_max_mapping_size().  A PV guest
 * has no IOMMU of its own to stitch scattered machine frames together either,
 * so such a mapping fails outright and the driver gets nothing.
 *
 * Make the caller's own pages machine contiguous instead of bouncing.  A
 * bounce buffer is not usable here: a driver may map a buffer and only then
 * write it, without a dma_sync_single_for_device() in between, and would then
 * hand the device stale bytes.  Exchanging in place keeps one copy of the data,
 * so what the CPU writes is what the device reads.
 *
 * XENMEM_exchange works in whole orders, so the pages between the end of the
 * mapping and the end of the order have to be ours for the duration.  Claim
 * them with alloc_contig_range() and hand them back once the exchange is done;
 * their frames stay contiguous with the mapping, which costs nothing.
 */
static bool xen_contig_inplace = true;
static int __init xen_contig_inplace_setup(char *s)
{
	return kstrtobool(s, &xen_contig_inplace);
}
early_param("xen_contig_inplace", xen_contig_inplace_setup);

static dma_addr_t xen_make_range_contiguous(struct device *dev, phys_addr_t phys,
					    size_t size)
{
	unsigned long nr = PFN_UP(size);
	unsigned int order = order_base_2(nr);
	unsigned long count = 1UL << order;
	unsigned long pad = count - nr;
	struct page *filler = NULL;
	unsigned long *pfns, i;
	dma_addr_t dma_addr;
	void *saved;
	int rc;

	if (!xen_contig_inplace)
		return DMA_MAPPING_ERROR;

	/* Page migration and the exchange hypercall both sleep. */
	if (!preemptible())
		return DMA_MAPPING_ERROR;

	if (!PAGE_ALIGNED(phys) || !pfn_valid(PHYS_PFN(phys)))
		return DMA_MAPPING_ERROR;

	pfns = kvmalloc_array(count, sizeof(*pfns), GFP_KERNEL);
	if (!pfns)
		return DMA_MAPPING_ERROR;

	for (i = 0; i < nr; i++)
		pfns[i] = PHYS_PFN(phys) + i;

	/*
	 * Pad up to the order with pages of our own, so the exchange never
	 * touches memory the caller does not own.
	 */
	if (pad) {
		filler = alloc_contig_pages(pad, GFP_KERNEL | __GFP_NOWARN,
					    dev_to_node(dev), NULL);
		if (!filler) {
			dev_warn_once(dev, "no %lu page range free to pad a %zu byte mapping\n",
				      pad, size);
			goto free_pfns;
		}
		for (i = 0; i < pad; i++)
			pfns[nr + i] = page_to_pfn(filler) + i;
	}

	/* The exchange scrubs the frames, so carry the caller's data across. */
	saved = vmalloc(size);
	if (!saved)
		goto free_filler;
	memcpy(saved, phys_to_virt(phys), size);

	rc = xen_remap_pfns_contiguous(pfns, count, fls64(dma_get_mask(dev)),
				       &dma_addr);
	if (rc) {
		dev_warn_once(dev,
			      "cannot make an order-%u region contiguous (%d); Xen's memop-max-order may be lower than that\n",
			      order, rc);
		goto free_saved;
	}

	memcpy(phys_to_virt(phys), saved, size);
	vfree(saved);
	if (filler)
		free_contig_range(page_to_pfn(filler), pad);
	kvfree(pfns);

	if (!dma_capable(dev, dma_addr, size, true))
		return DMA_MAPPING_ERROR;

	dev_info(dev, "made a %zu byte mapping contiguous in place\n", size);
	return dma_addr;

free_saved:
	vfree(saved);
free_filler:
	if (filler)
		free_contig_range(page_to_pfn(filler), pad);
free_pfns:
	kvfree(pfns);
	return DMA_MAPPING_ERROR;
}
#else
static dma_addr_t xen_make_range_contiguous(struct device *dev, phys_addr_t phys,
					    size_t size)
{
	return DMA_MAPPING_ERROR;
}
#endif /* CONFIG_X86 && CONFIG_CONTIG_ALLOC */

/*
 * Map a single buffer of the indicated size for DMA in streaming mode.  The
 * physical address to use is returned.
 *
 * Once the device is given the dma address, the device owns this memory until
 * either xen_swiotlb_unmap_page or xen_swiotlb_dma_sync_single is performed.
 */
static dma_addr_t xen_swiotlb_map_page(struct device *dev, struct page *page,
				unsigned long offset, size_t size,
				enum dma_data_direction dir,
				unsigned long attrs)
{
	phys_addr_t map, phys = page_to_phys(page) + offset;
	dma_addr_t dev_addr = xen_phys_to_dma(dev, phys);

	BUG_ON(dir == DMA_NONE);
	/*
	 * If the address happens to be in the device's DMA window,
	 * we can safely return the device addr and not worry about bounce
	 * buffering it.
	 */
	if (dma_capable(dev, dev_addr, size, true) &&
	    !dma_kmalloc_needs_bounce(dev, size, dir) &&
	    !range_straddles_page_boundary(phys, size) &&
		!xen_arch_need_swiotlb(dev, phys, dev_addr) &&
		!is_swiotlb_force_bounce(dev))
		goto done;

	/*
	 * Oh well, have to allocate and map a bounce buffer.
	 */
	trace_swiotlb_bounced(dev, dev_addr, size);

	if (size > swiotlb_max_mapping_size(dev)) {
		dma_addr_t contig = xen_make_range_contiguous(dev, phys, size);

		if (contig != DMA_MAPPING_ERROR) {
			dev_addr = contig;
			goto done;
		}
	}

	map = swiotlb_tbl_map_single(dev, phys, size, 0, dir, attrs);
	if (map == (phys_addr_t)DMA_MAPPING_ERROR)
		return DMA_MAPPING_ERROR;

	phys = map;
	dev_addr = xen_phys_to_dma(dev, map);

	/*
	 * Ensure that the address returned is DMA'ble
	 */
	if (unlikely(!dma_capable(dev, dev_addr, size, true))) {
		__swiotlb_tbl_unmap_single(dev, map, size, dir,
				attrs | DMA_ATTR_SKIP_CPU_SYNC,
				swiotlb_find_pool(dev, map));
		return DMA_MAPPING_ERROR;
	}

done:
	if (!dev_is_dma_coherent(dev) && !(attrs & DMA_ATTR_SKIP_CPU_SYNC)) {
		if (pfn_valid(PFN_DOWN(dma_to_phys(dev, dev_addr))))
			arch_sync_dma_for_device(phys, size, dir);
		else
			xen_dma_sync_for_device(dev, dev_addr, size, dir);
	}
	return dev_addr;
}

/*
 * Unmap a single streaming mode DMA translation.  The dma_addr and size must
 * match what was provided for in a previous xen_swiotlb_map_page call.  All
 * other usages are undefined.
 *
 * After this call, reads by the cpu to the buffer are guaranteed to see
 * whatever the device wrote there.
 */
static void xen_swiotlb_unmap_page(struct device *hwdev, dma_addr_t dev_addr,
		size_t size, enum dma_data_direction dir, unsigned long attrs)
{
	phys_addr_t paddr = xen_dma_to_phys(hwdev, dev_addr);
	struct io_tlb_pool *pool;

	BUG_ON(dir == DMA_NONE);

	if (!dev_is_dma_coherent(hwdev) && !(attrs & DMA_ATTR_SKIP_CPU_SYNC)) {
		if (pfn_valid(PFN_DOWN(dma_to_phys(hwdev, dev_addr))))
			arch_sync_dma_for_cpu(paddr, size, dir);
		else
			xen_dma_sync_for_cpu(hwdev, dev_addr, size, dir);
	}

	/* NOTE: We use dev_addr here, not paddr! */
	pool = xen_swiotlb_find_pool(hwdev, dev_addr);
	if (pool)
		__swiotlb_tbl_unmap_single(hwdev, paddr, size, dir,
					   attrs, pool);
}

static void
xen_swiotlb_sync_single_for_cpu(struct device *dev, dma_addr_t dma_addr,
		size_t size, enum dma_data_direction dir)
{
	phys_addr_t paddr = xen_dma_to_phys(dev, dma_addr);
	struct io_tlb_pool *pool;

	if (!dev_is_dma_coherent(dev)) {
		if (pfn_valid(PFN_DOWN(dma_to_phys(dev, dma_addr))))
			arch_sync_dma_for_cpu(paddr, size, dir);
		else
			xen_dma_sync_for_cpu(dev, dma_addr, size, dir);
	}

	pool = xen_swiotlb_find_pool(dev, dma_addr);
	if (pool)
		__swiotlb_sync_single_for_cpu(dev, paddr, size, dir, pool);
}

static void
xen_swiotlb_sync_single_for_device(struct device *dev, dma_addr_t dma_addr,
		size_t size, enum dma_data_direction dir)
{
	phys_addr_t paddr = xen_dma_to_phys(dev, dma_addr);
	struct io_tlb_pool *pool;

	pool = xen_swiotlb_find_pool(dev, dma_addr);
	if (pool)
		__swiotlb_sync_single_for_device(dev, paddr, size, dir, pool);

	if (!dev_is_dma_coherent(dev)) {
		if (pfn_valid(PFN_DOWN(dma_to_phys(dev, dma_addr))))
			arch_sync_dma_for_device(paddr, size, dir);
		else
			xen_dma_sync_for_device(dev, dma_addr, size, dir);
	}
}

/*
 * Unmap a set of streaming mode DMA translations.  Again, cpu read rules
 * concerning calls here are the same as for swiotlb_unmap_page() above.
 */
static void
xen_swiotlb_unmap_sg(struct device *hwdev, struct scatterlist *sgl, int nelems,
		enum dma_data_direction dir, unsigned long attrs)
{
	struct scatterlist *sg;
	int i;

	BUG_ON(dir == DMA_NONE);

	for_each_sg(sgl, sg, nelems, i)
		xen_swiotlb_unmap_page(hwdev, sg->dma_address, sg_dma_len(sg),
				dir, attrs);

}

static int
xen_swiotlb_map_sg(struct device *dev, struct scatterlist *sgl, int nelems,
		enum dma_data_direction dir, unsigned long attrs)
{
	struct scatterlist *sg;
	int i;

	BUG_ON(dir == DMA_NONE);

	for_each_sg(sgl, sg, nelems, i) {
		sg->dma_address = xen_swiotlb_map_page(dev, sg_page(sg),
				sg->offset, sg->length, dir, attrs);
		if (sg->dma_address == DMA_MAPPING_ERROR)
			goto out_unmap;
		sg_dma_len(sg) = sg->length;
	}

	return nelems;
out_unmap:
	xen_swiotlb_unmap_sg(dev, sgl, i, dir, attrs | DMA_ATTR_SKIP_CPU_SYNC);
	sg_dma_len(sgl) = 0;
	return -EIO;
}

static void
xen_swiotlb_sync_sg_for_cpu(struct device *dev, struct scatterlist *sgl,
			    int nelems, enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nelems, i) {
		xen_swiotlb_sync_single_for_cpu(dev, sg->dma_address,
				sg->length, dir);
	}
}

static void
xen_swiotlb_sync_sg_for_device(struct device *dev, struct scatterlist *sgl,
			       int nelems, enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nelems, i) {
		xen_swiotlb_sync_single_for_device(dev, sg->dma_address,
				sg->length, dir);
	}
}

static dma_addr_t xen_swiotlb_direct_map_resource(struct device *dev,
						  phys_addr_t paddr,
						  size_t size,
						  enum dma_data_direction dir,
						  unsigned long attrs)
{
	dma_addr_t dma_addr = paddr;

	if (unlikely(!dma_capable(dev, dma_addr, size, false))) {
		dev_err_once(dev,
			     "DMA addr %pad+%zu overflow (mask %llx, bus limit %llx).\n",
			     &dma_addr, size, *dev->dma_mask, dev->bus_dma_limit);
		WARN_ON_ONCE(1);
		return DMA_MAPPING_ERROR;
	}

	return dma_addr;
}

/*
 * Return whether the given device DMA address mask can be supported
 * properly.  For example, if your device can only drive the low 24-bits
 * during bus mastering, then you would pass 0x00ffffff as the mask to
 * this function.
 */
static int
xen_swiotlb_dma_supported(struct device *hwdev, u64 mask)
{
	return xen_phys_to_dma(hwdev, default_swiotlb_limit()) <= mask;
}

const struct dma_map_ops xen_swiotlb_dma_ops = {
#ifdef CONFIG_X86
	.alloc = xen_swiotlb_alloc_coherent,
	.free = xen_swiotlb_free_coherent,
#else
	.alloc = dma_direct_alloc,
	.free = dma_direct_free,
#endif
	.sync_single_for_cpu = xen_swiotlb_sync_single_for_cpu,
	.sync_single_for_device = xen_swiotlb_sync_single_for_device,
	.sync_sg_for_cpu = xen_swiotlb_sync_sg_for_cpu,
	.sync_sg_for_device = xen_swiotlb_sync_sg_for_device,
	.map_sg = xen_swiotlb_map_sg,
	.unmap_sg = xen_swiotlb_unmap_sg,
	.map_page = xen_swiotlb_map_page,
	.unmap_page = xen_swiotlb_unmap_page,
	.dma_supported = xen_swiotlb_dma_supported,
	.mmap = dma_common_mmap,
	.get_sgtable = dma_common_get_sgtable,
	.alloc_pages_op = dma_common_alloc_pages,
	.free_pages = dma_common_free_pages,
	.max_mapping_size = swiotlb_max_mapping_size,
	.map_resource = xen_swiotlb_direct_map_resource,
};
