/* SPDX-License-Identifier: 0BSD */
/* consoled —— 帧缓冲控制台服务。
 *
 * ★ 它是帧缓冲 MMIO 的**唯一持有者** ★
 * 内核在引导期画过一条蓝带（那是在把帧缓冲放进资源池之前），
 * 移交之后内核不再往屏幕上画任何东西。两边都画就会互相盖掉，
 * 而且用户态看到的是"自己以为的屏幕内容"，与实际像素不一致——
 * 那类不一致查起来非常贵（你不知道是你画错了还是别人盖掉了）。
 *
 * 分层：
 *   kbd 服务（驱动）──按键事件──▶ consoled（策略 + 渲染）──▶ 帧缓冲 MMIO
 *                                        ▲
 *                                        │ 写字符 / 读一行
 *                                      /dev/console 的客户端（shell 等）
 *
 * 它只做三件事：**渲染字符、维护光标与滚动、把输入攒成行**。
 * 它不做命令解析（那是 shell 的事），也不碰键盘硬件（那是 kbd 的事）。
 *
 * 自检方式：写一个字符后**把像素读回来**和字体位图逐位比对。
 * 帧缓冲是可读的 MMIO，所以"我以为我画上去了"可以被证伪——
 * 这比截图更硬（截图要靠人看，而它能在日志里自动判定）。 */
#include <fe_user.h>
#include <fe_kbd.h>
#include "font5x7.h"

/* ---------------- 进程内的锁 ----------------
 *
 * ★ 为什么控制台服务需要一把锁 ★
 * 它现在有**两个线程**：
 *   - 输入线程：等键盘事件 → 行编辑 → 回显到屏幕；
 *   - 服务线程：收 /dev/console 请求 → 把客户端要打印的文字画到屏幕。
 * 屏幕、光标、行缓冲都是两者共享的。没有锁就会出现：
 * 客户端的输出插进用户正在敲的那一行中间、光标残影、行缓冲被撕开。
 * 这类问题在单核上也会发生（抢占点就在两条线程之间），不是多核才有的。
 *
 * 用一个 test-and-set 自旋锁 + 让出 CPU 的退避就够：临界区是"画几十个像素"
 * 或"改几个字节"，纳秒级。**不引入更复杂的机制**（没有等待队列、没有优先级继承），
 * 因为没有需求：这里只有两条线程，且都会很快让出。 */
static volatile u32 g_lock;

static void con_lock(void)
{
    while (__atomic_exchange_n(&g_lock, 1u, __ATOMIC_ACQUIRE)) {
        fe_yield();
    }
}

static void con_unlock(void)
{
    __atomic_store_n(&g_lock, 0u, __ATOMIC_RELEASE);
}

/* ---------------- 帧缓冲 ---------------- */

static struct fe_fb_info g_fb;
static u8 *g_fbmem;                 /* 映射进来的帧缓冲 */
static u32 g_cols, g_rows;          /* 文本网格 */
static u32 g_scale = 2;             /* 整数缩放：5x7 字形在 1280x800 上太小 */
static u32 g_cx, g_cy;              /* 光标（字符格坐标） */
static u32 g_fg = 0xC0C0C0;         /* 前景（0xRRGGBB，会按掩码落到像素里） */
static u32 g_bg = 0x101014;

static u32 cell_w(void) { return (FE_FONT_W + 1) * g_scale; }
static u32 cell_h(void) { return (FE_FONT_H + 2) * g_scale; }

/* 把一个 0xRRGGBB 按帧缓冲的掩码/偏移塞进像素。
 * ★ 不能假设 32bpp 就是 0x00RRGGBB ★
 * 16 位 5-6-5、32 位 BGR 顺序都真实存在，而这些差异只能由引导器给的
 * mask/shift 表达。自己按 bpp 猜，换台机器就是满屏蓝绿颠倒。 */
static u32 pack_color(u32 rgb)
{
    u32 r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    u32 out = 0;
    if (g_fb.red_size) {
        out |= ((r >> (8 - g_fb.red_size)) & ((1u << g_fb.red_size) - 1))
               << g_fb.red_shift;
    }
    if (g_fb.green_size) {
        out |= ((g >> (8 - g_fb.green_size)) & ((1u << g_fb.green_size) - 1))
               << g_fb.green_shift;
    }
    if (g_fb.blue_size) {
        out |= ((b >> (8 - g_fb.blue_size)) & ((1u << g_fb.blue_size) - 1))
               << g_fb.blue_shift;
    }
    return out;
}

