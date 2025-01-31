/*
 * Multi Operating System (mOS)
 * Copyright (c) 2016, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/list.h>
#include <linux/bootmem.h>
#include <linux/memblock.h>
#include <linux/sizes.h>
#include <linux/sched.h>	/* For task_struct */
#include <linux/mutex.h>
#include <linux/mman.h>
#include <linux/slab.h>
#include <linux/perf_event.h>
#include <linux/printk.h>
#include <linux/mos.h>
#include <linux/vmalloc.h>
#include <linux/rmap.h>
#include <linux/pkeys.h>
#include <asm/setup.h>
#include "lwkmem.h"

#undef pr_fmt
#define pr_fmt(fmt)	"mOS-mem: " fmt
#define STRBUF_LEN		(256)

int lwkmem_debug;
static size_t lwkmem_n_nodes;

#define ADDR_MASK 0x000ffffffffff000
#define PG2M_MASK 0x000fffffffe00000
#define PG1G_MASK 0x000fffffc0000000

/*
 * LWK page size masks and shifts
 */
char *kind_str[kind_last] = {"4k", "2m", "4m", "1g"};
int64_t kind_size[kind_last] = {SZ_4K, SZ_2M, SZ_4M, SZ_1G};
char *lwkmem_type_str[lwkmem_type_last] = {"dram", "mcdram"};

unsigned long lwk_page_shift[kind_last] = {12, 21, 22, 30};
static unsigned long lwk_page_size[kind_last] = {
			(1UL << 12), (1UL << 21),
			(1UL << 22), (1UL << 30)};
static unsigned long lwk_page_mask[kind_last] = {
			~((1UL << 12) - 1), ~((1UL << 21) - 1),
			~((1UL << 22) - 1), ~((1UL << 30) - 1)};

#define MIN_CHUNK_SIZE	(SZ_2M)

static uint64_t pgran_available(struct lwk_process_granule *g)
{
	return g->granule->length - g->offset;
}

int lwkmem_get_debug_level(void)
{
	return lwkmem_debug;
}

void lwkmem_set_debug_level(int level)
{
	lwkmem_debug = level;
}

/*
 * Return number of dTLB entries of this kind on this architecture
 */
static int num_dTLB(enum lwkmem_kind_t kind)
{
	int rc = 0;
	static int limit = 5;

	switch (kind) {
	case kind_1g:
		rc = tlb_lld_1g[ENTRIES] ? tlb_lld_1g[ENTRIES] : 16;
		break;
	case kind_4m:
#if defined(CONFIG_X86_64) || defined(CONFIG_X86_PAE)
		rc = 0; /* 4m is not supported in IA-32e paging mode */
#else
		rc = tlb_lld_4m[ENTRIES];
#endif
		break;
	case kind_2m:
#if defined(CONFIG_X86_64) || defined(CONFIG_X86_PAE)
		rc = tlb_lld_2m[ENTRIES] ? tlb_lld_2m[ENTRIES] : 128;
#else
		rc = 0;
#endif
		break;
	case kind_4k:
		rc = tlb_lld_4k[ENTRIES] ? tlb_lld_4k[ENTRIES] : 256;
		break;
	case kind_last:
		rc = 0;
		break;
	default:
		if (limit > 0) {
			pr_err("Unknown num_dTLB(%d)! Called from %pS <- %pS\n",
				kind,  __builtin_return_address(0),
				__builtin_return_address(1));
			limit--;
		}
	}

	return rc;

} /* end of num_dTLB() */

#ifdef LWKMEM_DEBUG_ENABLED
/*
 * List all free memory blocks during early boot
 */
static void __init memblock_dump_free(void)
{
	uint64_t idx;
	phys_addr_t this_start, this_end, size;
	int p_nid;
	int i = 0;

	for_each_free_mem_range(idx, NUMA_NO_NODE, MEMBLOCK_NONE, &this_start,
			&this_end, &p_nid) {
		size = this_end - this_start + 1;
		memblock_dbg(" free[0x%x]\t[%#016llx-%#016llx], 0x%llx bytes on node %d\n",
			i++, (unsigned long long)this_start,
			(unsigned long long)this_end, size, p_nid);
	}

} /* end of memblock_dump_free() */
#endif /* LWKMEM_DEBUG_ENABLED */

/*
 * List and summarize the memory granules in a list
 */
static void dump_granule_list(struct list_head *dump_list)
{
	struct mos_lwk_mem_granule *g;
	uint64_t total_bytes = 0;
	unsigned num_granules = 0;

	list_for_each_entry(g, dump_list, list) {
		pr_info("\t[%pK-%pK], 0x%llx bytes (%lld MiB), owner %d nid %d\n",
			g->base, g->base + g->length - 1, g->length,
			g->length >> 20, g->owner, g->nid);
		total_bytes = total_bytes + g->length;
		num_granules++;
	}
	pr_info("Total %llu bytes (%llu MB) in %d granules\n", total_bytes,
		total_bytes >> 20, num_granules);

} /* end of dump_granule_list() */

#ifdef LWKMEM_DEBUG_ENABLED

static void dump_process_mem_list(struct mos_process_t *mos_p)
{
	struct lwk_process_granule *g;

	list_for_each_entry(g, &mos_p->lwkmem_list, list) {
		pr_info("  [%p-%p] len=0x%010llx (%9llu MB) offset=0x%010llx avail=%9llu MB owner=%d\n",
			g->granule->base,
			g->granule->base + g->granule->length - 1,
			g->granule->length,
			g->granule->length >> 20,
			g->offset,
			pgran_available(g) >> 20,
			g->granule->owner);
	}
}
/*
 * List and summarize the block lists
 */
static void dump_block_lists(struct mos_process_t *mos_p)
{
	struct blk_list *elt;
	int64_t total_mem_free[kind_last] = {0, 0, 0, 0};
	int64_t total_mem_assigned[kind_last] = {0, 0, 0, 0};
	unsigned long total_blks_free[kind_last] = {0, 0, 0, 0};
	unsigned long total_blks_assigned[kind_last] = {0, 0, 0, 0};
	enum lwkmem_kind_t k;

	pr_info("Block lists for process %d\n", mos_p->tgid);
	for (k = kind_4k; k < kind_last; k++) {
		list_for_each_entry(elt, &(mos_p->blk_list[k]), list) {
			unsigned long addr =
				(unsigned long)elt->phys->base +
				elt->offset;
			unsigned long sz = elt->num_blks * kind_size[k];

			if (LWKMEM_DEBUG_EXTREME)
				pr_info("  [%s] [%#016lx-%#016lx] [%#016lx-%#016lx] pid=%d nid=%d %3lld blocks %s\n",
					kind_str[k],
					addr,
					addr + sz - 1,
					elt->vma_addr,
					elt->free ? 0 : elt->vma_addr + sz - 1,
					elt->phys->owner,
					elt->phys->nid,
					elt->num_blks,
					elt->free ? "free" : "assigned");

			if (elt->free) {
				total_blks_free[k] += elt->num_blks;
				total_mem_free[k] += sz;
			} else {
				total_blks_assigned[k] += elt->num_blks;
				total_mem_assigned[k] += sz;
			}
		}
		pr_info("  [%s] Free: %ld blocks (%lld M)   Assigned %ld blocks (%lld M)\n",
			kind_str[k],
			total_blks_free[k],
			total_mem_free[k] >> 20,
			total_blks_assigned[k],
			total_mem_assigned[k] >> 20);
		if (total_blks_free[k] != mos_p->num_blks[k])
			pr_info("  [%s] (!) Inconsistent state detected (%ld vs. %lld).\n",
				kind_str[k],
				total_blks_free[k],
				mos_p->num_blks[k]);

	}
	pr_info("All block summary for process %d\n", mos_p->tgid);
	pr_info("  Free: %ld blocks (%lld M)   Assigned %ld blocks (%lld M)\n",
		total_blks_free[kind_4k] + total_blks_free[kind_2m] +
		total_blks_free[kind_4m] + total_blks_free[kind_1g],
		(total_mem_free[kind_4k] + total_mem_free[kind_2m] +
		total_mem_free[kind_4m] + total_mem_free[kind_1g]) >> 20,
		total_blks_assigned[kind_4k] + total_blks_assigned[kind_2m] +
		total_blks_assigned[kind_4m] + total_blks_assigned[kind_1g],
		(total_mem_assigned[kind_4k] + total_mem_assigned[kind_2m] +
		total_mem_assigned[kind_4m] +
		total_mem_assigned[kind_1g]) >> 20);

} /* end of dump_block_lists() */
#endif /* LWKMEM_DEBUG_ENABLED */

/*
 * During early boot, designate regions of memory per the lwkmem kernel
 * argument.  These granules are retained in a list; list node data is
 * stored in the granules (boot memory) since it is not used for anything
 * else during this phase.
 */
__initdata LIST_HEAD(mos_lwk_boot_list);

static uint64_t __init _lwkmem_designate_by_nid(
			 uint64_t lwk_mem_requested, int nid)
{
	struct mos_lwk_mem_granule *g;
	uint64_t lwk_mem_needed = 0;
	uint64_t lwk_mem_designated = 0;
	uint64_t block_size, try_size;

	/* Round up to the next 2 MB boundary */
	lwk_mem_needed = roundup(lwk_mem_requested, MIN_CHUNK_SIZE);

	memblock_dbg("Designating %lld (%lld) bytes from nid %d\n",
		     lwk_mem_requested, lwk_mem_needed, nid);

	/* No point in searching for something much larger than we need. */

	block_size = roundup_pow_of_two(lwk_mem_needed);

	/* While memory is desired from this NUMA domain, attempt to grab
	 * the largest block possible.
	 */

	while ((lwk_mem_needed > 0) && (block_size > 0)) {

		/* Is there a block of this size we can request? */
		try_size = min(lwk_mem_needed, block_size);

		memblock_dbg("Is there a block of size %lld? I need %lld more\n",
			     block_size, lwk_mem_needed);

		if (memblock_find_in_range_node(try_size, block_size, 0,
			BOOTMEM_ALLOC_ACCESSIBLE, nid, MEMBLOCK_NONE)) {

			/* Yes! Grab and use it */
			g = memblock_virt_alloc_try_nid_nopanic(try_size,
				block_size, 0, BOOTMEM_ALLOC_ACCESSIBLE, nid);
			if (!g) {
				pr_warn("Designating a block of %lld bytes failed\n",
					try_size);
				goto allocerr;
			}

			g->base = g;
			g->length = try_size;
			g->nid = nid;
			memblock_dbg("granule 0x%16p, 0x%llx bytes (%lld) nid=%d\n",
				     g->base, g->length, g->length, nid);
			list_add_tail(&g->list, &mos_lwk_boot_list);

			lwk_mem_needed -= try_size;
			lwk_mem_designated += try_size;
		} else {
			/* No block of that size available try a smaller block
			 * size.
			 */
			block_size >>= 1;
		}
	}

	if (lwk_mem_needed > 0)
		pr_warn("Could not designate %lld bytes of memory in NUMA domain %d\n",
			lwk_mem_requested, nid);

 allocerr:
	return lwk_mem_designated;

}

