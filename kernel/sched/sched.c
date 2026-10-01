/* SPDX-License-Identifier: 0BSD */
#include <fe/sched/sched.h>
#include <fe/sched/thread.h>
#include <fe/mm/kheap.h>
#include <fe/mm/vmm.h>
#include <fe/mm/pmm.h>
#include <fe/gdt.h>
#include <fe/idt.h>
#include <fe/time.h>
#include <fe/vectors.h>
#include <fe/kprintf.h>
#include <fe/panic.h>
#include <fe/string.h>
#include <fe/io.h>
#include <fe/regs.h>
#include <fe/task.h>
#include <fe/user.h>            /* TLS 布局常量（FE_TLS_*）与用户地址空间定义 */
#include <fe/syscall.h>
#include <fe/process.h>
#include <fe/fpu.h>

/* ------------------------------------------------------------------ */
/* 内部状态                                                            */
/* ------------------------------------------------------------------ */

struct fe_runqueue {
    struct fe_thread *head;
    struct fe_thread *tail;
};

static struct fe_runqueue g_runq[FE_THREAD_PRIO_LEVELS];
static u32  g_ready_mask;           /* 位 i 置位 = 优先级 i 的就绪队列非空 */
static struct fe_thread *g_current;
static struct fe_thread *g_all;     /* 全局线程链 */
static struct fe_thread *g_sleep_list;
static struct fe_thread *g_zombies;

static bool g_sched_enabled;
static volatile bool g_need_resched;
static u64  g_next_id = 1;
static u32  g_default_slice = 10;   /* 10 个节拍：1000Hz 下即 10ms */
static u64  g_created_total;

/* 线程死亡的唯一登记处（变僵尸 + 唤醒父进程的 wait）。
 * 前向声明：闸门 fe_sched_maybe_switch 在切换核心那一节，而它的定义在
 * 退出那一节——顺序是文件结构决定的，但两者是同一件事的两端。 */
static void thread_mark_dead(struct fe_thread *t, i32 code);

/* 空闲线程栈可以小一些 */
#define IDLE_STACK_SIZE 8192

/* ------------------------------------------------------------------ */
/* 就绪队列                                                            */
/* ------------------------------------------------------------------ */

static void rq_push(struct fe_thread *t)
{
    u32 p = t->priority;
    FE_ASSERT(p < FE_THREAD_PRIO_LEVELS);
    /* ★ 不变式：**一个线程被 push 时不能已经在就绪队列里** ★
     *
     * ★ 为什么这条断言值钱（它可能同时解释两种症状）★
     * 双 push 会在就绪链上造出**环**（同一个节点被接两次，`rq_next`
     * 互相指回去），而一个环能同时产生我们实测到的两种症状：
     *   - `rq_pick` 顺着环走，可能挑到一个指针已经失效的节点 ⇒ 读
     *     `next->task->iopb_slot` 就是 #PF（`CR2 = 任务地址 + 0x1060`）；
     *   - 队列永远转不回 `main` ⇒ 一次 yield 切走之后再也不回来（挂死）。
     * **两个症状、一个原因**——这比"逐个加固"更可能是真相，
     * 所以这里不是防御性代码，它是一次**证据收集**。
     *
     * 判据用"指针是不是已经挂在链上"（`rq_next`/`rq_prev`/队头队尾），
     * **不用"就绪位为 0"**：`g_ready_mask` 的位是**按优先级**的，同一级里
     * 只要有**别的**线程就绪，那一位就是 1——拿它当"这个线程在不在队列里"
     * 的判据会误报。
     *
     * ★ 违反时：点名 + **不 push** ★ 它已经在队列里了，再 push 一次正是
     * 造环的那一步；点名是为了让调用者（凶手）现形。 */
    if (t->rq_next != NULL || t->rq_prev != NULL ||
        g_runq[p].head == t || g_runq[p].tail == t) {
        fe_kprintf("[调度] **rq_push 收到一个已经在就绪队列里的线程**："
                   "%s(id=%llu state=%u prio=%u) rq_prev=%p rq_next=%p "
                   "队头=%p 队尾=%p 调用者=%#llx —— 已丢弃这次 push（否则造环）\n",
                   t->name, (unsigned long long)t->id, t->state, p,
                   (void *)t->rq_prev, (void *)t->rq_next,
                   (void *)g_runq[p].head, (void *)g_runq[p].tail,
                   (unsigned long long)(uptr)__builtin_return_address(0));
        return;
    }
    t->rq_next = NULL;
    t->rq_prev = g_runq[p].tail;
    if (g_runq[p].tail) {
        g_runq[p].tail->rq_next = t;
    } else {
        g_runq[p].head = t;
    }
    g_runq[p].tail = t;
    g_ready_mask |= (1u << p);
}

static void rq_remove(struct fe_thread *t)
{
    u32 p = t->priority;
    if (t->rq_prev) {
        t->rq_prev->rq_next = t->rq_next;
    } else if (g_runq[p].head == t) {
        g_runq[p].head = t->rq_next;
    }
    if (t->rq_next) {
        t->rq_next->rq_prev = t->rq_prev;
    } else if (g_runq[p].tail == t) {
        g_runq[p].tail = t->rq_prev;
    }
    t->rq_next = NULL;
    t->rq_prev = NULL;
    if (!g_runq[p].head) {
        g_ready_mask &= ~(1u << p);
    }
}

/* ★ 就绪链体检：抓"环"与"不在册的节点"（第 5 步第三步）★
 *
 * ★ 为什么要有它 ★ `rq_pick` 只取队头、`rq_remove` 只动相邻指针——两者都
 * **不遍历整条链**，所以一条被双 push 弄成环的链不会有任何症状，直到
 * 有人顺着它走：`fe_thread_dump_all` 会转不出来，而"某个线程永远轮不到"
 * 就是一次 yield 切走之后再也回不来（我们追了几轮的挂死）。
 * 这条体检是**唯一**能主动看见"链本身坏了"的地方。
 *
 * ★ 两条约束（都不许违反）★
 *   1. **检测器自己不许崩**：遇到"不在 `g_all` 在册链上"的指针时，**只打
 *      指针值就停**，绝不解引用它——读一块已释放内存的 `name`/`state`
 *      正是第 3 步那个 #PF 的形态（`CR2 = 任务地址 + 0x1060`）；
 *   2. **有界**：最多走 `8 × 在册线程数 + 16` 步，超了就打"疑似成环"并把
 *      走过的节点逐个打印，然后停。
 *
 * 只在发现异常时打印；为了不拖慢每次调度，每 64 次 pick 才体检一次。 */
static void rq_audit(u32 prio)
{
    /* 在册线程数（同时也是"合法节点集合"） */
    u32 registered = 0;
    for (struct fe_thread *q = g_all; q; q = q->all_next) {
        registered++;
    }
    u32 budget = registered * 8u + 16u;
    u32 steps = 0;
    for (struct fe_thread *q = g_runq[prio].head; q; q = q->rq_next) {
        if (++steps > budget) {
            fe_kprintf("[调度] **就绪链疑似成环**（prio=%u，走了 %u 步 > 上限 %u）"
                       "，走过的节点：\n", prio, steps, budget);
            u32 k = 0;
            for (struct fe_thread *r = g_runq[prio].head; r && k < steps; r = r->rq_next) {
                fe_kprintf("          第 %u 个=%p", k, (void *)r);
                for (struct fe_thread *s = g_all; s; s = s->all_next) {
                    if (s == r) {
                        fe_kprintf("（在册：%s id=%llu state=%u）", r->name,
                                   (unsigned long long)r->id, r->state);
                        break;
                    }
                }
                fe_kprintf("\n");
                k++;
            }
            return;
        }
        /* ★ 在册性检查：**只比指针值**，不解引用 ★ */
        bool known = false;
        for (struct fe_thread *s = g_all; s; s = s->all_next) {
            if (s == q) {
                known = true;
                break;
            }
        }
        if (!known) {
            fe_kprintf("[调度] **就绪链上有不在册的节点** %p（prio=%u，第 %u 个）"
                       "——它可能已经被回收；不解引用，体检到此为止\n",
                       (void *)q, prio, steps);
            return;
        }
    }
}

