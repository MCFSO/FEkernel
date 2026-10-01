/* SPDX-License-Identifier: 0BSD */
/* <sys/types.h> —— POSIX 的基本类型（见 docs/10-posix-layer.md）。
 *
 * 宽度全部按 LP64 选（x86_64 上 POSIX 的约定），不是"能装下就行"：
 * `pid_t` 选 int 而不是 long，是因为程序里会把它和常量比较、
 * 也会 `printf("%d")`；宽度选错在 32/64 位交界处会静默截断。
 *
 * ★ 句柄与 fd 是两种东西，故意给两种类型 ★
 *   fe_handle_t（u32）——内核的能力句柄，进程内索引；
 *   fd（int）        —— POSIX 的文件描述符，用户态表里的索引。
 * 它们今天碰巧都是小整数，但语义完全不同：fd 可以被 dup2 重排、
 * 可以指向"管道缓冲区"这种根本没有内核对象的东西。
 * 用同一个 typedef 会让"哪个是真句柄"这件事在代码里消失。
 */
#ifndef FE_POSIX_SYS_TYPES_H
#define FE_POSIX_SYS_TYPES_H

#include <stddef.h>

typedef unsigned char  u_int8_t;
typedef unsigned short u_int16_t;
typedef unsigned int   u_int32_t;
typedef unsigned long  u_int64_t;

typedef long           off_t;
typedef int            pid_t;
typedef unsigned int   mode_t;
typedef unsigned int   uid_t;
typedef unsigned int   gid_t;
typedef unsigned long  ino_t;
typedef long           time_t;
typedef long           suseconds_t;
typedef unsigned int   nlink_t;
typedef long           blksize_t;
typedef long           blkcnt_t;
typedef unsigned long  dev_t;

#endif /* FE_POSIX_SYS_TYPES_H */
