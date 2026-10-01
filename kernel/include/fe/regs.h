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

#endif /* FE_REGS_H */
