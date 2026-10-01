/* SPDX-License-Identifier: 0BSD */
/* killtest —— 进程终止（K2）的**用户态承重测试**。
 *
 * 它同时扮演两个角色，靠命令行切换：
 *   --spin              被终止的一方：一直空转让出，永远不自己退出
 *   --pub <名字>        被终止的一方：发布一个 devfs 名字后空转，
 *                       用来验证"杀掉之后资源真的回收了"
 *   （无参数）          监督者：拉起上面两种，然后把它们终止掉
 *
 * ★ 为什么被终止的一方是**它自己** ★
 * 需要一个"永远不退出"的程序来被杀。项目里没有这样的程序（每个客户端
 * 都自己退），而写一个只为被杀而存在的独立程序会把"怎么用"和"怎么测"
 * 拆到两个文件里——这个项目已经吃过"测试自己也错了"的亏（见
 * docs/12-drivers.md §9.4）。同一个可执行文件带参数分饰两角，
 * 保证两边看到的协议是同一份。
 *
 * ★ 要验的四件事（每一件都能失败）★
 *   1. **忙等的孩子被杀掉**，退出码是 FE_ERR_KILLED(-22)，而且
 *      `wait` 真的返回了（说明闸门把"主线程死亡"这件事也办了——
 *      否则父进程会永远等下去）；
 *   2. **被终止的任务不再消耗 CPU**：从 K3 的任务表里取它的 cpu_ticks，
 *      杀之前必须增长、杀之后必须不再变。★ 这一条是关键 ★
 *      "不再打印"证明不了任何事——一个被杀的任务与一个恰好没被调度的
 *      任务从外部看完全一样，只有计数能把它们分开；
 *   3. **资源真的回收了**：孩子发布过 /dev/killme0。杀掉 + wait + 关句柄
 *      之后，那个名字必须打不开（devfs 名字随任务销毁回收）。
 *      ★ 反向对照在前半句 ★ 杀之前它必须**打得开**，
 *      否则"杀之后打不开"可能只是名字从来没发布成功；
 *   4. **反向对照：杀不掉的必须被拒** —— 坏句柄 BADHANDLE、
 *      非任务对象 INVAL、以及一个**没有 TERMINATE 权限**的句柄必须被拒。
 *      最后一条这样构造：把孩子句柄经消息转交给另一个任务，
 *      收方拿到的权限是发送方权限减去 TRANSFER——所以 TERMINATE
 *      仍然在。真正能构造"没有该权限"的办法是让内核给一个不含它的句柄，
 *      而用户态拿不到这种句柄，于是这一条在内核自检里验（见
 *      kernel/task/killtest.c），这里用前两条。
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
    if (!cond) {
        g_fail++;
    }
}

static char g_prefix[32];

/* 拼一个本槽里的程序路径：<prefix>/bin/<name> */
static void slot_path(const char *name, char *out, u32 cap)
{
    u32 k = 0;
    const char *parts[3];
    parts[0] = g_prefix;
    parts[1] = "/bin/";
    parts[2] = name;
    for (u32 p = 0; p < 3; p++) {
        for (const char *s = parts[p]; *s && k < cap - 1; s++) {
            out[k++] = *s;
        }
    }
    out[k] = '\0';
}

/* 快照缓冲（K3 的表）。按宏算大小，见 fe_user.h 的布局说明。 */
#define TASK_CAP 32
#define THREAD_CAP 64
#define SNAP_BYTES (sizeof(struct fe_task_list) + \
                    TASK_CAP * FE_TASK_STRIDE + THREAD_CAP * FE_THREAD_STRIDE)

static u8 g_buf[SNAP_BYTES] __attribute__((aligned(8)));

/* ★ 为什么按"同名任务有几个（不含自己）"来统计，而不是按名字找"那一个" ★
 *
 * 孩子与监督者是**同一个可执行文件**（分饰两角），所以它们的任务名
 * 都是 "killtest" ——按名字找"孩子"，找到的第一个很可能是监督者自己。
 * 第一版就是这么错的：它断言"孩子从任务表里消失"，而那个名字（它自己）
 * 永远不会消失，于是三条断言全红。
 *
 * 换成计数之后身份问题就消失了：拉一个孩子 → 计数 +1；孩子销毁 →
 * 计数回到原位。**用可测量的量代替对身份的假设。** */
