/* SPDX-License-Identifier: 0BSD */
/* K11「等一个用户地址」的内核自检（docs/21-user-address-wait.md §7.1）。
 *
 * ★ 这一组**证明不了**什么（写在最前面，否则实现时会走捷径）★
 * 自检在**引导线程**里跑：它造探针任务/探针线程，直接调机制函数。
 * 所以它证明的是"等待/唤醒这一整套**决策**对不对"，
 * 而**证明不了**"从 `pthread_cond_wait` 的语义到这条机制"那一段——
 * 那一段只有 `user/bin/waitaddrtest` 那个真实客户端能证。
 * 这条分工与 K5（`kernel/task/faulttest.c`）完全同类。
 *
 * ★ 每条正向都配一条会失败的反向 ★
 * 反向的意思不是"断言反过来"，而是"**同一件事在不该成立的条件下必须不成立**"
 * ——没有它，"等对了"与"根本没在等"分不开。
 *
 * ★ 判据尽量用**事实**，不用墙钟 ★
 *   - "它睡过没有"用 `thread->switches`（被调度上 CPU 的次数）：
 *     没睡过就是 1（只被调度过一次），睡过再被叫醒就 ≥ 2；
 *   - "链上还有几个人"用 `fe_wait_addr_waiters()`（只读访问器）。
 */
#include <fe/ipc.h>
#include <fe/task.h>
#include <fe/object.h>
#include <fe/process.h>
#include <fe/sched/sched.h>
#include <fe/sched/thread.h>
#include <fe/time.h>
#include <fe/kprintf.h>
#include <fe/string.h>
#include <fe/mm/vmm.h>
#include <fe/mm/pmm.h>
#include <fe/user.h>

#define CHECK(cond) do { if (!(cond)) { fail++; } } while (0)

/* ★ 自检的等待必须用**绝对时间上界**，不能用"让出轮数" ★
 * 理由与实测见 kernel/task/killtest.c:206-216：纯让出循环在"只有 idle 可跑"
 * 时会被饿（`main` 每 100 个节拍只拿到 10 个），于是"跑不完"会被伪装成"挂死"。
 * 轮数在"被饿"的情况下不是时间；**时间才是时间**。 */
static int wait_done_ms(volatile u32 *flag, u32 ms)
{
    u64 deadline = fe_time_ms() + ms;
    for (;;) {
        if (*flag) {
            return 1;
        }
        if (fe_time_ms() >= deadline) {
            return 0;
        }
        fe_thread_sleep_ms(1);
    }
}

/* ================================================================== */
/* 探针装置：一个**真的**用户任务 + 一页真的映射可写的用户页           */
/* ================================================================== */

/* ★ 为什么必须有真的用户地址空间 ★
 * `fe_wait_addr` 读的是**当前任务的用户内存**（`fe_user_range_ok` +
 * 一次 `memcpy`）。引导线程属于内核任务（`space == NULL`），直接调它
 * 只会得到 FAULT——那验不到"读值"这条路上的任何东西。
 *
 * ★ 为什么不用切 CR3（K5 的 faulttest 那样）★
 * 同一页物理帧在内核里有 HHDM 别名，而 HHDM 在每个地址空间里都映射着。
 * 于是自检读写"用户内存"直接走
 * `frame + fe_vmm_hhdm_offset()` 就行，**一次 CR3 都不用切**——
 * 少一处"切了忘了切回来"的风险。 */
#define PROBE_VA 0x33000000ull          /* 探针页的用户虚拟地址（各探针任务共用）*/
#define PROBE_VA2 (PROBE_VA + 4)        /* 同一页里的**另一个**字（W3 用）*/
#define PROBE_HOLE (PROBE_VA + 0x100000ull)  /* 没映射的邻居（W5 用）*/

struct probe {
    struct fe_task *task;
    phys_addr_t frame;
    volatile u32 *kval;     /* 这一页的内核别名（HHDM）*/
};

/* 写/读探针页里的 u32（两个偏移都在同一页里）。 */
static void probe_store(struct probe *p, u64 off, u32 v)
{
    *(volatile u32 *)(void *)(uptr)((u8 *)p->kval + off) = v;
}

static u32 probe_load(struct probe *p, u64 off)
{
    return *(volatile u32 *)(void *)(uptr)((u8 *)p->kval + off);
}

static bool probe_new(struct probe *p, const char *name)
{
    memset(p, 0, sizeof(*p));
    p->task = fe_task_create_user(name);
    if (!p->task) {
        return false;
    }
    p->frame = fe_pmm_alloc_frame();
    if (p->frame == 0) {
        fe_object_unref(&p->task->hdr);
        p->task = NULL;
        return false;
    }
    p->kval = (volatile u32 *)(void *)(uptr)(p->frame + fe_vmm_hhdm_offset());
    memset((void *)(uptr)p->kval, 0, FE_FRAME_SIZE);
    if (fe_failed(fe_vmm_map(p->task->space, PROBE_VA, p->frame, FE_FRAME_SIZE,
                             FE_PTE_USER | FE_PTE_WRITE | FE_PTE_NX))) {
        fe_pmm_free_frame(p->frame);
        fe_object_unref(&p->task->hdr);
        p->task = NULL;
        return false;
    }
    return true;
}

