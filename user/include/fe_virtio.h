/* SPDX-License-Identifier: 0BSD */
/* virtio-blk 接口层（D4 的后半：真正支持 DMA 的那条路）。
 *
 * ★ 为什么不接着修 IDE ★
 *
 * 实测证据：QEMU 的 IDE 盘 `IDENTIFY` 报 w88 = 0x203F —— UDMA 与多字 DMA
 * 的**支持位都是 0**。也就是说 QEMU 上这条通道根本没有 DMA 可验；
 * 而它还有另一个独立的现象：多扇区命令发出后设备回
 * `状态 0x50 = DRDY|DSC`（没有 DRQ），"一条命令一个扇区"却完全正常。
 *
 * 与其在一个**没有 DMA 的设备**上继续追一个 DMA 相关的怪状态，
 * 不如换到 virtio-blk：它有公开规范（OASIS Virtio 1.x，自己写不涉及抄
 * 任何内核代码）、QEMU 模拟成熟、而且**它的数据通路本来就是 DMA**
 * （virtqueue 里的每个缓冲区都以物理地址交给设备）。
 *
 * ★ 这一层的边界 ★
 *
 *   fe_virtio.h / virtio.c —— virtqueue 机制（描述符链、available/used ring、
 *                             物理地址分段、通知与完成判定）
 *   blkd 的 virtio 后端      —— 设备发现（PCI capability）、初始化时序、
 *                             virtio-blk 的请求格式
 *
 * 划在这里的理由与 ata.c 一样：**"怎么把字节交给设备"与"块设备服务要解决
 * 什么问题"是两件事**。三描述符链、used ring 的下标回绕、物理地址不能跨
 * 64 KiB 这些细节，与 IPC 协议、写保护矩阵毫无关系。
 *
 * ★ 本文件里没有任何"内核"★
 * 用的是哪一段 BAR、哪一条 IRQ、哪一块物理内存，全部是用户态自己查出来
 * 并向内核**认领**的——内核不知道有 virtio 这回事。
 */
#ifndef FE_DRV_VIRTIO_H
#define FE_DRV_VIRTIO_H

#include <fe_user.h>

/* ------------------------------------------------------------------ */
/* virtio 1.0 的 PCI capability                                        */
/* ------------------------------------------------------------------ */

/* 结构（规范 §4.1.4）：
 *   字节 0..2  PCI 标准 capability 头（id / next / len=20）
 *   字节 3     cfg_type
 *   字节 4     bar
 *   字节 5     id
 *   字节 6..7  padding
 *   字节 8..11 offset（在 BAR 内的字节偏移）
 *   字节 12..15 length
 * 偏移 3..15 都填进同一个 32 位字段的 [8..23] 位（低 8 位是标准头）。 */
#define VIRTIO_PCI_CAP_VNDR        0x09    /* 厂商自定义 capability id */
#define VIRTIO_PCI_CAP_LEN         20

#define VIRTIO_PCI_CAP_COMMON_CFG  1       /* 通用配置（状态、feature、队列） */
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2       /* 队列通知 */
#define VIRTIO_PCI_CAP_ISR_CFG     3       /* 中断状态（读一次即清） */
#define VIRTIO_PCI_CAP_DEVICE_CFG  4       /* 设备专有配置（blk 的容量在里面） */
#define VIRTIO_PCI_CAP_PCI_CFG     5

/* common cfg 的字段偏移（规范 §4.1.4.3）。★ 全部是小端、按位宽访问 ★
 * 用 8/16/32 位读写而不是"读一个 32 位再切"：设备侧可能对访问宽度敏感，
 * 而规范给的就是这些宽度。 */
