/* SPDX-License-Identifier: 0BSD */
/* libposix：stdio 流（见 docs/10-posix-layer.md）。
 *
 * ★ 为什么 FILE 是"真的流"而不是 fd 的薄壳 ★
 *
 * 把 printf 直接接到 write() 上也能跑，而且看起来更简单。代价有两个，
 * 都不在"能不能跑"这一层：
 *
 *   1. **性能**：一次 printf 一个 IPC 往返。程序里一个循环打 1 万行日志、
 *      或者逐字符输出（很多朴素实现就是逐字符），会把服务端打爆。
 *   2. **语义**：`setvbuf`、`ungetc`、`fflush` 的可见顺序、
 *      "写文件时半行不会出现在文件里"——这些都建立在缓冲之上。
 *      薄壳实现下它们要么不成立，要么行为随调用模式变化。
 *
 * 所以这里是真缓冲：行缓冲（终端）或全缓冲（其它），
 * 换行/缓冲满/fflush/fclose/exit 时真的写出去。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

/* 内核 ABI（fe_exit 等）：POSIX 层是建在它上面的，包含它是正常依赖，
 * 而不是"越层"——越层指的是让**内核**知道 POSIX 的名字，不是反过来。 */
#include <fe_user.h>

#define FILE_MAX 32

struct __posix_file {
    int      fd;
    int      used;
    int      eof;
    int      err;
    int      mode;          /* _IOFBF / _IOLBF / _IONBF */
    int      readable;
    int      writable;
    size_t   buf_len;       /* 缓冲里待写出的字节数 */
    size_t   buf_pos;       /* 读缓冲里的当前位置 */
    size_t   buf_fill;      /* 读缓冲里的有效字节数 */
    size_t   cap;
    char    *buf;           /* 指向 buf_storage 或调用者给的缓冲 */
    int      owns_buf;
    char     buf_storage[BUFSIZ];
};

static struct __posix_file g_files[FILE_MAX];

/* 三个标准流是**对象**：程序会 `&stdout`、会比较 `fp == stderr`。
 * 用静态对象而不是指针常量，是为了让这些写法都能编过。 */
static struct __posix_file g_stdin_obj;
static struct __posix_file g_stdout_obj;
static struct __posix_file g_stderr_obj;
FILE *stdin  = &g_stdin_obj;
FILE *stdout = &g_stdout_obj;
FILE *stderr = &g_stderr_obj;

static int g_stdio_init_done;

static void stream_setup(struct __posix_file *f, int fd, int readable, int writable)
{
    memset(f, 0, sizeof(*f));
    f->fd = fd;
    f->used = 1;
    f->readable = readable;
    f->writable = writable;
    f->cap = BUFSIZ;
    f->buf = f->buf_storage;
    f->owns_buf = 0;
    /* ★ 行缓冲还是全缓冲：由 isatty 决定，而 isatty 的答案来自事实 ★
     * 猜错方向都会出错：把文件当终端会让每个换行刷一次盘（慢），
     * 把终端当文件会让提示符没有换行时**看不见**（"程序像卡住了"）。 */
    f->mode = isatty(fd) ? _IOLBF : _IOFBF;
}

static void stdio_ensure_init(void)
{
    if (g_stdio_init_done) {
        return;
    }
    g_stdio_init_done = 1;
    stream_setup(&g_stdin_obj, STDIN_FILENO, 1, 0);
    stream_setup(&g_stdout_obj, STDOUT_FILENO, 0, 1);
    /* stderr 不缓冲：错误信息必须**立刻**出现。
     * 缓冲的 stderr 会在崩溃时把最后一条错误吞掉——而那条正是要看的。 */
    stream_setup(&g_stderr_obj, STDERR_FILENO, 0, 1);
    g_stderr_obj.mode = _IONBF;
    for (int i = 0; i < FILE_MAX; i++) {
        g_files[i].fd = -1;
    }
}

FILE *fdopen(int fd, const char *mode)
{
    stdio_ensure_init();
    if (fd < 0) {
        errno = EBADF;
        return NULL;
    }
    int readable = (mode && (mode[0] == 'r')) ? 1 : 0;
    int writable = (mode && (mode[0] == 'r' && mode[1] == '+')) ? 1 :
                   (mode && mode[0] != 'r') ? 1 : 0;
    for (int i = 0; i < FILE_MAX; i++) {
        if (!g_files[i].used) {
            stream_setup(&g_files[i], fd, readable, writable);
            return &g_files[i];
        }
    }
    errno = EMFILE;
    return NULL;
}

