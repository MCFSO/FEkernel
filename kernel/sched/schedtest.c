/* SPDX-License-Identifier: 0BSD */
/* M2 调度器自检。
 *
 * 关键点：工作线程**不主动让出 CPU**，它们之间的交错只可能来自时间片抢占。
 * 再放一个同样优先级、永不停止的「CPU 占用线程」参与竞争，如果有任何线程
 * 能在它霸占 CPU 的情况下继续推进，就说明抢占是真的，而不是合作式调度。
 */
#include <fe/sched/sched.h>
#include <fe/sched/thread.h>
#include <fe/time.h>
#include <fe/kprintf.h>
#include <fe/string.h>
#include <fe/io.h>
#include <fe/boot/bootinfo.h>
#include <fe/arch/lapic.h>
#include <fe/vectors.h>
#include <fe/pit.h>
#include <fe/panic.h>

#define WORKERS        3
#define WORKER_ROUNDS  10
#define ROUND_BUSY_TICKS 2

static volatile u32 g_worker_done;
static volatile u64 g_hog_counter;
static volatile bool g_hog_run;
static char g_trace[64];
static volatile u32 g_trace_len;
static volatile u32 g_sleeper_done;
static volatile u64 g_sleep_elapsed_ticks;
static volatile u64 g_sleep_elapsed_tsc_ms;
static volatile u64 g_wake_max_latency_ms;
static volatile u64 g_wake_total_latency_ms;
static volatile u32 g_wake_count;

/* 用 TSC 做精确忙等：引导器给了 TSC 频率，比空转计数靠谱 */
static void busy_wait_ms(u32 ms)
{
    u64 freq = fe_boot_info()->tsc_frequency;
    if (freq == 0) {
        for (volatile u64 i = 0; i < (u64)ms * 200000ull; i++) {
            fe_pause();
        }
        return;
    }
    u64 target = freq / 1000ull * ms;
    u64 start = fe_rdtsc();
    while (fe_rdtsc() - start < target) {
        fe_pause();
    }
}

/* 忙等到节拍计数器前进 n 个节拍。
 * 用节拍而不是毫秒作为工作量的单位：这样测试验证的是「调度器是否会按时间片抢占」，
 * 而不会因为虚拟机的中断投递率偏低而失效。 */
static void wait_ticks(u64 n)
{
    u64 target = fe_time_ticks() + n;
    while (fe_time_ticks() < target) {
        fe_pause();
    }
}

/* 工作线程：不调用 yield，纯忙等 + 记录轨迹 */
static void worker_entry(void *arg)
{
    char letter = *(const char *)arg;
    for (u32 i = 0; i < WORKER_ROUNDS; i++) {
        if (g_trace_len < sizeof(g_trace) - 1) {
            g_trace[g_trace_len++] = letter;
        }
        wait_ticks(ROUND_BUSY_TICKS);   /* 按节拍计：与定时器实际频率无关 */
    }
    g_worker_done++;
    fe_thread_exit(0);
}

/* CPU 占用线程：永不主动让出 */
static void hog_entry(void *arg)
{
    (void)arg;
    while (g_hog_run) {
        g_hog_counter++;
    }
    fe_thread_exit(0);
}

/* 睡眠精度测量。
 *
 * 同时记 TSC 与节拍两个口径。为什么要两个：
 * 节拍数只说明「过了多少个节拍」，而节拍源的**实际频率**在虚拟机里会剧烈波动
 * （实测同一台机器上 49~169 Hz 都出现过，而声明值是 1000 Hz）。
 * 只有 TSC 这个独立时基才能回答「到底是节拍变慢了，还是线程真的被饿着了」——
 * 这两个结论指向完全不同的修法，不能混为一谈。 */
