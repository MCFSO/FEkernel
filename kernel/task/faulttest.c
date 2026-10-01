/* SPDX-License-Identifier: 0BSD */
/* K5 用户态异常处理者的内核自检（docs/18-user-fault-handler.md §5.1）。
 *
 * ★ 这一组**证明不了**什么（写在最前面，否则实现时会走捷径）★
 * 自检在**引导线程**里跑，**没有真的异常上下文**：它手工构造
 * `struct fe_regs` 并直接调 `fe_fault_deliver`。所以它证明的是
 * "投递 / 判据 / 回复这一整套**决策**对不对"，而**证明不了**
 * "从真实的 `#PF` 到投递"那一段——那一段只有 `user/bin/faulttest`
 * 那个真实客户端能证（ring 3 真异常 → isr_common → user_fault → 投递）。
 * 这条分工与 `fe_selftest_vmm_unmap` 的"自检可以直接对一个裸地址空间做"
 * 是同一回事：**自检验机制、客户端验路径**。
 *
 * ★ 每条正向都配一条会失败的反向 ★
 * 反向的意思不是"断言反过来"，而是"**同一件事在不该成立的条件下必须不成立**"
 * ——没有它，"投递成功"与"投递路径根本没在判"分不开。
 */
#include <fe/process.h>
#include <fe/sched/sched.h>
#include <fe/sched/thread.h>
#include <fe/ipc.h>
#include <fe/object.h>
#include <fe/task.h>
#include <fe/time.h>
#include <fe/kprintf.h>
#include <fe/string.h>
#include <fe/mm/vma.h>
#include <fe/mm/vmm.h>
#include <fe/mm/pmm.h>
#include <fe/mm/kheap.h>
#include <fe/user.h>            /* fe_task_create_user */

#define CHECK(cond) do { if (!(cond)) { fail++; } } while (0)

#define PROBE_CS 0x23ull
#define PROBE_SS 0x1Bull
#define PROBE_VA 0x31000000ull      /* 自检用的"用户缓冲"地址 */
#define PROBE_RIP 0x400123ull

/* 探针线程体（定义在下面） */
static void probe_victim_entry(void *arg);

/* ------------------------------------------------------------------ */
/* 探针：一个真的"用户任务"（地址空间 + 一页可写缓冲 + 一个处理者端点）     */
/* ------------------------------------------------------------------ */

struct probe {
    struct fe_task *task;
    fe_handle_t ep_handle;
    struct fe_endpoint *ep;
    phys_addr_t frame;
    struct fe_thread *victim;       /* 出错线程（探针线程） */
};

/* victim 线程体：它**不该**真的被调起来跑（自检只手工投递），
 * 但万一被调度到，它必须停下来而不是跑飞。 */
static void probe_victim_entry(void *arg)
{
    (void)arg;
    fe_thread_exit(0);
}

/* ★ 为什么自检要**手工造**一个用户任务 ★
 * `fe_fault_deliver` 拷现场用 `fe_copy_to_user`，它只认**当前任务**的
 * 地址空间，并且**逐页校验"这块内存确实可写"**。所以自检必须有一个"当前
 * 任务"具备：一个用户地址空间、一页真的映射成 user+write 的缓冲。
 * 引导线程（内核任务）这些都没有。
 *
 * ★ 为什么要 `fe_vmm_switch` ★ `fe_copy_to_user` 最后是一次
 * `memcpy(user_va, ...)`——那要求**当前 CR3 下**这个用户 VA 可达。
 * 只把 `fe_thread->task` 换掉不会换 CR3（换 CR3 只在调度路径上发生），
 * 所以自检自己切一次、用完切回来——`fe_selftest_vmm_unmap` 的
 * "新地址空间 → 切 CR3 → 读写 → 销毁 → 核对帧数复原"就是同一个做法。 */
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
    memset((void *)(uptr)(p->frame + fe_vmm_hhdm_offset()), 0, FE_FRAME_SIZE);
    if (fe_failed(fe_vmm_map(p->task->space, PROBE_VA, p->frame, FE_FRAME_SIZE,
                             FE_PTE_USER | FE_PTE_WRITE | FE_PTE_NX))) {
        fe_pmm_free_frame(p->frame);
        fe_object_unref(&p->task->hdr);
        p->task = NULL;
        return false;
    }
    if (fe_failed(fe_endpoint_create(p->task, 0, &p->ep_handle))) {
        fe_vmm_unmap(p->task->space, PROBE_VA, FE_FRAME_SIZE);
        fe_pmm_free_frame(p->frame);
        fe_object_unref(&p->task->hdr);
        p->task = NULL;
        return false;
    }
    struct fe_object_header *obj = NULL;
    if (fe_failed(fe_handle_lookup(&p->task->handles, p->ep_handle, 0, &obj)) ||
        obj->type != FE_OBJ_ENDPOINT) {
        return false;
    }
    p->ep = FE_OBJ_OF(obj, struct fe_endpoint);
    /* 登记处理者：缓冲区 = 那一页用户地址。 */
    if (fe_failed(fe_fault_set_handler(p->task, p->ep, PROBE_VA))) {
        return false;
    }
    return true;
}

/* 造一条"探针任务里的线程"当出错线程（`victim`）。
 *
 * ★ 为什么不拿自检自己当 victim ★ 自检自己属于**内核任务**
 * （`fe_task_kernel()`），`task` 为 NULL 的线程在 `fe_fault_deliver` 里
 * 直接返回 INVAL——那是对的语义（内核线程没有用户异常可投），
 * 但它不是这里要测的东西。 */
