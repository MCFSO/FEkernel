/* SPDX-License-Identifier: 0BSD */
/* 替换映像（K6：exec）的内核自检。
 *
 * ★ 这一组验的是"闸门扩到线程级"这套机制**在 exec 真正要用的形态下**对不对 ★
 * exec 的提交阶段第一步就是"叫停除调用者之外的每一个线程，并等它们死透"
 * （docs/15-exec.md §4），所以这里的 E1–E5 就是那一半的判据
 * （docs/13-tasks-and-kill.md §6.7）。真实的"调用者带着新映像回到用户态"
 * 那一半只能由真实客户端验（user/bin/exectest，属于 2c）。
 *
 * ★ 每条正向断言都配一条会失败的反向对照 ★
 * 没有反向对照的话，"死了"与"恰好没被调度"从外面看完全一样——
 * 这个项目已经为这条付过两次学费。
 *
 * 探针全部是**内核线程**（`fe_thread_create`，`reap_ok` 默认 false）：
 * 僵尸不会被回收，所以自检可以一直读它们的字段，而不会踩 use-after-free。
 */
#include <fe/task.h>
#include <fe/sched/sched.h>
#include <fe/sched/thread.h>
#include <fe/ipc.h>
#include <fe/process.h>
#include <fe/object.h>
#include <fe/kprintf.h>
#include <fe/string.h>
#include <fe/time.h>

#define CHECK(cond) do { if (!(cond)) { fail++; } } while (0)

/* ------------------------------------------------------------------ */
/* 探针                                                                */
/* ------------------------------------------------------------------ */

/* 忙等：**不让出**。要模拟"跑飞的服务"，而且只有不让出才能稳定拿到节拍
 * （让出型的探针常常不是"当前线程"，cpu_ticks 长不大 —— killtest.c 里记着
 *  这条教训）。 */
static volatile u32 g_busy_up;
static volatile u64 g_sink;
static void probe_busy(void *arg)
{
    (void)arg;
    g_busy_up = 1;
    for (;;) {
        g_sink++;
        __asm__ volatile("" ::: "memory");
    }
}

/* 反向对照：**把自己标记**，然后让出——它必须死在闸门上。 */
static volatile u32 g_self_entered;
static void probe_selfkill(void *arg)
{
    (void)arg;
    struct fe_thread *self = fe_thread_current();
    g_self_entered = 1;
    self->kill_pending = true;
    for (;;) {
        fe_thread_yield();
    }
}

/* E4：三种等待登记各一个探针（arg 选一种）。 */
#define E4_KIND_RECV  1
#define E4_KIND_WAIT  2
#define E4_KIND_NOTIF 3
static volatile long g_e4_handle;
static volatile u32  g_e4_entered;
static volatile u32  g_e4_done;
static void probe_e4(void *arg)
{
    long kind = (long)arg;
    struct fe_task *t = fe_task_current();
    g_e4_entered = 1;
    if (kind == E4_KIND_RECV) {
        struct fe_msg_header hdr;
        (void)fe_endpoint_recv(t, (fe_handle_t)g_e4_handle, &hdr, NULL, 0,
                               NULL, NULL);
    } else if (kind == E4_KIND_WAIT) {
        struct fe_wait_target tg;
        struct fe_wait_result res;
        tg.handle = (fe_handle_t)g_e4_handle;
        tg.kind = FE_WAIT_MSG;
        tg.mask = 0;
        (void)fe_wait_any(t, &tg, 1, &res, 0);
    } else {
        u64 bits = 0;
        (void)fe_notification_wait((fe_handle_t)g_e4_handle, 0x1, &bits);
    }
    g_e4_done = 1;
    fe_thread_exit(0);
}

/* ------------------------------------------------------------------ */
/* 工具                                                                */
/* ------------------------------------------------------------------ */

/* 等一个 volatile 标志变真，**绝对时间上界**。
 * ★ 为什么不用"让出轮数" ★ 让出轮数在会被饿的系统里不是时间
 * （实测过：main 每 100 个节拍只拿到 10 个节拍 ⇒ 20000 轮要 200 秒），
 * 那会把"跑不完"伪装成"挂死"。 */
static int wait_flag_ms(volatile u32 *flag, u32 ms)
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

