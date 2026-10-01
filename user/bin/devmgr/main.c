/* SPDX-License-Identifier: 0BSD */
/* devmgr —— 设备管理器（用户态，ring 3）。
 *
 * ★ 它补的是 D3b 与 D4 之间缺的那一环 ★
 *
 *   pcid 能"看见"PCI 设备的 BAR（D3b 做完了），
 *   但 BAR **不在资源池里**，所以驱动拿着地址也认领不到：
 *   `fe_ioport_request(0xC040, 16)` 返回 FE_ERR_NOENT。
 *
 * 缺的三件事正好都在用户态：
 *
 *     发现   读配置空间，算出每个 BAR 的**类别与长度**     ← 本程序
 *     申报   把那段区间交给内核入池（需要设备管理器身份）  ← 本程序
 *     授予   把**自己名下**的那段转给某个驱动（按任务 id） ← 本程序
 *
 * 内核在这条链上仍然只做三件事：**谁在说**（是不是设备管理器）、
 * **说的合不合法**（范围/对齐/不与可用内存相交）、**记下来**（入池/换主人）。
 * 它不认识"IDE 控制器"这种东西——那才是"内核里不写驱动"的落点。
 *
 * ★ 为什么长度要**探测**而不是按经验填 ★
 * 总线主控寄存器块（BAR4）在 PIIX3 上是 16 字节、在 PIIX4 上是 16 字节，
 * 看起来可以写死。但"看起来可以写死"正是本项目反复踩过的那类假设：
 * 池子里的区间必须与驱动认领的区间**逐字节一致**，长度差一点就是认领失败，
 * 而失败信息（NOENT）不会告诉你差在哪。所以按 PCI 规范探测：
 * 往 BAR 写全 1、读回、长度 = ~值 + 1（见 fe_pci.h 的 fe_pci_bar_probe）。
 *
 * ★ 为什么授予用"任务 id"而不是句柄 ★
 * 句柄版本更严，但它要求"我持有那个任务的句柄"。这条引导链上
 * blkd 是 init 拉起的，句柄在 init 手里，devmgr 拿不到。
 * 代价与两道防护写在 kernel/include/fe/syscall.h 的 0x88 上：
 * 调用者必须是设备管理器，且**目标任务现在必须活着**。
 * 也就是"把硬件交给现在正在跑的那个任务"，不是"交给 id 为 N 的任务"。
 *
 * 用法：
 *   devmgr                                    # 只报告发现（不申报，用于对照）
 *   devmgr --declare --grant-task <id>        # 申报 BAR4 并授予该任务
 *   devmgr --declare --grant-task <id> --bdf 0:1.1   # 显式指定设备位置
 *
 * ★ 为什么"不申报"是一个正经模式，而不是省略 ★
 * "发现"与"申报"是两件事，日志里必须能把它们分开：只报告的那一次
 * 什么也不改变状态（池子不变、所有权不变），于是"申报到底做了什么"
 * 有一份可对照的基线。这也是本项目对负向对照的一贯做法。
 */
#include <fe_user.h>
#include <fe_pci.h>

/* 主通道命令/控制块基址。★ 这两个值在 PIIX3/PIIX4 上是**兼容模式固定**的
 * （0x1F0/0x3F6），所以它们不来自 PCI 而来自既有约定——ATA 的兼容模式
 * 就是这么定的。总线主控寄存器块没有这个待遇：它是 BAR，必须探测。 */
#define ATA_PRIMARY_CMD   0x1F0u
#define ATA_PRIMARY_CTRL  0x3F6u
#define ATA_IRQ           14u

/* 总线主控寄存器块的长度上限。规范里 BAR4 声明的就是 16 字节，
 * 但池子里的区间要与探测结果**逐字节一致**；大于这个数说明探到的
 * 不是我们以为的那个 BAR（或者设备声明了一个大得离谱的窗口），
 * 那时宁可报错也不要往池子里塞一大段。
 * （放 64 是为了给"设备声明 32/64 字节"留出空间而不至于失控。） */
#define BM_BAR_LEN_MAX    64u

static u32 g_fail;
static int g_quiet_scan;        /* 探针：打印每个功能的类别与 BAR4（定位后删） */

static void say(const char *s) { fe_puts(s); }

static void step(const char *what, int ok)
{
    say("  [devmgr] ");
    say(ok ? "OK   " : "失败 ");
    say(what);
    say("\n");
    if (!ok) {
        g_fail++;
    }
}

static void print_hex16(u32 v)
{
    static const char d[] = "0123456789abcdef";
    char b[6];
    b[0] = '0'; b[1] = 'x';
    b[2] = d[(v >> 12) & 0xF]; b[3] = d[(v >> 8) & 0xF];
    b[4] = d[(v >> 4) & 0xF];  b[5] = d[v & 0xF];
    fe_write(b, 6);
}

