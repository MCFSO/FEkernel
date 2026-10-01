/* SPDX-License-Identifier: 0BSD */
/* 进程终止（K2）的内核自检。
 *
 * ★ 这一组要证明的不是"能杀掉"，而是**三条不同路径都能走到死亡**，
 * 以及**杀不掉的情形真的被拒** ★
 *
 * 三条成功路径（三条各自有各自的机制）：
 *   1. **忙等中的线程**（就绪/在跑）——走闸门：它下次回到用户态之前
 *      在 `fe_sched_maybe_switch` 里变成僵尸。这条用的是"什么都不做，
 *      只置标志"那条路；
 *   2. **阻塞在收消息上的线程**——走取消点：端点的 receiver 槽 +
 *      `FE_ERR_CANCELED`，然后照样经过闸门；
 *   3. **阻塞在 wait_any 上的线程**——走取消点，但摘除的是
 *      `wait_node`（与第 2 条是**两套不同的登记**，所以必须分别验）。
 *
 * ★ 反向对照（没有它们，"杀成功"和"线程恰好没被调度"分不开）★
 *   - 没有 TERMINATE 权限的句柄必须被拒（ACCESS）——否则"能力"这个词
 *     在这个原语上就是装饰；
 *   - 坏的/别人的句柄必须被拒（BADHANDLE）；
 *   - 对非任务对象必须被拒（INVAL）；
 *   - **被杀之后目标不再消耗 CPU**：对比 terminate 前后的 `cpu_ticks`。
 *     只看"它不再打印"是不够的——一个被杀的线程与一个恰好没被调度的
 *     线程从外部看完全一样。而"不再消耗 CPU"是**可测量**的差别。
 *
 * 探针线程都跑在内核里：这一组测的是机制，用户态那条路另有客户端
 * （`user/bin/killtest`）。
 */
#include <fe/task.h>
#include <fe/sched/sched.h>
#include <fe/sched/thread.h>
#include <fe/ipc.h>
#include <fe/process.h>
#include <fe/object.h>
#include <fe/kprintf.h>
#include <fe/string.h>

#define CHECK(cond) do { if (!(cond)) { fail++; } } while (0)

static volatile u32 g_spin_alive;

/* ---- 探针 1：**跑飞**的线程（纯忙等，不让出）----
 *
 * ★ 为什么用纯忙等而不是"忙等 + 让出" ★
 * 第一版是 `for(;;) fe_thread_yield();`，结果它的 `cpu_ticks` **一直是 0**：
 * 让出会把 CPU 交给空闲线程，而节拍是记在"当时在跑的那个线程"头上的——
 * 于是节拍全记到了 idle 账上，探针自己被记成"从没跑过"。
 * 断言"它在消耗 CPU"因此失败，而被测的机制完全正确。
 *
 * 纯忙等反而更贴近要模拟的东西：**"跑飞的服务"就是不让出的那种**。
 * 它不会被饿死别人——节拍处理在时间片用尽时会置上重调度请求，
 * 而定时器中断的返回路径会经过切换点（抢占正是在那里发生的）。 */
static volatile u64 g_spin_sink;
static void probe_spin(void *arg)
{
    (void)arg;
    g_spin_alive = 1;
    for (;;) {
        g_spin_sink++;
        __asm__ volatile("" ::: "memory");
    }
}

/* ---- 探针 2：阻塞在收消息上 ---- */
static struct fe_endpoint *g_probe_ep;
static long g_probe_ep_handle;
static volatile u32 g_recv_entered;
static volatile u32 g_recv_result;      /* 1 = 返回了；0 = 没返回 */
static volatile i32 g_recv_status;
static void probe_recv(void *arg)
{
    (void)arg;
    struct fe_task *t = fe_task_current();
    g_recv_entered = 1;
    struct fe_msg_header hdr;
    fe_status_t s = fe_endpoint_recv(t, (fe_handle_t)g_probe_ep_handle, &hdr,
                                     NULL, 0, NULL, NULL);
    g_recv_status = s;
    g_recv_result = 1;
    fe_thread_exit(0);
}
/* ---- 探针 3：阻塞在 wait_any 上 ---- */
static volatile u32 g_wait_entered;
static volatile u32 g_wait_result;
static volatile i32 g_wait_status;
static void probe_wait(void *arg)
{
    (void)arg;
    struct fe_task *t = fe_task_current();
    struct fe_wait_target tg;
    tg.handle = (fe_handle_t)g_probe_ep_handle;
    tg.kind = FE_WAIT_MSG;
    tg.mask = 0;
    struct fe_wait_result res;
    g_wait_entered = 1;
    fe_status_t s = fe_wait_any(t, &tg, 1, &res, 0);
    g_wait_status = s;
    g_wait_result = 1;
    fe_thread_exit(0);
}

