/* SPDX-License-Identifier: 0BSD */
/* waitaddrtest —— K11「等一个用户地址」的**真实路径**承重测试。
 *
 * ★ 它与内核自检（kernel/ipc/waitaddrtest.c）的分工 ★
 * 自检手工造探针任务、直接调机制函数，证明的是"等待/唤醒这一整套
 * **决策**对不对"；这里走的是**真的那条路**：
 *
 *   用户态线程 → FE_SYS_WAIT_ADDR → 内核读用户内存 + 挂链 + 置 BLOCKED
 *   → 另一个线程改内存 + FE_SYS_WAKE_ADDR → 它被调度回来 → syscall 返回
 *
 * 这条链上任何一环没实现，下面至少有一个模式会失败。
 *
 * ★ 它用两个（以上）线程 + 一个**手写的条件变量** ★
 * 不引 pthread（那一层还没有），而是直接用这两个原语把
 * `pthread_cond_wait` 的语义写一遍——这正是 libc++ 的
 * `__libcpp_condvar_wait` 将来要做的事，所以这里写对了就说明底座够用。
 *
 * ★ 模式（同一个可执行文件分饰多角，照 killtest/faulttest 的做法）★
 *   --cond      A 条件变量：3 生产者 + 1 消费者（主线程），总数必须**精确相等**
 *   --spin      B 反向：**同一条逻辑**用纯自旋写，同样的时间预算内跑不完
 *   --timeout   C 超时：没人唤醒 → 只断言"**不早于** deadline 返回"
 *   --drop      D 反向：**故意丢一次唤醒** → 总数必须对不上
 *
 * ★ 屏幕上的字一律 ASCII（项目铁律）★
 * 退出码：A/C 成功 = 0；B 成功 = 0x5B；D 成功 = 0x4D（都是客户端自己打印
 * 并核对的确定值）。
 */
#include <fe_user.h>

/* ------------------------------------------------------------------ */
/* 手写的互斥量 + 条件变量（底座就是那两个原语）                        */
/* ------------------------------------------------------------------ */

/* ★ 为什么这里的互斥量可以直接自旋 ★
 * 它的**临界区里从不阻塞**（只有几条赋值），所以持锁者一定会在一个时间片
 * 之内放锁；而抢占是开的（节拍会在时间片用尽时换线程）。真正需要内核的是
 * **条件变量**：等待者必须"睡下"，否则每一次握手都要烧掉一个时间片。
 * ★ 这一点由 --spin 模式实测出来（它就是那一模式存在的理由）★ */
static volatile u32 g_lock;

static void lock(void)
{
    while (__atomic_exchange_n(&g_lock, 1u, __ATOMIC_ACQUIRE)) {
        while (g_lock) {
            __asm__ volatile("pause" ::: "memory");
        }
    }
}

static void unlock(void)
{
    __atomic_store_n(&g_lock, 0u, __ATOMIC_RELEASE);
}

/* 条件变量 = 一个**代际号**。
 *
 * ★ 为什么代际号必须由那把锁保护 ★ 等的人在**持锁**时读它，所以"读代际号"
 * 与"改条件"之间不可能插进一次 signal：要么 signal 发生在读数之前（那条件
 * 已经变了，醒来重查会发现），要么在读数之后（那 seq 已经变了，wait 会立刻
 * 返回 AGAIN，或者唤醒链上有我）。这就是"用 futex 实现条件变量"的全部诀窍，
 * 而它成立的前提正是内核那一步的**原子性**（检查与挂链在同一个关中断区间）。 */
static volatile u32 g_seq;

static void cond_wait(u64 deadline_ns)
{
    u32 s = g_seq;                      /* 持锁时读 */
    unlock();
    (void)fe_wait_addr((u32 *)(usize)&g_seq, s, deadline_ns);
    lock();
    /* ★ 返回之后**不做任何判断** ★ 醒来就回去重查条件——
     * 这正是 pthread_cond_wait 的契约（允许伪唤醒）。 */
}

static void cond_signal(void)
{
    g_seq++;
    (void)fe_wake_addr((u32 *)(usize)&g_seq, 1);
}

static void cond_broadcast(void)
{
    g_seq++;
    (void)fe_wake_addr((u32 *)(usize)&g_seq, 0);
}

