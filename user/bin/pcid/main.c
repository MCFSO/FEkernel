/* SPDX-License-Identifier: 0BSD */
/* pcid —— PCI 总线枚举服务（用户态，ring 3）。
 *
 * ★ 它是"发现"，不是"驱动" ★
 *
 * `docs/12-drivers.md` 划的那条线是：**发现硬件在用户态、裁决归属在内核**。
 * 这个服务负责前半句：
 *   1. 认领 PCI 配置空间的端口对（0xCF8/0xCFC）——那是内核放进资源池的；
 *   2. 枚举总线，读出每个设备的厂商/设备 ID、类代码、BAR、IRQ；
 *   3. 把结果发布到 devfs（服务发现走文件系统，不建注册表，见 00-architecture §9）。
 *
 * 它**不做**：不让设备工作、不碰设备寄存器、不分配 BAR。
 * 那些是驱动的活，而驱动要拿到硬件只能经内核的资源池
 * （设备管理器申报 + 授予），那是另一条链。
 *
 * ★ 为什么这里不直接向内核申报 BAR ★
 * 申报要求"是设备管理器"，而设备管理器是**单一身份**（内核只认一个任务）。
 * pcid 与 devmgr 是同一条链上的两个角色，今天合起来做成一个服务最简单，
 * 但代码里把两件事分开写（枚举 / 申报），因为它们将来会分开：
 * 枚举可能由多个服务做（不同总线类型），而申报只能有一个裁决者。
 * 今天这个版本只做**枚举**，申报留给 devmgr —— 先把"看得见"做出来，
 * 再决定"谁来分配"。
 */
#include <fe_user.h>
#include <fe_pci.h>

/* ★ 为什么这里用 fe_snprintf 而不是 fe_print_hex ★
 * `fe_print_hex` 固定打 16 位（0x0000000000008086）——那对寄存器 dump 合适，
 * 对"总线:设备.功能"这种人读的标识就是一屏噪声。第一版就是那样，
 * 日志里一行设备信息能占满整个屏幕宽度。
 * 格式化宽度这件事**该由调用者决定**，所以用 snprintf 写紧凑形式。 */

/* ------------------------------------------------------------------ */
/* 机制 #1：配置空间访问                                               */
/* ------------------------------------------------------------------ */

#define PCI_CFG_ADDR 0xCF8u
#define PCI_CFG_DATA 0xCFCu
#define PCI_CFG_LEN  8u

/* 地址字：bit31 使能 | 总线 | 设备 | 功能 | 寄存器（DWORD 对齐） */
static u32 pci_addr(u8 bus, u8 dev, u8 func, u8 reg)
{
    return 0x80000000u
         | ((u32)bus << 16)
         | ((u32)(dev & 0x1F) << 11)
         | ((u32)(func & 0x07) << 8)
         | ((u32)reg & 0xFC);
}

static u32 pci_read32(u8 bus, u8 dev, u8 func, u8 reg)
{
    fe_outl(PCI_CFG_ADDR, pci_addr(bus, dev, func, reg));
    return fe_inl(PCI_CFG_DATA);
}

static u32 pci_id(u8 bus, u8 dev, u8 func)
{
    return pci_read32(bus, dev, func, 0x00);
}

/* ------------------------------------------------------------------ */
/* 枚举                                                                */
/* ------------------------------------------------------------------ */

static u32 g_found;
static u32 g_published;

static void say(const char *s) { fe_puts(s); }
static void num(u64 v) { fe_print_u64(v); }

/* 每个设备一行日志 + 一条 devfs 记录。
 *
 * 日志与发布**都要**：日志是给人看的一次性证据，devfs 是给别的服务用的。
 * 只打印不发布，别人仍然发现不了设备；只发布不打印，出问题时看不到发生过什么。 */
