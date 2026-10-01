/* SPDX-License-Identifier: 0BSD */
/* libposix：<string.h> 里 libfe 没有覆盖到的部分（见 docs/10-posix-layer.md）。
 *
 * ★ 为什么只有"补充"这一部分 ★
 * memcpy/memset/memcmp/strlen/strcmp/strcpy/strncpy/strcat/strncat/
 * strchr/strrchr/strncmp 已经在 libfe 里实现过一份（自研、拷贝带 SSE 优化、
 * 内存带宽实测 52 GB/s）。**再写一份就是两份实现**：
 * 两份 memcpy 混用时，哪一份被调用取决于链接顺序，而性能特征不同——
 * 这类问题在基准测试里会表现为"同一份代码两次跑出不同数字"。
 * 所以这里只补齐 POSIX 要求、而 libfe 当时没写的函数。
 */
#include <string.h>
#include <stdlib.h>
#include <errno.h>

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char want = (unsigned char)c;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == want) {
            return (void *)(p + i);
        }
    }
    return NULL;
}

/* memmove：libfe 只实现了 memcpy（**不处理重叠**），而 POSIX 要求 memmove
 * 在源与目标重叠时也正确。这不是学术差别：`memmove(buf, buf + 1, n)`
 * 用来删除一个字符是很常见的写法，用 memcpy 做会得到一段自我复制的垃圾。
 *
 * 判据是"目标是否落在源的后面且重叠"：只有那一种情况必须**从后往前**拷，
 * 其余用 memcpy 即可（快得多，而且是已经优化过的那一份）。 */
void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) {
        return dst;
    }
    if (d < s || d >= s + n) {
        return memcpy(dst, src, n);     /* 不重叠，或目标在前：正向拷 */
    }
    /* 目标在源的后面且重叠：从后往前，否则会自己覆盖自己 */
    for (size_t i = n; i > 0; i--) {
        d[i - 1] = s[i - 1];
    }
    return dst;
}

size_t strnlen(const char *s, size_t maxlen)
{
    size_t n = 0;
    while (n < maxlen && s[n]) {
        n++;
    }
    return n;
}

char *strstr(const char *hay, const char *needle)
{
    if (!hay || !needle) {
        return NULL;
    }
    if (!*needle) {
        return (char *)hay;
    }
    for (const char *p = hay; *p; p++) {
        const char *a = p;
        const char *b = needle;
        while (*a && *b && *a == *b) {
            a++;
            b++;
        }
        if (!*b) {
            return (char *)p;
        }
    }
    return NULL;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    for (; s[n]; n++) {
        if (!strchr(accept, s[n])) {
            break;
        }
    }
    return n;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    for (; s[n]; n++) {
        if (strchr(reject, s[n])) {
            break;
        }
    }
    return n;
}

char *strpbrk(const char *s, const char *accept)
{
    for (; *s; s++) {
        if (strchr(accept, *s)) {
            return (char *)s;
        }
    }
    return NULL;
}

char *strtok_r(char *s, const char *delim, char **saveptr)
{
    if (!saveptr || !delim) {
        return NULL;
    }
    char *p = s ? s : *saveptr;
    if (!p) {
        return NULL;
    }
    p += strspn(p, delim);          /* 跳过前导分隔符 */
    if (!*p) {
        *saveptr = NULL;
        return NULL;                /* 只有分隔符：没有下一个 token */
    }
    char *end = p + strcspn(p, delim);
    if (*end) {
        *end = '\0';
        *saveptr = end + 1;
    } else {
        *saveptr = NULL;
    }
    return p;
}

/* strtok 的可重入版本才是真的实现；非可重入版只是它的一个静态变量外壳。
 * 保留它是为了兼容老代码——但**不能在两个线程里同时用**，
 * 这一点 POSIX 自己也这么说（所以才有 strtok_r）。 */
static char *g_strtok_save;
char *strtok(char *s, const char *delim)
{
    return strtok_r(s, delim, &g_strtok_save);
}

char *strdup(const char *s)
{
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (!p) {
        errno = ENOMEM;
        return NULL;
    }
    memcpy(p, s, n);
    return p;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : *b;
        if (ca != cb) {
            return ca - cb;
        }
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int ca = (a[i] >= 'A' && a[i] <= 'Z') ? (a[i] + 32) : a[i];
        int cb = (b[i] >= 'A' && b[i] <= 'Z') ? (b[i] + 32) : b[i];
        if (ca != cb) {
            return ca - cb;
        }
        if (!a[i]) {
            return 0;
        }
    }
    return 0;
}

int strcoll(const char *a, const char *b)
{
    return strcmp(a, b);        /* 没有 locale：collate 就是字节序 */
}

/* strerror：★ 每个 errno 一句人话，而不是 "error 2" ★
 * 程序会把这句话直接打给用户（perror 就是这么做的）。
 * 打印数字等于把翻译工作推给读日志的人——而读日志的人往往不是写代码的人。 */
char *strerror(int errnum)
{
    switch (errnum) {
    case 0:      return "成功";
    case EPERM:  return "不允许的操作";
    case ENOENT: return "没有这个文件或目录";
    case ESRCH:  return "没有这个进程";
    case EINTR:  return "调用被中断";
    case EIO:    return "输入输出错误";
    case ENOEXEC:return "可执行文件格式错误";
    case EBADF:  return "坏的文件描述符";
    case ECHILD: return "没有子进程";
    case EAGAIN: return "资源暂时不可用";
    case ENOMEM: return "内存不足";
    case EACCES: return "权限不足";
    case EFAULT: return "地址无效";
    case EBUSY:  return "资源忙";
    case EEXIST: return "文件已存在";
    case ENODEV: return "没有这个设备";
    case ENOTDIR:return "不是目录";
    case EISDIR: return "是目录";
    case EINVAL: return "参数无效";
    case ENFILE: return "系统打开文件表已满";
    case EMFILE: return "进程打开文件过多";
    case ENOTTY: return "不是终端";
    case EFBIG:  return "文件太大";
    case ENOSPC: return "设备空间不足";
    case ESPIPE: return "不支持定位";
    case EROFS:  return "只读文件系统";
    case EPIPE:  return "管道已断开";
    case EDOM:   return "数学参数超出定义域";
    case ERANGE: return "结果超出范围";
    case ENOSYS: return "功能未实现";
    case ENOTEMPTY: return "目录非空";
    case ENAMETOOLONG: return "名字太长";
    case EOVERFLOW: return "值超出范围";
    case ENOTSUP: return "不支持该操作";
    case ETIMEDOUT: return "超时";
    default:     return "未知错误";
    }
}
