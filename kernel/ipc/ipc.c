/* SPDX-License-Identifier: 0BSD */
#include <fe/ipc.h>
#include <fe/mm/kheap.h>
#include <fe/mm/pmm.h>
#include <fe/mm/vmm.h>
#include <fe/sched/sched.h>
#include <fe/sched/thread.h>
#include <fe/resource.h>        /* fe_resource_forget_thread_locks（D2① 的锁登记摘除） */
#include <fe/task.h>            /* fe_task_clear_waiter（wait 的 waiter 槽摘除） */
#include <fe/user.h>
#include <fe/time.h>            /* K11：deadline 比的是 K7 的单调纳秒 */
#include <fe/kprintf.h>
#include <fe/panic.h>
#include <fe/string.h>
#include <fe/io.h>

static u64 g_endpoint_count;
static u64 g_notification_count;
static u64 g_memory_count;
static u64 g_messages_sent;
static u64 g_messages_recv;
static u64 g_messages_allocated;
static u64 g_messages_freed;

/* 内存对象映射区（内核侧）的虚拟地址分配器 */
static virt_addr_t g_objmap_next = FE_OBJMAP_BASE;

/* 消息池：IPC 是微内核最热的路径，每条消息都走一次堆分配/释放代价太高。
 * 用一个空闲链把消息结构体回收复用，容量设上限以免长期占着内存不放。 */
#define FE_MSG_POOL_MAX 64
static struct fe_message *g_msg_free_list;
static u32 g_msg_pool_size;

/* 端点对象池：同步 RPC 每次调用都要建一个临时回复端点再销毁，
 * 端点对象的堆分配/释放因此也成了 IPC 的固定开销。同样回收复用。 */
#define FE_EP_POOL_MAX 32
static struct fe_endpoint *g_ep_free_list;
static u32 g_ep_pool_size;

u32 fe_object_refcount(struct fe_object_header *hdr)
{
    return hdr ? hdr->refcount : 0;
}

void fe_ipc_init(void)
{
    g_objmap_next = FE_OBJMAP_BASE;
    g_endpoint_count = g_notification_count = g_memory_count = 0;
    g_messages_sent = g_messages_recv = 0;
    g_messages_allocated = g_messages_freed = 0;
    g_msg_free_list = NULL;
    g_msg_pool_size = 0;
    g_ep_free_list = NULL;
    g_ep_pool_size = 0;
}

/* ------------------------------------------------------------------ */
/* 消息                                                                */
/* ------------------------------------------------------------------ */

/* 消息池的空闲链（见文件开头的说明） */
static struct fe_message *g_msg_free_list;
static u32 g_msg_pool_size;

static struct fe_message *message_alloc(void)
{
    struct fe_message *m;
    if (g_msg_free_list) {
        m = g_msg_free_list;
        g_msg_free_list = m->next;
    } else {
        m = (struct fe_message *)fe_kmalloc(sizeof(*m));
        if (!m) {
            return NULL;
        }
        g_msg_pool_size++;
    }
    /* 只清必要的字段：整块 memset 会白清 256 字节载荷区 */
    m->next = NULL;
    m->sender = NULL;
    m->sender_task = NULL;
    m->reply_ep = NULL;
    m->hdr.protocol = 0;
    m->hdr.opcode = 0;
    m->hdr.payload_len = 0;
    m->hdr.handle_count = 0;
    m->hdr.request_id = 0;
    for (u32 i = 0; i < FE_MSG_MAX_HANDLES; i++) {
        m->transferred[i] = NULL;
        m->transferred_rights[i] = 0;
    }
    g_messages_allocated++;
    return m;
}

static void message_free(struct fe_message *m)
{
    if (!m) {
        return;
    }
    if (m->reply_ep) {
        fe_object_unref(&m->reply_ep->hdr);
        m->reply_ep = NULL;
    }
    for (u32 i = 0; i < FE_MSG_MAX_HANDLES; i++) {
        if (m->transferred[i]) {
            fe_object_unref(m->transferred[i]);
            m->transferred[i] = NULL;
        }
        m->transferred_rights[i] = 0;
    }
    g_messages_freed++;
    /* 回到池里复用，而不是还给堆 */
    if (g_msg_pool_size <= FE_MSG_POOL_MAX) {
        m->next = g_msg_free_list;
        g_msg_free_list = m;
    } else {
        fe_kfree(m);
        g_msg_pool_size--;
    }
}

/* 把消息排入端点队列 */
static void endpoint_push(struct fe_endpoint *ep, struct fe_message *m)
{
    m->next = NULL;
    if (ep->tail) {
        ep->tail->next = m;
    } else {
        ep->head = m;
    }
    ep->tail = m;
    ep->count++;
}

static struct fe_message *endpoint_pop(struct fe_endpoint *ep)
{
    struct fe_message *m = ep->head;
    if (!m) {
        return NULL;
    }
    ep->head = m->next;
    if (!ep->head) {
        ep->tail = NULL;
    }
    m->next = NULL;
    ep->count--;
    return m;
}

static void wake_receiver(struct fe_endpoint *ep)
{
    if (ep->receiver) {
        struct fe_thread *t = ep->receiver;
        ep->receiver = NULL;
        fe_sched_wake(t);
    }
}

/* 组装一条消息：复制载荷、引用要传递的句柄对象、挂上回复端点 */
static fe_status_t build_message(struct fe_task *t, struct fe_message **out,
                                 const struct fe_msg_header *hdr,
                                 const void *payload, const fe_handle_t *handles,
                                 struct fe_endpoint *reply_ep)
{
    if (hdr->payload_len > FE_MSG_MAX_PAYLOAD ||
        hdr->handle_count > FE_MSG_MAX_HANDLES) {
        return FE_ERR_INVAL;
    }
    /* 说"传了 N 个句柄"却给了空数组，是调用方的 bug。内核在这里挡住，
     * 而不是去解引用一个空指针——内核里的空指针解引用是崩溃，不是错误码。 */
    if (hdr->handle_count > 0 && !handles) {
        return FE_ERR_INVAL;
    }
    struct fe_message *m = message_alloc();
    if (!m) {
        return FE_ERR_NOMEM;
    }
    m->sender = fe_thread_current();
    m->sender_task = t;
    m->hdr = *hdr;
    if (payload && hdr->payload_len) {
        memcpy(m->payload, payload, hdr->payload_len);
    }

    /* 句柄传递 = 能力传递：发送方必须持有 TRANSFER 权限，
     * 内核在这里先把对象引用住，接收方取出时再安装到它自己的句柄表里。
     *
     * ★ 同时记下发送方当时持有的权限位 ★ 接收方按「发送方权限 - TRANSFER」
     * 安装。不记的话，接收方只能拿到一个"全权限"句柄，而那是权限放大：
     * A 把一个只读的内存对象交给 B，B 就会拿到可写的能力，还能无限转发。
     * 这里记的是**快照**而不是发送方句柄表的实时值：消息可能在发送方
     * 关掉那个句柄（甚至退出）之后才被取走。 */
    for (u32 i = 0; i < hdr->handle_count; i++) {
        struct fe_object_header *obj = NULL;
        fe_status_t s = fe_handle_lookup(&t->handles, handles[i],
                                         FE_RIGHT_TRANSFER, &obj);
        if (fe_failed(s)) {
            message_free(m);
            return s;
        }
        u32 rights = 0;
        /* 查得到对象就必然查得到权限（同一次查表），失败只可能是句柄号非法，
         * 而上面那次 lookup 已经把它挡掉了；这里仍然检查返回值，
         * 免得将来改动 lookup 语义时这里静默变成"权限为 0"。 */
        if (fe_failed(fe_handle_rights(&t->handles, handles[i], &rights))) {
            message_free(m);
            return FE_ERR_BADHANDLE;
        }
        fe_object_ref(obj);
        m->transferred[i] = obj;
        m->transferred_rights[i] = rights;
    }

    if (reply_ep) {
        fe_object_ref(&reply_ep->hdr);
        m->reply_ep = reply_ep;
    }
    *out = m;
    return FE_OK;
}

/* ------------------------------------------------------------------ */
/* 端点                                                                */
/* ------------------------------------------------------------------ */