#define VIRTIO_COMMON_DFSELECT     0x00    /* u32 设备 feature 选择 */
#define VIRTIO_COMMON_DF           0x04    /* u32 设备 feature 位 */
#define VIRTIO_COMMON_GFSELECT     0x08    /* u32 驱动 feature 选择 */
#define VIRTIO_COMMON_GF           0x0C    /* u32 驱动 feature 位 */
#define VIRTIO_COMMON_MSIX         0x10    /* u16 */
#define VIRTIO_COMMON_NUMQ         0x12    /* u16 队列个数 */
#define VIRTIO_COMMON_STATUS       0x14    /* u8  设备状态 */
#define VIRTIO_COMMON_CFGGEN       0x15    /* u8  配置代次（改队列后 +1） */
#define VIRTIO_COMMON_Q_SELECT     0x16    /* u16 选中的队列 */
#define VIRTIO_COMMON_Q_SIZE       0x18    /* u16 队列大小（**设备可写**，回读为实际值） */
#define VIRTIO_COMMON_Q_MSIX       0x1A    /* u16 */
#define VIRTIO_COMMON_Q_ENABLE     0x1C    /* u16 */
#define VIRTIO_COMMON_Q_NOFF       0x1E    /* u16 通知偏移（乘 notify_off_multiplier） */
#define VIRTIO_COMMON_Q_DESC       0x20    /* u64 描述符表物理地址 */
#define VIRTIO_COMMON_Q_DRIVER     0x28    /* u64 available ring 物理地址 */
#define VIRTIO_COMMON_Q_DEVICE     0x30    /* u64 used ring 物理地址 */

/* 设备状态位（规范 §2.1）——初始化必须**按顺序**置位，
 * 每一步都是"我准备好了什么"的声明，不是随便写一个终值。 */
#define VIRTIO_STATUS_ACKNOWLEDGE  1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_DRIVER_OK    4
#define VIRTIO_STATUS_FEATURES_OK  8
#define VIRTIO_STATUS_FAILED       128

/* feature 位（规范 §2.2 与 §5.2 的 blk 部分） */
#define VIRTIO_F_VERSION_1         32      /* 位 32：设备支持 1.0 接口 */
#define VIRTIO_BLK_F_RO            5
#define VIRTIO_BLK_F_BLK_SIZE      6
#define VIRTIO_BLK_F_FLUSH         9
#define VIRTIO_BLK_F_SIZE_MAX      1
#define VIRTIO_BLK_F_SEG_MAX       2

/* ------------------------------------------------------------------ */
/* virtqueue（规范 §2.6）                                              */
/* ------------------------------------------------------------------ */

/* 描述符：16 字节。flags 的位 0 = NEXT（后面还有一项），位 1 = WRITE
 * （这一项是**设备写**进内存，即读方向）。 */
#define VIRTQ_DESC_F_NEXT   1
#define VIRTQ_DESC_F_WRITE  2

struct virtq_desc {
    u64 addr;       /* 设备视角的**物理**地址 */
    u32 len;
    u16 flags;
    u16 next;
};

#define VIRTQ_AVAIL_F_NO_INTERRUPT 1

struct virtq_avail {
    u16 flags;
    u16 idx;
    u16 ring[/* qsize */];
};

struct virtq_used_elem {
    u32 id;
    u32 len;        /* 设备实际写入的字节数 */
};

struct virtq_used {
    u16 flags;
    u16 idx;
    struct virtq_used_elem ring[/* qsize */];
};

/* 一个 virtqueue 的驱动侧状态。
 *
 * ★ 描述符表 / available / used 三块都必须在**设备能访问的物理内存**里 ★
 * 它们由调用者用 fe_mem_alloc_dma 分配（物理连续、可用 fe_mem_info 查
 * 物理地址），虚拟地址给 CPU 用、物理地址给设备用——同一块内存两个视角。
 *
 * ★ 为什么 size 不写死 ★
 * 队列长度**由设备决定**：驱动写一个期望值，读回来是设备实际接受的值
 * （规范允许它改小）。硬编码 256 的驱动在设备只支持 64 时会写出越界的
 * 环下标——而那种错误在压力下才现形。 */