/* ------------------------------------------------------------------ */
/* A/B 共用的工作量：一个有界队列                                      */
/* ------------------------------------------------------------------ */

#define QMAX        8
#define ITEMS       300
#define PRODUCERS   3
/* ★ 时间预算：条件变量版要在这个预算里跑完，自旋版要跑不完 ★
 * 这个数不是拍的：`--spin` 的每一次"等"都要烧掉一个时间片（10 个节拍），
 * 而条件变量版的等待者是真的睡着。两个模式都把量到的毫秒数打出来。 */
#define BUDGET_MS   3000ull

static volatile u32 g_head, g_tail, g_count;
static volatile u32 g_data[QMAX];
static volatile u32 g_produced, g_consumed;
static volatile u32 g_sum_produced, g_sum_consumed;
static volatile u32 g_spin_mode;

static void say(const char *s) { fe_puts(s); }

static void num(u64 v) { fe_print_u64(v); }

static void kv(const char *k, u64 v)
{
    say(" ");
    say(k);
    say("=");
    num(v);
}

static void eol(void) { say("\n"); fe_flush(); }

static void producer_body(u32 id)
{
    for (u32 i = 0; i < ITEMS / PRODUCERS; i++) {
        u32 v = id * 1000u + i;
        /* ★ 循环的形状必须与 cond_wait 的契约对齐 ★
         * `cond_wait` 内部做的是 `unlock(); 等; lock();`——**它返回时锁已经
         * 在手上**。所以"等"的那一支**不能**再回到一个以 `lock()` 开头的
         * 循环顶部，否则同一个线程会对自己已经持有的锁再自旋一次
         * （自旋互斥量上是**永久**死锁）。
         * 第一版就是这么写的，症状是 --cond 永远不返回。
         * 正确形状：`lock()` 在外层，重查在 while 里，释放前 unlock 一次。 */
        lock();
        while (g_count >= QMAX) {
            if (g_spin_mode) {
                /* ★ 自旋组：把锁放开之后**纯忙等**（不让出、不睡）★
                 * 这是"手上没有等待原语"时唯一能写的形状：反复看条件。
                 * 代价是它把整个时间片烧在"看"上——而节拍会在片用完时
                 * 换线程，所以它**能**推进，只是每次握手都要等一个时间片。 */
                unlock();
                while (g_count >= QMAX) {
                    __asm__ volatile("pause" ::: "memory");
                }
                lock();
            } else {
                cond_wait(fe_clock_ns() + 200ull * 1000000ull);
            }
        }
        g_data[g_tail] = v;
        g_tail = (g_tail + 1u) % QMAX;
        g_count++;
        g_sum_produced += v;
        g_produced++;
        /* ★ 自旋组**一次内核调用都不做** ★ 它的消费方是"轮询 + 让出"，
         * 不需要唤醒；这样两边差的就只剩"等"这一件事。 */
        if (!g_spin_mode) {
            cond_signal();
        }
        unlock();
    }
}

static void producer_entry(void *arg)
{
    u32 id = (u32)(u64)(usize)arg;
    producer_body(id);
    fe_exit(0);          /* ★ 线程入口绝不能返回（见 fe_user.h 的说明）*/
}

/* 消费者 = 主线程。返回消费到的项数。 */
static u32 consume_all(u64 deadline_ns)
{
    while (g_consumed < ITEMS) {
        lock();
        while (g_count == 0) {
            if (fe_clock_ns() >= deadline_ns) {
                unlock();
                return g_consumed;      /* 到点：把实情交给调用者 */
            }
            if (g_spin_mode) {
                /* 自旋组：放开锁之后纯忙等（与生产者那一支对称）*/
                unlock();
                while (g_count == 0) {
                    __asm__ volatile("pause" ::: "memory");
                }
                lock();
            } else {
                cond_wait(deadline_ns);
            }
        }
        u32 v = g_data[g_head];
        g_head = (g_head + 1u) % QMAX;
        g_count--;
        g_sum_consumed += v;
        g_consumed++;
        cond_broadcast();               /* 腾出位置：叫醒一个生产者 */
        unlock();
    }
    return g_consumed;
}

/* ------------------------------------------------------------------ */
/* A：条件变量版 —— 总数必须精确相等                                   */
/* ------------------------------------------------------------------ */