/* 等某任务里剩余的存活线程数降到 keep 以下（绝对时间上界）。 */
static int wait_count_le(struct fe_task *t, u32 keep, u32 ms)
{
    u64 deadline = fe_time_ms() + ms;
    for (;;) {
        if (fe_task_thread_count(t) <= keep) {
            return 1;
        }
        if (fe_time_ms() >= deadline) {
            return 0;
        }
        fe_thread_sleep_ms(1);
    }
}

/* 等一个线程落到某个状态（绝对时间上界）。 */
static int wait_state_ms(struct fe_thread *t, u32 want, u32 ms)
{
    u64 deadline = fe_time_ms() + ms;
    for (;;) {
        if (t && t->state == want) {
            return 1;
        }
        if (fe_time_ms() >= deadline) {
            return 0;
        }
        fe_thread_sleep_ms(1);
    }
}

/* 造一个探针任务并挂上探针线程。
 *
 * ★ 为什么要临时换当前线程的 task ★
 * `fe_thread_create` 把新线程的 task 取成"当前任务"的，所以要让探针属于
 * 探针任务，只能在这个窗口里换一下（与 tlstest.c / killtest.c 同一手法）。
 * 创建期间不会被抢占：rq_push 那一小段是关中断的。 */
static struct fe_thread *spawn_probe(struct fe_task *task, const char *name,
                                     fe_thread_entry_t entry, void *arg)
{
    struct fe_thread *cur = fe_thread_current();
    struct fe_task *saved = cur ? cur->task : NULL;
    if (cur) {
        cur->task = task;
    }
    struct fe_thread *th = fe_thread_create(name, entry, arg, 16 * 1024,
                                            FE_PRIO_NORMAL);
    if (cur) {
        cur->task = saved;
    }
    if (th && !task->main_thread) {
        task->main_thread = th;
    }
    return th;
}

/* ------------------------------------------------------------------ */
/* E1–E5                                                               */
/* ------------------------------------------------------------------ */

/* E1：三个线程的任务，叫停两个、留一个。
 * 正向：另外两个 DEAD、count==1、keep 还活着。
 * 反向：① 杀**之前**必须断言 count==3（否则"==1"可能是恒真）；
 *       ② keep 的 cpu_ticks 必须继续增长（证明"死"是这次叫停造成的，
 *          不是它们自己恰好退了，也不是 keep 恰好没被调度）。 */
static u32 test_e1(void)
{
    u32 fail = 0;
    struct fe_task *task = fe_task_create_kernel("e1-kill");
    if (!task) {
        fe_kprintf("        探针任务创建失败\n");
        return 1;
    }
    g_busy_up = 0;
    struct fe_thread *m  = spawn_probe(task, "e1-main", probe_busy, NULL);
    struct fe_thread *w1 = spawn_probe(task, "e1-w1",   probe_busy, NULL);
    struct fe_thread *w2 = spawn_probe(task, "e1-w2",   probe_busy, NULL);
    if (!m || !w1 || !w2 || !wait_flag_ms(&g_busy_up, 200)) {
        fe_kprintf("        E1 探针没有跑起来\n");
        fe_object_unref(&task->hdr);
        return 1;
    }
    u32 before = fe_task_thread_count(task);
    if (before != 3) {
        fe_kprintf("        **E1 前置状态不成立**：杀之前线程数=%u（期望 3）\n",
                   before);
        fail++;
    } else {
        fe_kprintf("        E1 前置状态：杀之前 task 有 3 个线程\n");
        u32 marked = fe_task_kill_other_threads(task, m);
        CHECK(marked == 2);
        CHECK(wait_count_le(task, 1, 200));
        if (w1->state == FE_THREAD_DEAD && w2->state == FE_THREAD_DEAD &&
            fe_task_thread_count(task) == 1 && m->state != FE_THREAD_DEAD &&
            !m->kill_pending) {
            fe_kprintf("        E1 正向：标记 %u 个 → 另外两个 DEAD、count==1、"
                       "keep 未受伤（state=%u）\n", marked, m->state);
        } else {
            fe_kprintf("        **E1 失败**：marked=%u count=%u w1=%u w2=%u "
                       "keep=%u\n", marked, fe_task_thread_count(task),
                       w1->state, w2->state, m->state);
            fail++;
        }
        u64 t0 = m->cpu_ticks;
        for (u32 i = 0; i < 20 && m->cpu_ticks == t0; i++) {
            fe_thread_sleep_ms(5);
        }
        CHECK(m->cpu_ticks > t0);
        if (m->cpu_ticks > t0) {
            fe_kprintf("        E1 反向对照：keep 仍在消耗 CPU（%llu → %llu 节拍）"
                       "——死是这次叫停造成的\n",
                       (unsigned long long)t0, (unsigned long long)m->cpu_ticks);
        } else {
            fe_kprintf("        **E1 反向对照失效**：keep 的节拍没有增长\n");
            fail++;
        }
    }
    fe_task_cleanup_probes(task, "E1");
    fe_object_unref(&task->hdr);
    return fail;
}

