/* SPDX-License-Identifier: 0BSD */
/* 中断描述符表与异常分发。 */
#ifndef FE_IDT_H
#define FE_IDT_H

#include <fe/types.h>
#include <fe/regs.h>

/* 中断处理回调。可以对 r 指向的现场做修改（例如改 rip 跳过出错指令）。 */
typedef void (*fe_isr_handler_t)(struct fe_regs *r);

void fe_idt_init(void);
void fe_idt_set_handler(u8 vector, fe_isr_handler_t handler);

/* 由 isr.asm 的统一入口调用 */
void fe_isr_dispatch(struct fe_regs *r);

/* 中断总开关，成对使用 */
void fe_interrupts_enable(void);
void fe_interrupts_disable(void);

const char *fe_exception_name(u64 vector);

/* 最近一次来自 ring 3 的异常向量，以及累计次数。
 * 自检用它区分「被 #GP 挡下来了」与「因为别的原因碰巧也失败了」。 */
u64 fe_last_user_fault_vector(void);
u64 fe_user_fault_count(void);

#endif /* FE_IDT_H */