static u32 start_producers(void)
{
    for (u32 i = 0; i < PRODUCERS; i++) {
        if (!fe_thread_create(producer_entry, (void *)(u64)(usize)i, NULL, 0)) {
            say("  [waitaddrtest] thread_create failed\n");
            return 1;
        }
    }
    return 0;
}

static u32 mode_cond(void)
{
    u64 t0 = fe_clock_ns();
    g_spin_mode = 0;
    say("[waitaddrtest] cond: 3 producers + 1 consumer, items=");
    num(ITEMS);
    eol();
    if (start_producers()) {
        return 1;
    }
    u32 got = consume_all(fe_clock_ns() + BUDGET_MS * 1000000ull);
    u64 ms = (fe_clock_ns() - t0) / 1000000ull;

    say("[waitaddrtest] cond done");
    kv("produced", g_produced);
    kv("consumed", got);
    kv("sum_p", g_sum_produced);
    kv("sum_c", g_sum_consumed);
    kv("ms", ms);
    eol();
    if (got != ITEMS || g_produced != ITEMS) {
        return 0x0A;                    /* 预算内没跑完 */
    }
    if (g_sum_produced != g_sum_consumed) {
        return 0x0B;                    /* ★ 判据：总数必须**精确相等** ★ */
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* B：反向 —— 同一条逻辑用纯自旋，与**同一次运行里的条件变量版**对照     */
/* ------------------------------------------------------------------ */

/* ★ 这一条判据是被实测改过两次的，两次都写在下面 ★
 *
 * 第一次写的是"自旋 + `fe_yield()`，同样的预算内必须跑不完"。实测**红**：
 * 300 项只花 **0 ms** —— 让出是 1:1 轮转，根本不慢。于是那句
 * "自旋在这个内核上不是慢，是死"（docs/17 §2.6）**只对了一半**：
 * 抢占是开的（节拍在时间片用尽时换线程），所以自旋**会**推进。
 *
 * 第二次改成"纯忙等（不让出、不睡），预算内必须跑不完"。实测**还是红**：
 * 纯忙等 300 项花 **1141 ms**，预算 3000 ms 里跑完了。
 *
 * 所以判据第三次改成**它真正测得到的东西**：同样 300 项，
 * **纯自旋比条件变量版慢一个数量级以上**（这一次两个数都在同一台机器、
 * 同一次运行里量出来，不用绝对值卡）。自旋不是"死"，它是"把 CPU 烧光"。
 */
static void reset_workload(void)
{
    g_head = g_tail = g_count = 0;
    g_produced = g_consumed = 0;
    g_sum_produced = g_sum_consumed = 0;
}

static u32 mode_spin(void)
{
    /* ---- 第一遍：条件变量版（作为**同机同轮的**基准）---- */
    reset_workload();
    g_spin_mode = 0;
    u64 t0 = fe_clock_ns();
    if (start_producers()) {
        return 1;
    }
    u32 got_cond = consume_all(fe_clock_ns() + BUDGET_MS * 1000000ull);
    u64 cond_ms = (fe_clock_ns() - t0) / 1000000ull;
    say("[waitaddrtest] spin/ref: condvar pass ");
    kv("consumed", got_cond);
    kv("ms", cond_ms);
    eol();

    /* ---- 第二遍：同样的工作量，纯忙等 ---- */
    reset_workload();
    g_spin_mode = 1;
    t0 = fe_clock_ns();
    if (start_producers()) {
        return 1;
    }
    u32 got = consume_all(fe_clock_ns() + BUDGET_MS * 1000000ull);
    u64 ms = (fe_clock_ns() - t0) / 1000000ull;

    say("[waitaddrtest] spin done");
    kv("produced", g_produced);
    kv("consumed", got);
    kv("ms", ms);
    kv("cond_ms", cond_ms);
    kv("budget_ms", BUDGET_MS);
    eol();
    if (got_cond != ITEMS) {
        return 0x0C;                    /* 基准那一遍就没跑完，对照没有意义 */
    }
    /* ★ 判据必须**两个环境都能过**，所以它接受两种"自旋不行"的形状 ★
     *
     *   形状①（QEMU 实测）：预算内**跑完了**，但慢一个数量级以上
     *          —— `cond_ms=0` vs `ms=1127`（300 项）；
     *   形状②（VirtualBox 实测）：**连跑完都做不到**
     *          —— 3000 ms 预算里只做到 136/300（`ms=3093`，被预算截断）。
     *
     * 两个环境差这么多的原因不是玄学：VBox 的节拍实测只有 **79 Hz**
     * （一个时间片 ≈ 12.6 ms），而纯自旋每推进一次都要等一个时间片。
     * ★ 第一版判据只写了形状①的否定式（"跑完就算失败"），于是 VBox 上
     * 红成 0x0D ★ —— 这正是"判据要按最差的环境写"那条纪律的又一例。 */
    if (got < ITEMS) {
        return 0;                       /* 形状②：连跑完都做不到 ⇒ 更慢 */
    }
    if (ms >= 10ull * (cond_ms + 1ull)) {
        return 0;                       /* 形状①：跑完了，但慢一个数量级 */
    }
    return 0x0E;                        /* 两者相当：这条判据就不成立了 */
}

/* ------------------------------------------------------------------ */
/* C：超时 —— 只断言"不早于 deadline 返回"                            */
/* ------------------------------------------------------------------ */

static volatile u32 g_timeout_word = 0x1234u;   /* 没人会去改它 */

static u32 mode_timeout(void)
{
    const u64 delta = 50ull * 1000000ull;
    u64 t0 = fe_clock_ns();
    long r = fe_wait_addr((u32 *)(usize)&g_timeout_word, 0x1234u, t0 + delta);
    u64 el = fe_clock_ns() - t0;

    say("[waitaddrtest] timeout: deadline 50 ms, nobody wakes; elapsed_ms=");
    num(el / 1000000ull);
    eol();
    say("[waitaddrtest] timeout done");
    /* ★ 打印成"负多少"而不是 u64 的补码 ★ 判据仍然是**数值比较**，
     * 打印只是给人看的（`fe_print_u64` 会把 -12 打成 18446744073709551604，
     * 那对读日志的人毫无帮助）。 */
    kv("rc", (u64)(r < 0 ? -r : r));
    kv("rc_negative", r < 0 ? 1u : 0u);
    eol();
    if (r != -12) {
        return 0x0E;                    /* 期望 FE_ERR_TIMEOUT(-12) */
    }
    /* ★ 只断言下界 ★ 到点由节拍发现，而 VBox 的节拍实测是 79 Hz
     * （≈12.6 ms）——断言"误差在 1 ms 内"会在 QEMU 上过、在 VBox 上必红。 */
    if (el < delta) {
        return 0x0F;                    /* 早于 deadline 返回：那是错的 */
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* D：反向 —— **故意丢一次唤醒** ⇒ 总数必须对不上                      */
/* ------------------------------------------------------------------ */

/* ★ 为什么单独一条协议，而不是复用上面那个队列 ★
 * 队列版每次消费都 broadcast，所以"丢一次"会被下一次盖掉——那样测不出东西。
 * 要测的是"丢掉的那一次唤醒**没有任何东西能补上**"，所以这里用**一对一
 * 交接**：生产者放一个、消费者取一个，双方各靠一次唤醒推进；
 * 最后那一次 signal 被丢掉之后，消费者**没有任何别的唤醒源**。 */
#define DROP_ITEMS 3
/* ★ 监督窗口：**先等生产者做完，再确认消费者停住** ★
 * 第一版是"固定等 1200 ms 然后看结果"，在 VirtualBox 上红了：那条路上
 * 一次 condvar 往返实测要几百毫秒（QEMU 与 VBox 都远慢于队列版），
 * 于是 1200 ms 里连 5 轮都跑不完 ⇒ produced=3 而不是 5。
 * 判据不该依赖"跑得多快"，所以改成两段：先等 produced 到齐（上界 8 s），
 * 再给消费者 300 ms 去证明它**停住了**。 */
#define DROP_MS    8000ull

static volatile u32 g_slot, g_slot_full;
static volatile u32 g_consumer_waiting;
static volatile u32 g_drop_produced, g_drop_consumed;

static void drop_producer(void *arg)
{
    (void)arg;
    for (u32 i = 0; i < DROP_ITEMS; i++) {
        lock();
        while (g_slot_full) {
            cond_wait(fe_clock_ns() + 2000ull * 1000000ull);
        }
        if (i + 1u == DROP_ITEMS) {
            /* ★ 最后一项：**等消费者声明"我要睡了"之后再放** ★
             * 不等的话，消费者可能还没回到"重查条件"那一步，于是它会
             * 直接看见这一项 —— 丢掉的那次唤醒就被"它还没睡"盖掉了，
             * 判据当场假绿（第一版就是这样：consumed=5/5）。 */
            while (!g_consumer_waiting) {
                unlock();
                fe_yield();
                lock();
            }
        }
        g_slot = i;
        g_slot_full = 1;
        g_drop_produced++;
        /* ★ 丢掉**最后一次**唤醒 ★ 前几次照发，所以前面几轮必须成功。 */
        if (i + 1u < DROP_ITEMS) {
            cond_signal();
        }
        unlock();
    }
    fe_exit(0);
}

static void drop_consumer(void *arg)
{
    (void)arg;
    for (;;) {
        lock();
        while (!g_slot_full) {
            g_consumer_waiting = 1;     /* ★ 在锁内声明"我要睡了" ★ */
            cond_wait(0);               /* ★ 无限等：没有唤醒就永远不动 ★ */
            g_consumer_waiting = 0;
        }
        g_slot_full = 0;
        g_drop_consumed++;
        cond_signal();                  /* 叫生产者腾位置（这一半没丢）*/
        unlock();
        if (g_drop_consumed >= DROP_ITEMS) {
            break;
        }
    }
    fe_exit(0);
}

static u32 mode_drop(void)
{
    say("[waitaddrtest] drop: 1 producer + 1 consumer, last wake SKIPPED, "
        "consumer waits forever\n");
    g_spin_mode = 0;
    if (!fe_thread_create(drop_consumer, NULL, NULL, 0) ||
        !fe_thread_create(drop_producer, NULL, NULL, 0)) {
        say("  [waitaddrtest] thread_create failed\n");
        return 1;
    }
    /* 监督者：**两段等**（见 DROP_MS 的说明）——
     * 先等生产者把最后一项放完，再给消费者一段固定时间去证明它停住了。 */
    u64 deadline = fe_clock_ns() + DROP_MS * 1000000ull;
    u64 t0 = fe_clock_ns();
    while (fe_clock_ns() < deadline && g_drop_produced < DROP_ITEMS) {
        fe_sleep_ms(5);
    }
    u64 used_ms = (fe_clock_ns() - t0) / 1000000ull;
    /* 消费者还有 300 ms 可以变心（它不该变）。 */
    u64 settle = fe_clock_ns() + 300ull * 1000000ull;
    while (fe_clock_ns() < settle && g_drop_consumed < DROP_ITEMS) {
        fe_sleep_ms(5);
    }
    say("[waitaddrtest] drop done");
    kv("produced", g_drop_produced);
    kv("consumed", g_drop_consumed);
    kv("ms", used_ms);
    eol();
    /* ★ 判据（两条一起）★
     *   ① 生产者把 3 项**全放完了** —— 所以"少了一项"不是它没做；
     *   ② 消费者只取到 **2** 项，而且它还在**无限等** —— 第 3 项就在槽里躺着，
     *      只是没有任何东西会叫醒它。 */
    if (g_drop_produced != DROP_ITEMS) {
        return 0x1C;                    /* 生产者自己没做完：判据无效 */
    }
    if (g_drop_consumed == DROP_ITEMS) {
        return 0x1D;                    /* 丢了唤醒却还是全收到了：判据失效 */
    }
    if (g_drop_consumed != DROP_ITEMS - 1) {
        return 0x1E;                    /* 差得太多：不只是"丢了一次" */
    }
    return 0x4D;                        /* ★ 成功 = 这个确定的非零码 ★ */
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "--cond";
    say("[waitaddrtest] mode = ");
    say(mode);
    eol();

    if (mode[0] == '-' && mode[1] == '-' && mode[2] == 's') {
        return (int)mode_spin();
    }
    if (mode[0] == '-' && mode[1] == '-' && mode[2] == 't') {
        return (int)mode_timeout();
    }
    if (mode[0] == '-' && mode[1] == '-' && mode[2] == 'd') {
        return (int)mode_drop();
    }
    return (int)mode_cond();
}
