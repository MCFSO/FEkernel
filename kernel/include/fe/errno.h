/* SPDX-License-Identifier: 0BSD */
/* 错误码：**唯一**的定义处（内核与用户态共享）。
 *
 * ★ 为什么要单独一个文件 ★
 * 在路线 B 之前，这套错误码有两份：kernel/include/fe/status.h 里是全的，
 * 而 user/include/fe_user.h 里手抄了 12 个。手抄的那份漏了 FE_ERR_PIPE、
 * FE_ERR_IO、FE_ERR_KILLED…——于是"对端已关闭"在用户态只能落到 default 分支，
 * 被当成未知错误。两份清单必然漂移，而漂移的症状是"某个错误码在两边的名字不同"，
 * 这类问题只有在你恰好打印它时才会被发现。现在只有这一份。
 *
 * 约定（内核与用户态一致）：
 *   0     成功
 *   负数  失败，取值为下面的 FE_ERR_*
 * 系统调用返回时，rax 直接承载该值。
 */
#ifndef FE_ERRNO_H
#define FE_ERRNO_H

enum fe_errno {
    FE_OK                = 0,
    FE_ERR_INVAL         = -1,   /* 参数非法 */
    FE_ERR_NOMEM         = -2,   /* 内存不足 */
    FE_ERR_NOENT         = -3,   /* 不存在 */
    FE_ERR_EXIST         = -4,   /* 已存在 */
    FE_ERR_AGAIN         = -5,   /* 资源暂不可用，可重试 */
    FE_ERR_BUSY          = -6,   /* 忙 */
    FE_ERR_FAULT         = -7,   /* 用户内存访问越界 */
    FE_ERR_ACCESS        = -8,   /* 句柄无权限 / 能力不足 */
    FE_ERR_BADHANDLE     = -9,   /* 句柄无效 */
    FE_ERR_NOTSUP        = -10,  /* 未实现 */
    FE_ERR_IO            = -11,  /* 设备 I/O 错误 */
    FE_ERR_TIMEOUT       = -12,  /* 超时 */
    FE_ERR_RANGE         = -13,  /* 越界 */
    FE_ERR_NOSPC         = -14,  /* 空间不足 */
    FE_ERR_NAMETOOLONG   = -15,
    FE_ERR_NOTDIR        = -16,
    FE_ERR_ISDIR         = -17,
    FE_ERR_PIPE          = -18,  /* 对端已关闭 */
    FE_ERR_PROTO         = -19,  /* 协议错误 */
    FE_ERR_OVERFLOW      = -20,
    FE_ERR_CANCELED      = -21,
    FE_ERR_KILLED        = -22,
    FE_ERR_DEADLOCK      = -23,
    FE_ERR_INTERNAL      = -100, /* 内核内部不一致（bug） */
};

#endif /* FE_ERRNO_H */
