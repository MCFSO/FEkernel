/* SPDX-License-Identifier: 0BSD */
/* 键盘服务（用户态驱动，ring 3）。
 *
 * 它认领的是「IRQ1（独占）+ PS/2 端口 0x60..0x64（与鼠标服务共享）」这个组合。
 * 为什么端口是共享的：0x60/0x64 是 8042 控制器的寄存器对，键盘与鼠标共用，
 * 硬件上分不开——真正能分开的只有中断线。共享带来的命令序列交错问题，
 * 由共享区间上的控制器锁解决（见 kernel/include/fe/resource.h）。
 *
 * ★ 它对外的接口是"推送"★
 * 消费者（控制台服务）创建自己的输入端点并通过 devfs 发布，
 * 本服务找到它、把每个按键事件发过去。为什么不做成"客户端调用取事件"，
 * 见 user/include/fe_kbd.h 里那段说明——简单说：拉取要么轮询，
 * 要么挂起回复，而挂起回复与"等 IRQ 通知"没法在同一个线程里共存。
 *
 * 于是这里**只有一个线程**：等 IRQ → 读字节 → 解码 → 发出去。
 * 这也是它比鼠标服务简单的原因（鼠标没有消费者，只打印）。
 */
#include <fe_user.h>
#include <fe_drv.h>
#include <fe_kbd.h>

#define PS2_PORT_BASE 0x60
#define PS2_PORT_LEN  5
#define KBD_IRQ       1

static u32 g_fail;

static void say(const char *s) { fe_puts(s); }

static void step(const char *what, int ok)
{
    say("  [kbd] ");
    say(ok ? "OK   " : "失败 ");
    say(what);
    say("\n");
    if (!ok) {
        g_fail++;
    }
}

/* 扫描码 set 1 → ASCII。两张表：不带 Shift 与带 Shift。
 *
 * 只做能打印的键；其余返回 0（调用者据此知道"这个键没有字符"）。
 * ★ 修掉的问题 ★ 原版只有一张未按 Shift 的表，于是**打不出大写字母，
 * 也打不出冒号、引号、问号**——shell 里连一个带大写或带符号的命令都敲不出来。
 * 这不是"锦上添花"，是"能不能用"的区别。 */
static const char g_map_base[0x40] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,  'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,  '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,  ' ', 0,  0,   0,  0,   0,
};

static const char g_map_shift[0x40] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,  'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,  '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,  ' ', 0,  0,   0,  0,   0,
};

