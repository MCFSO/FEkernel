/* SPDX-License-Identifier: 0BSD */
/* FEKernel 用户态运行库（libfe）的公共接口。
 *
 * 这一层是「自定义精简 syscall ABI」在用户侧的投影：不模仿 POSIX，只提供内核
 * 真正愿意承诺的最小集合。类型与常量与内核头文件保持一致（见 kernel/include/fe/syscall.h）。
 */
#ifndef FE_USER_H
#define FE_USER_H

/* 只有 va_list 需要它。用 clang 的 freestanding 头，不依赖任何 C 库。 */
#include <stdarg.h>

/* ★ 基础类型只有一处定义（fe_base.h）★
 * 路线 B 引入 POSIX 头之后，如果再各自 typedef 一份，
 * 迟早会出现"两边的 u64 不是同一个类型"。见 fe_base.h 的说明。 */
#include <fe_base.h>

/* 没有 libc，NULL 得自己给——fe_base.h 里已经给了。 */

#define FE_HANDLE_INVALID 0u

/* ---------------- 用户态线程局部存储（TLS）的布局约定 ----------------
 *
 * ★ 这个数字在两边各写一次，必须一致 ★
 * 内核侧的同名常量在 kernel/include/fe/user.h。为什么不放进一个共享头：
 * 内核头不包含用户头、用户头也不包含内核头（这是分层），所以这条约定
 * 只能"两边各写一次 + 两边都写清为什么"。数值不一致的症状是
 * "线程局部变量读到另一个线程的值"——极难定位，所以：
 *
 *   %fs:小偏移 → 指向 TLS 数据块起始（编译器生成的访存序列就是这么取的）
 *   数据块      → .tdata（可执行文件里的初始化映像）+ .tbss（零）
 *
 * 实测过的访存序列（clang，`__thread int errno`）：
 *     mov %fs:0x0, %rcx
 *     movl $2, (%rcx,%rax)      ← 变量按各自偏移落在数据块里
 * 所以 `%fs:0` 那个位置**必须存着数据块的地址**，而不是变量本身。 */
#define FE_TLS_SELF_OFFSET 0u

/* ★ 错误码不再手抄一份 ★
 * 原来这里抄了 12 个，漏了 FE_ERR_PIPE / FE_ERR_IO / FE_ERR_KILLED 等——
 * 而"对端已关闭"正是服务客户端最常遇到的那个错误。
 * 现在直接包含内核那份：fe/errno.h 是内核与用户态共享的唯一来源。 */
#include <fe/errno.h>

/* 权限位 */
#define FE_PROT_READ  1u
#define FE_PROT_WRITE 2u
#define FE_PROT_EXEC  4u

/* 内存对象标志 */
#define FE_MEM_FLAG_DMA 1u

/* 系统调用号（与 kernel/include/fe/syscall.h 一致） */
#define FE_SYS_DEBUG_WRITE      0x00
#define FE_SYS_THREAD_EXIT      0x01
#define FE_SYS_THREAD_YIELD     0x02
#define FE_SYS_THREAD_CREATE    0x03
#define FE_SYS_SLEEP            0x04
#define FE_SYS_CLOCK_MONOTONIC  0x05
#define FE_SYS_ENDPOINT_CREATE  0x10
#define FE_SYS_ENDPOINT_SEND    0x11
#define FE_SYS_ENDPOINT_RECV    0x12
#define FE_SYS_ENDPOINT_CALL    0x13
#define FE_SYS_NOTIFICATION_CREATE 0x14
#define FE_SYS_NOTIFICATION_WAIT   0x15
#define FE_SYS_NOTIFICATION_SIGNAL 0x16
#define FE_SYS_MEM_ALLOC        0x20
#define FE_SYS_MEM_MAP          0x21
#define FE_SYS_MEM_UNMAP        0x22
#define FE_SYS_MEM_INFO         0x23
#define FE_SYS_HANDLE_CLOSE     0x30
#define FE_SYS_HANDLE_DUP       0x31
#define FE_SYS_IRQ_REGISTER     0x40
#define FE_SYS_IOPORT_REQUEST   0x41
#define FE_SYS_MMIO_MAP         0x42
#define FE_SYS_IRQ_ACK          0x43
#define FE_SYS_PROCESS_SPAWN    0x50
#define FE_SYS_PROCESS_WAIT     0x51
#define FE_SYS_RESOURCE_LOCK    0x44
#define FE_SYS_RESOURCE_UNLOCK  0x45
#define FE_SYS_DEVFS_PUBLISH    0x60
#define FE_SYS_DEVFS_OPEN       0x61
#define FE_SYS_PROTECT_ADD      0x70
#define FE_SYS_PROTECT_CHECK    0x72
#define FE_SYS_PROTECT_LIST     0x73
#define FE_SYS_PROTECT_STAT     0x74
#define FE_SYS_AB_INFO          0x80
#define FE_SYS_CMDLINE          0x81
#define FE_SYS_REBOOT           0x82
#define FE_SYS_FB_INFO          0x83
#define FE_SYS_TASK_LIST        0x84
#define FE_SYS_PF_STAT          0x85
#define FE_SYS_RESOURCE_POOL_ADD 0x86
#define FE_SYS_RESOURCE_GRANT    0x87
#define FE_SYS_RESOURCE_GRANT_ID 0x88
#define FE_SYS_DEVMGR_CLAIM      0x89
#define FE_SYS_DEVMGR_RELEASE    0x8A
#define FE_SYS_RESOURCE_RELEASE  0x8B
#define FE_SYS_TASK_TERMINATE    0x8C
#define FE_SYS_CLOCK_INFO        0x8D
#define FE_SYS_IRQ_MSI_ALLOC     0x8E
#define FE_SYS_IRQ_MSI_FREE      0x8F

