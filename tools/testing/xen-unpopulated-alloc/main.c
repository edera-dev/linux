// SPDX-License-Identifier: GPL-2.0
/*
 * Runs drivers/xen/unpopulated-alloc.c against a fake memory map of four
 * nodes.  After every step, check() asserts that each pfn the allocator was
 * given is free in exactly one place or held by a caller.
 */
#include "shim.h"
#include "generated/unpopulated-alloc.c"

#define BLOCK		XEN_UNPOPULATED_BLOCK_PAGES
#define FLOOR		MAPPING_FLOOR_PAGES

#define EXPECT(cond)							\
	do {								\
		if (!(cond)) {						\
			fprintf(stderr, "%s:%d: %s\n", __FILE__,	\
				__LINE__, #cond);			\
			exit(1);					\
		}							\
	} while (0)

/*
 * Extra memory regions, as arch_xen_unpopulated_init() hands them over, and
 * the node of their pages.
 */
static const struct {
	unsigned long start, nr_pages;
	int node;
} regions[] = {
	{ 1000, 200 * BLOCK + 300, 0 },		/* unaligned at both ends */
	{ 400 * BLOCK, 100 * BLOCK, 1 },
	{ 600 * BLOCK, 4 * BLOCK, 2 },		/* node 3 from NODE_SPLIT on */
	{ 700 * BLOCK + 10, 100, 0 },		/* holds no whole block */
	{ 1000 * BLOCK + 7, 700 * BLOCK, 1 },
	{ 900 * BLOCK + 7, 3 * BLOCK, 2 },	/* starts off a block */
};

#define NODE_SPLIT	(601 * BLOCK + 188)
#define DEVICE_PFN	(1792 * BLOCK)	/* fill_list() carves from here on */

static unsigned char known[NR_PFNS];	/* given to the allocator */
static unsigned char held[NR_PFNS];	/* taken and not given back */
static unsigned char is_free[NR_PFNS];	/* filled in by check() */

static struct page *pages[600000];
static struct page *heads[1024];

static void mark_free(unsigned long pfn)
{
	EXPECT(!is_free[pfn]);
	is_free[pfn] = 1;
}

static void check(void)
{
	unsigned long pfn, head, nr;
	struct page *pg;
	int node;

	EXPECT(!list_lock.locked);
	memset(is_free, 0, sizeof(is_free));
	for (node = 0; node < MAX_NUMNODES; node++) {
		nr = 0;
		for (pg = page_list[node]; pg; pg = pg->zone_device_data) {
			EXPECT(page_to_nid(pg) == node);
			mark_free(page_to_pfn(pg));
			nr++;
		}
		EXPECT(nr == list_count[node]);

		nr = 0;
		for (pg = block_list[node]; pg; pg = pg->zone_device_data) {
			head = page_to_pfn(pg);
			EXPECT(head % BLOCK == 0);
			for (pfn = head; pfn < head + BLOCK; pfn++) {
				EXPECT(memmap[pfn].nid == node);
				EXPECT(!memmap[pfn].zone_device);
				mark_free(pfn);
			}
			nr++;
		}
		EXPECT(nr == block_count[node]);
	}

	for (pfn = 0; pfn < NR_PFNS; pfn++) {
		if (known[pfn] || memmap[pfn].zone_device)
			EXPECT(is_free[pfn] + held[pfn] == 1);
		else
			EXPECT(!is_free[pfn] && !held[pfn]);
	}
}

static void hold(unsigned int nr_pages, unsigned char set)
{
	while (nr_pages--)
		held[page_to_pfn(pages[nr_pages])] = set;
}

static int alloc_node(unsigned int nr_pages, int node)
{
	int ret = xen_alloc_unpopulated_pages_node(nr_pages, pages, node);

	if (!ret)
		hold(nr_pages, 1);
	check();
	return ret;
}

static int alloc_any(unsigned int nr_pages)
{
	int ret = xen_alloc_unpopulated_pages(nr_pages, pages);

	if (!ret)
		hold(nr_pages, 1);
	check();
	return ret;
}

/* Each block taken must be aligned on its pfn and on one node. */
static unsigned int take_blocks(unsigned int nr_blocks)
{
	unsigned int nr = xen_alloc_unpopulated_blocks(heads, nr_blocks);
	unsigned long i, pfn;

	for (i = 0; i < nr; i++) {
		unsigned long head = page_to_pfn(heads[i]);

		EXPECT(head % BLOCK == 0);
		for (pfn = head; pfn < head + BLOCK; pfn++) {
			EXPECT(memmap[pfn].nid == page_to_nid(heads[i]));
			held[pfn] = 1;
		}
	}
	check();
	return nr;
}

/* Give back the pages of the first @nr_blocks blocks, as dom0's balloon does. */
static void give_back_blocks(unsigned int nr_blocks)
{
	unsigned long i;

	for (i = 0; i < nr_blocks * BLOCK; i++) {
		struct page *pg = heads[i / BLOCK] + i % BLOCK;

		held[page_to_pfn(pg)] = 0;
		xen_free_unpopulated_pages(1, &pg);
	}
	check();
}

static unsigned long all_pool_pages(void)
{
	unsigned long nr = 0;
	int node;

	for (node = 0; node < MAX_NUMNODES; node++)
		nr += pool_pages(node);
	return nr;
}

static void setup(void)
{
	unsigned long i, pfn;

	for (i = 0; i < ARRAY_SIZE(regions); i++) {
		for (pfn = regions[i].start;
		     pfn < regions[i].start + regions[i].nr_pages; pfn++) {
			memmap[pfn].nid = regions[i].node;
			known[pfn] = 1;
		}
	}
	for (pfn = NODE_SPLIT;
	     pfn < regions[2].start + regions[2].nr_pages; pfn++)
		memmap[pfn].nid = 3;
	next_device_pfn = DEVICE_PFN;

	EXPECT(unpopulated_init() == 0);
	for (i = 0; i < ARRAY_SIZE(regions); i++)
		xen_unpopulated_add_range(regions[i].start, regions[i].nr_pages);
	check();
}

/* Give back what the test still holds, then drop the whole layout. */
static void teardown(void)
{
	struct page *pg;
	unsigned long pfn;
	unsigned int i;
	int node;

	for (pfn = 0; pfn < NR_PFNS; pfn++) {
		if (held[pfn]) {
			pg = pfn_to_page(pfn);
			held[pfn] = 0;
			xen_free_unpopulated_pages(1, &pg);
		}
	}
	check();

	for (node = 0; node < MAX_NUMNODES; node++) {
		page_list[node] = NULL;
		list_count[node] = 0;
		block_list[node] = NULL;
		block_count[node] = 0;
	}
	for (i = 0; i < nr_remapped; i++) {
		kfree(remapped[i]->owner);
		kfree(remapped[i]);
	}
	nr_remapped = 0;
	target_resource = NULL;
	nr_warnings = 0;
	memset(memmap, 0, sizeof(memmap));
	memset(known, 0, sizeof(known));
}

static void test_whole_blocks_on_one_node_go_on_block_list(void)
{
	EXPECT(block_count[0] == 200);
	EXPECT(block_count[1] == 100 + 699);
	EXPECT(block_count[2] == 1 + 2);
	EXPECT(block_count[3] == 2);
}

static void test_spare_blocks_leave_each_node_its_floor(void)
{
	unsigned long spare0 = (pool_pages(0) - FLOOR) / BLOCK;
	unsigned long spare1 = (pool_pages(1) - FLOOR) / BLOCK;

	EXPECT(spare0 < block_count[0] && spare1 < block_count[1]);
	EXPECT(pool_pages(2) < FLOOR && pool_pages(3) < FLOOR);
	EXPECT(xen_unpopulated_spare_blocks() == spare0 + spare1);
}

static void test_blocks_come_from_the_lowest_node_first(void)
{
	unsigned long spare0 = (pool_pages(0) - FLOOR) / BLOCK;
	unsigned long i;

	EXPECT(take_blocks(spare0 + 1) == spare0 + 1);
	for (i = 0; i < spare0; i++)
		EXPECT(page_to_nid(heads[i]) == 0);
	EXPECT(page_to_nid(heads[spare0]) == 1);
}

static void test_taking_every_block_stops_at_each_floor(void)
{
	unsigned long spare = xen_unpopulated_spare_blocks();

	EXPECT(take_blocks(ARRAY_SIZE(heads)) == spare);
	EXPECT(xen_unpopulated_spare_blocks() == 0);
	EXPECT(pool_pages(0) >= FLOOR && pool_pages(0) < FLOOR + BLOCK);
	EXPECT(pool_pages(1) >= FLOOR && pool_pages(1) < FLOOR + BLOCK);
}

static void test_an_empty_page_list_takes_one_block(void)
{
	EXPECT(alloc_node(list_count[0] + 1, 0) == 0);
	EXPECT(block_count[0] == 200 - 1);
	EXPECT(list_count[0] == BLOCK - 1);
}

static void test_blocks_given_back_are_no_longer_spare(void)
{
	unsigned long spare1 = (pool_pages(1) - FLOOR) / BLOCK;

	EXPECT(take_blocks(100) == 100);
	give_back_blocks(100);
	EXPECT(block_count[0] == 200 - 100);
	EXPECT(xen_unpopulated_spare_blocks() == 100 + spare1);
}

static void test_node_alloc_never_takes_another_nodes_pages(void)
{
	EXPECT(alloc_node(pool_pages(2) + 1, 2) == -ENOMEM);
}

static void test_mappings_past_every_pool_carve_a_section(void)
{
	EXPECT(alloc_any(all_pool_pages() + 10) == 0);
	EXPECT(pages[0]->zone_device);
	EXPECT(all_pool_pages() == PAGES_PER_SECTION - 10);
}

#define TEST(fn)	{ fn, #fn }

static const struct {
	void (*fn)(void);
	const char *name;
} tests[] = {
	TEST(test_whole_blocks_on_one_node_go_on_block_list),
	TEST(test_spare_blocks_leave_each_node_its_floor),
	TEST(test_blocks_come_from_the_lowest_node_first),
	TEST(test_taking_every_block_stops_at_each_floor),
	TEST(test_an_empty_page_list_takes_one_block),
	TEST(test_blocks_given_back_are_no_longer_spare),
	TEST(test_node_alloc_never_takes_another_nodes_pages),
	TEST(test_mappings_past_every_pool_carve_a_section),
};

int main(void)
{
	unsigned int i;

	printf("1..%zu\n", ARRAY_SIZE(tests));
	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		setup();
		tests[i].fn();
		teardown();
		printf("ok %u %s\n", i + 1, tests[i].name);
	}
	return 0;
}