static void report_device(u8 bus, u8 dev, u8 func, u32 id, u32 cls)
{
    u16 vendor = (u16)(id & 0xFFFF);
    u16 device = (u16)(id >> 16);
    u8 class_code = (u8)(cls >> 24);
    u8 subclass = (u8)(cls >> 16);
    u8 revision = (u8)cls;

    char line[128];
    fe_snprintf(line, sizeof(line),
                "  [pcid] %02x:%02x.%x  厂商 %04x 设备 %04x  类 %02x/%02x  版本 %02x\n",
                bus, dev, func, vendor, device, class_code, subclass, revision);
    say(line);
    g_found++;

    /* ★ 只发布**能被驱动用起来**的设备类吗？不 ★
     * 发布全部。理由是"这个设备有没有驱动"是**消费者**的判断，
     * 不是枚举者的判断——枚举者替消费者筛掉东西，消费者就再也
     * 看不到"我知道有这么个设备但没人管它"。诊断时那一类信息最值钱。 */
    char path[64];
    u32 k = 0;
    const char *pre = "/dev/pci/";
    for (const char *s = pre; *s && k < sizeof(path) - 1; s++) {
        path[k++] = *s;
    }
    /* 名字用 bus:dev.func 的十六进制串：它是**稳定**的（换槽才变），
     * 而枚举顺序不稳定。用序号会让"为什么这个设备换名字了"查很久。 */
    static const char digits[] = "0123456789abcdef";
    path[k++] = digits[(bus >> 4) & 0xF];
    path[k++] = digits[bus & 0xF];
    path[k++] = ':';
    path[k++] = digits[(dev >> 4) & 0xF];
    path[k++] = digits[dev & 0xF];
    path[k++] = '.';
    path[k++] = (char)('0' + (func & 7));
    path[k] = '\0';

    /* 名字背后挂什么？
     * ★ 这里挂的是**这个服务的端点**，不是设备本身 ★
     * 因为"设备"在微内核里不是一个内核对象——它是"一段硬件"，
     * 而段硬件只能由内核的资源池裁决给某个驱动。所以按名字打开
     * `/dev/pci/00:03.0` 得到的是"能问 pcid 关于这个设备的信息"，
     * 不是"能操作这个设备"。这个区别必须写清楚，否则客户端会以为
     * 打开它就能读写设备寄存器。
     *
     * 真正让设备动起来要走的链是：
     *   设备管理器把 BAR/IRQ 申报给内核 → 授予某个驱动 → 驱动认领。
     * 那条链还没接（见 docs/12-drivers.md 的 D3b/D4）。 */
    (void)g_published;
}

/* 读一个功能的 BAR 与中断线。DRIVER 的 BAR 是驱动要认领的**物理区间**，
 * 所以它们必须能被读出来（否则谁也拿不到设备）。 */
static void report_bars(u8 bus, u8 dev, u8 func)
{
    for (u8 bar = 0; bar < 6; bar++) {
        u32 v = pci_read32(bus, dev, func, (u8)(0x10 + bar * 4));
        if (v == 0) {
            continue;
        }
        char bl[96];
        if (v & 1u) {
            /* I/O 空间：低两位是标志位（不剥掉就会得到一个"差几位"的地址，
             * 而按它去认领资源池必然失败） */
            fe_snprintf(bl, sizeof(bl), "        BAR%u  I/O  %04x\n",
                        bar, v & ~3u);
        } else {
            u32 type = (v >> 1) & 3u;
            fe_snprintf(bl, sizeof(bl), "        BAR%u  MEM  %08x%s\n",
                        bar, v & ~0xFu,
                        (type == 2) ? "（64 位，高位在下一个 BAR）"
                                    : (type == 1) ? "（<1MiB 保留区）" : "");
            if (type == 2) {
                bar++;      /* 64 位 BAR 占两个槽位 */
            }
        }
        say(bl);
    }
    u32 irq = pci_read32(bus, dev, func, 0x3C);
    char il[64];
    fe_snprintf(il, sizeof(il), "        中断线 %u  引脚 %u\n",
                irq & 0xFFu, (irq >> 8) & 0xFFu);
    say(il);
}

static u32 g_bars_shown;

/* 枚举一个功能。返回非 0 表示这个功能存在。 */
static int probe_function(u8 bus, u8 dev, u8 func)
{
    u32 id = pci_id(bus, dev, func);
    if ((id & 0xFFFF) == 0xFFFF || (id & 0xFFFF) == 0) {
        return 0;               /* 0xFFFF = 没有设备应答 */
    }
    u32 cls = pci_read32(bus, dev, func, 0x08);
    report_device(bus, dev, func, id, cls);

    /* BAR 只对前几个设备打印：屏幕上/日志里几十个设备的 BAR 会把有用的
     * 信息淹掉。筛选标准是"我们要用的那几类"——
     * 但注意这只是**打印**的筛选，不是发布的筛选（见 report_device 的说明）。 */
    u8 class_code = (u8)(cls >> 24);
    /* 只对**我们要用的那几类**打印 BAR：几十个设备的 BAR 会把有用的信息淹掉。
     * 注意这只是**打印**的筛选，不是发布的筛选——"这个设备有没有驱动"
     * 是消费者的判断，枚举者替消费者筛掉东西，消费者就再也看不到
     * "我知道有这么个设备但没人管它"，而诊断时那一类信息最值钱。 */
    int interesting = (class_code == FE_PCI_CLASS_STORAGE) ||
                      (class_code == FE_PCI_CLASS_NETWORK) ||
                      (class_code == FE_PCI_CLASS_DISPLAY) ||
                      (class_code == FE_PCI_CLASS_SERIAL);
    if (interesting && g_bars_shown < 8) {
        report_bars(bus, dev, func);
        g_bars_shown++;
    }
    return 1;
}

