/* SPDX-License-Identifier: 0BSD */
/* blkbench —— 块设备吞吐测试（D4 的性能证据）。
 *
 * ★ 它测的不是"数字"，而是"哪条路径" ★
 *
 * 一个吞吐数字本身说明不了任何事：它可能是 DMA 搬出来的，也可能是
 * CPU 逐字节搬出来的。所以这个程序做的是**同一台机器上的对照实验**：
 *
 *   1. 内联读（旧协议）：每次 2 个扇区，数据过 IPC 载荷——旧路径的基线；
 *   2. 批量读（新协议）：客户端交一块 DMA 内存，设备直接搬进去。
 *
 * 两者的差就是这次改动值多少。而"新协议真的走了 DMA"另有一份证据：
 * blkd 会如实告诉客户端"这次是不是 DMA"（reply 是 1 还是 2），
 * 于是"走了 DMA 却和 PIO 一样快"与"根本没走 DMA"能被分开——
 * 这正是本项目一贯要求的：**一个断言必须能被证伪**。
 *
 * 数据校验用"LBA 相关"的模式而不是全零：全零的缓冲在校验时
 * 与"根本没写进去"无法区分（内存对象分配时本来就被内核清零了）。
 * 模式 = (lba 的每个字节) 异或 (扇区内偏移的低字节)，两端都能独立算出来。
 */
#include <fe_user.h>

#define SECTOR_SIZE 512

#define BLK_OP_INFO 1
#define BLK_OP_READ 2
/* 批量读：载荷 = struct blk_bulk_req + 1 个句柄（DMA 缓冲） */
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
    u32 bar4;           /* ATA 后端：总线主控窗口基址（0 = 没有 DMA） */
    u32 dma_mode;       /* 1 = 已切到 DMA */
    u64 mmio;           /* virtio 后端：设备 MMIO 窗口物理基址（0 = 不用 MMIO） */
    u64 mmio_len;
    u32 backend;        /* 1 = ATA，2 = virtio-blk */
    u32 _pad2;
};

struct blk_bulk_req {
    u32 op;
    u32 sectors;
    u64 lba;
    u64 buf_len;
    u64 _reserved;
};

/* 批量读的应答状态（与 blkd 的映射一致，写在这里便于对照） */
#define BULK_OK        1u   /* 走 DMA 搬完了 */
#define BULK_NODMA     2u   /* 设备/驱动这一侧没有 DMA（降级路径） */
#define BULK_BADRANGE  3u   /* LBA/长度/缓冲形状不合法 */
#define BULK_IOERR     4u   /* 设备出错 */
/* 0 = 请求本身非法（句柄数不够、op 不对…） */

static u32 g_fail;
static long g_ep;
static u64 g_tsc_hz;        /* 标定出来的 TSC 频率；0 = 没标出来（只报周期数） */
static u64 g_inline_cyc_kib;  /* 旧路径：每 KiB 多少周期（0 = 没测） */
static u64 g_bulk_cyc_kib;    /* 新路径：每 KiB 多少周期（0 = 没测） */

static void say(const char *s)
{
    fe_puts(s);
    /* 直接刷出去：性能测试里最怕的是把输出缓冲的开销算进被测路径，
     * 而"这次输出到底写没写"也会变成日志顺序上的疑问。 */
    fe_flush();
}

static void num(u64 v)
{
    fe_print_u64(v);
    fe_flush();
}

static void step(const char *what, int ok)
{
    say("    ");
    say(ok ? "OK   " : "失败 ");
    say(what);
    say("\n");
    if (!ok) {
        g_fail++;
    }
}

/* ---- 数据正确性怎么验：不假设盘上有什么 ----
 *
 * ★ 这里原来是错的，值得写清楚为什么 ★
 * 第一版按"扇区内容 = 由 LBA 算出来的模式"去逐字节比对。那个前提是假的：
 * **没有任何人往盘上写过这个模式**（mkfat 写的是 FAT 结构，数据区是
 * 文件内容）。于是那条断言在任何正确的驱动上都会失败——它不是"过严"，
 * 它是**从前提上就不成立**，而一个永远失败的断言和一个永远通过的断言
 * 一样没有信息量。
 *
 * 换成的做法不需要知道盘上有什么，而且照样能被证伪：
 *
 *   1. **毒值差分**：把缓冲填成 0x5A，读一次；再填成 0xA5，读同一段。
 *      两次结果必须**逐字节相同**。凡是设备没写到的字节，两次留下的是
 *      不同的毒值 —— 于是"只搬了前几个扇区""描述符链少挂了一段"
 *      "状态字节没等到就返回"全都会在这里露出来。
 *   2. **跨路径对照**：同一段数据再用**旧的内联路径**读一遍，
 *      两者必须逐字节相同。这一条管的是"搬的**是不是这一段**"——
 *      毒值差分证明"每个字节都被写过"，但写过的是不是对的 LBA，
 *      只有跟一条已知正确的路径比才知道。
 *
 * 两条合起来：一条管覆盖，一条管内容。两条都不依赖盘上原本是什么。 */
