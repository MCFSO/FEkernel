/* SPDX-License-Identifier: 0BSD */
/* init —— 用户态第一个进程（ring 3）。
 *
 * ★ 它只做三件事 ★
 *   1. 把系统拉起来：按**声明的依赖**算出启动顺序，逐个拉起单元；
 *   2. 管 A/B 槽：读/写槽状态，触发更新器，必要时重启或回滚；
 *   3. 把引导期的策略（谁是设备管理器、硬件窗口归谁）落到资源池里。
 *
 * ★ 为什么启动顺序是"算出来的"而不是"排好的" ★
 * 硬编码一个数组是"人肉依赖注入"：服务一多，顺序靠人排，漏一次就是
 * 偶发启动失败——而失败现场（某服务打不开某个名字）离真正的原因
 * （它被排到了提供者前面）隔着好几层。所以每个单元**声明** PROVIDE 与
 * REQUIRE，顺序由 init 用 Kahn 拓扑排序算出来（学 BSD 的 rc.d/rcorder）。
 * 日志里"声明顺序 ≠ 启动顺序"本身就是排序生效的证据。
 *
 * ★ 为什么设备发现也在这里（D4）★
 * 内核不认识 "devmgr" 这个名字（那是策略），它只回答一个更小的问题：
 * **谁是引导者**。答案是 init —— 内核 exec 的第一个用户任务。
 * 所以设备管理器身份只能由 init 自己认领（`fe_devmgr_claim`），
 * 而"把哪一段硬件放进池子"这件事必须在**启动任何单元之前**做完：
 * 驱动的命令行里就要带这些地址，而它们只能由探测得到（PCI BAR 的地址
 * 由固件分配，两个环境不一样——这正是不能写死的原因）。
 *
 * ★ 它不做什么 ★
 * 内核里那套"机制/策略"的分界在这里同样成立：init 只**申报**硬件、
 * **不操作**硬件。真正碰设备的是驱动（blkd / fsd / consoled …）。
 */
#include <fe_user.h>
#include <fe_blk.h>
#include <fe_pci.h>             /* 配置空间访问 + 按规范探测 BAR 长度 */
#include <fe_virtio.h>          /* virtio 1.0 的 capability 解析（与驱动共用同一份） */

#define MAX_DEPS 4

/* 运行期前提（见 struct unit 的 needs）。
 * ★ 用位标志而不是函数指针 ★ 表里的东西应当**一眼能读出来**：
 * "这个单元要什么硬件"是一句声明，不是一个需要追进去看的回调。
 * 将来有第二个前提时，这里是加一位，而不是加一个机制。 */
#define NEED_VIRTIO_BLK 1u

/* ---- 全局状态（集中在这里，便于"谁在读谁在写"一眼看全）---- */
static u32 g_fail;
static u32 g_start_order[64];
static u32 g_order_len;
static int g_ready[64];                 /* 该单元的 provides 是否已出现 */
static char g_prefix[32] = "";          /* 槽前缀，如 "/slot_a" */

/* A/B */
static struct fe_ab_info g_ab;
static int g_ab_ok;
static int g_ab_update_wanted;
static int g_reboot_wanted;
static long g_ab_blk = -1;              /* /dev/blk0 的端点（A/B 要读写扇区） */

/* D4：设备管理器探测到的硬件窗口 */
static bool g_busmaster_ready;
static char g_bar4_arg[8];
static char g_bar4_val[8];

static u64 g_vio_mmio;
static u64 g_vio_len;
static u32 g_vio_irq;
static bool g_vio_ready;
static u32 g_vio_off[4];                /* common / notify / isr / device */
static u32 g_vio_mult;                  /* notify_off_multiplier */
static u32 g_vio_msix_off;              /* MSI-X 能力在配置空间里的偏移（0 = 没有） */
static u32 g_vio_msi_off;               /* MSI   能力在配置空间里的偏移（0 = 没有） */
static char g_vio_arg[12];
static char g_vio_base[20];
static char g_vio_size[20];
static char g_vio_irq_val[12];
static char g_vio_c_off[12];
static char g_vio_n_off[12];
static char g_vio_i_off[12];
static char g_vio_d_off[12];
static char g_vio_mult_s[12];
static char g_vio_msix_arg[10];
static char g_vio_msix_val[12];
static char g_vio_bd_val[12];
static u32  g_vio_dev;                  /* virtio-blk 在 PCI 上的设备号 */
static u32  g_vio_fn;

static void ab_state_machine(void);
static void ab_mark_successful(void);
static void ab_read_cmdline(void);
static void ab_write_record(u32 seq, u8 active, u8 successful, u8 attempts);

/* ================================================================== */
/* 打印                                                                */
/* ================================================================== */
/* 这一组小函数而不是直接用 printf：init 的绝大多数输出都是"标签 + 数字 +
 * 标签 + 数字"的形状，用 printf 会把格式串写得到处都是，而格式串里
 * 打错一个转换符是**运行期**才现形的错误（本项目已经为此浪费过时间——
 * `fe_kprintf` 的 `%u` 配 u64 会打印垃圾并把后面的输出吞掉）。
 * 显式调用虽然啰嗦，但每个参数的类型在编译期就被检查了。 */

static void say(const char *s) { fe_puts(s); }
static void num(u64 v) { fe_print_u64(v); }
static void hex4(u32 v)
{
    static const char d[] = "0123456789abcdef";
    char b[6];
    b[0] = '0'; b[1] = 'x';
    b[2] = d[(v >> 12) & 0xF];
    b[3] = d[(v >> 8) & 0xF];
    b[4] = d[(v >> 4) & 0xF];
    b[5] = d[v & 0xF];
    fe_write(b, 6);
}

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

/* 十六进制字面值 → 字符串（带 0x 前缀）。 */
static void hex_to_str(u64 v, char *out, u32 cap)
{
    static const char d[] = "0123456789abcdef";
    char tmp[17];
    u32 n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    }
    while (v && n < 16) {
        tmp[n++] = d[v & 0xF];
        v >>= 4;
    }
    u32 k = 0;
    if (cap > 3) {
        out[k++] = '0';
        out[k++] = 'x';
    }
    for (u32 i = 0; i < n && k + 1 < cap; i++) {
        out[k++] = tmp[n - 1 - i];
    }
    out[k] = '\0';
}

