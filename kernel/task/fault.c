/* SPDX-License-Identifier: 0BSD */
/* 用户态异常处理者（K5）—— 登记 / 注销 / 投递 / 回复。
 *
 * 设计与逐条语义见 docs/18-user-fault-handler.md（本文的每一条决定都能在那里
 * 找到"为什么"与"代价"）。这里只把形状讲清：
 *
 *   #PF/#GP/#UD/#DE/#BP 来自 ring 3
 *     → 先试按需分页（kernel/task/user.c 的路径，一个字没动）
 *     → 失败：这个任务登记了处理者吗？
 *         没有 → fe_thread_exit(-1)（今天的行为）
 *         有   → 拷现场 → 端点消息投递 → **就地让出**等回复
 *     → 回复"已处理"：回填现场并 return（异常路径 iretq 回去继续跑）
 *       回复"照旧杀"：fe_thread_exit(-1)
 *       没有回复 / 投递失败 / 嵌套超限 / 同现场重复 / 自投递：fe_thread_exit(-1)
 *
 * ★ 为什么单独一个文件，而不是塞进 user.c ★
 * `user.c` 现在是"按需分页 + 栈增长 + 用户线程创建"，再塞投递与等待会让那个
 * 文件同时承担"地址空间判断"与"调度交互"两件事——而这两件事的读者不同。
 *
 * ★ 三条不许走样的约束（走样就是安全洞或死锁）★
 *   1. **内核态的异常永远不投递**：判据在调用点（`idt.c`）的 `(r->cs & 3) == 3`。
 *      真要放进来的话，处理者改 `rip`/`rsp` 之后 iretq 就是任意内核代码执行
 *      ——不需要"能改 cs"，把 `cs` 原样弹回去就够了；
 *   2. **先按需分页、再问处理者**：顺序在 `idt.c`，这里不重复判；
 *   3. **投递后在 IF=0 里只能"就地让出 + 有界轮询"**：阻塞会要求
 *      "谁能唤醒它"，那要新增第六处等待登记与一条新的取消路径。
 */
#include <fe/process.h>
#include <fe/sched/sched.h>
#include <fe/sched/thread.h>
#include <fe/ipc.h>
#include <fe/object.h>
#include <fe/user.h>
#include <fe/kprintf.h>
#include <fe/string.h>
#include <fe/irq.h>
#include <fe/io.h>              /* fe_irq_save / fe_irq_restore */
#include <fe/panic.h>
#include <fe/mm.h>

/* ------------------------------------------------------------------ */
/* 诊断计数                                                            */
/* ------------------------------------------------------------------ */

static u64 g_delivered;     /* 成功投递出去（进了端点队列）的次数 */
static u64 g_resumed;       /* 处理者回 RESUME 并成功回填的次数 */
static u64 g_last_wait_rounds;  /* 最近一次投递实际等了几个让出轮（诊断） */

u64 fe_fault_delivered_count(void)
{
    return g_delivered;
}

u64 fe_fault_resumed_count(void)
{
    return g_resumed;
}

u64 fe_fault_last_wait_rounds(void)
{
    return g_last_wait_rounds;
}

/* ★ 自检钩子（**只给自检用**，生产路径上恒为 NULL）★
 *
 * ★ 为什么必须有它 ★ 自检里有几条要验的是"**处理者不回复**时会怎样"，
 * 而 `FE_FAULT_WAIT_ROUNDS` 是 20 万 —— 几条这样的用例加起来就是上百万次
 * 让出，自检会慢到把整轮启动拖垮。
 *
 * ★ 它不改生产行为 ★ 钩子在**轮询循环的同一位置**被调用（每轮一次），
 * 它既能观察"此刻的现场槽/端点是空的"这些事实，也能替自检**当场**回一个
 * 决定（回完之后循环下一轮就看见 `arrived` 并正常退出）。
 * 所以自检验的仍然是**真的那个循环**，只是不必空转 20 万次。
 * 钩子返回后循环继续按原样跑——没有"绕过判据"这回事。 */
