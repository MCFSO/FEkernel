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

/* ★ 最近一次来自 ring 3 的异常向量，以及累计次数 ★
 *
 * ★ K5 之后这两条的语义是"**全部**用户态异常"，不是"没被消化的" ★
 * （docs/18 §6.1.1）被处理者接管的异常在 `user_fault` 的出口③返回，
 * 走不到出口④的记账点——所以 K5 落地时**在出口③也补记了一笔**，
 * 让这两条的含义保持"全部"。想区分"被处理者消化的"读
 * `fe_user_fault_handled_count()`。
 *
 * 自检用它区分「被 #GP 挡下来了」与「因为别的原因碰巧也失败了」。 */
u64 fe_last_user_fault_vector(void);
u64 fe_user_fault_count(void);
/* 按向量分别计数（2c 加的）：判据该说"#GP **发生过**"，而不是
 * "最后一次是 #GP"——后者把断言押在异常发生的**顺序**上，
 * 而顺序不是被验的性质（exectest 的 mprotect 探针会故意制造一次 #PF，
 * 见 kernel/task/user.c 的 fe_selftest_user）。 */
u64 fe_user_fault_count_of(u32 vector);

/* ★ 被用户态处理者**消化掉**的次数（K5）★ 见上面那段说明。 */
u64 fe_user_fault_handled_count(void);

#endif /* FE_IDT_H */