fe_status_t fe_endpoint_create(struct fe_task *t, u32 flags, fe_handle_t *out)
{
    if (!t || !out) {
        return FE_ERR_INVAL;
    }
    struct fe_endpoint *ep;
    if (g_ep_free_list) {
        ep = g_ep_free_list;
        g_ep_free_list = ep->free_next;
        memset(ep, 0, sizeof(*ep));      /* 复用前必须清干净，队列/等待者字段都要归零 */
    } else {
        ep = (struct fe_endpoint *)fe_kmalloc(sizeof(*ep));
        if (!ep) {
            return FE_ERR_NOMEM;
        }
        memset(ep, 0, sizeof(*ep));
        g_ep_pool_size++;
    }
    fe_object_init(&ep->hdr, FE_OBJ_ENDPOINT);
    ep->flags = flags;
    g_endpoint_count++;

    fe_handle_t h = fe_handle_install(&t->handles, &ep->hdr,
                                      FE_RIGHT_SEND | FE_RIGHT_RECV | FE_RIGHT_DUP |
                                      FE_RIGHT_TRANSFER);
    if (h == FE_HANDLE_INVALID) {
        fe_object_unref(&ep->hdr);
        return FE_ERR_NOMEM;
    }
    /* 句柄已经持有引用，释放创建者的那一份；
     * 否则对象会永远多一个引用，句柄全部关闭也无法销毁（真实踩过的泄漏）。 */
    fe_object_unref(&ep->hdr);
    *out = h;
    return FE_OK;
}

void fe_endpoint_destroy(struct fe_endpoint *ep)
{
    if (!ep) {
        return;
    }
    struct fe_message *m = ep->head;
    while (m) {
        struct fe_message *next = m->next;
        message_free(m);
        m = next;
    }
    g_endpoint_count--;
    if (g_ep_pool_size <= FE_EP_POOL_MAX) {
        ep->free_next = g_ep_free_list;
        g_ep_free_list = ep;
    } else {
        fe_kfree(ep);
        g_ep_pool_size--;
    }
}

/* ★ 丢弃端点队列里的全部积压消息（K5 自检用）★
 *
 * ★ 为什么不给自检直接看 `ep->head` 就够 ★
 * 自检要判"这次投递到底有没有进队"，最干净的判据是"投递前清空、投递后
 * 恰好一条"。而释放消息要 `message_free`，它是本文件的 static ——
 * 所以把"清空"这件事放在**能拿到它**的这一侧，而不是让自检去数差值
 * （数差值在有残留时要写成 `before + 1`，可读性差且容易写错）。
 *
 * 它只丢消息、不动端点的任何其它状态（等待者、计数、引用）。
 * 生产路径上**没有调用者**，是给自检用的。 */
u32 fe_endpoint_drain(struct fe_endpoint *ep)
{
    if (!ep) {
        return 0;
    }
    u32 n = 0;
    u64 irq = fe_irq_save();
    struct fe_message *m = ep->head;
    ep->head = NULL;
    ep->tail = NULL;
    ep->count = 0;
    fe_irq_restore(irq);
    while (m) {
        struct fe_message *next = m->next;
        message_free(m);
        m = next;
        n++;
    }
    return n;
}

fe_status_t fe_endpoint_send(struct fe_task *t, fe_handle_t ep_h,
                             const struct fe_msg_header *hdr,
                             const void *payload, const fe_handle_t *handles)
{
    if (!t || !hdr) {
        return FE_ERR_INVAL;
    }
    struct fe_object_header *obj = NULL;
    fe_status_t s = fe_handle_lookup(&t->handles, ep_h, FE_RIGHT_SEND, &obj);
    if (fe_failed(s)) {
        return s;
    }
    if (obj->type != FE_OBJ_ENDPOINT) {
        return FE_ERR_INVAL;
    }
    struct fe_endpoint *ep = FE_OBJ_OF(obj, struct fe_endpoint);
    return fe_endpoint_send_obj(t, ep, hdr, payload, handles);
}

/* ★ 直接按**对象**发送（K5 用）★
 *
 * ★ 为什么 K5 不能走句柄那条路 ★
 * `fe_endpoint_send` 要 `fe_handle_lookup(..., FE_RIGHT_SEND, ...)`——
 * 于是"处理者能不能收到异常"就变成了"那个句柄此刻有什么权限位"，
 * 而权限位是用户态可以自己收窄的（`HANDLE_DUP` 收窄、`HANDLE_CLOSE` 关掉）。
 * **内核的异常投递不该依赖用户态当前的权限位**：登记那一刻内核已经把
 * "往这里投"这件事记下来了，并对端点加了一次引用；投递时用的是那次登记的
 * 结果，而不是重新去问句柄表。
 *
 * 主体与 `fe_endpoint_send` 共用（下面这一段是唯一的入队实现）：
 * 校验 → 建消息 → 队列满则 AGAIN → 入队 → 唤醒接收者与多对象等待者。 */
fe_status_t fe_endpoint_send_obj(struct fe_task *t, struct fe_endpoint *ep,
                                 const struct fe_msg_header *hdr,
                                 const void *payload, const fe_handle_t *handles)
{
    if (!t || !hdr || !ep) {
        return FE_ERR_INVAL;
    }
    struct fe_message *m = NULL;
    fe_status_t s = build_message(t, &m, hdr, payload, handles, NULL);
    if (fe_failed(s)) {
        return s;
    }

    u64 irq = fe_irq_save();
    if (ep->count >= FE_MSG_QUEUE_MAX) {
        fe_irq_restore(irq);
        message_free(m);
        return FE_ERR_AGAIN;
    }
    endpoint_push(ep, m);
    ep->sent_total++;
    g_messages_sent++;
    fe_irq_restore(irq);

    wake_receiver(ep);
    fe_ipc_wake_object_waiters(ep->waiters);
    return FE_OK;
}

fe_status_t fe_endpoint_recv(struct fe_task *t, fe_handle_t ep_h,
                             struct fe_msg_header *hdr,
                             void *payload, u32 payload_cap,
                             fe_handle_t *out_reply_ep, fe_handle_t *out_transferred)
{
    if (!t || !hdr) {
        return FE_ERR_INVAL;
    }
    struct fe_object_header *obj = NULL;
    fe_status_t s = fe_handle_lookup(&t->handles, ep_h, FE_RIGHT_RECV, &obj);
    if (fe_failed(s)) {
        return s;
    }
    if (obj->type != FE_OBJ_ENDPOINT) {
        return FE_ERR_INVAL;
    }
    struct fe_endpoint *ep = FE_OBJ_OF(obj, struct fe_endpoint);

    /* 无消息则阻塞等待：同步消息传递的核心。
     * 线程被标记为阻塞后走统一的切换路径让出 CPU，被唤醒后回到这里重新检查队列。 */
    for (;;) {
        /* ★ 取消点（K2；2a 起判据换成 fe_thread_should_die）★
         * 任务正在被终止、或**本线程**被单独标记"必须死"时不再等下去。
         * 这个重试循环**本来就必须有**——伪唤醒是允许的，所以每轮都要重查条件。
         * "取消"不是新加的机制，是"重新检查条件"这个既有动作顺带得到的能力。
         * 返回之后线程会走回用户态的路径，而那条路上有闸门（见 sched.c）。
         *
         * ★ 判据必须取"当前线程"，不能拿上面那个任务参数去查 ★
         * 这三处的局部变量 `t` 是**任务**：按任务去问"这个线程要不要死"，
         * 答案是"任务没在终止 ⇒ 不取消"，于是取消点**静默失效**
         * （症状是"有时杀得掉、有时杀不掉"，比崩掉难查得多）。
         * 线程只能从 fe_thread_current() 取。 */
        if (fe_thread_should_die(fe_thread_current())) {
            return FE_ERR_CANCELED;
        }
        u64 irq = fe_irq_save();
        struct fe_message *m = endpoint_pop(ep);
        if (m) {
            ep->recv_total++;
            g_messages_recv++;
            fe_irq_restore(irq);

            *hdr = m->hdr;
            /* 发送方身份由**内核**填写，覆盖发送方自己写的任何值。
             * 服务要按客户端做策略（如"A 槽只读、B 槽连读都不行"），
             * 就必须有一个它伪造不了的身份——放在载荷里是可伪造的，
             * 放在这里不是。 */
            hdr->sender_task = m->sender_task ? m->sender_task->id : 0;
            if (payload && payload_cap && m->hdr.payload_len) {
                u32 n = m->hdr.payload_len < payload_cap ? m->hdr.payload_len : payload_cap;
                memcpy(payload, m->payload, n);
            }
            /* 回复能力随消息投递：接收方拿到一个只能发消息的端点句柄 */
            if (out_reply_ep) {
                *out_reply_ep = FE_HANDLE_INVALID;
                if (m->reply_ep) {
                    *out_reply_ep = fe_handle_install(&t->handles, &m->reply_ep->hdr,
                                                      FE_RIGHT_SEND);
                }
            }
            /* 被传递过来的能力安装到接收方句柄表。
             *
             * ★ 权限 = 发送方权限 - TRANSFER ★（见 build_message 的说明）
             * TRANSFER 这一位必须减掉：不减的话 A 交给 B 之后 B 能无限转发，
             * 一条能力会像病毒一样扩散，而"谁持有这个端点"就再也答不出来了。
             *
             * 安装失败（句柄表满）时该槽位留 FE_HANDLE_INVALID 并**继续**：
             * 消息里的数据仍然有效，为"第 5 个句柄装不下"而丢掉整条消息
             * 只会让调用方看到一个与原因无关的失败。数量由 out_n 如实报告。 */
            if (out_transferred) {
                for (u32 i = 0; i < FE_MSG_MAX_HANDLES; i++) {
                    out_transferred[i] = FE_HANDLE_INVALID;
                    if (m->transferred[i]) {
                        u32 rights = m->transferred_rights[i] & ~FE_RIGHT_TRANSFER;
                        out_transferred[i] = fe_handle_install(&t->handles,
                                                               m->transferred[i], rights);
                    }
                }
            }
            message_free(m);
            return FE_OK;
        }
        /* 队列空：登记为等待者并阻塞 */
        ep->receiver = fe_thread_current();
        fe_irq_restore(irq);
        fe_sched_block_current();
    }
}