static void probe_spawn_victim(struct probe *p)
{
    struct fe_thread *cur = fe_thread_current();
    struct fe_task *saved = cur ? cur->task : NULL;
    if (cur) {
        cur->task = p->task;
    }
    p->victim = fe_thread_create("fault-victim", probe_victim_entry, NULL,
                                 8 * 1024, FE_PRIO_NORMAL);
    if (cur) {
        cur->task = saved;
    }
}

static void probe_free(struct probe *p)
{
    if (!p->task) {
        return;
    }
    fe_fault_clear_handler(p->task);
    fe_vmm_unmap(p->task->space, PROBE_VA, FE_FRAME_SIZE);
    fe_pmm_free_frame(p->frame);
    if (p->victim && p->victim->state != FE_THREAD_DEAD) {
        p->victim->kill_pending = true;
    }
    fe_object_unref(&p->task->hdr);
    p->task = NULL;
}

/* 进入/离开探针地址空间 + 临时成为探针任务。
 *
 * ★ 同时把"收件线程"钉成本线程 ★
 * §2.3.2 的判据是"出错线程不许是收件线程"，而 `fe_fault_reply` 校验
 * "回报者 == 收件线程"。自检里出错线程是 `p->victim`（不是本线程），
 * 所以把收件线程设成本线程是最自然的形状：**本线程当处理者**，
 * `victim` 当出错者，两者不同——正好满足那条判据。 */
static void probe_enter(struct probe *p, struct fe_task **saved_task,
                        struct fe_address_space **saved_space)
{
    struct fe_thread *me = fe_thread_current();
    *saved_task = me ? me->task : NULL;
    *saved_space = fe_vmm_kernel_space();
    if (me) {
        me->task = p->task;
    }
    p->task->handler_recv_thread = me;
    fe_vmm_switch(p->task->space);
}

static void probe_leave(struct fe_task *saved_task,
                        struct fe_address_space *saved_space)
{
    struct fe_thread *me = fe_thread_current();
    fe_vmm_switch(saved_space);
    if (me) {
        me->task = saved_task;
    }
}

/* 读回内核拷进用户缓冲的那份现场。
 * 必须在 probe_enter 之后（那时 CR3 是探针的，这个 VA 才可达）。 */
static void read_back_buf(struct fe_fault_regs *out)
{
    memcpy(out, (const void *)(uptr)PROBE_VA, sizeof(*out));
}

/* 造一个"来自 ring 3"的现场。★ 手工构造，真实路径由 user/bin/faulttest 验 ★ */
static void make_regs(struct fe_regs *r, u64 vector, u64 rip, u64 error)
{
    memset(r, 0, sizeof(*r));
    r->vector = vector;
    r->error = error;
    r->rip = rip;
    r->rsp = 0x7ffffff00000ull;
    r->cs = PROBE_CS;
    r->ss = PROBE_SS;
    r->rflags = 0x202;
    /* 每个通用寄存器一个**互不相同**的值：这样"逐字段相等"那条断言才真的
     * 在比字段（全填 0 的话漏拷一个字段也看不出来）。 */
    r->rax = 0xA1; r->rbx = 0xB2; r->rcx = 0xC3; r->rdx = 0xD4;
    r->rsi = 0xE5; r->rdi = 0xF6; r->rbp = 0x1718;
    r->r8 = 0x18; r->r9 = 0x19; r->r10 = 0x1A; r->r11 = 0x1B;
    r->r12 = 0x1C; r->r13 = 0x1D; r->r14 = 0x1E; r->r15 = 0x1F;
}

/* ------------------------------------------------------------------ */
/* 钩子：在**等待循环内部**观察事实，并当场替处理者回复                    */
/* ------------------------------------------------------------------ */

struct hook_state {
    u32  calls;                 /* 钩子被调了几次 = 循环真的轮了几次 */
    bool saw_slot_empty;        /* 第一轮：回复槽必须是空的 */
    bool saw_msg_queued;        /* 第一轮：端点里那条消息必须还在 */
    bool reply_now;             /* 要不要当场替处理者回一个 */
    bool set_rip;               /* 回复时把 rip 改成 new_rip（模拟"已处理，跳过"）*/
    u64  new_rip;
    u64  verdict;
    struct fe_task *t;
    struct fe_thread *replier;
    struct fe_fault_regs regs;  /* 处理者"手上那一份"：从用户缓冲读回来的事件 */
    u64  echoed_thread_id;      /* 从事件里读到的两个只读字段（供断言）*/
    u64  echoed_fault_count;
};

static struct hook_state g_hook;

/* ★ 钩子扮演的是**一个正确的处理者**，所以它做的事与真处理者逐字相同 ★
 * "recv 到事件 → 从自己的缓冲区读那份现场 → 当场回一个决定"。
 * 现场**从用户缓冲读回来**（而不是拿内核手里的那份），这一点很关键：
 * 回复里的 `thread_id`/`fault_count` 是**回显**，只有真的读过事件才回显得出；
 * 顺手也把"内核确实把这两个字段写进用户缓冲了"再证一遍。 */
