/* SPDX-License-Identifier: 0BSD */
/* 8254 可编程间隔定时器。
 *
 * M2 起系统节拍由 LAPIC 定时器提供（SMP 可扩展），8254 只在 APIC 不可用时作为
 * 降级节拍源。因此本文件只负责「把硬件编程成指定频率」，节拍计数与调度归 time.c。
 */
#ifndef FE_PIT_H
#define FE_PIT_H

#include <fe/types.h>

#define FE_PIT_IRQ      0
#define FE_PIT_BASE_HZ  1193182u

/* 把通道 0 编程为指定频率的方波，并打开通道 2 作为忙等延时源（LAPIC 校准用）。
 * 本函数不注册中断处理程序、不打开中断屏蔽位。 */
void fe_pit_program(u32 hz);

/* 忙等延时（不依赖中断，可在中断关闭时使用） */
void fe_pit_busy_wait_ms(u32 ms);

#endif /* FE_PIT_H */