/* 取出最高优先级的就绪线程（已从队列摘除） */
static struct fe_thread *rq_pick(void)
{
    if (g_ready_mask == 0) {
        return NULL;
    }
    u32 p = 31u - (u32)__builtin_clz(g_ready_mask);
    /* 体检（每 64 次一次，只在异常时打印）：见 rq_audit 的说明。 */
    static u32 audit_countdown;
    if (++audit_countdown >= 64u) {
        audit_countdown = 0;
        rq_audit(p);
    }
    struct fe_thread *t = g_runq[p].head;
    if (t) {
        rq_remove(t);
    }
    return t;
}

/* ------------------------------------------------------------------ */
/* 线程栈与初始现场                                                    */
/* ------------------------------------------------------------------ */

/* 线程入口蹦床：从初始现场恢复的 r12/r13 中取回真正的入口与参数。
 * 有了它，线程函数即使 return 也不会跳进垃圾地址，而是走正常的退出流程。 */
static FE_NORETURN void thread_trampoline(void)
{
    fe_thread_entry_t entry;
    void *arg;
    __asm__ volatile("mov %%r12, %0" : "=r"(entry));
    __asm__ volatile("mov %%r13, %0" : "=r"(arg));
    entry(arg);
    fe_thread_exit(0);
}

/* 把新线程的栈伪造成「刚被中断打断」的现场，这样 isr_common 的
 * 弹寄存器 + iretq 序列就能直接启动它。 */
static void setup_initial_frame(struct fe_thread *t, fe_thread_entry_t entry, void *arg)
{
    u64 stack_top = ((u64)(uptr)t->stack_base + t->stack_size) & ~0xFull;
    struct fe_regs *r = (struct fe_regs *)(uptr)(stack_top - sizeof(struct fe_regs));
    memset(r, 0, sizeof(*r));

    r->rip = (u64)(uptr)thread_trampoline;
    r->cs = FE_SEL_KCODE;
    r->rflags = 0x202;              /* IF=1，线程一启动就开中断 */
    /* SysV ABI：函数入口处 rsp % 16 == 8（模拟 call 压入返回地址后的状态）。
     * 栈顶 16 字节对齐，故减 8。这 8 字节落在已被消费的栈帧内，可以安全占用。 */
    r->rsp = stack_top - 8;
    r->ss = FE_SEL_KDATA;
    r->vector = 0;
    r->error = 0;
    r->rdi = (u64)(uptr)arg;
    r->r12 = (u64)(uptr)entry;      /* 蹦床从这里取入口 */
    r->r13 = (u64)(uptr)arg;        /* 以及参数 */

    t->rsp = (u64)(uptr)r;
}

/* ring 3 线程的初始现场：iretq 时因为 CS 的 RPL=3 而自动切到用户栈并降权。
 * 用户线程的入口就是用户代码本身，不存在「蹦床」——用户代码必须自己调用 exit。 */
static void setup_initial_frame_ring3(struct fe_thread *t, u64 entry, u64 user_stack,
                                      void *arg)
{
    u64 stack_top = ((u64)(uptr)t->stack_base + t->stack_size) & ~0xFull;
    struct fe_regs *r = (struct fe_regs *)(uptr)(stack_top - sizeof(struct fe_regs));
    memset(r, 0, sizeof(*r));

    r->rip = entry;
    r->cs = 0x23;               /* 用户代码段 0x20 | RPL 3 */
    r->rflags = 0x202;
    r->rsp = user_stack;
    r->ss = 0x1B;               /* 用户数据段 0x18 | RPL 3 */
    r->rdi = (u64)(uptr)arg;
    t->rsp = (u64)(uptr)r;
    t->kernel_stack_top = stack_top;
}

/* ------------------------------------------------------------------ */
/* 用户态 TLS（线程局部存储）                                          */
/* ------------------------------------------------------------------ */

/* 为线程准备一块 TLS 并返回它的**物理地址**（要给 %fs 基址用的就是它）。
 *
 * 布局约定见 fe/user.h 的说明。这里做三件事：
 *   1. 分配一块内核堆内存（用户态看不到它的虚拟地址，也不需要）；
 *   2. 把可执行文件的 PT_TLS 初始化映像拷进去，其余保持零（.tbss 语义）；
 *   3. 在 `%fs:0` 写入"数据块起始地址"——编译器就是这么取的。
 *
 * ★ 为什么 TLS 块可以在内核堆上、不必进用户地址空间 ★
 * 因为 `%fs` 基址装的是**物理地址**（wrmsr 写的是线性地址，而我们没有
 * 分页偏移问题：内核堆在 HHDM 之外的内核虚拟区……见下），CPU 用它加上
 * 偏移直接访存，完全不经过用户页表。所以：
 *   - 用户代码不能用普通 C 指针去取 TLS 变量的地址并打印它（那会是一个
 *     内核地址）——但真实程序本来也不需要这么做；
 *   - 反过来说，这也意味着 TLS 里的数据**天然与用户地址空间隔离**，
 *     用户态无法通过越界写碰到别的线程的 TLS。
 *
 * 注意：`%fs` 基址必须是**当前地址空间下仍然有效的线性地址**。
 * 我们的内核映射在所有地址空间里完全一致（见 00-architecture §2），
 * 所以直接写内存对象的线性地址即可。 */
static u64 thread_setup_user_tls(struct fe_thread *t)
{
    /* ★ 每个线程都要有 TLS 块，包括内核线程 ★
     *
     * 第一版只在 ring3 那条路径上装了 TLS，结果内核线程跑起来时 `%fs`
     * 仍是 0，第一次访存就 `#PF`（出错地址 0x0、内核态读——实测抓到的）。
     * 这与 FPU 状态区是同一条道理：**"没有"不能靠"不设"来表达**，
     * 因为切换路径会把上一个线程的值留在这里。
     * 没有 PT_TLS 描述时给一个只含 TCB 的块：`%fs:0` 指向它自己，
     * 于是任何误用都是"读到自己块里的零"而不是"读 0 地址崩掉"。 */
    struct fe_task *task = t->task;
    u64 img_size = (task && task->tls_init) ? task->tls_size : 0;
    u64 align = (task && task->tls_align) ? task->tls_align : 16;
    u64 size = img_size + FE_TLS_MIN_TCB;

    void *blk = fe_kzalloc(size);
    if (!blk) {
        /* 分配失败：`%fs` 留 0。用户态一碰 TLS 就会 #PF —— 如实失败，
         * 而不是给它一个指向别处的假基址（那会造成跨线程数据泄漏）。 */
        return 0;
    }
    if (img_size && task->tls_init) {
        memcpy(blk, task->tls_init, img_size);
    }
    /* 数据段起始 = 块首（对齐由分配器保证 ≥16，且 tls_align 已被夹到 ≤64）。
     * 用 memcpy 而不是类型转换赋值：块首的对齐只由分配器保证，
     * 编译器无法证明它 8 字节对齐，而 movaps 风格的优化会因此踩空。
     * 这一处是**跨 ABI 的裸地址写入**，用 memcpy 表达"写 8 个字节"最准确。 */
    u64 self = (u64)(uptr)blk;
    memcpy((u8 *)blk + FE_TLS_SELF_OFFSET, &self, sizeof(self));

    t->user_tls = blk;
    t->user_tls_size = size;
    t->user_fs_base = (u64)(uptr)blk;
    (void)align;
    return t->user_fs_base;
}

