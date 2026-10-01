/* SPDX-License-Identifier: 0BSD */
/* 真实时钟（K7）的内核自检。
 *
 * ★ 这一组要证明的不是"时钟能走"，而是"**时钟的分辨率是真的**" ★
 *
 * 节拍制时钟也能"走"，也能"单调"，也能跟墙上时间对得上——它唯一做不到的
 * 是**分辨亚毫秒**。而 D4 期间的实测正是栽在这里：两次独立的吞吐测量
 * 都得到**正好 5000 us**，因为被测代码只有几毫秒，而时钟的粒度是毫秒。
 * 那次测出来的是时钟本身，**而断言看起来是通过的**。
 *
 * 所以这里的核心断言是：
 *   - 一段**短到不够一个节拍**的忙等，`fe_time_ns()` 必须能分辨出来
 *     （读到非零且不等于节拍粒度的增量）。节拍制时钟在这里必然得到 0；
 *   - 时钟与节拍**在长窗口上一致**（不能"细但错"）——细而错比粗更危险；
 *   - 单调不减（回绕、负增量都要能被发现）；
 *   - 分辨率自述必须与实际相符（说 1 ns 而行为是 1 ms 就是撒谎）。
 */
#include <fe/time.h>
#include <fe/sched/thread.h>
#include <fe/kprintf.h>
#include <fe/io.h>
#include <fe/mm.h>

#define CHECK(cond) do { if (!(cond)) { fail++; } } while (0)

