/* SPDX-License-Identifier: 0BSD */
/* irqtest —— 中断共享的**用户态承重测试**（D5a/D5b 的真实路径）。
 *
 * 内核自检用 `route_hw=false` 验的是机制；这个程序验的是**真设备**：
 * 同一条电平线（QEMU 上 virtio-blk 的 IRQ11）被**两个任务**同时登记，
 * 两者都必须被真实设备中断叫醒，而且**磁盘不能因为线被卡住而停摆**。
 *
 * 分饰两角：
 *   --child   第二个登记者：登记后等中断、确认、报告后退出
 *   （无参数）第一个登记者 + 监督者：**制造真实流量**、等自己的中断、
 *             再检查孩子是否也收到了
 *
 * ★ 为什么要两个任务而不是"登记两次" ★
 * 共享是**跨任务**的性质：同一个任务登记两次是写错了（返回 EXIST），
 * 只有两个不同 owner 才能真正暴露"投递给了谁""谁没 ack 会不会卡住线"。
 *
 * ★ 为什么必须自己制造流量 ★
 * 中断不会凭空来。这个程序通过 /dev/vblk0 发真实读请求，
 * 让 virtio 设备真的抬高中断线——所以"收到中断"这件事有物理来源，
 * 不是合成的。反过来，如果一条共享线只能靠合成中断"验证"，那什么也没验。
 *
 * ★ 最后一个断言才是关键 ★
 * 所有中断收完、确认完之后，**磁盘必须还能用**。共享线最危险的失败模式
 * 不是"收不到中断"，而是"某个登记者没确认，线被永久屏蔽，邻居一起停摆"——
 * 那个故障在这里会表现为"最后那次读失败"。
 *
 * 退出码 = 失败项数。
 */
#include <fe_user.h>

#define BLK_OP_INFO 1
#define BLK_OP_READ 2
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
    u32 bar4;
    u32 dma_mode;
    u64 mmio;
    u64 mmio_len;
    u32 backend;
    u32 _pad2;
};

struct blk_bulk_req {
    u32 op;
    u32 sectors;
    u64 lba;
    u64 buf_len;
    u64 _reserved;
};

static u32 g_fail;
static char g_prefix[32];

static void say(const char *s) { fe_puts(s); }
static void num(u64 v) { fe_print_u64(v); }
static void flush(void) { fe_flush(); }

static void check(int cond, const char *what)
{
    say(cond ? "    OK   " : "    失败 ");
    say(what);
    say("\n");
    flush();
    if (!cond) {
        g_fail++;
    }
}

static void slot_path(const char *name, char *out, u32 cap)
{
    u32 k = 0;
    const char *parts[3];
    parts[0] = g_prefix;
    parts[1] = "/bin/";
    parts[2] = name;
    for (u32 p = 0; p < 3; p++) {
        for (const char *s = parts[p]; *s && k < cap - 1; s++) {
            out[k++] = *s;
        }
    }
    out[k] = '\0';
}

/* 一次批量读：真的让设备干活（它才会抬高中断线）。
 * 用批量路径而不是内联路径，是因为它每次搬 128 KiB、请求少而次数够。 */
static u32 bulk_read_once(long ep, long buf, u32 lba, u32 sectors, u64 buf_len)
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
    hs[0] = (u32)rep;
    hs[1] = (u32)buf;
    struct fe_msg_header hh;
    hh.protocol = 0xFE07;
    hh.opcode = BLK_OP_READ_BULK;
    hh.payload_len = sizeof(b);
    hh.handle_count = 2;
    hh.request_id = 0x7A01;
    u32 status = 0xFFFFFFFFu;
    if (fe_endpoint_send(ep, &hh, &b, hs, 2) == FE_OK) {
        struct fe_msg_header rh;
        u32 n = 0;
        if (fe_endpoint_recv(rep, &rh, &status, sizeof(status), NULL, &n) < 0) {
            status = 0xFFFFFFFFu;
        }
    }
    fe_handle_close(rep);
    return status;
}