struct fe_thread *fe_thread_create_ring3(struct fe_task *task, u64 entry, u64 user_stack,
                                         void *arg, u64 kstack_size, u32 priority,
                                         const char *name)
{
    if (!task || !task->space || entry == 0) {
        return NULL;
    }
    struct fe_thread *t = (struct fe_thread *)fe_kzalloc(sizeof(*t));
    if (!t) {
        return NULL;
    }
    if (kstack_size == 0) {
        kstack_size = FE_THREAD_DEFAULT_STACK;
    }
    t->stack_base = fe_kzalloc(fe_align_up(kstack_size, 16));
    if (!t->stack_base) {
        fe_kfree(t);
        return NULL;
    }
    t->stack_size = fe_align_up(kstack_size, 16);
    t->fpu_area = fe_fpu_area_alloc();
    t->id = g_next_id++;
    t->priority = priority < FE_THREAD_PRIO_LEVELS ? priority : FE_THREAD_PRIO_LEVELS - 1;
    t->state = FE_THREAD_READY;
    t->slice = (i32)g_default_slice;
    t->task = task;
    /* ★ 默认**不许**回收 ★ 见 reap_ok 在 thread.h 里的说明。
     *
     * 保守的默认值是刻意的：`fe_thread_join` 拿着裸指针在等某个特定线程，
     * 而"谁会被 join"在创建的那一刻并不总是知道。默认不回收 → 最坏情况是
     * 漏一个僵尸（可见、可查）；默认回收 → 最坏情况是 use-after-free
     * （不可见、随机崩）。两者不是一个量级的错误。
     * 明确知道"没人会 join 它"的调用者再把它打开（见 fe_process_create）。 */
    t->reap_ok = false;
    /* 线程**持有**任务的引用。这一条不是可选的：
     * 否则父进程一旦关掉子任务的句柄，正在运行中的子任务对象就被销毁了。 */
    fe_object_ref(&task->hdr);
    strlcpy(t->name, name ? name : "user", FE_THREAD_NAME_MAX);
    setup_initial_frame_ring3(t, entry, user_stack, arg);
    /* 用户线程必须有 TLS：libc 的 errno、C 的 __thread、C++ 的 thread_local
     * 全都靠 %fs 基址。失败不阻止线程启动，但那个线程一用 TLS 就会 #PF——
     * 这是**如实失败**，而不是给它一个假的基址。 */
    thread_setup_user_tls(t);

    t->all_next = g_all;
    g_all = t;
    g_created_total++;

    u64 irq = fe_irq_save();
    rq_push(t);
    fe_irq_restore(irq);
    return t;
}

struct fe_thread *fe_thread_create(const char *name, fe_thread_entry_t entry,
                                   void *arg, u64 stack_size, u32 priority)
{
    if (!entry) {
        return NULL;
    }
    if (stack_size == 0) {
        stack_size = FE_THREAD_DEFAULT_STACK;
    }
    stack_size = fe_align_up(stack_size, 16);
    if (priority >= FE_THREAD_PRIO_LEVELS) {
        priority = FE_THREAD_PRIO_LEVELS - 1;
    }

    struct fe_thread *t = (struct fe_thread *)fe_kzalloc(sizeof(*t));
    if (!t) {
        return NULL;
    }
    t->stack_base = fe_kzalloc(stack_size);
    if (!t->stack_base) {
        fe_kfree(t);
        return NULL;
    }
    t->stack_size = stack_size;
    t->fpu_area = fe_fpu_area_alloc();
    t->kernel_stack_top = ((u64)(uptr)t->stack_base + stack_size) & ~0xFull;
    t->id = g_next_id++;
    t->priority = priority;
    t->state = FE_THREAD_READY;
    t->slice = (i32)g_default_slice;
    t->exit_code = 0;
    strlcpy(t->name, name ? name : "thread", FE_THREAD_NAME_MAX);
    setup_initial_frame(t, entry, arg);

    t->task = g_current ? g_current->task : NULL;
    if (t->task) {
        fe_object_ref(&t->task->hdr);
    }
    /* ★ 内核线程这条路径也必须装 TLS ★
     * 见 thread_setup_user_tls 的说明：漏掉它的症状是内核线程第一次
     * 访存 `%fs` 就 #PF（实测抓到过），因为切换路径不区分线程种类。 */
    thread_setup_user_tls(t);
    t->all_next = g_all;
    g_all = t;
    g_created_total++;

    u64 irq = fe_irq_save();
    rq_push(t);
    fe_irq_restore(irq);
    return t;
}

/* ------------------------------------------------------------------ */
/* 切换核心                                                            */
/* ------------------------------------------------------------------ */

/* ★ 判据只有一份（K2 的闸门从任务级扩到线程级，
 * 见 docs/13-tasks-and-kill.md §6.1）★
 *
 * ★ 为什么是一个函数，而不是"闸门与三个取消点各写一遍或运算" ★
 * 这两份判据漂移的症状是**闸门放行了、取消点没放行**（或反过来）：
 * 它只在一部分调用序列上出现，表现成"有时杀得掉、有时杀不掉"——
 * 比崩掉难查得多。（`FE_TASK_STRIDE` 在两个头文件里各写一遍、两处还不
 * 一致的教训就在 docs/13-tasks-and-kill.md §2。）
 *
 * 两种来源：
 *   - `kill_pending`：线程级，**只有别的执行体**能置
 *     （fe_task_kill_other_threads）；线程自己从不置、从不清。
 *     它只说"这个线程不许再回到用户态"，不说"任务怎么了"——
 *     exec 之后任务还要继续活着跑新映像；
 *   - `task->dying`：任务级终止，全部线程一起走（K2 原有语义，一个字没改）。 */
bool fe_thread_should_die(const struct fe_thread *t)
{
    return t && (t->kill_pending || (t->task && t->task->dying));
}