/* 把字面值拷进固定缓冲（不用 strlcpy：这些缓冲的大小都是编译期已知的）。 */
static void copy_lit(char *dst, const char *src, u32 cap)
{
    u32 i = 0;
    for (; src[i] && i + 1 < cap; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* ================================================================== */
/* 设备管理器（D4）：发现 → 申报 → 交回端口                            */
/* ================================================================== */
/* ★ 这一步补的是 D3b 与 D4 之间缺的那一环 ★
 *
 * pcid（D3b）能"看见"PCI 设备的 BAR，但 BAR **不在资源池里**，
 * 所以驱动拿着地址也认领不到（fe_ioport_request 返回 NOENT）。
 * 缺的三件事都在用户态：
 *
 *     发现   读配置空间，算出 BAR 的类别与长度        ← 这里
 *     申报   把那段区间交给内核入池（需要 DM 身份）    ← 这里
 *     认领   驱动按常规路径去认领                      ← 驱动
 *
 * 内核在这条链上仍然只做三件事：**谁在说**（是不是设备管理器）、
 * **说的合不合法**（范围/对齐/不与可用内存相交）、**记下来**。
 * 它不认识"IDE 控制器"或"virtio-blk"——那才是"内核里不写驱动"的落点。 */

/* 找到"带 I/O BAR4 的 IDE 控制器"。
 *
 * ★ 判据为什么是"BAR4 是 I/O 空间"，而不是"类代码 == 0x01"★
 * 第一版两条都要：类别必须是 STORAGE/IDE。结果**一个都找不到**——
 * 而我当时打印出来的类别字节是错的，于是"类别不匹配"掩盖了
 * "BAR4 明明就在那里（类型 1、长度 16）"这个事实。
 * 教训很具体：**判据要建立在自己能验证的量上**。BAR4 的类型与长度是
 * 探出来的（写全 1 读回取反），它自证；而类别读在我算错的字节里。
 * PIIX 的 IDE 功能就是唯一带 I/O BAR4 的那个功能，所以这条判据更稳。 */
static u16 bm_find_bar4(u8 *out_fn, u64 *out_len)
{
    for (u8 fn = 0; fn < 8; fn++) {
        if (!fe_pci_present(0, 1, fn)) {
            continue;
        }
        u32 type = 0;
        u64 len = fe_pci_bar_probe(0, 1, fn, 4, &type);
        if (type != FE_PCI_BAR_IO || len == 0 || len > 64) {
            continue;
        }
        u32 id = fe_pci_cfg_read32(0, 1, fn, FE_PCI_REG_VENDOR_ID);
        say("[devmgr] 候选功能 0:1.");
        num(fn);
        say("  厂商 ");
        num(id & 0xFFFF);
        say(" 设备 ");
        num((id >> 16) & 0xFFFF);
        say("  BAR4 长度 ");
        num(len);
        say("\n");
        u32 bar4 = fe_pci_cfg_read32(0, 1, fn, 0x10 + 16);
        *out_fn = fn;
        *out_len = len;
        return (u16)(bar4 & 0xFFFCu);
    }
    return 0;
}


/* 找 virtio-blk，把它的 MMIO 窗口申报入池。
 *
 * ★ 为什么是 virtio 而不是继续修 IDE ★
 * 实测：QEMU 的 IDE 盘 IDENTIFY 报 w88=0x203F —— UDMA 与多字 DMA 的
 * **支持位都是 0**，那条通道上根本没有 DMA 可验。virtio-blk 的数据通路
 * 本来就是 DMA（virtqueue 里每个缓冲区都以物理地址交给设备），
 * 而且有公开规范（OASIS Virtio 1.x），自己写不涉及抄任何内核代码。
 *
 * ★ 为什么 capability 解析放在这里而不是驱动里 ★
 * 配置空间端口是**独占**资源（两次访问构成一次操作，见 fe_pci.h），
 * 而 pcid 单元在驱动之前就把它认领走了。让驱动去抢只会失败，
 * 于是由**已经持有端口**的这一步把四类结构的偏移量解析出来、
 * 通过命令行交给驱动。这样"谁读配置空间"只有一个答案，
 * 而那次读取的原始字节还能被核对（见 fe_virtio.h 的解析器注释）。 */
static bool virtio_blk_probe(void)
{
    u8 dev = 0, fn = 0;
    u32 bar_index = 0, bar_val = 0;
    u64 bar_len = 0;
    bool found = false;

    for (u8 d = 0; d < 32 && !found; d++) {
        for (u8 f = 0; f < 8; f++) {
            if (!fe_pci_present(0, d, f)) {
                continue;
            }
            u32 id = fe_pci_cfg_read32(0, d, f, FE_PCI_REG_VENDOR_ID);
            if ((id & 0xFFFF) != 0x1AF4u) {
                continue;               /* 不是 virtio 设备 */
            }
            if (((id >> 16) & 0xFFFF) != 0x1001u) {
                continue;               /* 不是 virtio-blk */
            }
            /* 挑**第一个内存 BAR**：virtio 1.0 把四类结构
             * （common/notify/isr/device）都放在同一个 MMIO 窗口里，
             * 具体位置由 capability 描述。传统设备的 BAR0 可能是 I/O 空间，
             * 所以按类型挑而不是按编号挑。 */
            for (u32 bi = 0; bi < 6; bi++) {
                u32 t = 0;
                u64 l = fe_pci_bar_probe(0, d, f, bi, &t);
                if (t == FE_PCI_BAR_MEM && l >= 0x1000) {
                    bar_index = bi;
                    bar_val = fe_pci_cfg_read32(0, d, f, (u8)(0x10 + bi * 4));
                    bar_len = l;
                    found = true;
                    break;
                }
            }
            if (found) {
                dev = d;
                fn = f;
                break;
            }
        }
    }
    if (!found) {
        say("[devmgr] 没找到 virtio-blk（厂商 1af4 设备 1001）："
            "基准测试只能走 IDE 那条没有 DMA 的路\n");
        return false;
    }

    /* 64 位 BAR（低 4 位 == 0x4）要把高 32 位拼起来。只取低 32 位在真机上
     * 会得到一个**看起来也合法**的错地址，症状是"映射成功但读回全 1"。 */
    u32 v = bar_val & 0xFFFFFFF0u;
    if ((bar_val & 0x7u) == 0x4u) {
        u32 hi = fe_pci_cfg_read32(0, dev, fn, (u8)(0x10 + (bar_index + 1) * 4));
        g_vio_mmio = ((u64)hi << 32) | (u64)v;
    } else {
        g_vio_mmio = (u64)v;
    }
    /* 长度按页向上取整：MMIO 映射与池子里的区间都按页对齐，
     * 不对齐的话驱动认领时永远匹配不上。 */
    g_vio_len = (bar_len + 0xFFFull) & ~0xFFFull;
    if (g_vio_len == 0) {
        g_vio_len = 0x1000;
    }
    /* ★ 中断线只取低字节 ★
     * 0x3C 是"中断线"、0x3D 是"中断引脚"，读 16 位会把引脚也读进来
     * （实测读到 0x010B 而不是 0x0B）。 */
    g_vio_irq = (u32)(fe_pci_cfg_read16(0, dev, fn, FE_PCI_REG_IRQ_LINE) & 0xFF);
    g_vio_dev = dev;
    g_vio_fn = fn;

    say("[devmgr] virtio-blk ");
    num(dev); say("."); num(fn);
    say("  MMIO ");
    hex4((u32)(g_vio_mmio >> 32));
    hex4((u32)(g_vio_mmio >> 16));
    hex4((u32)(g_vio_mmio & 0xFFFF));
    say(" + ");
    num(g_vio_len);
    say("  中断线 ");
    num(g_vio_irq);
    say("\n");

    /* 申报 MMIO 入池。★ 只入池、不认领 ★
     * MMIO 池条目是**独占**的：init 认领了，驱动就拿不到——
     * 而需要这个映射的是驱动（它要读写设备寄存器）。
     * 内核会自己再查一遍"这段是不是可用内存"，那一条是机制层的护栏，
     * 不能只写在入口（docs/12 §4 的教训）。 */
    if (fe_resource_pool_add(FE_RES_MMIO, g_vio_mmio, g_vio_len, 0) != FE_OK) {
        say("[devmgr] virtio MMIO 申报入池失败\n");
        return false;
    }
    /* IRQ 不在引导期池子里（它随设备走），所以也要申报。
     * 认领同样留给驱动——只有它知道自己要不要中断。
     *
     * ★ 申报为**可共享**（D5a）★
     * PCI 的 INTx 在物理上就是**可以被多条设备共用**的一根线。申报成独占
     * 的话，同一条线上第二台设备的驱动会在认领那一步被拒——而"这条线
     * 接了几台设备"是**拓扑**，是设备管理器知道的事，不是内核该管的。
     * 内核只如实执行"可共享/独占"这个决定，并按物理约束裁决：
     * **只有电平线**允许共享（边沿共享会丢中断，见 docs/14 §2.1）。
     * 这台机器上 IRQ11 按 ACPI 是电平触发，所以它确实可以共享。 */
    if (g_vio_irq > 0 && g_vio_irq < 255) {
        (void)fe_resource_pool_add(FE_RES_IRQ, g_vio_irq, 1, 1 /* shared */);
    }

    /* capability 解析：把设备的原始字节读出来交给共享的解析器。
     * ★ 数组下标就是**配置空间偏移**，链往哪个方向走都不影响解析 ★ */
    {
        /* ★ 缓冲必须能装下**任意** capability 偏移 ★
         * PCI 的 capability 头指针是配置空间里的字节偏移，合法范围是整个
         * 256 字节的配置空间头（0x40..0xFF 是设备自定义区）。第一版开了
         * 80 字节的数组，而 QEMU 给的 cap_ptr 是 **152** —— 于是循环的
         * 边界检查一上来就为假、一次都不执行，症状是"解析失败"，
         * 而真正的原因是**我自己的缓冲太小**。
         * 这类"自己的缓冲比对方给的偏移小"的错误没有任何提示，
         * 所以这里直接按 256 开满。 */
        u8 raw[256];
        u32 nraw = 0;
        u16 st = fe_pci_cfg_read16(0, dev, fn, FE_PCI_REG_STATUS);
        if (st & 0x10) {
            u32 guard = 0;
            u8 cap = (u8)(fe_pci_cfg_read32(0, dev, fn, 0x34) & 0xFF);
            /* 每个 capability **20 字节**（规范 §4.1.4）：
             *   +0 cap_vndr / +1 next / +2 cap_len / +3 cfg_type
             *   +4 bar / +5 id / +6..7 padding
             *   +8  offset (u32) / +12 length (u32)
             *   +16 notify_off_multiplier (u32，只有 NOTIFY 有)
             * ★ 必须一次读满 20 字节 ★ 第一版只读了 4 字节（"头"），
             * 于是 offset/length 全部落在我没读的地方、解析出来全是 0——
             * 而 found 位却是对的（type 就在头里），症状是
             * "四个区域都找到了，但地址全是 0"。 */
            while (cap && guard++ < 64 && (u32)cap + 20 <= sizeof(raw)) {
                for (u32 k = 0; k < 20; k += 4) {
                    u32 w = fe_pci_cfg_read32(0, dev, fn, (u8)(cap + k));
                    raw[cap + k + 0] = (u8)(w & 0xFF);
                    raw[cap + k + 1] = (u8)((w >> 8) & 0xFF);
                    raw[cap + k + 2] = (u8)((w >> 16) & 0xFF);
                    raw[cap + k + 3] = (u8)((w >> 24) & 0xFF);
                }
                if ((u32)cap + 20 > nraw) {
                    nraw = (u32)cap + 20;
                }
                cap = raw[cap + 1];
            }
            /* 链头为 0（没有 capability 链）时，保底把 0x40..0xFF 整段读进来：
             * 实测过 QEMU 把四个 virtio 能力放在 0x40/0x50/0x60/0x70，
             * 而 0x34 给的头指针指到了 MSI-X（0x98）。
             * 不去猜"为什么"，只保证**读到的字节是设备真正给的**。 */
            if (nraw == 0) {
                for (u32 off = 0x40; off + 20 <= sizeof(raw); off += 4) {
                    for (u32 k = 0; k < 20; k += 4) {
                        u32 w = fe_pci_cfg_read32(0, dev, fn, (u8)(off + k));
                        raw[off + k + 0] = (u8)(w & 0xFF);
                        raw[off + k + 1] = (u8)((w >> 8) & 0xFF);
                        raw[off + k + 2] = (u8)((w >> 16) & 0xFF);
                        raw[off + k + 3] = (u8)((w >> 24) & 0xFF);
                    }
                    nraw = off + 20;
                }
            }
        }
        struct fe_virtio_cap caps[6];
        if (fe_virtio_parse_caps(raw, nraw, 0, caps, 6) != FE_OK) {
            say("[devmgr] virtio capability 解析失败：驱动将无法初始化\n");
            return false;
        }
        say("[devmgr] capability：");
        for (u32 t = 1; t <= 4; t++) {
            if (!caps[t].found) {
                continue;
            }
            say(" 类型");
            num(t);
            say(" off=");
            num(caps[t].offset);
            say(" len=");
            num(caps[t].length);
        }
        say("  notify 乘数=");
        num(caps[VIRTIO_PCI_CAP_NOTIFY_CFG].multiplier);
        say("\n");
        g_vio_off[0] = caps[VIRTIO_PCI_CAP_COMMON_CFG].offset;
        g_vio_off[1] = caps[VIRTIO_PCI_CAP_NOTIFY_CFG].offset;
        g_vio_off[2] = caps[VIRTIO_PCI_CAP_ISR_CFG].offset;
        g_vio_off[3] = caps[VIRTIO_PCI_CAP_DEVICE_CFG].offset;
        g_vio_mult = caps[VIRTIO_PCI_CAP_NOTIFY_CFG].multiplier;

        /* ---- 找 MSI / MSI-X 能力（D5c）----
         *
         * ★ 为什么"发现"在这里、而"使能"在驱动里 ★
         * capability 链是 PCI 协议知识，读配置空间又是**独占资源**
         * （端口只有一份，init 拿着它探测完就还回池子）——所以位置
         * 由这里发现，经命令行交给驱动；驱动拿偏移去调内核的
         * IRQ_MSI_ALLOC，由内核分配向量、算消息地址/数据、让设备开始发消息。
         *
         * ★ 必须**走链**，不能在 raw[] 里搜字节 ★
         * 0x05 / 0x11 这种值在 offset/length 字段里到处都是，
         * 搜出来的位置是假的。链本身才说明"这是一个能力结构"。 */
        {
            u32 guard = 0;
            u8 c = (u8)(fe_pci_cfg_read32(0, dev, fn, 0x34) & 0xFF);
            while (c && guard++ < 64) {
                u32 hdr = fe_pci_cfg_read32(0, dev, fn, c);
                u8 id = (u8)(hdr & 0xFF);
                if (id == 0x11 && !g_vio_msix_off) {
                    g_vio_msix_off = c;
                } else if (id == 0x05 && !g_vio_msi_off) {
                    g_vio_msi_off = c;
                }
                c = (u8)((hdr >> 8) & 0xFF);
            }
        }
        say("[devmgr] MSI 能力：MSI-X ");
        if (g_vio_msix_off) {
            say("off=");
            num(g_vio_msix_off);
        } else {
            say("无");
        }
        say("，MSI ");
        if (g_vio_msi_off) {
            say("off=");
            num(g_vio_msi_off);
        } else {
            say("无");
        }
        say("\n");
    }

    /* 让设备能在总线上做主控。★ virtio 的每次 virtqueue 操作都是设备
     * **主动读**我们给的物理地址 ★：不使能 bit2 的话，设备收下描述符链、
     * 照样把请求放进 used ring，但**一个字节都不搬**——
     * 症状是"读回来全是零"，看起来像内存分配的问题。 */
    {
        u32 cmd = fe_pci_cfg_read32(0, dev, fn, FE_PCI_REG_COMMAND);
        u16 c16 = (u16)(cmd & 0xFFFF);
        if (!(c16 & FE_PCI_CMD_BUS_MASTER)) {
            fe_pci_cfg_write32(0, dev, fn, FE_PCI_REG_COMMAND,
                               (u32)(c16 | FE_PCI_CMD_BUS_MASTER));
        }
        u16 back = (u16)(fe_pci_cfg_read32(0, dev, fn, FE_PCI_REG_COMMAND) & 0xFFFF);
        say("[devmgr] virtio 总线主控");
        say((back & FE_PCI_CMD_BUS_MASTER) ? "已使能\n" : "**没置上**\n");
    }

    copy_lit(g_vio_arg, "--virtio", sizeof(g_vio_arg));
    hex_to_str(g_vio_mmio, g_vio_base, sizeof(g_vio_base));
    hex_to_str(g_vio_len, g_vio_size, sizeof(g_vio_size));
    hex_to_str(g_vio_irq, g_vio_irq_val, sizeof(g_vio_irq_val));
    hex_to_str(g_vio_off[0], g_vio_c_off, sizeof(g_vio_c_off));
    hex_to_str(g_vio_off[1], g_vio_n_off, sizeof(g_vio_n_off));
    hex_to_str(g_vio_off[2], g_vio_i_off, sizeof(g_vio_i_off));
    hex_to_str(g_vio_off[3], g_vio_d_off, sizeof(g_vio_d_off));
    hex_to_str(g_vio_mult, g_vio_mult_s, sizeof(g_vio_mult_s));
    /* MSI-X 的能力偏移与设备号（D5c）。★ 打包成一个值 ★
     * `bd = (bus << 8) | (dev << 3) | fn` —— vblkd 的 args[] 只有 16 格，
     * 而 virtio 那一组已经占了 12 格。一个"设备号"字段而不是三个，
     * 省下两格给将来；打包容错也简单（bus 只有 8 位、dev 5 位、fn 3 位，
     * 正好装进 16 位）。 */
    if (g_vio_msix_off) {
        copy_lit(g_vio_msix_arg, "--msix", sizeof(g_vio_msix_arg));
        hex_to_str(g_vio_msix_off, g_vio_msix_val, sizeof(g_vio_msix_val));
        hex_to_str(((u64)0 << 8) | ((u64)g_vio_dev << 3) | (u64)g_vio_fn,
                   g_vio_bd_val, sizeof(g_vio_bd_val));
    }
    g_vio_ready = true;
    return true;
}

/* 设备管理器身份 + IDE 的总线主控窗口。
 *
 * ★ 顺序要求 ★ 必须在**启动任何单元之前**做完，因为 blkd 的命令行里
 * 就要带 --bar4 / --virtio，而那些值只能由探测得到。
 * 也必须在 pcid 之前：两者都要用配置空间端口，而那是独占资源。 */
static bool busmaster_probe(void)
{
    say("[devmgr] 设备管理器：认领设备管理器身份\n");
    long r = fe_devmgr_claim();
    if (r != FE_OK) {
        say("[devmgr] 认领失败（只有引导者 init 能认领，且只能一次）\n");
        return false;
    }
    say("[devmgr] OK   身份已认领（只有 init 能认领，且只能一次）\n");

    r = fe_ioport_request(FE_PCI_CFG_ADDR_PORT, FE_PCI_CFG_LEN);
    if (r != FE_OK) {
        say("[devmgr] 认领 PCI 配置空间端口失败（另一个服务正持有它）\n");
        return false;
    }

    u8 fn = 0;
    u64 len = 0;
    u16 bar4 = bm_find_bar4(&fn, &len);
    if (!bar4) {
        /* ★ 找不到时必须说清"看过了什么" ★
         * 一条"没找到"的日志无法区分三种情况：设备不在、类别不匹配、
         * BAR4 不是 I/O 空间——而三者的修法完全不同。 */
        say("[devmgr] 逐功能扫描（设备 0:1）：\n");
        for (u8 f = 0; f < 8; f++) {
            if (!fe_pci_present(0, 1, f)) {
                continue;
            }
            u32 id = fe_pci_cfg_read32(0, 1, f, FE_PCI_REG_VENDOR_ID);
            u32 t2 = 0;
            u64 l2 = fe_pci_bar_probe(0, 1, f, 4, &t2);
            say("  fn ");
            num(f);
            say(" 厂商 ");
            hex4((id & 0xFFFF));
            say(" 设备 ");
            hex4((id >> 16) & 0xFFFF);
            say(" BAR4 值 ");
            hex4(fe_pci_cfg_read32(0, 1, f, 0x10 + 16));
            say(" 类型 ");
            num(t2);
            say(" 长度 ");
            num(l2);
            say("\n");
        }
        say("[devmgr] 没找到带总线主控 BAR 的 IDE 控制器：IDE 只能走 PIO\n");
        return false;
    }
    say("[devmgr] IDE 控制器 0:1.");
    num(fn);
    say("  总线主控窗口 ");
    hex4(bar4);
    say(" + ");
    num(len);
    say(" 字节（按 PCI 规范探测：写全 1 读回再取反）\n");

    /* 申报 + 认领：blkd 起来时会认领这段（它在 init 名下，
     * 所以是"从主人手里拿到"而不是"抢一个没人要的东西"）。 */
    r = fe_resource_pool_add(FE_RES_IOPORT, bar4, len, 0);
    if (r != FE_OK) {
        say("[devmgr] 申报入池失败\n");
        return false;
    }
    r = fe_ioport_request(bar4, len);
    if (r != FE_OK) {
        say("[devmgr] 认领已申报的区间失败\n");
        return false;
    }
    g_busmaster_ready = true;
    say("[devmgr] 总线主控窗口已入池并归本任务所有（申报 + 认领两步）\n");

    {
        static const char hexd[] = "0123456789abcdef";
        copy_lit(g_bar4_arg, "--bar4", sizeof(g_bar4_arg));
        g_bar4_val[0] = '0'; g_bar4_val[1] = 'x';
        g_bar4_val[2] = hexd[(bar4 >> 12) & 0xF];
        g_bar4_val[3] = hexd[(bar4 >> 8) & 0xF];
        g_bar4_val[4] = hexd[(bar4 >> 4) & 0xF];
        g_bar4_val[5] = hexd[bar4 & 0xF];
        g_bar4_val[6] = '\0';
    }

    /* 设备在总线上成为主控（属于发现者的设备配置，不是驱动逻辑）。 */
    {
        u32 cmd = fe_pci_cfg_read32(0, 1, fn, FE_PCI_REG_COMMAND);
        u16 c16 = (u16)(cmd & 0xFFFF);
        if (!(c16 & FE_PCI_CMD_BUS_MASTER)) {
            fe_pci_cfg_write32(0, 1, fn, FE_PCI_REG_COMMAND,
                               (u32)(c16 | FE_PCI_CMD_BUS_MASTER));
            u32 back = fe_pci_cfg_read32(0, 1, fn, FE_PCI_REG_COMMAND);
            say("[devmgr] 已使能总线主控（命令寄存器 bit2）");
            say((back & FE_PCI_CMD_BUS_MASTER) ? "，回读确认\n" : "，**回读没置上**\n");
        } else {
            say("[devmgr] 总线主控本来就是使能的\n");
        }
    }
    return true;
}

/* 两个探测都做完之后，把配置空间端口**还回池子**。
 *
 * ★ 还的是**所有权**（池子里重新出现这一段），不是"临时借出" ★
 * 独占资源同时只有一个持有者（见 fe_pci.h）。init 探测完就不再需要它，
 * 而 pcid 单元要拿它做枚举——不放回去的话 pcid 会直接失败，
 * 而那是"发现"这条链本身。 */
static void busmaster_release_ports(void)
{
    fe_resource_release_owner();
    say("[devmgr] 已把 PCI 配置空间端口还回池子（pcid 单元随后要用它）\n");
}

/* ================================================================== */
/* 启动单元表                                                          */
/* ================================================================== */

/* 一个可启动单元。服务与一次性程序共用同一个结构——
 * 这正是 rcorder 的样子：它排的是"依赖顺序"，不区分你是什么东西。 */
struct unit {
    const char *name;
    const char *path;
    const char *provides;   /* 就绪标志：devfs 上出现这个名字就算起来了；NULL = 不发布 */
    int daemon;             /* 1 = 拉起后不等它（常驻服务）；0 = 等它退出并校验退出码 */
    int expect;             /* daemon=0 时期望的退出码 */
    int check;              /* daemon=0 时是否校验退出码 */
    int expect_skip;        /* 负向测试：本单元**预期**被跳过 */
    int expect_unready;     /* 负向测试：本单元**预期**发布不出名字 */
    u32 needs;              /* 运行期前提：本机必须**有**某样硬件能力才启动。
                             * 0 = 无前提。★ 这是一个独立的类别 ★
                             * "没启动"有三种完全不同的原因，混在一起就没有
                             * 信息量了：
                             *   1. 依赖没就绪   → 系统没起来完整服务，算失败；
                             *   2. expect_skip  → 负向测试的预期结果，是**测试结论**；
                             *   3. needs 不成立 → 这台机器上根本没有那样硬件，
                             *                     既不是缺陷也不是测试，**不算失败**。
                             * 第三种必须单列：把它算成失败会让"失败项 N"
                             * 这条汇总在两种机器上给出不同的含义。 */
    void (*action)(void);   /* 非 NULL 时本单元是"动作"：直接在 init 里执行一个函数。
                             * A/B 槽管理就用它——它需要 /dev/blk0，因此理应进依赖图
                             * 排队，而不是硬编码在"blkd 之后"这个位置假设上。 */
    const char *args[16];   /* 传给程序的参数（NULL 结尾），可选。
                             * ★ 为什么留到 16 而不是"刚好够用"★
                             * 这一格是**隐式的**：少一个 NULL，或者写满整格，
                             * 都不会编译报错，只会在启动另一个程序时静默地
                             * 少传/多传参数。virtio 那台设备的参数正好 12 个，
                             * 卡在边界上，所以特意留出余量，
                             * 让"写满"这件事在结构上不可能发生。 */
    const char *require[MAX_DEPS];  /* 硬依赖：没就绪 → 跳过本单位 */
    const char *want[MAX_DEPS];     /* 软依赖：没就绪 → 只告警，仍然启动 */
};

/* ★ 声明顺序是**故意打乱**的，真正的启动顺序由 require 算出来 ★
 * 若日志里第一个跑的是 blkread 而不是 blkd，说明排序没生效。
 *
 * ★ 为什么不是 const ★
 * blkd 的 --bar4 / --virtio 是**运行时探测**出来的（PCI BAR 的地址由固件
 * 分配，两个环境不一样），所以这一项的参数要在探测之后回填。
 * 写成 const 就得把参数另存一份、再在启动时分支处理——那才是真的绕。 */
static struct unit g_units[] = {
    /* --- 一次性程序（写得靠前，但硬依赖让它们排到服务后面）--- */
    { .name = "blkread", .path = "/bin/blkread", .expect = 0, .check = 1,
      .require = { "/dev/blk0", NULL } },

    /* 文件系统客户端：依赖链的第三级 —— blkd → fsd → fs。
     * 单元表里它排在**最前面**，最终却必须排在服务之后，这就是排序的意义。 */
    { .name = "fs", .path = "/bin/fs", .expect = 0, .check = 1,
      .require = { "/dev/fs0", NULL } },

    /* 控制台客户端：**接口必须有人真的用过**。服务写完、发布出去、自检全绿，
     * 但"客户端能不能通过 IPC 把字打到屏幕上"是另一条路径——没跑过的路径
     * 等于没有。require /dev/console 让依赖排序保证它排在 consoled 之后。 */
    { .name = "contest", .path = "/bin/contest", .expect = 0, .check = 1,
      .require = { "/dev/console", NULL } },

    /* 扇区访问矩阵的承重测试：它从内核取区间表，检查"另一个槽连读都不行"。
     * 只依赖 /dev/blk0 而不依赖 /dev/fs0 —— 它测的是块层的策略。 */
    { .name = "protcheck", .path = "/bin/protcheck", .expect = 0, .check = 1,
      .require = { "/dev/blk0", NULL } },

    /* 块设备吞吐对照（D4）：内联读（旧路径）vs 批量读（DMA + 共享内存）。
     *
     * ★ 顺序为什么是 require /dev/fs0 而不是"排在探测之后" ★
     * 它走 require /dev/fs0 → fsd → blkd 这条依赖链，中间隔着 fsd 的完整
     * 初始化——那条路径比"探测 + 启动 blkd"长得多，所以当它跑到时，
     * 硬件窗口早已交给 blkd。这是可以核对的推理，不是"运气好"。
     *
     * ★ 它现在挂在 /dev/vblk0（virtio 数据盘）上，而不是系统盘 ★
     * 因为它要的是"同一条路径的对照"，而 QEMU 的 IDE 设备**根本不支持
     * DMA**（IDENTIFY w88=0x203F：UDMA 与多字 DMA 位全为 0），
     * 多扇区命令发出去设备不回 DRQ。那条路在 QEMU 上不是"慢"，是**没有**。
     * virtio-blk 的数据通路本身就是 DMA（virtqueue 的每个缓冲都以物理地址
     * 交给设备），所以只有它能把"DMA + 共享内存批量"这条路验出来。
     *
     * ★ 为什么 .check 从 0 改回 1 ★
     * 当初设成 0 是因为"批量读在降级路径上跑不通"——一个已知的、查清了的
     * 环境缺陷，算进失败项会让"自检存在 N 项失败"这条汇总失去指向性。
     * 现在批量读跑在 virtio 上，没有已知缺陷要豁免了，于是恢复成硬判据：
     * 它的失败项必须让人停下来看。 */
    { .name = "blkbench", .path = "/bin/blkbench", .expect = 0, .check = 1,
      .needs = NEED_VIRTIO_BLK,
      .args = { "--dev", "/dev/vblk0", NULL },
      .require = { "/dev/vblk0", NULL } },

    /* 负向测试：依赖**有提供者**（flaky），但提供者永远不会就绪 →
     * 本单元必须被跳过。（用"没有提供者"来测是错的：那是静态配置错误，
     * 走 preflight 那条路，根本测不到运行期场景。） */
    { .name = "depmissing", .path = "/bin/depmissing", .check = 1,
      .expect_skip = 1, .require = { "/dev/flaky0", NULL } },

    /* PCI 枚举（D3b）：认领配置空间端口 → 枚举总线 → 报告设备。
     * 它验证的是"用户态能发现硬件"这条链——在那之前，设备地址只能由
     * 内核硬编码（见 docs/12-drivers.md）。 */
    { .name = "pcid", .path = "/bin/pcid", .expect = 0, .check = 1 },

    /* 设备管理器（D4）的**独立复核**：用同一套 PCI 头文件再算一遍那个地址。
     *
     * ★ 它与 init 里的 busmaster_probe 是两件事，别混 ★
     * init 里那一份是**流程**（探测 → 申报 → 认领），启动期必须做完；
     * 本单元是**独立的一次复核**：两份输出可以对照，于是
     * "设备地址是发现出来的、不是硬编码的"这句话有了第二份证据。
     * 它故意不带 --declare（只发现、不改动状态）——这是"发现"与"裁决"
     * 两件事在本项目里的分界。它也不校验退出码（.check = 0）：
     * 它认领配置空间端口时会失败（init 正持有它），
     * 而那正是"独占资源只有一个持有者"的现场证据。 */
    { .name = "devmgr", .path = "/bin/devmgr", .check = 0 },

    { .name = "hello",   .path = "/bin/hello",   .expect = 42, .check = 1 },

    /* 句柄传递 / 批量 IPC：这一条是**能力模型**的端到端验证。
     * 它自己再拉起一份自己当服务端，在两个真实任务之间传内存对象，
     * 并核对两边看到的是同一块物理内存（1 MiB 双向）。
     * require /dev/fs0 是硬依赖：它要用 devfs 找服务、用文件系统传引导文件。 */
    { .name = "hxtest", .path = "/bin/hxtest", .expect = 0, .check = 1,
      .require = { "/dev/fs0", NULL } },

    /* POSIX 层（路线 B）：用**普通 C 程序**的写法去读真实文件并统计
     * 行数/词数/字节数。它跑起来就说明"别人的 C 代码能在 FEKernel 上
     * 编译并运行"至少对一类程序成立了。 */
    { .name = "posixprobe", .path = "/bin/posixprobe", .expect = 0, .check = 1,
      .args = { "-v", "/etc/protect.list", NULL },
      .require = { "/dev/fs0", NULL } },

    /* SIMD：既能用、也要**不串到别的线程**（这条是安全性质，不是性能性质）*/
    { .name = "simdtest", .path = "/bin/simdtest", .expect = 0, .check = 1 },
    { .name = "drvtest", .path = "/bin/drvtest", .expect = 0,  .check = 1 },
    /* drvdeny 故意不认领端口就执行 in，预期被 #GP 终止（退出码 -1） */
    { .name = "drvdeny", .path = "/bin/drvdeny", .expect = -1, .check = 1 },
    /* 基准程序只跑不判：它的输出是给人看的 */
    { .name = "bench",   .path = "/bin/bench",   .check = 0 },

    /* 任务可见性（K3）：从用户态取一张任务/线程快照并核对。
     *
     * ★ 为什么 require /dev/blk0 而不是"排在很后面" ★
     * 它要看的是一张**有内容**的表：文件系统、块设备、控制台这些常驻服务
     * 都在的时候，表里才有足够多的任务能核对（线程数、开关次数、归属）。
     * 而 require 一条硬依赖就能保证这一点，不必依赖"它在表里的位置"。 */
    { .name = "ps", .path = "/bin/ps", .expect = 0, .check = 1,
      .require = { "/dev/blk0", NULL } },

    /* 真实时钟（K7）：从用户态证明时钟能分辨亚毫秒，并与自读 TSC 交叉验证。
     * 不需要任何依赖——时钟在任何时刻都该是可用的。 */
    { .name = "clocktest", .path = "/bin/clocktest", .expect = 0, .check = 1 },

    /* 中断共享（D5a/D5b）的真实路径测试：两个任务登记同一条**真实**电平线
     * （virtio-blk 的 IRQ11），各自都要被设备中断叫醒，而且线不能被卡住。
     * require /dev/vblk0 —— 它要靠那块盘制造真实流量。 */
    { .name = "irqtest", .path = "/bin/irqtest", .expect = 0, .check = 1,
      .needs = NEED_VIRTIO_BLK,
      .require = { "/dev/vblk0", NULL } },

    /* 进程终止（K2）：拉起会跑飞的孩子，再把它们终止掉。
     *
     * ★ 它 require /dev/fs0 是因为要**按路径拉起自己** ★
     * killtest 用 `--spin` / `--pub` / `--recv` 分饰两角：同一个可执行
     * 文件既是被终止的一方、也是监督者。所以它必须能从文件系统里
     * 找到自己的路径——那要求 /dev/fs0 就绪。 */
    { .name = "killtest", .path = "/bin/killtest", .expect = 0, .check = 1,
      .require = { "/dev/fs0", NULL } },

    /* 替换映像（K6）+ 区间保护（mprotect）的**用户态运行期**验证（2c）。
     *
     * ★ 为什么是**三个**单元，而不是一个 ★
     * 一个进程只能有一个退出码：模式 A 成功时进程**变成**了子模式
     * （exit 0x5A），模式 C 成功时变成 chain3（exit 0x5C）——
     * 两者都想让 init 校验自己的退出码，就只能各占一次调用。
     * 把三种模式压进一次调用，"两个退出码"在物理上不可能同时成立
     * （第一版就是这样，结果模式 A 那段代码永远跑不到）。
     *
     * ★ 三个单元都 require /dev/fs0，而且**都没有 needs** ★
     * exectest 的核心动作就是 `fe_exec` **按路径装载自己**（探针还要
     * spawn 自己当孩子），所以没有文件系统它无从谈起；
     * 而它不依赖任何硬件——只用 ramfs 里的映像，所以两个环境
     * （QEMU 与 VirtualBox）都必须真的跑起来。按 needs 跳过它，
     * 等于把"提交路径只有编译级保证"这个空白又留回去（docs/15 §10.3）。 */
    { .name = "exectest", .path = "/bin/exectest", .expect = 0x5A, .check = 1,
      .require = { "/dev/fs0", NULL } },
    { .name = "exectest-rb", .path = "/bin/exectest", .expect = 0x5B, .check = 1,
      .args = { "--rollback", NULL },
      .require = { "/dev/fs0", NULL } },
    { .name = "exectest-ch", .path = "/bin/exectest", .expect = 0x5C, .check = 1,
      .args = { "--chain", NULL },
      .require = { "/dev/fs0", NULL } },

    /* 用户态异常处理者（K5）的**真实路径**验证：ring 3 真异常 → 投递 →
     * 处理者 recv → FAULT_REPLY → 出错线程从处理者指定的新 rip 继续跑。
     *
     * ★ 为什么是**六个**单元 ★ 与 exectest 同一条理由：一个进程只能有一个
     * 退出码，而这六种模式的成功判据各不相同——
     *   --segv       越界写被接管，进程**没死**（退出码 0）
     *   --ud         非法指令被跳过，进程没死（0）
     *   --de         除零被接管且处理者填的商真的被用上（0）
     *   --all        上面三次由**同一个**处理者接管（0，模式 E）
     *   --nohandler  **反向对照**：没登记处理者 → 必须**仍然被杀**（-1）
     *   --recursive  处理者自己出错 → 那条线程被杀（0x77，客户端核对的确定值）
     * 把这些压进一个单元，"-1 与 0 同时成立"在物理上不可能。
     * ★ `--nohandler` 的期望 -1 与 drvdeny 的期望完全一致 ★——这一条是
     * 整个 K5 最重要的反向对照：没有它，"投递机制生效"与"内核干脆不杀了"
     * 分不开（docs/18 §5.2 模式 D）。
     *
     * ★ 六个单元都**没有 needs** ★ 它不用块设备、不用中断线、不用文件系统
     * （现场缓冲区就是一个 .bss 全局），所以 QEMU 与 VirtualBox 两个环境
     * 都必须真的跑。按 needs 跳过它等于把"投递路径只有编译级保证"这个
     * 空白又留回去（docs/18 §5.4）。 */
    { .name = "faulttest", .path = "/bin/faulttest", .expect = 0, .check = 1 },
    { .name = "faulttest-ud", .path = "/bin/faulttest", .expect = 0, .check = 1,
      .args = { "--ud", NULL } },
    { .name = "faulttest-de", .path = "/bin/faulttest", .expect = 0, .check = 1,
      .args = { "--de", NULL } },
    { .name = "faulttest-all", .path = "/bin/faulttest", .expect = 0, .check = 1,
      .args = { "--all", NULL } },
    { .name = "faulttest-noh", .path = "/bin/faulttest", .expect = -1, .check = 1,
      .args = { "--nohandler", NULL } },
    { .name = "faulttest-rec", .path = "/bin/faulttest", .expect = 0x77, .check = 1,
      .args = { "--recursive", NULL } },

    /* 等一个用户地址（K11）的**真实路径**验证：用户态线程 → FE_SYS_WAIT_ADDR
     * → 内核读用户内存 + 挂链 + 置 BLOCKED → 另一个线程改内存 + FE_SYS_WAKE_ADDR
     * → 被调度回来。
     *
     * ★ 为什么是**四个**单元 ★ 与 exectest/faulttest 同一条理由：一个进程只有
     * 一个退出码，而这四种模式的成功判据各不相同——
     *   --cond    条件变量：3 生产者 + 1 消费者，总数**精确相等**（退出码 0）
     *   --spin    **反向**：同一条逻辑用纯自旋，同样预算内跑不完（0x5B）
     *   --timeout 超时：只断言"**不早于** deadline 返回"（0）
     *   --drop    **反向**：故意丢一次唤醒 → 总数必须对不上（0x4D）
     * ★ `--spin` 与 `--drop` 的"成功"是**确定的非零码**，不是 0 ★
     * 它们测的是"没有这条机制会怎样"，所以成功恰恰意味着"确实没做成"。
     *
     * ★ 四个单元都**没有 needs** ★ 不用块设备、不用中断线、不用文件系统
     * （共享状态就是 .bss 里的几个 u32），所以 QEMU 与 VirtualBox 两个环境
     * 都必须真的跑。按 needs 跳过它等于把"等待原语的用户态路径只有编译级
     * 保证"这个空白又留回去（docs/21 §7.3）。 */
    { .name = "waitaddrtest", .path = "/bin/waitaddrtest", .expect = 0, .check = 1 },
    { .name = "waitaddrtest-spin", .path = "/bin/waitaddrtest", .expect = 0,
      .check = 1, .args = { "--spin", NULL } },
    { .name = "waitaddrtest-to", .path = "/bin/waitaddrtest", .expect = 0, .check = 1,
      .args = { "--timeout", NULL } },
    { .name = "waitaddrtest-drop", .path = "/bin/waitaddrtest", .expect = 0x4D,
      .check = 1, .args = { "--drop", NULL } },

    /* --- 常驻服务 --- */
    { .name = "blkd",  .path = "/sbin/blkd",  .provides = "/dev/blk0", .daemon = 1,
      /* 参数在启动前由 busmaster_probe 回填（端口是**运行时发现**出来的）：
       *   --bar4 <端口>    IDE 的总线主控窗口
       * 没有它时 blkd 仍能起来——它退到"一条命令一个扇区"的 PIO 路径，
       * 而那条路在这台机器上一直是好的。
       * virtio 的参数不在这里：那台设备由 vblkd 用（见下面那一条）。 */
      .args = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL } },

    /* 第二块盘（D4 的基准数据盘）：**同一个驱动、换一个构造参数**。
     *
     * ★ 为什么是"第二个实例"而不是"让 blkd 同时管两块盘" ★
     * "哪个盘是系统盘"这件事只对引导链有意义：文件系统、写保护矩阵、
     * A/B 都挂在 /dev/blk0 上，而基准数据盘只是一段用来量吞吐的扇区。
     * 让一个实例同时管两块，就得在协议里加一个"设备号"——那会把
     * "多设备块层"这个**上层概念**塞进驱动，而今天没有任何消费者需要它。
     * 两个实例 + 两个名字，是当前需求下最小且不撒谎的形态。
     *
     * 它 require /dev/blk0（而不是 /dev/vblk0——后者由它自己提供），
     * 于是依赖排序保证"系统盘先就绪"，两个实例也不会同时抢同一个端点名。 */
    { .name = "vblkd", .path = "/sbin/blkd", .provides = "/dev/vblk0", .daemon = 1,
      .needs = NEED_VIRTIO_BLK,
      /* args[0..2] 固定；args[3..11] 由 virtio_blk_probe 回填成
       *   --virtio <基址> <长度> <common> <notify> <isr> <device> <乘数> <irq>
       * 一共正好 12 格，所以这个结构体特意留了 16 格（见 args 的说明）。 */
      .args = { "--publish", "/dev/vblk0", "--data-disk", NULL },
      .require = { "/dev/blk0", NULL } },


    /* A/B 槽管理：动作型单元。它 require /dev/blk0，所以依赖排序保证它
     * 排在 blkd 就绪之后——**而不是靠"我把它写在 blkd 后面"这种位置假设**。 */
    { .name = "ab-state", .action = ab_state_machine,
      .require = { "/dev/blk0", NULL } },
    /* fsd 依赖 blkd：这条边让 blkd 必然排在 fsd 前面，
     * 而 blkread / fs 又依赖 fsd，于是整条链自动排好。 */
    { .name = "fsd",   .path = "/sbin/fsd",   .provides = "/dev/fs0",
      .daemon = 1, .require = { "/dev/blk0", NULL } },
    { .name = "kbd",   .path = "/sbin/kbd",   .daemon = 1 },
    /* 控制台：帧缓冲的唯一持有者，发布 /dev/console。
     * 它不依赖 kbd（渲染与输入是两件事），所以两者可以并行起来。 */
    { .name = "consoled", .path = "/sbin/consoled", .provides = "/dev/console",
      .daemon = 1 },
    { .name = "mouse", .path = "/sbin/mouse", .daemon = 1 },

    /* 负向测试夹具：被拉起但永不发布名字，模拟"服务启动就挂"。预期未就绪。 */
    { .name = "flaky", .path = "/sbin/flaky", .provides = "/dev/flaky0",
      .daemon = 1, .expect_unready = 1 },

    /* 交互式 shell：**放在最后**，因为它是给人用的——自检与诊断先跑完，
     * 屏幕上的输出才不会被 shell 的提示符打断。它是"常驻"（不发布名字、
     * 不等退出）：shell 会一直跑到用户敲 exit，而 init 不该为了它在
     * fe_wait 上挂住——那样后面的收尾统计就永远打不出来。 */
    { .name = "sh", .path = "/bin/sh", .daemon = 1,
      .require = { "/dev/console", NULL } },
};

#define NUNITS ((u32)(sizeof(g_units) / sizeof(g_units[0])))

/* ================================================================== */
/* A/B 槽管理                                                          */
/* ================================================================== */
/* 槽状态记录放在 FAT32 的保留扇区里（misc），两份、各带 CRC32、交替写：
 * **坏掉一次写入不会让系统失去"下次启动哪一份"这个信息**，另一份还在。
 * 这是 Android bootloader_control 的同一套做法。
 *
 * 引导控制块（Limine 的 default_entry 那一个字节）由 mkfat.py 建镜像时
 * 算好位置写进清单，所以这里完全不需要懂 FAT。 */

static void ab_write_record(u32 seq, u8 active, u8 successful, u8 attempts)
{
    if (!g_ab_ok || !(g_ab.flags & FE_AB_FLAG_SLOT_STATE)) {
        return;
    }
    static u8 sec[FE_BLK_SECTOR];
    struct fe_ab_slot_state st;
    st.magic = FE_AB_MAGIC;
    st.seq = seq;
    st.active = active;
    st.successful = successful;
    st.boot_attempts = attempts;
    st._pad = 0;
    /* CRC 覆盖前 12 字节（magic..boot_attempts），见 fe_ab_slot_state 的注释 */
    st.crc32 = fe_crc32(&st, 12);

    /* 两份记录交替写：序号大的那份是"较新"的，读的时候取有效的最大者。
     * 写同一份两次没有意义——掉电时那一份正好在写，就没有退路了。 */
    u32 slot = seq & 1u;
    u64 lba = g_ab.misc_lba + slot;
    if (fe_blk_read(g_ab_blk, lba, 1, sec) != 0) {
        say("[AB] **读槽状态扇区失败**（LBA ");
        num(lba);
        say("）\n");
        return;
    }
    const u8 *src = (const u8 *)&st;
    for (u32 i = 0; i < sizeof(st); i++) {
        sec[i] = src[i];
    }
    if (fe_blk_write(g_ab_blk, lba, 1, sec) != 0) {
        say("[AB] **写槽状态失败**（LBA ");
        num(lba);
        say("）\n");
        g_fail++;
    }
}

static void ab_read_cmdline(void)
{
    static char cl[256];
    long r = fe_cmdline(cl, sizeof(cl));
    if (r < 0) {
        return;
    }
    say("[引导] 命令行: 「");
    say(cl);
    say("」\n");
    /* ★ 内核**不解释**命令行（唯一的例外是 slot=，那是它启动 init 要用的）★
     * 命令行是引导器与用户态之间的通道；内核一旦开始解释它，就等于把
     * "启动策略"搬进了内核。所以"本次要不要执行更新"在这里判。 */
    for (u32 i = 0; cl[i]; i++) {
        if (cl[i] == 'f' && cl[i + 1] == 'e' && cl[i + 2] == 'k' &&
            cl[i + 3] == '.') {
            if (cl[i + 4] == 'u' && cl[i + 5] == 'p' && cl[i + 6] == 'd' &&
                cl[i + 7] == 'a' && cl[i + 8] == 't' && cl[i + 9] == 'e' &&
                cl[i + 10] == '=' && cl[i + 11] == '1') {
                g_ab_update_wanted = 1;
            } else if (cl[i + 4] == 'r' && cl[i + 5] == 'e' && cl[i + 6] == 'b' &&
                       cl[i + 7] == 'o' && cl[i + 8] == 'o' && cl[i + 9] == 't' &&
                       cl[i + 10] == '=' && cl[i + 11] == '1') {
                g_reboot_wanted = 1;
            }
        }
    }
}

static void ab_run_updater(void)
{
    char full[96];
    fe_slot_path(g_prefix, "/bin/abupdate", full, sizeof(full));
    say("[AB] 执行更新器 ");
    say(full);
    say("\n");
    char *argv[2];
    argv[0] = full;
    argv[1] = (char *)0;
    long h = fe_spawn(full, argv, 1);
    if (h < 0) {
        say("[AB] 更新器启动失败，返回 -");
        num((u64)(-h));
        say("\n");
        g_fail++;
        return;
    }
    int status = 0;
    long r = fe_wait(h, &status);
    fe_handle_close(h);
    if (r < 0) {
        say("[AB] 更新器 wait 失败\n");
        g_fail++;
        return;
    }
    say("[AB] 更新器退出码 = ");
    if (status < 0) {
        say("-");
        num((u64)(-status));
    } else {
        num((u64)status);
    }
    say("\n");
    if (status != 0) {
        say("[AB] 更新失败 → **不重启**（留在当前槽，它还能用）\n");
        g_fail++;
        return;
    }
    say("[AB] 更新已提交：下次启动槽 ");
    say(g_ab.boot_slot == 0 ? "B" : "A");
    say("，现在重启进入新槽\n");
    fe_flush();
    fe_reboot();
}

static void ab_state_machine(void)
{
    if (!g_ab_ok) {
        say("[AB] 清单没给出完整的 A/B 布局（misc/bootsel）→ 单槽启动，跳过状态机\n");
        return;
    }
    if (!(g_ab.flags & FE_AB_FLAG_SLOT_STATE)) {
        say("[AB] 清单没给出完整的 A/B 布局（misc/bootsel）→ 单槽启动，跳过状态机\n");
        return;
    }
    g_ab_blk = fe_devfs_open("/dev/blk0");
    if (g_ab_blk <= 0) {
        say("[AB] 打不开 /dev/blk0 → 跳过槽状态机\n");
        return;
    }
    say("[AB] --- 槽状态机 ---\n");
    say("[AB] 当前槽 ");
    say(g_ab.boot_slot == 0 ? "A" : "B");
    say("；misc LBA ");
    num(g_ab.misc_lba);
    say(" + ");
    num(g_ab.misc_count);
    say("；控制块 LBA ");
    num(g_ab.bootsel_lba);
    say(" 偏移 ");
    num(g_ab.bootsel_offset);
    say("\n");

    /* 读两份记录，取"有效且 seq 最大"的那一份。 */
    static u8 sec[FE_BLK_SECTOR];
    u32 best_seq = 0;
    int best = -1;
    struct fe_ab_slot_state rec[2];
    for (u32 i = 0; i < 2; i++) {
        if (fe_blk_read(g_ab_blk, g_ab.misc_lba + i, 1, sec) != 0) {
            say("[AB] 记录份 ");
            num(i);
            say(": **读失败**\n");
            continue;
        }
        const u8 *src = sec;
        u8 *dst = (u8 *)&rec[i];
        for (u32 k = 0; k < sizeof(rec[i]); k++) {
            dst[k] = src[k];
        }
        u32 crc = fe_crc32(&rec[i], 12);
        int magic_ok = (rec[i].magic == FE_AB_MAGIC);
        int crc_ok = (crc == rec[i].crc32);
        say("[AB] 记录份 ");
        num(i);
        say(": magic ");
        say(magic_ok ? "OK" : "**坏**");
        say(", CRC ");
        say(crc_ok ? "OK" : "**坏**");
        say(", seq=");
        num(rec[i].seq);
        say(", active=");
        say(rec[i].active == 0 ? "A" : "B");
        say(", successful=");
        num(rec[i].successful);
        say(", attempts=");
        num(rec[i].boot_attempts);
        say("\n");
        if (magic_ok && crc_ok && (best < 0 || rec[i].seq >= best_seq)) {
            best_seq = rec[i].seq;
            best = (int)i;
        }
    }
    if (best < 0) {
        say("[AB] 两份记录都不可用 → 按当前槽重建记录，并当成一次试用启动\n");
        ab_write_record(1, (u8)g_ab.boot_slot, 0, 1);
        return;
    }
    say("[AB] → 采用第 ");
    num((u64)best);
    say(" 份（seq 较大且 CRC 有效）\n");
    struct fe_ab_slot_state *r = &rec[best];
    u8 cur = (u8)g_ab.boot_slot;

    if (r->active == cur && r->successful == 1 && r->boot_attempts == 0) {
        say("[AB] 记录与现状一致（active=");
        say(cur == 0 ? "A" : "B");
        say(", successful=1, attempts=0）→ 不写盘\n");
        if (g_ab_update_wanted) {
            /* ★ 只有"已经确认可用的槽"才去改另一个槽 ★
             * 一个自己还没被确认可用的槽去动另一个槽，是把仅有的退路也押上。 */
            ab_run_updater();
        }
        return;
    }
    if (r->active == cur) {
        /* 同一个槽又在启动：计一次尝试。次数多了说明这个槽起不来。 */
        u8 attempts = (u8)(r->boot_attempts + 1);
        say("[AB] 同一槽再次启动 → attempts ");
        num(r->boot_attempts);
        say(" → ");
        num(attempts);
        say("\n");
        ab_write_record(r->seq + 1, cur, r->successful, attempts);
        if (attempts >= 3) {
            say("[AB] **连续 ");
            num(attempts);
            say(" 个失败项 → 不自报成功**；下次启动再试（连续多次会触发回滚）\n");
        }
        return;
    }
    /* 记录里的 active 不是当前槽。★ 这里有两种完全不同的情形 ★
     *
     *   (a) **正常的更新后首次启动**：更新器翻了引导控制块，但按设计
     *       **不写**槽状态记录（"下次启动谁"由控制块决定，槽状态由 init
     *       维护）。于是记录还写着旧槽 active，而我们已经跑在新槽里。
     *       此时 r->successful == 1：旧槽是**被确认可用**的，
     *       没有任何东西失败。
     *   (b) **真的回滚**：记录里的槽 active 且 successful == 0
     *       —— 它上次启动没能自报成功，所以我们退回了当前槽。
     *
     * ★ 第一版把两种情形打成了同一句话 ★
     * 它说"槽 A 没能自报成功 → 切回槽 B"，而此时记录明明是
     * successful=1（A 好得很）。日志把**正常路径**描述成了一次回滚，
     * 而这正是最贵的一类误导：看到它的人会去查"A 为什么失败了"，
     * 而 A 根本没失败。分开报之后，两种情形各说各的话。 */
    if (r->successful == 1) {
        say("[AB] 记录里的 active 是槽 ");
        say(r->active == 0 ? "A" : "B");
        say("（successful=1），而本次启动的是槽 ");
        say(cur == 0 ? "A" : "B");
        say(" —— 引导控制块刚被更新器翻过，以现状为准重写记录\n");
    } else {
        say("[AB] **回滚**：槽 ");
        say(r->active == 0 ? "A" : "B");
        say(" 上次没能自报成功（successful=0）→ 切回槽 ");
        say(cur == 0 ? "A" : "B");
        say("\n");
    }
    ab_write_record(r->seq + 1, cur, 0, 0);
}

static void ab_mark_successful(void)
{
    if (!g_ab_ok || !(g_ab.flags & FE_AB_FLAG_SLOT_STATE) || g_ab_blk <= 0) {
        return;
    }
    static u8 sec[FE_BLK_SECTOR];
    /* 读回当前槽的记录：只有"还没自报成功"时才写。
     * ★ 写早了等于"服务还没起来就说自己好" ★ 那样的 successful 没有意义，
     * 所以这个调用点必须放在**所有单元都走完之后**（见 main 的收尾）。 */
    if (fe_blk_read(g_ab_blk, g_ab.misc_lba, 1, sec) == 0) {
        struct fe_ab_slot_state r;
        const u8 *src = sec;
        u8 *dst = (u8 *)&r;
        for (u32 k = 0; k < sizeof(r); k++) {
            dst[k] = src[k];
        }
        if (r.magic == FE_AB_MAGIC && fe_crc32(&r, 12) == r.crc32 &&
            r.active == (u8)g_ab.boot_slot && r.successful == 1) {
            say("[AB] 本槽此前已自报成功，本次无需改写记录\n");
            return;
        }
    }
    if (g_fail > 0) {
        say("[AB] 本次启动有 ");
        num(g_fail);
        say(" 个失败项 → **不自报成功**；下次启动再试\n");
        return;
    }
    ab_write_record(3, (u8)g_ab.boot_slot, 1, 0);
    say("[AB] 本槽已自报成功（successful=1）\n");
}

/* ================================================================== */
/* 依赖排序与启动                                                      */
/* ================================================================== */

/* 把 "/dev/xxx" 解析成"哪个单元提供它"。返回下标，找不到返回 -1。 */
static int provider_of(const char *name)
{
    if (!name) {
        return -1;
    }
    for (u32 i = 0; i < NUNITS; i++) {
        const struct unit *u = &g_units[i];
        if (!u->provides) {
            continue;
        }
        u32 k = 0;
        for (; name[k] && u->provides[k] && name[k] == u->provides[k]; k++) {
        }
        if (name[k] == '\0' && u->provides[k] == '\0') {
            return (int)i;
        }
    }
    return -1;
}

/* 静态检查：每个硬依赖都必须有提供者。
 * ★ 这一步不能省 ★ "依赖一个没有提供者的名字"是**配置错误**，
 * 而它在运行期表现为"这个单元被跳过了"——与"提供者存在但没起来"
 * 完全同形。把两者分开的办法就是启动前先把配置查一遍。 */
static u32 preflight(void)
{
    u32 bad = 0;
    for (u32 i = 0; i < NUNITS; i++) {
        const struct unit *u = &g_units[i];
        for (u32 k = 0; k < MAX_DEPS && u->require[k]; k++) {
            if (provider_of(u->require[k]) < 0) {
                say("[init] **静态检查失败**：");
                say(u->name);
                say(" 硬依赖 ");
                say(u->require[k]);
                say("，但没有任何单元提供它（这是配置错误，不是运行期问题）\n");
                bad++;
            }
        }
        for (u32 k = 0; k < MAX_DEPS && u->want[k]; k++) {
            if (provider_of(u->want[k]) < 0) {
                say("[init] 警告：");
                say(u->name);
                say(" 软依赖 ");
                say(u->want[k]);
                say(" 没有提供者（只告警）\n");
            }
        }
    }
    return bad;
}

/* Kahn 拓扑排序：只按**硬依赖**（require）排。
 * 软依赖（want）只影响顺序、不影响可启动性，所以它由 apply_want_ordering
 * 单独处理，而不参与"入度归零"的判定——否则一个软依赖没起来就会把整张图卡住。 */
static int topo_sort(void)
{
    u32 indeg[NUNITS];
    int done[NUNITS];
    for (u32 i = 0; i < NUNITS; i++) {
        indeg[i] = 0;
        done[i] = 0;
    }
    for (u32 i = 0; i < NUNITS; i++) {
        const struct unit *u = &g_units[i];
        for (u32 k = 0; k < MAX_DEPS && u->require[k]; k++) {
            if (provider_of(u->require[k]) >= 0) {
                indeg[i]++;
            }
        }
    }
    g_order_len = 0;
    while (g_order_len < NUNITS) {
        int picked = -1;
        for (u32 i = 0; i < NUNITS; i++) {
            if (!done[i] && indeg[i] == 0) {
                picked = (int)i;
                break;
            }
        }
        if (picked < 0) {
            return -1;              /* 有环 */
        }
        done[picked] = 1;
        g_start_order[g_order_len++] = (u32)picked;
        for (u32 i = 0; i < NUNITS; i++) {
            if (done[i]) {
                continue;
            }
            const struct unit *v = &g_units[i];
            for (u32 k = 0; k < MAX_DEPS && v->require[k]; k++) {
                if (provider_of(v->require[k]) == picked && indeg[i] > 0) {
                    indeg[i]--;
                }
            }
        }
    }
    return 0;
}

/* 软依赖（want）只调顺序：把"被依赖者"尽量排到前面。
 * 做法很土但可核对：反复扫描，只要某个单元的 want 提供者排在它后面就前移。
 * 单元只有二十几个，代价可以忽略，而"能一眼看懂"比"聪明"重要。 */
static void apply_want_ordering(void)
{
    for (u32 pass = 0; pass < NUNITS; pass++) {
        int moved = 0;
        for (u32 a = 0; a < g_order_len; a++) {
            const struct unit *u = &g_units[g_start_order[a]];
            for (u32 k = 0; k < MAX_DEPS && u->want[k]; k++) {
                int p = provider_of(u->want[k]);
                if (p < 0) {
                    continue;
                }
                u32 pb = g_order_len;
                for (u32 b = 0; b < g_order_len; b++) {
                    if (g_start_order[b] == (u32)p) {
                        pb = b;
                        break;
                    }
                }
                if (pb > a) {
                    u32 v = g_start_order[pb];
                    for (u32 b = pb; b > a; b--) {
                        g_start_order[b] = g_start_order[b - 1];
                    }
                    g_start_order[a] = v;
                    moved = 1;
                }
            }
        }
        if (!moved) {
            break;
        }
    }
}

/* 等某个 devfs 名字出现。最多等 ~150 次睡眠（每次 10 ms）。
 * ★ 用睡眠而不是忙等 ★ `fe_thread_yield` 是**让出**不是阻塞，
 * 忙等的线程会一直占着就绪队列把别人饿死（本项目踩过）。 */
static int wait_ready(const char *name, u32 tries)
{
    for (u32 i = 0; i < tries; i++) {
        long h = fe_devfs_open(name);
        if (h > 0) {
            fe_handle_close(h);
            return 1;
        }
        fe_sleep_ms(10);
    }
    return 0;
}

/* 该单元的硬依赖现在满足了没有。返回 0 = 可以启动；1 = 有硬依赖缺失。 */
static int deps_ready(u32 idx, const char **missing)
{
    const struct unit *u = &g_units[idx];
    for (u32 k = 0; k < MAX_DEPS && u->require[k]; k++) {
        int p = provider_of(u->require[k]);
        if (p < 0 || !g_ready[p]) {
            if (missing) {
                *missing = u->require[k];
            }
            return 1;
        }
    }
    for (u32 k = 0; k < MAX_DEPS && u->want[k]; k++) {
        int p = provider_of(u->want[k]);
        if (p >= 0 && !g_ready[p]) {
            say("[init] ");
            say(u->name);
            say(" 的软依赖 ");
            say(u->want[k]);
            say(" 未就绪，仍然启动\n");
        }
    }
    return 0;
}

static void start_unit(u32 idx)
{
    const struct unit *u = &g_units[idx];

    /* ★ 硬件前提在**依赖检查之前**判 ★
     * 顺序是有讲究的：没有 virtio 设备时，/dev/vblk0 永远出不来，
     * 于是"依赖未就绪"这条判断**必然**也会成立。先判它，报出来的原因
     * 才是真的（本机没这个设备），而不是一个由它派生出来的假原因
     * （某个服务没起来）。报错指向错误的原因是排查里最贵的一种浪费。 */
    if ((u->needs & NEED_VIRTIO_BLK) && !g_vio_ready) {
        say("[init] 跳过 ");
        say(u->name);
        say("：本机没有 virtio-blk 设备 —— **硬件前提不成立，不算失败**\n");
        return;
    }

    const char *missing = NULL;
    if (deps_ready(idx, &missing)) {
        if (u->expect_skip) {
            say("[init] 跳过 ");
            say(u->name);
            say("：依赖 ");
            say(missing);
            say(" 未就绪（提供者 ");
            int p = provider_of(missing);
            say(p >= 0 ? g_units[p].name : "?");
            say(" 没能发布它）\n");
            say("[init] （这是负向测试的**预期结果**）\n");
            return;
        }
        say("[init] **跳过 ");
        say(u->name);
        say("**：依赖 ");
        say(missing);
        say(" 未就绪\n");
        g_fail++;   /* 跳过算失败：说明系统没起来完整的服务 */
        return;
    }

    char full[96];

    /* 动作型单元：依赖检查走过了，这里直接执行函数。
     * 它没有路径、没有退出码，也不发布 devfs 名字——但它在依赖图里的
     * 位置和其它单元一样是被算出来的。这一步必须在 slot_path 之前：
     * 动作型单元的 path 是 NULL。 */
    if (u->action) {
        u->action();
        return;
    }

    fe_slot_path(g_prefix, u->path, full, sizeof(full));

    char *argv[18];
    u32 argc = 1;
    argv[0] = full;
    for (u32 k = 0; k < 16 && u->args[k]; k++) {
        argv[argc++] = (char *)u->args[k];
    }
    argv[argc] = (char *)0;
    long h = fe_spawn(full, argv, argc);
    if (h < 0) {
        say("[init] ");
        say(u->name);
        say(" 启动失败，spawn 返回 -");
        num((u64)(-h));
        say("（路径 ");
        say(full);
        say("）\n");
        g_fail++;
        return;
    }

    if (u->daemon) {
        /* 常驻服务：等它发布自己的名字。
         * 句柄故意不关——init 是它的父进程，留着句柄才能管它。 */
        if (u->provides) {
            int ok = wait_ready(u->provides, 150);
            g_ready[idx] = ok;
            say("[init] ");
            say(u->name);
            say(ok ? " 已就绪（" : " **未就绪**（");
            say(u->provides);
            say(ok ? " 已发布）\n" : " 未出现）\n");
            if (!ok && u->expect_unready) {
                say("[init] （这是负向测试夹具的**预期结果**，它靠这个构造"
                    "「依赖未就绪」场景）\n");
            } else if (!ok) {
                g_fail++;
            } else if (u->expect_unready) {
                say("[init] flaky 竟然就绪了 —— 负向测试前提被破坏\n");
                g_fail++;
            }
        } else {
            say("[init] ");
            say(u->name);
            say(" 已拉起（不发布名字，无需等待）\n");
        }
        return;
    }

    /* 一次性程序：等它退出并校验退出码 */
    int status = 0;
    long r = fe_wait(h, &status);
    fe_handle_close(h);
    if (r < 0) {
        say("[init] ");
        say(u->name);
        say(" wait 失败，错误码 -");
        num((u64)(-r));
        say("\n");
        g_fail++;
        return;
    }
    say("[init] ");
    say(u->name);
    say(" 退出码 = ");
    if (status < 0) {
        say("-");
        num((u64)(-status));
    } else {
        num((u64)status);
    }
    if (!u->check) {
        say("（不校验）\n");
    } else if (status == u->expect) {
        say("  期望 ");
        if (u->expect < 0) {
            say("-");
            num((u64)(-u->expect));
        } else {
            num((u64)u->expect);
        }
        say("  —— 相符\n");
    } else {
        say("  期望 ");
        if (u->expect < 0) {
            say("-");
            num((u64)(-u->expect));
        } else {
            num((u64)u->expect);
        }
        say("  —— **不符**\n");
        g_fail++;
    }
}

/* ================================================================== */

int main(int argc, char **argv)
{
    (void)arg_is;

    say("\n=== init 启动（用户态第一个进程）===\n");
    say("[init] argc = ");
    num((u64)argc);
    say(", argv[0] = \"");
    say((argc > 0 && argv && argv[0]) ? argv[0] : "(空)");
    say("\"\n");
    fe_slot_prefix((argc > 0 && argv) ? argv[0] : 0, g_prefix, sizeof(g_prefix));
    say("[init] 槽前缀 = \"");
    say(g_prefix[0] ? g_prefix : "(无 —— 单槽布局)");
    say("\"\n");
    ab_read_cmdline();
    if (g_reboot_wanted) {
        /* 这里**故意**在拉起任何服务之前重启：复位路径的验证不该依赖
         * 任何服务起来了没有。 */
        say("[AB] 立即重启（复位路径自检）……\n");
        fe_flush();
        fe_reboot();
    }

    /* ★ 设备管理器身份与硬件窗口：必须在启动任何单元之前做完 ★
     * 因为 blkd 的命令行里就要带 --bar4 / --virtio，而那两个值只能由探测
     * 得到。顺序上它排在 pcid 之前：两者都要用配置空间端口，而那是独占资源。 */
    bool any_probe = busmaster_probe();
    if (virtio_blk_probe()) {
        any_probe = true;
    }
    if (any_probe) {
        busmaster_release_ports();
    }
    if (g_busmaster_ready || g_vio_ready) {
        for (u32 i = 0; i < NUNITS; i++) {
            /* ★ 探测到的参数分给**不同的实例** ★
             * 系统盘（blkd）走 IDE 的总线主控窗口；基准数据盘（vblkd）
             * 走 virtio 的 MMIO 窗口。把两组参数混给同一个实例，
             * 它就会用"有 virtio 就用 virtio"的规则去开系统盘——
             * 而那正是我上一版踩到的：文件系统会挂到一块空盘上。 */
            if (arg_is(g_units[i].name, "blkd")) {
                if (g_busmaster_ready) {
                    g_units[i].args[0] = g_bar4_arg;
                    g_units[i].args[1] = g_bar4_val;
                }
            } else if (arg_is(g_units[i].name, "vblkd")) {
                /* vblkd 的 args[0..2] 是 --publish <名字> --data-disk，
                 * 所以 virtio 参数从 args[3] 起。顺序必须与 blkd 的解析器
                 * 一字不差：--virtio <基址> <长度> <common> <notify> <isr>
                 * <device> <乘数> <irq>。也就是**一个标志带 8 个值**，
                 * 连中断线也在里面——中断线也是从配置空间读出来的，
                 * 和另外七个数是同一批事实，分成两个标志只会多一处
                 * "数目对不上"的机会（见 blkd 里那段注释）。 */
                if (g_vio_ready) {
                    g_units[i].args[3] = g_vio_arg;
                    g_units[i].args[4] = g_vio_base;
                    g_units[i].args[5] = g_vio_size;
                    g_units[i].args[6] = g_vio_c_off;
                    g_units[i].args[7] = g_vio_n_off;
                    g_units[i].args[8] = g_vio_i_off;
                    g_units[i].args[9] = g_vio_d_off;
                    g_units[i].args[10] = g_vio_mult_s;
                    g_units[i].args[11] = g_vio_irq_val;
                    /* MSI-X（D5c）：有就传，没有就不传（驱动会退回 INTx） */
                    if (g_vio_msix_off) {
                        g_units[i].args[12] = g_vio_msix_arg;
                        g_units[i].args[13] = g_vio_msix_val;
                        g_units[i].args[14] = g_vio_bd_val;
                    }
                }
            }
        }
    }
    say("\n");

    /* A/B 的派生事实（misc 位置、控制块位置、更新器身份、当前槽）。
     * ★ 从清单算出来的，而不是让用户态自己再解析一遍清单 ★
     * 两边各解析一次只会引入"两份解析结果不一致"的风险。 */
    if (fe_ab_get_info(&g_ab) == FE_OK) {
        g_ab_ok = 1;
    }

    say("[init] 单元表: 声明 ");
    num((u64)NUNITS);
    say(" 个\n");
    for (u32 i = 0; i < NUNITS; i++) {
        const struct unit *u = &g_units[i];
        say("         ");
        num((u64)i);
        say(". ");
        say(u->name);
        if (u->provides) {
            say("  提供 ");
            say(u->provides);
        }
        for (u32 k = 0; k < MAX_DEPS && u->require[k]; k++) {
            say("  require ");
            say(u->require[k]);
        }
        if (u->daemon) {
            say("  [常驻]");
        }
        if (u->action) {
            say("  [动作：在 init 内执行]");
        }
        if (u->needs) {
            /* 把"要什么硬件"打在表上：没有它的机器上这行会解释后面的跳过。 */
            say("  [需要 ");
            say((u->needs & NEED_VIRTIO_BLK) ? "virtio-blk" : "?");
            say("]");
        }
        say("\n");
    }

    u32 pf = preflight();
    g_fail += pf;
    if (topo_sort() != 0) {
        say("[init] 无法定序，按声明顺序退回（**不是**正确顺序，请修依赖）\n");
        for (u32 i = 0; i < NUNITS; i++) {
            g_start_order[i] = i;
        }
        g_order_len = NUNITS;
        g_fail++;
    }
    apply_want_ordering();

    /* 把算出来的顺序打出来。这是拓扑排序生效的直接证据——
     * 表里 blkread 声明在 blkd 前面，如果它先跑就说明排序没生效。 */
    say("[init] 解析出启动顺序: ");
    for (u32 i = 0; i < g_order_len; i++) {
        say(g_units[g_start_order[i]].name);
        say(i + 1 < g_order_len ? " → " : "\n");
    }
    {
        int reordered = 0;
        for (u32 i = 0; i < g_order_len; i++) {
            if (g_start_order[i] != i) {
                reordered = 1;
            }
        }
        say(reordered ? "[init] 注：启动顺序与声明顺序**不同**，依赖排序起了作用\n"
                      : "[init] 注：本次启动顺序与声明顺序恰好一致\n");
    }

    for (u32 i = 0; i < g_order_len; i++) {
        start_unit(g_start_order[i]);
    }

    /* 所有单元都走完了：这时候才知道"这次启动到底成不成"。
     * 自报成功必须放在**这里**而不是依赖单元里就写完——
     * 写早了等于"服务还没起来就说自己好"，那样的 successful 没有意义。 */
    ab_mark_successful();

    say("\n=== init 结束，失败项 ");
    num((u64)g_fail);
    say(" ===\n");
    return (int)g_fail;
}
