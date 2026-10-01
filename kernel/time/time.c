/* SPDX-License-Identifier: 0BSD */
/* 系统时间基准。
 *
 * 节拍源优先级：
 *   1. LAPIC 定时器的 TSC-deadline 模式 —— 精度只取决于 TSC，与 LAPIC 计数速率无关；
 *   2. LAPIC 定时器周期模式（需校准）—— 在虚拟机上不可靠，仅作退路；
 *   3. 8254 + 8259（必须把 LINT0 配成 ExtINT，否则 8259 的中断根本到不了 CPU）。
 *
 * 为什么首选 TSC-deadline：实测在同一台虚拟机上，同一段校准代码前后测出的
 * LAPIC 计数速率能差近 9 倍（62945 vs 547940 计数/ms），而 TSC 频率用 8254
 * 这个独立时基交叉验证是准的（3034 MHz vs 引导器声明 2999 MHz，误差 1.2%）。
 * 因此凡是「时间」都应当以 TSC 为准，LAPIC 只当触发源。
 */
#include <fe/time.h>
#include <fe/arch/lapic.h>
#include <fe/pit.h>
#include <fe/pic.h>
#include <fe/idt.h>
#include <fe/vectors.h>
#include <fe/sched/sched.h>
#include <fe/boot/bootinfo.h>
#include <fe/kprintf.h>
#include <fe/panic.h>
#include <fe/io.h>
#include <fe/acpi.h>
#include <fe/arch/ioapic.h>

static volatile u64 g_ticks;
static u32 g_hz = 1000;
static fe_time_callback_t g_callback;
static const char *g_source = "无";

enum tick_mode { TICK_NONE, TICK_LAPIC_ONESHOT, TICK_LAPIC_PERIODIC, TICK_PIT };
static enum tick_mode g_mode = TICK_NONE;
static u64 g_deadline_interval;

/* 节拍 ↔ 毫秒换算的快速路径参数（见 fe_time_ms 的说明） */
static u32  g_ticks_per_ms = 1;
static u32  g_lapic_per_tick = 0;   /* LAPIC 周期模式的每节拍计数，restart 时复用 */
static bool g_ms_is_identity = true;

/* 中断服务耗时统计（诊断节拍率偏低的成因） */
static volatile u64 g_isr_total_cycles;
static volatile u64 g_isr_max_cycles;
static volatile u64 g_isr_count;

void fe_time_get_isr_stats(u64 *count, u64 *total_cycles, u64 *max_cycles)
{
    *count = g_isr_count;
    *total_cycles = g_isr_total_cycles;
    *max_cycles = g_isr_max_cycles;
}

/* 采样中断允许位：判断 CPU 是否长期处于关中断状态 */
void fe_time_sample_if(u32 *off_samples, u32 *total_samples)
{
    u32 off = 0;
    for (u32 i = 0; i < 200000; i++) {
        if (!(fe_read_rflags() & 0x200ull)) {
            off++;
        }
    }
    *off_samples = off;
    *total_samples = 200000;
}

static void enable_legacy_pic_path(void);

/* 统一节拍处理：无论中断来自哪个源都走这里 */
static void tick_isr(struct fe_regs *r)
{
    (void)r;
    u64 t0 = fe_rdtsc();
    g_ticks++;

    /* 有 LAPIC 时一律由 LAPIC 结束中断：
     * 无论中断源是 LAPIC 定时器还是经 IOAPIC 转发的外部中断，都是如此。 */
    if (fe_lapic_present()) {
        fe_lapic_eoi();
    } else {
        fe_pic_eoi(FE_PIT_IRQ);
    }

    if (g_mode == TICK_LAPIC_ONESHOT) {
        /* 立刻排下一次：以 TSC 为准，不受 LAPIC 计数速率影响 */
        fe_lapic_timer_rearm_deadline(g_deadline_interval);
    }

    fe_sched_tick();

    if (g_callback) {
        g_callback(g_ticks);
    }

    u64 dt = fe_rdtsc() - t0;
    g_isr_total_cycles += dt;
    if (dt > g_isr_max_cycles) {
        g_isr_max_cycles = dt;
    }
    g_isr_count++;
}

