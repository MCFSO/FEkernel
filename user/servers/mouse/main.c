/* SPDX-License-Identifier: 0BSD */
/* 鼠标服务（用户态驱动，ring 3）。
 *
 * 与键盘服务是一对：**同一个 8042 控制器的两个设备，两个独立的用户态进程**。
 *   键盘：IRQ1  + 共享端口 0x60..0x64
 *   鼠标：IRQ12 + 共享端口 0x60..0x64
 * 中断线天然分开，所以各自独占；端口物理上分不开，所以共享 + 控制器锁。
 *
 * 数据包是 3 字节：flags / dx / dy。第一个字节的 bit3 恒为 1，可以拿它做同步——
 * 万一位移和标志位错开了一位，靠这个特征能重新对齐，否则光标会一直乱跳。
 */
#include <fe_user.h>
#include <fe_drv.h>

#define PS2_PORT_BASE 0x60
#define PS2_PORT_LEN  5
#define MOUSE_IRQ     12

static u32 g_fail;

static void step(const char *what, int ok)
{
    fe_puts("  [mouse] ");
    fe_puts(ok ? "OK   " : "失败 ");
    fe_puts(what);
    fe_puts("\n");
    if (!ok) {
        g_fail++;
    }
}

static void print_hex8(u8 v)
{
    static const char d[] = "0123456789abcdef";
    char b[2];
    b[0] = d[(v >> 4) & 0xF];
    b[1] = d[v & 0xF];
    fe_write(b, 2);
}

static void print_i64(long v)
{
    if (v < 0) {
        fe_puts("-");
        fe_print_u64((u64)(-v));
    } else {
        fe_print_u64((u64)v);
    }
}

/* 解一个 3 字节包。
 *
 * ★ PS/2 的增量是"**无符号字节 + flags 里的符号位**"组成的 9 位补码 ★
 * 正确写法：把数据字节当**无符号**读（0..255），符号位在 flags 里，
 * 为 1 就减 256。
 *
 * 原来的写法是 `(i8)pkt[1]` 先按有符号解释一次，**然后又减 256**——
 * 符号用了两次。数值小于 128 时看不出来（(i8) 不改变它），
 * 一旦数据字节 ≥ 128 就错得离谱：
 *   真实值 -5  →  字节 0xfb = 251
 *   正确：251 - 256 = -5
 *   出错：251 被当成 -5，再减 256 → **-261**
 * 这个 bug 直到我第一次真的注入鼠标事件才暴露（此前鼠标路径从未被触发过）。 */
static void decode_packet(const u8 pkt[3], i64 *out_dx, i64 *out_dy)
{
    i64 dx = (i64)pkt[1];               /* 无符号：0..255 */
    i64 dy = (i64)pkt[2];
    if (pkt[0] & 0x10) { dx -= 256; }   /* X 符号位 = flags.bit4 */
    if (pkt[0] & 0x20) { dy -= 256; }   /* Y 符号位 = flags.bit5 */
    *out_dx = dx;
    *out_dy = dy;
}

/* 解码逻辑的自检：**用合成包验函数**，不依赖注入。
 *
 * ★ 为什么不能只靠注入事件 ★
 * 注入能证明"IRQ、读取、组包"这条路径通，但证明不了**数值算得对**——
 * 因为注入端（QEMU monitor）到底发了什么字节，不受我们控制；
 * 我按"我注入的是 dx=25"去断言，结果对不上，还分不清是解码错还是注入没按预期编码。
 * 拆开之后各测各的：这里用确定的字节验算法，注入那边只验"包能收全"。
 *
 * ★ flags 字节的位（写错这个就是我第一版自检失败的唯一原因）★
 *   bit0/1/2 = 左/右/中键     bit3 = 恒为 1（同步）
 *   bit4 = X 符号（0x10）      bit5 = Y 符号（0x20）
 * 我第一版把"两个符号都置位"写成了 0x18，而 0x18 = bit3|bit4 ——
 * 只置了 X 的符号，于是 dy 的期望值错了。**自检抓的正是这种错**：
 * 如果它只是"把代码再算一遍"，就永远发现不了。 */
