/* SPDX-License-Identifier: 0BSD */
#include <fe/kprintf.h>
#include <fe/string.h>

/* ------------------------------------------------------------------ */
/* 格式化核心：以字符回调方式输出，避免固定大小缓冲区带来的截断问题     */
/* ------------------------------------------------------------------ */

typedef void (*emit_fn)(void *ctx, char c);

struct fmt_sink {
    emit_fn emit;
    void *ctx;
    size_t written;
};

static void sink_putc(struct fmt_sink *s, char c)
{
    s->emit(s->ctx, c);
    s->written++;
}

static void sink_puts(struct fmt_sink *s, const char *str, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        sink_putc(s, str[i]);
    }
}

static void sink_pad(struct fmt_sink *s, char c, int count)
{
    for (int i = 0; i < count; i++) {
        sink_putc(s, c);
    }
}

struct fmt_spec {
    bool left;      /* '-' 左对齐 */
    bool zero;      /* '0' 补零 */
    bool plus;      /* '+' 强制符号 */
    bool space;     /* ' ' 正数留空格 */
    bool alt;       /* '#' 进制前缀 */
    int  width;
    int  precision; /* -1 表示未指定 */
    enum { LEN_NONE, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_Z } len;
    char conv;
};

static const char *digit_table(bool uppercase)
{
    return uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
}

/* 把无符号数按规格写入 sink。prefix 仅在 alt 标志下使用。 */
static void emit_unsigned(struct fmt_sink *s, const struct fmt_spec *sp, u64 value,
                          unsigned base, bool uppercase, const char *prefix)
{
    char tmp[72];
    const char *digits = digit_table(uppercase);
    int n = 0;

    /* 精度为 0 且值为 0：输出空串（标准 printf 语义） */
    if (!(value == 0 && sp->precision == 0)) {
        do {
            tmp[n++] = digits[value % base];
            value /= base;
        } while (value);
    }

    int plen = (sp->alt && prefix) ? (int)strlen(prefix) : 0;
    int zeros = (sp->precision > n) ? sp->precision - n : 0;
    int total = n + zeros + plen;
    int pad = (sp->width > total) ? sp->width - total : 0;
    bool zero_pad = sp->zero && !sp->left && sp->precision < 0;

    if (!sp->left && !zero_pad) {
        sink_pad(s, ' ', pad);
    }
    if (plen) {
        sink_puts(s, prefix, (size_t)plen);
    }
    if (!sp->left && zero_pad) {
        sink_pad(s, '0', pad);
    }
    sink_pad(s, '0', zeros);
    while (n--) {
        sink_putc(s, tmp[n]);
    }
    if (sp->left) {
        sink_pad(s, ' ', pad);
    }
}

/* 有符号数：符号位与数字正文一起参与宽度计算 */
static void emit_signed(struct fmt_sink *s, const struct fmt_spec *sp, i64 value,
                        unsigned base, bool uppercase)
{
    bool negative = value < 0;
    /* 取绝对值时避免 INT64_MIN 溢出：先 +1 再取反 */
    u64 mag = negative ? ((u64)(-(value + 1)) + 1u) : (u64)value;

    char sign = '\0';
    if (negative) {
        sign = '-';
    } else if (sp->plus) {
        sign = '+';
    } else if (sp->space) {
        sign = ' ';
    }

    char tmp[72];
    const char *digits = digit_table(uppercase);
    int n = 0;
    if (!(mag == 0 && sp->precision == 0)) {
        do {
            tmp[n++] = digits[mag % base];
            mag /= base;
        } while (mag);
    }

    int zeros = (sp->precision > n) ? sp->precision - n : 0;
    int total = n + zeros + (sign ? 1 : 0);
    int pad = (sp->width > total) ? sp->width - total : 0;
    bool zero_pad = sp->zero && !sp->left && sp->precision < 0;

    if (!sp->left && !zero_pad) {
        sink_pad(s, ' ', pad);
    }
    if (sign) {
        sink_putc(s, sign);
    }
    if (!sp->left && zero_pad) {
        sink_pad(s, '0', pad);
    }
    sink_pad(s, '0', zeros);
    while (n--) {
        sink_putc(s, tmp[n]);
    }
    if (sp->left) {
        sink_pad(s, ' ', pad);
    }
}

static void emit_string(struct fmt_sink *s, const struct fmt_spec *sp, const char *str)
{
    if (!str) {
        str = "(null)";
    }
    size_t len = (sp->precision >= 0) ? strnlen(str, (size_t)sp->precision) : strlen(str);
    int pad = (sp->width > (int)len) ? sp->width - (int)len : 0;
    if (!sp->left) {
        sink_pad(s, ' ', pad);
    }
    sink_puts(s, str, len);
    if (sp->left) {
        sink_pad(s, ' ', pad);
    }
}

/* 注意：在 x86_64 SysV ABI 下 va_list 是数组类型，按值传参等价于传首元素指针，
 * va_arg 会直接推进调用方的取值状态，因此这里用 va_list（而非 va_list *）。 */
static u64 fetch_unsigned(va_list ap, const struct fmt_spec *sp)
{
    switch (sp->len) {
    case LEN_LL: return va_arg(ap, unsigned long long);
    case LEN_L:  return va_arg(ap, unsigned long);
    case LEN_Z:  return va_arg(ap, size_t);
    default:     return (u64)va_arg(ap, unsigned int);
    }
}

