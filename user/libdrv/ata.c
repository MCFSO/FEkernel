/* SPDX-License-Identifier: 0BSD */
/* ATA（PIO 兼容 IDE）通道访问层 —— libdrv 的一部分，由 blkd 使用。
 *
 * ★ 这一层为什么存在（而不是继续写在 blkd 里）★
 *
 * 升级到总线主控 DMA 之后，"一次传输"这句话的含义变多了：
 * 它可能是一次 PIO 轮询、也可能是一次 DMA（要配 PRDT、要启停引擎、
 * 要读 BMDMA 状态寄存器确认设备真的搬完了）。这些细节**与块设备服务
 * 要解决的问题无关**——blkd 该管的是"哪个 LBA、多少扇区、给谁"。
 *
 * 所以边界划在这里：
 *   ata.c  —— "怎么把 N 个扇区搬进这段内存"（含 DMA 引擎的时序）
 *   blkd   —— IPC 协议、写保护查询、请求排队、把数据交给客户端
 *
 * ★ 本文件里没有任何"内核"★
 * 端口来自 fe_ioport_request 的认领，DMA 内存来自 fe_mem_alloc_dma（物理连续），
 * 总线主控端口来自 PCI BAR（由设备管理器申报进资源池）。用的是哪一段
 * 端口、哪一块物理内存，全部是**用户态自己查出来的**——内核不知道有磁盘。
 *
 * ★ 为什么所有等待都用 fe_clock_ns 而不是循环次数 ★
 * 与 ps2.c 同一条理由：虚拟机与真机的速度差一个数量级，
 * 按次数写死会在一边过于宽松、在另一边误报超时。
 */
#include <fe_drv.h>
#include "ata_internal.h"

/* 超时。单次传输的预算是"设备搬完 N 个扇区"，按扇区数放大；
 * 500 ms 的底数是为了覆盖最慢的 PIO 模式与虚拟机的调度抖动。 */
#define ATA_TIMEOUT_BASE_NS 500000000ull
#define ATA_TIMEOUT_PER_SECTOR_NS 2000000ull

static u64 transfer_timeout_ns(u32 count)
{
    return ATA_TIMEOUT_BASE_NS + (u64)count * ATA_TIMEOUT_PER_SECTOR_NS;
}

/* ------------------------------------------------------------------ */
/* 状态等待                                                            */
/* ------------------------------------------------------------------ */

/* 等 BSY 清。**必须走这条路径再碰任何寄存器**：BSY 为 1 时读到的其它
 * 寄存器内容是未定义的（规范原文），"先读一下看看"在这里是错的。 */
static int wait_not_busy(struct fe_ata_channel *ch)
{
    u64 t0 = fe_clock_ns();
    while (fe_inb(ch->cmd_base + ATA_R_STATUS) & ATA_ST_BSY) {
        if (fe_clock_ns() - t0 > ATA_TIMEOUT_BASE_NS) {
            return FE_ATA_ERR_TIMEOUT;
        }
    }
    return FE_ATA_OK;
}

/* 等 BSY 清且 DRQ 置（数据已就绪）。
 *
 * ★ 这里为什么要**重试**而不是"看一眼就下结论" ★
 * 实测（QEMU 的 IDE 盘）：多扇区读发出后，状态寄存器会先出现
 * `0x50 = DRDY | DSC` —— DSC（bit5，寻道完成）**正在变**的那一刻被读到，
 * 它既不是 DRQ 也不是错误，只是"设备还在忙自己的事"。
 * 第一版看到 DRQ 没置就返回失败，于是**每一次多扇区读都在第一个扇区失败**，
 * 而单扇区读碰巧躲过了这个窗口（这解释了当时"一个扇区能读、八个扇区读不了"
 * 这个看起来毫无道理的现象）。
 *
 * 正确做法与 Linux 的 `_ide_wait` 一致：**忽略 DSC**，继续等到
 * "BSY 清且（DRQ 置 或 出错）"为止；只有超时才是真失败。
 * 也就是说"设备暂时没准备好"必须与"设备说不行了"分开——
 * 前者要等，后者要报错，把它们混成一个返回值是这一处 bug 的根。 */
static int wait_drq(struct fe_ata_channel *ch)
{
    u64 t0 = fe_clock_ns();
    for (;;) {
        u8 st = fe_inb(ch->cmd_base + ATA_R_STATUS);
        if (!(st & ATA_ST_BSY)) {
            if (st & (ATA_ST_ERR | ATA_ST_DF)) {
                return FE_ATA_ERR_DEVICE;
            }
            if (st & ATA_ST_DRQ) {
                return FE_ATA_OK;
            }
            /* 没有 DRQ，也没有错误 → 设备还在准备（DSC 可能正在跳变）。
             * 继续等，而不是把它当成"设备拒绝了"。 */
        }
        if (fe_clock_ns() - t0 > ATA_TIMEOUT_BASE_NS) {
            return FE_ATA_ERR_TIMEOUT;
        }
    }
}