static int __init mos_lwkmem_setup(char *s)
{
	uint64_t total_designated, total_requested, designated, requested;
	char *nidstr, *memstr;
	int failures, nid, rc;
	static char tmp[COMMAND_LINE_SIZE] __initdata;

	total_designated = total_requested = failures = 0;

	/* Determine the number of NUMA domains. */
	for_each_online_node(nid)
		if (lwkmem_n_nodes < (nid + 1))
			lwkmem_n_nodes = nid + 1;

	pr_info("There are %ld on-line NUMA domains.\n", lwkmem_n_nodes);

	memblock_dbg("Early memblock info ---------------------------------------\n");
	memblock_dump_all();
	memblock_dump_free();

	s = strcpy(tmp, s);

	while ((nidstr = strsep(&s, ","))) {

		memstr = strchr(nidstr, ':');
		if (!memstr) {
			nid = NUMA_NO_NODE;
			memstr = nidstr;
		} else {
			*(memstr++) = '\0';
			rc = kstrtoint(nidstr, 0, &nid);
			if (rc || nid < 0 || nid >= lwkmem_n_nodes) {
				pr_warn("(!) invalid NUMA id: \"%s\"\n",
					nidstr);
				nid = NUMA_NO_NODE;
			}
		}

		requested = memparse(memstr, 0);
		total_requested += requested;

		if (nid == NUMA_NO_NODE) {
			requested /= lwkmem_n_nodes;
			for_each_online_node(nid) {
				designated =
					_lwkmem_designate_by_nid(requested, nid);
				total_designated += designated;
				if (designated < requested)
					failures++;
			}
		} else {
			designated = _lwkmem_designate_by_nid(requested, nid);
			total_designated += designated;
			if (designated < requested)
				failures++;
		}
	}

	if (failures > 0)
		pr_warn("Only designated %lld of %lld bytes of LWK memory.\n",
			total_designated, total_requested);
	else
		pr_info("Designated %lld bytes of LWK memory.\n",
			total_designated);

	return -failures;
}

__setup("lwkmem=", mos_lwkmem_setup);

static int __init mos_lwkmem_debug_setup(char *s)
{
	if (!get_option(&s, &lwkmem_debug))
		pr_warn("(!) could not parse lwkmem_debug=%s\n", s);

	return 0;
}

__setup("lwkmem_debug=", mos_lwkmem_debug_setup);

static int lwkmem_process_init(struct mos_process_t *mosp)
{
	enum lwkmem_kind_t k;
	enum lwkmem_type_t m;

	mosp->lwkmem = 0;
	mosp->brk = mosp->brk_end = 0;
	mosp->lwkmem_brk_disable = false;
	mosp->max_page_size = SZ_1G;
	mosp->heap_page_size = SZ_2M;
	mosp->mcdram_minimum = 0;
	mosp->mcdram_exempt_flags = 0;
	mosp->lwkmem_mmap_fixed = 0;
	mosp->lwkmem_next_addr = 0x300000000000;
	mosp->brk_clear_len = 4096;
	mosp->lwkmem_interleave_disable = false;
	mosp->lwkmem_load_elf_segs = true;

	/* Don't randomize address space for LWK processes! */
	current->personality |= ADDR_NO_RANDOMIZE;

	INIT_LIST_HEAD(&mosp->lwkmem_list);
	for (k = kind_4k; k < kind_last; k++) {
		int n;

		INIT_LIST_HEAD(&mosp->blk_list[k]);
		mosp->num_blks[k] = 0;
		for_each_node_mask(n, node_online_map)
			mosp->blks_allocated[k][n] = 0;
	}

	mosp->report_blks_allocated = 0;
	for (m = lwkmem_dram; m < lwkmem_type_last; m++) {
		for (k = kind_4k; k < kind_last; k++)
			mosp->domain_order_index[m][k] = 0;
		mosp->domain_info_len[m] = 0;
	}

	return 0;
}

static int partition_task_mem(struct mos_process_t *, int64_t);

static int lwkmem_process_start(struct mos_process_t *mosp)
{
	int rc;

	rc = partition_task_mem(mosp, mosp->lwkmem);
	if (rc)
		return -ENOMEM;

	if (LWKMEM_DEBUG)
		dump_process_mem_list(mosp);

	return 0;
}

static struct mos_process_callbacks_t lwkmem_callbacks = {
	.mos_process_init = lwkmem_process_init,
	.mos_process_start = lwkmem_process_start,
	.mos_process_exit = lwkmem_release,
};

static int lwkmem_brk_disable_cb(const char *ignored,
				 struct mos_process_t *mosp)
{
	pr_info("(!) lwkmem brk support is disabled.\n");
	mosp->lwkmem_brk_disable = true;

	return 0;
}

static int lwkmem_max_pg_size_cb(const char *val, struct mos_process_t *mosp)
{
	if (!val)
		goto err;

	if (strcasecmp(val, "4k") == 0)
		mosp->max_page_size = SZ_4K;
	else if (strcasecmp(val, "2m") == 0)
		mosp->max_page_size = SZ_2M;
	else if (strcasecmp(val, "4m") == 0)
		mosp->max_page_size = SZ_4M;
	else if (strcasecmp(val, "1g") == 0)
		mosp->max_page_size = SZ_1G;
	else
		goto err;

	pr_info("Maximum LWK page size set to %s\n", val);

	return 0;

 err:
	pr_err("(!) Invalid maximum page size: %s\n", val);
	return -EINVAL;
}

static int lwkmem_heap_pg_size_cb(const char *val, struct mos_process_t *mosp)
{
	if (!val)
		goto err;

	if (strcasecmp(val, "4k") == 0)
		mosp->heap_page_size = SZ_4K;
	else if (strcasecmp(val, "2m") == 0)
		mosp->heap_page_size = SZ_2M;
	else if (strcasecmp(val, "4m") == 0)
		mosp->heap_page_size = SZ_4M;
	else if (strcasecmp(val, "1g") == 0)
		mosp->heap_page_size = SZ_1G;
	else
		goto err;

	pr_info("LWK heap page size set to %s\n", val);

	return 0;

 err:
	pr_err("(!) Invalid LWK heap page size: %s\n", val);
	return -EINVAL;
}

static int lwkmem_mcdram_enable_cb(const char *val,
				 struct mos_process_t *mosp)
{
	int rc;
	char *tok, *opt = 0, *elem;

	if (!val)
		goto invalid;

	opt = kstrdup(val, GFP_KERNEL);
	if (!opt)
		return -ENOMEM;

	/* Format: lwkmem-mcdram-enable=<size>[;x:<flags>]
	 *
	 * where:
	 *
	 *   <size> identifies the MCDRAM preference line; allocations equal
	 *       to or larger than this size will first try to use MCDRAM as
	 *       the backing storage.
	 *   <flags> identifies MAP_* flags that are exempt, i.e. will NOT
	 *       prefer MCDRAM as a backing store.
	 *
	 * Example: lwkmem-mcdram-enable=1;x:20000 will greedily back all
	 *   memory requests with MCDRAM until it is exhausted, except for
	 *   requests that are identified with MAP_STACK (x20000).
	 */

	while ((tok = strsep(&opt, ";"))) {
		if (tok[0] == 'x') {
			elem = strsep(&tok, ":");
			if (elem[1])
				goto invalid;
			elem = strsep(&tok, ":");
			rc = kstrtoul(elem, 16, &mosp->mcdram_exempt_flags);
			if (rc)
				goto invalid;
		} else {
			rc = kstrtoul(tok, 0, &mosp->mcdram_minimum);
			if (rc)
				goto invalid;
		}
	}

	if (LWKMEM_DEBUG)
		pr_info("(!) MCDRAM line set to %ld / 0x%lx.  Exempt flags:0x%lx\n",
			mosp->mcdram_minimum, mosp->mcdram_minimum,
			mosp->mcdram_exempt_flags);

	kfree(opt);
	return 0;

 invalid:
	kfree(opt);
	pr_err("(!) Illegal value (%s) detected in %s.\n", val, __func__);
	return -EINVAL;
}

static int lwkmem_mmap_fixed_cb(const char *val, struct mos_process_t *mosp)
{
	int rc;

	if (!val)
		goto invalid;

	rc = kstrtoul(val, 0, &mosp->lwkmem_mmap_fixed);

	if (rc)
		goto invalid;

	if (LWKMEM_DEBUG)
		pr_info("(*) lwkmem-mmap-fixed=%lx\n",
			mosp->lwkmem_mmap_fixed);

	return 0;

 invalid:
	pr_err("(!) Illegal value (%s) detected in %s.\n", val, __func__);
	return -EINVAL;
}

static int lwkmem_blocks_allocated_cb(const char *val,
					struct mos_process_t *mosp)
{
	mosp->report_blks_allocated = 1;

	if (LWKMEM_DEBUG)
		pr_info("(*) lwkmem-blocks-allocated\n");

	return 0;
}

static int lwkmem_brk_clear_len_cb(const char *val, struct mos_process_t *mosp)
{
	int rc;

	if (!val)
		goto invalid;

	rc = kstrtol(val, 0, &mosp->brk_clear_len);

	if (rc)
		goto invalid;

	if (LWKMEM_DEBUG)
		pr_info("(*) lwkmem-brk-clear-len=%lx\n",
			mosp->brk_clear_len);

	return 0;

 invalid:
	pr_err("(!) Illegal value (%s) detected in %s.\n", val, __func__);
	return -EINVAL;
}

static int lwkmem_interleave_disable_cb(const char *val,
					struct mos_process_t *mosp)
{
	mosp->lwkmem_interleave_disable = true;

	if (LWKMEM_DEBUG)
		pr_info("(*) lwkmem-interleave-disable\n");

	return 0;
}

static int lwkmem_load_elf_disable_cb(const char *val,
				      struct mos_process_t *mosp)
{
	mosp->lwkmem_load_elf_segs = false;

	if (LWKMEM_DEBUG)
		pr_info("(*) lwkmem_load_elf_segs set to false\n");

	return 0;
}

/*
 * Later during boot, gather all of the memory granules into a
 * consolidated list.  The list meta data is migrated from the
 * memory granules into vmalloc'd data.
 */
LIST_HEAD(mos_lwk_memory_list);

