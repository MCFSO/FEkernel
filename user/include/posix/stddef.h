/* SPDX-License-Identifier: 0BSD */
/* <stddef.h> —— 路线 B 的 POSIX 头之一（见 docs/10-posix-layer.md）。
 *
 * ★ 这些头是**我们自己写的**，不是抄来的 ★
 * 函数原型、常量取值按 POSIX 规范写（公开标准，与本项目"允许遵循 x86_64
 * 既成约定"是同一条边界）；实现全部在 user/libposix/ 里自研。
 *
 * ★ 为什么用标准头名（<stdio.h> 而不是 <fe_stdio.h>）★
 * 目标就是"普通 C 程序不改一行源码"，`#include <stdio.h>` 必须能工作。
 * 加上 -I user/include/posix 后，clang 的 freestanding 环境里没有同名头，
 * 不会冲突——这一点是实测确认的（第一个错误就是 'stdio.h' file not found）。
 */
#ifndef FE_POSIX_STDDEF_H
#define FE_POSIX_STDDEF_H

/* 内核 ABI 的基础类型（u8/u32/u64/usize…）也在这里可见：
 * libposix 的实现要用它们去对接 syscall，而 POSIX 头必须能独立包含。
 * 单一来源：fe_base.h。 */
#include <fe_base.h>

typedef unsigned long  size_t;
typedef long           ptrdiff_t;
typedef long           ssize_t;     /* POSIX 头之间共享，重复 typedef 无害 */
typedef int            wchar_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

#define offsetof(type, member) __builtin_offsetof(type, member)

#endif /* FE_POSIX_STDDEF_H */
