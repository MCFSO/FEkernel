/* SPDX-License-Identifier: 0BSD */
/* M3 自检：对象/句柄/IPC。
 *
 * 每项都针对一个具体机制，并且要求「用完必须回到基线」——对象泄漏和句柄泄漏
 * 都会让计数对不上。
 */
#include <fe/ipc.h>
#include <fe/task.h>
#include <fe/object.h>
#include <fe/sched/sched.h>
#include <fe/sched/thread.h>
#include <fe/mm.h>
#include <fe/mm/kheap.h>
#include <fe/mm/vmm.h>
#include <fe/kprintf.h>
#include <fe/string.h>
#include <fe/status.h>
#include <fe/io.h>              /* 读对象状态时短暂关中断 */

#define RPC_ROUNDS 64

static fe_handle_t g_srv_ep;
static volatile u32 g_rpc_done;
static volatile u32 g_rpc_errors;
static volatile u64 g_rpc_checksum;

/* ------------------------------------------------------------------ */
/* 服务线程：收请求 → 变换 → 回复                                       */
/* ------------------------------------------------------------------ */

static void rpc_server_entry(void *arg)
{
    (void)arg;
    struct fe_task *t = fe_task_kernel();
    for (u32 i = 0; i < RPC_ROUNDS; i++) {
        struct fe_msg_header hdr;
        u8 buf[64];
        fe_handle_t reply_ep = FE_HANDLE_INVALID;

        fe_status_t s = fe_endpoint_recv(t, g_srv_ep, &hdr, buf, sizeof(buf),
                                         &reply_ep, NULL);
        if (fe_failed(s) || reply_ep == FE_HANDLE_INVALID) {
            g_rpc_errors++;
            break;
        }
        /* 可校验的变换：每个字节加轮次，客户端可独立算出期望值 */
        for (u32 k = 0; k < hdr.payload_len; k++) {
            buf[k] = (u8)(buf[k] + (u8)i);
        }
        fe_msg_reply(t, reply_ep, buf, hdr.payload_len);
        fe_handle_close(&t->handles, reply_ep);   /* 回复能力用完即关 */
    }
    fe_thread_exit(0);
}

/* ------------------------------------------------------------------ */

static u32 test_basic_sendrecv(void)
{
    u32 fail = 0;
    struct fe_task *t = fe_task_kernel();

    fe_handle_t ep = FE_HANDLE_INVALID;
    if (fe_failed(fe_endpoint_create(t, 0, &ep))) {
        fe_kprintf("        端点创建失败\n");
        return 1;
    }

    const char *text = "FEKernel IPC";
    struct fe_msg_header sh = {
        .protocol = 0xFE01, .opcode = 7, .payload_len = (u32)strlen(text) + 1,
        .handle_count = 0, .request_id = 0x1234,
    };
    if (fe_failed(fe_endpoint_send(t, ep, &sh, text, NULL))) {
        fe_kprintf("        发送失败\n");
        fail++;
    }

    struct fe_msg_header rh;
    char buf[64];
    if (fe_failed(fe_endpoint_recv(t, ep, &rh, buf, sizeof(buf), NULL, NULL))) {
        fe_kprintf("        接收失败\n");
        fail++;
    } else {
        if (rh.protocol != 0xFE01 || rh.opcode != 7 || rh.request_id != 0x1234) {
            fe_kprintf("        消息头不一致\n");
            fail++;
        }
        if (strcmp(buf, text) != 0) {
            fe_kprintf("        载荷不一致: '%s'\n", buf);
            fail++;
        }
    }
    fe_handle_close(&t->handles, ep);
    fe_kprintf("        单次收发（含消息头字段与载荷）: %s\n", fail ? "失败" : "OK");
    return fail;
}

static u32 test_handle_rights(void)
{
    u32 fail = 0;
    struct fe_task *t = fe_task_kernel();
    struct fe_ipc_stats st0, st1;

    fe_ipc_get_stats(&st0);

    fe_handle_t ep = FE_HANDLE_INVALID;
    fe_endpoint_create(t, 0, &ep);

    /* 复制一个「只能发不能收」的句柄 */
    fe_handle_t send_only = FE_HANDLE_INVALID;
    fe_status_t s = fe_handle_dup(&t->handles, ep, FE_RIGHT_SEND, &send_only);
    if (fe_failed(s)) {
        fe_kprintf("        句柄复制失败\n");
        fe_handle_close(&t->handles, ep);
        return 1;
    }

    /* 用只发句柄去收：必须被拒绝 */
    struct fe_msg_header h;
    char b[8];
    s = fe_endpoint_recv(t, send_only, &h, b, sizeof(b), NULL, NULL);
    if (s != FE_ERR_ACCESS) {
        fe_kprintf("        只发句柄竟然可以接收（返回 %d）\n", s);
        fail++;
    } else {
        fe_kprintf("        只发句柄执行接收被拒绝: %s\n", fe_status_name(s));
    }

    /* 权限放大必须被拒绝：试图复制出一个权限更大的句柄 */
    fe_handle_t bigger = FE_HANDLE_INVALID;
    s = fe_handle_dup(&t->handles, send_only, FE_RIGHT_SEND | FE_RIGHT_WRITE, &bigger);
    if (s != FE_ERR_ACCESS) {
        fe_kprintf("        权限放大竟然成功（返回 %d）\n", s);
        fail++;
    } else {
        fe_kprintf("        权限放大被拒绝: %s\n", fe_status_name(s));
    }

    /* 无效句柄 */
    s = fe_handle_lookup(&t->handles, 9999, 0, NULL);
    if (s != FE_ERR_BADHANDLE) {
        fe_kprintf("        越界句柄未被拒绝（返回 %d）\n", s);
        fail++;
    }

    fe_handle_close(&t->handles, send_only);
    fe_handle_close(&t->handles, ep);

    fe_ipc_get_stats(&st1);
    if (st1.endpoints != st0.endpoints) {
        fe_kprintf("        端点对象泄漏: %llu -> %llu\n",
                   (unsigned long long)st0.endpoints, (unsigned long long)st1.endpoints);
        fail++;
    }
    return fail;
}