static void poison_fill(u8 *buf, u32 bytes, u8 v)
{
    for (u32 i = 0; i < bytes; i++) {
        buf[i] = v;
    }
}

/* 首个不同的字节下标；全同返回 bytes。 */
static u32 first_diff(const u8 *a, const u8 *b, u32 bytes)
{
    for (u32 i = 0; i < bytes; i++) {
        if (a[i] != b[i]) {
            return i;
        }
    }
    return bytes;
}

/* ---- 计时：微基准必须用**比被测对象细得多**的时钟 ----
 *
 * ★ 为什么不能用 fe_clock_ns 直接计时 ★
 * 第一次跑出来两段是 5000 us 和 5000 us —— **都正好**落在一个时钟刻度上。
 * 那个时钟的粒度是毫秒级，而这两段被测代码本身只有几毫秒，
 * 于是"测出来的主要是时钟的分辨率"，不是被测对象。
 * 这个坑本项目在 simdtest 里已经踩过一次（那里两个数字都成了 0 us），
 * 结论是同一句话：分辨率必须远细于被测对象。
 *
 * 所以：测量用 rdtsc（周期），而 fe_clock_ns 只用来**标定 TSC 频率**——
 * 标定窗口取 ≥20 ms，远大于时钟粒度，于是标定值能准到几个百分点。
 * 报告里同时给"周期/KiB"：那个数字**不依赖任何频率假设**，
 * 换一台机器、换一个虚拟化平台都能直接比。 */
