/* SPDX-License-Identifier: 0BSD */
/* libposix：格式化与流（见 docs/10-posix-layer.md）。
 *
 * ★ 为什么这里**重写**了一个格式化器，而不是复用 libfe 的 fe_vsnprintf ★
 *
 * libfe 里那份是"够内核日志用"的最小实现：只认 %d/%i/%u/%x/%X/%p/%c/%s/%%，
 * 没有精度、没有 %o、没有浮点。POSIX 程序用的是**完整**的那一套：
 * `%.3s`、`%08.2f`、`%*d`、`%o`、`%e`…少一个就是"输出少了一截"。
 *
 * 而两份**共用同名函数**的实现放在同一个进程里更糟：链接顺序决定用哪个，
 * 于是"某个程序的 printf 少了精度支持"会取决于一行构建脚本的顺序。
 * 所以：libposix 提供完整版的 vsnprintf/snprintf（POSIX 语义），
 * libfe 保留它自己那个（fe_snprintf，内核日志风格），两者同名不同前缀，
 * 谁都不会悄悄替掉谁。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>

/* stdio.h 里的 fwrite 在 out_flush_tmp 里被用到，但它实现在 stdio.c；
 * 这里显式声明一次，免得依赖"两个文件里原型碰巧一致"。 */
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp);

/* ------------------------------------------------------------------ */
/* 输出目标：既能写进缓冲，也能写进流                                  */
/* ------------------------------------------------------------------ */

struct out {
    char *buf;          /* 不为 NULL：写进缓冲 */
    size_t cap;
    size_t len;         /* 本该写入的长度（不含结尾 '\0'） */
    FILE *fp;           /* 不为 NULL：写进流（缓冲模式下先攒再吐） */
    char tmp[256];
    size_t tmp_len;
};

static void out_flush_tmp(struct out *o)
{
    if (o->fp && o->tmp_len) {
        /* 直接调 fwrite：它自己会走流的缓冲。**不要**在这里绕回 fprintf，
         * 那会把格式化重入一遍。 */
        size_t n = fwrite(o->tmp, 1, o->tmp_len, o->fp);
        (void)n;
        o->tmp_len = 0;
    }
}

static void out_putc(struct out *o, char c)
{
    if (o->fp) {
        o->tmp[o->tmp_len++] = c;
        if (o->tmp_len == sizeof(o->tmp)) {
            out_flush_tmp(o);
        }
    } else if (o->buf && o->len + 1 < o->cap) {
        o->buf[o->len] = c;
    }
    o->len++;               /* 截断时仍然计数：调用者靠它检测截断 */
}

static void out_str(struct out *o, const char *s, int maxlen)
{
    if (!s) {
        s = "(null)";
    }
    int n = 0;
    while (*s && (maxlen < 0 || n < maxlen)) {
        out_putc(o, *s++);
        n++;
    }
}

static void out_pad(struct out *o, char c, int n)
{
    for (int i = 0; i < n; i++) {
        out_putc(o, c);
    }
}

/* ------------------------------------------------------------------ */
/* 整数与浮点                                                          */
/* ------------------------------------------------------------------ */

/* 把无符号数转成数字串（低位在前），返回长度 */
static int u_to_digits(u64 v, unsigned base, int upper, char *out)
{
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    if (v == 0) {
        out[n++] = '0';
        return n;
    }
    while (v) {
        out[n++] = dig[v % base];
        v /= base;
    }
    return n;
}

/* 浮点 → 十进制。没有 libc 的 sprintf 可用，所以自己做。
 *
 * ★ 只在**不带指数**的常规范围内保证正确 ★
 * 用整数部分与小数部分分别取整的方式（而不是逐位乘 10 逼近），
 * 因为后者在二进制浮点下会累积误差（"0.1 打印成 0.0999999"就是这么来的）。
 * 超出 u64 能表示的范围时**退回科学计数法**（fmt_exp）——
 * 第一版写成"递归调自己"，那对 1e19 以上的值会无限递归。
 * 这类 bug 的可怕之处是它只在极端输入下出现，而 %f 平时都在小数上。 */
static void fmt_double(struct out *o, double v, int precision, int width,
                       int left, int zero, int plus, int space, int upper);
static void fmt_exp(struct out *o, double v, int precision, int upper);