static u32 test_rpc(void)
{
    u32 fail = 0;
    struct fe_task *t = fe_task_kernel();

    if (fe_failed(fe_endpoint_create(t, 0, &g_srv_ep))) {
        fe_kprintf("        服务端点创建失败\n");
        return 1;
    }
    g_rpc_done = 0;
    g_rpc_errors = 0;
    g_rpc_checksum = 0;

    struct fe_thread *srv = fe_thread_create("rpc-srv", rpc_server_entry, NULL,
                                             16 * 1024, FE_PRIO_NORMAL);
    if (!srv) {
        fe_kprintf("        服务线程创建失败\n");
        return 1;
    }

    u32 mismatches = 0;
    for (u32 i = 0; i < RPC_ROUNDS; i++) {
        u8 req[32];
        u8 rep[32];
        for (u32 k = 0; k < sizeof(req); k++) {
            req[k] = (u8)(i + k);
        }
        u32 reply_len = 0;
        fe_status_t s = fe_endpoint_call(t, g_srv_ep, req, sizeof(req),
                                         rep, sizeof(rep), &reply_len);
        if (fe_failed(s) || reply_len != sizeof(req)) {
            fe_kprintf("        第 %u 次 RPC 失败: %s (len=%u)\n", i, fe_status_name(s),
                       reply_len);
            fail++;
            break;
        }
        for (u32 k = 0; k < sizeof(req); k++) {
            if (rep[k] != (u8)(req[k] + (u8)i)) {
                mismatches++;
                break;
            }
        }
        g_rpc_checksum += rep[0];
        g_rpc_done++;
    }

    i32 rc = fe_thread_join(srv);
    (void)rc;

    fe_kprintf("        RPC 往返 %u/%u 次, 校验和 %llu, 应答不匹配 %u, 服务端错误 %u\n",
               g_rpc_done, RPC_ROUNDS, (unsigned long long)g_rpc_checksum,
               mismatches, g_rpc_errors);
    if (g_rpc_done != RPC_ROUNDS || mismatches != 0 || g_rpc_errors != 0) {
        fail++;
    }
    fe_handle_close(&t->handles, g_srv_ep);
    return fail;
}

static u32 test_notification(void)
{
    u32 fail = 0;
    struct fe_task *t = fe_task_kernel();
    fe_handle_t nt = FE_HANDLE_INVALID;

    if (fe_failed(fe_notification_create(t, &nt))) {
        fe_kprintf("        通知对象创建失败\n");
        return 1;
    }

    /* 未置位时等待应超时阻塞——这里只验证置位路径，阻塞路径由 RPC 测试覆盖 */
    fe_notification_signal(nt, 0x5);
    u64 bits = 0;
    fe_status_t s = fe_notification_wait(nt, 0xF, &bits);
    if (fe_failed(s) || bits != 0x5) {
        fe_kprintf("        通知等待结果异常: %s bits=%#llx\n", fe_status_name(s),
                   (unsigned long long)bits);
        fail++;
    } else {
        fe_kprintf("        通知置位/等待: 得到 %#llx（只返回被置位且被等待的位）\n",
                   (unsigned long long)bits);
    }
    /* 取出即清除：再等应该拿不到（这里用非阻塞方式检查 bits 已清零） */
    fe_handle_close(&t->handles, nt);
    return fail;
}

static u32 test_memory_object(void)
{
    u32 fail = 0;
    struct fe_task *t = fe_task_kernel();
    struct fe_ipc_stats st0, st1;
    fe_ipc_get_stats(&st0);

    const u64 size = 3 * FE_FRAME_SIZE;
    fe_handle_t mo = FE_HANDLE_INVALID;
    if (fe_failed(fe_memory_create(t, size, 0, &mo))) {
        fe_kprintf("        内存对象创建失败\n");
        return 1;
    }

    void *v1 = NULL;
    void *v2 = NULL;
    fe_status_t s = fe_memory_map_kernel(t, mo, FE_PROT_READ | FE_PROT_WRITE, &v1);
    if (fe_failed(s)) {
        fe_kprintf("        第一次映射失败: %s\n", fe_status_name(s));
        fe_handle_close(&t->handles, mo);
        return 1;
    }
    /* 同一个内存对象再映射一次：两块虚拟地址应当看到同一份物理内存 */
    s = fe_memory_map_kernel(t, mo, FE_PROT_READ | FE_PROT_WRITE, &v2);
    if (fe_failed(s)) {
        fe_kprintf("        第二次映射失败: %s\n", fe_status_name(s));
        fail++;
    } else {
        memset(v1, 0xA5, size);
        const u8 *p = (const u8 *)v2;
        u32 bad = 0;
        for (u64 i = 0; i < size; i++) {
            if (p[i] != 0xA5) {
                bad++;
            }
        }
        if (bad) {
            fe_kprintf("        两次映射未共享同一份内存（%u 字节不一致）\n", bad);
            fail++;
        } else {
            fe_kprintf("        内存对象两次映射共享同一物理内存（%llu 字节核对一致）\n",
                       (unsigned long long)size);
        }
        fe_memory_unmap_kernel(v2, size);
    }
    fe_memory_unmap_kernel(v1, size);

    struct fe_object_header *mobj = NULL;
    fe_handle_lookup(&t->handles, mo, FE_RIGHT_READ, &mobj);
    fe_kprintf("        内存对象引用计数（句柄表持有）: %u\n",
               fe_object_refcount(mobj));

    fe_handle_close(&t->handles, mo);

    fe_ipc_get_stats(&st1);
    if (st1.memory_objects != st0.memory_objects) {
        fe_kprintf("        内存对象泄漏: %llu -> %llu\n",
                   (unsigned long long)st0.memory_objects,
                   (unsigned long long)st1.memory_objects);
        fail++;
    }
    return fail;
}

/* ------------------------------------------------------------------ */
/* 句柄传递（能力传递）：这是微内核里"服务间交换内存对象"的唯一通道     */
/* ------------------------------------------------------------------ */