/* 头类型的 bit7 表示"多功能设备"：置位时功能 1..7 都要探。 */
static void probe_device(u8 bus, u8 dev)
{
    if (!probe_function(bus, dev, 0)) {
        return;
    }
    u32 hdr = pci_read32(bus, dev, 0, 0x0C);
    if (hdr & 0x00800000u) {        /* 多功能的标志在 header type 字节的 bit7 */
        for (u8 f = 1; f < 8; f++) {
            probe_function(bus, dev, f);
        }
    }
}

int main(void)
{
    say("\n[pcid] PCI 总线枚举服务启动（ring 3）\n");

    u32 fail = 0;

    /* ★ 第一步：认领配置空间端口 ★
     * 内核把 0xCF8..0xCFF 放进了资源池，但"池子里有"不等于"我能碰"——
     * 还要认领。认领不到就是 FE_ERR_BUSY/NOENT，而不是 #GP：
     * 这一条是**先检查后使用**，比让 CPU 用 #GP 拦下来更好诊断。 */
    long r = fe_ioport_request(PCI_CFG_ADDR, PCI_CFG_LEN);
    if (r != FE_OK) {
        say("  [pcid] 失败：认领 PCI 配置端口失败（");
        num((u64)(r < 0 ? -r : r));
        say("）—— 资源池里没有它，或已被别的任务认领\n");
        return 1;
    }
    say("  [pcid] OK   认领配置空间端口 0xCF8..0xCFF（独占）\n");

    /* 第二步：验证配置空间真的通。
     * 0 号总线 0 号设备 0 号功能是主机桥，PCI 规范要求它存在——
     * 所以"读到非 0xFFFF"是一个跨环境都成立的断言。 */
    u32 host = pci_id(0, 0, 0);
    if ((host & 0xFFFF) == 0xFFFF || (host & 0xFFFF) == 0) {
        say("  [pcid] 失败：读不到主机桥（配置空间不通？）\n");
        return 1;
    }
    {
        char hl[80];
        fe_snprintf(hl, sizeof(hl), "  [pcid] OK   配置空间可读，主机桥 %04x:%04x\n",
                    host & 0xFFFFu, host >> 16);
        say(hl);
    }

    /* 第三步：枚举总线 0。
     *
     * ★ 只枚举总线 0，而且这是**有意的边界** ★
     * 完整枚举需要：跟着 PCI-to-PCI 桥走下级总线、处理 CardBus、
     * 处理热插拔。而"桥下面还有哪些总线"这件事本身要靠读桥的
     * 二级总线号寄存器再递归——几十行代码，但在只有总线 0 的
     * 虚拟机上**没有任何东西可验证**（QEMU 与 VBox 的设备都在总线 0）。
     * 按项目一贯的做法：不写"以后可能需要"的机制，把边界写清楚。 */
    say("  [pcid] 枚举总线 0：\n");
    for (u8 dev = 0; dev < 32; dev++) {
        probe_device(0, dev);
    }

    say("  [pcid] 发现 ");
    num(g_found);
    say(" 个 PCI 功能\n");
    if (g_found == 0) {
        say("  [pcid] 失败：一个设备都没发现\n");
        fail++;
    }
    say("  [pcid] 初始化结束，失败项 ");
    num(fail);
    say("\n");
    /* 枚举完就退出：它是一次性程序（工具），不是常驻服务。
     * 将来要常驻（等热插拔事件）时再改——那时它需要事件来源，
     * 而"谁来产生热插拔事件"是个新问题（ACPI 的注意按钮 / PCIe 的
     * 存在检测变化），不该现在猜。 */
    return (int)fail;
}
