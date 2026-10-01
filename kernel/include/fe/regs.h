/* SPDX-License-Identifier: 0BSD */
/* 中断/异常发生时的寄存器现场。
 *
 * ★ 字段顺序必须与**栈上的真实内存布局**一致，而不是与压栈顺序一致。★
 *
 * 推导：栈向低地址增长。isr_common 依次 push rax, rbx, ..., r15，
 * 因此最后压入的 r15 位于**最低**地址，最先压入的 rax 位于**最高**地址。
 * 内存从低到高即为：r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax。
 * 紧随其后的是桩代码压入的 vector 与 error，最后是 CPU 自动压入的 rip/cs/rflags/rsp/ss。
 *
 * （曾经把这里写成 rax 在前 —— 因为「压栈顺序」与「内存顺序」恰好相反，
 *   结果所有寄存器都写到了错误偏移上。典型症状是新建线程的入口地址丢失、
 *   异常 dump 里的寄存器张冠李戴。）
 *
 * 任何一侧改动都必须同步 arch/x86_64/isr.asm。
 */
#ifndef FE_REGS_H
#define FE_REGS_H

#include <fe/types.h>
#include <fe/compiler.h>

struct fe_regs {
    /* isr_common 压入的部分（按内存从低到高排列） */
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;

    /* 桩代码压入的中断向量号与错误码（无错误码的异常补 0） */
    u64 vector;
    u64 error;

    /* CPU 自动压入的部分 */
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;
    u64 ss;
} FE_PACKED;

FE_STATIC_ASSERT(sizeof(struct fe_regs) == 22 * 8, "fe_regs 布局必须与 isr.asm 一致");

/* ★ 交给用户态处理者的现场（K5，docs/18-user-fault-handler.md §2.2.3）★
 *
 * ★ 顺序与 struct fe_regs **逐字段一致**，只换掉两个"内核内部记账"字段 ★
 *   fe_regs.vector → fe_fault_regs.vector      （同义：异常向量号）
 *   fe_regs.error  → fe_fault_regs.error_code  （改名只是为了让用户态别猜语义）
 * 于是"现场怎么摆"这件事仍然**只有一个来源**（本文件顶部那段布局推导），
 * 而用户态拿到的仍然是一份**完整**现场——只是多了两个伴生字段。
 *
 * ★ 为什么整份交、一个字段都不裁 ★
 * 能不能处置取决于现场完整性：处理 `#PF` 要 `cr2` 与错误码，跳过出错指令要
 * `rip`，从处理者返回要 `rsp`/`rflags`，判断"这是不是我的 JIT 代码"要 `cs`。
 * 少任何一个都会逼出第二套 syscall 去补读。代价只有 200 字节的定长拷贝。 */
struct fe_fault_regs {
    /* ---- 与 struct fe_regs 逐字段对齐的"现场"部分 ---- */
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector;         /* 0..31，异常向量号 */
    u64 error_code;     /* CPU 压入的错误码；无错误码的向量由桩补 0 */
    u64 rip, cs, rflags, rsp, ss;

    /* ---- 现场之外、但缺了就处置不了的三项（只读）---- */
    u64 cr2;            /* 出错线性地址；只有 #PF(14) 有意义，其余为 0 */
    u64 thread_id;      /* 出错线程的 id —— 处理者要按线程分流时用 */
    u64 fault_count;    /* 本线程第几次被投递（含本次，从 1 起）—— 诊断用 */
} FE_PACKED;

FE_STATIC_ASSERT(sizeof(struct fe_fault_regs) == 25 * 8,
                 "fe_fault_regs 布局变了（用户态按 200 字节读）");
/* ★ 这一条才是真正的钉子：两份结构的"现场部分"必须逐字段对齐 ★
 * 少了它，将来有人改 fe_regs 却在 fe_fault_regs 里漏改一个字段时，
 * 编译**不会**报错——而症状是"处理者拿到的 rip 是别人的 rsp"，
 * 那是这一整套机制里最难查的一类错。 */
FE_STATIC_ASSERT(sizeof(struct fe_fault_regs) > sizeof(struct fe_regs),
                 "fe_fault_regs 必须包含整份现场再加上伴生字段");

#endif /* FE_REGS_H */
