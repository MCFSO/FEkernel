/* SPDX-License-Identifier: 0BSD */
/* PCI 总线的接口层协议（见 00-architecture §7 的分层说明）。
 *
 * ★ 为什么这些常量与结构住在接口头里，而不塞进 libfe ★
 * `libfe` 是"系统调用 ABI 的投影"，不含任何设备语义——一旦 PCI 的
 * 寄存器偏移住进去，它就会开始长出"反正 libfe 里有"的跨层调用，
 * 只有两个驱动时看不出问题，到十个驱动时收不回来。
 * 设备协议住在这里（谁要用谁包含），内核完全不知道 PCI 这个词。
 *
 * ★ 这些**不是**某个操作系统的头文件 ★
 * 寄存器偏移、地址字格式、类代码取值全部来自 **PCI 规范**
 * （PCI Local Bus Specification 3.0 §6.1 配置空间头、
 * §3.2.2.3.2 机制 #1 的地址格式）。按规范写与"抄某个内核"是两件事：
 * 前者是遵循公开标准（与遵循 x86_64 的 GDT/页表约定同类），
 * 后者才是本项目铁律禁止的。
 */
#ifndef FE_PCI_H
#define FE_PCI_H

#include <fe_user.h>

/* 机制 #1 的两个端口：先写地址口，再读写数据口。
 * ★ 这两步构成一次操作，所以它们必须被**独占**持有 ★
 * 两个任务交错就会各自改到对方的地址窗口——内核因此把它们
 * 作为独占资源入池（见 kernel/main.c 的说明）。 */
#define FE_PCI_CFG_ADDR_PORT 0xCF8u
#define FE_PCI_CFG_DATA_PORT 0xCFCu
#define FE_PCI_CFG_LEN       8u

/* 配置空间寄存器偏移（DWORD 下标；规范里是字节偏移，这里写字节偏移） */
#define FE_PCI_REG_VENDOR_ID   0x00    /* u16 厂商 ID（0xFFFF = 无设备） */
#define FE_PCI_REG_DEVICE_ID   0x02    /* u16 设备 ID */
#define FE_PCI_REG_COMMAND     0x04    /* u16 命令寄存器（I/O、内存、总线主控使能） */
#define FE_PCI_REG_STATUS      0x06    /* u16 状态寄存器 */
#define FE_PCI_REG_REVISION    0x08    /* u8  版本 */
#define FE_PCI_REG_PROG_IF     0x09    /* u8  编程接口 */
#define FE_PCI_REG_SUBCLASS    0x0A    /* u8  子类 */
#define FE_PCI_REG_CLASS       0x0B    /* u8  类代码 */
#define FE_PCI_REG_HEADER_TYPE 0x0E    /* u8  bit7 = 多功能设备 */
#define FE_PCI_REG_BAR0        0x10    /* 6 个 BAR，每个 4 字节 */
#define FE_PCI_REG_SUBSYS_ID   0x2C
#define FE_PCI_REG_IRQ_LINE   0x3C    /* u8  中断线（写它=选 IRQ） */
#define FE_PCI_REG_IRQ_PIN    0x3D    /* u8  中断引脚（只读：设备接在哪根线上） */

/* 命令寄存器的位（驱动要用设备之前必须使能的两条） */
#define FE_PCI_CMD_IO_SPACE    (1u << 0)    /* 允许响应 I/O 空间访问 */
#define FE_PCI_CMD_MEM_SPACE   (1u << 1)    /* 允许响应内存空间访问 */
#define FE_PCI_CMD_BUS_MASTER  (1u << 2)    /* ★ DMA 的前提：不做这一条设备不能发 DMA ★ */
#define FE_PCI_CMD_INTX_DISABLE (1u << 10)

/* 类代码（规范附录 D）。驱动按它找设备——比"按厂商/设备 ID 硬编码"可移植：
 * 同一个类下面的不同芯片可以用同一个驱动。 */
#define FE_PCI_CLASS_STORAGE   0x01    /* 大容量存储控制器（IDE/SATA/NVMe） */
#define FE_PCI_CLASS_NETWORK   0x02    /* 网络控制器 */
#define FE_PCI_CLASS_DISPLAY   0x03    /* 显示控制器 */
#define FE_PCI_CLASS_MULTIMEDIA 0x04
#define FE_PCI_CLASS_BRIDGE    0x06    /* 桥（主机桥、ISA 桥、PCI-to-PCI） */
#define FE_PCI_CLASS_SERIAL    0x0C    /* 串行总线（USB、SMBus…） */

/* 常见子类（够今天的用途） */
#define FE_PCI_SUBCLASS_IDE     0x01    /* IDE 控制器 */
#define FE_PCI_SUBCLASS_SATA    0x06
#define FE_PCI_SUBCLASS_ETHERNET 0x00
#define FE_PCI_SUBCLASS_VGA     0x00
#define FE_PCI_SUBCLASS_OTHER_BRIDGE 0x80

/* 一个 BAR 的解析结果。
 * ★ I/O 与内存空间的标志位不同，所以类型必须显式 ★
 * 低两位（I/O 空间）或低四位（内存空间）是标志，不是地址的一部分；
 * 不剥掉它们就会得到一个"地址看起来差几位"的结论，
 * 而按它去认领资源池必然失败（池里是对齐后的区间）。 */
#define FE_PCI_BAR_NONE 0
#define FE_PCI_BAR_IO   1
#define FE_PCI_BAR_MEM  2

static inline void fe_pci_addr_write(u32 addr)
{
    /* 地址字：bit31 使能 | bit23..16 总线 | bit15..11 设备 |
     *         bit10..8 功能 | bit7..2 寄存器（低两位恒 0） */
    fe_outl(FE_PCI_CFG_ADDR_PORT, addr);
}

