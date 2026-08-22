/*
 * MMU operations common to all auto-translated physmap guests.
 *
 * Copyright (C) 2015 Citrix Systems R&D Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include <asm/xen/hypercall.h>
#include <asm/xen/hypervisor.h>

#include <xen/xen.h>
#include <xen/xen-ops.h>
#include <xen/page.h>
#include <xen/interface/xen.h>
#include <xen/interface/memory.h>
#include <xen/balloon.h>

/* Issue up to this many GFN mapping requests in a batch at a time. */
#define XLATE_BATCH_SIZE 16

typedef void (*xen_gfn_fn_t)(unsigned long gfn, void *data);

/* Break down the pages in 4KB chunk and call fn for each gfn */
static void xen_for_each_gfn(struct page **pages, unsigned nr_gfn,
			     xen_gfn_fn_t fn, void *data)
{
	unsigned long xen_pfn = 0;
	struct page *page;
	int i;

	for (i = 0; i < nr_gfn; i++) {
		if ((i % XEN_PFN_PER_PAGE) == 0) {
			page = pages[i / XEN_PFN_PER_PAGE];
			xen_pfn = page_to_xen_pfn(page);
		}
		fn(pfn_to_gfn(xen_pfn++), data);
	}
}

struct remap_pfn {
	struct mm_struct *mm;
	struct page **pages;
	pgprot_t prot;
	unsigned long i;
};

static int remap_pfn_fn(pte_t *ptep, unsigned long addr, void *data)
{
	struct remap_pfn *r = data;
	struct page *page = r->pages[r->i];
	pte_t pte = pte_mkspecial(pfn_pte(page_to_pfn(page), r->prot));

	set_pte_at(r->mm, addr, ptep, pte);
	r->i++;

	return 0;
}

struct remap_data {
	xen_pfn_t *fgfn; /* foreign domain's gfn */
	int nr_fgfn; /* Number of foreign gfn left to map */
	pgprot_t prot;
	domid_t  domid;
	struct vm_area_struct *vma;
	int index;
	struct page **pages;
	struct xen_remap_gfn_info *info;
	int *err_ptr;
	int mapped;

	/* Hypercall parameters.
	 *
	 * idxs: source GFNs in foreign domain P2M (combined with XENMAPSPACE_gmfn_foreign)
	 * gpfns: destination GFNs in dom0's P2M. Extracted from pages[]
	 *
	 * h_idxs and h_gpfns exist as pairs: we map into dom0's GFN
	 * the same MFN for the GFN in the foreign domain.
	 */
	int h_errs[XLATE_BATCH_SIZE];
	xen_ulong_t h_idxs[XLATE_BATCH_SIZE];
	xen_pfn_t h_gpfns[XLATE_BATCH_SIZE];

	int h_iter;
};

static void setup_hparams(unsigned long gfn, void *data)
{
	struct remap_data *info = data;

	info->h_idxs[info->h_iter] = *info->fgfn;
	info->h_gpfns[info->h_iter] = gfn;
	info->h_errs[info->h_iter] = 0;

	info->h_iter++;
	info->fgfn++;
}

int xen_xlate_remap_gfn_array(struct vm_area_struct *vma,
			      unsigned long addr,
			      xen_pfn_t *gfn, int nr,
			      int *err_ptr, pgprot_t prot,
			      unsigned domid,
			      struct page **pages)
{
	struct remap_data data;
	int page_off = 0;

	BUILD_BUG_ON(XLATE_BATCH_SIZE % XEN_PFN_PER_PAGE != 0);

	/* Kept here for the purpose of making sure code doesn't break
	   x86 PVOPS */
	BUG_ON(!((vma->vm_flags & (VM_PFNMAP | VM_IO)) == (VM_PFNMAP | VM_IO)));

	data.fgfn = gfn;
	data.nr_fgfn = nr;
	data.prot  = prot;
	data.domid = domid;
	data.vma   = vma;
	data.pages = pages;
	data.index = 0;
	data.err_ptr = err_ptr;
	data.mapped = 0;