/* 接收线程：收到内存对象句柄 → 映射 → 在**同一块物理内存**上写一个模式
 * → 把"我写了多少字节"回给发送方。
 *
 * 为什么让接收方写、发送方读：单向校验（发送方写、接收方读）只能证明
 * "对方看到了一样的内容"，而两份**内容相同的副本**也满足这一点。
 * 双向都过才排除了"其实各有一份拷贝"。
 *
 * ★ 两条端点，方向各自唯一 ★
 * 第一版用「同一个端点收请求、同一端点收回信」，结果是**客户端先把自己的
 * 请求收了回来**（send 之后立刻 recv，而新线程还没被调度上 CPU）。
 * 于是这次改成：服务端有自己的收件端点（只收），客户端有自己的回信端点（只收），
 * 每个方向上的端点只被一个线程使用。这样"谁会收到这条消息"不再取决于
 * 调度顺序——**方向的唯一性用结构表达，而不是靠让出 CPU 的时机去赌**。 */
#define XFER_SIZE   (3 * FE_FRAME_SIZE)
#define XFER_BYTES  97
#define XFER_PAT_B  0x5Au

static fe_handle_t g_xfer_srv;      /* 服务端收件端点 */
static fe_handle_t g_xfer_cli;      /* 客户端收件端点（服务端往上回信） */
static volatile u32 g_xfer_err;

static void xfer_server_entry(void *arg)
{
    (void)arg;
    struct fe_task *t = fe_task_kernel();
    struct fe_msg_header hdr;
    u8 buf[16];
    fe_handle_t got[FE_MSG_MAX_HANDLES];

    fe_status_t s = fe_endpoint_recv(t, g_xfer_srv, &hdr, buf, sizeof(buf),
                                     NULL, got);
    if (fe_failed(s)) {
        g_xfer_err = 1;
        fe_thread_exit(0);
    }
    if (hdr.handle_count != 1 || got[0] == FE_HANDLE_INVALID) {
        g_xfer_err = 2;
        fe_thread_exit(0);
    }
    /* 收到的能力必须**不能转发**（接收侧权限 = 发送方权限 - TRANSFER）。
     * 反向对照：把它再发给别人必须被拒，否则能力会像病毒一样扩散。 */
    {
        fe_handle_t re[1] = { got[0] };
        if (fe_endpoint_send(t, g_xfer_srv, &hdr, buf, re) != FE_ERR_ACCESS) {
            g_xfer_err = 3;
        }
    }
    void *p = NULL;
    s = fe_memory_map_kernel(t, got[0], FE_PROT_READ | FE_PROT_WRITE, &p);
    if (fe_failed(s)) {
        g_xfer_err = 4;
        fe_thread_exit(0);
    }
    /* 先验发送方写进去的模式（读方向） */
    const u8 *q = (const u8 *)p;
    for (u32 i = 0; i < XFER_BYTES; i++) {
        if (q[i] != (u8)(i ^ 0x3C)) {
            g_xfer_err = 5;
            break;
        }
    }
    /* 再写自己的模式（写方向），发送方会回读校验 */
    memset(p, XFER_PAT_B, XFER_SIZE);

    /* 恢复权限：映射用的是「句柄带 WRITE」这条路径，实例上检查一次 */
    u32 rights = 0;
    fe_handle_rights(&t->handles, got[0], &rights);
    if (!(rights & FE_RIGHT_WRITE) || (rights & FE_RIGHT_TRANSFER)) {
        g_xfer_err = 6;
    }
    fe_memory_unmap_kernel(p, XFER_SIZE);

    u8 rlen = (u8)XFER_SIZE;
    /* ★ 回复必须新建消息头并设 payload_len ★（项目里踩过的坑）：
     * 复用收到的那份头会把 payload_len 当成**请求**的长度发出去。 */
    struct fe_msg_header rh = {
        .protocol = 0xFEFE, .opcode = 2, .payload_len = 1,
        .handle_count = 0, .request_id = 0,
    };
    fe_endpoint_send(t, g_xfer_cli, &rh, &rlen, NULL);
    fe_handle_close(&t->handles, got[0]);
    fe_thread_exit(0);
}