static u32 decode_selftest(void)
{
    static const struct {
        u8 pkt[3];
        i64 dx, dy;
        const char *what;
    } cases[] = {
        { {0x08, 0x00, 0x00},    0,    0, "静止（无按键、无位移）" },
        { {0x08, 0x7f, 0x7f},  127,  127, "最大正值（符号位为 0）" },
        { {0x08, 0x80, 0x80},  128,  128, "字节 0x80：无符号位时是 +128" },
        { {0x18, 0x01, 0x01}, -255,    1, "只有 X 符号位（bit4）" },
        { {0x28, 0x01, 0x01},    1, -255, "只有 Y 符号位（bit5）" },
        { {0x38, 0x01, 0x01}, -255, -255, "两个符号位都置（bit4|bit5）" },
        { {0x28, 0x19, 0xf6},   25,  -10, "上右下左各一路：+25 / -10" },
        { {0x38, 0xfb, 0xfb},   -5,   -5, "0xfb 带符号 = -5（原 bug 会算成 -261）" },
        { {0x09, 0x01, 0x00},    1,    0, "左键按下 + 右移 1" },
    };
    u32 fail = 0;
    for (u32 i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        i64 dx = 0, dy = 0;
        decode_packet(cases[i].pkt, &dx, &dy);
        if (dx != cases[i].dx || dy != cases[i].dy) {
            fe_puts("  [mouse] **解码自检失败** ");
            fe_puts(cases[i].what);
            fe_puts("：期望 dx=");
            print_i64((long)cases[i].dx);
            fe_puts(" dy=");
            print_i64((long)cases[i].dy);
            fe_puts("，得到 dx=");
            print_i64((long)dx);
            fe_puts(" dy=");
            print_i64((long)dy);
            fe_puts("\n");
            fail++;
        }
    }
    return fail;
}