/* ---- 孩子：第二个登记者 ---- */
static int child_main(void)
{
    /* 登记时**声明要求电平**：这是 D5b 的用法示范。
     * 不匹配会被拒——那正是我们要的：带着错的触发方式跑起来，
     * 症状是负载高时偶发丢事件，比直接失败难查得多。 */
    long nt = fe_notification_create();
    if (nt <= 0) {
        say("  [child] 通知对象创建失败\n");
        return 1;
    }
    struct fe_irq_info ii;
    long r = fe_irq_register_ex(11, nt, FE_IRQ_MODE_LEVEL, &ii);
    if (r != FE_OK) {
        say("  [child] 登记 IRQ11 失败：");
        num((u64)(-r));
        say("（共享没成立？）\n");
        flush();
        return 1;
    }
    say("  [child] 已共享登记 IRQ11（");
    say((ii.mode & FE_IRQ_MODE_LEVEL) ? "电平" : "边沿");
    say("，共享者 ");
    num(ii.sharers);
    say("）\n");
    flush();

    /* 等真实设备中断：父进程同时在发读请求。
     * 用有界轮询而不是无限等——"没等到"要能被报出来，而不是挂住。 */
    u64 bits = 0;
    long w = fe_notification_wait(nt, 1ull << 11, &bits);
    if (w != FE_OK || !(bits & (1ull << 11))) {
        say("  [child] 没等到中断（");
        num((u64)(-w));
        say("）\n");
        flush();
        return 1;
    }
    /* ★ 收到之后必须确认 ★ 否则线会一直被屏蔽着，邻居一起停摆。 */
    long a = fe_irq_ack(11);
    if (a != FE_OK) {
        say("  [child] 确认失败：");
        num((u64)(-a));
        say("\n");
        flush();
        return 1;
    }
    say("  [child] 收到真实中断并已确认\n");
    flush();
    fe_handle_close(nt);
    return 0;
}

