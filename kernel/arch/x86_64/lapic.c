/* SPDX-License-Identifier: 0BSD */
#include <fe/arch/lapic.h>
#include <fe/boot/bootinfo.h>
#include <fe/mm/vmm.h>
#include <fe/kprintf.h>
#include <fe/panic.h>
#include <fe/io.h>
#include <fe/pit.h>
#include <fe/vectors.h>

static volatile u32 *g_lapic;
static u32 g_lapic_phys;
static bool g_present;

FE_INLINE u32 lapic_read_reg(u32 reg)
{
    return g_lapic[reg / 4];
}

FE_INLINE void lapic_write_reg(u32 reg, u32 value)
{
    g_lapic[reg / 4] = value;
    (void)g_lapic[reg / 4];     /* 回读一次，确保写下去（MMIO 写需要立即生效） */
}

u32 fe_lapic_read(u32 reg) { return lapic_read_reg(reg); }
void fe_lapic_write(u32 reg, u32 value) { lapic_write_reg(reg, value); }

bool fe_lapic_present(void) { return g_present; }
u32  fe_lapic_id(void) { return g_present ? (lapic_read_reg(FE_LAPIC_ID) >> 24) : 0; }
u32  fe_lapic_version(void) { return g_present ? lapic_read_reg(FE_LAPIC_VERSION) : 0; }

void fe_lapic_eoi(void)
{
    if (g_present) {
        lapic_write_reg(FE_LAPIC_EOI, 0);
    }
}

void fe_lapic_set_tpr(u8 priority)
{
    if (g_present) {
        lapic_write_reg(FE_LAPIC_TPR, priority);
    }
}

void fe_lapic_init(u32 spurious_vector)
{
    /* 1. 从 MSR 读出基址并确保全局使能位置位 */
    u64 base_msr = fe_rdmsr(FE_MSR_APIC_BASE);
    g_lapic_phys = (u32)(base_msr & 0xFFFFF000ull);
    if (g_lapic_phys == 0) {
        g_lapic_phys = 0xFEE00000u;
    }
    if (!(base_msr & (1ull << 11))) {
        fe_wrmsr(FE_MSR_APIC_BASE, base_msr | (1ull << 11));
    }

    /* 2. 映射到 MMIO 窗口（必须关闭缓存） */
    g_lapic = (volatile u32 *)fe_vmm_map_mmio(g_lapic_phys, FE_FRAME_SIZE);
    if (!g_lapic) {
        fe_kprintf("[LAPIC] MMIO 映射失败\n");
        return;
    }

    /* 3. 软件使能 + 伪中断向量；TPR 清零以接收所有优先级的中断 */
    lapic_write_reg(FE_LAPIC_TPR, 0);
    lapic_write_reg(FE_LAPIC_SVR, (spurious_vector & 0xFFu) | (1u << 8));

    /* 4. 关掉所有 LVT 条目，避免启动期冒出意外中断 */
    lapic_write_reg(FE_LAPIC_LVT_TIMER, FE_LAPIC_LVT_MASKED);
    lapic_write_reg(FE_LAPIC_LVT_THERMAL, FE_LAPIC_LVT_MASKED);
    lapic_write_reg(FE_LAPIC_LVT_PERF, FE_LAPIC_LVT_MASKED);
    lapic_write_reg(FE_LAPIC_LVT_LINT0, FE_LAPIC_LVT_MASKED);
    lapic_write_reg(FE_LAPIC_LVT_LINT1, FE_LAPIC_LVT_MASKED);
    lapic_write_reg(FE_LAPIC_LVT_ERROR, FE_VEC_LAPIC_ERROR);
    /* 清掉可能残留的错误状态 */
    lapic_write_reg(FE_LAPIC_ESR, 0);
    lapic_write_reg(FE_LAPIC_ESR, 0);

    g_present = true;
}

/* 用 TSC 计时 10ms 数 LAPIC 计数；TSC 频率未知时用 8254 计时。
 * 返回每毫秒的 LAPIC 计数（已含分频）。 */
