/* SPDX-License-Identifier: 0BSD */
/* 进程间通信（M3）。
 *
 * 三种机制，构成微内核的全部 IPC 原语：
 *   - 端点 endpoint：同步消息传递。可阻塞的收/发，构成 RPC 的基础；
 *   - 通知 notification：轻量的位图信号，用于中断投递与异步完成事件；
 *   - 内存对象 memory object：物理帧的引用计数容器，可映射到任务地址空间共享。
 *
 * 消息可以携带句柄（能力传递），接收方在自己的句柄表里得到这些能力的副本。
 */
#ifndef FE_IPC_H
#define FE_IPC_H

#include <fe/types.h>
#include <fe/status.h>
#include <fe/object.h>
#include <fe/syscall.h>
#include <fe/task.h>

/* 单条消息的内联载荷上限。
 *
 * 从 256 提到 1024 是为了块设备：一个 ATA 扇区是 512 字节，
 * 256 的载荷连一个扇区都装不下，服务端就得把一次读拆成两条消息，
 * 而拆消息的边界处理（半扇区、重试）是纯粹的额外复杂度。
 *
 * 代价是消息池的内存（64 条 × 1.2 KiB ≈ 77 KiB）与系统调用路径上的
 * 两次 1 KiB 栈缓冲。**这不是长久方案**：真正的批量传输应该走
 * 「随消息传一个内存对象、由服务端直接写进去」，让内核完全不碰数据。
 * 那件事留给 DMA 那一代。 */
#define FE_MSG_MAX_PAYLOAD 1024
#define FE_MSG_MAX_HANDLES 4
#define FE_MSG_QUEUE_MAX   64

/* 一个对象上最多允许几个等待者。
 *
 * 为什么不是 1：`wait_any` 让一个线程同时等 N 个对象，而**多个线程**
 * 也可能等同一个对象（例如两个服务线程等同一个请求端点）。
 * 上限取 4：真实的驱动形态是"一个服务线程等自己的端点 + 中断通知"，
 * 再加一两个上级/下级对象就到顶了；再多应该用通知对象做二级唤醒
 * （把 N 个来源合成一个位图），而不是无限扩这张表。 */
#define FE_WAITERS_MAX 4

/* ---------------- 端点 ---------------- */

struct fe_endpoint {
    struct fe_object_header hdr;
    struct fe_message *head;
    struct fe_message *tail;
    u32 count;
    u32 flags;
    struct fe_thread *receiver;     /* 阻塞等待接收的线程（兼容单等待者路径） */
    /* 多对象等待的登记：这些线程的等待节点覆盖了本端点。
     * 唤醒时**只需要** fe_sched_wake + 置 satisfied，摘除由线程自己做。 */
    struct fe_thread *waiters[FE_WAITERS_MAX];
    u64 sent_total;
    u64 recv_total;
    struct fe_endpoint *free_next;  /* 对象池空闲链（仅在已销毁、待复用时有效） */
};

/* ---------------- 通知 ---------------- */

struct fe_notification {
    struct fe_object_header hdr;
    volatile u64 bits;
    struct fe_thread *waiter;
    u64 wait_mask;
    struct fe_thread *waiters[FE_WAITERS_MAX];  /* 见 fe_endpoint 的说明 */
    u64 signal_total;
};

/* ---------------- 内存对象 ---------------- */

struct fe_memory_object {
    struct fe_object_header hdr;
    phys_addr_t *frames;        /* 物理帧数组 */
    u64 page_count;
    u64 size;
    u32 flags;
};

/* ---------------- 消息 ---------------- */

struct fe_message {
    struct fe_message *next;
    struct fe_thread *sender;
    struct fe_task *sender_task;
    struct fe_endpoint *reply_ep;   /* 内核直接持有回复端点（引用计数） */
    struct fe_msg_header hdr;
    struct fe_object_header *transferred[FE_MSG_MAX_HANDLES];   /* 随消息传递的能力 */
    /* 发送方持有的权限位。接收方按「发送方权限 - TRANSFER」安装：
     * 能力只能收窄，不能放大——这是能力模型的核心约束的落点。
     * 单独存一份而不是查发送方句柄表：消息可能在发送方关掉句柄之后才被取走。 */
    u32 transferred_rights[FE_MSG_MAX_HANDLES];
    u8 payload[FE_MSG_MAX_PAYLOAD];
};

/* ---------------- 供内核自身使用的 API（M4 起由系统调用包装） ---------------- */

void fe_ipc_init(void);

fe_status_t fe_endpoint_create(struct fe_task *t, u32 flags, fe_handle_t *out);

