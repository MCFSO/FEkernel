/* SPDX-License-Identifier: 0BSD */
/* 基础整数类型：**唯一**的定义处。
 *
 * ★ 为什么要单独一个文件 ★
 * 在路线 B 之前，这套 u8/u32/u64/usize 定义在 user/include/fe_user.h 里，
 * 而 POSIX 头需要 size_t/ssize_t 这些标准名。两套定义并存的结果必然是漂移：
 * 某天有人在一边加了 `typedef unsigned long u64;`，另一边是 `unsigned long long`，
 * 于是同一个结构体在两处的大小不同——症状是"字段莫名其妙是 0"
 * （这个项目已经为这类问题栽过几次，见 docs/08 的教训）。
 *
 * 现在：内核 ABI 的这套类型住在这里，fe_user.h 与 POSIX 头都只包含它。
 * 标准名（size_t 等）在 <stddef.h> 里，它们是同一批底层类型的别名，
 * 不是另一套定义。
 */
#ifndef FE_BASE_H
#define FE_BASE_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long      u64;
typedef signed char        i8;
typedef signed short       i16;
typedef signed int         i32;
typedef signed long        i64;
typedef unsigned long      usize;
typedef signed long        isize;

#ifndef __cplusplus
typedef _Bool bool;
#define true  1
#define false 0
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

#endif /* FE_BASE_H */
