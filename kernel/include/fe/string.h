/* SPDX-License-Identifier: 0BSD */
/* 内核内存/字符串操作。
 * 这些符号会被编译器为结构体拷贝、数组清零等生成的隐式调用命中，因此签名必须与标准一致。
 */
#ifndef FE_STRING_H
#define FE_STRING_H

#include <fe/types.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *hay, const char *needle);
char  *strcat(char *dst, const char *src);

/* 更安全的变体（内核内部优先使用） */
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);

/* 数字解析：成功返回值指针，失败返回 NULL */
const char *parse_u64(const char *s, u64 *out, int base);

#endif /* FE_STRING_H */