static void wait_hook(struct fe_thread *victim, void *ctx)
{
    struct hook_state *h = (struct hook_state *)ctx;
    if (!h) {
        return;
    }
    if (h->calls == 0) {
        h->saw_slot_empty = (victim->fault_reply.arrived == false);
        h->saw_msg_queued = (h->t && h->t->handler_ep &&
                             h->t->handler_ep->count > 0);
        read_back_buf(&h->regs);            /* 处理者读它自己那一页 */
        h->echoed_thread_id = h->regs.thread_id;
        h->echoed_fault_count = h->regs.fault_count;
    }
    h->calls++;
    if (h->reply_now && h->calls == 1) {
        if (h->set_rip) {
            h->regs.rip = h->new_rip;       /* "已处理：跳过出错的那条指令" */
        }
        fe_fault_reply(h->t, h->replier, &h->regs, h->verdict);
    }
}

static void hook_reset(struct fe_task *t, struct fe_thread *replier,
                       bool reply, u64 verdict)
{
    memset(&g_hook, 0, sizeof(g_hook));
    g_hook.t = t;
    g_hook.replier = replier;
    g_hook.reply_now = reply;
    g_hook.verdict = verdict;
    fe_fault_set_wait_hook(wait_hook, &g_hook);
}

static void hook_off(void)
{
    fe_fault_set_wait_hook(NULL, NULL);
}

/* 清掉端点里积压的消息，让下一次投递从"空"开始数。
 * 实现放在 ipc.c（`message_free` 是那里的 static）——
 * 这样判据可以写成"投递后恰好一条"，比数差值更直白。 */
static void ep_drain(struct fe_endpoint *ep)
{
    (void)fe_endpoint_drain(ep);
}

/* ================================================================== */
/* F1：投递可达 + 现场逐字段相等（含反向：换 vector 必须是新值）            */
/* ================================================================== */
static u32 f1_deliver(void)
{
    u32 fail = 0;
    struct probe p;
    if (!probe_new(&p, "f1-user")) {
        fe_kprintf("        F1 探针任务创建失败\n");
        return 1;
    }
    probe_spawn_victim(&p);
    if (!p.victim) {
        fe_kprintf("        F1 探针线程创建失败\n");
        probe_free(&p);
        return 1;
    }
    u32 before = fail;
    struct fe_regs r;
    make_regs(&r, 14, PROBE_RIP, 0x7);
    const u64 cr2 = 0xDEADBEEF000ull;

    /* 处理者回 RESUME 且**不改现场**（KEEP_REGS）：这样投递侧拿到的就是
     * 它自己填的那一份，正好用来做"逐字段相等"。 */
    hook_reset(p.task, fe_thread_current(), true,
               FE_FAULT_RESUME | FE_FAULT_FLAG_KEEP_REGS);
    struct fe_task *st = NULL;
    struct fe_address_space *ss = NULL;
    probe_enter(&p, &st, &ss);

    u32 msgs_before = p.ep->count;
    struct fe_fault_regs out;
    memset(&out, 0, sizeof(out));
    fe_status_t s = fe_fault_deliver(p.victim, &r, cr2, &out);

    /* ---- 正向：真的投了一条消息 ---- */
    CHECK(fe_ok(s));
    CHECK(p.ep->count == msgs_before + 1);

    /* ---- 正向：用户缓冲区里那一份与传进去的现场逐字段相等 ---- */
    struct fe_fault_regs got;
    memset(&got, 0, sizeof(got));
    read_back_buf(&got);
    CHECK(got.vector == 14);
    CHECK(got.error_code == 0x7);
    CHECK(got.cr2 == cr2);
    CHECK(got.rip == PROBE_RIP);
    CHECK(got.cs == PROBE_CS);
    CHECK(got.ss == PROBE_SS);
    CHECK(got.thread_id == p.victim->id);
    CHECK(got.fault_count == 1);
    CHECK(got.rax == r.rax); CHECK(got.rbx == r.rbx); CHECK(got.rcx == r.rcx);
    CHECK(got.rdx == r.rdx); CHECK(got.rsi == r.rsi); CHECK(got.rdi == r.rdi);
    CHECK(got.rbp == r.rbp); CHECK(got.r8 == r.r8);   CHECK(got.r9 == r.r9);
    CHECK(got.r10 == r.r10); CHECK(got.r11 == r.r11); CHECK(got.r12 == r.r12);
    CHECK(got.r13 == r.r13); CHECK(got.r14 == r.r14); CHECK(got.r15 == r.r15);
    CHECK(got.rflags == r.rflags);
    CHECK(got.rsp == r.rsp);

    /* ---- 反向①：**换一个 vector 再投一次，消息里必须是新值** ----
     * 这一条证明"拷的是现场"，不是"拷了一个常量"。
     * 注意要换 rip：否则会撞上"同一个 (rip, cr2) 连续两次"那条判据。
     * 这一次处理者**真的改了现场**（把 rip 挪到出错指令之后）且不带
     * KEEP_REGS —— 于是回填必须用**处理者给的那一份**，而用户缓冲里
     * 内核写进去的那一份不受影响。两件事一起断言，才分得开
     * "回填用的是处理者的现场"与"回填用的是内核自己的拷贝"。 */
    ep_drain(p.ep);
    struct fe_regs r2;
    make_regs(&r2, 13, PROBE_RIP + 0x40, 0x0);   /* #GP */
    hook_reset(p.task, fe_thread_current(), true, FE_FAULT_RESUME);
    g_hook.set_rip = true;
    g_hook.new_rip = PROBE_RIP + 0x200;
    memset(&out, 0, sizeof(out));
    s = fe_fault_deliver(p.victim, &r2, 0, &out);
    CHECK(fe_ok(s));
    memset(&got, 0, sizeof(got));
    read_back_buf(&got);
    CHECK(got.vector == 13);
    CHECK(got.rip == PROBE_RIP + 0x40);          /* 缓冲里仍是内核写的原值 */
    CHECK(got.cr2 == 0);
    /* ---- 反向②：thread_id 必须是**出错线程**的 id，不是别的 ---- */
    CHECK(got.thread_id == p.victim->id);
    CHECK(got.thread_id != fe_thread_current()->id);
    /* ---- 正向：fault_count 是**每线程**的"第几次"（§2.2.3），从 1 起 ---- */
    CHECK(got.fault_count == 2);
    CHECK(p.victim->fault_count == 2);
    /* ---- 正向：处理者回显的两个只读字段与内核写进去的一致 ---- */
    CHECK(g_hook.echoed_thread_id == p.victim->id);
    CHECK(g_hook.echoed_fault_count == 2);
    /* ---- 正向：回填用的是**处理者给的那一份**（rip 被改过），
     *      其余字段逐字段等于事件里那一份 ---- */
    CHECK(out.rip == PROBE_RIP + 0x200);
    CHECK(out.rip != got.rip);
    CHECK(out.vector == 13 && out.error_code == 0 && out.cr2 == 0);
    CHECK(out.cs == PROBE_CS && out.ss == PROBE_SS);
    CHECK(out.rax == r2.rax && out.rbx == r2.rbx && out.rcx == r2.rcx);
    CHECK(out.rdx == r2.rdx && out.rsi == r2.rsi && out.rdi == r2.rdi);
    CHECK(out.rbp == r2.rbp && out.r8 == r2.r8 && out.r9 == r2.r9);
    CHECK(out.r10 == r2.r10 && out.r11 == r2.r11 && out.r12 == r2.r12);
    CHECK(out.r13 == r2.r13 && out.r14 == r2.r14 && out.r15 == r2.r15);
    CHECK(out.rflags == r2.rflags && out.rsp == r2.rsp);

    probe_leave(st, ss);
    hook_off();
    if (fail == before) {
        fe_kprintf("        F1：投递可达、现场逐字段相等（vector/cr2/thread_id/"
                   "fault_count + 全部通用寄存器），换 vector 后是新值\n");
    }
    probe_free(&p);
    return fail;
}