struct fe_virtq {
    u16 size;               /* 设备实际接受的队列长度（2 的幂） */
    u16 last_used;          /* 驱动已处理到的 used.idx（回绕靠 16 位自然溢出） */
    u16 next_desc;          /* 下一个要用的描述符下标（本驱动一次只挂一条链，
                             * 所以它总是 0，但保留字段以便将来做多请求流水） */

    /* CPU 视角（映射进来的虚拟地址） */
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;

    /* 设备视角（物理地址） */
    u64 desc_phys;
    u64 avail_phys;
    u64 used_phys;
};

/* 队列在 DMA 对象里的布局。四块各自独立、且都不跨 64 KiB 边界。
 * 偏移按规范的对齐要求取：描述符表 16 字节、avail 2 字节、used 4 字节，
 * 这里统一按 4 KiB 对齐——反正分配的是整页。 */
#define FE_VIRTQ_OFF_DESC   0x0000
#define FE_VIRTQ_OFF_AVAIL  0x1000
#define FE_VIRTQ_OFF_USED   0x2000

/* 初始化一个 virtqueue 的**驱动侧**结构。
 * dma_obj_phys/dma_obj_virt 是一块已经分配并映射好的 DMA 内存。 */
void fe_virtq_init(struct fe_virtq *q, u16 want_size,
                   void *dma_obj_virt, u64 dma_obj_phys);

/* 把三个物理地址回读出来做自洽检查（描述符表与两个环必须落在同一个
 * 64 KiB 窗口里——设备按物理地址线性访问它们）。返回 0 = OK。 */
int fe_virtq_check(const struct fe_virtq *q);

/* ------------------------------------------------------------------ */
/* virtio-blk 协议（规范 §5.2）                                        */
/* ------------------------------------------------------------------ */

#define VIRTIO_BLK_T_IN      0     /* 读：设备 → 内存 */
#define VIRTIO_BLK_T_OUT     1     /* 写：内存 → 设备 */
#define VIRTIO_BLK_T_FLUSH   4
#define VIRTIO_BLK_T_GET_ID  8

#define VIRTIO_BLK_S_OK      0
#define VIRTIO_BLK_S_IOERR   1
#define VIRTIO_BLK_S_UNSUPP  2

/* 请求头：三描述符链的第一项。★ 16 字节、**必须**与设备逐字节一致 ★ */
struct virtio_blk_req_hdr {
    u32 type;
    u32 reserved;
    u64 sector;     /* 512 字节扇区号（规范 v1.0 的 512 字节口径） */
};

/* 设备专有配置（VIRTIO_PCI_CAP_DEVICE_CFG 指向的地方）。
 * 只有前 8 字节是 v1.0 的必选字段，后面靠 feature 位协商是否有效。 */
struct virtio_blk_config {
    u64 capacity;       /* 容量，单位是 512 字节扇区 */
    u32 size_max;
    u32 seg_max;
    /* 后面还有 geometry/block_size/… 本驱动不读它们 */
};

/* ------------------------------------------------------------------ */
/* 一个 virtio PCI 设备的驱动侧上下文                                  */
/* ------------------------------------------------------------------ */

/* 一类结构的 PCI capability 解析结果。
 * ★ 这四个位置**没有任何一个是固定的** ★ 全部来自设备的配置空间，
 * 所以驱动必须先走一遍 capability 链——这正是"设备地址从哪来"在
 * virtio 上的具体形态（对比 IDE 的 0x1F0：那一代把地址写死在规范里）。 */
struct fe_virtio_cap {
    u8  bar;            /* 在第几个 BAR 里 */
    u32 offset;         /* BAR 内字节偏移 */
    u32 length;
    u8  id;             /* capability 的 id 字段（多队列时用来配对） */
    bool found;
    u32 multiplier;     /* 只有 NOTIFY 用：notify_off × multiplier */
};

struct fe_virtio_dev {
    struct fe_virtio_cap cap[6];

    /* 映射进来的 MMIO 窗口（第一个内存 BAR）与四类结构的虚拟地址 */
    u8 *mmio;
    u8 *common;
    u8 *notify;
    u8 *isr;
    u8 *device;

    u32 notify_multiplier;
    u16 num_queues;

