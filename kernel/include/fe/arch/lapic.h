/* SPDX-License-Identifier: 0BSD */
/* 本地 APIC（LAPIC）：每个 CPU 一个，负责本地定时器、EOI、IPI。
 *
 * 访问方式：通过 MSR 0x1B 拿到基址（默认 0xFEE00000），映射到 MMIO 窗口后按寄存器偏移访问。
 * 定时器频率无法从硬件直接读出，必须校准：这里用引导器提供的 TSC 频率计时 10ms，
 * 数出 LAPIC 定时器在这段时间里走了多少格（TSC 不可用时退回用 8254 计时）。
 */
#ifndef FE_LAPIC_H
#define FE_LAPIC_H

#include <fe/types.h>

/* 寄存器偏移 */
#define FE_LAPIC_ID      0x020
#define FE_LAPIC_VERSION 0x030
#define FE_LAPIC_TPR     0x080
#define FE_LAPIC_EOI     0x0B0
#define FE_LAPIC_LDR     0x0D0
#define FE_LAPIC_DFR     0x0E0
#define FE_LAPIC_SVR     0x0F0
#define FE_LAPIC_ESR     0x280
#define FE_LAPIC_ICR_LOW 0x300
#define FE_LAPIC_ICR_HI  0x310
#define FE_LAPIC_LVT_TIMER 0x320
#define FE_LAPIC_LVT_THERMAL 0x330
#define FE_LAPIC_LVT_PERF 0x340
#define FE_LAPIC_LVT_LINT0 0x350
#define FE_LAPIC_LVT_LINT1 0x360
#define FE_LAPIC_LVT_ERROR 0x370
#define FE_LAPIC_TIMER_INIT  0x380
#define FE_LAPIC_TIMER_CUR   0x390
#define FE_LAPIC_TIMER_DIV   0x3E0

#define FE_LAPIC_LVT_MASKED   (1u << 16)
#define FE_LAPIC_LVT_PERIODIC (1u << 17)
#define FE_LAPIC_LVT_ONESHOT  (0u << 17)
#define FE_LAPIC_LVT_TSC_DEADLINE (2u << 17)

#define FE_LAPIC_DIV_1   0xBu
#define FE_LAPIC_DIV_2   0x0u
#define FE_LAPIC_DIV_4   0x1u
#define FE_LAPIC_DIV_8   0x2u
#define FE_LAPIC_DIV_16  0x3u
#define FE_LAPIC_DIV_32  0x8u
#define FE_LAPIC_DIV_64  0x9u
#define FE_LAPIC_DIV_128 0xAu

void fe_lapic_init(u32 spurious_vector);
bool fe_lapic_present(void);
u32  fe_lapic_id(void);
u32  fe_lapic_version(void);
u32  fe_lapic_read(u32 reg);
void fe_lapic_write(u32 reg, u32 value);
void fe_lapic_eoi(void);
void fe_lapic_set_tpr(u8 priority);

/* 启动周期定时器：ticks 为每个周期的计数（由校准得出） */
void fe_lapic_timer_start_periodic(u8 vector, u32 ticks);
void fe_lapic_timer_stop(void);

/* 校准：返回每个毫秒的 LAPIC 计数（0 表示校准失败） */
u32 fe_lapic_calibrate(void);

/* ---- TSC-deadline 模式（首选） ----
 * 为什么首选它：普通周期模式的频率必须靠校准得到，而校准依赖「LAPIC 计数速率恒定」
 * 这一假设——在虚拟机上该假设并不成立（实测同一段代码前后测出的速率能差近 9 倍）。
 * TSC-deadline 模式直接告诉 LAPIC「当 TSC 到达某个值时中断」，而 TSC 频率可以
 * 用 8254 这种独立时基交叉验证，因此节拍精度只取决于 TSC，与 LAPIC 的计数速率无关。
 */
bool fe_lapic_has_tsc_deadline(void);
void fe_lapic_timer_start_deadline(u8 vector, u64 interval_tsc);
void fe_lapic_timer_rearm_deadline(u64 interval_tsc);

/* 发送固定投递 IPI */
void fe_lapic_send_ipi(u32 dest_apic_id, u8 vector);

void fe_lapic_dump(void);

#endif /* FE_LAPIC_H */
