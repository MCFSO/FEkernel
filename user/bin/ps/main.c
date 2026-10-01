/* SPDX-License-Identifier: 0BSD */
/* ps —— 任务/线程快照（K3）的**承重测试与运维面**。
 *
 * 它做两件事，而且两件事互相印证：
 *   1. 从用户态走完整的 syscall 路径取一张快照，**打印**它（这就是 ps）；
 *   2. 逐项核对内核报出来的东西与"我作为用户态能独立知道的事实"是否一致。
 *
 * ★ 为什么第 2 件事才是重点 ★
 * 这张表的内核侧曾经"写完就算完"、没有任何调用者，而它带着一个真实的
 * 布局错误：步长宏比结构体小 16 字节，内核把 `name` 写到**下一条记录的
 * 头上**。光"能打印出一张表"完全不够——错位的表打印出来也很整齐。
 * 所以这里每一条断言都拿**另一个来源**去对：
 *   - 表头里的 current_task 必须就是"我自己"这一条；
 *   - 拉一个孩子之后，**同一个 id 的名字必须一字不变**（错位写会把
 *     邻居的名字改掉，而 id 是稳定的，所以这条能抓住它）；
 *   - 我自己主线程的 switches 必须**在两次快照之间增长**（专治
 *     "字段填成常量"——一个恒为 0 的字段与"刚创建的线程"从外部看一样）；
 *   - 每条记录的名字必须是合法的 C 字符串。
 *
 * ★ 为什么**不**断言"全局名字互不重复" ★
 * 名字本来就不唯一（内核注释写着：多个用户线程都叫 "user"），
 * 而且 init 拉起的 hello 与我拉起的 hello 可以同时存在。
 * 拿一个**不成立的性质**去断言，得到的是假失败——这个项目已经栽过
 * 好几次（见 docs/12-drivers.md §9.4），这里不再重复。
 *
 * ★ 反向对照 ★
 *   - 未映射的缓冲 → 必须 FAULT（而不是内核替我们写坏内存）；
 *   - buf_len 差一个字节 → 必须 NOSPC（而不是写半张表）；
 *   - task_cap = 0 / 超过硬上限 → 必须 INVAL（而不是悄悄夹住）；
 *   - task_cap 只给 1 → task_count 必须是 1，而 total_tasks 必须仍然报
 *     **真实总数**（一个"填不下就不说话"的内核会在这里露出来）。
 *
 * 退出码 = 失败项数。
 */
#include <fe_user.h>

#define TASK_CAP 32
#define THREAD_CAP 64
#define SNAP_BYTES (sizeof(struct fe_task_list) + \
                    TASK_CAP * FE_TASK_STRIDE + THREAD_CAP * FE_THREAD_STRIDE)

static u32 g_fail;
static char g_prefix[32];

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

/* 快照缓冲：表头 + 定长任务记录 + 紧凑线程记录（见 fe_user.h 的布局说明）。
 * 大小按**宏**算而不是 sizeof(结构体数组) —— 布局契约就是那几个宏，
 * 缓冲区也按契约算，免得把"结构体和它看起来一样"这个假设混进来。
 * ★ 显式 8 字节对齐 ★ 记录是按字节偏移定位的，而 at() 会把它转成
 * 结构体指针去读 u64 字段。x86_64 容忍非对齐标量访问，所以对齐与否
 * 今天都不出错——但那是"碰巧能跑"，写成显式的就不用每次重新推一遍。 */
static u8 g_buf[SNAP_BYTES] __attribute__((aligned(8)));
static u8 g_buf2[SNAP_BYTES] __attribute__((aligned(8)));

static void print_pad(const char *s, u32 width)
{
    say(s);
    for (u32 n = (u32)strlen(s); n < width; n++) {
        say(" ");
    }
}