static void probe_free(struct probe *p)
{
    if (!p->task) {
        return;
    }
    fe_vmm_unmap(p->task->space, PROBE_VA, FE_FRAME_SIZE);
    fe_pmm_free_frame(p->frame);
    fe_object_unref(&p->task->hdr);
    p->task = NULL;
}

/* 在探针任务里造一条线程。
 * ★ 为什么要临时换当前线程的 task ★ `fe_thread_create` 把新线程的 task
 * 取成"当前任务"的（与 killtest 的 spawn_probe 是同一个做法）。 */
static struct fe_thread *spawn_probe(struct probe *p, const char *name,
                                     fe_thread_entry_t entry, void *arg)
{
    struct fe_thread *cur = fe_thread_current();
    struct fe_task *saved = cur ? cur->task : NULL;
    if (cur) {
        cur->task = p->task;
    }
    struct fe_thread *th = fe_thread_create(name, entry, arg, 16 * 1024,
                                            FE_PRIO_NORMAL);
    if (cur) {
        cur->task = saved;
    }
    return th;
}

/* ================================================================== */
/* W2：★ 丢唤醒窗口 ★（docs/21 §3.1 审计出来的那个窗口）              */
/* ================================================================== */

/* ★ 这一条要验的是什么 ★
 *
 * `fe_wait_any` 的循环里，"检查一遍有没有已经就绪的"与"登记上去"原本被
 * 一个**开着中断**的窗口隔开。落在这个窗口里的"条件变真 + 唤醒"会丢：
 * 唤醒方那一刻看不到任何等待者（我们还没登记），而我们随后照样睡下。
 * 症状是**永久睡死**——不是"慢"。
 *
 * ★ 修之前的原文（`build/k11-w2-red.txt`）★
 *     W2：★ 丢唤醒窗口 ★ **漏醒**——条件在检查与登记之间被置真，
 *     探针照样睡下，没人叫它（红）
 *     => 等待用户地址失败项: 1
 *     （总判据行）自检存在 1 项失败
 *
 * ★ 怎么把只有几条指令宽的窗口变成可复现的 ★
 * 用一支只给自检用的钩子（`fe_wait_any_set_hook`），它被钉在
 * "原子区间之前的那一刻"。钩子当场做两件事：**把条件置真**、**调一次唤醒**。
 *
 * ★ 两个键各判一次 ★ 通知对象（窗口与键无关，而它在今天的代码上就有，
 * 所以红测试不必等新键落地）与用户地址（K11 自己的那条路）。
 */

static struct fe_notification *g_w2_nt;
static volatile u32 g_w2_bits_done;
static volatile i32 g_w2_bits_status;
static struct probe *g_w2_probe;
static volatile u32 g_w2_addr_done;
static volatile i32 g_w2_addr_status;
static volatile u32 g_w2_hook_hits;

/* 探针 A：在通知对象上等一个**没人置位**的位；钩子会在窗口里替"别人"置上它。 */
static void w2_probe_bits(void *arg)
{
    struct fe_task *t = fe_task_current();
    struct fe_wait_target tg;
    tg.handle = (fe_handle_t)(u64)(uptr)arg;
    tg.kind = FE_WAIT_BITS;
    tg.mask = 0x2;
    struct fe_wait_result res;

    fe_status_t s = fe_wait_any(t, &tg, 1, &res, 0);
    g_w2_bits_status = s;
    g_w2_bits_done = 1;
    fe_thread_exit(0);
}

/* 探针 B：在用户地址上等 0x1111；钩子会在窗口里把它改成 0x2222 并唤醒。 */
static void w2_probe_addr(void *arg)
{
    struct fe_task *t = fe_task_current();
    (void)arg;
    fe_status_t s = fe_wait_addr(t, PROBE_VA, 0x1111u, 0);
    g_w2_addr_status = s;
    g_w2_addr_done = 1;
    fe_thread_exit(0);
}

/* 钩子：**在窗口里**把条件置真并尝试唤醒。只注入一次。 */
static void w2_hook(struct fe_task *t, const struct fe_wait_target *tg, u32 n, void *ctx)
{
    (void)ctx;
    if (g_w2_hook_hits++ != 0) {
        return;
    }
    for (u32 i = 0; i < n; i++) {
        if (tg[i].kind == FE_WAIT_BITS) {
            /* 等价于"另一个执行体在窗口里把这个通知置位了"。 */
            fe_notification_signal_obj(g_w2_nt, tg[i].mask);
        } else if (tg[i].kind == FE_WAIT_ADDR) {
            /* 等价于"另一个执行体在窗口里把那个字改了，然后 wake 一次"。
             * 此刻链上还没有我们，所以这次 wake 唤醒 0 个——正是丢唤醒的形状。 */
            probe_store(g_w2_probe, 0, 0x2222u);
            (void)fe_wake_addr(t->space, tg[i].addr, 0);
        }
    }
}