/* 回复：向消息携带的回复端点发送。这里直接用回复端点句柄。 */
fe_status_t fe_msg_reply(struct fe_task *t, fe_handle_t reply_ep, const void *payload, u32 len)
{
    if (!t) {
        return FE_ERR_INVAL;
    }
    struct fe_msg_header hdr = {
        .protocol = 0, .opcode = 0, .payload_len = len, .handle_count = 0, .request_id = 0,
    };
    return fe_endpoint_send(t, reply_ep, &hdr, payload, NULL);
}

fe_status_t fe_endpoint_call(struct fe_task *t, fe_handle_t ep_h,
                             const void *req, u32 req_len,
                             void *reply, u32 reply_cap, u32 *reply_len)
{
    if (!t) {
        return FE_ERR_INVAL;
    }
    /* 建临时回复端点，把「回信地址」随请求交给服务端 */
    fe_handle_t reply_h = FE_HANDLE_INVALID;
    fe_status_t s = fe_endpoint_create(t, 0, &reply_h);
    if (fe_failed(s)) {
        return s;
    }
    struct fe_object_header *robj = NULL;
    s = fe_handle_lookup(&t->handles, reply_h, FE_RIGHT_RECV, &robj);
    if (fe_failed(s)) {
        fe_handle_close(&t->handles, reply_h);
        return s;
    }
    struct fe_endpoint *reply_ep = FE_OBJ_OF(robj, struct fe_endpoint);

    struct fe_object_header *eobj = NULL;
    s = fe_handle_lookup(&t->handles, ep_h, FE_RIGHT_SEND, &eobj);
    if (fe_failed(s) || eobj->type != FE_OBJ_ENDPOINT) {
        fe_handle_close(&t->handles, reply_h);
        return fe_failed(s) ? s : FE_ERR_INVAL;
    }
    struct fe_endpoint *ep = FE_OBJ_OF(eobj, struct fe_endpoint);

    struct fe_msg_header hdr = {
        .protocol = 0, .opcode = 0, .payload_len = req_len,
        .handle_count = 0, .request_id = 0,
    };
    struct fe_message *m = NULL;
    s = build_message(t, &m, &hdr, req, NULL, reply_ep);
    if (fe_failed(s)) {
        fe_handle_close(&t->handles, reply_h);
        return s;
    }
    u64 irq = fe_irq_save();
    endpoint_push(ep, m);
    ep->sent_total++;
    g_messages_sent++;
    fe_irq_restore(irq);
    wake_receiver(ep);
    /* wait_any 的登记者也要唤醒（见 fe_ipc_wake_object_waiters 的说明） */
    fe_ipc_wake_object_waiters(ep->waiters);

    struct fe_msg_header rhdr;
    s = fe_endpoint_recv(t, reply_h, &rhdr, reply, reply_cap, NULL, NULL);
    if (reply_len) {
        *reply_len = rhdr.payload_len;
    }
    fe_handle_close(&t->handles, reply_h);
    return s;
}

/* ------------------------------------------------------------------ */
/* 多对象等待（wait_any）                                              */
/* ------------------------------------------------------------------ */

/* ★ 自检钩子（K11 的 W2）：**只给自检用**，生产路径上恒为 NULL ★
 *
 * 它在"检查完、还没登记"的那一刻被调用（`fe_wait_any` 的循环里），
 * 于是自检能把那个丢唤醒窗口**确定性地**造出来。
 * 实现该放哪、为什么放那里，见调用点的注释。 */
static void (*g_wait_hook)(struct fe_task *t, const struct fe_wait_target *tg,
                           u32 n, void *ctx);
static void *g_wait_hook_ctx;

void fe_wait_any_set_hook(void (*fn)(struct fe_task *, const struct fe_wait_target *,
                                     u32, void *), void *ctx)
{
    g_wait_hook = fn;
    g_wait_hook_ctx = ctx;
}

/* 等待节点：一个线程一次等待的**全部信息**。
 * 放在调用者的栈上——它只在这次调用期间有效，而"有效"由 thread->waiting 表达。 */
struct fe_wait_node {
    u32 n;
    u32 _pad;
    struct fe_wait_target tgt[8];
};

/* 把一个线程登记到某个对象的等待表里（调用者必须已关中断）。 */
static void waiters_add(struct fe_thread **table, struct fe_thread *t)
{
    for (u32 i = 0; i < FE_WAITERS_MAX; i++) {
        if (table[i] == t) {
            return;             /* 同一线程等同一对象的两个条件：只登记一次 */
        }
    }
    for (u32 i = 0; i < FE_WAITERS_MAX; i++) {
        if (!table[i]) {
            table[i] = t;
            return;
        }
    }
    /* 表满：不登记，**也不报错**。wait_any 的语义是"任意一个就绪"，
     * 少登记一个目标只是"这个目标这次唤不醒它"，而它与其它目标
     * 共享同一个节点，被别的目标唤醒后照样会重新检查全部目标。 */
}

/* ------------------------------------------------------------------ */
/* ★ K11：地址等待队列（键 = (地址空间, 用户地址)）★                   */
/* ------------------------------------------------------------------ */

/* ★ 为什么是哈希桶 + 侵入式双向链，而不是 `waiters[4]` 那种定长表 ★
 *
 * 内核对象上的等待表是定长的（`FE_WAITERS_MAX = 4`），而且**表满静默不登记**
 * （见上面 waiters_add 的注释）——对"多目标等待"那样做有它的道理。
 * ★ 但对 futex 语义是致命的 ★ `pthread_cond_wait` 的等待者**必须**真的睡下，
 * "没登记就睡"= **永久睡死**，而且是"3 条线程过、5 条线程挂"那种形状。
 * 所以这里给足容量：链无上限，摘除是 O(1)（双向链）。
 *
 * ★ 桶的键为什么必须带地址空间 ★ 微内核里没有共享地址空间（除非将来有
 * fork/CLONE_VM），于是**同一个虚拟地址在两个任务里是两个变量**。
 * 只用地址做键 ⇒ 跨任务串唤醒：A 改自己的变量，却把 B 的等待者叫醒了。
 * 只有键完全对上的才唤醒，这一条由自检 W4 用反向堵死。 */
#define FE_ADDR_BUCKETS 64
static struct fe_thread *g_addr_wait[FE_ADDR_BUCKETS];
static u32 g_addr_wait_count;       /* 诊断：当前一共几个等待者 */

