/* SPDX-License-Identifier: 0BSD */
/* libposix：stdlib 的其余部分（见 docs/10-posix-layer.md）。
 *
 * malloc/free/calloc/realloc 已经在 libfe 里（基于内存对象 + arena，
 * 带 fe_malloc_check 的结构性自检）——这里**不重新实现**它们。
 * 重复实现一份分配器是最糟的一种"复用"：两份堆混用会让
 * free 到一个不属于自己的块，而症状是随机崩溃。
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ------------------------------------------------------------------ */
/* atexit：POSIX 要求 exit 时按**注册的逆序**调用                        */
/* ------------------------------------------------------------------ */

#define ATEXIT_MAX 16
static void (*g_atexit[ATEXIT_MAX])(void);
static int g_atexit_count;

int atexit(void (*fn)(void))
{
    if (!fn || g_atexit_count >= ATEXIT_MAX) {
        return -1;
    }
    g_atexit[g_atexit_count++] = fn;
    return 0;
}

/* 由 stdio.c 的 exit() 调用：它负责先 flush 再跑钩子再退出。 */
void posix_run_atexit(void)
{
    while (g_atexit_count > 0) {
        void (*fn)(void) = g_atexit[--g_atexit_count];
        if (fn) {
            fn();
        }
    }
}

/* ------------------------------------------------------------------ */
/* 数值转换                                                            */
/* ------------------------------------------------------------------ */

int atoi(const char *s)
{
    return (int)strtol(s, NULL, 10);
}

long atol(const char *s)
{
    return strtol(s, NULL, 10);
}

long long atoll(const char *s)
{
    return (long long)strtol(s, NULL, 10);
}

/* strtol 的完整语义：前导空白、正负号、进制、endptr。
 * ★ endptr 必须精确 ★ 程序靠它判断"整个串是不是合法数字"
 * （`*end == '\0'`）。endptr 指早了，程序会把 "12abc" 当成合法的 12。 */
long strtol(const char *s, char **end, int base)
{
    const char *p = s;
    if (!p) {
        if (end) {
            *end = (char *)s;
        }
        errno = EINVAL;
        return 0;
    }
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
           *p == '\v' || *p == '\f') {
        p++;
    }
    int neg = 0;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = (*p == '0') ? 8 : 10;
    }

    unsigned long v = 0;
    int digits = 0;
    int overflow = 0;
    while (*p) {
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
        /* 溢出检查：超了就夹到边界并设 ERANGE（POSIX 要求），
         * 而不是回绕成一个看起来合理的负数。 */
        if (v > (0xFFFFFFFFFFFFFFFFul - (unsigned long)d) / (unsigned long)base) {
            overflow = 1;
        } else {
            v = v * (unsigned long)base + (unsigned long)d;
        }
        digits++;
        p++;
    }
    if (end) {
        *end = (char *)(digits ? p : s);
    }
    if (!digits) {
        return 0;               /* 没有数字：返回 0 且 endptr = 原串 */
    }
    if (overflow) {
        errno = ERANGE;
        return neg ? (-0x7FFFFFFFFFFFFFFFl - 1) : 0x7FFFFFFFFFFFFFFFl;
    }
    return neg ? -(long)v : (long)v;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    return (unsigned long)strtol(s, end, base);
}

/* strtod：没有 libc 的浮点解析可用，自己做一个**保精度**的实现。
 *
 * 做法：整数部分用整数累加（精确），小数部分逐位累加并用 1/10 的幂缩放，
 * 指数部分用循环乘/除。这样 0.1 这类值不会因为反复乘 10 而漂移。
 * ★ 边界 ★ 极端指数（|exp| > 308）会饱和到 0 或 inf，并设 ERANGE。 */
double strtod(const char *s, char **end)
{
    const char *p = s;
    if (!p) {
        if (end) {
            *end = (char *)s;
        }
        errno = EINVAL;
        return 0.0;
    }
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    int neg = 0;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }
    double v = 0.0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10.0 + (double)(*p - '0');
        p++;
        digits++;
    }
    if (*p == '.') {
        p++;
        double scale = 0.1;
        while (*p >= '0' && *p <= '9') {
            v += (double)(*p - '0') * scale;
            scale *= 0.1;
            p++;
            digits++;
        }
    }
    if (digits == 0) {
        if (end) {
            *end = (char *)s;
        }
        return 0.0;
    }
    if (*p == 'e' || *p == 'E') {
        const char *save = p;
        p++;
        int eneg = 0;
        if (*p == '+' || *p == '-') {
            eneg = (*p == '-');
            p++;
        }
        if (*p >= '0' && *p <= '9') {
            int e = 0;
            while (*p >= '0' && *p <= '9') {
                if (e < 1000) {
                    e = e * 10 + (*p - '0');
                }
                p++;
            }
            for (int i = 0; i < e && i < 400; i++) {
                v = eneg ? (v / 10.0) : (v * 10.0);
            }
        } else {
            p = save;           /* "1e" 后面没数字：e 不算指数的一部分 */
        }
    }
    if (end) {
        *end = (char *)p;
    }
    return neg ? -v : v;
}