/* 资源类别（与内核 fe/syscall.h 一致） */
#define FE_RES_IOPORT 1u
#define FE_RES_MMIO   2u
#define FE_RES_IRQ    3u

/* 一条 IPC 消息的**内联载荷**上限（与内核 fe/ipc.h 的 FE_MSG_MAX_PAYLOAD 一致）。
 *
 * 客户端必须知道这个数字：它决定了"一次调用最多能搬到多少字节"。
 * 块设备协议今天的"一次最多 2 个扇区"不是驱动能力决定的，正是这个数决定的
 * （见 docs/07-performance-roadmap.md 主线 A 第 2 步：批量传输要走共享内存，
 * 否则这条上限会一直卡着磁盘吞吐）。 */
#define FE_MSG_MAX_PAYLOAD 1024

/* 一条消息最多携带几个句柄（与内核 FE_MSG_MAX_HANDLES 一致）。
 * 超了**拒绝**（不是截断）：截断会让发送方以为 5 个都过去了而接收方只看到 4 个。 */
#define FE_MSG_MAX_HANDLES 4

/* 句柄权限位（与内核 fe/object.h 的 FE_RIGHT_* 一致）。
 *
 * 用户态只需要知道自己用的那几位；这里给全是为了让"收窄"这个动作可表达：
 * 客户端把内存对象交给服务时，可以只给 READ，服务就**不可能**改那块内存
 * ——这不是约定，是内核按权限位算出来的。 */
#define FE_RIGHT_READ     (1u << 0)
#define FE_RIGHT_WRITE    (1u << 1)
#define FE_RIGHT_EXEC     (1u << 2)
#define FE_RIGHT_SEND     (1u << 3)
#define FE_RIGHT_RECV     (1u << 4)
#define FE_RIGHT_SIGNAL   (1u << 5)
#define FE_RIGHT_WAIT     (1u << 6)
#define FE_RIGHT_DUP      (1u << 7)
#define FE_RIGHT_TRANSFER (1u << 8)
#define FE_RIGHT_ALL      0x1FFu    /* 全部权限位（内核 FE_RIGHT_ALL 一致） */

struct fe_msg_header {
    u32 protocol;
    u32 opcode;
    u32 payload_len;
    u32 handle_count;
    u64 request_id;
    /* 发送方任务 id。**由内核在投递时填写**——收到消息时这里就是可信的身份，
     * 不要读自己发出去时填的值（那不算数）。 */
    u64 sender_task;
};

/* ---------------- 扇区访问矩阵（WRP/WFP 第 2 道防线 + A/B） ----------------
 *
 * 表由内核持有，**强制点是块设备服务**：它每次读/写之前问一次内核，
 * 并把请求方的任务 id（从消息头的 sender_task 取）一起带上。
 * 详见 docs/04-write-protection.md 与 docs/05-ab-update.md。 */
#define FE_PROT_DENY_WRITE (1u << 0)
#define FE_PROT_DENY_READ  (1u << 1)
#define FE_PROT_DENY_ALL   (FE_PROT_DENY_WRITE | FE_PROT_DENY_READ)

struct fe_protect_info {
    u64 lba;
    u64 count;
    u64 exempt_task;
    u32 mode;
    u32 _pad;
};

/* 登记一段受保护区间（只能加不能减，所以开放给任何任务都是安全的） */
long fe_protect_add(u64 lba, u64 count, u32 mode, u64 exempt_task);
/* 访问检查：返回 FE_OK 或 FE_ERR_ACCESS */
long fe_protect_check(u64 lba, u64 count, int is_write, u64 requester_task);
/* 内核最多登记多少段受保护区间。**调用者的缓冲必须按它开**，
 * 否则会静默拿到前缀（见 fe_protect_list 的说明）。 */
#define FE_PROT_MAX_EXTENTS 256

/* 导出区间表（检查程序靠它核对矩阵，而不是把布局硬编码进测试）。
 *
 * 返回写入的段数；**cap = 0 时返回内核里的真实段数**（查询模式，不写内存）。
 *
 * ★ 为什么要留一个"问总数"的口子 ★
 * 这个接口原来把 cap 悄悄夹到 64，而内核最多登记 256 段。区间数超过 64 时
 * 调用者拿到的是**前缀**，而它完全看不出来——`protcheck` 据此算过一个
 * "在所有受保护区间之外"的 LBA，那一段其实排在第 65 段之后，
 * 于是失败信息变成"正向对照需要一个可读的探测扇区"，
 * 看起来和写保护毫无关系。
 * 现在调用者可以 `n = fe_protect_list(NULL, 0)` 先问规模，
 * 再断言"我这次拿到的段数 == 总数"，把截断变成**可见的**。 */
long fe_protect_list(struct fe_protect_info *out, u32 cap);
/* 累计被拒绝的访问次数 */
u64  fe_protect_violations(void);

/* ---------------- A/B 槽管理 ----------------
 *
 * 槽状态记录放在 FAT32 的保留扇区里（misc），两份、各带 CRC32、交替写：
 * **坏掉一次写入不会让系统失去"下次启动哪一份"这个信息**，另一份还在。
 * 这是 Android bootloader_control 的同一套做法。 */
#define FE_AB_MAGIC 0x4C534546u     /* 'FESL' */

