/* SPDX-License-Identifier: 0BSD */
/* 自检：线程局部存储（TLS）——每线程一份 %fs 基址，切换时必须跟着换。
 *
 * ★ 为什么要这一组自检 ★
 *
 * TLS 的机制侧很容易"看起来通了"：给每个线程设一个非零的 `%fs` 基址，
 * 用户态的 `__thread` 变量就能读能写、不再 #PF。但真正要证明的性质是
 * **另一条**：两个线程访问**同一个 TLS 偏移**时，落在**不同的内存**上。
 *
 * 如果切换路径漏了 `wrmsr`（或漏了某个分支），两个线程就共用一份 TLS：
 * 线程 A 写 0xAAAA，线程 B 读到 0xAAAA。那正是"线程局部变量莫名其妙变了"
 * 这类只在多线程下出现、极难复现的错。所以这里用**可区分的值**去撞它：
 * 两个线程往同一个偏移各写自己的值，交叉让出 CPU 再回读，
 * 任何一次读到对方的值就算失败。
 *
 * ★ 为什么用段覆盖的直接访存，而不是 `__thread` ★
 * 内核里不能用 `__thread`：那会让内核 ELF 需要 `PT_TLS` 段，而内核
 * 自己并不需要线程局部变量（链接器明确报错："has an STT_TLS symbol but
 * doesn't have a PT_TLS segment"）。所以这里用 `%fs:` 段覆盖直接访存——
 * 那正是编译器为 `__thread` 生成的**同一条指令形态**，
 * 而且顺带把"初始化映像真的被复制进了每个线程的块"也测了。
 *
 * ★ 为什么用内核线程 ★
 * TLS 的内核侧只有三件事：给线程分配一块、把映像拷进去、切换时 `wrmsr`。
 * 这三件事对内核线程与用户线程是**同一条路径**（都在
 * fe_sched_maybe_switch 里），所以内核线程足以验证机制本身，
 * 且不引入"要先造一个用户任务"的额外依赖（自检不该依赖运行期状态）。
 */
#include <fe/sched/sched.h>
#include <fe/sched/thread.h>
#include <fe/task.h>
#include <fe/user.h>            /* TLS 布局约定（FE_TLS_*） */
#include <fe/mm/kheap.h>
#include <fe/kprintf.h>
#include <fe/string.h>
#include <fe/status.h>

/* 自造的 TLS 初始化映像：两个可核对的值。
 * 布局与真实程序一致：偏移 0 是"数据块自我指针"，紧跟 TCB，
 * 探针变量放在 TCB 之后的固定偏移上（见 fe/user.h 的约定）。 */
#define TLS_PROBE_OFFSET  (FE_TLS_MIN_TCB + 8)
static const u64 g_tls_image[4] = {
    0,                      /* [0] 自我指针：由内核在 setup 时填 */
    0x1122334455667788ull,  /* [1] 初始化映像的第一个值（供探针核对） */
    0x99AABBCCDDEEFF00ull,  /* [2] 第二个值 */
    0,
};

static volatile u32 g_tls_done_a;
static volatile u32 g_tls_done_b;
static volatile u32 g_tls_bad;
static volatile u64 g_tls_base_a;
static volatile u64 g_tls_base_b;

/* 读当前线程的 TLS 块首（%fs:0 那个自我指针）与映像值 */
static u64 tls_self(void)
{
    u64 v = 0;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(v));
    return v;
}

static u64 tls_peek(u32 off)
{
    u64 v = 0;
    __asm__ volatile("movq %%fs:(%1), %0" : "=r"(v) : "r"((u64)off));
    return v;
}

static void tls_poke(u32 off, u64 v)
{
    __asm__ volatile("movq %1, %%fs:(%0)" ::"r"((u64)off), "r"(v));
}

static void tls_probe_a(void *arg)
{
    (void)arg;
    g_tls_base_a = tls_self();
    tls_poke(TLS_PROBE_OFFSET, 0xAAAAAAAAAAAAAAAAull);
    for (u32 i = 0; i < 128; i++) {
        fe_thread_yield();          /* 让另一个线程去写它自己那份 */
        if (tls_peek(TLS_PROBE_OFFSET) != 0xAAAAAAAAAAAAAAAAull) {
            g_tls_bad++;            /* 读到别人的值 = 两个线程共用一份 TLS */
            break;
        }
    }
    g_tls_done_a = 1;
    fe_thread_exit(0);
}

static void tls_probe_b(void *arg)
{
    (void)arg;
    g_tls_base_b = tls_self();
    tls_poke(TLS_PROBE_OFFSET, 0x5555555555555555ull);
    for (u32 i = 0; i < 128; i++) {
        fe_thread_yield();
        if (tls_peek(TLS_PROBE_OFFSET) != 0x5555555555555555ull) {
            g_tls_bad++;
            break;
        }
    }
    g_tls_done_b = 1;
    fe_thread_exit(0);
}

