/* SPDX-License-Identifier: 0BSD */
/* 内核线程对象。
 *
 * 关键设计：所有上下文切换都发生在**中断返回路径**上。
 *   - 每个线程有独立的内核栈；
 *   - 中断发生时，CPU 与 isr_common 把寄存器现场压在当前线程的栈上；
 *   - 若需要切换，调度器只是把当前 rsp 存进线程对象、把下一个线程的 rsp 交给
 *     isr_common，后者在该栈上继续「弹寄存器 + iretq」；
 *   - 新建线程的初始栈被**伪造成一个刚被中断的现场**，因此同一条路径就能启动它。
 * 这样抢占、主动让出（yield）、线程退出、首次启动四种情形共用一套代码，
 * 不存在「从函数中间切走再切回来」这类容易出错的特例。
 */
#ifndef FE_SCHED_THREAD_H
#define FE_SCHED_THREAD_H

#include <fe/types.h>
#include <fe/compiler.h>
#include <fe/regs.h>            /* struct fe_fault_regs（K5 的回复槽要用） */

struct fe_task;

#define FE_THREAD_NAME_MAX     16
#define FE_THREAD_PRIO_LEVELS  32

/* 优先级：数值越大优先级越高 */
#define FE_PRIO_IDLE    0
#define FE_PRIO_LOW     8
#define FE_PRIO_NORMAL  16
#define FE_PRIO_HIGH    24
#define FE_PRIO_REALTIME 31

#define FE_THREAD_DEFAULT_STACK (16 * 1024)

enum fe_thread_state {
    FE_THREAD_UNUSED = 0,
    FE_THREAD_READY,
    FE_THREAD_RUNNING,
    FE_THREAD_SLEEPING,
    FE_THREAD_BLOCKED,
    FE_THREAD_DEAD,
};

typedef void (*fe_thread_entry_t)(void *arg);

struct fe_thread {
    /* 上下文 */
    u64 rsp;                        /* 内核栈指针（切换时保存/恢复） */

    /* 就绪队列链（同优先级内 FIFO → 时间片轮转） */
    struct fe_thread *rq_next;
    struct fe_thread *rq_prev;

    /* 全局线程链（诊断用） */
    struct fe_thread *all_next;

    /* 睡眠链（按唤醒时刻升序） */
    struct fe_thread *sleep_next;
    u64 wake_ns;        /* 睡到什么时候（**绝对纳秒**，不是节拍数——
                         * 见 fe_sched_sleep_until 的说明：节拍数在
                         * 虚拟机上会被突发投递，睡眠会提前几倍返回） */

    /* 僵尸链（已退出、等待回收） */
    struct fe_thread *zombie_next;

    /* ★ 这个线程死了之后，调度器可不可以直接回收它（free）★
     *
     * ★ 为什么必须有这一位：僵尸线程**持有任务的引用** ★
     * 一个已退出的线程如果不被释放，它对任务的那次 `fe_object_ref` 就
     * 一直不放开，于是任务对象永远不销毁——挂在销毁路径上的资源
     * （端口 / MMIO / IRQ / devfs 名字 / 地址空间 / 区间表）**全部泄漏**。
     *
     * 实测（K2/K3 那一轮）：`fe_sched_reap` 写好了却**没有任何调用者**，
     * 而 `fe_thread_join` 只释放它自己等的那一个，于是每一个跑完的程序
     * 都留在任务表里——第一次跑 `ps` 时表里躺着二十多个"已退出"的任务。
     * 这个泄漏一直都在，只是**没有任何东西枚举过任务**，所以没人看见。
     *
     * 但"谁来收尸"不能只有一个答案：`fe_thread_join` 拿着一根裸指针
     * 在等某个特定线程，如果回收器把那个线程先释放了，join 就会踩空
     * （`fe_thread_join` 的注释里记着这个坑）。所以：
     *   - reap_ok = true  → 僵尸由调度器回收（**默认**，进程主线程属此类：
     *                        父进程靠 `fe_process_wait` 拿退出码，不 join 线程）；
     *   - reap_ok = false → 有人在 join 它，回收器不许碰（今天只有引导
     *                        线程 join 的 init 主线程属于此类）。 */
    bool reap_ok;