/* 半场 A：通知对象。返回失败项数。 */
static u32 w2_bits(bool *out_ok)
{
    u32 fail = 0;
    *out_ok = false;
    struct fe_task *task = fe_task_create_kernel("w2-win");
    if (!task) {
        fe_kprintf("        W2(通知) 探针任务创建失败\n");
        return 1;
    }
    fe_handle_t h = FE_HANDLE_INVALID;
    struct fe_object_header *obj = NULL;
    if (fe_failed(fe_notification_create(task, &h)) ||
        fe_failed(fe_handle_lookup(&task->handles, h, 0, &obj)) ||
        obj->type != FE_OBJ_NOTIFICATION) {
        fe_kprintf("        W2(通知) 通知对象创建/查询失败\n");
        fe_object_unref(&task->hdr);
        return 1;
    }
    g_w2_nt = FE_OBJ_OF(obj, struct fe_notification);
    g_w2_bits_done = 0;
    g_w2_bits_status = 0;
    g_w2_hook_hits = 0;
    fe_wait_any_set_hook(w2_hook, NULL);

    struct fe_thread *cur = fe_thread_current();
    struct fe_task *saved = cur ? cur->task : NULL;
    if (cur) {
        cur->task = task;
    }
    struct fe_thread *th = fe_thread_create("w2-notif", w2_probe_bits,
                                            (void *)(uptr)h, 16 * 1024,
                                            FE_PRIO_NORMAL);
    if (cur) {
        cur->task = saved;
    }
    if (!th) {
        fe_kprintf("        W2(通知) 探针线程创建失败\n");
        fe_wait_any_set_hook(NULL, NULL);
        fe_object_unref(&task->hdr);
        return 1;
    }
    bool ret = wait_done_ms(&g_w2_bits_done, 300) != 0;
    CHECK(ret);
    if (ret) {
        CHECK(g_w2_bits_status == FE_OK);
    }
    fe_wait_any_set_hook(NULL, NULL);
    fe_task_terminate(task);
    u32 guard = 0;
    while (!g_w2_bits_done && guard++ < 2000) {
        fe_thread_sleep_ms(1);
    }
    if (!g_w2_bits_done) {
        fe_kprintf("        W2(通知) 收尾失败：探针被终止之后仍未返回\n");
        fail++;
    }
    fe_object_unref(&task->hdr);
    *out_ok = (fail == 0 && ret);
    return fail;
}

/* 半场 B：用户地址。★ 这一半是判据的核心 ★ ——
 * "值在检查与登记之间被改" 之后，等待者**一次都不许睡**。
 * 判据用 `switches`（被调度上 CPU 的次数）：没睡过就是 1。 */
static u32 w2_addr(bool *out_ok)
{
    u32 fail = 0;
    *out_ok = false;
    struct probe p;
    if (!probe_new(&p, "w2-addr")) {
        fe_kprintf("        W2(地址) 探针任务创建失败\n");
        return 1;
    }
    probe_store(&p, 0, 0x1111u);
    g_w2_probe = &p;
    g_w2_addr_done = 0;
    g_w2_addr_status = 0;
    g_w2_hook_hits = 0;
    fe_wait_any_set_hook(w2_hook, NULL);

    struct fe_thread *th = spawn_probe(&p, "w2-aprobe", w2_probe_addr, NULL);
    if (!th) {
        fe_kprintf("        W2(地址) 探针线程创建失败\n");
        fe_wait_any_set_hook(NULL, NULL);
        probe_free(&p);
        return 1;
    }
    bool ret = wait_done_ms(&g_w2_addr_done, 300) != 0;
    CHECK(ret);
    if (ret) {
        /* ★ 值在"检查与登记之间"被改掉 ⇒ 进门那次检查就看见新值 ⇒ EAGAIN ★
         * （按 futex 语义：你给的期望值已经不对了，**我没睡**。）
         * `switches == 1` 才是这一条的**核心判据**：它一次都没睡过。
         * 漏醒的实现里它会睡下并被叫醒（≥2），或者干脆睡死（永远不返回）。 */
        CHECK(g_w2_addr_status == FE_ERR_AGAIN);
        CHECK(th->switches == 1);
    }
    fe_wait_any_set_hook(NULL, NULL);
    fe_object_unref(&p.task->hdr);   /* 连同它的线程一起收掉 */
    p.task = NULL;
    *out_ok = (fail == 0 && ret);
    return fail;
}

/* ================================================================== */
/* W1：值没变就睡、值变了就醒（含"一开始就不等于期望"的反向）          */
/* ================================================================== */

static volatile u32 g_w1_done;
static volatile i32 g_w1_status;
static u64 g_w1_expect;

static void w1_probe(void *arg)
{
    struct fe_task *t = fe_task_current();
    (void)arg;
    fe_status_t s = fe_wait_addr(t, PROBE_VA, g_w1_expect, 0);
    g_w1_status = s;
    g_w1_done = 1;
    fe_thread_exit(0);
}

