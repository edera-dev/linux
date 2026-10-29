/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Userspace stand-ins for what drivers/xen/unpopulated-alloc.c uses from the
 * kernel.
 *
 * memmap[] is a fake struct page for each pfn, with the node it belongs to.
 * allocate_resource() hands out ranges from next_device_pfn on, and
 * memremap_pages() marks them ZONE_DEVICE.
 */
#ifndef XEN_UNPOPULATED_ALLOC_SHIM_H
#define XEN_UNPOPULATED_ALLOC_SHIM_H

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define __init
#define __initdata
#define __weak			__attribute__((weak))
#define EXPORT_SYMBOL(sym)
#define early_initcall(fn) \
	static int (*const fn##_initcall)(void) __attribute__((used)) = fn

static int nr_warnings;

#define pr_err(...)		(nr_warnings++, fprintf(stderr, __VA_ARGS__))
#define pr_warn(...)		(nr_warnings++, fprintf(stderr, __VA_ARGS__))

#define PAGE_SHIFT		12
#define PAGE_SIZE		(1UL << PAGE_SHIFT)
#define PFN_DOWN(x)		((x) >> PAGE_SHIFT)
#define PAGES_PER_SECTION	(1UL << (27 - PAGE_SHIFT))
#define XEN_UNPOPULATED_BLOCK_PAGES	(1UL << (21 - PAGE_SHIFT))

#define MAX_NUMNODES		4
#define NUMA_NO_NODE		(-1)

#define min(a, b)		((a) < (b) ? (a) : (b))
#define ARRAY_SIZE(a)		(sizeof(a) / sizeof((a)[0]))
#define ALIGN(x, a)		(((x) + (a) - 1) & ~((typeof(x))(a) - 1))
#define IS_ALIGNED(x, a)	(((x) & ((typeof(x))(a) - 1)) == 0)
#define round_up(x, y)		ALIGN(x, y)
#define DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))

/* The tests run on one thread, so a lock only checks that it is balanced. */
struct mutex {
	bool locked;
};

#define DEFINE_MUTEX(name)	struct mutex name

static inline void mutex_lock(struct mutex *lock)
{
	assert(!lock->locked);
	lock->locked = true;
}

static inline void mutex_unlock(struct mutex *lock)
{
	assert(lock->locked);
	lock->locked = false;
}

#define kzalloc_obj(obj)	calloc(1, sizeof(obj))
#define kfree(ptr)		free(ptr)

#define IS_ERR(ptr)		((unsigned long)(ptr) >= (unsigned long)-4095)
#define PTR_ERR(ptr)		((long)(ptr))

struct dev_pagemap;

struct page {
	struct dev_pagemap *pgmap;
	void *zone_device_data;
	int nid;
	bool zone_device;
};

#define NR_PFNS			(1UL << 20)

static struct page memmap[NR_PFNS];

static inline struct page *pfn_to_page(unsigned long pfn)
{
	assert(pfn < NR_PFNS);
	return &memmap[pfn];
}

static inline unsigned long page_to_pfn(const struct page *page)
{
	return page - memmap;
}

static inline int page_to_nid(const struct page *page)
{
	return page->nid;
}

static inline struct page *virt_to_page(const void *addr)
{
	return pfn_to_page(PFN_DOWN((unsigned long)addr));
}

static inline int numa_node_id(void)
{
	return 0;
}

struct resource {
	const char *name;
	unsigned long start, end, flags;
};

#define IORESOURCE_MEM		0x00000200
#define IORESOURCE_BUSY		0x80000000

static struct resource iomem_resource;
static unsigned long next_device_pfn;

static inline int allocate_resource(struct resource *root,
				    struct resource *new, unsigned long size,
				    unsigned long min, unsigned long max,
				    unsigned long align, void *alignf,
				    void *alignf_data)
{
	unsigned long pfn = ALIGN(next_device_pfn, PFN_DOWN(align));

	next_device_pfn = pfn + PFN_DOWN(size);
	assert(next_device_pfn <= NR_PFNS);
	new->start = pfn << PAGE_SHIFT;
	new->end = new->start + size - 1;
	return 0;
}

static inline int request_resource(struct resource *root,
				   struct resource *new)
{
	return 0;
}

static inline void release_resource(struct resource *old)
{
}

struct range {
	unsigned long start, end;
};

static inline struct range mhp_get_pluggable_range(bool need_mapping)
{
	return (struct range){ .start = 0, .end = ~0UL };
}

enum memory_type {
	MEMORY_DEVICE_GENERIC,
};

struct dev_pagemap {
	struct range range;
	int nr_range;
	enum memory_type type;
	void *owner;
};

/* Every dev_pagemap passed to memremap_pages(), to free between tests. */
static struct dev_pagemap *remapped[8];
static unsigned int nr_remapped;

static inline void *memremap_pages(struct dev_pagemap *pgmap, int nid)
{
	unsigned long pfn;

	assert(nr_remapped < ARRAY_SIZE(remapped));
	remapped[nr_remapped++] = pgmap;
	for (pfn = PFN_DOWN(pgmap->range.start);
	     pfn <= PFN_DOWN(pgmap->range.end); pfn++) {
		memmap[pfn].pgmap = pgmap;
		memmap[pfn].nid = nid;
		memmap[pfn].zone_device = true;
	}
	return (void *)pgmap->range.start;
}

static inline bool xen_domain(void)
{
	return true;
}

static inline int xen_alloc_ballooned_pages(unsigned int nr_pages,
					    struct page **pages)
{
	abort();
}

static inline void xen_free_ballooned_pages(unsigned int nr_pages,
					    struct page **pages)
{
	abort();
}

#endif /* XEN_UNPOPULATED_ALLOC_SHIM_H */