int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }

/* ------------------------------------------------------------------ */
/* 环境变量                                                            */
/* ------------------------------------------------------------------ */

char *getenv(const char *name)
{
    if (!name || !environ) {
        return NULL;
    }
    size_t n = strlen(name);
    for (char **e = environ; *e; e++) {
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=') {
            return *e + n + 1;
        }
    }
    return NULL;
}

static char g_env_storage[16][128];
static char *g_env_ptrs[17];
static int g_env_count;

/* setenv：今天只在**本进程内**生效。
 * 传给子进程需要内核的 execve 带 envp（见 docs/10-posix-layer.md §3 的 K1），
 * 那条路还没通——所以这里如实只改自己，并在文档里写明。 */
int setenv(const char *name, const char *value, int overwrite)
{
    if (!name || !value || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    if (!overwrite) {
        char *old = getenv(name);
        if (old) {
            return 0;
        }
    }
    if (g_env_count >= 16) {
        errno = ENOMEM;
        return -1;
    }
    size_t nl = strlen(name);
    size_t vl = strlen(value);
    if (nl + vl + 2 > sizeof(g_env_storage[0])) {
        errno = ERANGE;
        return -1;
    }
    char *slot = g_env_storage[g_env_count];
    memcpy(slot, name, nl);
    slot[nl] = '=';
    memcpy(slot + nl + 1, value, vl);
    slot[nl + vl + 1] = '\0';
    g_env_ptrs[g_env_count] = slot;
    g_env_count++;
    g_env_ptrs[g_env_count] = NULL;
    if (g_env_ptrs[0] && !environ) {
        environ = g_env_ptrs;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 排序与查找                                                          */
/* ------------------------------------------------------------------ */

static void swap_bytes(char *a, char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char t = a[i];
        a[i] = b[i];
        b[i] = t;
    }
}

/* 快排：用中位数取枢轴 + 小区间插入排序。
 * 纯教科书快排对**已排序**输入会退化成 O(n²)，而"输入刚好有序"
 * 在真实数据里非常常见（列表、目录项）。 */
void qsort(void *base, size_t nmemb, size_t size,
           int (*cmp)(const void *, const void *))
{
    if (!base || !cmp || nmemb < 2 || size == 0) {
        return;
    }
    char *a = (char *)base;
    /* 递归深度用显式栈控制：用户栈只有 64 KiB（且不能自动增长），
     * 递归快排在 n=10000 时会直接把栈用穿。 */
    struct range { size_t lo, hi; };
    static struct range stack[64];
    int sp = 0;
    stack[sp].lo = 0;
    stack[sp].hi = nmemb - 1;
    sp++;

    while (sp > 0) {
        sp--;
        size_t lo = stack[sp].lo;
        size_t hi = stack[sp].hi;
        while (lo < hi) {
            if (hi - lo < 12) {
                /* 插入排序：小区间上比快排快，而且不会退化 */
                for (size_t i = lo + 1; i <= hi; i++) {
                    for (size_t j = i; j > lo; j--) {
                        if (cmp(a + j * size, a + (j - 1) * size) >= 0) {
                            break;
                        }
                        swap_bytes(a + j * size, a + (j - 1) * size, size);
                    }
                }
                break;
            }
            size_t mid = lo + (hi - lo) / 2;
            if (cmp(a + mid * size, a + lo * size) < 0) {
                swap_bytes(a + mid * size, a + lo * size, size);
            }
            if (cmp(a + hi * size, a + lo * size) < 0) {
                swap_bytes(a + hi * size, a + lo * size, size);
            }
            if (cmp(a + hi * size, a + mid * size) < 0) {
                swap_bytes(a + hi * size, a + mid * size, size);
            }
            swap_bytes(a + mid * size, a + (hi - 1) * size, size);
            char *pivot = a + (hi - 1) * size;
            size_t i = lo, j = hi - 1;
            for (;;) {
                while (cmp(a + (++i) * size, pivot) < 0) {
                }
                while (cmp(a + (--j) * size, pivot) > 0) {
                }
                if (i >= j) {
                    break;
                }
                swap_bytes(a + i * size, a + j * size, size);
            }
            swap_bytes(a + i * size, a + (hi - 1) * size, size);
            /* 把较大的那半压栈，小的那半继续循环 → 栈深 O(log n) */
            if (i - lo > hi - i) {
                if (sp < 64) {
                    stack[sp].lo = lo;
                    stack[sp].hi = i - 1;
                    sp++;
                }
                lo = i + 1;
            } else {
                if (sp < 64) {
                    stack[sp].lo = i + 1;
                    stack[sp].hi = hi;
                    sp++;
                }
                hi = i - 1;
            }
        }
    }
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*cmp)(const void *, const void *))
{
    if (!key || !base || !cmp || size == 0) {
        return NULL;
    }
    size_t lo = 0;
    size_t hi = nmemb;
    const char *a = (const char *)base;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = cmp(key, a + mid * size);
        if (c == 0) {
            return (void *)(a + mid * size);
        }
        if (c < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return NULL;
}