/* ================================================================== */
/* F2：闸门在前（task->dying 时不投递）                                */
/* ================================================================== */
static u32 f2_gate(void)
{
    u32 fail = 0;
    struct probe p, q;
    if (!probe_new(&p, "f2-dying") || !probe_new(&q, "f2-live")) {
        fe_kprintf("        F2 探针任务创建失败\n");
        return 1;
    }
    probe_spawn_victim(&p);
    /* 给 q 也造一条 victim */
    struct fe_thread *qv = NULL;
    {
        struct fe_thread *cur = fe_thread_current();
        struct fe_task *saved = cur ? cur->task : NULL;
        if (cur) { cur->task = q.task; }
        qv = fe_thread_create("f2-live-victim", probe_victim_entry, NULL,
                              8 * 1024, FE_PRIO_NORMAL);
        if (cur) { cur->task = saved; }
    }
    if (!p.victim || !qv) {
        fe_kprintf("        F2 探针线程创建失败\n");
        probe_free(&p); probe_free(&q);
        return 1;
    }
    u32 before = fail;
    struct fe_regs r;
    make_regs(&r, 6, PROBE_RIP, 0);            /* #UD */
    struct fe_fault_regs out;

    struct fe_task *st = NULL;
    struct fe_address_space *ss = NULL;
    probe_enter(&p, &st, &ss);

    /* ---- 正向：任务正在被终止 → 不投递、端点空 ---- */
    ep_drain(p.ep);
    p.task->dying = true;
    hook_reset(p.task, fe_thread_current(), false, 0);
    memset(&out, 0, sizeof(out));
    fe_status_t s = fe_fault_deliver(p.victim, &r, 0, &out);
    CHECK(!fe_ok(s));                          /* 不该成功 */
    CHECK(p.ep->count == 0);                   /* 端点里一条都没有 */

    /* ---- 反向：同一时刻另一个**没有**被终止的任务用同一现场投递 → 有消息 ----
     * 这一条证明"没投递"是 `dying` 造成的，不是投递路径坏了。 */
    p.task->dying = false;                     /* 让 p 回到可用状态，先测 q */
    p.task->handler_recv_thread = fe_thread_current();
    ep_drain(q.ep);
    q.task->handler_recv_thread = fe_thread_current();
    hook_reset(q.task, fe_thread_current(), true,
               FE_FAULT_RESUME | FE_FAULT_FLAG_KEEP_REGS);
    /* q 的 victim 用同样的现场投（q 自己的缓冲要能写：q 的 VA 是同一个
     * 地址、也在 q 的地址空间里映过——所以这里要先切到 q 的空间。 */
    fe_vmm_switch(q.task->space);
    {
        struct fe_thread *me = fe_thread_current();
        struct fe_task *saved = me->task;
        me->task = q.task;
        memset(&out, 0, sizeof(out));
        s = fe_fault_deliver(qv, &r, 0, &out);
        me->task = saved;
    }
    CHECK(fe_ok(s));
    CHECK(q.ep->count == 1);
    fe_vmm_switch(p.task->space);

    probe_leave(st, ss);
    hook_off();
    if (fail == before) {
        fe_kprintf("        F2：闸门在前（dying 的任务不投递、端点空；"
                   "同一现场投给未被终止的任务则有一条消息）\n");
    }
    probe_free(&p);
    probe_free(&q);
    return fail;
}

