/* SPDX-License-Identifier: 0BSD */
/* blkd —— 块设备服务（ATA，用户态驱动）。
 *
 * ★ 这一轮的改动：从"PIO 轮询 + 每次 1~2 扇区"变成"总线主控 DMA + 一整段" ★
 *
 * 为什么原来只有 0.18 MB/s，以及为什么那两个原因都不是"驱动能力"：
 *   1. **每次只搬 1~2 个扇区**，而这个上限来自 IPC 内联载荷（1024 字节），
 *      不来自设备。ATA 一条读命令本来就能读 256 个扇区（128 KiB）；
 *   2. **一次一个扇区地发命令**：PIO 路径里 `ata_read_sector` 是逐扇区调用的，
 *      于是每 512 字节都要走一遍"选盘 → 写 LBA → 写命令 → 等 DRQ"的完整握手。
 *
 * 所以这一轮做两件事，各自针对一个原因：
 *   - 走 **总线主控 DMA**（BMDMA）：CPU 不再逐字节搬数据，只在命令前后
 *     配置一次描述符表；
 *   - 走 **共享内存批量协议**（BLK_OP_READ_BULK）：客户端把一段 DMA 内存
 *     随请求交过来，驱动让设备**直接搬进那块内存**——数据不经过 IPC 载荷，
 *     内核完全不碰它。这是 `docs/07` 主线 A 说的那条路。
 *
 * ★ 总线主控寄存器块（BAR4）从哪来 ★
 * 它**不在**内核资源池里（内核不知道 PCI BAR 的地址——那是枚举的结果）。
 * 所以由设备管理器（devmgr）读配置空间探测出来、申报入池、再按任务 id
 * 授予本服务；init 把基址通过 `--bar4` 传进来。没有它时驱动**降级到 PIO**，
 * 而不是拒绝启动——"DMA 不可用"与"设备不能用"是两件事。
 *
 * 对外暴露的仍是 IPC 协议（不是系统调用）：客户端先按路径打开
 * /dev/blk0 拿到一条通往本服务的端点，再用 call/reply 请求数据。
 * 这就是微内核里的驱动形态——**内核完全不知道有块设备这回事**。
 */
#include <fe_user.h>
#include <fe_drv.h>
#include <fe_pci.h>             /* virtio 的 capability 链要走配置空间 */
#include <fe_virtio.h>          /* virtio 1.0 的寄存器布局与 blk 协议 */

#define ATA_CMD_BASE   0x1F0
#define ATA_CMD_LEN    8
#define ATA_CTRL_BASE  0x3F6
#define ATA_CTRL_LEN   1
#define ATA_IRQ        14

#define SECTOR_SIZE 512
/* 单次**内联**读的上限，由 IPC 内联载荷（1024 字节）决定，不是驱动能力决定的 */
#define MAX_READ_SECTORS 2
/* 单次写的上限：载荷 = 头 + 数据，所以比读少一点余量 */
#define MAX_WRITE_SECTORS 1

/* 批量读一次的上限。★ 这个数字是**协议上限**，不是设备上限 ★
 * 设备一条命令能读 256 个扇区（128 KiB），这里取 512 KiB 是因为
 * 客户端一次交过来的 DMA 内存可以更大——驱动会把它切成多条命令。
 * 再大就没有意义了（多出来的部分只是让单次调用的延迟变长）。 */
#define BULK_MAX_BYTES (512u * 1024u)

/* 协议：op 放在载荷第一个字段里 */
#define BLK_OP_INFO 1
#define BLK_OP_READ 2
#define BLK_OP_WRITE 3
#define BLK_OP_FLUSH 4     /* 把写缓存刷到盘上（一个**独立**的操作，见下） */
/* 批量读：数据走**共享内存**（随消息传来的内存对象句柄），不走载荷。
 * 请求载荷 = struct blk_bulk_req；应答载荷 = u32 status。 */
#define BLK_OP_READ_BULK 5

struct blk_req {
    u32 op;
    u32 count;
    u64 lba;
};

struct blk_info {
    u64 sectors;
    u32 sector_size;
    u32 _pad;
    char model[41];
    /* ★ "我有 DMA" 的可证伪表述（0 = 没有）★
     *
     * 吞吐数字本身证明不了"数据是 DMA 搬的"。这里报出**一段真实存在的
     * 硬件窗口**：客户端拿它去 {端口, MMIO} 认领一次，拿到"忙"就说明
     * 这段硬件确实存在且已被本服务独占；拿到 OK 说明根本没人持有它，
     * 那么"DMA"就是假的。
     *   - ATA 后端：总线主控寄存器的 I/O 端口基址（bar4）
     *   - virtio 后端：设备的 MMIO 窗口物理基址（mmio）
     * 两者都只为"被认领一次"而报出来，不代表客户端能拿它做别的事。 */
    u32 bar4;
    u32 dma_mode;       /* 1 = DMA；0 = 只有 PIO */
    u64 mmio;           /* virtio 后端的 MMIO 窗口物理基址（0 = 不用 MMIO） */
    u64 mmio_len;
    u32 backend;        /* 1 = ATA，2 = virtio-blk */
    u32 _pad2;
};

/* 批量读请求：LBA/长度 + 目标缓冲（随消息传来的内存对象句柄，1 个）。
 *
 * ★ 为什么"目标缓冲"必须由客户端提供、而不是驱动自己分配 ★
 * 因为数据要落到**客户端能读到的地方**。如果驱动自己分配一块内存、
 * 读完再用 IPC 回给客户端，那就又回到"数据过内核拷贝"的老路上去了
 * （而且受 1024 字节载荷限制）。让客户端交一块内存过来，
 * 设备就能直接 DMA 进去——这条路径上**没有任何一次数据拷贝**。
 *
 * ★ buf_len 到底管什么 ★
 * 它是客户端**声明**的缓冲长度，而客户端说的任何长度都不可信。
 * 所以它只用来做一件事：`buf_len >= want` ——"你自己说你的缓冲装得下"。
 * **真正的边界是内核给出的对象真实大小 `bi.size`**（见下面那一段）。
 *
 * ★ 为什么"声明得比对象大"不是错误 ★
 * 因为驱动**从不写超过 want 的字节**（want 由 sectors 算出），
 * 而 `want <= bi.size` 是被独立核对过的。于是一个客户端说
 * "我的缓冲有 512 KiB"而对象只有 4 KiB 时，多出来的那部分声明
 * 驱动根本不看——它既不多写一个字节，也没被误导。
 * 反过来，"声明得比需要的小"（`buf_len < want`）必须拒：
 * 那说明客户端自己算错了，而错的一方不该由驱动去猜。
 *
 * 这一条原来写反了（注释说会拒"声明 512 KiB 而对象只有 4 KiB"，
 * 而代码并不拒），于是 blkbench 照着注释写了一条断言，
 * 测出来是红的。**注释承诺了一个不存在的护栏**，
 * 比没有注释更糟——它会让后来的人以为那道墙在那儿。 */
struct blk_bulk_req {
    u32 op;
    u32 sectors;
    u64 lba;
    u64 buf_len;        /* 客户端声明的缓冲长度：只需 >= sectors*512 */
    u64 _reserved;      /* 显式保留：将来要加字段时不必改 ABI 长度 */
};

static u32 g_fail;
static u64 g_sectors;
static char g_model[41];

struct blk_backend {
    const char *name;
    /* 初始化。返回 0 = 成功。 */
    int (*init)(void);
    /* 把 count 个扇区读到 virt 指向的内存；phys 是**同一块内存**的设备
     * 视角地址（DMA 后端用它，PIO 后端忽略它）。返回 0 = 成功。
     * 两个地址都由调用者提供，见下面 backend_read_phys 的说明。 */
    int (*read)(u32 lba, u32 count, u8 *virt, u64 phys);
    /* 是否具备 DMA 能力（决定 blk_info 怎么报、批量路径走哪条） */
    bool dma;
    /* ★ 这个后端**只认物理地址** ★
     *
     * 这条声明的是"数据往哪落"由谁决定：
     *   - false（ATA）：CPU 把数据从端口搬进内存，所以给它一个虚拟地址
     *     就够了——PIO 路径就是这么工作的；
     *   - true （virtio）：设备自己往内存里搬，它只认物理地址。
     *     给它一个虚拟地址不是"效率差一点"，而是**它会把数据写到
     *     那个数字对应的物理页上去**——也就是写到别的任务的内存里。
     *
     * 所以"能不能写虚拟地址"是**传输方式**的性质，不是设备的性质：
     * 同一个 ATA 盘，PIO 能、BMDMA 不能（而 ATA 有 PIO 可以退，
     * 所以它整体算能）。内联读那条路需要一个虚拟地址落点，
     * 于是只有物理视角的后端必须在中间垫一块自己的 DMA 缓冲，
     * 代价是每次多一次 1 KiB 的拷贝——见 backend_read_to_virt。 */
    bool phys_only;
};

/* 当前选中的后端（main 里按命令行选）。 */
static const struct blk_backend *g_be;

/* ---- ATA 后端的通道与 DMA 状态 ---- */
static struct fe_ata_channel g_ch;
static bool g_dma_ready;
static bool g_vio_ok;

/* 描述符表所在的那块 DMA 内存（驱动自己分配，一次分配长期复用）。
 * ★ 为什么它必须单独一块 ★ PRDT 自己也要有物理地址（设备要读它），
 * 而物理地址只能从 DMA 内存对象拿到。它很小（8 项 × 8 字节 = 64 字节），
 * 但"小"不影响这条约束。 */
static long g_prd_obj = -1;
static u64  g_prd_phys;
static void *g_prd_virt;

/* DMA 验证用的两块缓冲（各 512 字节）。
 * ★ 必须是**有物理地址**的内存 ★ 所以两块都用 fe_mem_alloc_dma 分配：
 * 一块给 PIO 读做基准，一块给 DMA 读（先写 0xA5）。
 * 复用描述符表那块内存是**不行**的——DMA 会把表本身覆盖掉。 */
static long g_dma_probe_obj = -1;
static u64  g_dma_probe_phys;
static u8  *g_dma_probe_buf;

/* 内联读的**垫脚缓冲**：只给"只认物理地址"的后端用（见 phys_only）。
 *
 * ★ 为什么不用 g_prd_virt 那块 ★
 * 那块是 ATA 的 PRDT 表 + DMA 回退缓冲，只在 ATA 分支里分配——
 * 让 virtio 的内联读去依赖"一个 ATA 专用的分配"，就是一条看不见的耦合：
 * 换后端时它会静默变成 NULL。这里单独一块，两块各有各的主人。
 * 大小 = 内联读的上限（MAX_READ_SECTORS 个扇区）。
 * 代价：常驻一个 4 KiB 的 DMA 页。 */
static long g_stage_obj = -1;
static u64  g_stage_phys;
static void *g_stage_virt;
#define STAGE_BYTES 4096
static u8   g_pio_probe_buf[SECTOR_SIZE];

/* 发布到 devfs 的名字，以及"这块是不是引导链的系统盘"。
 *
 * ★ 为什么这两样必须分开 ★
 * 引导链上只有**系统盘**的第 0 扇区一定带 MBR（那是 mkfat 建镜像时写进去的）；
 * 一块基准数据盘通常是空的。而"驱动能不能读扇区"与"扇区里有没有 MBR"
 * 是两件事——把后者写成驱动级断言，就会让一块**正常工作的空盘**
 * 报出"失败项 2"（实测踩到，看起来像 virtio 读错了数据）。 */
static const char *g_dev_path = "/dev/blk0";
static bool g_system_disk = true;

/* ---- virtio 后端状态（实现在下面） ---- */
static u64 g_vio_phys;          /* MMIO 窗口的**物理**基址（设备视角） */
static u64 g_vio_len;
static u32 g_vio_irq;
static u32 g_vio_off[4];        /* common / notify / isr / device 的 BAR 内偏移 */
static u32 g_vio_mult;          /* notify_off_multiplier */
static void *g_vio_mmio_va;
/* ---- MSI-X（D5c）----
 * ★ 表项由**内核**写，不是驱动 ★ 实测（`docs/14-interrupt-semantics.md` §8.1）
 * 把原设计推翻了：virtio-blk-pci 的 MSI-X 表在 **BAR1**，而这里映射的是
 * **BAR4**（virtio 四类结构那一段）——本驱动手里**根本没有**表项所在的映射，
 * "驱动自己写表项"在真实设备上走不通。内核在自己的 MMIO 窗口里代写那 16 字节，
 * 本驱动只需要等那一位（见下面的 `g_msi_seen`，以及 `vio_backend_init` 里
 * 打印"表项已由内核写入"那段实测输出）。
 * ★ 本文件早先的两处注释写反了（这一处与 `vio_backend_init` 里"用法分三步"那段），
 * 已按实测更正（§8.1）★——同一份代码里两句话打架，比一处错更危险，
 * 所以在这里留一道痕。 */