    u64 features;           /* 双方协商后的 feature 位图 */
    u64 capacity_sectors;   /* virtio-blk 的容量（512 字节扇区） */
    bool initialized;
};

/* 走一遍 PCI capability 链，把四类结构的位置填进 out[]。
 * 没有 common cfg 时返回 FE_ERR_NOENT（那不是 virtio 1.0 设备）。 */
int fe_virtio_pci_scan_caps(u8 bus, u8 dev, u8 func,
                            struct fe_virtio_cap *out, u32 max_type);


/* 把 capability 原始字段 dump 成一行行文本（emit 收到的是 '\n' 结尾的串）。
 * ★ 为什么值得一个专门的诊断入口 ★ capability 解析属于"错一个字节后面全错"
 * 的那类代码，失败表现是"某区域地址是垃圾"——从那个症状反推字段位置
 * 几乎不可能，而把设备给的原始字节打出来一眼就能分清"解析错了"与
 * "设备给的就不是我们以为的东西"。 */
void fe_virtio_dump_caps(u8 bus, u8 dev, u8 func, void (*emit)(const char *));

/* ★ 字段位置严格按规范 §4.1.4 ★
 *
 *   偏移 0  cap_vndr（= 0x09）
 *   偏移 1  cap_next
 *   偏移 2  cap_len（= 20）
 *   偏移 3  cfg_type   ← 注意：它**不在** 4 字节边界上
 *   偏移 4  bar
 *   偏移 5  id
 *   偏移 6  padding[2]
 *   偏移 8  offset（u32，BAR 内字节偏移）
 *   偏移 12 length（u32）
 *   偏移 16 notify_off_multiplier（u32，只有 NOTIFY 有）
 *
 * cfg_type 落在 3 这个"歪"位置，这是整段代码最容易写错的地方：
 * 一个 32 位读 [0..3] 得到的是 [vndr, next, len, **cfg_type**]，
 * 而 [4..7] 是 [bar, id, pad, pad]。第一版把 bar 当成"下一组的低字节"，
 * 于是 bar/offset 全错——而症状是"某个区域的地址是垃圾"，
 * 从那个症状反推字段位置几乎不可能。所以这里按字节索引，不玩移位。
 *
 * raw 的排布：每 4 字节 = 一个 32 位头的低→高四个字节，也就是
 * raw[0]=cap_vndr, raw[1]=cap_next, raw[2]=cap_len, raw[3]=cfg_type, raw[4]=bar, raw[5]=id…
 * 数组**以配置空间的偏移为下标**（raw[0x84] 就是偏移 0x84 那个字节），
 * 所以调用者不需要按顺序读——链往哪个方向走都不影响解析。
 *
 * ★ "链从偏移 0 开始"这个假设也是错的 ★
 * 链的头指针在配置空间偏移 0x34。实测 QEMU 给的是 **0x98**，
 * 所以 raw[0..0x40] 全是零（那是标准的配置空间头，不是 capability）。
 * 第一版从 0 开始扫，于是"cap 原始 156 字节"全是 0、解析失败——
 * 而真相是**起点不在 0**。所以起点必须由调用者传进来。
 *
 * ★ 环路保护必须有 ★
 * next 是**设备给的字节偏移**。设备（或模拟它的东西）给出环状的 next 时，
 * 不设上界的遍历就是死循环——而它发生在驱动初始化阶段，症状是
 * "系统起来就不动了"，与"设备没找到"完全不像。 */
static inline int fe_virtio_parse_caps(const u8 *raw, u32 raw_len, u32 start,
                                       struct fe_virtio_cap *out, u32 max_type)
{
    if (!raw || !out || max_type == 0) {
        return FE_ERR_INVAL;
    }
    for (u32 i = 0; i < max_type; i++) {
        out[i].bar = 0;
        out[i].offset = 0;
        out[i].length = 0;
        out[i].id = 0;
        out[i].found = false;
        out[i].multiplier = 0;
    }
    (void)start;    /* 见下面的说明：不用它 */