/* 400 ns 的"伪读"：写命令寄存器之后，状态寄存器要过一小段时间才反映
 * 新命令。规范给的做法是连读 4 次状态口（每次 I/O 访问本身就有延迟），
 * 而不是真的去睡——真的睡会把每次传输都拖慢至少一个调度周期。 */
static u8 status_after_command(struct fe_ata_channel *ch)
{
    u8 st = 0;
    for (u32 i = 0; i < 4; i++) {
        st = fe_inb(ch->cmd_base + ATA_R_STATUS);
    }
    return st;
}

/* ------------------------------------------------------------------ */
/* 总线主控（BMDMA）引擎                                              */
/* ------------------------------------------------------------------ */

/* PRDT 表项：4 字节物理地址 + 4 字节字节数（bit31 = 最后一项）。
 * ★ 字节数只有 16 位、且**必须偶数** ★：所以单个表项最多 64 KiB，
 * 而且不能是 65536（那个值没法用 16 位表达，只能靠 bit31 收尾）。
 * 这一条决定了 DMA 缓冲的长度上限与"能不能跨界"的约束，见 fe_ata_dma_usable。 */
struct prd_entry {
    u32 phys;
    u32 count;
};

static void prd_fill(struct prd_entry *tbl, u64 phys, u32 len)
{
    tbl[0].phys = (u32)phys;
    tbl[0].count = (len & 0xFFFFu) | ATA_PRD_EOT;
}

/* 一次 DMA 命令搬一**段**（定义在后面，这里先声明：enable/verify 要用它）。 */
static int read_dma_segment(struct fe_ata_channel *ch, u32 lba, u32 count,
                            u64 base_phys, u64 tot_len,
                            void *desc_virt, u64 desc_phys);

/* ------------------------------------------------------------------ */
/* 公开接口                                                            */
/* ------------------------------------------------------------------ */

u32 fe_ata_sector_count(const struct fe_ata_channel *ch)
{
    return ch ? ch->sector_count : 0;
}

/* 单次命令能覆盖的扇区数上限。
 *
 * ★ 三个约束叠在一起，取最小 ★
 *   1. 命令块的扇区计数字段只有 8 位 → 一条命令最多 256 个扇区（128 KiB，
 *      写 256 时寄存器值是 0，这是 ATA 的历史包袱）；
 *   2. PRDT 表项长度只有 16 位且必须偶数 → 单个表项最多 64 KiB；
 *   3. 本项目给 DMA 缓冲的约定是**一个缓冲、一条命令**（见下）。
 * 于是"一次能搬多少"由缓冲决定：只要缓冲满足 fe_ata_dma_usable()，
 * 一条命令最多能搬 buf_len/512 个扇区，上限 256。 */
u32 fe_ata_max_sectors(const struct fe_ata_channel *ch)
{
    (void)ch;
    return 256;
}

bool fe_ata_dma_usable(const struct fe_ata_channel *ch, u64 phys, u64 len)
{
    if (!ch || !ch->dma) {
        return false;
    }
    if (len == 0 || (len & 1) != 0) {
        return false;               /* PRDT 长度字段必须是偶数 */
    }
    if (len > 0x10000ull) {
        return false;               /* 单个 PRDT 表项装不下 64 KiB 以上 */
    }
    /* ★ 不跨 64 KiB 边界 ★
     * 设备侧的地址加法在描述符粒度内不处理进位，跨界是**未定义行为**。
     * 这条限制不是我们的选择，是 ATA BMDMA 的既有约定；
     * 违反它的症状是"偶尔搬到相邻内存"，属于最难查的一类。 */
    if ((phys & ~0xFFFFull) != ((phys + len - 1) & ~0xFFFFull)) {
        return false;
    }
    return true;
}

/* 让设备进入 DMA 模式。
 *
 * ★ 这一版**不再把 IDENTIFY 的 DMA 掩码当作判据**，而是真的试一次 ★
 *
 * 起因是实测：QEMU 的 IDE 盘 IDENTIFY 报 w88=0x203F（UDMA 与多字 DMA
 * 的支持位都是 0），按"掩码为 0 就不支持 DMA"的判据，驱动直接放弃 DMA。
 * 但那只是**设备自我声明**，而"能不能 DMA"是**主机侧的总线主控行为**：
 * BMDMA 引擎在 PIIX 里，与设备怎么自称不是一回事。
 *
 * 所以这里只负责"把设备切到 DMA 模式"（发 SET FEATURES），
 * **能不能用由 fe_ata_verify_dma 用一次真实传输来判定**——
 * 把"支持与否"从**声明**变成**事实**。
 *
 * 掩码为 0 时也发一条最保守的（多字 DMA0）：这正是上面那种
 * "自称与事实不一致"的情况。 */