/* E2：调用者（keep）没被误杀。
 * 反向：一个**故意把自己标记**的探针必须在下一次让出时死在闸门
 * ——它证明闸门对"被标记的线程"确实生效，也就是说"排除调用者"不是可选的。 */
static u32 test_e2(void)
{
    u32 fail = 0;
    struct fe_task *task = fe_task_create_kernel("e2-keep");
    if (!task) {
        fe_kprintf("        探针任务创建失败\n");
        return 1;
    }
    g_busy_up = 0;
    struct fe_thread *w = spawn_probe(task, "e2-w", probe_busy, NULL);
    if (!w || !wait_flag_ms(&g_busy_up, 200)) {
        fe_kprintf("        E2 探针没有跑起来\n");
        fe_object_unref(&task->hdr);
        return 1;
    }
    struct fe_thread *self = fe_thread_current();
    u64 sw0 = self->switches;
    u32 marked = fe_task_kill_other_threads(task, self);
    CHECK(marked == 1);
    CHECK(wait_count_le(task, 1, 200));
    for (u32 i = 0; i < 20 && self->switches == sw0; i++) {
        fe_thread_sleep_ms(5);
    }
    if (self->state == FE_THREAD_RUNNING && !self->kill_pending &&
        self->switches > sw0 && w->state == FE_THREAD_DEAD) {
        fe_kprintf("        E2 正向：keep 未受伤（state=%u kill_pending=0，"
                   "switches %llu → %llu），受害者 DEAD\n", self->state,
                   (unsigned long long)sw0, (unsigned long long)self->switches);
    } else {
        fe_kprintf("        **E2 失败**：keep state=%u kill_pending=%u "
                   "switches=%llu（起始 %llu）w=%u\n", self->state,
                   (u32)self->kill_pending, (unsigned long long)self->switches,
                   (unsigned long long)sw0, w->state);
        fail++;
    }
    fe_task_cleanup_probes(task, "E2");
    fe_object_unref(&task->hdr);

    struct fe_task *t2 = fe_task_create_kernel("e2-selfkill");
    if (!t2) {
        fe_kprintf("        探针任务创建失败\n");
        return fail + 1;
    }
    g_self_entered = 0;
    struct fe_thread *sk = spawn_probe(t2, "e2-self", probe_selfkill, NULL);
    if (!sk || !wait_flag_ms(&g_self_entered, 200)) {
        fe_kprintf("        E2 反向对照探针没有跑起来\n");
        fail++;
    } else if (wait_state_ms(sk, FE_THREAD_DEAD, 500)) {
        fe_kprintf("        E2 反向对照：自标记的线程在下一次让出时死在闸门"
                   "（state=%u）——排除 keep 不是可选的\n", sk->state);
    } else {
        fe_kprintf("        **E2 反向对照失效**：自标记的线程没有死（state=%u）\n",
                   sk->state);
        fail++;
    }
    fe_task_cleanup_probes(t2, "E2 反向");
    fe_object_unref(&t2->hdr);
    return fail;
}

/* E3：死透之后停止消耗 CPU。
 * 正向：被叫停的线程死前节拍涨过，死后不再涨。
 * 反向：**同一个窗口里**一个没被叫停的忙等线程必须继续涨——否则"不涨"
 * 可能只是因为节拍源停了，那样这一条什么都没证明。 */