static int __init mos_collect_bootmem(void)
{
	struct mos_lwk_mem_granule *g;
	unsigned long total_bytes = 0;
	struct page *p;
	long i, nr_pages;

	list_for_each_entry(g, &mos_lwk_boot_list, list) {
		struct mos_lwk_mem_granule *newg;

		newg = vmalloc(sizeof(struct mos_lwk_mem_granule));
		if (!newg)
			goto collect_err;

		/* Mark as free */
		g->owner = -1;

		memcpy(newg, g, sizeof(struct mos_lwk_mem_granule));

		/*
		** Space for the struct page entries has already been allocated
		** by the kernel. Make sure the flags are as we need them.
		*/
		p = virt_to_page(g->base);
		nr_pages = g->length / PAGE_SIZE;
		for (i = 0; i < nr_pages; i++, p++) {
			/* Init each struct page in this granule */
			SetPageReserved(p);
			SetPagePrivate(p);
			set_bit(PG_writeback, &p->flags);
			SetPageActive(p);
			SetPageUnevictable(p);
			SetPageMlocked(p);
			p->private = _LWKPG;	/* Mark it as an LWK page */

			/* Not needed at the moment, but may come in handy
			   during debugging. */
			p->index = (pgoff_t)((unsigned long)(g->base) +
				(i * PAGE_SIZE));
			p->active = page_to_pfn(p);
		}

		/* This sorts granules by size, not phys location. May want to
		 * change this sometime in the future. */
		list_add_tail(&newg->list, &mos_lwk_memory_list);
		total_bytes += newg->length;
	}

	if (LWKMEM_DEBUG) {
		memblock_dbg("Here are the granules reserved for lwkmem\n");
		dump_granule_list(&mos_lwk_memory_list);
	}

	list_for_each_entry(g, &mos_lwk_memory_list, list) {
		/* Clear the granule */
		memzero_explicit(g->base, g->length);
	}

	mos_register_process_callbacks(&lwkmem_callbacks);

	mos_register_option_callback("lwkmem-brk-disable",
				     lwkmem_brk_disable_cb);
	mos_register_option_callback("lwkmem-max-page-size",
				     lwkmem_max_pg_size_cb);
	mos_register_option_callback("lwkmem-mcdram-enable",
				     lwkmem_mcdram_enable_cb);
	mos_register_option_callback("lwkmem-mmap-fixed",
				     lwkmem_mmap_fixed_cb);
	mos_register_option_callback("lwkmem-heap-page-size",
				     lwkmem_heap_pg_size_cb);
	mos_register_option_callback("lwkmem-blocks-allocated",
				     lwkmem_blocks_allocated_cb);
	mos_register_option_callback("lwkmem-brk-clear-len",
				     lwkmem_brk_clear_len_cb);
	mos_register_option_callback("lwkmem-interleave-disable",
				     lwkmem_interleave_disable_cb);
	mos_register_option_callback("lwkmem-load-elf-disable",
				     lwkmem_load_elf_disable_cb);

	return 0;

collect_err:
	return -ENOMEM;

} /* end of mos_collect_bootmem() */

subsys_initcall(mos_collect_bootmem);

void list_vmas(struct mm_struct *mm)
{
	struct vm_area_struct *start;
	struct vm_area_struct *vma;
	char len_str[STRBUF_LEN];
	char lwk_str[STRBUF_LEN];

	start = mm->mmap;
	vma = start;
	while (vma) {
		unsigned long len;

		len = vma->vm_end - vma->vm_start;
		if (len < (1 << 10))
			snprintf(len_str, STRBUF_LEN, "%6ld  ", len);
		else if (len < (1 << 20))
			snprintf(len_str, STRBUF_LEN, "%6ldk ", len >> 10);
		else if (len < (1 << 30))
			snprintf(len_str, STRBUF_LEN, "%6ldM ", len >> 20);
		else
			snprintf(len_str, STRBUF_LEN, "%6ldG ", len >> 30);

		if (is_lwkmem(vma))
			snprintf(lwk_str, STRBUF_LEN, " LWK");
		else
			snprintf(lwk_str, STRBUF_LEN, "    ");

		pr_info("[0x%016lx - 0x%016lx] len %s flags 0x%8lx %s file %p\n",
			vma->vm_start, vma->vm_end, len_str, vma->vm_flags,
			lwk_str, vma->vm_file);

		vma = vma->vm_next;
		if (vma == start)
			/* Wrapped around. Just in case this is possible */
			return;
	}

	return;

} /* list_vmas() */

/**
 * Construct a block list of the specified length and kind using
 * the granule.
 */
static struct blk_list *create_and_link_block(struct lwk_process_granule *pgran,
			      uint64_t length, enum lwkmem_kind_t knd,
			      struct mos_process_t *mosp)
{
	struct blk_list *newb;
	uint64_t addr;

	newb = vmalloc(sizeof(struct blk_list));
	if (!newb)
		return NULL;

	newb->offset = pgran->offset;
	newb->num_blks = length / kind_size[knd];
	newb->free = 1;
	newb->phys = pgran->granule;
	newb->vma_addr = 0;
	list_add(&newb->list, &mosp->blk_list[knd]);

	/* Move the watermark in the process granule: */
	pgran->offset += length;

	/* Incr. the block count for this TLB size: */
	mosp->num_blks[knd] += newb->num_blks;

	addr = (uint64_t)newb->phys->base + newb->offset;

	if (addr != roundup(addr, kind_size[knd]))
		pr_err("(!) block list %#016llx [%s] is not aligned (%#llX)\n",
		       addr, kind_str[knd], kind_size[knd]-1);

	if (pgran->offset > pgran->granule->length) {
		pr_err("(!) block list %#016llx [%s] overflows granule [%#016llx-%#016llX]\n",
		       addr, kind_str[knd], (uint64_t)pgran->granule->base,
		       (uint64_t)pgran->granule->base + pgran->granule->length);
	}

	if (LWKMEM_DEBUG_VERBOSE) {
		pr_info("Consume %s granule [%#016llx, %llu (%llu MB), 0x%llx]\n",
			pgran->offset >= pgran->granule->length ? "entire"
			: "partial", addr, length, length >> 20,
			pgran->offset);
		pr_info("Block %s x %llu blocks.\n", kind_str[knd],
			newb->num_blks);
	}

	return newb;
}


static int partition_task_mem(struct mos_process_t *mosp, int64_t reserved)
{
	struct lwk_process_granule *g;
	enum lwkmem_kind_t knd, nxt;
	uint64_t addr, delta;

	list_for_each_entry(g, &mosp->lwkmem_list, list) {

		/* Incrementally align with the next sized TLB */
		for (knd = 0; knd < kind_last - 1; knd++) {
			if (num_dTLB(knd) == 0)
				continue;

			for (nxt = knd + 1; nxt < kind_last; nxt++)
				if (nxt < kind_last && num_dTLB(nxt) > 0)
					break;

			addr = (uint64_t)g->granule->base + g->offset;

			/* If we are not aligned with the next sized TLB, then
			 * construct a block list that consumes the region
			 * between the current location and the next alignment
			 * boundary.
			 */
			if (nxt < kind_last && (addr & (kind_size[nxt] - 1))) {
				delta = roundup(addr, kind_size[nxt]) - addr;
				delta = min(delta, g->granule->length -
					    g->offset);
				if (!delta)
					continue;
				if (!create_and_link_block(g, delta, knd, mosp))
					return -1;
			}
		}

		/* We are now aligned to the largest TLB boundary (1G).  Consume
		 * the remainder of the granule in the largest possible blocks:
		 */

		for (knd = kind_last - 1; knd >= 0; knd--) {
			if (g->offset >= g->granule->length)
				break;

			if (num_dTLB(knd) == 0)
				continue;

			delta = rounddown(g->granule->length - g->offset,
					  kind_size[knd]);
			if (!delta)
				continue;
			if (!create_and_link_block(g, delta, knd, mosp))
				return -1;
		}
	}

	if (LWKMEM_DEBUG) {
		pr_info("Process granule list for pid %d after block partitioning:\n",
			current->pid);
		dump_process_mem_list(mosp);
	}

	if (LWKMEM_DEBUG_VERBOSE) {
		pr_info("Block list for pid %d after partitioning:\n",
			current->pid);
		dump_block_lists(mosp);
	}

	return 0;
}

static struct mos_lwk_mem_granule *find_free_granule_for_nid(int nid)
{
	struct mos_lwk_mem_granule *g;

	list_for_each_entry(g, &mos_lwk_memory_list, list) {
		if (g->owner <= 0 && (g->nid == nid || nid == NUMA_NO_NODE))
			return g;
	}

	return NULL;
}

/*
 * Free the memory used to store the block lists and mark the corresponding
 * physical regions as free
 */
static void release_task_mem_blocks(void)
{
	struct blk_list *p_blk;
	struct blk_list *tmp;
	struct mos_process_t *mos_p;
	enum lwkmem_kind_t k;

	mos_p = current->mos_process;
	if (!mos_p)
		return;

	for (k = kind_4k; k < kind_last; k++) {
		list_for_each_entry_safe(p_blk, tmp, &mos_p->blk_list[k],
				list) {
			p_blk->phys->owner = -1;
			list_del(&p_blk->list);
			vfree(p_blk);
		}
		mos_p->num_blks[k] = 0;
	}

} /* end of release_task_mem_blocks() */

/*
 * This mOS process is going away. Release all resources related to LWK memory.
 * We assume that freeing all data structures realted to the vma will be done
 * by Linux when the process exits.
 */
void lwkmem_release(struct mos_process_t *mos_p)
{
	struct lwk_process_granule *p_granule;
	struct lwk_process_granule *tmp;
	struct mos_lwk_mem_granule *elt;
	struct mos_lwk_mem_granule *next;
	struct mos_lwk_mem_granule *save;

	if (LWKMEM_DEBUG)
		pr_info("lwkmem_release() will release %lld MB of LWK memory for process %d\n",
		       mos_p->lwkmem >> 20, current->pid);

	release_task_mem_blocks();

	/* There is a problem, if lwkmem_list is empty */
	if (list_empty(&(mos_p->lwkmem_list))) {
		pr_warn("lwkmem_release() process %d has no memory!\n",
			current->pid);
		return;
	}

	if (mos_p->report_blks_allocated) {
		int n, rc;
		enum lwkmem_kind_t k;
		char str[MAX_NUMNODES*16];

		/* Output process memory information header. */
		pr_info("PID %u memory usage:\n", mos_p->tgid);
		rc = snprintf(str, sizeof(str), "mem/nid\t");
		for_each_node_mask(n, node_online_map)
			rc += snprintf((str+rc), sizeof(str)-rc, "%8u ", n);
		pr_info("%s\n", str);

		/* Output block usage data. */
		for (k = kind_4k; k < kind_last; k++) {
			rc = snprintf(str, sizeof(str), "%s\t", kind_str[k]);
			for_each_node_mask(n, node_online_map)
				rc += snprintf((str+rc), sizeof(str)-rc, "%8lu ",
						mos_p->blks_allocated[k][n]); 
			pr_info("%s\n", str);
		}
	}

	/* Reset the granules that were assigned to this process in the
	 * global mos_lwk_memory_list.
	 */
	list_for_each_entry_safe(p_granule, tmp, &mos_p->lwkmem_list, list) {
		if (LWKMEM_DEBUG_VERBOSE)
			pr_info("releasing [%16p-%16p], 0x%llx bytes (%lld MB), owner %d\n",
			       p_granule->granule->base,
			       p_granule->granule->base +
			       p_granule->granule->length - 1,
			       p_granule->granule->length,
			       p_granule->granule->length >> 20,
			       p_granule->granule->owner);

		p_granule->granule->owner = -1;
		p_granule->granule = NULL;
		list_del(&p_granule->list);
		vfree(p_granule);
	}

	if (LWKMEM_DEBUG_VERBOSE) {
		pr_info("mos_lwk_memory_list before merge\n");
		dump_granule_list(&mos_lwk_memory_list);
	}

	/* (Re)Merge granules that are physically adjacent */
	list_for_each_entry_safe(elt, save, &mos_lwk_memory_list, list) {
		next = list_next_entry(elt, list);
		if ((elt->base + elt->length == next->base) &&
				(elt->owner == -1) && (next->owner == -1)) {
			next->base = elt->base;
			next->length = next->length + elt->length;
			list_del(&elt->list);
			vfree(elt);
	    }
	}

	if (LWKMEM_DEBUG) {
		pr_info("The latest mos_lwk_memory_list\n");
		dump_granule_list(&mos_lwk_memory_list);
	}

} /* end of lwkmem_release() */