/* 发送：把消息排入目标端点队列。handles 可传 NULL。 */
fe_status_t fe_endpoint_send(struct fe_task *t, fe_handle_t ep,
                             const struct fe_msg_header *hdr,
                             const void *payload, const fe_handle_t *handles);

/* ★ 直接按**对象**发送（K5 的异常投递用）★
 *
 * 与上面那条的唯一区别是"不查句柄表、不要求 FE_RIGHT_SEND"。理由：
 * 内核的异常投递**不该依赖用户态当前的权限位**——登记那一刻内核已经记下
 * "往这里投"并对端点加了一次引用，投递时用那次登记的结果就够了；
 * 走句柄那条路的话，用户态 `HANDLE_DUP` 收窄权限或 `HANDLE_CLOSE` 关掉句柄
 * 都会让"处理者能不能收到异常"变成另一个问题的答案。
 *
 * `ep` 由调用者保证有效（持有引用的对象指针）。入队主体与
 * `fe_endpoint_send` 共用同一段实现。 */
fe_status_t fe_endpoint_send_obj(struct fe_task *t, struct fe_endpoint *ep,
                                 const struct fe_msg_header *hdr,
                                 const void *payload, const fe_handle_t *handles);

/* 丢弃端点队列里的全部积压消息，返回丢了几条（**给自检用**：
 * 生产路径上没有调用者）。理由见 ipc.c 里实现的说明。 */
u32 fe_endpoint_drain(struct fe_endpoint *ep);

/* 接收：无消息则阻塞当前线程直到有消息。
 * out_reply_ep 返回「回复能力」句柄（发送方带回复端点时有效），
 * out_transferred 返回随消息传递过来的句柄（长度 FE_MSG_MAX_HANDLES，无效项为 0）。 */
fe_status_t fe_endpoint_recv(struct fe_task *t, fe_handle_t ep,
                             struct fe_msg_header *hdr,
                             void *payload, u32 payload_cap,
                             fe_handle_t *out_reply_ep,
                             fe_handle_t *out_transferred);

/* 回复一条收到的消息：reply_ep 来自 fe_endpoint_recv 的 out_reply_ep */
fe_status_t fe_msg_reply(struct fe_task *t, fe_handle_t reply_ep,
                         const void *payload, u32 len);

/* ------------------------------------------------------------------ */
/* 多对象等待（wait_any）                                              */
/* ------------------------------------------------------------------ */

/* 等待的**条件**类型。数值与用户 ABI（fe/syscall.h）一致。 */
#define FE_WAIT_MSG  1      /* 端点上来了消息 */
#define FE_WAIT_BITS 2      /* 通知对象上有掩码里的位被置位 */
#define FE_WAIT_ADDR 3      /* ★ K11 ★ 一个用户地址上的值**不再等于**期望值 */

/* 一个等待目标：句柄 + 条件 + 通知掩码（FE_WAIT_BITS 时才用）
 *                          + 用户地址三项（FE_WAIT_ADDR 时才用） */
struct fe_wait_target {
    fe_handle_t handle;
    u32 kind;               /* FE_WAIT_* */
    u64 mask;               /* FE_WAIT_BITS：等哪些位 */
    /* ★ K11：只有 kind == FE_WAIT_ADDR 用这三项 ★
     * 设计与代价见 docs/21-user-address-wait.md §4.2。 */
    u64 addr;               /* 本地址空间里的用户地址，**必须 4 字节对齐** */
    u64 expected;           /* 期望的 u32 值（值≠它就**不睡**，返回 AGAIN）*/
    u64 deadline_ns;        /* **绝对**单调纳秒；0 = 无限等 */
};

/* 一次等待的结果。`index` 是**第几个目标**就绪（0 起）。 */
struct fe_wait_result {
    u32 index;
    u32 _pad;
    u64 bits;               /* 通知：被置位且被等待的位 */
    /* 端点就绪时，消息**顺带**收下来：不这么做的话调用者还得再调一次
     * recv，而那两次调用之间消息可能被同一任务的**另一个线程**取走
     * （POSIX 的 select 就是这个形状——它只报"可读"，不保证读得到）。
     * 驱动服务通常只有一个线程读自己的端点，所以顺带收更简单也更安全。 */
    struct fe_msg_header hdr;
    fe_handle_t reply_ep;
    u8 payload[FE_MSG_MAX_PAYLOAD];
    u32 payload_cap;
};