void (*g_fault_wait_hook)(struct fe_thread *victim, void *ctx);
void *g_fault_wait_hook_ctx;

void fe_fault_set_wait_hook(void (*fn)(struct fe_thread *, void *), void *ctx)
{
    g_fault_wait_hook = fn;
    g_fault_wait_hook_ctx = ctx;
}

/* ------------------------------------------------------------------ */
/* 登记 / 注销                                                         */
/* ------------------------------------------------------------------ */

/* ★ 登记时内核**自己**对端点对象加一次引用 ★
 * 用户态可以 `HANDLE_CLOSE` 掉那个句柄，但内核的登记里还存着端点指针——
 * 不加引用就是悬空指针；而那次引用必须在任务销毁路径上放开
 * （`task.c` 的 `case FE_OBJ_TASK`），否则端点对象永不销毁（它挂着消息队列
 * 与等待者表）。两种都不报错，只是"慢漏"或"随机崩"。
 *
 * ★ 登记时把"谁来收"钉住（收件线程 = 调用线程）★
 * 见 task.h 里 handler_recv_thread 的说明：这不是可选优化，
 * 它拦的是"单线程程序自己给自己登记"那个最自然的用法——
 * 那种情形下出错线程卡在异常里等回复，而唯一能回复的线程正是它自己，
 * 结果是等满轮数才死（"登记了处理者但每次异常都要等很久"）。 */
fe_status_t fe_fault_set_handler(struct fe_task *t, struct fe_endpoint *ep, u64 buf)
{
    if (!t) {
        return FE_ERR_INVAL;
    }
    struct fe_thread *self = fe_thread_current();
    u64 flags = fe_irq_save();
    if (t->handler_ep && t->handler_ep != ep) {
        /* 换一个处理者：先放掉旧的那次引用，再拿新的。
         * **不允许**"两个都对"——那会漏掉一次 unref。 */
        fe_object_unref(&t->handler_ep->hdr);
    } else if (t->handler_ep == ep) {
        /* 重复登记同一个端点：只更新缓冲区地址与收件线程，引用不加第二次。 */
        t->handler_buf = buf;
        t->handler_recv_thread = self;
        fe_irq_restore(flags);
        return FE_OK;
    }
    t->handler_ep = ep;
    t->handler_buf = buf;
    t->handler_recv_thread = self;      /* "谁登记谁收" */
    if (ep) {
        fe_object_ref(&ep->hdr);
    }
    fe_irq_restore(flags);
    return FE_OK;
}

/* 注销。★ 注销**不影响**正在等回复的那一次 ★
 * 理由：那一次的现场已经拷出去、消息已经在队列里，收尾由 `fault_depth` 与
 * 回复槽负责；在这里把它一并清掉，等于让那条正在等的线程永远等不到回复
 * （它还得等满轮数才死）。所以注销只摘"以后的异常往哪投"。 */
fe_status_t fe_fault_clear_handler(struct fe_task *t)
{
    if (!t) {
        return FE_ERR_INVAL;
    }
    u64 flags = fe_irq_save();
    struct fe_endpoint *ep = t->handler_ep;
    t->handler_ep = NULL;
    t->handler_buf = 0;
    t->handler_recv_thread = NULL;
    fe_irq_restore(flags);
    if (ep) {
        fe_object_unref(&ep->hdr);
    }
    return FE_OK;
}

/* 任务销毁路径要调的：放掉处理者端点的那次引用。
 *
 * ★ 与 `fe_fault_clear_handler` 的区别：这里**只释放引用**，不做别的 ★
 * 任务正在销毁，它自己的字段马上就不存在了，所以不需要"清干净"这件事；
 * 而 `unref` 必须有——否则端点对象永远不销毁。 */
void fe_fault_release_handler(struct fe_task *t)
{
    if (!t) {
        return;
    }
    struct fe_endpoint *ep = t->handler_ep;
    t->handler_ep = NULL;
    t->handler_buf = 0;
    t->handler_recv_thread = NULL;
    t->fault_depth = 0;
    t->fault_owner = NULL;
    if (ep) {
        fe_object_unref(&ep->hdr);
    }
}