struct fe_ab_info {
    u64 misc_lba;
    u64 misc_count;
    u64 bootsel_lba;
    u64 bootsel_offset;
    u64 updater_task;   /* 0 = 本进程不是被指定的更新器 */
    u32 boot_slot;      /* 0 = A, 1 = B */
    u32 flags;          /* FE_AB_FLAG_* */
};

/* flags 的位。用**名字**而不是裸的 1/2：这两位的含义决定了
 * "这次启动能不能做 A/B"，读代码的人不该去数位。 */
#define FE_AB_FLAG_SLOT_STATE 0x1u  /* misc 位置有效：能读写槽状态记录 */
#define FE_AB_FLAG_BOOTSEL    0x2u  /* 引导控制块位置有效：能切槽 */

/* 引导控制块那一个字节的取值。
 * ★ Limine 的 default_entry 是 1 起算的 ★：'1' = 第一个条目（槽 A），
 * '2' = 第二个条目（槽 B）。把它写成 '0'/'1' 的后果是
 * "0 和 1 都启动第一个条目"——更新报告成功、控制块回读也对，
 * 但每次还是进同一个槽。这类错只能靠"真的重启一次看进哪个槽"发现。 */
#define FE_AB_BOOTSEL_A '1'
#define FE_AB_BOOTSEL_B '2'

struct fe_ab_slot_state {
    u32 magic;
    u32 seq;
    u8  active;         /* 0 = A, 1 = B：下次启动用哪个 */
    u8  successful;     /* 活动槽是否已自报启动成功 */
    u8  boot_attempts;  /* 活动槽已尝试启动的次数 */
    u8  _pad;
    u32 crc32;          /* 覆盖前 12 字节 */
};

long fe_ab_get_info(struct fe_ab_info *out);
u32  fe_crc32(const void *data, u32 len);

/* 引导器给的命令行（原样）。装不下返回 FE_ERR_NOSPC，**不截断**。 */
long fe_cmdline(char *out, u32 cap);

/* 机器复位。正常情况不返回。
 * A/B 的切换点是重启，所以这是流程的一部分而不是调试接口。 */
void fe_reboot(void);

/* ---------------- 帧缓冲（控制台服务的入口） ----------------
 *
 * 与内核 struct fe_fb_info 逐字节一致。
 * 拿到之后第一件事是 fe_mmio_map(phys, size_bytes, FE_PROT_READ|FE_PROT_WRITE)
 * ——帧缓冲是**设备内存**，必须走关缓存的 MMIO 映射，不能用内核的线性地址。 */
struct fe_fb_info {
    u64 phys;
    u64 size_bytes;
    u32 width;
    u32 height;
    u32 pitch;
    u32 bpp;
    u8  memory_model;   /* 1 = RGB */
    u8  red_size,   red_shift;
    u8  green_size, green_shift;
    u8  blue_size,  blue_shift;
    u8  _pad[5];
};

long fe_fb_get_info(struct fe_fb_info *out);

/* 按需分页累计解析的缺页次数（诊断）。
 * ★ 它是"栈真的在按需增长"的唯一证据 ★ 只看"程序没崩"分不出
 * "缺页被补上了"与"一开始就全映射好了"——两者行为一样。
 * >0 才说明走的是前者。 */
u64 fe_pf_resolved_count(void);

/* ---------------- 任务可见性（TASK_LIST） ----------------
 *
 * 与内核 struct fe_task_info 逐字节一致（见 kernel/include/fe/syscall.h）。
 *
 * ★ 为什么是"只读快照"，而不是"内核给一个任务表句柄" ★
 * 运维面（ps 这类东西）需要的是**全系统的状态**，而不是"我持有的能力"。
 * 把它做成句柄就得回答"谁有权持有它"，而在能力模型里那个答案会一路膨胀：
 * 拿到任务表句柄的进程等于能看见并（如果再有写口）干预所有进程。
 * 只读快照既满足"看得见"，又不产生任何新的权限——**看不见的能力也就无法被滥用**。
 *
 * 反过来，这也意味着它**不能**用来杀进程：这里只有 id，没有句柄，
 * 而"杀"需要句柄（那才是不可伪造的能力）。详见 docs/13-tasks-and-kill.md。 */
#define FE_TASK_LIST_MAX   32
#define FE_TASK_NAME_MAX   16
#define FE_TASK_THREADS_MAX 4

/* Task 记录是固定步长，threads 是紧凑数组：
 * 这样内核可以在**不知道用户态结构体布局**的前提下按固定字节数填表，
 * 用户态也只需要一次拷贝就能跨越整张表。
 * ★ 步长写成宏而不是 sizeof(结构体) ★ —— sizeof 会随两边编译器的填充决定漂移，
 * 而"字段莫名其妙是 0"正是这类漂移表现出来时的样子。
 *
 * ★ 但宏必须被**核对**，否则它只是一个说法 ★
 * 上面那句只说了前半句（不要用 sizeof 去**定义** ABI），漏了后半句：
 * 宏一旦与结构体对不上，就再也没有任何东西会报错。
 * 实测代价见 docs/13-tasks-and-kill.md：两个步长都写成 48，而两个结构体
 * 都是 **64** 字节——于是内核把 `name` 写到**下一条记录的头 16 字节上**，
 * 用户态按 48 步长读到的 `name` 是下一条记录的字节。
 * 之所以一直没暴露，是因为**从来没有任何调用者用过这张表**。
 * 现在两边都用 _Static_assert 把宏钉在结构体上（见下面两个结构体之后）：
 * 数字仍然是契约，但"契约与本编译器的布局是否一致"由编译器判，不再靠人算。 */