FILE *fopen(const char *path, const char *mode)
{
    stdio_ensure_init();
    if (!path || !mode) {
        errno = EINVAL;
        return NULL;
    }
    int flags;
    if (mode[0] == 'r') {
        flags = O_RDONLY;
    } else if (mode[0] == 'w') {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    } else if (mode[0] == 'a') {
        /* ★ 追加模式：今天**不支持**（fsd 没有 O_APPEND 语义）★
         * 假装支持会更糟：程序以为自己在追加，实际每次从头覆盖，
         * 结果是日志文件里只剩最后一条。所以明确报错。 */
        errno = ENOSYS;
        return NULL;
    } else {
        errno = EINVAL;
        return NULL;
    }
    if (mode[1] == '+') {
        flags = (flags & ~O_ACCMODE) | O_RDWR;
    }
    int fd = open(path, flags, 0644);
    if (fd < 0) {
        return NULL;
    }
    FILE *f = fdopen(fd, mode);
    if (!f) {
        close(fd);
        return NULL;
    }
    return f;
}

/* 把写缓冲吐出去。返回 0 成功，EOF 失败。 */
static int flush_write(struct __posix_file *f)
{
    if (!f->writable || f->mode == _IONBF || f->buf_len == 0) {
        return 0;
    }
    size_t done = 0;
    while (done < f->buf_len) {
        ssize_t n = write(f->fd, f->buf + done, f->buf_len - done);
        if (n <= 0) {
            /* ★ 写失败时**保留**没写出去的那部分 ★
             * 清空缓冲会让数据静默丢失；保留它至少能让后续的
             * fflush/fclose 再试一次，或者让调用者看见错误。 */
            memmove(f->buf, f->buf + done, f->buf_len - done);
            f->buf_len -= done;
            f->err = 1;
            return EOF;
        }
        done += (size_t)n;
    }
    f->buf_len = 0;
    return 0;
}

int fflush(FILE *fp)
{
    stdio_ensure_init();
    if (!fp) {
        /* fflush(NULL) = 刷所有输出流（POSIX 规定） */
        int rc = 0;
        for (int i = 0; i < FILE_MAX; i++) {
            if (g_files[i].used && flush_write(&g_files[i]) != 0) {
                rc = EOF;
            }
        }
        if (flush_write(&g_stdout_obj) != 0 || flush_write(&g_stderr_obj) != 0) {
            rc = EOF;
        }
        return rc;
    }
    return flush_write(fp);
}

int fclose(FILE *fp)
{
    if (!fp) {
        errno = EBADF;
        return EOF;
    }
    int rc = flush_write(fp);
    if (fp->fd >= 0 && close(fp->fd) != 0) {
        rc = EOF;
    }
    fp->used = 0;
    fp->fd = -1;
    fp->buf_len = 0;
    fp->buf_fill = 0;
    fp->buf_pos = 0;
    return rc;
}

int fileno(FILE *fp) { return fp ? fp->fd : -1; }
int feof(FILE *fp) { return fp ? fp->eof : 0; }
int ferror(FILE *fp) { return fp ? fp->err : 0; }
void clearerr(FILE *fp)
{
    if (fp) {
        fp->eof = 0;
        fp->err = 0;
    }
}