/* `expect_ok`：值一开始就等于期望（要睡）还是不等（不许睡）。 */
static u32 w1_case(bool sleep_case, bool *out_ok)
{
    u32 fail = 0;
    *out_ok = false;
    struct probe p;
    if (!probe_new(&p, "w1-probe")) {
        fe_kprintf("        W1 探针任务创建失败\n");
        return 1;
    }
    probe_store(&p, 0, 0x1111u);
    g_w1_done = 0;
    g_w1_status = 0;
    g_w1_expect = sleep_case ? 0x1111u : 0x9999u;

    struct fe_thread *th = spawn_probe(&p, "w1-th", w1_probe, NULL);
    if (!th) {
        fe_kprintf("        W1 探针线程创建失败\n");
        probe_free(&p);
        return 1;
    }
    if (sleep_case) {
        /* 等它真的睡下（链上有它）之后，改值 + 唤醒。 */
        u32 spin = 0;
        while (fe_wait_addr_waiters(p.task->space, PROBE_VA) == 0 && spin++ < 300) {
            fe_thread_sleep_ms(1);
        }
        CHECK(fe_wait_addr_waiters(p.task->space, PROBE_VA) == 1);
        CHECK(th->waiting == 1);
        CHECK(th->addr_waiting == 1);
        probe_store(&p, 0, 0x2222u);
        CHECK(fe_wake_addr(p.task->space, PROBE_VA, 0) == 1);
    }
    bool ret = wait_done_ms(&g_w1_done, 300) != 0;
    CHECK(ret);
    if (ret) {
        CHECK(g_w1_status == (sleep_case ? FE_OK : FE_ERR_AGAIN));
        if (sleep_case) {
            /* 睡过 ⇒ 至少被调度两次（睡下 + 被叫醒）*/
            CHECK(th->switches >= 2);
        } else {
            /* ★ 反向的核心：**一次都没睡** ★ */
            CHECK(th->switches == 1);
        }
    }
    /* 收尾：链上必须没有残留 */
    CHECK(fe_wait_addr_waiters(p.task->space, PROBE_VA) == 0);
    fe_object_unref(&p.task->hdr);
    p.task = NULL;
    *out_ok = (fail == 0 && ret);
    return fail;
}

/* ================================================================== */
/* W3：唤醒**别的地址**不许误伤（同一个地址空间里的另一个字）           */
/* ================================================================== */

static volatile u32 g_w3_done;
static volatile i32 g_w3_status;

static void w3_probe(void *arg)
{
    struct fe_task *t = fe_task_current();
    (void)arg;
    fe_status_t s = fe_wait_addr(t, PROBE_VA, 0x1111u, 0);
    g_w3_status = s;
    g_w3_done = 1;
    fe_thread_exit(0);
}

static u32 w3_wrong_addr(bool *out_ok)
{
    u32 fail = 0;
    *out_ok = false;
    struct probe p;
    if (!probe_new(&p, "w3-probe")) {
        fe_kprintf("        W3 探针任务创建失败\n");
        return 1;
    }
    probe_store(&p, 0, 0x1111u);
    probe_store(&p, 4, 0x1111u);        /* 邻字：**也**是 0x1111，但地址不同 */
    g_w3_done = 0;
    g_w3_status = 0;

    struct fe_thread *th = spawn_probe(&p, "w3-th", w3_probe, NULL);
    if (!th) {
        fe_kprintf("        W3 探针线程创建失败\n");
        probe_free(&p);
        return 1;
    }
    u32 spin = 0;
    while (fe_wait_addr_waiters(p.task->space, PROBE_VA) == 0 && spin++ < 300) {
        fe_thread_sleep_ms(1);
    }
    /* ★ 反向：唤醒**邻字**——值一样、地址不一样 ★ */
    fe_wake_addr(p.task->space, PROBE_VA2, 0);
    CHECK(fe_wait_addr_waiters(p.task->space, PROBE_VA) == 1);  /* 还在等 */
    CHECK(!g_w3_done);
    /* 正向：唤醒**正确的地址** ⇒ 它醒，而且只醒了一个 */
    probe_store(&p, 0, 0x2222u);
    u32 n = fe_wake_addr(p.task->space, PROBE_VA, 0);
    CHECK(n == 1);
    bool ret = wait_done_ms(&g_w3_done, 300) != 0;
    CHECK(ret);
    if (ret) {
        CHECK(g_w3_status == FE_OK);
    }
    fe_object_unref(&p.task->hdr);
    p.task = NULL;
    *out_ok = (fail == 0 && ret);
    return fail;
}

/* ================================================================== */
/* W4：★ 跨地址空间 ★ 同一个虚拟地址、两个任务，唤一个不许动另一个    */
/* ================================================================== */

static volatile u32 g_w4_done_a;
static volatile u32 g_w4_done_b;
static volatile i32 g_w4_status_a;
static volatile i32 g_w4_status_b;

static void w4_probe(void *arg)
{
    struct fe_task *t = fe_task_current();
    volatile u32 *done = (volatile u32 *)arg;
    fe_status_t s = fe_wait_addr(t, PROBE_VA, 0x1111u, 0);
    if (done == &g_w4_done_a) {
        g_w4_status_a = s;
    } else {
        g_w4_status_b = s;
    }
    *done = 1;
    fe_thread_exit(0);
}