static u32 test_handle_transfer(void)
{
    u32 fail = 0;
    struct fe_task *t = fe_task_kernel();
    struct fe_ipc_stats st0, st1;
    fe_ipc_get_stats(&st0);

    if (fe_failed(fe_endpoint_create(t, 0, &g_xfer_srv)) ||
        fe_failed(fe_endpoint_create(t, 0, &g_xfer_cli))) {
        fe_kprintf("        端点创建失败\n");
        return 1;
    }
    g_xfer_err = 0;

    fe_handle_t mo = FE_HANDLE_INVALID;
    if (fe_failed(fe_memory_create(t, XFER_SIZE, 0, &mo))) {
        fe_kprintf("        内存对象创建失败\n");
        fe_handle_close(&t->handles, g_xfer_srv);
        fe_handle_close(&t->handles, g_xfer_cli);
        return 1;
    }
    void *p = NULL;
    if (fe_failed(fe_memory_map_kernel(t, mo, FE_PROT_READ | FE_PROT_WRITE, &p))) {
        fe_kprintf("        内存对象映射失败\n");
        fe_handle_close(&t->handles, mo);
        fe_handle_close(&t->handles, g_xfer_srv);
        fe_handle_close(&t->handles, g_xfer_cli);
        return 1;
    }
    for (u32 i = 0; i < XFER_BYTES; i++) {
        ((u8 *)p)[i] = (u8)(i ^ 0x3C);
    }

    struct fe_thread *srv = fe_thread_create("xfer-srv", xfer_server_entry, NULL,
                                             16 * 1024, FE_PRIO_NORMAL);
    if (!srv) {
        fe_kprintf("        接收线程创建失败\n");
        fail++;
    } else {
        /* ---- 反向对照 1：没有 TRANSFER 权限的句柄不许发 ---- */
        fe_handle_t no_xfer = FE_HANDLE_INVALID;
        fe_status_t s = fe_handle_dup(&t->handles, mo,
                                      FE_RIGHT_READ | FE_RIGHT_WRITE, &no_xfer);
        if (fe_failed(s)) {
            fe_kprintf("        构造「不可转交」句柄失败\n");
            fail++;
        } else {
            fe_handle_t arr[1] = { no_xfer };
            struct fe_msg_header h = {
                .protocol = 0xFEFE, .opcode = 9, .payload_len = 0,
                .handle_count = 1, .request_id = 0,
            };
            s = fe_endpoint_send(t, g_xfer_srv, &h, NULL, arr);
            if (s != FE_ERR_ACCESS) {
                fe_kprintf("        没有 TRANSFER 权限的句柄竟然发得出去（返回 %d）\n", s);
                fail++;
            } else {
                fe_kprintf("        无 TRANSFER 权限的句柄被拒绝: %s\n", fe_status_name(s));
            }
            fe_handle_close(&t->handles, no_xfer);
        }

        /* ---- 反向对照 2：不存在的句柄号 ---- */
        {
            fe_handle_t arr[1] = { 200 };
            struct fe_msg_header h = {
                .protocol = 0xFEFE, .opcode = 9, .payload_len = 0,
                .handle_count = 1, .request_id = 0,
            };
            fe_status_t s2 = fe_endpoint_send(t, g_xfer_srv, &h, NULL, arr);
            if (s2 != FE_ERR_BADHANDLE) {
                fe_kprintf("        越界句柄号未被拒绝（返回 %d）\n", s2);
                fail++;
            } else {
                fe_kprintf("        越界句柄号被拒绝: %s\n", fe_status_name(s2));
            }
        }

        /* ---- 反向对照 3：说传句柄却不给数组 ---- */
        {
            struct fe_msg_header h = {
                .protocol = 0xFEFE, .opcode = 9, .payload_len = 0,
                .handle_count = 1, .request_id = 0,
            };
            fe_status_t s3 = fe_endpoint_send(t, g_xfer_srv, &h, NULL, NULL);
            if (s3 != FE_ERR_INVAL) {
                fe_kprintf("        handle_count=1 而数组为空未被拒绝（返回 %d）\n", s3);
                fail++;
            }
        }

        /* ---- 反向对照 4：超过句柄上限 ---- */
        {
            fe_handle_t arr[FE_MSG_MAX_HANDLES + 1] = { mo, mo, mo, mo, mo };
            struct fe_msg_header h = {
                .protocol = 0xFEFE, .opcode = 9, .payload_len = 0,
                .handle_count = FE_MSG_MAX_HANDLES + 1, .request_id = 0,
            };
            fe_status_t s4 = fe_endpoint_send(t, g_xfer_srv, &h, NULL, arr);
            if (s4 != FE_ERR_INVAL) {
                fe_kprintf("        超过 %u 个句柄未被拒绝（返回 %d）\n",
                           FE_MSG_MAX_HANDLES, s4);
                fail++;
            }
        }

        /* ---- 正向：真把内存对象交过去 ---- */
        u8 req[4] = { 1, 2, 3, 4 };
        fe_handle_t arr[1] = { mo };
        struct fe_msg_header h = {
            .protocol = 0xFEFE, .opcode = 1, .payload_len = (u32)sizeof(req),
            .handle_count = 1, .request_id = 0x55,
        };
        fe_status_t s5 = fe_endpoint_send(t, g_xfer_srv, &h, req, arr);
        if (fe_failed(s5)) {
            fe_kprintf("        内存对象发送失败: %s\n", fe_status_name(s5));
            fail++;
        } else {
            struct fe_msg_header rh;
            u8 rbuf[8];
            fe_handle_t rgot[FE_MSG_MAX_HANDLES];
            fe_status_t s6 = fe_endpoint_recv(t, g_xfer_cli, &rh, rbuf, sizeof(rbuf),
                                              NULL, rgot);
            if (fe_failed(s6)) {
                fe_kprintf("        等待应答失败: %s\n", fe_status_name(s6));
                fail++;
            } else if (rh.protocol != 0xFEFE || rh.opcode != 2 || rh.payload_len != 1) {
                fe_kprintf("        应答头异常: proto=%#x op=%u len=%u\n",
                           rh.protocol, rh.opcode, rh.payload_len);
                fail++;
            } else if (rh.sender_task != t->id) {
                /* 服务线程与客户端在**同一个任务**里，所以 sender_task 相同。
                 * 这一条是"内核真的在填这个字段"的正面证据——它是所有
                 * per-client 策略（A/B 访问矩阵）的前提。 */
                fe_kprintf("        应答的 sender_task=%llu，期望 %llu\n",
                           (unsigned long long)rh.sender_task, (unsigned long long)t->id);
                fail++;
            } else if (rbuf[0] != (u8)XFER_SIZE) {
                fe_kprintf("        服务端报告写入 %u 字节，期望 %llu\n", rbuf[0],
                   (unsigned long long)XFER_SIZE);
                fail++;
            } else {
                /* 接收方在上面把整块内存写成了 XFER_PAT_B：
                 * 发送方在这里回读**自己**的映射，看得见就证明两边是同一块物理内存。 */
                const u8 *q = (const u8 *)p;
                u32 bad = 0;
                for (u32 i = 0; i < XFER_SIZE; i++) {
                    if (q[i] != XFER_PAT_B) {
                        bad++;
                    }
                }
                if (bad) {
                    fe_kprintf("        接收方写入的内容发送方看不到（%u/%llu 字节不符）\n",
                               bad, (unsigned long long)XFER_SIZE);
                    fail++;
                }
            }
        }
        fe_thread_join(srv);
    }

    if (g_xfer_err) {
        fe_kprintf("        服务端错误码 %u\n", g_xfer_err);
        fail++;
    }
    if (fail == 0) {
        fe_kprintf("        句柄传递: 内存对象跨线程转交 + %llu 字节内容双向核对一致\n",
                   (unsigned long long)XFER_SIZE);
        fe_kprintf("        反向对照: 无 TRANSFER 权限 / 越界句柄 / 空数组 / 超上限 / 二次转发 全部被拒\n");
    }

    fe_memory_unmap_kernel(p, XFER_SIZE);
    fe_handle_close(&t->handles, mo);
    fe_handle_close(&t->handles, g_xfer_srv);
    fe_handle_close(&t->handles, g_xfer_cli);

    fe_ipc_get_stats(&st1);
    if (st1.endpoints != st0.endpoints || st1.memory_objects != st0.memory_objects) {
        fe_kprintf("        句柄传递测试后有对象泄漏: 端点 %llu->%llu 内存 %llu->%llu\n",
                   (unsigned long long)st0.endpoints, (unsigned long long)st1.endpoints,
                   (unsigned long long)st0.memory_objects,
                   (unsigned long long)st1.memory_objects);
        fail++;
    }
    return fail;
}