int fe_ata_enable_dma(struct fe_ata_channel *ch)
{
    if (!ch || !ch->dma) {
        return FE_ATA_ERR_NODMA;
    }
    int r = wait_not_busy(ch);
    if (r != FE_ATA_OK) {
        return r;
    }

    u8 mode = 0x20;                 /* 默认：多字 DMA0（最保守的 DMA 档） */
    if (ch->udma_mask) {
        u8 m = 0;
        for (u8 i = 0; i < 7; i++) {
            if (ch->udma_mask & (1u << i)) {
                m = i;              /* 取支持的最高档 */
            }
        }
        mode = (u8)(0x40u | m);     /* 0x40 = UDMA0，依次往上 */
    } else if (ch->mwdma_mask) {
        u8 m = 0;
        for (u8 i = 0; i < 3; i++) {
            if (ch->mwdma_mask & (1u << i)) {
                m = i;
            }
        }
        mode = (u8)(0x20u | m);     /* 0x20 = 多字 DMA0 */
    }

    fe_outb(ch->cmd_base + ATA_R_FEATURE, ATA_FEAT_TRANSFER_MODE);
    fe_outb(ch->cmd_base + ATA_R_SECCNT, mode);
    fe_outb(ch->cmd_base + ATA_R_COMMAND, ATA_CMD_SET_FEATURES);
    u8 st = status_after_command(ch);
    if (wait_not_busy(ch) != FE_ATA_OK) {
        return FE_ATA_ERR_TIMEOUT;
    }
    if (st & (ATA_ST_ERR | ATA_ST_DF)) {
        return FE_ATA_ERR_DEVICE;
    }
    ch->active_mode = mode;
    ch->dma_enabled = true;
    return FE_ATA_OK;
}

/* 软复位通道（设备控制寄存器 bit2 = SRST）。
 *
 * ★ 为什么需要它 ★
 * 一条**被拒绝**的命令（ERR 置位、ABRT）会把设备留在错误状态里，
 * 而 ERR 位是**粘滞**的：此后每条新命令返回时它还在，于是
 * wait_drq/wait_not_busy 一律判成失败——症状是"探完 DMA 之后再也不能读盘"。
 * 实测踩到的正是这个：SET FEATURES 被拒（这台设备不支持 DMA），
 * 紧接着的 PIO 批量读在第一块就失败。
 *
 * 复位时序按规范：SRST 置 1 → 等 ≥5us → 清 0 → 等 BSY 落下（最多 30s，
 * 我们给 500ms 的预算，超时如实报错）。 */
int fe_ata_soft_reset(struct fe_ata_channel *ch)
{
    if (!ch) {
        return FE_ATA_ERR_INVAL;
    }
    fe_outb(ch->ctrl_base + ATA_C_DEVCTL, 0x04);    /* SRST = 1 */
    /* 规范要求 ≥5us。上界用 400ns 的伪读不合适，这里直接读 8 次状态口
     * （每次 I/O 访问都有微秒级延迟），比空转循环更可移植。 */
    for (u32 i = 0; i < 8; i++) {
        (void)fe_inb(ch->cmd_base + ATA_R_STATUS);
    }
    fe_outb(ch->ctrl_base + ATA_C_DEVCTL, 0x00);    /* SRST = 0，中断使能 */
    if (wait_not_busy(ch) != FE_ATA_OK) {
        return FE_ATA_ERR_TIMEOUT;
    }
    /* 复位后设备的状态位（ERR/DF）应当已经清掉；不清就是真有问题 */
    u8 st = fe_inb(ch->cmd_base + ATA_R_STATUS);
    if (st & (ATA_ST_ERR | ATA_ST_DF)) {
        return FE_ATA_ERR_DEVICE;
    }
    /* 复位会把传输模式打回上电默认（PIO），所以这里如实清掉标志——
     * 否则调用方会以为设备仍在 DMA 模式。 */
    ch->dma_enabled = false;
    ch->active_mode = 0;
    return FE_ATA_OK;
}

/* 用**一次真实传输**验证 DMA 是否真的工作。
 *
 * 调用者给两块内存：pio_buf（PIO 读到这儿做基准）与 dma_buf/dma_phys + desc
 * （DMA 读到那儿）。两者内容全等才算 DMA 真的可用。
 *
 * ★ 为什么必须比对，而不能"enable_dma 没报错就算数" ★
 * SET FEATURES 会被接受（不报错），但设备可能仍按 PIO 响应；那时 DMA 命令
 * （0xC8）会"成功完成"而**一个字节都不搬**——缓冲区保持原样，于是批量读把
 * **旧内容**当成读到的数据交回客户端。这种错误不崩、不报错，
 * 只静默地给出错误数据。
 * 所以调用者要把目标缓冲**预先写成一个不可能的值**，再由本函数比对：
 * "没搬"与"搬对了"才区分得开。 */