    /* ★ 这个线程已被判定"必须死"（K2 的闸门从任务级扩到线程级，
     * 见 docs/13-tasks-and-kill.md §6.1）★
     *
     * 语义三条，缺一条就会有人用错：
     *   1. **它只说"这个线程不许再回到用户态"**，不说"任务怎么了"。
     *      任务可以继续活着、继续持有句柄表、devfs 名字、资源认领、
     *      任务 id——`exec` 要的正是这个：映像换掉，身份一个字节都不动；
     *   2. **它只有置位、没有清除**。置它的只有"别的执行体"
     *      （`fe_task_kill_other_threads`）；线程自己从不置、从不清。
     *      线程 `state == FE_THREAD_DEAD` 之后这一位不再有意义（回收时不必清）；
     *   3. **它不改变退出码**：走到闸门时仍然是 `FE_ERR_KILLED`，
     *      与任务级终止一致。僵尸线程的退出码今天没有任何观察者
     *      （非主线程退出时 fe_process_on_thread_exit 直接返回，
     *        而用户态的 join 语义今天不存在）。
     *
     * 判据只有一份：`fe_thread_should_die()`（闸门与三个取消点共用）。
     * 代价：一个 bool，且**没有为它加锁**——单核，置位路径在关中断区间里
     * （见 fe_task_kill_other_threads）。 */
    bool kill_pending;

    u64 id;
    u32 state;
    u32 priority;
    i32 slice;                      /* 剩余时间片（节拍数） */
    i32 exit_code;

    struct fe_task *task;           /* 所属任务（决定能用哪些句柄） */

    /* ---- 多对象等待（wait_any）的登记信息 ----
     *
     * ★ 为什么是"线程身上挂一个节点"而不是"每个对象挂一个等待队列" ★
     *
     * 等 N 个对象需要让**每个**对象都能唤醒这一个线程。做法有两条：
     *   (a) 每个对象维护一个等待队列，等待时把自己插进 N 个队列；
     *   (b) 线程身上挂一个"等待节点"，N 个对象都指向它。
     * 选 (b)：节点只有一个、插入是 N 次指针赋值、唤醒时不需要
     * 从 N 个队列里摘（对象只需 `fe_sched_wake`，清理由被唤醒的线程自己做）。
     * (a) 要在每个对象上分配队列项，而队列项本身还得有内存管理——
     * 在这个规模下是纯负担。
     *
     * ★ 指针的安全性靠"全在关中断区间内操作 + waiting 标志" ★
     * 对象里存的是**线程指针**，而线程可能已经退出。所以唤醒前必须查
     * `waiting`（在同一个关中断区间里置位/清除），否则就是对已释放内存
     * 写标志——那是这类设计最容易出的错。 */
    void *wait_node;                /* 正在等待的节点（NULL = 没有在等） */
    u32   wait_index;               /* 节点里的第几项被满足了 */
    u32   wait_satisfied;           /* 0 = 没满足；非 0 = 第 (N-1) 项就绪 */
    u32   waiting;                  /* 1 = 已登记进各对象（唤醒者必须先查它） */

    void *stack_base;
    u64   stack_size;
    u64   kernel_stack_top;         /* 中断/系统调用进入内核时使用的栈顶 */

    /* ★ 用户态 TLS 基址（线程局部存储）★
     *
     * 这是 `%fs` 基址要装载的值：**用户的线程局部变量**（C 的 `__thread`、
     * C++ 的 `thread_local`、libc 的 errno）都通过 `%fs:offset` 访问。
     *
     * 为什么必须是内核来设：
     *   - `wrmsr(IA32_FS_BASE)` 是特权指令，用户态做不到；
     *   - 它必须在**每次线程切换时**跟着换（否则两个线程会看到同一份 TLS，
     *     那正是"线程局部变量被别的线程改掉"这类最难查的错）；
     *   - 而切换点是内核独占的。
     *
     * ★ 为什么是 %fs 而不是 %gs ★
     * `%gs` 已经被内核拿去做每 CPU 的 syscall 暂存区（`gs:0`/`gs:8`），
     * 而且我们的 syscall 入口**不做 swapgs**（进内核后 `%gs` 仍是用户那个值
     * 也一样能用，因为内核只改 `%gs` 的基址一次、用户态约定不碰它）。
     * 既然 `%gs` 归内核，用户的 TLS 就只能落在 `%fs` 上——
     * 这与 Linux 在 x86-64 上的分工**正好相反**（Linux 用 `%fs` 做 per-CPU、
     * `%gs` 给用户 TLS），所以这两个架构的汇编片段不能互相照搬。
     * 0 表示"这个线程没有 TLS"（内核线程与未设过的用户线程）。 */
    u64   user_fs_base;

    /* 该线程 TLS 块的字节数（诊断用；0 = 没有 TLS） */
    u64   user_tls_size;

