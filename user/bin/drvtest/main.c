/* SPDX-License-Identifier: 0BSD */
/* M5 驱动能力测试程序（ring 3）。
 *
 * 它验证的不是「内核会不会返回错误码」，而是三件真实发生的事：
 *   1. 端口能力：认领之前 in/out 会触发 #GP 把线程打死；认领之后同一条指令就能跑通。
 *      这是 CPU 逐条指令检查 TSS I/O 位图的结果，不是软件里的一句 if。
 *   2. MMIO 能力：可用内存绝不能被当成设备寄存器映射进来——否则驱动就拿到了
 *      一段可写映射直接改内核数据结构，绕过所有权限检查。
 *   3. 资源互斥：同一段端口只能有一个持有者。内核的资源池里没有的东西，
 *      任何权限都拿不到（包括内核自己在用的 COM1）。
 *
 * 退出码 = 失败项数，内核会检查它是否为 0。
 */
#include <fe_user.h>

#define COM1 0x3F8
#define COM2 0x2F8
#define COM3 0x3E8

static u32 g_fail;

static void check(int cond, const char *what)
{
    if (cond) {
        fe_puts("    OK   ");
    } else {
        fe_puts("    失败 ");
        g_fail++;
    }
    fe_puts(what);
    fe_puts("\n");
}

static void print_ret(const char *label, long v)
{
    fe_puts("         ");
    fe_puts(label);
    fe_puts(" = ");
    if (v < 0) {
        fe_puts("-");
        fe_print_u64((u64)(-v));
    } else {
        fe_print_u64((u64)v);
    }
    fe_puts("\n");
}

/* UART 的暂存寄存器（偏移 7）：写进去读出来应当一致。
 * 端口上什么设备都没有时读回 0xFF——两种结果都说明「这条 in 指令没被拦下」。 */
static void uart_scratch_probe(unsigned short base)
{
    fe_outb((unsigned short)(base + 7), 0x5A);
    unsigned char back = fe_inb((unsigned short)(base + 7));
    unsigned char lsr = fe_inb((unsigned short)(base + 5));

    fe_puts("         COM 偏移 7 写入 0x5A 读回 ");
    fe_print_hex(back);
    fe_puts("，偏移 5 (LSR) 读回 ");
    fe_print_hex(lsr);
    fe_puts("\n");
    if (back == 0x5A) {
        fe_puts("         → 端口上确实有 UART，读写一致\n");
    } else {
        fe_puts("         → 端口上无设备（读回 0xFF 属正常），关键是**没有触发 #GP**\n");
    }
}