/*
 * Add up how much of the LWK memory reserved for this process is still
 * available. Used by in fs/proc/meminfo.c
 */
void lwkmem_available(unsigned long *totalraam, unsigned long *freeraam)
{
	int64_t available[kind_last] = {0, 0, 0, 0};
	int64_t total_avail = 0;
	int64_t total_alloc = 0;
	enum lwkmem_kind_t k;
	struct blk_list *p_blk;
	struct mos_process_t *mos_p;

	mos_p = current->mos_process;
	if (!mos_p)
		return;

	if (LWKMEM_DEBUG)
		pr_info("lwkmem_available() for pid %d\n", current->pid);

	for (k = kind_4k; k < kind_last; k++) {
		list_for_each_entry(p_blk, &mos_p->blk_list[k], list)
			if (p_blk->free) {
				available[k] = available[k] +
					p_blk->num_blks * kind_size[k];
				total_avail = total_avail +
					p_blk->num_blks * kind_size[k];
			} else {
				total_alloc = total_alloc +
					p_blk->num_blks * kind_size[k];
			}
		if (LWKMEM_DEBUG)
			pr_info("pid %d %s blocks available: %16lld M\n",
				current->pid, kind_str[k], available[k] >> 20);
	}

	*totalraam = (total_avail + total_alloc) >> PAGE_SHIFT;
	*freeraam = total_avail >> PAGE_SHIFT;

} /* end of lwkmem_available() */

/*
 * For a given kind; e.g., 1g, 4m, 2m, or 4k, figure out how many blocks we
 * should allocate. Return the appropriate number of blocks.
 */
static int64_t blocks_wanted(int64_t len, int64_t *wanted,
			  struct allocate_options_t *options)
{
	int64_t total = 0;
	enum lwkmem_kind_t k;

	for (k = kind_last - 1; (int)k >= 0 && len > 0; k--) {
		if (num_dTLB(k) <= 0 || kind_size[k] > options->max_page_size)
			continue;

		wanted[k] = len / kind_size[k];

		len -= wanted[k] * kind_size[k];
		total += wanted[k] * kind_size[k];

		if (LWKMEM_DEBUG && wanted[k])
			pr_info("  Want %6lld x %s blocks - available:%6lld %s remain:%12lld\n",
				wanted[k], kind_str[k],
				options->mosp->num_blks[k],
				(wanted[k] > options->mosp->num_blks[k])
				? "(!)"	: " ", len);
	}

	return total;
}

/*
** Convert a user virtual address to a pointer to the corresponding struct
** page. LWK memory is always pinned, so there is always a struct page for
** an LWK memory user address.
*/
struct page *lwkmem_user_to_page(struct mm_struct *mm, unsigned long addr)
{
	unsigned long *PML4E;
	unsigned long *PDPTE;
	unsigned long *PDE;
	unsigned long *PTE;
	pgd_t *pgd;
	long int offset;
	struct page *pg;

	pgd = pgd_offset(mm, addr);
	PML4E = (unsigned long *)mm->pgd;
	PDPTE = __va(PML4E[pgd_index(addr)] & PHYSICAL_PAGE_MASK);
	if (PDPTE[pud_index(addr)] & _PAGE_PSE) {
		/* This is a pointer to a 1g page */
		pg = virt_to_page(__va(PDPTE[pud_index(addr)] & PG1G_MASK));
		offset = addr & 0x03fffffff;
		pg = pg + (offset / PAGE_SIZE);
		return pg;
	}
	PDE = __va(PDPTE[pud_index(addr)] & PHYSICAL_PAGE_MASK);
	if (PDE[pmd_index(addr)] & _PAGE_PSE) {
		/* This is a pointer to a 2m page */
		pg = virt_to_page(__va(PDE[pmd_index(addr)] & PG2M_MASK));
		offset = addr & 0x0001fffff;
		pg = pg + (offset / PAGE_SIZE);
		return pg;
	}
	PTE = __va(PDE[pmd_index(addr)] & PHYSICAL_PAGE_MASK);
	if (PTE[pte_index(addr)] & _PAGE_PRESENT) {
		/* This is a pointer to a 4k page */
		pg = virt_to_page(__va(PTE[pte_index(addr)] & ADDR_MASK));
		/* No need to adjust; offset is < PAGE_SIZE */
		return pg;
	}
	pr_err("Addr 0x%lx is not in a mapped LWK page. Something is wrong!\n",
		addr);

	return NULL;

} /* end of lwkmem_user_to_page() */

/*
 * Print CR3 and page table entry structure flags for IA=32e paging mode
 * From Fugure 4-11 in Intel 64 and IA-32 Architectures Software Developer's
 * Manual, Volume 3A: System Programming Guide, Part 1
 * lvl are {CR3 = 0, PML4E = 1, PDPTE = 2, PDE = 3, PTE = 4}
 */
static char *print_flags(int lvl, unsigned long entry)
{
	static char buf1[STRBUF_LEN];
	static char buf2[STRBUF_LEN];

	switch (lvl) {
	case 0:
		snprintf(buf1, STRBUF_LEN, "CR3:   ");
		break;
	case 1:
		snprintf(buf1, STRBUF_LEN, "PML4E: ");
		break;
	case 2:
		snprintf(buf1, STRBUF_LEN, "PDPTE: ");
		break;
	case 3:
		snprintf(buf1, STRBUF_LEN, "PDE:   ");
		break;
	case 4:
		snprintf(buf1, STRBUF_LEN, "PTE:   ");
		break;
	default:
		snprintf(buf1, STRBUF_LEN, "unknown paging entry level\n");
		return buf1;
	}

	if (lvl == 0) {
		snprintf(buf2, STRBUF_LEN, "PML4 table at     0x%016lx",
			entry & ADDR_MASK);
		strcat(buf1, buf2);
		if (entry & _PAGE_PWT)
			strcat(buf1, " PWT");
		if (entry & _PAGE_PCD)
			strcat(buf1, " PCD");
		strcat(buf1, "\n");
		return buf1;
	}

	if (!(entry & _PAGE_PRESENT)) {
		strcat(buf1, "not present\n");
		return buf1;
	}

	if (lvl == 1) {
		/* PML4E */
		snprintf(buf2, STRBUF_LEN, "page dir table at 0x%016lx",
			entry & ADDR_MASK);
		strcat(buf1, buf2);
	} else if (lvl == 2) {
		/* PDPTE */
		if (entry & _PAGE_PSE) {
			snprintf(buf2, STRBUF_LEN, "1GB page frame at 0x%016lx",
				entry & PG1G_MASK);
			strcat(buf1, buf2);
			if (entry & _PAGE_PAT_LARGE)
				strcat(buf1, " large PAT");
		} else {
			snprintf(buf2, STRBUF_LEN, "page dir at       0x%016lx",
				entry & ADDR_MASK);
			strcat(buf1, buf2);
		}
	} else if (lvl == 3) {
		/* PDE */
		if (entry & _PAGE_PSE) {
			snprintf(buf2, STRBUF_LEN, "2MB page frame at 0x%016lx",
				entry & PG2M_MASK);
			strcat(buf1, buf2);
			if (entry & _PAGE_PAT_LARGE)
				strcat(buf1, " large PAT");
		} else {
			snprintf(buf2, STRBUF_LEN, "page table at     0x%016lx",
				entry & ADDR_MASK);
			strcat(buf1, buf2);
		}
	} else {
		/* PTE */
		snprintf(buf2, STRBUF_LEN, "4kB page frame at 0x%016lx",
			entry & ADDR_MASK);
		strcat(buf1, buf2);
		if (entry & _PAGE_PAT)
			strcat(buf1, " PAT");
	}

	/* Common flags for entries that are present */
	if (entry & _PAGE_GLOBAL)
		strcat(buf1, " global");

	if (entry & _PAGE_DIRTY)
		strcat(buf1, " dirty");

	if (entry & _PAGE_ACCESSED)
		strcat(buf1, " accessed");

	if (entry & _PAGE_PCD)
		strcat(buf1, " cache disabled");

	if (entry & _PAGE_PWT)
		strcat(buf1, " write through");

	if (entry & _PAGE_USER)
		strcat(buf1, " user space");

	if (entry & _PAGE_RW)
		strcat(buf1, " writeable");

	strcat(buf1, "\n");
	return buf1;

} /* end of print_flags() */

static void print_cr3(char *str, unsigned long cr3)
{
	unsigned long PML4_addr;
	char buf1[STRBUF_LEN];

	PML4_addr = (cr3 & 0x0fffffffffffff000);

	snprintf(buf1, STRBUF_LEN, "%sPML4 table address is 0x%016lx Flags:",
		str, PML4_addr);
	if (cr3 & _PAGE_PWT)
		strcat(buf1, " PWT");
	if (cr3 & _PAGE_PCD)
		strcat(buf1, " PCD");
	pr_info("%s\n", buf1);

} /* end of print_cr3() */

static void idx_range(int idx, int max, int *idx_start, int *idx_end)
{
	*idx_end = *idx_start = idx;
	if (*idx_start > 0)
		*idx_start = *idx_start - 1;
	else
		*idx_end = *idx_end + 1;
	if (*idx_end < (max - 1))
		*idx_end = *idx_end + 1;
	else
		*idx_start = *idx_start - 1;

} /* end of idx_range() */