    /* TLS 块本身（内核堆上）。用户态**从不直接访问**它的虚拟地址——
     * 它只通过 `%fs` 基址被 CPU 使用，而那是物理地址。
     * 留着这个指针是为了在线程销毁时释放（见 thread_free）。 */
    void *user_tls;

    /* FPU/SIMD 状态区（每线程一块，64 字节对齐）。
     * ★ 每个线程都要有，包括内核线程 ★ 内核线程自己不用 SIMD
     * （内核是 -mgeneral-regs-only 编译的），但如果没有自己的区，
     * 切换进它时上一个线程留在寄存器里的数据就会被它"继承"——
     * 那是**信息泄漏**，不是性能问题。创建失败时为 NULL，
     * 切换路径会用"干净状态"兜底（见 fe_fpu_restore(NULL)）。 */
    void *fpu_area;

    /* 统计 */
    u64 switches;
    u64 cpu_ticks;

    /* ★ 用户态异常处理者（K5）：每线程一份的记忆与回复槽 ★
     *
     * ★ 为什么这些必须是"每线程"而不是"每任务" ★
     * 两条线程可以**同时**出错（各自在自己的异常上下文里等回复），
     * 放任务上会在那一刻互相踩——而那是"跨线程现场伪装"，
     * 是安全边界问题，不是健壮性问题。
     *
     * `last_fault_rip`/`last_fault_cr2`：同一个 `(rip, cr2)` 连续两次就
     * **不再投递**（直接杀）。防的是一个**纯用户态可控的活锁**：
     * 处理者回 RESUME 但没改 `rip`，那条指令会再犯一次，于是再投递……
     * 循环下去。代价是两个 u64；想真的重试的处理者应该改 `rip` 或改内存
     * 映射，让下一次进去时现场不同——那是它自己的责任。
     * ★ 只在"新的一次投递"时更新这两个字段，不要在回复成功之后清 ★
     * （§5.3 那张表：留着会让"处理者接管过一次之后所有异常都不再投递"）。 */
    u64 last_fault_rip;
    u64 last_fault_cr2;

    /* "本线程第几次被投递"（含本次，从 1 起）——就是送进
     * `fe_fault_regs.fault_count` 的那个数（docs/18 §2.2.3）。
     *
     * ★ 为什么是每线程而不是每任务 ★ 文档那一行写的是"**本线程**第几次被投递"。
     * 任务级的累计数在"同一个任务换了一条线程出错"时会给出错误答案，而处理者
     * 正是靠这个数判断"这是不是第二次"。
     *
     * ★ 它同时充当回复的"轮次号" ★ 见下面 `fault_reply.seq`。 */
    u64 fault_count;

    /* 回复槽。★ 投递前必须清（`arrived = false`）★
     * 上一轮的回复不能变成这一轮的答案——漏了它的症状是"处理者这次没回复，
     * 出错线程却拿着上次的现场继续跑了"，比卡住更难查：它表现为
     * "程序偶尔接着跑，但状态是上一次的"。
     *
     * ★ `seq` 是"我这一轮期望处理者回哪个轮次号"★
     * 投递时写成本轮的 `fault_count`，回复时与处理者回显的
     * `fe_fault_regs.fault_count` 逐位比对：对不上就是"回复的是上一轮"
     * （或者处理者改了这个只读字段）→ INVAL 且什么都不做。
     * 光靠 `arrived` 一个布尔量分不出"晚到的上一轮回复"与"本轮的回复"，
     * 而那种错配的表现是"A 的决定被装到 B 的现场上"。 */
    struct fe_fault_reply {
        u64 verdict;                /* FE_FAULT_* */
        u64 seq;                    /* 本轮期望的回显轮次号（== fault_count）*/
        struct fe_fault_regs regs;  /* RESUME 时用它回填现场 */
        bool arrived;
    } fault_reply;

    char name[FE_THREAD_NAME_MAX];
};

/* 创建线程。成功返回线程对象，失败返回 NULL。 */
struct fe_thread *fe_thread_create(const char *name, fe_thread_entry_t entry,
                                   void *arg, u64 stack_size, u32 priority);

/* 结束当前线程（不返回） */
FE_NORETURN void fe_thread_exit(i32 code);

/* 主动让出 CPU，让同/低优先级的线程有机会运行 */
void fe_thread_yield(void);

void fe_thread_sleep_ms(u64 ms);
void fe_thread_sleep_ticks(u64 ticks);

