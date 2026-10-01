/* SPDX-License-Identifier: 0BSD */
#include <fe/pic.h>
#include <fe/io.h>
#include <fe/kprintf.h>

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

#define ICW1_INIT  0x10
#define ICW1_ICW4  0x01
#define ICW4_8086  0x01

#define PIC_EOI    0x20

static u16 g_mask = 0xFFFF;

static void pic_write_masks(void)
{
    fe_outb(PIC1_DATA, (u8)(g_mask & 0xFF));
    fe_outb(PIC2_DATA, (u8)((g_mask >> 8) & 0xFF));
}

void fe_pic_init(void)
{
    /* 保存原中断向量表偏移并重新映射到 32..47，避免与 CPU 异常向量冲突 */
    fe_outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
    fe_io_wait();
    fe_outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);
    fe_io_wait();

    fe_outb(PIC1_DATA, FE_IRQ_BASE);        /* 主片 → 向量 32 */
    fe_io_wait();
    fe_outb(PIC2_DATA, FE_IRQ_BASE + 8);    /* 从片 → 向量 40 */
    fe_io_wait();

    fe_outb(PIC1_DATA, 0x04);               /* 主片：IRQ2 上挂从片 */
    fe_io_wait();
    fe_outb(PIC2_DATA, 0x02);               /* 从片：级联标识 */
    fe_io_wait();

    fe_outb(PIC1_DATA, ICW4_8086);
    fe_io_wait();
    fe_outb(PIC2_DATA, ICW4_8086);
    fe_io_wait();

    g_mask = 0xFFFF;
    pic_write_masks();
}

void fe_pic_mask(u8 irq)
{
    if (irq >= FE_IRQ_COUNT) {
        return;
    }
    g_mask |= (u16)(1u << irq);
    pic_write_masks();
}

void fe_pic_unmask(u8 irq)
{
    if (irq >= FE_IRQ_COUNT) {
        return;
    }
    g_mask &= (u16)~(1u << irq);
    pic_write_masks();
}

void fe_pic_mask_all(void)
{
    g_mask = 0xFFFF;
    pic_write_masks();
}

void fe_pic_eoi(u8 irq)
{
    if (irq >= 8) {
        fe_outb(PIC2_CMD, PIC_EOI);
    }
    fe_outb(PIC1_CMD, PIC_EOI);
}