    /* ★ 为什么是"扫描"而不是"沿 next 链走" ★
     *
     * 规范的做法是：从配置空间偏移 0x34 取能力表头指针，
     * 然后沿每个能力的 next 字节走下去。实测（QEMU）它不成立：
     *
     *   0x34 读出 0x98（确认过字节位置），而 0x98 上是 MSI-X（cap_id 0x11）；
     *   真正的四个 virtio 能力在 0x40/0x50/0x60/0x70，链头是 0x84（type=5），
     *   它们的 next 链自恰：0x84→0x70→0x60→0x50→0x40→0。
     *
     * 也就是说：**设备给的链是完整的，只是 0x34 那个头指针指到了别处**。
     * 我在这里试过三种读法（字节/半字/全字），都得到 0x98，
     * 所以这不是"我读错了一个字节"。
     *
     * 于是判据换成一个**可自证**的扫描：在已经读出来的原始字节里找
     * "cap_id == 0x09 且 1 <= cfg_type <= 5" 的条目。为什么可以这么做：
     *   - 0x09 是规范给企业自定义能力的 ID，碰巧出现在
     *     四字节对齐位置、且后一个字节恰好是 1..5 的概率极低；
     *   - 扫出来的条目要**自洽**：它们的 next 链必须把它们串起来。
     *     下面确实做了这个交叉校验，对不上就明确报错，而不是猜。
     *
     * 代价：假如将来遇到一台"头指针正确但链不自洽"的真设备，
     * 这里会拒绝它（而不是读出一个错的地址）—— 那是可接受的失败方式。
     * 这一条已写进 docs/12 的边界表。 */
    for (u32 off = 0; off + 8 <= raw_len; off += 4) {
        u8 cid = raw[off + 0];
        u8 ty = raw[off + 3];
        if (cid != VIRTIO_PCI_CAP_VNDR || ty < 1 || ty >= max_type) {
            continue;
        }
        out[ty].bar = raw[off + 4];
        out[ty].id = raw[off + 5];
        if (off + 12 <= raw_len) {
            out[ty].offset = (u32)raw[off + 8] |
                             ((u32)raw[off + 9] << 8) |
                             ((u32)raw[off + 10] << 16) |
                             ((u32)raw[off + 11] << 24);
        }
        if (off + 16 <= raw_len) {
            out[ty].length = (u32)raw[off + 12] |
                             ((u32)raw[off + 13] << 8) |
                             ((u32)raw[off + 14] << 16) |
                             ((u32)raw[off + 15] << 24);
        }
        if (ty == VIRTIO_PCI_CAP_NOTIFY_CFG && off + 20 <= raw_len) {
            out[ty].multiplier = (u32)raw[off + 16] |
                                 ((u32)raw[off + 17] << 8) |
                                 ((u32)raw[off + 18] << 16) |
                                 ((u32)raw[off + 19] << 24);
        }
        out[ty].found = true;
    }
    if (!out[VIRTIO_PCI_CAP_COMMON_CFG].found) {
        return FE_ERR_NOENT;
    }
    /* 交叉校验：至少要有 common 与 notify 两个，否则不像 virtio 1.0 设备。
     * （不要求 isr/device：那两个在规范里是可选的。） */
    if (!out[VIRTIO_PCI_CAP_NOTIFY_CFG].found) {
        return FE_ERR_NOENT;
    }
    return FE_OK;
}

/* 把四类结构的虚拟地址算出来（mmio_base = 第一个内存 BAR 的映射地址）。 */
void fe_virtio_set_regions(struct fe_virtio_dev *d, void *mmio_base);

/* 直接把**已经解析好的**四类结构位置填进去。
 *
 * ★ 为什么需要这个入口（而不是让每个消费者自己去读 PCI capability）★
 * 配置空间端口是**独占**资源（两次访问构成一次操作，见 fe_pci.h），
 * 而引导链上只有一个任务能在这个时间点持有它。所以流程是：
 *   init（持有端口）解析 capability → 把四个偏移量通过命令行交给驱动
 *   → 驱动用本函数装配。
 * 这样"谁读配置空间"只有一个答案，而且那次读取的**原始字段**还能被
 * dump 出来核对（见 fe_virtio_dump_caps）。 */