static u32 addr_bucket(const struct fe_address_space *sp, u64 addr)
{
    /* 地址右移 2 位（4 字节对齐，低两位恒 0）、地址空间指针右移 4 位
     * （对象对齐），再异或——够散，且不需要随机数。 */
    u64 h = ((u64)(uptr)sp >> 4) ^ (addr >> 2);
    return (u32)(h % FE_ADDR_BUCKETS);
}

/* 调用者必须已关中断。 */
static void addr_chain_add(struct fe_thread *t, struct fe_address_space *sp, u64 addr)
{
    u32 b = addr_bucket(sp, addr);
    t->addr_space = sp;
    t->addr_key = addr;
    t->addr_prev = NULL;
    t->addr_next = g_addr_wait[b];
    if (g_addr_wait[b]) {
        g_addr_wait[b]->addr_prev = t;
    }
    g_addr_wait[b] = t;
    t->addr_waiting = 1;
    g_addr_wait_count++;
}

/* 调用者必须已关中断。可以重复调（不在链上时什么都不做）。 */
static void addr_chain_del(struct fe_thread *t)
{
    if (!t || !t->addr_waiting) {
        return;
    }
    u32 b = addr_bucket(t->addr_space, t->addr_key);
    if (t->addr_prev) {
        t->addr_prev->addr_next = t->addr_next;
    } else if (g_addr_wait[b] == t) {
        g_addr_wait[b] = t->addr_next;
    } else {
        /* ★ 链头不是它、它又没有前驱 ⇒ 链被破坏了 ★
         * 静默走开会让这条链永远带着一个坏节点（那才是难查的形态），
         * 所以点名之后再线性兜底找一次。 */
        fe_kprintf("[K11] **地址等待链不一致**：线程 %s(id=%llu) 不在桶 %u 头部"
                   "却没有前驱——线性兜底\n",
                   t->name, (unsigned long long)t->id, b);
        for (struct fe_thread *p = g_addr_wait[b]; p; p = p->addr_next) {
            if (p->addr_next == t) {
                p->addr_next = t->addr_next;
                break;
            }
        }
    }
    if (t->addr_next) {
        t->addr_next->addr_prev = t->addr_prev;
    }
    t->addr_next = NULL;
    t->addr_prev = NULL;
    t->addr_waiting = 0;
    t->addr_key = 0;
    t->addr_space = NULL;
    if (g_addr_wait_count) {
        g_addr_wait_count--;
    }
}

u32 fe_wait_addr_waiters(struct fe_address_space *space, u64 addr)
{
    if (!space) {
        return 0;
    }
    u64 irq = fe_irq_save();
    u32 n = 0;
    for (struct fe_thread *t = g_addr_wait[addr_bucket(space, addr)]; t;
         t = t->addr_next) {
        if (t->addr_waiting && t->addr_space == space && t->addr_key == addr) {
            n++;
        }
    }
    fe_irq_restore(irq);
    return n;
}

/* 只给调度器调：到点了，把线程从地址链上摘掉。状态归调用者。 */
void fe_wait_addr_on_deadline(struct fe_thread *t)
{
    if (!t) {
        return;
    }
    u64 irq = fe_irq_save();
    addr_chain_del(t);
    fe_irq_restore(irq);
}

u32 fe_wake_addr(struct fe_address_space *space, u64 addr, u32 count)
{
    if (!space) {
        return 0;
    }
    u64 irq = fe_irq_save();
    u32 woken = 0;
    struct fe_thread *t = g_addr_wait[addr_bucket(space, addr)];
    while (t) {
        struct fe_thread *next = t->addr_next;      /* 提前取：唤醒可能摘链 */
        if (t->addr_waiting && t->addr_space == space && t->addr_key == addr) {
            /* ★ 只在它**真的在睡**时点名 ★
             * 已经被叫醒过（READY/RUNNING）的线程再叫一次会被
             * `fe_sched_wake` 按"状态不对"记一行诊断——那是噪声，
             * 而它本来就在就绪队列里，会自己回去重查。 */
            if (t->state == FE_THREAD_BLOCKED) {
                t->wait_satisfied = 1;      /* ★ wait_satisfied 的第一个读者 ★ */
                fe_sched_wake(t);           /* 顺带摘睡眠链（若带 deadline）*/
                woken++;
                if (count != 0 && woken >= count) {
                    break;
                }
            }
        }
        t = next;
    }
    fe_irq_restore(irq);
    return woken;
}

/* 地址目标的一次判定。**必须在关中断区间里调**（这是原子性的一半）。
 *
 * 返回：
 *   FE_OK        值已经不等于期望 ⇒ 立刻返回（不睡）
 *   FE_ERR_AGAIN 值等于期望 ⇒ 该睡（调用者去登记）
 *   其余：见 fe/ipc.h */
static fe_status_t addr_precheck(struct fe_task *t, const struct fe_wait_target *tg)
{
    if ((tg->addr & 3u) != 0) {
        return FE_ERR_INVAL;
    }
    /* ★ 内核**永不信任用户指针** ★ 未映射的地址必须是**明确的返回码**，
     * 不许被当成"值不相等"——后者会让"地址早就 munmap 了"表现为
     * "条件不满足"，用户态于是永远自旋重试（docs/21 §4.4）。 */
    if (!fe_user_range_ok(t->space, tg->addr, sizeof(u32), false)) {
        return FE_ERR_FAULT;
    }
    u32 val = 0;
    memcpy(&val, (const void *)(uptr)tg->addr, sizeof(val));
    if ((u64)val != (tg->expected & 0xffffffffull)) {
        return FE_OK;                   /* 值已经变了：不睡 */
    }
    if (tg->deadline_ns && fe_time_ns() >= tg->deadline_ns) {
        return FE_ERR_TIMEOUT;          /* 已经到点了 */
    }
    return FE_ERR_AGAIN;
}

/* 由 send / signal 调用：唤醒所有等在这个对象上的线程。
 *
 * ★ 为什么唤醒**全部**而不是挑一个 ★
 * 一个线程等 N 个对象，只有它自己知道是第几项满足了。让唤醒方去猜
 * （读它栈上的节点）会引入"唤醒方持有别人栈指针"的风险。全部唤醒 +
 * 被唤醒者自查，代价是多几次 rq_push，收益是**唤醒方不碰别人的内存**。 */
void fe_ipc_wake_object_waiters(struct fe_thread **table)
{
    for (u32 i = 0; i < FE_WAITERS_MAX; i++) {
        struct fe_thread *w = table[i];
        if (!w) {
            continue;
        }
        /* ★ 必须先查 waiting ★ 线程可能已经退出，指针不再有效。
         * 清除与置位都在关中断区间内完成，所以这里读到的值可判定：
         * waiting 为真 ⇒ 它的节点与等待表都还活着。 */
        if (w->waiting && w->wait_node) {
            w->wait_satisfied = 1;
            fe_sched_wake(w);
        }
        table[i] = NULL;
    }
}

/* 目标"现在就就绪了吗"。就绪时填结果并**消费掉**这次就绪。 */
static bool wait_target_ready(struct fe_task *t, const struct fe_wait_target *tg,
                              struct fe_wait_result *res, u32 cap, u32 index)
{
    struct fe_object_header *obj = NULL;
    if (fe_failed(fe_handle_lookup(&t->handles, tg->handle, 0, &obj))) {
        return false;
    }
    if (tg->kind == FE_WAIT_MSG) {
        if (obj->type != FE_OBJ_ENDPOINT) {
            return false;
        }
        struct fe_endpoint *ep = FE_OBJ_OF(obj, struct fe_endpoint);
        if (ep->count == 0) {
            return false;               /* 没消息：不满足 */
        }
        /* 有消息：**顺带收下来**（见头文件里"为什么顺带收"的说明）。
         * 这里不调 fe_endpoint_recv——那会再走一遍句柄查表，而且它是阻塞语义的。 */
        u64 irq = fe_irq_save();
        struct fe_message *m = endpoint_pop(ep);
        if (!m) {
            fe_irq_restore(irq);
            return false;               /* 竞态：刚被别人取走 */
        }
        ep->recv_total++;
        g_messages_recv++;
        fe_irq_restore(irq);

        res->index = index;
        res->hdr = m->hdr;
        res->hdr.sender_task = m->sender_task ? m->sender_task->id : 0;
        res->reply_ep = FE_HANDLE_INVALID;
        if (m->reply_ep) {
            res->reply_ep = fe_handle_install(&t->handles, &m->reply_ep->hdr,
                                              FE_RIGHT_SEND);
        }
        u32 pn = m->hdr.payload_len;
        if (pn > cap) {
            pn = cap;
        }
        res->payload_cap = pn;
        if (pn) {
            memcpy(res->payload, m->payload, pn);
        }
        message_free(m);
        return true;
    }
    if (tg->kind == FE_WAIT_BITS) {
        if (obj->type != FE_OBJ_NOTIFICATION) {
            return false;
        }
        struct fe_notification *nt = FE_OBJ_OF(obj, struct fe_notification);
        u64 mask = tg->mask ? tg->mask : ~0ull;
        u64 irq = fe_irq_save();
        u64 hit = nt->bits & mask;
        if (hit) {
            nt->bits &= ~hit;       /* 取出即清除：与 notification_wait 一致 */
        }
        fe_irq_restore(irq);
        if (!hit) {
            return false;
        }
        res->index = index;
        res->bits = hit;
        return true;
    }
    return false;
}