/* 把 PIT 的中断接到 CPU 上。
 *
 * 关键点（踩过的坑）：**系统里有 IOAPIC 时，ISA IRQ0 走的是 IOAPIC 的 GSI 2，
 * 而不是 8259**（MADT 的 Interrupt Source Override 就是这么描述的）。
 * 因此必须按 ACPI 给出的 GSI/极性/触发方式去路由，直接打开 8259 是没有用的。 */
static void route_pit_irq(void)
{
    if (fe_ioapic_present()) {
        u32 gsi = FE_PIT_IRQ;
        bool level = true;
        bool active_low = true;
        fe_acpi_irq_to_gsi(FE_PIT_IRQ, &gsi, &level, &active_low);
        fe_ioapic_route(gsi, FE_VEC_IRQ(FE_PIT_IRQ), fe_lapic_id(),
                        level, active_low, false);
        fe_pic_mask(FE_PIT_IRQ);
        fe_kprintf("[时间] PIT 经 IOAPIC 路由: IRQ%u -> GSI %u -> 向量 %u (%s, %s)\n",
                   FE_PIT_IRQ, gsi, FE_VEC_IRQ(FE_PIT_IRQ),
                   level ? "电平" : "边沿", active_low ? "低有效" : "高有效");
        return;
    }
    /* 没有 IOAPIC 时才退回 8259；此时必须把 LINT0 配成 ExtINT，否则中断到不了 CPU */
    enable_legacy_pic_path();
    fe_pic_unmask(FE_PIT_IRQ);
    fe_kprintf("[时间] PIT 经 8259 路由（LINT0 = ExtINT）\n");
}

/* 8259 的输出接到 LAPIC 的 LINT0 上。要用 8259 就必须把 LINT0 配成 ExtINT，
 * 否则它会被屏蔽，8254 的中断永远到不了 CPU（这是踩过的坑）。 */
static void enable_legacy_pic_path(void)
{
    fe_lapic_write(FE_LAPIC_LVT_LINT0, 0x700u);   /* ExtINT(111) + 不屏蔽 */
    fe_lapic_set_tpr(0);
}

bool fe_time_init(u32 hz)
{
    if (hz == 0) {
        hz = 1000;
    }
    g_hz = hz;
    g_ticks = 0;
    g_ticks_per_ms = (hz >= 1000u) ? (hz / 1000u) : 1u;
    g_ms_is_identity = (hz == 1000u);

    const struct fe_boot_info *bi = fe_boot_info();

    /* 首选：TSC-deadline（精确，无需校准） */
    if (fe_lapic_has_tsc_deadline() && bi->tsc_frequency != 0) {
        g_deadline_interval = bi->tsc_frequency / hz;
        if (g_deadline_interval > 0) {
            fe_idt_set_handler(FE_VEC_LAPIC_TIMER, tick_isr);
            g_mode = TICK_LAPIC_ONESHOT;
            fe_lapic_timer_start_deadline(FE_VEC_LAPIC_TIMER, g_deadline_interval);
            g_source = "LAPIC TSC-deadline";
            fe_pic_mask(FE_PIT_IRQ);
            fe_kprintf("[时间] LAPIC TSC-deadline %u Hz（TSC %llu MHz, 每节拍 %llu 计数）\n",
                       hz, (unsigned long long)(bi->tsc_frequency / 1000000),
                       (unsigned long long)g_deadline_interval);
            return true;
        }
    }
    fe_kprintf("[时间] 本机不支持 TSC-deadline（CPUID.01H:ECX[24]=0）\n");

    /* 退路一：LAPIC 周期模式（需要校准，虚拟机下可能不准） */
    if (fe_lapic_present()) {
        u32 per_ms = fe_lapic_calibrate();
        if (per_ms >= 100) {
            u32 per_tick = per_ms * 1000u / hz;
            if (per_tick == 0) {
                per_tick = 1;
            }
            g_lapic_per_tick = per_tick;        /* 记下来，restart 要用 */
            fe_idt_set_handler(FE_VEC_LAPIC_TIMER, tick_isr);
            g_mode = TICK_LAPIC_PERIODIC;
            fe_lapic_timer_start_periodic(FE_VEC_LAPIC_TIMER, per_tick);
            g_source = "LAPIC 周期模式";
            fe_pic_mask(FE_PIT_IRQ);
            fe_kprintf("[时间] LAPIC 周期定时器 %u Hz（校准 %u 计数/ms，每节拍 %u）"
                       "——注意：该模式依赖校准，虚拟机下可能不准\n",
                       hz, per_ms, per_tick);
            return true;
        }
    }

    /* 退路二：8254；中断路径由 route_pit_irq 决定（IOAPIC 优先，否则 8259） */
    fe_pit_program(hz);
    fe_idt_set_handler(FE_VEC_IRQ(FE_PIT_IRQ), tick_isr);
    route_pit_irq();
    g_mode = TICK_PIT;
    g_source = "8254";
    /* 切到 8254 就必须确认 LAPIC 定时器**已经停了**。
     * 两个节拍源同时跑，g_ticks 会以两者之和前进——那不是「快了一点」，
     * 而是所有以节拍为单位的量（时间片、睡眠、超时）全部失真，
     * 并且症状随两个源的相对漂移而变，极难定位。 */
    fe_lapic_timer_stop();
    fe_kprintf("[时间] 8254 定时器 %u Hz（降级路径）\n", hz);
    return false;
}

