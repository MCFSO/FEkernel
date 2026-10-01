/* SPDX-License-Identifier: 0BSD */
/* 进程模型（学 Linux）。
 *
 * Linux 的形状是：可执行文件由**路径**标识；clone 造出新进程，execve 换掉映像；
 * 父进程用 wait 取子进程的退出状态。这里保留这套语义，只在两处按微内核改：
 *
 *   1. **没有全局 PID 表**。spawn 返回的是新任务的**句柄**（能力），
 *      父进程只能 wait 自己持有句柄的任务。Linux 的 pid 是全局可猜的整数，
 *      谁都能对别人 kill/wait；句柄不可伪造，这个问题就不存在。
 *   2. **没有 fork**。Linux 自己也在往 posix_spawn 上靠（glibc 就是用
 *      clone(CLONE_VM|CLONE_VFORK)+execve 实现的），而微内核里复制整个地址空间
 *      既昂贵又没什么用——服务之间共享数据走 IPC 与内存对象，不靠继承。
 *
 * 初始栈按 Linux x86-64 的约定摆放：
 *
 *     rsp → argc, argv[0..argc-1], NULL, envp[0..envc-1], NULL
 *
 * 字符串本身也在栈上（不能指向内核内存）。这样用户态 _start 的写法与 Linux
 * 完全一致，将来接 libc 不用改 ABI。
 */
#ifndef FE_PROCESS_H
#define FE_PROCESS_H

#include <fe/types.h>
#include <fe/status.h>
#include <fe/syscall.h>
#include <fe/task.h>            /* FE_TASK_NAME_MAX：候选现场要带新映像的名字 */

struct fe_task;
struct fe_thread;
struct fe_inode;
struct fe_address_space;
struct fe_vma_table;

/* argc/argv/envp 上限。取小值是有意的：内核要为它们准备定长缓冲，
 * 而真正的服务不需要几百个参数。超限返回 FE_ERR_RANGE 而不是截断——
 * 静默截断会让人以为是程序自己的 bug。 */
#define FE_ARGV_MAX 16
#define FE_ENVP_MAX 8
#define FE_ARG_MAX  128

/* ---------------- 候选现场：装载与"挂上任务"分开 ----------------
 *
 * ★ 为什么要分成两步 ★
 *
 * `exec` 与 `spawn` 干的是同一件事（把一份 ELF 变成"正在跑的程序"），
 * 但两者的**失败语义**完全不同：
 *
 *   spawn 失败 → 新任务销毁，什么都没有发生过；
 *   exec  失败 → **原程序必须完好无损、还得接着跑**（这是它可用的前提：
 *                shell 靠它执行命令，测试靠它测回滚）。
 *
 * 而装载路径上原来那三个函数是"顺手改任务对象"的（fe_elf_load 写 tls_*、
 * fe_user_map_stack 写 vmas/stack_low）。在 exec 里那是错的：一旦后面失败，
 * 原程序还活着，可它的 TLS 模板已经指向别人的映像了。
 *
 * 所以：**准备**阶段只往这个结构里写（不碰任何任务对象），全部成功后
 * 再由 `fe_task_attach_space` 一次性挂上去（**提交**）。
 *
 * 与 Linux 对照：这就是 `execve` 内部"先把新 mm 造好、再换掉旧的"那一段
 * （prepare_binprm / begin_new_exec 那一串），只是我们把边界写在了类型上。 */
struct fe_new_image {
    struct fe_address_space *as;        /* 候选地址空间（页表已建好） */
    struct fe_vma_table *vmas;          /* 候选区间表 */
    u64 stack_low;                      /* 栈已映射到的最低地址 */
    u64 user_map_next;                  /* mmap 分配指针（重置，不继承旧程序） */
    u64 user_stack_next;                /* 用户线程栈分配指针（同上） */
    u64 entry;                          /* 提交时要写进返回帧的 rip */
    u64 sp;                             /* 提交时要写进返回帧的 rsp */
    const void *tls_init;               /* 指向新映像内部的 PT_TLS 映像 */
    u64 tls_size;
    u64 tls_align;
    char name[FE_TASK_NAME_MAX];        /* 新映像的 basename（ps 要诚实） */
};

