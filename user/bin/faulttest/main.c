/* SPDX-License-Identifier: 0BSD */
/* faulttest —— 用户态异常处理者（K5）的**真实路径**承重测试。
 *
 * ★ 它与内核自检（kernel/task/faulttest.c）的分工 ★
 * 内核自检手工构造现场、直接调 `fe_fault_deliver`，所以它证明的是
 * "投递 / 判据 / 回复这一整套**决策**对不对"，**证明不了**从真实的
 * `#PF` 到投递那一段。这里走的是**真的那条路**，一次运行就覆盖：
 *
 *   登记 syscall → ring 3 真异常 → isr_common → fe_isr_dispatch
 *   → user_fault → 投递（拷现场 + 端点消息）→ 处理者线程被唤醒
 *   → 处理者 recv（从自己的缓冲区读现场）→ FAULT_REPLY syscall
 *   → 出错线程从 iretq 回到**处理者指定的新 rip**
 *
 * 链上任何一环没实现，下面至少有一个模式会失败。
 *
 * ★ 为什么必须有**第二条线程** ★
 * "谁登记谁收"（docs/18 §2.3.2）：收件线程 = 登记时的那条调用线程。
 * 出错线程卡在异常上下文里等回复，所以它**不能**是收件线程——自己救不了
 * 自己。于是本程序的最小形状是：主线程 = 干活/出错的线程，
 * 另起一条线程 = 登记并 `recv` 的处理者线程。
 * ★ 处理者线程不要做会出异常的事 ★ 它一做，投递深度已经是 1，
 * 内核按 §2.4 直接把它杀掉（`--recursive` 模式专门验这一条）。
 *
 * ★ 处理者也不能依赖"第一次访问才映射"的内存 ★
 * 它的栈由内核在 `fe_thread_create` 里**预映射**（所以这里传 stack=NULL），
 * 现场缓冲区是本文件里的一个全局，在登记之前先碰一次。
 * 处理者内部**不做任何 IPC/打印**：它只读现场、改现场、回一个 syscall，
 * 把"看到了什么"记进全局，由主线程在恢复之后统一打印。
 * 这样处理者路径上就没有"等另一个服务回话"这件事（那会拖长错误线程的
 * 有界等待，最坏是白等满轮数）。
 *
 * ★ 模式（同一个可执行文件分饰多角，照 killtest 的做法）★
 *   --segv         A 越界写：#PF(14) → 处理者把 rip 改到"我处理完了"的标签
 *   --ud           B 非法指令：ud2 → #UD(6) → rip += 2（ud2 就是 2 字节，不猜）
 *   --de           C 除零：#DE(0) → 改商寄存器 rax 并跳过 divq
 *   --all          E 上面三次由**同一个**处理者接管，核对 vector/fault_count
 *   --nohandler    D **反向对照**：不登记处理者，同样越界写 → 必须仍然被杀
 *   --recursive    F 处理者自己故意出错 → 那条线程被杀（处理者打印只出现一次）
 *
 * ★ 退出码 ★ 成功 = 0；失败 = 非 0（每个失败点一个不同的值，便于归因）。
 * 例外：`--nohandler` 成功就是**被杀**（退出码 -1，与 drvdeny 完全一致）；
 * `--recursive` 成功是 `EXIT_RECURSIVE_OK`（0x77，客户端自己核对过的确定值）。
 *
 * 屏幕上的字一律 ASCII（项目铁律）。
 */
#include <fe_user.h>

/* ------------------------------------------------------------------ */
/* 常量与全局                                                          */
/* ------------------------------------------------------------------ */

/* 两个"没有映射、也不在任何区间里"的地址。
 * 0x51000000 / 0x52000000 落在用户映像、栈、堆之外，所以
 * `fe_user_resolve_fault` 三种情况一种都不认——它一定落到处理者那条路上。 */
#define BAD_ADDR   0x51000000ull
#define BAD_ADDR2  0x52000000ull