/* ---- 探针 4：**自己终止自己所属的任务** ---- */
static volatile u32 g_self_entered;
static void probe_self(void *arg)
{
    struct fe_task *task = fe_task_current();
    (void)arg;
    g_self_entered = 1;
    /* ★ 返回值观察不到，这是语义的一部分 ★
     * 它返回之后线程会去做一次让出，而"让出"要经过中断返回路径，
     * 闸门就在那里——所以它死在返回用户态（对内核线程来说：死在
     * 下一次进出中断）的路上，看不到自己的返回值。
     *
     * 内核线程的闸门为什么也在那里：内核线程不会"返回用户态"，
     * 但它每次 yield/阻塞/被中断都会经过 `fe_sched_maybe_switch`，
     * 闸门就挂在那条路上。**一个从不经过那条路的线程（纯计算死循环）
     * 不受闸门保护**——这一条写进 docs/13-tasks-and-kill.md 的边界里：
     * 单核上那样的线程本来也会把系统占死，所以那不是"杀不掉"，
     * 而是"系统已经不可调度了"。 */
    fe_task_terminate(task);
    /* 让出 → 经过闸门 → 在那里变成僵尸。 */
    for (;;) {
        fe_thread_yield();
    }
}

/* 等一个 volatile 标志变真，最多等 rounds 轮让出。 */
static int wait_flag(volatile u32 *flag, u32 rounds)
{
    for (u32 i = 0; i < rounds; i++) {
        if (*flag) {
            return 1;
        }
        fe_thread_yield();
    }
    return 0;
}

/* 一个任务的线程累计占用了多少节拍（"它还在消耗 CPU 吗"的判据）。 */
static u64 task_ticks(struct fe_task *t)
{
    u64 n = 0;
    for (struct fe_thread *th = fe_task_thread_first(t); th;
         th = fe_thread_next_of(th)) {
        n += th->cpu_ticks;
    }
    return n;
}

/* 造一个探针任务并挂上探针线程。
 *
 * ★ 为什么要临时换当前线程的 task ★
 * `fe_thread_create` 把新线程的 task 取成"当前任务"的。要让探针线程属于
 * 探针任务（而不是调用者任务），只能在这个窗口里换一下——与 tlstest.c
 * 里的做法一样（创建期间不会被抢占：rq_push 那一小段是关中断的）。 */
static struct fe_thread *spawn_probe_arg(struct fe_task *task, const char *name,
                                         fe_thread_entry_t entry, void *arg);

static struct fe_thread *spawn_probe(struct fe_task *task, const char *name,
                                     fe_thread_entry_t entry)
{
    return spawn_probe_arg(task, name, entry, NULL);
}

/* 同上，但把 arg 交给探针线程（D 组有的探针要拿参数）。
 * ★ 参数必须是**静态**变量或全局量 ★ 探针可能在本函数返回之后才第一次跑，
 * 指到调用者栈上的东西就是悬空指针。 */
static struct fe_thread *spawn_probe_arg(struct fe_task *task, const char *name,
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
        /* 第一个建出来的当主线程：这样"父进程的 wait 会返回"这条性质
         * 也能一起验（那是 fe_process_on_thread_exit 的责任）。 */
        task->main_thread = th;
    }
    return th;
}

/* 等一个 volatile 标志变真，最多等 rounds 轮让出。 */
/* （wait_flag 在下面；这里再加一个"真的睡"的版本给时间窗口用。） */

/* 与 wait_flag 同一件事，但**每一轮睡 5 ms**。
 *
 * ★ 为什么需要第二个（不能只用轮让出的那个）★
 * 等待"睡着的线程被叫醒/睡到点"时，光让出是不够的：让出不会让时间前进
 * （实测过：连着让出二十万次也未必跨过一个节拍），而睡眠的到期判据是
 * **时间**（fe_sched_sleep_until 用绝对纳秒）。所以凡是要观察"时间窗口"
 * 的地方都必须真的睡，尺度沿用 killtest 已有的「5 ms × 20 轮」。 */
static int wait_flag_sleep(volatile u32 *flag, u32 rounds)
{
    for (u32 i = 0; i < rounds; i++) {
        if (*flag) {
            return 1;
        }
        fe_thread_sleep_ms(5);
    }
    return *flag ? 1 : 0;
}

/* ================================================================== */
/* D 组：先证明这四条缺陷今天真的存在（docs/13-tasks-and-kill.md §6.7） */
/*                                                                    */
/* ★ 这一版**只有自检、没有任何修复** ★ 所以它们应当如实变红。         */
/* ================================================================== */

/* ---- D1 的探针：阻塞在**通知对象**上 ----
 *
 * 这一处的登记是 `nt->waiter` 单槽（与等待表 waiters[] 是两套东西）。
 * `fe_thread_cancel` 今天只摘 wait_node 与端点 receiver，从不碰它，
 * 所以被取消的等待者会带着一根指向自己的指针死在通知对象里。 */
static volatile long g_d1_nt_handle;
static volatile u32  g_d1_entered;
static volatile i32  g_d1_status;
static void probe_d1_notify(void *arg)
{
    (void)arg;
    u64 bits = 0;
    g_d1_entered = 1;
    fe_status_t s = fe_notification_wait((fe_handle_t)g_d1_nt_handle, 0x1, &bits);
    g_d1_status = s;
    fe_thread_exit(0);
}

/* ---- D2② 的探针：阻塞在 fe_process_wait 上（等一个永不退出的子任务）----
 *
 * ★ 为什么不从 fe_task_current() 取任务 ★
 * 自检是在**内核任务**里跑的，而"拉起子进程再等它"的服务线程属于
 * **探针任务**——子任务句柄装在那张表里（与用户态的形态一致）。
 * 用 fe_task_current() 查句柄会查到自检自己那张空表上（实测：BADHANDLE）。 */
