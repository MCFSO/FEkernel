/* SPDX-License-Identifier: 0BSD */
/* 系统时间基准。
 *
 * 优先使用 LAPIC 定时器作为调度节拍源（SMP 下每个 CPU 都有本地定时器，天然可扩展）；
 * LAPIC 不可用时退回 8254。两种来源对上层暴露同一套接口。
 */
#ifndef FE_TIME_H
#define FE_TIME_H

#include <fe/types.h>

/* 启动节拍源。成功返回 true。hz 为每秒节拍数（建议 1000）。 */
bool fe_time_init(u32 hz);

u32 fe_time_hz(void);
u64 fe_time_ticks(void);            /* 自启动以来的节拍数 */
u64 fe_time_ms(void);               /* 自启动以来的毫秒数 */
u64 fe_time_uptime_ms(void);        /* 同上（语义更明确） */

/* ---- 真实时钟（K7）----
 *
 * ★ 节拍与时钟是两件事 ★
 * 节拍是**调度**单位（时间片、睡眠都用它），粒度 1 ms；而"现在几点"
 * 必须建在 TSC 上。用节拍当时间基准的后果不是"精度差一点"，是
 * **测量的全部有效位都来自量化误差**——D4 期间两次独立的吞吐测量
 * 都是正好 5000 us，那次测出来的是时钟本身。
 *
 * 频率来自引导器并与 8254 交叉验证过（误差 1.2%）；引导器没给频率时
 * 退回节拍制，并由 fe_time_clock_is_tsc() 如实报出去。
 * 调用顺序：fe_time_init() 之后、中断打开之后调 fe_time_clock_init()。 */
void fe_time_clock_init(void);
u64  fe_time_ns(void);              /* 自启动以来的纳秒数（单调） */
bool fe_time_clock_is_tsc(void);    /* false = 退回节拍制（分辨率 1 ms） */
u64  fe_time_clock_hz(void);        /* 当前时钟的计数频率 */

/* 定时器源的名称，用于日志 */
const char *fe_time_source(void);

/* 节拍回调（在中断上下文执行，必须极短） */
typedef void (*fe_time_callback_t)(u64 ticks);
void fe_time_set_callback(fe_time_callback_t cb);

/* 毫秒 → 节拍（向上取整，至少 1） */
u64 fe_ms_to_ticks(u64 ms);

/* 测一次 LAPIC 定时器自身的计数速率（10ms 窗口），测完恢复节拍源。
 * 返回 false 表示无法测量（无 LAPIC 或无 TSC）。 */
bool fe_time_measure_lapic_rate(u32 *out_counts_10ms);

/* 按 g_hz 重新武装当前节拍源。只有时间子系统自己该改这个编程；
 * 别处（尤其自检）动过 LAPIC 寄存器之后必须调它恢复，否则系统的
 * 节拍率会被静默改掉。 */
void fe_time_restart_tick(void);

/* 节拍源自检与自愈：必须在**中断已打开**之后调用。
 * 用 TSC 实测节拍率，不达标则自动切换到 8254。返回最终是否达标。 */
bool fe_time_verify(void);

/* 诊断：中断服务耗时统计（判断节拍率偏低的成因） */
void fe_time_get_isr_stats(u64 *count, u64 *total_cycles, u64 *max_cycles);
void fe_time_sample_if(u32 *off_samples, u32 *total_samples);

/* 真实时钟（K7）的自检：短窗口分辨率 / 长窗口与节拍一致 / 单调 / 自述相符。
 * 返回失败项数。必须在 fe_time_clock_init() 之后调用。 */
u32 fe_selftest_clock(void);

#endif /* FE_TIME_H */