void print_pgd(pgd_t *cr3, unsigned long addr)
{
	unsigned long *PML4E;
	unsigned long *PDPTE;
	unsigned long *PDE;
	unsigned long *PTE;
	int idx_start, idx_end, i;
	int count;
#ifdef DEBUG_DEBUG
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	struct page *page = NULL;
#endif /* DEBUG_DEBUG */

	print_cr3("", read_cr3());
	PML4E = (unsigned long *)cr3;
#ifdef DEBUG_DEBUG
	pgd = pgd_offset(current->mm, addr);
	pr_err("pgd_offset(mm)  is 0x%016lx\n", pgd_val(*pgd));
#endif /* DEBUG_DEBUG */

	/* I want to print 3 PML4E entries near idx */
	idx_range(pgd_index(addr), PTRS_PER_PGD, &idx_start, &idx_end);

	count = 0;
	for (i = 0; i < PTRS_PER_PGD; i++)
		if (PML4E[i] & _PAGE_PRESENT)
			count++;

	pr_info("Looking up virtual address 0x%016lx\n", addr);
	pr_info("PML4E at 0x%016lx has %d entries present\n",
		(unsigned long)PML4E, count);
	for (i = idx_end; i >= idx_start; i--) {
		if (i == pgd_index(addr))
			pr_info(" --->[%3d] %s", i,
				print_flags(1, PML4E[i]));
		else
			pr_info("     [%3d] %s", i,
				print_flags(1, PML4E[i]));
	}

	PDPTE = __va(PML4E[pgd_index(addr)] & PHYSICAL_PAGE_MASK);
#ifdef DEBUG_DEBUG
	pud = pud_offset(pgd, addr);
	pr_err("pud_offset(pgd) is 0x%016lx\n", pud_val(*pud));
#endif /* DEBUG_DEBUG */

	idx_range(pud_index(addr), PTRS_PER_PUD, &idx_start, &idx_end);

	count = 0;
	for (i = 0; i < PTRS_PER_PUD; i++)
		if (PDPTE[i] & _PAGE_PRESENT)
			count++;

	pr_info("PDPTE at 0x%016lx has %d entries present\n",
		(unsigned long)PDPTE, count);
	for (i = idx_end; i >= idx_start; i--) {
		if (i == pud_index(addr))
			pr_info(" --->[%3d] %s", i, print_flags(2, PDPTE[i]));
		else
			pr_info("     [%3d] %s", i, print_flags(2, PDPTE[i]));
	}

	if (PDPTE[pud_index(addr)] & _PAGE_PSE) {
		/* This is a pointer to a 1g page */
		pr_info("addr 0x%lx is in a 1g page at %p and offset %ld\n",
			addr, __va(PDPTE[pud_index(addr)] & PG1G_MASK),
			addr & 0x03fffffff);
		return;
	}

	/* This is a pointer to a page directory */
	PDE = __va(PDPTE[pud_index(addr)] & PHYSICAL_PAGE_MASK);
#ifdef DEBUG_DEBUG
	pmd = pmd_offset(pud, addr);
	pr_err("pud_offset(pud) is 0x%016lx\n", pmd_val(*pmd));
#endif /* DEBUG_DEBUG */

	idx_range(pmd_index(addr), PTRS_PER_PMD, &idx_start, &idx_end);

	count = 0;
	for (i = 0; i < PTRS_PER_PMD; i++)
		if (PDE[i] & _PAGE_PRESENT)
			count++;

	pr_info("PDE at 0x%016lx has %d entries present\n",
		(unsigned long)PDE, count);
	for (i = idx_end; i >= idx_start; i--) {
		if (i == pmd_index(addr))
			pr_info(" --->[%3d] %s", i, print_flags(3, PDE[i]));
		else
			pr_info("     [%3d] %s", i, print_flags(3, PDE[i]));
	}

	if (PDE[pmd_index(addr)] & _PAGE_PSE) {
		/* This is a pointer to a 2m page */
		pr_info("addr 0x%lx is in a 2m page at %p and offset %ld\n",
			addr, __va(PDE[pmd_index(addr)] & PG2M_MASK),
			addr & 0x0001fffff);
		return;
	}

	/* This is a pointer to a page table */
	PTE = __va(PDE[pmd_index(addr)] & PHYSICAL_PAGE_MASK);
#ifdef DEBUG_DEBUG
	pte = pte_offset_map(pmd, addr);
	pr_err("pte_offset(pmd) is 0x%016lx\n", pte_val(*pte));
#endif /* DEBUG_DEBUG */


	idx_range(pte_index(addr), PTRS_PER_PTE, &idx_start, &idx_end);

	count = 0;
	for (i = 0; i < PTRS_PER_PTE; i++)
		if (PTE[i] & _PAGE_PRESENT)
			count++;

	pr_info("PTE at 0x%016lx has %d entries present\n",
		(unsigned long)PTE, count);
	for (i = idx_end; i >= idx_start; i--) {
		if (i == pte_index(addr))
			pr_info(" --->[%3d] %s", i, print_flags(4, PTE[i]));
		else
			pr_info("     [%3d] %s", i, print_flags(4, PTE[i]));
	}

	if (PTE[pte_index(addr)] & _PAGE_PRESENT) {
		/* This is a pointer to a 4k page */
		pr_info("addr 0x%lx is in a 4k page at %p and offset %ld\n",
			addr, __va(PTE[pte_index(addr)] & ADDR_MASK),
			addr & 0x000000fff);
		return;
	}
	pr_info("addr 0x%lx is NOT fully mapped. PTE is 0x%lx\n", addr,
		PTE[pte_index(addr)]);
#ifdef DEBUG_DEBUG
	page = pte_page(*pte);
	if (!page)
		pr_err("There is No struct page for this 4k page!\n");
	else {
		pr_err("There IS a struct page for this 4k page! PFN %ld, pfn_valid %d\n",
			pte_pfn(*pte), pfn_valid(pte_pfn(*pte)));
	}
	pte_unmap(pte);
#endif /* DEBUG_DEBUG */

	return;

} /* end of print_pgd() */

/*
 * Build the page table entries for this vma
 * Each lwkmem vma covers a physically contigous range of memory of a given
 * page size specified by knd. The memory range starts at phys.
 */
static int build_pagetbl(enum lwkmem_kind_t knd, struct vm_area_struct *vma,
			 unsigned long phys, unsigned long addr,
			 unsigned long end)
{
	struct mm_struct *mm = current->mm;
	int rc = 0;

	phys = (phys + lwk_page_size[knd] - 1) & lwk_page_mask[knd];
	while (addr < end) {
		struct page *p;
		unsigned long pfn;
		unsigned long addr_end = addr + kind_size[knd];

		/* Build the page table */
		pgd_t *pgd;
		pud_t *pud;
		pmd_t *pmd;
		pte_t *pte;
		spinlock_t *ptl;

		pgd = pgd_offset(mm, addr);

		pud = pud_alloc(mm, pgd, addr);
		if (!pud) {
			pr_err("pud_alloc() failed on line %d\n", __LINE__);
			rc = -ENOMEM;
			goto pagetbl_err;
		}

		if (knd == kind_1g) {
			/* Use cpu_has_gbpages from asm/cpufeature.h for 1g */
			/* Have a look at setup_hugepagesz() */
			pud_t entry;

			ptl = &mm->page_table_lock;
			spin_lock(ptl); /* Not sure this is actually needed */

			/* Setup the PUD (for a 1g page) */
			entry = __pud(((phys_addr_t)phys) |
					massage_pgprot(vma->vm_page_prot));
			pfn = pud_pfn(entry);

			/* _PAGE_PWT cache write combining */
			/* _PAGE_PCD */
			/* _PAGE_PCD | _PAGE_PWT == uncached; don't want that */
			entry =  __pud(native_pud_val(entry) & ~(_PAGE_PWT|
					_PAGE_PCD));

			entry = __pud(native_pud_val(entry) | (_PAGE_PRESENT|
					_PAGE_USER|_PAGE_RW|_PAGE_ACCESSED|
					_PAGE_PSE));

			/* *pud = entry */
			set_pud(pud, entry);
			spin_unlock(ptl);

		} else if (knd == kind_2m) {
			pmd_t entry;

			pmd = pmd_alloc(mm, pud, addr);
			if (!pmd) {
				pr_err("pmd_alloc() failed on line %d\n",
					__LINE__);
				rc = -ENOMEM;
				goto pagetbl_err;
			}
			ptl = pmd_lock(mm, pmd);

			/* Setup the PMD (for a 2m page) */
			entry = pfn_pmd(phys >> PAGE_SHIFT, vma->vm_page_prot);
			pfn = pmd_pfn(entry);

			/* _PAGE_PWT cache write combining */
			/* _PAGE_PCD */
			/* _PAGE_PCD | _PAGE_PWT == uncached; don't want that */
			entry = pmd_clear_flags(entry, _PAGE_PWT|_PAGE_PCD);

			entry = pmd_set_flags(entry, _PAGE_PRESENT|_PAGE_USER|
					_PAGE_RW|_PAGE_ACCESSED|_PAGE_PSE);

			/* *pmd = entry */
			set_pmd_at(mm, addr, pmd, entry);

			spin_unlock(ptl);
		} else if (knd == kind_4k) {
			pte_t entry;

			pmd = pmd_alloc(mm, pud, addr);
			if (!pmd) {
				pr_err("pmd_alloc() failed on line %d\n",
					__LINE__);
				rc = -ENOMEM;
				goto pagetbl_err;
			}

			pte = pte_alloc_map_lock(mm, pmd, addr, &ptl);
			if (!pte) {
				pr_err("pte_alloc_map_lock() failed on line %d\n",
					__LINE__);
				rc = -ENOMEM;
				goto pagetbl_err;
			}

			entry = pfn_pte((phys >> PAGE_SHIFT),
					vm_get_page_prot(vma->vm_flags));
			pfn = pte_pfn(entry);

			/* _PAGE_PCD | _PAGE_PWT == uncached; don't want that */
			entry = pte_clear_flags(entry, _PAGE_PWT|_PAGE_PCD);

			entry = pte_set_flags(entry, _PAGE_PRESENT|_PAGE_USER|
					_PAGE_RW|_PAGE_ACCESSED);
			/* *pte = entry */
			set_pte_at(mm, addr, pte, entry);

			pte_unmap_unlock(pte, ptl);
		} else {
			pr_err("Other page sizes not supported yet!\n");
			rc = -ENOMEM;
			goto pagetbl_err;
		}

		/* Add mapping for each page. */
		for (p = pfn_to_page(pfn); addr < addr_end; p++,
		     addr += PAGE_SIZE) {
			page_add_new_anon_rmap(p, vma, addr, false);
			ClearPageSwapBacked(p);
		}

		phys += kind_size[knd];
	}

pagetbl_err:
	return rc;

} /* end of build_pagetbl() */

/* Return False (zero) if the specified region is adjacent to the previous
 * or subsequent VMAs and those VMAs are either not LWK VMAs or they are
 * backed with a different page size.  Otherwise return True.
 *
 * Note that a positive result does not imply that the VMAs are, indeed
 * mergeable.  Rather, a negative result means that it makes no sense to
 * call vma_merge.
 */

static int lwkvma_attempt_merge(struct mm_struct *mm, unsigned long addr,
		    unsigned long end, struct vm_area_struct *prev,
		    enum lwkmem_kind_t knd)
{
	struct vm_area_struct *next;

	if (prev)
		next = prev->vm_next;
	else
		next = mm->mmap;

	if (prev && prev->vm_end == addr &&
	    (!is_lwkmem(prev) || LWK_PAGE_SHIFT(prev) != lwk_page_shift[knd]))
		return 0;