static u32 test_e3(void)
{
    u32 fail = 0;
    struct fe_task *task = fe_task_create_kernel("e3-ticks");
    struct fe_task *busy_task = fe_task_create_kernel("e3-busy");
    if (!task || !busy_task) {
        fe_kprintf("        探针任务创建失败\n");
        return 1;
    }
    g_busy_up = 0;
    struct fe_thread *v = spawn_probe(task, "e3-victim", probe_busy, NULL);
    struct fe_thread *busy = spawn_probe(busy_task, "e3-busy", probe_busy, NULL);
    if (!v || !busy || !wait_flag_ms(&g_busy_up, 200)) {
        fe_kprintf("        E3 探针没有跑起来\n");
        fe_object_unref(&task->hdr);
        fe_object_unref(&busy_task->hdr);
        return 1;
    }
    u64 t0 = v->cpu_ticks;
    for (u32 i = 0; i < 20 && v->cpu_ticks == t0; i++) {
        fe_thread_sleep_ms(5);
    }
    u64 t1 = v->cpu_ticks;
    CHECK(t1 > t0);
    CHECK(fe_task_kill_other_threads(task, fe_thread_current()) == 1);
    CHECK(wait_count_le(task, 0, 200));
    CHECK(v->state == FE_THREAD_DEAD);
    u64 t2 = v->cpu_ticks;
    u64 b0 = busy->cpu_ticks;
    fe_thread_sleep_ms(30);
    u64 t3 = v->cpu_ticks;
    u64 b1 = busy->cpu_ticks;
    /* 死者最多再记 1 个节拍（被判死那一次调度的余数） */
    CHECK(t3 <= t2 + 1);
    CHECK(b1 > b0);
    if (t3 <= t2 + 1 && b1 > b0) {
        fe_kprintf("        E3 正向：受害者在死前涨过（%llu → %llu），死后不再"
                   "增长（%llu → %llu）；同期未被叫停的忙等线程仍在增长"
                   "（%llu → %llu）\n",
                   (unsigned long long)t0, (unsigned long long)t1,
                   (unsigned long long)t2, (unsigned long long)t3,
                   (unsigned long long)b0, (unsigned long long)b1);
    } else {
        fe_kprintf("        **E3 失败**：victim %llu→%llu（期望最多 +1），"
                   "busy %llu→%llu（期望增长）\n",
                   (unsigned long long)t2, (unsigned long long)t3,
                   (unsigned long long)b0, (unsigned long long)b1);
        fail++;
    }
    fe_task_cleanup_probes(task, "E3");
    fe_task_cleanup_probes(busy_task, "E3 对照");
    fe_object_unref(&task->hdr);
    fe_object_unref(&busy_task->hdr);
    return fail;
}

/* E4：阻塞中的受害者必须走到**取消点**、而且登记被摘干净。
 * 三条登记各验一次：端点的 receiver 单槽、wait_any 的 wait_node、
 * 通知对象的 waiter 单槽。
 *
 * ★ 判据分两层（闸门在"回到被打断的代码之前"就可能先把它判死）★
 *   ① 线程死透；
 *   ② **它登记的槽被摘干净**——这一条不依赖它是否真的被调度过，是最硬的。 */