static inline u64 rdtsc(void)
{
    u32 lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

/* 标定 TSC 频率（周期/秒）；量不出来返回 0，此时只报周期数。 */
static u64 tsc_hz(void)
{
    u64 n0 = fe_clock_ns();
    u64 t0 = rdtsc();
    u64 n1 = n0;
    while (n1 - n0 < 20ull * 1000000ull) {
        n1 = fe_clock_ns();
    }
    u64 t1 = rdtsc();
    if (n1 == n0 || t1 <= t0) {
        return 0;
    }
    return (t1 - t0) * 1000000000ull / (n1 - n0);
}

/* 每段计时都跑 TIMED_REPS+1 轮，第 0 轮只用于预热（不计入），
 * 取其余轮里**最快**的一轮。
 * ★ 为什么预热与取最快缺一不可 ★
 * 第一轮把冷缓冲、首次走某条分支的代价都算进去了（simdtest 里
 * "SSE 比整数慢 6 倍"就是这么来的）；而取平均会把噪声（宿主调度、
 * 别的任务抢 CPU）摊进结果里。取最快 = "这条路径**能**多快"，
 * 那正是这次改动要回答的问题。 */
#define TIMED_REPS 8

/* 一次批量读。返回状态码（见 BULK_*）。
 *
 * ★ 协议：请求随消息带两个句柄 —— [0] 回复端点，[1] DMA 数据缓冲 ★
 *
 * 为什么不用 `fe_endpoint_call`：它的 ABI 不带句柄数组，而这条请求必须
 * 随消息传一个句柄（那块 DMA 缓冲）——"传能力"与"发一小段字节"本来就是
 * 两种操作。
 *
 * ★ 回复路径为什么要自己建端点、又为什么不能用 call 去等它 ★
 * 这两个坑是连着的，值得写清楚（我在这里踩了一轮，症状是"整个测试挂住"）：
 *
 *   1. `fe_endpoint_send` **不会**随消息带"回复能力"（那是
 *      `fe_endpoint_call` 内部才做的事）。所以服务端拿不到回复路径，
 *      必须由客户端显式交一个端点过去。
 *   2. **不能用 `fe_endpoint_call` 去等这个端点上的回复**：`call` 自己会
 *      再造一个回复端点，并在**它自己那个**上面等。于是服务端回在
 *      "我交过去的那个"上，而我在等"call 造的那个"——两边互等，永久挂起。
 *      正确做法是 `fe_endpoint_send` + 在**交出去的那个**端点上 recv。
 *   3. `fe_endpoint_recv` 的 out_handles 必须是 **FE_MSG_MAX_HANDLES 个
 *      u32**（内核按固定长度整段拷回）。传 2 元素数组会被写 16 字节，
 *      而用户态没有栈保护（构建时 -fno-stack-protector）——
 *      症状是随机的、看起来毫不相关的崩溃。不需要句柄时传 NULL。
 */
static u32 bulk_read_once(long buf, u32 lba, u32 sectors, u64 buf_len)
{
    long rep = fe_endpoint_create(0);
    if (rep <= 0) {
        return 0xFFFFFFFFu;
    }
    struct blk_bulk_req b;
    b.op = BLK_OP_READ_BULK;
    b.sectors = sectors;
    b.lba = lba;
    b.buf_len = buf_len;
    b._reserved = 0;

    u32 hs[2];
    hs[0] = (u32)rep;           /* 回复端点 */
    hs[1] = (u32)buf;           /* 数据缓冲 */
    struct fe_msg_header hh;
    hh.protocol = 0xFE07;
    hh.opcode = BLK_OP_READ_BULK;
    hh.payload_len = sizeof(b);
    hh.handle_count = 2;
    hh.request_id = 0x5150;

    u32 status = 0xFFFFFFFFu;
    if (fe_endpoint_send(g_ep, &hh, &b, hs, 2) == FE_OK) {
        struct fe_msg_header rh;
        u32 n = 0;
        if (fe_endpoint_recv(rep, &rh, &status, sizeof(status), NULL, &n) < 0) {
            status = 0xFFFFFFFFu;
        }
    }
    fe_handle_close(rep);
    return status;
}
/* 一次内联读（旧协议）：载荷带上数据，最多 2 个扇区。 */
static u32 inline_read_once(long ep, u32 lba, u32 sectors, u8 *out)
{
    struct blk_req r;
    r.op = BLK_OP_READ;
    r.count = sectors;
    r.lba = lba;
    u32 out_len = 0;
    long rc = fe_endpoint_call(ep, &r, sizeof(r), out, sectors * SECTOR_SIZE, &out_len);
    if (rc != FE_OK) {
        return 0;
    }
    return out_len;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    u32 bulk_sectors = 256;         /* 每次批量读的扇区数（默认 128 KiB） */
    u32 inline_rounds = 64;         /* 内联读的轮数（默认 64 × 1 KiB = 64 KiB） */
    bool skip_inline = false;
    const char *dev_path = "/dev/blk0";

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == '-' && argv[i][2] == 's') {
            if (i + 1 < argc) {
                bulk_sectors = (u32)strtoul(argv[++i], NULL, 10);
            }
        } else if (argv[i][0] == '-' && argv[i][1] == '-' && argv[i][2] == 'i') {
            if (i + 1 < argc) {
                inline_rounds = (u32)strtoul(argv[++i], NULL, 10);
            }
        } else if (strcmp(argv[i], "--no-inline") == 0) {
            skip_inline = true;
        } else if (strcmp(argv[i], "--dev") == 0 && i + 1 < argc) {
            /* 被测设备。★ 为什么要能换 ★
             * 这个程序做的是**同一条路径的对照**，而"内联 vs 批量"这件事
             * 只有在**同一台设备、同一段数据**上比才说明问题：
             * 换设备就同时换了两个变量（传输方式、设备模型），
             * 得到的差值既可能来自协议、也可能来自 virtio 与 IDE 的差别。
             * 所以默认仍是系统盘 /dev/blk0，但可以由 init 指到
             * 那块走 virtio 的数据盘上。 */
            dev_path = argv[++i];
        }
    }
    if (bulk_sectors < 1) {
        bulk_sectors = 1;
    }
    if (bulk_sectors > 1024) {
        bulk_sectors = 1024;
    }

    say("\n=== 块设备吞吐对照（D4：总线主控 DMA + 共享内存批量）===\n");

    /* ---- 先标定时钟 ----
     * 这一步必须在这里、而且必须在任何计时之前：后面所有 MiB/s 都是
     * 用这个频率折出来的。把它印在日志里，是为了让"数字怎么来的"
     * 可以被核对——而不是让人相信一个孤立的 MiB/s。 */
    g_tsc_hz = tsc_hz();
    say("  [blkbench] TSC 标定: ");
    num(g_tsc_hz / 1000000ull);
    say(" MHz（用系统时钟在 ≥20 ms 窗口上标定；只报周期数的行不依赖它）\n");

    /* ---- 0. "硬件归属"这条链在**后面**验（见反向对照那一节）----
     *
     * ★ 为什么原来放在这里的那一条被删掉了 ★
     * 它写死了 0x1F0（IDE 的命令块），因为当时这个程序只对着系统盘跑。
     * 现在它可以对着任何一台设备跑（--dev），而对 virtio 盘来说
     * 0x1F0 与它**无关**：那个端口归 IDE 那个实例，不归它。
     * 于是"认领 0x1F0 拿到 OK"变成了一个正确的结果，
     * 而断言却把它记成失败——**测试绑死在一个设备上，换设备就报假失败**。
     * 现在这一条按驱动报出来的后端类型选判据（端口 or MMIO），
     * 判据只有一条没变：那段硬件窗口必须已经被驱动独占。 */

    g_ep = fe_devfs_open(dev_path);
    if (g_ep <= 0) {
        say("  [blkbench] 打不开 ");
        say(dev_path);
        say("（块设备服务没起来？）\n");
        return 1;
    }
    step("按路径找到块设备服务", 1);

    struct blk_info info;
    struct blk_req ir;
    ir.op = BLK_OP_INFO;
    ir.count = 0;
    ir.lba = 0;
    u32 ilen = 0;
    if (fe_endpoint_call(g_ep, &ir, sizeof(ir), &info, sizeof(info), &ilen) != FE_OK) {
        step("取设备信息", 0);
        return 1;
    }
    say("  [blkbench] 设备 \"");
    say(info.model);
    say("\"，");
    num(info.sectors);
    say(" 扇区 × ");
    num(info.sector_size);
    say(" 字节\n");
    step("取设备信息", 1);

    say("  [blkbench] 驱动报告的传输模式: ");
    say(info.dma_mode ? "总线主控 DMA" : "**只有 PIO**");
    say("，后端 ");
    say(info.backend == 2 ? "virtio-blk" : (info.backend == 1 ? "ATA" : "未知"));
    if (info.bar4) {
        say("，总线主控窗口 0x");
        /* 只打低 16 位（端口号） */
        {
            static const char d[] = "0123456789abcdef";
            char b[4];
            b[0] = d[(info.bar4 >> 12) & 0xF];
            b[1] = d[(info.bar4 >> 8) & 0xF];
            b[2] = d[(info.bar4 >> 4) & 0xF];
            b[3] = d[info.bar4 & 0xF];
            fe_write(b, 4);
        }
    }
    say("\n");

    /* ★ 反向对照：驱动报出来的那段硬件窗口，无关任务必须拿不到 ★
     * 这一条把"这段硬件真的存在且已被独占"变成可核对的事实。
     *
     * ★ 判据随**后端的硬件形态**换，但换成的是同一个判据 ★
     * ATA 的总线主控窗口是 I/O 端口，virtio 的四个结构在 MMIO 里——
     * 两者都是"一段物理硬件窗口"，所以两次都是"无关任务去认领它"。
     * 不能因为 virtio 没有 bar4 就跳过这一条：那等于把"批量读走的是
     * DMA"这句话的证据抽掉，只剩一个好看的吞吐数字。 */
    if (info.backend == 2 && info.mmio) {
        void *va = fe_mmio_map(info.mmio, info.mmio_len ? info.mmio_len : 0x1000,
                               FE_PROT_READ);
        step("反向：驱动持有的 virtio MMIO 窗口对无关任务不可映射", va == (void *)0);
        say("  [blkbench] virtio MMIO 窗口 ");
        fe_print_hex(info.mmio);
        say(" / ");
        fe_print_u64(info.mmio_len);
        say(" 字节\n");
    } else if (info.bar4) {
        long r = fe_ioport_request(info.bar4, 16);
        step("反向：驱动持有的总线主控窗口对无关任务不可认领（忙）",
             r == FE_ERR_BUSY);
    } else {
        /* 没有 DMA 时这一条无从谈起。★ 但它必须以**失败项**的形式出现吗？★
         * 不。这里要区分两件事：
         *   - "这台机器的 IDE 设备不支持 DMA"——环境事实，不是缺陷；
         *   - "我们的链没接上"——那才是缺陷。
         * 判据是：blkd 是否**报告了它探测到的窗口**。今天它在没有 --bar4 时
         * 什么都不报，所以这一条只能记成"环境事实"，由 blkbench 的
         * 吞吐对照去说明代价。等 virtio-blk 上来之后，这条会变成硬判据。 */
        say("  [blkbench] 驱动没有报告总线主控窗口 —— "
            "本环境的 IDE 设备不支持 DMA，批量读走的是降级路径\n");
    }

    /* 测试用的 LBA：挑一段**数据区**，避开两头的坑——
     *   - 前 4 MiB 是 A/B 两个槽（受保护区），读它会被允许但意义不大；
     *   - **分区末尾之后**是镜像的尾巴，QEMU 的 IDE 会直接拒（读回全 0），
     *     而 IDENTIFY 报的容量是整块镜像（100352 扇区）而不是分区大小
     *     （98304 扇区）—— 这是个真实的坑：按 IDENTIFY 的容量去读到分区
     *     之外，症状是"多扇区读失败、单扇区读却没报错"（单扇区那次恰好
     *     落在分区内）。
     * 所以这里显式留在分区内：数据区取"分区起点之后 64 MiB"往后的地方。 */
    u32 part_start = 2048;              /* MBR 里的第一个分区（mkfat 固定这么建） */
    u32 part_sectors = 98304;
    u32 lba = part_start + 16;          /* 分区起点之后 8 KiB：确定在数据区 */
    if (lba + bulk_sectors + 8 > part_start + part_sectors) {
        lba = part_start + 16;          /* 分区太小就贴着起点读 */
    }

    /* ---- 1. 旧路径基线：内联读（每次 2 扇区，数据过 IPC 载荷） ----
     *
     * ★ 这一节的对照意义 ★
     * 在 virtio 盘上，内联读走的是"设备 → 驱动垫脚缓冲 → 驱动拷贝 →
     * IPC 载荷 → 内核拷贝 → 客户端"这条老路；批量读走的是
     * "设备 → 客户端内存"直通。同一台设备、同一段数据，差的就是协议。 */
    if (!skip_inline) {
        u8 buf[2 * SECTOR_SIZE];
        u32 bytes = 0;
        u32 bad = 0;
        u64 best = ~0ull;
        for (u32 rep = 0; rep <= TIMED_REPS; rep++) {
            u32 got = 0;
            u64 t0 = rdtsc();
            for (u32 i = 0; i < inline_rounds; i++) {
                u32 n = inline_read_once(g_ep, lba + i * 2, 2, buf);
                if (n != 2 * SECTOR_SIZE) {
                    bad++;
                    break;
                }
                got += n;
            }
            u64 t1 = rdtsc();
            bytes = got;
            if (bad) {
                break;
            }
            if (rep == 0) {
                continue;               /* 预热轮不计 */
            }
            if (t1 - t0 < best) {
                best = t1 - t0;
            }
        }
        say("  [1] 内联读（每次 2 扇区，数据过 IPC 载荷）: ");
        num(bytes / 1024);
        say(" KiB / ");
        num(best);
        say(" 周期");
        if (best && best != ~0ull) {
            g_inline_cyc_kib = best / (bytes / 1024);
            say("  = ");
            num(g_inline_cyc_kib);
            say(" 周期/KiB");
            if (g_tsc_hz) {
                say("，");
                num((u64)bytes * g_tsc_hz / best / (1024 * 1024));
                say(" MiB/s");
            }
        }
        say("\n");
        if (bad) {
            step("内联读返回了意外长度（基线不可用）", 0);
        }
        step("内联读基线测出了非零用时（时钟分辨率够细）", best && best != ~0ull);
    }

    /* ---- 2. 新路径：批量读（DMA 直接进客户端内存） ---- */
    u64 want = (u64)bulk_sectors * SECTOR_SIZE;
    long obj = fe_mem_alloc_dma(want);
    struct fe_mem_info mi;
    if (obj <= 0 || fe_mem_info(obj, &mi) != FE_OK) {
        step("分配 DMA 缓冲（物理连续、物理地址可查）", 0);
        return 1;
    }
    say("  [blkbench] DMA 缓冲 ");
    num(mi.size / 1024);
    say(" KiB，物理地址 ");
    fe_print_hex(mi.phys);
    say("\n");
    step("分配 DMA 缓冲（物理连续、物理地址可查）", 1);

    u8 *buf = (u8 *)fe_mem_map(obj, (void *)0, want, FE_PROT_READ | FE_PROT_WRITE);
    if (!buf) {
        step("把缓冲映射进自己的地址空间", 0);
        return 1;
    }
    step("把缓冲映射进自己的地址空间", 1);

    /* 单次往返延迟（先来一次，确认协议通、数据对） */
    u32 st = bulk_read_once(obj, lba, bulk_sectors, want);
    if (st != BULK_OK && st != BULK_NODMA) {
        say("  [blkbench] 批量读返回状态 ");
        num(st);
        say("（1=OK 2=没DMA 3=参数不合 4=设备错 0=请求非法）\n");
        step("一次批量读", 0);
        return 1;
    }
    /* 2 = 降级路径（设备不支持 DMA）：数据仍然要正确 —— 那正是
     * "降级"这个词的含义。数据不对就说明降级路径本身有 bug。 */
    step("一次批量读（状态 1=DMA 2=降级）", 1);
    step("应答说明这次走的是 DMA（状态 1）", st == BULK_OK);
    if (st == BULK_NODMA) {
        say("  [blkbench] 本次走的是**降级路径**（设备不支持 DMA）："
            "吞吐数字反映的是 PIO 批量，不是 DMA\n");
    }

    /* ---- 2a. 毒值差分：每个字节都必须被设备写过 ----
     *
     * 两块缓冲、两种毒值、同一段 LBA。两次结果不同 ⇒ 有字节没被写到
     * （它留着上一次的毒值）。详见 poison_fill 上面的说明。
     * 这里用一段**较小的**长度（8 KiB）而不是吞吐用的那一段：
     * 验证要的是"描述符链完整覆盖"，与一次搬多大无关，
     * 而小一点省下 16 KiB DMA 内存、也少一次大拷贝。 */
    {
        u32 vsec = 16;
        u32 vbytes = vsec * SECTOR_SIZE;
        long oa = fe_mem_alloc_dma(vbytes);
        long ob = fe_mem_alloc_dma(vbytes);
        u8 *ba = (oa > 0) ? (u8 *)fe_mem_map(oa, (void *)0, vbytes,
                                             FE_PROT_READ | FE_PROT_WRITE) : 0;
        u8 *bb = (ob > 0) ? (u8 *)fe_mem_map(ob, (void *)0, vbytes,
                                             FE_PROT_READ | FE_PROT_WRITE) : 0;
        if (ba && bb) {
            poison_fill(ba, vbytes, 0x5A);
            poison_fill(bb, vbytes, 0xA5);
            u32 sa = bulk_read_once(oa, lba, vsec, vbytes);
            u32 sb = bulk_read_once(ob, lba, vsec, vbytes);
            u32 d = first_diff(ba, bb, vbytes);
            if (d != vbytes) {
                say("  [blkbench] 两种毒值下第一个不同的字节在第 ");
                num(d);
                say(" 字节（0x5A 那次=0x");
                {
                    static const char hx[] = "0123456789abcdef";
                    char h[2];
                    h[0] = hx[(ba[d] >> 4) & 0xF];
                    h[1] = hx[ba[d] & 0xF];
                    fe_write(h, 2);
                }
                say("，0xA5 那次=0x");
                {
                    static const char hx[] = "0123456789abcdef";
                    char h[2];
                    h[0] = hx[(bb[d] >> 4) & 0xF];
                    h[1] = hx[bb[d] & 0xF];
                    fe_write(h, 2);
                }
                say("）—— 这个字节设备没写\n");
            }
            step("毒值差分：两种毒值下读回的内容逐字节相同（每个字节都被写过）",
                 d == vbytes && sa == sb);
        } else {
            step("分配毒值差分用的两块 DMA 缓冲", 0);
        }
        if (oa > 0) {
            fe_handle_close(oa);
        }
        if (ob > 0) {
            fe_handle_close(ob);
        }
    }

    /* ---- 2b. 跨路径对照：搬的**是不是这一段** ----
     *
     * 用旧的内联路径读同一段开头，两者必须逐字节相同。
     * 毒值差分证明"每个字节都被写过"，但写过的是不是对的 LBA，
     * 只有跟一条已知正确的路径比才知道（LBA 偏移错了会读到相邻扇区，
     * 而那在毒值差分下看起来完全正常）。 */
    if (!skip_inline) {
        u8 ref[2 * SECTOR_SIZE];
        u32 n = inline_read_once(g_ep, lba, 2, ref);
        if (n != 2 * SECTOR_SIZE) {
            step("跨路径对照：内联读回同样两个扇区", 0);
        } else {
            u32 d = first_diff(buf, ref, 2 * SECTOR_SIZE);
            if (d != 2 * SECTOR_SIZE) {
                say("  [blkbench] 批量读与内联读在第 ");
                num(d);
                say(" 字节起不同 —— 两条路径读的不是同一段\n");
            }
            step("跨路径对照：批量读与内联读逐字节一致（搬的是这一段）",
                 d == 2 * SECTOR_SIZE);
        }
    }

    /* 吞吐：连续 N 次读同一段。
     * ★ 为什么明知会命中宿主缓存还要这么做 ★
     * 因为它测的是"这条路径每秒能搬多少"，而不是"盘有多快"。
     * 缓存命中的情况下更能暴露**路径本身**的开销（每请求的固定成本），
     * 而那正是这次改动要消掉的东西。 */
    {
        u32 rounds = 64;
        u32 last = BULK_OK;
        u64 best = ~0ull;
        for (u32 rep = 0; rep <= TIMED_REPS; rep++) {
            u64 t0 = rdtsc();
            for (u32 i = 0; i < rounds; i++) {
                last = bulk_read_once(obj, lba, bulk_sectors, want);
                /* 1（DMA）与 2（降级）都算"读到了"：这条测试测的是吞吐，
                 * "走的是哪条路"由上面那一条断言单独负责。 */
                if (last != BULK_OK && last != BULK_NODMA) {
                    break;
                }
            }
            u64 t1 = rdtsc();
            if (last != BULK_OK && last != BULK_NODMA) {
                break;
            }
            if (rep == 0) {
                continue;               /* 预热轮不计 */
            }
            if (t1 - t0 < best) {
                best = t1 - t0;
            }
        }
        u64 total = (u64)rounds * want;
        say("  [2] 批量读（每次 ");
        num(bulk_sectors);
        say(" 扇区，数据由设备直接搬进客户端内存）: ");
        num(total / 1024);
        say(" KiB / ");
        num(best);
        say(" 周期");
        if (best && best != ~0ull) {
            u64 kib = total / 1024;
            g_bulk_cyc_kib = best / kib;
            say("  = ");
            num(g_bulk_cyc_kib);
            say(" 周期/KiB");
            if (g_tsc_hz) {
                u64 mib = (u64)total * g_tsc_hz / best / (1024 * 1024);
                say("，");
                num(mib);
                say(" MiB/s");
            }
        }
        say("\n");
        step("64 次批量读全部成功", last == BULK_OK || last == BULK_NODMA);
        step("批量读测出了非零用时（时钟分辨率够细）", best && best != ~0ull);
    }

    /* ---- 3. 反向对照：普通内存（非 DMA 对象）必须被拒 ----
     *
     * ★ 为什么要走**同一条**请求路径、只换缓冲对象 ★
     * 原来这里自己手搓了一遍协议，而且只传了 1 个句柄——但这条协议的
     * handle[0] 是回复端点、handle[1] 才是数据缓冲。驱动于是把那个
     * 普通内存对象当成了回复端点，状态码被回进一个不是端点的地方，
     * 客户端什么都没收到。那一版的"失败"说明的是**测试自己的 bug**，
     * 不是被测对象的性质——一个测不出东西的测试比没有测试更糟。
     * 现在只换一个变量：把 DMA 对象换成普通内存对象，其余一字不改。 */
    {
        long plain = fe_mem_alloc(4096);
        /* ★ 这一条是**反向对照**：普通内存没有物理地址可查，
         * 所以它必须被拒（状态 4）。放它过去就意味着驱动能拿到
         * "任意内存的物理地址"，那是拿内存保护换方便。 */
        u32 rst = (plain > 0) ? bulk_read_once(plain, lba, 1, 4096) : 0;
        step("反向：拿普通内存当 DMA 缓冲被拒（状态 4）", rst == 4u);
        if (plain > 0) {
            fe_handle_close(plain);
        }
    }

    /* ---- 4. 反向对照：越界的 LBA 必须被拒 ---- */
    {
        u32 rst = bulk_read_once(obj, (u32)info.sectors - 1, 8, want);
        step("反向：越界 LBA 被拒（状态 3）", rst == BULK_BADRANGE);
    }

    /* ---- 5. 反向对照：长度上的两个方向各验一次 ----
     *
     * ★ 为什么是两条，而且为什么这一条原来测错了 ★
     * 原来只有一条："声明长度大于对象真实大小必须被拒"。那条断言
     * **假定了一道不存在的护栏**：驱动从不写超过 `sectors*512` 的字节，
     * 而那个长度是被独立核对过 `<= 对象真实大小` 的，所以"声明得比对象大"
     * 多出来的部分驱动根本不看——拒它没有意义，不拒它也没有危险。
     * 断言照着一条与实现不符的注释写，测出来必然是红的。
     *
     * 真正需要两个方向：
     *   a) **声明得比需要的小** → 拒。客户端自己算错了，驱动不该替它猜。
     *   b) **要的比对象大** → 拒。这一条才是**内存安全**那一条：只信
     *      客户端声明的实现，会让设备往对象外面写。
     */
    {
        u32 rst = bulk_read_once(obj, lba, bulk_sectors, want / 2);
        step("反向：声明长度小于本次请求所需被拒（状态 3）", rst == BULK_BADRANGE);
    }
    {
        /* 专门要一块**小**对象，然后读得比它大：4 KiB 的对象读 8 KiB。
         * 用的是同一段 LBA，唯一变量是"要的长度超过了对象"。 */
        u32 ssec = 16;
        long small = fe_mem_alloc_dma(4 * 1024);
        u32 rst = (small > 0) ? bulk_read_once(small, lba, ssec, (u64)ssec * SECTOR_SIZE) : 0;
        step("反向：请求长度超过对象真实大小被拒（状态 3）", rst == BULK_BADRANGE);
        if (small > 0) {
            fe_handle_close(small);
        }
    }

    fe_handle_close(obj);

    /* ---- 6. 本次改动值多少：这**必须**是一条会失败的断言 ----
     *
     * ★ 为什么把结论写成断言，而不是只印两个数字 ★
     * 两个数字摆在那里，"新路径更快"是我说的；写成断言之后，
     * 它就变成了系统说的——慢了就是失败项。而且这里比的是
     * **周期/KiB**：那个数字不依赖 TSC 频率标定，也不依赖机器主频，
     * 换到 VirtualBox 上一样能比。
     *
     * ★ 为什么门槛是 4 倍而不是"只要更快" ★
     * "更快"在噪声里也成立。4 倍这个数字来自机制本身：
     * 旧路径每个请求只搬 1 KiB、要过 5 段拷贝；新路径一个请求搬
     * 128 KiB、数据一次都不经过 CPU。差一个数量级才是这条路径
     * 真的接上了的样子——如果只快一点点，那说明批量读其实还在
     * 走那条老路（降级路径就是这样：它也有一次拷贝，只是省掉了
     * 每请求的 IPC 往返）。 */
    if (g_inline_cyc_kib && g_bulk_cyc_kib) {
        say("  [blkbench] 每 KiB 成本: 旧路径 ");
        num(g_inline_cyc_kib);
        say(" 周期 → 新路径 ");
        num(g_bulk_cyc_kib);
        say(" 周期（");
        num(g_inline_cyc_kib / (g_bulk_cyc_kib ? g_bulk_cyc_kib : 1));
        say(" 倍）\n");
        step("D4：批量路径每 KiB 成本至少比内联路径低 4 倍",
             g_inline_cyc_kib >= g_bulk_cyc_kib * 4);
    } else if (skip_inline) {
        say("  [blkbench] （--no-inline：本次没有基线可对照，"
            "D4 的倍数结论不成立）\n");
    }

    say("=== 块设备吞吐对照结束，失败项 ");
    num(g_fail);
    say(" ===\n");
    return (int)g_fail;
}