/* 句柄复制（能力收窄）。
 *
 * ★ 为什么这件事要专门测 ★
 * 复制一个句柄时最容易出的错是"复制出来的权限跟原件一样"——
 * 那样"只给读"就变成了"其实还能写"，而调用者以为自己已经收窄了。
 * 症状是安静的数据损坏，不是崩溃。所以这里用**可观察的行为**验证收窄：
 * 收到只读句柄的那一方**写那块内存必须失败**。 */
static u32 test_handle_dup(void)
{
    u32 fail = 0;
    struct fe_task *t = fe_task_kernel();
    struct fe_ipc_stats st0, st1;
    fe_ipc_get_stats(&st0);

    fe_handle_t mo = FE_HANDLE_INVALID;
    if (fe_failed(fe_memory_create(t, FE_FRAME_SIZE, 0, &mo))) {
        fe_kprintf("        内存对象创建失败\n");
        return 1;
    }
    fe_handle_t ro = FE_HANDLE_INVALID;
    fe_status_t s = fe_handle_dup(&t->handles, mo, FE_RIGHT_READ, &ro);
    if (fe_failed(s)) {
        fe_kprintf("        句柄复制失败: %s\n", fe_status_name(s));
        fe_handle_close(&t->handles, mo);
        return 1;
    }

    /* ---- 正向：只读副本可以只读映射 ---- */
    void *pr = NULL;
    if (fe_failed(fe_memory_map_kernel(t, ro, FE_PROT_READ, &pr))) {
        fe_kprintf("        只读副本的可读映射失败\n");
        fail++;
    } else {
        fe_memory_unmap_kernel(pr, FE_FRAME_SIZE);
    }

    /* ---- 反向对照：只读副本**不能**可写映射 ---- */
    void *pw = NULL;
    s = fe_memory_map_kernel(t, ro, FE_PROT_READ | FE_PROT_WRITE, &pw);
    if (s != FE_ERR_ACCESS) {
        fe_kprintf("        只读副本竟然可以可写映射（返回 %s）\n", fe_status_name(s));
        fail++;
        if (!fe_failed(s)) {
            fe_memory_unmap_kernel(pw, FE_FRAME_SIZE);
        }
    } else {
        fe_kprintf("        只读副本被拒绝可写映射: %s\n", fe_status_name(s));
    }

    /* ---- 反向对照：原件不受影响 ---- */
    void *po = NULL;
    if (fe_failed(fe_memory_map_kernel(t, mo, FE_PROT_READ | FE_PROT_WRITE, &po))) {
        fe_kprintf("        原句柄的可写映射受影响（收窄不该动到原件）\n");
        fail++;
    } else {
        fe_memory_unmap_kernel(po, FE_FRAME_SIZE);
    }

    /* ---- 反向对照：权限放大必须被拒 ---- */
    fe_handle_t big = FE_HANDLE_INVALID;
    s = fe_handle_dup(&t->handles, ro, FE_RIGHT_READ | FE_RIGHT_WRITE, &big);
    if (s != FE_ERR_ACCESS) {
        fe_kprintf("        从只读副本放大出写权限竟然成功（返回 %s）\n", fe_status_name(s));
        fail++;
        if (!fe_failed(s)) {
            fe_handle_close(&t->handles, big);
        }
    }

    /* ---- 反向对照：没有 DUP 权限的句柄不能被复制 ---- */
    fe_handle_t no_dup = FE_HANDLE_INVALID;
    if (!fe_failed(fe_handle_dup(&t->handles, mo, FE_RIGHT_READ | FE_RIGHT_WRITE |
                                 FE_RIGHT_TRANSFER, &no_dup))) {
        fe_handle_t again = FE_HANDLE_INVALID;
        s = fe_handle_dup(&t->handles, no_dup, FE_RIGHT_READ, &again);
        if (s != FE_ERR_ACCESS) {
            fe_kprintf("        无 DUP 权限的句柄竟然能再复制（返回 %s）\n", fe_status_name(s));
            fail++;
        }
        fe_handle_close(&t->handles, no_dup);
    } else {
        fe_kprintf("        构造无 DUP 权限的句柄失败\n");
        fail++;
    }

    /* ---- 两个句柄各自关闭：对象活到最后一个 ---- */
    fe_handle_close(&t->handles, ro);
    fe_handle_close(&t->handles, mo);

    fe_ipc_get_stats(&st1);
    if (st1.memory_objects != st0.memory_objects) {
        fe_kprintf("        复制出来的句柄没有独立释放: 内存对象 %llu -> %llu\n",
                   (unsigned long long)st0.memory_objects,
                   (unsigned long long)st1.memory_objects);
        fail++;
    } else if (fail == 0) {
        fe_kprintf("        句柄复制: 收窄生效（只读副本不可写）、放大被拒、原件不受影响、"
                   "两个句柄独立关闭\n");
    }
    return fail;
}

/* ------------------------------------------------------------------ */


/* ------------------------------------------------------------------ */
/* 多对象等待（wait_any）                                              */
/* ------------------------------------------------------------------ */