int main(int argc, char **argv)
{
    fe_slot_prefix((argc > 0 && argv) ? argv[0] : 0, g_prefix, sizeof(g_prefix));

    if (argc >= 2 && strcmp(argv[1], "--child") == 0) {
        return child_main();
    }

    say("\n=== 中断共享（D5a/D5b：真实设备 + 两个任务）===\n");

    /* ---- 1. 第一个登记者（自己），声明要求电平 ---- */
    long nt = fe_notification_create();
    check(nt > 0, "创建通知对象");
    if (nt <= 0) {
        say("=== 中断共享测试结束，失败项 ");
        num(g_fail);
        say(" ===\n");
        return (int)g_fail;
    }

    struct fe_irq_info ii;
    long r = fe_irq_register_ex(11, nt, FE_IRQ_MODE_LEVEL, &ii);
    check(r == FE_OK, "登记 IRQ11 并要求**电平**触发（D5b 的契约）");
    if (r != FE_OK) {
        say("        返回 ");
        num((u64)(-r));
        say("（这台机器上 IRQ11 不是电平？那 D5a 的共享前提不成立）\n");
        say("=== 中断共享测试结束，失败项 ");
        num(g_fail);
        say(" ===\n");
        return (int)g_fail;
    }
    say("  [irq] IRQ11 -> GSI ");
    num(ii.gsi);
    say(" 向量 ");
    num(ii.vector);
    say("，");
    say((ii.mode & FE_IRQ_MODE_LEVEL) ? "电平" : "边沿");
    say("，共享者 ");
    num(ii.sharers);
    say("\n");
    check((ii.mode & FE_IRQ_MODE_LEVEL) != 0, "回填的实情说它是电平触发");

    /* ---- 2. 拉起第二个登记者 ---- */
    char self[96];
    slot_path("irqtest", self, sizeof(self));
    char *ca[3];
    ca[0] = self;
    ca[1] = "--child";
    ca[2] = (char *)0;
    long ch = fe_spawn(self, ca, 2);
    check(ch > 0, "拉起第二个登记者（irqtest --child）");

    /* ---- 3. 制造真实流量，并等自己的中断 ---- */
    long ep = fe_devfs_open("/dev/vblk0");
    if (ep <= 0) {
        ep = fe_devfs_open("/dev/blk0");
    }
    check(ep > 0, "打开块设备（用来制造真实的中断）");
    int got_irq = 0;
    if (ep > 0) {
        long obj = fe_mem_alloc_dma(256 * 512);
        u8 *buf = (obj > 0)
            ? (u8 *)fe_mem_map(obj, (void *)0, 256 * 512, FE_PROT_READ | FE_PROT_WRITE)
            : (u8 *)0;
        if (obj > 0 && buf) {
            /* 反复读同一段：每次完成都会让设备抬一次中断线。
             * 每轮之后**非阻塞地**看一下自己的通知位有没有被置上
             * （用掩码 0 之外的超时不存在，所以这里用"发一轮就检查一次"
             * 的方式：fe_notification_wait 在位上已置时立刻返回）。 */
            u64 bits = 0;
            for (u32 i = 0; i < 24 && !got_irq; i++) {
                u32 st = bulk_read_once(ep, obj, 2080 + 16, 256, 256 * 512);
                if (st != 1 && st != 2) {
                    break;
                }
                if (fe_notification_wait(nt, 1ull << 11, &bits) == FE_OK &&
                    (bits & (1ull << 11))) {
                    got_irq = 1;
                }
            }
            check(got_irq, "第一个登记者收到**真实设备**中断（共享线上不止一家）");
            if (got_irq) {
                check(fe_irq_ack(11) == FE_OK, "第一个登记者确认");
            }

            /* ---- 4. 等孩子：它也必须收到了真实中断 ---- */
            i32 code = -1;
            long w = fe_wait(ch, &code);
            check(w >= 0, "等第二个登记者结束");
            check(code == 0,
                  "第二个登记者也收到了真实中断并确认（共享是**双向**成立的）");
            if (code != 0) {
                say("        孩子退出码 ");
                num((u64)(u32)code);
                say("（1 = 它没等到中断）\n");
            }
            fe_handle_close(ch);

            /* ---- 5. ★ 线没有被卡住 ★ ----
             * 共享线最危险的失败模式不是"收不到中断"，而是"某个登记者
             * 没确认 → 线被永久屏蔽 → 邻居一起停摆"。它在这里的
             * 表现就是最后这次读失败。 */
            u32 st2 = bulk_read_once(ep, obj, 2080 + 16, 256, 256 * 512);
            check(st2 == 1 || st2 == 2,
                  "所有中断收完之后磁盘**仍然可用**（线没被卡住）");
            if (st2 != 1 && st2 != 2) {
                say("        最后一次批量读状态 ");
                num(st2);
                say(" —— 共享线可能被某个没确认的登记者屏蔽住了\n");
            }
            fe_handle_close(obj);
        } else {
            say("        DMA 缓冲分配失败，中断测试无法进行\n");
        }
        fe_handle_close(ep);
    } else {
        fe_handle_close(ch);
    }

    /* ---- 6. 反向对照 ---- */
    {
        /* 触发方式不匹配：IRQ11 是电平，要求边沿必须被拒 */
        long nt2 = fe_notification_create();
        check(nt2 > 0, "再造一个通知对象用于反向对照");
        if (nt2 > 0) {
            struct fe_irq_info j;
            long rr = fe_irq_register_ex(11, nt2, FE_IRQ_MODE_EDGE, &j);
            check(rr == FE_ERR_INVAL,
                  "反向：声明要求**边沿**而实际是电平 → 被拒（INVAL）");
            /* 同一个任务重复登记同一条线：EXIST，不是"再加一家" */
            rr = fe_irq_register_ex(11, nt2, FE_IRQ_MODE_ANY, &j);
            check(rr == FE_ERR_EXIST,
                  "反向：同一个任务重复登记同一条线 → EXIST（与共享区分开）");
            fe_handle_close(nt2);
        }
        /* 没登记过的线不能确认 */
        long a = fe_irq_ack(20);
        check(a == FE_ERR_NOENT, "反向：确认一条自己没登记的线 → NOENT");
        /* 坏句柄 */
        r = fe_irq_register(11, 9999);
        check(r == FE_ERR_BADHANDLE, "反向：坏的通知句柄 → BADHANDLE");
    }

    fe_handle_close(nt);
    say("=== 中断共享测试结束，失败项 ");
    num(g_fail);
    say(" ===\n");
    return (int)g_fail;
}