/* 等待线程结束并回收其栈与对象。返回线程退出码。 */
i32 fe_thread_join(struct fe_thread *t);

struct fe_thread *fe_thread_current(void);
u64  fe_thread_total_created(void);
u64  fe_thread_live_count(void);

/* ---- 线程级"必须死"的**唯一**判据（闸门与全部取消点共用）----
 *
 * 返回 true 表示：这个线程不许再回到用户态（它要么已经被单独标记，
 * 要么所属任务正在被终止）。判据只有一份，理由见
 * docs/13-tasks-and-kill.md §6.1：两份判据漂移的症状是"闸门放行了、
 * 取消点没放行"（或反过来），表现成"有时杀得掉、有时杀不掉"——
 * 比崩掉难查得多。 */
bool fe_thread_should_die(const struct fe_thread *t);

/* 线程在等什么：返回 ASCII 短标签，给"叫停超时"那条日志指认凶手用。
 *
 * ★ 屏幕上打印的字符串只能用 ASCII（本项目铁律）★ 所以状态由调用者按
 * **数值**打印（`state=%u`），这里只给等待点的短标签。
 * ★ 为什么需要它 ★ "卡在睡眠上"与"卡在别处"是两种不同的诊断方向。 */
const char *fe_thread_wait_site(const struct fe_thread *t);

/* ★ 任务对象销毁**之前**必须调它 ★（实现在 sched.c，由 task.c 的
 * fe_object_destroy 调用）
 *
 * 把**所有** task 字段等于 task 的线程（含已死的僵尸）的 task 摘成 NULL。
 *
 * ★ 为什么需要它（这是一条实测抓到的 use-after-free）★
 * 有些线程活得比任务久：内核线程 `reap_ok` 默认 false ⇒ 它们变成僵尸后
 * **永远不回收**，而它们对任务的那次引用在 `thread_free` 里才放（永远不放）。
 * 于是任务照样会销毁，而僵尸的 `task` 成了悬空指针；地址被堆复用之后，
 * 这些僵尸一旦再被调度，`fe_sched_maybe_switch` 读
 * `next->task->iopb_slot`（偏移 0x1060）就是一次页错误
 * （QEMU 实测：`RIP=fe_sched_maybe_switch+0x231`、`CR2=0xffffffffa00a4060`，
 * 取证打印指认到 `tls-a/tls-b 的 task 正是最近销毁的那个任务`）。
 *
 * 它与 `thread_mark_dead` 里那句 `rq_remove` 是**两道闸**、防两条不同的路：
 * 那句维护不变式"DEAD 的线程不在就绪队列里"（正本清源）；这句保证即使
 * 僵尸又被排进队列，它的 task 也已经是 NULL、只会被当成内核线程处理。
 *
 * 它不改变任何线程的存活状态，也不释放任何东西。 */
void fe_task_detach_threads(struct fe_task *task);

/* ★ 重建一个线程的 TLS 块（`exec` 提交阶段用，K6）★
 *
 * 它按 `t->task->tls_*`（**新映像**的模板）重新分配并填充 `%fs` 那块，
 * 写回 `t->user_tls` / `user_fs_base` / `user_tls_size`，返回新的 `%fs` 基址。
 *
 * ★ 调用顺序：必须在 `fe_task_attach_space()` **之后** ★ 否则用的是旧映像
 * 的模板（症状：新程序读到的 `__thread` 变量还是旧程序的那一份）。
 * ★ 为什么不让 exec 自己写一份 ★ 见 sched.c 里那段说明：TLS 布局的约定
 * 已经写在两处，第三处必然漂移，而漂移的症状是"线程局部变量读到别人的值"。 */
u64 fe_thread_tls_rebuild(struct fe_thread *t);

/* ---- 线程枚举（TASK_LIST 快照用，见 kernel/arch/x86_64/syscall.c 的说明）----
 * 按"过滤全局线程链"实现，不额外维护每任务的线程链：
 * 唯一的那条链只有两处维护点（创建时插、销毁时摘），漏一处就是
 * "任务退出后线程还挂在它链上"这类只能靠遍历时崩掉才发现的错误。
 * 三者都跳过已退出的线程（DEAD）——枚举要回答的是"谁在跑"。 */
struct fe_thread *fe_task_thread_first(struct fe_task *task);
struct fe_thread *fe_thread_next_of(struct fe_thread *cur);
u32  fe_task_thread_count(struct fe_task *task);

void fe_thread_dump_all(void);
const char *fe_thread_state_name(u32 state);

#endif /* FE_SCHED_THREAD_H */
