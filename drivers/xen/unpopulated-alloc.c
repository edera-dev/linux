// SPDX-License-Identifier: GPL-2.0
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/memremap.h>
#include <linux/numa.h>
#include <linux/slab.h>

#include <asm/page.h>

#include <xen/balloon.h>
#include <xen/page.h>
#include <xen/xen.h>

/*
 * Free pages are kept on per-node lists indexed by Linux node id.  Each
 * fill_list() call grabs a fresh section-aligned IOMEM region and
 * registers it with memremap_pages() against a specific node, so all
 * struct pages in that section report that node via page_to_nid().
 * Backends that learn the host node of a foreign frame (via
 * xenbus_ring_host_node) can request placeholder pages from the
 * matching pool so page_to_nid agrees with the actual host placement.
 *
 * A single mutex covers all per-node lists.  Alloc/free are
 * connect-time events on the I/O backends and not contention-sensitive.
 */
static DEFINE_MUTEX(list_lock);
static struct page *page_list[MAX_NUMNODES];
static unsigned int list_count[MAX_NUMNODES];

static struct resource *target_resource;

static int xen_unpopulated_clamp_node(int node)
{
	if (node == NUMA_NO_NODE || node < 0 || node >= MAX_NUMNODES)
		return numa_node_id();
	return node;
}

/* Pages to subtract from the memory count when setting balloon target. */
unsigned long xen_unpopulated_pages __initdata;

/*
 * If arch is not happy with system "iomem_resource" being used for
 * the region allocation it can provide it's own view by creating specific
 * Xen resource with unused regions of guest physical address space provided
 * by the hypervisor.
 */
int __weak __init arch_xen_unpopulated_init(struct resource **res)
{
	*res = &iomem_resource;

	return 0;
}

static int fill_list(unsigned int nr_pages, int node)
{
	struct dev_pagemap *pgmap;
	struct resource *res, *tmp_res = NULL;
	void *vaddr;
	unsigned int i, alloc_pages = round_up(nr_pages, PAGES_PER_SECTION);
	struct range mhp_range;
	int ret;

	res = kzalloc(sizeof(*res), GFP_KERNEL);
	if (!res)
		return -ENOMEM;

	res->name = "Xen scratch";
	res->flags = IORESOURCE_MEM | IORESOURCE_BUSY;

	mhp_range = mhp_get_pluggable_range(true);

	ret = allocate_resource(target_resource, res,
				alloc_pages * PAGE_SIZE, mhp_range.start, mhp_range.end,
				PAGES_PER_SECTION * PAGE_SIZE, NULL, NULL);
	if (ret < 0) {
		pr_err("Cannot allocate new IOMEM resource\n");
		goto err_resource;
	}

	/*
	 * Reserve the region previously allocated from Xen resource to avoid
	 * re-using it by someone else.
	 */
	if (target_resource != &iomem_resource) {
		tmp_res = kzalloc(sizeof(*tmp_res), GFP_KERNEL);
		if (!tmp_res) {
			ret = -ENOMEM;
			goto err_insert;
		}

		tmp_res->name = res->name;
		tmp_res->start = res->start;
		tmp_res->end = res->end;
		tmp_res->flags = res->flags;

		ret = request_resource(&iomem_resource, tmp_res);
		if (ret < 0) {
			pr_err("Cannot request resource %pR (%d)\n", tmp_res, ret);
			kfree(tmp_res);
			goto err_insert;
		}
	}

	pgmap = kzalloc(sizeof(*pgmap), GFP_KERNEL);
	if (!pgmap) {
		ret = -ENOMEM;
		goto err_pgmap;
	}

	pgmap->type = MEMORY_DEVICE_GENERIC;
	pgmap->range = (struct range) {
		.start = res->start,
		.end = res->end,
	};
	pgmap->nr_range = 1;
	pgmap->owner = res;

#ifdef CONFIG_XEN_HAVE_PVMMU
        /*
         * memremap will build page tables for the new memory so
         * the p2m must contain invalid entries so the correct
         * non-present PTEs will be written.
         *
         * If a failure occurs, the original (identity) p2m entries
         * are not restored since this region is now known not to
         * conflict with any devices.
         */
	if (xen_pv_domain()) {
		xen_pfn_t pfn = PFN_DOWN(res->start);

		for (i = 0; i < alloc_pages; i++) {
			if (!set_phys_to_machine(pfn + i, INVALID_P2M_ENTRY)) {
				pr_warn("set_phys_to_machine() failed, no memory added\n");
				ret = -ENOMEM;
				goto err_memremap;
			}
                }
	}
#endif

	/*
	 * Register the section against @node so page_to_nid() of any
	 * page in this section reports that value.  Grant-map operations
	 * later install foreign MFNs into these slots; as long as the
	 * caller picks the section matching the foreign MFN's host node
	 * (which is the contract callers of xen_alloc_unpopulated_pages_node
	 * are expected to honour), page_to_nid is correct by construction.
	 */
	vaddr = memremap_pages(pgmap, node);
	if (IS_ERR(vaddr)) {
		pr_err("Cannot remap memory range\n");
		ret = PTR_ERR(vaddr);
		goto err_memremap;
	}

	for (i = 0; i < alloc_pages; i++) {
		struct page *pg = virt_to_page(vaddr + PAGE_SIZE * i);

		pg->zone_device_data = page_list[node];
		page_list[node] = pg;
		list_count[node]++;
	}

	return 0;

err_memremap:
	kfree(pgmap);
err_pgmap:
	if (tmp_res) {
		release_resource(tmp_res);
		kfree(tmp_res);
	}
err_insert:
	release_resource(res);
err_resource:
	kfree(res);
	return ret;
}

