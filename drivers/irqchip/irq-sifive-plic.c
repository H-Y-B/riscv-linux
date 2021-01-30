// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017 SiFive
 * Copyright (C) 2018 Christoph Hellwig
 */
#define pr_fmt(fmt) "plic: " fmt
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <asm/smp.h>

/*
 * This driver implements a version of the RISC-V PLIC with the actual layout
 * specified in chapter 8 of the SiFive U5 Coreplex Series Manual:
 *
 *     https://static.dev.sifive.com/U54-MC-RVCoreIP.pdf
 *
 * The largest number supported by devices marked as 'sifive,plic-1.0.0', is
 * 1024, of which device 0 is defined as non-existent by the RISC-V Privileged
 * Spec.
 */

#define MAX_DEVICES			1024
#define MAX_CONTEXTS			15872

/*
 * Each interrupt source has a priority register associated with it.
 * We always hardwire it to one in Linux.
 */
#define PRIORITY_BASE			0
#define     PRIORITY_PER_ID		4

/*
 * Each hart context has a vector of interrupt enable bits associated with it.
 * There's one bit for each interrupt source.
 */
#define ENABLE_BASE			0x2000
#define     ENABLE_PER_HART		0x80

/*
 * Each hart context has a set of control registers associated with it.  Right
 * now there's only two: a source priority threshold over which the hart will
 * take an interrupt, and a register to claim interrupts.
 */
#define CONTEXT_BASE			0x200000
#define     CONTEXT_PER_HART		0x1000
#define     CONTEXT_THRESHOLD		0x00
#define     CONTEXT_CLAIM		0x04

/*@
ref link: https://github.com/riscv/riscv-plic-spec/blob/master/riscv-plic.adoc
----------------------------------------------------------
base + 0x000000 : Reserved  (interrupr source 0 does not exist)
base + 0x000004 : Interrupt source 1 priority
......
base + 0x000FFC : Interrupt source 1023 priority
----------------------------------------------------------
base + 0x001000 : Interrupt Pending bit 0-31
......
base + 0x001000 : Interrupt Pending bit 992-1023



上下文  指的是  核0-s 核1-s......

----------------------------------------------------------
中断使能 (每一个上下文，都有自己对于1024个中断的使能)
base + 0x002000 : Enable bits for source 0-31 on context  0
......
base + 0x00207F : Enable bits for source 992-1023 on context  0
...... on context 1
----------------------------------------------------------
(每一个上下文，都有自己的 优先级阈值、完成ID)
base + 0x1FFFFC : R
base + 0x200000 : Priority threhold for context 0
base + 0x200004 : Claim/complete for context 0 中断ID号
----------------------------------------------------------
base + 0x200FFC : R
base + 0x201000 : Priority threhold for context 1
base + 0x201004 : Claim/complete for context 1
---------------------------------------------------------
*/




static void __iomem *plic_regs;

struct plic_handler {
	bool			present;
	int			ctxid;
};
static DEFINE_PER_CPU(struct plic_handler, plic_handlers);

static inline void __iomem *plic_hart_offset(int ctxid)
{
	return plic_regs + CONTEXT_BASE + ctxid * CONTEXT_PER_HART;
        //@                 0x200000                    0x1000
}

static inline u32 __iomem *plic_enable_base(int ctxid)
{
	return plic_regs + ENABLE_BASE + ctxid * ENABLE_PER_HART;
}

/*
 * Protect mask operations on the registers given that we can't assume that
 * atomic memory operations work on them.
 */
static DEFINE_RAW_SPINLOCK(plic_toggle_lock);

static inline void plic_toggle(int ctxid, int hwirq, int enable)//开关 ctxid中的 hwirq中断 
{
	u32 __iomem *reg = plic_enable_base(ctxid) + (hwirq / 32);
	u32 hwirq_mask = 1 << (hwirq % 32);

	raw_spin_lock(&plic_toggle_lock);  //@加锁
	if (enable)
		writel(readl(reg) | hwirq_mask, reg);
	else
		writel(readl(reg) & ~hwirq_mask, reg);
	raw_spin_unlock(&plic_toggle_lock);//@解锁
}

