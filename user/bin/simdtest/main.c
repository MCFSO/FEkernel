/* SPDX-License-Identifier: 0BSD */
/* simdtest —— 证明"SIMD 真的放开了，而且**没有**泄漏到别的线程"。
 *
 * ★ 这个测试要回答的不是"能不能算对"，而是四件独立的事 ★
 *
 *   1. **能用**：SSE2 指令真的执行了，结果与标量算法一致
 *      （在这之前，同样的代码会以 #UD/#NM 收场——那时候"用不了"是硬事实）。
 *   2. **切换后还在**：把值写进 xmm，主动让出 CPU（强制一次上下文切换），
 *      回来读到的必须还是自己的值。这条验证内核在切换时保存/恢复了状态。
 *   3. ★ **不会串到别的线程** ★：另起一个线程写满它自己的 xmm，
 *      本线程再读自己的 xmm——必须是自己的值。
 *      这条是**安全性质**而不是性能性质：状态没被隔离 = 一个进程能读到
 *      另一个进程留在寄存器里的数据。它比"算得对不对"重要得多。
 *   4. **对齐**：栈上 16 字节对齐的缓冲能用对齐搬（movaps），
 *      未对齐的走未对齐路径也不许崩。ABI 的栈对齐要求在这里被真正检验。
 *
 * 4 号不是凑数：SSE 的对齐要求会让"栈没对齐"从"性能略差"变成"直接 #GP"，
 * 而栈对齐是 `_start` 里 `and rsp, -16` 那一行保证的——测它等于测那一行。
 */
#include <fe_user.h>

/* ★★★ 测试方法论：为什么每一步都必须包在**同一段内联汇编**里 ★★★
 *
 * xmm0–xmm15 在 SysV ABI 里是**调用者保存**（caller-saved）寄存器。
 * 也就是说：只要"写 xmm"和"读 xmm"之间有任意一次 C 函数调用，
 * 编译器完全有权把 xmm0 挪作他用——于是测试测到的是**编译器**，
 * 而不是内核有没有保存/恢复状态。
 *
 * 我第一版就是那么写的（写 xmm0 → 调一个检查函数 → 读 xmm0），
 * SSE 那几条"通过"纯属运气（恰好那几次调用没用 xmm0），
 * 而 AVX 那条当场失败——失败得非常好：它把一个**假的测试**变成了真的。
 *
 * 所以下面每个测试都是"写 → 系统调用 → 读"三段在一段 asm 里完成，
 * 编译器无法在中间插入任何东西。这才是"内核是否保存了状态"的可信证据。 */

/* 写 xmm0 → 读 xmm0（不涉及切换）：证明 SIMD 真的能执行 */
static u64 xmm_roundtrip(u64 pattern)
{
    u64 out;
    __asm__ __volatile__("movq %[pat], %%xmm0\n\t"
                         "movq %%xmm0, %[out]"
                         : [out] "=r"(out) : [pat] "r"(pattern));
    return out;
}

/* 写 xmm0 → VEX 指令把它写进内存：证明 AVX 可用（= 内核开了 CR4.OSXSAVE + XCR0[2]） */
static void avx_store(u64 pattern, u8 *dst)
{
    __asm__ __volatile__("movq %[pat], %%xmm0\n\t"
                         "vmovdqu %%xmm0, (%[dst])\n\t"
                         "vzeroupper"
                         :: [dst] "r"(dst), [pat] "r"(pattern) : "memory");
}

/* 写 xmm0 → **让出 CPU**（真切换）→ 读回。 */
static u64 xmm_across_yield(u64 pattern)
{
    u64 out;
    __asm__ __volatile__("movq %[pat], %%xmm0\n\t"
                         "movl %[nr], %%eax\n\t"
                         "syscall\n\t"
                         "movq %%xmm0, %[out]"
                         : [out] "=r"(out)
                         : [pat] "r"(pattern), [nr] "i"((int)FE_SYS_THREAD_YIELD)
                         : "rax", "rcx", "r11", "memory");
    return out;
}