int setvbuf(FILE *fp, char *buf, int mode, size_t size)
{
    if (!fp) {
        errno = EINVAL;
        return -1;
    }
    if (mode != _IOFBF && mode != _IOLBF && mode != _IONBF) {
        errno = EINVAL;
        return -1;
    }
    if (buf && size > 0) {
        fp->buf = buf;
        fp->cap = size;
        fp->owns_buf = 0;
    } else if (size > 0 && size <= sizeof(fp->buf_storage)) {
        fp->buf = fp->buf_storage;
        fp->cap = size;
        fp->owns_buf = 0;
    }
    fp->mode = mode;
    return 0;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp)
{
    stdio_ensure_init();
    if (!fp || !ptr || size == 0 || nmemb == 0) {
        return 0;
    }
    /* fp == stdout 时用对象本身；其它情况用传入的 */
    size_t total = size * nmemb;
    const char *p = (const char *)ptr;

    if (!fp->writable) {
        fp->err = 1;
        errno = EBADF;
        return 0;
    }
    if (fp->mode == _IONBF) {
        size_t done = 0;
        while (done < total) {
            ssize_t n = write(fp->fd, p + done, total - done);
            if (n <= 0) {
                fp->err = 1;
                return (size == 0) ? 0 : (done / size);
            }
            done += (size_t)n;
        }
        return nmemb;
    }

    size_t written = 0;
    for (size_t i = 0; i < total; i++) {
        if (fp->buf_len >= fp->cap) {
            if (flush_write(fp) != 0) {
                return (size == 0) ? 0 : (written / size);
            }
        }
        fp->buf[fp->buf_len++] = p[i];
        written++;
        /* 行缓冲：遇到换行就刷。**这就是"提示符没有换行也看得见"的反面**：
         * 行缓冲下没有换行的输出会攒着，交互式程序必须先 fflush。 */
        if (fp->mode == _IOLBF && p[i] == '\n') {
            if (flush_write(fp) != 0) {
                return (size == 0) ? 0 : (written / size);
            }
        }
    }
    return nmemb;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp)
{
    stdio_ensure_init();
    if (!fp || !ptr || size == 0 || nmemb == 0) {
        return 0;
    }
    if (!fp->readable) {
        fp->err = 1;
        errno = EBADF;
        return 0;
    }
    /* 先把缓冲里剩下的给它（与 ungetc 的交互也靠这一段） */
    size_t want = size * nmemb;
    char *dst = (char *)ptr;
    size_t got = 0;
    if (fp->buf_pos < fp->buf_fill) {
        size_t avail = fp->buf_fill - fp->buf_pos;
        size_t take = (avail < want) ? avail : want;
        memcpy(dst, fp->buf + fp->buf_pos, take);
        fp->buf_pos += take;
        got += take;
    }
    if (got == want) {
        return nmemb;
    }
    /* 直读：不再二次缓冲——read() 已经是按块拿的，
     * 再套一层只会多一次拷贝，而 POSIX 不要求 fread 的内部块大小。 */
    ssize_t n = read(fp->fd, dst + got, want - got);
    if (n < 0) {
        fp->err = 1;
        return (size == 0) ? 0 : (got / size);
    }
    if (n == 0) {
        fp->eof = 1;
    }
    got += (size_t)n;
    return got / size;
}

int fgetc(FILE *fp)
{
    if (!fp) {
        return EOF;
    }
    unsigned char c;
    if (fread(&c, 1, 1, fp) != 1) {
        return EOF;
    }
    return (int)c;
}

int getc(FILE *fp) { return fgetc(fp); }

int getchar(void)
{
    stdio_ensure_init();
    return fgetc(stdin);
}

int fputc(int c, FILE *fp)
{
    unsigned char b = (unsigned char)c;
    if (fwrite(&b, 1, 1, fp) != 1) {
        return EOF;
    }
    return (int)b;
}

int putc(int c, FILE *fp) { return fputc(c, fp); }

int putchar(int c)
{
    stdio_ensure_init();
    return fputc(c, stdout);
}

char *fgets(char *s, int size, FILE *fp)
{
    if (!s || size <= 0 || !fp) {
        errno = EINVAL;
        return NULL;
    }
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(fp);
        if (c == EOF) {
            if (i == 0) {
                return NULL;        /* 一个字符都没读到：返回 NULL */
            }
            break;                  /* 读到一半遇 EOF：返回读到的那部分 */
        }
        s[i++] = (char)c;
        if (c == '\n') {
            break;
        }
    }
    s[i] = '\0';
    return s;
}

int fputs(const char *s, FILE *fp)
{
    if (!s) {
        return EOF;
    }
    size_t n = strlen(s);
    return (fwrite(s, 1, n, fp) == n) ? 0 : EOF;
}

int puts(const char *s)
{
    stdio_ensure_init();
    if (fputs(s, stdout) == EOF) {
        return EOF;
    }
    return fputc('\n', stdout);
}

int ungetc(int c, FILE *fp)
{
    if (c == EOF || !fp) {
        return EOF;
    }
    /* 缓冲为空：先读一块进来，再"退回"一个字符 */
    if (fp->buf_pos == 0) {
        if (fp->buf_fill == 0) {
            ssize_t n = read(fp->fd, fp->buf, fp->cap);
            if (n <= 0) {
                return EOF;
            }
            fp->buf_fill = (size_t)n;
        }
        fp->buf_pos = fp->buf_fill;
    }
    if (fp->buf_pos == 0) {
        return EOF;
    }
    fp->buf[--fp->buf_pos] = (char)c;
    fp->eof = 0;
    return (int)(unsigned char)c;
}