/* 按路径找到可执行的映像。三种错因三种错误码：
 *   FE_ERR_NOENT  路径不存在
 *   FE_ERR_ISDIR  那是个目录
 *   FE_ERR_NOTSUP 文件在，但没有可执行位（对应 Linux 的 ENOEXEC）
 * create 与 exec 共用它——这条规矩只有一份。 */
fe_status_t fe_image_lookup(const char *path, struct fe_inode **out_ino);

/* 准备候选现场：建栈 → 装载 ELF → 构造初始栈（argv 在用户栈上）。
 *
 * as/vmas 由调用者提供：新建进程用新任务自己的那一对（那个任务还没被任何人
 * 看见）；exec 自己新建一对，好让失败时整个扔掉。
 * 失败时半成品留在 as/vmas 里，由调用者按自己的所有权规则回收。
 * **本函数不碰任何任务对象。** */
fe_status_t fe_image_prepare(struct fe_new_image *img,
                             struct fe_address_space *as, struct fe_vma_table *vmas,
                             struct fe_inode *ino, const char *path,
                             char *const argv[], u32 argc);

/* 提交：把候选现场挂到任务上（space/vmas/三个分配指针/TLS 三元组，一次挂全）。
 *
 * ★ 它是"赋值"而不是"换一个" ★ 旧空间与旧区间表的释放由调用者自己做，
 * 而且必须在**换完 CR3 之后**做——顺序反了就是"释放正在跑的页表"
 * （症状见 kernel/sched/sched.c 里那段 K2/K3 的注释）。 */
void fe_task_attach_space(struct fe_task *t, const struct fe_new_image *img);

/* 按路径创建进程并立刻投入运行。
 *
 * argv 指向的是**内核内存**里的字符串（系统调用负责先把用户参数拷进来），
 * 或者内核自己的静态字符串（启动 init 时）。argv 可以为 NULL（等价于 argc=0，
 * 此时程序自己的路径会作为 argv[0] 补上，与 Linux 一致）。
 *
 * ★ boot_init：这是不是引导者 init ★
 * 它必须作为参数传进来，而不能等返回之后再补记。原因是**新线程在
 * 这个函数返回之前就已经可运行了**（fe_thread_create_ring3 里 rq_push
 * 把它挂上就绪队列），于是调用方"创建完之后再记一笔"是一个竞态：
 * 只要那一刻发生一次抢占，新任务就会在"内核还没记下它是谁"的时候跑起来。
 * 实测症状（VirtualBox 上稳定复现、QEMU 上不出现）：init 认领设备管理器
 * 身份被拒 → 后续 in 指令 #GP → 整个用户态没起来，而内核日志一切正常。
 * 传进来之后，内核可以在**线程被创建之前**记下它。
 *
 * 成功返回新任务对象（引用计数已加一，调用方负责 unref），
 * 并通过 out_thread 返回它的主线程（可能为 NULL，表示调用方不关心）。
 * 失败返回 NULL，*out_status 给出原因。 */
struct fe_task *fe_process_create(const char *path, char *const argv[], u32 argc,
                                  fe_status_t *out_status, struct fe_thread **out_thread,
                                  bool boot_init);

/* spawn：创建进程，并把它的任务句柄装进 parent 的句柄表。
 * 这就是用户态 fe_spawn 的实现，也是 init 拉起各驱动服务的路径。 */
fe_status_t fe_process_spawn(struct fe_task *parent, const char *path,
                             char *const argv[], u32 argc, fe_handle_t *out_handle);

/* wait：等 parent 持有句柄的那个任务结束，取回退出码。
 * 与 Linux 的 wait4 一样会阻塞；不同之处是它只认句柄，不认 pid。 */
fe_status_t fe_process_wait(struct fe_task *parent, fe_handle_t task_handle,
                            i32 *out_status);

/* 主线程退出时由调度器通知（进程结束 → 唤醒正在 wait 的父进程）。
 * 在中断上下文里被调用，因此只置标志 + 唤醒，不做任何分配。 */
void fe_process_on_thread_exit(struct fe_thread *t);