static void fmt_double(struct out *o, double v, int precision, int width,
                       int left, int zero, int plus, int space, int upper)
{
    char num[64];
    int n = 0;
    int neg = 0;

    if (v != v) {                       /* NaN：自己判，不用 <math.h> */
        out_str(o, upper ? "NAN" : "nan", -1);
        return;
    }
    /* 无穷大：v-v != 0 对有限数成立 */
    if (v - v != 0.0) {
        const char *s = (v > 0) ? (upper ? "INF" : "inf") : (upper ? "-INF" : "-inf");
        out_str(o, s, -1);
        return;
    }

    if (precision < 0) {
        precision = 6;
    }
    if (precision > 17) {
        precision = 17;                 /* 再多的位数是二进制噪声 */
    }
    if (v < 0.0) {
        neg = 1;
        v = -v;
    }

    /* 超出整数法能处理的范围：交给科学计数法（有限工作、结果正确） */
    if (v >= 1.8e19) {
        fmt_exp(o, neg ? -v : v, precision > 0 ? precision : 0, upper);
        return;
    }

    /* 四舍五入到 precision 位：整数放大法，避免"0.1 → 0.0999999" */
    double scale = 1.0;
    for (int i = 0; i < precision; i++) {
        scale *= 10.0;
    }
    double scaled = v * scale;
    u64 ip = (u64)scaled;
    double frac = scaled - (double)ip;
    if (frac >= 0.5) {
        ip++;
    }

    u64 int_part = ip;
    u64 frac_part = 0;
    if (precision > 0) {
        u64 div = 1;
        for (int i = 0; i < precision; i++) {
            div *= 10;
        }
        int_part = ip / div;
        frac_part = ip % div;
    }

    char tmp[32];
    int t = u_to_digits(int_part, 10, 0, tmp);
    for (int i = t - 1; i >= 0; i--) {
        num[n++] = tmp[i];
    }
    if (precision > 0) {
        num[n++] = '.';
        char ftmp[32];
        int f = u_to_digits(frac_part, 10, 0, ftmp);
        for (int i = 0; i < precision - f; i++) {
            num[n++] = '0';             /* 前导零：0.05 而不是 0.5 */
        }
        for (int i = f - 1; i >= 0; i--) {
            num[n++] = ftmp[i];
        }
    }

    const char *sign = neg ? "-" : (plus ? "+" : (space ? " " : ""));
    int total = (int)strlen(sign) + n;
    int pad = width - total;
    if (!left && !zero) {
        out_pad(o, ' ', pad);
    }
    if (*sign) {
        out_putc(o, *sign);
    }
    if (!left && zero) {
        out_pad(o, '0', pad);
    }
    for (int i = 0; i < n; i++) {
        out_putc(o, num[i]);
    }
    if (left) {
        out_pad(o, ' ', pad);
    }
}

/* 科学计数法：d.dddde±XX。同样用整数法取有效数字，不做逐位逼近。 */
static void fmt_exp(struct out *o, double v, int precision, int upper)
{
    int neg = 0;
    if (v < 0.0) {
        neg = 1;
        v = -v;
    }
    int exp10 = 0;
    if (v != 0.0) {
        while (v >= 10.0) {
            v /= 10.0;
            exp10++;
        }
        while (v < 1.0) {
            v *= 10.0;
            exp10--;
        }
    }
    /* 复用 %f 的路径打印尾数（此时 1 <= v < 10），再补上指数 */
    char mant[48];
    struct out mo;
    mo.buf = mant;
    mo.cap = sizeof(mant);
    mo.len = 0;
    mo.fp = NULL;
    mo.tmp_len = 0;
    fmt_double(&mo, v, precision, 0, 0, 0, 0, 0, 0);
    size_t mlen = (mo.len < sizeof(mant)) ? mo.len : sizeof(mant) - 1;
    mant[mlen] = '\0';

    if (neg) {
        out_putc(o, '-');
    }
    for (size_t i = 0; i < mlen; i++) {
        out_putc(o, mant[i]);
    }
    out_putc(o, upper ? 'E' : 'e');
    out_putc(o, exp10 < 0 ? '-' : '+');
    int e = exp10 < 0 ? -exp10 : exp10;
    char eb[8];
    int en = u_to_digits((u64)e, 10, 0, eb);
    if (en < 2) {
        out_putc(o, '0');               /* 指数至少两位：e+05 而不是 e+5 */
    }
    for (int i = en - 1; i >= 0; i--) {
        out_putc(o, eb[i]);
    }
}