/* --de 模式约定：处理者把商寄存器改成这个值，主线程核对它被用上了 */
#define DE_QUOTIENT 0x777ull
/* --segv 模式约定：累加器初值 + 处理者恢复后加的那个数 = 这个结果 */
#define SEGV_BASE   2ull
#define SEGV_ADD    40ull
#define SEGV_TOTAL  (SEGV_BASE + SEGV_ADD)

/* --recursive 的确定退出码（主线程自己打印并核对） */
#define EXIT_RECURSIVE_OK  0x77
#define EXIT_RECURSIVE_MISS 0x78   /* 处理者没接管：不该发生 */
#define EXIT_RECURSIVE_MANY 0x79   /* 处理者接管了不止一次：说明它没被杀 */

#define MAX_SEEN 8

/* 内核把现场拷进这里；处理者就地在它上面改，然后把同一个指针回给内核。
 * ★ 它必须在登记之前是"已映射且可写"的 ★（登记时内核当场校验，否则
 * -7 FE_ERR_FAULT）。它是本文件的 .bss 全局，映像段在启动时已映射；
 * 但仍先 memset 一次，把"第一次访问"这件事挪到登记之前。 */
static struct fe_fault_regs g_regs;

/* 处理者按轮次记下"我看到了什么"（只有处理者写，主线程在恢复后读）。 */
struct seen {
    u64 vector;
    u64 error_code;
    u64 rip;
    u64 cr2;
    u64 fault_count;
    u64 thread_id;
    u64 reply_rc;       /* fe_fault_reply 的返回值：0 = 采纳 */
    u64 hits;           /* 处理到这一轮时，本线程累计接管了几次 */
};
static struct seen g_seen[MAX_SEEN];
static volatile u64 g_seen_n;

/* 主线程 → 处理者：这一轮期望什么、要改成什么。 */
struct expect {
    u64 want_vector;
    u64 fix_rip;        /* != 0：把 rip 改成它（--segv 用标签地址）*/
    u64 rip_delta;      /* != 0：rip += 它（--ud / --de 用指令长度）*/
    u64 set_rax;
    u64 rax_value;
    u64 recursive;      /* != 0：处理者自己故意再出一次错 */
};
static volatile struct expect g_exp;

static volatile u64 g_handler_hits;     /* 处理者一共接管了几次 */
static volatile u64 g_registered;       /* 处理者线程：登记动作已做完 */
static volatile long g_register_rc;     /* 登记 syscall 的返回值 */
static volatile u64 g_worker_ok;        /* worker 被成功救活 */
static volatile u64 g_worker_tid;
static long g_ep_handle = -1;

/* --segv 用的累加器：初值 + 处理者恢复后加的那个数 == 约定结果。
 * "算出的数与预期相等"才算过——"没崩"证明不了什么。 */
static volatile u64 g_accum_now;
/* --ud 用的哨兵：越过错指令之后的那条指令把它置 1 */
static volatile u64 g_ud_after;

/* ------------------------------------------------------------------ */
/* 输出小工具（全是 ASCII）                                             */
/* ------------------------------------------------------------------ */

static void say(const char *s) { fe_puts(s); }

static void say_num(u64 v) { fe_print_u64(v); }

/* fe_print_hex 自己带 "0x" 前缀（见 libfe.c），这里不再加 */
static void say_hex(u64 v) { fe_print_hex(v); }

/* 打印一条 "键=值"（十进制） */
static void kv(const char *k, u64 v)
{
    say(" ");
    say(k);
    say("=");
    say_num(v);
}

static void kvh(const char *k, u64 v)
{
    say(" ");
    say(k);
    say("=");
    say_hex(v);
}

static void eol(void) { say("\n"); fe_flush(); }

/* 安全的前缀判据。
 * ★ 不用 `strncmp` ★ `fe_user.h:883` 声明了它，但整个仓库里**没有任何实现**
 * （libfe 与 libposix 都没有），用它的后果是一个链接错误——这算运气好的那种。
 * 也不用 `mode[2]`：参数短一点就读过了字符串尾巴。 */
