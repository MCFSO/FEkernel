/* SPDX-License-Identifier: 0BSD */
/* <string.h> —— 见 docs/10-posix-layer.md。
 * 实现全部自研（user/libposix/string.c），原型按 POSIX 规范。
 * 其中 memcpy/memset/memcmp/strlen/strcmp/strcpy/strchr… 已经在 libfe 里
 * 实现过一份（自研、带 SSE 优化的拷贝）；这里只补齐 POSIX 要求、
 * 而 libfe 当时没写的那些（strstr/strtok/strspn/memchr/strdup…）。 */
#ifndef FE_POSIX_STRING_H
#define FE_POSIX_STRING_H

#include <stddef.h>

void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *s, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
char  *strncat(char *dst, const char *src, size_t n);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcoll(const char *a, const char *b);       /* 无 locale：等同 strcmp */
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *hay, const char *needle);
char  *strpbrk(const char *s, const char *accept);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char  *strtok(char *s, const char *delim);
char  *strtok_r(char *s, const char *delim, char **saveptr);
char  *strdup(const char *s);
char  *strerror(int errnum);
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);

#endif /* FE_POSIX_STRING_H */