/* ================================================================== */
/* F3：顺序——按需分页在前（不进处理者）                                */
/* ================================================================== */
static u32 f3_order(void)
{
    u32 fail = 0;
    struct probe p;
    if (!probe_new(&p, "f3-order")) {
        fe_kprintf("        F3 探针任务创建失败\n");
        return 1;
    }
    u32 before = fail;
    struct fe_task *st = NULL;
    struct fe_address_space *ss = NULL;
    probe_enter(&p, &st, &ss);

    /* 在一个 ANON 区间里造一次缺页：`fe_user_resolve_fault` 必须返回 true
     * （它按 VMA 的权限补一页），于是**不会**走到处理者那条路。 */
    const virt_addr_t anon_va = 0x32000000ull;
    const virt_addr_t anon_end = anon_va + 4 * FE_FRAME_SIZE;
    bool added = fe_ok(fe_vma_add(p.task->vmas, anon_va, anon_end,
                                  FE_VMA_READ | FE_VMA_WRITE | FE_VMA_ANON,
                                  NULL));
    CHECK(added);
    p.task->stack_low = 0;      /* 别让栈增长那条路插进来 */

    u64 resolved_before = fe_pf_resolved_count();
    bool handled = fe_user_resolve_fault(p.task, anon_va, 0x6 /* 写 */);
    CHECK(handled);                                     /* 按需分页认领了 */
    (void)resolved_before;
    CHECK(fe_vmm_translate(p.task->space, anon_va) != 0);   /* 页真的补上了 */

    /* ---- 反向：同一个区间的**区间外**地址 → 解析失败 → 该进处理者 ---- */
    const virt_addr_t outside = anon_end + 0x100000ull;
    bool handled2 = fe_user_resolve_fault(p.task, outside, 0x6);
    CHECK(!handled2);                                   /* 内核不认领 */
    CHECK(fe_vmm_translate(p.task->space, outside) == 0);

    /* 这一条把"两条路互斥"钉死：认领的那条不会进处理者，不认领的那条会。
     * "会进处理者"由 F1 已经证明（手工投递成功），这里只证明**互斥**。 */
    fe_vmm_unmap_free(p.task->space, anon_va, FE_FRAME_SIZE);
    fe_vma_remove_range(p.task->vmas, anon_va, anon_end);

    probe_leave(st, ss);
    if (fail == before) {
        fe_kprintf("        F3：顺序（ANON 区间的缺页被按需分页认领并真的补了页；"
                   "区间外的同一类地址不被认领）——两条路互斥\n");
    }
    probe_free(&p);
    return fail;
}

/* ================================================================== */
/* F4：深度 1（fault_depth != 0 时不投递）                             */
/* ================================================================== */
static u32 f4_depth(void)
{
    u32 fail = 0;
    struct probe p;
    if (!probe_new(&p, "f4-depth")) {
        fe_kprintf("        F4 探针任务创建失败\n");
        return 1;
    }
    probe_spawn_victim(&p);
    if (!p.victim) {
        fe_kprintf("        F4 探针线程创建失败\n");
        probe_free(&p);
        return 1;
    }
    u32 before = fail;
    struct fe_regs r;
    make_regs(&r, 14, PROBE_RIP, 0x6);
    struct fe_fault_regs out;
    struct fe_task *st = NULL;
    struct fe_address_space *ss = NULL;
    probe_enter(&p, &st, &ss);

    /* ---- 正向：深度已经是 1 → 不投递 ---- */
    ep_drain(p.ep);
    p.task->fault_depth = 1;
    p.task->fault_owner = p.victim;
    hook_reset(p.task, fe_thread_current(), false, 0);
    memset(&out, 0, sizeof(out));
    fe_status_t s = fe_fault_deliver(p.victim, &r, 0, &out);
    CHECK(!fe_ok(s));
    CHECK(p.ep->count == 0);
    p.task->fault_depth = 0;
    p.task->fault_owner = NULL;

    /* ---- 反向①：深度 0 时同样的现场 → 有消息 ---- */
    ep_drain(p.ep);
    p.victim->last_fault_rip = 0;
    p.victim->last_fault_cr2 = 0;
    hook_reset(p.task, fe_thread_current(), true,
               FE_FAULT_RESUME | FE_FAULT_FLAG_KEEP_REGS);
    memset(&out, 0, sizeof(out));
    s = fe_fault_deliver(p.victim, &r, 0, &out);
    CHECK(fe_ok(s));
    CHECK(p.ep->count == 1);
    /* ★ 收尾一定要查：深度必须已经减回 0 ★
     * 漏减一的症状是"处理者工作过一次之后再也不工作了"，很难查。 */
    CHECK(p.task->fault_depth == 0);
    CHECK(p.task->fault_owner == NULL);

    /* ---- 反向②：注销处理者之后投递 → 端点空（"没登记"独立于深度）---- */
    ep_drain(p.ep);
    fe_fault_clear_handler(p.task);
    p.victim->last_fault_rip = 0;
    p.victim->last_fault_cr2 = 0;
    hook_reset(p.task, fe_thread_current(), false, 0);
    memset(&out, 0, sizeof(out));
    s = fe_fault_deliver(p.victim, &r, 0, &out);
    CHECK(s == FE_ERR_NOENT);
    CHECK(p.ep->count == 0);
    /* 重新登记，收尾路径才干净 */
    fe_fault_set_handler(p.task, p.ep, PROBE_VA);

    probe_leave(st, ss);
    hook_off();
    if (fail == before) {
        fe_kprintf("        F4：深度 1（depth=1 不投递、depth=0 投递、"
                   "注销后投递返回 NOENT 且端点空）——且每次投递后深度都减回 0\n");
    }
    probe_free(&p);
    return fail;
}

