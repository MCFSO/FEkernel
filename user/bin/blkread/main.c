/* SPDX-License-Identifier: 0BSD */
/* blkread —— 块设备的客户端测试程序。
 *
 * 它是 M7 那一整套机制的**端到端证据**，因为它刻意不用任何"提前塞进来的句柄"：
 *
 *   按路径 /dev/blk0 打开（devfs 服务发现）
 *     → 拿到一条通往 blkd 的端点
 *     → 请求设备信息、读扇区
 *     → 自己解析 MBR、跟着分区表找到分区、再解析它的 FAT32 BPB
 *
 * 中间任何一环断掉，下面这些断言就过不去：devfs 没用就找不到 blkd；
 * 端口权限没生效 blkd 读不出数据；IPC 载荷上限不够 512 字节的扇区装不下。
 *
 * 分层上有一点刻意为之：**分区表与文件系统的解析在客户端，不在驱动里**。
 * blkd 只提供扇区读写，它不该知道什么叫分区、什么叫 FAT32。
 * （写这一版时我第一稿把 BPB 解析放进了驱动，结果读到全 0——
 *   因为 LBA 0 是 MBR，FAT32 引导扇区在分区起始处。那个错误本身就说明了这条边界。）
 *
 * 退出码 = 失败项数。
 */
#include <fe_user.h>

#define BLK_OP_INFO 1
#define BLK_OP_READ 2

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
};

static u32 g_fail;
static long g_dev;