/* ------------------------------------------------------------------ */
/* 回复                                                                */
/* ------------------------------------------------------------------ */

/* ★ 回复必须校验"这是不是等我这一个回复"（三项一起对）★
 * docs/18 §2.3 要求的是 `(task_id, thread_id, fault_seq)` 三项：
 *
 *   - `task_id`    → `t` 本身。syscall 层用 `fe_task_current()` 传进来
 *                    （§2.3 末尾："系统调用天然携带谁在回复"）；
 *   - `thread_id`  → `replier` 必须是**登记时钉住的那条收件线程**
 *                    （§2.3.2"谁登记谁收"）。这一条把"谁能替我回复"
 *                    钉死在内核里，防的是"同任务里另一条线程冒充处理者"；
 *   - `fault_seq`  → 处理者**回显**它正在处置的那一轮的轮次号。
 *
 * ★ 为什么轮次号走 `regs` 里的字段，而不是再开一个参数 ★
 * `FE_SYS_FAULT_REPLY` 的形状是 `(verdict, regs_user_ptr)`（§4.1），
 * 没有第三个参数可以带轮次号；而处理者手上那份 `fe_fault_regs` 里本来就有
 * 两个**只读**字段（`thread_id`、`fault_count`，§2.2.3 那句"下面是现场之外、
 * 但缺了就处置不了的三项（只读）"）。所以"回显"就是**把只读字段原样带回来**：
 * 改了就拒。这不是新增 ABI，是把那句"只读"从注释变成判据。
 *
 * ★ 与 §2.4 代价 2（"`fault_count` 是诊断用、不参与判据"）的关系 ★
 * 那句话针对的是**递归深度判据**（深度只看 `fault_depth` 的 0/1，绝不能用
 * 历史计数代替）。这里的用途不同：它是**回复与投递的配对号**，是
 * `arrived` 一个布尔量做不到的那件事（区分"晚到的上一轮回复"）。
 * 两处结论都在 §6.1.7 记着。
 *
 * 对不上返回 `FE_ERR_INVAL` 并**不做任何事**（不写槽、不唤醒）。 */
fe_status_t fe_fault_reply(struct fe_task *t, struct fe_thread *replier,
                           const struct fe_fault_regs *regs, u64 verdict)
{
    if (!t) {
        return FE_ERR_INVAL;
    }
    u64 flags = fe_irq_save();
    if (t->fault_depth == 0 || !t->fault_owner) {
        fe_irq_restore(flags);
        /* "没有正在等的投递"与"登记不在了"是两件事：
         * 前者是回复来晚了/重复回复（INVAL），后者见 §6.1.5
         * （exec 之后应当 NOENT）——那个区分由 syscall 层做。 */
        return FE_ERR_INVAL;
    }
    struct fe_thread *owner = t->fault_owner;
    /* `fault_owner` 是**出错线程**（它在等回复），而 `replier` 是**处理者线程**
     * ——两者必然不同（§2.3.2：出错线程就是收件线程时根本不投递）。
     * 所以这里比的不是"回报者 == owner"，而是"回报者 == 收件线程"。 */
    if (replier && replier != t->handler_recv_thread) {
        fe_irq_restore(flags);
        return FE_ERR_INVAL;
    }
    /* ---- thread_id：回复的必须是**正在等的那个线程**的现场 ----
     * 没有 `regs` 就没有身份，一律拒（"我不认识这一份现场"）。
     * 这一条拦的是"两条线程同时出错"时的跨线程现场伪装。 */
    if (!regs || regs->thread_id != owner->id) {
        fe_irq_restore(flags);
        return FE_ERR_INVAL;
    }
    /* ---- fault_seq：轮次号必须回显成**本轮** ----
     * `owner->fault_reply.seq` 是投递时写下的"本轮期望值"，处理者回显的是
     * 它收到的那一份 `fault_count`。对不上 = 回的是上一轮（或者改了只读字段）。 */
    if (regs->fault_count != owner->fault_reply.seq) {
        fe_irq_restore(flags);
        return FE_ERR_INVAL;
    }
    u32 action = (u32)(verdict & FE_FAULT_ACTION_MASK);
    if (action != FE_FAULT_RESUME && action != FE_FAULT_KILL &&
        action != FE_FAULT_RETHROW) {
        fe_irq_restore(flags);
        return FE_ERR_INVAL;
    }
    owner->fault_reply.verdict = verdict;
    if (verdict & FE_FAULT_FLAG_KEEP_REGS) {
        /* KEEP_REGS：现场用原来的。先把槽里的现场清零，
         * 免得它看起来像"用户给了现场"——投递方按 flag 决定用哪一份。 */
        memset(&owner->fault_reply.regs, 0, sizeof(owner->fault_reply.regs));
    } else {
        owner->fault_reply.regs = *regs;
    }
    owner->fault_reply.arrived = true;
    fe_irq_restore(flags);
    return FE_OK;
}