static int g_vio_msix;                  /* 1 = 命令行给了 --msix */
static u64 g_vio_msix_cap;              /* 能力结构在配置空间里的偏移 */
static u32 g_vio_msix_dev, g_vio_msix_fn;
static u32 g_msi_irq = 0xFFFFFFFFu;     /* MSI 的伪中断号 */
static long g_msi_nt = -1;
static volatile u32 g_msi_seen;

/* 等 MSI 那一位。
 * ★ 为什么用另一个线程 ★ "等中断"与"制造中断"在同一个线程里会互相等死：
 * 主线程必须先发出读请求，设备才会发 MSI。所以让辅助线程去等，
 * 主线程继续发请求——这也是真实驱动里"中断线程 + 服务线程"的雏形。
 *
 * ★ 为什么结尾必须 fe_exit()，不能直接返回 ★
 * 用户线程的入口**没有返回地址**（内核搭的初始栈就是"被调用的函数"那一格，
 * 而那一格是零），所以 entry 一旦 `ret` 就会跳到地址 0 → 取指 #PF。
 * 实测症状：`出错指令 0x0`，而任务名只是 "user"，看不出是谁。
 * 进程主线程靠 `fe_exit` 收尾是既有约定，**用户线程同样要遵守**——
 * 这一条以前没写下来过，因为既有的用户线程（contest/hxtest）都是
 * 显式退出、从不返回，所以从没暴露。 */
static void msi_wait_thread(void *arg)
{
    (void)arg;
    u64 bits = 0;
    if (fe_notification_wait(g_msi_nt, 1ull << g_msi_irq, &bits) == FE_OK &&
        (bits & (1ull << g_msi_irq))) {
        g_msi_seen = 1;
    }
    fe_exit(0);
}

/* ================================================================== */
/* 传输后端：把"块设备服务"与"数据怎么进出设备"分开                    */
/* ================================================================== */
/* ★ 为什么要这一层抽象（而不是在 read 里写 if (virtio) … else …）★
 *
 * 到这一轮为止，blkd 下面已经挂过两种完全不同的传输：
 *   - ATA：端口寄存器 + BMDMA（PRDT 表、引擎时序、物理地址约束）
 *   - virtio：MMIO 寄存器 + virtqueue（描述符链、available/used 环）
 * 它们的**共同点只有一句**："把 N 个扇区从 LBA 搬到这段内存"。
 * 其余一切（寄存器怎么访问、完成怎么判定、缓冲有什么形状要求）
 * 都不同。
 *
 * 把那一句话抽成一个函数指针，换来三件事：
 *   1. blkd 的 IPC 协议、写保护矩阵、请求循环**一行都不用改**，
 *      于是 fsd / blkread / protcheck / blkbench 也全都不用改；
 *   2. 两种后端可以并存（按命令行选），对照实验是同一个客户端跑的，
 *      差异只可能来自后端本身；
 *   3. 将来加 AHCI 只需再填一个 vtable，不动上面任何一层。
 *
 * ★ 代价也要说清 ★
 * 间接调用多一层，而且在"每次请求都要调"的热路径上。实测这一层
 * 相对于设备往返（几十微秒）完全可以忽略——但如果将来做
 * "每扇区一次调用"那种形态，就该重新考虑。 */

/* ---- 面向上层的两个读入口 ----
 *
 * ★ 为什么后端契约同时要虚拟地址与物理地址 ★
 *
 * 这不是冗余，而是一次真实踩坑之后的修正：两种传输对"内存"的需求
 * **本来就不一样**。
 *   - **PIO**（CPU 逐字搬）只认虚拟地址：它要往那个地址写数据；
 *   - **DMA**（设备自己搬）只认物理地址：它按物理地址往内存里写，
 *     而**驱动手里根本没有那个虚拟地址**（客户端的缓冲映射在客户端的
 *     地址空间里，映射到驱动这边只是多此一举）。
 *
 * 第一版把契约定成"只给物理地址"，然后在 ATA 的 PIO 回退路径里
 * 用 `phys + HHDM 偏移` 去算虚拟地址——那是**错的**：用户态不知道
 * 内核的 HHDM 偏移（它是内核的实现常量），算出来的地址指向别处。
 * 症状会是"PIO 回退时数据写到野地址"，而它在有 DMA 的机器上永远不出现。
 *
 * 所以契约改成"两个都给，谁用谁取"：DMA 用 phys，PIO 用 virt。 */
static long backend_read_phys(u32 lba, u32 count, u8 *virt, u64 phys)
{
    if (!g_be || !g_be->read) {
        return FE_ERR_NOTSUP;
    }
    return g_be->read(lba, count, virt, phys) == 0 ? FE_OK : FE_ERR_IO;
}

static long backend_read_to_virt(u32 lba, u32 count, u8 *dst)
{
    if (!dst || count == 0 || count > 8) {
        return FE_ERR_INVAL;
    }
    if (g_be && g_be->phys_only) {
        /* ★ 只认物理地址的后端：设备搬进**我们自己的** DMA 缓冲，
         * 再从那里拷到 dst ★
         *
         * 这一步拷贝是**内联路径本身**的开销，不该被藏起来：
         * 内联读之所以慢，正是因为数据必须经过服务进程。
         * 把它做得"看起来免费"（比如让设备直接写 dst 的虚拟地址数字）
         * 不是优化，是把数据写到别的物理页上——那是一个 bug，
         * 只是在有 DMA 的机器上才显形。
         *
         * 代价：每次内联读多 1 KiB 拷贝 + 一次设备往返。 */
        u32 bytes = count * SECTOR_SIZE;
        if (!g_stage_virt || bytes > STAGE_BYTES) {
            return FE_ERR_NOTSUP;
        }
        long r = backend_read_phys(lba, count, (u8 *)g_stage_virt, g_stage_phys);
        if (r != FE_OK) {
            return r;
        }
        memcpy(dst, g_stage_virt, bytes);
        return FE_OK;
    }
    /* 能写虚拟地址的后端（ATA 的 PIO 路径）：虚拟地址就是落点，
     * phys 只在它真的要 DMA 时才被用到。 */
    return backend_read_phys(lba, count, dst, (u64)(usize)dst);
}

/* 编译器屏障：告诉编译器"这行之前的内存写入必须在这行之后才可能被看见"。
 *
 * ★ 为什么 virtqueue 的非提交**必须有它** ★
 * 设备（或模拟它的线程）可能在任何一刻读 available ring。驱动这边是：
 * 先写描述符 → 再写 ring 项 → 最后写 idx。少了屏障，编译器完全可以把
 * 三次描述符赋值挪到 idx++ 之后——它们在 C 语义里没有依赖关系。
 * 设备于是可能看到"idx 已经变了、描述符还是旧的"，读到的地址是垃圾。
 *
 * x86 的 TSO 保证**存储顺序**不被 CPU 打乱，所以这里只需要挡住编译器；
 * 换到弱序架构（ARM）时同一处要换成真正的硬件屏障指令——
 * 这也是为什么它必须是一个**有名字的动作**，而不是"反正 x86 不会乱序"。
 */
static void fe_mb(void)
{
    __asm__ volatile("" ::: "memory");
}

static void say(const char *s) { fe_puts(s); }

static void step(const char *what, int ok)
{
    say("  [blkd] ");
    say(ok ? "OK   " : "失败 ");
    say(what);
    say("\n");
    if (!ok) {
        g_fail++;
    }
}

static void print_hex8(u8 v)
{
    static const char d[] = "0123456789abcdef";
    char b[2];
    b[0] = d[(v >> 4) & 0xF];
    b[1] = d[v & 0xF];
    fe_write(b, 2);
}

/* 打印 16 位十六进制（0x 前缀）。总线主控窗口的基址是端口号，
 * 只需要 16 位——用 print_hex8 打两次就够，不值得引 fe_printf。 */
static void print_hex16(u32 v)
{
    say("0x");
    print_hex8((u8)(v >> 8));
    print_hex8((u8)(v & 0xFF));
}

/* 回复一次请求。
 *
 * 用户态没有 fe_msg_reply 这个系统调用：**「回复能力」本身就是一条端点**，
 * 收到消息时内核把它交给你，你往它上面发一条普通消息就是回复。
 * 这是端点模型自洽的地方——回复不是特殊操作，就是一次定向发送；
 * 也正因为它是句柄，回复能力可以被转交、可以关闭、可以过期，
 * 不需要再发明一套「谁能回复谁」的规则。
 *
 * 头的 payload_len 必须自己填对：接收方（fe_endpoint_call）靠它确定返回长度，
 * 填 0 就等于回复了一个空消息。 */