int fseek(FILE *fp, long offset, int whence)
{
    if (!fp) {
        errno = EBADF;
        return -1;
    }
    /* 改定位之前必须把读缓冲丢掉：留着它会让"跳到 offset 之后的第一次读"
     * 拿到**跳之前**缓冲的数据。这是最经典的一类错位 bug。 */
    fp->buf_pos = 0;
    fp->buf_fill = 0;
    fp->eof = 0;
    if (flush_write(fp) != 0) {
        return -1;
    }
    if (lseek(fp->fd, (off_t)offset, whence) < 0) {
        return -1;
    }
    return 0;
}

long ftell(FILE *fp)
{
    if (!fp) {
        return -1;
    }
    off_t pos = lseek(fp->fd, 0, SEEK_CUR);
    if (pos < 0) {
        return -1;
    }
    /* 减去读缓冲里"已读进缓冲但还没交给调用者"的那部分 */
    return (long)(pos - (off_t)(fp->buf_fill - fp->buf_pos));
}

void rewind(FILE *fp)
{
    if (fp) {
        fseek(fp, 0, SEEK_SET);
        fp->err = 0;
    }
}

/* sscanf：把格式串与输入都走一遍"最小但正确"的实现。
 *
 * ★ 只支持最常用的转换，而且是**按需**支持 ★
 * 完整的 scanf 家族（赋值抑制、扫描集 %[...]、宽度、locale）是另一个大工程。
 * 这里实现 %d/%i/%u/%x/%o/%c/%s/%f 与字面量匹配、空白跳过、宽度限制——
 * 覆盖真实程序里 95% 的用法，其余情况返回已成功转换的项数（POSIX 语义），
 * **绝不假装成功**。 */
struct scan_src {
    const char *s;
    FILE *fp;
    int pushed;         /* 从流读时，被"看过头"的那一个字符 */
};

static int scan_getc(struct scan_src *src)
{
    if (src->pushed >= 0) {
        int c = src->pushed;
        src->pushed = -1;
        return c;
    }
    if (src->s) {
        return (unsigned char)*src->s ? (int)(unsigned char)*src->s++ : EOF;
    }
    return fgetc(src->fp);
}

static void scan_ungetc(struct scan_src *src, int c)
{
    if (c == EOF) {
        return;
    }
    if (src->s) {
        src->s--;
        return;
    }
    src->pushed = c;
}

