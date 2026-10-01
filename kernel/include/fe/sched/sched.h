/* SPDX-License-Identifier: 0BSD */
/* 调度器。
 *
 * 策略：多级优先队列 + 同级时间片轮转。
 *   - 32 个优先级，每级一条 FIFO 就绪队列，用位图 O(1) 找到最高非空级别；
 *   - 每个线程有固定时间片，用尽后放回本级队尾，实现同级轮转；
 *   - 高优先级线程只要就绪就一定能抢占低优先级线程（严格优先级）。
 */
#ifndef FE_SCHED_SCHED_H
#define FE_SCHED_SCHED_H

#include <fe/types.h>
#include <fe/sched/thread.h>

/* 创建一个 ring 3（用户态）线程：它的初始现场会让 iretq 直接掉进用户态。
 * task 决定它跑在哪个地址空间、能用哪些句柄。 */
struct fe_thread *fe_thread_create_ring3(struct fe_task *task, u64 entry, u64 user_stack,
                                         void *arg, u64 kstack_size, u32 priority,
                                         const char *name);

/* 初始化：注册 yield/exit 中断、建立空闲线程、把当前执行流登记为 main 线程。
 * 此时调度还未开启，中断仍可正常处理。 */
void fe_sched_init(void);

/* 开启调度并让出 CPU（此后当前执行流就是一个普通线程） */
void fe_sched_start(void);

/* 是否已开启调度 */
bool fe_sched_running(void);

/* 由定时器中断调用：计时、唤醒睡眠线程、扣减时间片 */
void fe_sched_tick(void);

/* 由 isr.asm 在中断返回前调用。返回应当继续执行的栈指针
 * （无需切换时原样返回传入的 rsp）。 */
u64 fe_sched_maybe_switch(u64 rsp);

/* 请求重新调度（下一个中断返回点生效） */
void fe_sched_request(void);

/* 默认时间片（节拍数） */
void fe_sched_set_default_slice(u32 ticks);
u32  fe_sched_default_slice(void);

/* 让出 CPU：把当前线程放回队尾 */
void fe_sched_yield_current(void);

/* 把当前线程置为睡眠，直到指定时刻（**绝对纳秒**，来自 fe_time_ns()）。
 * 用时间而不是节拍数：节拍在虚拟机上会被突发投递，按"第 N 个节拍"
 * 唤醒会提前几倍返回（VBox 实测 sleep(50ms) 只用 14 ms）。 */
void fe_sched_sleep_until(u64 wake_ns);

/* 把一个线程置为阻塞（唤醒由其它机制负责，M3 的 IPC 用） */
void fe_sched_block_current(void);
void fe_sched_wake(struct fe_thread *t);

/* 回收僵尸线程（M2 由 join 直接回收，这里供后续的 reaper 用） */
u32 fe_sched_reap(void);

/* 回收所有僵尸线程，但跳过 keep（供 join 等待期间使用） */
void fe_sched_reap_except(struct fe_thread *keep);

void fe_sched_dump(void);

/* M2 调度器自检：返回失败项数（0 = 全部通过）。会创建并回收若干测试线程。 */
u32 fe_selftest_sched(void);

/* ★ 让出代价自检：**量一次让出要花几个节拍**（第 7 步的最小复现）★
 *
 * 判据是"让出 N 次所花的**节拍数**不超过 4N"——量的是时间，不是轮数
 * （轮数在被饿的情况下不是时间的度量，这是第 6 步的教训）。
 * 返回失败项数。 */
u32 fe_selftest_yield(void);

/* ---- 调度公平性的**计数**（不是打印）----
 *
 * ★ 为什么是计数器 ★ 串口是轮询输出、一行上百微秒到毫秒，而节拍是 1 ms；
 * 在调度路径上打印会**改变被测对象**（实测：加打印后同一段基准的切换数
 * 从 10 万涨到 24 万）。所以判据用只加几十条指令的计数器：
 *   picked  = 成功取到线程的次数（含"取到的就是当前线程"）
 *   fast    = 取到的就是当前线程 → 这次让出**没有换人**
 *   switched= 真的换了栈
 *   nullpick= 就绪位图为空 → 没人可换，当前线程继续跑
 *   idle_run= 选中空闲线程的次数
 *   tick_idle_higher_ready = CPU 落在空闲线程上、而更高优先级有人就绪的节拍数
 *                            （严格优先级下这个数**应当为 0**） */
void fe_sched_fairness(u64 *picked, u64 *fast, u64 *switched, u64 *nullpick,
                       u64 *idle_run, u64 *tick_idle_higher_ready);
void fe_sched_fairness_reset(void);

/* "队列里只有更低优先级的就绪者，于是让出退化成空操作"的次数。
 * 它应当随"有多少次让出是空转"增长——第 7 步修复的直接证据。 */
u64 fe_sched_kept_lower(void);
u64 fe_sched_idle_cand(void);

#endif /* FE_SCHED_SCHED_H */

/* TLS 自检：每线程独立的 %fs 基址（隔离性，含反向对照）。返回失败项数。 */
u32 fe_selftest_tls(void);