/* ★ 这一组要证明三件事，每件都可能单独坏掉 ★
 *
 *   1. **已经就绪的目标立刻返回**（不阻塞）——否则驱动会在
 *      "设备早就中断了"的情况下白等一个节拍。
 *   2. **被通知唤醒**：等两个对象、只有通知被置位时，必须返回
 *      通知那一项，而且位要被取走（取出即清除）。
 *   3. **被消息唤醒**：同样两个对象、只有端点来消息时，必须返回
 *      端点那一项，而且消息要**顺带收到**（这是 wait_any 与
 *      "poll + 再收一次"的区别：不会在两次调用之间被别人取走）。
 *
 * ★ 反向对照在哪 ★
 * 第 2/3 条各自都有一个"**不该醒的那个**"：等两个对象、只动其中一个，
 * 返回的下标必须是动的那个。如果实现里把下标写死成 0（或者"醒来就报
 * 第一项"），这两条就会互相抓住——它们要求的返回值不同。
 * 第 2 条还多一条反向：被取走的位**必须真的没了**（"取出即清除"）。
 *
 * ★ 这一组挂了整整一轮，根因是**反向对照自己写错**，记在这里 ★
 *
 * 第一版第 2 条的反向是这么写的：
 *     u64 left = 0;
 *     if (fe_notification_wait(g_wa_nt, 0, &left) == FE_OK) { 报错 }
 * 它的契约是"mask == 0 视为 ~0（等所有位）"，而此刻位已经被 wait_any
 * 取走、再没有人会置位 —— 于是这次调用**永久阻塞**，自检挂死在那里。
 *
 * 误导性极强：日志停在"第 2 组 OK"之后，第 3 组一行都没有，
 * 看上去完全像"第 3 组的消息唤醒没被唤醒"。**实际上第 3 组根本没被调用过。**
 * 我在探针里同时打了"主线程即将进入 wait_any"（调用点）和
 * "阻塞结束、被唤醒"（callee），两个一对照就排除了第 3 组——
 * 所以定位这类问题要**在调用点与 callee 各打一个探针**，只打一边必然误判。
 *
 * ★ 教训（项目里已有同源的一条，这里再记一次）：★
 *   **自检里绝不能用"本该阻塞"的接口去验证它没立刻返回。**
 *   "没返回"与"永远不返回"在日志上长得一模一样。
 * 正确做法：验证"位还在不在"是**对象状态**问题，直接读对象状态最准确、
 * 也没有任何时序问题——下面第 2 条就是这么改的。 */

/* ★ 夹具（两个 helper 线程）的握手：换过三版，两种错法都值得留着 ★
 *
 * 1. 用 `while (!flag) fe_thread_yield();` 等 —— **yield 是让出不是阻塞**，
 *    那条线程始终在就绪队列里空转，把主线程饿死。症状与"机制不唤醒"
 *    一模一样。
 * 2. 用"共享世代号 + 创建后置位" —— `fe_thread_create` **返回前**线程
 *    就已经在就绪队列里，于是存在竞争窗口；我把条件写反之后，
 *    helper 干脆永远等下去，而我花了好几轮才看出那是夹具的错。
 *
 * 现在这样最笨也最稳：**每个测试一份自己的标志**（test A 用 g_wa_go_a、
 * test B 用 g_wa_go_b），在**创建 helper 之前**清 0（此刻上一轮的 helper
 * 早已 join 退出，不会误读），建完线程再置 1——于是"创建后置位"那个窗口里
 * helper 最多多睡一轮，绝不会看到别人留下的值。
 * helper 用**阻塞睡眠**等，并且**多睡一个节拍**让主线程先进入阻塞，
 * 否则测到的就是"已经就绪"那条路径，而不是"被唤醒"。
 *
 * 教训：测试夹具的同步状态**不要跨测试复用**，也不要用忙等。
 * 省一个变量，换来的是"某条测试偶尔挂住"这种最难查的现象。 */
static fe_handle_t g_wa_ep;          /* 用于"已经就绪"那一组 */
static fe_handle_t g_wa_ep2;         /* 用于"被唤醒"那两组 */
static fe_handle_t g_wa_nt;          /* 通知对象 */
static volatile u32 g_wa_go_a;
static volatile u32 g_wa_go_b;

static void wa_signaler(void *arg)
{
    (void)arg;
    while (!g_wa_go_a) {
        fe_thread_sleep_ms(1);
    }
    /* 让主线程先跑到 wait_any 里阻塞下来：这样测的才是"被唤醒"，
     * 而不是"已经就绪那一遍就返回了"。 */
    fe_thread_sleep_ms(2);
    fe_notification_signal(g_wa_nt, 0x4);       /* 只置 bit2 */
    fe_thread_exit(0);
}

static void wa_sender(void *arg)
{
    (void)arg;
    while (!g_wa_go_b) {
        fe_thread_sleep_ms(1);
    }
    fe_thread_sleep_ms(2);
    const char *text = "WAKE-BY-MSG";
    struct fe_msg_header h = {
        .protocol = 0xFE07, .opcode = 3, .payload_len = (u32)strlen(text) + 1,
        .handle_count = 0, .request_id = 0x99,
    };
    (void)fe_endpoint_send(fe_task_kernel(), g_wa_ep2, &h, text, NULL);
    fe_thread_exit(0);
}