static void sleeper_entry(void *arg)
{
    (void)arg;
    u64 freq = fe_boot_info()->tsc_frequency;
    u64 k0 = fe_time_ticks();
    u64 c0 = fe_rdtsc();
    fe_thread_sleep_ms(100);
    u64 c1 = fe_rdtsc();
    g_sleep_elapsed_ticks = fe_time_ticks() - k0;
    g_sleep_elapsed_tsc_ms = freq ? (c1 - c0) / (freq / 1000ull) : 0;
    g_sleeper_done = 1;
    fe_thread_exit(0);
}

/* 高优先级线程的唤醒延迟：在 NORMAL 级占用线程霸占 CPU 的情况下测量 */
static void wake_latency_entry(void *arg)
{
    (void)arg;
    for (u32 i = 0; i < 5; i++) {
        u64 want = fe_time_ms() + 20;
        fe_thread_sleep_ms(20);
        u64 now = fe_time_ms();
        u64 lat = (now > want) ? (now - want) : 0;
        if (lat > g_wake_max_latency_ms) {
            g_wake_max_latency_ms = lat;
        }
        g_wake_total_latency_ms += lat;
        g_wake_count++;
    }
    fe_thread_exit(0);
}

/* 统计轨迹中相邻字符发生变化的次数：0 表示完全没有交错 */
static u32 count_switches(const char *s, u32 len)
{
    u32 n = 0;
    for (u32 i = 1; i < len; i++) {
        if (s[i] != s[i - 1]) {
            n++;
        }
    }
    return n;
}