/* 把当前节拍源按 g_hz 重新武装一遍。
 *
 * 为什么需要这个入口：内核里唯一有权改节拍源编程的应该是**时间子系统自己**。
 * 别处（尤其是自检）为了诊断去动 LAPIC 寄存器，如果忘了恢复，
 * 整个系统的节拍率就被改了——而这个改动不会有任何报错，
 * 只会让后面所有基于节拍的量静默地偏掉。
 * 自检的职责是「测量并报告」，不是「顺便把系统重编一遍」。 */
void fe_time_restart_tick(void)
{
    switch (g_mode) {
    case TICK_LAPIC_ONESHOT:
        fe_lapic_timer_rearm_deadline(g_deadline_interval);
        break;
    case TICK_LAPIC_PERIODIC:
        fe_lapic_timer_start_periodic(FE_VEC_LAPIC_TIMER,
                                      g_lapic_per_tick ? g_lapic_per_tick : 1);
        break;
    case TICK_PIT:
    default:
        fe_pit_program(g_hz);
        fe_lapic_timer_stop();      /* 确保 LAPIC 那条路是关着的 */
        break;
    }
}

u32 fe_time_hz(void) { return g_hz; }
u64 fe_time_ticks(void) { return g_ticks; }

/* ------------------------------------------------------------------ */
/* 真实时钟（K7）：以 TSC 为准，节拍只当调度单位                        */
/* ------------------------------------------------------------------ */

/* ★ 为什么时钟不能建在节拍上 ★
 *
 * 节拍是**调度**单位（1 ms），不是一个时钟。用它当时间基准，任何测量都被
 * 量化到 1 ms 的整数倍——D4 期间实测到两次独立的吞吐测量都是**正好**
 * 5000 us，那次"测出来的其实是时钟"。这不是精度差一点的问题：
 * 当一个数字的全部有效位都来自量化误差时，它是不可用的。
 *
 * 文件头已经写了这条判断（"凡是「时间」都应当以 TSC 为准，LAPIC 只当触发源"），
 * 这一节把它落到用户态看得见的那个时钟上。
 *
 * ★ 为什么需要 TSC 频率 ★ 来自引导器（Limine），并且已经与 8254 这个
 * 独立时基交叉验证过（实测 3034 MHz vs 声明 2999 MHz，误差 1.2%）。
 * 频率为 0 说明引导器没给，这时**退回节拍**并把来源如实报出去——
 * 一个不知道自己精度的时钟比一个慢时钟更危险。 */
static u64  g_tsc_at_boot;
static u64  g_tsc_hz;
static bool g_tsc_clock;