static u32 w4_cross_space(bool *out_ok)
{
    u32 fail = 0;
    *out_ok = false;
    struct probe a, b;
    if (!probe_new(&a, "w4-a") || !probe_new(&b, "w4-b")) {
        fe_kprintf("        W4 探针任务创建失败\n");
        probe_free(&a);
        probe_free(&b);
        return 1;
    }
    /* ★ 两个任务在**同一个虚拟地址**上各放一个自己的字 ★ */
    probe_store(&a, 0, 0x1111u);
    probe_store(&b, 0, 0x1111u);
    g_w4_done_a = 0;
    g_w4_done_b = 0;
    g_w4_status_a = 0;
    g_w4_status_b = 0;

    struct fe_thread *ta = spawn_probe(&a, "w4-ta", w4_probe, (void *)&g_w4_done_a);
    struct fe_thread *tb = spawn_probe(&b, "w4-tb", w4_probe, (void *)&g_w4_done_b);
    if (!ta || !tb) {
        fe_kprintf("        W4 探针线程创建失败\n");
        fe_object_unref(&a.task->hdr);
        fe_object_unref(&b.task->hdr);
        return 1;
    }
    u32 spin = 0;
    while ((fe_wait_addr_waiters(a.task->space, PROBE_VA) == 0 ||
            fe_wait_addr_waiters(b.task->space, PROBE_VA) == 0) && spin++ < 300) {
        fe_thread_sleep_ms(1);
    }
    CHECK(fe_wait_addr_waiters(a.task->space, PROBE_VA) == 1);
    CHECK(fe_wait_addr_waiters(b.task->space, PROBE_VA) == 1);

    /* 只改 a 的值、只唤醒 a 的空间 */
    probe_store(&a, 0, 0x2222u);
    u32 n = fe_wake_addr(a.task->space, PROBE_VA, 0);
    CHECK(n == 1);                       /* ★ 只醒了一个 ★ */
    bool ra = wait_done_ms(&g_w4_done_a, 300) != 0;
    CHECK(ra);
    if (ra) {
        CHECK(g_w4_status_a == FE_OK);
    }
    /* ★ 另一个必须**一动不动**：还在等、b 的值没变 ★ */
    CHECK(!g_w4_done_b);
    CHECK(g_w4_status_b == 0);
    CHECK(fe_wait_addr_waiters(b.task->space, PROBE_VA) == 1);
    CHECK(probe_load(&b, 0) == 0x1111u);

    /* 收尾：b 也得醒（否则它永远等着）*/
    probe_store(&b, 0, 0x3333u);
    CHECK(fe_wake_addr(b.task->space, PROBE_VA, 0) == 1);
    bool rb = wait_done_ms(&g_w4_done_b, 300) != 0;
    CHECK(rb);

    fe_object_unref(&a.task->hdr);
    fe_object_unref(&b.task->hdr);
    *out_ok = (fail == 0 && ra && rb);
    return fail;
}

/* ================================================================== */
/* W5：地址读不了 ⇒ FAULT（**不是** EAGAIN）；映射之后就不是 FAULT     */
/* ================================================================== */

static volatile u32 g_w5_done;
static volatile i32 g_w5_status;
static u64 g_w5_addr;
static u64 g_w5_deadline;

static void w5_probe(void *arg)
{
    struct fe_task *t = fe_task_current();
    (void)arg;
    fe_status_t s = fe_wait_addr(t, g_w5_addr, 0x1111u, g_w5_deadline);
    g_w5_status = s;
    g_w5_done = 1;
    fe_thread_exit(0);
}

static u32 w5_unmapped(bool *out_ok)
{
    u32 fail = 0;
    *out_ok = false;
    struct probe p;
    if (!probe_new(&p, "w5-probe")) {
        fe_kprintf("        W5 探针任务创建失败\n");
        return 1;
    }
    probe_store(&p, 0, 0x1111u);

    /* ---- 正向：没映射的邻居 → FAULT ---- */
    g_w5_done = 0;
    g_w5_status = 0;
    g_w5_addr = PROBE_HOLE;
    g_w5_deadline = 0;
    if (!spawn_probe(&p, "w5-th1", w5_probe, NULL) ||
        !wait_done_ms(&g_w5_done, 300)) {
        fe_kprintf("        W5 探针没有返回\n");
        fail++;
    } else {
        CHECK(g_w5_status == FE_ERR_FAULT);
        if (g_w5_status != FE_ERR_FAULT) {
            fe_kprintf("        W5：没映射的地址返回的是 %d，期望 %d(FAULT)\n",
                       (int)g_w5_status, (int)FE_ERR_FAULT);
        }
    }
    /* ★ 反向：**同一个字、映射着的那个地址** → 不是 FAULT ★
     * 用带 deadline 的等（没人唤醒）⇒ 必须是 TIMEOUT，证明
     * "刚才那次 FAULT 是映射问题，不是这条路上永远返回 FAULT"。 */
    g_w5_done = 0;
    g_w5_status = 0;
    g_w5_addr = PROBE_VA;
    g_w5_deadline = fe_time_ns() + 20ull * 1000000ull;
    if (!spawn_probe(&p, "w5-th2", w5_probe, NULL) ||
        !wait_done_ms(&g_w5_done, 1000)) {
        fe_kprintf("        W5 反向：探针没有返回\n");
        fail++;
    } else {
        CHECK(g_w5_status == FE_ERR_TIMEOUT);
        if (g_w5_status != FE_ERR_TIMEOUT) {
            fe_kprintf("        W5 反向：映射着的地址返回的是 %d，期望 %d(TIMEOUT)\n",
                       (int)g_w5_status, (int)FE_ERR_TIMEOUT);
        }
    }
    fe_object_unref(&p.task->hdr);
    p.task = NULL;
    *out_ok = (fail == 0);
    return fail;
}