static struct fe_task *g_d2_probe_task;
static volatile long g_d2_child_handle;
static volatile u32  g_d2_wait_entered;
static volatile u32  g_d2_wait_done;
static volatile i32  g_d2_wait_status;
static void probe_d2_taskwait(void *arg)
{
    (void)arg;
    g_d2_wait_entered = 1;
    i32 code = 0;
    fe_status_t s = fe_process_wait(g_d2_probe_task,
                                    (fe_handle_t)g_d2_child_handle, &code);
    g_d2_wait_status = s;
    g_d2_wait_done = 1;
    fe_thread_exit(0);
}

/* ---- D3 的探针：睡很久，醒来之后让出（它必须能被"叫醒"）---- */
static volatile u32 g_d3_sleeper_up;
static void probe_d3_sleeper(void *arg)
{
    (void)arg;
    g_d3_sleeper_up = 1;
    fe_thread_sleep_ms(60000);
    for (;;) {
        fe_thread_yield();
    }
}

/* ---- D3 的反向对照 L：同样睡 60 s，但**没有被标记** ----
 * 它在窗口结束时必须**仍然 SLEEPING**，否则"100 ms 内醒了"可能是
 * "窗口长到谁都会醒"，断言就失去了分辨力。 */
static volatile u32 g_d3_control_up;
static void probe_d3_control(void *arg)
{
    (void)arg;
    g_d3_control_up = 1;
    fe_thread_sleep_ms(60000);
    for (;;) {
        fe_thread_yield();
    }
}

/* ---- D3 的反向对照 L2：睡 300 ms 之后自己退出 ----
 * 它证明"这套轮询能观察到睡到点"——窗口结束之后的对照段里，它必须
 * **自然**变成 DEAD。★ 它睡的时间必须**比探针窗口长、又比对照段短** ★
 * 第一版写的是 10 ms：它在"三个都睡着"这个前置条件成立之前就醒过了，
 * 于是前置条件永远不成立，整条断言变成了自相矛盾（"只有 2 个在睡"）。 */
static volatile u32 g_d3_short_up;
static void probe_d3_short(void *arg)
{
    (void)arg;
    g_d3_short_up = 1;
    fe_thread_sleep_ms(300);
    fe_thread_exit(0);
}

/* D3 的窗口：5 ms × 20 轮 = 100 ms。★ 不是实测值 ★
 * 尺度沿用 killtest 已有的「5 ms × 20 轮」有界轮询；探针睡的是 60 s，
 * 比它大三个数量级，所以"今天必失败"没有歧义。 */
#define D3_WINDOW_ROUNDS 20
/* 反向对照段：L2 睡 300 ms，所以这里最多等 400 × 5 ms = 2 s（有界）。 */
#define D3_CONTROL_ROUNDS 400

/* D2② 里"永不退出的子任务"：它没有任何线程，所以 `exited` 永远不置位
 * （fe_process_on_thread_exit 只在主线程退出时才置），等它等于永远等。
 * 句柄装进**探针任务**（owner）的句柄表——用户态里"拉起子进程再等它"的
 * 服务线程就是这样持有子任务句柄的。 */
static struct fe_task *make_childless_task(struct fe_task *owner,
                                           const char **out_reason)
{
    struct fe_task *child = fe_task_create_kernel("d2-childless");
    if (!child) {
        *out_reason = "子任务创建失败";
        return NULL;
    }
    fe_handle_t h = FE_HANDLE_INVALID;
    fe_object_ref(&child->hdr);         /* 这一次引用交给句柄表 */
    h = fe_handle_install(&owner->handles, &child->hdr,
                          FE_RIGHT_READ | FE_RIGHT_WAIT);
    if (h == FE_HANDLE_INVALID) {
        /* 安装失败：把刚加上去的那次引用还掉，再放创建者引用 */
        fe_object_unref(&child->hdr);
        fe_object_unref(&child->hdr);
        *out_reason = "子任务句柄安装失败";
        return NULL;
    }
    fe_object_unref(&child->hdr);       /* 放掉创建者引用；句柄持有另一次 */
    g_d2_child_handle = (long)h;
    return child;
}