#define FE_TASK_STRIDE   64     /* struct fe_task_info 的字节数 */
#define FE_THREAD_STRIDE 64     /* struct fe_thread_info 的字节数 */

struct fe_task_info {
    u64 id;
    u64 parent_id;              /* 0 = 没有父任务（内核任务或 init） */
    u32 thread_count;           /* 本任务**存活**的线程数 */
    u32 thread_base;            /* 它的线程在下面 threads[] 里的起始下标 */
    u32 granted_ports;          /* 已认领的端口区间数（诊断用，只增不减） */
    u32 handle_count;           /* 句柄表里已用的槽位 */
    u64 cpu_ticks;              /* 本任务所有线程累计占用的节拍 */
    u8  exited;                 /* 主线程已退出，等待父进程收尸 */
    u8  is_kernel;              /* 1 = 内核任务（没有用户地址空间） */
    u8  _pad[6];                /* 显式填充：让两个定义天然一致，
                                 * 而不是靠"两边编译器碰巧算出同一个数" */
    char name[FE_TASK_NAME_MAX];
};

struct fe_thread_info {
    u64 id;
    u64 cpu_ticks;              /* 该线程累计占用的节拍 */
    u64 switches;               /* 被调度上 CPU 的次数 */
    u32 priority;
    u32 state;                  /* FE_THREAD_* 常量 */
    i32 slice;
    i32 exit_code;
    u32 index;                  /* 在本任务内的线程序号（= 它在 threads[] 里的位置
                                 * 减去本任务的 thread_base），**纯粹是序号** */
    u32 flags;                  /* FE_THREAD_FLAG_*；见下面。
                                 * ★ 这一格原来是 _pad，而"哪一个是主线程"
                                 * 原来打算靠"index 0 = 主线程"来表达 ★
                                 * 那是个**假的**约定：线程枚举走的是全局链
                                 * （新建的插在表头），所以下标 0 是**最近创建
                                 * 的那个**线程，不是主线程。与其约束遍历顺序
                                 * （那会让"覆盖全部线程"变得脆弱），
                                 * 不如把这个事实直接写成一bit。 */
    char name[FE_TASK_NAME_MAX];
};

/* 线程标志（fe_thread_info.flags） */
#define FE_THREAD_FLAG_MAIN 1u  /* 本任务是它的主线程：它退出即进程结束 */

/* 步长宏与结构体的**编译期**核对（见步长宏上面那段说明）。
 * 放在结构体之后、任何使用之前：写错了就编译不过，而不是运行期读到垃圾。 */
_Static_assert(sizeof(struct fe_task_info) == FE_TASK_STRIDE,
               "FE_TASK_STRIDE 与 struct fe_task_info 不一致");
_Static_assert(sizeof(struct fe_thread_info) == FE_THREAD_STRIDE,
               "FE_THREAD_STRIDE 与 struct fe_thread_info 不一致");

/* 线程状态。数值必须与内核 enum fe_thread_state 一致——
 * 这是共享 ABI 的一部分，不是"我们自己的一套"。 */
#define FE_THREAD_UNUSED   0
#define FE_THREAD_READY    1
#define FE_THREAD_RUNNING  2
#define FE_THREAD_SLEEPING 3
#define FE_THREAD_BLOCKED  4
#define FE_THREAD_DEAD     5

/* Task 记录是固定步长，threads 是紧凑数组（步长宏与核对见上面结构体之前）。 */
struct fe_task_list {
    u32 task_count;             /* 实际写入的任务数（可能 = cap，表示被截断） */
    u32 thread_count;
    u64 total_tasks;            /* 内核里当前存活的任务总数（用于判断是否截断） */
    u64 total_threads;          /* 内核里当前存活的线程总数 */
    u64 current_task;           /* 调用者自己的任务 id */
    u32 tasks_cap;
    u32 threads_cap;
    u32 _pad[2];
    /* 之后紧跟 tasks_cap 条 fe_task_info（每条 FE_TASK_STRIDE 字节），
     * 再紧跟 threads_cap 条 fe_thread_info。 */
};

/* 取一次任务/线程快照。返回写入的任务数，负数 = 错误。
 *
 * ★ 输出缓冲布局是"表头 + 定长任务记录 + 紧凑线程记录" ★
 * 而不是"C 结构体数组"：内核必须能在**不知道用户态结构体布局**的前提
 * 下按固定步长填这张表，否则两边任何一次填充差异都会变成
 * "字段莫名其妙是 0"（这个项目已经栽过好几次）。 */
long fe_task_list(void *buf, u32 buf_len, u32 task_cap, u32 thread_cap);

/* 便捷包装：把快照取进调用者自己的缓冲，并按名字找任务。
 * 这只是**在共享内存里做一次线性查找**，不需要任何额外权限。 */
struct fe_task_view {
    const struct fe_task_list *hdr;
    const u8 *raw;
    u32 raw_len;
};
long fe_task_view_get(struct fe_task_view *out, void *buf, u32 buf_len,
                      u32 task_cap, u32 thread_cap);
const struct fe_task_info *fe_task_at(const struct fe_task_view *v, u32 i);
const struct fe_thread_info *fe_thread_at(const struct fe_task_view *v, u32 i);
const struct fe_task_info *fe_task_find(const struct fe_task_view *v, const char *name);

const char *fe_thread_state_str(u32 state);

/* MEM_INFO 的返回结构（与内核 struct fe_mem_info 逐字节一致） */
struct fe_mem_info {
    u64 phys;
    u64 size;
    u32 flags;
    u32 page_count;
};