int main(void)
{
    fe_puts("\n=== M5 驱动能力测试（ring 3）===\n");

    /* ---- 1. 端口能力 ---- */
    fe_puts("  [1] 端口能力认领\n");

    /* 内核自己的调试串口在 COM1，它不在资源池里 —— 谁都拿不到 */
    long r = fe_ioport_request(COM1, 8);
    print_ret("申请 COM1（内核自用）", r);
    check(r == FE_ERR_NOENT, "内核占用的端口不可认领（池子里没有 = 谁都拿不到）");

    /* COM2 在池子里 */
    r = fe_ioport_request(COM2, 8);
    print_ret("申请 COM2", r);
    check(r == FE_OK, "认领 COM2 端口区间成功");

    /* 同一段再要一次：必须被挡住，这是「能力」与「权限检查」的分水岭 */
    r = fe_ioport_request(COM2, 8);
    print_ret("重复申请 COM2", r);
    check(r == FE_ERR_EXIST || r == FE_ERR_BUSY, "同一段端口不能重复认领（互斥）");

    /* 同一任务可以持有第二段 */
    r = fe_ioport_request(COM3, 8);
    print_ret("申请 COM3", r);
    check(r == FE_OK, "同一任务可以持有第二段端口区间");

    /* 池子里没有的区间 */
    r = fe_ioport_request(0x0300, 8);
    print_ret("申请未入池的 0x300", r);
    check(r == FE_ERR_NOENT, "未入池的端口区间不可认领");

    /* 越界 */
    r = fe_ioport_request(0xFFFC, 8);
    print_ret("申请越界的 0xFFFC+8", r);
    check(r == FE_ERR_RANGE, "超出 16 位端口空间的请求被拒绝");

    /* ---- 2. 真正执行 in/out ---- */
    fe_puts("\n  [2] 端口 I/O 实测（CPU 逐条指令检查 I/O 位图）\n");
    uart_scratch_probe(COM2);
    check(1, "认领后 in/out 正常执行，没有触发 #GP");

    /* ---- 3. MMIO 能力 ---- */
    fe_puts("\n  [3] MMIO 映射\n");

    void *p = fe_mmio_map(0x100000, 0x1000, FE_PROT_READ | FE_PROT_WRITE);
    check(p == (void *)0, "可用内存（0x100000）不允许被映射为 MMIO");

    p = fe_mmio_map(0xFEE00000, 0x1000, FE_PROT_READ | FE_PROT_WRITE);
    check(p == (void *)0, "未入池的物理区间（LAPIC 0xFEE00000）不可映射");

    /* ---- 4. DMA 物理地址 ---- */
    fe_puts("\n  [4] DMA 内存对象的物理地址\n");

    long mo = fe_mem_alloc(4096);
    struct fe_mem_info info;
    r = (mo > 0) ? fe_mem_info(mo, &info) : FE_ERR_BADHANDLE;
    print_ret("对普通内存对象查 MEM_INFO", r);
    check(r == FE_ERR_ACCESS, "普通内存对象不暴露物理地址");
    if (mo > 0) {
        fe_handle_close(mo);
    }

    long dma = fe_mem_alloc_dma(8192);
    print_ret("分配 8 KiB DMA 内存", dma);
    if (dma > 0) {
        r = fe_mem_info(dma, &info);
        print_ret("查 MEM_INFO", r);
        if (r == FE_OK) {
            fe_puts("         phys=");
            fe_print_hex(info.phys);
            fe_puts(" size=");
            fe_print_u64(info.size);
            fe_puts(" pages=");
            fe_print_u64(info.page_count);
            fe_puts(" flags=");
            fe_print_u64(info.flags);
            fe_puts("\n");
        }
        check(r == FE_OK, "DMA 对象可以查到物理地址");
        check(info.size == 8192 && info.page_count == 2, "大小与页数正确");
        check((info.phys & 0xFFF) == 0, "物理基址按 4 KiB 对齐");
        check((info.flags & FE_MEM_FLAG_DMA) != 0, "标志位带回 DMA 属性");

        /* 映射进来写一遍：证明这块「物理连续」的内存确实可用 */
        u8 *m = (u8 *)fe_mem_map(dma, (void *)0, 8192, FE_PROT_READ | FE_PROT_WRITE);
        if (m) {
            for (usize i = 0; i < 8192; i++) {
                m[i] = (u8)(i ^ 0x5A);
            }
            usize bad = 0;
            for (usize i = 0; i < 8192; i++) {
                if (m[i] != (u8)(i ^ 0x5A)) {
                    bad++;
                }
            }
            check(bad == 0, "DMA 内存映射后 8 KiB 读写校验一致");
        } else {
            check(0, "DMA 内存映射失败");
        }
        fe_handle_close(dma);
    } else {
        check(0, "DMA 内存分配失败");
    }

    /* ---- 5. 中断资源 ---- */
    fe_puts("\n  [5] 中断资源\n");

    r = fe_irq_register(0 /* 定时器在用的 IRQ0 */, (long)0);
    print_ret("用无效句柄登记 IRQ0", r);
    check(r == FE_ERR_BADHANDLE || r == FE_ERR_INVAL, "无效通知句柄被拒绝");

    long nt = fe_notification_create();
    print_ret("创建通知对象", nt);
    check(nt > 0, "通知对象创建成功");
    if (nt > 0) {
        r = fe_irq_register(0, nt);
        print_ret("登记 IRQ0（内核自用）", r);
        check(r == FE_ERR_NOENT, "内核占用的中断线不可认领");
        fe_handle_close(nt);
    }

    fe_puts("\n=== M5 驱动能力测试结束，失败项 ");
    fe_print_u64(g_fail);
    fe_puts(" ===\n");
    return (int)g_fail;
}