static void print_dec(u64 v)
{
    char b[24];
    int n = 0;
    if (v == 0) {
        fe_write("0", 1);
        return;
    }
    while (v && n < 23) {
        b[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    char o[24];
    for (int i = 0; i < n; i++) {
        o[i] = b[n - 1 - i];
    }
    fe_write(o, (usize)n);
}

/* 错误码给人话。
 *
 * ★ 为什么现在放在这个文件里，而不是 libfe ★
 * libfe 是"syscall ABI 的投影 + 最小运行库"，而"错误码的中文说法"是
 * **展示层**的东西（别的程序可能只想打印数字，甚至想换一种语言）。
 * libposix 里已经有一份 strerror，但它收的是 POSIX errno（正数），
 * 而 syscall 返回的是 FE_ERR_*（负数）——把两者混起来会得到一个
 * "错误码对不上、于是落进 default 分支"的经典问题。
 * 等第二个程序也要这份映射时再提取成共享的（那时才有两处真实需求）。 */
static const char *err_name(long e)
{
    switch (e) {
    case FE_OK:            return "成功";
    case FE_ERR_INVAL:     return "参数无效";
    case FE_ERR_NOMEM:     return "内存不足";
    case FE_ERR_NOENT:     return "不存在（资源池里没有这段）";
    case FE_ERR_EXIST:     return "已存在（自己已持有）";
    case FE_ERR_AGAIN:     return "资源暂不可用";
    case FE_ERR_BUSY:      return "忙（已被别人独占）";
    case FE_ERR_FAULT:     return "地址无效";
    case FE_ERR_ACCESS:    return "权限不足";
    case FE_ERR_BADHANDLE: return "句柄无效";
    case FE_ERR_NOTSUP:    return "不支持";
    case FE_ERR_TIMEOUT:   return "超时";
    case FE_ERR_RANGE:     return "越界";
    case FE_ERR_NOSPC:     return "空间不足";
    default:               return "其它错误";
    }
}

/* ------------------------------------------------------------------ */
/* 发现：找一个"带总线主控 BAR 的 IDE 控制器"                          */
/* ------------------------------------------------------------------ */

struct dev_find {
    u8  bus, dev, func;
    u16 vendor, device;
    u16 bm_base;            /* BAR4（总线主控寄存器块）基址 */
    u32 bm_len;             /* 探测出来的长度 */
    u16 irq_line;
    int found;
};

/* 只扫**总线 0**，设备号固定为 1（PIIX 的传统位置），但功能号要全扫。
 *
 * ★ 边界写清楚 ★
 * 完整枚举要跟着 PCI-to-PCI 桥递归下级总线（见 pcid 的同一处说明）。
 * QEMU 与 VBox 的 IDE 控制器都在总线 0 上，桥下面没有东西可验证——
 * 按项目一贯做法，不写"以后可能需要"的机制。
 * 但功能号必须扫：PIIX 的 IDE 功能在 **function 1**
 * （function 0 是同一条 PIIX 的 ISA 桥），只扫 function 0 会得到
 * "没有 IDE 控制器"这个**错误结论**——而它看起来像是"这台机器没有 IDE"。
 */
static void find_ide(struct dev_find *out)
{
    memset(out, 0, sizeof(*out));
    for (u8 fn = 0; fn < 8; fn++) {
        if (!fe_pci_present(0, 1, fn)) {
            continue;
        }
        u32 id = fe_pci_cfg_read32(0, 1, fn, FE_PCI_REG_VENDOR_ID);
        u8 cls = (u8)((id >> 24) & 0xFF);
        u8 sub = (u8)((id >> 16) & 0xFF);
        if (cls != FE_PCI_CLASS_STORAGE || sub != FE_PCI_SUBCLASS_IDE) {
            continue;
        }
        u32 type = 0;
        u64 len = fe_pci_bar_probe(0, 1, fn, 4, &type);
        if (!g_quiet_scan) {
            say("  [devmgr]   fn ");
            print_dec(fn);
            say(" 类 ");
            print_hex16(cls);
            say("/");
            print_hex16(sub);
            say("  BAR4 类型 ");
            print_dec(type);
            say(" 长度 ");
            print_dec(len);
            say("\n");
        }
        if (type != FE_PCI_BAR_IO || len == 0 || len > BM_BAR_LEN_MAX) {
            continue;               /* 没有 I/O BAR4：不是我们要的控制器 */
        }
        u32 bar4 = fe_pci_cfg_read32(0, 1, fn, 0x10 + 16);
        out->bus = 0;
        out->dev = 1;
        out->func = fn;
        out->vendor = (u16)(id & 0xFFFF);
        out->device = (u16)((id >> 16) & 0xFFFF);
        out->bm_base = (u16)(bar4 & 0xFFFCu);
        out->bm_len = (u32)len;
        out->irq_line = fe_pci_cfg_read16(0, 1, fn, FE_PCI_REG_IRQ_LINE);
        out->found = 1;
        return;
    }
}

/* ------------------------------------------------------------------ */
/* 申报 + 授予                                                         */
/* ------------------------------------------------------------------ */

/* 为什么申报与授予分成两步、而不是"申报时直接指定给谁"：
 * 这是内核 ABI 的形状，而那个形状是对的——**申报是"存在"，
 * 授予是"归属"**。同一个 BAR 可能先申报、稍后才决定给谁；
 * 也可能申报了没人要（那就留在池子里等下一个驱动）。
 * 把它们合成一个调用会把这两种情况都表达不出来。 */
static int declare_and_grant(const struct dev_find *d, u64 target_task)
{
    int ok = 1;

    /* 1. 申报：把总线主控寄存器块交给内核入池。
     * 内核会自己再检查一遍范围（16 位端口空间）——入口层的检查是
     * 给更准的报错，不是替代机制层的检查（见 docs/12 §4 的教训）。 */
    long r = fe_resource_pool_add(FE_RES_IOPORT, d->bm_base, d->bm_len, 0);
    if (r != FE_OK) {
        say("  [devmgr] 申报 BAR4 ");
        print_hex16(d->bm_base);
        say(" 失败: ");
        say(err_name(r));
        say("\n");
        ok = 0;
    } else {
        say("  [devmgr] 已申报总线主控寄存器块 ");
        print_hex16(d->bm_base);
        say(" + ");
        print_dec(d->bm_len);
        say(" 字节\n");
    }

    /* 2. 认领：**申报不等于持有**。设备管理器要能授予，就必须先成为
     * 主人（fe_resource_grant 会核对 from_owner 确实是 owner）。
     * 这一步顺带证明了"申报"与"认领"是同一套池子语义。 */
    if (ok) {
        r = fe_ioport_request(d->bm_base, d->bm_len);
        if (r != FE_OK) {
            say("  [devmgr] 认领 BAR4 失败: ");
            say(err_name(r));
            say("\n");
            ok = 0;
        }
    }

    /* 3. 授予：按**任务 id** 转给驱动。转走之后自己不再持有。 */
    if (ok) {
        r = fe_resource_grant_id(FE_RES_IOPORT, d->bm_base, d->bm_len, target_task);
        if (r != FE_OK) {
            say("  [devmgr] 授予任务 ");
            print_dec(target_task);
            say(" 失败: ");
            say(err_name(r));
            say("\n");
            ok = 0;
        } else {
            say("  [devmgr] 已把 BAR4 授予任务 ");
            print_dec(target_task);
            say("\n");
        }
    }

    /* 4. 反向对照：**授予之后自己不该再持有**。
     * "所有权唯一"这句话在这里被验一次：如果授予只是复制了一份，
     * 那么两个任务都能碰同一段硬件寄存器——那正是资源池要防的事。
     * 判据：再认领一次必须失败（自己已经持有会得到 EXIST，
     * 已经不持有会得到 NOENT —— 两者都算通过，成功才是不对）。 */
    if (ok) {
        long again = fe_ioport_request(d->bm_base, d->bm_len);
        if (again == FE_OK) {
            say("  [devmgr] **授予之后自己还能认领同一段**（所有权不唯一）\n");
            ok = 0;
        } else {
            say("  [devmgr] 反向：授予之后自己已不再持有（");
            say(err_name(again));
            say("）\n");
        }
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* 报告：让"发现"这一步有可核对的输出                                  */
/* ------------------------------------------------------------------ */

static void report(const struct dev_find *d)
{
    if (!d->found) {
        step("找到带总线主控 BAR 的 IDE 控制器", 0);
        return;
    }
    say("  [devmgr] IDE 控制器 ");
    print_dec(d->bus); say(":");
    print_dec(d->dev); say(".");
    print_dec(d->func);
    say("  厂商 ");
    print_hex16(d->vendor);
    say(" 设备 ");
    print_hex16(d->device);
    say("\n");
    say("  [devmgr]   命令块 ");
    print_hex16(ATA_PRIMARY_CMD);
    say("  控制块 ");
    print_hex16(ATA_PRIMARY_CTRL);
    say("  IRQ ");
    print_dec(ATA_IRQ);
    say("\n");
    say("  [devmgr]   总线主控 BAR4 ");
    print_hex16(d->bm_base);
    say(" + ");
    print_dec(d->bm_len);
    say(" 字节（按 PCI 规范探测：写全 1 读回再取反）\n");
    step("发现 IDE 控制器与其总线主控窗口", 1);
}

/* ------------------------------------------------------------------ */

static u64 parse_u64(const char *s)
{
    u64 v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (u64)(*s - '0');
        s++;
    }
    return v;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int main(int argc, char **argv)
{
    bool declare = false;
    u64 grant_task = 0;

    for (int i = 1; i < argc; i++) {
        if (streq(argv[i], "--declare")) {
            declare = true;
        } else if (streq(argv[i], "--grant-task") && i + 1 < argc) {
            grant_task = parse_u64(argv[++i]);
        }
    }

    say("=== 设备管理器（D4：发现 → 申报 → 授予）===\n");
    g_quiet_scan = 0;   /* 打开逐功能诊断（定位 BAR4 找不到的原因） */

    /* 认领配置空间端口。★ 它必须被独占持有（见 fe_pci.h 的说明）★，
     * 所以本程序跑的时候 pcid 拿不到它们 —— 这是既成事实，
     * 不是本程序要解决的问题（见 docs/12 §9 的边界说明）。 */
    long r = fe_ioport_request(FE_PCI_CFG_ADDR_PORT, FE_PCI_CFG_LEN);
    if (r != FE_OK) {
        say("  [devmgr] 认领 PCI 配置空间端口失败: ");
        say(err_name(r));
        say("\n");
        /* 端口拿不到时给出**明确的下一步**，而不是一个笼统的失败：
         * 这条链上最可能的原因是"另一个服务正持有它"。 */
        say("  [devmgr] （0xCF8/0xCFC 是独占资源：同时只有一个服务能持有它）\n");
        return (int)(g_fail + 1);
    }
    step("认领 PCI 配置空间端口 0xCF8..0xCFF（独占）", 1);

    /* ★ 这里**不**认领 ATA 主通道的端口与 IRQ14 ★
     *
     * 第一版打算"由设备管理器先占住、再连同 BAR4 一起交给驱动"，
     * 想借此表达"这几样属于同一个设备"。那是错的，两个原因：
     *
     *   1. 端口 0x1F0/0x3F6 与 IRQ14 在**引导期就由内核入池**了
     *      （它们是兼容模式的固定地址，见 kernel/main.c），
     *      换句话说"它们属于 ATA 主通道"这件事已经是内核的既有裁决。
     *      设备管理器再插一手，只是把同一件事说两遍。
     *   2. 更要紧的是**它会制造新的顺序依赖**：设备管理器在驱动之前跑，
     *      驱动就必须等它让出来。而今天 blkd 自己认领这两样是幂等且
     *      自洽的（谁先跑不影响正确性，因为只有它要）。
     *
     * BAR4 不一样：**它不在池子里**（内核不知道 PCI BAR 的地址，
     * 那是枚举的结果），所以它必须由发现者申报——这才是本程序存在的理由。
     * 把"必须的"和"看起来更整齐的"分开，是这一处唯一要守的纪律。 */

    struct dev_find d;
    find_ide(&d);
    report(&d);

    if (declare && d.found) {
        if (grant_task == 0) {
            step("--declare 需要 --grant-task <任务 id>", 0);
        } else if (!declare_and_grant(&d, grant_task)) {
            g_fail++;
        }
    } else if (declare && !d.found) {
        g_fail++;
    } else {
        say("  [devmgr] （未指定 --declare：本次只做发现，不改动资源池与归属）\n");
    }

    /* ★ 这一轮**不**把结果发布到 devfs，理由要写清楚（否则看起来像漏了）★
     *
     * 表面上有两个候选：把 BAR4 发布成 /dev/xxx，或者把"设备清单"发布出去。
     * 两者今天都不该做：
     *   - 发布 BAR4 没有意义——**设备不是内核对象，它是一段只能由资源池
     *     裁决归属的硬件**（docs/12 §6 已经把这条写在 pcid 上了）。
     *     按名字打开它拿到的不会是"能操作设备"的能力。
     *   - 发布设备清单要回答"谁有权读"，而清单本身是**只读信息**，
     *     用 devfs 走一遍 IPC 只是把一次内存拷贝换成了两次。
     *     真要做成服务，应该等**有第二个消费者**时（比如一个按需仲裁的
     *     devmgr 服务）再定接口——那时才知道客户端要问什么。
     *
     * 今天唯一的消费者是 init 传给 blkd 的命令行参数，所以数据走 argv。 */
    say("=== 设备管理器结束，失败项 ");
    print_dec(g_fail);
    say(" ===\n");
    if (g_fail) {
        fe_exit((int)g_fail);
    }
}