static void put_pixel(u32 x, u32 y, u32 color)
{
    u32 bpp = g_fb.bpp;
    if (x >= g_fb.width || y >= g_fb.height) {
        return;
    }
    u8 *p = g_fbmem + (u64)y * g_fb.pitch + (u64)x * (bpp / 8);
    switch (bpp) {
    case 32: *(volatile u32 *)p = color; break;
    case 24: p[0] = (u8)(color & 0xFF); p[1] = (u8)((color >> 8) & 0xFF);
             p[2] = (u8)((color >> 16) & 0xFF); break;
    case 16: *(volatile u16 *)p = (u16)color; break;
    default: break;
    }
}

static u32 get_pixel(u32 x, u32 y)
{
    u32 bpp = g_fb.bpp;
    const u8 *p = g_fbmem + (u64)y * g_fb.pitch + (u64)x * (bpp / 8);
    switch (bpp) {
    case 32: return *(volatile u32 *)p;
    case 24: return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16);
    case 16: return *(volatile u16 *)p;
    default: return 0;
    }
}

/* 画一个字符。返回其占用的像素矩形（供自检比对）。 */
static void draw_glyph(u32 col, u32 row, char ch, u32 fg, u32 bg)
{
    u32 code = (u8)ch;
    if (code < FE_FONT_FIRST || code > FE_FONT_LAST) {
        code = '?';
    }
    const u8 *glyph = &g_font5x7[(code - FE_FONT_FIRST) * FE_FONT_STRIDE];
    u32 x0 = col * cell_w(), y0 = row * cell_h();
    u32 cw = cell_w(), chh = cell_h();

    for (u32 y = 0; y < chh; y++) {
        for (u32 x = 0; x < cw; x++) {
            /* 字形占 (W*scale) x (H*scale)，右边留 1 格、下面留 2 格间距 */
            u32 gx = x / g_scale, gy = y / g_scale;
            u32 color = bg;
            if (gx < FE_FONT_W && gy < FE_FONT_H) {
                if (glyph[gy] & (1u << gx)) {
                    color = fg;
                }
            }
            put_pixel(x0 + x, y0 + y, color);
        }
    }
}

/* ---------------- 网格与滚动 ---------------- */

static void fill_screen(u32 color)
{
    for (u32 y = 0; y < g_fb.height; y++) {
        for (u32 x = 0; x < g_fb.width; x++) {
            put_pixel(x, y, color);
        }
    }
}

static void scroll_up(void)
{
    u32 line = cell_h();
    u64 keep = (u64)(g_fb.height - line) * g_fb.pitch;
    u8 *dst = g_fbmem;
    u8 *src = g_fbmem + (u64)line * g_fb.pitch;
    memcpy(dst, src, (usize)keep);
    /* 最后一行清成背景 */
    for (u32 y = g_fb.height - line; y < g_fb.height; y++) {
        for (u32 x = 0; x < g_fb.width; x++) {
            put_pixel(x, y, pack_color(g_bg));
        }
    }
}

static void newline(void)
{
    g_cx = 0;
    if (++g_cy >= g_rows) {
        g_cy = g_rows - 1;
        scroll_up();
    }
}

/* 把光标位置用一个实心块画出来。
 * 光标是 consoled 自己的状态：**重画字符前必须先擦掉旧光标**，
 * 否则屏幕上会留下一串光标残影。 */
static u32 g_cursor_drawn;

static void cursor_erase(void)
{
    if (!g_cursor_drawn) {
        return;
    }
    draw_glyph(g_cx, g_cy, ' ', pack_color(g_bg), pack_color(g_bg));
    g_cursor_drawn = 0;
}

static void cursor_draw(void)
{
    /* 用一个下划线样式的块做光标：不破坏字符本身的可读性 */
    u32 x0 = g_cx * cell_w(), y0 = g_cy * cell_h() + FE_FONT_H * g_scale;
    u32 color = pack_color(g_fg);
    for (u32 x = 0; x < FE_FONT_W * g_scale; x++) {
        for (u32 y = 0; y < g_scale; y++) {
            put_pixel(x0 + x, y0 + y, color);
        }
    }
    g_cursor_drawn = 1;
}