static inline void plic_irq_toggle(struct irq_data *d, int enable)
{
	int cpu;

	writel(enable, plic_regs + PRIORITY_BASE + d->hwirq * PRIORITY_PER_ID); //设置中断优先级
	//@                               0x0                        0x4

	for_each_cpu(cpu, irq_data_get_affinity_mask(d)) {
		struct plic_handler *handler = per_cpu_ptr(&plic_handlers, cpu);

		if (handler->present)
			plic_toggle(handler->ctxid, d->hwirq, enable);
	}
}

static void plic_irq_enable(struct irq_data *d)
{
	plic_irq_toggle(d, 1);
}

static void plic_irq_disable(struct irq_data *d)
{
	plic_irq_toggle(d, 0);
}

static struct irq_chip plic_chip = {
	.name		= "SiFive PLIC",
	/*
	 * There is no need to mask/unmask PLIC interrupts.  They are "masked"
	 * by reading claim and "unmasked" when writing it back.
	 */
	.irq_enable	= plic_irq_enable,
	.irq_disable	= plic_irq_disable,
};
//@以上 创建了irq_chip结构体，对应一个中断控制器###################################################


static int plic_irqdomain_map(struct irq_domain *d, unsigned int irq,
			      irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &plic_chip, handle_simple_irq);
	irq_set_chip_data(irq, NULL);
	irq_set_noprobe(irq);
	return 0;
}

static const struct irq_domain_ops plic_irqdomain_ops = {
	.map		= plic_irqdomain_map,
	.xlate		= irq_domain_xlate_onecell,
};

static struct irq_domain *plic_irqdomain;

/*
 * Handling an interrupt is a two-step process: first you claim the interrupt
 * by reading the claim register, then you complete the interrupt by writing
 * that source ID back to the same claim register.  This automatically enables
 * and disables the interrupt, so there's nothing else to do.
 */
static void plic_handle_irq(struct pt_regs *regs)      //@ PLIC 处理外部中断 
{
	struct plic_handler *handler = this_cpu_ptr(&plic_handlers);//@获取本hart的  per-cpu变量


	void __iomem *claim = plic_hart_offset(handler->ctxid) + CONTEXT_CLAIM;//@ 本hart的 claim寄存器 地址

	irq_hw_number_t hwirq;             //@ type:unsigned long

	WARN_ON_ONCE(!handler->present);

	csr_clear(sie, SIE_SEIE); //@ 关闭S-外部中断
	while ((hwirq = readl(claim))) {     //@ read claim, id为0表示没有外部中断 
		int irq = irq_find_mapping(plic_irqdomain, hwirq);  //@ interrupt number

		//@处理中断
		if (unlikely(irq <= 0))
			pr_warn_ratelimited("can't find mapping for hwirq %lu\n",hwirq);
		else
			generic_handle_irq(irq);
		
                writel(hwirq, claim);                //@ write claim
	}
	csr_set(sie, SIE_SEIE);  //@ 开启S-外部中断
}

/*
 * Walk up the DT tree until we find an active RISC-V core (HART) node and
 * extract the cpuid from it.
 */
static int plic_find_hart_id(struct device_node *node)
{
	for (; node; node = node->parent) {
		if (of_device_is_compatible(node, "riscv"))
			return riscv_of_processor_hartid(node);
	}

	return -1;
}

static int __init plic_init(struct device_node *node,    //@ PLIC init
		struct device_node *parent)
{
	int error = 0, nr_handlers, nr_mapped = 0, i;
	u32 nr_irqs;

	if (plic_regs) {
		pr_warn("PLIC already present.\n");
		return -ENXIO;
	}

	plic_regs = of_iomap(node, 0);//@PLIC基地址
	if (WARN_ON(!plic_regs))
		return -EIO;

