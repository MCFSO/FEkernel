/* SPDX-License-Identifier: 0BSD */
/* 用户态测试程序。
 *
 * 它在 ring 3 里跑，通过自定义 syscall ABI 使用内核能力：
 *   打印 → 时间 → 内存对象 → 创建第二个用户线程 → 退出。
 * 内核会检查它的退出码，以此证明「用户程序的返回值能传回内核」。
 */
#include <fe_user.h>

static volatile u64 g_thread_ran;
static volatile usize g_memcheck_bad;
static volatile u64 g_thread_slept_ms;

/* 第二个用户线程：验证 syscall 在多线程下同样可用 */
static void worker(void *arg)
{
    u64 t0 = fe_clock_ns();
    fe_sleep_ms(20);
    u64 t1 = fe_clock_ns();
    g_thread_slept_ms = (t1 - t0) / 1000000ull;
    fe_puts("  [ring3-worker] 我是第二个用户线程，已通过 sleep 让出 CPU\n");
    g_thread_ran = 1;
    fe_exit(0);
}

int main(void)
{
    fe_puts("\n=== 用户态程序开始运行（ring 3）===\n");

    /* 1. 打印：能到这里就说明 syscall 入口、换栈、参数传递都通了 */
    fe_puts("  [1] syscall 打印正常，这是从 ring 3 发出来的\n");

    /* 2. 时间与睡眠 */
    u64 t0 = fe_clock_ns();
    fe_sleep_ms(10);
    u64 t1 = fe_clock_ns();
    fe_puts("  [2] 单调时钟可用，sleep(10ms) 实测约 ");
    fe_print_u64((t1 - t0) / 1000000ull);
    fe_puts(" ms\n");

    /* 3. 内存对象：分配 → 映射 → 读写校验 */
    long mo = fe_mem_alloc(8192);
    if (mo <= 0) {
        fe_puts("  [3] 内存对象分配失败\n");
    } else {
        u8 *p = (u8 *)fe_mem_map(mo, (void *)0, 8192, FE_PROT_READ | FE_PROT_WRITE);
        if (!p) {
            fe_puts("  [3] 内存对象映射失败\n");
        } else {
            for (usize i = 0; i < 8192; i++) {
                p[i] = (u8)(i * 31 + 7);
            }
            usize bad = 0;
            for (usize i = 0; i < 8192; i++) {
                if (p[i] != (u8)(i * 31 + 7)) {
                    bad++;
                }
            }
            g_memcheck_bad = bad;
            fe_puts("  [3] 共享内存映射到用户空间，8192 字节读写校验不一致数 = ");
            fe_print_u64(g_memcheck_bad);
            fe_puts("\n");
            /* 用一个只读的全局变量把结果带出去，避免编译器把校验循环优化掉 */
            g_memcheck_bad = bad;
        }
        fe_handle_close(mo);
    }

    /* 4. 创建第二个用户线程 */
    u64 tid = fe_thread_create(worker, (void *)0, (void *)0, 0);
    fe_puts("  [4] 创建用户线程，返回 id = ");
    fe_print_u64(tid);
    fe_puts("\n");

    /* 等它跑完（用户态没有 join，用 sleep 轮询） */
    for (int i = 0; i < 200 && !g_thread_ran; i++) {
        fe_sleep_ms(2);
    }
    if (g_thread_ran) {
        fe_puts("  [5] 用户线程已完成，它测得的 sleep 约 ");
        fe_print_u64(g_thread_slept_ms);
        fe_puts(" ms\n");
    } else {
        fe_puts("  [5] 用户线程没有完成\n");
    }

    fe_puts("=== 用户态程序结束，退出码 42 ===\n");
    return 42;
}