void fe_time_clock_init(void)
{
    const struct fe_boot_info *bi = fe_boot_info();
    if (bi->tsc_frequency == 0) {
        g_tsc_clock = false;
        fe_kprintf("[时间] 时钟：**退回节拍制**（引导器没给 TSC 频率，"
                   "分辨率 1 ms）\n");
        return;
    }
    g_tsc_hz = bi->tsc_frequency;
    g_tsc_at_boot = fe_rdtsc();
    g_tsc_clock = true;
    fe_kprintf("[时间] 时钟：TSC 制，%llu MHz（分辨率约 0.3 ns；"
               "节拍只作调度单位）\n",
               (unsigned long long)(g_tsc_hz / 1000000));
}

bool fe_time_clock_is_tsc(void) { return g_tsc_clock; }
u64  fe_time_clock_hz(void) { return g_tsc_clock ? g_tsc_hz : g_hz; }

/* 从启动到现在的纳秒数。
 *
 * ★ 为什么不写成一个 128 位乘除 ★
 * `(unsigned __int128)delta * 1e9 / hz` 是最直观的写法，但它会引出
 * `__udivti3` —— 编译器运行时库里的 128 位除法辅助函数，而内核是
 * **freestanding** 链接的（`-nostdlib`），那个符号不存在，链接直接失败
 * （实测：`ld.lld: error: undefined symbol: __udivti3`）。
 * 内核里没有 libgcc，所以 128 位除法这条路是堵死的。
 *
 * ★ 拆成两段 64 位就没这个问题，而且**精度不损** ★
 *   ns = (delta / hz) * 1e9 + ((delta % hz) * 1e9) / hz
 * 第二项的余数 < hz ≤ ~3.1e9，乘 1e9 最大 3.1e18 < 2^64（1.8e19），
 * 所以不会溢出；两次整数除法的误差合计 < 1 ns。
 * 前提是 TSC 频率别超过 18.4 GHz（3.1e18 的反推），这在今天的机器上
 * 不是限制——真到了那一天，这一行会因为溢出而**算错**而不是崩溃，
 * 所以把上限写在这里。 */
u64 fe_time_ns(void)
{
    if (!g_tsc_clock) {
        return fe_time_ms() * 1000000ull;
    }
    u64 delta = fe_rdtsc() - g_tsc_at_boot;
    u64 whole = (delta / g_tsc_hz) * 1000000000ull;
    u64 frac = ((delta % g_tsc_hz) * 1000000000ull) / g_tsc_hz;
    return whole + frac;
}

/* 测一次 LAPIC 定时器**自身**的计数速率（10 ms 窗口），测完把节拍源恢复原状。
 *
 * 为什么这个函数住在这里，而不是让自检自己去动 LAPIC 寄存器：
 * 测量必然要临时改定时器编程（一次性模式 + 遮断中断），而「改完必须恢复」
 * 是这段代码的责任。放在时间子系统里，恢复逻辑就和它破坏的东西写在一起，
 * 不会出现「谁忘了恢复」——而忘了恢复的后果是**整个系统的节拍率被静默改掉**，
 * 所有以节拍为单位的量（时间片、睡眠、超时）一起偏，且没有任何报错。
 * 这不是假想的风险：自检曾经把 1000 Hz 改成 100 Hz，还让 LAPIC 与 8254
 * 两个节拍源同时跑。 */