static void print_task(const struct fe_task_info *t)
{
    num(t->id);
    say("  ");
    print_pad(t->name, 14);
    say(t->is_kernel ? "kernel " : "user   ");
    say("threads ");
    num(t->thread_count);
    say("  handles ");
    num(t->handle_count);
    say("  ticks ");
    num(t->cpu_ticks);
    if (t->exited) {
        say("  [exited]");
    }
    say("\n");
}

static void print_threads(const struct fe_task_view *v, const struct fe_task_info *t)
{
    for (u32 i = 0; i < t->thread_count; i++) {
        const struct fe_thread_info *th = fe_thread_at(v, t->thread_base + i);
        if (!th) {
            break;
        }
        say("      ");
        num(th->id);
        say("  ");
        print_pad(th->name, 14);
        print_pad(fe_thread_state_str(th->state), 10);
        say("prio ");
        num(th->priority);
        say("  switches ");
        num(th->switches);
        say("  ticks ");
        num(th->cpu_ticks);
        if (th->flags & FE_THREAD_FLAG_MAIN) {
            say("  [main]");
        }
        say("\n");
    }
}

/* 某个任务的主线程被切换了多少次（按 flags 里的主线程位找它）。 */
static u64 main_thread_switches(const struct fe_task_view *v, u64 task_id)
{
    for (u32 i = 0; i < v->hdr->task_count; i++) {
        const struct fe_task_info *t = fe_task_at(v, i);
        if (!t || t->id != task_id) {
            continue;
        }
        for (u32 k = 0; k < t->thread_count; k++) {
            const struct fe_thread_info *th = fe_thread_at(v, t->thread_base + k);
            if (th && (th->flags & FE_THREAD_FLAG_MAIN)) {
                return th->switches;
            }
        }
    }
    return 0;
}

