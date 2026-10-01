/* SPDX-License-Identifier: 0BSD */
/* 任务/线程快照（K3）：把内核里的任务与线程状态拼成一张**定长记录**的表。
 *
 * ★ 为什么这段代码从 syscall.c 里搬出来 ★
 * 它原来整个长在 sys_task_list 里，而那个函数只接受**用户指针**。
 * 于是内核自检**没法测它**——自检手里只有内核地址，`fe_copy_to_user`
 * 会拒绝。结果是这段代码在 D4 那一轮"写完就算完"，没有任何东西跑过它，
 * 而它当时带着一个真实的布局错误（步长 48 vs 结构体 64，见下）。
 *
 * 现在拆成两层：
 *   fe_task_snapshot_build()  —— 只认"一块缓冲"，内核内存也行；
 *   sys_task_list()           —— 校验用户指针 + 一次拷贝。
 * 自检直接调前者，测的就是**真正在跑的那份代码**，不是它的复制品。
 *
 * ★ 布局：表头 + 定长任务记录 + 紧凑线程记录 ★
 * 不是"C 结构体数组"：内核必须能在**不知道用户态结构体布局**的前提下
 * 按固定步长填表。两边的一致性靠 _Static_assert 钉住（见 fe/syscall.h
 * 与 user/include/fe_user.h 里的那两组断言）——这是实测教训：
 * 步长宏曾经与结构体差 16 字节，而没有任何东西会报错。
 *
 * ★ 表头里的 total_* 是**真实总数** ★
 * cap 是调用者给的容量。填不下时如实报出总数，调用者据此知道自己
 * 看到的是不是全部——而不是"填了 32 条就以为系统里只有 32 个任务"。
 */
#include <fe/task.h>
#include <fe/sched/thread.h>
#include <fe/object.h>
#include <fe/syscall.h>
#include <fe/string.h>
#include <fe/types.h>

#define FE_TASK_INFO_BYTES   FE_TASK_STRIDE_X
#define FE_THREAD_INFO_BYTES FE_THREAD_STRIDE_X
#define FE_LIST_HDR_BYTES    FE_TASK_LIST_HDR_X

static void fill_task_info(u8 *dst, const struct fe_task *t, u32 thread_base,
                           u32 thread_count, u64 cpu_ticks)
{
    for (u32 i = 0; i < FE_TASK_INFO_BYTES; i++) {
        dst[i] = 0;
    }
    /* ★ 逐字段 memcpy 而不是强转赋值 ★
     * 用户态结构体在内核眼里只是"64 个字节"，写入位置由**偏移常量**
     * 决定（与 fe_user.h 的 layout 一一对应）。用强转会在将来某次
     * 字段调整时静默错位——而"字段莫名其妙是 0"正是那种错的表现。
     *
     * ★ 每个偏移旁边都写着它对应哪个字段 ★
     * 这一组偏移曾经全部正确、而"每条记录多长"是错的——两者是独立的
     * 两件事，所以核对时也要分开核：偏移靠这里逐个写明，
     * 长度靠 _Static_assert。 */
    u64 id = t->id;
    u64 parent = 0;                 /* 今天没有父子链：0 = 没有父任务 */
    u64 ticks = cpu_ticks;
    u32 tc = thread_count;
    u32 tb = thread_base;
    u32 gp = t->granted_ports;
    u32 hc = fe_handle_table_used(&t->handles);
    u8 exited = t->exited ? 1 : 0;
    u8 is_kernel = t->space ? 0 : 1;
    memcpy(dst + 0, &id, 8);            /* id */
    memcpy(dst + 8, &parent, 8);        /* parent_id */
    memcpy(dst + 16, &tc, 4);           /* thread_count（存活） */
    memcpy(dst + 20, &tb, 4);           /* thread_base */
    memcpy(dst + 24, &gp, 4);           /* granted_ports */
    memcpy(dst + 28, &hc, 4);           /* handle_count */
    memcpy(dst + 32, &ticks, 8);        /* cpu_ticks（本任务累计） */
    memcpy(dst + 40, &exited, 1);       /* exited */
    memcpy(dst + 41, &is_kernel, 1);    /* is_kernel */
    /* dst+42..47 是显式填充（用户态结构体里那 6 个 _pad 字节） */
    for (u32 i = 0; i < FE_TASK_NAME_MAX_X && t->name[i]; i++) {
        dst[48 + i] = (u8)t->name[i];   /* name[16]，偏移 48..63 */
    }
}