/* 从所有对象上摘除自己（被唤醒后、以及出错路径上都要调）。 */
/* ★ 任务要显式传进来，不能用 fe_task_current() ★
 * 因为终止一个任务时，取消方要替**别人**摘除登记（那个线程正阻塞着，
 * 自己走不到这里）。用"当前任务"去查它的句柄只会查错人。 */
static void wait_unregister(struct fe_task *task, struct fe_thread *t,
                            struct fe_wait_node *node, u32 n)
{
    if (!task || !t || !node) {
        return;
    }
    u64 irq = fe_irq_save();
    t->waiting = 0;                 /* 先清：之后唤醒方就不会再碰这些表 */
    for (u32 i = 0; i < n; i++) {
        /* ★ K11：地址键走桶链，不走句柄 ★
         * 一次等待里只会有一种键（见 fe_wait_any 开头那条校验），
         * 所以这一支与下面那一支不会同时为真。 */
        if (node->tgt[i].kind == FE_WAIT_ADDR) {
            addr_chain_del(t);      /* 摘地址链 */
            continue;               /* 睡眠链归调度器（wake 路径会摘）*/
        }
        struct fe_object_header *obj = NULL;
        if (fe_failed(fe_handle_lookup(&task->handles, node->tgt[i].handle, 0, &obj))) {
            continue;
        }
        struct fe_thread **table = NULL;
        if (obj->type == FE_OBJ_ENDPOINT) {
            table = FE_OBJ_OF(obj, struct fe_endpoint)->waiters;
        } else if (obj->type == FE_OBJ_NOTIFICATION) {
            table = FE_OBJ_OF(obj, struct fe_notification)->waiters;
        }
        if (!table) {
            continue;
        }
        for (u32 k = 0; k < FE_WAITERS_MAX; k++) {
            if (table[k] == t) {
                table[k] = NULL;
            }
        }
    }
    t->wait_node = NULL;
    fe_irq_restore(irq);
}

fe_status_t fe_wait_any(struct fe_task *t, const struct fe_wait_target *targets,
                        u32 n, struct fe_wait_result *res, u32 payload_cap)
{
    if (!t || !targets || !res || n == 0 || n > 8) {
        return FE_ERR_INVAL;
    }
    if (payload_cap > FE_MSG_MAX_PAYLOAD) {
        payload_cap = FE_MSG_MAX_PAYLOAD;
    }
    /* ★ K11：地址目标必须**独占**一次等待 ★
     * 它的返回值语义（AGAIN / FAULT / TIMEOUT）与"多目标里任意一个就绪"
     * 不是一回事；混在一起就要发明一套混合语义，而今天**没有任何调用者
     * 需要它**（`pthread_cond_wait` 只需要一个地址）。说清楚比含糊地
     * "支持了但语义不清"好。 */
    for (u32 i = 0; i < n; i++) {
        if (targets[i].kind == FE_WAIT_ADDR && n != 1) {
            return FE_ERR_INVAL;
        }
    }

    struct fe_wait_node node;
    node.n = n;
    for (u32 i = 0; i < n; i++) {
        node.tgt[i] = targets[i];
    }
    struct fe_thread *self = fe_thread_current();
    if (!self) {
        return FE_ERR_INVAL;
    }
    /* ★ "我这一轮睡过没有" —— 它决定"看到值已经变了"该回哪个码 ★
     * 进门第一次就发现值不等于期望 ⇒ `EAGAIN`（futex 的语义：你给我的
     * 那个期望值已经不对了，**我没睡**）；被唤醒之后重查才发现 ⇒ `OK`。
     * 两者对调用者都意味着"回去重查条件"，但对**排查**来说差别很大：
     * 前者说明"我根本没参与等待"，后者说明"有人叫了我"。 */
    bool slept_this_wait = false;

    for (;;) {
        /* 取消点（K2；2a 起判据换成 fe_thread_should_die）：任务正在被终止、
         * 或**本线程**被单独标记"必须死"时不再等下去。
         * 见 fe_endpoint_recv 里那段说明（局部 `t` 是任务，判据必须取当前线程）。 */
        if (fe_thread_should_die(self)) {
            return FE_ERR_CANCELED;
        }

        /* ★★ 自检钩子（K11 的 W2；**只给自检用**，生产路径上恒为 NULL）★★
         *
         * ★ 它为什么钉在**这一个位置** ★
         * 它就是"原子区间之前的那一刻"：钩子在这里把条件置真并调一次唤醒，
         * 等价于"另一个执行体在我要睡下的前一瞬完成了唤醒"。
         * 区间**没有**包住检查的年代（K11 之前），这一下会漏；
         * 现在检查在区间里，这一下会被区间内的重查抓到。
         * 所以同一个钩子既是红测试（修之前红）又是回归判据（修之后绿）。 */
        if (g_wait_hook) {
            g_wait_hook(t, node.tgt, n, g_wait_hook_ctx);
        }

        /* ★★ K11 §3.4：区间从**检查之前**开始 ★★
         *
         * 原先"先看有没有已经就绪的"在区间**外**（它与区间之间夹着
         * `wait_target_ready` 的全部工作、`FE_WAIT_MSG` 分支里甚至有一次
         * 最多 1024 字节的 `memcpy`）。落在那段里的"条件变真 + 唤醒"会丢，
         * 而本函数上面那句注释逐字写着这条不变式——**代码与注释不一致**，
         * 这就是 W2 修之前红的原因（`build/k11-w2-red.txt` 是它的原文）。
         *
         * 现在"检查 → 登记 → 置 BLOCKED"全在同一个区间里，
         * 于是唤醒方只有两种可能：区间**之前**来（那时条件已经变了，
         * 区间内的重查会看见）或者区间**之后**来（那时登记已经在表上，
         * 唤醒方找得到我们）。**中间那一种不存在了。** */
        u64 flags = fe_irq_save();

        if (node.tgt[0].kind == FE_WAIT_ADDR) {
            /* 地址目标：它决定"睡不睡"，而不是"就绪没就绪" */
            fe_status_t a = addr_precheck(t, &node.tgt[0]);
            if (a == FE_OK) {
                fe_irq_restore(flags);
                return slept_this_wait ? FE_OK : FE_ERR_AGAIN;
            }
            if (a != FE_ERR_AGAIN) {
                fe_irq_restore(flags);
                return a;               /* FAULT / TIMEOUT / INVAL */
            }
        } else {
            /* 1. 先看有没有"已经就绪"的：有就立刻返回，不阻塞。
             *    这一遍必须在登记**之前**——反过来的话，一个已经就绪的对象
             *    会先因登记而唤醒我们，然后我们再查一次，白绕一圈。 */
            memset(res, 0, sizeof(*res));
            res->reply_ep = FE_HANDLE_INVALID;
            bool ready = false;
            for (u32 i = 0; i < n; i++) {
                if (wait_target_ready(t, &node.tgt[i], res, payload_cap, i)) {
                    ready = true;
                    break;
                }
            }
            if (ready) {
                fe_irq_restore(flags);
                return FE_OK;
            }
        }

        /* 2. 都不就绪：登记到每个对象上（地址则登记进桶链），然后阻塞。
         *
         * 顺序也有讲究：**先把指针写进对象、最后置 waiting**。
         * 唤醒方查 waiting 为真才碰指针，所以只要 waiting 是最后置的，
         * 就不存在"指针已写入但唤醒方看不到"的窗口；
         * 反过来会出现"唤醒方以为指针有效、实际还没写"的窗口。 */
        for (u32 i = 0; i < n; i++) {
            if (node.tgt[i].kind == FE_WAIT_ADDR) {
                addr_chain_add(self, t->space, node.tgt[i].addr);
                /* ★ 顺手挂睡眠链（deadline == 0 时它什么都不做）★
                 * 两条链、一个状态：见 docs/21 §5.3 与 fe_sched_sleep_arm。 */
                fe_sched_sleep_arm(node.tgt[i].deadline_ns);
                continue;
            }
            struct fe_object_header *obj = NULL;
            if (fe_failed(fe_handle_lookup(&t->handles, node.tgt[i].handle, 0, &obj))) {
                fe_irq_restore(flags);
                wait_unregister(t, self, &node, n);
                return FE_ERR_BADHANDLE;
            }
            if (node.tgt[i].kind == FE_WAIT_MSG && obj->type == FE_OBJ_ENDPOINT) {
                waiters_add(FE_OBJ_OF(obj, struct fe_endpoint)->waiters, self);
            } else if (node.tgt[i].kind == FE_WAIT_BITS &&
                       obj->type == FE_OBJ_NOTIFICATION) {
                waiters_add(FE_OBJ_OF(obj, struct fe_notification)->waiters, self);
            } else {
                fe_irq_restore(flags);
                wait_unregister(t, self, &node, n);
                return FE_ERR_INVAL;    /* kind 与对象类型不匹配 */
            }
        }
        self->wait_node = &node;
        self->wait_satisfied = 0;
        self->wait_timed_out = 0;
        self->waiting = 1;
        /* ★★ K11 §3.4 ②：置 BLOCKED 也在这个区间里 ★★
         * 于是"已登记"与"能被唤醒"**同时**成立——原先把这一句留给
         * 区间之外的 `fe_sched_block_current()`，中间那几微秒里到达的唤醒
         * 会被 `fe_sched_wake` 按"状态不对"丢弃（它只接受 BLOCKED/SLEEPING），
         * 而本函数随后又把线程压回 BLOCKED ⇒ 那次唤醒白叫了。 */
        self->state = FE_THREAD_BLOCKED;
        slept_this_wait = true;         /* 这一轮之后"值变了"就不再是 EAGAIN */
        fe_irq_restore(flags);

        /* ★ §3.4 ③：`fe_sched_block_current` 现在是幂等的 ★
         * 唤醒若抢在前面把它置成 READY，这里**不会**再压回 BLOCKED。 */
        fe_sched_block_current();

        wait_unregister(t, fe_thread_current(), &node, n);

        if (self->wait_timed_out) {
            self->wait_timed_out = 0;
            return FE_ERR_TIMEOUT;
        }
        if (node.tgt[0].kind == FE_WAIT_ADDR) {
            /* ★ futex 契约：被唤醒就回"有人叫了你"，**由调用者重查** ★
             * 这里刻意**不**重查再判 AGAIN：被 broadcast 波及的等待者
             * 醒来时值可能还没变，而标准要求它醒来后自己重查条件——
             * 把"没变"翻译成失败会把正常的 broadcast 变成错误。 */
            if (self->wait_satisfied) {
                return FE_OK;
            }
            continue;               /* 既没超时也没被满足：回去重查 */
        }
        /* 对象目标：回到循环顶部**重新检查全部目标**。
         * 不假设"唤醒我的就是就绪的那个"——唤醒方只知道"这个对象动了"，
         * 而通知的位可能已经被同一任务的另一个线程取走。
         * 重新检查是关键，它让"伪唤醒"自动变成正确行为。 */
    }
}