/* ================================================================== */
/* W6：容量 —— 同一个地址上 8 个等待者（> FE_WAITERS_MAX = 4）都要真的挂上 */
/* ================================================================== */

#define W6_N 8
static u32 g_w6_done[W6_N];
static volatile i32 g_w6_status[W6_N];
/* 报出来的**事实**（失败时也要看得见数字，不是只有"失败"两个字）*/
static u32 g_w6_onchain, g_w6_n1, g_w6_n2, g_w6_woke;

static void w6_probe(void *arg)
{
    struct fe_task *t = fe_task_current();
    u32 idx = (u32)(u64)(uptr)arg;
    fe_status_t s = fe_wait_addr(t, PROBE_VA, 0x1111u, 0);
    g_w6_status[idx] = s;
    g_w6_done[idx] = 1;
    fe_thread_exit(0);
}

static u32 w6_capacity(bool *out_ok)
{
    u32 fail = 0;
    *out_ok = false;
    struct probe p;
    if (!probe_new(&p, "w6-probe")) {
        fe_kprintf("        W6 探针任务创建失败\n");
        return 1;
    }
    probe_store(&p, 0, 0x1111u);
    for (u32 i = 0; i < W6_N; i++) {
        g_w6_done[i] = 0;
        g_w6_status[i] = 0;
        if (!spawn_probe(&p, "w6-th", w6_probe, (void *)(u64)(uptr)i)) {
            fe_kprintf("        W6 第 %u 条探针线程创建失败\n", i);
            fe_object_unref(&p.task->hdr);
            return fail + 1;
        }
    }
    u32 spin = 0;
    while (fe_wait_addr_waiters(p.task->space, PROBE_VA) < W6_N && spin++ < 500) {
        fe_thread_sleep_ms(1);
    }
    /* ★ 正向：8 个**全都**在链上 ★（定长 4 项的实现会在这里红）
     * 这一条就是"表满静默不登记"那个坑的判据：漏登记的等待者会睡死，
     * 而链上的人数会少于 8。 */
    u32 onchain = fe_wait_addr_waiters(p.task->space, PROBE_VA);
    g_w6_onchain = onchain;
    CHECK(onchain == W6_N);
    if (onchain != W6_N) {
        fe_kprintf("        W6：链上只有 %u 个等待者，期望 %u 个\n", onchain, W6_N);
    }
    /* ---- 计数语义：count = 1 只醒 1 个 ----
     * ★ 判据取 `fe_wake_addr` 的**返回值**，不取"链上有几个" ★
     * 被唤醒的那条线程要等它自己被调度到才会把自己从链上摘掉
     * （"被唤醒者清理自己的登记"是这个模块一贯的形状），
     * 所以 `fe_wait_addr_waiters` 在那一刻可能还是 8。 */
    probe_store(&p, 0, 0x2222u);
    u32 n1 = fe_wake_addr(p.task->space, PROBE_VA, 1);
    g_w6_n1 = n1;
    CHECK(n1 == 1);
    /* ★ 等它真的跑起来，而不是"立刻读一个还没被写的标志" ★
     * 这里第一版就是栽在这个竞态上：`fe_wake_addr` 只把线程置 READY，
     * 它还没被调度，`g_w6_done` 当然还是 0 ⇒ 断言假红。 */
    u32 woke = 0;
    for (u32 tries = 0; tries < 300 && woke < 1; tries++) {
        woke = 0;
        for (u32 i = 0; i < W6_N; i++) {
            if (g_w6_done[i]) {
                woke++;
            }
        }
        if (woke < 1) {
            fe_thread_sleep_ms(1);
        }
    }
    g_w6_woke = woke;
    CHECK(woke == 1);
    /* ---- count = 0 = 全部：这一条把"其余 7 个仍在睡"钉死 ----
     * `fe_wake_addr` 只点名 BLOCKED 的线程，所以"它返回 7"同时证明了
     * "刚才那一次只醒了 1 个"与"还有 7 个在等"。 */
    u32 n2 = fe_wake_addr(p.task->space, PROBE_VA, 0);
    g_w6_n2 = n2;
    CHECK(n2 == W6_N - 1);
    for (u32 i = 0; i < W6_N; i++) {
        if (!wait_done_ms(&g_w6_done[i], 300)) {
            fe_kprintf("        W6：第 %u 条探针没有被唤醒\n", i);
            fail++;
        } else {
            CHECK(g_w6_status[i] == FE_OK);
        }
    }
    CHECK(fe_wait_addr_waiters(p.task->space, PROBE_VA) == 0);
    fe_object_unref(&p.task->hdr);
    p.task = NULL;
    *out_ok = (fail == 0);
    return fail;
}