int fe_ata_verify_dma(struct fe_ata_channel *ch, u32 lba, u32 count,
                      u8 *pio_buf, u8 *dma_buf, u64 dma_phys,
                      void *desc_virt, u64 desc_phys)
{
    if (!ch || !ch->dma_enabled || !pio_buf || !dma_buf) {
        return FE_ATA_ERR_NODMA;
    }
    if (fe_ata_read_pio_sectors(ch, lba, count, pio_buf) != count) {
        return FE_ATA_ERR_DEVICE;
    }
    int r = read_dma_segment(ch, lba, count, dma_phys,
                             (u64)count * FE_ATA_SECTOR_SIZE, desc_virt, desc_phys);
    if (r != FE_ATA_OK) {
        return r;
    }
    for (u32 i = 0; i < count * FE_ATA_SECTOR_SIZE; i++) {
        if (pio_buf[i] != dma_buf[i]) {
            /* 不相等 = "没搬"或"搬错了"。两种都必须退回 PIO：
             * 前者会给出旧数据，后者会给出错误数据。 */
            return FE_ATA_ERR_DMA_MISMATCH;
        }
    }
    return FE_ATA_OK;
}

/* 从 IDENTIFY 的 512 字节里取一个字。 */
static u16 id_word(const u8 *id, u32 idx)
{
    return (u16)(id[idx * 2] | ((u16)id[idx * 2 + 1] << 8));
}

/* 发 IDENTIFY 并读回 512 字节（用 PIO：这是初始化路径，每次启动只走一次，
 * 不值得为它写一套 DMA）。 */
static int ata_identify_words(struct fe_ata_channel *ch, u8 *out512, bool *present)
{
    *present = false;
    if (wait_not_busy(ch) != FE_ATA_OK) {
        return FE_ATA_ERR_TIMEOUT;
    }
    fe_outb(ch->cmd_base + ATA_R_DRIVE, (u8)(0xA0 | (ch->slave ? 0x10 : 0) |
                                             (ch->lba_high << 4)));
    fe_outb(ch->cmd_base + ATA_R_SECCNT, 0);
    fe_outb(ch->cmd_base + ATA_R_LBA0, 0);
    fe_outb(ch->cmd_base + ATA_R_LBA1, 0);
    fe_outb(ch->cmd_base + ATA_R_LBA2, 0);
    fe_outb(ch->cmd_base + ATA_R_COMMAND, ATA_CMD_IDENTIFY);

    u8 st = status_after_command(ch);
    if (st == 0) {
        return FE_ATA_ERR_NO_DEVICE;    /* 端口上什么都没有：读回 0 */
    }
    if (wait_not_busy(ch) != FE_ATA_OK) {
        return FE_ATA_ERR_TIMEOUT;
    }
    /* 有设备但不响应 IDENTIFY（ATAPI 包设备就是这种）时 LBA1/LBA2 非 0 */
    if (fe_inb(ch->cmd_base + ATA_R_LBA1) != 0 ||
        fe_inb(ch->cmd_base + ATA_R_LBA2) != 0) {
        return FE_ATA_ERR_NO_DEVICE;
    }
    int r = wait_drq(ch);
    if (r != FE_ATA_OK) {
        return r;
    }
    for (u32 i = 0; i < 256; i++) {
        u16 w = fe_inw(ch->cmd_base + ATA_R_DATA);
        out512[i * 2] = (u8)(w & 0xFF);
        out512[i * 2 + 1] = (u8)(w >> 8);
    }
    *present = true;
    return FE_ATA_OK;
}