static void check(int cond, const char *what)
{
    fe_puts(cond ? "    OK   " : "    失败 ");
    fe_puts(what);
    fe_puts("\n");
    if (!cond) {
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

/* 读一个扇区。成功返回 0。 */
static int read_sector(u64 lba, u8 *out)
{
    struct blk_req req;
    u32 got = 0;
    req.op = BLK_OP_READ;
    req.count = 1;
    req.lba = lba;
    long r = fe_endpoint_call(g_dev, &req, sizeof(req), out, 512, &got);
    return (r == FE_OK && got == 512) ? 0 : -1;
}

int main(void)
{
    fe_puts("\n=== 块设备客户端（M7：devfs + IPC + 分区解析）===\n");

    /* 1. 服务发现：只有路径，没有任何提前传进来的句柄 */
    long dev = fe_devfs_open("/dev/blk0");
    if (dev < 0) {
        fe_puts("  [1] 打开 /dev/blk0 失败，错误码 -");
        fe_print_u64((u64)(-dev));
        fe_puts("\n");
        check(0, "通过 devfs 找到块设备服务");
        return (int)g_fail;
    }
    g_dev = dev;
    fe_puts("  [1] 按路径打开 /dev/blk0 -> 句柄 ");
    fe_print_u64((u64)dev);
    fe_puts("\n");
    check(1, "通过 devfs 按路径找到块设备服务");
    check(fe_devfs_open("/dev/没有这个东西") < 0, "打开不存在的设备名被拒绝");

    /* 2. 设备信息 */
    struct blk_req req;
    struct blk_info info;
    u32 got = 0;
    req.op = BLK_OP_INFO;
    req.count = 0;
    req.lba = 0;
    long r = fe_endpoint_call(g_dev, &req, sizeof(req), &info, sizeof(info), &got);
    check(r == FE_OK && got == sizeof(info), "IPC 取得设备信息");
    if (r == FE_OK && got == sizeof(info)) {
        fe_puts("         型号 \"");
        fe_puts(info.model);
        fe_puts("\"，");
        fe_print_u64(info.sectors);
        fe_puts(" 扇区 × ");
        fe_print_u64(info.sector_size);
        fe_puts(" 字节\n");
        check(info.sector_size == 512, "扇区大小 512");
        check(info.sectors > 0, "容量非 0");
    }

    /* 3. 读 LBA 0（MBR）。512 字节正好是 IPC 载荷上限的一半。 */
    u8 mbr[512];
    check(read_sector(0, mbr) == 0, "IPC 读回一个完整扇区（512 字节）");
    if (g_fail) {
        fe_puts("=== 失败项 ");
        fe_print_u64(g_fail);
        fe_puts(" ===\n");
        return (int)g_fail;
    }
    check(mbr[510] == 0x55 && mbr[511] == 0xAA, "LBA 0 是合法的 MBR（签名 0x55AA）");

    /* 4. 跟分区表走。这是客户端自己的事——驱动不解释扇区内容。 */
    u64 part_lba = 0;
    u32 part_cnt = 0;
    u8  part_type = 0;
    for (int i = 0; i < 4; i++) {
        const u8 *e = &mbr[446 + i * 16];
        u32 start = (u32)(e[8] | (e[9] << 8) | (e[10] << 16) | (e[11] << 24));
        u32 count = (u32)(e[12] | (e[13] << 8) | (e[14] << 16) | (e[15] << 24));
        if (e[4] == 0 || start == 0) {
            continue;
        }
        part_type = e[4];
        part_lba = start;
        part_cnt = count;
        break;
    }
    check(part_lba != 0, "MBR 里找到第一个分区");
    if (part_lba == 0) {
        fe_puts("=== 失败项 ");
        fe_print_u64(g_fail);
        fe_puts(" ===\n");
        return (int)g_fail;
    }
    fe_puts("         分区类型 0x");
    print_hex8(part_type);
    fe_puts("，起始 LBA ");
    fe_print_u64(part_lba);
    fe_puts("，");
    fe_print_u64(part_cnt);
    fe_puts(" 扇区\n");

    /* 5. 读分区引导扇区，解析 BPB —— 注意 LBA 不是 0 */
    u8 bs[512];
    check(read_sector(part_lba, bs) == 0, "读分区引导扇区（非 0 的 LBA）");

    check(bs[510] == 0x55 && bs[511] == 0xAA, "分区引导扇区签名 0x55AA");

    /* 文件系统类型由**簇数**判定，这是微软规范给的口径
     * （<4085 是 FAT12，<65525 是 FAT16，再往上才是 FAT32）。
     * 为什么不用分区类型字节：那个字节是「声称」，簇数才是「实际」，
     * 两者不一致的镜像在真实世界里很常见。 */
    u16 bps = (u16)(bs[11] | (bs[12] << 8));
    u8  spc = bs[13];
    u16 reserved = (u16)(bs[14] | (bs[15] << 8));
    u8  fat_count = bs[16];
    u16 root_entries = (u16)(bs[17] | (bs[18] << 8));
    u32 total16 = (u16)(bs[19] | (bs[20] << 8));
    u32 total32 = (u32)(bs[32] | (bs[33] << 8) | (bs[34] << 16) | (bs[35] << 24));
    u32 total = total16 ? total16 : total32;
    u32 fat16_sz = (u16)(bs[22] | (bs[23] << 8));
    u32 fat32_sz = (u32)(bs[36] | (bs[37] << 8) | (bs[38] << 16) | (bs[39] << 24));

    check(bps == 512 && spc != 0 && reserved != 0 && fat_count >= 1 && total != 0,
          "BPB 基本字段自洽");

    u32 root_sectors = (root_entries * 32 + bps - 1) / bps;
    u32 fatsz = fat16_sz ? fat16_sz : fat32_sz;
    u32 data_sectors = total - (reserved + fat_count * fatsz + root_sectors);
    u32 clusters = spc ? data_sectors / spc : 0;
    const char *fstype = (clusters < 4085) ? "FAT12" : (clusters < 65525) ? "FAT16" : "FAT32";

    fe_puts("         FS 类型 ");
    fe_puts(fstype);
    fe_puts("（");
    fe_print_u64(clusters);
    fe_puts(" 簇），OEM \"");
    for (int i = 0; i < 8; i++) {
        fe_write((const char *)&bs[3 + i], 1);
    }
    fe_puts("\"，每簇 ");
    fe_print_u64(spc);
    fe_puts(" 扇区，");
    fe_print_u64(fat_count);
    fe_puts(" 份 FAT × ");
    fe_print_u64(fatsz);
    fe_puts(" 扇区，共 ");
    fe_print_u64(total);
    fe_puts(" 扇区\n");

    check(fatsz != 0 && clusters >= 4085, "文件系统是 FAT16 或 FAT32（容量大于 2 MiB）");

    /* 卷标不强制非空：合法卷可以没有卷标。有就打出来，没有就说没有——
     * 把「可选字段缺失」当成失败，会训练人去忽略红色结果。 */
    char label[12];
    for (int i = 0; i < 11; i++) {
        label[i] = (char)bs[71 + i];
    }
    label[11] = '\0';
    int has_label = 0;
    for (int i = 0; i < 11; i++) {
        if (label[i] != ' ' && label[i] != '\0') {
            has_label = 1;
        }
    }
    fe_puts("         卷标 ");
    if (has_label) {
        fe_puts("\"");
        fe_puts(label);
        fe_puts("\"");
    } else {
        fe_puts("（未设置）");
    }
    fe_puts("\n");

    fe_handle_close(g_dev);
    fe_puts("=== 块设备客户端结束，失败项 ");
    fe_print_u64(g_fail);
    fe_puts(" ===\n");
    return (int)g_fail;
}
