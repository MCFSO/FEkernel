/* SPDX-License-Identifier: 0BSD */
/* libdrv —— **驱动公共设施**的接口。
 *
 * 分层：libfe 是「系统调用 ABI 的投影 + 最小运行库」，任何程序都可以链接；
 * libdrv 里放的是**设备语义**（8042 控制器时序、将来的 PCI 配置空间访问、
 * 环形缓冲…），只链接进 user/servers/ 下的常驻服务。
 *
 * 这个分界不是洁癖：`hello` 这种程序链进一份 8042 时序代码毫无意义，
 * 而且一旦设备代码住进运行库，它就会开始长出「反正 libfe 里有」的
 * 跨层调用——那种耦合在只有两个驱动时看不出问题，到十个驱动时收不回来。
 *
 * 构建系统按目录决定链接谁（见 tools/build.py 的 find_user_programs）：
 *   user/servers/  → libfe + libdrv
 *   user/bin/      → libfe
 */
#ifndef FE_DRV_H
#define FE_DRV_H

#include <fe_user.h>

/* ---------------- 8042（PS/2）控制器访问层 ----------------
 * 键盘与鼠标两个服务共用。这一层只做「把字节可靠地送进/取出控制器」，
 * 不含任何设备语义——扫描码与鼠标包的解析在各自的服务里。 */

int fe_ps2_read(u8 *out, int *from_mouse);
int fe_ps2_write_data(u8 v);
int fe_ps2_write_cmd(u8 v);
int fe_ps2_enable_irq(u8 mask);
int fe_ps2_device_cmd(u8 cmd, int to_mouse, u8 *resp);
int fe_ps2_device_cmd_param(u8 cmd, u8 param, int to_mouse, u8 *resp);
void fe_ps2_flush(void);

/* ---------------- ATA（PIO 兼容 IDE）通道 ----------------
 *
 * ★ 这一层为什么在 libdrv 里、而不是在 blkd 里 ★
 * 升级到总线主控 DMA 之后，"搬 N 个扇区"这件事多了三样与块设备语义
 * 无关的细节：PRDT 填法、BMDMA 引擎时序、物理地址约束。
 * 它们属于**设备访问层**，不属于"块设备服务"。
 * 将来的 AHCI / virtio-blk 会各自长一层，blkd 只换掉下面这一层。
 *
 * ★ 物理地址约束（这一层最容易被忽略的部分）★
 * DMA 缓冲不只是"一段内存"，它必须是：
 *   - **物理连续**（设备只认物理地址，没有 IOMMU 时散页拼不起来）；
 *   - 长度 ≤ 64 KiB 且**不跨 64 KiB 边界**（BMDMA 的 PRDT 表项长度只有
 *     16 位且必须偶数，设备侧的地址加法不在表项内处理进位）。
 * 所以驱动不能拿任意一块内存去 DMA——必须先问 fe_mem_alloc_dma 要
 * 物理连续的块，再用 fe_ata_dma_usable 检查边界条件。
 * 违反它的症状是"偶尔搬到相邻内存"，属于最难查的一类错误。 */

#define FE_ATA_OK            0
#define FE_ATA_ERR_INVAL    (-1)    /* 参数错 */
#define FE_ATA_ERR_NO_DEVICE (-2)   /* 通道上没有设备（或不是 ATA 设备） */
#define FE_ATA_ERR_TIMEOUT  (-3)
#define FE_ATA_ERR_DEVICE   (-4)    /* 设备报了 ERR/DF */
#define FE_ATA_ERR_NODMA    (-5)    /* 没有总线主控端口，或设备不支持 DMA */
#define FE_ATA_ERR_RANGE    (-6)    /* LBA/扇区数越界，或缓冲不满足 DMA 约束 */
#define FE_ATA_ERR_DMA_MISMATCH (-7) /* DMA 读回来的数据与 PIO 不一致 */

#define FE_ATA_MODEL_MAX 41
#define FE_ATA_SECTOR_SIZE 512

/* 一个 ATA 通道（主/从各一条，或主通道的 master/slave 两个盘） */
struct fe_ata_channel {
    u16 cmd_base;                   /* 命令块端口基址（0x1F0 或 0x170） */
    u16 ctrl_base;                  /* 控制块端口（0x3F6 或 0x376） */
    u16 bm_base;                    /* 总线主控寄存器基址（PCI BAR4 + 通道偏移） */
    bool bm_primary;                /* true = 通道 0（偏移 0），false = 通道 1（偏移 8） */
    bool slave;                     /* true = 从盘（DRIVE 寄存器的 bit4） */
    u8   lba_high;                  /* 驱动/磁头寄存器的 bit4..7 */
    bool dma;                       /* 有没有总线主控端口 */
    bool dma_enabled;               /* 设备是否已被切到 DMA 模式 */
    u8   active_mode;               /* SET FEATURES 生效的那个模式字节 */
    u8   udma_mask;                 /* IDENTIFY 字 88 低 7 位：支持的 UDMA 档位 */
    u8   mwdma_mask;                /* 支持的多字 DMA 档位 */
    u32  sector_count;              /* 容量（LBA28 口径，与今天的命令集一致） */
    char model[FE_ATA_MODEL_MAX];
};

/* 探测通道上的设备并读出型号/容量/支持的传输模式。
 * bm_base 为 0 表示没有总线主控端口（此时只能用 PIO）。
 * 返回 FE_ATA_OK 或负错误码。 */
int fe_ata_init(struct fe_ata_channel *ch, u16 cmd_base, u16 ctrl_base,
                u16 bm_base, bool bm_primary, bool slave);

/* 让设备切到 DMA 模式（UDMA 优先，其次多字 DMA）。
 * ★ 它只负责"发 SET FEATURES"，不代表 DMA 真的能用 ★
 * 能不能用由 fe_ata_verify_dma 用一次真实传输判定（理由见 ata.c）。 */