int fe_ata_init(struct fe_ata_channel *ch, u16 cmd_base, u16 ctrl_base,
                u16 bm_base, bool bm_primary, bool slave)
{
    if (!ch || cmd_base == 0 || ctrl_base == 0) {
        return FE_ATA_ERR_INVAL;
    }
    for (u32 i = 0; i < sizeof(*ch); i++) {
        ((u8 *)ch)[i] = 0;
    }
    ch->cmd_base = cmd_base;
    ch->ctrl_base = ctrl_base;
    ch->bm_base = bm_base;
    ch->bm_primary = bm_primary;
    ch->slave = slave;
    ch->lba_high = 0;
    ch->sector_count = 0;
    ch->dma = (bm_base != 0);

    /* 设备控制寄存器写 0：保持中断使能（nIEN = 0）。
     * 我们要的是"设备搬完了"这个事实，而它只有 BM_STATUS 的 INTR 位
     * 能可靠回答——所以中断不该被关掉（即使这一层不去等它）。 */
    fe_outb(ch->ctrl_base + ATA_C_DEVCTL, 0);

    u8 id[512];
    bool present = false;
    int r = ata_identify_words(ch, id, &present);
    if (r != FE_ATA_OK) {
        return r;
    }
    if (!present) {
        return FE_ATA_ERR_NO_DEVICE;
    }

    /* 型号（字 27..46）：ATA 的字符串是**字交换**的，所以要按字序拼 */
    for (u32 i = 0; i < 20; i++) {
        u16 w = id_word(id, 27 + i);
        ch->model[i * 2] = (char)(w >> 8);
        ch->model[i * 2 + 1] = (char)(w & 0xFF);
    }
    for (u32 i = 39; i > 0 && ch->model[i] == ' '; i--) {
        ch->model[i] = '\0';
    }
    ch->model[40] = '\0';

    /* 容量。
     *
     * ★ 为什么**优先用 LBA28**（字 60..61）而不是 LBA48（字 100..103）★
     * 第一版把 LBA48 放在前面，结果在 QEMU 上报出 268435455 扇区
     * （= 128 GiB，正好是 2^28-1）——那块盘实际只有 49 MiB。
     * 原因是 LBA48 字段在这块盘上并不为 0，而按 LBA48 读出来的值
     * 与"按 LBA28 命令集实际能寻址的范围"不是一回事。
     *
     * 本驱动全程用 **LBA28 命令**（在 64 KiB/s 以下的路径上这不成问题），
     * 所以容量应当报"LBA28 能寻址的范围"：读字 60..61。
     * 只有它恰好为 0（少见，但规范允许）时才回落到 LBA48，
     * 并按 28 位夹紧——**夹紧而不是截断**：截断会让容量看起来变小，
     * 夹紧保证"不超出命令集能表达的范围"。 */
    ch->sector_count = (u32)id_word(id, 60) | ((u32)id_word(id, 61) << 16);
    if (ch->sector_count == 0) {
        u32 hi = (u32)id_word(id, 100) | ((u32)id_word(id, 101) << 16);
        u32 lo = (u32)id_word(id, 102) | ((u32)id_word(id, 103) << 16);
        u64 cap = ((u64)hi << 32) | lo;
        ch->sector_count = (u32)(cap > 0x0FFFFFFFull ? 0x0FFFFFFFull : cap);
    }

    /* ★ IDENTIFY 报的"支持哪些 DMA 模式"是**能力**，不是当前模式 ★
     * 字 88 的 bit0..6 = UDMA0..6 支持位、bit8..10 = 多字 DMA0..2，
     * bit14 = 字 88 有效；字 63 的 bit0..2 = 多字 DMA 支持位。 */
    u16 w88 = id_word(id, 88);
    u16 w63 = id_word(id, 63);

    if (w88 & 0x4000) {
        ch->udma_mask = (u8)(w88 & 0x7F);
        ch->mwdma_mask = (u8)((w88 >> 8) & 0x07);
    } else if (w63 & 0x0400) {
        ch->mwdma_mask = (u8)(w63 & 0x07);
    }
    return FE_ATA_OK;
}

/* 把 LBA 与扇区数写进命令块（LBA28）。 */
static void set_lba_count(struct fe_ata_channel *ch, u32 lba, u32 count)
{
    fe_outb(ch->cmd_base + ATA_R_DRIVE, (u8)(0xE0 | (ch->slave ? 0x10 : 0) |
                                             ((lba >> 24) & 0x0F)));
    fe_outb(ch->cmd_base + ATA_R_SECCNT, (u8)(count == 256 ? 0 : count));
    fe_outb(ch->cmd_base + ATA_R_LBA0, (u8)(lba & 0xFF));
    fe_outb(ch->cmd_base + ATA_R_LBA1, (u8)((lba >> 8) & 0xFF));
    fe_outb(ch->cmd_base + ATA_R_LBA2, (u8)((lba >> 16) & 0xFF));
}

/* PIO 读：一条命令读多个扇区，数据按 16 位从数据口取。
 * 返回实际搬完的扇区数（出错时是出错前已经搬完的扇区数）。 */
/* 最近一次 PIO 失败的现场（诊断用；读它的人只需要知道"设备当时怎么说的"） */
static u8 g_last_status;
static u8 g_last_error;

u8 fe_ata_last_status(void)
{
    return g_last_status;
}

u8 fe_ata_last_error(void)
{
    return g_last_error;
}

u32 fe_ata_read_pio_sectors(struct fe_ata_channel *ch, u32 lba, u32 count, u8 *out)
{
    if (!ch || !out || count == 0 || count > fe_ata_max_sectors(ch)) {
        return 0;
    }
    if ((u64)lba + count > ch->sector_count) {
        return 0;
    }
    u32 done = 0;
    set_lba_count(ch, lba, count);
    fe_outb(ch->cmd_base + ATA_R_COMMAND, ATA_CMD_READ_PIO);
    u8 st = status_after_command(ch);

    u8 first = status_after_command(ch);
    for (u32 s = 0; s < count; s++) {
        if (st & (ATA_ST_ERR | ATA_ST_DF)) {
            break;
        }
        if (wait_drq(ch) != FE_ATA_OK) {
            break;
        }
        u16 *w = (u16 *)(void *)(out + (usize)s * FE_ATA_SECTOR_SIZE);
        for (u32 i = 0; i < FE_ATA_SECTOR_SIZE / 2; i++) {
            w[i] = fe_inw(ch->cmd_base + ATA_R_DATA);
        }
        done++;
        st = fe_inb(ch->cmd_base + ATA_R_STATUS);
    }
    g_last_status = st;
    g_last_error = fe_inb(ch->cmd_base + ATA_R_ERROR);
    (void)first;
    return done;
}

