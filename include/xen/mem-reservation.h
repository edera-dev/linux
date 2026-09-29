/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Xen memory reservation utilities.
 *
 * Copyright (c) 2003, B Dragovic
 * Copyright (c) 2003-2004, M Williamson, K Fraser
 * Copyright (c) 2005 Dan M. Smith, IBM Corporation
 * Copyright (c) 2010 Daniel Kiper
 * Copyright (c) 2018 Oleksandr Andrushchenko, EPAM Systems Inc.
 */

#ifndef _XENMEM_RESERVATION_H
#define _XENMEM_RESERVATION_H

#include <linux/highmem.h>

#include <xen/page.h>

extern bool xen_scrub_pages;

static inline void xenmem_reservation_scrub_page(struct page *page)
{
	if (xen_scrub_pages)
		clear_highpage(page);
}

#ifdef CONFIG_XEN_HAVE_PVMMU
void __xenmem_reservation_va_mapping_update(unsigned long count,
					    struct page **pages,
					    xen_pfn_t *frames);

void __xenmem_reservation_va_mapping_reset(unsigned long count,
					   struct page **pages);

void __xenmem_reservation_va_mapping_update_contig(unsigned long count,
						   struct page *page,
						   xen_pfn_t frame);

void __xenmem_reservation_va_mapping_reset_contig(unsigned long count,
						  struct page *page);

int __xenmem_reservation_p2m_prealloc(unsigned long count, struct page *page);
#endif

static inline void xenmem_reservation_va_mapping_update(unsigned long count,
							struct page **pages,
							xen_pfn_t *frames)
{
#ifdef CONFIG_XEN_HAVE_PVMMU
	if (xen_pv_domain())
		__xenmem_reservation_va_mapping_update(count, pages, frames);
#endif
}

static inline void xenmem_reservation_va_mapping_reset(unsigned long count,
						       struct page **pages)
{
#ifdef CONFIG_XEN_HAVE_PVMMU
	if (xen_pv_domain())
		__xenmem_reservation_va_mapping_reset(count, pages);
#endif
}

/*
 * Prepare a run of @count physically contiguous pages starting at @page to be
 * mapped by xenmem_reservation_va_mapping_update_contig(), which cannot
 * allocate. Call this before asking Xen to populate the run: it is the only
 * step that can fail, and failing it after the populate would strand the
 * frames Xen handed over.
 */
static inline int xenmem_reservation_p2m_prealloc(unsigned long count,
						  struct page *page)
{
#ifdef CONFIG_XEN_HAVE_PVMMU
	if (xen_pv_domain())
		return __xenmem_reservation_p2m_prealloc(count, page);
#endif
	return 0;
}

/*
 * Variants for a run of @count physically contiguous pages starting at @page,
 * backed by the equally contiguous machine frames starting at @frame.  Xen
 * reports only the base frame of a multi-page extent, so the ordered populate
 * path has no frame array to pass.
 */
static inline void xenmem_reservation_va_mapping_update_contig(unsigned long count,
							       struct page *page,
							       xen_pfn_t frame)
{
#ifdef CONFIG_XEN_HAVE_PVMMU
	if (xen_pv_domain())
		__xenmem_reservation_va_mapping_update_contig(count, page, frame);
#endif
}

static inline void xenmem_reservation_va_mapping_reset_contig(unsigned long count,
							      struct page *page)
{
#ifdef CONFIG_XEN_HAVE_PVMMU
	if (xen_pv_domain())
		__xenmem_reservation_va_mapping_reset_contig(count, page);
#endif
}

int xenmem_reservation_increase(int count, xen_pfn_t *frames);

int xenmem_reservation_increase_order(int count, xen_pfn_t *frames,
				      unsigned int order);

int xenmem_reservation_decrease(int count, xen_pfn_t *frames);

#endif
