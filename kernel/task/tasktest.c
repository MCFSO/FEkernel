/* SPDX-License-Identifier: 0BSD */
/* 任务/线程快照（K3）的内核自检。
 *
 * ★ 这一组自检为什么必须存在 ★
 * 快照的内核侧在 D4 那一轮就"写完"了，然后一直**没有任何调用者**，
 * 也没有任何自检。它当时带着一个真实错误：两个步长宏都是 48，
 * 而两个记录结构体都是 64 字节。内核于是把 `name` 写到**下一条记录的
 * 头上**，用户态按 48 步长读到的 `name` 是别人的字节。
 * 构建通过、启动通过、整机自检全部通过——因为没有东西跑过它。
 *
 * 所以这一组的第一条断言不是"表填得对"，而是**"表和内核里的事实对得上"**：
 * 拿快照里的每一条记录，回到内核的对象上去核对。这样即使将来偏移或步长
 * 又被改错，错的也是"和事实不一致"，而不是"看起来还挺整齐"。
 *
 * ★ 反向对照在哪 ★
 *   - buf 小一个字节 → 必须被拒（而不是写半张表）；
 *   - task_cap = 0  → 必须被拒；
 *   - task_cap = 1  → task_count 必须是 1，而 total_tasks 必须仍报**真实总数**
 *     （这一条专治"填不下就假装只有这么多"）；
 *   - 每条记录的 name 必须以 '\0' 结束且与内核里的名字相等
 *     （专治"name 写到下一条记录头上"这类越界写——它表现成**别人**的名字变了）。
 */
#include <fe/task.h>
#include <fe/sched/thread.h>
#include <fe/syscall.h>
#include <fe/kprintf.h>
#include <fe/string.h>
#include <fe/object.h>

/* 按共享 ABI 的偏移常量读回一条记录。★ 故意不复用内核结构体 ★
 * 这个自检要验的就是"用户态按这些偏移能读到什么"，所以它必须站在
 * 用户态那一侧读——用内核结构体去读等于把要验的东西假设成对的。 */
static u64 rd_u64(const u8 *p) { u64 v; memcpy(&v, p, 8); return v; }
static u32 rd_u32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static i32 rd_i32(const u8 *p) { i32 v; memcpy(&v, p, 4); return v; }

static const u8 *task_rec(const u8 *buf, u32 i)
{
    return buf + FE_TASK_LIST_HDR_X + (u64)i * FE_TASK_STRIDE_X;
}

static const u8 *thread_rec(const u8 *buf, u32 task_cap, u32 i)
{
    return buf + FE_TASK_LIST_HDR_X + (u64)task_cap * FE_TASK_STRIDE_X +
           (u64)i * FE_THREAD_STRIDE_X;
}

static int name_eq(const char *a, const u8 *rec48)
{
    for (u32 i = 0; i < FE_TASK_NAME_MAX_X; i++) {
        char c = (char)rec48[i];
        if (c != a[i]) {
            return 0;
        }
        if (c == '\0') {
            return 1;
        }
    }
    return 1;       /* 16 个字符都相同（名字满格）也算相同 */
}

#define CHECK(cond) do { if (!(cond)) { fail++; } } while (0)