/* DMA 读（单表项版本已被多表项替换，见 fe_ata_read_dma_segment）。
 *
 * ★ 完成判定为什么读 BM_STATUS 而不是等中断 ★
 * 两者回答的是同一个问题（"设备搬完了吗"），但：
 *   - 中断是**异步**的，而这一层是同步接口（调用者要的是"读完了"）；
 *   - BM_STATUS.INTR 由 DMA 引擎在传输结束时置位，与中断投递走的是
 *     两个不同的路径（中断可能被虚拟化层限流，状态位不会）；
 *   - VBox 的中断投递实测只有 49~169 Hz，靠中断做同步会把每次传输
 *     都拖到 6~20 ms。这是实测数据，不是猜测（见 docs/12 §9）。
 * blkd 仍然认领并使用 IRQ14：中断被投递到通知对象，于是"设备完成了"
 * 这件事同时能从两条路看到——状态位是**同步**的判据，中断是**可选**的加速。 */

static int read_dma_segment(struct fe_ata_channel *ch, u32 lba, u32 count,
                            u64 base_phys, u64 tot_len,
                            void *desc_virt, u64 desc_phys);

static int read_dma_segment(struct fe_ata_channel *ch, u32 lba, u32 count,
                            u64 base_phys, u64 tot_len,
                            void *desc_virt, u64 desc_phys);

int fe_ata_read(struct fe_ata_channel *ch, u32 lba, u32 count, u8 *buf, bool dma,
                u64 buf_phys, u64 desc_phys, u32 *out_done)
{
    /* 单表项版本的便利包装：把描述符表的物理地址当作它的虚拟地址用
     * （只在内核/映射了一一对应的场合成立）。完整接口见 fe_ata_read_ex。 */
    return fe_ata_read_ex(ch, lba, count, buf, dma, buf_phys, desc_phys,
                          (void *)(usize)desc_phys, out_done);
}

int fe_ata_read_ex(struct fe_ata_channel *ch, u32 lba, u32 count, u8 *buf, bool dma,
                   u64 buf_phys, u64 desc_phys, void *desc_virt, u32 *out_done)
{
    if (out_done) {
        *out_done = 0;
    }
    (void)buf;
    if (!ch || count == 0) {
        return FE_ATA_ERR_INVAL;
    }
    if (count > fe_ata_max_sectors(ch)) {
        return FE_ATA_ERR_RANGE;
    }
    if ((u64)lba + count > ch->sector_count) {
        return FE_ATA_ERR_RANGE;
    }

    if (dma) {
        if (!ch->dma_enabled) {
            return FE_ATA_ERR_NODMA;
        }
        /* 表由 read_dma_segment 自己按需建（可能用多个表项），
         * 所以这里不需要预先填 —— 传进来的 desc_virt 只是"表放在哪"。
         * ★ desc_virt 必须是**调用者地址空间里已映射**的地址 ★
         * 描述符表也要由 CPU 写，而 CPU 用的是虚拟地址：
         * 设备用 desc_phys 读表，我们用 desc_virt 写表，两者指向同一块内存。 */
        int r = read_dma_segment(ch, lba, count, buf_phys,
                                 (u64)count * FE_ATA_SECTOR_SIZE,
                                 desc_virt, desc_phys);
        if (out_done && r == FE_ATA_OK) {
            *out_done = count;
        }
        return r;
    }

    u32 done = fe_ata_read_pio_sectors(ch, lba, count, buf);
    if (out_done) {
        *out_done = done;
    }
    return done == count ? FE_ATA_OK : FE_ATA_ERR_DEVICE;
}

/* 写一批扇区（PIO）。**故意不在这里发 FLUSH CACHE**：
 * 刷新的意义是"在某个提交点之前写的东西必须真的在盘上"，而那个提交点
 * 只有调用方知道（A/B 更新里它是"翻引导控制块之前"）。第一版每个扇区
 * 后面都跟一条 FLUSH，结果是 2717 个扇区的拷贝用了 14.3 秒——每次 FLUSH
 * 都是一次真实的落盘屏障，在虚拟化下就是一次 fsync，慢到像是卡死。
 * 所以这里只负责把数据交给驱动器，落盘由 fe_ata_flush 显式承担。 */