/* ---- 1. 已经就绪的目标立刻返回 ---- */
static u32 test_wait_any_ready(void)
{
    u32 fail = 0;
    struct fe_task *t = fe_task_kernel();
    struct fe_wait_target tg[3];
    struct fe_wait_result res;

    /* 三项：通知（没置位）/ 端点（有消息）/ 通知（没置位）。
     * **中间**那项就绪，所以返回的下标必须是 1——"写死 0"或
     * "总是报第一项/最后一项"都会在这里失败。 */
    const char *text = "READY-NOW";
    struct fe_msg_header h = {
        .protocol = 0xFE07, .opcode = 1, .payload_len = (u32)strlen(text) + 1,
        .handle_count = 0, .request_id = 0,
    };
    if (fe_failed(fe_endpoint_send(t, g_wa_ep, &h, text, NULL))) {
        fe_kprintf("        准备消息失败\n");
        return 1;
    }
    tg[0].handle = g_wa_nt;  tg[0].kind = FE_WAIT_BITS; tg[0].mask = 0xFF;
    tg[1].handle = g_wa_ep;  tg[1].kind = FE_WAIT_MSG;  tg[1].mask = 0;
    tg[2].handle = g_wa_nt;  tg[2].kind = FE_WAIT_BITS; tg[2].mask = 0xFF;

    fe_status_t s = fe_wait_any(t, tg, 3, &res, sizeof(res.payload));
    if (fe_failed(s)) {
        fe_kprintf("        wait_any 失败: %s\n", fe_status_name(s));
        fail++;
    } else if (res.index != 1) {
        fe_kprintf("        就绪项下标错误: 实际 %u，期望 1\n", res.index);
        fail++;
    } else if (res.hdr.protocol != 0xFE07 || res.hdr.opcode != 1) {
        fe_kprintf("        消息头不对: proto=%#x op=%u\n", res.hdr.protocol,
                   res.hdr.opcode);
        fail++;
    } else if (res.payload_cap != strlen(text) + 1 ||
               strcmp((const char *)res.payload, text) != 0) {
        fe_kprintf("        顺带收下的载荷不对\n");
        fail++;
    } else if (res.hdr.sender_task != t->id) {
        fe_kprintf("        sender_task=%llu，期望 %llu\n",
                   (unsigned long long)res.hdr.sender_task,
                   (unsigned long long)t->id);
        fail++;
    } else {
        fe_kprintf("        已就绪的目标立刻返回（下标 %u，消息顺带收到）: OK\n",
                   res.index);
    }
    return fail;
}

/* ---- 2. 被通知唤醒（端点在 0 号位、通知在 1 号位） ---- */
static u32 test_wait_any_by_bits(void)
{
    u32 fail = 0;
    struct fe_task *t = fe_task_kernel();
    struct fe_wait_target tg[2];
    struct fe_wait_result res;

    /* 建线程**之前**清标志：此刻 g_wa_ep2 是空的、通知没置位，
     * 所以 helper 不能提前动手（提前动手只会让"已经就绪"那条路径
     * 被多测一次，不影响本条的判定，但我们仍然按最紧的顺序写）。 */
    g_wa_go_a = 0;
    struct fe_thread *th = fe_thread_create("wa-signal", wa_signaler, NULL,
                                            16 * 1024, FE_PRIO_NORMAL);
    if (!th) {
        fe_kprintf("        唤醒线程创建失败\n");
        return 1;
    }
    tg[0].handle = g_wa_ep2; tg[0].kind = FE_WAIT_MSG;  tg[0].mask = 0;
    tg[1].handle = g_wa_nt;  tg[1].kind = FE_WAIT_BITS; tg[1].mask = 0xF;
    g_wa_go_a = 1;

    fe_status_t s = fe_wait_any(t, tg, 2, &res, 0);
    fe_thread_join(th);
    if (fe_failed(s)) {
        fe_kprintf("        wait_any 失败: %s\n", fe_status_name(s));
        fail++;
    } else if (res.index != 1) {
        fe_kprintf("        被通知唤醒却报了下标 %u（期望 1）\n", res.index);
        fail++;
    } else if (res.bits != 0x4) {
        fe_kprintf("        取到的位不对: %#llx（期望 0x4）\n",
                   (unsigned long long)res.bits);
        fail++;
    } else {
        fe_kprintf("        被通知唤醒（下标 1，取到位 %#llx，取出即清除）: OK\n",
                   (unsigned long long)res.bits);
        /* ---- 反向对照：位应当已被 wait_any 取走（取出即清除） ----
         *
         * ★ 这里**必须直接读对象状态**，不能再调一次 fe_notification_wait ★
         *
         * 第一版写的是：
         *     u64 left = 0;
         *     if (fe_notification_wait(g_wa_nt, 0, &left) == FE_OK) { ... }
         * 它的后果是**把自检自己挂死**：`fe_notification_wait` 的契约是
         * "mask == 0 视为 ~0（等所有位）"，而此刻位已经被取走、再没有人会置位，
         * 于是这一次调用**永远阻塞**。
         *
         * 症状极具误导性：日志停在第二组的 OK 之后、第三组一行都没有，
         * 看起来像"第三组的消息唤醒没实现/没被唤醒"。实际上第三组
         * 根本没被调用过——自检卡在第二组的反向对照里。
         *
         * 教训（与项目里已有的那条同源，值得再记一次）：
         *   **自检里绝不能用"本该阻塞"的接口去验证它没立刻返回。**
         *   "没返回"与"永远不返回"在日志上长得一模一样。
         * 要用阻塞接口做反向对照，只有一条安全写法：**换一条 helper 线程**
         * 去等、主线程用超时或计数判定。这里连那一步都不需要——
         * "位还在不在"是**对象的状态**，直接查对象最准确，也没有时序问题。 */
        struct fe_object_header *obj = NULL;
        if (fe_failed(fe_handle_lookup(&t->handles, g_wa_nt, 0, &obj)) ||
            obj->type != FE_OBJ_NOTIFICATION) {
            fe_kprintf("        通知对象查不到了（句柄表状态异常）\n");
            fail++;
        } else {
            struct fe_notification *nt = FE_OBJ_OF(obj, struct fe_notification);
            u64 irq = fe_irq_save();
            u64 left = nt->bits;
            fe_irq_restore(irq);
            if (left != 0) {
                fe_kprintf("        位没有被取走（取出即清除失效）: bits=%#llx\n",
                           (unsigned long long)left);
                fail++;
            } else {
                fe_kprintf("        反向：位已被取走（对象 bits 归零），"
                           "同一个掩码不再就绪\n");
            }
        }
    }
    return fail;
}

