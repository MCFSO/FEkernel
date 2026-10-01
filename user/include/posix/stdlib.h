/* SPDX-License-Identifier: 0BSD */
/* <stdlib.h> —— 见 docs/10-posix-layer.md。
 *
 * malloc/free/calloc/realloc 已经在 libfe 里实现（基于内存对象 + arena），
 * 这里只是把 POSIX 名字暴露出来，并补上 POSIX 要求的其余入口。
 *
 * ★ exit() 与 _exit() 的区别必须保留 ★
 *   exit    —— 先冲洗 stdio 缓冲、跑 atexit 钩子，再结束进程；
 *   _exit   —— 立刻结束，**不冲洗**。
 * 我们的 fe_exit 是"立刻结束"（它自己会 flush 行缓冲，因为那是 libfe 的
 * 日志缓冲，不是 stdio 的）。把 exit 实现成直接调 fe_exit 会让
 * `printf("...")` 后跟 `exit(1)` 的输出**丢失**——那是最经典的
 * "程序明明跑了却没输出"。 */
#ifndef FE_POSIX_STDLIB_H
#define FE_POSIX_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

int    atoi(const char *s);
long   atol(const char *s);
long long atoll(const char *s);
long   strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
double strtod(const char *s, char **end);

void   abort(void) __attribute__((noreturn));
void   exit(int status) __attribute__((noreturn));
void   _exit(int status) __attribute__((noreturn));
int    atexit(void (*fn)(void));

char  *getenv(const char *name);
int    setenv(const char *name, const char *value, int overwrite);

int    abs(int v);
long   labs(long v);

void   qsort(void *base, size_t nmemb, size_t size,
             int (*cmp)(const void *, const void *));
void  *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
               int (*cmp)(const void *, const void *));

/* 环境变量块。POSIX 里它是一个 char*[]；我们用同一个形状，
 * 好让 `extern char **environ` 这种写法不用改。 */
extern char **environ;

#endif /* FE_POSIX_STDLIB_H */