/* ------------------------------------------------------------------ */
/* 投递                                                                */
/* ------------------------------------------------------------------ */

/* 一次投递要用的三种判据，各自对应 docs/18 的一节：
 *
 *   1. `fe_thread_should_die(victim)`  —— §2.7 闸门判据在前。
 *      现场就算交给处理者、处理者就算"修好"了它，闸门仍会在下一次回用户态时
 *      把它杀掉；投递一次只是浪费一次往返，并给处理者一个"我救活了它"的错觉；
 *   2. `t->fault_depth != 0`          —— §2.4 深度 1，第二次就杀。
 *      为什么不是 2 或 3：处理者自己要能跑，就必须有已经映射好的栈与代码。
 *      它因为自己的栈没映射而 #PF，那是它**没有准备好**；再给一层机会只会把
 *      "没准备好"变成"有时能跑"，而"有时能跑"是这个项目反复列为最坏的症状；
 *   3. `victim == t->handler_recv_thread` —— §2.3.2 自投递。
 *      与深度判据**不重复**：那一刻深度还是 0，深度判据放行。
 *
 * 外加 §2.5.2 的"同现场重复"：同一个 `(rip, cr2)` 连续两次就不再投递。
 * 它防的是一个**纯用户态可控的活锁**（处理者回 RESUME 但没改 rip）。 */
static bool fault_same_site_twice(const struct fe_thread *victim, u64 rip, u64 cr2)
{
    return victim->last_fault_rip == rip && victim->last_fault_cr2 == cr2;
}

/* 把内核现场 + 伴生字段组装成交给用户态的那一份。 */
static void fault_fill_regs(struct fe_fault_regs *out, const struct fe_regs *r,
                            u64 cr2, u64 thread_id, u64 count)
{
    out->r15 = r->r15; out->r14 = r->r14; out->r13 = r->r13; out->r12 = r->r12;
    out->r11 = r->r11; out->r10 = r->r10; out->r9 = r->r9;   out->r8 = r->r8;
    out->rbp = r->rbp; out->rdi = r->rdi; out->rsi = r->rsi; out->rdx = r->rdx;
    out->rcx = r->rcx; out->rbx = r->rbx; out->rax = r->rax;
    out->vector = r->vector;
    out->error_code = r->error;
    out->rip = r->rip; out->cs = r->cs; out->rflags = r->rflags;
    out->rsp = r->rsp; out->ss = r->ss;
    out->cr2 = cr2;
    out->thread_id = thread_id;
    out->fault_count = count;
}

/* ★ 回填：把用户态改过的现场装回 `r` ★
 *
 * ★ 逐字段列全，不许抽样比 ★ §5.3 那张表里有一条症状就是"回填漏了某个寄存器
 * （尤其是 rax/rcx/r11 这些被指令隐式使用的）"，而症状是"改完现场后程序继续跑
 * 但算出了错的值"——那比崩溃难查。
 *
 * ★ `cs`/`ss` 一律拒绝改动 ★ 这是安全边界，不是风格选择。
 * 见 docs/18 §3.1：内核态异常不投递 与 拒绝改 cs/ss 是**同一个洞的两个入口**。
 * 注意 `iretq` 从栈上弹的 `cs` 就是那个 `0x08`——我们只是把它弹回去了，
 * 所以"允许改 cs"不需要任何额外条件就已经是任意内核代码执行。
 *
 * 返回 false = 用户态给了非法现场（cs/ss 被改），调用者照旧杀线程。 */