	if (next && end == next->vm_start &&
	    (!is_lwkmem(next) || LWK_PAGE_SHIFT(next) != lwk_page_shift[knd]))
		return 0;

	return 1;
}

static long build_lwkvma(enum lwkmem_kind_t knd, unsigned long addr,
			 unsigned long len, unsigned long prot,
			 unsigned long mmap_flags, unsigned long pgoff,
			 unsigned long phys, unsigned long total_length)
{

	int rc = 0;
	int pkey = 0;
	struct mm_struct *mm = current->mm;
	vm_flags_t vm_flags;
	struct rb_node **rb_link, *rb_parent;
	struct vm_area_struct *vma, *prev;

	/* Ignore those per man page */
	mmap_flags &= ~(MAP_EXECUTABLE | MAP_DENYWRITE);

	/* offset overflow? */
	if ((pgoff + (len >> PAGE_SHIFT)) < pgoff)
		return -EOVERFLOW;

	/* There is no need to align the length. This function gets called
	 * with len requests of multiples of blocks sizes. */
	if (unlikely(len != ALIGN(len, lwk_page_size[knd])))
		return -EINVAL;

	if (mmap_flags & MAP_FIXED) {
		if (addr == 0)
			/* Supposed to map at 0. Let Linux do that. */
			return -ENOSYS;
		if ((addr & (lwk_page_size[knd] - 1)) != 0) {
			pr_err("Hint addr 0x%lx not aligned with %s page\n",
				addr, kind_str[knd]);
			return -ENOSYS;
		}
	} else {
		struct vm_unmapped_area_info info;

		info.flags = 0;
		info.length = total_length;
		info.low_limit = current->mm->mmap_legacy_base;
		info.high_limit = TASK_SIZE;
		info.align_mask = lwk_page_size[knd] - 1;
		info.align_offset = 0;

		/* x86_64 arch_get_unmapped_area() -> vm_unmapped_area()
		 * -> unmapped_area()
		 * We can't use get_unmapped_area because we really want to
		 * align addr to lwk_page_size[knd].
		 */
		addr = unmapped_area(&info);
	}

	if (prot == PROT_EXEC) {
		pkey = execute_only_pkey(mm);
		if (pkey < 0)
			pkey = 0;
	}

	vm_flags = calc_vm_prot_bits(prot, pkey) |
		calc_vm_flag_bits(mmap_flags) | mm->def_flags | VM_MAYREAD |
		VM_MAYWRITE | VM_MAYEXEC | VM_READ | VM_WRITE | VM_ACCOUNT;

	/* Pretend I/O space, but map cacheable (see below)
	 * We don't want to set VM_HUGETLB in vm_flags because we are doing
	 * things not entirely compatible with hugeTLB.
	 * However, there are places in Linux where that flag gets checked.
	 * In some of those, we need to insert our own code to handle LWK mem.
	 * Look for places where is_vm_hugetlb_page() gets called.
	 * PAT reserves whole VMA at once (x86).  We aren't setting VM_PAT
	 * to get out from untrack_pfn() faster.
	 */

	switch (mmap_flags & MAP_TYPE) {
	case MAP_SHARED:
		if (vm_flags & (VM_GROWSDOWN | VM_GROWSUP))
			return -EINVAL;
		/* Ignore pgoff. */
		pgoff = 0;
		vm_flags |= VM_SHARED | VM_MAYSHARE;
		break;
	case MAP_PRIVATE:
		/* Set pgoff according to addr for anon_vma. */
		pgoff = addr >> PAGE_SHIFT;
		break;
	default:
		return -EINVAL;
	}

	/* find_vma_links() finds the place where this vm entry should go */
	down_write(&mm->mmap_sem);
	rc = find_vma_links(mm, addr, addr + len, &prev, &rb_link, &rb_parent);
	if (rc) {
		pr_err("%s: find_vma_links(%p, 0x%lx, 0x%lx, ...)=%i\n",
		       __func__, mm, addr, addr + len, rc);
		do_munmap(mm, addr, addr + len);
		goto out;
	}

	/* See if we can merge with an existing vma */
	vma = 0;
	if (lwkvma_attempt_merge(mm, addr, addr + len, prev, knd))
		vma = vma_merge(mm, prev, addr, addr + len, vm_flags, NULL,
				NULL, pgoff, NULL, prev->vm_userfaultfd_ctx);
	if (!vma) {
		/* Get us some memory to store our vm_area_struct structure */
		vma = kmem_cache_zalloc(vm_area_cachep, GFP_KERNEL);
		if (!vma) {
			pr_err("kmem_cache_zalloc() failed on line %d\n",
				__LINE__);
			goto out;
		}

		vma->vm_mm = mm;
		vma->vm_start = addr;
		vma->vm_end = addr + len;
		vma->vm_flags = vm_flags;
		vma->vm_page_prot = vm_get_page_prot(vm_flags);
		vma->vm_pgoff = pgoff;

		/* Now link our entry into the vma list */
		INIT_LIST_HEAD(&vma->anon_vma_chain);
		vma_link(mm, vma, prev, rb_link, rb_parent);
	}

	/* Mark as LWK memory and record page size.  Used by show_map_vma() */
	vma->vm_private_data = (void *)(_LWKMEM |
		(lwk_page_shift[knd] - PAGE_SHIFT));

	rc = anon_vma_prepare(vma);
	if (rc) {
		kmem_cache_free(vm_area_cachep, vma);
		addr = rc;
		goto out;
	}

	perf_event_mmap(vma);

	rc = build_pagetbl(knd, vma, phys, addr, addr + len);

	if (LWKMEM_DEBUG && !rc) {
		pr_info("built a %s x %lld vma vm_start 0x%lx, vm_end 0x%lx\n",
			kind_str[knd], len / kind_size[knd], vma->vm_start,
			vma->vm_end);
	}

out:
	up_write(&mm->mmap_sem);
	return rc ? rc : addr;

} /* end of build_lwkvma() */

static DEFINE_MUTEX(lwkmem_mutex);

/**
 * For the given block size, obtains a free block of a larger size and
 * turns it into a (free) block of the given kind.  Returns the
 * number of blocks created; or negative if there is no free space.
 */
static struct blk_list *divide_block(enum lwkmem_kind_t knd,
			      struct allocate_options_t *opts)
{
	enum lwkmem_kind_t nxt;
	struct blk_list *elt = 0, *newb = 0;

	/* Find the next larger sized block that is actually used: */
	for (nxt = knd + 1; num_dTLB(nxt) == 0 && nxt < kind_last; nxt++)
		;

	if (nxt >= kind_last) {
		if (LWKMEM_DEBUG)
			pr_err("Cannot divide %s blocks\n", kind_str[knd]);
		return 0;
	}

	if (LWKMEM_DEBUG)
		pr_info("Dividing %s block into a %s block.\n", kind_str[nxt],
			kind_str[knd]);

	elt = opts->find_available(nxt, opts);

	/* If there are no blocks of the next larger size available,
	 * then recurse.
	 */
	if (!elt) {
		if (LWKMEM_DEBUG)
			pr_info("There are no %s blocks ... going to the next larger size.\n",
				kind_str[nxt]);
		elt = divide_block(nxt, opts);

		if (!elt)
			return 0;
	}

	/* If there is more than one free block of the next larger
	 * size, then consume just the first free block.  Otherwise
	 * the entire block needs to be consumed.
	 */
	if (elt->num_blks > 1) {
		newb = vmalloc(sizeof(struct blk_list));
		if (!newb)
			return 0;
		newb->phys = elt->phys;
		newb->offset = elt->offset;
		newb->num_blks = kind_size[nxt] / kind_size[knd];
		newb->free = 1;
		newb->vma_addr = 0;
		elt->num_blks--;
		elt->offset += kind_size[nxt];
	} else {

		list_del(&(elt->list));

		newb = elt;
		newb->num_blks *= (kind_size[nxt] / kind_size[knd]);
	}

	/* Add the block to the list */
	list_add(&(newb->list), &(opts->mosp->blk_list[knd]));

	/* Update block counts */
	opts->mosp->num_blks[knd] += newb->num_blks;
	opts->mosp->num_blks[nxt] -= 1;

	if (LWKMEM_DEBUG_VERBOSE)
		dump_block_lists(opts->mosp);

	return newb;
}

struct blk_list *find_available_blk_list(enum lwkmem_kind_t knd,
					 struct allocate_options_t *opts)
{
	struct blk_list *elt;
	int count = 0;
	list_for_each_entry(elt, &opts->mosp->blk_list[knd], list) {
		count++;
		if (elt->free) {
			if (LWKMEM_DEBUG)
				pr_info("%s elt=%016llX %s free:%lld depth:%d\n",
					__func__,
					(unsigned long)elt->phys->base +
					elt->offset, kind_str[knd],
					elt->num_blks, count);
			return elt;
		}

	}

	if (LWKMEM_DEBUG_VERBOSE)
		pr_info("(<) %s elt=NONE %s depth=%d\n",
			__func__, kind_str[knd], count);

	return 0;
}

static struct blk_list *find_available_by_nid(enum lwkmem_kind_t knd, int nid,
		       struct allocate_options_t *opts)
{
	int count = 0;
	struct blk_list *elt;

	list_for_each_entry(elt, &opts->mosp->blk_list[knd], list) {
		count++;
		if (!elt->free)
			continue;

		if (elt->phys->nid == nid || nid == -1) {
			if (LWKMEM_DEBUG)
				pr_info("(<) %s nid=%d elt=%016llX %s depth=%d\n",
					__func__, nid,
					(unsigned long)elt->phys->base +
					elt->offset, kind_str[knd], count);
			return elt;
		}
	}

	if (LWKMEM_DEBUG)
		pr_info("(<) %s nid=%d elt=NONE %s depth=%d\n",
			__func__, nid, kind_str[knd], count);

	return 0;
}

static struct blk_list *find_available_by_nid_list(enum lwkmem_kind_t knd,
					 struct allocate_options_t *opts)
{
	struct blk_list *elt;
	int i, nid;
	enum lwkmem_kind_t k;
	struct mos_process_t *mosp = opts->mosp;

	for (i = 0; i < lwkmem_type_last; i++) {
		int start_domain_index;
		enum lwkmem_type_t t = opts->type_order[i];

		if (opts->nid_order_len[t] == 0)
			continue;

		if (opts->mosp->lwkmem_interleave_disable)
			mosp->domain_order_index[t][knd] = 0;

		start_domain_index = mosp->domain_order_index[t][knd];

		do {
			int domain_index = mosp->domain_order_index[t][knd]++;

			nid = opts->nid_order[t][domain_index];
			mosp->domain_order_index[t][knd] %=
				opts->nid_order_len[t];

			for (k = knd; k < kind_last; k++) {
				elt = find_available_by_nid(k, nid, opts);
				if (elt) {
					if (LWKMEM_DEBUG)
						pr_info("(<) %s nid=%d elt=%016llX %s\n",
							__func__, nid,
							(unsigned long)
							elt->phys->base +
							elt->offset,
							kind_str[k]);

					/* If we found a block but it is of
					 * a larger size, then exit now,
					 * returning NULL.  This will allow the
					 * calling code to subdivide a block
					 * from this highest priority NID versus
					 * finding a block of the requested size
					 * from a lower priority NID.
					 */
					if (k > knd)
						elt = NULL;

					return elt;
				}
			}
		} while (mosp->domain_order_index[t][knd] !=
			 start_domain_index);
	}