/* ★ K11 的对外入口：等一个用户地址 ★ */
fe_status_t fe_wait_addr(struct fe_task *t, u64 addr, u64 expected, u64 deadline_ns)
{
    if (!t) {
        return FE_ERR_INVAL;
    }
    struct fe_wait_target tg;
    memset(&tg, 0, sizeof(tg));
    tg.kind = FE_WAIT_ADDR;
    tg.addr = addr;
    tg.expected = expected;
    tg.deadline_ns = deadline_ns;
    struct fe_wait_result res;
    return fe_wait_any(t, &tg, 1, &res, 0);
}

/* ------------------------------------------------------------------ */
/* 取消一个正阻塞着的线程（K2 的取消点机制那一半）                     */
/* ------------------------------------------------------------------ */

/* 把一个线程从它正在等的东西上摘下来，然后唤醒它。
 *
 * ★ 为什么"摘下来"这步不能省 ★
 * 线程阻塞在内核里时，它的指针留在**对象的等待表**上（或端点的 receiver
 * 槽里）。如果直接把它标记为死亡，那些登记就成了悬空指针：
 * 唤醒方（fe_ipc_wake_object_waiters）会去解引用它们，
 * 而僵尸被回收之后那就是一次 use-after-free。
 * 所以顺序是死的：**先摘除登记，再唤醒**。
 *
 * 醒来之后它回到重试循环顶部，那里有取消点，于是它带着 FE_ERR_CANCELED
 * 返回用户态——途中经过 fe_sched_maybe_switch 的闸门，在那里死掉。
 *
 * ★ 今天只有两类等待登记，所以只有两条摘除路径 ★
 *   1. wait_any 的节点：线程身上挂着 wait_node，节点里有它等过的那些句柄；
 *   2. fe_endpoint_recv：端点的 receiver 单槽。
 * 第二条靠遍历**该任务句柄表里的端点**来定位：正在收消息的线程必然持有
 * 那个端点的句柄，所以"谁在等"可以从句柄表这一侧回答，不需要给端点加链。
 *
 * ★ 上面那两句在 D1 落地时被实测推翻了：还有第三条——通知的 waiter 单槽 ★
 * 同一个句柄表扫描就能覆盖它（正在等这个通知的线程必然持有它的句柄，
 * 与端点那条**完全对称**），所以两条路径变成三条、扫描只有一趟。
 * 留这段是因为"漏加一条取消路径"的症状值得记住：**那个线程杀不掉**，
 * 它会一直阻塞下去，于是任务永远不消失；而更隐蔽的是**不崩**——
 * 线程带着 CANCELED 走到闸门、死、僵尸被回收，可对象里的槽还指着它，
 * 下次 signal 就把已释放的内存当线程去唤醒（use-after-free）。
 *
 * ★ 将来若有第四种等待登记，必须同时加一条取消路径 ★
 * 这一条写在 docs/13-tasks-and-kill.md §3.5 的边界里。 */
