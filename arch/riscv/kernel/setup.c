/*
 * Copyright (C) 2009 Sunplus Core Technology Co., Ltd.
 *  Chen Liqin <liqin.chen@sunplusct.com>
 *  Lennox Wu <lennox.wu@sunplusct.com>
 * Copyright (C) 2012 Regents of the University of California
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see the file COPYING, or write
 * to the Free Software Foundation, Inc.,
 */

#include <linux/init.h>
#include <linux/mm.h>
#include <linux/memblock.h>
#include <linux/sched.h>
#include <linux/initrd.h>
#include <linux/console.h>
#include <linux/screen_info.h>
#include <linux/of_fdt.h>
#include <linux/of_platform.h>
#include <linux/sched/task.h>
#include <linux/swiotlb.h>

#include <asm/setup.h>
#include <asm/sections.h>
#include <asm/pgtable.h>
#include <asm/smp.h>
#include <asm/sbi.h>
#include <asm/tlbflush.h>
#include <asm/thread_info.h>

#ifdef CONFIG_EARLY_PRINTK
static void sbi_console_write(struct console *co, const char *buf,
			      unsigned int n)
{
	int i;

	for (i = 0; i < n; ++i) {
		if (buf[i] == '\n')
			sbi_console_putchar('\r');
		sbi_console_putchar(buf[i]);
	}
}

struct console riscv_sbi_early_console_dev __initdata = {
	.name	= "early",
	.write	= sbi_console_write,
	.flags	= CON_PRINTBUFFER | CON_BOOT | CON_ANYTIME,
	.index	= -1
};
#endif

#ifdef CONFIG_DUMMY_CONSOLE
struct screen_info screen_info = {
	.orig_video_lines	= 30,
	.orig_video_cols	= 80,
	.orig_video_mode	= 0,
	.orig_video_ega_bx	= 0,
	.orig_video_isVGA	= 1,
	.orig_video_points	= 8
};
#endif

unsigned long va_pa_offset;
EXPORT_SYMBOL(va_pa_offset);
unsigned long pfn_base;
EXPORT_SYMBOL(pfn_base);

unsigned long empty_zero_page[PAGE_SIZE / sizeof(unsigned long)] __page_aligned_bss;
EXPORT_SYMBOL(empty_zero_page);

/* The lucky hart to first increment this variable will boot the other cores */
atomic_t hart_lottery;
unsigned long boot_cpu_hartid;

unsigned long __cpuid_to_hartid_map[NR_CPUS] = {
	[0 ... NR_CPUS-1] = INVALID_HARTID
};

void __init smp_setup_processor_id(void)
{
	cpuid_to_hartid_map(0) = boot_cpu_hartid;  //@将 cpu id 0  与 boot_cpu_hartid 绑定
}

#ifdef CONFIG_BLK_DEV_INITRD
static void __init setup_initrd(void)
{
	unsigned long size;

	if (initrd_start >= initrd_end) {
		printk(KERN_INFO "initrd not found or empty");
		goto disable;
	}
	if (__pa(initrd_end) > PFN_PHYS(max_low_pfn)) {
		printk(KERN_ERR "initrd extends beyond end of memory");
		goto disable;
	}

	size =  initrd_end - initrd_start;
	memblock_reserve(__pa(initrd_start), size);
	initrd_below_start_ok = 1;

	printk(KERN_INFO "Initial ramdisk at: 0x%p (%lu bytes)\n",
		(void *)(initrd_start), size);
	return;
disable:
	pr_cont(" - disabling initrd\n");
	initrd_start = 0;
	initrd_end = 0;
}
#endif /* CONFIG_BLK_DEV_INITRD */

pgd_t swapper_pg_dir[PTRS_PER_PGD] __page_aligned_bss;
pgd_t trampoline_pg_dir[PTRS_PER_PGD] __initdata __aligned(PAGE_SIZE);//@一个页大小

#ifndef __PAGETABLE_PMD_FOLDED
#define NUM_SWAPPER_PMDS ((uintptr_t)-PAGE_OFFSET >> PGDIR_SHIFT)
pmd_t swapper_pmd[PTRS_PER_PMD*((-PAGE_OFFSET)/PGDIR_SIZE)] __page_aligned_bss;
pmd_t trampoline_pmd[PTRS_PER_PGD] __initdata __aligned(PAGE_SIZE);
#endif
/* 四级分页机制
 * 页全局目录（Page Global Directory）
 * 页上级目录（Page Upper Directory）
 * 页中间目录（Page Middle Directory） PMD
 * 页表（Page Table）
 */