/* 等 [targets, targets+n) 里**任意一个**就绪。n 必须 ≥ 1 且 ≤ 8。
 *
 * 语义：
 *   - 先检查每个目标"现在已经就绪吗"，是就立刻返回（不阻塞）；
 *   - 都不就绪则阻塞，直到其中之一被（另一个线程/中断）置为就绪；
 *   - 返回 FE_OK 表示某个目标触发了（res->index 指出是哪个）；
 *     返回负值是错误（句柄坏、kind 非法、n 越界）。
 *
 * ★ 为什么这是一个内核机制，而不是"用户态轮询" ★
 * 轮询要烧 CPU 且有延迟；而"一个线程同时等中断与请求"是驱动的**常态**
 * （驱动主循环就是"等设备、等命令"的合流）。没有它，每个驱动都要开
 * 两条线程加一套自己写的唤醒逻辑——那套逻辑的正确性（丢唤醒）
 * 每个驱动作者都要重新踩一遍。
 *
 * ★ 注意：本函数**只等**，不改变对象状态 ★
 * 端点的消息与通知的位都在 res 里被取走（这是"消费掉这次就绪"），
 * 但没有就绪的目标**一动都不动**——不会因为"顺手检查"而丢掉什么。 */
fe_status_t fe_wait_any(struct fe_task *t, const struct fe_wait_target *targets,
                        u32 n, struct fe_wait_result *res,
                        u32 payload_cap);

/* ★ 自检钩子（K11 的 W2）：**只给自检用**，生产路径上恒为 NULL ★
 *
 * 它在"已经检查过一遍、还没登记"的那一刻被调用 —— 那正是丢唤醒窗口。
 * 自检用它把窗口确定性地造出来（钩子当场把条件置真 + 调一次唤醒），
 * 于是"漏醒"变成可复现的红，而不是靠概率。
 * 实现与调用点见 kernel/ipc/ipc.c 的 fe_wait_any。 */
void fe_wait_any_set_hook(void (*fn)(struct fe_task *, const struct fe_wait_target *,
                                     u32, void *), void *ctx);

/* ------------------------------------------------------------------ */
/* 等一个用户地址（K11，docs/21-user-address-wait.md）                 */
/* ------------------------------------------------------------------ */

/* 等 `t->space` 里 `addr` 处的 u32 **不再等于** `expected`。
 *
 * 语义（逐条都能证伪，见 docs/21 §4.2）：
 *   - `FE_OK`          被唤醒（**必须重查**：内核不保证为什么醒）；
 *   - `FE_ERR_AGAIN`   值**不等于**期望（**没有睡过**——这就是 futex 的 EAGAIN）；
 *   - `FE_ERR_TIMEOUT` deadline 到了；
 *   - `FE_ERR_FAULT`   地址读不了（未映射/不可读）——**不是**"值不相等"；
 *   - `FE_ERR_CANCELED` 线程被取消 / 任务被终止；
 *   - `FE_ERR_INVAL`   地址没 4 字节对齐。
 *
 * ★ 原子性（这是这个机制存在的**全部理由**）★ "读值"与"挂上等待链 +
 * 置 BLOCKED"在**同一个关中断区间**里完成，所以不存在"值在我检查之后、
 * 睡下之前被改掉，而我照样睡死"那个窗口。窗口的存在性由自检 W2 证明
 * （它用钩子把窗口确定性地造出来，修之前是红的）。
 *
 * ★ 唤醒是"点名"，不是"投递" ★ 返回 `FE_OK` 只说明"有人叫了你"，
 * 条件的真假**必须由调用者自己重查**——这条契约让伪唤醒（超时、
 * broadcast 波及、取消）自动变成正确行为。 */
fe_status_t fe_wait_addr(struct fe_task *t, u64 addr, u64 expected, u64 deadline_ns);

/* 唤醒 `t->space` 里等在 `addr` 上的至多 `count` 个线程（`count == 0` = 全部）。
 * 返回实际唤醒的个数。★ 键是 `(地址空间, 地址)`：别的任务在**同一个虚拟
 * 地址**上等的一个都不许被唤醒（W4 就是堵这个）★ */
u32 fe_wake_addr(struct fe_address_space *space, u64 addr, u32 count);

/* ★ 只给自检用的只读访问器 ★ 数一个地址上有几个等待者（键要一起对上）。
 * 为什么需要它：判据必须是**可读的事实**，而不是"跑起来没崩"
 * （与 `fe_notification_waiter` 同一条理由，见 docs/13 §6.5 的教训）。 */
u32 fe_wait_addr_waiters(struct fe_address_space *space, u64 addr);

