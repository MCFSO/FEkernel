/* SPDX-License-Identifier: 0BSD */
/* 任务（进程的内核侧表示）。
 *
 * M3 阶段任务只承载「句柄表」——即这个执行体持有哪些能力。
 * M4 引入用户态后，任务还会带上地址空间、线程组、父子关系等；
 * 但「能力由句柄表表达」这一点从 M3 起就固定下来了。
 */
#ifndef FE_TASK_H
#define FE_TASK_H

#include <fe/types.h>
#include <fe/object.h>

#define FE_TASK_NAME_MAX 16

struct fe_address_space;
struct fe_thread;
struct fe_endpoint;

struct fe_task {
    struct fe_object_header hdr;
    struct fe_handle_table handles;
    struct fe_address_space *space;     /* 用户任务才有；内核任务为 NULL */
    struct fe_task *next;               /* 全局任务链 */

    /* ★ 线程局部存储（TLS）的初始化映像 ★
     *
     * 可执行文件里的 `PT_TLS` 段给出"每个线程开始时该有什么"：
     *   tls_init  = 映像字节（在**内核可见**的地址上，来自引导模块）
     *   tls_size  = 每个线程要分配多大（p_memsz，通常 > p_filesz，多出来的是 .tbss）
     *   tls_align = 对齐要求
     *
     * 为什么要把它存在**任务**上：TLS 是"每线程一份"，但映像只有一份
     * （在可执行文件里）。所以新建线程时要从这里复制一份出去——
     * 而线程创建可能发生在任意时刻，那时早就找不到 ELF 了。
     * 全体为零表示这个程序没有 TLS。 */
    const void *tls_init;
    u64 tls_size;
    u64 tls_align;

    u64 user_map_next;                  /* 用户态 mmap 区的递增分配指针 */
    u64 user_stack_next;                /* 用户线程栈区的递减分配指针 */

    /* ★ 地址空间区间表（按需分页的基础，见 fe/mm/vma.h）★
     * 指针而不是内嵌结构：区间表有 32 项、约 1.3 KiB，内嵌会让每个
     * **内核**任务也白白占着它（内核任务没有用户地址空间）。
     * 只有用户任务才分配，NULL 表示"没有区间表"（等价于没有用户空间）。 */
    struct fe_vma_table *vmas;

    /* 主栈当前已映射到的最低地址（含）。
     * 栈按需向下增长时用它判断"这一页是不是紧挨着栈下方"——
     * 只看 VMA 的 base（那是**允许**的下界）会把"跳过一大段往下访问"
     * 也当成栈增长，那等于把栈的保留区变成任意分配的入口。 */
    u64 stack_low;
    int iopb_slot;                      /* TSS I/O 位图槽位；-1 = 禁止一切端口 */
    u32 granted_ports;                  /* 已授予的端口区间数（只增不减，用于诊断） */
    u64 id;

    /* ---- 进程语义（学 Linux）----
     * 主线程退出即进程结束；父进程的 wait 通过 waiter/exited 这对字段同步，
     * 走的正是通知对象那一套「置标志 + 唤醒」的模式。 */
    struct fe_thread *main_thread;
    struct fe_thread *waiter;           /* 正在 wait 本任务的线程（至多一个） */
    i32  exit_code;
    bool exited;

    /* ★ 本任务正在被终止（K2）★
     *
     * 置上之后这个任务的线程**再也不会回到用户态**：唯一的闸门在
     * `fe_sched_maybe_switch`（isr 与 syscall 两条返回路径的必经之处），
     * 它会把当前线程就地变成僵尸再切走。
     *
     * 阻塞中的线程走不到闸门，所以它们由**取消点**唤醒：
     * 内核在每个阻塞重试循环顶部检查这一位，成立就返回 FE_ERR_CANCELED，
     * 线程于是带着错误返回用户态——途中经过闸门，在那里死掉。
     * 设计与代价见 docs/13-tasks-and-kill.md §3。 */
    bool dying;
    /* 已经因为"任务正在被终止"而被唤醒/标记过的线程数（诊断）。 */
    u32 killed_threads;