static u32 test_e4(void)
{
    u32 fail = 0;

    /* ① fe_endpoint_recv → 端点的 receiver 槽 */
    {
        struct fe_task *task = fe_task_create_kernel("e4-recv");
        long ep = -1;
        if (!task || fe_failed(fe_endpoint_create(task, 0, (fe_handle_t *)&ep))) {
            fe_kprintf("        E4① 端点创建失败\n");
            if (task) {
                fe_object_unref(&task->hdr);
            }
            return fail + 1;
        }
        struct fe_object_header *o = NULL;
        fe_handle_lookup(&task->handles, (fe_handle_t)ep, 0, &o);
        struct fe_endpoint *epx = FE_OBJ_OF(o, struct fe_endpoint);
        g_e4_handle = ep;
        g_e4_entered = 0;
        g_e4_done = 0;
        struct fe_thread *v = spawn_probe(task, "e4-recv", probe_e4,
                                          (void *)E4_KIND_RECV);
        if (!v || !wait_flag_ms(&g_e4_entered, 200) ||
            !wait_state_ms(v, FE_THREAD_BLOCKED, 500)) {
            fe_kprintf("        E4① 探针没有进入阻塞\n");
            fail++;
        } else {
            CHECK(epx->receiver == v);          /* 前置状态：登记确实在 */
            CHECK(fe_task_kill_other_threads(task, fe_thread_current()) == 1);
            CHECK(wait_count_le(task, 0, 200));
            CHECK(epx->receiver == NULL);
            if (epx->receiver == NULL && fe_task_thread_count(task) == 0) {
                fe_kprintf("        E4① 正向：recv 受害者死透、receiver 槽已清空"
                           "（调用% s）\n", g_e4_done ? "返回了" : "未走完");
            } else {
                fe_kprintf("        **E4① 失败**：receiver=%p count=%u\n",
                           (void *)epx->receiver, fe_task_thread_count(task));
                fail++;
            }
        }
        fe_task_cleanup_probes(task, "E4①");
        fe_object_unref(&task->hdr);
    }

    /* ② fe_wait_any → 线程身上的 wait_node */
    {
        struct fe_task *task = fe_task_create_kernel("e4-wait");
        long ep = -1;
        if (!task || fe_failed(fe_endpoint_create(task, 0, (fe_handle_t *)&ep))) {
            fe_kprintf("        E4② 端点创建失败\n");
            if (task) {
                fe_object_unref(&task->hdr);
            }
            return fail + 1;
        }
        g_e4_handle = ep;
        g_e4_entered = 0;
        g_e4_done = 0;
        struct fe_thread *v = spawn_probe(task, "e4-wait", probe_e4,
                                          (void *)E4_KIND_WAIT);
        if (!v || !wait_flag_ms(&g_e4_entered, 200) ||
            !wait_state_ms(v, FE_THREAD_BLOCKED, 500)) {
            fe_kprintf("        E4② 探针没有进入阻塞\n");
            fail++;
        } else {
            CHECK(v->waiting == 1);             /* 前置状态：登记确实在 */
            CHECK(v->wait_node != NULL);
            CHECK(fe_task_kill_other_threads(task, fe_thread_current()) == 1);
            CHECK(wait_count_le(task, 0, 200));
            CHECK(v->waiting == 0);
            CHECK(v->wait_node == NULL);
            if (v->waiting == 0 && v->wait_node == NULL) {
                fe_kprintf("        E4② 正向：wait_any 登记者死透、登记摘干净"
                           "（waiting=0 wait_node=NULL）\n");
            } else {
                fe_kprintf("        **E4② 失败**：waiting=%u wait_node=%p\n",
                           v->waiting, v->wait_node);
                fail++;
            }
        }
        fe_task_cleanup_probes(task, "E4②");
        fe_object_unref(&task->hdr);
    }

    /* ③ fe_notification_wait → 通知对象的 waiter 单槽 */
    {
        struct fe_task *task = fe_task_create_kernel("e4-notif");
        long nth = -1;
        if (!task || fe_failed(fe_notification_create(task, (fe_handle_t *)&nth))) {
            fe_kprintf("        E4③ 通知创建失败\n");
            if (task) {
                fe_object_unref(&task->hdr);
            }
            return fail + 1;
        }
        struct fe_object_header *o = NULL;
        fe_handle_lookup(&task->handles, (fe_handle_t)nth, 0, &o);
        struct fe_notification *ntx = FE_OBJ_OF(o, struct fe_notification);
        g_e4_handle = nth;
        g_e4_entered = 0;
        g_e4_done = 0;
        struct fe_thread *v = spawn_probe(task, "e4-notif", probe_e4,
                                          (void *)E4_KIND_NOTIF);
        if (!v || !wait_flag_ms(&g_e4_entered, 200) ||
            !wait_state_ms(v, FE_THREAD_BLOCKED, 500)) {
            fe_kprintf("        E4③ 探针没有进入阻塞\n");
            fail++;
        } else {
            CHECK(fe_notification_waiter(ntx) == v);    /* 前置状态 */
            CHECK(fe_task_kill_other_threads(task, fe_thread_current()) == 1);
            CHECK(wait_count_le(task, 0, 200));
            CHECK(fe_notification_waiter(ntx) == NULL);
            if (fe_notification_waiter(ntx) == NULL) {
                fe_kprintf("        E4③ 正向：通知等待者死透、nt->waiter 已清空\n");
            } else {
                fe_kprintf("        **E4③ 失败**：waiter=%p（这属于 D1："
                           "fe_thread_cancel 还没扩到通知对象）\n",
                           (void *)fe_notification_waiter(ntx));
                fail++;
            }
        }
        fe_task_cleanup_probes(task, "E4③");
        fe_object_unref(&task->hdr);
    }
    return fail;
}