bool fe_fault_apply_regs(struct fe_regs *r, const struct fe_fault_regs *f)
{
    if (f->cs != r->cs || f->ss != r->ss) {
        fe_kprintf("[异常处理] **拒绝改 cs/ss**（cs=%#llx ss=%#llx，原值 "
                   "%#llx/%#llx）——这是提权链，处理者的这一份现场被丢弃\n",
                   (unsigned long long)f->cs, (unsigned long long)f->ss,
                   (unsigned long long)r->cs, (unsigned long long)r->ss);
        return false;
    }
    r->r15 = f->r15; r->r14 = f->r14; r->r13 = f->r13; r->r12 = f->r12;
    r->r11 = f->r11; r->r10 = f->r10; r->r9 = f->r9;   r->r8 = f->r8;
    r->rbp = f->rbp; r->rdi = f->rdi; r->rsi = f->rsi; r->rdx = f->rdx;
    r->rcx = f->rcx; r->rbx = f->rbx; r->rax = f->rax;
    r->rip = f->rip; r->rflags = f->rflags; r->rsp = f->rsp;
    return true;
}

fe_status_t fe_fault_deliver(struct fe_thread *victim, const struct fe_regs *r,
                             u64 cr2, struct fe_fault_regs *out_regs)
{
    if (!victim || !r || !out_regs) {
        return FE_ERR_INVAL;
    }
    struct fe_task *t = victim->task;
    if (!t) {
        return FE_ERR_INVAL;        /* 内核线程（没有任务）：不投递 */
    }

    /* ---- 判据 1：闸门在前（§2.7）---- */
    if (fe_thread_should_die(victim)) {
        return FE_ERR_ACCESS;
    }
    /* ---- 判据 2：深度（§2.4）---- */
    if (t->fault_depth != 0) {
        return FE_ERR_BUSY;
    }
    /* ---- 判据 3：自投递（§2.3.2）---- */
    if (t->handler_recv_thread == victim) {
        fe_kprintf("[异常处理] **出错线程就是收件线程**（%s id=%llu）——"
                   "它自己卡在异常里，没人能替它回复；直接杀（这不是缺陷："
                   "处理者路径要求至少有**另一条**线程能跑）\n",
                   victim->name, (unsigned long long)victim->id);
        return FE_ERR_INVAL;
    }
    /* ---- 判据 4：同一个现场连续两次（§2.5.2）---- */
    if (fault_same_site_twice(victim, r->rip, cr2)) {
        fe_kprintf("[异常处理] **同一现场被重复投递**（rip=%#llx cr2=%#llx）"
                   "——处理者回了 RESUME 却没改 rip，再投一次就是活锁；直接杀\n",
                   (unsigned long long)r->rip, (unsigned long long)cr2);
        return FE_ERR_AGAIN;
    }

    struct fe_endpoint *ep = t->handler_ep;
    if (!ep) {
        return FE_ERR_NOENT;        /* 没登记：今天的行为（照旧杀） */
    }

    /* ---- 组装现场并拷进用户缓冲（§2.2.2：用 fe_copy_to_user）----
     *
     * ★ 为什么是 fe_copy_to_user 而不是 fe_user_write_space ★
     * 后者虽然能对**任意**地址空间写，但它**只查"这一页有没有映射"**，
     * 不查 user 位、不查可写——用它把现场写进一块只读页或 MMIO 页是可能的，
     * 而"内核往用户的只读页里写 200 字节"这种事不该由 K5 引入。
     * `fe_copy_to_user` 逐页校验"这块内存确实可写"，而且它只认当前任务
     * ——投递正跑在出错线程自己的上下文里，当前 CR3 就是它的地址空间。 */
    struct fe_fault_regs fregs;
    memset(&fregs, 0, sizeof(fregs));
    /* ★ "本线程第几次被投递"（§2.2.3）：每线程的，不是任务级的 ★
     * 任务级的累计数在"同一个任务换了另一条线程出错"时会给错答案，
     * 而处理者正是靠它判断"这是不是第二次"。任务级的那个
     * （`t->fault_count_total`）留着当诊断。 */
    u64 fcount = victim->fault_count + 1;
    fault_fill_regs(&fregs, r, cr2, victim->id, fcount);

    fe_status_t s = fe_copy_to_user((void *)(uptr)t->handler_buf, &fregs,
                                    sizeof(fregs));
    if (fe_failed(s)) {
        fe_kprintf("[异常处理] 现场写不进用户缓冲（buf=%#llx，%s）——"
                   "处理者的缓冲区必须预先映射且可写；直接杀\n",
                   (unsigned long long)t->handler_buf,
                   s == FE_ERR_FAULT ? "未映射/不可写" : "错误");
        return s;
    }

    /* ---- 发一条消息把处理者叫醒（§2.1：走端点，但内核持有对象指针）---- */
    struct fe_msg_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.protocol = FE_FAULT_PROTO;
    hdr.opcode = FE_FAULT_OP_EVENT;
    hdr.payload_len = 0;        /* 现场在缓冲区里；载荷留空（§2.2.2）*/
    hdr.handle_count = 0;
    s = fe_endpoint_send_obj(t, ep, &hdr, NULL, NULL);
    if (fe_failed(s)) {
        /* ★ 队列满（FE_ERR_AGAIN）只能杀线程 ★
         * IF=0 里不能等（等就是死锁），也不能丢（丢了线程永远挂着）。
         * 代价说清楚：处理者自己积压 64 条不处理时，下一个故障线程直接被杀。
         * 这是"处理者必须及时收"的硬约束。 */
        fe_kprintf("[异常处理] 投递失败（%s）——处理者积压或端点已关闭；直接杀\n",
                   s == FE_ERR_AGAIN ? "队列满" : "错误");
        return s;
    }

    /* ---- 记账：加一、记主、清回复槽、更新"同现场"记忆 ---- */
    u64 flags = fe_irq_save();
    t->fault_depth = 1;
    t->fault_owner = victim;
    t->fault_seq++;
    u64 seq_at_deliver = t->fault_seq;   /* 记下"我这一轮的号"，供收尾比对 */
    t->fault_count_total++;              /* 任务级累计（诊断，与线程级的那个不同）*/
    victim->fault_count = fcount;
    victim->fault_reply.arrived = false;
    /* 本轮期望的回显轮次号：回复必须把它原样带回来（见 fe_fault_reply）。 */
    victim->fault_reply.seq = fcount;
    victim->fault_reply.verdict = 0;
    victim->last_fault_rip = r->rip;
    victim->last_fault_cr2 = cr2;
    fe_irq_restore(flags);
    g_delivered++;

    /* ---- 就地让出 + 有界轮询（§2.6.3）----
     *
     * ★ 为什么是"让出"而不是 `fe_sched_block_current` ★
     * 让出是软中断转发，**不受 IF 影响**（异常入口到 user_fault 之间没有
     * 任何一处 sti），而且它自己会回来看——有界、且不需要"谁能唤醒我"这个
     * 问题。阻塞则要求另一个执行体显式 `fe_sched_wake`，那要**新增第六处
     * 等待登记**与一条新的取消路径（docs/13-tasks-and-kill.md §6.3 那张表
     * 的教训：漏摘就是悬空指针），而且唤醒源丢了就是**永远卡在 IF=0 里**。
     *
     * ★ 为什么"让出"在异常上下文里是安全的 ★
     * `fe_sched_maybe_switch` 只在返回用户态的路上判闸门与切线程，而它自己
     * 跑在 IF=0 里。异常上下文里的让出走的正是同一条路径：现场留在本线程的
     * 内核栈上（`struct fe_regs` 就压在那儿），切回来时 rsp 恢复，一切照旧
     * ——**没有任何"从函数中间切走再切回来"的新形态**。
     *
     * ★ 日志纪律 ★ 允许一次性的一行总结，**禁止在等待循环里逐轮打印**：
     * `fe_kprintf` 在这条路上能用（无锁、不分配、不睡眠、串口轮询输出），
     * 但每个字符都有有界自旋（最多 10 万次 LSR 读），逐轮打印会把那个自旋
     * 乘以轮数。 */
    u32 waited = 0;
    for (u32 i = 0; i < FE_FAULT_WAIT_ROUNDS; i++) {
        if (victim->fault_reply.arrived) {
            break;
        }
        if (fe_thread_should_die(victim)) {
            break;      /* 正在被杀就不再等（§2.7）*/
        }
        /* 自检钩子：在生产路径上恒为 NULL，所以这一段对真实运行**零影响**。
         * 它在**轮询循环的同一位置**，自检因此验的是真的那个循环
         * （见 fe_fault_set_wait_hook 的说明）。 */
        if (g_fault_wait_hook) {
            g_fault_wait_hook(victim, g_fault_wait_hook_ctx);
        }
        fe_thread_yield();
        waited++;
    }
    g_last_wait_rounds = waited;

    /* ---- 收尾：无论走哪一条路，深度都必须减回去 ---- ★
     *
     * ★ 减一只写一处 ★ §2.4 代价 3：漏掉一次，这个任务的异常处理就**永久
     * 失效**（所有异常直接杀），症状是"处理者工作过一次之后再也不工作了"。
     * 所以这里用"唯一出口"的写法：先把回复取出来，再统一清账。 */
    u64 verdict = 0;
    bool arrived = false;
    struct fe_fault_regs user_regs;
    memset(&user_regs, 0, sizeof(user_regs));

    flags = fe_irq_save();
    if (victim->fault_reply.arrived) {
        arrived = true;
        verdict = victim->fault_reply.verdict;
        user_regs = victim->fault_reply.regs;
    }
    victim->fault_reply.arrived = false;
    /* ★ 减一只写这一处（§2.4 代价 3）★
     * 只有"这一次投递还是我"才清深度：极端情形下（比如处理者线程被杀、
     * 任务被终止）可能有别人已经重新登记过——那时不该把它清掉。
     * 漏掉这一次减一的症状是"处理者工作过一次之后再也不工作了"（所有异常
     * 直接杀），很难查——所以整个函数只有这一条减一路径。 */
    if (t->fault_owner == victim && t->fault_seq == seq_at_deliver) {
        t->fault_depth = 0;
        t->fault_owner = NULL;
    }
    fe_irq_restore(flags);

    if (!arrived) {
        fe_kprintf("[异常处理] 处理者 %u 轮内没有回复线程 %s(id=%llu)——"
                   "直接杀（有界轮询到点了）\n",
                   (u32)FE_FAULT_WAIT_ROUNDS, victim->name,
                   (unsigned long long)victim->id);
        return FE_ERR_TIMEOUT;
    }

    u32 action = (u32)(verdict & FE_FAULT_ACTION_MASK);
    if (action == FE_FAULT_RESUME) {
        if (verdict & FE_FAULT_FLAG_KEEP_REGS) {
            /* "已处理，现场不改"：等价于"让出错的那条指令**重新执行一遍**"。
             * 对 #PF 来说这是最有用的一种（处理者刚刚补好了映射）。 */
            *out_regs = fregs;
            g_resumed++;
            return FE_OK;
        }
        *out_regs = user_regs;
        g_resumed++;
        return FE_OK;
    }
    if (action == FE_FAULT_RETHROW) {
        /* 处理者主动要求再走一轮（比如转交给上游）。
         * 它算一次新的投递，但**不受**"同现场"判据约束——否则"再抛一次"
         * 等于"立刻自杀"。不受约束的代价由深度 1 兜住：再抛一轮之后
         * 如果处理者自己出错，就是死。 */
        victim->last_fault_rip = 0;
        victim->last_fault_cr2 = 0;
        return FE_ERR_AGAIN;
    }
    /* FE_FAULT_KILL：照旧杀（与今天完全同一条路，退出码也一致）。 */
    return FE_ERR_ACCESS;
}