/* 终止一个任务（K2）：它的所有线程都会在**取消点**上结束，
 * 此后不会再有任何线程回到用户态。**异步**——返回时只保证标记完成，
 * 不保证线程已死透；要确定没了就 fe_process_wait 那个任务句柄。
 *
 * ★ 调用者必须先按 FE_RIGHT_TERMINATE 校验句柄 ★
 * 这个函数只管机制，不管权限：权限是句柄表的事（见 syscall.c 的
 * sys_task_terminate）。把两者分开，是为了让"谁能杀"只有一个答案。
 *
 * 设计与代价（为什么不能就地拔掉一个线程、取消点在哪）见
 * docs/13-tasks-and-kill.md §3。 */
void fe_task_terminate(struct fe_task *task);

/* ---- 2a：闸门从任务级扩到线程级（docs/13-tasks-and-kill.md §6.2/§6.4）---- */

/* 等"别的线程都死透"的上限：200000 次让出。
 * ★ 与 fe_thread_join 的 2,000,000（sched.c）同族的保守值，但小一个数量级
 * ★ exec 是一次系统调用，不能像 join 那样"等到天荒地老"。
 * 1000 Hz 节拍下，200000 次让出至少覆盖数十秒的墙钟时间。 */
#define FE_EXEC_KILL_ROUNDS 200000u

/* ★ 叫停本任务里除 keep 之外的每一个线程，然后立刻返回（**异步**）★
 *
 * 对每个受害者：`kill_pending = true` → 若它在 BLOCKED/SLEEPING 就摘登记 +
 * 唤醒（就绪的什么都不用做：它们下次回用户态时会被闸门拦下）。
 * 返回被标记的线程数。
 *
 * ★ keep 必须是本任务的主线程（task->main_thread）★
 * 闸门对**当前线程**同样成立，所以调用者自己必须在名单之外；"调用者不是
 * 主线程就拒绝"是 exec 那一层（15-exec.md §7 的 FE_ERR_INVAL）保证的前提，
 * 而它背后是"主线程退出即进程结束"这条语义（fe_process_on_thread_exit
 * 只看主线程）。放宽它是一次语义改动，不是放开一个判断。 */
u32 fe_task_kill_other_threads(struct fe_task *task, struct fe_thread *keep);

/* ★ 等"别的线程都死透"★ 判据是 `fe_task_thread_count(task) <= 1`
 * （调用者自己还活着，它就是那一个）——**不能**用调用者手里那个线程指针
 * 的 state：受害者 reap_ok=true，空闲线程会在任意一次让出之后把它 free，
 * 裸指针立刻悬空。
 *
 * 返回 FE_OK = 死透了；FE_ERR_TIMEOUT = 在 max_rounds 次让出内没等到。
 *
 * ★ 绝不 panic ★ 这条路径用户态可触发，内核 panic 等于把它变成 DoS。
 * 代价：**kill_pending 不撤回**——超时之后进程已经残缺，调用者不能再
 * 把它当完整的用（见 docs/13-tasks-and-kill.md §6.4）。 */
u32 fe_task_wait_others_dead(struct fe_task *task, u32 max_rounds);

/* 超时日志：一行头 + 每个仍然活着且不是 keep 的线程一行（name/id/state 数值/
 * 它登记的等待点）+ 一行明说"这个进程已经残缺"。 */
void fe_task_dump_stuck_threads(struct fe_task *task, struct fe_thread *keep);

/* 内核从 cmdline 解析出的引导槽（'a' 或 'b'；没有 cmdline 时是 'a'）。
 * 它同时决定 exec 哪个 init、以及 A/B 访问矩阵里哪个槽"正在运行"。 */
char fe_boot_slot(void);

/* 内核侧启动 init：等价于 Linux 的 kernel_execve("/init")。
 * 只在内核启动流程里调用一次；返回 init 的退出码，启动失败返回负数。 */
i32 fe_process_start_init(void);

u32 fe_selftest_process(void);

/* 进程终止（K2）的自检：三条死亡路径 + 反向对照。返回失败项数。 */
u32 fe_selftest_kill(void);

#endif /* FE_PROCESS_H */