u64 fe_sched_maybe_switch(u64 rsp)
{
    /* ★ K2 的闸门：判据是 fe_thread_should_die()——**线程级**的一票 ★
     *
     * 这里是唯一的必经之路——`isr.asm` 与 `syscall.asm` 都在弹出寄存器之前
     * 调它（一个走 iretq，一个走 sysret）。把判据放在这里，而不是在每个
     * syscall 里各写一遍：漏一处就是一个"已经被杀了却还能继续跑"的洞，
     * 而那种洞只在特定调用序列下才显形。
     *
     * ★ 判据只有一份 ★ 它与三个取消点共用 `fe_thread_should_die()`：
     * 任务级终止（`task->dying`，K2）与线程级标记（`kill_pending`，2a）
     * 是同一个问题的两种来源。闸门对**当前线程**同样成立——所以
     * `exec` 的调用者自己必须在名单之外（见 fe_task_kill_other_threads）。
     *
     * ★ 必须在 g_need_resched 的早退**之前**判 ★
     * 这个函数在没有重调度请求时会直接返回 rsp（快路径，绝大多数系统调用
     * 都走它）。把闸门放在早退之后，等于"只有恰好要切换时才检查"——
     * 一个被杀的任务只要连着做几次不需要重调度的系统调用，就能继续跑下去。
     *
     * 就地标记为僵尸然后当作"需要重调度"往下走：因为状态已是 DEAD，
     * 下面的 `prev->state == FE_THREAD_RUNNING` 不成立，它不会被放回队列，
     * 于是它的 rsp 被丢弃、永远不再被选中——与 exit_isr 的结局完全一样。 */
    if (g_current && fe_thread_should_die(g_current)) {
        thread_mark_dead(g_current, FE_ERR_KILLED);
        g_need_resched = true;
    }

    if (!g_sched_enabled || !g_need_resched) {
        return rsp;
    }
    g_need_resched = false;

    struct fe_thread *prev = g_current;
    struct fe_thread *next = rq_pick();

    if (!next || next == prev) {
        /* ★ 这一条早退对"刚被标记死亡"的线程是**禁止**的 ★
         * 返回 rsp 意味着"继续跑当前的"，而当前的已经死了——
         * 那就等于闸门没关。就绪队列理论上不会空（空闲线程常驻），
         * 所以这里是"理论上不会发生"的分支：**发生了就说明闸门漏了**，
         * 必须大声崩掉，而不是悄悄放一个已死的线程回用户态。 */
        if (prev && prev->state == FE_THREAD_DEAD) {
            fe_panic("被终止的线程 %s 没有可切换的目标——闸门失效",
                     prev->name);
        }
        return rsp;     /* 没有别的可运行线程：继续跑当前的 */
    }
    /* ★ FPU/SIMD 状态：必须在换栈之前保存/恢复 ★
     *
     * 放在这里而不是别处的三个理由：
     *   1. **只在真的切换时执行**——`next == prev` 或不需要重调度时
     *      这条路径根本不进来，所以不用 SIMD 的系统调用不付任何代价；
     *   2. 此刻中断是关的（本函数只从 ISR / syscall 尾部调用），
     *      不会被抢占打断成"保存到一半"；
     *   3. 内核自己**从不使用**向量寄存器（-mgeneral-regs-only），
     *      所以中断与系统调用不需要保存它们——这一条让延迟保持低，
     *      也是"内核不用 SIMD"这个决定最大的回报。
     *
     * 急切保存（而不是 CR0.TS 惰性）：XSAVEOPT 会跳过未修改的部件，
     * 从没用过 FPU 的线程只写一个头部，代价可预测、没有异常抖动。 */
    if (fe_fpu_enabled()) {
        if (prev) {
            fe_fpu_save(prev->fpu_area);        /* NULL 区 = 不用保存 */
        }
        fe_fpu_restore(next->fpu_area);         /* NULL 区 = 恢复成干净状态 */
    }
    if (prev) {
        prev->rsp = rsp;
        if (prev->state == FE_THREAD_RUNNING) {
            prev->state = FE_THREAD_READY;
            rq_push(prev);      /* 放回队尾 → 同级轮转 */
        }
    }
    /* 与栈和地址空间有关的硬件状态必须在真正换栈之前更新：
     *   - TSS.rsp0：ring 3 发生中断时 CPU 去哪找内核栈；
     *   - gs:8    ：syscall 指令进入时入口桩去哪找内核栈；
     *   - CR3     ：不同任务要换地址空间（内核半区共享，换 CR3 不影响内核代码与栈）。 */
    fe_tss_set_rsp0(next->kernel_stack_top);
    fe_syscall_set_kernel_stack(next->kernel_stack_top);
    /* ★ 用户态 TLS 基址跟着线程走 ★
     *
     * 这一条**必须**在切换路径上，而且必须是每次切换都设：
     * 两个用户线程共享同一个 `%fs` 基址的话，一个线程写的 `__thread` 变量
     * 会被另一个线程看到——那正是"线程局部变量莫名其妙变了"这类
     * 只在多线程下出现的、极难复现的错误。
     *
     * 内核线程的基址设成 0：它们**不访问** %fs（内核是 -mgeneral-regs-only
     * 编译的，也不碰 __thread）。设 0 而不是留着上一个用户线程的值，
     * 是因为"留着"意味着内核线程一旦误用 %fs 就会读到别的进程的 TLS——
     * 那是信息泄漏；而 0 会让它当场 #PF，暴露问题而不是掩盖问题。 */
    fe_wrmsr(FE_MSR_FS_BASE, next->user_fs_base);
    /* 端口权限跟着任务走：只改 TSS 里的 iopb_offset，不搬 8 KiB 位图。
     * 内核线程（task 为 NULL）一律按「禁止一切端口」处理。 */
    fe_tss_set_iopb_slot(next->task ? next->task->iopb_slot : -1);
    /* ★ 地址空间：内核线程也要换（换回内核空间）★
     *
     * 原来的条件是 `next->task->space && 与 prev 不同` ——于是切到内核线程
     * （idle 就是）时**什么都不做**，CR3 里留着上一个用户任务的页表。
     * 那不只是"多留了一会儿"：内核线程随后可能**释放那个地址空间**
     * （空闲线程要回收僵尸线程 → 僵尸持有任务引用 → 任务是最后一个引用时
     * 销毁 → 归还页表与 PML4 帧）。释放之后 CR3 仍然指着已经被 PMM
     * 收回的那一帧，下一次页表遍历读到的就是别人写进去的数据。
     *
     * 实测症状（K2/K3 那一轮）：先是一个用户线程在**自己的代码地址**上
     * #PF（"页不存在"，而它明明一直在跑），紧接着
     * `fe_pmm_free_frames 参数非法: base=0x0` —— 那是一次对已释放结构体的
     * 二次销毁，读到的 pml4_phys 是 0。
     *
     * 所以判据改成"**两个任务各自的地址空间不同就切**"，并把
     * NULL（内核线程）当成内核空间。这也是"内核线程运行在内核地址空间里"
     * 这条语义该有的样子：它不该继承别人的用户映射。 */
    struct fe_address_space *next_as = next->task ? next->task->space : NULL;
    struct fe_address_space *prev_as = (prev && prev->task) ? prev->task->space : NULL;
    if (next_as != prev_as) {
        fe_vmm_switch(next_as ? next_as : fe_vmm_kernel_space());
    }


    next->state = FE_THREAD_RUNNING;
    next->slice = (i32)g_default_slice;
    next->switches++;
    g_current = next;
    return next->rsp;
}

void fe_sched_request(void)
{
    g_need_resched = true;
}

void fe_sched_yield_current(void)
{
    g_need_resched = true;
}

/* ------------------------------------------------------------------ */
/* 睡眠队列                                                            */
/* ------------------------------------------------------------------ */