/* ---------------- 系统调用封装 ---------------- */

long fe_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6);

long fe_write(const char *buf, usize len);
__attribute__((noreturn)) void fe_exit(int code);
void fe_yield(void);
/* 创建一个用户线程。stack 传 NULL、size 传 0 时由内核分配一段。
 *
 * ★ 线程入口**绝不能返回** ★
 * 内核给线程搭的初始栈是"被调用的函数"那一格，而**那一格是零**——
 * 入口一旦 `ret` 就会跳到地址 0，症状是取指 #PF、出错指令 0x0，
 * 而任务名只会显示 "user"，看不出是谁。所以入口必须以 `fe_exit()`
 * 收尾（进程主线程同理，只是那条约定早就写在别处了）。
 *
 * 这一条以前没写下来过，因为既有的用户线程都是显式退出、从不返回；
 * 直到 D5c 里 blkd 的中断等待线程第一次不小心返回，才把它照出来。 */
u64  fe_thread_create(void (*entry)(void *), void *arg, void *stack, usize stack_size);
void fe_sleep_ms(u64 ms);
/* 单调时钟，**纳秒**（K7）。
 *
 * ★ 它现在是 TSC 制的，而这改变了它的可用范围 ★
 * 在 K7 之前这个接口实际返回的是"毫秒 × 1e6"——任何亚毫秒的测量都只会
 * 得到 0 或 1000000 的倍数。D4 的吞吐对照里两次独立测量都是**正好**
 * 5000 us，那次测出来的其实是时钟的粒度，而断言**看起来是通过的**。
 *
 * 但"够细"不是无条件的：引导器没给 TSC 频率时会退回节拍制（1 ms）。
 * 想判断该不该信它，先问 fe_clock_info()——**不要假设精度**。 */
u64  fe_clock_ns(void);

/* 时钟自述（K7）。返回 0 成功；负数 = 错误。 */
#define FE_CLOCK_FLAG_TSC 1u
struct fe_clock_info {
    u64 hz;                 /* 时基计数频率 */
    u64 resolution_ns;      /* 一个计数 = 多少纳秒；至少 1 */
    u32 flags;              /* FE_CLOCK_FLAG_* */
    u32 _pad;
};
long fe_clock_info(struct fe_clock_info *out);
long fe_mem_alloc(u64 size);
long fe_mem_alloc_dma(u64 size);
void *fe_mem_map(long handle, void *hint, u64 size, u32 prot);
/* 撤销一段由 fe_mem_map 建立的映射。
 *
 * ★ 它不释放内存 ★：帧归内存对象所有，对象还可能被别的任务映射着。
 * 想真正释放就关掉最后一个句柄（那是对象生命周期的事）。
 * 只接受 mmap 区内的地址，详见 docs/09-handle-transfer.md §4.1。 */
long fe_mem_unmap(void *addr, u64 size);
long fe_mem_info(long handle, struct fe_mem_info *out);
long fe_handle_close(long handle);
/* 句柄复制（权限只能收窄）。new_rights = 0 表示沿用原权限。
 * 用途：把同一个能力给同一个任务的另一个线程用——**不要**用共享变量传句柄号，
 * 句柄号是进程内索引，一个线程 close 掉另一个线程的号就指向别的东西了。 */
long fe_handle_dup(long handle, u32 new_rights);
long fe_endpoint_create(u32 flags);
/* 发送。handles 为随消息传递的句柄数组（可为 NULL），其长度必须与
 * hdr->handle_count 一致——不一致内核会拒绝（见 docs/09-handle-transfer.md §2）。
 *
 * 这是**能力传递**的唯一通道：把内存对象交过去，服务端直接映射它、
 * 自己读写那块内存，内核完全不碰数据。1024 字节的载荷上限因此只约束控制面。 */
long fe_endpoint_send(long ep, const struct fe_msg_header *hdr, const void *payload,
                      const u32 *handles, u32 handle_count);
/* 接收。out_handles 为 FE_MSG_MAX_HANDLES 个 u32 的数组（可为 NULL），
 * out_count 接收实际装上了几个（可为 NULL）。
 *
 * 收到的句柄权限 = 发送方权限 - TRANSFER：能力只能收窄。
 * 这两个指针若未映射会直接返回 FE_ERR_FAULT 且**不消耗消息**——
 * 若在收到之后才拷不回去，用户态会拿到一堆自己不知道句柄号的句柄（不可回收）。 */
long fe_endpoint_recv(long ep, struct fe_msg_header *hdr, void *payload, u32 cap,
                      u32 *out_handles, u32 *out_count);
long fe_endpoint_call(long ep, const void *req, u32 req_len, void *rep, u32 rep_cap,
                      u32 *out_len);

/* ---------------- 驱动能力（M6） ----------------
 *
 * 三样硬件手段都必须「认领」而不是「申请」：内核资源池里没有的东西，
 * 任何权限都拿不到。认领成功后：
 *   - fe_ioport_request 之后 in/out 指令对本任务不再触发 #GP；
 *   - fe_mmio_map 返回一段可读写的设备寄存器虚拟地址；
 *   - fe_irq_register 之后中断会置位通知对象的第 (1 << irq) 位。 */
long fe_ioport_request(u64 base, u64 count);
void *fe_mmio_map(u64 phys, u64 size, u32 prot);