int fe_ata_enable_dma(struct fe_ata_channel *ch);

/* 用一次真实传输验证 DMA：PIO 读一份做基准，DMA 读一份比对。
 * 调用者必须把 dma_buf 预先写成不可能的值（例如全 0xA5），
 * 否则"没搬"与"搬对了"区分不开。返回 FE_ATA_OK 才算 DMA 可用。 */
int fe_ata_verify_dma(struct fe_ata_channel *ch, u32 lba, u32 count,
                      u8 *pio_buf, u8 *dma_buf, u64 dma_phys,
                      void *desc_virt, u64 desc_phys);

/* 软复位通道。
 *
 * ★ 为什么它是必需的、而不是"出错时才用的兜底" ★
 * 一条被拒绝的命令（ERR 置位）会把设备留在错误状态里，而 ERR 位是**粘滞**的：
 * 此后每条新命令返回时它还在，于是所有状态等待一律判失败——
 * 症状是"探完 DMA 之后再也不能读盘"。实测踩到的正是这个：
 * SET FEATURES 被拒（设备不支持 DMA），紧接着的 PIO 批量读第一块就失败。
 *
 * 复位会把传输模式打回上电默认（PIO），所以本函数会把 dma_enabled 清掉。 */
int fe_ata_soft_reset(struct fe_ata_channel *ch);

u32  fe_ata_sector_count(const struct fe_ata_channel *ch);
u32  fe_ata_max_sectors(const struct fe_ata_channel *ch);
bool fe_ata_dma_usable(const struct fe_ata_channel *ch, u64 phys, u64 len);
const char *fe_ata_mode_name(const struct fe_ata_channel *ch);

/* 最近一次 PIO 失败时设备报的状态/错误寄存器（诊断用）。
 * ★ 为什么值得暴露 ★ 只有"失败"这一个字无法分辨是超时、设备 ABRT、
 * 还是我们自己把参数写错了——而三者的修法完全不同。 */
u8 fe_ata_last_status(void);
u8 fe_ata_last_error(void);

/* 读 count 个扇区（1..256）。dma = true 时必须给出 buf_phys 与 desc_phys
 * （两者都要是设备能访问的物理地址），buf 是同一块内存在**调用者地址空间里
 * 已映射**的虚拟地址——一次传输要同时用到两类地址：
 * CPU 用虚拟地址（buf / desc_virt）读写，设备用物理地址（buf_phys / desc_phys）搬运。
 * 描述符表由本层按需建（可能用多个表项），调用者只需提供一块够大的 DMA 内存。 */
int fe_ata_read(struct fe_ata_channel *ch, u32 lba, u32 count, u8 *buf, bool dma,
                u64 buf_phys, u64 desc_phys, u32 *out_done);

/* 同上，但明确给出描述符表的**虚拟**地址（表要在调用者地址空间里已映射）。
 * fe_ata_read 是它把 desc_phys 当虚拟地址用的简便版本。 */
int fe_ata_read_ex(struct fe_ata_channel *ch, u32 lba, u32 count, u8 *buf, bool dma,
                   u64 buf_phys, u64 desc_phys, void *desc_virt, u32 *out_done);

/* 按 16 位端口直接读一批扇区（PIO）。返回实际搬完的扇区数。
 *
 * ★ 为什么把 PIO 也做成"一批"而不是"一个扇区一次" ★
 * 一条 ATA 读命令本来就能读多个扇区（扇区计数字段 1..256），
 * "一次一个扇区"是**调用方的选择**，不是设备的能力。
 * 于是它的代价是每 512 字节多一轮完整握手——这正好是本项目
 * 0.18 MB/s 那个数字的一半来源（另一半是 2 扇区的 IPC 上限）。 */
u32 fe_ata_read_pio_sectors(struct fe_ata_channel *ch, u32 lba, u32 count, u8 *out);

/* 写一批扇区（PIO）。返回实际写完的扇区数。
 * ★ 不发 FLUSH CACHE ★ 落盘由 fe_ata_flush 显式承担（理由见 ata.c）。 */
u32 fe_ata_write_pio_sectors(struct fe_ata_channel *ch, u32 lba, u32 count,
                             const u8 *in);

/* 刷写缓存（屏障）。0 = 有保证；1 = 设备不实现 FLUSH（数据已交给驱动器，
 * 但没有落盘保证，调用方自己决定接不接受）；负值 = 失败。 */
int fe_ata_flush(struct fe_ata_channel *ch);

/* 往一块 DMA 内存里填"一个表项"的 PRDT（blkd 在启动 DMA 前调用）。 */
void fe_ata_fill_prd(void *tbl, u64 phys, u32 len);

/* ---- 一次命令搬一**整段**（多表项 PRDT） ----
 *
 * ★ 为什么需要它，而不是"单表项 + 调用者自己切" ★
 * 单表项最多 64 KiB 且不能跨 64 KiB 边界，于是"一次能搬多少"会取决于
 * 缓冲区**恰好落在哪个 64 KiB 窗口**——吞吐随分配结果波动，
 * 那是最难解释的一类性能现象。多表项让"一次搬多少"只由内存大小决定。
 *
 * 代价：描述符表要自己准备（必须是一段设备能访问的 DMA 内存，
 * 且它自己不能跨 64 KiB 边界）。调用者传 desc_virt / desc_phys 进来。 */
bool fe_ata_dma_covers(u64 phys, u64 len);
int fe_ata_read_dma_segment(struct fe_ata_channel *ch, u32 lba, u32 count,
                            u64 base_phys, void *desc_virt, u64 desc_phys);

#endif /* FE_DRV_H */