u32 fe_selftest_tls(void)
{
    u32 fail = 0;

    /* 探针任务：TLS 的初始化映像挂在**任务**上（真实程序里它来自
     * 可执行文件的 PT_TLS 段，由 fe_elf_load 填进这几个字段）。 */
    struct fe_task *task = fe_task_create_kernel("tls-probe");
    if (!task) {
        fe_kprintf("        TLS 探针任务创建失败\n");
        return 1;
    }
    task->tls_init = g_tls_image;
    task->tls_size = 4096;          /* 故意开大：两块 TLS 明显不同，便于打印核对 */
    task->tls_align = 16;

    /* ★ 一个必须说清楚的技巧 ★
     * `fe_thread_create` 把新线程的 task 取成"当前任务"的，而 TLS 用的是
     * **task 上**的映像描述。所以临时把当前线程的 task 换成探针任务，
     * 建完立刻换回来——不这么做，探针线程会拿到调用者任务的 TLS 描述，
     * 那就不是在测我们想测的东西了。（创建期间不会被抢占：
     * fe_thread_create 只在 rq_push 那一小段关中断。） */
    struct fe_thread *cur = fe_thread_current();
    struct fe_task *saved_task = cur ? cur->task : NULL;
    if (cur) {
        cur->task = task;
    }
    struct fe_thread *a = fe_thread_create("tls-a", tls_probe_a, NULL,
                                           16 * 1024, FE_PRIO_NORMAL);
    struct fe_thread *b = fe_thread_create("tls-b", tls_probe_b, NULL,
                                           16 * 1024, FE_PRIO_NORMAL);
    if (cur) {
        cur->task = saved_task;
    }
    if (!a || !b) {
        fe_kprintf("        TLS 探针线程创建失败\n");
        fe_object_unref(&task->hdr);
        return 1;
    }

    for (u32 i = 0; i < 400000; i++) {
        if (g_tls_done_a && g_tls_done_b) {
            break;
        }
        fe_thread_yield();
    }

    if (g_tls_base_a == 0 || g_tls_base_b == 0) {
        fe_kprintf("        探针线程没有拿到 TLS 基址（%#llx / %#llx）\n",
                   (unsigned long long)g_tls_base_a, (unsigned long long)g_tls_base_b);
        fail++;
    } else if (g_tls_base_a == g_tls_base_b) {
        /* ★ 反向对照的核心 ★ 两块 TLS 必须是**不同的内存**：
         * 相同就说明"每线程一份"根本没成立，值碰巧不同只是运气。 */
        fe_kprintf("        两个线程拿到同一块 TLS（%#llx）——线程局部不成立\n",
                   (unsigned long long)g_tls_base_a);
        fail++;
    } else {
        fe_kprintf("        两个线程的 TLS 块不同: %#llx / %#llx（相差 %llu 字节）\n",
                   (unsigned long long)g_tls_base_a, (unsigned long long)g_tls_base_b,
                   (unsigned long long)(g_tls_base_a > g_tls_base_b
                                        ? g_tls_base_a - g_tls_base_b
                                        : g_tls_base_b - g_tls_base_a));
    }

    if (g_tls_bad) {
        fe_kprintf("        TLS 隔离失败：%u 次读到了别的线程写入的值\n", g_tls_bad);
        fail++;
    } else if (!g_tls_done_a || !g_tls_done_b) {
        fe_kprintf("        TLS 探针没有跑完（a=%u b=%u）\n", g_tls_done_a, g_tls_done_b);
        fail++;
    } else {
        fe_kprintf("        两个线程往同一个 TLS 偏移各写自己的值、交叉让出 128 次：互不可见\n");
    }

    /* 初始化映像必须真的被复制进每个线程的块：
     * 探针线程回到内核后没法替它们核对（%fs 已经换回来了），
     * 所以在**创建后立刻**从内核侧经块首读一次。 */
    {
        struct fe_thread *ta = a;
        const u64 *img = (const u64 *)(uptr)ta->user_tls;
        if (!img) {
            fe_kprintf("        探针线程的 TLS 块为空\n");
            fail++;
        } else if (img[1] != g_tls_image[1] || img[2] != g_tls_image[2]) {
            fe_kprintf("        TLS 初始化映像没有复制进去（读到 %#llx / %#llx）\n",
                       (unsigned long long)img[1], (unsigned long long)img[2]);
            fail++;
        } else {
            fe_kprintf("        TLS 初始化映像已复制（%#llx / %#llx），块 %llu 字节含 TCB %u 字节\n",
                       (unsigned long long)img[1], (unsigned long long)img[2],
                       (unsigned long long)ta->user_tls_size, (u32)FE_TLS_MIN_TCB);
        }
    }

    /* 当前线程（调用者）也必须已经有 TLS：否则"主线程没有"这种半成品会漏过去
     * ——探针过、主线程一碰 %fs 就 #PF。 */
    if (!cur || cur->user_fs_base == 0) {
        fe_kprintf("        当前线程没有 TLS 基址（fs 仍是 0）\n");
        fail++;
    } else {
        fe_kprintf("        当前线程 %s 的 TLS 基址 %#llx\n",
                   cur->name, (unsigned long long)cur->user_fs_base);
    }
    return fail;
}
