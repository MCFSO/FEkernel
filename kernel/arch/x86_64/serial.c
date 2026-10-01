/* SPDX-License-Identifier: 0BSD */
#include <fe/serial.h>
#include <fe/io.h>
#include <fe/kprintf.h>

/* 16550 寄存器偏移 */
#define REG_DATA        0   /* 发送/接收保持寄存器 (DLAB=0) */
#define REG_IER         1   /* 中断使能 (DLAB=0) */
#define REG_DLL         0   /* 波特率除数低字节 (DLAB=1) */
#define REG_DLM         1   /* 波特率除数高字节 (DLAB=1) */
#define REG_FCR         2   /* FIFO 控制 (写) */
#define REG_IIR         2   /* 中断标识 (读) */
#define REG_LCR         3   /* 线路控制 */
#define REG_MCR         4   /* 调制解调器控制 */
#define REG_LSR         5   /* 线路状态 */
#define REG_MSR         6   /* 调制解调器状态 */
#define REG_SCRATCH     7   /* 暂存寄存器，用于探测设备是否存在 */

#define LSR_DATA_READY  0x01
#define LSR_THR_EMPTY   0x20

static u16 g_console_port = FE_COM1;

bool fe_serial_present(u16 port)
{
    /* 暂存寄存器回读是检测 16550 是否存在的标准手段 */
    fe_outb(port + REG_SCRATCH, 0xA5);
    if (fe_inb(port + REG_SCRATCH) != 0xA5) {
        return false;
    }
    fe_outb(port + REG_SCRATCH, 0x5A);
    return fe_inb(port + REG_SCRATCH) == 0x5A;
}

void fe_serial_init(u16 port)
{
    fe_outb(port + REG_IER, 0x00);  /* 关闭串口中断：我们用轮询输出 */
    fe_outb(port + REG_LCR, 0x80);  /* 打开 DLAB 以设置波特率 */
    fe_outb(port + REG_DLL, 0x01);  /* 除数 1 → 115200 bps */
    fe_outb(port + REG_DLM, 0x00);
    fe_outb(port + REG_LCR, 0x03);  /* 8 位数据、1 位停止、无校验 */
    fe_outb(port + REG_FCR, 0xC7);  /* 使能 FIFO、清空、14 字节触发 */
    fe_outb(port + REG_MCR, 0x0B);  /* DTR + RTS + OUT2 */
}

void fe_serial_putc(u16 port, char c)
{
    /* 等待发送保持寄存器空。加超时保护：万一串口不存在也不会死循环卡住内核。 */
    for (u32 spin = 0; spin < 100000; spin++) {
        if (fe_inb(port + REG_LSR) & LSR_THR_EMPTY) {
            break;
        }
    }
    if (c == '\n') {
        fe_outb(port + REG_DATA, '\r');
        for (u32 spin = 0; spin < 100000; spin++) {
            if (fe_inb(port + REG_LSR) & LSR_THR_EMPTY) {
                break;
            }
        }
    }
    fe_outb(port + REG_DATA, (u8)c);
}

void fe_serial_write(u16 port, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        fe_serial_putc(port, s[i]);
    }
}

void fe_serial_puts(u16 port, const char *s)
{
    while (*s) {
        fe_serial_putc(port, *s++);
    }
}

u16 fe_serial_console_port(void)
{
    return g_console_port;
}

void fe_serial_set_console_port(u16 port)
{
    g_console_port = port;
}

/* ---------------- 内核控制台后端 ---------------- */

void fe_console_putc(char c)
{
    fe_serial_putc(g_console_port, c);
}

void fe_console_write(const char *s, size_t n)
{
    fe_serial_write(g_console_port, s, n);
}

void fe_console_puts(const char *s)
{
    fe_serial_puts(g_console_port, s);
}