	if (LWKMEM_DEBUG)
		pr_info("(<) %s elt=NONE\n", __func__);

	return 0;
}

long allocate_blocks(unsigned long addr, int64_t len,
		     unsigned long prot, unsigned long mmap_flags,
		     unsigned long pgoff, struct allocate_options_t *opts)
{
	int64_t wanted[kind_last] = {0, 0, 0, 0};
	int64_t total = 0;
	struct blk_list *elt, *new_blk;
	enum lwkmem_kind_t k;
	long new_addr;
	long first_addr = 0;

	mutex_lock(&lwkmem_mutex);

	if (LWKMEM_DEBUG_EXTREME) {
		pr_info("(>) %s(addr=%lx len=%lld / 0x%llx prot=%lx flags=%lx off=%lx) CPU=%d\n",
			__func__, addr, len, len, prot, mmap_flags, pgoff,
			smp_processor_id());
		dump_block_lists(opts->mosp);
	}

	/* Round up to the nearest, smallest page */
	len = roundup(len, kind_size[0]);
	total = opts->blocks_wanted(len, wanted, opts);

	/* Go through and map the allocated blocks */
	for (k = kind_last - 1; (int)k >= 0; k--) {
		if (wanted[k] <= 0)
			continue;

		if (num_dTLB(k) == 0) {
			wanted[k - 1] += wanted[k] *
				kind_size[k] / kind_size[k - 1];
			wanted[k] = 0;
			continue;
		}

		while (wanted[k] > 0) {
			elt = opts->find_available(k, opts);
			if (!elt) {
				elt = opts->divide_block(k, opts);

				if (!elt && k > 0) {
					wanted[k - 1] += wanted[k] *
						kind_size[k] / kind_size[k - 1];
					wanted[k] = 0;
					continue;
				}
			}

			if (!elt) {
				pr_err("INTERNAL ERROR: %s block list is empty!\n",
				       kind_str[k]);
				new_addr = -ENOMEM;
				goto alloc_err;
			}

			/* Let's grab some or all of this block */
			if (wanted[k] < elt->num_blks) {
				/* Split block in two and grab first one */
				new_addr = build_lwkvma(k, addr,
					wanted[k] * kind_size[k], prot,
					mmap_flags, pgoff,
					virt_to_phys(elt->phys->base +
							elt->offset), len);
				if (new_addr <= 0)
					goto alloc_err;

				new_blk = vmalloc(sizeof(struct blk_list));
				if (!new_blk) {
					new_addr = -ENOMEM;
					goto alloc_err;
				}

				new_blk->phys = elt->phys;
				new_blk->offset = elt->offset;
				new_blk->num_blks = wanted[k];
				new_blk->free = 0;
				new_blk->vma_addr = new_addr;
				list_add(&new_blk->list, &elt->list);

				elt->offset += wanted[k] * kind_size[k];
				elt->num_blks -= wanted[k];
				elt = new_blk;

				addr = new_addr + wanted[k] * kind_size[k];
				if (!first_addr) {
					first_addr = new_addr;
					mmap_flags |= MAP_FIXED;
				}
				total -= wanted[k] * kind_size[k];
				opts->mosp->num_blks[k] -= wanted[k];
				if (opts->mosp->report_blks_allocated)
					opts->mosp->blks_allocated[k][elt->phys->nid] += wanted[k];
				wanted[k] = 0;
			} else {
				/* Grab the whole block */
				new_addr = build_lwkvma(k, addr,
					elt->num_blks * kind_size[k], prot,
					mmap_flags, pgoff,
					virt_to_phys(elt->phys->base +
						     elt->offset), len);
				if (new_addr <= 0)
					goto alloc_err;

				elt->vma_addr = new_addr;
				elt->free = 0;
				wanted[k] -= elt->num_blks;
				total -= elt->num_blks * kind_size[k];
				addr = new_addr + elt->num_blks * kind_size[k];
				if (!first_addr) {
					first_addr = new_addr;
					mmap_flags |= MAP_FIXED;
				}
				opts->mosp->num_blks[k] -= elt->num_blks;
				if (opts->mosp->report_blks_allocated)
					opts->mosp->blks_allocated[k][elt->phys->nid] += elt->num_blks;
			}
		}
	}

	if (total != 0) {
		pr_err("%s: INTERNAL ERROR: %lld bytes unallocated.\n",
			__func__, total);
		new_addr = -ENOMEM;
		goto alloc_err;
	}

	if (LWKMEM_DEBUG_VERBOSE)
		dump_block_lists(opts->mosp);

	mutex_unlock(&lwkmem_mutex);

	return first_addr;

alloc_err:
	mutex_unlock(&lwkmem_mutex);
	return new_addr;

} /* end of allocate_blocks() */

unsigned long allocate_blocks_fixed(unsigned long inaddr, unsigned long len,
				    unsigned long prot, unsigned long flags,
				    enum allocate_site_t site)
{
	enum lwkmem_kind_t knd, nxt;
	struct allocate_options_t *opts = 0;
	unsigned long addr = inaddr, boundary, delta, ret;
	struct mos_process_t *mosp;

	if (LWKMEM_DEBUG_VERBOSE)
		pr_info("(>) %s(addr=%lx len=%ld, prot=%lx flags=%lx, site=%d)\n",
			__func__, inaddr, len, prot, flags, site);

	if (addr == 0 || !(flags & MAP_FIXED))
		return -EINVAL;

	mosp = current->mos_process;

	for (knd = 0; knd < kind_last; knd++) {
		if (num_dTLB(knd) == 0)
			continue;

		if (knd < kind_last - 1) {
			/* Find the next larger sized TLB that is actually
			 * used:
			 */
			nxt = knd;
			while (num_dTLB(++nxt) == 0)
				;

			/* The amount to allocate in this pass is the lesser of
			 * the distance to the next sized boundary and the
			 * amount remaining.  This value needs to be adjusted
			 * down to a multiple the current TLB size.
			 */
			boundary = roundup(addr, kind_size[nxt]);
			delta = boundary - addr;
			delta = min(delta, len);
			delta = rounddown(delta, kind_size[knd]);

			if (LWKMEM_DEBUG_VERBOSE)
				pr_info("%s: allocating %lld x %s = %ld bytes at %lx for %s alignment. Remaining:%ld\n",
					__func__, delta / kind_size[knd],
					kind_str[knd], delta, addr,
					kind_str[nxt], len - delta);

		} else {
			/* On the last pass, allocate whatever is left. */
			delta = len;
		}

		if (delta == 0)
			continue;

		opts = allocate_options_factory(site, delta, flags, mosp);
		if (unlikely(!opts)) {
			ret = -ENOMEM;
			goto out;
		}

		ret = allocate_blocks(addr, delta, prot, flags, 0, opts);
		kfree(opts);
		if (ret != addr) {
			ret = -ENOMEM;
			goto out;
		}

		addr += delta;
		len -= delta;
	}

	ret = inaddr;

 out:
	if (LWKMEM_DEBUG_VERBOSE)
		pr_info("(<) %s(addr=%lx len=%ld, prot=%lx flags=%lx, site=%d) = %lx\n",
			__func__, inaddr, len, prot, flags, site, ret);

	return ret;
}

static int _all_granules(struct mos_lwk_mem_granule *g)
{
	return 1;
}

static int _in_use_granules(struct mos_lwk_mem_granule *g)
{
	return g->owner > 0;
}

static int _lwkmem_get(unsigned long *lwkm, size_t *n,
		       int (*filter)(struct mos_lwk_mem_granule *))
{
	struct mos_lwk_mem_granule *g;
	int rc;

	rc = 0;

	if (*n < lwkmem_n_nodes) {
		pr_warn("(!) lwkmem request buffer too small: %ld but need %ld\n",
			*n, lwkmem_n_nodes);
		rc = -EINVAL;
		goto out;
	}

	memset(lwkm, 0, lwkmem_n_nodes * sizeof(unsigned long));

	mutex_lock(&lwkmem_mutex);

	list_for_each_entry(g, &mos_lwk_memory_list, list) {

		if (g->nid >= 0 && g->nid < *n) {
			if (filter(g))
				lwkm[g->nid] += g->length;
		} else {
			pr_warn("(!) NID out of bounds: %pS-%lld-%d-%d\n",
				g->base, g->length, g->nid, g->owner);
			rc = -EINVAL;
			goto unlock;
		}
	}

	*n = lwkmem_n_nodes;

 unlock:
	mutex_unlock(&lwkmem_mutex);

 out:
	return rc;
}

int lwkmem_get(unsigned long *lwkm, size_t *n)
{
	return _lwkmem_get(lwkm, n, _all_granules);
}


int lwkmem_reserved_get(unsigned long *lwkm, size_t *n)
{
	return _lwkmem_get(lwkm, n, _in_use_granules);
}


int lwkmem_request(struct mos_process_t *mos_p, unsigned long *req, size_t n)
{
	size_t i;
	int rc;
	unsigned long wanted;
	struct mos_lwk_mem_granule *elt;
	struct mos_lwk_mem_granule *newg;
	struct lwk_process_granule *p_granule;

	rc = 0;
	mos_p->lwkmem = 0;

	if (list_empty(&mos_lwk_memory_list)) {
		pr_err("%s: No memory in mos_lwk_memory_list!\n", __func__);
		return -EINVAL;
	}

	/* There should be no memory reserved for this process yet */
	if (!list_empty(&(mos_p->lwkmem_list))) {
		pr_err("%s: Process %d already has some memory!\n",
			__func__, current->pid);
		return -EINVAL;
	}

	mutex_lock(&lwkmem_mutex);

	for (i = 0; i < n ; i++) {
		if (req[i] == 0)
			continue;

		wanted = max_t(unsigned long, rounddown(req[i], MIN_CHUNK_SIZE),
				MIN_CHUNK_SIZE);
		pr_debug("Requesting %lu bytes from nid %ld\n", wanted, i);

		while (wanted > 0) {
			elt = find_free_granule_for_nid(i);
			if (!elt) {
				rc = -ENOMEM;
				goto unlock;
			}

			if (wanted < elt->length) { /* Split granule into two */
				newg =
				vmalloc(sizeof(struct mos_lwk_mem_granule));
				if (!newg) {
					rc = -ENOMEM;
					goto unlock;
				}

				newg->base = elt->base + elt->length - wanted;
				list_add(&newg->list, &elt->list);

				newg->length = wanted;
				newg->owner = current->pid;
				newg->nid = elt->nid;
				elt->length -= wanted;

				pr_debug("Split granule : new [addr=%p len=%9lluMB owner=%d]\n",
					 newg->base, newg->length >> 20,
					 newg->owner);
				pr_debug("Split granule:  rem [addr=%p len=%9lluMB owner=%d]\n",
					 elt->base, elt->length >> 20,
					 elt->owner);

				/* We should be done */
				elt = newg;
				mos_p->lwkmem += wanted;
				wanted = 0;

			} else { /* Consume entire granule */
				elt->owner = current->pid;
				mos_p->lwkmem += elt->length;
				wanted = wanted - elt->length;

				pr_debug("Entire granule : [addr=%pS len=%lluMB owner=%d] remaining=%lu\n",
					 elt->base, elt->length >> 20,
					 elt->owner, wanted);
			}

			/* Add this granule to the processes' list */
			p_granule = vmalloc(sizeof(struct lwk_process_granule));
			if (!p_granule) {
				rc = -ENOMEM;
				goto unlock;
			}
			p_granule->granule = elt;
			p_granule->offset = 0;
			list_add(&p_granule->list, &mos_p->lwkmem_list);
		}
	}

	pr_debug("Reserved %llu MiB of LWK memory for PID %d\n",
		 mos_p->lwkmem >> 20, current->pid);
	if (LWKMEM_DEBUG_VERBOSE)
		dump_granule_list(&mos_lwk_memory_list);

 unlock:
	mutex_unlock(&lwkmem_mutex);
	return rc;
}