	error = -EINVAL;
	of_property_read_u32(node, "riscv,ndev", &nr_irqs);//@ 问题：nr_irq的值是多少？ 有多少个外部中断
	if (WARN_ON(!nr_irqs))
		goto out_iounmap;

	nr_handlers = of_irq_count(node);//@问题：nr_handlers的值是多少？有多少核心，每个核心有两个11和9
	if (WARN_ON(!nr_handlers))
		goto out_iounmap;
	if (WARN_ON(nr_handlers < num_possible_cpus()))
		goto out_iounmap;

	error = -ENOMEM;
	//@ 创建irq_domain（抽象的中断控制器）
	plic_irqdomain = irq_domain_add_linear(node, 
										   nr_irqs + 1,         //该中断控制器支持的irq的个数
										   &plic_irqdomain_ops, 
										   NULL);
	if (WARN_ON(!plic_irqdomain))
		goto out_iounmap;

	for (i = 0; i < nr_handlers; i++) {//@遍历  上下文； 双核，4次
		/*
			在设备数中的 中断控制器节点 中的    interrupts-extend=<cpu0  0xffff_ffff  M-外部中断
															  cpu0  0x9          S-外部中断
															  cpu1  0xffff_ffff  M-外部中断
															  cpu1  0x9>         S-外部中断
		
		*/
		struct of_phandle_args parent;
		struct plic_handler *handler;
		irq_hw_number_t hwirq;
		int cpu, hartid;

		if (of_irq_parse_one(node, i, &parent)) {
			pr_err("failed to parse parent for context %d.\n", i);
			continue;
		}

		/* skip context holes */
		if (parent.args[0] == -1)  //@  如果是 M-外部中断 忽略；  如果是 S-外部中断 继续
			continue;

		hartid = plic_find_hart_id(parent.np);
		if (hartid < 0) {
			pr_warn("failed to parse hart ID for context %d.\n", i);
			continue;
		}

		cpu = riscv_hartid_to_cpuid(hartid);
		handler = per_cpu_ptr(&plic_handlers, cpu);
		handler->present = true;
		handler->ctxid = i;//与PLIC寄存器绑定  0-[cpu0-s]  1-[cpu1-s]

		/* priority must be > threshold to trigger an interrupt */
		writel(0, plic_hart_offset(i) + CONTEXT_THRESHOLD);//@  把 上下文的优先级阈值  都设置为 0， 这样只要 优先级大于0，就可以响应
		for (hwirq = 1; hwirq <= nr_irqs; hwirq++)
			plic_toggle(i, hwirq, 0);
		nr_mapped++;
	}

	pr_info("mapped %d interrupts to %d (out of %d) handlers.\n",    //@ boot log: in plic_init function
		nr_irqs, nr_mapped, nr_handlers);
	set_handle_irq(plic_handle_irq);   //@ set external interrupt haldler in kernel/irq/handle.c
	return 0;

out_iounmap:
	iounmap(plic_regs);
	return error;
}

IRQCHIP_DECLARE(sifive_plic, "sifive,plic-1.0.0", plic_init);
IRQCHIP_DECLARE(riscv_plic0, "riscv,plic0", plic_init); /* for legacy systems */
/*
#define IRQCHIP_DECLARE(name, compat, fn) OF_DECLARE_2(irqchip, name, compat, fn)
                                |
								v
#define OF_DECLARE_2(table, name, compat, fn) \ 
        _OF_DECLARE(table, name, compat, fn, of_init_fn_2)
								|
								v
#define _OF_DECLARE(table, name, compat, fn, fn_type)     \ 
static const struct of_device_id __of_table_##name        \ 
        __used __section(__##table##_of_table)            \ 
         = { .compatible = compat,                        \ 
             .data = (fn == (fn_type)NULL) ? fn : fn  }

static const struct of_device_id __of_table_##name        \ 
        __used __section(__##table##_of_table)            \ 
         = { .compatible = "riscv,plic0",                        \ 
             .data =   plic_init    }
struct of_device_id {
	char	name[32];
	char	type[32];
	char	compatible[128];
	const void *data;
};
*/
