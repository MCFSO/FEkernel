/* SPDX-License-Identifier: 0BSD */
/* <errno.h> —— 错误码（见 docs/10-posix-layer.md）。
 *
 * ★ 错误码的取值按 POSIX 的约定，不是我们随便编号 ★
 * 程序里有 `if (errno == ENOENT)` 这种硬编码比较，
 * 也有的地方把 errno 直接打印出来给人看。取值一旦自创，
 * 别人的程序就会"编译通过、判断永远为假"。
 *
 * ★ errno 是**每线程**一份，靠内核设置的 %fs 基址 ★
 *
 * POSIX 要求 errno 每线程一份：两个线程同时读文件失败时，后失败的那个
 * 会覆盖前一个的错误码，症状是"A 报的错其实是 B 的"。
 *
 * 线程局部存储需要内核配合——`__thread` 编译出来是 `mov %fs:0x0`，
 * 而 fs 基址只能由内核在上下文切换时设置。这件事**已经补上了**：
 *   - 内核从可执行文件的 PT_TLS 段取初始化映像，给每个线程复制一份；
 *   - 切换线程时 `wrmsr(IA32_FS_BASE, 该线程的块)`。
 *
 * 补上之前这里写的是进程级的一份，而症状很直接：第一次写 errno 就 #PF
 * （出错指令 `movq %fs:0x0, %rcx`、出错地址 0，反汇编定位）。
 * 现在改回 `__thread`——**并且要有对照**：只证明"改回线程级还能跑"
 * 是不够的，还要证明"两个线程的 errno 真的互不影响"（见 libposix 的自检）。
 */
#ifndef FE_POSIX_ERRNO_H
#define FE_POSIX_ERRNO_H

/* POSIX 的取值（Linux 数字，见 errno(3)）。我们只实现其中用得到的，
 * 但**编号与值必须对齐**——这是跨系统共享的约定，不是本地实现细节。 */
#define EPERM    1   /* 不允许的操作 */
#define ENOENT   2   /* 没有这个文件或目录 */
#define ESRCH    3   /* 没有这个进程 */
#define EINTR    4   /* 系统调用被中断 */
#define EIO      5   /* 输入输出错误 */
#define ENXIO    6   /* 没有这个设备或地址 */
#define E2BIG    7   /* 参数表过长 */
#define ENOEXEC  8   /* 可执行格式错误 */
#define EBADF    9   /* 坏的文件描述符 */
#define ECHILD  10   /* 没有子进程 */
#define EAGAIN  11   /* 资源暂时不可用（= EWOULDBLOCK） */
#define ENOMEM  12   /* 内存不足 */
#define EACCES  13   /* 权限不足 */
#define EFAULT  14   /* 坏地址 */
#define ENOTBLK 15   /* 需要块设备 */
#define EBUSY   16   /* 设备或资源忙 */
#define EEXIST  17   /* 文件已存在 */
#define EXDEV   18   /* 跨设备链接 */
#define ENODEV  19   /* 没有这个设备 */
#define ENOTDIR 20   /* 不是目录 */
#define EISDIR  21   /* 是目录 */
#define EINVAL  22   /* 参数无效 */
#define ENFILE  23   /* 系统打开文件表满 */
#define EMFILE  24   /* 进程打开文件过多 */
#define ENOTTY  25   /* 不是终端 */
#define EFBIG   27   /* 文件太大 */
#define ENOSPC  28   /* 设备无剩余空间 */
#define ESPIPE  29   /* 非法定位 */
#define EROFS   30   /* 只读文件系统 */
#define EPIPE   32   /* 管道断裂 */
#define EDOM    33   /* 数学参数超出定义域 */
#define ERANGE  34   /* 结果超出范围 */
#define ENOSYS  38   /* 功能未实现 */
#define ENOTEMPTY 39 /* 目录非空 */
#define ELOOP   40   /* 符号链接层数过多 */
#define ENAMETOOLONG 36
#define EOVERFLOW 75
#define ENOTSUP 95   /* 不支持（= EOPNOTSUPP） */
#define ETIMEDOUT 110
#define EWOULDBLOCK EAGAIN

extern __thread int errno;

#endif /* FE_POSIX_ERRNO_H */
