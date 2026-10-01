/* SPDX-License-Identifier: 0BSD */
/* clocktest —— 真实时钟（K7）的**用户态承重测试**。
 *
 * ★ 它要证明的不是"时钟能走"，而是"能分辨亚毫秒" ★
 *
 * 节拍制时钟也能走、也能单调、也能跟睡眠对得上——它唯一做不到的是
 * 分辨亚毫秒。而 D4 期间正是栽在这里：`fe_clock_ns()` 实际返回
 * "毫秒 × 1e6"，被测代码只有几毫秒，于是两次**独立**的吞吐测量都得到
 * **正好 5000 us**。那次测出来的是时钟本身，而断言看起来是通过的。
 *
 * 所以这里的核心断言是第 2 条：一段短到不够一个节拍的循环，必须读到
 * 非零的增量。**节拍制时钟在同一条断言上必然得到 0。**
 *
 * ★ 第 4 条是交叉验证 ★
 * 客户端自己读 TSC（用内核自述的频率换算成纳秒），与 `fe_clock_ns()`
 * 比。如果内核的时钟其实建在节拍上，这两条路会差出三个数量级——
 * 而"两条独立路径互相印证"比"一个数字看起来合理"强得多。
 *
 * 退出码 = 失败项数。
 */
#include <fe_user.h>

static u32 g_fail;

static void say(const char *s) { fe_puts(s); }
static void num(u64 v) { fe_print_u64(v); }
static void flush(void) { fe_flush(); }

static void check(int cond, const char *what)
{
    say(cond ? "    OK   " : "    失败 ");
    say(what);
    say("\n");
    flush();
    if (!cond) {
        g_fail++;
    }
}

static inline u64 rdtsc(void)
{
    u32 lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    say("\n=== 真实时钟（K7：TSC 制）===\n");

    /* ---- 0. 先问清楚精度，而不是假设 ---- */
    struct fe_clock_info ci;
    long r = fe_clock_info(&ci);
    check(r == FE_OK, "取时钟自述（fe_clock_info）");
    if (r != FE_OK) {
        say("=== 真实时钟测试结束，失败项 ");
        num(g_fail);
        say(" ===\n");
        return (int)g_fail;
    }
    say("  [clock] 时基 ");
    num(ci.hz / 1000000ull);
    say(" MHz，分辨率 ");
    num(ci.resolution_ns);
    say(" ns，来源 ");
    say((ci.flags & FE_CLOCK_FLAG_TSC) ? "TSC" : "节拍（本机没有 TSC 频率）");
    say("\n");
    check(ci.resolution_ns > 0, "自述的分辨率是正数（不是 0 = 假装无限精确）");

    const int is_tsc = (ci.flags & FE_CLOCK_FLAG_TSC) != 0;
    if (!is_tsc) {
        /* 环境事实，不是缺陷：本机没有 TSC 频率。如实说明并跳过依赖
         * TSC 的那两条——**不假装测过**。 */
        say("  [clock] 本机时钟退回节拍制（1 ms 粒度），"
            "亚毫秒断言不适用\n");
    }

    /* ---- 1. 单调 + 长窗口对得上睡眠 ---- */
    {
        u64 t0 = fe_clock_ns();
        u64 prev = t0;
        u32 back = 0;
        for (u32 i = 0; i < 200; i++) {
            u64 now = fe_clock_ns();
            if (now < prev) {
                back++;
            }
            prev = now;
        }
        check(back == 0, "连续取样 200 次，时钟从不倒退");

        u64 a = fe_clock_ns();
        fe_sleep_ms(20);
        u64 b = fe_clock_ns();
        u64 ms = (b - a) / 1000000ull;
        say("  [clock] 睡 20 ms，时钟量到 ");
        num(ms);
        say(" ms\n");
        /* 上界放宽（虚拟机对定时器中断限流），下界必须够紧：
         * 睡 20 ms 却量到 5 ms 就说明时钟快得离谱。 */
        check(ms >= 10 && ms <= 200, "睡眠时长被量在合理范围内（10~200 ms）");
    }

    /* ---- 2. ★ 核心：短窗口必须能分辨出亚毫秒 ★ ----
     * 这段循环量级在几十微秒。节拍制时钟在这里只能给出 0
     * （或者一整个 1000000 ns 的台阶）。 */
    if (is_tsc) {
        u64 t0 = fe_clock_ns();
        u64 sink = 0;
        for (u32 i = 0; i < 20000; i++) {
            sink += i;
            __asm__ __volatile__("" : "+r"(sink));
        }
        u64 t1 = fe_clock_ns();
        u64 d = t1 - t0;
        say("  [clock] 短循环（20000 次累加）读到 ");
        num(d);
        say(" ns\n");
        check(d > 0, "短循环读到**非零**增量（节拍制时钟在这里只能给 0）");
        check(d < 1000000ull,
              "短循环的增量**小于一个节拍**（1 ms）——分辨率是真的");
        if (d == 0 || d >= 1000000ull) {
            say("        注意：这一条正是 D4 那次两个 5000 us 的来源\n");
        }

        /* ---- 3. 分辨率下限：连读两次不能差出一个节拍 ---- */
        u64 a = fe_clock_ns();
        u64 b = fe_clock_ns();
        u64 step = (b > a) ? (b - a) : 0;
        say("  [clock] 两次连读相差 ");
        num(step);
        say(" ns\n");
        check(step < 100000ull, "两次连读的差值远小于一个节拍（说明粒度确实很小）");

        /* ---- 4. ★ 交叉验证：与客户端自己读的 TSC 比 ★ ----
         * 内核说它的时基是 ci.hz；客户端自己读 TSC 并按同一频率换算。
         * 两条独立路径必须给出同一个时间。 */
        u64 c0 = fe_clock_ns();
        u64 s0 = rdtsc();
        fe_sleep_ms(10);
        u64 c1 = fe_clock_ns();
        u64 s1 = rdtsc();
        u64 by_clock = c1 - c0;
        /* 客户端侧的换算（用内核自述的频率）：同样避免 128 位除法。 */
        u64 dtsc = s1 - s0;
        u64 by_tsc = (dtsc / ci.hz) * 1000000000ull +
                     ((dtsc % ci.hz) * 1000000000ull) / ci.hz;
        say("  [clock] 同一段 10 ms：内核时钟量到 ");
        num(by_clock / 1000);
        say(" us，客户端自己读 TSC 量到 ");
        num(by_tsc / 1000);
        say(" us\n");
        /* 允许 5%：两次读之间有真实开销，而且 TSC 与内核时钟起点不同
         * （这里比的是**增量**，起点会抵消）。 */
        u64 lo = by_tsc - by_tsc / 20;
        u64 hi = by_tsc + by_tsc / 20;
        check(by_clock >= lo && by_clock <= hi,
              "内核时钟与客户端自读 TSC 一致（两条独立路径互相印证）");
        if (by_clock < lo || by_clock > hi) {
            say("        差值超出 5% —— 内核时钟可能不是建在 TSC 上\n");
        }
    }

    /* ---- 反向对照 ---- */
    check(fe_clock_info(0) == FE_ERR_INVAL,
          "反向：fe_clock_info(NULL) 被拒（INVAL）");

    say("=== 真实时钟测试结束，失败项 ");
    num(g_fail);
    say(" ===\n");
    return (int)g_fail;
}