/* ================================================================== */
/* F5：活锁判据（同一个 (rip, cr2) 连续两次不投递）                     */
/* ================================================================== */
static u32 f5_livelock(void)
{
    u32 fail = 0;
    struct probe p;
    if (!probe_new(&p, "f5-live")) {
        fe_kprintf("        F5 探针任务创建失败\n");
        return 1;
    }
    probe_spawn_victim(&p);
    if (!p.victim) {
        fe_kprintf("        F5 探针线程创建失败\n");
        probe_free(&p);
        return 1;
    }
    u32 before = fail;
    struct fe_regs r;
    make_regs(&r, 14, PROBE_RIP, 0x6);
    const u64 cr2 = 0xC0FFEE000ull;
    struct fe_fault_regs out;
    struct fe_task *st = NULL;
    struct fe_address_space *ss = NULL;
    probe_enter(&p, &st, &ss);

    /* 第一次：投递成功（处理者回 RESUME 但**不改 rip** —— 这正是一个
     * "处理不了又不想让它死"的处理者会写出来的东西）。 */
    ep_drain(p.ep);
    hook_reset(p.task, fe_thread_current(), true, FE_FAULT_RESUME);
    memset(&out, 0, sizeof(out));
    fe_status_t s = fe_fault_deliver(p.victim, &r, cr2, &out);
    CHECK(fe_ok(s));
    CHECK(p.ep->count == 1);

    /* ---- 正向：同一个 (rip, cr2) 第二次 → **不投递** ---- */
    ep_drain(p.ep);
    hook_reset(p.task, fe_thread_current(), false, 0);
    memset(&out, 0, sizeof(out));
    s = fe_fault_deliver(p.victim, &r, cr2, &out);
    CHECK(!fe_ok(s));
    CHECK(p.ep->count == 0);

    /* ---- 反向：换一个 rip 再投 → **投递成功** ----
     * 证明判据比的是"同一个现场"，不是"第二次就杀"。 */
    ep_drain(p.ep);
    struct fe_regs r2;
    make_regs(&r2, 14, PROBE_RIP + 0x100, 0x6);
    hook_reset(p.task, fe_thread_current(), true,
               FE_FAULT_RESUME | FE_FAULT_FLAG_KEEP_REGS);
    memset(&out, 0, sizeof(out));
    s = fe_fault_deliver(p.victim, &r2, cr2, &out);
    CHECK(fe_ok(s));
    CHECK(p.ep->count == 1);

    probe_leave(st, ss);
    hook_off();
    if (fail == before) {
        fe_kprintf("        F5：活锁判据（同 (rip,cr2) 第二次不投递；换 rip 后"
                   "投递成功——比的是同一个现场，不是第二次就杀）\n");
    }
    probe_free(&p);
    return fail;
}