/* ================================================================== */
/* W7：★ 取消路径 ★ 等待中的线程被终止 ⇒ 登记必须摘干净、wake 不许崩  */
/* ================================================================== */

static volatile u32 g_w7_done;
static volatile i32 g_w7_status;

static void w7_probe(void *arg)
{
    struct fe_task *t = fe_task_current();
    (void)arg;
    fe_status_t s = fe_wait_addr(t, PROBE_VA, 0x1111u, 0);
    g_w7_status = s;
    g_w7_done = 1;
    fe_thread_exit(0);
}

static u32 w7_cancel(bool *out_ok)
{
    u32 fail = 0;
    *out_ok = false;
    struct probe p;
    if (!probe_new(&p, "w7-probe")) {
        fe_kprintf("        W7 探针任务创建失败\n");
        return 1;
    }
    probe_store(&p, 0, 0x1111u);
    g_w7_done = 0;
    g_w7_status = 0;
    struct fe_thread *th = spawn_probe(&p, "w7-th", w7_probe, NULL);
    if (!th) {
        fe_kprintf("        W7 探针线程创建失败\n");
        probe_free(&p);
        return 1;
    }
    u32 spin = 0;
    while (fe_wait_addr_waiters(p.task->space, PROBE_VA) == 0 && spin++ < 300) {
        fe_thread_sleep_ms(1);
    }
    CHECK(fe_wait_addr_waiters(p.task->space, PROBE_VA) == 1);
    /* ★ 终止整个任务：走的就是 fe_thread_cancel（取消点 + 摘除登记）★ */
    fe_task_terminate(p.task);
    bool ret = wait_done_ms(&g_w7_done, 500) != 0;
    CHECK(ret);
    if (ret) {
        CHECK(g_w7_status == FE_ERR_CANCELED);
    }
    /* ★ 摘干净：链上没有它、两个标志都清了、随后 wake 不崩且返回 0 ★ */
    CHECK(fe_wait_addr_waiters(p.task->space, PROBE_VA) == 0);
    CHECK(th->addr_waiting == 0);
    CHECK(th->waiting == 0);
    CHECK(th->wait_node == NULL);
    u32 n = fe_wake_addr(p.task->space, PROBE_VA, 0);
    CHECK(n == 0);
    fe_object_unref(&p.task->hdr);
    p.task = NULL;
    *out_ok = (fail == 0 && ret);
    return fail;
}

/* ================================================================== */
/* W8：超时 —— 到点返回 TIMEOUT，而且**两条链都摘干净**               */
/* ================================================================== */

static volatile u32 g_w8_done;
static volatile i32 g_w8_status;
static u64 g_w8_deadline;

static void w8_probe(void *arg)
{
    struct fe_task *t = fe_task_current();
    (void)arg;
    fe_status_t s = fe_wait_addr(t, PROBE_VA, 0x1111u, g_w8_deadline);
    g_w8_status = s;
    g_w8_done = 1;
    fe_thread_exit(0);
}

static u32 w8_timeout(bool *out_ok)
{
    u32 fail = 0;
    *out_ok = false;
    struct probe p;
    if (!probe_new(&p, "w8-probe")) {
        fe_kprintf("        W8 探针任务创建失败\n");
        return 1;
    }
    probe_store(&p, 0, 0x1111u);

    /* ---- 正向：带 20 ms deadline，没人唤醒 ⇒ TIMEOUT ---- */
    g_w8_done = 0;
    g_w8_status = 0;
    g_w8_deadline = fe_time_ns() + 20ull * 1000000ull;
    struct fe_thread *th = spawn_probe(&p, "w8-th1", w8_probe, NULL);
    if (!th) {
        fe_kprintf("        W8 探针线程创建失败\n");
        probe_free(&p);
        return 1;
    }
    bool ret = wait_done_ms(&g_w8_done, 3000) != 0;
    CHECK(ret);
    if (ret) {
        CHECK(g_w8_status == FE_ERR_TIMEOUT);
    }
    /* ★ 两条链都摘干净 ★ 地址链（访问器）与睡眠链（sleep_armed）*/
    CHECK(fe_wait_addr_waiters(p.task->space, PROBE_VA) == 0);
    if (th->sleep_armed) {
        fe_kprintf("        W8：到点之后线程**还挂在睡眠链上**\n");
        fail++;
    }
    CHECK(th->sleep_armed == 0);
    CHECK(th->addr_waiting == 0);
    CHECK(th->waiting == 0);

    /* ---- 反向：同样的等待但 deadline = 0，由别人唤醒 ⇒ OK（不是 TIMEOUT）---- */
    g_w8_done = 0;
    g_w8_status = 0;
    g_w8_deadline = 0;
    if (!spawn_probe(&p, "w8-th2", w8_probe, NULL)) {
        fe_kprintf("        W8 反向探针创建失败\n");
        fail++;
    } else {
        u32 spin = 0;
        while (fe_wait_addr_waiters(p.task->space, PROBE_VA) == 0 && spin++ < 300) {
            fe_thread_sleep_ms(1);
        }
        CHECK(fe_wait_addr_waiters(p.task->space, PROBE_VA) == 1);
        probe_store(&p, 0, 0x2222u);
        CHECK(fe_wake_addr(p.task->space, PROBE_VA, 0) == 1);
        if (!wait_done_ms(&g_w8_done, 500)) {
            fe_kprintf("        W8 反向：探针没有返回\n");
            fail++;
        } else {
            CHECK(g_w8_status == FE_OK);
        }
    }
    fe_object_unref(&p.task->hdr);
    p.task = NULL;
    *out_ok = (fail == 0 && ret);
    return fail;
}