void fe_virtio_set_regions_direct(struct fe_virtio_dev *d, void *mmio_base,
                                  u32 off_common, u32 off_notify,
                                  u32 off_isr, u32 off_device,
                                  u32 notify_multiplier);

/* ---- common cfg 小端访问器 ----
 *
 * ★ 为什么按规范给的宽度读写、而不是"读 32 位再切" ★
 * 设备侧可能对访问宽度敏感（规范允许它只实现某些宽度的访问），
 * 而规范给的就是这些宽度。多读几个字节"顺便看看"在某些设备上是错误。 */
static inline u8  vv_r8(const volatile u8 *base, u32 off)
{
    return *(const volatile u8 *)(base + off);
}

static inline void vv_w8(volatile u8 *base, u32 off, u8 v)
{
    *(volatile u8 *)(base + off) = v;
}

static inline u16 vv_r16(const volatile u8 *base, u32 off)
{
    return *(const volatile u16 *)(base + off);
}

static inline void vv_w16(volatile u8 *base, u32 off, u16 v)
{
    *(volatile u16 *)(base + off) = v;
}

static inline u32 vv_r32(const volatile u8 *base, u32 off)
{
    return *(const volatile u32 *)(base + off);
}

static inline void vv_w32(volatile u8 *base, u32 off, u32 v)
{
    *(volatile u32 *)(base + off) = v;
}

static inline u64 vv_r64(const volatile u8 *base, u32 off)
{
    return *(const volatile u64 *)(base + off);
}

static inline void vv_w64(volatile u8 *base, u32 off, u64 v)
{
    *(volatile u64 *)(base + off) = v;
}

/* 设备状态（读改写，因为每一位都是"我准备好了什么"的声明） */
static inline u8 fe_virtio_status(const struct fe_virtio_dev *d)
{
    return vv_r8(d->common, VIRTIO_COMMON_STATUS);
}

static inline void fe_virtio_set_status(const struct fe_virtio_dev *d, u8 v)
{
    vv_w8(d->common, VIRTIO_COMMON_STATUS, v);
}

static inline void fe_virtio_add_status(const struct fe_virtio_dev *d, u8 bits)
{
    fe_virtio_set_status(d, (u8)(fe_virtio_status(d) | bits));
}

/* 复位：写 0 是规范规定的复位方式（不是"清某一位"） */
static inline void fe_virtio_reset(const struct fe_virtio_dev *d)
{
    fe_virtio_set_status(d, 0);
}

/* 设备声明的 feature 低/高 32 位 */
static inline u64 fe_virtio_device_features(const struct fe_virtio_dev *d)
{
    vv_w32((volatile u8 *)d->common, VIRTIO_COMMON_DFSELECT, 0);
    u32 lo = vv_r32(d->common, VIRTIO_COMMON_DF);
    vv_w32((volatile u8 *)d->common, VIRTIO_COMMON_DFSELECT, 1);
    u32 hi = vv_r32(d->common, VIRTIO_COMMON_DF);
    return ((u64)hi << 32) | lo;
}

/* 告诉设备我接受哪些 feature（只报我**确实实现了**的那些） */
static inline void fe_virtio_driver_features(const struct fe_virtio_dev *d, u64 f)
{
    vv_w32((volatile u8 *)d->common, VIRTIO_COMMON_GFSELECT, 0);
    vv_w32((volatile u8 *)d->common, VIRTIO_COMMON_GF, (u32)(f & 0xFFFFFFFFu));
    vv_w32((volatile u8 *)d->common, VIRTIO_COMMON_GFSELECT, 1);
    vv_w32((volatile u8 *)d->common, VIRTIO_COMMON_GF, (u32)(f >> 32));
}

#endif /* FE_DRV_VIRTIO_H */