static u64 g_me;

static u32 count_named(const char *name, u64 *out_ticks)
{
    struct fe_task_view v;
    u32 n = 0;
    u64 ticks = 0;
    if (fe_task_view_get(&v, g_buf, sizeof(g_buf), TASK_CAP, THREAD_CAP) < 0) {
        return 0;
    }
    if (v.hdr->current_task) {
        g_me = v.hdr->current_task;
    }
    for (u32 i = 0; i < v.hdr->task_count; i++) {
        const struct fe_task_info *t = fe_task_at(&v, i);
        if (!t || t->id == g_me || strcmp(t->name, name) != 0) {
            continue;
        }
        n++;
        ticks += t->cpu_ticks;
    }
    if (out_ticks) {
        *out_ticks = ticks;
    }
    return n;
}

/* 等同名任务数降到不超过 want 为止。 */
static int wait_count(const char *name, u32 want, u32 rounds)
{
    for (u32 i = 0; i < rounds; i++) {
        if (count_named(name, NULL) <= want) {
            return 1;
        }
        fe_sleep_ms(2);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 被终止的那两方                                                      */
/* ------------------------------------------------------------------ */

/* 空转让出，永远不退出。★ 用让出而不是死循环 ★
 * 死循环会把同优先级的其它线程饿死，而我们要的是"它在跑"而不是"它霸占"。 */
static int child_spin(void)
{
    for (;;) {
        fe_yield();
    }
}

/* 发布一个 devfs 名字，然后同样空转。
 * 名字背后的端点由本进程持有——任务被销毁时内核会把它收回。 */
static int child_pub(const char *path)
{
    long ep = fe_endpoint_create(0);
    if (ep <= 0) {
        say("  [child] 端点创建失败\n");
        flush();
        return 1;
    }
    long r = fe_devfs_publish(path, ep);
    if (r != FE_OK) {
        say("  [child] 发布失败：");
        num((u64)(-r));
        say("\n");
        flush();
        return 1;
    }
    say("  [child] 已发布 ");
    say(path);
    say("\n");
    flush();
    return child_spin();
}

/* 发布一个名字，然后**阻塞在收消息上**（永远等不到消息）。
 *
 * ★ 这一条走的是与"忙等"完全不同的内核路径 ★
 * 忙等中的线程是被闸门（回到用户态之前）拦下的；而阻塞中的线程根本
 * 走不到闸门——它必须由内核的**取消点**唤醒（把它从端点的 receiver
 * 槽上摘下来 + 唤醒，然后那次 recv 以 FE_ERR_CANCELED 返回）。
 * 两条路各自会坏，所以必须分别验。 */
static int child_recv(const char *path)
{
    long ep = fe_endpoint_create(0);
    if (ep <= 0) {
        return 1;
    }
    if (fe_devfs_publish(path, ep) != FE_OK) {
        return 1;
    }
    say("  [child] 已发布并阻塞在收消息上 ");
    say(path);
    say("\n");
    flush();
    struct fe_msg_header hdr;
    /* 没有任何人会往这个端点发消息，所以这里会一直阻塞。
     * 被终止时它应当以 FE_ERR_CANCELED 返回——但返回值观察不到，
     * 因为线程紧接着就在返回用户态的路上死了。 */
    (void)fe_endpoint_recv(ep, &hdr, NULL, 0, NULL, NULL);
    say("  [child] **不该看到这一行**：阻塞的 recv 居然正常返回了\n");
    flush();
    return 0;
}

/* ------------------------------------------------------------------ */
/* 监督者                                                              */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    fe_slot_prefix((argc > 0 && argv) ? argv[0] : 0, g_prefix, sizeof(g_prefix));

    if (argc >= 2 && strcmp(argv[1], "--spin") == 0) {
        return child_spin();
    }
    if (argc >= 3 && strcmp(argv[1], "--pub") == 0) {
        return child_pub(argv[2]);
    }
    if (argc >= 3 && strcmp(argv[1], "--recv") == 0) {
        return child_recv(argv[2]);
    }

    say("\n=== 进程终止（K2：闸门 + 取消点 + 资源回收）===\n");

    char self[96];
    slot_path("killtest", self, sizeof(self));

    /* ================= 1. 忙等的孩子：杀掉 + 退出码 + 停止消耗 CPU ======= */
    {
        /* 基线：此刻有几个叫 killtest 的任务（不含我自己）——正常情况下是 0。 */
        u32 base = count_named("killtest", NULL);
        char *ca[3];
        ca[0] = self;
        ca[1] = "--spin";
        ca[2] = (char *)0;
        long h = fe_spawn(self, ca, 2);
        check(h > 0, "拉起一个忙等的孩子（killtest --spin）");
        if (h > 0) {
            check(wait_count("killtest", base + 1, 200),
                  "孩子在任务表里出现（K3 的表能用来看它）");
            u64 t0 = 0, t1 = 0;
            u32 c0 = count_named("killtest", &t0);
            /* ★ 等到计数**真的动了**为止，而不是"睡一觉再看一眼" ★
             *
             * 原来是"睡 40 ms，要求累计节拍变大"。那个断言把正确性押在
             * "40 ms 内一定会有节拍"上——而 VBox 实测节拍是**突发投递**的
             * （同一个 8254 源：被动 50 ms 窗口只数到 5 个，而睡眠期间
             * 却能以 ~3.5 kHz 前进）。突发的间隙里 40 ms 可能一个节拍都没有，
             * 于是断言失败，而被测的机制完全正确。
             *
             * 这和内核自检里踩过的是同一个坑（"让出 2000 次要求计数变大"）：
             * **计数类断言必须等到计数有时间变化**，否则测的是节拍源，
             * 不是被测对象。 */
            int grew = 0;
            for (u32 i = 0; i < 20 && !grew; i++) {
                fe_sleep_ms(50);
                u32 cn = count_named("killtest", &t1);
                if (cn == base + 1 && t1 > t0) {
                    grew = 1;
                }
                c0 = cn;
            }
            u32 c1 = count_named("killtest", &t1);
            /* ★ 先证明它**真的在跑** ★ 否则下面"杀掉之后不再增长"
             * 可能只是因为它在被杀之前就没在跑。 */
            check(grew && c0 == base + 1 && c1 == base + 1,
                  "孩子确实在消耗 CPU（杀之前同名任务的累计节拍在增长）");

            long r = fe_task_terminate(h);
            check(r == FE_OK, "终止孩子（返回 FE_OK）");

            i32 code = 0;
            long w = fe_wait(h, &code);
            /* ★ wait 必须返回 ★ 被终止任务的**主线程**死亡要走
             * fe_process_on_thread_exit 唤醒父进程，否则父进程永远等下去。
             * 这一条同时验证了"闸门走的是与正常退出同一条死亡登记"。 */
            check(w >= 0, "等孩子结束（wait 返回了，说明死亡登记生效）");
            check(code == FE_ERR_KILLED,
                  "孩子的退出码是 FE_ERR_KILLED(-22)（被终止，不是自己退的）");
            if (code != FE_ERR_KILLED) {
                say("        实际退出码 ");
                num((u64)(u32)code);
                say("\n");
            }

            /* ★ 反向对照：不再消耗 CPU ★
             * ★ 这里**故意先不关句柄** ★ 任务对象在最后一个引用消失时
             * 才销毁，而父进程手里的句柄就是引用之一。所以此刻它还在表里，
             * 只是不再消耗 CPU——这一条恰好证明"终止 ≠ 销毁"：
             * 线程死了，但对象还在（等持有者放手）。 */
            u64 t2 = 0, t3 = 0;
            u32 c2 = count_named("killtest", &t2);
            /* 同样等到"该动的时候"：睡够几轮再看它有没有动。
             * 它必须**始终不动**——这一条的方向是"证明不变"，
             * 所以多睡几轮反而更强。 */
            for (u32 i = 0; i < 6; i++) {
                fe_sleep_ms(50);
            }
            u32 c3 = count_named("killtest", &t3);
            check(c2 == base + 1 && c3 == base + 1 && t3 == t2,
                  "杀掉之后它的累计节拍**不再增长**（线程真的停了，不是没被调度）");

            /* 幂等：再杀一次不报错、也不崩。 */
            check(fe_task_terminate(h) == FE_OK,
                  "重复终止同一个句柄是安全的（幂等）");

            /* ★ 关掉句柄 = 放掉最后一个引用 → 任务对象才销毁 ★
             * 销毁会把 devfs 名字 / 端口 / MMIO / 地址空间一并归还，
             * 并把任务从全局链摘掉。 */
            fe_handle_close(h);
            check(wait_count("killtest", base, 200),
                  "关掉句柄之后它才从任务表里消失（销毁由最后一个引用触发）");
            if (!wait_count("killtest", base, 1)) {
                say("        注意：句柄已关而任务仍在表里，说明回收链没走通\n");
            }
        }
    }

    /* ================= 2. 被终止的任务：资源真的回收了 ================= */
    {
        const char *node = "/dev/killme0";
        char *ca[4];
        u32 base = count_named("killtest", NULL);
        ca[0] = self;
        ca[1] = "--pub";
        ca[2] = (char *)node;
        ca[3] = (char *)0;
        long h = fe_spawn(self, ca, 3);
        check(h > 0, "拉起一个发布 devfs 名字的孩子");
        if (h > 0) {
            /* ★ 反向对照必须先成立 ★ 杀之前它必须打得开，
             * 否则"杀之后打不开"也可能只是名字从来没发布成功。 */
            long ep = -1;
            for (u32 i = 0; i < 200 && ep <= 0; i++) {
                ep = fe_devfs_open(node);
                if (ep <= 0) {
                    fe_sleep_ms(2);
                }
            }
            check(ep > 0, "杀之前：孩子发布的名字**打得开**（反向对照的前提）");
            if (ep > 0) {
                fe_handle_close(ep);
            }

            check(fe_task_terminate(h) == FE_OK, "终止这个孩子");
            i32 code = 0;
            long w = fe_wait(h, &code);
            check(w >= 0 && code == FE_ERR_KILLED, "它也是被终止结束的（-22）");
            /* ★ 必须关掉句柄 ★ 任务对象在最后一个引用消失时才销毁，
             * 而"资源回收"挂在那条路径上（fe_object_destroy 里归还
             * 端口/MMIO/IRQ/devfs 名字/地址空间）。父进程手里的这个句柄
             * 是最后一个引用——不关，任务对象还在，名字也还在。
             * 这一条本身是语义的一部分：**句柄持有者决定资源何时回收**。 */
            long ep0 = fe_devfs_open(node);
            check(ep0 > 0, "还没关句柄时名字**仍然在**（销毁由引用计数触发，不是终止触发）");
            if (ep0 > 0) {
                fe_handle_close(ep0);
            }
            fe_handle_close(h);

            check(wait_count("killtest", base, 200),
                  "关掉句柄后孩子从任务表里消失（回收链走通了）");
            long ep2 = fe_devfs_open(node);
            check(ep2 < 0, "杀掉并销毁之后：那个 devfs 名字**打不开了**（资源随任务回收）");
            if (ep2 > 0) {
                say("        名字还在，说明 devfs 名字没有随任务销毁回收\n");
                fe_handle_close(ep2);
            }
        }
    }

    /* ================= 3. 反向对照：杀不掉的必须被拒 ================= */
    {
        /* 坏句柄 */
        long r = fe_task_terminate(9999);
        check(r == FE_ERR_BADHANDLE, "反向：坏句柄被拒（BADHANDLE）");
        if (r != FE_ERR_BADHANDLE) {
            say("        实际返回 ");
            num((u64)(-r));
            say("\n");
        }
        /* 非任务对象：拿一个端点句柄去当任务句柄。
         *
         * ★ 期望是 ACCESS 而不是 INVAL，这一点值得写清楚 ★
         * 判据的顺序是"先按权限位查句柄、再看对象类型"。端点句柄上
         * 没有 FE_RIGHT_TERMINATE，所以查权限那一步就已经失败了——
         * 类型检查根本轮不到。也就是说：**一个不含所需权限的句柄，
         * 无论它指向什么，得到的都是"你没这个权限"**。
         * 第一版这里断言 INVAL，于是它必然失败（而那说明的是断言错了，
         * 不是内核错了）。 */
        long ep = fe_endpoint_create(0);
        check(ep > 0, "造一个端点句柄用于反向对照");
        if (ep > 0) {
            r = fe_task_terminate(ep);
            check(r == FE_ERR_ACCESS,
                  "反向：不含 TERMINATE 权限的句柄被拒（ACCESS，权限判在类型之前）");
            if (r != FE_ERR_ACCESS) {
                say("        实际返回 ");
                num((u64)(-r));
                say("\n");
            }
            fe_handle_close(ep);
        }
        /* 0 号句柄 */
        r = fe_task_terminate(0);
        check(r < 0, "反向：句柄 0 被拒");
    }

    /* ================= 4. 阻塞在收消息上的孩子：走取消点那条路 ========== */
    {
        const char *node = "/dev/killme1";
        u32 base = count_named("killtest", NULL);
        char *ca[4];
        ca[0] = self;
        ca[1] = "--recv";
        ca[2] = (char *)node;
        ca[3] = (char *)0;
        long h = fe_spawn(self, ca, 3);
        check(h > 0, "拉起一个阻塞在收消息上的孩子");
        if (h > 0) {
            long ep = -1;
            for (u32 i = 0; i < 200 && ep <= 0; i++) {
                ep = fe_devfs_open(node);
                if (ep <= 0) {
                    fe_sleep_ms(2);
                }
            }
            check(ep > 0, "它也把名字发布出来了（说明已经走到阻塞那一步附近）");
            if (ep > 0) {
                fe_handle_close(ep);
            }
            /* 等它真的进入阻塞：从任务表里看到 state = blocked。
             * ★ 不"睡一会儿假定它阻塞了" ★ 假定会让这条测试在慢机器上
             * 变成随机失败，而那时人只会去怀疑关闭机制。 */
            int blocked = 0;
            for (u32 i = 0; i < 300 && !blocked; i++) {
                struct fe_task_view v;
                if (fe_task_view_get(&v, g_buf, sizeof(g_buf),
                                     TASK_CAP, THREAD_CAP) >= 0) {
                    const struct fe_task_info *t = fe_task_find(&v, "killtest");
                    for (u32 k = 0; t && k < t->thread_count; k++) {
                        const struct fe_thread_info *th =
                            fe_thread_at(&v, t->thread_base + k);
                        if (th && th->state == FE_THREAD_BLOCKED) {
                            blocked = 1;
                            break;
                        }
                    }
                }
                if (!blocked) {
                    fe_sleep_ms(2);
                }
            }
            check(blocked, "孩子进入了阻塞态（K3 的表能看到它卡住了）");

            check(fe_task_terminate(h) == FE_OK, "终止一个**阻塞中**的孩子");
            i32 code = 0;
            long w = fe_wait(h, &code);
            /* ★ 这一条是取消点机制的端到端证据 ★
             * 如果没有取消点，这个线程会永远挂在端点的等待表上，
             * 于是 wait 永远不返回——测试会挂住而不是报失败。 */
            check(w >= 0 && code == FE_ERR_KILLED,
                  "阻塞中的孩子也被终止了（取消点把它唤醒，wait 返回了 -22）");
            fe_handle_close(h);
            check(wait_count("killtest", base, 200), "它从任务表里消失");
            long ep2 = fe_devfs_open(node);
            check(ep2 < 0, "它发布的名字也回收了");
            if (ep2 > 0) {
                fe_handle_close(ep2);
            }
        }
    }

    /* ================= 5. 没有 TERMINATE 权限的句柄 ====================
     *
     * ★ 这一条**在用户态构造不出来**，所以它只在内核自检里验 ★
     * 用户态能拿到的任务句柄只有两类：spawn 给的（带 TERMINATE），
     * 以及别人随消息转交的（权限 = 发送方权限 - TRANSFER，所以
     * TERMINATE 仍然在）。也就是说"有一个任务句柄但不含 TERMINATE"
     * 这个状态只能由内核自己造出来——而权限判据本身是内核的事，
     * 所以它在那里验更直接（见 kernel/task/killtest.c 的最后一节）。
     * 写在这里是为了让读的人知道**为什么这里没有这一条**。 */

    say("=== 进程终止测试结束，失败项 ");
    num(g_fail);
    say(" ===\n");
    return (int)g_fail;
}