/* 触发方式与极性（D5b）。 */
#define FE_IRQ_MODE_EDGE       1u
#define FE_IRQ_MODE_LEVEL      2u
#define FE_IRQ_MODE_ACTIVE_LOW 4u
/* 不声明触发方式的要求（默认）。注意它与 FE_IRQ_F_NO_MASK 是**两件事**：
 * 前者说"我不在乎是电平还是边沿"，后者说"不要为我屏蔽这条线"。 */
#define FE_IRQ_MODE_ANY        0u

/* "不要为我屏蔽这条线"（bit 3，D5a）。见 fe_irq_register 的说明。 */
#define FE_IRQ_F_NO_MASK       8u

struct fe_irq_info {
    u32 gsi;
    u8  vector;
    u8  _pad[3];
    u32 mode;               /* FE_IRQ_MODE_* */
    u32 sharers;            /* 当前有几个登记者 */
};

/* 登记一个中断，投递到通知对象的第 (1 << irq) 位。
 *
 * require_mode 非 0 时表示"我要求这条线是这个触发方式"，不匹配就拒绝登记
 * （返回 FE_ERR_INVAL）。**建议驱动都声明**：带着错的触发方式跑比直接失败
 * 更糟——它不会立刻崩，只会在负载高的时候偶发丢事件，而那种症状会被
 * 归因到驱动自己的逻辑上。
 *
 * out_info 非 0 时回填这条线的实情（GSI / 向量 / 触发方式 / 共享者数）。
 *
 * ★ 共享一根线（D5a）—— 它改变了驱动的写法，必须读完再动手 ★
 *
 * 两台 PCI 设备可以接在同一条 INTx 上。内核允许**电平触发**的线被最多
 * 4 个任务登记（边沿线**不允许**：A 的中断正在处理时 B 拉的边沿没有
 * 第二次跳变可检测，那一次就永久丢了——所以要共享请用 MSI）。
 *
 * 两条由此而来的契约：
 *
 *   1. **必须接受伪唤醒。** 线不携带"是哪台设备"，内核无法挑一个登记者，
 *      所以每台设备的中断都会把**所有**登记者叫醒。驱动每次醒来都要问
 *      自己的设备"是你吗"，不是就立刻返回——这是 PCI 共享中断的标准写法。
 *   2. **必须确认。** 电平线投递后会**屏蔽到所有登记者都确认**为止
 *      （不是"谁先确认谁放行"——那样线还高着就会立刻重投，变成中断风暴）。
 *      所以处理完必须调 fe_irq_ack。
 *
 * ★ 代价要说清楚 ★ 一个不确认的登记者会把**整条线**卡住，与它共享的
 * 邻居一起收不到中断。独占时这个代价只落在自己头上，共享时落在别人头上。
 * 唯一的缓解是"任务退出时内核替它清掉欠账"（fe_irq_release_owner），
 * 所以**不要指望"忘了 ack"能自己好**。
 *
 * ★ 如果你靠轮询判定完成、只把中断当提示，请加 FE_IRQ_F_NO_MASK ★
 * 那种驱动不会去 ack（它压根不等这个通知），而默认的"屏蔽到确认"会让
 * 线在**第一次中断之后就被永久屏蔽**——绑定还在、通知再也不会来。
 * `blkd` 就是这个形状：它读 used ring 判完成、顺手读 ISR 寄存器清设备
 * 条件，所以它声明 NO_MASK（不屏蔽不会风暴，因为设备条件被清掉了）。
 * 一句话：**要么 ack，要么声明 NO_MASK；两者都不做就是把自己的中断
 * 悄悄掐掉**。 */
long fe_irq_register(u64 irq, long notif_handle);
long fe_irq_register_ex(u64 irq, long notif_handle, u32 require_mode,
                        struct fe_irq_info *out_info);
long fe_irq_ack(u64 irq);

/* ---- MSI / MSI-X（D5c）----
 *
 * ★ 它解决的是共享 INTx 解决不了的那个问题 ★
 * 共享线上"是哪台设备"内核答不出来，所以每个驱动都得接受伪唤醒、
 * 都得去问自己的设备。MSI 是**每台设备一条独立消息**：身份由向量本身
 * 携带，没有共享、也没有电平/边沿的问题（消息写天然是边沿，
 * 不需要 ack）。真实驱动默认走它。
 *
 * ★ 分段 ★ 能力结构在哪（cap_off）由**设备管理器**读配置空间发现，
 * 然后经命令行/协议交给驱动；驱动拿它调这个接口。内核负责"分配向量 +
 * 算消息地址/数据 + 让设备开始发消息"，三件事原子完成。
 *
 * 用法（MSI-X）：
 *   1. `fe_irq_msi_alloc(nt, bus, dev, fn, cap_off, &mi)`；
 *   2. 把 `mi.addr` / `mi.data` 写进 MSI-X 表的**第 0 项**
 *      （位置 = 你映射好的 `mi.table_bar` 那个 BAR 的 `mi.table_offset` 处，
 *        每项 16 字节：addr 低 32 / addr 高 32 / data / 控制字）；
 *   3. 等 `1 << mi.irq` 这一位（`fe_notification_wait`）；
 *   4. 退出前 `fe_irq_msi_free(mi.irq)`。
 *
 * ★ 为什么表项由驱动写而不是内核 ★ 表在设备的 BAR 里，而 BAR 的映射
 * 是驱动自己的事（资源池 + fe_mmio_map）。内核要写它就得先映射一段
 * 属于别人的 BAR——那正是资源池要避免的。
 *
 * ★ 安全边界（说清楚）★ 驱动拿到 (addr,data) 之后可以给别人制造伪中断，
 * 但**共享 INTx 下它本来就能**（线的语义就是"内核无法分辨是谁"）。
 * 所以这不是一项新能力。 */