/* ================================================================== */
/* W9：唤醒一个**没有等待者**的地址：返回 0、不改任何状态              */
/* ================================================================== */

static u32 w9_empty(bool *out_ok)
{
    u32 fail = 0;
    *out_ok = false;
    struct probe p;
    if (!probe_new(&p, "w9-probe")) {
        fe_kprintf("        W9 探针任务创建失败\n");
        return 1;
    }
    probe_store(&p, 0, 0x1111u);
    probe_store(&p, 4, 0x1111u);
    CHECK(fe_wake_addr(p.task->space, PROBE_VA, 0) == 0);
    CHECK(fe_wake_addr(p.task->space, PROBE_VA, 1) == 0);
    CHECK(fe_wait_addr_waiters(p.task->space, PROBE_VA) == 0);
    /* ★ 值不许被唤醒方动过 ★ 唤醒是"点名"，不是"投递"。 */
    CHECK(probe_load(&p, 0) == 0x1111u);
    CHECK(probe_load(&p, 4) == 0x1111u);
    fe_object_unref(&p.task->hdr);
    p.task = NULL;
    *out_ok = (fail == 0);
    return fail;
}

/* ================================================================== */

u32 fe_selftest_wait_addr(void)
{
    u32 fail = 0;
    bool ok = false;

    fail += w2_bits(&ok);
    fe_kprintf("        W2a：★ 丢唤醒窗口（通知键）★ %s\n",
               ok ? "条件在\"检查之后、登记之前\"被置真并唤醒 → 探针立刻返回"
                  : "**漏醒**（红）");

    fail += w2_addr(&ok);
    fe_kprintf("        W2b：★ 丢唤醒窗口（地址键）★ %s\n",
               ok ? "值在检查与登记之间被改 → 等待者**一次都没睡**（switches==1）"
                  : "**漏醒或睡了**（红）");

    fail += w1_case(true, &ok);
    fe_kprintf("        W1a：值没变就睡、被唤醒后返回 OK，且确实睡过（switches>=2）%s\n",
               ok ? "" : "  **失败**");

    fail += w1_case(false, &ok);
    fe_kprintf("        W1b：反向——值一开始就不等于期望 ⇒ 立刻返回 AGAIN，"
               "且**一次都没睡**（switches==1）%s\n", ok ? "" : "  **失败**");

    fail += w3_wrong_addr(&ok);
    fe_kprintf("        W3：唤醒**别的地址**不许误伤（邻字值相同也不许醒）；"
               "唤醒正确地址只醒 1 个%s\n", ok ? "" : "  **失败**");

    fail += w4_cross_space(&ok);
    fe_kprintf("        W4：★ 跨地址空间 ★ 同一个虚拟地址、两个任务，"
               "唤一个另一个纹丝不动%s\n", ok ? "" : "  **失败**");

    fail += w5_unmapped(&ok);
    fe_kprintf("        W5：读不了的地址 ⇒ FAULT（不是 AGAIN）；"
               "映射着的地址就不是 FAULT%s\n", ok ? "" : "  **失败**");

    fail += w6_capacity(&ok);
    fe_kprintf("        W6：容量 —— 同一地址 %u 个等待者全都真的挂上"
               "（> FE_WAITERS_MAX=%u）；count=1 只醒 1 个、count=0 醒全部"
               "（实测：链上 %u、wake(1)=%u、跑起来 %u、wake(0)=%u）%s\n",
               (u32)W6_N, (u32)FE_WAITERS_MAX,
               g_w6_onchain, g_w6_n1, g_w6_woke, g_w6_n2,
               ok ? "" : "  **失败**");

    fail += w7_cancel(&ok);
    fe_kprintf("        W7：★ 取消路径 ★ 等待中被终止 ⇒ 登记摘干净、"
               "随后 wake 不许崩%s\n", ok ? "" : "  **失败**");

    fail += w8_timeout(&ok);
    fe_kprintf("        W8：超时到点返回 TIMEOUT，且**两条链**（地址链 + 睡眠链）"
               "都摘干净；deadline=0 时则是被唤醒%s\n", ok ? "" : "  **失败**");

    fail += w9_empty(&ok);
    fe_kprintf("        W9：唤醒一个没有等待者的地址 ⇒ 返回 0、不改任何状态%s\n",
               ok ? "" : "  **失败**");

    return fail;
}