bool fe_time_measure_lapic_rate(u32 *out_counts_10ms)
{
    if (!fe_lapic_present() || !out_counts_10ms) {
        return false;
    }
    const struct fe_boot_info *bi = fe_boot_info();
    if (bi->tsc_frequency == 0) {
        return false;       /* 没有独立时基，这个测量没有意义 */
    }

    fe_lapic_timer_stop();
    fe_lapic_write(FE_LAPIC_TIMER_DIV, FE_LAPIC_DIV_16);
    fe_lapic_write(FE_LAPIC_LVT_TIMER, FE_LAPIC_LVT_MASKED | FE_LAPIC_LVT_ONESHOT);
    fe_lapic_write(FE_LAPIC_TIMER_INIT, 0xFFFFFFFFu);

    u64 c0 = fe_rdtsc();
    u64 target = bi->tsc_frequency / 100;       /* 10 ms —— 注意是 /100 不是 /10 */
    while (fe_rdtsc() - c0 < target) {
        fe_pause();
    }

    u32 cur = fe_lapic_read(FE_LAPIC_TIMER_CUR);
    u32 elapsed = 0xFFFFFFFFu - cur;
    fe_lapic_write(FE_LAPIC_TIMER_INIT, 0);

    /* 不管测出来是什么，都必须把节拍源交还回去 */
    fe_time_restart_tick();

    *out_counts_10ms = elapsed;
    return true;
}

/* ------------------------------------------------------------------ */
/* 节拍源自检与自愈                                                    */
/* ------------------------------------------------------------------ */

/* LAPIC 定时器的编程值在虚拟机上可能完全不被遵守（实测 VirtualBox 上无论怎么设
 * 重装值，实际中断率都只有 ~60Hz）。这里在中断打开之后，用 TSC 实测节拍率，
 * 不达标就自动切换到 8254。必须先开中断再调用本函数。 */
bool fe_time_verify(void)
{
    const struct fe_boot_info *bi = fe_boot_info();
    if (bi->tsc_frequency == 0 || g_mode != TICK_LAPIC_PERIODIC) {
        return true;        /* 没有独立时基可校验，或本来就是非 LAPIC 源 */
    }

    u64 t0 = g_ticks;
    u64 c0 = fe_rdtsc();
    u64 target = bi->tsc_frequency / 50;        /* 20 ms */
    while (fe_rdtsc() - c0 < target) {
        fe_pause();
    }
    u64 c1 = fe_rdtsc();
    u64 dt = g_ticks - t0;
    u32 measured = (u32)(dt * bi->tsc_frequency / (c1 - c0));

    fe_kprintf("[时间] 节拍校验: 20ms 内 %llu 个节拍 → 实测 %u Hz（目标 %u Hz）\n",
               (unsigned long long)dt, measured, g_hz);

    if (measured >= g_hz * 8 / 10 && measured <= g_hz * 12 / 10) {
        return true;
    }

    fe_kprintf("[时间] LAPIC 定时器实际中断率与编程值严重不符，切换到 8254\n");
    fe_lapic_timer_stop();
    fe_pit_program(g_hz);
    fe_idt_set_handler(FE_VEC_IRQ(FE_PIT_IRQ), tick_isr);
    route_pit_irq();
    g_mode = TICK_PIT;
    g_source = "8254";

    /* 切换后再校验一次，确认 8254 这条路是通的 */
    t0 = g_ticks;
    c0 = fe_rdtsc();
    while (fe_rdtsc() - c0 < target) {
        fe_pause();
    }
    c1 = fe_rdtsc();
    dt = g_ticks - t0;
    measured = (u32)(dt * bi->tsc_frequency / (c1 - c0));
    fe_kprintf("[时间] 切换到 8254 后复核: 20ms 内 %llu 个节拍 → 实测 %u Hz\n",
               (unsigned long long)dt, measured);
    return measured >= g_hz * 8 / 10 && measured <= g_hz * 12 / 10;
}
/* 节拍 → 毫秒。避免每次调用都做一次 64 位除法（约 20~40 周期，
 * 在 130 周期的系统调用里占比可观）：默认 hz=1000 时 ticks 本身就是毫秒。 */
u64 fe_time_ms(void)
{
    if (g_ms_is_identity) {
        return g_ticks;
    }
    return g_ticks / g_ticks_per_ms;
}
u64 fe_time_uptime_ms(void) { return fe_time_ms(); }
const char *fe_time_source(void) { return g_source; }

void fe_time_set_callback(fe_time_callback_t cb)
{
    g_callback = cb;
}

u64 fe_ms_to_ticks(u64 ms)
{
    u64 t = (ms * g_hz + 999ull) / 1000ull;
    return t ? t : 1;
}