#define FE_MSI_KIND_MSI   1u
#define FE_MSI_KIND_MSIX  2u
struct fe_msi_info {
    u32 irq;            /* 伪中断号：通知位是 (1 << irq)，取值 32..63 */
    u8  vector;
    u8  kind;           /* FE_MSI_KIND_* */
    u8  _pad[2];
    u32 table_bar;      /* MSI-X：表在哪个 BAR（0..5）；MSI 为 0xFF */
    u32 table_offset;   /* MSI-X：表相对该 BAR 的字节偏移 */
    u32 data;           /* 写进设备（或表项）的消息数据 */
    u64 addr;           /* 写进设备（或表项）的消息地址 */
};
_Static_assert(sizeof(struct fe_msi_info) == 32,
               "fe_msi_info 与内核侧不一致（两边都是 32 字节）");

long fe_irq_msi_alloc(long notif_handle, u32 bus, u32 dev, u32 fn, u32 cap_off,
                      struct fe_msi_info *out);
/* 关掉设备的 MSI 并释放向量与伪中断号。只认自己分配的那个。 */
long fe_irq_msi_free(u32 irq);
long fe_notification_create(void);
long fe_notification_wait(long handle, u64 mask, u64 *out_bits);
long fe_notification_signal(long handle, u64 bits);

/* 端口 I/O：只有认领过相应端口区间的任务才能用，否则触发 #GP 并被内核终止 */
unsigned char  fe_inb(unsigned short port);
void           fe_outb(unsigned short port, unsigned char val);
unsigned short fe_inw(unsigned short port);
void           fe_outw(unsigned short port, unsigned short val);
unsigned int   fe_inl(unsigned short port);
void           fe_outl(unsigned short port, unsigned int val);

/* ---------------- 进程模型（M5，学 Linux） ----------------
 *
 * 可执行文件由**路径**标识（内核从根文件系统装载），spawn 返回新任务的**句柄**
 * 而不是 pid —— 句柄不可伪造，所以只能 wait 自己持有的任务，没有 Linux
 * 「谁都能 kill/wait 别人」的那个问题。wait 阻塞到目标进程结束并取回退出码。 */
long fe_spawn(const char *path, char *const argv[], u32 argc);
long fe_wait(long task_handle, int *out_status);

/* 终止一个任务（K2）。成功返回 FE_OK。
 *
 * ★ 需要句柄上有 FE_RIGHT_TERMINATE ★
 * `fe_spawn` 返回的那个句柄**天然带它**（拉起者可以终止它拉起的东西），
 * 所以最常见的情形——服务跑飞了、拉起它的那方把它停掉——直接可用。
 * 经消息把任务句柄转交出去时，这一位也会跟着走（能力只收窄不扩张，
 * 但 TRANSFER 本身不带"剥夺某一位"的语义，所以收方拿到的是发送方有的那些）。
 *
 * ★ 它是**异步**的 ★ 返回时只保证"目标的所有线程都已被标记"，
 * 不保证它们已经死透。要确定没了就 `fe_wait(handle, &code)`。
 * 被终止任务的主线程退出码是 FE_ERR_KILLED(-22)。
 *
 * ★ 阻塞中的调用会以 FE_ERR_CANCELED(-21) 返回 ★
 * 一个正卡在 recv/wait 上的线程必须先被唤醒才走得到"回用户态"那条路，
 * 所以内核在阻塞重试点上检查终止标志，让那次调用带着 -21 返回——
 * 然后线程在返回用户态的途中结束，**不会执行用户态的下一条指令**。
 * 因此用户态应当把 -21 当作"这次调用没做完"，而不是"参数错"。 */
long fe_task_terminate(long task_handle);

/* 共享资源上的控制器锁（协作锁）。只对内核标记为可共享的区间有效，
 * 独占区间调用会返回 FE_ERR_NOTSUP。 */
long fe_resource_lock(u32 kind, u64 base, u64 len);
long fe_resource_unlock(u32 kind, u64 base, u64 len);

/* ---------------- 设备管理器（发现 → 申报 → 授予） ----------------
 *
 * ★ 这条链是 D3b 与 D4 之间缺的那一环 ★
 * pcid 能"看见"PCI 设备的 BAR，但 BAR **不在资源池里**，所以驱动拿着
 * 地址也认领不到（fe_ioport_request 返回 FE_ERR_NOENT）。
 * 补上它需要的三件事正好都在用户态：
 *
 *   发现   读配置空间，算出每个 BAR 的类别与长度          （pcid 已做）
 *   申报   把那段区间交给内核入池（FE_SYS_RESOURCE_POOL_ADD，需 DM 身份）
 *   授予   把**自己名下**的那段转给某个驱动（RESOURCE_GRANT[_ID]）
 *
 * 内核在这条链上只做三件事：**谁在说**（是不是设备管理器）、
 * **说的合不合法**（范围/对齐/不与可用内存相交）、**记下来**（入池/换主人）。
 * 它依然不认识"IDE 控制器"这种东西——那才是"内核里不写驱动"的落点。
 *
 * ★ 身份从哪来 ★
 * 只有引导者（init）能认领设备管理器身份，且只能认领一次（内核侧规则，
 * 见 kernel/include/fe/resource.h）。init 认领之后既可以直接申报，
 * 也可以像今天这样把活交给 pcid —— 但 **pcid 本身不是设备管理器**，
 * 它只是 init 手里的一把工具，所以"谁有资格申报"这件事仍然唯一。 */