    /* ★ 用户态异常处理者（K5，docs/18-user-fault-handler.md）★
     *
     * 登记在**任务**上（不是线程）：端点是句柄、句柄表是任务的；
     * 运行库的形态就是"一个进程一个崩溃处理器"（C++ 的 std::terminate、
     * JIT 的 SIGSEGV 兜底都是进程级的）；每线程一个端点会把注册/注销变成
     * N 份，而 `FE_HANDLE_TABLE_SIZE` 是 256。
     *
     * ★ `handler_ep` 是**对象指针**，内核自己对它加过一次引用 ★
     * 用户态可以 `HANDLE_CLOSE` 掉那个句柄，但内核的登记里还存着指针——
     * 所以登记时 `fe_object_ref`，任务销毁路径上（task.c 的
     * `case FE_OBJ_TASK`）必须 `fe_object_unref`。
     * 漏掉这一次 unref = 端点对象永不销毁（它挂着消息队列与等待者表）；
     * 多加一次 = 用户关掉句柄之后端点还在。两种都不报错，只是慢漏或不漏。
     *
     * ★ 为什么是对象指针而不是句柄号 ★
     * 用句柄号就要在投递时走 `fe_handle_lookup`，而它要求 `FE_RIGHT_SEND`
     * ——于是"处理者能不能收到异常"变成了"那个句柄此刻有什么权限位"，
     * 而权限位是用户态可以自己收窄的。**内核的异常投递不该依赖用户态当前的权限位。** */
    struct fe_endpoint *handler_ep;     /* NULL = 没登记 */
    u64 handler_buf;                    /* 用户现场缓冲区（虚拟地址，属**映像**）*/
    /* ★ 收件线程：登记时的调用线程 ★ "谁登记谁收"
     * 理由：端点的接收权不受线程限制，但"我登记的时候打算让谁收"只有调用者
     * 自己知道。钉成主线程会在"工作线程登记、主线程收"这种布局下强迫用户改代码。
     * ★ 它拦的是一种实现时一定会漏的情形 ★ "单线程程序自己给自己登记"——
     * 出错线程卡在 user_fault 里等回复，而唯一能回复的线程正是它自己：
     * 结果是等满轮数才死（"登记了处理者但每次异常都要等很久"），比崩溃难查。
     * 这条判据与 `fault_depth` **不重复**：那时深度还是 0，深度判据放行。 */
    struct fe_thread *handler_recv_thread;
    /* ★ 投递深度（0 或 1）+ 谁在等 ★ 为什么在任务上：两条线程各持一个
     * 0/1 的线程级计数就能同时各投递一次，"深度 1"退化成了"深度 N"。
     * 但它必须有主：回复槽是**每线程**的，加一的那一处要同时说清"谁的槽在等"。 */
    u32  fault_depth;
    struct fe_thread *fault_owner;      /* fault_depth != 0 时：正在等回复的线程 */
    u64  fault_seq;                     /* 投递序号：回复必须对上它 */
    u64  fault_count_total;             /* 本任务累计投递次数（诊断） */

    char name[FE_TASK_NAME_MAX];
};

/* 分配任务对象（不含地址空间）；由 task.c 实现 */
struct fe_task *fe_task_alloc(const char *name);

/* 创建一个内核任务（无独立地址空间，共享内核页表） */
struct fe_task *fe_task_create_kernel(const char *name);

/* 当前线程所属任务；若当前线程没有任务则返回内核任务 */
struct fe_task *fe_task_current(void);

/* 内核任务（引导线程所属），系统启动时自动建立 */
struct fe_task *fe_task_kernel(void);

void fe_task_init(void);

void fe_task_dump_all(void);

/* 当前存活的任务数（自检用来验证任务对象确实被销毁了） */
u32 fe_task_live_count(void);

/* 按 id 找任务，找不到返回 NULL。
 *
 * ★ 它是给"内核自己需要确认一个 id 是否还存在"用的 ★
 * 目前唯一的调用者是 RESOURCE_GRANT_ID：设备管理器按 id 授予硬件之前，
 * 内核必须确认那个任务**现在活着**——否则等于给一个还没出现的 id
 * 预留了位置，而 id 会被复用（见 fe/syscall.h 里 0x88 的说明）。
 * 用户态拿不到这个能力：TASK_LIST 给的是只读快照，没有句柄。 */
struct fe_task *fe_task_by_id(u64 id);

/* 按名字找任务（第一个匹配）。名字**不唯一**（多个用户线程都叫 "user"），
 * 调用者问的应该是服务名（引导链上唯一），而不是拿它当唯一标识。 */
struct fe_task *fe_task_by_name(const char *name);

/* 全局任务链的表头（TASK_LIST 快照按它遍历）。 */
struct fe_task *fe_task_first(void);

/* ★ 摘掉"某个线程正在等这个任务"的登记（wait 的第五处等待登记）★
 *
 * `fe_process_wait` 会把 `child->waiter` 设成自己再阻塞；而
 * `fe_process_on_thread_exit` 是唯一会清它的地方（孩子退出时才清）。
 * 于是线程**被取消**（而不是孩子退出）时，那个槽留着它——线程随后返回
 * CANCELED、走闸门、死、僵尸被回收，槽里就是一根指向已释放内存的指针，
 * 而下次孩子退出时 `fe_process_on_thread_exit` 会拿它去 `fe_sched_wake`
 * （读 `t->state`）——一次 use-after-free。
 *
 * 与 D1 的 `nt->waiter` 是同一类缺陷的第五个落点。判据是**线程指针**：
 * `child->waiter == t` 才清（`waiter` 是单槽，清错了会把别人的等待弄丢）。
 * 需要扫全局任务链，因为 `fe_process_wait` 的调用者手里只有**子任务句柄**，
 * 而"谁在等我"这个消息记在那**子任务**自己的结构里。 */
void fe_task_clear_waiter(struct fe_thread *t);

/* 把任务/线程快照按**共享 ABI**（见 fe/syscall.h 的 FE_TASK_*_X 常量）
 * 拼进 buf。返回写入的任务数；负数 = 错误
 * （FE_ERR_INVAL 参数非法 / FE_ERR_NOSPC 缓冲不够）。
 *
 * ★ buf 是**内核内存** ★ 调用者（sys_task_list）负责把它整块拷给用户态。
 * 之所以让这一层只认"一块缓冲"，是为了内核自检能直接调它——
 * 自检手里没有用户指针，走不到"往用户态拷"那一层。见
 * kernel/task/tasklist.c 开头的说明。 */
i64 fe_task_snapshot_build(u8 *buf, u64 buf_len, u32 task_cap, u32 thread_cap,
                           u64 caller_task_id);

/* 任务/线程快照的自检（K3）。返回失败项数。 */
u32 fe_selftest_tasklist(void);

#endif /* FE_TASK_H */