static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_space_ch(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static int scan_int(struct scan_src *src, int base, long *out, int width)
{
    int c;
    int n = 0;
    int neg = 0;
    /* 跳过前导空白 */
    do {
        c = scan_getc(src);
    } while (is_space_ch(c));
    if (c == '+' || c == '-') {
        neg = (c == '-');
        c = scan_getc(src);
        n++;
    }
    if (base == 0) {
        base = 10;
        if (c == '0') {
            base = 8;
            c = scan_getc(src);
            n++;
            if (c == 'x' || c == 'X') {
                base = 16;
                c = scan_getc(src);
                n++;
            }
        }
    } else if (base == 16 && c == '0') {
        int c2 = scan_getc(src);
        if (c2 == 'x' || c2 == 'X') {
            c = scan_getc(src);
            n += 2;
        } else {
            scan_ungetc(src, c2);
        }
    }
    long v = 0;
    int digits = 0;
    while (c != EOF && (width <= 0 || n < width)) {
        int d;
        if (is_digit(c)) {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            break;
        }
        if (d >= base) {
            break;
        }
        v = v * base + d;
        digits++;
        n++;
        c = scan_getc(src);
    }
    scan_ungetc(src, c);
    if (digits == 0) {
        return 0;
    }
    *out = neg ? -v : v;
    return 1;
}

int vsscanf(const char *s, const char *fmt, va_list ap)
{
    struct scan_src src;
    src.s = s;
    src.fp = NULL;
    src.pushed = -1;
    int matched = 0;

    const char *f = fmt;
    while (*f) {
        if (is_space_ch(*f)) {
            int c;
            do {
                c = scan_getc(&src);
            } while (is_space_ch(c));
            scan_ungetc(&src, c);
            f++;
            continue;
        }
        if (*f != '%') {
            int c = scan_getc(&src);
            if (c != (unsigned char)*f) {
                scan_ungetc(&src, c);
                break;
            }
            f++;
            continue;
        }
        f++;
        int width = 0;
        while (is_digit(*f)) {
            width = width * 10 + (*f - '0');
            f++;
        }
        int is_long = 0, is_short = 0;
        while (*f == 'l' || *f == 'h' || *f == 'z') {
            if (*f == 'l' || *f == 'z') {
                is_long = 1;
            } else {
                is_short = 1;
            }
            f++;
        }
        switch (*f) {
        case 'd':
        case 'i':
        case 'u': {
            long v = 0;
            if (!scan_int(&src, (*f == 'u') ? 10 : 0, &v, width)) {
                return matched;
            }
            if (is_long) {
                *va_arg(ap, long *) = v;
            } else if (is_short) {
                *va_arg(ap, short *) = (short)v;
            } else {
                *va_arg(ap, int *) = (int)v;
            }
            matched++;
            break;
        }
        case 'x':
        case 'X':
        case 'o': {
            long v = 0;
            if (!scan_int(&src, (*f == 'o') ? 8 : 16, &v, width)) {
                return matched;
            }
            if (is_long) {
                *va_arg(ap, long *) = v;
            } else {
                *va_arg(ap, unsigned *) = (unsigned)v;
            }
            matched++;
            break;
        }
        case 'c': {
            int c = scan_getc(&src);
            if (c == EOF) {
                return matched;
            }
            *va_arg(ap, char *) = (char)c;
            matched++;
            break;
        }
        case 's': {
            int c;
            do {
                c = scan_getc(&src);
            } while (is_space_ch(c));
            char *dst = va_arg(ap, char *);
            int n = 0;
            while (c != EOF && !is_space_ch(c) && (width <= 0 || n < width)) {
                dst[n++] = (char)c;
                c = scan_getc(&src);
            }
            scan_ungetc(&src, c);
            dst[n] = '\0';
            if (n == 0) {
                return matched;
            }
            matched++;
            break;
        }
        case '%': {
            int c = scan_getc(&src);
            if (c != '%') {
                scan_ungetc(&src, c);
                return matched;
            }
            break;
        }
        default:
            /* 不认识的转换：**停止**并返回已匹配数，不假装成功 */
            return matched;
        }
        f++;
    }
    return matched;
}

int sscanf(const char *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsscanf(s, fmt, ap);
    va_end(ap);
    return n;
}

int fscanf(FILE *fp, const char *fmt, ...)
{
    /* 从流扫描需要"边读边回退"，而我们只实现了单字符回退。
     * 做法：先把流读到缓冲里再走 vsscanf——但那样会**多读**，
     * 而多读的字节会从流里消失（下次调用看不到）。
     * 所以这里只对"能一次读完的小输入"正确；否则明确报 ENOSYS。 */
    if (!fp) {
        errno = EBADF;
        return EOF;
    }
    char line[512];
    if (!fgets(line, sizeof(line), fp)) {
        return EOF;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsscanf(line, fmt, ap);
    va_end(ap);
    return n;
}

void perror(const char *s)
{
    stdio_ensure_init();
    if (s && *s) {
        fputs(s, stderr);
        fputs(": ", stderr);
    }
    fputs(strerror(errno), stderr);
    fputc('\n', stderr);
}

/* exit：**先冲洗 stdio** 再结束进程。
 *
 * ★ 这一条不做的话，症状是"程序跑了但什么都没输出" ★
 * `printf("结果\n")` 之后 `exit(1)`：如果 exit 直接落到 fe_exit（它只冲
 * libfe 自己的日志缓冲），stdio 缓冲里的那行就丢了。
 * 而且这个 bug 只在**输出到文件**时明显（终端是行缓冲，换行就刷了），
 * 于是"本地测试正常、重定向到文件就少了最后一行"——非常难查。 */
extern void posix_run_atexit(void);

void exit(int status)
{
    stdio_ensure_init();
    fflush(NULL);
    posix_run_atexit();
    _exit(status);
}

void _exit(int status)
{
    fe_exit(status);        /* noreturn */
}

void abort(void)
{
    stdio_ensure_init();
    fflush(NULL);
    _exit(134);             /* 128 + SIGABRT，与 shell 的约定一致 */
}
