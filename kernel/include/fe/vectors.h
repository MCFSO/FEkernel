/* SPDX-License-Identifier: 0BSD */
/* 中断向量分配表。
 *
 * 0..31    CPU 异常（见 idt.c 的异常名表）
 * 32..47   传统 ISA 中断（IRQ0..15）：8259 直接映射，或经 IOAPIC 路由
 * 48..63   内核软件中断（yield / 线程退出 等）
 * 64..79   LAPIC 本地中断（定时器、性能计数、错误）
 * 80..     设备 MSI 向量（M6 起使用）
 * 255      LAPIC 伪中断（spurious）
 */
#ifndef FE_VECTORS_H
#define FE_VECTORS_H

#define FE_VEC_EXCEPTION_BASE 0
#define FE_VEC_EXCEPTION_MAX  31

#define FE_VEC_IRQ_BASE   32
#define FE_VEC_IRQ_MAX    47
#define FE_VEC_IRQ(n)     (FE_VEC_IRQ_BASE + (n))

#define FE_VEC_KERNEL_BASE 48
#define FE_VEC_YIELD       48    /* 主动让出 CPU */
#define FE_VEC_THREAD_EXIT 49    /* 线程退出（不返回） */
#define FE_VEC_RESCHED     50    /* 其它 CPU 发来的重新调度 IPI（M9 用） */
# define FE_VEC_KERNEL_MAX  63

/* syscall 指令进入内核时使用的伪向量号：仅用于标记栈帧来源，
 * 返回路径据此选择 sysretq 还是 iretq。 */
#define FE_VEC_SYSCALL     0x80

#define FE_VEC_LAPIC_BASE  64
#define FE_VEC_LAPIC_TIMER 64
#define FE_VEC_LAPIC_ERROR 65
#define FE_VEC_LAPIC_SPUR  255

#define FE_VEC_MSI_BASE    80

/* 96..127 外部设备中断（M6 起使用）：由 fe_irq_register 动态分配，一个 IRQ 一个向量。
 * 为什么不复用 32..47：那段是传统 ISA 的固定映射，多个设备共享一个向量时
 * 中断处理程序无法分辨是谁触发的，只能靠轮询所有驱动——微内核里这正是要避免的。 */
#define FE_VEC_GSI_BASE    96
#define FE_VEC_GSI_COUNT   32
#define FE_VEC_GSI_MAX     (FE_VEC_GSI_BASE + FE_VEC_GSI_COUNT - 1)

#endif /* FE_VECTORS_H */