static void console_putc(char c)
{
    cursor_erase();
    if (c == '\n') {
        newline();
    } else if (c == '\r') {
        g_cx = 0;
    } else if (c == '\b') {
        /* ★ 退格要能跨行回卷 ★
         * 原来只做 "g_cx--"，在行首就停住了——于是"擦掉上一行末尾"
         * 这件事做不到，跨行写进去的内容也擦不干净。
         * 终端的退格本来就该回卷，这是它的语义，不是特例。
         * 行编辑（输入线程）里 g_cx 永远 > 0（前面有提示符），
         * 所以这条改动不会影响它。 */
        if (g_cx > 0) {
            g_cx--;
        } else if (g_cy > 0) {
            g_cy--;
            g_cx = g_cols ? g_cols - 1 : 0;
        }
        draw_glyph(g_cx, g_cy, ' ', pack_color(g_bg), pack_color(g_bg));
    } else if ((u8)c >= 32) {
        draw_glyph(g_cx, g_cy, c, pack_color(g_fg), pack_color(g_bg));
        if (++g_cx >= g_cols) {
            newline();
        }
    }
    cursor_draw();
}

static void console_write(const char *s)
{
    for (; *s; s++) {
        console_putc(*s);
    }
}

static void console_write_n(const char *s, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        console_putc(s[i]);
    }
}

/* ---------------- 服务 ---------------- */

#define CON_OP_INFO  1
#define CON_OP_WRITE 2
#define CON_OP_READ  3

struct con_req {
    u32 op;
    u32 len;
};

struct con_info {
    u32 width, height;      /* 像素 */
    u32 cols, rows;         /* 字符格 */
    u32 bpp;
    u32 scale;
};

/* ---- 输入行缓冲（规范模式：客户端拿到的永远是完整的一行）----
 *
 * ★ 行编辑放在这里而不是 shell 里 ★
 * 谁拥有屏幕，谁负责回显。如果让 shell 去回显按键，它会和本服务的
 * 光标/滚动逻辑打架（两边都在写同一块屏幕，而且互不知道对方写了什么）。
 * 所以本服务做 tty 的"规范模式"：攒够一行才交给客户端。
 *
 * 现在还没有接键盘（S1.2 做），所以行缓冲只会被"注入"填充——
 * 但读接口的语义**先按最终形态实现**：
 * 没有行就**把回复挂起**，等有行再回复。
 * 用一个"返回空行"的假语义会让调用者（shell）的循环写法完全不同，
 * 到 S1.2 还得改一遍——那才是真正的返工。 */
#define LINE_MAX 256
static char g_line[LINE_MAX];
static u32  g_line_len;
static long g_reader_reply;         /* 挂起的读请求（回复端点句柄） */
static u32  g_reader_cap;           /* 调用者缓冲区大小 */

static int line_ready(void) { return g_line_len > 0; }

/* 有一行就交给挂起的读者。没有读者就留在缓冲里（不丢）。
 *
 * ★ 调用者必须持有 con_lock ★
 * 因为"检查有没有读者"与"看有没有行"必须是一个原子判断：
 * 否则会出现经典的丢唤醒——服务线程判断"没有行"准备挂起，
 * 而输入线程正好在这一刻把行放好并检查"没有读者"，两边都以为对方会处理。 */
static void deliver_line_locked(void)
{
    if (g_reader_reply <= 0 || !line_ready()) {
        return;
    }
    u32 n = g_line_len;
    if (n >= g_reader_cap) {
        n = g_reader_cap ? g_reader_cap - 1 : 0;
    }
    struct fe_msg_header rh;
    memset(&rh, 0, sizeof(rh));
    rh.payload_len = n;
    fe_endpoint_send(g_reader_reply, &rh, g_line, NULL, 0);
    fe_handle_close(g_reader_reply);
    g_reader_reply = 0;
    g_line_len = 0;
}

/* ---- 键盘输入线程 ----
 *
 * 它做三件事：收按键事件、做行编辑（退格/回车/回显）、把整行交给读者。
 * ★ 行编辑放在这里而不是 shell 里 ★ 谁拥有屏幕，谁负责回显。
 * 见上面 line buffer 的说明。 */
static void input_thread(void *arg)
{
    long in_ep = (long)arg;
    for (;;) {
        struct fe_msg_header hdr;
        struct fe_kbd_event ev;
        long r = fe_endpoint_recv(in_ep, &hdr, &ev, sizeof(ev), NULL, NULL);
        if (r < 0) {
            return;
        }
        if (hdr.payload_len < sizeof(ev) || !ev.pressed) {
            continue;       /* 松开事件不产生字符（这一版不做按键重复） */
        }
        char c = (char)ev.ascii;
        if (c == 0) {
            continue;       /* 功能键：没有可打印字符 */
        }

        con_lock();
        if (c == '\r' || c == '\n') {
            console_putc('\n');
            /* 把这一行交给等待的读者；没有人等就先存着 */
            deliver_line_locked();
        } else if (c == '\b' || c == 0x7F) {
            if (g_line_len > 0) {
                g_line_len--;
                console_putc('\b');     /* 擦掉屏幕上的字符 */
            }
        } else if ((u8)c >= 32 && g_line_len < LINE_MAX - 1) {
            g_line[g_line_len++] = c;
            console_putc(c);            /* 回显 */
        }
        con_unlock();
    }
}

