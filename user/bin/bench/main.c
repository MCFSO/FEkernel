/* SPDX-License-Identifier: 0BSD */
/* 用户态性能基准测试。
 *
 * 为什么在用户态测：这里测到的每一条路径都包含**完整代价**（syscall 进出、换栈、
 * 参数校验、调度器参与），而不是内核内部某个函数的热身数据。
 * 计时用 TSC（rdtsc 不需要特权指令），TSC 频率已用 8254 独立时基交叉验证过。
 *
 * 关于取值方式：本机 VirtualBox 与 Hyper-V 共存，vCPU 调度不稳，同一项测试不同轮次
 * 能差 2~3 倍。干扰只会让测量值**变大**，因此每项都跑 REPEAT 轮、取**最小值**
 * 作为该路径的固有代价估计；同时把轮次区间打出来，让人能看到抖动有多大。
 */
#include <fe_user.h>

#define ROUNDS_SYSCALL  20000
#define ROUNDS_YIELD    20000
#define RPC_WARMUP      50
#define ROUNDS_RPC      10000
#define RPC_TOTAL       (RPC_WARMUP + ROUNDS_RPC)
#define ROUNDS_ALLOC    2000
#define MEMCPY_SIZE     (256 * 1024)
#define MEMCPY_ROUNDS   100
#define REPEAT          5

static u64 rdtsc(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static void report(const char *name, u64 best, u64 worst, u64 rounds)
{
    fe_puts("  ");
    fe_puts(name);
    fe_puts(": 最快 ");
    fe_print_u64(rounds ? best / rounds : 0);
    fe_puts(" 周期/次");
    if (worst != best) {
        fe_puts("  (最慢 ");
        fe_print_u64(rounds ? worst / rounds : 0);
        fe_puts(")");
    }
    fe_puts("   [");
    fe_print_u64(REPEAT);
    fe_puts(" 轮取最快]\n");
}

/* ------------------------------------------------------------------ */
/* RPC 服务线程                                                        */
/* ------------------------------------------------------------------ */

static long g_rpc_ep;
static volatile u32 g_rpc_served;

static void rpc_server(void *arg)
{
    (void)arg;
    /* 预留一轮重试量：客户端可能因抖动多发几次 */
    for (u32 i = 0; i < RPC_TOTAL * REPEAT + 64; i++) {
        struct fe_msg_header hdr;
        u8 buf[32];
        long reply = fe_endpoint_recv(g_rpc_ep, &hdr, buf, sizeof(buf), NULL, NULL);
        if (reply <= 0) {
            break;
        }
        struct fe_msg_header rh = { 0, 0, hdr.payload_len, 0, 0, 0 };
        /* sender_task 留 0：它由内核在投递时填写，发送方写什么都不算数 */
        fe_endpoint_send(reply, &rh, buf, NULL, 0);
        fe_handle_close(reply);
        g_rpc_served++;
    }
    fe_exit(0);
}

/* ------------------------------------------------------------------ */

static u8 g_src[MEMCPY_SIZE];
static u8 g_dst[MEMCPY_SIZE];

int main(void)
{
    u64 best, worst, t0, t1, dt;

    fe_puts("\n=== 性能基准测试（ring 3，TSC 计时）===\n");

    /* ---------------- 1. 系统调用往返 ---------------- */
    for (u32 i = 0; i < 200; i++) {
        fe_clock_ns();                     /* 预热 */
    }
    best = ~0ull;
    worst = 0;
    for (u32 rep = 0; rep < REPEAT; rep++) {
        t0 = rdtsc();
        for (u32 i = 0; i < ROUNDS_SYSCALL; i++) {
            fe_clock_ns();
        }
        t1 = rdtsc();
        dt = t1 - t0;
        if (dt < best) { best = dt; }
        if (dt > worst) { worst = dt; }
    }
    report("系统调用往返 (fe_clock_ns)", best, worst, ROUNDS_SYSCALL);

    /* ---------------- 2. 上下文切换（主动让出） ---------------- */
    best = ~0ull;
    worst = 0;
    for (u32 rep = 0; rep < REPEAT; rep++) {
        t0 = rdtsc();
        for (u32 i = 0; i < ROUNDS_YIELD; i++) {
            fe_yield();
        }
        t1 = rdtsc();
        dt = t1 - t0;
        if (dt < best) { best = dt; }
        if (dt > worst) { worst = dt; }
    }
    report("线程让出 (fe_yield，含完整切换)", best, worst, ROUNDS_YIELD);

    /* ---------------- 3. IPC 往返（同步 RPC） ---------------- */
    g_rpc_ep = fe_endpoint_create(0);
    if (g_rpc_ep > 0) {
        fe_thread_create(rpc_server, (void *)0, (void *)0, 0);
        for (u32 i = 0; i < 50; i++) {
            fe_sleep_ms(1);                /* 等服务端进入阻塞 */
        }
        u8 req[16];
        u8 rep_buf[16];
        for (u32 i = 0; i < sizeof(req); i++) {
            req[i] = (u8)i;
        }
        u32 reply_len = 0;
        best = ~0ull;
        worst = 0;
        for (u32 r = 0; r < REPEAT; r++) {
            t0 = rdtsc();
            for (u32 i = 0; i < ROUNDS_RPC; i++) {
                fe_endpoint_call(g_rpc_ep, req, sizeof(req), rep_buf, sizeof(rep_buf),
                                 &reply_len);
            }
            t1 = rdtsc();
            dt = t1 - t0;
            if (dt < best) { best = dt; }
            if (dt > worst) { worst = dt; }
        }
        report("IPC 往返 (call+reply)", best, worst, ROUNDS_RPC);
        fe_handle_close(g_rpc_ep);
    } else {
        fe_puts("  端点创建失败，跳过 IPC 基准\n");
    }

    /* ---------------- 4. 内存拷贝带宽 ---------------- */
    for (usize i = 0; i < MEMCPY_SIZE; i++) {
        g_src[i] = (u8)i;
    }
    best = ~0ull;
    for (u32 rep = 0; rep < REPEAT; rep++) {
        t0 = rdtsc();
        for (u32 r = 0; r < MEMCPY_ROUNDS; r++) {
            memcpy(g_dst, g_src, MEMCPY_SIZE);
        }
        t1 = rdtsc();
        dt = t1 - t0;
        if (dt < best) { best = dt; }
    }
    {
        u64 bytes = (u64)MEMCPY_SIZE * MEMCPY_ROUNDS;
        /* 按 3.0 GHz 折算：字节/周期 × 3000 ≈ MB/s */
        u64 mb_per_s = best ? (bytes * 3000ull / best) : 0;
        fe_puts("  内存拷贝带宽: ");
        fe_print_u64(mb_per_s);
        fe_puts(" MB/s  (");
        fe_print_u64(bytes / 1024);
        fe_puts(" KiB，最快一轮)\n");
    }

    /* ---------------- 5. 内存对象分配 ---------------- */
    best = ~0ull;
    worst = 0;
    for (u32 rep = 0; rep < REPEAT; rep++) {
        t0 = rdtsc();
        for (u32 i = 0; i < ROUNDS_ALLOC; i++) {
            long mo = fe_mem_alloc(4096);
            if (mo > 0) {
                fe_handle_close(mo);
            }
        }
        t1 = rdtsc();
        dt = t1 - t0;
        if (dt < best) { best = dt; }
        if (dt > worst) { worst = dt; }
    }
    report("内存对象 4KiB 分配+释放", best, worst, ROUNDS_ALLOC);

    fe_puts("=== 基准测试结束 ===\n");
    return 0;
}