long fe_resource_pool_add(u32 kind, u64 base, u64 len, int shared);
/* 授予：目标是**任务句柄**（不可伪造的能力，默认做法） */
long fe_resource_grant(u32 kind, u64 base, u64 len, long target_task_handle);
/* 授予：目标是**任务 id**。用于"驱动不是设备管理器拉起的"那种引导链——
 * 句柄在 init 手里时，设备管理器拿不到它。代价（id 可猜）与两道防护
 * 写在 kernel/include/fe/syscall.h 的 0x88 上。 */
long fe_resource_grant_id(u32 kind, u64 base, u64 len, u64 target_task_id);
/* 认领 / 交还设备管理器身份（只有引导者能认领成功） */
long fe_devmgr_claim(void);
long fe_devmgr_release(void);
/* 释放本任务名下的全部硬件资源所有权（区间回到池子）。
 * 语义与"任务退出自动回收"一致，只是提前发生——引导链上有中间点需要它。 */
long fe_resource_release_owner(void);


/* ---------------- 服务发现（M7，学 BSD） ----------------
 *
 * 没有注册表服务，也没有查找协议：命名空间就是**文件系统**。
 * 服务把自己的端点发布到 /dev 下的一个名字，客户端按路径打开它。
 *
 * 发布要求端点句柄带「可转交」权限——能把它交出去是一种权限，不是默认就有的。
 * 同名发布会被拒绝，所以没有服务能被别的服务顶替掉。 */
long fe_devfs_publish(const char *path, long endpoint_handle);
long fe_devfs_open(const char *path);

/* ---------------- A/B 槽前缀 ----------------
 *
 * 内核 exec 的是 /slot_<x>/init，那个路径原样进了 argv[0]，
 * 所以**"我在哪个槽"这件事不需要问任何人**——它写在程序自己的名字里。 */
void fe_slot_prefix(const char *arg0, char *out, u32 cap);
void fe_slot_path(const char *prefix, const char *rel, char *out, u32 cap);

/* ---------------- 运行库（无 libc，自己实现） ----------------
 *
 * ★ 这一节是"能不能写超过 500 行的程序"的分界线 ★
 * 在这之前，用户态每写一个稍大的东西都要手搓字符串拼接与定长缓冲：
 * 症状不是编译错误，而是**写程序的速度**——每个程序多花半天在样板代码上，
 * 而且样板代码里全是"缓冲区够不够"这类只能在运行期发现的错。
 *
 * 全部从零实现（不引 libc、不抄实现），语义按 C 标准：
 * 特别是 fe_snprintf **返回本该写入的长度**（不含结尾 '\0'），
 * 于是调用者能检测截断——返回值被截断长度"修正"过的实现
 * 会让所有检查截断的代码永远看不到截断。 */

void *memcpy(void *dst, const void *src, usize n);
void *memmove(void *dst, const void *src, usize n);
void *memset(void *dst, int c, usize n);
int   memcmp(const void *a, const void *b, usize n);

usize strlen(const char *s);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, usize n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, usize n);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, usize n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);

int      atoi(const char *s);
long     strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);

/* 格式化。返回**本该写入的字节数**（不含结尾 '\0'）：
 * 它 >= cap 就说明被截断了，而缓冲区里永远是合法的、以 '\0' 结尾的串
 * （cap > 0 时）。cap == 0 只计算长度，不写一个字节。 */
int fe_snprintf(char *buf, usize cap, const char *fmt, ...);
int fe_vsnprintf(char *buf, usize cap, const char *fmt, va_list ap);

/* ---------------- 堆：基于内存对象 + 映射的 arena（见 libfe.c 顶部注释） ----------------
 *
 * 返回的指针 16 字节对齐；失败返回 NULL（**不 panic、不休眠、不返回野指针**）。 */
void *malloc(usize size);
void  free(void *ptr);
void *calloc(usize nmemb, usize size);
void *realloc(void *ptr, usize size);

/* 堆的自检与统计。这不是"调试便利"，而是**让"没有泄漏"变成可断言的事实**：
 * 原来只能靠"跑完没崩"，那证明不了任何事。 */
struct fe_malloc_stat {
    u64 arena_size;         /* 已经拿到的堆字节数（0 = 还没初始化） */
    u64 arena_used;         /* 高水位：从起点到最后一个块的偏移 */
    u64 live_bytes;         /* 当前已分配（去掉头部）的字节数 */
    u64 live_blocks;
    u64 total_blocks;       /* 块总数（含空闲）；用来观察合并是否真的发生 */
    u64 free_blocks;
    u64 alloc_calls;
    u64 free_calls;
    u64 peak_live_bytes;
};
void fe_malloc_stat_get(struct fe_malloc_stat *out);
/* 走一遍堆，返回**发现的结构性错误数**（0 = 堆结构完好）。
 * 检查：块边界与大小自洽、相邻空闲块**没有被合并**（这是分配器最容易出的错，
 * 而它的症状是"明明有空闲却分配不出来"）、已分配块的内容没被写坏。
 * 它会重算每个块的幻数，所以用野指针写过界一定会被抓到。 */
u32 fe_malloc_check(void);
/* 便捷判断：现在有没有还没释放的块（>0 = 泄漏） */
u32 fe_malloc_live_blocks(void);

void fe_puts(const char *s);
void fe_flush(void);
void fe_print_u64(u64 v);
void fe_print_hex(u64 v);
void fe_printf(const char *fmt, ...);

#endif /* FE_USER_H */