static void say(const char *s) { fe_puts(s); }
static void num(u64 v) { fe_print_u64(v); }

static u32 g_fail;

static void check(int cond, const char *what)
{
    say(cond ? "  [consoled] OK   " : "  [consoled] 失败 ");
    say(what);
    say("\n");
    if (!cond) {
        g_fail++;
    }
}

/* 回复一次请求。
 *
 * ★ 必须**新建**一个头，不能复用请求的头 ★
 * 内核的 endpoint_send 是按 `hdr.payload_len` 从用户指针拷贝载荷的，
 * 也就是说**回复长度完全由服务端写的那个数字决定**。
 * 我第一版直接 `fe_endpoint_send(reply, &hdr, &info)` 复用了收到的头——
 * 那个头的 payload_len 是**请求**的长度（8 字节），于是 24 字节的
 * con_info 只回了 8 字节，客户端判定"应答不完整"。
 *
 * 症状很有误导性：客户端报告"INFO 请求有应答"失败，看起来像 IPC 不通，
 * 实际是服务端少写了一个字段。
 * 抽成这个函数之后，"忘记设长度"在结构上不可能再发生。 */
static void con_reply(long reply_ep, const void *data, u32 len)
{
    struct fe_msg_header rh;
    memset(&rh, 0, sizeof(rh));
    rh.payload_len = len;
    fe_endpoint_send(reply_ep, &rh, data, NULL, 0);
}

/* 帧缓冲里"我刚画的这个字符"的像素必须与字形位图一致。
 * ★ 这是本服务唯一有分量的自检 ★ 它把"我以为画上去了"变成可证伪的：
 * MMIO 写丢、映射错、掩码算错、pitch 用错，全都会在这里露出来。 */
static int verify_glyph(u32 col, u32 row, char ch, u32 fg, u32 bg)
{
    u32 code = (u8)ch;
    const u8 *glyph = &g_font5x7[(code - FE_FONT_FIRST) * FE_FONT_STRIDE];
    u32 x0 = col * cell_w(), y0 = row * cell_h();
    u32 bad = 0;
    for (u32 gy = 0; gy < FE_FONT_H; gy++) {
        for (u32 gx = 0; gx < FE_FONT_W; gx++) {
            u32 want = (glyph[gy] & (1u << gx)) ? fg : bg;
            /* 只看每个字形像素的左上角那一个像素：缩放是复制，不必全查 */
            u32 got = get_pixel(x0 + gx * g_scale, y0 + gy * g_scale);
            if (got != want) {
                bad++;
            }
        }
    }
    return bad == 0;
}