u32 fe_ata_write_pio_sectors(struct fe_ata_channel *ch, u32 lba, u32 count,
                             const u8 *in)
{
    if (!ch || !in || count == 0 || count > fe_ata_max_sectors(ch)) {
        return 0;
    }
    if ((u64)lba + count > ch->sector_count) {
        return 0;
    }
    u32 done = 0;
    set_lba_count(ch, lba, count);
    fe_outb(ch->cmd_base + ATA_R_COMMAND, ATA_CMD_WRITE_PIO);
    u8 st = status_after_command(ch);

    for (u32 s = 0; s < count; s++) {
        if (st & (ATA_ST_ERR | ATA_ST_DF)) {
            break;
        }
        if (wait_drq(ch) != FE_ATA_OK) {
            break;
        }
        const u16 *w = (const u16 *)(const void *)(in + (usize)s * FE_ATA_SECTOR_SIZE);
        for (u32 i = 0; i < FE_ATA_SECTOR_SIZE / 2; i++) {
            fe_outw(ch->cmd_base + ATA_R_DATA, w[i]);
        }
        done++;
        /* 等这一条命令真正完成（BSY 落下、无 ERR）：不等的话调用方
         * 会在设备还在写的时候继续下一步，出错也报不出来。 */
        if (wait_not_busy(ch) != FE_ATA_OK) {
            break;
        }
        st = fe_inb(ch->cmd_base + ATA_R_STATUS);
    }
    return done;
}

/* 刷写缓存。0 = 有屏障保证；1 = 设备不实现 FLUSH（数据已交给驱动器，
 * 但没有落盘保证）；负值 = 失败。
 *
 * ★ 为什么"设备不支持"要单独一个返回值，而不是并入失败 ★
 * 报成"写失败"会诱导调用者重试整个拷贝，而重试并不能让一个不支持
 * flush 的盘变得支持。诚实的做法是告诉调用者"没有屏障"，由它决定
 * 要不要接受——A/B 更新那种场景就不能接受，一次普通写就可以。 */
int fe_ata_flush(struct fe_ata_channel *ch)
{
    if (!ch) {
        return FE_ATA_ERR_INVAL;
    }
    if (wait_not_busy(ch) != FE_ATA_OK) {
        return FE_ATA_ERR_TIMEOUT;
    }
    fe_outb(ch->cmd_base + ATA_R_COMMAND, ATA_CMD_FLUSH_CACHE);
    (void)status_after_command(ch);
    if (wait_not_busy(ch) != FE_ATA_OK) {
        return FE_ATA_ERR_TIMEOUT;
    }
    if (fe_inb(ch->cmd_base + ATA_R_STATUS) & (ATA_ST_ERR | ATA_ST_DF)) {
        return 1;
    }
    return 0;
}

/* 往一块 DMA 内存里填"一个表项"的 PRDT。
 * 单独暴露出来是因为"表在哪"是调用者的事（只有它知道哪块内存有物理地址）。 */
void fe_ata_fill_prd(void *tbl, u64 phys, u32 len)
{
    prd_fill((struct prd_entry *)tbl, phys, len);
}

/* ---- 多表项的 PRDT：一次命令搬完一整段，不受 64 KiB 边界限制 ----
 *
 * ★ 为什么需要它 ★
 * 单表项的最大长度是 64 KiB，而"不跨 64 KiB 边界"是设备的约束。
 * 于是单表项方案下，"搬 256 个扇区"这件事在缓冲起点靠近边界时
 * **做不到**——不是驱动不想做，是描述符表达不了。
 *
 * 规范给的解法就是 PRDT 本来就有的能力：**多个表项**。
 * 于是把"一段内存"切成若干个互不跨界、每个 ≤ 64 KiB 的片段即可。
 * 一个 128 KiB 的缓冲区最多切成 3 段（跨界那一段被切开），
 * 远小于 8 项的容量。
 *
 * 这样做的价值不只是"能跑"：它让"一次命令搬多少"由**内存大小**决定，
 * 而不是由"这段内存恰好落在哪个 64 KiB 窗口"决定——
 * 后者会让吞吐随分配结果波动，是最难解释的一类性能现象。 */
#define ATA_PRD_MAX 8

bool fe_ata_dma_covers(u64 phys, u64 len)
{
    if (len == 0 || (len & 1) != 0 || len > (u64)ATA_PRD_MAX * 0x10000ull) {
        return false;
    }
    u64 at = phys;
    u64 left = len;
    for (u32 i = 0; i < ATA_PRD_MAX && left > 0; i++) {
        u64 page_left = 0x10000ull - (at & 0xFFFFull);
        u64 take = left < page_left ? left : page_left;
        if (take == 0 || (take & 1) != 0 || take > 0xFFFFull) {
            return false;
        }
        at += take;
        left -= take;
    }
    return left == 0;
}

/* 返回用掉的表项数；无法表达时返回 0（调用者按 RANGE 报错）。 */
static u32 prd_build_multi(struct prd_entry *tbl, u64 phys, u64 len, u32 max_entries)
{
    u32 n = 0;
    u64 at = phys;
    u64 left = len;
    while (left > 0) {
        if (n >= max_entries) {
            return 0;
        }
        u64 page_left = 0x10000ull - (at & 0xFFFFull);
        u64 take = left < page_left ? left : page_left;
        if (take == 0 || (take & 1) != 0 || take > 0xFFFFull) {
            return 0;
        }
        tbl[n].phys = (u32)at;
        tbl[n].count = (u32)take;
        n++;
        at += take;
        left -= take;
    }
    if (n == 0) {
        return 0;
    }
    tbl[n - 1].count |= ATA_PRD_EOT;
    return n;
}