int main(void)
{
    fe_puts("\n[mouse] 鼠标服务启动（ring 3，独立进程）\n");

    /* 先验算法，再碰硬件：解码错了的话，后面收到多少包都没意义 */
    u32 fail = decode_selftest();
    step("3 字节包解码（含符号与边界）", fail == 0);

    long r = fe_ioport_request(PS2_PORT_BASE, PS2_PORT_LEN);
    step("认领 PS/2 端口 0x60..0x64（与键盘服务共享）", r == FE_OK);
    if (r != FE_OK) {
        return 1;
    }

    long nt = fe_notification_create();
    step("创建通知对象", nt > 0);
    if (nt <= 0) {
        return 1;
    }
    r = fe_irq_register(MOUSE_IRQ, nt);
    step("认领 IRQ12（独占）", r == FE_OK);
    if (r != FE_OK) {
        return 1;
    }

    /* --- 控制器初始化，整段拿锁 --- */
    r = fe_resource_lock(FE_RES_IOPORT, PS2_PORT_BASE, PS2_PORT_LEN);
    step("取得 8042 控制器锁", r == FE_OK);

    /* 使能辅助设备（鼠标）接口：0xA8 */
    int ok = (fe_ps2_write_cmd(0xA8) == 0);
    step("使能鼠标接口 0xA8", ok);

    /* 紧接着就把控制器命令字节的 IRQ12 位打开、并保证 aux 时钟线是开的。
     * 顺序照 Linux 的 i8042 驱动来：先让辅助端口**可用且中断可达**，
     * 再和鼠标对话。反过来（先对话后开端口）实测在 VirtualBox 上
     * 设备命令全部读超时——端口没真正启用时它不应答。 */
    ok = (fe_ps2_enable_irq(0x02) == 0);
    step("使能控制器 IRQ12（命令字节 bit1）", ok);

    /* 鼠标接口自检 0xA9 → 0x00 通过 */
    u8 resp = 0xFF;
    fe_ps2_flush();
    ok = (fe_ps2_write_cmd(0xA9) == 0) && (fe_ps2_read(&resp, 0) == 0);
    fe_puts("  [mouse] 鼠标接口自检 0xA9 -> ");
    print_hex8(resp);
    fe_puts(ok && resp == 0x00 ? "  （通过）\n" : "  （未通过）\n");
    if (!ok || resp != 0x00) {
        g_fail++;
    }

    /* 开数据上报（0xF4），再设采样率（0xF3 0x64）。
     *
     * 顺序是有讲究的：先把设备**唤醒**（0xF4 让它开始上报），再去调参数。
     * 实测在 VirtualBox 的 8042 模拟上，如果先发 0xF3，鼠标根本不应答
     * （读超时），而先发 0xF4 能拿到 0xFA。协议上两者都合法，
     * 但「先让设备进入已知状态、再微调」对模拟器和真机都更稳。 */
    resp = 0;
    ok = (fe_ps2_device_cmd(0xF4, 1, &resp) == 0);
    fe_puts("  [mouse] 开数据上报 0xF4 -> ");
    print_hex8(resp);
    fe_puts(ok && resp == 0xFA ? "  （ACK）\n" : "  （无 ACK）\n");
    if (!ok || resp != 0xFA) {
        g_fail++;
    }

    /* 设采样率 100Hz。
     *
     * ★ 这一条在 VirtualBox 的 8042 模拟上**读不到应答**（上面几条都能）。★
     * 不影响功能：PS/2 鼠标上电默认采样率本来就是 100 次/秒，这条命令只是
     * 显式确认一遍。所以这里只警告、不计入失败——但也不掩盖：
     * 真机上它是否响应，目前没有证据，得等有真机测试才能下结论。 */
    resp = 0;
    ok = (fe_ps2_device_cmd_param(0xF3, 0x64, 1, &resp) == 0);
    fe_puts("  [mouse] 设采样率 100Hz (0xF3 0x64) -> ");
    print_hex8(resp);
    if (ok && resp == 0xFA) {
        fe_puts("  （ACK）\n");
    } else {
        fe_puts("  （无应答；默认采样率即 100Hz，不影响功能）\n");
    }

    fe_resource_unlock(FE_RES_IOPORT, PS2_PORT_BASE, PS2_PORT_LEN);
    step("释放 8042 控制器锁", 1);

    fe_puts("  [mouse] 初始化结束，失败项 ");
    fe_print_u64(g_fail);
    fe_puts("；进入等待鼠标数据循环\n");

    /* --- 主循环：3 字节一包 --- */
    u8 pkt[3];
    u32 got = 0;
    u32 packets = 0;
    for (;;) {
        u64 bits = 0;
        if (fe_notification_wait(nt, 1ull << MOUSE_IRQ, &bits) != FE_OK) {
            continue;
        }
        if (!(bits & (1ull << MOUSE_IRQ))) {
            continue;
        }
        fe_irq_ack(MOUSE_IRQ);

        u8 b = 0;
        int from_mouse = 0;
        if (fe_ps2_read(&b, &from_mouse) < 0) {
            continue;
        }
        if (!from_mouse) {
            continue;       /* 键盘的字节，不是我的 */
        }

        /* 同步：包的第一个字节 bit3 必须为 1。错位时丢掉重新对齐，
         * 否则一次错位之后每一包都是歪的。 */
        if (got == 0 && !(b & 0x08)) {
            continue;
        }
        pkt[got++] = b;
        if (got < 3) {
            continue;
        }
        got = 0;
        packets++;

        i64 dx = 0, dy = 0;
        decode_packet(pkt, &dx, &dy);

        fe_puts("  [mouse] 第 ");
        fe_print_u64(packets);
        fe_puts(" 包: 按键=");
        fe_print_u64(pkt[0] & 0x07);
        fe_puts(" dx=");
        print_i64((long)dx);
        fe_puts(" dy=");
        print_i64((long)dy);
        fe_puts("  原始 ");
        print_hex8(pkt[0]);
        fe_puts(" ");
        print_hex8(pkt[1]);
        fe_puts(" ");
        print_hex8(pkt[2]);
        fe_puts("\n");
    }
}