void fe_thread_cancel(struct fe_task *task, struct fe_thread *victim)
{
    if (!task || !victim) {
        return;
    }
    u64 flags = fe_irq_save();

    /* 路径 1：wait_any 的登记 */
    if (victim->waiting && victim->wait_node) {
        struct fe_wait_node *node = (struct fe_wait_node *)victim->wait_node;
        wait_unregister(task, victim, node, node->n);
    }
    /* 路径 2 / 3：端点接收槽与通知等待槽。★ 必须在关中断区间里做 ★
     * 否则"找到它、清掉它"之间对方可能刚好收到消息并被唤醒，
     * 于是我们清掉的是一个已经不在等的线程的登记（无害），
     * 或者更糟：漏清一个仍然在等的登记（有害）。 */
    for (u32 i = 0; i < FE_HANDLE_TABLE_SIZE; i++) {
        struct fe_object_header *obj = NULL;
        fe_handle_t h = (fe_handle_t)(i + 1);
        if (fe_failed(fe_handle_lookup(&task->handles, h, 0, &obj))) {
            continue;
        }
        if (!obj) {
            continue;
        }
        if (obj->type == FE_OBJ_ENDPOINT) {
            struct fe_endpoint *ep = FE_OBJ_OF(obj, struct fe_endpoint);
            if (ep->receiver == victim) {
                ep->receiver = NULL;
            }
        } else if (obj->type == FE_OBJ_NOTIFICATION) {
            /* ★ D1：通知的单槽等待登记（`fe_notification_wait` 的兼容路径）★
             *
             * 不摘它的后果分两步显形，而且第二步才是真正危险的那一步：
             *   1. 受害者返回 CANCELED → 经过闸门 → 死 → 僵尸被回收
             *      （`reap_ok=true`，见 user.c:543-545）；
             *   2. 槽里那根指针**还指着已经释放的内存**，下次
             *      `fe_notification_signal_obj` 命中掩码时会拿它去
             *      `fe_sched_wake`（读 `t->state`）——use-after-free。
             * `wait_mask` 一起清：留着它等于给"下一次 signal"留一份
             * 与被取消的等待者有关的陈旧条件。 */
            struct fe_notification *nt = FE_OBJ_OF(obj, struct fe_notification);
            if (nt->waiter == victim) {
                nt->waiter = NULL;
                nt->wait_mask = 0;
            }
        }
    }
    /* 路径 4：共享区间控制器锁上的等待登记（D2① 的另一半）。
     *
     * ★ 为什么它不在上面那趟句柄表扫描里 ★
     * 锁不是句柄对象——它挂在**资源池条目**上（`resource.c` 的 g_pool，
     * 对 ipc.c 不可见）。所以这一条走 `fe_resource_forget_thread_locks`，
     * 由资源池自己去扫它的条目。放在这里（同一个关中断区间里、唤醒之前）
     * 是为了让"摘登记"这一步与另外三条一样是**先做后唤醒**。
     *
     * ★ 不清它的后果（实测，见 D2① 的第二个正向断言）★
     * `e->lock_waiter` 留着已经死掉/即将被回收的线程 ⇒
     *   1. 真正该被唤醒的后来者永远拿不到锁（解锁唤醒的是这个陈旧登记）；
     *   2. 僵尸回收之后 `fe_sched_wake` 读的是已释放内存。 */
    fe_resource_forget_thread_locks(victim);
    /* 路径 5：目标任务上的 `waiter` 槽（`fe_process_wait` 的登记）。
     *
     * ★ 与路径 3（通知的 waiter）完全同类，也是"只在正常唤醒时才清"★
     * `child->waiter` 只在**孩子退出**时被清（`fe_process_on_thread_exit`）。
     * 线程被取消时没人清它 ⇒ 线程返回 CANCELED、走闸门、死、僵尸被回收
     * ⇒ 槽里是一根指向已释放内存的指针，下次孩子退出时
     * `fe_process_on_thread_exit` 会拿它去 `fe_sched_wake`（读 `t->state`）
     * ——use-after-free。
     * 实测（本次修复前）：`D2②` 段落后那条 `CHECK(child->waiter == NULL)`
     * 是**唯一**还红着的一项（`进程终止失败项: 1`）。
     *
     * ★ 判据是线程指针，不是"槽非空"★ 见 fe_task_clear_waiter 的说明。 */
    fe_task_clear_waiter(victim);
    fe_irq_restore(flags);

    /* 唤醒放在关中断区间之外：fe_sched_wake 自己会关中断，
     * 而它同时要求状态是 BLOCKED（已是其它状态时它是安全的空操作）。 */
    fe_sched_wake(victim);
}

/* ---- 只读访问器：通知对象当前的等待者 ----
 *
 * ★ 为什么需要它 ★ D1 的判据（"通知对象里不残留指向已死线程的 waiter"）
 * 必须读一个**可读的事实**，而不是"跑起来没崩"（后者什么都证明不了）。
 * 结构体虽然是公开的（fe/ipc.h），但把"谁在等这个通知"收在一处，
 * 就不必让每个读者自己决定该看 `waiter` 还是 `waiters[]`。只看不写。 */
struct fe_thread *fe_notification_waiter(const struct fe_notification *nt)
{
    return nt ? nt->waiter : NULL;
}

/* ------------------------------------------------------------------ */
/* 通知                                                                */
/* ------------------------------------------------------------------ */

fe_status_t fe_notification_create(struct fe_task *t, fe_handle_t *out)
{
    if (!t || !out) {
        return FE_ERR_INVAL;
    }
    struct fe_notification *nt = (struct fe_notification *)fe_kzalloc(sizeof(*nt));
    if (!nt) {
        return FE_ERR_NOMEM;
    }
    fe_object_init(&nt->hdr, FE_OBJ_NOTIFICATION);
    g_notification_count++;
    fe_handle_t h = fe_handle_install(&t->handles, &nt->hdr,
                                      FE_RIGHT_SIGNAL | FE_RIGHT_WAIT |
                                      FE_RIGHT_DUP | FE_RIGHT_TRANSFER);
    if (h == FE_HANDLE_INVALID) {
        fe_object_unref(&nt->hdr);
        return FE_ERR_NOMEM;
    }
    fe_object_unref(&nt->hdr);      /* 释放创建者引用，见端点创建处的说明 */
    *out = h;
    return FE_OK;
}

void fe_notification_destroy(struct fe_notification *nt)
{
    if (!nt) {
        return;
    }
    g_notification_count--;
    fe_kfree(nt);
}

fe_status_t fe_notification_signal(fe_handle_t nt_h, u64 bits)
{
    struct fe_task *t = fe_task_current();
    struct fe_object_header *obj = NULL;
    fe_status_t s = fe_handle_lookup(&t->handles, nt_h, FE_RIGHT_SIGNAL, &obj);
    if (fe_failed(s)) {
        return s;
    }
    if (obj->type != FE_OBJ_NOTIFICATION) {
        return FE_ERR_INVAL;
    }
    return fe_notification_signal_obj(FE_OBJ_OF(obj, struct fe_notification), bits);
}

fe_status_t fe_notification_signal_obj(struct fe_notification *nt, u64 bits)
{
    if (!nt) {
        return FE_ERR_INVAL;
    }

    u64 irq = fe_irq_save();
    nt->bits |= bits;
    nt->signal_total++;
    bool wake = false;
    struct fe_thread *w = nt->waiter;
    if (w && (nt->bits & nt->wait_mask)) {
        nt->waiter = NULL;
        wake = true;
    }
    /* 除了单等待者路径（waiter），还要唤醒 wait_any 的登记者：
     * 它们等的是"这个通知上有没有位被置"，与 wait_mask 无关。 */
    bool any_ready = (nt->bits != 0);
    fe_irq_restore(irq);
    if (wake) {
        fe_sched_wake(w);
    }
    if (any_ready) {
        fe_ipc_wake_object_waiters(nt->waiters);
    }
    return FE_OK;
}

fe_status_t fe_notification_wait(fe_handle_t nt_h, u64 mask, u64 *out_bits)
{
    struct fe_task *t = fe_task_current();
    struct fe_object_header *obj = NULL;
    fe_status_t s = fe_handle_lookup(&t->handles, nt_h, FE_RIGHT_WAIT, &obj);
    if (fe_failed(s)) {
        return s;
    }
    if (obj->type != FE_OBJ_NOTIFICATION) {
        return FE_ERR_INVAL;
    }
    struct fe_notification *nt = FE_OBJ_OF(obj, struct fe_notification);
    if (mask == 0) {
        mask = ~0ull;
    }

    for (;;) {
        /* 取消点（K2；2a 起判据换成 fe_thread_should_die）：任务正在被终止、
         * 或**本线程**被单独标记"必须死"时不再等下去。
         * 见 fe_endpoint_recv 里那段说明（局部 `t` 是任务，判据必须取当前线程）。 */
        if (fe_thread_should_die(fe_thread_current())) {
            return FE_ERR_CANCELED;
        }
        u64 irq = fe_irq_save();
        u64 hit = nt->bits & mask;
        if (hit) {
            nt->bits &= ~hit;       /* 取出即清除 */
            fe_irq_restore(irq);
            if (out_bits) {
                *out_bits = hit;
            }
            return FE_OK;
        }
        nt->waiter = fe_thread_current();
        nt->wait_mask = mask;
        fe_irq_restore(irq);
        fe_sched_block_current();
    }
}

/* ------------------------------------------------------------------ */
/* 内存对象                                                            */
/* ------------------------------------------------------------------ */