/* 一次 DMA 命令搬一**段**（tot_len 字节，起始物理地址 base_phys）。
 * 与 fe_ata_read 的 DMA 分支是同一套时序，区别只在这里由自己建表。
 * 调用者必须保证：desc_virt 可写、desc_phys 是它的物理地址、
 * base_phys..+tot_len 全部能被设备访问。 */
static int read_dma_segment(struct fe_ata_channel *ch, u32 lba, u32 count,
                            u64 base_phys, u64 tot_len,
                            void *desc_virt, u64 desc_phys)
{
    u32 n = prd_build_multi((struct prd_entry *)desc_virt, base_phys, tot_len,
                            ATA_PRD_MAX);
    if (n == 0) {
        return FE_ATA_ERR_RANGE;
    }
    /* PRDT 必须在**同一个 64 KiB 页**里：设备按物理地址线性读表，
     * 表项自身跨界时设备侧的加法会失真。表很小（≤128 字节），
     * 只要分配它的那块 DMA 内存起点离边界够远就不会发生——这里检查一次，
     * 免得将来有人把表挪到一段"看起来更省事"的内存上。 */
    if ((desc_phys & ~0xFFFFull) != ((desc_phys + (u64)n * 8 - 1) & ~0xFFFFull)) {
        return FE_ATA_ERR_RANGE;
    }
    if (wait_not_busy(ch) != FE_ATA_OK) {
        return FE_ATA_ERR_TIMEOUT;
    }

    fe_outb(ch->bm_base + ATA_BM_CMD, 0);
    fe_outb(ch->bm_base + ATA_BM_STATUS, ATA_BM_ST_ERR | ATA_BM_ST_INTR);
    fe_outl(ch->bm_base + ATA_BM_PRDT, (u32)desc_phys);
    fe_outb(ch->bm_base + ATA_BM_CMD, ATA_BM_CMD_READ);

    set_lba_count(ch, lba, count);
    fe_outb(ch->cmd_base + ATA_R_COMMAND, ATA_CMD_READ_DMA);
    (void)status_after_command(ch);
    fe_outb(ch->bm_base + ATA_BM_CMD, ATA_BM_CMD_READ | ATA_BM_CMD_START);

    u64 t0 = fe_clock_ns();
    u64 limit = transfer_timeout_ns(count);
    for (;;) {
        if (!(fe_inb(ch->bm_base + ATA_BM_CMD) & ATA_BM_CMD_START)) {
            break;
        }
        if (fe_clock_ns() - t0 > limit) {
            fe_outb(ch->bm_base + ATA_BM_CMD, 0);
            return FE_ATA_ERR_TIMEOUT;
        }
    }
    u8 bms = fe_inb(ch->bm_base + ATA_BM_STATUS);
    fe_outb(ch->bm_base + ATA_BM_CMD, 0);
    if (!(bms & ATA_BM_ST_INTR) || (bms & ATA_BM_ST_ERR)) {
        return FE_ATA_ERR_DEVICE;
    }
    fe_outb(ch->bm_base + ATA_BM_STATUS, ATA_BM_ST_ERR | ATA_BM_ST_INTR);
    if (wait_not_busy(ch) != FE_ATA_OK) {
        return FE_ATA_ERR_TIMEOUT;
    }
    if (fe_inb(ch->cmd_base + ATA_R_STATUS) & (ATA_ST_ERR | ATA_ST_DF)) {
        return FE_ATA_ERR_DEVICE;
    }
    return FE_ATA_OK;
}

int fe_ata_read_dma_segment(struct fe_ata_channel *ch, u32 lba, u32 count,
                            u64 base_phys, void *desc_virt, u64 desc_phys)
{
    if (!ch || !ch->dma_enabled) {
        return FE_ATA_ERR_NODMA;
    }
    if (count == 0 || count > fe_ata_max_sectors(ch)) {
        return FE_ATA_ERR_RANGE;
    }
    if ((u64)lba + count > ch->sector_count) {
        return FE_ATA_ERR_RANGE;
    }
    u64 bytes = (u64)count * FE_ATA_SECTOR_SIZE;
    if (!fe_ata_dma_covers(base_phys, bytes)) {
        return FE_ATA_ERR_RANGE;
    }
    return read_dma_segment(ch, lba, count, base_phys, bytes, desc_virt, desc_phys);
}

const char *fe_ata_mode_name(const struct fe_ata_channel *ch)
{
    if (!ch) {
        return "无";
    }
    if (!ch->dma_enabled) {
        return "PIO";
    }
    if (ch->active_mode & 0x40) {
        return "UDMA";
    }
    return "多字DMA";
}