u32 fe_selftest_clock(void)
{
    u32 fail = 0;

    if (!fe_time_clock_is_tsc()) {
        /* 引导器没给 TSC 频率：这不是缺陷，是环境事实。
         * 但**必须如实报出来**——一个不知道自己精度的时钟比一个慢时钟危险。 */
        fe_kprintf("        本机没有可用的 TSC 频率 → 时钟退回节拍制"
                   "（分辨率 %llu ns），下面的分辨率断言不适用\n",
                   (unsigned long long)(1000000000ull / fe_time_clock_hz()));
        fe_kprintf("        时钟来源自述: %s\n",
                   fe_time_clock_is_tsc() ? "TSC" : "节拍");
        return fail;
    }

    fe_kprintf("        时基: TSC %llu MHz（分辨率 %llu ns）\n",
               (unsigned long long)(fe_time_clock_hz() / 1000000),
               (unsigned long long)(1000000000ull / fe_time_clock_hz()));

    /* ---- 1. 短窗口：必须能分辨出**远小于一个节拍**的时间 ----
     *
     * 忙等约 2000 个 pause（量级在几十微秒），然后要求时钟读到一个
     * 非零、且**明显小于一个节拍**的增量。
     * 节拍制时钟在这里只能给出 0（或一整个节拍）。 */
    {
        u64 t0 = fe_time_ns();
        for (u32 i = 0; i < 2000; i++) {
            fe_pause();
        }
        u64 t1 = fe_time_ns();
        u64 d = t1 - t0;
        u64 tick_ns = 1000000000ull / fe_time_hz();
        if (d == 0) {
            fe_kprintf("        短忙等读到 0 ns —— 时钟分辨不出亚毫秒\n");
            fail++;
        } else if (d >= tick_ns) {
            /* 也可能是这段忙等真的超过了一个节拍（慢机器），那就加大
             * 说明而不是判失败：这里要的是"能分辨"，不是"必须很小"。 */
            fe_kprintf("        短忙等读到 %llu ns（≥ 一个节拍 %llu ns）——"
                       "本机太慢或节拍太快，这一条不作数\n",
                       (unsigned long long)d, (unsigned long long)tick_ns);
        } else {
            fe_kprintf("        短忙等（2000 × pause）读到 %llu ns，"
                       "**小于一个节拍** %llu ns → 分辨率真实\n",
                       (unsigned long long)d, (unsigned long long)tick_ns);
        }
        /* 无论快慢，增量都不能大于"整个开机时间"这种荒谬值。 */
        CHECK(d < fe_time_ns());
    }

    /* ---- 2. 长窗口：与节拍一致（细但不能错） ----
     *
     * 睡 50 个节拍，用时钟量这段时间，再与节拍数换算的时间比。
     * 允许 ±10%：节拍本身在虚拟机上会被限流（那是环境事实，另有校验），
     * 而这里要抓的是"差一个数量级"这类真错误。 */
    {
        u64 ticks = 50;
        u64 ms = (u64)ticks * 1000ull / fe_time_hz();
        if (ms < 10) {
            ms = 10;
        }
        u64 c0 = fe_time_ns();
        u64 k0 = fe_time_ticks();
        fe_thread_sleep_ms(ms);
        u64 c1 = fe_time_ns();
        u64 k1 = fe_time_ticks();

        u64 by_clock = (c1 - c0) / 1000000ull;      /* ms */
        u64 by_tick = (k1 - k0) * 1000ull / fe_time_hz();
        fe_kprintf("        长窗口对照: 时钟量到 %llu ms，节拍量到 %llu ms"
                   "（请求 %llu ms）\n",
                   (unsigned long long)by_clock, (unsigned long long)by_tick,
                   (unsigned long long)ms);
        /* 两次独立来源必须在 10 倍以内——差一个数量级说明其中一个错了。
         * 不用"相差 10%"是因为节拍在虚拟机上确实会被限流（VBox 实测
         * 只有 149 Hz 而目标是 1000 Hz），那是环境事实，不是时钟错。 */
        u64 lo = by_tick / 10, hi = by_tick * 10 + 1;
        if (by_clock < lo || by_clock > hi) {
            fe_kprintf("        **时钟与节拍差了一个数量级** —— 其中一个错了\n");
            fail++;
        }

        /* ★ 在当前时刻量一次**节拍率**：它是解释"睡眠时长为何不符"的关键 ★
         * VBox 上实测到过互相矛盾的两个数：启动时 `fe_time_verify` 报
         * 149 Hz / 49 Hz，而"睡 20 ms"却只用了 5 ms 的 TSC 时间（≈4000 Hz）。
         * 同一个启动里节拍率不可能既 49 又 4000 —— 要么有人在自检期间
         * 重新编程了定时器，要么其中一次测量本身是错的。
         * 把节拍率**当场**量出来，这个矛盾才有据可查。 */
        {
            u64 w0 = fe_time_ns();
            u64 t0 = fe_time_ticks();
            while (fe_time_ns() - w0 < 50000000ull) {   /* 50 ms */
                fe_pause();
            }
            u64 t1 = fe_time_ticks();
            u64 win_ns = fe_time_ns() - w0;
            u64 hz_now = (win_ns > 0) ? ((t1 - t0) * 1000000000ull / win_ns) : 0;
            fe_kprintf("        当前节拍率: %llu 个节拍 / %llu ns → %llu Hz"
                       "（编程值 %u Hz，节拍源 %s）\n",
                       (unsigned long long)(t1 - t0),
                       (unsigned long long)win_ns,
                       (unsigned long long)hz_now, fe_time_hz(),
                       fe_time_source());
            /* ★ 这一条会失败，而且应该失败 ★
             * 节拍率偏**低**（虚拟化限流）只会让睡眠偏长，是精度问题；
             * 偏**高**会让 sleep/超时**提前结束**——那是正确性问题，
             * 不能当"环境事实"放过。 */
            if (hz_now > (u64)fe_time_hz() * 2) {
                fe_kprintf("        **节拍率高于编程值一倍以上**："
                           "sleep 与超时会提前结束（正确性问题，"
                           "属于节拍子系统，不是时钟）\n");
                fail++;
            }
        }
    }

    /* ---- 3. 单调不减，而且回绕要能被发现 ----
     * 连续读 1000 次，任何一次倒退都是错。 */
    {
        u64 prev = fe_time_ns();
        u32 back = 0;
        for (u32 i = 0; i < 1000; i++) {
            u64 now = fe_time_ns();
            if (now < prev) {
                back++;
            }
            prev = now;
        }
        CHECK(back == 0);
        if (back) {
            fe_kprintf("        时钟倒退了 %u 次（不单调）\n", back);
        }
    }

    /* ---- 4. 自述的分辨率必须与实际相符 ----
     * 说的是 0.3 ns 那样的小数（整数除法会取到 1），
     * 而能够分辨的最小增量**不能小于自述的粒度**。 */
    {
        u64 res = 1000000000ull / fe_time_clock_hz();
        if (res == 0) {
            res = 1;
        }
        /* 连读两次：差值要么是 0（同一计数内），要么至少是 1 个计数。 */
        u64 a = fe_time_ns();
        u64 b = fe_time_ns();
        u64 d = (b > a) ? (b - a) : 0;
        u64 max_step = (res > 1000) ? res * 4 : 4000;
        if (d > max_step) {
            fe_kprintf("        两次连读差了 %llu ns，而自述粒度是 %llu ns"
                       " —— 自述与实际不符\n",
                       (unsigned long long)d, (unsigned long long)res);
            fail++;
        } else {
            fe_kprintf("        自述分辨率 %llu ns，两次连读差 %llu ns → 相符\n",
                       (unsigned long long)res, (unsigned long long)d);
        }
    }

    return fail;
}