asmlinkage void __init setup_vm(void)
{
	extern char _start;
	uintptr_t i;
	uintptr_t pa = (uintptr_t) &_start;   //0x0000000080200000
	pgprot_t prot = __pgprot(pgprot_val(PAGE_KERNEL) | _PAGE_EXEC);//@内核 页表项的属性

	va_pa_offset = PAGE_OFFSET - pa;    //@0xffffffe000000000-0x0000000080200000 = 0xffffffdf7fe00000
										//@PAGE_OFFSET其实就是物理地址与线性地址之间的位移量。
	pfn_base = PFN_DOWN(pa);            //@0x0000000000080200

	/* Sanity check alignment and size */
	BUG_ON((PAGE_OFFSET % PGDIR_SIZE) != 0);
	BUG_ON((pa % (PAGE_SIZE * PTRS_PER_PTE)) != 0);

#ifndef __PAGETABLE_PMD_FOLDED
	trampoline_pg_dir[(PAGE_OFFSET >> PGDIR_SHIFT) % PTRS_PER_PGD] =
		pfn_pgd(PFN_DOWN((uintptr_t)trampoline_pmd),
			__pgprot(_PAGE_TABLE));
	trampoline_pmd[0] = pfn_pmd(PFN_DOWN(pa), prot);

	for (i = 0; i < (-PAGE_OFFSET)/PGDIR_SIZE; ++i) {
		size_t o = (PAGE_OFFSET >> PGDIR_SHIFT) % PTRS_PER_PGD + i;
		swapper_pg_dir[o] =
			pfn_pgd(PFN_DOWN((uintptr_t)swapper_pmd) + i,
				__pgprot(_PAGE_TABLE));
	}
	for (i = 0; i < ARRAY_SIZE(swapper_pmd); i++)
		swapper_pmd[i] = pfn_pmd(PFN_DOWN(pa + i * PMD_SIZE), prot);
#else
	trampoline_pg_dir[(PAGE_OFFSET >> PGDIR_SHIFT) % PTRS_PER_PGD] =  //@虚拟地址； 赋值 trampoline_pg_dir[]中的一个全局页表项（leaf pte）
		pfn_pgd(PFN_DOWN(pa), prot);

	for (i = 0; i < (-PAGE_OFFSET)/PGDIR_SIZE; ++i) {//@遍历  从PAGE_OFFSET开始的全局页表项
		size_t o = (PAGE_OFFSET >> PGDIR_SHIFT) % PTRS_PER_PGD + i;
		swapper_pg_dir[o] =
			pfn_pgd(PFN_DOWN(pa + i * PGDIR_SIZE), prot);//@（leaf pte）
	}
#endif
}

void __init parse_dtb(unsigned int hartid, void *dtb) //@call function : device tree
{
	early_init_dt_scan(__va(dtb));//@in drivers/of/fdt.c
}

static void __init setup_bootmem(void)
{
	struct memblock_region *reg;
	phys_addr_t mem_size = 0;

	/* Find the memory region containing the kernel */
	for_each_memblock(memory, reg) {
		phys_addr_t vmlinux_end = __pa(_end);    //@ 80d0c07c
		phys_addr_t end = reg->base + reg->size; //@ reg-base:80200000  reg-size:7fe00000

		if (reg->base <= vmlinux_end && vmlinux_end <= end) {
			/*
			 * Reserve from the start of the region to the end of
			 * the kernel
			 */
			memblock_reserve(reg->base, vmlinux_end - reg->base);//@ 增加了一个逻辑块，但是其增加到memblock.reserved中
			mem_size = min(reg->size, (phys_addr_t)-PAGE_OFFSET);//@ 0x7fe00000
		}
	}
	BUG_ON(mem_size == 0);

	set_max_mapnr(PFN_DOWN(mem_size));   //@实际内存中可以容纳的 页 个数
	max_low_pfn = memblock_end_of_DRAM();//@0x100000000 [0x80200000~0x100000000]  bug:max_low_pfn should be pfn_size not byte_size.

#ifdef CONFIG_BLK_DEV_INITRD
	setup_initrd();
#endif /* CONFIG_BLK_DEV_INITRD */

	early_init_fdt_reserve_self();      //@ 预留设备树自身加载所占内存
	early_init_fdt_scan_reserved_mem(); //@ 初始化设备树扫描reserved-memory节点预留内存
	memblock_allow_resize();
	memblock_dump_all();

	for_each_memblock(memory, reg) {
		//@ reg-base:80200000  reg-size:7fe00000
		unsigned long start_pfn = memblock_region_memory_base_pfn(reg);//@  0x 80200
		unsigned long end_pfn = memblock_region_memory_end_pfn(reg);   //@  0x100000

		//@将某一类型的memblock区域 设置 所属的 nodeID
		memblock_set_node(PFN_PHYS(start_pfn),                //@80200000
		                  PFN_PHYS(end_pfn - start_pfn),      //@7fe00000
		                  &memblock.memory, 
						  0);
	}
}

void __init setup_arch(char **cmdline_p)
{
#if defined(CONFIG_EARLY_PRINTK)
       if (likely(early_console == NULL)) {
               early_console = &riscv_sbi_early_console_dev;
               register_console(early_console);
       }
#endif
	*cmdline_p = boot_command_line;//@编译之前配置的Boot Options
				       //@或者 bootloader传入的bootargs	

	parse_early_param();//@解析 saved_command_line

	init_mm.start_code = (unsigned long) _stext;
	init_mm.end_code   = (unsigned long) _etext;
	init_mm.end_data   = (unsigned long) _edata;
	init_mm.brk        = (unsigned long) _end;   //初始化 堆的起始地址

	setup_bootmem();
	paging_init();            //@in arch/riscv/mm/init.c

#ifdef CONFIG_SWIOTLB
	swiotlb_init(1);  //@in kernel/dma/swiotlb.c
#endif

#ifdef CONFIG_SMP
	setup_smp();//@ in arch/riscv/kernel/smpboot.c
#endif

#ifdef CONFIG_DUMMY_CONSOLE
	conswitchp = &dummy_con;
#endif

	riscv_fill_hwcap();
}