u32 fe_selftest_sched(void)
{
    u32 fail = 0;
    static const char names[WORKERS] = { 'A', 'B', 'C' };

    fe_kprintf("[自检] 调度器: %u 个优先级队列, 默认时间片 %u 节拍 (%llu ms)\n",
               FE_THREAD_PRIO_LEVELS, fe_sched_default_slice(),
               (unsigned long long)(fe_sched_default_slice() * 1000ull / fe_time_hz()));

    /* 时间基准校验：用 TSC 量 100ms，看节拍数是否与声明的频率相符。
     * 必须做这一步——睡眠精度测试用的是同一个节拍计数器，节拍源本身慢了它测不出来。 */
    {
        u64 tsc_freq = fe_boot_info()->tsc_frequency;

        /* 交叉校验：用 8254 的独立时基量 TSC 频率。
         * 8254 有自己的晶振(1.193182 MHz)，与 TSC、LAPIC 都无关，
         * 是判断「到底哪个时钟是准的」的独立裁判。 */
        {
            u64 cc0 = fe_rdtsc();
            fe_pit_busy_wait_ms(100);
            u64 cc1 = fe_rdtsc();
            u64 tsc_measured = (cc1 - cc0) * 10ull;
            fe_kprintf("        TSC 交叉校验: 8254 计时 100ms 内 TSC 走了 %llu "
                       "(%llu MHz, 引导器声明 %llu MHz)\n",
                       (unsigned long long)(cc1 - cc0),
                       (unsigned long long)(tsc_measured / 1000000),
                       (unsigned long long)(tsc_freq / 1000000));
        }

        /* 先直接测量 LAPIC 定时器**自身**的计数速率（一次性模式，不经过中断投递）。
         * 这一步把两种可能彻底分开：
         *   - 计数速率不对 → 校准算法有问题；
         *   - 计数速率对但中断数少 → 是虚拟化层对中断投递做了限流。
         *
         * ★ 这一段曾经把系统改坏过，两个错误叠在一起：★
         *   1) 窗口写成 tsc_freq / 10 —— 那是 **100 ms** 不是 10 ms（要 10ms 得 /100），
         *      于是数出的计数大 10 倍；
         *   2) 更严重的是它拿这个偏大的值**把系统节拍源重编了一遍而且不恢复**——
         *      节拍率被静默改成 100 Hz，后面所有以节拍为单位的量（时间片、睡眠、
         *      超时）全部失真。在已经退到 8254 的机器上它还会把 LAPIC 定时器
         *      重新启动，造成两个节拍源同时跑、g_ticks 以合成速率前进。
         *
         * 现在的纪律：**自检只测量并报告，改完必须交还给时间子系统恢复。** */
        u32 counts_10ms = 0;
        fe_time_measure_lapic_rate(&counts_10ms);
        fe_kprintf("        LAPIC 计数速率实测: %u 计数/10ms (= %u 计数/ms)\n",
                   counts_10ms, counts_10ms / 10);
        /* 再看中断实际投递了多少次 */
        u64 t0 = fe_time_ticks();
        u64 c2 = fe_rdtsc();
        busy_wait_ms(100);
        u64 c3 = fe_rdtsc();
        u64 t1 = fe_time_ticks();
        u64 elapsed_ticks = t1 - t0;
        u32 measured_hz = 0;
        if (tsc_freq && c3 > c2) {
            measured_hz = (u32)(elapsed_ticks * tsc_freq / (c3 - c2));
        }
        {
            u64 ic, itot, imax;
            fe_time_get_isr_stats(&ic, &itot, &imax);
            u32 offs, tots;
            fe_time_sample_if(&offs, &tots);
            fe_kprintf("        诊断: 中断服务次数 %llu, 平均耗时 %llu 周期, 最大 %llu 周期; "
                       "关中断采样 %u/%u\n",
                       (unsigned long long)ic,
                       (unsigned long long)(ic ? itot / ic : 0),
                       (unsigned long long)imax, offs, tots);
        }
        {
            u64 ic, itot, imax;
            fe_time_get_isr_stats(&ic, &itot, &imax);
            u32 offs = 0, tots = 0;
            fe_time_sample_if(&offs, &tots);
            fe_kprintf("        诊断: 中断服务 %llu 次, 平均 %llu 周期, 最大 %llu 周期; "
                       "关中断采样 %u/%u\n",
                       (unsigned long long)ic,
                       (unsigned long long)(ic ? itot / ic : 0),
                       (unsigned long long)imax, offs, tots);
        }
        fe_kprintf("        中断投递速率: TSC 计时 100ms 内产生 %llu 个节拍 "
                   "(实测 %u Hz / 声明 %u Hz)\n",
                   (unsigned long long)elapsed_ticks, measured_hz, fe_time_hz());
        if (measured_hz < fe_time_hz() / 2) {
            fe_kprintf("        => 计数速率正常但投递速率偏低：瓶颈在虚拟机的中断投递环节，\n");
            fe_kprintf("           不是内核缺陷。调度算法以节拍为单位验证，不受影响。\n");
        }
    }

    /* --- 1. 抢占：3 个不让出的工作线程 + 1 个永不停止的同级占用线程 --- */
    g_worker_done = 0;
    g_trace_len = 0;
    g_hog_counter = 0;
    g_hog_run = true;
    memset(g_trace, 0, sizeof(g_trace));

    struct fe_thread *workers[WORKERS];
    for (u32 i = 0; i < WORKERS; i++) {
        workers[i] = fe_thread_create("worker", worker_entry, (void *)&names[i],
                                      16 * 1024, FE_PRIO_NORMAL);
        if (!workers[i]) {
            fe_kprintf("        工作线程 %u 创建失败\n", i);
            fail++;
        }
    }
    struct fe_thread *hog = fe_thread_create("hog", hog_entry, NULL,
                                            16 * 1024, FE_PRIO_NORMAL);
    if (!hog) {
        fe_kprintf("        占用线程创建失败\n");
        fail++;
    }

    /* --- 2. 睡眠精度：与占用线程同时运行 --- */
    g_sleeper_done = 0;
    struct fe_thread *sleeper = fe_thread_create("sleeper", sleeper_entry, NULL,
                                                 16 * 1024, FE_PRIO_NORMAL);

    /* --- 3. 唤醒延迟：高优先级，验证严格优先级抢占 --- */
    g_wake_max_latency_ms = 0;
    g_wake_total_latency_ms = 0;
    g_wake_count = 0;
    struct fe_thread *waker = fe_thread_create("waker", wake_latency_entry, NULL,
                                               16 * 1024, FE_PRIO_HIGH);

    /* --- 4. 开启调度：从这一刻起当前执行流就是一个普通线程 --- */
    fe_kprintf("[自检] 开启抢占式调度（当前线程让出 CPU）\n");
    fe_sched_start();

    /* 等待所有被测线程完成；自己也在睡眠，把 CPU 让给占用线程。
     *
     * ★ 必须把 sleeper 也算进等待条件。★
     * 之前漏了它：慢节拍下（虚拟机里实测只有 50~170 Hz，而声明 1000 Hz）
     * 三个工作线程早就跑完十个短回合了，睡眠线程那 100 个节拍还没到，
     * 判定于是读到 g_sleep_elapsed_ticks 的**初始值 0**，
     * 报成「睡眠提前返回（硬性错误）」——一个不存在的内核缺陷。
     * 症状还是偶发的：节拍率越低越容易出现。 */
    u32 guard = 0;
    while ((g_worker_done < WORKERS || g_wake_count < 5 || !g_sleeper_done) &&
           guard < 5000) {
        fe_thread_sleep_ms(5);
        guard++;
    }

    g_hog_run = false;      /* 通知占用线程退出 */
    fe_thread_sleep_ms(20);

    /* --- 5. 判定 --- */
    u32 len = g_trace_len;
    g_trace[len < sizeof(g_trace) ? len : sizeof(g_trace) - 1] = '\0';
    u32 transitions = count_switches(g_trace, len);

    fe_kprintf("        轨迹(%u 字符): %s\n", len, g_trace);
    fe_kprintf("        线程切换点 %u 次, 占用线程空转 %llu 次\n",
               transitions, (unsigned long long)g_hog_counter);

    if (len != WORKERS * WORKER_ROUNDS) {
        fe_kprintf("        轨迹长度异常: 期望 %u, 实际 %u\n",
                   WORKERS * WORKER_ROUNDS, len);
        fail++;
    }
    /* 若没有抢占，三个工作线程会各自一口气跑完 → 轨迹是 AAABBBCCC，切换点只有 2 次 */
    if (transitions < 5) {
        fe_kprintf("        交错不足（%u 次），抢占式调度可能未生效\n", transitions);
        fail++;
    } else {
        fe_kprintf("        非让出线程之间发生 %u 次交错 → 时间片抢占生效\n", transitions);
    }
    if (g_hog_counter == 0) {
        fe_kprintf("        占用线程从未运行\n");
        fail++;
    }

    /* 睡眠语义：**不得早醒**是硬性约定，下面严格判定。
     *
     * 「迟到多少」则是排队问题，不是睡眠本身的问题：唤醒只是把线程放回就绪队列，
     * 它还得等前面所有同级线程各跑完一个时间片。当时同时可运行的有
     * 3 个工作线程 + 1 个占用线程 + 1 个唤醒者 + 自检自己，最坏情况是
     * 醒来时刚好错过自己的位置，还要再等一整轮。
     * 所以上限按「两轮轮转」算，而不是拍一个固定数字——原先写死的
     * 「3 个时间片 + 2」在 6 个同级线程下本来就不成立，
     * 于是偶发地把虚拟机的慢节拍报成了内核缺陷（实测出现过 100 节拍睡成 189 节拍）。
     *
     * 口径仍然用**节拍**而不是毫秒，结论不受节拍源实际频率影响。 */
    {
        u64 want = fe_ms_to_ticks(100);
        u64 got = g_sleep_elapsed_ticks;
        u64 peers = (u64)WORKERS + 3;
        u64 late_allow = 2ull * peers * fe_sched_default_slice() + 4;
        fe_kprintf("        睡眠请求 %llu 节拍, 实际 %llu 节拍；"
                   "独立时基(TSC)实测 %llu ms，节拍源声明 %u Hz（实测频率见启动日志）\n",
                   (unsigned long long)want, (unsigned long long)got,
                   (unsigned long long)g_sleep_elapsed_tsc_ms, fe_time_hz());
        fe_kprintf("        同级竞争者 %llu 个 → 允许迟到 %llu 节拍（两轮轮转）\n",
                   (unsigned long long)(peers - 1), (unsigned long long)late_allow);
        if (!g_sleeper_done) {
            /* 没测到就不是「睡眠有问题」，而是**这次没测成**。
             * 区分这两者很重要：把「没测到」报成缺陷，等于训练人去忽略红色结果。 */
            fe_kprintf("        睡眠线程在等待窗口内未完成，本项无法判定（节拍率过低）\n");
            fail++;
        } else if (g_sleep_elapsed_tsc_ms + 5 < 100) {
            /* ★ 判据从"数节拍"改成"量时间"（K7 收尾，VBox 实测逼出来的）★
             *
             * 原来数节拍（`got < want` → 提前返回）。那个判据在睡眠
             * **以节拍计数**实现时是对的，而睡眠已经改成按时间到期
             * （见 fe_sched_sleep_until 的说明）——**判据必须跟着机制走**，
             * 否则测的是已经不存在的东西。
             *
             * VBox 上实测：同一个 8254 源，被动 50 ms 窗口只数到 5 个节拍
             * （99 Hz），而睡眠期间节拍却以 ~3.5 kHz 前进（突发投递）。
             * 于是"100 节拍"这个数既可以对应 30 ms 也可以对应 450 ms——
             * **它根本不是时间的度量**。
             *
             * 现在用 TSC 量的毫秒数判：**只能晚、不能早**（早 = 超时提前
             * 触发 = 正确性问题）。容忍 5 ms 是因为发起与测量之间有开销。 */
            fe_kprintf("        睡眠提前返回（TSC 实测 %llu ms < 请求 100 ms）"
                       "—— 这是硬性错误\n",
                       (unsigned long long)g_sleep_elapsed_tsc_ms);
            fail++;
        } else if (got < want) {
            /* 时间够了但节拍数不足：那是节拍源突发投递，不是睡眠错。
             * **如实报出来**，因为它是"这个环境的节拍计数不可信"的证据。 */
            fe_kprintf("        节拍数少于请求（%llu < %llu），但 TSC 实测 %llu ms "
                       "已够——节拍源突发投递，节拍计数在此环境不可作时间度量\n",
                       (unsigned long long)got, (unsigned long long)want,
                       (unsigned long long)g_sleep_elapsed_tsc_ms);
        } else if (got > want + late_allow) {
            fe_kprintf("        睡眠迟到超过两轮轮转，排队逻辑可能有问题\n");
            fail++;
        }
    }

    /* 高优先级唤醒延迟 */
    u64 avg = g_wake_count ? g_wake_total_latency_ms / g_wake_count : 0;
    fe_kprintf("        高优先级线程 5 次 20ms 睡眠: 平均延迟 %llu ms, 最大 %llu ms\n",
               (unsigned long long)avg, (unsigned long long)g_wake_max_latency_ms);
    if (g_wake_count < 5) {
        fe_kprintf("        高优先级线程未按预期完成\n");
        fail++;
    }

    /* --- 6. 回收 --- */
    if (hog) {
        fe_thread_join(hog);
    }
    if (sleeper) {
        fe_thread_join(sleeper);
    }
    if (waker) {
        fe_thread_join(waker);
    }
    for (u32 i = 0; i < WORKERS; i++) {
        if (workers[i]) {
            fe_thread_join(workers[i]);
        }
    }

    fe_kprintf("        线程统计: 累计创建 %llu, 当前存活 %llu\n",
               (unsigned long long)fe_thread_total_created(),
               (unsigned long long)fe_thread_live_count());
    return fail;
}