/* ★ 只给调度器调 ★ 一个带 deadline 的地址等待到点了：把线程从地址链上
 * 摘掉（状态与就绪队列**不归这里管**，归 sched.c）。
 * 为什么要有它：线程在两条链上，而"到点"那条路径由 sched.c 发现——
 * 它不认识地址桶，所以走这个回调。docs/21 §5.3。 */
void fe_wait_addr_on_deadline(struct fe_thread *t);

/* 同步调用：发请求并等待回复（内部建立临时回复端点） */
fe_status_t fe_endpoint_call(struct fe_task *t, fe_handle_t ep,
                             const void *req, u32 req_len,
                             void *reply, u32 reply_cap, u32 *reply_len);

fe_status_t fe_notification_create(struct fe_task *t, fe_handle_t *out);
fe_status_t fe_notification_signal(fe_handle_t nt, u64 bits);/* 直接对通知对象置位，不走句柄表。中断投递（irq.c）与其它内核内部路径使用：
 * 中断发生时正在跑的线程未必是通知的持有者，用句柄查表会查错人。 */
fe_status_t fe_notification_signal_obj(struct fe_notification *nt, u64 bits);
/* 等待：mask 中任意一位被置位即返回，返回时把这些位从对象中清除 */
fe_status_t fe_notification_wait(fe_handle_t nt, u64 mask, u64 *out_bits);

fe_status_t fe_memory_create(struct fe_task *t, u64 size, u32 flags, fe_handle_t *out);/* 映射到内核地址空间，返回可直接读写的虚拟地址 */
fe_status_t fe_memory_map_kernel(struct fe_task *t, fe_handle_t mo, u32 prot,
                                 void **out_virt);
/* 映射到用户任务地址空间（M4 起使用）：hint 为 0 时由内核选地址 */
fe_status_t fe_memory_map_user(struct fe_task *t, fe_handle_t mo, u64 hint, u64 size,
                               u32 prot, void **out_virt);

/* 解除内核侧映射（帧仍由内存对象持有，不会因此释放） */
fe_status_t fe_memory_unmap_kernel(void *virt, u64 size);

/* 引用计数归零时由 object.c 调用 */
void fe_endpoint_destroy(struct fe_endpoint *ep);void fe_notification_destroy(struct fe_notification *nt);
void fe_memory_destroy(struct fe_memory_object *mo);

/* 引用计数查询（诊断与自检用） */
u32 fe_object_refcount(struct fe_object_header *hdr);

/* 统计 */
struct fe_ipc_stats {
    u64 endpoints;
    u64 notifications;
    u64 memory_objects;
    u64 messages_sent;
    u64 messages_recv;
    u64 messages_allocated;
    u64 messages_freed;
};
void fe_ipc_get_stats(struct fe_ipc_stats *out);

/* 由 send / signal 调用：唤醒等在这个对象上的 wait_any 登记者。
 * 参数是对象里那张等待者表（端点与通知各有一张，见结构定义）。
 * 放在文件末尾是因为它依赖 struct fe_thread 的完整定义。 */
void fe_ipc_wake_object_waiters(struct fe_thread **table);

/* 取消一个正阻塞着的线程（K2）：把它从等待对象上摘下来并唤醒，
 * 好让它回到阻塞循环顶部的取消点、带着 FE_ERR_CANCELED 返回。
 *
 * task 必须是 victim 所属的任务（**不能**用 fe_task_current()：
 * 取消方是在替别人摘登记）。victim 必须正阻塞在
 * fe_endpoint_recv / fe_notification_wait / fe_wait_any 之一上；
 * 对不在阻塞的线程调用它是安全的空操作。 */
void fe_thread_cancel(struct fe_task *task, struct fe_thread *victim);

/* 通知对象当前的等待者（NULL = 没有）。D1 的判据用它把"槽里有没有残留
 * 一个已经死掉的线程"读成一个**可读的事实**——判据不能靠"跑起来没崩"。
 * 只看不写。 */
struct fe_thread *fe_notification_waiter(const struct fe_notification *nt);

/* 自检：多对象等待（wait_any：已就绪、被通知唤醒、被消息唤醒）。
 * 返回失败项数。 */
u32 fe_selftest_wait_any(void);

/* K11 的自检（丢唤醒窗口 W2、键、容量、取消、超时……）。
 * 返回失败项数。设计与逐条判据见 docs/21-user-address-wait.md §7.1。 */
u32 fe_selftest_wait_addr(void);

/* M3 自检：返回失败项数（0 = 全部通过） */
u32 fe_selftest_ipc(void);

#endif /* FE_IPC_H */