/* ================================================================== */
/* F6：回复校验（错的 seq / 错的回报者 → INVAL 且什么都不做）            */
/* ================================================================== */
static u32 f6_reply_check(void)
{
    u32 fail = 0;
    struct probe p;
    if (!probe_new(&p, "f6-reply")) {
        fe_kprintf("        F6 探针任务创建失败\n");
        return 1;
    }
    probe_spawn_victim(&p);
    if (!p.victim) {
        fe_kprintf("        F6 探针线程创建失败\n");
        probe_free(&p);
        return 1;
    }
    u32 before = fail;
    struct fe_regs r;
    make_regs(&r, 14, PROBE_RIP, 0x6);
    struct fe_fault_regs out;
    struct fe_task *st = NULL;
    struct fe_address_space *ss = NULL;
    probe_enter(&p, &st, &ss);

    /* 这一次**不由钩子回复**：钩子只观察，回复由下面的代码显式做，
     * 这样才能分别试"错的"与"对的"各种回复。 */
    ep_drain(p.ep);
    hook_reset(p.task, fe_thread_current(), false, 0);
    memset(&out, 0, sizeof(out));

    /* ★ 这里不能真的调 fe_fault_deliver：它会跑满 20 万轮。
     * 用"手工把状态摆成投递之后的样子"来测回复校验——回复校验本身就是
     * 一个纯函数性质（看三个字段对不对），与"谁把状态摆成这样"无关。
     * 深度/记账那几条已经由 F4/F5 用真投递验过了。 */
    p.task->fault_depth = 1;
    p.task->fault_owner = p.victim;
    p.task->fault_seq = 7;
    p.victim->fault_count = 3;
    p.victim->fault_reply.arrived = false;
    p.victim->fault_reply.seq = 3;      /* 本轮期望的回显轮次号 */

    /* 处理者手上"那一份现场"：身份字段按投递时收到的值回显。 */
    struct fe_fault_regs good;
    memset(&good, 0, sizeof(good));
    good.rip = 0x1234;
    good.vector = 14;
    good.cs = PROBE_CS;
    good.ss = PROBE_SS;
    good.thread_id = p.victim->id;
    good.fault_count = 3;

    /* ---- 反向①：错的回报者（出错线程冒充处理者）→ INVAL 且不做任何事 ---- */
    fe_status_t s = fe_fault_reply(p.task, p.victim, &good, FE_FAULT_RESUME);
    CHECK(s == FE_ERR_INVAL);
    CHECK(p.victim->fault_reply.arrived == false);      /* 槽仍为空 */

    /* ---- 反向②：**错的 thread_id**（把别人的现场装到这个线程上）----
     * 这一条正是 §2.3 说的"跨线程现场伪装"：没有它，"无条件接受任何回复"
     * 也能通过下面那条正向。 */
    struct fe_fault_regs bad = good;
    bad.thread_id = fe_thread_current()->id;            /* 不是出错线程 */
    s = fe_fault_reply(p.task, fe_thread_current(), &bad, FE_FAULT_RESUME);
    CHECK(s == FE_ERR_INVAL);
    CHECK(p.victim->fault_reply.arrived == false);

    /* ---- 反向③：**错的 fault_seq**（回的是上一轮）→ INVAL ---- */
    bad = good;
    bad.fault_count = 2;                                /* 上一轮的号 */
    s = fe_fault_reply(p.task, fe_thread_current(), &bad, FE_FAULT_RESUME);
    CHECK(s == FE_ERR_INVAL);
    CHECK(p.victim->fault_reply.arrived == false);

    /* ---- 反向④：没有现场（regs == NULL）→ INVAL ---- */
    s = fe_fault_reply(p.task, fe_thread_current(), NULL, FE_FAULT_RESUME);
    CHECK(s == FE_ERR_INVAL);
    CHECK(p.victim->fault_reply.arrived == false);

    /* ---- 反向⑤：非法 verdict → INVAL ---- */
    s = fe_fault_reply(p.task, fe_thread_current(), &good, 0x99);
    CHECK(s == FE_ERR_INVAL);
    CHECK(p.victim->fault_reply.arrived == false);

    /* ---- 反向⑥：没有正在等的投递（深度 0）→ INVAL，即使别的都对 ---- */
    p.task->fault_depth = 0;
    s = fe_fault_reply(p.task, fe_thread_current(), &good, FE_FAULT_RESUME);
    CHECK(s == FE_ERR_INVAL);
    CHECK(p.victim->fault_reply.arrived == false);
    p.task->fault_depth = 1;

    /* ---- 正向：**三项全对** → OK、槽被写满、深度不被动过 ---- */
    s = fe_fault_reply(p.task, fe_thread_current(), &good, FE_FAULT_RESUME);
    CHECK(fe_ok(s));
    CHECK(p.victim->fault_reply.arrived == true);
    CHECK(p.victim->fault_reply.verdict == FE_FAULT_RESUME);
    CHECK(p.victim->fault_reply.regs.rip == 0x1234);
    CHECK(p.victim->fault_reply.regs.thread_id == p.victim->id);
    CHECK(p.task->fault_depth == 1);        /* 深度归投递侧收尾管，回复不动它 */

    /* 收尾：把状态清回去（深度不能留着，否则这个任务以后的异常全被杀）。 */
    p.task->fault_depth = 0;
    p.task->fault_owner = NULL;
    p.victim->fault_reply.arrived = false;

    probe_leave(st, ss);
    hook_off();
    if (fail == before) {
        fe_kprintf("        F6：回复校验（错回报者/错 thread_id/错 fault_count/"
                   "无现场/非法 verdict/深度 0 六条都被拒且槽仍为空；"
                   "三项全对的回复 OK 且槽被写满）\n");
    }
    probe_free(&p);
    return fail;
}

/* ================================================================== */
/* F8：自投递（出错线程 == 收件线程 → 不投递）                          */
/* ================================================================== */
static u32 f8_self(void)
{
    u32 fail = 0;
    struct probe p;
    if (!probe_new(&p, "f8-self")) {
        fe_kprintf("        F8 探针任务创建失败\n");
        return 1;
    }
    probe_spawn_victim(&p);
    struct fe_thread *other = NULL;
    {
        struct fe_thread *cur = fe_thread_current();
        struct fe_task *saved = cur ? cur->task : NULL;
        if (cur) { cur->task = p.task; }
        other = fe_thread_create("f8-other", probe_victim_entry, NULL,
                                 8 * 1024, FE_PRIO_NORMAL);
        if (cur) { cur->task = saved; }
    }
    if (!p.victim || !other) {
        fe_kprintf("        F8 探针线程创建失败\n");
        probe_free(&p);
        return 1;
    }
    u32 before = fail;
    struct fe_regs r;
    make_regs(&r, 14, PROBE_RIP, 0x6);
    struct fe_fault_regs out;
    struct fe_task *st = NULL;
    struct fe_address_space *ss = NULL;
    probe_enter(&p, &st, &ss);

    /* ---- 正向：收件线程设成**出错线程自己** → 不投递 ---- */
    ep_drain(p.ep);
    p.task->handler_recv_thread = p.victim;
    hook_reset(p.task, fe_thread_current(), false, 0);
    memset(&out, 0, sizeof(out));
    fe_status_t s = fe_fault_deliver(p.victim, &r, 0, &out);
    CHECK(!fe_ok(s));
    CHECK(p.ep->count == 0);

    /* ---- 反向：收件线程设成**另一条**线程，同一个现场 → 有消息 ----
     * 这一条证明"不投递"是那条判据造成的，不是投递坏了。
     * ★ 这条最容易漏，因为它在单线程用法下才暴露。 */
    ep_drain(p.ep);
    p.task->handler_recv_thread = other;
    p.victim->last_fault_rip = 0;
    p.victim->last_fault_cr2 = 0;
    hook_reset(p.task, other, true, FE_FAULT_RESUME | FE_FAULT_FLAG_KEEP_REGS);
    memset(&out, 0, sizeof(out));
    s = fe_fault_deliver(p.victim, &r, 0, &out);
    CHECK(fe_ok(s));
    CHECK(p.ep->count == 1);

    probe_leave(st, ss);
    hook_off();
    if (fail == before) {
        fe_kprintf("        F8：自投递（出错线程就是收件线程时不投递；换成另一条"
                   "线程则投递成功）——这条在单线程用法下才暴露\n");
    }
    probe_free(&p);
    return fail;
}