fe_status_t fe_memory_create(struct fe_task *t, u64 size, u32 flags, fe_handle_t *out)
{
    if (!t || !out || size == 0) {
        return FE_ERR_INVAL;
    }
    u64 pages = (size + FE_FRAME_SIZE - 1) / FE_FRAME_SIZE;
    struct fe_memory_object *mo = (struct fe_memory_object *)fe_kzalloc(sizeof(*mo));
    if (!mo) {
        return FE_ERR_NOMEM;
    }
    mo->frames = (phys_addr_t *)fe_kmalloc(sizeof(phys_addr_t) * pages);
    if (!mo->frames) {
        fe_kfree(mo);
        return FE_ERR_NOMEM;
    }
    fe_object_init(&mo->hdr, FE_OBJ_MEMORY);
    mo->page_count = pages;
    mo->size = pages * FE_FRAME_SIZE;
    mo->flags = flags;

    if (flags & FE_MEM_FLAG_DMA) {
        /* DMA 要求物理连续：设备只认物理地址，散页得靠 IOMMU 才能拼起来。
         * 这里一次要整段，失败就是失败，不做退化成散页——退化了驱动也用不了。 */
        phys_addr_t base = fe_pmm_alloc_frames(pages);
        if (base == 0) {
            fe_kfree(mo->frames);
            fe_kfree(mo);
            return FE_ERR_NOMEM;
        }
        for (u64 i = 0; i < pages; i++) {
            mo->frames[i] = base + i * FE_FRAME_SIZE;
        }
    } else {
        for (u64 i = 0; i < pages; i++) {
            phys_addr_t f = fe_pmm_alloc_frame();
            if (f == 0) {
                for (u64 k = 0; k < i; k++) {
                    fe_pmm_free_frame(mo->frames[k]);
                }
                fe_kfree(mo->frames);
                fe_kfree(mo);
                return FE_ERR_NOMEM;
            }
            mo->frames[i] = f;
        }
    }

    /* 清零：物理帧是从全局池里来的，不清零就意味着新任务能读到上一个任务
     * 释放的内存内容。这是经典的信息泄漏，必须在分配时堵死而不是靠调用方自觉。 */
    for (u64 i = 0; i < pages; i++) {
        memset((void *)(uptr)(mo->frames[i] + fe_vmm_hhdm_offset()), 0, FE_FRAME_SIZE);
    }
    g_memory_count++;

    fe_handle_t h = fe_handle_install(&t->handles, &mo->hdr,
                                      FE_RIGHT_READ | FE_RIGHT_WRITE |
                                      FE_RIGHT_DUP | FE_RIGHT_TRANSFER);
    if (h == FE_HANDLE_INVALID) {
        fe_object_unref(&mo->hdr);
        return FE_ERR_NOMEM;
    }
    fe_object_unref(&mo->hdr);      /* 释放创建者引用，见端点创建处的说明 */
    *out = h;
    return FE_OK;
}

void fe_memory_destroy(struct fe_memory_object *mo)
{
    if (!mo) {
        return;
    }
    if ((mo->flags & FE_MEM_FLAG_DMA) && mo->page_count > 0) {
        fe_pmm_free_frames(mo->frames[0], mo->page_count);
    } else {
        for (u64 i = 0; i < mo->page_count; i++) {
            fe_pmm_free_frame(mo->frames[i]);
        }
    }
    fe_kfree(mo->frames);
    g_memory_count--;
    fe_kfree(mo);
}

fe_status_t fe_memory_map_kernel(struct fe_task *t, fe_handle_t mo_h, u32 prot,
                                 void **out_virt)
{
    if (!t || !out_virt) {
        return FE_ERR_INVAL;
    }
    struct fe_object_header *obj = NULL;
    fe_status_t s = fe_handle_lookup(&t->handles, mo_h,
                                     (prot & FE_PROT_WRITE) ? FE_RIGHT_WRITE : FE_RIGHT_READ,
                                     &obj);
    if (fe_failed(s)) {
        return s;
    }
    if (obj->type != FE_OBJ_MEMORY) {
        return FE_ERR_INVAL;
    }
    struct fe_memory_object *mo = FE_OBJ_OF(obj, struct fe_memory_object);

    virt_addr_t base = FE_FRAME_ALIGN_UP(g_objmap_next);
    if (base + mo->size > FE_OBJMAP_END) {
        return FE_ERR_NOSPC;
    }
    u64 flags = FE_PTE_GLOBAL;
    if (prot & FE_PROT_WRITE) {
        flags |= FE_PTE_WRITE;
    }
    for (u64 i = 0; i < mo->page_count; i++) {
        s = fe_vmm_map(fe_vmm_kernel_space(), base + i * FE_FRAME_SIZE,
                       mo->frames[i], FE_FRAME_SIZE, flags);
        if (fe_failed(s)) {
            return s;
        }
    }
    g_objmap_next = base + mo->size;
    *out_virt = (void *)(uptr)base;
    return FE_OK;
}

/* 映射到**用户**任务地址空间。用户半区的页必须是 USER 可访问的，
 * 并按请求的读写权限设置；不可执行的映射一律加 NX。 */
fe_status_t fe_memory_map_user(struct fe_task *t, fe_handle_t mo_h, u64 hint, u64 size,
                               u32 prot, void **out_virt)
{
    if (!t || !t->space || !out_virt) {
        return FE_ERR_INVAL;
    }
    struct fe_object_header *obj = NULL;
    fe_status_t s = fe_handle_lookup(&t->handles, mo_h,
                                     (prot & FE_PROT_WRITE) ? FE_RIGHT_WRITE : FE_RIGHT_READ,
                                     &obj);
    if (fe_failed(s)) {
        return s;
    }
    if (obj->type != FE_OBJ_MEMORY) {
        return FE_ERR_INVAL;
    }
    struct fe_memory_object *mo = FE_OBJ_OF(obj, struct fe_memory_object);

    u64 map_size = size ? FE_FRAME_ALIGN_UP(size) : mo->size;
    if (map_size > mo->size) {
        return FE_ERR_INVAL;        /* 不允许映射超出对象范围 */
    }

    virt_addr_t base;
    if (hint) {
        base = FE_FRAME_ALIGN_DOWN(hint);
        if (base < FE_USER_MMAP_BASE || base + map_size > FE_USER_MMAP_END) {
            return FE_ERR_INVAL;
        }
    } else {
        base = FE_FRAME_ALIGN_UP(t->user_map_next);
        if (base < FE_USER_MMAP_BASE || base + map_size > FE_USER_MMAP_END) {
            return FE_ERR_NOSPC;
        }
        t->user_map_next = base + map_size;
    }

    u64 flags = FE_PTE_USER | FE_PTE_NX;
    if (prot & FE_PROT_WRITE) {
        flags |= FE_PTE_WRITE;
    }
    if (prot & FE_PROT_EXEC) {
        flags &= ~FE_PTE_NX;
    }
    /* ★ 这些帧归**内存对象**所有，不归调用者的地址空间 ★
     * 见 FE_PTE_NOFREE 的说明：不置这一位，任务销毁时会把对象的内存
     * 还给 PMM——而对象可能还活着、可能还有别的任务在映射它。 */
    flags |= FE_PTE_NOFREE;

    /* 失败要回滚已映射的部分。
     *
     * ★ 不回滚会留下半截映射 ★：调用者拿到一个错误码，却已经有一块地址
     * 被映射进了它的地址空间——它不知道那块地址在哪（返回值为负时没人去看
     * 输出参数），于是既用不了也收不回。而"部分成功"在这里没有任何用处：
     * 映射 1 MiB 只成功前 600 KiB 对调用方等于失败。 */
    u64 done = 0;
    for (; done < map_size; done += FE_FRAME_SIZE) {
        s = fe_vmm_map(t->space, base + done, mo->frames[done / FE_FRAME_SIZE],
                       FE_FRAME_SIZE, flags);
        if (fe_failed(s)) {
            if (done > 0) {
                /* 用 unmap（不还帧）：帧归内存对象所有，不是这次映射分配的 */
                fe_vmm_unmap(t->space, base, done);
            }
            return s;
        }
    }
    *out_virt = (void *)(uptr)base;
    return FE_OK;
}

fe_status_t fe_memory_unmap_kernel(void *virt, u64 size)
{
    if (!virt || size == 0) {
        return FE_ERR_INVAL;
    }
    return fe_vmm_unmap(fe_vmm_kernel_space(), (virt_addr_t)(uptr)virt,
                        FE_FRAME_ALIGN_UP(size));
}

/* ------------------------------------------------------------------ */

void fe_ipc_get_stats(struct fe_ipc_stats *out)
{
    memset(out, 0, sizeof(*out));
    out->endpoints = g_endpoint_count;
    out->notifications = g_notification_count;
    out->memory_objects = g_memory_count;
    out->messages_sent = g_messages_sent;
    out->messages_recv = g_messages_recv;
    out->messages_allocated = g_messages_allocated;
    out->messages_freed = g_messages_freed;
}