u32 fe_lapic_calibrate(void)
{
    if (!g_present) {
        return 0;
    }
    const u32 div = FE_LAPIC_DIV_16;
    lapic_write_reg(FE_LAPIC_TIMER_DIV, div);
    lapic_write_reg(FE_LAPIC_LVT_TIMER, FE_LAPIC_LVT_MASKED | FE_LAPIC_LVT_ONESHOT);
    lapic_write_reg(FE_LAPIC_TIMER_INIT, 0xFFFFFFFFu);

    const struct fe_boot_info *bi = fe_boot_info();
    if (bi->tsc_frequency != 0) {
        u64 target = bi->tsc_frequency / 100;    /* 10 ms */
        u64 start = fe_rdtsc();
        while (fe_rdtsc() - start < target) {
            fe_pause();
        }
    } else {
        /* 退化路径：用 8254 的通道 2 做一次性延时（不产生中断） */
        fe_outb(0x61, (u8)((fe_inb(0x61) & ~0x02) | 0x01));
        fe_outb(0x43, 0xB0);                     /* 通道2, 模式0, 先低后高 */
        u16 divisor = (u16)(FE_PIT_BASE_HZ / 100);   /* 10 ms */
        fe_outb(0x42, (u8)(divisor & 0xFF));
        fe_outb(0x42, (u8)(divisor >> 8));
        /* 等计数到 0（读回值从 0xFFFF 递减到 0） */
        u16 last = 0xFFFF;
        for (;;) {
            fe_outb(0x43, 0x80);                 /* 锁存通道 2 */
            u8 lo = fe_inb(0x42);
            u8 hi = fe_inb(0x42);
            u16 now = (u16)(lo | (hi << 8));
            if (now > last) {
                break;                            /* 回绕即到期 */
            }
            last = now;
            fe_pause();
        }
    }

    u32 remaining = lapic_read_reg(FE_LAPIC_TIMER_CUR);
    lapic_write_reg(FE_LAPIC_TIMER_INIT, 0);
    lapic_write_reg(FE_LAPIC_LVT_TIMER, FE_LAPIC_LVT_MASKED);

    u32 elapsed = 0xFFFFFFFFu - remaining;
    return elapsed / 10;
}

void fe_lapic_timer_start_periodic(u8 vector, u32 ticks)
{
    if (!g_present || ticks == 0) {
        return;
    }
    lapic_write_reg(FE_LAPIC_TIMER_DIV, FE_LAPIC_DIV_16);
    lapic_write_reg(FE_LAPIC_LVT_TIMER, (u32)vector | FE_LAPIC_LVT_PERIODIC);
    lapic_write_reg(FE_LAPIC_TIMER_INIT, ticks);
}

void fe_lapic_timer_stop(void)
{
    if (g_present) {
        lapic_write_reg(FE_LAPIC_TIMER_INIT, 0);
        lapic_write_reg(FE_LAPIC_LVT_TIMER, FE_LAPIC_LVT_MASKED);
    }
}

void fe_lapic_send_ipi(u32 dest_apic_id, u8 vector)
{
    if (!g_present) {
        return;
    }
    lapic_write_reg(FE_LAPIC_ICR_HI, dest_apic_id << 24);
    lapic_write_reg(FE_LAPIC_ICR_LOW, vector);      /* 固定投递、物理目标、边沿 */
    for (u32 spin = 0; spin < 100000; spin++) {
        if (!(lapic_read_reg(FE_LAPIC_ICR_LOW) & (1u << 12))) {
            break;                                   /* bit12 = 投递完成 */
        }
    }
}

/* ------------------------------------------------------------------ */
/* TSC-deadline 模式                                                   */
/* ------------------------------------------------------------------ */

#define FE_MSR_TSC_DEADLINE 0x6E0u

static u64 g_next_deadline;

static bool cpu_has_tsc_deadline(void)
{
    u32 a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0u));
    (void)a;
    (void)b;
    (void)d;
    return (c & (1u << 24)) != 0;    /* CPUID.01H:ECX[24] = TSC_DEADLINE */
}

bool fe_lapic_has_tsc_deadline(void)
{
    return g_present && cpu_has_tsc_deadline();
}

void fe_lapic_timer_start_deadline(u8 vector, u64 interval_tsc)
{
    if (!fe_lapic_has_tsc_deadline() || interval_tsc == 0) {
        return;
    }
    /* 先解除武装，设置 LVT 为 TSC-deadline 模式，再写第一个截止时刻 */
    fe_wrmsr(FE_MSR_TSC_DEADLINE, 0);
    lapic_write_reg(FE_LAPIC_LVT_TIMER, (u32)vector | FE_LAPIC_LVT_TSC_DEADLINE);
    g_next_deadline = fe_rdtsc() + interval_tsc;
    fe_wrmsr(FE_MSR_TSC_DEADLINE, g_next_deadline);
}

void fe_lapic_timer_rearm_deadline(u64 interval_tsc)
{
    if (!g_present || interval_tsc == 0) {
        return;
    }
    /* 无漂移推进：以「上一次的截止时刻 + 周期」为基准，而不是以当前时刻为基准，
     * 否则每次中断的处理延迟都会累积成时钟漂移。 */
    u64 now = fe_rdtsc();
    g_next_deadline += interval_tsc;
    if (g_next_deadline <= now) {
        g_next_deadline = now + interval_tsc;    /* 落后太多则重新对齐 */
    }
    fe_wrmsr(FE_MSR_TSC_DEADLINE, g_next_deadline);
}

void fe_lapic_dump(void)
{
    if (!g_present) {
        fe_kprintf("[LAPIC] 不可用\n");
        return;
    }
    u32 ver = fe_lapic_version();
    fe_kprintf("[LAPIC] id=%u @ %#x, 版本 %#x (LVT 条目 %u 个), 最大优先级 %u\n",
               fe_lapic_id(), g_lapic_phys, ver, ((ver >> 16) & 0xFF) + 1,
               ver & 0xFF);
}