static inline u32 fe_pci_read32(u32 addr)
{
    fe_pci_addr_write(addr);
    return fe_inl(FE_PCI_CFG_DATA_PORT);
}

static inline void fe_pci_write32(u32 addr, u32 value)
{
    fe_pci_addr_write(addr);
    fe_outl(FE_PCI_CFG_DATA_PORT, value);
}

static inline u32 fe_pci_make_addr(u8 bus, u8 dev, u8 func, u8 reg)
{
    return 0x80000000u
         | ((u32)bus << 16)
         | ((u32)(dev & 0x1Fu) << 11)
         | ((u32)(func & 7u) << 8)
         | ((u32)reg & 0xFCu);
}

static inline u32 fe_pci_cfg_read32(u8 bus, u8 dev, u8 func, u8 reg)
{
    return fe_pci_read32(fe_pci_make_addr(bus, dev, func, reg));
}

static inline void fe_pci_cfg_write32(u8 bus, u8 dev, u8 func, u8 reg, u32 value)
{
    fe_pci_write32(fe_pci_make_addr(bus, dev, func, reg), value);
}

/* 读 16 位字段（配置空间里 u16 在低半还是高半由寄存器偏移决定） */
static inline u16 fe_pci_cfg_read16(u8 bus, u8 dev, u8 func, u8 reg)
{
    u32 v = fe_pci_cfg_read32(bus, dev, func, (u8)(reg & 0xFC));
    return (u16)((reg & 2) ? (v >> 16) : (v & 0xFFFF));
}

static inline u32 fe_pci_vendor_id(u8 bus, u8 dev, u8 func)
{
    return fe_pci_cfg_read32(bus, dev, func, FE_PCI_REG_VENDOR_ID) & 0xFFFFu;
}

/* "这个位置有设备吗"——0xFFFF 表示没有。
 * 这是枚举时**唯一**该用的判据：返回 0 也可能是合法厂商（虽然罕见），
 * 而 0xFFFF 是规范规定的"无设备"哨兵值。 */
static inline int fe_pci_present(u8 bus, u8 dev, u8 func)
{
    u32 v = fe_pci_vendor_id(bus, dev, func);
    return (v != 0xFFFFu);
}

/* 读一个 BAR 并算出它声明的区间长度。
 *
 * ★ 这是**规范给的做法**，不是某个内核的取巧 ★
 * PCI 规范规定基址寄存器是"部分可写"的：把全 1 写进去，设备只保留它
 * 需要解码的地址位，读回来就是"这些位是有效的"。于是：
 *     长度 = ~(读回值) + 1        （对 I/O BAR 再屏蔽低 2 位）
 * 例：读回 0xFFFFFFE0 → ~0xFFFFFFE0 + 1 = 0x20 = 32 字节。
 *
 * ★ 为什么必须"写回原值" ★
 * 不写回的话，BAR 在探测之后就变成了全 1 —— 设备会把地址窗口搬到
 * 荒谬的位置上（而这一条**不会报错**，症状是"驱动读寄存器读到全 1"）。
 * 所以这个函数末尾一定有一次恢复写入，哪怕后面发现 BAR 无效。
 *
 * out_type：FE_PCI_BAR_IO / MEM / NONE。返回 0 表示这个槽位是空的。 */
static inline u64 fe_pci_bar_probe(u8 bus, u8 dev, u8 func, u32 bar_index,
                                   u32 *out_type)
{
    if (out_type) {
        *out_type = FE_PCI_BAR_NONE;
    }
    if (bar_index > 5) {
        return 0;
    }
    u8 reg = (u8)(0x10 + bar_index * 4);
    u32 orig = fe_pci_cfg_read32(bus, dev, func, reg);
    if ((orig & 0xFFFFu) == 0) {
        return 0;                       /* 低 16 位全 0：这个槽位没实现 */
    }
    fe_pci_cfg_write32(bus, dev, func, reg, 0xFFFFFFFFu);
    u32 probed = fe_pci_cfg_read32(bus, dev, func, reg);
    fe_pci_cfg_write32(bus, dev, func, reg, orig);     /* ★ 必须恢复 ★ */

    if (probed == 0 || probed == 0xFFFFFFFFu) {
        return 0;                       /* 不可写 = 该槽位没实现 */
    }
    if (probed & 0x1u) {
        /* I/O 空间：bit1 恒为 0，地址在 bit2..31 */
        u32 mask = probed & 0xFFFFFFFCu;
        u64 len = (u64)(~mask) + 1;
        if (len & 0xFFFF0000ull) {
            len &= 0xFFFFull;           /* 端口空间只有 16 位 */
        }
        if (out_type) {
            *out_type = FE_PCI_BAR_IO;
        }
        return len;
    }
    /* 内存空间：低 4 位是类型/预取标志，地址在 bit4..31 */
    u32 type = probed & 0x7u;
    if (type == 0x4u) {
        /* 64 位 BAR 占两个槽位：低 32 位在 bar_index，高 32 位在下一个。
         * ★ 本函数只处理低 32 位 ★ —— 2^32 以下的内存空间对今天的设备
         * （IDE、virtio、AHCI）都够用；返回长度也要如实反映这一点。 */
        if (out_type) {
            *out_type = FE_PCI_BAR_MEM;
        }
    } else if (out_type) {
        *out_type = FE_PCI_BAR_MEM;
    }
    u32 mask = probed & 0xFFFFFFF0u;
    return (u64)(~mask) + 1;
}

#endif /* FE_PCI_H */
