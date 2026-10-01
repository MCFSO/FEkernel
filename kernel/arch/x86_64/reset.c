/* SPDX-License-Identifier: 0BSD */
/* 机器复位（x86_64）。
 *
 * 三条路径依次尝试，因为**没有一条在三个环境里都保证有效**：
 *
 *   1. 8042 键盘控制器的 RESET 线（向 0x64 写 0xFE）
 *      —— 最老、最通用，而且它是**一次干净的系统复位**：
 *         芯片组不留下任何"粘住"的状态位。
 *   2. PCI 复位控制寄存器 0xCF9
 *      —— ICH/PIIX4 起的标准做法，能表达"这是系统复位而不是掉电"。
 *         ★ 但它是**第二条**，原因见下 ★
 *   3. 三重故障（lidt 一个空 IDT 再 int3）
 *      —— 必然复位，但固件只会看到"这个 CPU 崩了"，所以排最后。
 *
 * ★ 顺序为什么是 8042 在前（这是实测改过来的）★
 * 第一版按"越现代越优先"排，把 0xCF9 放第一位。结果在 **QEMU 和 VirtualBox
 * 里症状完全一样**：init 请求重启之后串口再无输出、进程 99% CPU 空转。
 * 用 `-d cpu_reset` 数下来，复位**确实发生了**（普通启动 2 次 CPU Reset，
 * 请求重启后 3 次），但固件再也没回来。
 *
 * 两个彼此独立的模拟器给出同一个症状，那就先怀疑自己的序列：
 * 0xCF9 的 RST_CPU 位（bit 2）是**保持 CPU 在复位态**的意思，
 * 而我们的序列 0x06 → 0x0E 两次都带着它。真实芯片组上系统复位会连带
 * 清掉它，但"复位之后谁来清"这件事在不同实现里并不一致——
 * 请求一次复位却把 CPU 扣在复位态，表现正好就是"复位了，但没人回来"。
 *
 * 教训不是"0xCF9 不能用"，而是**首选应当是副作用最小的那条**：
 * 8042 的 RESET 线只表达"复位"，不附加任何别的要求。 */
#include <fe/reset.h>
#include <fe/io.h>
#include <fe/kprintf.h>

/* 等 8042 输入缓冲空：不等的话后面那条命令会被丢掉。
 * 有界循环——复位路径上**不能**再引入一个可能卡住的地方。 */
static void kbd_wait_input(void)
{
    for (u32 i = 0; i < 100000; i++) {
        if (!(fe_inb(0x64) & 0x02)) {
            return;
        }
    }
}

void fe_machine_reset(void)
{
    fe_kprintf("[复位] 尝试 8042 RESET 线（0x64 <- 0xFE）\n");
    kbd_wait_input();
    fe_outb(0x64, 0xFE);

    /* 走到这里说明 8042 那条没生效 */
    fe_kprintf("[复位] 8042 未生效，尝试 PCI 复位控制寄存器 0xCF9\n");
    kbd_wait_input();
    fe_outb(0xCF9, 0x06);   /* RST_CPU | SYS_RST */
    fe_outb(0xCF9, 0x0E);   /* FULL_RST | RST_CPU | SYS_RST */
    /* 万一芯片组要求"清掉保持位"才放 CPU 出来，这里补一次：
     * 反正前面两条都没成功，多写一次不会更糟。 */
    fe_outb(0xCF9, 0x00);

    fe_kprintf("[复位] 0xCF9 也未生效，改用三重故障（固件会把它看成崩溃）\n");
    {
        /* limit=0、base=0 的 IDT：下一条异常没人接 → 双重故障 → 三重故障 → 复位。 */
        struct { u16 limit; u64 base; } __attribute__((packed)) idtr;
        idtr.limit = 0;
        idtr.base = 0;
        __asm__ __volatile__("lidt %0" :: "m"(idtr));
        __asm__ __volatile__("int3");
    }

    /* 三条都没成功：停在这里，**不要返回**。返回去会让调用者以为重启成功了。 */
    fe_kprintf("[复位] **三条复位路径全部失败**，停机\n");
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}