i64 fe_task_snapshot_build(u8 *buf, u64 buf_len, u32 task_cap, u32 thread_cap,
                           u64 caller_task_id)
{
    if (!buf) {
        return FE_ERR_INVAL;
    }
    if (task_cap == 0 || task_cap > FE_TASK_LIST_MAX ||
        thread_cap > FE_TASK_LIST_MAX) {
        return FE_ERR_INVAL;
    }
    u64 need = FE_LIST_HDR_BYTES +
               (u64)task_cap * FE_TASK_INFO_BYTES +
               (u64)thread_cap * FE_THREAD_INFO_BYTES;
    if (buf_len < need) {
        /* 如实报错，不静默填一半：调用者拿到"一半是这次、一半是上次"的表
         * 是看不出来的。 */
        return FE_ERR_NOSPC;
    }

    /* 第一遍：数总数（用户态靠它判断有没有被截断） */
    u64 total_tasks = 0, total_threads = 0;
    for (struct fe_task *t = fe_task_first(); t; t = t->next) {
        total_tasks++;
        total_threads += fe_task_thread_count(t);
    }

    for (u32 i = 0; i < FE_LIST_HDR_BYTES; i++) {
        buf[i] = 0;
    }

    u32 ntask = 0, nthr = 0;
    for (struct fe_task *t = fe_task_first(); t && ntask < task_cap; t = t->next) {
        u32 base = nthr;
        u64 ticks = 0;
        u32 cnt = 0;
        struct fe_thread *th = fe_task_thread_first(t);
        for (; th && nthr < thread_cap; th = fe_thread_next_of(th)) {
            u8 *d = buf + FE_LIST_HDR_BYTES +
                    (u64)task_cap * FE_TASK_INFO_BYTES +
                    (u64)nthr * FE_THREAD_INFO_BYTES;
            for (u32 k = 0; k < FE_THREAD_INFO_BYTES; k++) {
                d[k] = 0;
            }
            u64 tid = th->id;
            u64 tticks = th->cpu_ticks;
            u64 nsw = th->switches;
            u32 prio = th->priority;
            u32 state = th->state;
            i32 slice = th->slice;
            i32 code = th->exit_code;
            u32 idx = cnt;
            /* ★ 主线程这个事实必须**显式**表达 ★
             * 原来靠"index 0 = 主线程"，而那是假的：线程枚举走全局链
             * （新建的插在表头），下标 0 是最近创建的那个。 */
            u32 flags = (th == t->main_thread) ? FE_THREAD_FLAG_MAIN : 0;
            memcpy(d + 0, &tid, 8);         /* id */
            memcpy(d + 8, &tticks, 8);      /* cpu_ticks */
            memcpy(d + 16, &nsw, 8);        /* switches */
            memcpy(d + 24, &prio, 4);       /* priority */
            memcpy(d + 28, &state, 4);      /* state */
            memcpy(d + 32, &slice, 4);      /* slice */
            memcpy(d + 36, &code, 4);       /* exit_code */
            memcpy(d + 40, &idx, 4);        /* index */
            memcpy(d + 44, &flags, 4);      /* flags */
            for (u32 k = 0; k < FE_TASK_NAME_MAX_X && th->name[k]; k++) {
                d[48 + k] = (u8)th->name[k];    /* name[16]，偏移 48..63 */
            }
            ticks += tticks;
            cnt++;
            nthr++;
        }
        u8 *d = buf + FE_LIST_HDR_BYTES + (u64)ntask * FE_TASK_INFO_BYTES;
        fill_task_info(d, t, base, cnt, ticks);
        ntask++;
    }

    /* 表头：task_count/thread_count 是**实际写入**的，
     * total_* 是内核里的真实总数——两者不等就说明被截断了。 */
    {
        u32 v32;
        u64 v64;
        u32 pad[2] = { 0, 0 };
        v32 = ntask;         memcpy(buf + 0, &v32, 4);   /* task_count */
        v32 = nthr;          memcpy(buf + 4, &v32, 4);   /* thread_count */
        v64 = total_tasks;   memcpy(buf + 8, &v64, 8);   /* total_tasks */
        v64 = total_threads; memcpy(buf + 16, &v64, 8);  /* total_threads */
        v64 = caller_task_id; memcpy(buf + 24, &v64, 8); /* current_task */
        v32 = task_cap;      memcpy(buf + 32, &v32, 4);  /* tasks_cap */
        v32 = thread_cap;    memcpy(buf + 36, &v32, 4);  /* threads_cap */
        memcpy(buf + 40, pad, 8);
    }
    return (i64)ntask;
}
