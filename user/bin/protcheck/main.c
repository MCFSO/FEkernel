/* SPDX-License-Identifier: 0BSD */
/* protcheck —— 扇区访问矩阵的**承重测试**。
 *
 * 它验证的是"另一个槽连读都不行"这条保证真的成立，而且验证方式是
 * **从内核取区间表**，不是把布局硬编码进测试：
 * 硬编码的测试只能验证"我以为的布局"，而布局是 mkfat.py 每次建镜像时算出来的。
 *
 * 检查表：
 *   正在运行的槽（DENY_WRITE）  → 读必须成功、写必须被拒
 *   另一个槽     （DENY_ALL）   → 读和写都必须被拒
 *   区间之外     （无约束）      → 读写都应当成功（正向对照）
 *
 * ★ 两个方向都要有证据 ★
 * 只有"被拒绝了"不够——万一整个写路径本来就是坏的，"拒绝"和"坏掉"看不出区别。
 * 所以既要有拒绝，也要有**同样一条路径上的成功**做对照。
 * 而且每次拒绝之后都要核对内核的违规计数**确实增加了**：
 * 磁盘层面（读回来内容没变）与内核层面（记了一笔）两个证据缺一不可。
 *
 * 退出码 = 失败项数。
 */
#include <fe_user.h>

#define BLK_OP_INFO 1
#define BLK_OP_READ 2
#define BLK_OP_WRITE 3

struct blk_req {
    u32 op;
    u32 count;
    u64 lba;
};

static u32 g_fail;
static long g_blk = -1;

static void say(const char *s) { fe_puts(s); }
static void num(u64 v) { fe_print_u64(v); }

static void check(int cond, const char *what)
{
    say(cond ? "    OK   " : "    失败 ");
    say(what);
    say("\n");
    if (!cond) {
        g_fail++;
    }
}

/* 读一个扇区。返回 0 成功，-1 被拒或出错。 */
static int blk_read(u64 lba, u8 *out)
{
    struct blk_req req;
    u32 got = 0;
    req.op = BLK_OP_READ;
    req.count = 1;
    req.lba = lba;
    long r = fe_endpoint_call(g_blk, &req, sizeof(req), out, 512, &got);
    return (r == FE_OK && got == 512) ? 0 : -1;
}

/* 写一个扇区。返回 0 成功，-1 被拒或出错。 */
static int blk_write(u64 lba, const u8 *in)
{
    /* 载荷 = 请求头 + 512 字节数据，所以用一个连续的缓冲 */
    static u8 buf[sizeof(struct blk_req) + 512];
    struct blk_req req;
    req.op = BLK_OP_WRITE;
    req.count = 1;
    req.lba = lba;
    for (u32 i = 0; i < sizeof(req); i++) {
        buf[i] = ((const u8 *)&req)[i];
    }
    for (u32 i = 0; i < 512; i++) {
        buf[sizeof(req) + i] = in[i];
    }
    u32 status = 0;
    u32 got = 0;
    long r = fe_endpoint_call(g_blk, buf, (u32)sizeof(buf), &status, sizeof(status), &got);
    if (r != FE_OK || got != sizeof(status) || status != 1) {
        return -1;
    }
    return 0;
}