/**
 * xen_alloc_unpopulated_pages_node - alloc unpopulated pages on a node
 * @nr_pages: Number of pages
 * @pages: pages returned
 * @node: Preferred Linux node id, or NUMA_NO_NODE for current CPU's node
 *
 * The returned pages are drawn from a per-node pool registered with
 * memremap_pages() against @node, so page_to_nid() reports @node for
 * every returned page.  Callers that know the host node of a foreign
 * frame should pass it here to keep page_to_nid in agreement with the
 * actual host placement after a subsequent grant-map.
 *
 * Returns 0 on success, error otherwise.
 */
int xen_alloc_unpopulated_pages_node(unsigned int nr_pages, struct page **pages,
				     int node)
{
	unsigned int i;
	int ret = 0;

	/*
	 * Fallback to default behavior if we do not have any suitable resource
	 * to allocate required region from and as the result we won't be able to
	 * construct pages.
	 */
	if (!target_resource)
		return xen_alloc_ballooned_pages(nr_pages, pages);

	node = xen_unpopulated_clamp_node(node);

	mutex_lock(&list_lock);
	if (list_count[node] < nr_pages) {
		ret = fill_list(nr_pages - list_count[node], node);
		if (ret)
			goto out;
	}

	for (i = 0; i < nr_pages; i++) {
		struct page *pg = page_list[node];

		BUG_ON(!pg);
		page_list[node] = pg->zone_device_data;
		list_count[node]--;
		pages[i] = pg;

#ifdef CONFIG_XEN_HAVE_PVMMU
		if (xen_pv_domain()) {
			ret = xen_alloc_p2m_entry(page_to_pfn(pg));
			if (ret < 0) {
				unsigned int j;

				for (j = 0; j <= i; j++) {
					pages[j]->zone_device_data = page_list[node];
					page_list[node] = pages[j];
					list_count[node]++;
				}
				goto out;
			}
		}
#endif
	}

out:
	mutex_unlock(&list_lock);
	return ret;
}
EXPORT_SYMBOL(xen_alloc_unpopulated_pages_node);

/**
 * xen_alloc_unpopulated_pages - alloc unpopulated pages
 * @nr_pages: Number of pages
 * @pages: pages returned
 * @return 0 on success, error otherwise
 *
 * Equivalent to xen_alloc_unpopulated_pages_node() with the current
 * CPU's node as the preference.
 */
int xen_alloc_unpopulated_pages(unsigned int nr_pages, struct page **pages)
{
	return xen_alloc_unpopulated_pages_node(nr_pages, pages, numa_node_id());
}
EXPORT_SYMBOL(xen_alloc_unpopulated_pages);

/**
 * xen_free_unpopulated_pages - return unpopulated pages
 * @nr_pages: Number of pages
 * @pages: pages to return
 *
 * Each page returns to the pool of the node it was originally allocated
 * from, identified via page_to_nid().  Sections registered to a
 * specific node yield pages whose nid reflects that node, so freed
 * pages naturally land back in the matching list.
 */
void xen_free_unpopulated_pages(unsigned int nr_pages, struct page **pages)
{
	unsigned int i;

	if (!target_resource) {
		xen_free_ballooned_pages(nr_pages, pages);
		return;
	}

	mutex_lock(&list_lock);
	for (i = 0; i < nr_pages; i++) {
		int node = xen_unpopulated_clamp_node(page_to_nid(pages[i]));

		pages[i]->zone_device_data = page_list[node];
		page_list[node] = pages[i];
		list_count[node]++;
	}
	mutex_unlock(&list_lock);
}
EXPORT_SYMBOL(xen_free_unpopulated_pages);

static int __init unpopulated_init(void)
{
	int ret;

	if (!xen_domain())
		return -ENODEV;

	ret = arch_xen_unpopulated_init(&target_resource);
	if (ret) {
		pr_err("xen:unpopulated: Cannot initialize target resource\n");
		target_resource = NULL;
	}

	return ret;
}
early_initcall(unpopulated_init);