	while (data.nr_fgfn > 0) {
		int batch = min_t(int, XLATE_BATCH_SIZE, data.nr_fgfn); /* xen GFN units */
		/* NOTE: on ARM, page size may be larger than in Xen */
		int batch_pages = DIV_ROUND_UP(batch, XEN_PFN_PER_PAGE);
		unsigned long batch_range = (unsigned long)batch_pages << PAGE_SHIFT;

		struct xen_add_to_physmap_range xatp = {
			.domid = DOMID_SELF,
			.foreign_domid = domid,
			.space = XENMAPSPACE_gmfn_foreign,
			.size = batch,
		};

		int rc, i;

		data.h_iter = 0;
		xen_for_each_gfn(&pages[page_off], batch, setup_hparams, &data);
		data.nr_fgfn -= batch;

		set_xen_guest_handle(xatp.idxs,  data.h_idxs);
		set_xen_guest_handle(xatp.gpfns, data.h_gpfns);
		set_xen_guest_handle(xatp.errs,  data.h_errs);

		rc = HYPERVISOR_memory_op(XENMEM_add_to_physmap_range, &xatp);

		/*
		 * Note: The hypercall will return 0 in most of the case if even if
		 * all the fgmfn are not mapped. We still have to update the pte
		 * as the userspace may decide to continue.
		 */
		if (rc == 0) {
			struct remap_pfn r = {
				.mm    = vma->vm_mm,
				.pages = &pages[page_off],
				.prot  = prot,
				.i     = 0,
			};
			int walk_err = apply_to_page_range(vma->vm_mm, addr,
				      batch_range,
				      remap_pfn_fn, &r);
			if (walk_err < 0)
				return walk_err;
		}

		/* err_ptr expected to have one error status per Xen PFN */
		for (i = 0; i < batch; i++) {
			int err = (rc < 0) ? rc : data.h_errs[i];
			*(data.err_ptr++) = err;
			if (!err)
				data.mapped++;
		}

		addr += batch_range;
		page_off += batch_pages;
		cond_resched();
	}

      return data.mapped;
}
EXPORT_SYMBOL_GPL(xen_xlate_remap_gfn_array);

static void unmap_gfn(unsigned long gfn, void *data)
{
	struct xen_remove_from_physmap xrp;

	xrp.domid = DOMID_SELF;
	xrp.gpfn = gfn;
	(void)HYPERVISOR_memory_op(XENMEM_remove_from_physmap, &xrp);
}

int xen_xlate_unmap_gfn_range(struct vm_area_struct *vma,
			      int nr, struct page **pages)
{
	xen_for_each_gfn(pages, nr, unmap_gfn, NULL);

	return 0;
}
EXPORT_SYMBOL_GPL(xen_xlate_unmap_gfn_range);

struct map_balloon_pages {
	xen_pfn_t *pfns;
	unsigned int idx;
};

static void setup_balloon_gfn(unsigned long gfn, void *data)
{
	struct map_balloon_pages *info = data;

	info->pfns[info->idx++] = gfn;
}

/**
 * xen_xlate_map_ballooned_pages - map a new set of ballooned pages
 * @gfns: returns the array of corresponding GFNs
 * @virt: returns the virtual address of the mapped region
 * @nr_grant_frames: number of GFNs
 * @return 0 on success, error otherwise
 *
 * This allocates a set of ballooned pages and maps them into the
 * kernel's address space.
 */
int __init xen_xlate_map_ballooned_pages(xen_pfn_t **gfns, void **virt,
					 unsigned long nr_grant_frames)
{
	struct page **pages;
	xen_pfn_t *pfns;
	void *vaddr;
	struct map_balloon_pages data;
	int rc;
	unsigned long nr_pages;

	BUG_ON(nr_grant_frames == 0);
	nr_pages = DIV_ROUND_UP(nr_grant_frames, XEN_PFN_PER_PAGE);
	pages = kcalloc(nr_pages, sizeof(pages[0]), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	pfns = kcalloc(nr_grant_frames, sizeof(pfns[0]), GFP_KERNEL);
	if (!pfns) {
		kfree(pages);
		return -ENOMEM;
	}
	rc = xen_alloc_unpopulated_pages(nr_pages, pages);
	if (rc) {
		pr_warn("%s Couldn't balloon alloc %ld pages rc:%d\n", __func__,
			nr_pages, rc);
		kfree(pages);
		kfree(pfns);
		return rc;
	}

	data.pfns = pfns;
	data.idx = 0;
	xen_for_each_gfn(pages, nr_grant_frames, setup_balloon_gfn, &data);

	vaddr = vmap(pages, nr_pages, 0, PAGE_KERNEL);
	if (!vaddr) {
		pr_warn("%s Couldn't map %ld pages rc:%d\n", __func__,
			nr_pages, rc);
		xen_free_unpopulated_pages(nr_pages, pages);
		kfree(pages);
		kfree(pfns);
		return -ENOMEM;
	}
	kfree(pages);

	*gfns = pfns;
	*virt = vaddr;

	return 0;
}

/* Used by the privcmd module, but has to be built-in on ARM */
int xen_remap_vma_range(struct vm_area_struct *vma, unsigned long addr, unsigned long len)
{
	struct remap_pfn r = {
		.mm = vma->vm_mm,
		.pages = vma->vm_private_data,
		.prot = vma->vm_page_prot,
	};

	return apply_to_page_range(vma->vm_mm, addr, len, remap_pfn_fn, &r);
}
EXPORT_SYMBOL_GPL(xen_remap_vma_range);