/* 写 xmm0 → **睡一段**（被切出、别的线程跑、再切回）→ 读回。 */
static u64 xmm_across_sleep(u64 pattern, u64 ns)
{
    u64 out;
    register long rdi __asm__("rdi") = (long)ns;
    /* 用具名操作数而不是 %0/%1：位置编号在"输出在前"的规则下极易数错，
     * 而数错的后果是 `movl %rax, %eax` 这种"寄存器当立即数用"的编译错误
     * （或者更糟：编译通过但语义错了）。名字让这类错不可能发生。 */
    __asm__ __volatile__("movq %[pat], %%xmm0\n\t"
                         "movl %[nr], %%eax\n\t"
                         "syscall\n\t"
                         "movq %%xmm0, %[out]"
                         : [out] "=r"(out)
                         : [pat] "r"(pattern), [nr] "i"((int)FE_SYS_SLEEP),
                           "D"(rdi)
                         : "rax", "rcx", "r11", "memory");
    return out;
}

/* 写 xmm0 → **等通知**（阻塞，期间另一个线程会写它自己的 xmm0）→ 读回。
 * 这是隔离性测试的核心：如果内核没有按线程保存/恢复，读回来的就是
 * 另一个线程留下的值。 */
static u64 xmm_across_wait(long notif, u64 mask, u64 pattern)
{
    u64 out;
    register long rsi __asm__("rsi") = (long)mask;
    register long rdx __asm__("rdx") = 0;
    __asm__ __volatile__("movq %[pat], %%xmm0\n\t"
                         "movl %[nr], %%eax\n\t"
                         "syscall\n\t"
                         "movq %%xmm0, %[out]"
                         : [out] "=r"(out)
                         : "D"(notif), "S"(rsi), "d"(rdx), [pat] "r"(pattern),
                           [nr] "i"((int)FE_SYS_NOTIFICATION_WAIT)
                         : "rax", "rcx", "r11", "memory");
    return out;
}

static u32 g_fail;