static i64 fetch_signed(va_list ap, const struct fmt_spec *sp)
{
    switch (sp->len) {
    case LEN_LL: return va_arg(ap, long long);
    case LEN_L:  return va_arg(ap, long);
    case LEN_Z:  return (i64)va_arg(ap, size_t);
    default:     return (i64)va_arg(ap, int);
    }
}

static void format_impl(struct fmt_sink *s, const char *fmt, va_list ap)
{
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            sink_putc(s, *p);
            continue;
        }
        p++;
        if (*p == '%') {
            sink_putc(s, '%');
            continue;
        }

        struct fmt_spec sp = {
            .left = false, .zero = false, .plus = false, .space = false, .alt = false,
            .width = 0, .precision = -1, .len = LEN_NONE, .conv = 0,
        };

        for (;; p++) {          /* 标志 */
            if (*p == '-')      { sp.left = true; }
            else if (*p == '0') { sp.zero = true; }
            else if (*p == '+') { sp.plus = true; }
            else if (*p == ' ') { sp.space = true; }
            else if (*p == '#') { sp.alt = true; }
            else                { break; }
        }
        if (*p == '*') {        /* 宽度 */
            int w = va_arg(ap, int);
            if (w < 0) {
                sp.left = true;
                w = -w;
            }
            sp.width = w;
            p++;
        } else {
            while (*p >= '0' && *p <= '9') {
                sp.width = sp.width * 10 + (*p - '0');
                p++;
            }
        }
        if (*p == '.') {        /* 精度 */
            p++;
            if (*p == '*') {
                sp.precision = va_arg(ap, int);
                if (sp.precision < 0) {
                    sp.precision = -1;
                }
                p++;
            } else {
                sp.precision = 0;
                while (*p >= '0' && *p <= '9') {
                    sp.precision = sp.precision * 10 + (*p - '0');
                    p++;
                }
            }
        }
        if (*p == 'h') {        /* 长度 */
            p++;
            if (*p == 'h') { sp.len = LEN_HH; p++; } else { sp.len = LEN_H; }
        } else if (*p == 'l') {
            p++;
            if (*p == 'l') { sp.len = LEN_LL; p++; } else { sp.len = LEN_L; }
        } else if (*p == 'z') {
            sp.len = LEN_Z;
            p++;
        }

        sp.conv = *p;
        if (!sp.conv) {
            break;
        }

        switch (sp.conv) {
        case 'd':
        case 'i':
            emit_signed(s, &sp, fetch_signed(ap, &sp), 10, false);
            break;
        case 'u':
        case 'x':
        case 'X':
        case 'o':
        case 'b': {
            unsigned base = (sp.conv == 'o') ? 8u
                          : (sp.conv == 'b') ? 2u
                          : (sp.conv == 'u') ? 10u : 16u;
            const char *prefix = NULL;
            if (sp.conv == 'x')      { prefix = "0x"; }
            else if (sp.conv == 'X') { prefix = "0X"; }
            else if (sp.conv == 'o') { prefix = "0"; }
            else if (sp.conv == 'b') { prefix = "0b"; }
            emit_unsigned(s, &sp, fetch_unsigned(ap, &sp), base,
                          sp.conv == 'X', prefix);
            break;
        }
        case 'p': {
            struct fmt_spec ps = sp;
            ps.alt = true;
            ps.zero = true;
            if (ps.width == 0) {
                ps.width = 18; /* 0x + 16 位十六进制，定宽便于对齐阅读 */
            }
            emit_unsigned(s, &ps, (u64)(uptr)va_arg(ap, void *), 16, false, "0x");
            break;
        }
        case 'c': {
            int c = va_arg(ap, int);
            int pad = (sp.width > 1) ? sp.width - 1 : 0;
            if (!sp.left) {
                sink_pad(s, ' ', pad);
            }
            sink_putc(s, (char)c);
            if (sp.left) {
                sink_pad(s, ' ', pad);
            }
            break;
        }
        case 's':
            emit_string(s, &sp, va_arg(ap, const char *));
            break;
        default:
            sink_putc(s, '%');
            sink_putc(s, sp.conv);
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 输出后端                                                            */
/* ------------------------------------------------------------------ */

static void console_emit(void *ctx, char c)
{
    (void)ctx;
    fe_console_putc(c);
}

void fe_kvprintf(const char *fmt, va_list ap)
{
    struct fmt_sink s = { .emit = console_emit, .ctx = NULL, .written = 0 };
    format_impl(&s, fmt, ap);
}

void fe_kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fe_kvprintf(fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* snprintf 家族（内核内部组装字符串用）                                */
/* ------------------------------------------------------------------ */

struct sn_sink {
    char *buf;
    size_t size;
    size_t pos;
};

static void sn_emit(void *ctx, char c)
{
    struct sn_sink *s = (struct sn_sink *)ctx;
    if (s->pos + 1 < s->size) {
        s->buf[s->pos] = c;
    }
    s->pos++;
}

int fe_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    if (size == 0) {
        return 0;
    }
    struct sn_sink sn = { .buf = buf, .size = size, .pos = 0 };
    struct fmt_sink s = { .emit = sn_emit, .ctx = &sn, .written = 0 };
    format_impl(&s, fmt, ap);
    size_t end = (sn.pos < size - 1) ? sn.pos : size - 1;
    buf[end] = '\0';
    return (int)sn.pos;
}

int fe_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = fe_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