/* ------------------------------------------------------------------ */
/* vsnprintf 本体                                                      */
/* ------------------------------------------------------------------ */

static int fmt_core(struct out *o, const char *fmt, va_list ap)
{
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            out_putc(o, *p);
            continue;
        }
        p++;
        if (*p == '%') {
            out_putc(o, '%');
            continue;
        }

        int left = 0, plus = 0, space = 0, alt = 0, zero = 0;
        for (;; p++) {
            if (*p == '-')      { left = 1; }
            else if (*p == '+') { plus = 1; }
            else if (*p == ' ') { space = 1; }
            else if (*p == '#') { alt = 1; }
            else if (*p == '0') { zero = 1; }
            else break;
        }
        /* 宽度：数字或 *（从参数取） */
        int width = 0;
        if (*p == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = 1;
                width = -width;
            }
            p++;
        } else {
            while (*p >= '0' && *p <= '9') {
                width = width * 10 + (*p - '0');
                p++;
            }
        }
        /* 精度：.数字 或 .* 或 .（等价 .0） */
        int prec = -1;
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') {
                prec = va_arg(ap, int);
                if (prec < 0) {
                    prec = -1;          /* 负精度 = 没给精度（C 标准如此） */
                }
                p++;
            } else {
                while (*p >= '0' && *p <= '9') {
                    prec = prec * 10 + (*p - '0');
                    p++;
                }
            }
        }
        /* 长度修饰符 */
        int is64 = 0, is_short = 0;
        for (;; p++) {
            if (*p == 'l') {
                is64 = 1;               /* ll 与 l 在 LP64 上同义 */
            } else if (*p == 'z' || *p == 'j' || *p == 't') {
                is64 = 1;
            } else if (*p == 'h') {
                is_short = 1;
            } else {
                break;
            }
        }

        char dbuf[64];
        int dn;
        char prefix[3];
        int plen = 0;
        const char *text = NULL;
        int text_from_arg = 0;

        switch (*p) {
        case 'd':
        case 'i': {
            i64 v = is64 ? va_arg(ap, i64)
                         : (is_short ? (i64)(short)va_arg(ap, int) : (i64)va_arg(ap, int));
            u64 mag = (v < 0) ? (u64)(-(v + 1)) + 1u : (u64)v;   /* 防 INT64_MIN */
            if (v < 0) {
                prefix[plen++] = '-';
            } else if (plus) {
                prefix[plen++] = '+';
            } else if (space) {
                prefix[plen++] = ' ';
            }
            dn = u_to_digits(mag, 10, 0, dbuf);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            u64 v = is64 ? va_arg(ap, u64)
                         : (is_short ? (u64)(unsigned short)va_arg(ap, unsigned int)
                                     : (u64)va_arg(ap, unsigned int));
            unsigned base = (*p == 'o') ? 8u : ((*p == 'u') ? 10u : 16u);
            /* ★ %#o 的前导 0 必须在算出位数之后处理 ★
             * 第一版把它写成了一个"先用 dn 再赋值"的表达式，dn 那会儿还没算——
             * 于是 %#o 的前导零逻辑是未定义行为。这类错误编译器不一定报
             * （自赋值表达式在语法上合法），而输出会随优化等级变化。 */
            dn = u_to_digits(v, base, (*p == 'X'), dbuf);
            if (alt && *p == 'o' && dbuf[dn - 1] != '0') {
                dbuf[dn++] = '0';       /* 数字串是低位在前，所以"补一个 0"就是加最高位 */
            }
            if (alt && v != 0 && (*p == 'x' || *p == 'X')) {
                prefix[plen++] = '0';
                prefix[plen++] = (*p == 'X') ? 'X' : 'x';
            }
            break;
        }
        case 'p': {
            u64 v = (u64)(size_t)va_arg(ap, void *);
            if (!v) {
                text = "(nil)";
                text_from_arg = 1;
                dn = 0;
                break;
            }
            prefix[plen++] = '0';
            prefix[plen++] = 'x';
            dn = u_to_digits(v, 16, 0, dbuf);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            dbuf[0] = c;
            dn = 1;
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) {
                s = "(null)";
            }
            int slen = 0;
            while (s[slen] && (prec < 0 || slen < prec)) {
                slen++;
            }
            int pad = width - slen;
            if (!left) {
                out_pad(o, ' ', pad);
            }
            for (int i = 0; i < slen; i++) {
                out_putc(o, s[i]);
            }
            if (left) {
                out_pad(o, ' ', pad);
            }
            continue;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G': {
            double v = va_arg(ap, double);
            if (*p == 'e' || *p == 'E') {
                fmt_exp(o, v, prec < 0 ? 6 : prec, *p == 'E');
                continue;
            }
            if (*p == 'g' || *p == 'G') {
                /* %g 是"自动选短的那个"。这里按 C 的规则做一个简化版：
                 * 指数小于 -4 或大于等于精度时用科学计数法，否则用 %f。 */
                int prec_g = (prec < 0) ? 6 : (prec ? prec : 1);
                double av = (v < 0) ? -v : v;
                int use_exp = 0;
                if (av != 0.0) {
                    double t = av;
                    int e10 = 0;
                    while (t >= 10.0) { t /= 10.0; e10++; }
                    while (t < 1.0)   { t *= 10.0; e10--; }
                    use_exp = (e10 < -4 || e10 >= prec_g);
                }
                if (use_exp) {
                    fmt_exp(o, v, prec_g - 1, *p == 'G');
                } else {
                    fmt_double(o, v, prec_g - 1, width, left, zero, plus, space,
                               *p == 'G');
                }
                continue;
            }
            fmt_double(o, v, prec, width, left, zero, plus, space, *p == 'F');
            continue;
        }
        case 'n': {
            if (is64) {
                *(va_arg(ap, long *)) = (long)o->len;
            } else {
                *(va_arg(ap, int *)) = (int)o->len;
            }
            continue;
        }
        default:
            /* 不认识的转换：原样输出，**不要静默吞掉**。
             * 吞掉的后果是"某段输出莫名其妙不见了"，而根因在格式串里。 */
            out_putc(o, '%');
            if (*p) {
                out_putc(o, *p);
            }
            continue;
        }

        /* 数值类：按精度补零、按符号+宽度补齐 */
        int zeros = (prec > dn) ? (prec - dn) : 0;
        /* ★ 精度为 0 且值为 0 时**什么都不输出**（C 标准）★
         * `printf("%.0d", 0)` 的结果是空串，不是 "0"。这条看着古怪，
         * 但程序真的依赖它（"%5.0d" 用来输出 5 个空格）。 */
        if (prec == 0 && dn == 1 && dbuf[0] == '0' && plen == 0) {
            dn = 0;
        }
        int total = plen + zeros + dn;
        if (text_from_arg && prec >= 0) {
            int slen = 0;
            while (text[slen] && slen < prec) {
                slen++;
            }
            total = slen;
        }
        int pad = width - total;
        if (!left && !zero) {
            out_pad(o, ' ', pad);
        }
        for (int i = 0; i < plen; i++) {
            out_putc(o, prefix[i]);
        }
        /* 给了精度就用精度补零，零填充标志对它无效（C 标准） */
        if (!left && zero && prec < 0) {
            out_pad(o, '0', pad);
        }
        out_pad(o, '0', zeros);
        if (text_from_arg) {
            int slen = 0;
            while (text[slen] && (prec < 0 || slen < prec)) {
                slen++;
            }
            for (int i = 0; i < slen; i++) {
                out_putc(o, text[i]);
            }
        } else {
            for (int i = dn - 1; i >= 0; i--) {
                out_putc(o, dbuf[i]);
            }
        }
        if (left) {
            out_pad(o, ' ', pad);
        }
    }
    return 0;
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    struct out o;
    o.buf = buf;
    o.cap = cap;
    o.len = 0;
    o.fp = NULL;
    o.tmp_len = 0;
    fmt_core(&o, fmt, ap);
    if (buf && cap > 0) {
        size_t end = (o.len < cap) ? o.len : cap - 1;
        buf[end] = '\0';
    }
    return (int)o.len;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, (size_t)-1, fmt, ap);    /* 无界：调用者负责够大 */
    va_end(ap);
    return n;
}

int vfprintf(FILE *fp, const char *fmt, va_list ap)
{
    struct out o;
    o.buf = NULL;
    o.cap = 0;
    o.len = 0;
    o.fp = fp;
    o.tmp_len = 0;
    fmt_core(&o, fmt, ap);
    out_flush_tmp(&o);
    return (int)o.len;
}

int fprintf(FILE *fp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(fp, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

int vprintf(const char *fmt, va_list ap)
{
    return vfprintf(stdout, fmt, ap);
}