u32 fe_selftest_tasklist(void)
{
    u32 fail = 0;

    /* 造一个名字独一无二的内核任务，好在快照里认出它来。
     * 名字必须唯一：多个内核线程都叫 "user"，靠名字认人是不行的。 */
    static const char *probe_name = "k3-probe";
    struct fe_task *probe = fe_task_create_kernel(probe_name);
    if (!probe) {
        fe_kprintf("        探针任务创建失败\n");
        return 1;
    }

    enum { TASK_CAP = 32, THREAD_CAP = 32 };
    static u8 buf[FE_TASK_LIST_HDR_X +
                  TASK_CAP * FE_TASK_STRIDE_X +
                  THREAD_CAP * FE_THREAD_STRIDE_X];

    u64 me = fe_task_current() ? fe_task_current()->id : 0;
    i64 n = fe_task_snapshot_build(buf, sizeof(buf), TASK_CAP, THREAD_CAP, me);
    if (n <= 0) {
        fe_kprintf("        快照构建失败（返回 %lld）\n", (long long)n);
        fe_object_unref(&probe->hdr);
        return 1;
    }

    u32 task_count = rd_u32(buf + 0);
    u32 thread_count = rd_u32(buf + 4);
    u64 total_tasks = rd_u64(buf + 8);
    u64 total_threads = rd_u64(buf + 16);
    u64 current_task = rd_u64(buf + 24);
    u32 tasks_cap = rd_u32(buf + 32);
    u32 threads_cap = rd_u32(buf + 36);

    CHECK((i64)task_count == n);
    CHECK(task_count <= tasks_cap);
    CHECK(thread_count <= threads_cap);
    CHECK(tasks_cap == TASK_CAP && threads_cap == THREAD_CAP);
    /* current_task 必须是我自己——它是"这张表是谁看到的"那个字段，
     * 填错的话 `ps` 就标不出哪一行是自己。 */
    CHECK(current_task == me);

    /* 把内核里真实的任务数数一遍，和 total_tasks 对比。
     * ★ 这一条与"task_count <= cap"合起来才有意义 ★
     * 只查"没超过容量"永远成立；要能发现"漏报了任务"，必须与事实对照。 */
    u64 real_tasks = 0, real_threads = 0;
    for (struct fe_task *t = fe_task_first(); t; t = t->next) {
        real_tasks++;
        real_threads += fe_task_thread_count(t);
    }
    CHECK(total_tasks == real_tasks);
    CHECK(total_threads == real_threads);

    /* ---- 逐条记录回到内核事实上去核对 ---- */
    u32 found_probe = 0, found_me = 0, bad_name = 0, bad_threads = 0;
    for (u32 i = 0; i < task_count; i++) {
        const u8 *r = task_rec(buf, i);
        u64 id = rd_u64(r + 0);
        u32 tc = rd_u32(r + 16);
        u32 tb = rd_u32(r + 20);
        struct fe_task *t = fe_task_by_id(id);
        if (!t) {
            fe_kprintf("        记录 %u 的 id %llu 在内核里不存在\n",
                       i, (unsigned long long)id);
            fail++;
            continue;
        }
        /* 名字必须与内核里的那个任务一致。★ 这一条是步长错位的探针 ★
         * 步长写小的时候，前一条的 name 会盖住这一条的 id/parent_id，
         * 而这一条的 name 又是下一条的字节——两个方向都会在这里露出来。 */
        if (!name_eq(t->name, r + 48)) {
            fe_kprintf("        记录 %u（id %llu）的名字不符：快照里是 \"",
                       i, (unsigned long long)id);
            for (u32 k = 0; k < FE_TASK_NAME_MAX_X && r[48 + k]; k++) {
                fe_kprintf("%c", r[48 + k]);
            }
            fe_kprintf("\"，内核里是 \"%s\"\n", t->name);
            bad_name++;
        }
        CHECK(tc == fe_task_thread_count(t));
        CHECK(tc == 0 || tb + tc <= thread_count);

        /* 线程记录：逐个与内核里的线程核对 */
        u32 seen = 0;
        for (struct fe_thread *th = fe_task_thread_first(t); th;
             th = fe_thread_next_of(th)) {
            if (tb + seen >= thread_count) {
                break;
            }
            const u8 *tr = thread_rec(buf, TASK_CAP, tb + seen);
            if (rd_u64(tr + 0) != th->id) {
                bad_threads++;
                break;
            }
            CHECK(rd_u32(tr + 40) == seen);                 /* index 是稳定序号 */
            CHECK(rd_u32(tr + 28) == th->state);            /* state */
            CHECK(rd_u32(tr + 24) == th->priority);
            /* flags 里的主线程位必须与内核一致 */
            u32 flags = rd_u32(tr + 44);
            bool is_main = (th == t->main_thread);
            CHECK(((flags & FE_THREAD_FLAG_MAIN) != 0) == is_main);
            CHECK(name_eq(th->name, tr + 48));
            seen++;
        }
        CHECK(seen == tc);
        if (id == probe->id) {
            found_probe = 1;
        }
        if (id == me) {
            found_me = 1;
        }
    }
    CHECK(found_probe);
    CHECK(found_me);
    CHECK(bad_name == 0);
    CHECK(bad_threads == 0);

    if (bad_name || bad_threads) {
        fe_kprintf("        名字不符 %u 条，线程记录错位 %u 条\n", bad_name, bad_threads);
    }

    /* ---- switches 必须是真实计数，而不能是常量 ---- */
    {
        /* 调用者自己这个线程显然已经被调度过（它正在跑）。
         * ★ 这一条专治"字段写死 0" ★ 写 0 的系统与"刚创建的线程"
         * 从外部看完全一样，所以必须有一个非零的样本。 */
        u32 mine = 0;
        u64 my_switches = 0;
        for (u32 i = 0; i < task_count; i++) {
            const u8 *r = task_rec(buf, i);
            if (rd_u64(r + 0) != me) {
                continue;
            }
            if (rd_u32(r + 16) > 0) {
                mine = rd_u32(r + 20);
                my_switches = rd_u64(thread_rec(buf, TASK_CAP, mine) + 16);
            }
            break;
        }
        if (my_switches == 0) {
            fe_kprintf("        当前线程的 switches 是 0 —— 这个字段没有在填\n");
            fail++;
        } else {
            fe_kprintf("        快照 %u 个任务 / %u 个线程（真实 %llu / %llu），"
                       "当前线程 switches = %llu\n",
                       task_count, thread_count,
                       (unsigned long long)total_tasks,
                       (unsigned long long)total_threads,
                       (unsigned long long)my_switches);
        }
    }

    /* ---- 反向对照 1：缓冲小一个字节必须被拒 ---- */
    {
        i64 r = fe_task_snapshot_build(buf, sizeof(buf) - 1, TASK_CAP, THREAD_CAP, me);
        /* ★ 这一条不是"锦上添花" ★ 一个"能填多少填多少"的实现
         * 会让调用者拿到半张表却以为拿到了全部，而那正是本组自检
         * 最后一条要防的同一个错误。 */
        CHECK(r == FE_ERR_NOSPC);
    }
    /* ---- 反向对照 2：容量为 0 必须被拒 ---- */
    CHECK(fe_task_snapshot_build(buf, sizeof(buf), 0, THREAD_CAP, me) == FE_ERR_INVAL);
    /* ---- 反向对照 3：超过硬上限必须被拒（不许悄悄夹到上限） ---- */
    CHECK(fe_task_snapshot_build(buf, sizeof(buf), FE_TASK_LIST_MAX + 1, 1, me)
          == FE_ERR_INVAL);
    /* ---- 反向对照 4：NULL 缓冲必须被拒 ---- */
    CHECK(fe_task_snapshot_build(NULL, sizeof(buf), 1, 1, me) == FE_ERR_INVAL);

    /* ---- 反向对照 5：容量只给 1 时，必须如实说出"总数不止这么多" ---- */
    {
        static u8 small[FE_TASK_LIST_HDR_X + 1 * FE_TASK_STRIDE_X + 1 * FE_THREAD_STRIDE_X];
        i64 r = fe_task_snapshot_build(small, sizeof(small), 1, 1, me);
        CHECK(r == 1);
        u32 c = rd_u32(small + 0);
        u64 tot = rd_u64(small + 8);
        CHECK(c == 1);
        /* 关键：total_tasks 是真实总数，不是"我写了 1 条所以是 1"。
         * 系统里此刻至少有内核任务、调用者任务和探针任务，所以必然 > 1。 */
        CHECK(tot == real_tasks && tot > 1);
        if (tot <= 1) {
            fe_kprintf("        截断时没有如实报出总数（报了 %llu）\n",
                       (unsigned long long)tot);
        }
    }

    fe_object_unref(&probe->hdr);
    return fail;
}
