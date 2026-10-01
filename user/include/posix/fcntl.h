/* SPDX-License-Identifier: 0BSD */
/* <fcntl.h> —— 打开标志（见 docs/10-posix-layer.md）。
 *
 * ★ 取值用 POSIX 的八进制约定 ★ 程序里有 `open(p, O_WRONLY|O_CREAT, 0644)`
 * 这种调用，而我们**不支持**的位必须能被识别出来并报错，
 * 不能当成"没给"——那会让"要追加写"静默变成"覆盖写"。 */
#ifndef FE_POSIX_FCNTL_H
#define FE_POSIX_FCNTL_H

#include <sys/types.h>

#define O_RDONLY   00000000
#define O_WRONLY   00000001
#define O_RDWR     00000002
#define O_ACCMODE  00000003

#define O_CREAT    00000100
#define O_EXCL     00000200
#define O_NOCTTY   00000400
#define O_TRUNC    00001000
#define O_APPEND   00002000
#define O_NONBLOCK 00004000
#define O_DIRECTORY 00200000
#define O_CLOEXEC  02000000

int open(const char *path, int flags, ...);
int creat(const char *path, mode_t mode);

#endif /* FE_POSIX_FCNTL_H */
