/* SPDX-License-Identifier: 0BSD */
/* <stdio.h> —— 格式化输出与文件流（见 docs/10-posix-layer.md）。
 *
 * ★ FILE 是**真的流对象**，不是 fd 的别名 ★
 * 程序依赖 stdio 的缓冲语义：`printf` 攒着、遇到换行或缓冲满才真的写；
 * `fflush`/`fclose`/`exit` 会把它吐出去。把 FILE 做成 fd 的薄壳
 * （每次 printf 都 write 一次系统调用）会**看起来也能跑**，
 * 直到某个程序发现自己 1 万次 printf 变成 1 万次 IPC——
 * 那是性能问题；更糟的是 `setvbuf`/`ungetc` 这类语义会直接不成立。
 *
 * 我们实现有缓冲的流：BUFSIZ 大小，行缓冲（终端）或全缓冲（文件）。
 * "是不是终端"由 isatty(fd) 回答，而那个答案来自内核给我们的信息
 * （帧缓冲控制台是字符设备）——不能靠猜。 */
#ifndef FE_POSIX_STDIO_H
#define FE_POSIX_STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>

#define BUFSIZ 4096
#define EOF    (-1)
#define FOPEN_MAX 32

typedef struct __posix_file FILE;

/* 三个标准流。它们是**对象**而不是指针常量：
 * 程序会取 &stdout 的地址、会比较 fp == stderr。 */
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE  *fopen(const char *path, const char *mode);
FILE  *fdopen(int fd, const char *mode);
int    fclose(FILE *fp);
int    fflush(FILE *fp);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp);
int    fgetc(FILE *fp);
int    getc(FILE *fp);
int    getchar(void);
int    fputc(int c, FILE *fp);
int    putc(int c, FILE *fp);
int    putchar(int c);
char  *fgets(char *s, int size, FILE *fp);
int    fputs(const char *s, FILE *fp);
int    puts(const char *s);
int    ungetc(int c, FILE *fp);
int    feof(FILE *fp);
int    ferror(FILE *fp);
void   clearerr(FILE *fp);
int    fileno(FILE *fp);
int    fseek(FILE *fp, long offset, int whence);
long   ftell(FILE *fp);
void   rewind(FILE *fp);
int    setvbuf(FILE *fp, char *buf, int mode, size_t size);
int    remove(const char *path);
int    rename(const char *oldpath, const char *newpath);
int    fscanf(FILE *fp, const char *fmt, ...);
int    sscanf(const char *s, const char *fmt, ...);

int    printf(const char *fmt, ...);
int    fprintf(FILE *fp, const char *fmt, ...);
int    sprintf(char *buf, const char *fmt, ...);
int    snprintf(char *buf, size_t cap, const char *fmt, ...);
int    vprintf(const char *fmt, va_list ap);
int    vfprintf(FILE *fp, const char *fmt, va_list ap);
int    vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
void   perror(const char *s);

/* 缓冲模式 */
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

#endif /* FE_POSIX_STDIO_H */