struct allocate_options_t *allocate_options_factory(enum allocate_site_t site,
			    unsigned long len, unsigned long flags,
			    struct mos_process_t *mosp)
{
	struct allocate_options_t *options;

	options = kmalloc(sizeof(struct allocate_options_t), GFP_KERNEL);
	if (!options)
		return NULL;

	options->mosp = mosp;
	options->blocks_wanted = blocks_wanted;
	options->find_available = find_available_blk_list;
	options->divide_block = divide_block;
	options->max_page_size = mosp->max_page_size;

	switch (site) {
	case lwkmem_brk:
		if (mosp->heap_page_size < options->max_page_size)
			options->max_page_size = mosp->heap_page_size;
		break;

	case lwkmem_mmap:
	case lwkmem_mremap:
		break;
	}

	if (mosp->mcdram_minimum > 0) {

		int i;
		enum lwkmem_type_t t;

		options->find_available = find_available_by_nid_list;

		if (len >= mosp->mcdram_minimum &&
		    !(mosp->mcdram_exempt_flags & flags)) {
			options->type_order[0] = lwkmem_mcdram;
			options->type_order[1] = lwkmem_dram;
		} else {
			options->type_order[0] = lwkmem_dram;
			options->type_order[1] = lwkmem_mcdram;
		}

		for (i = 0; i < lwkmem_type_last; i++) {
			int index = options->type_order[i];

			memcpy(options->nid_order[index],
			       mosp->domain_info[index],
			       mosp->domain_info_len[index] * sizeof(int));
			options->nid_order_len[index] =
				mosp->domain_info_len[index];
		}

		if (LWKMEM_DEBUG_VERBOSE)
			for (t = 0; t < lwkmem_type_last; t++)
				for (i = 0; i < options->nid_order_len[t]; i++)
					pr_info("nid_order(%d) = %d\n", i,
						options->nid_order[t][i]);
	}

	return options;
}

int lwkmem_set_domain_info(struct mos_process_t *mos_p, enum lwkmem_type_t typ,
			   unsigned long *nids, size_t n)
{
	size_t i;

	if (n > ARRAY_SIZE(mos_p->domain_info[0]))
		return -EINVAL;

	for (i = 0; i < n; i++) {
		mos_p->domain_info[typ][i] = nids[i];
		pr_debug("domain_info[%s][%ld] = %ld\n", lwkmem_type_str[typ],
			 i, nids[i]);
	}

	mos_p->domain_info_len[typ] = n;

	return 0;
}

unsigned long next_lwkmem_address(unsigned long len, struct mos_process_t *mosp)
{
	unsigned long addr;

	mutex_lock(&lwkmem_mutex);
	addr = mosp->lwkmem_next_addr;
	mosp->lwkmem_next_addr = roundup(mosp->lwkmem_next_addr + len, SZ_1G);
	mutex_unlock(&lwkmem_mutex);

	return addr;
}

long unmap_pagetbl(enum lwkmem_kind_t k, unsigned long addr, unsigned long len)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	spinlock_t *ptl;
	unsigned long end = addr + len;

	if (LWKMEM_DEBUG_VERBOSE)
		pr_info("(>) %s addr=%lx len=%ld (%lx)\n",
			__func__, addr, len, len);

	while (addr < end) {

		pgd = pgd_offset(current->mm, addr);
		pud = pud_offset(pgd, addr);

		if (k == kind_1g) {
			ptl = &current->mm->page_table_lock;
			spin_lock(ptl);
			pud_clear(pud);
			spin_unlock(ptl);
		} else if (k == kind_2m) {
			pmd = pmd_offset(pud, addr);
			ptl = pmd_lock(current->mm, pmd);
			pmd_clear(pmd);
			spin_unlock(ptl);
		} else if (k == kind_4k) {
			pte = get_locked_pte(current->mm, addr, &ptl);
			pte_clear(current->mm, addr, pte);
			spin_unlock(ptl);
		} else {
			pr_err("Other page sizes not supported.\n");
			return -1;
		}

		addr += kind_size[k];
	}

	if (LWKMEM_DEBUG_VERBOSE)
		pr_info("(<) %s addr=%lx len=%ld (%lx)\n",
			__func__, addr, len, len);
	return len;
}

struct blk_list *split_block(struct blk_list *bl, enum lwkmem_kind_t k,
			     unsigned long offset, struct mos_process_t *mosp)
{
	struct blk_list *newb;
	unsigned long blks = offset / kind_size[k];

	if (unlikely((blks >= bl->num_blks) || (offset & (kind_size[k] - 1)))) {
		pr_err("(!) %s cannot split block [%#016llX,%lld] at offset %lx (%ld)\n",
		       __func__, (unsigned long)bl->phys->base + bl->offset,
		       bl->num_blks, offset, blks);
		return NULL;
	}

	newb = vmalloc(sizeof(struct blk_list));
	if (!newb)
		return NULL;

	newb->phys = bl->phys;
	newb->offset = bl->offset + offset;
	newb->num_blks = bl->num_blks - blks;
	newb->vma_addr = bl->vma_addr + offset;
	newb->free = bl->free;
	bl->num_blks = blks;

	list_add(&newb->list, &mosp->blk_list[k]);

	if (LWKMEM_DEBUG_VERBOSE) {
		pr_info("(<) %s bl=%p offs=%lx sz=%llx\n",
			__func__, bl, offset, kind_size[k]);
		dump_block_lists(mosp);
	}

	return newb;
}

static long deallocate_block(unsigned long addr, unsigned long len,
			     struct mos_process_t *mosp)
{
	enum lwkmem_kind_t k;
	struct blk_list *bl, *newb;
	unsigned long sz, offset;
	long rc = 0;

	if (LWKMEM_DEBUG_VERBOSE)
		pr_info("(>) %s addr=%lX len=%ld\n",
			__func__, addr, len);

	for (k = 0; k < kind_last; k++) {
		list_for_each_entry(bl, &mosp->blk_list[k], list) {
			if (bl->free)
				continue;

			sz = bl->num_blks * kind_size[k];

			if (addr >= bl->vma_addr && addr < bl->vma_addr + sz) {
				int left = addr == bl->vma_addr;
				int right = (addr + len) >= (bl->vma_addr + sz);

				if (addr & (kind_size[k] - 1)) {
					pr_warn("(!) %s addr=%lx is not %s aligned\n",
						__func__, addr, kind_str[k]);
					return -1;
				}

				if (!right && ((addr + len) & (kind_size[k] - 1))) {
					pr_warn("(!) %s addr+len=%lx+%ld is not %s aligned\n",
						__func__, addr, len,
						kind_str[k]);
					return -1;
				}

				if (LWKMEM_DEBUG_VERBOSE) {
					char annot = left & right ? 'X' :
						(left ? 'L' :
						 (right ? 'R' : 'M'));

					pr_info("%s %#016lx,%ld -> <%c> [%#016lx-%#016lx] [%#016llx] %3lld x %s = %ld\n",
						__func__, addr, len, annot,
						bl->vma_addr,
						bl->vma_addr + sz - 1,
						(uint64_t)bl->phys->base + bl->offset,
						bl->num_blks, kind_str[k], sz);
				}

				offset = addr - bl->vma_addr;

				if (left && right) {
					/* deallocate entire block */
					bl->vma_addr = 0;
					bl->free = 1;
					mosp->num_blks[k] += bl->num_blks;
					len = sz;
				} else if (left) {
					/* deallocate left side of the block */
					newb = split_block(bl, k, len, mosp);
					if (!newb)
						return -ENOMEM;

					bl->free = 1;
					bl->vma_addr = 0;
					mosp->num_blks[k] += bl->num_blks;
				} else if (right) {
					/* deallocate right side of the block */
					newb = split_block(bl, k, offset, mosp);
					if (!newb)
						return -ENOMEM;

					newb->free = 1;
					newb->vma_addr = 0;
					mosp->num_blks[k] += newb->num_blks;
					len = sz - offset;
				} else {
					/* deallocate middle of the block */
					unsigned long offs2;

					offs2 = addr + len - bl->vma_addr;

					newb = split_block(bl, k, offs2, mosp);
					if (!newb)
						return -ENOMEM;

					newb = split_block(bl, k, offset, mosp);
					if (!newb)
						return -ENOMEM;

					newb->free = 1;
					newb->vma_addr = 0;
					mosp->num_blks[k] += newb->num_blks;
				}

				rc = unmap_pagetbl(k, addr, len);
				goto out;
			}
		}
	}

 out:
	if (LWKMEM_DEBUG_VERBOSE)
		pr_info("(<) %s addr=%lX len=%ld rc=%ld\n",
			__func__, addr, len, rc);
	return rc;
}

long deallocate_blocks(unsigned long addr, unsigned long len,
		       struct mos_process_t *mosp)
{
	long ret = 0;

	mutex_lock(&lwkmem_mutex);

	if (LWKMEM_DEBUG_VERBOSE)
		pr_info("(>) %s addr=%#016lx len=%ld\n", __func__, addr, len);

	len = roundup(len, kind_size[0]);

	while (len > 0) {
		ret = deallocate_block(addr, len, mosp);
		if (ret <= 0)
			goto out;

		len -= ret;
		addr += ret;
	}

 out:
	if (LWKMEM_DEBUG_VERBOSE) {
		dump_block_lists(mosp);
		pr_info("(<) %s addr=%#016lx len=%ld --> ret=%ld\n",
			__func__, addr, len, ret);
	}
	mutex_unlock(&lwkmem_mutex);

	return ret;
}
