/* SPDX-License-Identifier: 0BSD */
/* 8259A 可编程中断控制器。
 *
 * 说明：M0/M1 阶段用 8259 + 8254 让中断先跑起来；M2 会切换到
 * LAPIC + IOAPIC（SMP 必需）。8259 的代码在 APIC 就绪后仍保留，
 * 用于不支持 APIC 的老平台降级路径。
 */
#ifndef FE_PIC_H
#define FE_PIC_H

#include <fe/types.h>

#define FE_IRQ_BASE      0x20   /* 主片映射到向量 32 */
#define FE_IRQ_COUNT     16

void fe_pic_init(void);
void fe_pic_mask(u8 irq);
void fe_pic_unmask(u8 irq);
void fe_pic_mask_all(void);
void fe_pic_eoi(u8 irq);

#endif /* FE_PIC_H */