/* ★ 睡眠的到期时刻用**纳秒**，不用节拍数（K7 收尾时改的）★
 *
 * 原来是"睡 N 个节拍"，即 `fe_time_ticks() + N`。那个写法把睡眠的正确性
 * 押在"节拍会均匀到达"这个假设上，而这个假设**在虚拟机上不成立**：
 *
 * VBox 实测（同一个启动、同一个 8254 源）：
 *   - 被动地在一个 50 ms 窗口里数节拍 → 5 个（≈99 Hz）
 *   - 而 `sleep(50 ms)`（= 50 个节拍）却在 **14 ms** 后就返回了
 * 也就是说中断是**突发投递**的，节拍率在 99 Hz 与 ~3.5 kHz 之间跳。
 * "睡 N 个节拍"于是可能提前 3~4 倍返回——对超时语义来说那是**错误**，
 * 不是精度差（超时提前触发比延后触发危险得多）。
 *
 * 改成绝对时刻之后语义变得干净：**醒来的条件是"时间到了"**，
 * 节拍只负责"过一会儿来看一眼"。代价是唤醒精度受限于节拍
 * （VBox 上最坏可能晚到几十毫秒），但方向是**只会晚、不会早**——
 * 这正是超时该有的方向。 */
void fe_sched_sleep_until(u64 wake_ns)
{
    struct fe_thread *t = g_current;
    if (!t) {
        return;
    }
    u64 irq = fe_irq_save();
    t->wake_ns = wake_ns;
    t->state = FE_THREAD_SLEEPING;

    /* 按唤醒时刻升序插入，唤醒时只需从表头扫。
     * 绝大多数情况下新线程的唤醒时刻晚于表头，因此先走「头部插入」这条快速路径，
     * 避免每次睡眠都遍历链表（遍历仍保留环检测：链表被破坏时要明确报错而不是静默挂死）。 */
    if (!g_sleep_list || g_sleep_list->wake_ns > wake_ns) {
        t->sleep_next = g_sleep_list;
        g_sleep_list = t;
    } else {
        struct fe_thread **pp = &g_sleep_list;
        u32 guard = 0;
        while (*pp && (*pp)->wake_ns <= wake_ns) {
            if (++guard > 8192) {
                fe_panic("睡眠链表成环（调度器内部一致性被破坏）");
            }
            pp = &(*pp)->sleep_next;
        }
        t->sleep_next = *pp;
        *pp = t;
    }
    fe_irq_restore(irq);

    /* 走统一的切换路径：因为状态不是 RUNNING，不会把自己放回就绪队列 */
    fe_sched_request();
    __asm__ volatile("int %0" ::"i"(FE_VEC_YIELD));
}

static void wake_sleepers(u64 now_ns)
{
    while (g_sleep_list && g_sleep_list->wake_ns <= now_ns) {
        struct fe_thread *t = g_sleep_list;
        g_sleep_list = t->sleep_next;
        t->sleep_next = NULL;
        if (t->state == FE_THREAD_SLEEPING) {
            t->state = FE_THREAD_READY;
            rq_push(t);
        }
    }
}

void fe_sched_block_current(void)
{
    struct fe_thread *t = g_current;
    if (!t) {
        return;
    }
    t->state = FE_THREAD_BLOCKED;
    fe_sched_request();
    __asm__ volatile("int %0" ::"i"(FE_VEC_YIELD));
}

void fe_sched_wake(struct fe_thread *t)
{
    if (!t) {
        return;
    }
    /* ★ 点名，但**不许静默 return**（队列卫生，第 5 步）★
     *
     * 静默正是让这类 bug 活下来的原因：一个"叫醒了一个不该被叫醒的线程"
     * 的调用会安安静静地什么都不做，于是症状延后到别处才显形
     * （就绪链上多一个不该在的节点、`rq_pick` 挑到失效指针、或者
     * 一次 yield 之后再也回不来）。
     *
     * ★ 判据不许直接解引用可疑指针 ★
     * 传进来的 `t` 可能指向**已经 unmap 的页**——读 `t->state` 就是
     * 一次 #PF（第 3 步实测过：`CR2 = 任务地址 + 0x1060`）。
     * 所以先用**注册表**（`g_all` 线程链）判断它在不在册：
     *   - 不在册 → 只打指针值与调用者（`__builtin_return_address(0)`，
     *     与 fe_pmm_free_frames 用的是同一招），**一个字节都不碰它**；
     *   - 在册   → 指针可信，可以安全打印 name/id/state。 */
    bool known = false;
    for (struct fe_thread *q = g_all; q; q = q->all_next) {
        if (q == t) {
            known = true;
            break;
        }
    }
    if (!known) {
        fe_kprintf("[调度] **fe_sched_wake 收到一个不在册的线程指针** %p"
                   "（调用者=%#llx）——它可能已经被回收；不解引用，直接丢弃\n",
                   (void *)t,
                   (unsigned long long)(uptr)__builtin_return_address(0));
        return;
    }
    u64 irq = fe_irq_save();
    if (t->state == FE_THREAD_BLOCKED) {
        t->state = FE_THREAD_READY;
        rq_push(t);
    } else if (t->state == FE_THREAD_SLEEPING) {
        /* ★ 只点名、**不动它**（第 5 步第二步的收窄）★
         * 叫醒 SLEEPING 是 **D3 的修复范围**，不属于"队列卫生"这一步：
         * 把它做进来会偷偷修掉 D3，让"D 组仍如实变红"这条判据失去意义
         * ——单一变量优先于"顺手修好"。 */
        fe_kprintf("[调度] **fe_sched_wake 收到 SLEEPING 的线程**：%s(id=%llu)"
                   "（本步不动它——叫醒睡眠是 D3 的修复范围；调用者=%#llx）\n",
                   t->name, (unsigned long long)t->id,
                   (unsigned long long)(uptr)__builtin_return_address(0));
    } else {
        fe_kprintf("[调度] **fe_sched_wake 收到状态不对的线程**：%s(id=%llu "
                   "state=%u)（只接受 BLOCKED=%u；调用者=%#llx）"
                   "——指针在册，所以这些字段是可信的\n",
                   t->name, (unsigned long long)t->id, t->state,
                   FE_THREAD_BLOCKED,
                   (unsigned long long)(uptr)__builtin_return_address(0));
    }
    fe_irq_restore(irq);
}

/* ------------------------------------------------------------------ */
/* 节拍处理                                                            */
/* ------------------------------------------------------------------ */