u32 fe_selftest_kill(void)
{
    u32 fail = 0;

    /* ================= 1. 忙等中的线程（闸门那条路） ================= */
    {
        struct fe_task *task = fe_task_create_kernel("kill-spin");
        if (!task) {
            fe_kprintf("        探针任务创建失败\n");
            return 1;
        }
        g_spin_alive = 0;
        struct fe_thread *th = spawn_probe(task, "spin", probe_spin);
        if (!th) {
            fe_kprintf("        忙等探针线程创建失败\n");
            fe_object_unref(&task->hdr);
            return 1;
        }
        if (!wait_flag(&g_spin_alive, 20000)) {
            fe_kprintf("        忙等探针没有跑起来\n");
            fe_object_unref(&task->hdr);
            return 1;
        }
        /* ★ 先确认它**真的在消耗 CPU** ★ 否则"杀掉之后不再消耗"什么都证明不了。
         *
         * ★ 判据用线程自己的 cpu_ticks，而且必须**睡够一个节拍** ★
         * 第一版是"让出 2000 次，要求节拍数变大"。那个循环在一毫秒内就跑完了
         * （1000 Hz 的节拍根本来不及跳），于是 t1 == t0 —— 断言失败，
         * 而被测的机制完全是对的。**计数类断言必须等到计数有时间变化**，
         * 否则测的是"循环够不够慢"。 */
        u64 t0 = th->cpu_ticks;
        for (u32 i = 0; i < 20 && th->cpu_ticks == t0; i++) {
            fe_thread_sleep_ms(5);
        }
        u64 t1 = th->cpu_ticks;
        if (t1 == t0) {
            fe_kprintf("        忙等线程没有消耗任何节拍（%llu）—— 计不了时\n",
                       (unsigned long long)t0);
            fail++;
        }

        fe_task_terminate(task);
        CHECK(task->dying);
        /* 给它若干次调度机会走到闸门。 */
        for (u32 i = 0; i < 20000 && th->state != FE_THREAD_DEAD; i++) {
            fe_thread_yield();
        }
        if (th->state != FE_THREAD_DEAD) {
            fe_kprintf("        忙等线程没有被闸门拦下（状态 %u）\n", th->state);
            fail++;
        } else {
            /* ★ 反向对照：死了之后它的节拍**不再增长** ★
             *
             * 用线程自己的计数器而不是"任务下存活线程的合计"：
             * 死掉的线程已经不在枚举里了，合计会变成 0 —— 那是**恒真**的，
             * 什么都证明不了。而这个线程结构体在自检手里（内核线程
             * reap_ok=false，不会被回收），所以它能一直读。
             * 一个"状态是 DEAD 却还在跑"的线程会在这里露出来。 */
            u64 t2 = th->cpu_ticks;
            fe_thread_sleep_ms(30);
            u64 t3 = th->cpu_ticks;
            if (t3 != t2) {
                fe_kprintf("        被终止的线程仍在消耗 CPU：%llu → %llu 节拍\n",
                           (unsigned long long)t2, (unsigned long long)t3);
                fail++;
            } else {
                fe_kprintf("        忙等线程被终止并停止占用 CPU"
                           "（终止前 %llu 节拍，之后 30 ms 内不再增长）\n",
                           (unsigned long long)t2);
            }
        }
        fe_object_unref(&task->hdr);
    }

    /* ================= 2. 阻塞在收消息上（取消点：端点 receiver） ============ */
    {
        struct fe_task *task = fe_task_create_kernel("kill-recv");
        if (!task) {
            fe_kprintf("        探针任务创建失败\n");
            return fail + 1;
        }
        g_probe_ep = NULL;
        g_probe_ep_handle = -1;
        if (fe_failed(fe_endpoint_create(task, 0, (fe_handle_t *)&g_probe_ep_handle))) {
            fe_kprintf("        端点创建失败\n");
            fe_object_unref(&task->hdr);
            return fail + 1;
        }
        g_recv_entered = 0;
        g_recv_result = 0;
        g_recv_status = 0;
        if (!spawn_probe(task, "recv", probe_recv) ||
            !wait_flag(&g_recv_entered, 20000)) {
            fe_kprintf("        接收探针没有跑起来\n");
            fe_object_unref(&task->hdr);
            return fail + 1;
        }
        /* 等它真的阻塞下去（第一次让出之后状态就该是 BLOCKED）。 */
        {
            struct fe_thread *th = fe_task_thread_first(task);
            for (u32 i = 0; i < 20000 && th && th->state != FE_THREAD_BLOCKED; i++) {
                fe_thread_yield();
                th = fe_task_thread_first(task);
            }
            if (!th || th->state != FE_THREAD_BLOCKED) {
                fe_kprintf("        接收探针没有进入阻塞（状态 %u）\n",
                           th ? th->state : 0);
                fail++;
            } else {
                fe_task_terminate(task);
                for (u32 i = 0; i < 20000 && !g_recv_result; i++) {
                    fe_thread_yield();
                }
                /* ★ 两条都要成立 ★
                 * 返回了（说明它被唤醒，没有永远挂在等待表上），
                 * 而且带的是 CANCELED（说明不是被别的消息唤醒的）。 */
                CHECK(g_recv_result == 1);
                CHECK(g_recv_status == FE_ERR_CANCELED);
                fe_kprintf("        阻塞在收消息上的线程被唤醒并返回 %d"
                           "（FE_ERR_CANCELED = %d）\n",
                           (int)g_recv_status, (int)FE_ERR_CANCELED);
            }
        }
        fe_object_unref(&task->hdr);
    }

    /* ================= 3. 阻塞在 wait_any 上（取消点：wait_node） ============ */
    {
        struct fe_task *task = fe_task_create_kernel("kill-wait");
        if (!task) {
            fe_kprintf("        探针任务创建失败\n");
            return fail + 1;
        }
        long ep = -1;
        if (fe_failed(fe_endpoint_create(task, 0, (fe_handle_t *)&ep))) {
            fe_kprintf("        端点创建失败\n");
            fe_object_unref(&task->hdr);
            return fail + 1;
        }
        g_probe_ep_handle = ep;
        g_wait_entered = 0;
        g_wait_result = 0;
        g_wait_status = 0;
        if (!spawn_probe(task, "wait", probe_wait) ||
            !wait_flag(&g_wait_entered, 20000)) {
            fe_kprintf("        wait_any 探针没有跑起来\n");
            fe_object_unref(&task->hdr);
            return fail + 1;
        }
        {
            struct fe_thread *th = fe_task_thread_first(task);
            for (u32 i = 0; i < 20000 && th && th->state != FE_THREAD_BLOCKED; i++) {
                fe_thread_yield();
                th = fe_task_thread_first(task);
            }
            if (!th || th->state != FE_THREAD_BLOCKED) {
                fe_kprintf("        wait_any 探针没有进入阻塞（状态 %u）\n",
                           th ? th->state : 0);
                fail++;
            } else {
                /* 登记必须真的存在：waiting 与 wait_node 是唤醒方要读的两样。 */
                CHECK(th->waiting == 1);
                CHECK(th->wait_node != NULL);
                fe_task_terminate(task);
                for (u32 i = 0; i < 20000 && !g_wait_result; i++) {
                    fe_thread_yield();
                }
                CHECK(g_wait_result == 1);
                CHECK(g_wait_status == FE_ERR_CANCELED);
                /* ★ 摘除必须干净 ★ 唤醒方以后再也不会看到这个线程的指针。 */
                if (th->waiting || th->wait_node) {
                    fe_kprintf("        取消之后登记没有摘干净"
                               "（waiting=%u wait_node=%p）\n",
                               th->waiting, th->wait_node);
                    fail++;
                } else {
                    fe_kprintf("        wait_any 登记者被取消并摘除干净，返回 %d\n",
                               (int)g_wait_status);
                }
            }
        }
        fe_object_unref(&task->hdr);
    }

    /* ================= 反向对照：杀不掉的必须被拒 ================= */
    {
        struct fe_task *task = fe_task_create_kernel("kill-perm");
        if (task) {
            /* 只给 READ，**不给 TERMINATE**。syscall 那一层会按权限位拒；
             * 这里直接验权限位本身（内核层面的判据）。 */
            fe_handle_t h = FE_HANDLE_INVALID;
            fe_object_ref(&task->hdr);
            h = fe_handle_install(&fe_task_current()->handles, &task->hdr,
                                  FE_RIGHT_READ | FE_RIGHT_WAIT);
            fe_object_unref(&task->hdr);
            CHECK(h != FE_HANDLE_INVALID);
            struct fe_object_header *obj = NULL;
            fe_status_t s = fe_handle_lookup(&fe_task_current()->handles, h,
                                             FE_RIGHT_TERMINATE, &obj);
            CHECK(fe_failed(s));        /* 没有那一位 → 查不到 */
            if (!fe_failed(s)) {
                fe_kprintf("        没有 TERMINATE 权限的句柄竟然查得到\n");
            }
            fe_handle_close(&fe_task_current()->handles, h);
            fe_object_unref(&task->hdr);
        }
    }

    /* ================= 反向对照：终止一个已经死透的任务是安全的 ========== */
    {
        struct fe_task *task = fe_task_create_kernel("kill-dead");
        if (task) {
            fe_task_terminate(task);        /* 它本来就没有线程 */
            fe_task_terminate(task);        /* 再来一次：幂等，不该崩 */
            CHECK(task->dying);
            fe_kprintf("        终止一个没有线程的任务是安全的（可重复调用）\n");
            fe_object_unref(&task->hdr);
        }
    }

    /* ================= 4. 自我终止：整个任务结束 ==========================
     *
     * ★ 这一条**只能在核心里验** ★ 用户态拿不到"指向自己的任务句柄"
     * （没有"取自己的句柄"这种 syscall，也不该有——那是把一个能力
     * 白白发出去）。所以"终止自己"这条语义只能在这里构造：
     * 探针线程拿到 `fe_task_current()`，直接调内核函数。 */
    {
        struct fe_task *task = fe_task_create_kernel("kill-self");
        if (!task) {
            fe_kprintf("        探针任务创建失败\n");
            return fail + 1;
        }
        g_self_entered = 0;
        if (!spawn_probe(task, "self", probe_self) ||
            !wait_flag(&g_self_entered, 20000)) {
            fe_kprintf("        自我终止探针没有跑起来\n");
            fe_object_unref(&task->hdr);
            return fail + 1;
        }
        /* 它自己把任务置成了 dying，然后在下一次经过闸门时死掉。 */
        CHECK(task->dying);
        /* 等到它真的死透。★ 判据是"任务不再有存活线程" ★
         * 只看 dying 是不够的——那一位在调用 terminate 的那一刻就是真了，
         * 而线程可能还没走到闸门。 */
        u32 alive = 1;
        for (u32 i = 0; i < 20000 && alive; i++) {
            fe_thread_yield();
            alive = fe_task_thread_count(task);
        }
        if (alive) {
            fe_kprintf("        自我终止的线程没有走到闸门（仍有 %u 个存活线程）\n",
                       alive);
            fail++;
        } else {
            CHECK(task->exited);        /* 主线程死亡登记生效 */
            fe_kprintf("        自我终止：任务的所有线程都已结束，"
                       "父进程方向的 exited 也已置位\n");
        }
        fe_object_unref(&task->hdr);
    }

    /* ================================================================== */
    /* D 组：先证缺陷存在（docs/13-tasks-and-kill.md §6.7）                */
    /*                                                                    */
    /* ★ 这一版只有自检、没有修复 ★ 所以它们应当如实变红。                */
    /* ================================================================== */

    /* ---------- D1：通知对象的 waiter 槽没有摘除路径 ---------- */
    {
        struct fe_task *task = fe_task_create_kernel("d1-notify");
        struct fe_notification *nt = NULL;
        if (!task) {
            fe_kprintf("        探针任务创建失败\n");
            return fail + 1;
        }
        long h = -1;
        if (fe_failed(fe_notification_create(task, (fe_handle_t *)&h))) {
            fe_kprintf("        通知对象创建失败\n");
            fe_object_unref(&task->hdr);
            return fail + 1;
        }
        struct fe_object_header *nobj = NULL;
        if (fe_failed(fe_handle_lookup(&task->handles, (fe_handle_t)h, 0, &nobj))) {
            fe_kprintf("        通知对象句柄查不到\n");
            fe_object_unref(&task->hdr);
            return fail + 1;
        }
        nt = FE_OBJ_OF(nobj, struct fe_notification);
        /* ★ 观察物必须活得比被终止的任务久 ★
         * 自检自己的任务也持一个句柄（第三方持有者）：探针任务被销毁时
         * 它的句柄表会被清空（task.c 的 fe_object_destroy），只有这第二个
         * 引用能让通知对象活到断言那一刻。 */
        fe_object_ref(&nt->hdr);
        fe_handle_t keep_h = fe_handle_install(&fe_task_current()->handles, &nt->hdr,
                                               FE_RIGHT_WAIT | FE_RIGHT_SIGNAL);
        fe_object_unref(&nt->hdr);
        if (keep_h == FE_HANDLE_INVALID) {
            fe_kprintf("        第三方持有者句柄安装失败\n");
            fe_object_unref(&task->hdr);
            return fail + 1;
        }

        g_d1_nt_handle = h;
        g_d1_entered = 0;
        g_d1_status = 0;
        struct fe_thread *w = spawn_probe(task, "d1-waiter", probe_d1_notify);
        if (!w || !wait_flag(&g_d1_entered, 20000)) {
            fe_kprintf("        D1 等待者没有跑起来\n");
            fe_object_unref(&task->hdr);
            return fail + 1;
        }
        /* 同步点：等到它**真的**登记进通知对象（waiter 是单槽，看得见）。 */
        if (!wait_flag_sleep(&g_d1_entered, 20) || nt->waiter != w) {
            fe_kprintf("        D1 等待者没有登记到通知对象上（waiter=%p want=%p）\n",
                       (void *)nt->waiter, (void *)w);
            fail++;
        } else {
            /* 反向对照（今天也通过）：这一刻槽里**确实**是它——
             * 所以后面读到的非 NULL 不能是别的东西。 */
            fe_kprintf("        D1 正向：等待者已登记（nt->waiter == 探针）\n");
            /* ★ 走 fe_task_kill_other_threads——那才是 exec 走的路 ★
             * 这个任务只有 w 一个线程，keep 传自检自己的线程（它不属于这个
             * 任务，所以遍历不到）：标记数必须是 1，而且**自检自己不受影响**。 */
            CHECK(fe_task_kill_other_threads(task, fe_thread_current()) == 1);
            CHECK(fe_ok(fe_task_wait_others_dead(task, 20000)));
            CHECK(fe_task_thread_count(task) == 0);
            /* ★ 被测的那一条 ★ 受害者死透之后 nt->waiter 必须已经被摘掉。
             * 没修之前没人摘 ⇒ 非 NULL ⇒ 失败。
             * 只比较指针**值**、不解引用，所以"失败"本身是安全可观察的。 */
            CHECK(fe_notification_waiter(nt) == NULL);
            if (fe_notification_waiter(nt)) {
                fe_kprintf("        **D1 失败**：等待者已死，通知对象里仍留着"
                           "指向它的 waiter（%p，已死线程）——之后 signal 会唤醒"
                           "它的状态字段\n", (void *)fe_notification_waiter(nt));
            } else {
                fe_kprintf("        D1 修复后：取消之后 nt->waiter 已被摘成 NULL\n");
            }
        }
        fe_object_unref(&task->hdr);
        fe_handle_close(&fe_task_current()->handles, keep_h);

        /* 反向对照①（今天也通过）：**正常唤醒**路径会把槽清掉。
         * 它证明 `nt->waiter == NULL` 这条判据不是恒假——今天它只在
         * "有匹配位被置位"那条路上成立，取消路径上不成立。 */
        struct fe_task *t2 = fe_task_create_kernel("d1-normal");
        if (!t2) {
            fe_kprintf("        探针任务创建失败\n");
            fail++;
        } else {
            long h2 = -1;
            if (fe_ok(fe_notification_create(t2, (fe_handle_t *)&h2))) {
                struct fe_object_header *nobj2 = NULL;
                if (fe_failed(fe_handle_lookup(&t2->handles, (fe_handle_t)h2, 0,
                                               &nobj2))) {
                    fe_kprintf("        D1 反向①：通知句柄查不到\n");
                    fail++;
                    fe_object_unref(&t2->hdr);
                } else {
                    struct fe_notification *nt2 =
                        FE_OBJ_OF(nobj2, struct fe_notification);
                    g_d1_nt_handle = h2;
                    g_d1_entered = 0;
                    g_d1_status = 0;
                    struct fe_thread *w2 = spawn_probe(t2, "d1-normal",
                                                       probe_d1_notify);
                    if (w2 && wait_flag_sleep(&g_d1_entered, 20) &&
                        nt2->waiter == w2) {
                        fe_notification_signal_obj(nt2, 0x1);
                        u32 done = 0;
                        for (u32 i = 0; i < 20000 && !done; i++) {
                            fe_thread_yield();
                            done = (g_d1_status != 0);
                        }
                        CHECK(g_d1_status == FE_OK);
                        CHECK(nt2->waiter == NULL);
                        fe_kprintf("        D1 反向①：正常唤醒会清掉槽"
                                   "（waiter=NULL，返回 %d）——判据不是恒假\n",
                                   (int)g_d1_status);
                    } else {
                        fe_kprintf("        D1 反向①：等待者没有登记上\n");
                        fail++;
                    }
                    fe_object_unref(&t2->hdr);
                }
            } else {
                fe_kprintf("        通知对象创建失败\n");
                fail++;
            }
        }
    }

    /* ---------- D2②：fe_process_wait 没有取消点 ---------- */
    {
        struct fe_task *task = fe_task_create_kernel("d2-taskwait");
        const char *why = "?";
        if (!task) {
            fe_kprintf("        探针任务创建失败\n");
            return fail + 1;
        }
        struct fe_task *child = make_childless_task(task, &why);
        if (!child) {
            fe_kprintf("        D2②: %s\n", why);
            fe_object_unref(&task->hdr);
            return fail + 1;
        }
        g_d2_probe_task = task;
        g_d2_wait_entered = 0;
        g_d2_wait_done = 0;
        g_d2_wait_status = 0;
        struct fe_thread *b = spawn_probe(task, "d2-waiter", probe_d2_taskwait);
        if (!b || !wait_flag(&g_d2_wait_entered, 20000)) {
            fe_kprintf("        D2② 等待者没有跑起来\n");
            fe_object_unref(&task->hdr);
            fe_object_unref(&child->hdr);
            return fail + 1;
        }
        /* ★ 先证明探针处在预期状态，再谈缺陷 ★
         * 判据是它自己报的"还没返回"（done）与线程状态 BLOCKED 同时成立；
         * 轮询用"睡 1 ms"而不是"让出"——让出只在当前线程自己的时间片里转，
         * 被唤醒的线程可能一次都选不上（D2① 上实测过）。 */
        struct fe_thread *cur_b = b;
        u32 spins = 0;
        for (u32 i = 0; i < 200 && cur_b && cur_b->state != FE_THREAD_BLOCKED; i++) {
            fe_thread_sleep_ms(1);
            spins++;
            cur_b = fe_task_thread_first(task);
        }
        if (!cur_b || cur_b->state != FE_THREAD_BLOCKED) {
            fe_kprintf("        D2② 等待者没有进入阻塞（%u 轮后状态 %u，"
                       "fe_process_wait 返回 %d done=%u）\n",
                       spins, cur_b ? cur_b->state : 0, (int)g_d2_wait_status,
                       (u32)g_d2_wait_done);
            fail++;
        } else {
            /* 证据行：探针此刻确实阻塞在 fe_process_wait 里。 */
            fe_kprintf("        D2② 前置状态：等待者 BLOCKED（%u 轮后），"
                       "child->exited=%u 且 done=%u\n",
                       spins, (u32)child->exited, (u32)g_d2_wait_done);
            CHECK(g_d2_wait_done == 0);
            CHECK(child->exited == false);
            CHECK(fe_task_kill_other_threads(task, fe_thread_current()) == 1);
            u32 waited = fe_task_wait_others_dead(task, 20000);
            /* ★ 再给一个有界窗口：wait_others_dead 的让出**不保证**唤醒的
             * 线程被选中（实测：它可能一直停在就绪队列上，直到某个节拍把它
             * 推上去）。 */
            for (u32 i = 0; i < 20 && fe_task_thread_count(task) != 0; i++) {
                fe_thread_sleep_ms(2);
            }
            u32 alive = fe_task_thread_count(task);
            /* ★ 缺陷期的形态 ★ 受害者死在闸门上，而这次 fe_process_wait
             * **永远不返回**——循环顶没有取消判据，它留在 child->waiter 上的
             * 指针一直没摘。 */
            if (g_d2_wait_done == 0 || g_d2_wait_status != FE_ERR_CANCELED) {
                fe_kprintf("        **D2② 缺陷**：等待者已 DEAD=%u，但"
                           "fe_process_wait %s（done=%u status=%d，期望 %d）"
                           "——循环顶没有取消判据\n",
                           alive == 0 ? 1u : 0u,
                           g_d2_wait_done ? "返回了别的码" : "永远没有返回",
                           (u32)g_d2_wait_done, (int)g_d2_wait_status,
                           (int)FE_ERR_CANCELED);
                fail++;
            } else {
                fe_kprintf("        D2② 修复后：fe_process_wait 返回 %d"
                           "（FE_ERR_CANCELED），等待者已死透\n",
                           (int)g_d2_wait_status);
            }
            CHECK(waited == FE_OK);
            CHECK(alive == 0);
            CHECK(g_d2_wait_done == 1);
            CHECK(g_d2_wait_status == FE_ERR_CANCELED);
            /* 反向对照：目标任务上的 waiter 槽不许留着已死的线程 */
            CHECK(child->waiter == NULL);
        }
        fe_object_unref(&task->hdr);
        fe_object_unref(&child->hdr);
    }

    /* ---------- D3：睡着的线程叫不醒（fe_sched_wake 只认 BLOCKED） ---------- */
    {
        struct fe_task *task = fe_task_create_kernel("d3-sleeper");
        struct fe_task *ctl  = fe_task_create_kernel("d3-control");
        if (!task || !ctl) {
            fe_kprintf("        探针任务创建失败\n");
            return fail + 1;
        }
        g_d3_sleeper_up = 0;
        g_d3_control_up = 0;
        g_d3_short_up = 0;
        struct fe_thread *s  = spawn_probe(task, "d3-sleep", probe_d3_sleeper);
        struct fe_thread *l  = spawn_probe(ctl,  "d3-ctrl",  probe_d3_control);
        struct fe_thread *l2 = spawn_probe_arg(ctl, "d3-short", probe_d3_short, NULL);
        if (!s || !l || !l2) {
            fe_kprintf("        D3 探针线程创建失败\n");
            fe_object_unref(&task->hdr);
            fe_object_unref(&ctl->hdr);
            return fail + 1;
        }
        /* 三个都进入睡眠之后才开窗口，否则窗口里量的是"还没睡下去"。 */
        if (!wait_flag_sleep(&g_d3_sleeper_up, 20) ||
            !wait_flag_sleep(&g_d3_control_up, 20) ||
            !wait_flag_sleep(&g_d3_short_up, 20)) {
            fe_kprintf("        D3 探针没有跑起来\n");
            fe_object_unref(&task->hdr);
            fe_object_unref(&ctl->hdr);
            return fail + 1;
        }
        /* ★ 先证明探针处在预期状态，再谈缺陷 ★
         * 三个探针都必须真的落到 SLEEPING（登记在睡眠链上才是"叫不醒"的
         * 那个状态）。判据用状态值本身，并把读数打出来当证据。 */
        u32 slept = 0;
        for (u32 i = 0; i < 40; i++) {
            slept = 0;
            if (s->state == FE_THREAD_SLEEPING)   { slept++; }
            if (l->state == FE_THREAD_SLEEPING)   { slept++; }
            if (l2->state == FE_THREAD_SLEEPING)  { slept++; }
            if (slept == 3) {
                break;
            }
            fe_thread_sleep_ms(5);
        }
        if (slept < 3) {
            fe_kprintf("        D3 前置状态不成立：探针没有都进入睡眠"
                       "（s=%u l=%u l2=%u）\n", s->state, l->state, l2->state);
            fail++;
        } else {
            fe_kprintf("        D3 前置状态：s/l/l2 都是 SLEEPING（%u）\n",
                       FE_THREAD_SLEEPING);
            CHECK(fe_task_kill_other_threads(task, fe_thread_current()) == 1);
            /* ★ 100 ms 窗口：有界轮询（5 ms × 20 轮）★ */
            u32 dead_round = 0;
            for (u32 i = 0; i < D3_WINDOW_ROUNDS; i++) {
                if (s->state == FE_THREAD_DEAD) {
                    dead_round = i + 1;
                    break;
                }
                fe_thread_sleep_ms(5);
            }
            CHECK(s->state == FE_THREAD_DEAD);
            if (s->state != FE_THREAD_DEAD) {
                fe_kprintf("        **D3 失败**：睡 60 s 的线程被标记后 %u ms 内"
                           "仍是状态 %u（fe_sched_wake 对 SLEEPING 是空操作）\n",
                           D3_WINDOW_ROUNDS * 5u, s->state);
            } else {
                fe_kprintf("        D3 修复后：睡 60 s 的线程在第 %u 轮"
                           "（≈%u ms）内变成 DEAD\n", dead_round, dead_round * 5u);
            }
            /* ★ 反向对照：窗口本身有分辨力 ★
             * ① 同样睡 60 s 但没被杀的 L 必须**仍然 SLEEPING**；
             * ② 睡 300 ms 的 L2 必须在随后的对照段里**自然**变成 DEAD
             *    （证明这套轮询看得见"睡到点"，不是永远读到 SLEEPING）。 */
            CHECK(l->state == FE_THREAD_SLEEPING);
            if (l->state != FE_THREAD_SLEEPING) {
                fe_kprintf("        **D3 反向对照失效**：没被杀的睡眠线程状态变成了"
                           " %u（窗口长到任何线程都会醒，正向断言就没有意义了）\n",
                           l->state);
                fail++;
            } else {
                fe_kprintf("        D3 反向对照①：没被杀的睡眠线程在窗口结束时"
                           "仍 SLEEPING（%u）\n", l->state);
            }
            u32 ctl_round = 0;
            for (u32 i = 0; i < D3_CONTROL_ROUNDS && l2->state != FE_THREAD_DEAD; i++) {
                fe_thread_sleep_ms(5);
                ctl_round++;
            }
            CHECK(l2->state == FE_THREAD_DEAD);
            if (l2->state != FE_THREAD_DEAD) {
                fe_kprintf("        **D3 反向对照失效**：睡 300 ms 的线程在 %u ms 的"
                           "对照段里没有变成 DEAD（这套轮询看不到睡眠到期，"
                           "状态 %u）\n", ctl_round * 5u, l2->state);
                fail++;
            } else {
                fe_kprintf("        D3 反向对照②：睡 300 ms 的线程在 %u ms 后自然"
                           "变成 DEAD（轮询看得见睡到点）\n", ctl_round * 5u);
            }
        }
        fe_object_unref(&task->hdr);
        fe_object_unref(&ctl->hdr);
    }

    return fail;
}