/* ================================================================== */
/* F9：回复槽必须清（否则上一轮的回复会变成这一轮的答案）                */
/* ================================================================== */
static u32 f9_slot_cleared(void)
{
    u32 fail = 0;
    struct probe p;
    if (!probe_new(&p, "f9-slot")) {
        fe_kprintf("        F9 探针任务创建失败\n");
        return 1;
    }
    probe_spawn_victim(&p);
    if (!p.victim) {
        fe_kprintf("        F9 探针线程创建失败\n");
        probe_free(&p);
        return 1;
    }
    u32 before = fail;
    struct fe_regs r;
    make_regs(&r, 14, PROBE_RIP, 0x6);
    struct fe_fault_regs out;
    struct fe_task *st = NULL;
    struct fe_address_space *ss = NULL;
    probe_enter(&p, &st, &ss);

    /* 第一次：投递 → 回复（走真路径，钩子当场回）。 */
    ep_drain(p.ep);
    hook_reset(p.task, fe_thread_current(), true,
               FE_FAULT_RESUME | FE_FAULT_FLAG_KEEP_REGS);
    memset(&out, 0, sizeof(out));
    fe_status_t s = fe_fault_deliver(p.victim, &r, 0, &out);
    CHECK(fe_ok(s));
    /* 回复被取走之后槽必须已经被清（不然下一次会捡到它）。 */
    CHECK(p.victim->fault_reply.arrived == false);

    /* ---- 正向：第二次投递而**不回复** → 必须等到轮数用完 ---- ★
     *
     * ★ 这一条正是在读"投递时有没有清槽" ★
     * 若没清，钩子看到的第一轮就会是 `arrived == true`，循环立刻退出，
     * `fe_fault_last_wait_rounds()` 就是 0；清了则钩子会跑满整轮。
     * 两个值天差地别，所以"不清"当场可见。 */
    ep_drain(p.ep);
    p.victim->last_fault_rip = 0;
    p.victim->last_fault_cr2 = 0;
    hook_reset(p.task, fe_thread_current(), false, 0);   /* 不回复 */
    memset(&out, 0, sizeof(out));
    s = fe_fault_deliver(p.victim, &r, 0, &out);
    CHECK(!fe_ok(s));                       /* 等不到回复 → 失败 */
    CHECK(g_hook.calls == (u32)FE_FAULT_WAIT_ROUNDS);     /* 钩子跑满了整轮 */
    CHECK(fe_fault_last_wait_rounds() == (u64)FE_FAULT_WAIT_ROUNDS);
    CHECK(g_hook.saw_slot_empty);           /* 第一轮时槽确实是空的 */
    CHECK(g_hook.saw_msg_queued);           /* 第一轮时那条消息确实在队列里 */
    /* 深度也必须减回去了 */
    CHECK(p.task->fault_depth == 0);

    probe_leave(st, ss);
    hook_off();
    if (fail == before) {
        fe_kprintf("        F9：回复槽必须清（第二次不回复时等满 %u 轮，"
                   "且第一轮时槽为空、消息在队列里）\n", (u32)FE_FAULT_WAIT_ROUNDS);
    }
    probe_free(&p);
    return fail;
}

/* ================================================================== */
/* F10：ABI 钉子（内核填出来的字节数 == 用户侧镜像的数值）               */
/* ================================================================== */
static u32 f10_abi(void)
{
    u32 fail = 0;
    u32 before = fail;
    /* 编译器已经用 FE_STATIC_ASSERT 判过"布局没变"；这一条判的是
     * "两个数值常量与结构体是同一件事"——宏与结构体对不上正是
     * docs/13 §2 记的那次实测事故（FE_TASK_STRIDE 写成 48、实际 64）。 */
    CHECK(sizeof(struct fe_fault_regs) == FE_FAULT_REGS_SIZE);
    CHECK(FE_FAULT_REGS_SIZE == FE_FAULT_REGS_SIZE_X);
    if (fail == before) {
        fe_kprintf("        F10：ABI 钉子（sizeof(fe_fault_regs)=%u == "
                   "FE_FAULT_REGS_SIZE == 用户侧镜像）\n",
                   (u32)sizeof(struct fe_fault_regs));
    }
    return fail;
}

/* ================================================================== */

u32 fe_selftest_fault(void)
{
    u32 fail = 0;
    fe_kprintf("        K5：处理者登记 / 投递判据 / 回复校验"
               "（等待上界 %u 轮，见 FE_FAULT_WAIT_ROUNDS 的注释）\n",
               (u32)FE_FAULT_WAIT_ROUNDS);
    fail += f10_abi();
    fail += f1_deliver();
    fail += f2_gate();
    fail += f3_order();
    fail += f4_depth();
    fail += f5_livelock();
    fail += f6_reply_check();
    fail += f8_self();
    fail += f9_slot_cleared();
    return fail;
}
