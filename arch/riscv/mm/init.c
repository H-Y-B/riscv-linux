/*
 * Copyright (C) 2012 Regents of the University of California
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation, version 2.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/mm.h>
#include <linux/memblock.h>
#include <linux/initrd.h>
#include <linux/swap.h>
#include <linux/sizes.h>

#include <asm/tlbflush.h>
#include <asm/sections.h>
#include <asm/pgtable.h>
#include <asm/io.h>

static void __init zone_sizes_init(void)
{
	unsigned long max_zone_pfns[MAX_NR_ZONES] = { 0, };

#ifdef CONFIG_ZONE_DMA32
	max_zone_pfns[ZONE_DMA32] = PFN_DOWN(min(4UL * SZ_1G, max_low_pfn));//@[0]=0x100000
#endif
	max_zone_pfns[ZONE_NORMAL] = max_low_pfn;                           //@[1]=0x100000000  bug : max_low_pfn should be pfn_size not byte_size.
	//@该数组是UMA系统内存结点的各个内存域的最大PFN.

	free_area_init_nodes(max_zone_pfns);  //@ in mm/page_alloc.c
}

void setup_zero_page(void)
{
	memset((void *)empty_zero_page, 0, PAGE_SIZE);
	//@为了适应COW（Copy On Write）机制，内核中定义了一个empty_zero_page，即全部为0的一个页面，
	//@当我们读取一个共享的页面时，读出的全部为0，就是读出的empty_zero_page中的内容，
	//@只有在写的时候才会COW。
}

void __init paging_init(void)
{
	setup_zero_page();
	local_flush_tlb_all(); //@ __asm__ __volatile__ ("sfence.vma" : : : "memory");
	zone_sizes_init();
}

void __init mem_init(void)
{
#ifdef CONFIG_FLATMEM
	BUG_ON(!mem_map);
#endif /* CONFIG_FLATMEM */

	high_memory = (void *)(__va(PFN_PHYS(max_low_pfn)));
	memblock_free_all();

	mem_init_print_info(NULL);
}

void free_initmem(void)
{
	free_initmem_default(0);
}

#ifdef CONFIG_BLK_DEV_INITRD
void free_initrd_mem(unsigned long start, unsigned long end)
{
}
#endif /* CONFIG_BLK_DEV_INITRD */