static void blk_reply(long reply_ep, const void *data, u32 len)
{
    if (reply_ep <= 0) {
        return;
    }
    struct fe_msg_header rh;
    rh.protocol = 0;
    rh.opcode = 0;
    rh.payload_len = len;
    rh.handle_count = 0;
    rh.request_id = 0;
    fe_endpoint_send(reply_ep, &rh, data, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* 设备访问层：**全部委托给 libdrv/ata.c**                              */
/* ------------------------------------------------------------------ */
/* ★ 为什么这一段从 blkd 里搬走了 ★
 *
 * 原来这里有 180 行"等 BSY / 等 DRQ / 逐扇区搬"的代码，其中
 * 设备控制寄存器（0x3F6）的 400ns 延迟、状态位判定、LBA28 写入顺序
 * 都是 ATA 协议的细节。加上 DMA 之后这些细节会再翻一倍（PRDT、引擎时序、
 * 物理地址约束），而它们**与"块设备服务"要解决的问题毫无关系**。
 *
 * 现在边界是：
 *   libdrv/ata.c —— "怎么把 N 个扇区搬进这段内存"（含 DMA 引擎时序）
 *   blkd         —— IPC 协议、写访问矩阵、把数据交给客户端
 *
 * 这也是"将来的 AHCI / virtio-blk 只换下面那一层"这句话的落点：
 * blkd 里除了 `fe_ata_*` 这几个调用，没有一处知道 ATA 是什么。
 */

/* 逐扇区 PIO 读（保存旧的调用形状，便于对照与降级路径）。
 * 内部走的是"一条命令读一个扇区"，语义与原来完全一致。 */
static int ata_read_sector(u32 lba, u8 *out)
{
    return fe_ata_read_pio_sectors(&g_ch, lba, 1, out) == 1 ? 0 : -1;
}

/* 一批扇区的 PIO 读。★ 这是这一轮性能修复的另一半 ★
 * 原来每 512 字节走一遍完整握手（选盘 → 写 LBA → 写命令 → 等 DRQ），
 * 而 ATA 一条命令本来就能读 256 个扇区——"一次一个"是我们自己的选择。 */
static int ata_read_pio(u32 lba, u32 count, u8 *out)
{
    return fe_ata_read_pio_sectors(&g_ch, lba, count, out) == count ? 0 : -1;
}

/* 一批扇区的 **DMA** 读：数据由设备直接搬进 [phys, +count*512)。 */
static int ata_read_dma(u32 lba, u32 count, u8 *buf, u64 buf_phys)
{
    return fe_ata_read_ex(&g_ch, lba, count, buf, true, buf_phys,
                          g_prd_phys, g_prd_virt, NULL);
}

/* 按"哪一个能表达这段内存"在 DMA 与 PIO 之间选。
 *
 * ★ 判据是**内存形状**，不是"设备行不行" ★
 * DMA 需要物理地址且不跨 64 KiB 边界（PRDT 表达能力的约束）；
 * 形状不合适就退回 PIO——那不代表 DMA 坏了。所以这里的选择
 * 只影响"这一次传输怎么走"，不影响"这台设备有没有 DMA"。 */
static int ata_read_auto(u32 lba, u32 count, u8 *virt, u64 phys)
{
    if (g_dma_ready && virt &&
        fe_ata_dma_covers(phys, (u64)count * SECTOR_SIZE)) {
        return ata_read_dma(lba, count, virt, phys);
    }
    if (!virt) {
        /* 只有物理地址、又用不了 DMA：无法回退（PIO 写不了物理地址）。
         * 明确报出来，而不是拿一个假虚拟地址去写。 */
        return -1;
    }
    return ata_read_pio(lba, count, virt);
}

/* 写一个扇区（保持 PIO）。
 *
 * ★ 为什么写路径**不**跟着改 DMA ★
 * 设备的写缓存把"写完成"与"数据真的在盘上"分开了，所以写吞吐几乎总是
 * 由**缓存策略**决定，而不是由"数据怎么进去"决定：一次 DMA 写与一次
 * PIO 写在有写缓存的盘上是一回事（都由 FLUSH 决定何时落盘）。
 * 而写路径的**正确性**风险高得多（部分写、掉电、保护矩阵），
 * 在没有测量数据说明它值得之前，不动它是更负责的选择。
 * 这一条是**取舍**，不是遗漏——见 docs/12 §9 的边界表。 */
static int ata_write_sector(u32 lba, const u8 *in)
{
    /* 与原来同一套 PIO 写时序，只是搬进了 libdrv（读路径要共用那套状态等待）。
     * ★ 这里**故意不**发 FLUSH CACHE ★ 第一版每个扇区后面都跟一条 FLUSH
     * （"写完就落盘"听起来更安全），结果是 2717 个扇区的拷贝用了 14.3 秒
     * ——每次 FLUSH 都是一次真实的落盘屏障，在虚拟化下就是一次 fsync，
     * 慢到像是卡死（我一开始就是这么误判的）。刷新的意义是"在某个提交点
     * 之前写的东西必须真的在盘上"，而那个提交点只有调用方知道：
     * 对 A/B 更新来说它是**翻引导控制块之前**，全流程只要一次。
     * 所以落盘由 BLK_OP_FLUSH 显式承担。 */
    return fe_ata_write_pio_sectors(&g_ch, lba, 1, in) == 1 ? 0 : -1;
}

/* 刷写缓存（BLK_OP_FLUSH）。返回 0 = 有屏障保证，1 = 刷了但设备不保证。 */
static int ata_flush_cache(void)
{
    return fe_ata_flush(&g_ch);
}

/* IDENTIFY：由 libdrv 做，型号/容量/支持的 DMA 档位一次拿全。
 * blkd 只把它摊到自己原来的两个全局变量上（协议字段不变，
 * 客户端看到的 blk_info 逐字节与上一版一致）。 */
static int ata_identify(void)
{
    int r = fe_ata_init(&g_ch, ATA_CMD_BASE, ATA_CTRL_BASE, 0, true, false);
    if (r != FE_ATA_OK) {
        return -1;
    }
    for (u32 i = 0; i < sizeof(g_model); i++) {
        g_model[i] = (i < FE_ATA_MODEL_MAX) ? g_ch.model[i] : '\0';
    }
    g_model[40] = '\0';
    g_sectors = fe_ata_sector_count(&g_ch);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 批量读：数据走共享内存，内核完全不碰它                              */
/* ------------------------------------------------------------------ */

static u32 g_bulk_calls;        /* 批量读累计次数（诊断/自证用） */
static u64 g_dma_bytes;         /* 经 DMA 搬过的字节数 */
static u64 g_pio_bytes;         /* 经 PIO 搬过的字节数 */

/* ================================================================== */
/* virtio-blk 后端                                                     */
/* ================================================================== */
/* ★ 为什么第二个后端选 virtio 而不是继续修 ATA ★
 *
 * 实测：QEMU 的 IDE 盘 IDENTIFY 报 w88=0x203F —— UDMA 与多字 DMA 的
 * **支持位都是 0**，那条通道上根本没有 DMA 可验。而 virtio-blk：
 *   - 数据通路本来就是 DMA（virtqueue 里每个缓冲区都以物理地址交给设备）；
 *   - 有公开规范（OASIS Virtio 1.x），自己写不涉及抄任何内核代码；
 *   - QEMU 的模拟成熟，双环境里至少 QEMU 侧能给出真数字。
 *
 * ★ 一次请求的布局（三描述符链，规范 §5.2.6）★
 *
 *   [0] 请求头（16 字节）      设备读     ← 只含 type/sector，**不含** status
 *   [1] 数据缓冲（sectors×512）设备**写**  ← 读操作的方向是设备→内存
 *   [2] 状态字节（1 字节）      设备写     ← 三种取值：OK / IOERR / UNSUPP
 *
 * 三块都在同一块 DMA 对象里：头在 0，数据在 +0x1000，状态在 +0x1000+len。
 * 这样只需要一个内存对象、一次物理地址查询，而"状态紧跟在数据之后"
 * 是规范推荐（也是 QEMU 期望）的形态。
 *
 * ★ 为什么要有 g_vio_obj 这块对象，而不直接用客户端交来的缓冲 ★
 * 客户端的缓冲只有物理地址（我们查得到），但**我们写不了它的虚拟地址**——
 * 状态字节与请求头都得由驱动写进内存。而 virtio 的"设备写"目标必须
 * 同时可被设备访问（物理地址）与可被驱动写（虚拟地址）。
 * 所以数据先由设备搬进我们的对象，再拷给客户端的物理地址……
 * 不，那样就多了一次拷贝。见 virtio_read 的说明：**数据直接落在
 * 客户端交来的物理区间上**，我们只额外准备"头 + 状态"那一小块。 */

#define VIO_REQ_HDR_OFF   0x0000u   /* 请求头（16 字节） */
#define VIO_REQ_STAT_OFF  0x0800u   /* 状态字节（1 字节） */
#define VIO_REQ_DATA_OFF  0x1000u   /* 数据区起点 */

/* 我们自己那块"头 + 状态"对象的大小。一页足够，而且**必须一页**：
 * 头与状态都要与数据落在同一个 64 KiB 窗口里，才能用一条描述符链表达。 */
#define VIO_AUX_BYTES     0x1000u

static struct fe_virtio_dev g_vio;
static struct fe_virtq      g_vq;
static long g_vio_aux_obj = -1;
static u64  g_vio_aux_phys;
static u8  *g_vio_aux_virt;
static void *g_vio_mmio_va;



/* 通知设备"队列里有新东西"。
 *
 * ★ 这里有两个容易漏的东西 ★
 *   1. 通知地址 = notify 区偏移 + queue_notify_off × multiplier。
 *      multiplier 来自 capability（规范 §4.1.4.4）。QEMU 上它是 4 而
 *      queue_notify_off 是 0，所以**漏掉 multiplier 在 QEMU 上照样跑**；
 *      到真设备（off != 0）就写到别的队列的地址上去了。
 *   2. 通知的值是**队列号**（16 位）。 */
static void vio_notify(void)
{
    u32 off = vv_r16(g_vio.common, VIRTIO_COMMON_Q_NOFF);
    volatile u16 *p = (volatile u16 *)(void *)
        (g_vio.notify + (u64)off * (g_vio.notify_multiplier ? g_vio.notify_multiplier : 1));
    *p = 0;
}

/* 等设备把第 want_idx 条链的完成项放进 used ring。
 *
 * ★ 判定为什么是 `used->idx != last_used` 而不是 `<` ★
 * 两个下标都是 16 位、靠**自然溢出**回绕。写成 `<` 的实现在第 65536 次
 * 请求之后会永久卡住——而那种"跑很久突然不动了"的现象几乎不可能靠
 * 读代码看出来。这也是本项目"时间/计数一律按回绕语义写"的又一处。 */
static int vio_wait_used(u64 timeout_ns)
{
    u64 t0 = fe_clock_ns();
    while (g_vq.used->idx == g_vq.last_used) {
        if (fe_clock_ns() - t0 > timeout_ns) {
            return FE_ERR_TIMEOUT;
        }
    }
    g_vq.last_used++;
    /* 读一次 ISR 状态寄存器：它同时是"确认中断"（读即清） */
    (void)vv_r8(g_vio.isr, 0);
    return FE_OK;
}

/* 一次读：把 count 个扇区搬到 buf_phys（设备视角的物理地址）。
 *
 * ★ 数据直接落在调用者给的物理区间上，没有中间拷贝 ★
 * 头与状态走我们自己的 aux 对象（它们小、必须由驱动写），
 * 数据走调用者的区间（设备直接 DMA 进去）。所以这条路径上
 * 只有"描述符建立"的开销，没有数据搬运的开销。 */
static int vio_read(u32 lba, u32 count, u64 buf_phys)
{
    if (!g_vio.initialized || !g_vio_aux_virt) {
        return -1;
    }
    u64 data_bytes = (u64)count * SECTOR_SIZE;
    /* 装不下就明确拒绝，而不是截断：截断会让调用者以为读到了全部数据。 */
    if (data_bytes == 0 || data_bytes > 0x100000ull) {
        return -1;
    }

    struct virtio_blk_req_hdr *h =
        (struct virtio_blk_req_hdr *)(void *)(g_vio_aux_virt + VIO_REQ_HDR_OFF);
    h->type = VIRTIO_BLK_T_IN;
    h->reserved = 0;
    h->sector = lba;
    *(volatile u8 *)(g_vio_aux_virt + VIO_REQ_STAT_OFF) = 0xFF;  /* 哨兵 */

    /* 描述符链：头（读）→ 数据（设备写）→ 状态（设备写）。
     * 本实现一次只挂一条链、只用一个描述符组（下标 0/1/2），
     * 所以"一次只允许一个请求在飞"。理由见文件末尾的边界说明。 */
    struct virtq_desc *d = &g_vq.desc[0];
    d[0].addr = g_vio_aux_phys + VIO_REQ_HDR_OFF;
    d[0].len = (u32)sizeof(*h);
    d[0].flags = VIRTQ_DESC_F_NEXT;
    d[0].next = 1;

    d[1].addr = buf_phys;
    d[1].len = (u32)data_bytes;
    d[1].flags = (u16)(VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE);
    d[1].next = 2;

    d[2].addr = g_vio_aux_phys + VIO_REQ_STAT_OFF;
    d[2].len = 1;
    d[2].flags = VIRTQ_DESC_F_WRITE;
    d[2].next = 0;

    /* 提交：avail->ring[idx % size] = 链头，然后 idx++。
     * ★ 顺序不能反，而且中间要有一道屏障 ★
     * 设备可能在任何一刻读 available ring。描述符写在 idx 之前，
     * 屏障保证"设备看到 idx 变化时，描述符内容已经写好了"——
     * 少了它，编译器完全可以把三次 desc 赋值挪到 idx++ 之后。 */
    u16 slot = (u16)(g_vq.avail->idx % g_vq.size);
    g_vq.avail->ring[slot] = 0;
    fe_mb();
    g_vq.avail->idx = (u16)(g_vq.avail->idx + 1);
    fe_mb();
    vio_notify();

    int r = vio_wait_used(500000000ull);    /* 500 ms：覆盖最慢的模拟实现 */
    if (r != FE_OK) {
        return r;
    }
    u8 st = *(volatile u8 *)(g_vio_aux_virt + VIO_REQ_STAT_OFF);
    if (st == VIRTIO_BLK_S_OK) {
        return 0;
    }
    return (st == VIRTIO_BLK_S_UNSUPP) ? -2 : -1;
}

/* virtio-blk 设备的初始化（规范的顺序，一步都不能省也不能调）。
 *
 * ★ 为什么这里**不读** PCI 配置空间 ★
 * 配置空间端口是**独占**资源（两次访问构成一次操作，见 fe_pci.h），
 * 而 pcid 单元在驱动之前就把它认领走了。所以 capability 的解析由
 * **已经持有端口**的 init 做（它用的是 fe_virtio_parse_caps —— 同一份
 * 实现），四个区域偏移量通过命令行交过来。
 *
 * 好处不只是"绕开端口冲突"：这样"谁读配置空间"只有一个答案，
 * 而那次读取还能把原始字段 dump 出来核对（见 init 的日志）。
 *
 * ★ 为什么顺序是有意义的、不是仪式 ★
 * 每一步都是**对设备的声明**：ACKNOWLEDGE = "我看到你了"；
 * DRIVER = "我知道怎么用你"；FEATURES_OK = "下面这些特性我认了"；
 * DRIVER_OK = "可以开始干活了"。设备会在 FEATURES_OK 之后回读这一位，
 * 用来判断"驱动接受的特性组合我能不能满足"——**不回读就不知道协商失败**，
 * 而不检查它的驱动会在设备已经放弃特性协商之后继续发请求。 */
static int vio_init_device(void)
{
    long r = fe_ioport_request(FE_PCI_CFG_ADDR_PORT, FE_PCI_CFG_LEN);
    if (r != FE_OK) {
        /* 端口拿不到不算致命：四个区域偏移已经由 init 通过命令行给全了，
         * 这里只是想顺手 dump 一份 capability 供核对。 */
        say("  [blkd] （PCI 配置空间端口被占用，跳过 capability dump）\n");
    }
    /* 1. 映射 MMIO 窗口（物理地址由 init 探测并申报入池）。 */
    void *va = fe_mmio_map(g_vio_phys, g_vio_len, FE_PROT_READ | FE_PROT_WRITE);
    if (!va) {
        say("  [blkd] virtio MMIO 映射失败（这段区间入池了吗？）\n");
        return -1;
    }
    g_vio_mmio_va = va;
    /* 2. 装配四类结构的地址（偏移量来自命令行）。 */
    fe_virtio_set_regions_direct(&g_vio, va,
                                 (u32)g_vio_off[0], (u32)g_vio_off[1],
                                 (u32)g_vio_off[2], (u32)g_vio_off[3],
                                 (u32)g_vio_mult);
    /* ★ "没找到" 与 "偏移为 0" 是两件事 ★
     * 第一版把这两条写成一个判断（`!found || offset == 0`），
     * 而 common cfg 的偏移**本来就可以是 0** —— 它就是 BAR 的头 4 KiB，
     * QEMU 报的正是 0。于是驱动判自己"没解析出 capability"并退回 ATA，
     * 而 init 打印的参数（偏移 0 12288 4096 8192）完全正确。
     * 判据只能是 `found`：它由解析器在真的读到那个条目时置位。 */
    if (!g_vio.cap[VIRTIO_PCI_CAP_COMMON_CFG].found) {
        say("  [blkd] 没有 common cfg（init 没能从配置空间解析出 capability）\n");
        return -1;
    }
    if (!g_vio.cap[VIRTIO_PCI_CAP_NOTIFY_CFG].found) {
        say("  [blkd] 没有 notify cfg（virtio 1.0 必须有它才能通知设备）\n");
        return -1;
    }
    say("  [blkd] 四类结构已装配：common/notify/isr/device 偏移 = ");
    fe_print_u64(g_vio_off[0]); say(" ");
    fe_print_u64(g_vio_off[1]); say(" ");
    fe_print_u64(g_vio_off[2]); say(" ");
    fe_print_u64(g_vio_off[3]);
    say("  notify 乘数 = ");
    fe_print_u64(g_vio_mult);
    say("\n");

    /* 3. 复位 → ACKNOWLEDGE → DRIVER */
    fe_virtio_reset(&g_vio);
    fe_virtio_add_status(&g_vio, VIRTIO_STATUS_ACKNOWLEDGE);
    fe_virtio_add_status(&g_vio, VIRTIO_STATUS_DRIVER);

    /* 4. 特性协商。
     * ★ 只接受"我确实实现了的" ★ 这一条比看起来重要：
     * 接受一个没实现的特性（例如 INDIRECT_DESC）会让设备按那个语义发请求，
     * 而驱动这边根本没处理——症状是"偶尔读到错的数据"，查起来极贵。 */
    u64 devf = fe_virtio_device_features(&g_vio);
    say("  [blkd] 设备 feature 低位 ");
    fe_print_hex(devf & 0xFFFFFFFFull);
    say("  高位 ");
    fe_print_hex(devf >> 32);
    say("\n");
    u64 want = 0;
    if (devf & (1ull << VIRTIO_F_VERSION_1)) {
        want |= (1ull << VIRTIO_F_VERSION_1);
    } else {
        /* 没有 VERSION_1 说明这是传统接口设备。本驱动的寄存器访问方式
         * 是 v1.0 的，遇到传统设备**明确拒绝**而不是猜着跑。 */
        say("  [blkd] 设备不支持 VIRTIO_F_VERSION_1（传统接口）：本驱动不处理\n");
        fe_virtio_add_status(&g_vio, VIRTIO_STATUS_FAILED);
        return -1;
    }
    if (devf & (1ull << VIRTIO_BLK_F_FLUSH)) {
        want |= (1ull << VIRTIO_BLK_F_FLUSH);   /* 支持屏障，BLK_OP_FLUSH 用得上 */
    }
    fe_virtio_driver_features(&g_vio, want);
    fe_virtio_add_status(&g_vio, VIRTIO_STATUS_FEATURES_OK);
    /* ★ 回读 FEATURES_OK ★ 设备清掉它就表示"这个特性组合我不接受"。
     * 不回读的驱动会在协商已经失败之后继续往下走，然后在某个
     * 说不清的地方出错。 */
    if (!(fe_virtio_status(&g_vio) & VIRTIO_STATUS_FEATURES_OK)) {
        say("  [blkd] 设备拒绝了特性协商（FEATURES_OK 被清）\n");
        fe_virtio_add_status(&g_vio, VIRTIO_STATUS_FAILED);
        return -1;
    }
    g_vio.features = want;

    /* 5. 队列 0：选它、读回**设备给的**长度、把三个环的物理地址写进去 */
    vv_w16((volatile u8 *)g_vio.common, VIRTIO_COMMON_Q_SELECT, 0);
    u16 maxq = vv_r16(g_vio.common, VIRTIO_COMMON_Q_SIZE);
    say("  [blkd] 队列 0 最大长度 ");
    fe_print_u64(maxq);
    say("\n");
    if (maxq < 4) {
        say("  [blkd] 队列太小（三描述符链都放不下）\n");
        fe_virtio_add_status(&g_vio, VIRTIO_STATUS_FAILED);
        return -1;
    }

    long qobj = fe_mem_alloc_dma(0x3000);
    struct fe_mem_info qi;
    if (qobj <= 0 || fe_mem_info(qobj, &qi) != FE_OK) {
        say("  [blkd] 队列内存分配失败\n");
        return -1;
    }
    void *qv = fe_mem_map(qobj, (void *)0, 0x3000, FE_PROT_READ | FE_PROT_WRITE);
    if (!qv) {
        say("  [blkd] 队列内存映射失败\n");
        return -1;
    }
    fe_virtq_init(&g_vq, maxq, qv, qi.phys);
    if (fe_virtq_check(&g_vq) != FE_OK) {
        say("  [blkd] 队列三个环不在同一个 64 KiB 窗口里（设备访问会失真）\n");
        return -1;
    }
    say("  [blkd] 队列内存：描述符 ");
    fe_print_hex(g_vq.desc_phys);
    say("  avail ");
    fe_print_hex(g_vq.avail_phys);
    say("  used ");
    fe_print_hex(g_vq.used_phys);
    say("\n");

    vv_w16((volatile u8 *)g_vio.common, VIRTIO_COMMON_Q_SIZE, g_vq.size);
    vv_w64((volatile u8 *)g_vio.common, VIRTIO_COMMON_Q_DESC, g_vq.desc_phys);
    vv_w64((volatile u8 *)g_vio.common, VIRTIO_COMMON_Q_DRIVER, g_vq.avail_phys);
    vv_w64((volatile u8 *)g_vio.common, VIRTIO_COMMON_Q_DEVICE, g_vq.used_phys);
    vv_w16((volatile u8 *)g_vio.common, VIRTIO_COMMON_Q_ENABLE, 1);
    /* 回读实际生效的队列长度：设备**允许**改小，而我们必须按它的值算环下标 */
    g_vq.size = vv_r16(g_vio.common, VIRTIO_COMMON_Q_SIZE);
    say("  [blkd] 队列已启用，实际长度 ");
    fe_print_u64(g_vq.size);
    say("\n");

    /* 6. 设备专有配置：容量 */
    if (g_vio.cap[VIRTIO_PCI_CAP_DEVICE_CFG].found) {
        g_vio.capacity_sectors = vv_r64(g_vio.device, 0);
        say("  [blkd] virtio-blk 容量 ");
        fe_print_u64(g_vio.capacity_sectors);
        say(" 扇区（");
        fe_print_u64(g_vio.capacity_sectors / 2048);
        say(" MiB）\n");
    }

    /* 7. DRIVER_OK：从这一刻起设备可以开始处理请求 */
    fe_virtio_add_status(&g_vio, VIRTIO_STATUS_DRIVER_OK);
    g_vio.initialized = true;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 两个后端：把上面两种传输塞进同一张 vtable                            */
/* ------------------------------------------------------------------ */

/* ATA 后端：沿用已有的 g_ch / PRDT / DMA 状态。 */
static int ata_backend_init(void)
{
    if (ata_identify() != 0) {
        return -1;
    }
    return 0;
}

static int ata_backend_read(u32 lba, u32 count, u8 *virt, u64 phys)
{
    (void)virt;
    (void)phys;
    /* ATA 的读实现在上面（ata_read_dma / ata_read_pio）。
     * 这里按"哪一个能表达这段内存"在两条路之间选：
     *   - DMA 可用且 PRDT 能表达这段形状 → DMA（物理地址）；
     *   - 否则 → PIO（虚拟地址）。
     * 回退不是错误："DMA 不可用"与"设备不能用"是两件事。 */
    return ata_read_auto(lba, count, virt, phys);
}

/* virtio 后端。 */
static int vio_backend_init(void)
{
    /* aux 对象：请求头 + 状态字节。★ 必须自己有物理地址 ★
     * 设备要写状态字节（DMA），而驱动要写请求头——两个视角缺一不可。 */
    g_vio_aux_obj = fe_mem_alloc_dma(VIO_AUX_BYTES);
    struct fe_mem_info ai;
    if (g_vio_aux_obj <= 0 || fe_mem_info(g_vio_aux_obj, &ai) != FE_OK) {
        say("  [blkd] virtio aux 对象分配失败\n");
        return -1;
    }
    g_vio_aux_phys = ai.phys;
    g_vio_aux_virt = fe_mem_map(g_vio_aux_obj, (void *)0, VIO_AUX_BYTES,
                                FE_PROT_READ | FE_PROT_WRITE);
    if (!g_vio_aux_virt) {
        say("  [blkd] virtio aux 对象映射失败\n");
        return -1;
    }

    if (vio_init_device() != 0) {
        return -1;
    }
    /* ★ g_vio_ok 必须在中断这一段**之前**置上 ★
     * MSI-X 的验证要发一次真实读请求（`vio_read` 会检查这一位），
     * 而"设备真的干了活"正是那条 MSI 消息的来源——没有请求就没有中断，
     * 于是验证会变成"等一个永远不会来的东西"。 */
    g_vio_ok = true;

    /* 中断：认领线并绑到通知对象。
     *
     * ★ 完成判定不依赖它 ★ 见 vio_wait_used：判据是 used ring 的下标
     * （同步、不受中断投递限流影响）。中断在这里的作用是"让等待不必纯自旋"
     * 与"将来做异步队列"。VBox 的中断投递实测只有 49~169 Hz，
     * 把同步判据建在它上面会把每次请求拖到 6~20 ms。
     *
     * ★ 用 FE_IRQ_F_NO_MASK 登记，而且**必须**用（D5a）★
     * IRQ11 在这台机器上是**电平**触发，内核默认会"投递后屏蔽到确认"——
     * 而本驱动**从不 ack**（它不等这个通知，只是顺手绑上），
     * 于是线在第一次中断之后就被永久屏蔽：绑定还在、通知再也不会来。
     * 声明 NO_MASK 才符合它的真实用法：**靠轮询判定完成、只把中断当提示**。
     *
     * 不屏蔽不会引起中断风暴，因为设备侧的条件是被清掉的：
     * `vio_wait_used` 里读了一次 ISR 状态寄存器（读即清）。
     * 那条读原来只是"顺手确认一下"，现在它成了**不屏蔽的前提**——
     * 所以两处必须一起看：这一位与那次读是一对。
     *
     * ★ MSI-X 优先（D5c）★
     * 有 MSI-X 就用它，而不是 INTx：每台设备一条独立消息，
     * 没有共享、没有电平/边沿、不需要 ack。用法分三步——
     *   1. 内核 `fe_irq_msi_alloc` 分配向量、算出消息地址/数据、**并把表项写好**、
     *      置设备的使能位（一次调用里全做完）；
     *   2. ★ **本驱动不写表项** ★——表项已经由第 1 步写好了，**不要再写第二遍**
     *      （本文件早先这里写的是"本驱动把 addr/data 写进第 0 项（表在 BAR4 里）"，
     *      **那句是错的**，见下）；
     *   3. 记下伪中断号，随时可以等 `1 << irq` 那一位。
     *
     * ★ 为什么第 2 步不在驱动里 ★ 原设计的理由是"表在**我们的** BAR 里，
     * 内核不该去映射一段属于别人的 BAR"（`docs/14-interrupt-semantics.md` §4.4）。
     * **实测推翻了它**（同文档 §8.1）：virtio-blk-pci 的 MSI-X 表在 **BAR1**，
     * 而我们只映射了 **BAR4**——**驱动手里根本没有那张表**。要让驱动写，就得再
     * 认领一个 BAR、再映射一次并处理资源池的归属：那不是"更干净的分工"，
     * 是把一次写放大成一条资源链。改法是内核用 `fe_vmm_map_mmio` 把那 16 字节
     * 映进**它自己已有的 MMIO 窗口**写一次，**不产生任何所有权声明**
     * （资源池里 BAR1 归谁完全不受影响）。
     * ★ 所以 `mi.table_bar` / `mi.table_offset` 只是**诊断信息**（下面会打印出来，
     * 让你知道内核把表写在哪个 BAR），**不是"轮到你写"的信号**。★
     *
     * ★ 怎么证明"真的走了 MSI-X" ★
     * 与 D4 的"真的走了 DMA"同一个问题：判据不能是"中断来了"
     * （INTx 也会来）。这里的做法是——**只登记 MSI，完全不登记 INTx**，
     * 然后让一个辅助线程去等那一位，主线程发一次真实读请求。
     * 那一位被置上就只可能是 MSI 送来的：那条路是唯一存在的路。 */
    if (g_vio_msix && g_vio_msix_cap) {
        long nt = fe_notification_create();
        struct fe_msi_info mi;
        long r = (nt > 0)
            ? fe_irq_msi_alloc(nt, 0, g_vio_msix_dev, g_vio_msix_fn,
                               g_vio_msix_cap, &mi)
            : nt;
        say("  [blkd] MSI-X 使能：");
        if (r != FE_OK) {
            say("**失败**（返回 ");
            fe_print_u64((u64)(-r));
            say("），退回 INTx\n");
        } else {
            say("伪中断 ");
            fe_print_u64(mi.irq);
            say(" 向量 ");
            fe_print_u64(mi.vector);
            say("，表在 BAR");
            fe_print_u64(mi.table_bar);
            say(" 偏移 ");
            fe_print_u64(mi.table_offset);
            say("，addr ");
            fe_print_hex(mi.addr);
            say(" data ");
            fe_print_hex(mi.data);
            say("\n");
            /* 表项由**内核**写（用内核自己的 MMIO 窗口）。
             * 驱动只需要在这里等那一位，并制造真实 I/O。 */
            say("  [blkd] MSI-X 表项已由内核写入（表在 BAR");
            fe_print_u64(mi.table_bar);
            say("，内核用自己已有的 MMIO 窗口写，不需要驱动去认领那个 BAR）\n");

            /* ★ 还要告诉**设备**去用 MSI-X 的第 0 项 ★
             * 这一步落在 virtio 协议层，所以只有驱动能做：
             * common config 里 `queue_msix_vector`（0x1A）的默认值是
             * **0xFFFF = 不用 MSI-X、走 INTx**。只把表项写好、使能位置上，
             * 设备照样不会去发那条消息——实测症状就是"表写对了、中断不来"。
             * 同一件事有两个开关（PCI 的使能位 + virtio 的向量号），
             * 少一个都不生效，而它们分属两层，各由该管的人管。 */
            vv_w16((volatile u8 *)g_vio.common, VIRTIO_COMMON_MSIX, 0);
            vv_w16((volatile u8 *)g_vio.common, VIRTIO_COMMON_Q_SELECT, 0);
            vv_w16((volatile u8 *)g_vio.common, VIRTIO_COMMON_Q_MSIX, 0);
            u16 qv = vv_r16(g_vio.common, VIRTIO_COMMON_Q_MSIX);
            say("  [blkd] 已告诉设备用 MSI-X 表项 0（回读 queue_msix_vector=");
            fe_print_u64(qv);
            say(qv == 0 ? "，设备确认了" : "，**设备没接受**");
            say("）\n");

            g_msi_irq = (u32)mi.irq;
            g_msi_nt = nt;
            /* ★ 辅助线程去等那一位，主线程**发一次真实读请求** ★
             * 光等是等不来的：设备只有真的干了活才会写那条消息。
             * 这一对（等的人 + 干活的人）就是真实驱动里
             * "中断线程 + 服务线程"的雏形；放在同一个线程里会互相等死。 */
            u64 th = fe_thread_create(msi_wait_thread, NULL, NULL, 0);
            if (th) {
                /* 先让辅助线程跑到"已经在等"的状态，再发请求。
                 * 不睡这一下的话，请求可能在它登记之前就完成、
                 * 那一位被置上而没人取——症状是"MSI 明明生效却说没到"。 */
                fe_sleep_ms(20);
                long rr = vio_read(0, 1, g_prd_phys);
                for (u32 i = 0; i < 500 && !g_msi_seen; i++) {
                    fe_sleep_ms(2);
                }
                say("  [blkd] MSI-X 中断");
                if (g_msi_seen) {
                    say("**已到达**（辅助线程被那一位唤醒；读请求");
                    say(rr == 0 ? "成功" : "失败");
                    say("）—— 这条路上**没有登记 INTx**，"
                        "所以只可能是 MSI-X 送来的\n");
                    /* ★ 验证完就**还回去**，这是刻意的 ★
                     *
                     * 真实驱动应当常开 MSI-X（它比 INTx 严格更好：每台设备
                     * 一条独立消息、没有共享、不需要 ack）。这里却把它关掉，
                     * 理由是本套件里还有一条**用 INTx 共享线**做真实路径的
                     * 测试（`irqtest` 让两个任务共享 IRQ11）——而 MSI-X 一旦
                     * 使能，设备就**不再拉 INTx**，那条测试会收不到任何中断。
                     *
                     * 也就是说：**一个设备的中断方式只有一种**，两条真实路径
                     * 测试抢的是同一个开关。选择是"留哪条测试"：
                     * 留共享线，因为 MSI-X 的证据已经在上面这行拿到了
                     * （"没有登记 INTx 却收到中断"是不可伪造的），
                     * 而共享线需要设备持续拉 INTx 才能被验。
                     *
                     * 代价写在这里：**这个驱动的稳定状态仍是 INTx**，
                     * 于是"中断只是提示、完成靠轮询"那条设计不变。 */
                    long fr = fe_irq_msi_free((u32)mi.irq);
                    say("  [blkd] MSI-X 已关掉（返回 ");
                    fe_print_u64((u64)(fr == FE_OK ? 0 : 1));
                    say("）：设备退回 INTx，好让 irqtest 的**共享线**测试"
                        "也能拿到真实中断\n");
                } else {
                    say("**没有到达** —— MSI-X 没有真正生效\n");
                }
            } else {
                say("  [blkd] 辅助线程创建失败，无法验证 MSI-X 投递\n");
            }
        }
    } else if (g_vio_irq > 0 && g_vio_irq < 255) {
        long nt = fe_notification_create();
        struct fe_irq_info ii;
        long ir = (nt > 0)
            ? fe_irq_register_ex(g_vio_irq, nt, FE_IRQ_F_NO_MASK, &ii)
            : nt;
        say("  [blkd] virtio 中断线 ");
        fe_print_u64(g_vio_irq);
        if (ir == FE_OK) {
            say(" 已认领并绑定通知对象（");
            say((ii.mode & FE_IRQ_MODE_LEVEL) ? "电平" : "边沿");
            say("，不屏蔽，共享者 ");
            fe_print_u64(ii.sharers);
            say("）\n");
        } else {
            say(" 认领失败（不影响轮询完成）\n");
        }
    }

    return 0;
}

static int vio_backend_read(u32 lba, u32 count, u8 *virt, u64 phys)
{
    (void)virt;
    if (!g_vio_ok) {
        return -1;
    }
    /* virtio 只需要物理地址：数据由设备**直接**搬进去，
     * 驱动不碰它的虚拟地址（那块内存在客户端的地址空间里）。 */
    return vio_read(lba, count, phys);
}

static const struct blk_backend g_ata_backend = {
    .name = "ATA(PIO/BMDMA)",
    .init = ata_backend_init,
    .read = ata_backend_read,
    /* ★ dma 是**能力声明**，不是"现在走的是 DMA" ★
     * ATA 后端的 DMA 可用性要等初始化时真的试一次才知道，所以这里由
     * g_dma_ready 动态决定——见 backend_has_dma()。 */
    .dma = false,
    /* PIO 路径写虚拟地址，所以整体上"能写虚拟地址"。
     * （BMDMA 那一支不能，但它有 PIO 可退——见 phys_only 的说明。） */
    .phys_only = false,
};

static const struct blk_backend g_vio_backend = {
    .name = "virtio-blk",
    .init = vio_backend_init,
    .read = vio_backend_read,
    .dma = true,
    /* 见 phys_only 的说明：设备自己搬，只认物理地址。 */
    .phys_only = true,
};

static bool backend_has_dma(void)
{
    if (g_be == &g_vio_backend) {
        return g_vio_ok;
    }
    return g_dma_ready;
}

/* ------------------------------------------------------------------ */
/* 主流程                                                              */
/* ------------------------------------------------------------------ */

/* 解析十六进制字面值（0x 前缀可选）。找不到合法数字时返回 0。 */
static u64 parse_hex(const char *s)
{
    u64 v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    while (*s) {
        u32 d;
        char c = *s++;
        if (c >= '0' && c <= '9') {
            d = (u32)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (u32)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = (u32)(c - 'A' + 10);
        } else {
            break;
        }
        v = (v << 4) | d;
    }
    return v;
}

/* ---- 面向上层的两个读入口 ----
 *
 * ★ 为什么后端契约同时要虚拟地址与物理地址 ★
 *
 * 这不是冗余，而是一次真实踩坑之后的修正：两种传输对"内存"的需求
 * **本来就不一样**。
 *   - **PIO**（CPU 逐字搬）只认虚拟地址：它要往那个地址写数据；
 *   - **DMA**（设备自己搬）只认物理地址：它按物理地址往内存里写，
 *     而**驱动手里根本没有那个虚拟地址**（客户端的缓冲映射在客户端的
 *     地址空间里，映射到驱动这边只是多此一举）。
 *
 * 第一版把契约定成"只给物理地址"，然后在 ATA 的 PIO 回退路径里
 * 用 `phys + HHDM 偏移` 去算虚拟地址——那是**错的**：用户态不知道
 * 内核的 HHDM 偏移（它是内核的实现常量），算出来的地址指向别处。
 * 症状会是"PIO 回退时数据写到野地址"，而它在有 DMA 的机器上永远不出现。
 *
 * 所以契约改成"两个都给，谁用谁取"：DMA 用 phys，PIO 用 virt。 */
static bool arg_is(const char *a, const char *flag)
{
    u32 i = 0;
    for (; flag[i]; i++) {
        if (a[i] != flag[i]) {
            return false;
        }
    }
    return a[i] == '\0';
}

/* ---- ATA 的总线主控设置 ----
 *
 * ★ 少了任何一步都会"静默降级"，所以每一步都单独报 ★
 *   1. 没有 --bar4：设备管理器没把总线主控窗口交过来 → 用 PIO；
 *   2. 认领失败：BAR4 不在资源池里，或已被别人持有；
 *   3. 设备说它不支持 DMA（SET FEATURES 被拒）→ 用 PIO；
 *   4. SET FEATURES 接受了、但传输没真的发生 → 用 PIO。
 * 四种情况都是"能工作但慢"，所以必须是**日志里看得见的一行**，
 * 而不是一个只有性能数字会变的现象。
 *
 * 第 4 条是这一版新加的：DMA 命令会"成功完成"而一个字节都不搬
 * （见 libdrv/ata.c 的 fe_ata_verify_dma），只有真的搬一次、比对内容
 * 才知道。这是把"支持与否"从**声明**变成**事实**。 */
/* 批量读的实现放在请求循环之后（读代码的人先看到协议，再看细节）。 */
static void blk_bulk(long reply, const struct fe_msg_header *hdr,
                     const u8 *payload, const u32 *handles, u32 hcount, u64 who);

static void blk_ata_dma_setup(u64 bar4)
{
    if (!bar4) {
        say("  [blkd] 没有 --bar4：本次只用 PIO（DMA 需要设备管理器先申报 BAR4）\n");
        return;
    }
    say("  [blkd] 总线主控窗口 ");
    print_hex16((u32)bar4);
    say("（由设备管理器探测并授权）\n");
    long r = fe_ioport_request(bar4, 16);
    step("认领总线主控寄存器块（16 字节）", r == FE_OK);
    if (r != FE_OK) {
        return;
    }
    g_ch.bm_base = (u16)bar4;
    int er = fe_ata_enable_dma(&g_ch);
    /* ★ 这里**不用 step()**（它会把失败计进失败项）★
     * "设备不支持 DMA"是**环境事实**，不是内核/驱动的缺陷。
     * 把它算成失败会让 blkd 的失败项永远是 1，于是那一行汇总数字
     * 从"哪里有问题"退化成"这台机器没有 DMA"。 */
    if (er != FE_ATA_OK) {
        say("  [blkd] 设备不支持 DMA 模式（SET FEATURES 被拒）"
            "——这是设备能力，不是缺陷；下面用 PIO\n");
        /* ★ 被拒的命令会把 ERR 位置起来，而它是**粘滞**的：之后每条命令
         * 的状态检查都会失败——症状是"探完 DMA 之后连 PIO 都读不了盘"。
         * 实测踩到的就是这个，所以失败路径上必须软复位。 */
        int rr = fe_ata_soft_reset(&g_ch);
        say("  [blkd] 已软复位恢复（");
        say(rr == FE_ATA_OK ? "成功" : "**复位也失败**");
        say("）\n");
        return;
    }
    step("让设备切到 DMA 模式（SET FEATURES）", 1);

    /* 描述符表要单独一块 DMA 内存（PRDT 自己也得有物理地址）。 */
    struct fe_mem_info mi;
    g_prd_obj = fe_mem_alloc_dma(4096);
    if (g_prd_obj > 0 && fe_mem_info(g_prd_obj, &mi) == FE_OK) {
        g_prd_phys = mi.phys;
        g_prd_virt = fe_mem_map(g_prd_obj, (void *)0, 4096,
                                FE_PROT_READ | FE_PROT_WRITE);
    }
    /* 验证缓冲：目标先写成 0xA5，于是"没搬"在比对时立刻现形
     * （内存对象分配时被内核清零，"没搬"与"搬进来一片零"本无法区分）。 */
    {
        struct fe_mem_info di;
        g_dma_probe_obj = fe_mem_alloc_dma(SECTOR_SIZE);
        if (g_dma_probe_obj > 0 && fe_mem_info(g_dma_probe_obj, &di) == FE_OK) {
            g_dma_probe_phys = di.phys;
            g_dma_probe_buf = fe_mem_map(g_dma_probe_obj, (void *)0,
                                         SECTOR_SIZE,
                                         FE_PROT_READ | FE_PROT_WRITE);
        }
    }
    if (!g_prd_virt || !g_dma_probe_buf) {
        say("  [blkd] DMA 验证缓冲准备失败：退回 PIO\n");
        return;
    }
    for (u32 i = 0; i < SECTOR_SIZE; i++) {
        g_dma_probe_buf[i] = 0xA5;
    }
    int vr = fe_ata_verify_dma(&g_ch, 0, 1, g_pio_probe_buf,
                               g_dma_probe_buf, g_dma_probe_phys,
                               g_prd_virt, g_prd_phys);
    if (vr == FE_ATA_OK) {
        g_dma_ready = true;
    }
    step("用一次真实传输验证 DMA（与 PIO 结果逐字节一致）", vr == FE_ATA_OK);
    if (vr != FE_ATA_OK) {
        say("  [blkd] DMA 验证失败（错误码 ");
        fe_print_u64((u64)(-vr));
        say("）：退回 PIO。设备自报的模式掩码 UDMA=");
        fe_print_u64(g_ch.udma_mask);
        say(" 多字DMA=");
        fe_print_u64(g_ch.mwdma_mask);
        say("\n");
        /* 那次 DMA 命令可能把设备留在错误状态，一样要复位。 */
        (void)fe_ata_soft_reset(&g_ch);
    }
}

int main(int argc, char **argv)
{
    u64 bar4 = 0;
    u64 vio_base = 0, vio_len = 0, vio_irq = 0;
    u64 vio_off_c = 0, vio_off_n = 0, vio_off_i = 0, vio_off_d = 0, vio_mult = 0;
    for (int i = 1; i < argc; i++) {
        if (arg_is(argv[i], "--bar4") && i + 1 < argc) {
            bar4 = parse_hex(argv[++i]);
        } else if (arg_is(argv[i], "--virtio") && i + 8 < argc) {
            /* ★ 一个标志带 **8 个值**，其中最后一个就是中断线 ★
             *
             *   --virtio <基址> <长度> <common> <notify> <isr> <device> <乘数> <irq>
             *
             * 为什么收成一个标志而不是"7 个值 + 另一个 --virtio-irq"：
             * 这类"标志 + N 个值"最容易出的错就是**数目对不上**——
             * 少消费一个值，后面的值就会被当成新标志去匹配，于是
             * 参数静默地错位、走另一条分支（我在这里踩过：
             * 只消费 2 个值，结果"后端选成 ATA"而日志看不出为什么）。
             * 一个标志 + 固定 8 个值，让"数目"只在一处出现，
             * 而且 `i + 8 < argc` 这一条就把它检查掉了。 */
            vio_base = parse_hex(argv[++i]);
            vio_len = parse_hex(argv[++i]);
            vio_off_c = parse_hex(argv[++i]);
            vio_off_n = parse_hex(argv[++i]);
            vio_off_i = parse_hex(argv[++i]);
            vio_off_d = parse_hex(argv[++i]);
            vio_mult = parse_hex(argv[++i]);
            vio_irq = parse_hex(argv[++i]);
        } else if (arg_is(argv[i], "--msix") && i + 2 < argc) {
            /* --msix <能力偏移> <设备号>，设备号 = (bus<<8)|(dev<<3)|fn。
             * 这两个值都是**设备管理器探测出来的**（端口在它手里），
             * 驱动只负责用（D5c 的分工：发现是策略、使能是机制）。 */
            g_vio_msix = 1;
            g_vio_msix_cap = parse_hex(argv[++i]);
            u64 bd = parse_hex(argv[++i]);
            g_vio_msix_dev = (u32)((bd >> 3) & 0x1F);
            g_vio_msix_fn = (u32)(bd & 0x07);
        } else if (arg_is(argv[i], "--publish") && i + 1 < argc) {
            /* 发布到哪个 devfs 名字（默认 /dev/blk0 = 系统盘）。 */
            g_dev_path = argv[++i];
        } else if (arg_is(argv[i], "--data-disk")) {
            /* 这是一块**数据盘**：不做"必须有 MBR"那条断言。
             * 理由见 g_system_disk 的说明——那是盘的性质，不是驱动的契约。 */
            g_system_disk = false;
        }
    }
    g_vio_off[0] = (u32)vio_off_c;
    g_vio_off[1] = (u32)vio_off_n;
    g_vio_off[2] = (u32)vio_off_i;
    g_vio_off[3] = (u32)vio_off_d;
    g_vio_mult = (u32)vio_mult;

    /* 把选到的后端与关键参数打出来。
     * ★ "后端选错了"与"后端起不来"在日志上完全不同，而前者只有这一行能看出来 ★ */
    say("  [blkd] \u53c2\u6570: --bar4=");
    fe_print_hex(bar4);
    say(" --virtio=\u57fa\u5740 ");
    fe_print_hex(vio_base);
    say(" \u957f\u5ea6 ");
    fe_print_u64(vio_len);
    say(" \u504f\u79fb ");
    fe_print_u64(vio_off_c);
    say(" ");
    fe_print_u64(vio_off_n);
    say(" ");
    fe_print_u64(vio_off_i);
    say(" ");
    fe_print_u64(vio_off_d);
    say(" \u4e58\u6570 ");
    fe_print_u64(vio_mult);
    say(" irq ");
    fe_print_u64(vio_irq);
    say("\n");

    /* ---- 选后端 ----
     *
     * ★ 为什么"有 virtio 就用 virtio"★
     * virtio-blk 的数据通路本来就是 DMA（virtqueue 的每个缓冲区都以
     * 物理地址交给设备），而 QEMU 的 IDE 盘实测**不支持 DMA**
     * （IDENTIFY w88=0x203F）。所以两者都在时报出来的话，virtio 是
     * 唯一能验证"DMA + 共享内存批量"那条路的后端。
     * 两个都没有时退回 ATA-PIO —— 那条路一直可用，只是慢。 */
    if (vio_base && vio_irq) {
        g_be = &g_vio_backend;
    } else {
        g_be = &g_ata_backend;
    }

    /* 内联读的垫脚缓冲：**不分后端，一律先分配好**。
     *
     * ★ 为什么不在 virtio 分支里分配 ★
     * 因为"要垫一块"这件事由 phys_only 决定，而 phys_only 是后端选出来
     * 之后才知道的。放在这里，两个事实（选了谁、谁能写虚拟地址）在
     * 同一处对齐；放进分支里就会出现"换了后端、垫脚缓冲忘了分配"
     * 这种只在一条路径上才显形的漏洞。
     * 分配失败**不致命**：ATA 后端根本不需要它，那时候它只是 4 KiB 白占。
     * 真需要它而又没有时，内联读会明确返回 NOTSUP，而不是写坏内存。 */
    {
        g_stage_obj = fe_mem_alloc_dma(STAGE_BYTES);
        struct fe_mem_info si;
        if (g_stage_obj > 0 && fe_mem_info(g_stage_obj, &si) == FE_OK) {
            g_stage_phys = si.phys;
            g_stage_virt = fe_mem_map(g_stage_obj, (void *)0, STAGE_BYTES,
                                      FE_PROT_READ | FE_PROT_WRITE);
        }
        if (g_be->phys_only && !g_stage_virt) {
            say("  [blkd] 内联读垫脚缓冲分配失败：内联读将不可用\n");
        }
    }

    say("\n[blkd] 块设备服务启动（ring 3，后端 ");
    say(g_be->name);
    say("）\n");

    if (g_be == &g_ata_backend) {
        long r = fe_ioport_request(ATA_CMD_BASE, ATA_CMD_LEN);
        step("认领 ATA 命令块 0x1F0..0x1F7", r == FE_OK);
        if (r != FE_OK) {
            return 1;
        }
        r = fe_ioport_request(ATA_CTRL_BASE, ATA_CTRL_LEN);
        step("认领 ATA 控制块 0x3F6", r == FE_OK);
        if (r != FE_OK) {
            return 1;
        }
        /* IRQ14：中断线本身是**资源**，认领了别人才拿不到。
         *
         * ★ 注意它现在的双重角色 ★
         * 完成判定读的是 BMDMA 状态位（同步、不受中断投递限流影响），
         * 但中断照样投递到通知对象——于是"设备完成了"这件事同时能从两条路
         * 看到：状态位是判据，中断是可选的通知源（将来的异步队列要用它）。
         * VBox 的中断投递实测只有 49~169 Hz，把同步判据建在它上面会把
         * 每次传输拖到 6~20 ms —— 那是实测数据，不是猜测。 */
        long nt = fe_notification_create();
        r = (nt > 0) ? fe_irq_register(ATA_IRQ, nt) : nt;
        step("认领 IRQ14（中断投递到通知对象）", r == FE_OK);

        if (ata_identify() != 0) {
            step("IDENTIFY 主盘", 0);
            say("  [blkd] 没有可用的 ATA 主盘，退出\n");
            return 1;
        }
        step("IDENTIFY 主盘", 1);
        say("  [blkd] 型号 \"");
        say(g_model);
        say("\"，LBA28 容量 ");
        fe_print_u64(g_sectors);
        say(" 扇区（");
        fe_print_u64(g_sectors / 2048);
        say(" MiB）\n");
        say("  [blkd] 设备支持的传输模式: UDMA 掩码 ");
        print_hex8(g_ch.udma_mask);
        say("  多字DMA 掩码 ");
        print_hex8(g_ch.mwdma_mask);
        say("\n");
        blk_ata_dma_setup(bar4);
    } else {
        /* ---- virtio 后端 ---- */
        g_vio_phys = vio_base;
        g_vio_len = vio_len;
        g_vio_irq = (u32)vio_irq;
        if (g_be->init() != 0) {
            /* ★ 后端起不来时**不退出**，退回 ATA ★
             * "virtio 没起来"与"没有块设备"是两件事：这台机器上 IDE
             * 一直可用，退回去至少能让文件系统、写保护这些上层继续验。
             * 但这个退回必须**在日志里大声说出来** —— 否则后面的
             * 吞吐数字会被当成 virtio 的成绩。 */
            say("  [blkd] virtio 后端初始化失败：退回 ATA 后端（性能会低很多）\n");
            g_be = &g_ata_backend;
            long r2 = fe_ioport_request(ATA_CMD_BASE, ATA_CMD_LEN);
            if (r2 != FE_OK ||
                fe_ioport_request(ATA_CTRL_BASE, ATA_CTRL_LEN) != FE_OK) {
                say("  [blkd] ATA 端口也认领不到，退出\n");
                return 1;
            }
            if (ata_identify() != 0) {
                say("  [blkd] ATA 也认不出来，退出\n");
                return 1;
            }
            blk_ata_dma_setup(bar4);
        } else {
            g_sectors = g_vio.capacity_sectors;
            /* 型号：virtio-blk 没有 ATA 那种 IDENTIFY 字符串，
             * 但规范给了 VIRTIO_BLK_T_GET_ID 查询设备序列号。
             * 今天不查它——块设备服务用不到型号，而"能少一次
             * 设备往返"在初始化路径上也值一点。 */
            const char *m = "virtio-blk (PCI 1af4:1001)";
            for (u32 i = 0; i < sizeof(g_model) - 1 && m[i]; i++) {
                g_model[i] = m[i];
            }
            step("virtio-blk 初始化（能力解析 → 特性协商 → 队列 → DRIVER_OK）", 1);
        }
    }

    /* 读第 0 扇区。这是「端口/MMIO 权限真的生效 + 时序正确」最直接的证据。
     *
     * ★ 这里**只**校验 MBR，不解析 FAT32 的 BPB。★
     * 分区表与文件系统是块设备**之上**的概念，属于 fsd 的职责。
     * 我第一版在这里读 BPB 并且读出来全是 0——因为 LBA 0 是 MBR，
     * FAT32 引导扇区在分区起始处（本镜像里是 LBA 2048）。
     * 那个错误本身就把这条分层边界指出来了：驱动只管扇区，
     * 谁想知道扇区里是什么意思，谁自己去解析。 */
    u8 sec[SECTOR_SIZE];
    long br = backend_read_to_virt(0, 1, sec);
    if (br != 0) {
        step("读 LBA 0", 0);
        say("  [blkd] 读失败，错误码 ");
        fe_print_u64((u64)(-br));
        say("\n");
        return 1;
    }
    step("读 LBA 0", 1);

    /* ★ "第 0 扇区有 MBR" 不是驱动的不变式，它只是**系统盘**的性质 ★
     *
     * 第一版把签名与分区表都写成 step()（失败即计失败项），而在 virtio
     * 后端上那块盘是**基准数据盘**——它本来就是空的（稀疏全零文件），
     * 第 0 扇区当然没有 55AA。于是驱动报"失败项 2"，看起来像 virtio 读错了，
     * 而实际上 `读 LBA 0` 是 OK 的：**数据真的搬进来了，只是搬的是零**。
     *
     * 分层的道理：块设备驱动只保证"能读回扇区"。扇区里是什么，
     * 由盘决定、由上层解释（这正是这个文件开头就写着的那条边界）。
     * 所以这里只在**系统盘**上做这条断言（那是引导链装进去的），
     * 其它盘只把观察到的内容打出来。 */
    int sig_ok = (sec[510] == 0x55 && sec[511] == 0xAA);
    say("  [blkd] LBA 0 签名 ");
    print_hex8(sec[511]);
    print_hex8(sec[510]);
    if (g_system_disk) {
        say(sig_ok ? "  （55AA，通过）\n" : "  （签名不对）\n");
        if (!sig_ok) {
            g_fail++;
        }
    } else {
        say(sig_ok ? "  （55AA）\n" : "  （没有 MBR —— 这是数据盘，属正常）\n");
    }

    /* 打印分区表。驱动不该解释分区内容，但把原始表项报出来是有用的诊断：
     * 上层解析出问题时，能立刻分辨是「磁盘上没写对」还是「解析写错了」。 */
    u32 parts = 0;
    for (int i = 0; i < 4; i++) {
        const u8 *e = &sec[446 + i * 16];
        u32 start = (u32)(e[8] | (e[9] << 8) | (e[10] << 16) | (e[11] << 24));
        u32 count = (u32)(e[12] | (e[13] << 8) | (e[14] << 16) | (e[15] << 24));
        if (e[4] == 0 && start == 0 && count == 0) {
            continue;
        }
        say("  [blkd] 分区 ");
        fe_print_u64((u64)i);
        say(": 类型 ");
        print_hex8(e[4]);
        say(", 起始 LBA ");
        fe_print_u64(start);
        say(", ");
        fe_print_u64(count);
        say(" 扇区\n");
        parts++;
    }
    if (g_system_disk) {
        step("MBR 里存在分区表项", parts > 0);
    } else {
        say("  [blkd] 分区表项 ");
        fe_print_u64(parts);
        say(" 个（数据盘，不判定）\n");
    }

    /* 发布到 devfs —— 这就是服务发现的全部：
     * 其它服务/程序按这个名字就能找到我。
     * ★ 名字是一个**构造参数** ★ "系统盘"与"基准数据盘"是两个设备，
     * 各自发布自己的名字（/dev/blk0 与 /dev/vblk0）——把两者混成一个
     * 名字，会让"文件系统挂在哪个盘上"变成一个隐式的运行时事实。 */
    long ep = fe_endpoint_create(0);
    step("创建服务端点", ep > 0);
    if (ep <= 0) {
        return 1;
    }
    long pub = fe_devfs_publish(g_dev_path, ep);
    step("发布块设备名字", pub == FE_OK);
    say("  [blkd] 发布为 ");
    say(g_dev_path);
    say("\n");

    say("  [blkd] 初始化结束，失败项 ");
    fe_print_u64(g_fail);
    say("；进入请求服务循环\n");

    /* ---- 请求循环 ---- */
    for (;;) {
        struct fe_msg_header hdr;
        static u8 payload[2 * SECTOR_SIZE + 64];
        u32 in_handles[FE_MSG_MAX_HANDLES];
        u32 in_count = 0;
        long reply = fe_endpoint_recv(ep, &hdr, payload, sizeof(payload),
                                      in_handles, &in_count);
        /* ★ recv 的返回值**不是错误码**：< 0 才是错误，0 只表示
         * "这条消息没带回复能力"（发送方用 send 而不是 call）★
         *
         * 第一版写成 `reply <= 0 → continue`，那会把所有用 send 发来的
         * 请求**静默丢掉**：客户端在回复端点上等到天荒地老，而服务端
         * 看起来"什么都没收到"。而这个协议恰恰必须用 send
         * （要随消息传句柄，call 的 ABI 不带句柄数组）。
         *
         * 同一个 ABI 语义坑在本项目里出现过三次了（hxtest 注释里记着一次），
         * 所以这里把话写死在代码里：**返回值的含义必须按定义读。** */
        if (reply < 0) {
            continue;
        }
        /* 没带回复能力，而这条协议要求"每条请求都有回信地址"——
         * 没有就明确丢弃并记一行，而不是继续往下走去回复一个空的句柄。 */
        if (reply == 0 && hdr.payload_len >= sizeof(struct blk_bulk_req) &&
            payload[0] == BLK_OP_READ_BULK && in_count < 1) {
            say("  [blkd] 批量读请求没有随消息带回复端点，丢弃\n");
            continue;
        }
        /* ★ 收到的句柄必须**无条件**关掉 ★
         * 无论这次请求走哪条分支、成功还是失败：句柄是引用计数的一份，
         * 不关就是每次请求泄漏一个（而内存对象持有整段物理内存）。
         * 所以先记下来，循环末尾统一关——中间的分支一个都不用记得这件事。 */
        if (hdr.payload_len < sizeof(struct blk_req)) {
            blk_reply(reply, 0, 0);
            for (u32 i = 0; i < in_count; i++) {
                fe_handle_close(in_handles[i]);
            }
            fe_handle_close(reply);
            continue;
        }
        struct blk_req req;
        for (u32 i = 0; i < sizeof(req); i++) {
            ((u8 *)&req)[i] = payload[i];
        }

        /* ★ 访问矩阵在这里执行 ★
         *
         * hdr.sender_task 是**内核填的**，发送方伪造不了——这就是为什么
         * 服务能按客户端做策略。内核只回答"这个请求方能不能碰这段扇区"，
         * 它看不到 IPC 的语义流向，所以 requester 只能由 blkd 提供。
         *
         * 也就是说：**强制点是 blkd**。一个不问的 blkd 就绕过了整张表，
         * 这正是"A 槽只读、B 槽连读都不行"这条保证的边界所在。 */
        u64 who = hdr.sender_task;

        if (req.op == BLK_OP_INFO) {
            struct blk_info info;
            info.sectors = g_sectors;
            info.sector_size = SECTOR_SIZE;
            info._pad = 0;
            /* ★ 报的是"哪一个后端 + 它拿到的硬件窗口" ★
             * 客户端据此能独立验证两件事：
             *   1. 走的是哪条路（backend）；
             *   2. 那段硬件**真的存在且已被独占**（拿它去认领会拿到"忙"）。
             * 吞吐数字本身证明不了数据是 DMA 搬的，这两条能。 */
            if (g_be == &g_vio_backend && g_vio_ok) {
                info.bar4 = 0;
                info.dma_mode = 1;
                info.mmio = g_vio_phys;
                info.mmio_len = g_vio_len;
                info.backend = 2;
            } else {
                info.bar4 = g_dma_ready ? (u32)g_ch.bm_base : 0;
                info.dma_mode = g_dma_ready ? 1u : 0u;
                info.mmio = 0;
                info.mmio_len = 0;
                info.backend = 1;
            }
            info._pad2 = 0;
            for (u32 i = 0; i < sizeof(info.model); i++) {
                info.model[i] = (i < 40) ? g_model[i] : '\0';
            }
            blk_reply(reply, &info, sizeof(info));
        } else if (req.op == BLK_OP_READ && req.count >= 1 && req.count <= MAX_READ_SECTORS) {
            /* 一次最多回 MAX_READ_SECTORS 个扇区：上限由 IPC 内联载荷决定
             * （FE_MSG_MAX_PAYLOAD = 1024 = 2 个扇区），不是驱动能力决定的。
             *
             * 这不是长久方案。真正的批量传输应该走「随消息传一个内存对象、
             * 由服务端直接写进去」，让内核完全不碰数据——那件事和 DMA 一起做。
             * 现在够用，因为 mkfat.py 建出来的卷每簇只有 1 个扇区。 */
            if (fe_protect_check(req.lba, req.count, 0, who) != FE_OK) {
                blk_reply(reply, 0, 0);     /* 内核已记下这次违规 */
                fe_handle_close(reply);
                continue;
            }
            static u8 buf[MAX_READ_SECTORS * SECTOR_SIZE];
            /* ★ 内联读也走**后端抽象**，而不是直接调 ATA ★
             * 它原本是 `for (i) ata_read_sector(...)`——逐扇区一条命令，
             * 每次都要重新选盘 + 写 LBA + 发命令 + 等 DRQ。这里改成一
             * 条命令读完整段（ATA 的扇区计数字段支持 1..256）。
             * 分段再往上做只会重复 blk_bulk 已经写清楚的逻辑。 */
            int ok = (backend_read_to_virt((u32)req.lba, req.count, buf) == FE_OK);
            blk_reply(reply, ok ? buf : 0, ok ? req.count * SECTOR_SIZE : 0);
        } else if (req.op == BLK_OP_WRITE && req.count >= 1 &&
                   req.count <= MAX_WRITE_SECTORS) {
            /* 写请求的载荷是「请求头 + 数据」——数据紧跟在 blk_req 之后 */
            u32 need = (u32)sizeof(struct blk_req) + req.count * SECTOR_SIZE;
            if (hdr.payload_len < need) {
                blk_reply(reply, 0, 0);
                fe_handle_close(reply);
                continue;
            }
            if (fe_protect_check(req.lba, req.count, 1, who) != FE_OK) {
                blk_reply(reply, 0, 0);     /* 内核已记下这次违规 */
                fe_handle_close(reply);
                continue;
            }
            const u8 *data = payload + sizeof(struct blk_req);
            int ok = 1;
            for (u32 i = 0; i < req.count; i++) {
                if (ata_write_sector((u32)req.lba + i, data + i * SECTOR_SIZE) != 0) {
                    ok = 0;
                    break;
                }
            }
            u32 status = ok ? 1u : 0u;
            blk_reply(reply, &status, sizeof(status));
        } else if (req.op == BLK_OP_FLUSH) {
            /* 刷新不需要保护检查：它不读也不写任何一段扇区，
             * 只是要求"把已经写下去的东西落到盘上"。放它过去不会让
             * 任何人多看到或多改一个字节。 */
            int fr = ata_flush_cache();
            u32 status = (fr == 0) ? 1u : ((fr == 1) ? 2u : 0u);
            /* 1 = 成功；2 = 没有屏障保证（驱动器不支持）；0 = 失败 */
            blk_reply(reply, &status, sizeof(status));
        } else if (req.op == BLK_OP_READ_BULK) {
            blk_bulk(reply, &hdr, payload, in_handles, in_count, who);
        } else {
            blk_reply(reply, 0, 0);      /* 不支持的请求（含超上限的多扇区） */
        }
        /* ★ 收到的句柄**无条件**全部关掉 ★
         * 无论走哪条分支、成功还是失败：句柄是引用计数的一份，不关就是
         * 每次请求泄漏一个（而内存对象持有整段物理内存）。统一在这里关，
         * 中间的分支一个都不用记得这件事。 */
        for (u32 i = 0; i < in_count; i++) {
            fe_handle_close(in_handles[i]);
        }
        fe_handle_close(reply);
    }
}

/* ------------------------------------------------------------------ */
/* 批量读的实现（放在请求循环之后：读代码的人先看到协议，再看细节）    */
/* ------------------------------------------------------------------ */

/* 把 [lba, lba+sectors) 读进一个**设备能访问的物理区间**。
 *
 * ★ 这里没有任何一次数据拷贝 ★
 * 设备直接把数据 DMA 进客户端交过来的那块内存。这条路径上：
 *   客户端内存 → （设备搬）→ 客户端内存
 * 驱动只是"告诉设备往哪搬"，数据一个字节都没经过驱动、也没经过内核。
 * 对比原来的路径（设备 → 驱动缓冲 → IPC 载荷 → 内核拷贝 → 客户端），
 * 那一次是 5 段拷贝、每段上限 1024 字节。
 *
 * 分段规则来自**设备**的上限，取小：
 *   - ATA：一条命令最多 256 个扇区（扇区计数字段 8 位），且缓冲要能被
 *     PRDT 表达（不跨 64 KiB 边界）；
 *   - virtio：描述符链可以跨多个 64 KiB 窗口，但一次仍按设备的
 *     seg_max 限制分段（本驱动保守取 256 扇区）。
 * 两个后端的"一段多大"都由同一个常量回答，所以这里不再需要知道是哪个。 */
#define BULK_CHUNK_SECTORS 256

static int blk_read_bulk(u32 lba, u32 sectors, u64 buf_phys)
{
    u32 done = 0;
    while (done < sectors) {
        u32 chunk = sectors - done;
        if (chunk > BULK_CHUNK_SECTORS) {
            chunk = BULK_CHUNK_SECTORS;
        }
        u64 phys = buf_phys + (u64)done * SECTOR_SIZE;
        /* ★ 这里只给物理地址、虚拟地址传 NULL ★
         * 批量路径的数据落在**客户端**的内存里，驱动既不该也不方便
         * 拿到它的虚拟地址（那需要把客户端的缓冲映射进驱动的地址空间，
         * 纯属多此一举）。真需要虚拟地址的后端（ATA 的 PIO 回退）
         * 在这一支里表达不了，于是**明确失败**——见 ata_read_auto。 */
        long r = backend_read_phys(lba + done, chunk, NULL, phys);
        if (r != FE_OK) {
            return (r == FE_ERR_NOTSUP) ? FE_ERR_NOTSUP : r;
        }
        g_dma_bytes += (u64)chunk * SECTOR_SIZE;
        done += chunk;
    }
    return FE_OK;
}

/* 没有 DMA 时的降级路径：用一小块自己的内存读，再拷进客户端缓冲。
 *
 * ★ 为什么保留它（而不是"没 DMA 就拒绝批量读"）★
 * "DMA 不可用"与"设备不能用"是两件事。降级路径让同一套客户端代码在
 * 两种情形下都能跑，只是慢——这也正是**对照实验**需要的：
 * 同一台机器上 PIO 批量与 DMA 批量读同一段数据，吞吐差多少，
 * 就是这次改动值多少的直接证据（见 blkbench 的输出）。
 *
 * 代价是这里有**一次拷贝**（设备 → 我们的缓冲 → 客户端缓冲），
 * 所以它是降级，不是等价物。
 *
 * ★ 它不是 ATA 专用的 ★ 因为"读进我自己的缓冲再拷出去"这件事
 * 与用哪种传输无关：任何后端都能做，所以它走的是同一张 vtable。 */
#define PIO_CHUNK_SECTORS 8     /* 8 × 512 = 4 KiB，正好是描述符表内存的大小 */

static int blk_read_bulk_pio(u32 lba, u32 sectors, long buf_handle, u64 want)
{
    /* ★ 映射的长度必须是**整段**，而且不能超过对象本身 ★
     * 第一版按 4096 映射、却按 128 KiB 去写——越界写会打穿到别的映射。
     * 这里用 want（调用者已核对过 want <= 对象真实大小）。 */
    void *dst = fe_mem_map(buf_handle, (void *)0, want, FE_PROT_READ | FE_PROT_WRITE);
    if (!dst) {
        return FE_ERR_ACCESS;
    }
    u32 done = 0;
    while (done < sectors) {
        u32 chunk = sectors - done;
        if (chunk > PIO_CHUNK_SECTORS) {
            chunk = PIO_CHUNK_SECTORS;
        }
        /* 读进我们自己的 4 KiB 缓冲（g_prd_virt 就是那块：DMA 不用它时
         * 它正好空着，而它**有物理地址**，所以两种后端都能往里写）。 */
        long r = backend_read_phys(lba + done, chunk, (u8 *)g_prd_virt,
                                   g_prd_phys);
        if (r != FE_OK) {
            say("  [blkd] 降级批量读失败：lba=");
            fe_print_u64(lba + done);
            say(" 扇区=");
            fe_print_u64(chunk);
            say(" 错误=");
            fe_print_u64((u64)(-r));
            say("\n");
            return FE_ERR_IO;
        }
        /* ★ 用 memcpy 而不是逐字节循环 ★
         * 逐字节的版本在 4 KiB 上要 4096 次迭代、每次一次 load+store，
         * 而这个项目里 libfe 的 memcpy 是过了 SIMD 优化的那一份
         * （见 docs/07 §6：缓存内 5.3 → 36 GB/s）。
         * 降级路径本来就慢，没必要再自己把它拖慢一倍。 */
        memcpy((u8 *)dst + (usize)done * SECTOR_SIZE, g_prd_virt,
               (usize)chunk * SECTOR_SIZE);
        g_pio_bytes += (u64)chunk * SECTOR_SIZE;
        done += chunk;
    }
    return FE_OK;
}

/* 处理一次批量读请求。载荷 = struct blk_bulk_req，随消息带 2 个句柄：
 *   [0] 客户端建的**临时回复端点**（状态回在它上面）
 *   [1] 客户端分配的一块 **DMA** 内存对象（数据就落在它里面）
 *
 * ★ 为什么回复端点要由客户端显式交过来 ★
 * 内核的"回复能力随消息投递"只在 `fe_endpoint_call` 那条路径上带
 * （`send` 不带）。而这条请求必须带句柄，所以用不了 call。
 * 自己建一个临时端点、当普通句柄传过来，是同一套机制的直接复用
 * ——"回复能力就是一条端点"（见 docs/09）。
 *
 * ★ 顺序：[0] 回复、[1] 数据 ★ 为什么不是反过来：这条协议里
 * "回复"是**每条请求都必须有的**，而数据缓冲也是；把必需的放前面，
 * 将来若要加可选句柄就往后排，老客户端不用改。
 *
 * ★ 为什么描述符表不给客户端管 ★
 * 第一版想让客户端也交一块"描述符表内存"过来，理由是"让请求方能看到
 * 设备被指向了哪里"。那是没必要的：表只有驱动需要写，而**表本身也要有
 * 物理地址**这件事驱动自己就能满足（启动时分配一次、长期复用）。
 * 多要一个句柄只是把一次请求的失败面从 1 个变成 2 个。
 *
 * ★ 状态码 ★ 1 = DMA 搬完；2 = 降级（无 DMA，数据仍然对）；
 * 3 = 参数/形状不合法；4 = 设备或内存查询出错；0 = 请求本身非法。 */
static void blk_bulk(long reply, const struct fe_msg_header *hdr,
                     const u8 *payload, const u32 *handles, u32 hcount, u64 who)
{
    u32 status = 0;
    u64 client_reply = reply;   /* 默认用内核给的回复能力（走 call 时才有） */
    struct blk_bulk_req b;

    if (hdr->payload_len < sizeof(b) || hcount < 1) {
        /* 句柄是硬要求：数据要落进客户端的内存，那就必须由客户端交过来。
         * 少了它**明确拒绝**，不去猜"是不是想让我自己分配再拷回去"
         * ——那正是这条协议要避免的老路。 */
        blk_reply(reply, &status, sizeof(status));
        return;
    }
    for (u32 i = 0; i < sizeof(b); i++) {
        ((u8 *)&b)[i] = payload[i];
    }
    /* 客户端交来的临时回复端点（send 路径必然带它）。
     * 内核给的 reply 只在 `call` 路径上才有效，那时 handles 为空。 */
    if (hcount >= 1 && handles[0] != FE_HANDLE_INVALID) {
        client_reply = (long)handles[0];
    }
    if (b.op != BLK_OP_READ_BULK || b.sectors == 0) {
        blk_reply(client_reply, &status, sizeof(status));
        return;
    }

    /* ---- 参数护栏（**不信任客户端声明的任何长度**）---- */
    u64 want = (u64)b.sectors * SECTOR_SIZE;
    if (want > BULK_MAX_BYTES || (u64)b.lba + b.sectors > g_sectors) {
        status = 3;
        /* ★ 这一支原来回在 `reply` 上，是个**挂起**的 bug ★
         * `reply` 是内核在 `call` 路径上给的回复能力；而批量读走的是
         * `send`（它才能带句柄），这条路上 `reply` **根本无效**。
         * 于是越界请求的应答被扔进一个不是端点的地方，
         * 客户端在它自己建的回复端点上**永远等不到**——症状是
         * "越界读把整个程序挂住"，而不是"越界读被拒"。
         * 这个 bug 只在反向对照里显形：正向路径每一支用的都是
         * client_reply，所以怎么测吞吐都是好的。 */
        blk_reply(client_reply, &status, sizeof(status));
        return;
    }
    /* 保护矩阵：与内联读走**同一个**检查。★ 这一条不能只在一条路径上做 ★
     * 否则"批量读"就成了绕过"另一个槽禁读"的后门——而被绕过的正是
     * A/B 更新的安全基础。 */
    if (fe_protect_check(b.lba, b.sectors, 0, who) != FE_OK) {
        blk_reply(client_reply, &status, sizeof(status));   /* 内核已记下违规 */
        return;
    }
    if (b.buf_len < want) {
        status = 3;
        blk_reply(client_reply, &status, sizeof(status));
        return;
    }

    /* ---- 拿到缓冲的物理地址 ----
     *
     * ★ 这一段是本轮内核侧机制的唯一使用者 ★
     * `fe_mem_info` 只对 FE_MEM_FLAG_DMA 的对象开放——它挡住的正是
     * "拿普通内存冒充 DMA 缓冲"：否则等于给驱动一个"任意内存的物理地址"
     * 查询接口，那是拿内存保护换方便（见 kernel/include/fe/syscall.h）。
     * 而"物理连续"是设备的前提：没有 IOMMU 时散页在设备眼里拼不起来。 */
    struct fe_mem_info bi;
    if (hcount < 2 || fe_mem_info((long)handles[1], &bi) != FE_OK) {
        status = 4;
        blk_reply(client_reply, &status, sizeof(status));
        return;
    }
    /* ★ 客户端声明的长度要与对象**真实大小**相符 ★
     * 只信 b.buf_len 的话，客户端可以声明 512 KiB 而对象只有 4 KiB：
     * 设备会照着我们给的长度往对象外面写。BULK_MAX_BYTES 那道护栏
     * 挡不住这个（它管的是协议上限，不是这块内存有多大）。 */
    if (bi.size < want) {
        status = 3;
        blk_reply(client_reply, &status, sizeof(status));
        return;
    }

    int sr;
    if (backend_has_dma()) {
        sr = blk_read_bulk((u32)b.lba, b.sectors, bi.phys);
    } else {
        sr = blk_read_bulk_pio((u32)b.lba, b.sectors, (long)handles[1], want);
        if (sr == FE_OK) {
            sr = FE_ERR_NOTSUP;     /* 如实告诉客户端"这次走的不是 DMA" */
        }
    }
    g_bulk_calls++;
    status = (sr == FE_OK) ? 1u
           : (sr == FE_ERR_NOTSUP) ? 2u
           : (sr == FE_ERR_RANGE) ? 3u
           : 4u;
    blk_reply(client_reply, &status, sizeof(status));
}