/* E5：**旧的任务级终止没有被新机制削掉**。
 * 正向：fe_task_terminate(task) 之后每个线程都 DEAD、count==0、exited 置位。
 * 反向：① 另一个**只**被 fe_task_kill_other_threads 处理的任务里 keep 仍活着
 *      （证明线程级标记不会顺手把整个任务带走）；
 *      ② 第三个任务的线程完全不受影响（证明两种终止都只作用于目标任务）。 */
static u32 test_e5(void)
{
    u32 fail = 0;
    struct fe_task *ta = fe_task_create_kernel("e5-term");
    struct fe_task *tb = fe_task_create_kernel("e5-keep");
    struct fe_task *tc = fe_task_create_kernel("e5-third");
    if (!ta || !tb || !tc) {
        fe_kprintf("        探针任务创建失败\n");
        return 1;
    }
    g_busy_up = 0;
    struct fe_thread *a1 = spawn_probe(ta, "e5-a1", probe_busy, NULL);
    struct fe_thread *a2 = spawn_probe(ta, "e5-a2", probe_busy, NULL);
    struct fe_thread *bk = spawn_probe(tb, "e5-keep", probe_busy, NULL);
    struct fe_thread *c1 = spawn_probe(tc, "e5-c1", probe_busy, NULL);
    if (!a1 || !a2 || !bk || !c1 || !wait_flag_ms(&g_busy_up, 200)) {
        fe_kprintf("        E5 探针没有跑起来\n");
        fe_object_unref(&ta->hdr);
        fe_object_unref(&tb->hdr);
        fe_object_unref(&tc->hdr);
        return 1;
    }
    CHECK(fe_task_thread_count(ta) == 2);
    fe_task_terminate(ta);
    CHECK(ta->dying);
    CHECK(wait_count_le(ta, 0, 500));
    CHECK(ta->exited);
    CHECK(fe_task_kill_other_threads(tb, bk) == 0);   /* 它只有一个线程 */
    if (fe_task_thread_count(ta) == 0 && ta->exited &&
        bk->state != FE_THREAD_DEAD && c1->state != FE_THREAD_DEAD &&
        !bk->kill_pending && !c1->kill_pending) {
        fe_kprintf("        E5 正向：任务级终止带走全部线程（count=0、exited=1）；"
                   "只被线程级处理的 keep 仍 %u；第三个任务的线程仍 %u\n",
                   bk->state, c1->state);
    } else {
        fe_kprintf("        **E5 失败**：count=%u exited=%u keep=%u third=%u\n",
                   fe_task_thread_count(ta), (u32)ta->exited, bk->state, c1->state);
        fail++;
    }
    fe_task_cleanup_probes(ta, "E5 受害者");
    fe_task_cleanup_probes(tb, "E5 对照");
    fe_task_cleanup_probes(tc, "E5 第三方");
    fe_object_unref(&ta->hdr);
    fe_object_unref(&tb->hdr);
    fe_object_unref(&tc->hdr);
    return fail;
}

/* ------------------------------------------------------------------ */

u32 fe_selftest_exec(void)
{
    u32 fail = 0;
    fe_kprintf("        E1：叫停其它线程（三个线程留一个）\n");
    fail += test_e1();
    fe_kprintf("        E2：调用者未被误杀（+ 自标记探针必死）\n");
    fail += test_e2();
    fe_kprintf("        E3：死透之后停止消耗 CPU\n");
    fail += test_e3();
    fe_kprintf("        E4：三条取消点各验一次\n");
    fail += test_e4();
    fe_kprintf("        E5：任务级终止没有被新机制削掉\n");
    fail += test_e5();
    return fail;
}
