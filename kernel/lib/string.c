/* SPDX-License-Identifier: 0BSD */
#include <fe/string.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    /* 大块按 8 字节步进；用 __builtin_memcpy 做定长 8 字节拷贝，
     * 既避免了对齐告警，也不会引入对 memcpy 自身的递归调用。 */
    if (n >= 16) {
        while (n >= 8) {
            __builtin_memcpy(d, s, 8);
            d += 8;
            s += 8;
            n -= 8;
        }
    }
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        return memcpy(dst, src, n);
    }
    /* 反向拷贝 */
    d += n;
    s += n;
    while (n--) {
        *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    u8 *d = (u8 *)dst;
    u8 v = (u8)c;
    if (n >= 16) {
        u64 pattern = (u64)v * 0x0101010101010101ull;
        while (n >= 8) {
            __builtin_memcpy(d, &pattern, 8);
            d += 8;
            n -= 8;
        }
    }
    while (n--) {
        *d++ = v;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = (const u8 *)a;
    const u8 *y = (const u8 *)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return (int)x[i] - (int)y[i];
        }
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const u8 *p = (const u8 *)s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (u8)c) {
            return (void *)(p + i);
        }
    }
    return NULL;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n]) {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(u8)*a - (int)(u8)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return (int)(u8)*a - (int)(u8)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *r = dst;
    while ((*dst++ = *src++) != '\0') {
    }
    return r;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = '\0';
    }
    return dst;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) {
            return (char *)s;
        }
        if (!*s) {
            return NULL;
        }
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c) {
            last = s;
        }
        if (!*s) {
            break;
        }
    }
    return (char *)last;
}

char *strstr(const char *hay, const char *needle)
{
    if (!*needle) {
        return (char *)hay;
    }
    for (; *hay; hay++) {
        const char *h = hay;
        const char *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n) {
            return (char *)hay;
        }
    }
    return NULL;
}

char *strcat(char *dst, const char *src)
{
    char *r = dst;
    while (*dst) {
        dst++;
    }
    while ((*dst++ = *src++) != '\0') {
    }
    return r;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t src_len = strlen(src);
    if (size > 0) {
        size_t n = (src_len < size - 1) ? src_len : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return src_len;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t dst_len = strnlen(dst, size);
    if (dst_len == size) {
        return size + strlen(src);
    }
    return dst_len + strlcpy(dst + dst_len, src, size - dst_len);
}

const char *parse_u64(const char *s, u64 *out, int base)
{
    if (!s || !*s) {
        return NULL;
    }
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
            s += 2;
        } else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
            base = 2;
            s += 2;
        } else if (s[0] == '0') {
            base = 8;
        } else {
            base = 10;
        }
    }
    u64 value = 0;
    const char *p = s;
    for (;; p++) {
        int d;
        char c = *p;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'z') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'Z') {
            d = c - 'A' + 10;
        } else {
            break;
        }
        if (d >= base) {
            break;
        }
        value = value * (u64)base + (u64)d;
    }
    if (p == s) {
        return NULL;
    }
    *out = value;
    return p;
}