static char scancode_to_ascii(u8 sc, u8 mods)
{
    if (sc >= 0x40) {
        return 0;
    }
    char c = (mods & FE_KBD_MOD_SHIFT) ? g_map_shift[sc] : g_map_base[sc];
    /* Caps Lock 只影响字母（与真实键盘一致：它不影响数字与符号键） */
    if ((mods & FE_KBD_MOD_CAPS) && !(mods & FE_KBD_MOD_SHIFT) &&
        c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    return c;
}

static void print_hex8(u8 v)
{
    static const char d[] = "0123456789abcdef";
    char b[2];
    b[0] = d[(v >> 4) & 0xF];
    b[1] = d[v & 0xF];
    fe_write(b, 2);
}

/* --- 推送到控制台的输入端点 ---
 *
 * ★ 必须能容忍"控制台还没起来" ★
 * 依赖顺序里 kbd 排在 consoled 之前（键盘是硬件，控制台是它的消费者），
 * 所以打开端点会先失败一阵子。这里**重试**而不是放弃，
 * 并且在还没连上时把事件丢掉（而不是缓存）：开机瞬间的按键没有意义，
 * 而为了它们去维护一个队列是给不存在的需求造机制。 */
static long g_console_in = -1;
static u32 g_dropped;

static void try_connect(void)
{
    if (g_console_in > 0) {
        return;
    }
    long h = fe_devfs_open(FE_KBD_CONSOLE_IN_PATH);
    if (h > 0) {
        g_console_in = h;
        say("  [kbd] 已连上控制台输入端点 ");
        say(FE_KBD_CONSOLE_IN_PATH);
        say(g_dropped ? "（此前丢弃的按键数见末尾）" : "");
        say("\n");
    }
}

static void push_event(const struct fe_kbd_event *ev)
{
    if (g_console_in <= 0) {
        g_dropped++;
        return;
    }
    struct fe_msg_header hdr;
    for (u32 i = 0; i < sizeof(hdr); i++) {
        ((u8 *)&hdr)[i] = 0;
    }
    hdr.payload_len = (u32)sizeof(*ev);
    if (fe_endpoint_send(g_console_in, &hdr, ev, NULL, 0) != FE_OK) {
        /* 队列满（没有背压机制，见 07-performance-roadmap 的已知项）：
         * 丢一个按键比让服务卡住强，但要计数，别让它无声无息。 */
        g_dropped++;
    }
}

int main(void)
{
    say("\n[kbd] 键盘服务启动（ring 3，独立进程）\n");

    /* --- 1. 认领共享端口 --- */
    long r = fe_ioport_request(PS2_PORT_BASE, PS2_PORT_LEN);
    step("认领 PS/2 端口 0x60..0x64（与鼠标服务共享）", r == FE_OK);
    if (r != FE_OK) {
        say("  [kbd] 无法继续，退出\n");
        return 1;
    }

    /* --- 2. 认领 IRQ1（独占；鼠标拿的是 IRQ12） --- */
    long nt = fe_notification_create();
    step("创建通知对象", nt > 0);
    if (nt <= 0) {
        return 1;
    }
    r = fe_irq_register(KBD_IRQ, nt);
    step("认领 IRQ1（独占）", r == FE_OK);
    if (r != FE_OK) {
        return 1;
    }

    /* --- 3. 控制器初始化，整段拿锁 ---
     * 这几步是「读命令字节 → 改一位 → 写回」，鼠标服务也在做同样的事，
     * 不串行化就是经典的丢更新。 */
    r = fe_resource_lock(FE_RES_IOPORT, PS2_PORT_BASE, PS2_PORT_LEN);
    step("取得 8042 控制器锁", r == FE_OK);

    /* 键盘接口自检（0xAB）：回 0x00 表示通过。不动 0xAA 整机自检——
     * 那会同时复位鼠标，而 BIOS 在 POST 时已经做过一次了。 */
    u8 resp = 0xFF;
    fe_ps2_flush();     /* 丢掉上电期间可能残留的字节，否则下面读到的是它 */
    int ok = (fe_ps2_write_cmd(0xAB) == 0) && (fe_ps2_read(&resp, 0) == 0);
    say("  [kbd] 键盘接口自检 0xAB -> ");
    print_hex8(resp);
    say(ok && resp == 0x00 ? "  （通过）\n" : "  （未通过）\n");
    if (!ok || resp != 0x00) {
        g_fail++;
    }

    /* 回显：0xEE 应当原样回 0xEE。这是「双向通路真的通」的最直接证据。 */
    resp = 0;
    ok = (fe_ps2_device_cmd(0xEE, 0, &resp) == 0);
    say("  [kbd] 键盘回显 0xEE -> ");
    print_hex8(resp);
    say(ok && resp == 0xEE ? "  （通路正常）\n" : "  （无响应）\n");
    if (!ok || resp != 0xEE) {
        g_fail++;
    }

    /* 使能扫描（0xF4），期望 ACK 0xFA */
    resp = 0;
    ok = (fe_ps2_device_cmd(0xF4, 0, &resp) == 0);
    say("  [kbd] 使能扫描 0xF4 -> ");
    print_hex8(resp);
    say(ok && resp == 0xFA ? "  （ACK）\n" : "  （无 ACK）\n");
    if (!ok || resp != 0xFA) {
        g_fail++;
    }

    /* 打开控制器命令字节里的 IRQ1 使能位 */
    ok = (fe_ps2_enable_irq(0x01) == 0);
    step("使能控制器 IRQ1（命令字节 bit0）", ok);

    fe_resource_unlock(FE_RES_IOPORT, PS2_PORT_BASE, PS2_PORT_LEN);
    step("释放 8042 控制器锁", 1);

    /* 试图连上控制台（连不上也没关系，主循环里会重试） */
    try_connect();

    say("  [kbd] 初始化结束，失败项 ");
    fe_print_u64(g_fail);
    say("；进入等待按键循环\n");

    /* --- 4. 主循环：等 IRQ1 → 读一个字节 → 解码 → 推送 --- */
    u8 extended = 0;
    u8 mods = 0;
    u32 keys = 0;
    for (;;) {
        u64 bits = 0;
        if (fe_notification_wait(nt, 1ull << KBD_IRQ, &bits) != FE_OK) {
            continue;
        }
        if (!(bits & (1ull << KBD_IRQ))) {
            continue;
        }
        fe_irq_ack(KBD_IRQ);

        u8 sc = 0;
        int from_mouse = 0;
        if (fe_ps2_read(&sc, &from_mouse) < 0) {
            continue;
        }
        if (from_mouse) {
            continue;       /* 鼠标的字节，不是我的（AUX 位说了算） */
        }
        if (sc == 0xE0) {
            extended = 1;
            continue;
        }
        if (sc == 0xE1) {   /* Pause 键的 6 字节序列：整段跳过 */
            extended = 2;
            continue;
        }

        u8 pressed = 1;
        if (sc & 0x80) {
            pressed = 0;
            sc &= 0x7F;
        }

        /* 修饰键：Shift / Ctrl / Alt / CapsLock 的按下与松开 */
        if (sc == 0x2A || sc == 0x36) {
            if (pressed) { mods |= FE_KBD_MOD_SHIFT; }
            else         { mods &= (u8)~FE_KBD_MOD_SHIFT; }
        } else if (sc == 0x1D) {
            if (pressed) { mods |= FE_KBD_MOD_CTRL; }
            else         { mods &= (u8)~FE_KBD_MOD_CTRL; }
        } else if (sc == 0x38) {
            if (pressed) { mods |= FE_KBD_MOD_ALT; }
            else         { mods &= (u8)~FE_KBD_MOD_ALT; }
        } else if (sc == 0x3A && pressed) {
            mods ^= FE_KBD_MOD_CAPS;        /* Caps Lock 是切换键 */
        }

        struct fe_kbd_event ev;
        for (u32 i = 0; i < sizeof(ev); i++) {
            ((u8 *)&ev)[i] = 0;
        }
        ev.scancode = sc;
        ev.pressed = pressed;
        ev.mods = mods;
        ev.extended = extended ? 1 : 0;
        ev.ascii = (pressed && !extended) ? (u8)scancode_to_ascii(sc, mods) : 0;

        if (extended == 2) {
            extended = 0;       /* Pause 序列结束 */
            continue;
        }
        extended = 0;
        keys++;

        try_connect();          /* 控制台可能刚刚才起来 */
        push_event(&ev);

        /* 串口上仍然打印一份：控制台还没连上时，这是唯一的现场 */
        say("  [kbd] #");
        fe_print_u64(keys);
        say(" 扫描码 ");
        print_hex8(sc);
        say(pressed ? " 按下" : " 松开");
        if (ev.ascii) {
            say(" -> '");
            fe_write((const char *)&ev.ascii, 1);
            say("'");
        }
        if (mods & FE_KBD_MOD_SHIFT) { say(" [Shift]"); }
        if (mods & FE_KBD_MOD_CTRL)  { say(" [Ctrl]"); }
        if (mods & FE_KBD_MOD_ALT)   { say(" [Alt]"); }
        if (mods & FE_KBD_MOD_CAPS)  { say(" [Caps]"); }
        if (g_console_in <= 0) {
            say("  **控制台输入端点未连上，已丢弃**");
        }
        say("\n");
    }
}
