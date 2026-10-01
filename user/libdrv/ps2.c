/* SPDX-License-Identifier: 0BSD */
/* 8042（PS/2）控制器访问层，由键盘与鼠标两个服务共用。
 *
 * 为什么放在 libfe 旁边：构建系统里用户态只有「libfe 公共库 + 各程序」两层，
 * 没有第三方共享库的位置。这段代码是**驱动代码**而不是运行库，
 * 但让两个服务各抄一份显然更糟（改一处忘一处，正是驱动 bug 的经典来源）。
 *
 * 这一层只做「把字节可靠地送进/取出控制器」，不含任何设备语义——
 * 键盘扫描码与鼠标包的解析在各自的服务里。
 *
 * 所有等待都有超时：设备不存在时宁可报错也不要挂死，
 * 因为这两个服务是开机就跑的，挂死会变成「系统启动卡住」而不是「驱动有问题」。
 */
#include <fe_drv.h>

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define PS2_ST_OBF 0x01     /* 输出缓冲满 → 可以读数据 */
#define PS2_ST_IBF 0x02     /* 输入缓冲满 → 还不能写 */
#define PS2_ST_AUX 0x20     /* 数据来自鼠标而不是键盘 */

/* 超时用时间而不是循环次数：虚拟机与真机的速度差一个数量级，
 * 按次数写死会在一边过于宽松、在另一边误报。 */
#define PS2_TIMEOUT_NS 100000000ull     /* 100 ms */

static int wait_clear(int bit)
{
    u64 t0 = fe_clock_ns();
    while (fe_inb(PS2_STATUS) & bit) {
        if (fe_clock_ns() - t0 > PS2_TIMEOUT_NS) {
            return -1;
        }
    }
    return 0;
}

static int wait_set(int bit)
{
    u64 t0 = fe_clock_ns();
    while (!(fe_inb(PS2_STATUS) & bit)) {
        if (fe_clock_ns() - t0 > PS2_TIMEOUT_NS) {
            return -1;
        }
    }
    return 0;
}

/* 读一个字节。from_mouse 非空时带回它是否来自鼠标（AUX 位）。 */
int fe_ps2_read(u8 *out, int *from_mouse)
{
    if (wait_set(PS2_ST_OBF) < 0) {
        return -1;
    }
    u8 st = fe_inb(PS2_STATUS);
    *out = fe_inb(PS2_DATA);
    if (from_mouse) {
        *from_mouse = (st & PS2_ST_AUX) ? 1 : 0;
    }
    return 0;
}

int fe_ps2_write_data(u8 v)
{
    if (wait_clear(PS2_ST_IBF) < 0) {
        return -1;
    }
    fe_outb(PS2_DATA, v);
    return 0;
}

int fe_ps2_write_cmd(u8 v)
{
    if (wait_clear(PS2_ST_IBF) < 0) {
        return -1;
    }
    fe_outb(PS2_CMD, v);
    return 0;
}

/* 读控制器的命令字节（0x20），并按 mask 置位后写回（0x60）。
 * 键盘要 bit0（IRQ1 使能），鼠标要 bit1（IRQ12 使能）。
 * 这是读-改-写，两个服务同时做就会互相覆盖——所以调用方必须持有控制器锁。 */
int fe_ps2_enable_irq(u8 mask)
{
    if (fe_ps2_write_cmd(0x20) < 0) {
        return -1;
    }
    u8 cb = 0;
    if (fe_ps2_read(&cb, 0) < 0) {
        return -1;
    }
    if ((cb & mask) == mask) {
        return 0;                   /* 已经开了，不必再写一遍 */
    }
    cb |= mask;
    /* 关掉扫描码翻译（bit6）：我们自己按 set 1 解析，翻译开着反而会串 */
    if (fe_ps2_write_cmd(0x60) < 0) {
        return -1;
    }
    return fe_ps2_write_data(cb);
}

/* 带参数的设备命令（如 0xF3 <采样率>）。协议规定**每个字节都要单独 ACK**，
 * 所以参数字节也得走同样的 flush + RESEND 重发逻辑——
 * 之前只在命令字节上做了重发，参数字节撞上 0xFE 就当成「无 ACK」报错了。 */
int fe_ps2_device_cmd_param(u8 cmd, u8 param, int to_mouse, u8 *resp)
{
    for (int attempt = 0; attempt < 4; attempt++) {
        fe_ps2_flush();
        if (to_mouse && fe_ps2_write_cmd(0xD4) < 0) {
            return -1;
        }
        if (fe_ps2_write_data(cmd) < 0) {
            return -1;
        }
        u8 r = 0;
        int aux = 0;
        if (fe_ps2_read(&r, &aux) < 0) {
            return -1;
        }
        if (r == 0xFE) {
            continue;
        }
        if (r != 0xFA) {
            if (resp) {
                *resp = r;
            }
            return -1;          /* 命令字节就没被接受 */
        }
        if (fe_ps2_write_data(param) < 0) {
            return -1;
        }
        if (fe_ps2_read(&r, &aux) < 0) {
            return -1;
        }
        if (r == 0xFE) {
            continue;           /* 参数字节要求重发：整条命令重来 */
        }
        if (resp) {
            *resp = r;
        }
        return 0;
    }
    return -1;
}

/* 清空输出缓冲里的陈旧字节。
 *
 * 这是 PS/2 驱动最常见的坑：上一个命令的残余应答（或设备主动发出的 0xFE RESEND）
 * 还躺在输出缓冲里，下一个命令读到的就是**它的**应答，
 * 于是驱动以为自己发的命令得到了一个莫名其妙的回复。
 * 实测就撞上了：0xF4 使能扫描读回来的其实是之前残留的 0xFE。 */
void fe_ps2_flush(void)
{
    for (int guard = 0; guard < 32; guard++) {
        if (!(fe_inb(PS2_STATUS) & PS2_ST_OBF)) {
            return;
        }
        (void)fe_inb(PS2_DATA);
    }
}

/* 给**设备**发一个字，并等它的应答。
 *
 * 应答语义（PS/2 协议）：
 *   0xFA = ACK      —— 收到并接受
 *   0xFE = RESEND   —— 没听懂，要求重发。**必须重发**，否则命令等于没发。
 *   0xFC/0xFD       —— 自检失败等
 * 之前没处理 RESEND，结果「使能扫描」这条命令实际没生效，
 * 表现是按键完全没反应——而日志里只看到一个空白的应答字节。 */
int fe_ps2_device_cmd(u8 cmd, int to_mouse, u8 *resp)
{
    for (int attempt = 0; attempt < 4; attempt++) {
        fe_ps2_flush();
        if (to_mouse) {
            if (fe_ps2_write_cmd(0xD4) < 0) {   /* 下一个数据字节发给鼠标 */
                return -1;
            }
        }
        if (fe_ps2_write_data(cmd) < 0) {
            return -1;
        }
        u8 r = 0;
        int aux = 0;
        if (fe_ps2_read(&r, &aux) < 0) {
            return -1;
        }
        if (r == 0xFE) {
            continue;               /* 设备要求重发 */
        }
        if (resp) {
            *resp = r;
        }
        return 0;
    }
    return -1;      /* 连续重发仍然失败 */
}