void fe_sched_tick(void)
{

    wake_sleepers(fe_time_ns());

    struct fe_thread *t = g_current;
    if (t && t->state == FE_THREAD_RUNNING) {
        t->cpu_ticks++;
        if (t->slice > 0) {
            t->slice--;
        }
        if (t->slice <= 0) {
            g_need_resched = true;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 线程退出 / 让出 / 睡眠                                              */
/* ------------------------------------------------------------------ */

static void yield_isr(struct fe_regs *r)
{
    (void)r;
    g_need_resched = true;
}

/* ★ 线程死亡的**唯一**登记处 ★
 *
 * 两条路都会走到这里，而且只在这里做"变僵尸"这件事：
 *   1. 线程自己跑 `exit_isr`（正常退出、被异常打死）；
 *   2. 调度器在**回到用户态之前**发现它的任务正在被终止（K2 的闸门）。
 *
 * 抽出来的理由是硬性的：僵尸链、`fe_process_on_thread_exit`（唤醒父进程的
 * wait）这两件事必须**每次线程死亡都发生，且只发生一次**。
 * 两处各写一遍的话，漏掉的那一处表现成"父进程的 wait 永远不返回"——
 * 而那只在被终止的那条路径上出现。 */
static void thread_mark_dead(struct fe_thread *t, i32 code)
{
    if (!t || t->state == FE_THREAD_DEAD) {
        return;
    }
    /* ★ 不变式：**DEAD 的线程一定不在任何就绪队列里** ★
     *
     * ★ 为什么死亡登记必须同时摘队列（实测抓到的 use-after-free）★
     * 线程有三种方式被标记死亡，其中**闸门**那一种是作用在"正要回用户态、
     * 状态是 READY、而且已经在就绪队列里"的线程上的：它可能在时间片用完
     * 时被抢占、进了队列，下一次被 `rq_pick()` 选中并切到它，才在返回路径上
     * 被闸门判死（`fe_sched_maybe_switch` 的判据在**切换之后**才生效）。
     * 只置 `state = DEAD` 而不摘队列，就留下一个**僵尸挂在队列上**：
     *   - 僵尸链上的它 `reap_ok=true`（用户线程 / 进程主线程）⇒ 空闲线程会
     *     把它 free（`thread_free`：栈、TLS、FPU 区、对象本身），并且放掉它
     *     对任务的那次引用（任务可能就此销毁）；
     *   - 而队列里那根指针还在 → 下一次 `rq_pick` 选中一块**已释放内存**，
     *     `fe_sched_maybe_switch` 读 `next->task->iopb_slot`（偏移 0x1060）
     *     就是页错误。
     *
     * ★ 为什么必须在**这里**摘、而不是在 `rq_pick` 里判状态 ★
     * `rq_pick` 只看优先级位图、**不看线程状态**——它信任"队列里都是可运行
     * 的线程"这条不变式。把状态判断塞进 `rq_pick` 等于把不变式的维护点
     * 扩散到消费者一侧（每个 `rq_pick` 调用者都得再判一次），而这里是
     * **所有死亡路径的唯一登记处**（`exit_isr` 与闸门），摘一次就够。
     *
     * ★ 为什么摘队列是安全的 ★ `rq_remove` 只在真的挂在队列上时才动指针
     * （它先判 `rq_prev`/`rq_next`/队头队尾），对不在队列里的线程是空操作。
     *
     * ★ 实测症状（QEMU，红而不崩那一刀之前）★
     *   `异常: 页错误 (#PF) ... CR2 = 0xffffffffa00a4060`
     *   `RIP = fe_sched_maybe_switch+0x231`（正是 `movl 0x1060(%rax), %edi`，
     *   而 `%rax` = `next->task`，指向一个已经被销毁的任务对象）。
     * 触发它的场景是**普通的**：任何"线程活得比任务久"的地方（内核线程
     * `reap_ok=false` ⇒ 僵尸永不被回收 ⇒ 它们对任务的引用永不释放，
     * 任务照样可能被销毁）。`exec` 杀线程会天天踩它。 */
    rq_remove(t);
    t->exit_code = code;
    t->state = FE_THREAD_DEAD;
    t->zombie_next = g_zombies;
    g_zombies = t;
    /* 主线程退出 = 进程结束，要唤醒正在 wait 的父进程。 */
    fe_process_on_thread_exit(t);
}

/* 线程退出中断：把当前线程标记为僵尸并请求重新调度。
 * 注意本函数**会正常返回**到 isr_common —— 真正的「不再回来」发生在中断返回路径上：
 * 因为该线程状态已是 DEAD，调度器不会把它放回就绪队列，于是它永远不会被再次选中。 */
static void exit_isr(struct fe_regs *r)
{
    (void)r;
    struct fe_thread *t = g_current;

    if (t) {
        thread_mark_dead(t, t->exit_code);
    }
    /* 必须确认还有别的线程可运行，否则切换会失败、跑回已死的线程。
     * 这里只查位图，绝不能调用 rq_pick——那会把线程从队列里摘走。 */
    if (g_ready_mask == 0) {
        fe_panic("所有线程都已退出（空闲线程也应存在）: %s", t ? t->name : "?");
    }
    g_need_resched = true;
}

FE_NORETURN void fe_thread_exit(i32 code)
{
    struct fe_thread *t = g_current;
    if (t) {
        t->exit_code = code;
    }
    __asm__ volatile("int %0" ::"i"(FE_VEC_THREAD_EXIT));
    /* exit_isr 一定会把当前线程切走；回到这里说明调度失败 */
    fe_panic("线程退出后仍被调度回来: %s", t ? t->name : "?");
}

void fe_thread_yield(void)
{
    __asm__ volatile("int %0" ::"i"(FE_VEC_YIELD));
}

void fe_thread_sleep_ticks(u64 ticks)
{
    if (ticks == 0) {
        fe_thread_yield();
        return;
    }
    /* ★ 节拍数先换算成**时间**再睡 ★
     * 这样"睡 N 个节拍"的语义变成"睡够 N 个节拍本该代表的时间"，
     * 而不是"等到第 N 个中断到来"。在节拍被虚拟化环境突发投递时，
     * 后者会提前几倍返回（见 fe_sched_sleep_until 的说明）。 */
    u64 ns = ticks * (1000000000ull / (fe_time_hz() ? fe_time_hz() : 1));
    fe_sched_sleep_until(fe_time_ns() + ns);
}

void fe_thread_sleep_ms(u64 ms)
{
    if (ms == 0) {
        fe_thread_yield();
        return;
    }
    /* 毫秒直接换算成纳秒：**不经过节拍**。
     * 经过节拍就等于把"睡多久"交给节拍率去解释，而这个项目刚刚用
     * VBox 上的实测证明了节拍率不可靠（99 Hz 与 3.5 kHz 之间跳）。 */
    fe_sched_sleep_until(fe_time_ns() + ms * 1000000ull);
}

/* ------------------------------------------------------------------ */
/* 回收与查询                                                          */
/* ------------------------------------------------------------------ */

/* 线程对象的统一销毁路径。
 * 先放掉它对任务的引用，再释放栈与自身——任务的引用计数由线程持有，
 * 这样「父进程提前关掉子任务句柄」不会把还在跑的子任务拆掉。 */
static void thread_free(struct fe_thread *t)
{
    if (!t) {
        return;
    }
    /* ★ 不变式：**任何被释放的线程都不在任何队列上** ★
     * 与 thread_mark_dead 里那句 `rq_remove` 是同一条不变式的两个落点：
     * 那句保证"死了就不在队列里"，这句保证"被释放了就不在队列里"。
     * 两道都要——中间还隔着"回收"这一步（僵尸链 → `thread_free`），
     * 而僵尸是**可能**被重新排进队列的（`fe_sched_wake` 收到失效指针、
     * 或某条路径重复 push）。队列上留着一根指向已释放内存的指针，
     * 下一次 `rq_pick` 就会把它当线程用。 */
    rq_remove(t);
    if (t->stack_base) {
        fe_kfree(t->stack_base);
        t->stack_base = NULL;
    }
    /* TLS 块与地址空间的关系见 thread_setup_user_tls：用户态从不直接
     * 引用它的虚拟地址，所以这里释放即可，不需要拆映射。 */
    if (t->user_tls) {
        fe_kfree(t->user_tls);
        t->user_tls = NULL;
        t->user_fs_base = 0;
        t->user_tls_size = 0;
    }
    fe_fpu_area_free(t->fpu_area);
    t->fpu_area = NULL;
    struct fe_task *task = t->task;
    fe_kfree(t);
    if (task) {
        fe_object_unref(&task->hdr);
    }
}

u32 fe_sched_reap(void)
{
    u32 n = 0;
    struct fe_thread **pp = &g_zombies;
    while (*pp) {
        struct fe_thread *z = *pp;
        /* ★ 有人在 join 它的不许碰 ★ 见 reap_ok 的说明：
         * join 者拿着这根指针在等，释放掉就是悬空指针 + 双重释放。 */
        if (!z->reap_ok) {
            pp = &z->zombie_next;
            continue;
        }
        *pp = z->zombie_next;
        z->zombie_next = NULL;
        /* 从全局链摘除。★ 这一步 join 那条路也要做，而且必须做彻底 ★
         * 曾经 join 只摘了全局链、没摘僵尸链，于是僵尸链上留着
         * 一根已经 free 的指针——只要将来有人真的调用回收器
         * （也就是现在），那就是一次 use-after-free。 */
        struct fe_thread **ap = &g_all;
        while (*ap && *ap != z) {
            ap = &(*ap)->all_next;
        }
        if (*ap) {
            *ap = z->all_next;
        }
        thread_free(z);
        n++;
    }
    return n;
}

i32 fe_thread_join(struct fe_thread *t)
{
    if (!t) {
        return FE_ERR_INVAL;
    }
    /* 等待目标线程退出。
     *
     * ★ 这里绝不能顺手回收其它僵尸线程。★
     * 曾经在循环里调用 fe_sched_reap_except(t)「避免僵尸堆积」，后果是：
     * 逐个 join 多个线程时，第一个 join 的回收会把**后面还要 join 的线程**提前释放，
     * 后续 join 拿到悬空指针 → fe_kfree 双重释放 → 内核堆被破坏 →
     * 表现为「某个内核线程一直在跑却什么都不做」这种极难定位的症状。
     * 未被 join 的线程会留在僵尸链上，等将来有专门的回收服务再处理。
     *
     * 等待方式用「主动让出」而不是睡眠：让出不会把自己的调度状态卷进睡眠链表。 */
    u32 spins = 0;
    while (t->state != FE_THREAD_DEAD) {
        fe_thread_yield();
        if (++spins >= 2000000u) {
            fe_kprintf("[join] 等待线程 %s 超时（状态 %u），放弃等待\n",
                       t->name, t->state);
            break;
        }
    }
    i32 code = t->exit_code;
    struct fe_thread **pp = &g_all;
    while (*pp && *pp != t) {
        pp = &(*pp)->all_next;
    }
    if (*pp) {
        *pp = t->all_next;
    }
    /* ★ 僵尸链上也要摘 ★
     * 原来只摘了全局链。僵尸链于是留着这根指针，而它马上就会被 free——
     * 回收器一开始工作（现在它在空闲线程里跑了），那就是 use-after-free。
     * 两处链表都是"这个线程还在"的登记，摘一处等于没摘。 */
    struct fe_thread **zp = &g_zombies;
    while (*zp && *zp != t) {
        zp = &(*zp)->zombie_next;
    }
    if (*zp) {
        *zp = t->zombie_next;
    }
    t->zombie_next = NULL;
    thread_free(t);
    return code;
}

/* 回收僵尸，但保留指定线程（供 join 使用） */
void fe_sched_reap_except(struct fe_thread *keep)
{
    struct fe_thread **pp = &g_zombies;
    while (*pp) {
        struct fe_thread *z = *pp;
        if (z == keep) {
            pp = &z->zombie_next;
            continue;
        }
        *pp = z->zombie_next;
        z->zombie_next = NULL;
        struct fe_thread **ap = &g_all;
        while (*ap && *ap != z) {
            ap = &(*ap)->all_next;
        }
        if (*ap) {
            *ap = z->all_next;
        }
        thread_free(z);
    }
}

struct fe_thread *fe_thread_current(void)
{
    return g_current;
}

/* ★ 任务对象销毁**之前**必须调它：把还指着它的线程的 task 摘成 NULL ★
 *
 * ★ 为什么需要它（这是实测抓到的第二个 use-after-free，不是防御性代码）★
 * 有些线程**活得比任务久**：内核线程（自检的探针、以及任何用
 * "临时换 cur->task" 手法造出来的线程）`reap_ok` 默认是 false ⇒ 它们变成
 * 僵尸之后**永远不会被回收**；而它们对任务的那次引用在 `thread_free`
 * 里才放——也就是**永远不放**。
 * 反过来看：一个"最后一个存活线程已经死了、引用也放掉了"的任务会正常销毁，
 * 可那些僵尸线程的 `task` 字段仍然指着它。之后：
 *   - 那个地址被堆分配器复用；
 *   - 僵尸线程又被调度到（它在就绪队列里，或者被 `fe_sched_wake` 推上去）；
 *   - `fe_sched_maybe_switch` 读 `next->task->iopb_slot`（偏移 0x1060）——
 *     落在复用后的对象之外 ⇒ **页错误**。
 *
 * ★ 与 `thread_mark_dead` 里那句 `rq_remove` 的分工 ★
 * 那一句保证"DEAD 的线程不会留在就绪队列里"（**不变式**，正本清源）；
 * 这一句是**第二道闸**：即使某个僵尸因为别的路径又被排进队列
 * （例如 `fe_sched_wake` 被误用在一个已死的线程上），它的 `task` 也已经是
 * NULL、只会被当作内核线程处理（`next->task ? ...->iopb_slot : -1`），
 * 不会踩已释放内存。两道都要，因为它们防的是两条不同的路。
 *
 * ★ 实测症状（QEMU）★ `RIP = fe_sched_maybe_switch+0x231`
 * （`movl 0x1060(%rax), %edi`）、`CR2 = 0xffffffffa00a4060`；加了取证打印
 * 之后指认到具体线程：`线程 tls-a/tls-b 的 task=... 正是最近销毁的那个任务`。
 *
 * 做法：把**所有** task 字段等于它的线程的 task 置成"无所属任务"（NULL）。
 * 它不改变任何线程的存活状态，也不释放任何东西。 */
void fe_task_detach_threads(struct fe_task *task)
{
    if (!task) {
        return;
    }
    u32 n = 0;
    for (struct fe_thread *t = g_all; t; t = t->all_next) {
        if (t->task == task) {
            t->task = NULL;
            n++;
        }
    }
    /* 有线程活得比任务久 ⇒ 它永远不会被回收、它的 task 只能靠这里摘。
     * 这是一条**应该被看见**的事实（不是错误，但要能解释"为什么任务表里
     * 少了一个任务、而线程链上还有它的线程"）。 */
    if (n) {
        fe_kprintf("[调度] 任务 %s(%llu) 销毁：%u 个线程仍指着它，"
                   "已把它们的 task 摘成空（否则它们下一次被调度就是"
                   "一次 use-after-free）\n",
                   task->name, (unsigned long long)task->id, n);
    }
}

u64 fe_thread_total_created(void)
{
    return g_created_total;
}

u64 fe_thread_live_count(void)
{
    u64 n = 0;
    for (struct fe_thread *t = g_all; t; t = t->all_next) {
        if (t->state != FE_THREAD_DEAD) {
            n++;
        }
    }
    return n;
}

/* ---- 线程枚举：TASK_LIST 快照要按任务把线程列出来 ----
 *
 * ★ 为什么用"按全局链过滤"而不是"每个任务维护一条线程链" ★
 * 后者要在创建/销毁两条路径上同时维护，而漏一处就是"任务退出后线程
 * 还挂在它的链上"——那是一类只能靠遍历时崩掉才发现的错误。
 * 全局链是**唯一**的链表（创建时插、销毁时摘，只有两处），
 * 过滤一遍的代价是 O(线程数)，而枚举本来就要遍历它们。 */
struct fe_thread *fe_task_thread_first(struct fe_task *task)
{
    for (struct fe_thread *t = g_all; t; t = t->all_next) {
        if (t->task == task && t->state != FE_THREAD_DEAD) {
            return t;
        }
    }
    return NULL;
}

struct fe_thread *fe_thread_next_of(struct fe_thread *cur)
{
    if (!cur) {
        return NULL;
    }
    for (struct fe_thread *t = cur->all_next; t; t = t->all_next) {
        if (t->task == cur->task && t->state != FE_THREAD_DEAD) {
            return t;
        }
    }
    return NULL;
}

u32 fe_task_thread_count(struct fe_task *task)
{
    u32 n = 0;
    for (struct fe_thread *t = g_all; t; t = t->all_next) {
        if (t->task == task && t->state != FE_THREAD_DEAD) {
            n++;
        }
    }
    return n;
}

const char *fe_thread_state_name(u32 state)
{
    switch (state) {
    case FE_THREAD_UNUSED:   return "未使用";
    case FE_THREAD_READY:    return "就绪";
    case FE_THREAD_RUNNING:  return "运行";
    case FE_THREAD_SLEEPING: return "睡眠";
    case FE_THREAD_BLOCKED:  return "阻塞";
    case FE_THREAD_DEAD:     return "已退出";
    default:                 return "?";
    }
}

/* ★ "它在等什么"：给"叫停超时"那条日志直接指认凶手（ASCII 短标签）★
 *
 * ★ 为什么返回 ASCII 而不是 fe_thread_state_name() 那种中文 ★
 * 屏幕上打印的字符串只能用 ASCII（本项目铁律）——`state` 由调用者按
 * **数值**打印（`state=%u`），这里只给等待点的短标签。
 *
 * ★ 今天能分辨的与不能分辨的（写清楚，免得日志撒谎）★
 *   - `wait_any`   ：线程身上挂着等待节点（`waiting` + `wait_node`）——
 *                    这一条只读线程自己的字段，判得准；
 *   - `sleep`      ：状态是 SLEEPING（挂在睡眠链上，到点才醒）；
 *   - `endpoint_recv` / `notification` / `reslock_*` / `task_wait`：
 *                    ★ 今天**分辨不出来** ★ 它们要扫任务句柄表 / 资源池 /
 *                    全局任务链，而那三处的"只读访问器"是
 *                    docs/13-tasks-and-kill.md §6.7 那一小节的交付物。
 *                    在它们到位之前，这些情形统一报 `blocked`——
 *                    如实说"只知道它阻塞着"，而不是猜一个具体位置。 */
const char *fe_thread_wait_site(const struct fe_thread *t)
{
    if (!t) {
        return "unknown";
    }
    if (t->waiting && t->wait_node) {
        return "wait_any";
    }
    switch (t->state) {
    case FE_THREAD_SLEEPING: return "sleep";
    case FE_THREAD_BLOCKED:  return "blocked";
    case FE_THREAD_READY:    return "runqueue";
    case FE_THREAD_RUNNING:  return "running";
    case FE_THREAD_DEAD:     return "dead";
    default:                 return "unknown";
    }
}

void fe_thread_dump_all(void)
{
    fe_kprintf("  ID  名称             状态    优先级  时间片  切换  占用节拍\n");
    for (struct fe_thread *t = g_all; t; t = t->all_next) {
        fe_kprintf("  %-4llu %-16s %-6s %5u %6d %6llu %8llu\n",
                   (unsigned long long)t->id, t->name,
                   fe_thread_state_name(t->state), t->priority, t->slice,
                   (unsigned long long)t->switches,
                   (unsigned long long)t->cpu_ticks);
    }
}

void fe_sched_set_default_slice(u32 ticks)
{
    if (ticks > 0) {
        g_default_slice = ticks;
    }
}

u32 fe_sched_default_slice(void)
{
    return g_default_slice;
}

bool fe_sched_running(void)
{
    return g_sched_enabled;
}

void fe_sched_dump(void)
{
    fe_kprintf("[调度] 就绪位图 %#x, 当前线程 %s(id=%llu), 默认时间片 %u 节拍\n",
               g_ready_mask, g_current ? g_current->name : "(无)",
               g_current ? (unsigned long long)g_current->id : 0, g_default_slice);
    fe_thread_dump_all();
}

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */

static void idle_entry(void *arg)
{
    (void)arg;
    for (;;) {
        /* ★ 收尸在这里做（K2/K3 那一轮补上）★
         *
         * 空闲线程是唯一"确定没有别的事要做"的地方：回收会释放线程栈、
         * TLS 块、FPU 区，并可能连带销毁任务对象（进而归还端口 / MMIO /
         * IRQ / devfs 名字 / 地址空间）。这些都不该在中断上下文里做，
         * 也不该在某个正在赶路的系统调用里顺手做。
         *
         * ★ 为什么不能"谁退出谁自己收" ★ 一个线程没法释放自己的栈——
         * 它正在用。所以必须由别人来收，而"别人"就是空闲线程。 */
        fe_sched_reap();
        /* 空闲线程只在没有任何其它就绪线程时运行；sti;hlt 保证中断能唤醒 CPU。
         * 这里用 sti 而不是无条件 sti：中断处理完会 iretq 回 hlt 之后。 */
        fe_sti();
        fe_hlt();
    }
}

void fe_sched_init(void)
{
    memset(g_runq, 0, sizeof(g_runq));
    g_ready_mask = 0;
    g_all = NULL;
    g_sleep_list = NULL;
    g_zombies = NULL;
    g_need_resched = false;
    g_sched_enabled = false;

    /* 当前执行流（引导线程）登记为一个线程对象，复用引导栈 */
    struct fe_thread *main_t = (struct fe_thread *)fe_kzalloc(sizeof(*main_t));
    if (!main_t) {
        fe_panic("无法为 main 线程分配对象");
    }
    main_t->id = g_next_id++;
    main_t->state = FE_THREAD_RUNNING;
    main_t->priority = FE_PRIO_NORMAL;
    main_t->slice = (i32)g_default_slice;
    strlcpy(main_t->name, "main", FE_THREAD_NAME_MAX);
    main_t->task = fe_task_kernel();
    /* ★ 手工造的这个线程也要装 TLS ★
     * main 线程不走 fe_thread_create（它复用引导栈），所以是唯一一个
     * "创建路径之外"的线程。漏掉它的症状很具体：main 线程一碰 %fs 就 #PF，
     * 而它恰恰是最早跑起来、且会调到自检的那个线程
     * ——自检里的这条检查就是为了不让这种"半成品"漏过去。 */
    thread_setup_user_tls(main_t);
    main_t->stack_base = NULL;      /* 复用引导栈，不回收 */
    extern char fe_boot_stack_top[];
    main_t->kernel_stack_top = (u64)(uptr)fe_boot_stack_top;
    main_t->all_next = g_all;
    g_all = main_t;
    g_current = main_t;
    g_created_total++;

    /* yield 与 exit 两个软中断 */
    fe_idt_set_handler(FE_VEC_YIELD, yield_isr);
    fe_idt_set_handler(FE_VEC_THREAD_EXIT, exit_isr);

    /* 空闲线程：最低优先级，保证任何时刻都有东西可跑 */
    struct fe_thread *idle = fe_thread_create("idle", idle_entry, NULL,
                                              IDLE_STACK_SIZE, FE_PRIO_IDLE);
    if (!idle) {
        fe_panic("无法创建空闲线程");
    }
}

void fe_sched_start(void)
{
    g_sched_enabled = true;
    fe_sched_request();
    fe_thread_yield();
}