/* ---- 3. 被消息唤醒（端点仍在 0 号位，但这次动的是它） ---- */
static u32 test_wait_any_by_msg(void)
{
    u32 fail = 0;
    struct fe_task *t = fe_task_kernel();
    struct fe_wait_target tg[2];
    struct fe_wait_result res;

    g_wa_go_b = 0;
    struct fe_thread *th = fe_thread_create("wa-send", wa_sender, NULL,
                                            16 * 1024, FE_PRIO_NORMAL);
    if (!th) {
        fe_kprintf("        发送线程创建失败\n");
        return 1;
    }
    /* 顺序与上一组相同，但**动的对象不同**：上一组动通知（期望下标 1），
     * 这一组动端点（期望下标 0）。两条一起排除了"把下标写死"。 */
    tg[0].handle = g_wa_ep2; tg[0].kind = FE_WAIT_MSG;  tg[0].mask = 0;
    tg[1].handle = g_wa_nt;  tg[1].kind = FE_WAIT_BITS; tg[1].mask = 0xF;
    g_wa_go_b = 1;

    fe_status_t s = fe_wait_any(t, tg, 2, &res, sizeof(res.payload));
    fe_thread_join(th);
    if (fe_failed(s)) {
        fe_kprintf("        wait_any 失败: %s\n", fe_status_name(s));
        fail++;
    } else if (res.index != 0) {
        fe_kprintf("        被消息唤醒却报了下标 %u（期望 0）\n", res.index);
        fail++;
    } else if (res.payload_cap == 0 ||
               strcmp((const char *)res.payload, "WAKE-BY-MSG") != 0) {
        fe_kprintf("        顺带收下的消息不对\n");
        fail++;
    } else {
        fe_kprintf("        被消息唤醒（下标 0，消息顺带收到）: OK\n");
    }
    return fail;
}

/* ---- 4. 反向对照：格式错误必须被拒，而不是"当作没就绪"永远等下去 ---- */
static u32 test_wait_any_rejects(void)
{
    u32 fail = 0;
    struct fe_task *t = fe_task_kernel();
    struct fe_wait_target tg[2];
    struct fe_wait_result res;

    /* 类型不匹配：拿通知当端点等 */
    tg[0].handle = g_wa_nt; tg[0].kind = FE_WAIT_MSG; tg[0].mask = 0;
    if (fe_wait_any(t, tg, 1, &res, 0) != FE_ERR_INVAL) {
        fe_kprintf("        把通知当端点等竟然没被拒\n");
        fail++;
    }
    /* 坏句柄 */
    tg[0].handle = 9999; tg[0].kind = FE_WAIT_MSG; tg[0].mask = 0;
    if (fe_wait_any(t, tg, 1, &res, 0) != FE_ERR_BADHANDLE) {
        fe_kprintf("        坏句柄竟然没被拒\n");
        fail++;
    }
    /* n 的边界 */
    tg[0].handle = g_wa_nt; tg[0].kind = FE_WAIT_BITS; tg[0].mask = 1;
    if (fe_wait_any(t, tg, 0, &res, 0) != FE_ERR_INVAL) {
        fe_kprintf("        n=0 竟然没被拒\n");
        fail++;
    }
    if (fe_wait_any(t, tg, 9, &res, 0) != FE_ERR_INVAL) {
        fe_kprintf("        n=9 竟然没被拒\n");
        fail++;
    }
    if (fail == 0) {
        fe_kprintf("        反向：类型不匹配 / 坏句柄 / n 越界 全部被拒: OK\n");
    }
    return fail;
}

u32 fe_selftest_wait_any(void)
{
    u32 fail = 0;
    struct fe_task *t = fe_task_kernel();
    struct fe_ipc_stats st0, st1;
    fe_ipc_get_stats(&st0);

    if (fe_failed(fe_endpoint_create(t, 0, &g_wa_ep)) ||
        fe_failed(fe_endpoint_create(t, 0, &g_wa_ep2)) ||
        fe_failed(fe_notification_create(t, &g_wa_nt))) {
        fe_kprintf("        对象创建失败\n");
        return 1;
    }

    fail += test_wait_any_ready();
    fail += test_wait_any_by_bits();
    fail += test_wait_any_by_msg();
    fail += test_wait_any_rejects();

    fe_handle_close(&t->handles, g_wa_ep);
    fe_handle_close(&t->handles, g_wa_ep2);
    fe_handle_close(&t->handles, g_wa_nt);

    fe_ipc_get_stats(&st1);
    if (st1.endpoints != st0.endpoints || st1.notifications != st0.notifications) {
        fe_kprintf("        wait_any 自检后有对象泄漏\n");
        fail++;
    }
    return fail;
}

/* ------------------------------------------------------------------ */

u32 fe_selftest_ipc(void)
{
    u32 fail = 0;
    struct fe_ipc_stats st0, st1;
    fe_ipc_get_stats(&st0);
    fe_kprintf("[自检] IPC: 起始状态 端点 %llu 通知 %llu 内存对象 %llu, 内核任务句柄 %u\n",
               (unsigned long long)st0.endpoints, (unsigned long long)st0.notifications,
               (unsigned long long)st0.memory_objects,
               fe_handle_table_used(&fe_task_kernel()->handles));

    fail += test_basic_sendrecv();
    fail += test_handle_rights();
    fail += test_handle_dup();
    fail += test_notification();
    fail += test_memory_object();
    fail += test_handle_transfer();
    fail += test_rpc();

    fe_ipc_get_stats(&st1);
    fe_kprintf("        消息累计: 发送 %llu 接收 %llu, 分配 %llu 释放 %llu\n",
               (unsigned long long)st1.messages_sent, (unsigned long long)st1.messages_recv,
               (unsigned long long)st1.messages_allocated,
               (unsigned long long)st1.messages_freed);
    if (st1.messages_allocated != st1.messages_freed) {
        fe_kprintf("        消息有泄漏: 分配 %llu 释放 %llu\n",
                   (unsigned long long)st1.messages_allocated,
                   (unsigned long long)st1.messages_freed);
        fail++;
    }
    if (st1.endpoints != st0.endpoints || st1.notifications != st0.notifications ||
        st1.memory_objects != st0.memory_objects) {
        fe_kprintf("        对象计数未回到基线: 端点 %llu->%llu 通知 %llu->%llu 内存 %llu->%llu\n",
                   (unsigned long long)st0.endpoints, (unsigned long long)st1.endpoints,
                   (unsigned long long)st0.notifications, (unsigned long long)st1.notifications,
                   (unsigned long long)st0.memory_objects, (unsigned long long)st1.memory_objects);
        fail++;
    }
    return fail;
}