int main(void)
{
    say("\n=== 帧缓冲控制台服务（consoled）===\n");

    /* 1. 问内核要帧缓冲的几何与物理地址 */
    if (fe_fb_get_info(&g_fb) != FE_OK) {
        say("  [consoled] **取不到帧缓冲信息**（这台机器没有帧缓冲？）\n");
        return 1;
    }
    say("         帧缓冲 ");
    num(g_fb.width);
    say("x");
    num(g_fb.height);
    say(" ");
    num(g_fb.bpp);
    say("bpp，行距 ");
    num(g_fb.pitch);
    say("，物理地址 ");
    fe_print_hex(g_fb.phys);
    say("，");
    num(g_fb.size_bytes / 1024);
    say(" KiB\n");
    say("         像素格式：R");
    num(g_fb.red_size);
    say("@");
    num(g_fb.red_shift);
    say(" G");
    num(g_fb.green_size);
    say("@");
    num(g_fb.green_shift);
    say(" B");
    num(g_fb.blue_size);
    say("@");
    num(g_fb.blue_shift);
    say("\n");

    /* 2. 认领并映射。★ 这是"认领"不是"申请" ★
     * 池子里没有这段 MMIO 就拿不到——内核把帧缓冲放进池子，
     * 就是把它的所有权交出来了。 */
    g_fbmem = fe_mmio_map(g_fb.phys, g_fb.size_bytes, FE_PROT_READ | FE_PROT_WRITE);
    if (!g_fbmem) {
        say("  [consoled] **帧缓冲 MMIO 映射失败**（资源池里没有？已被别人认领？）\n");
        return 1;
    }
    check(1, "帧缓冲 MMIO 已认领并映射");

    /* 3. 算文本网格。缩放取"能让至少 80 列"的最大整数，
     * 这样换分辨率时网格自动适配，而不是写死 2 倍。 */
    while (g_scale > 1 &&
           (g_fb.width / ((FE_FONT_W + 1) * g_scale)) < 80) {
        g_scale--;
    }
    g_cols = g_fb.width / cell_w();
    g_rows = g_fb.height / cell_h();
    say("         文本网格 ");
    num(g_cols);
    say(" 列 x ");
    num(g_rows);
    say(" 行（缩放 ");
    num(g_scale);
    say("x，字符格 ");
    num(cell_w());
    say("x");
    num(cell_h());
    say(" 像素）\n");
    check(g_cols >= 80 && g_rows >= 20, "文本网格至少 80x20");

    /* 4. 接管屏幕：整屏重画（覆盖内核引导期画的那条蓝带） */
    fill_screen(pack_color(g_bg));
    g_cx = 0;
    g_cy = 0;
    cursor_draw();

    /* 5. 自检：写字符 → 读像素 → 与位图比对 */
    {
        const char *probe = "Aa0#";
        u32 all_ok = 1;
        cursor_erase();
        for (u32 i = 0; probe[i]; i++) {
            g_cx = i;
            g_cy = 0;
            draw_glyph(g_cx, g_cy, probe[i], pack_color(g_fg), pack_color(g_bg));
            if (!verify_glyph(g_cx, g_cy, probe[i],
                              pack_color(g_fg), pack_color(g_bg))) {
                all_ok = 0;
                say("         字符 '");
                fe_write(&probe[i], 1);
                say("' 的像素与字形位图**不一致**\n");
            }
        }
        check(all_ok, "写入的像素与字体位图逐点一致（帧缓冲真的收到了）");
        /* 反向对照：故意按"错的前景/背景"去比，必须**比不中**。
         * 没有这一条，"一致"可能只是因为我的比对函数永远返回真。 */
        if (verify_glyph(0, 0, 'A', pack_color(0x00FF00), pack_color(g_bg))) {
            check(0, "反向对照失败：用错的颜色比对竟然也通过（比对函数有问题）");
        } else {
            check(1, "反向对照：用错的颜色比对不通过（比对函数真的在比）");
        }
        g_cx = 0;
        g_cy = 0;
    }

    /* 6. 打出接管信息。
     * ★ 屏幕上的字符串必须是 ASCII ★
     * 字体只覆盖 ASCII 32..126（95 个字形，见 tools/mkfont.py），
     * 所以中文在屏幕上会变成 '?'——第一次截屏就是这么显示的
     * （"FEKernel ??????"）。这不是 bug，是"自绘 5x7 字体"的边界：
     * 加 CJK 要 16x16 点阵、几千个字形，那是另一个工程。
     * 代码注释用中文、屏显用英文，这个分工要写清楚，
     * 否则下一个人往里塞中文，屏幕上就是一串问号。 */
    cursor_erase();
    console_write("FEKernel console (consoled)\n");
    console_write("========================\n");
    console_write("framebuffer owned by a user-space service\n");
    console_write("mode ");
    {
        char tmp[24];
        u32 n = 0;
        char rev[12];
        u32 r = 0;
        u32 w = (u32)g_fb.width;
        do { rev[r++] = (char)('0' + w % 10); w /= 10; } while (w);
        while (r) { tmp[n++] = rev[--r]; }
        tmp[n++] = 'x';
        w = (u32)g_fb.height;
        r = 0;
        do { rev[r++] = (char)('0' + w % 10); w /= 10; } while (w);
        while (r) { tmp[n++] = rev[--r]; }
        tmp[n++] = ' ';
        w = g_cols;
        r = 0;
        do { rev[r++] = (char)('0' + w % 10); w /= 10; } while (w);
        while (r) { tmp[n++] = rev[--r]; }
        tmp[n++] = 'x';
        w = g_rows;
        r = 0;
        do { rev[r++] = (char)('0' + w % 10); w /= 10; } while (w);
        while (r) { tmp[n++] = rev[--r]; }
        tmp[n] = 0;
        console_write(tmp);
    }
    console_write(" text cells\n");
    console_write("type 'help' for commands\n");

    /* 7. 输入端点 + 输入线程。
     *
     * ★ 顺序要紧：先发布端点，再启动线程 ★
     * 端点先发布，kbd 才可能连上来；如果先起线程再发布，
     * 中间那一小段时间里 kbd 找不到端点（它会重试，所以不致命，
     * 但"先发布再消费"是更清楚的因果顺序）。 */
    long in_ep = fe_endpoint_create(0);
    if (in_ep <= 0) {
        say("  [consoled] **创建输入端点失败**\n");
        return 1;
    }
    if (fe_devfs_publish(FE_KBD_CONSOLE_IN_PATH, in_ep) != FE_OK) {
        say("  [consoled] **发布输入端点失败**\n");
        return 1;
    }
    say("  [consoled] OK   发布输入端点 ");
    say(FE_KBD_CONSOLE_IN_PATH);
    say("\n");
    if (fe_thread_create(input_thread, (void *)(long)in_ep, 0, 0) == 0) {
        say("  [consoled] **输入线程创建失败**（键盘不会有人处理）\n");
        return 1;
    }
    say("  [consoled] OK   输入线程已启动\n");

    /* 8. 发布 /dev/console，进入服务循环 */
    long ep = fe_endpoint_create(0);
    if (ep <= 0) {
        say("  [consoled] **创建端点失败**\n");
        return 1;
    }
    if (fe_devfs_publish("/dev/console", ep) != FE_OK) {
        say("  [consoled] **发布 /dev/console 失败**\n");
        return 1;
    }
    say("  [consoled] OK   发布 /dev/console\n");
    say("  [consoled] 初始化结束，失败项 ");
    num((u64)g_fail);
    say("；进入请求服务循环\n");

    static struct con_req req;
    static char payload[FE_MSG_MAX_PAYLOAD];
    for (;;) {
        struct fe_msg_header hdr;
        long reply = fe_endpoint_recv(ep, &hdr, payload, sizeof(payload), NULL, NULL);
        if (reply <= 0) {
            continue;
        }
        if (hdr.payload_len < sizeof(req)) {
            con_reply(reply, NULL, 0);
            fe_handle_close(reply);
            continue;
        }
        memcpy(&req, payload, sizeof(req));
        if (req.op == CON_OP_INFO) {
            struct con_info info;
            info.width = (u32)g_fb.width;
            info.height = (u32)g_fb.height;
            info.cols = g_cols;
            info.rows = g_rows;
            info.bpp = g_fb.bpp;
            info.scale = g_scale;
            con_reply(reply, &info, sizeof(info));
        } else if (req.op == CON_OP_WRITE) {
            u32 n = req.len;
            if (n > hdr.payload_len - sizeof(req)) {
                n = hdr.payload_len - sizeof(req);
            }
            console_write_n(payload + sizeof(req), n);
            u32 status = 1;
            con_reply(reply, &status, sizeof(status));
        } else if (req.op == CON_OP_READ) {
            con_lock();
            if (line_ready()) {
                u32 n = g_line_len;
                if (n >= req.len) {
                    n = req.len ? req.len - 1 : 0;
                }
                con_reply(reply, g_line, n);
                g_line_len = 0;
                con_unlock();
            } else if (g_reader_reply > 0) {
                /* 已经有一个读者在等：明确告知"暂时没有"，
                 * 而不是把它的回复也挂起。控制台**只有一个读者**（shell），
                 * 为一个不存在的第二个读者去实现等待队列是虚构的需求。 */
                con_unlock();
                con_reply(reply, NULL, 0);
            } else {
                /* ★ 挂起回复：把回复端点**留着**，等有行了再回复 ★
                 *
                 * 这就是"阻塞读"的实现方式，也是本服务第一个**异步**路径：
                 * 此前的所有请求都是"收到就立即回"。留句柄不是泄漏——
                 * 它正是这个未完成请求的代表，行到了就消费掉它并关闭。
                 * 反过来如果用"立即回一个空行"来偷懒，调用者（shell）
                 * 就得写轮询循环，而按键是事件流，轮询毫无必要。 */
                g_reader_reply = reply;
                g_reader_cap = req.len;
                con_unlock();
                continue;       /* ★ 不能关 reply：它就是我们要留的那个句柄 ★ */
            }
        } else {
            con_reply(reply, NULL, 0);
        }
        fe_handle_close(reply);
    }
}