int main(void)
{
    say("\n=== 扇区访问矩阵测试（M8：A/B + 写保护）===\n");

    g_blk = fe_devfs_open("/dev/blk0");
    if (g_blk <= 0) {
        say("  [1] 打不开 /dev/blk0\n");
        check(0, "找到块设备服务");
        return (int)g_fail;
    }
    check(1, "通过 devfs 找到块设备服务");

    /* ---- 从内核取区间表 ----
     *
     * ★ 缓冲必须按 FE_PROT_MAX_EXTENTS 开，而且**要核对有没有被截断** ★
     * 这里原来是 ext[64]、cap = 64，而内核最多登记 256 段。
     * 区间数一旦超过 64，拿到的就是**前缀**——而前缀与全表看起来一样。
     * 后果不是"少检查几段"，是后面"找一个不在任何区间里的 LBA"那一步
     * 会挑到一个**其实受保护**的位置，失败信息变成
     * "正向对照需要一个可读的探测扇区"，与写保护毫无关系的样子。
     * 触发它的只是一次无关的改动（往镜像里加了一个程序，FAT 布局变了）。
     * 所以：先问总数，再断言自己拿到了全部。 */
    static struct fe_protect_info ext[FE_PROT_MAX_EXTENTS];
    long total = fe_protect_list(NULL, 0);      /* 查询模式：只问有几段 */
    long n = fe_protect_list(ext, FE_PROT_MAX_EXTENTS);
    say("  [2] 内核里的受保护区间: ");
    num((u64)(n < 0 ? 0 : n));
    say(" 段（内核报总数 ");
    num((u64)(total < 0 ? 0 : total));
    say("）\n");
    check(n > 0, "内核持有非空的访问矩阵");
    check(total == n, "拿到的区间表是完整的（没有被缓冲容量截断）");
    if (n <= 0) {
        say("=== 失败项 ");
        num((u64)g_fail);
        say(" ===\n");
        return (int)g_fail;
    }

    u64 deny_write_lba = 0;     /* 正在运行的槽：可读不可写 */
    u64 deny_all_lba = 0;       /* 另一个槽：连读都不行 */
    for (long i = 0; i < n; i++) {
        if (ext[i].mode == FE_PROT_DENY_WRITE && !deny_write_lba) {
            deny_write_lba = ext[i].lba;
        }
        if ((ext[i].mode & FE_PROT_DENY_ALL) == FE_PROT_DENY_ALL && !deny_all_lba) {
            deny_all_lba = ext[i].lba;
        }
    }
    say("        正在运行的槽: LBA ");
    num(deny_write_lba);
    say("（禁写）\n        另一个槽:     LBA ");
    num(deny_all_lba);
    say("（禁读禁写）\n");
    check(deny_write_lba != 0, "矩阵里存在「正在运行的槽」（禁写）");
    check(deny_all_lba != 0, "矩阵里存在「另一个槽」（禁读禁写）");
    if (!deny_write_lba || !deny_all_lba) {
        say("=== 失败项 ");
        num((u64)g_fail);
        say(" ===\n");
        return (int)g_fail;
    }

    /* ---- 正在运行的槽：读允许 ---- */
    static u8 sec[512];
    check(blk_read(deny_write_lba, sec) == 0, "运行中的槽：**读允许**");
    /* 保存原内容，等会儿要核对没被改 */
    static u8 original[512];
    for (u32 i = 0; i < 512; i++) {
        original[i] = sec[i];
    }

    /* ---- 正在运行的槽：写拒绝，且盘上内容不变 ---- */
    u64 v0 = fe_protect_violations();
    u8 copy[512];
    for (u32 i = 0; i < 512; i++) {
        copy[i] = original[i];
    }
    copy[0] ^= 0xFF;                    /* 改一个字节再写回去 */
    check(blk_write(deny_write_lba, copy) < 0, "运行中的槽：**写被拒绝**");
    u64 v1 = fe_protect_violations();
    check(v1 == v0 + 1, "内核记下了一次违规（计数 +1）");

    /* 磁盘层面的证据：读回来必须逐字节还是原来的内容。
     * 只有"写返回了错误"是不够的——那也可能写进去了却报错。 */
    static u8 after[512];
    if (blk_read(deny_write_lba, after) == 0) {
        u32 diff = 0;
        for (u32 i = 0; i < 512; i++) {
            if (after[i] != original[i]) {
                diff++;
            }
        }
        say("         读回来与原来不一致的字节数: ");
        num(diff);
        say("\n");
        check(diff == 0, "磁盘内容**逐字节未变**（写确实没落到盘上）");
    } else {
        check(0, "拒绝之后再读一次");
    }

    /* ---- 另一个槽：连读都不行 ---- */
    u64 v2 = fe_protect_violations();
    check(blk_read(deny_all_lba, sec) < 0, "另一个槽：**读被拒绝**");
    u64 v3 = fe_protect_violations();
    check(v3 == v2 + 1, "读违规也被记下（计数 +1）");

    u64 v4 = fe_protect_violations();
    check(blk_write(deny_all_lba, original) < 0, "另一个槽：**写被拒绝**");
    check(fe_protect_violations() == v4 + 1, "写违规也被记下");

    /* ---- 正向对照 ---- */
    /* 没有它就无法区分"保护生效"与"块设备写功能整个是坏的"。
     *
     * ★ 这里原来用一个错的启发式，而且它**被一次无关的改动弄坏了** ★
     * 原做法是"取所有区间末尾的最大值"，注释写着"在所有受保护区间之外"。
     * 那只在区间**互不相邻**时成立：两个槽是紧挨着的（A 槽的最后一
     * 个区间到 X 结束，B 槽的第一个区间就从 X 开始），于是那个"最大末尾"
     * 恰好是**另一个区间的开头**，读它当然被拒。
     * 触发它的是往镜像里加了一个程序（ps）——FAT 布局一变，区间就不再
     * 留缝。**"上一个测试能过"因此完全不能说明这个测试是对的。**
     *
     * 改成按定义找：从一个候选位置开始，只要它**落在任何区间里**，
     * 就跳到那个区间的末尾，再看一遍。这样得到的 LBA 按构造就不在任何
     * 区间内，与布局有没有缝无关。 */
    say("  [3] 正向对照：往不受保护的扇区写\n");
    u64 free_lba = 0;
    for (long i = 0; i < n; i++) {
        u64 e = ext[i].lba + ext[i].count;
        if (e > free_lba) {
            free_lba = e;
        }
    }
    /* 跳到"不被任何区间覆盖"为止。每一步都前进（跳到覆盖它的区间末尾），
     * 区间数是有限的，所以必然终止。 */
    for (u32 guard = 0; guard < 1024; guard++) {
        u64 next = free_lba;
        for (long i = 0; i < n; i++) {
            if (free_lba >= ext[i].lba && free_lba < ext[i].lba + ext[i].count) {
                u64 e = ext[i].lba + ext[i].count;
                if (e > next) {
                    next = e;
                }
            }
        }
        if (next == free_lba) {
            break;              /* 没有区间覆盖它了 */
        }
        free_lba = next;
    }
    say("         试探 LBA ");
    num(free_lba);
    say("（逐个核过：不在任何受保护区间内）\n");
    if (blk_read(free_lba, sec) == 0) {
        u8 tmp[512];
        for (u32 i = 0; i < 512; i++) {
            tmp[i] = sec[i];
        }
        tmp[0] ^= 0x5A;
        int wr = blk_write(free_lba, tmp);
        check(wr == 0, "区间之外：**写成功**（证明写路径本身是好的）");
        check(blk_write(free_lba, sec) == 0, "写回原内容");
    } else {
        say("         该 LBA 读不到，跳过正向对照\n");
        check(0, "正向对照需要一个可读的探测扇区");
    }

    fe_handle_close(g_blk);
    say("=== 访问矩阵测试结束，失败项 ");
    num((u64)g_fail);
    say(" ===\n");
    return (int)g_fail;
}
