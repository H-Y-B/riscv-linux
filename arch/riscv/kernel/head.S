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

#include <asm/thread_info.h>
#include <asm/asm-offsets.h>
#include <asm/asm.h>
#include <linux/init.h>
#include <linux/linkage.h>
#include <asm/thread_info.h>
#include <asm/page.h>
#include <asm/csr.h>

__INIT
ENTRY(_start)  //0x0000000080200000<->0xffffffe000000000
	/* Mask all interrupts */
	csrw sie, zero

	/* Load the global pointer */
.option push                      //入栈；  入栈什么？之前option的设置  为什么要入栈？将之前的设置存起来，设置新的option，即下面的norelax
.option norelax                   //不允许链接器松弛 ； “松弛”？
	la gp, __global_pointer$
.option pop                       //出栈

	/*
	 * Disable FPU to detect illegal usage of
	 * floating point in kernel space
	 */
	li t0, SR_FS
	csrc sstatus, t0

	/* Pick one hart to run the main boot sequence */
	la a3, hart_lottery
	li a2, 1
	amoadd.w a3, a2, (a3)
	bnez a3, .Lsecondary_start //@ only a hart succeed to perform amoadd.w and continue;
                               //@ other harts perform Lsecondary_start
	



	/* Clear BSS for flat non-ELF images */
	la a3, __bss_start
	la a4, __bss_stop
	ble a4, a3, clear_bss_done
clear_bss:
	REG_S zero, (a3)  //store 0 ->(a3)
	add a3, a3, RISCV_SZPTR
	blt a3, a4, clear_bss
clear_bss_done:





	/* Save hart ID and DTB physical address */
	mv s0, a0
	mv s1, a1

	//将hart id赋值给boot_cpu_hartid变量 ，（该变量定义在arch/riscv/kernel/setup.c中）
	la a2, boot_cpu_hartid
	REG_S a0, (a2)       //a0(hart id) -> (a2)(boot_cpu_hartid)

	/* Initialize page tables and relocate to virtual addresses */
	la sp, init_thread_union + THREAD_SIZE
	call setup_vm              //in arch/riscv/kernel/setup.c
	call relocate

	/* Restore C environment */
	la tp, init_task         //值：0xffffffe008e07cc0
	sw zero, TASK_TI_CPU(tp)

	la sp, init_thread_union //值：0xffffffe008e00000
	li a0, ASM_THREAD_SIZE
	add sp, sp, a0           //值：0xffffffe008e02000

	/* Start the kernel */
	mv a0, s0
	mv a1, s1
	call parse_dtb          //@call function : device tree
 	tail start_kernel




relocate:
	/* Relocate return address */
	li a1, PAGE_OFFSET
	la a0, _start
	sub a1, a1, a0
	add ra, ra, a1  //保存返回地址

	/* Point stvec to virtual address of intruction after satp write */
	la a0, 1f
	add a0, a0, a1
	csrw stvec, a0  //临时的异常入口
					//值：0xffffffe0000000c4  PC变成虚拟地址的关键

	/* Compute satp for kernel page tables, but don't load it yet */
	la a2, swapper_pg_dir
	srl a2, a2, PAGE_SHIFT //右移12位 
	li a1, SATP_MODE       //地址转换模式   32位-Sv32   其他-Sv39  
	or a2, a2, a1          //MODE-ASID-PPN

	/*
	 * Load trampoline page directory, which will cause us to trap to
	 * stvec if VA != PA, or simply fall through if VA == PA
	 */
	la a0, trampoline_pg_dir//数组地址：0x0000000080220000 实地址
	srl a0, a0, PAGE_SHIFT  //右移12位 
	or a0, a0, a1           //MODE-ASID-PPN
	sfence.vma
	csrw sptbr, a0 //根页表 地址 0x【8】【0000】【00000080220】  
				   //从这里开始 进入虚拟地址
				   //该指令执行完后，下条指令发生缺页异常进入 1f （虚拟地址的向量入口地址）
   /*
	*这里 构建 了两个 根页表地址 from swapper_pg_dir and trampoline_pg_dir
	*这两个的区别是什么？
	* swapper_pg_dir 表示内核页表的起始地址，是真正需要的内核页表
	* trampoline_pg_dir “trampoline”是蹦床的意思，充当临时的页表，因为在进入 1f（以虚拟地址为入口的异常）需要页表
	*
	* 为什么把sptbr赋值了两次，第二次赋值还是通过异常来实现的
	* 答：  异常入口的地址就是虚拟地址了，使得pc从0x00000000802****变成了0xffffffe0000****
	*/

.align 2
1:
	/* Set trap vector to spin forever to help debug */
	la a0, .Lsecondary_park
	csrw stvec, a0  //把死循环 作为 异常向量入口

	/* Reload the global pointer */
.option push
.option norelax
	la gp, __global_pointer$
.option pop

	/* Switch to kernel page tables */
	csrw sptbr, a2  //根页表 地址  0x80000000000890d0

	ret
//end relocate


.Lsecondary_start:
#ifdef CONFIG_SMP
	li a1, CONFIG_NR_CPUS           //配置的处理器核心数
	bgeu a0, a1, .Lsecondary_park   //多余的处理器核 去 死循环

	/* Set trap vector to spin forever to help debug */
	la a3, .Lsecondary_park
	csrw stvec, a3    //把死循环 作为 异常向量入口

	slli a3, a0, LGREG  //a3= hartid << 3
	la a1, __cpu_up_stack_pointer //void 数组[NR_CPUS]  这里的void和左移3（64位cpu）对应
	la a2, __cpu_up_task_pointer  //void 数组[NR_CPUS]
	add a1, a3, a1
	add a2, a3, a2

	/*
	 * This hart didn't win the lottery, so we wait for the winning hart to
	 * get far enough along the boot process that it should continue.
	 */
.Lwait_for_cpu_up:
	/* FIXME: We should WFI to save some energy here. */
	REG_L sp, (a1)
	REG_L tp, (a2)
	beqz sp, .Lwait_for_cpu_up
	beqz tp, .Lwait_for_cpu_up
	fence

	/* Enable virtual memory and relocate to virtual address */
	call relocate

	tail smp_callin
#endif

.align 2
.Lsecondary_park: //让其他核在这里死循环
	/* We lack SMP support or have too many harts, so park this hart */
	wfi
	j .Lsecondary_park
END(_start)

__PAGE_ALIGNED_BSS
	/* Empty zero page */
	.balign PAGE_SIZE