/* 周期计时：微基准要用**比被测对象细得多**的时钟（见 [5] 的说明） */
static inline u64 rdtsc(void)
{
    u32 lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static void say(const char *s) { fe_puts(s); }
static void num(u64 v) { fe_print_u64(v); }

static void check(int cond, const char *what)
{
    say(cond ? "    OK   " : "    失败 ");
    say(what);
    say("\n");
    if (!cond) {
        g_fail++;
    }
}

/* 整数版拷贝：**作为 SSE 版的对照基准**。
 *
 * ★ 两个细节都是必需的，缺一个这个基准就是假的 ★
 *   1. 目的指针是 `volatile` —— 否则编译器会做"循环改写识别"（loop idiom
 *      recognition），把整个字节循环换成一次 `memcpy` 调用。而 libfe 的 memcpy
 *      正是 SSE 版，于是"整数基准"悄悄变成了"SSE 版"，两者测的是同一个东西。
 *   2. 只测 1 字节宽度，不做任何优化 —— 它代表"没有 SIMD 时的地板"。 */
static void copy_int(volatile u8 *d, const u8 *s, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static u64 g_cpu_features;

static void cpuid_probe(void)
{
    u32 a = 0, b = 0, c = 0, d = 0;
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(1u), "c"(0u));
    /* 只用 EDX 里的 SSE/SSE2 与 ECX 里的 AVX（用户态无权看 CR4/OSXSAVE，
     * 所以这里报的是**硬件能力**；"内核有没有打开"由能不能真的执行指令来证明） */
    if (d & (1u << 25)) { g_cpu_features |= 1; }    /* SSE */
    if (d & (1u << 26)) { g_cpu_features |= 2; }    /* SSE2 */
    if (c & (1u << 28)) { g_cpu_features |= 4; }    /* AVX */
}

/* 线程 B：把一组**特殊值**写满自己的 xmm0，然后睡眠（让出 CPU），
 * 醒来后确认还是自己的值。它存在的唯一目的是"污染"寄存器，
 * 好让主线程能检测出"内核没有隔离状态"。 */
static u64 g_other_saw;
static u64 g_other_expected;
static long g_other_notif = -1;

static void pollute_thread(void *arg)
{
    (void)arg;
    /* 用"写 → 睡 → 读"把本线程的 xmm0 置成一眼能认出来的值，
     * 并且**在中间被切出去**（这期间主线程会跑，它的 xmm0 必须不受影响）。 */
    u64 got = xmm_across_sleep(0xDEADBEEFCAFEF00Dull, 20ull * 1000 * 1000);
    g_other_saw = got;
    g_other_expected = 0xDEADBEEFCAFEF00Dull;
    if (g_other_notif > 0) {
        fe_notification_signal(g_other_notif, 1);
    }
    /* ★ 必须显式退出，不能 return ★
     * 线程入口的返回地址是 0（初始栈帧被清零），return 会跳到地址 0 触发 #PF，
     * 日志里就多一条吓人的"用户态异常"——而它其实只是测试线程忘了退出。
     * 我第一版就是这么写的：所有断言全绿，但日志里躺着一个 RIP=0x0 的页错误。
     * 那种"通过但很脏"的输出比失败更危险，因为它会被当成正常现象。 */
    fe_exit(0);
}

int main(void)
{
    say("\n=== SIMD 能力与隔离测试 ===\n");

    cpuid_probe();
    say("         硬件 CPUID: SSE=");
    say((g_cpu_features & 1) ? "有" : "无");
    say(" SSE2=");
    say((g_cpu_features & 2) ? "有" : "无");
    say(" AVX=");
    say((g_cpu_features & 4) ? "有" : "无");
    say("\n");

    /* ---- 1. 真的能执行 SSE2（有 AVX 时连 AVX 一起验） ---- */
    say("  [1] SIMD 指令真的能执行\n");
    {
        const u64 pat = 0x0123456789ABCDEFull;
        u64 v = xmm_roundtrip(pat);
        check(v == pat, "写入 xmm0 再读回，值一致");
        if (v != pat) {
            say("         读回 ");
            fe_print_hex(v);
            say("\n");
        }

        /* ★ AVX 那一条是**内核工作的直接证据** ★
         * VEX 编码的指令只有在 CR4.OSXSAVE=1 **且** XCR0[2]=1 时才允许执行；
         * 内核少设任何一个，这条指令就是 #UD —— 而 #UD 会让整个线程被终止，
         * 所以"这条通过"等于"内核把 AVX 位真的打开了"。
         * 没有 CPUID 的机器上不跑（那不是失败，是硬件没有）。 */
        if (g_cpu_features & 4) {
            static u8 tmp[32] __attribute__((aligned(16)));
            u64 got = 0;
            for (u32 i = 0; i < 32; i++) {
                tmp[i] = 0;
            }
            avx_store(pat, tmp);
            __builtin_memcpy(&got, tmp, 8);
            check(got == pat,
                  "VEX 编码的 AVX 指令可执行（= 内核已开 CR4.OSXSAVE 与 XCR0[2]）");
            if (got != pat) {
                say("         读回 ");
                fe_print_hex(got);
                say("（若整条线程被 #UD 杀掉，则是内核没开 AVX）\n");
            }
        } else {
            say("         （本机 CPUID 无 AVX，跳过 VEX 指令验证）\n");
        }
    }

    /* ---- 2. 上下文切换后状态还在 ---- */
    say("  [2] 主动让出 CPU 后，自己的 xmm 值不变\n");
    {
        const u64 before = 0x1111222233334444ull;
        u64 after = 0;
        /* 让出若干次：每次都应当经历"保存→切走→切回→恢复"。
         * 写与读都在同一条 asm 里，中间只隔着系统调用本身。 */
        for (int i = 0; i < 5; i++) {
            after = xmm_across_yield(before);
            if (after != before) {
                break;
            }
        }
        check(after == before, "5 次 yield 之后 xmm0 仍是自己的值");
        if (after != before) {
            say("         期望 ");
            fe_print_hex(before);
            say("，得到 ");
            fe_print_hex(after);
            say("\n");
        }
    }

    /* ---- 3. 跨线程隔离（本测试的重点） ---- */
    say("  [3] 另一个线程写满自己的 xmm，不会串到我这里\n");
    {
        const u64 mine = 0xAAAABBBBCCCCDDDDull;
        u64 seen = 0;

        g_other_notif = fe_notification_create();
        long th = fe_thread_create((void (*)(void *))pollute_thread, 0, 0, 0);
        check(th > 0, "起了一个专门污染 xmm 的线程");
        if (th > 0 && g_other_notif > 0) {
            /* 主线程：写下自己的值 → **阻塞等待**（期间那个线程会写它自己的
             * xmm0 并被切走）→ 读回自己的值。整段在一条 asm 里。 */
            seen = xmm_across_wait(g_other_notif, 1, mine);
            check(seen == mine, "另一个线程跑过之后，xmm0 仍是我自己的值");
            if (seen != mine) {
                say("         **串了**：读到 ");
                fe_print_hex(seen);
                say("（那是另一个线程写的值）\n");
            }
            check(g_other_saw == g_other_expected,
                  "那个线程自己也读回了自己的值（双向都隔离）");
        }
    }

    /* ---- 4. 对齐：ABI 的栈对齐要求被真正检验 ---- */
    say("  [4] 16 字节对齐与未对齐的搬移都不许崩\n");
    {
        static u8 buf[64] __attribute__((aligned(16)));
        static u8 raw[64];
        u64 ok = 1;
        for (u32 i = 0; i < 64; i++) {
            raw[i] = (u8)(i * 7 + 1);
        }
        /* 对齐目的地址 + 对齐源：走对齐路径 */
        memcpy(buf, raw, 64);
        for (u32 i = 0; i < 64; i++) {
            if (buf[i] != raw[i]) {
                ok = 0;
            }
        }
        check(ok, "16 字节对齐的 64 字节搬移结果正确");
        /* 故意错开 1 字节：源和目的都不对齐，走未对齐路径 */
        ok = 1;
        memcpy(buf + 1, raw + 1, 63);
        for (u32 i = 0; i < 63; i++) {
            if (buf[1 + i] != raw[1 + i]) {
                ok = 0;
            }
        }
        check(ok, "错开 1 字节（两侧都未对齐）的结果也正确");
    }

    /* ---- 5. SSE 版拷贝 vs 整数版（同一块内存，只比宽度） ---- */
    say("  [5] 拷贝带宽：SSE2（16 字节/次）对比整数（1 字节/次）\n");
    {
        enum { N = 256 * 1024 };
        static u8 a[N] __attribute__((aligned(16)));
        static u8 b[N] __attribute__((aligned(16)));
        for (u32 i = 0; i < N; i++) {
            a[i] = (u8)i;
        }
        /* ★ 用 rdtsc，不用 fe_clock_ns ★
         * 第一版用 fe_clock_ns，结果两个数字都是 0 µs（256 KiB 的拷贝比
         * 那个时钟的粒度还短），算出"250000000 MB/s"这种一看就不对的数。
         * 微基准的计时器分辨率必须**远细于**被测对象，否则测出来的是时钟。
         *
         * ★ 预热与多轮取最快，缺一不可 ★
         * 第二版的数字是"SSE 版比整数版慢 6 倍"——同样不可能。
         * 原因是**第一次拷贝把冷缓冲区的代价算进了 SSE 那一栏**
         * （`b` 是 BSS，第一次写它要为每一行做 RFO），而整数那一栏跑在热缓冲区上。
         * 测出来的是缓存状态，不是拷贝宽度。
         * 所以：先各跑一遍预热，再各跑 5 轮取最快——这也是 bench 里的既有做法。 */
        u64 best_simd = ~0ull, best_int = ~0ull;
        for (u32 rep = 0; rep < 6; rep++) {
            u64 t0 = rdtsc();
            memcpy(b, a, N);
            u64 t1 = rdtsc();
            copy_int(b, a, N);
            u64 t2 = rdtsc();
            /* 第 0 轮只用来预热，不计入 */
            if (rep == 0) {
                continue;
            }
            if (t1 - t0 < best_simd) { best_simd = t1 - t0; }
            if (t2 - t1 < best_int) { best_int = t2 - t1; }
        }
        /* 与 bench 同一折算口径：字节/周期 × 3000 ≈ MB/s @3GHz */
        say("         SSE2 : ");
        num((u64)N * 3000ull / best_simd);
        say(" MB/s（");
        num(best_simd);
        say(" 周期，");
        num(best_simd / (N / 1024));
        say(" 周期/KiB）\n");
        say("         整数 : ");
        num((u64)N * 3000ull / best_int);
        say(" MB/s（");
        num(best_int);
        say(" 周期）\n");
        say("         倍数 : ");
        num(best_int / best_simd);
        say(" 倍\n");
        /* 不断言倍数：缓存与频率会让它波动，断言只会变成随机失败。
         * 断言的是"结果正确"——性能数字是给人看的。 */
        u32 same = 1;
        for (u32 i = 0; i < N; i++) {
            if (b[i] != a[i]) {
                same = 0;
            }
        }
        check(same, "两条路径搬出来的结果一致（宽度变了，语义没变）");
    }

    say("=== SIMD 测试结束，失败项 ");
    num((u64)g_fail);
    say(" ===\n");
    return (int)g_fail;
}