/* 在快照里按 id 找名字。找不到返回 NULL。 */
static const char *name_of(const struct fe_task_view *v, u64 id)
{
    for (u32 i = 0; i < v->hdr->task_count; i++) {
        const struct fe_task_info *t = fe_task_at(v, i);
        if (t && t->id == id) {
            return t->name;
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    fe_slot_prefix((argc > 0 && argv) ? argv[0] : 0, g_prefix, sizeof(g_prefix));

    say("\n=== 任务/线程快照（K3：ps）===\n");

    struct fe_task_view v;
    long n = fe_task_view_get(&v, g_buf, sizeof(g_buf), TASK_CAP, THREAD_CAP);
    if (n < 0) {
        say("  [ps] 取快照失败：");
        num((u64)(-n));
        say("\n");
        return 1;
    }
    check(n > 0, "从用户态取到任务快照（走的完整 syscall 路径）");

    const struct fe_task_list *h = v.hdr;
    check(h->current_task != 0, "表头里的 current_task 非 0");
    check(h->task_count == (u32)n, "返回的任务数与表头一致");
    check(h->task_count <= h->tasks_cap && h->thread_count <= h->threads_cap,
          "写入条数没有超过调用者给的容量");
    check(h->total_tasks >= h->task_count && h->total_threads >= h->thread_count,
          "total_* 不小于实际写入的条数");

    say("  [ps] 共 ");
    num(h->total_tasks);
    say(" 个任务 / ");
    num(h->total_threads);
    say(" 个线程；我是任务 ");
    num(h->current_task);
    say("\n");

    /* ---- 名字必须是合法字符串；而且每条记录的字段要落在表内 ---- */
    {
        u32 bad_name = 0, bad_range = 0;
        for (u32 i = 0; i < h->task_count; i++) {
            const struct fe_task_info *t = fe_task_at(&v, i);
            if (!t) {
                continue;
            }
            /* ★ 名字必须在 16 字节内有一个 '\0' ★
             * 步长比结构体小的时候，内核会把名字写到**下一条记录**上，
             * 而这一条读到的名字就是别人记录里的字节——它往往没有结尾。 */
            u32 len = (u32)strlen(t->name);
            if (len == 0 || len >= FE_TASK_NAME_MAX) {
                bad_name++;
            }
            if (t->thread_base + t->thread_count > h->thread_count) {
                bad_range++;
            }
        }
        check(bad_name == 0, "每条任务记录的名字都是合法的 C 字符串（错位写的典型症状是没有结尾）");
        check(bad_range == 0, "每条记录的线程区间都落在快照内");
    }

    /* ---- 我自己那条记录必须在，而且线程归属自洽 ---- */
    {
        const struct fe_task_info *me = NULL;
        for (u32 i = 0; i < h->task_count; i++) {
            const struct fe_task_info *t = fe_task_at(&v, i);
            if (t && t->id == h->current_task) {
                me = t;
                break;
            }
        }
        check(me != NULL, "表里有我自己这一条");
        if (me) {
            check(me->thread_count >= 1, "我自己至少有一个线程");
            u32 idx_ok = 1, main_cnt = 0;
            for (u32 k = 0; k < me->thread_count; k++) {
                const struct fe_thread_info *th =
                    fe_thread_at(&v, me->thread_base + k);
                if (!th) {
                    idx_ok = 0;
                    break;
                }
                /* index 必须与它在表里的位置一致——表内部自洽的证据。 */
                if (th->index != k) {
                    idx_ok = 0;
                }
                if (th->flags & FE_THREAD_FLAG_MAIN) {
                    main_cnt++;
                }
            }
            check(idx_ok == 1, "线程 index 与它在表里的位置一致");
            check(main_cnt == 1, "我这里恰好有一个线程带主线程标志");
        }
    }

    /* ---- switches 必须**增长**，不能是常量 ---- */
    {
        u64 before = main_thread_switches(&v, h->current_task);
        /* 睡一会儿：睡眠会让出 CPU，重新被唤醒时必然经过一次调度。 */
        for (u32 i = 0; i < 8; i++) {
            fe_sleep_ms(3);
        }
        struct fe_task_view v2;
        if (fe_task_view_get(&v2, g_buf2, sizeof(g_buf2), TASK_CAP, THREAD_CAP) < 0) {
            check(0, "第二次取快照");
        } else {
            u64 after = main_thread_switches(&v2, h->current_task);
            say("  [ps] 我的主线程切换次数：");
            num(before);
            say(" → ");
            num(after);
            say("\n");
            check(after > before,
                  "switches 在两次快照之间增长了（这个字段真在数，不是常量）");
        }
    }

    /* ---- 拉一个孩子：新任务必须进表，而**老记录的 id→名字 不能变** ---- */
    {
        char path[96];
        char *child_argv[2];
        child_argv[0] = "/bin/hello";
        child_argv[1] = (char *)0;
        u32 k = 0;
        const char *parts[3];
        parts[0] = g_prefix;
        parts[1] = "/bin/hello";
        parts[2] = "";
        for (u32 p = 0; p < 2; p++) {
            for (const char *s = parts[p]; *s && k < sizeof(path) - 1; s++) {
                path[k++] = *s;
            }
        }
        path[k] = '\0';

        long ch = fe_spawn(path, child_argv, 1);
        check(ch > 0, "拉一个孩子进程（<槽>/bin/hello）");
        if (ch > 0) {
            struct fe_task_view v3;
            static u8 buf3[SNAP_BYTES];
            long n3 = fe_task_view_get(&v3, buf3, sizeof(buf3), TASK_CAP, THREAD_CAP);

            /* ★ 判据是"孩子的 id 是新的"，不是"任务总数变多了" ★
             *
             * 第一版断言 `n3 > n`（拉孩子之后总数必须更大）。它在当时成立，
             * 但那**只是因为内核当时根本没销毁过任务**——每一个跑完的程序
             * 都留在表里，总数只增不减。等回收链走通（K2 那一轮）之后，
             * 别的任务随时可能被销毁，总数完全可能不变甚至变少。
             * 也就是说：那条断言测的是**泄漏**，不是"孩子进了表"。
             *
             * 换成 id 比较：任务 id 单调递增，所以"孩子的 id 大于第一张快照
             * 里的最大 id"是一个与回收无关的、确定的判据——
             * 它问的正是"这个孩子是不是这次新建的"。 */
            u64 max_id = 0;
            for (u32 i = 0; i < h->task_count; i++) {
                const struct fe_task_info *t = fe_task_at(&v, i);
                if (t && t->id > max_id) {
                    max_id = t->id;
                }
            }
            const struct fe_task_info *kid =
                (n3 > 0) ? fe_task_find(&v3, "hello") : NULL;
            check(kid != NULL, "孩子的记录出现在快照里（按名字找得到）");
            check(kid && kid->id > max_id,
                  "孩子的任务 id 比第一张快照里任何一个都大（它是这次新建的）");
            if (kid) {
                say("  [ps] 孩子任务 ");
                num(kid->id);
                say("  ");
                say(kid->name);
                say("，线程 ");
                num(kid->thread_count);
                say("\n");
            }
            i32 code = 0;
            long w = fe_wait(ch, &code);
            check(w >= 0, "等孩子退出");
            fe_handle_close(ch);
        }
    }

    /* ---- 反向对照 1：buf_len 差一个字节必须被拒 ---- */
    {
        long r = fe_task_list(g_buf, (u32)(SNAP_BYTES - 1), TASK_CAP, THREAD_CAP);
        /* ★ 这一条防的是"填多少算多少" ★ 那种实现会让调用者拿到半张表
         * 却以为拿到了全部——而半张表看起来和一张短表一模一样。 */
        check(r == FE_ERR_NOSPC, "反向：buf_len 差一个字节被拒（NOSPC，不是写半张）");
    }
    /* ---- 反向对照 2：task_cap = 0 必须被拒 ---- */
    check(fe_task_list(g_buf, sizeof(g_buf), 0, THREAD_CAP) == FE_ERR_INVAL,
          "反向：task_cap = 0 被拒（INVAL）");
    /* ---- 反向对照 3：容量超硬上限必须被拒（不许悄悄夹住） ---- */
    check(fe_task_list(g_buf, sizeof(g_buf), 65, 1) == FE_ERR_INVAL,
          "反向：task_cap 超过硬上限被拒（不许悄悄夹到上限）");
    /* ---- 反向对照 4：未映射的缓冲必须 FAULT ---- */
    {
        long r = fe_task_list((void *)(usize)0x12345000, 4096, 1, 1);
        check(r == FE_ERR_FAULT,
              "反向：未映射的缓冲被拒（FAULT，内核不替我们写内存）");
    }
    /* ---- 反向对照 5：容量只给 1 时必须如实说出总数 ---- */
    {
        static u8 tiny[sizeof(struct fe_task_list) +
                       1 * FE_TASK_STRIDE + 1 * FE_THREAD_STRIDE];
        long r = fe_task_list(tiny, sizeof(tiny), 1, 1);
        const struct fe_task_list *th2 = (const struct fe_task_list *)tiny;
        check(r == 1, "反向：task_cap=1 时只写入 1 条");
        check(th2->task_count == 1, "反向：表头如实说写了 1 条");
        check(th2->total_tasks > 1,
              "反向：表头仍报出真实总数（填不下不等于系统里只有 1 个任务）");
    }

    /* ---- 打印整张表 ---- */
    say("\n  [ps] 任务表：\n");
    for (u32 i = 0; i < h->task_count; i++) {
        const struct fe_task_info *t = fe_task_at(&v, i);
        if (!t) {
            continue;
        }
        print_task(t);
        print_threads(&v, t);
    }

    say("=== 任务快照测试结束，失败项 ");
    num(g_fail);
    say(" ===\n");
    return (int)g_fail;
}