static int has_prefix(const char *s, const char *p)
{
    while (*p) {
        if (*s++ != *p++) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* 处理者线程                                                          */
/* ------------------------------------------------------------------ */

/* 读现场里的两个**只读**字段也要原样带回：内核用它判"这是不是等我这一个
 * 回复"（thread_id + fault_count）。本处理者只改 rip/rax，别的一律不动。 */
static void handler_entry(void *arg)
{
    (void)arg;
    /* ★ 登记必须在**处理者线程**里做 ★ "谁登记谁收"——在别处登记，
     * 收件线程就是别的那条线程了，而出错线程恰好是它时内核不投递。 */
    long rc = fe_fault_set_handler(g_ep_handle, &g_regs);
    g_register_rc = rc;
    g_registered = 1;
    if (rc != 0) {
        /* 登记失败：主线程会看见 g_register_rc 并自己失败退出。
         * 本线程什么都不做地活着（不能返回：线程入口绝不能 ret）。 */
        for (;;) {
            fe_sleep_ms(1000);
        }
    }
    for (;;) {
        struct fe_msg_header hdr;
        long r = fe_endpoint_recv(g_ep_handle, &hdr, NULL, 0, NULL, NULL);
        if (r < 0) {
            fe_sleep_ms(1);
            continue;
        }
        if (hdr.protocol != FE_FAULT_PROTO || hdr.opcode != FE_FAULT_OP_EVENT) {
            continue;       /* 不是异常事件：丢掉（本程序只用这一个协议）*/
        }

        u64 idx = g_seen_n;
        if (idx >= MAX_SEEN) {
            idx = MAX_SEEN - 1;
        }
        struct seen *s = &g_seen[idx];
        g_handler_hits++;
        s->hits = g_handler_hits;
        s->vector = g_regs.vector;
        s->error_code = g_regs.error_code;
        s->rip = g_regs.rip;
        s->cr2 = g_regs.cr2;
        s->fault_count = g_regs.fault_count;
        s->thread_id = g_regs.thread_id;
        g_seen_n = idx + 1;

        if (g_exp.recursive) {
            /* ★ 处理者自己出错 ★ 此刻投递深度已经是 1（内核记账里
             * "已投递未回复"），所以这一次**不会再投递**，内核按 §2.4
             * 直接杀掉本线程。这正是我们要验的：处理者不能靠"再投一次"
             * 来救自己。 */
            *(volatile u64 *)(usize)BAD_ADDR2 = 0xBAD;
            /* 到不了这里；真到了说明内核没按深度判据办事。 */
            for (;;) {
                fe_sleep_ms(1000);
            }
        }

        if (g_regs.vector != g_exp.want_vector) {
            /* 期望之外的向量：不猜，交回内核照旧杀（主线程会以失败退出）。
             * 这一条同时是"处理者拿到的是不是**这一轮**的现场"的判据：
             * 拿错了现场，vector 就对不上。 */
            s->reply_rc = (u64)fe_fault_reply(FE_FAULT_KILL, &g_regs);
            continue;
        }
        if (g_exp.fix_rip) {
            g_regs.rip = g_exp.fix_rip;         /* 跳过出错的那条指令 */
        } else if (g_exp.rip_delta) {
            g_regs.rip += g_exp.rip_delta;
        }
        if (g_exp.set_rax) {
            g_regs.rax = g_exp.rax_value;       /* 除法的商：填一个约定值 */
        }
        s->reply_rc = (u64)fe_fault_reply(FE_FAULT_RESUME, &g_regs);
    }
}

/* ------------------------------------------------------------------ */
/* 公共装置：建端点 + 起处理者线程 + 等它登记完                          */
/* ------------------------------------------------------------------ */

/* 返回 0 成功。失败时打印原因。 */
static int setup_handler(void)
{
    memset(&g_regs, 0, sizeof(g_regs));         /* 登记之前先碰一次 */
    memset((void *)g_seen, 0, sizeof(g_seen));
    memset((void *)&g_exp, 0, sizeof(g_exp));

    g_ep_handle = fe_endpoint_create(0);
    if (g_ep_handle < 0) {
        say("[faulttest] endpoint_create failed rc=");
        say_num((u64)g_ep_handle);
        eol();
        return 1;
    }
    /* 栈交给内核分配（它会把前几页**预映射**好）——处理者不能依赖按需分页 */
    u64 tid = fe_thread_create(handler_entry, NULL, NULL, 0);
    if (!tid) {
        say("[faulttest] thread_create(handler) failed\n");
        return 1;
    }
    /* 等它把登记做完。超时上界给得很宽：它只是跑一个 syscall。 */
    for (u32 i = 0; i < 2000 && !g_registered; i++) {
        fe_sleep_ms(1);
    }
    if (!g_registered) {
        say("[faulttest] handler thread never registered\n");
        return 1;
    }
    if (g_register_rc != 0) {
        say("[faulttest] FAULT_HANDLER rc=");
        say_num((u64)g_register_rc);
        say(" (expected 0)\n");
        return 1;
    }
    say("[faulttest] handler registered ep=");
    say_num((u64)g_ep_handle);
    eol();
    return 0;
}

/* 起一条 worker 线程并等它结束（或消失）。
 * 返回 0 = 它已经不在线程表里了。用于 --recursive。 */
static int wait_thread_gone(u64 tid, u32 max_ms)
{
    static u8 buf[8192];
    for (u32 waited = 0; waited < max_ms; waited += 10) {
        struct fe_task_view v;
        if (fe_task_view_get(&v, buf, sizeof(buf), 16, 64) >= 0) {
            const struct fe_task_info *me = fe_task_find(&v, "faulttest");
            if (!me) {
                return 0;       /* 整个任务都不在了 */
            }
            int found = 0;
            for (u32 i = 0; i < me->thread_count; i++) {
                const struct fe_thread_info *th =
                    fe_thread_at(&v, me->thread_base + i);
                if (th && th->id == tid) {
                    found = 1;
                    if (th->state == FE_THREAD_DEAD) {
                        return 0;
                    }
                    break;
                }
            }
            if (!found) {
                return 0;       /* 已经被回收 */
            }
        }
        fe_sleep_ms(10);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* A / B / C 三个"出错点"                                              */
/* ------------------------------------------------------------------ */

/* A：往一个没有映射的地址写。#PF(14)，cr2 == 那个地址，错误码写位为 1。
 * 处理者把 rip 指到一段"我处理完了"的代码——**落点地址由出错方自己给出**，
 * 而不是让处理者去算"出错的指令有多长"（算错就是再出一次错）。
 *
 * ★ 落点为什么是一个汇编符号，而不是 `&&label` ★
 * 第一版写的是 `g_exp.fix_rip = (u64)&&recovered;`，它**编译通过、
 * 反汇编之后是错的**：clang 把"只被 `&&label` 取址、没有任何间接跳转指向它"
 * 的标号当成不可达，整块代码被删掉，而 `(u64)&&recovered` 被折叠成**常量 1**
 * （实测：`movq $0x1, g_exp+0x8`，`recovered` 那段代码在 .text 里根本不存在）。
 * 症状是最难查的一种：进程没崩在内核里，而是**跳到地址 1 取指**，
 * 拿到一次 instruction-fetch #PF（错误码 0x14），rip=cr2=1。
 * 换成下面这条内联汇编里的 `.globl` 符号之后，落点地址是链接期常量，
 * 而且那段代码是**顺序可达**的（汇编块前面的 store 落空才会走到它），
 * 编译器不会再把它当死代码删掉。
 * 判据不靠信任：`build/user/faulttest.elf` 反汇编里能看见落点符号，
 * 而运行期看到的 `rip` 必须落在它上面。 */
__asm__(".globl fe_faulttest_segv_resume");
extern char fe_faulttest_segv_resume[];

static u32 run_segv(void)
{
    volatile u64 *bad = (volatile u64 *)(usize)BAD_ADDR;
    u64 want_cr2 = (u64)(usize)bad;

    g_exp.want_vector = 14;
    g_exp.fix_rip = (u64)(usize)fe_faulttest_segv_resume;
    g_exp.rip_delta = 0;
    g_exp.set_rax = 0;
    g_exp.recursive = 0;

    *bad = 0x1234;      /* ← #PF 在这里 */

    /* ★ 处理者会把 rip 指到下面这一行 ★
     * 这一行在 C 里是"那条 store 之后的落点"，所以编译器为它准备的寄存器与
     * 栈状态，正好等于"store 正常执行完"的状态——而我们只改 rip、
     * 其余寄存器原样回填，两者严丝合缝。（没被救活时进程已被内核杀掉，
     * 根本走不到这里。） */
    __asm__ volatile(".globl fe_faulttest_segv_resume\n"
                     "fe_faulttest_segv_resume:" ::: "memory");

    g_accum_now += SEGV_ADD;
    if (g_accum_now != SEGV_TOTAL) {
        return 0xA2;
    }
    if (g_seen_n != 1) {
        return 0xA3;
    }
    const struct seen *s = &g_seen[0];
    say("[faulttest] segv handled");
    kv("vector", s->vector);
    kvh("cr2", s->cr2);
    kvh("rip", s->rip);
    kv("write", (s->error_code >> 1) & 1u);
    kv("count", s->fault_count);
    kv("reply_rc", s->reply_rc);
    eol();
    if (s->vector != 14) return 0xA4;
    if (s->cr2 != want_cr2) return 0xA5;
    if (((s->error_code >> 1) & 1u) != 1u) return 0xA6;
    if (s->reply_rc != 0) return 0xA7;
    return 0;
}

/* B：执行一条 ud2。#UD(6)。处理者把 rip += 2 —— ud2 的编码就是 0F 0B
 * 两个字节，客户端自己知道，**不猜**（内核不提供指令长度，见 docs/18 §2.2.4）。 */
static u32 run_ud(void)
{
    u64 idx = g_seen_n;

    g_exp.want_vector = 6;
    g_exp.fix_rip = 0;
    g_exp.rip_delta = 2;
    g_exp.set_rax = 0;
    g_exp.recursive = 0;

    __asm__ volatile("ud2" ::: "memory");       /* ← #UD 在这里 */

    g_ud_after = 1;                             /* 越过错指令之后这条被执行 */
    if (!g_ud_after) {
        return 0xB1;
    }    if (g_seen_n != idx + 1) {
        return 0xB2;
    }
    const struct seen *s = &g_seen[idx];
    say("[faulttest] ud handled");
    kv("vector", s->vector);
    kvh("rip", s->rip);
    kvh("cr2", s->cr2);
    kv("after", g_ud_after);
    kv("count", s->fault_count);
    kv("reply_rc", s->reply_rc);
    eol();
    if (s->vector != 6) return 0xB3;
    if (s->reply_rc != 0) return 0xB4;
    /* ★ cr2 只有 #PF(14) 有意义，按 ABI 其余向量必须是 0 ★
     * 这一条是**实测出来的**：K5 第一版里 `user_fault` 无条件读 CR2，
     * 而 CR2 只有 #PF 才会被 CPU 写——于是 #UD/#DE 报回来的是**上一次
     * 缺页的地址**（第一次跑到这里时打出来的是 `cr2=0x51000000`，
     * 正是 --segv 那个地址）。处理者按 ABI 判断"cr2 为 0 说明这不是缺页"，
     * 而它拿到了一个非 0 的陈旧值——那是**会误导处理者**的那种错。 */
    if (s->cr2 != 0) return 0xB5;
    return 0;
}

/* C：整数除零。#DE(0)。处理者把商寄存器 rax 改成约定的值并跳过 divq
 * （`divq %rcx` 的编码是 48 F7 F1，3 字节——本文件构建后用反汇编核对过）。
 * 判据是"结果等于按那个值算出来的数"，不是"没崩"。 */
static u32 run_de(void)
{
    u64 idx = g_seen_n;
    u64 q = 0;

    g_exp.want_vector = 0;
    g_exp.fix_rip = 0;
    g_exp.rip_delta = 3;
    g_exp.set_rax = 1;
    g_exp.rax_value = DE_QUOTIENT;
    g_exp.recursive = 0;

    __asm__ volatile("xorl %%edx, %%edx\n\t"
                     "divq %%rcx"
                     : "=a"(q)
                     : "a"(1ull), "c"(0ull)
                     : "rdx", "cc");

    if (g_seen_n != idx + 1) {
        return 0xC1;
    }
    const struct seen *s = &g_seen[idx];
    say("[faulttest] de handled");
    kv("vector", s->vector);
    kvh("rip", s->rip);
    kvh("cr2", s->cr2);
    kv("quotient", q);
    kv("count", s->fault_count);
    kv("reply_rc", s->reply_rc);
    eol();
    if (s->vector != 0) return 0xC2;
    if (s->reply_rc != 0) return 0xC3;
    if (q != DE_QUOTIENT) return 0xC4;      /* 处理者填的值真的被用上了 */
    if (s->cr2 != 0) return 0xC5;           /* 非 #PF 的 cr2 必须是 0（见 run_ud）*/
    return 0;
}

/* ------------------------------------------------------------------ */
/* D：反向对照 —— 不登记处理者，越界写必须**仍然被杀**                  */
/* ------------------------------------------------------------------ */

/* ★ 这是整个文件最重要的一条 ★ 没有它，"投递机制生效"与
 * "内核干脆不杀了"无法区分。判据在 init 侧：退出码必须是 -1，
 * 与今天的 drvdeny 完全一致。 */
static u32 run_nohandler(void)
{
    volatile u64 *bad = (volatile u64 *)(usize)BAD_ADDR;
    say("[faulttest] nohandler: writing an unmapped address, no handler "
        "registered -- this process MUST die with -1\n");
    fe_flush();
    *bad = 0x99;        /* ← #PF；没有处理者 → 内核走"照旧杀" */
    return 0xD1;        /* 到不了这里 */
}

/* ------------------------------------------------------------------ */
/* F：处理者自己出错（投递深度 1）                                      */
/* ------------------------------------------------------------------ */

/* 形状必须是三条线程：主线程在这里当**策略**，处理者线程与 worker 线程
 * 各自去死。主线程必须活着，才能"按自己的策略退出"并打印那个确定值——
 * 主线程一旦参与出错，退出码就只能是内核给的 -1，与崩溃分不开。 */
static void rec_worker_entry(void *arg)
{
    (void)arg;
    volatile u64 *bad = (volatile u64 *)(usize)BAD_ADDR;
    /* ★ 这里**不能**碰 `g_exp.recursive` ★ 第一版在这一行写了
     * `g_exp.recursive = 0`，把主线程刚设好的 1 覆盖掉了——于是处理者
     * 不递归、照常回 RESUME，worker 回到同一条指令上再错一次，
     * 被"同一现场连续两次"判据杀掉，而日志看起来"很像"预期结果
     * （handler_hits=1、worker 死了）。**差一点就信了**：
     * 真正的判据是"处理者自己被打死"，而那一版里它是活着的。
     * 只设这一轮**该由 worker 决定**的两项（要哪个向量、往哪写）。 */
    g_exp.want_vector = 14;
    g_exp.fix_rip = 0;
    g_exp.rip_delta = 0;
    g_exp.set_rax = 0;
    *bad = 0x77;                /* ← #PF：投递给处理者 */
    g_worker_ok = 1;            /* 只有被救活才会到这一行 */
    for (;;) {
        fe_sleep_ms(1000);
    }
}

static u32 run_recursive(void)
{
    g_exp.recursive = 1;        /* 处理者收到第一个事件后自己出错 */

    u64 tid = fe_thread_create(rec_worker_entry, NULL, NULL, 0);
    if (!tid) {
        say("[faulttest] recursive: worker thread_create failed\n");
        return 0xF1;
    }
    g_worker_tid = tid;

    /* 等 worker 消失（被杀）或超时。worker 在等回复，而唯一能回复的处理者
     * 已经死了，所以它要等满内核的有界轮数才被杀——这段时间由内核决定，
     * 这里只负责"等到它真的不在线程表里"。 */
    int still = wait_thread_gone(tid, 20000);

    say("[faulttest] recursive done");
    kv("handler_hits", g_handler_hits);
    kv("worker_ok", g_worker_ok);
    kv("worker_gone", still ? 0u : 1u);
    eol();

    if (still) {
        return 0xF2;            /* worker 一直没死：有界等待没起作用 */
    }
    if (g_worker_ok) {
        return EXIT_RECURSIVE_MISS;     /* 处理者没接管：不该发生 */
    }
    /* ★ 判据：处理者只被投递了一次 ★
     * 若深度判据没生效，处理者会**第二次**收到事件（它自己的那次异常），
     * 于是 hits 变成 2——那一轮它的打印就会出现两次，而这里是一个数，
     * 比数日志行数更硬。 */
    if (g_handler_hits != 1) {
        return EXIT_RECURSIVE_MANY;
    }
    return EXIT_RECURSIVE_OK;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "--segv";

    say("[faulttest] mode = ");
    say(mode);
    eol();

    if (has_prefix(mode, "--nohandler")) {
        /* --nohandler：故意**不**建处理者 */
        fe_exit((int)run_nohandler());
    }

    if (setup_handler() != 0) {
        fe_exit(0xEE);
    }

    if (has_prefix(mode, "--recursive")) {
        fe_exit((int)run_recursive());
    }
    if (has_prefix(mode, "--all")) {
        /* --all：A/B/C 三次由**同一个**处理者接管。
         * ★ 正向 ★ 三次都拿到正确的 vector，且 fault_count 每次都加一；
         * ★ 反向 ★ 三次的 rip **互不相同** —— 证明每一轮拿到的是各自的
         * 现场，而不是第一份被重复使用。
         * （只比 rip：`cr2` 只有 #PF 有意义，另外两次按 ABI 是 0，
         * 它**不可能**互不相同——这一点与 docs/18 §5.2 模式 E 的措辞
         * 不一致，见提交信息里那条"文档与内核对不上的地方"。） */
        u32 f = 0;
        g_accum_now = SEGV_BASE;
        f += run_segv();
        f += run_ud();
        f += run_de();
        if (f == 0) {
            if (g_seen_n != 3) {
                f = 0xE5;
            } else if (!(g_seen[1].fault_count == g_seen[0].fault_count + 1 &&
                         g_seen[2].fault_count == g_seen[1].fault_count + 1)) {
                f = 0xE6;       /* fault_count 没递增 */
            } else if (g_seen[0].vector != 14 || g_seen[1].vector != 6 ||
                       g_seen[2].vector != 0) {
                f = 0xE7;       /* 三次的向量不是各自的 */
            } else if (g_seen[0].rip == g_seen[1].rip ||
                       g_seen[1].rip == g_seen[2].rip ||
                       g_seen[0].rip == g_seen[2].rip) {
                f = 0xE8;       /* rip 撞了：现场被重复使用 */
            }
        }
        say("[faulttest] all: vectors");
        kv("v1", g_seen[0].vector);
        kv("v2", g_seen[1].vector);
        kv("v3", g_seen[2].vector);
        kv("c1", g_seen[0].fault_count);
        kv("c2", g_seen[1].fault_count);
        kv("c3", g_seen[2].fault_count);
        kv("fail", f);
        eol();
        fe_exit((int)f);
    }

    u32 f;
    if (has_prefix(mode, "--ud")) {
        f = run_ud();
    } else if (has_prefix(mode, "--de")) {
        f = run_de();
    } else {
        g_accum_now = SEGV_BASE;
        f = run_segv();                     /* --segv（默认）*/
    }
    fe_exit((int)f);
    return 0;
}
