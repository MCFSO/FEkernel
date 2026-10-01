/* SPDX-License-Identifier: 0BSD */
#include <fe/pit.h>
#include <fe/io.h>

#define PIT_CH0_DATA 0x40
#define PIT_CH1_DATA 0x41
#define PIT_CH2_DATA 0x42
#define PIT_CMD      0x43
#define PIT_PORT_B   0x61

/* 命令字：通道0 + 先低后高字节 + 模式3(方波) + 二进制计数 */
#define PIT_CMD_CH0_MODE3 0x36

void fe_pit_program(u32 hz)
{
    if (hz == 0) {
        hz = 100;
    }
    u32 divisor = FE_PIT_BASE_HZ / hz;
    if (divisor == 0) {
        divisor = 1;
    }
    if (divisor > 65535) {
        divisor = 65535;
    }
    fe_outb(PIT_CMD, PIT_CMD_CH0_MODE3);
    fe_outb(PIT_CH0_DATA, (u8)(divisor & 0xFF));
    fe_outb(PIT_CH0_DATA, (u8)((divisor >> 8) & 0xFF));
}

/* 用通道 2 做与中断无关的忙等延时。
 * 通道 2 的门控位在端口 0x61 的 bit0，扬声器使能在 bit1（保持关闭）。 */
void fe_pit_busy_wait_ms(u32 ms)
{
    for (u32 i = 0; i < ms; i++) {
        u8 port_b = fe_inb(PIT_PORT_B);
        fe_outb(PIT_PORT_B, (u8)((port_b & ~0x02) | 0x01));   /* 打开门控，关扬声器 */
        fe_outb(PIT_CMD, 0xB0);                               /* 通道2, 模式0, 先低后高 */
        u16 divisor = (u16)(FE_PIT_BASE_HZ / 1000u);           /* 1 ms */
        fe_outb(PIT_CH2_DATA, (u8)(divisor & 0xFF));
        fe_outb(PIT_CH2_DATA, (u8)(divisor >> 8));

        u16 last = 0xFFFF;
        for (;;) {
            fe_outb(PIT_CMD, 0x80);                           /* 锁存通道 2 计数值 */
            u8 lo = fe_inb(PIT_CH2_DATA);
            u8 hi = fe_inb(PIT_CH2_DATA);
            u16 now = (u16)(lo | (hi << 8));
            if (now > last) {
                break;                                        /* 回绕 = 到期 */
            }
            last = now;
        }
        fe_outb(PIT_PORT_B, (u8)(fe_inb(PIT_PORT_B) & ~0x01)); /* 关回门控 */
    }
}
