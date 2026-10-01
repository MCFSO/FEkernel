/* SPDX-License-Identifier: 0BSD */
/* fsd —— FAT32 文件系统服务（用户态，**只读**）。
 *
 * 它在分层里的位置：
 *
 *     客户端程序 ──IPC──▶ fsd ──IPC──▶ blkd ──端口 I/O──▶ ATA
 *                  (路径/文件)     (扇区/LBA)
 *
 * fsd 完全不碰端口，也不知道 ATA 是什么；blkd 完全不知道什么叫文件。
 * 两边各自只做自己那一层的事，中间是一条 IPC 协议。
 *
 * ★ 本轮只做**读**。★ 写路径刻意不做，因为它和保护层是同一件事：
 * 一旦能写，就必须同时决定"谁能写哪些扇区"——那正是
 * docs/03-protection.md 里的块层写白名单。把写和强制点分开做，
 * 结果一定是先有一个没有任何约束的写路径，然后再去补，那时候已经晚了。
 *
 * FAT32 按微软公开规范（FAT32 File System Specification）实现，
 * 支持 VFAT 长文件名（目录项里 0x0F 属性的项是长名的分片）。
 */
#include <fe_user.h>

#define BLK_OP_INFO 1
#define BLK_OP_READ 2
#define BLK_OP_WRITE 3
#define BLK_OP_FLUSH 4

#define SECTOR 512

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

/* ---------------- 文件系统服务协议 ----------------
 *
 * ★ 协议定义**只有一处**：user/include/fe_fs.h ★
 *
 * 这里原先各抄了一份（FS_OP_*、struct fs_req、struct fs_dirent…），
 * 与头文件里的 FE_FS_* 逐字段重复。抄一份的代价不是那几十行，
 * 而是**两份"我以为协议是这样的"会各自漂移**——加一个字段只改一边，
 * 症状是"客户端与服务端对同一条消息的理解差几个字节"，
 * 而那表现成随机的数据错位，查起来要同时盯着两个进程。
 *
 * 所以现在服务端直接包含协议头。`FS_*` 这些短名字保留为别名，
 * 是为了让下面几百行处理代码不用改；别名指向的是唯一那份定义。
 */
#include <fe_fs.h>

#define FS_OP_INFO   FE_FS_OP_INFO
#define FS_OP_LIST   FE_FS_OP_LIST
#define FS_OP_READ   FE_FS_OP_READ
#define FS_OP_CREATE FE_FS_OP_CREATE
#define FS_OP_WRITE  FE_FS_OP_WRITE
#define FS_OP_UNLINK FE_FS_OP_UNLINK
#define FS_OP_STAT   FE_FS_OP_STAT

#define FS_PATH_MAX  FE_FS_PATH_MAX
#define NAME_MAX     FE_FS_NAME_MAX
#define LIST_MAX     FE_FS_LIST_MAX
#define READ_MAX     FE_FS_CHUNK

typedef struct fe_fs_req        fs_req_t_placeholder_unused;    /* 占位，防误用 */
#define fs_req          fe_fs_req
#define fs_info         fe_fs_info
#define fs_dirent       fe_fs_dirent
#define fs_list_reply   fe_fs_list_reply
#define fs_read_reply   fe_fs_read_reply

/* ---------------- 全局状态 ---------------- */

static u32 g_fail;
static long g_blk = -1;

struct fat32 {
    u32 part_lba;
    u16 bytes_per_sec;
    u8  sec_per_clus;
    u16 reserved_secs;
    u8  num_fats;
    u32 fat_size_secs;
    u32 root_clus;
    u32 total_secs;
    u32 fat_start_lba;
    u32 data_start_lba;
    u32 cluster_count;
    char label[12];
    char oem[9];
};

static struct fat32 g_fs;

static void say(const char *s) { fe_puts(s); }
static void num(u64 v) { fe_print_u64(v); }

static void step(const char *what, int ok)
{
    say("  [fsd] ");
    say(ok ? "OK   " : "失败 ");
    say(what);
    say("\n");
    if (!ok) {
        g_fail++;
    }
}

static void fs_reply(long reply_ep, const void *data, u32 len);

/* ------------------------------------------------------------------ */
/* 块设备访问（全部经由 IPC 到 blkd）                                   */
/* ------------------------------------------------------------------ */

/* 读 count 个扇区。blkd 单次最多回 2 个，所以这里循环。
 * 缓冲必须至少有 count*512 字节。 */
static int blk_read(u32 lba, u32 count, u8 *out)
{
    u32 done = 0;
    while (done < count) {
        u32 chunk = count - done;
        if (chunk > 2) {
            chunk = 2;
        }
        struct blk_req req;
        req.op = BLK_OP_READ;
        req.count = chunk;
        req.lba = lba + done;
        u32 got = 0;
        long r = fe_endpoint_call(g_blk, &req, sizeof(req),
                                  out + done * SECTOR, chunk * SECTOR, &got);
        if (r != FE_OK || got != chunk * SECTOR) {
            return -1;
        }
        done += chunk;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* FAT32 基本换算                                                      */
/* ------------------------------------------------------------------ */

static u32 clus_size_bytes(void)
{
    return (u32)g_fs.sec_per_clus * g_fs.bytes_per_sec;
}

static u32 clus_to_lba(u32 clus)
{
    return g_fs.data_start_lba + (clus - 2) * g_fs.sec_per_clus;
}

static int clus_valid(u32 c)
{
    return c >= 2 && c < g_fs.cluster_count + 2;
}

/* 读 FAT 表里某个簇的下一簇号。
 * FAT 项是 4 字节，可能跨扇区边界——统一按"读两个扇区"处理，
 * 与其为边界做特例，不如多读一个扇区（一次 blk_read 本来就支持）。 */
static u32 fat_next(u32 clus)
{
    u32 fat_offset = clus * 4;
    u32 sec = g_fs.fat_start_lba + fat_offset / g_fs.bytes_per_sec;
    u32 ent = fat_offset % g_fs.bytes_per_sec;

    u8 buf[SECTOR * 2];
    if (blk_read(sec, 2, buf) != 0) {
        return 0x0FFFFFFFu;     /* 读失败当链尾：调用方会看到长度不对 */
    }
    u32 v = (u32)buf[ent] | ((u32)buf[ent + 1] << 8) |
            ((u32)buf[ent + 2] << 16) | ((u32)buf[ent + 3] << 24);
    return v & 0x0FFFFFFFu;     /* 高 4 位保留，规范要求忽略 */
}

/* ------------------------------------------------------------------ */
/* 目录遍历                                                            */
/* ------------------------------------------------------------------ */

struct dirent_raw {
    char short_name[13];
    char long_name[NAME_MAX];
    int  has_long;
    u32  size;
    u8   attr;
    u32  first_clus;
};

/* 8.3 短名格式化：去尾部空格、中间补 '.'、0x05 还原成 0xE5（规范里的日文假名转义） */
static void format_short(const u8 *e, char *out)
{
    char base[9];
    char ext[4];
    for (int i = 0; i < 8; i++) {
        base[i] = (char)e[i];
    }
    base[8] = '\0';
    for (int i = 0; i < 3; i++) {
        ext[i] = (char)e[8 + i];
    }
    ext[3] = '\0';
    if ((u8)base[0] == 0x05) {
        base[0] = (char)0xE5;
    }
    for (int i = 7; i >= 0 && base[i] == ' '; i--) {
        base[i] = '\0';
    }
    for (int i = 2; i >= 0 && ext[i] == ' '; i--) {
        ext[i] = '\0';
    }
    u32 k = 0;
    for (const char *p = base; *p && k < 12; p++) {
        out[k++] = *p;
    }
    if (ext[0]) {
        if (k < 12) {
            out[k++] = '.';
        }
        for (const char *p = ext; *p && k < 12; p++) {
            out[k++] = *p;
        }
    }
    out[k] = '\0';
}

/* 取目录里"第 want_idx 个有效项"。
 *
 * 每次从目录开头重新走链。目录通常只有几项到几十项，重走的代价远小于
 * 维护一个跨调用的游标状态（那个状态还要处理簇边界、续传、被删项计数）。
 * 真遇到大目录再改成游标。
 *
 * 返回 1 = 取到，0 = 到末尾，-1 = I/O 错误。 */
static int dir_get(u32 dir_clus, u32 want_idx, struct dirent_raw *out)
{
    u32 idx = 0;
    u32 clus = dir_clus;
    u16 lfn[260];
    u32 lfn_len = 0;
    static const u8 lfn_pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

    while (clus_valid(clus)) {
        /* 一个簇有 sec_per_clus 个扇区，目录项跨扇区、也跨簇连续排布 */
        for (u32 s = 0; s < g_fs.sec_per_clus; s++) {
            u8 secbuf[SECTOR];
            if (blk_read(clus_to_lba(clus) + s, 1, secbuf) != 0) {
                return -1;
            }
            for (u32 off = 0; off < SECTOR; off += 32) {
                const u8 *e = &secbuf[off];
                if (e[0] == 0x00) {
                    return 0;           /* 目录到此结束 */
                }
                if (e[0] == 0xE5) {
                    lfn_len = 0;        /* 已删除：连它的长名一起丢弃 */
                    continue;
                }
                u8 attr = e[11];
                if (attr == 0x0F) {
                    /* 长名分片：序号在低 6 位，13 个 UTF-16 字符散布在固定偏移。
                     * 分片按**倒序**出现（…3、2、1），所以按下标回填。 */
                    u32 ord = (u32)(e[0] & 0x3F);
                    if (ord >= 1 && ord <= 20) {
                        u32 base = (ord - 1) * 13;
                        for (u32 i = 0; i < 13; i++) {
                            u16 ch = (u16)(e[lfn_pos[i]] | (e[lfn_pos[i] + 1] << 8));
                            if (base + i < 260) {
                                lfn[base + i] = ch;
                            }
                        }
                        if (base + 13 > lfn_len) {
                            lfn_len = base + 13;
                        }
                    }
                    continue;
                }
                if (attr & 0x08) {
                    continue;           /* 卷标项不是文件 */
                }

                if (idx == want_idx) {
                    format_short(e, out->short_name);
                    out->has_long = 0;
                    out->long_name[0] = '\0';
                    if (lfn_len > 0) {
                        /* UTF-16 → ASCII。非 ASCII 一律替换成 '?'：
                         * 宁可显示得难看，也不要假装支持一个没实现的编码转换。 */
                        u32 k = 0;
                        for (u32 i = 0; i < lfn_len && k < NAME_MAX - 1; i++) {
                            u16 ch = lfn[i];
                            if (ch == 0x0000 || ch == 0xFFFF) {
                                break;
                            }
                            out->long_name[k++] = (ch < 0x80) ? (char)ch : '?';
                        }
                        out->long_name[k] = '\0';
                        out->has_long = (k > 0);
                    }
                    out->attr = attr;
                    out->size = (u32)e[28] | ((u32)e[29] << 8) |
                                ((u32)e[30] << 16) | ((u32)e[31] << 24);
                    out->first_clus = (((u32)e[20] | ((u32)e[21] << 8)) << 16) |
                                      ((u32)e[26] | ((u32)e[27] << 8));
                    return 1;
                }
                idx++;
                lfn_len = 0;
            }
        }
        clus = fat_next(clus);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 路径解析                                                            */
/* ------------------------------------------------------------------ */

static int name_eq_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') {
            x = (char)(x - 32);
        }
        if (y >= 'a' && y <= 'z') {
            y = (char)(y - 32);
        }
        if (x != y) {
            return 0;
        }
    }
    return *a == '\0' && *b == '\0';
}

/* 解析绝对路径。成功返回 1。根目录也算成功（attr 是目录、first_clus 是根簇）。 */
static int path_resolve(const char *path, struct dirent_raw *out)
{
    if (!path || path[0] != '/') {
        return 0;
    }
    const char *p = path;
    while (*p == '/') {
        p++;
    }
    if (*p == '\0') {
        out->short_name[0] = '/';
        out->short_name[1] = '\0';
        out->has_long = 0;
        out->long_name[0] = '\0';
        out->attr = 0x10;
        out->size = 0;
        out->first_clus = g_fs.root_clus;
        return 1;
    }

    u32 cur_clus = g_fs.root_clus;
    for (;;) {
        char comp[NAME_MAX];
        u32 k = 0;
        while (*p && *p != '/') {
            if (k < NAME_MAX - 1) {
                comp[k++] = *p;
            }
            p++;
        }
        comp[k] = '\0';
        while (*p == '/') {
            p++;
        }
        int is_last = (*p == '\0');

        struct dirent_raw ent;
        int found = 0;
        for (u32 i = 0;; i++) {
            int r = dir_get(cur_clus, i, &ent);
            if (r <= 0) {
                break;
            }
            const char *nm = ent.has_long ? ent.long_name : ent.short_name;
            if (name_eq_ci(nm, comp) || name_eq_ci(ent.short_name, comp)) {
                found = 1;
                break;
            }
        }
        if (!found) {
            return 0;
        }
        if (is_last) {
            *out = ent;
            return 1;
        }
        if (!(ent.attr & 0x10)) {
            return 0;               /* 路径中间遇到了文件 */
        }
        cur_clus = ent.first_clus;
    }
}

/* ------------------------------------------------------------------ */
/* 挂载                                                                */
/* ------------------------------------------------------------------ */
/* 写路径（M9：让文件系统真的能存东西）                                  */
/* ------------------------------------------------------------------ */
/* ★ 这一层为什么必须存在 ★
 * 在它之前，这个"操作系统"什么都存不住：配置、日志、用户数据、
 * 装进来的程序——全都只能活在内存里。有了它，`echo hi > /home/a.txt`
 * 才第一次成为一件有意义的事。
 *
 * 写入的落点也说明了 A/B 的设计：`/home` **不在任何槽的区间里**，
 * 所以它不受写保护、也永远不参与 A/B 拷贝；而往 `/slot_a/...` 写
 * 会被保护表拒绝（调用者是 fsd 自己，不是被豁免的更新器）。
 * 一边能写、一边写不了，这个对比正是 C5/C8 的验收内容。 */

/* 写 count 个扇区。blkd 单次最多写 1 个扇区（请求要带 16 字节头，
 * 内联载荷上限 1024），所以这里按 1 累加。 */
/* 本次请求里是否发生过“写被访问矩阵拒绝”。
 *
 * ★ 为什么用标志而不是层层返回错误码 ★
 * 一次 create 会经过 fat_set / dir_add / dir_update_entry 四三层，
 * 每层都转一遍 -2 只是把同一个信息抄四遍。
 * 而调用者真正要的答案是“这次为什么失败”——一个标志就够了。
 * 每个写处理函数在入口清零、出口检查。 */
static int g_write_denied;

static int blk_write(u32 lba, u32 count, const u8 *in)
{    for (u32 i = 0; i < count; i++) {
        struct blk_req req;
        u32 status = 0;
        u32 got = 0;
        /* 载荷 = 请求头 + 一个扇区：与 fe_blk.h 的 fe_blk_write 同一布局 */
        static u8 buf[sizeof(struct blk_req) + SECTOR];
        req.op = BLK_OP_WRITE;
        req.count = 1;
        req.lba = lba + i;
        memcpy(buf, &req, sizeof(req));
        memcpy(buf + sizeof(req), in + (u64)i * SECTOR, SECTOR);
        long r = fe_endpoint_call(g_blk, buf, (u32)sizeof(buf),
                                  &status, sizeof(status), &got);
        if (r != FE_OK) {
            return -1;
        }
        /* ★ 空应答 = 被访问矩阵拒绝 ★
         * blkd 在保护检查不过时回**空载荷**（它没法回状态——那次请求
         * 根本没到设备），而设备真出错时回 status=0。
         * 这两种"写失败"对用户的含义完全不同：
         *   一个说"这个区域受保护，你改不了"（设计如此），
         *   另一个说"磁盘或驱动出问题了"（要去查）。
         * 第一版把两者都报成 "I/O error"——用户看到"运行中的槽写不了"
         * 会以为盘坏了，而实际上那正是访问矩阵在起作用。 */
        if (got == 0) {
            g_write_denied = 1;
            return -2;              /* 被写保护拒绝 */
        }
        if (got != sizeof(status) || status != 1) {
            return -1;
        }
    }
    return 0;
}

/* 数据真正落盘。★ 调用点是"用户看得见的操作结束时"，不是每个扇区后面 ★
 * 每个扇区都刷一次的话，一次 1 KiB 的写要等两次真实落盘屏障——
 * 这正是 A/B 更新器踩过的坑（14 秒的拷贝）。 */
static int blk_flush(void)
{
    struct blk_req req;
    u32 status = 0;
    u32 got = 0;
    req.op = BLK_OP_FLUSH;
    req.count = 0;
    req.lba = 0;
    long r = fe_endpoint_call(g_blk, &req, sizeof(req), &status, sizeof(status), &got);
    if (r != FE_OK || got != sizeof(status)) {
        return -1;
    }
    return status == 1 ? 0 : -1;
}

/* 写一个 FAT 项。★ 两份 FAT 都要写 ★
 * 只写第一份的话，掉电后（或另一个实现读第二份时）文件系统就自相矛盾了。
 * 规范允许两份不一致时以第一份为准，但"允许"不等于"应该"——
 * 那会让 fsck 类的工具在将来有活干。 */
static int fat_set(u32 clus, u32 value)
{
    u32 fat_offset = clus * 4;
    u32 sec_off = fat_offset / g_fs.bytes_per_sec;
    u32 ent = fat_offset % g_fs.bytes_per_sec;
    static u8 buf[SECTOR];

    for (u32 f = 0; f < g_fs.num_fats; f++) {
        u32 lba = g_fs.fat_start_lba + f * g_fs.fat_size_secs + sec_off;
        if (blk_read(lba, 1, buf) != 0) {
            return -1;
        }
        u32 old = (u32)buf[ent] | ((u32)buf[ent + 1] << 8) |
                  ((u32)buf[ent + 2] << 16) | ((u32)buf[ent + 3] << 24);
        /* 高 4 位是保留位，规范要求写的时候保持原值 */
        u32 nv = (old & 0xF0000000u) | (value & 0x0FFFFFFFu);
        buf[ent] = (u8)(nv & 0xFF);
        buf[ent + 1] = (u8)((nv >> 8) & 0xFF);
        buf[ent + 2] = (u8)((nv >> 16) & 0xFF);
        buf[ent + 3] = (u8)((nv >> 24) & 0xFF);
        if (blk_write(lba, 1, buf) != 0) {
            return -1;
        }
    }
    return 0;
}

/* 分配一个簇并标成链尾。返回 0 表示失败（0 不是合法簇号）。
 *
 * 从上次的位置往后扫（`g_alloc_hint`）：每次都从头扫的话，
 * 磁盘越满越慢，而且最先被反复使用的是同一批低簇号——
 * 那种"总是从 0 开始"的策略在 FAT 时代就是有名的慢。 */
static u32 g_alloc_hint = 2;

static u32 clus_alloc(void)
{
    static u8 buf[SECTOR];
    u32 per_sec = g_fs.bytes_per_sec / 4;
    u32 start = g_alloc_hint >= 2 ? g_alloc_hint : 2;

    for (u32 pass = 0; pass < 2; pass++) {
        u32 from = pass == 0 ? start : 2;
        u32 to = pass == 0 ? g_fs.cluster_count + 2 : start;
        if (pass == 1 && start <= 2) {
            break;
        }
        for (u32 c = from; c < to; c += per_sec) {
            u32 sec_off = (c * 4) / g_fs.bytes_per_sec;
            u32 lba = g_fs.fat_start_lba + sec_off;
            if (blk_read(lba, 1, buf) != 0) {
                return 0;
            }
            for (u32 i = 0; i < per_sec; i++) {
                u32 clus = c + i;
                if (clus < 2 || clus >= g_fs.cluster_count + 2) {
                    continue;
                }
                u32 v = (u32)buf[i * 4] | ((u32)buf[i * 4 + 1] << 8) |
                        ((u32)buf[i * 4 + 2] << 16) | ((u32)buf[i * 4 + 3] << 24);
                if ((v & 0x0FFFFFFFu) == 0) {
                    if (fat_set(clus, 0x0FFFFFFFu) != 0) {
                        return 0;
                    }
                    g_alloc_hint = clus + 1;
                    return clus;
                }
            }
        }
    }
    return 0;       /* 盘满了 */
}

/* 释放一整条簇链。删除文件与截断都要用。 */
static int clus_free_chain(u32 start)
{
    u32 c = start;
    u32 guard = 0;
    while (clus_valid(c) && guard++ < g_fs.cluster_count) {
        u32 next = fat_next(c);
        if (fat_set(c, 0) != 0) {
            return -1;
        }
        if (next >= 0x0FFFFFF8u) {
            break;
        }
        c = next;
    }
    return 0;
}

/* 把 "NAME.EXT" 转成 FAT 的 11 字节 8.3 形式。返回 0 成功，-1 表示名字不合规。
 *
 * ★ 为什么这一版只支持 8.3 ★
 * 长名（LFN）要生成一串 UTF-16 分片条目、还要算校验和、还要为短名做
 * "NAME~1.TXT" 的去重——那是几百行，而且它的价值是"名字更好看"，
 * 不是"能不能存东西"。**先让它能存**，长名作为明确的后续项。
 * 不支持时要给出**明确的原因**，而不是静默失败或者截断名字。 */
static int make_8_3(const char *name, u8 out[11])
{
    static const char *bad = "\"*+,/:;<=>?[\\]| ";
    u32 base_len = 0, ext_len = 0;
    const char *dot = 0;

    for (const char *p = name; *p; p++) {
        if (*p == '.') {
            dot = p;            /* 最后一个点才是扩展名分隔 */
        }
        if (*p < 32 || *p > 126) {
            return -1;
        }
        for (const char *b = bad; *b; b++) {
            if (*p == *b) {
                return -1;
            }
        }
    }
    for (const char *p = name; *p && p != dot; p++) {
        base_len++;
    }
    if (dot) {
        for (const char *p = dot + 1; *p; p++) {
            ext_len++;
        }
    }
    if (base_len == 0 || base_len > 8 || ext_len > 3) {
        return -1;
    }
    if (dot && ext_len == 0) {
        return -1;
    }

    for (u32 i = 0; i < 11; i++) {
        out[i] = ' ';
    }
    u32 k = 0;
    for (const char *p = name; *p && p != dot; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        out[k++] = (u8)c;
    }
    k = 8;
    if (dot) {
        for (const char *p = dot + 1; *p; p++) {
            char c = *p;
            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            out[k++] = (u8)c;
        }
    }
    return 0;
}

/* 构造一个 32 字节目录项。时间戳一律写 0：
 * 这一版没有 RTC，"把当前时间写进去"只能靠瞎编，而瞎编的时间戳
 * 比没有时间戳更糟（看起来像真的）。加 RTC 之后再补。 */
static void build_dirent(u8 e[32], const u8 name11[11], u8 attr,
                         u32 first_clus, u32 size)
{
    for (u32 i = 0; i < 32; i++) {
        e[i] = 0;
    }
    for (u32 i = 0; i < 11; i++) {
        e[i] = name11[i];
    }
    e[11] = attr;
    e[12] = 0;                                  /* 保留（大小写标志） */
    e[20] = (u8)((first_clus >> 16) & 0xFF);    /* 首簇高 16 位 */
    e[21] = (u8)((first_clus >> 24) & 0xFF);
    e[26] = (u8)(first_clus & 0xFF);            /* 首簇低 16 位 */
    e[27] = (u8)((first_clus >> 8) & 0xFF);
    e[28] = (u8)(size & 0xFF);
    e[29] = (u8)((size >> 8) & 0xFF);
    e[30] = (u8)((size >> 16) & 0xFF);
    e[31] = (u8)((size >> 24) & 0xFF);
}

/* 往目录里加一个条目。返回 0 成功。
 * 目录满了就给它**扩一个新簇**并挂到链尾——这是"能一直往里放文件"的前提，
 * 也是 mkfat.py 那条"目录自动扩容"在运行期的对应物。 */
static int dir_add(u32 dir_clus, const u8 name11[11], u8 attr,
                   u32 first_clus, u32 size)
{
    static u8 sec[SECTOR];
    u8 ent[32];
    build_dirent(ent, name11, attr, first_clus, size);

    u32 c = dir_clus;
    u32 guard = 0;
    u32 last = dir_clus;
    while (clus_valid(c) && guard++ < g_fs.cluster_count) {
        u32 base = clus_to_lba(c);
        for (u32 s = 0; s < g_fs.sec_per_clus; s++) {
            if (blk_read(base + s, 1, sec) != 0) {
                return -1;
            }
            for (u32 off = 0; off < SECTOR; off += 32) {
                if (sec[off] == 0x00 || sec[off] == 0xE5) {
                    /* 0x00 = 目录到此结束（后面全是自由空间），0xE5 = 已删除。
                     * 两种都能直接用。 */
                    for (u32 i = 0; i < 32; i++) {
                        sec[off + i] = ent[i];
                    }
                    /* 若这是本扇区的最后一个条目且原先是 0x00，
                     * 需要把"下一个"也标成 0x00——否则目录看起来会在
                     * 我们的新条目之后继续，而后面是垃圾。
                     * 简单做法：若 off+32 < 512，把它置 0x00 作为新的结尾。 */
                    if (sec[off] != 0xE5 && off + 32 < SECTOR) {
                        sec[off + 32] = 0x00;
                    }
                    return blk_write(base + s, 1, sec);
                }
            }
        }
        last = c;
        u32 next = fat_next(c);
        if (next >= 0x0FFFFFF8u || !clus_valid(next)) {
            break;
        }
        c = next;
    }

    /* 走到这里说明目录链用完了：扩一个簇 */
    u32 nc = clus_alloc();
    if (nc == 0) {
        return -1;
    }
    for (u32 i = 0; i < SECTOR; i++) {
        sec[i] = 0;
    }
    for (u32 i = 0; i < 32; i++) {
        sec[i] = ent[i];
    }
    if (fat_set(last, nc) != 0) {
        return -1;
    }
    if (blk_write(clus_to_lba(nc), 1, sec) != 0) {
        return -1;
    }
    /* 新簇剩下的扇区也要清零：目录里 0x00 表示"到此结束"，
     * 留着盘上的旧数据等于让目录项指向随机的旧内容。 */
    for (u32 i = 0; i < SECTOR; i++) {
        sec[i] = 0;
    }
    for (u32 s = 1; s < g_fs.sec_per_clus; s++) {
        if (blk_write(clus_to_lba(nc) + s, 1, sec) != 0) {
            return -1;
        }
    }
    return 0;
}

/* 在目录里按名字找一个条目，返回它的绝对位置（簇、扇区、扇区内偏移）。
 * 删除、改大小、追加都要"改回原来那一条"，所以必须能定位到它。 */
struct dir_slot {
    u32 clus;
    u32 lba;
    u32 off;
    int found;
};

static int dir_lookup(u32 dir_clus, const char *name, struct dir_slot *out)
{
    static u8 sec[SECTOR];
    u32 c = dir_clus;
    u32 guard = 0;
    out->found = 0;
    while (clus_valid(c) && guard++ < g_fs.cluster_count) {
        u32 base = clus_to_lba(c);
        for (u32 s = 0; s < g_fs.sec_per_clus; s++) {
            if (blk_read(base + s, 1, sec) != 0) {
                return -1;
            }
            for (u32 off = 0; off < SECTOR; off += 32) {
                if (sec[off] == 0x00) {
                    return 0;               /* 目录结束 */
                }
                if (sec[off] == 0xE5 || sec[off + 11] == 0x0F) {
                    continue;               /* 已删除 / 长名分片 */
                }
                char short_name[13];
                format_short(&sec[off], short_name);
                if (name_eq_ci(short_name, name)) {
                    out->clus = c;
                    out->lba = base + s;
                    out->off = off;
                    out->found = 1;
                    return 0;
                }
            }
        }
        u32 next = fat_next(c);
        if (next >= 0x0FFFFFF8u || !clus_valid(next)) {
            break;
        }
        c = next;
    }
    return 0;
}

/* 把路径拆成「父目录簇 + 最后一段名字」。
 * 只支持绝对路径——相对路径要引入"当前目录"这个每进程状态，
 * 而 shell 与所有现有调用者都用绝对路径，加它只是凭空多一个概念。 */
static int resolve_parent(const char *path, u32 *out_clus, char *out_name, u32 cap)
{
    if (!path || path[0] != '/') {
        return -1;
    }
    const char *last = 0;
    for (const char *p = path; *p; p++) {
        if (*p == '/') {
            last = p;
        }
    }
    if (!last) {
        return -1;
    }
    u32 n = 0;
    for (const char *p = last + 1; *p; p++) {
        if (n + 1 >= cap) {
            return -1;
        }
        out_name[n++] = *p;
    }
    out_name[n] = '\0';
    if (n == 0) {
        return -1;              /* 路径以 '/' 结尾：那是目录，不是文件 */
    }

    char parent[NAME_MAX];
    u32 k = 0;
    if (last == path) {
        parent[k++] = '/';
    } else {
        for (const char *p = path; p < last; p++) {
            if (k + 1 >= sizeof(parent)) {
                return -1;
            }
            parent[k++] = *p;
        }
    }
    parent[k] = '\0';

    struct dirent_raw dir;
    if (!path_resolve(parent, &dir) || !(dir.attr & 0x10)) {
        return -1;
    }
    *out_clus = dir.first_clus;
    return 0;
}

/* 读一个文件条目（名字 + 首簇 + 大小）。追加写要用它。 */
static int lookup_file(const char *path, struct dirent_raw *out)
{
    if (!path_resolve(path, out) || (out->attr & 0x10)) {
        return -1;
    }
    return 0;
}

/* 改一个已存在条目的首簇与大小。追加写、截断都要用它。 */
static int dir_update_entry(const struct dir_slot *slot, u32 first_clus, u32 size)
{
    static u8 sec[SECTOR];
    if (blk_read(slot->lba, 1, sec) != 0) {
        return -1;
    }
    sec[slot->off + 20] = (u8)((first_clus >> 16) & 0xFF);
    sec[slot->off + 21] = (u8)((first_clus >> 24) & 0xFF);
    sec[slot->off + 26] = (u8)(first_clus & 0xFF);
    sec[slot->off + 27] = (u8)((first_clus >> 8) & 0xFF);
    sec[slot->off + 28] = (u8)(size & 0xFF);
    sec[slot->off + 29] = (u8)((size >> 8) & 0xFF);
    sec[slot->off + 30] = (u8)((size >> 16) & 0xFF);
    sec[slot->off + 31] = (u8)((size >> 24) & 0xFF);
    return blk_write(slot->lba, 1, sec);
}

/* 取"第 n 个簇"的簇号；必要时沿链**扩展**。
 * 返回 0 表示失败（越界或盘满）。`*head` 在首簇为空时会被填上新分配的首簇。 */
static u32 clus_nth_or_extend(u32 *head, u32 n, int extend)
{
    if (*head == 0) {
        if (!extend) {
            return 0;
        }
        u32 c = clus_alloc();
        if (c == 0) {
            return 0;
        }
        *head = c;
        return c;
    }
    u32 c = *head;
    for (u32 i = 0; i < n; i++) {
        u32 next = fat_next(c);
        if (next >= 0x0FFFFFF8u || !clus_valid(next)) {
            if (!extend) {
                return 0;
            }
            u32 nc = clus_alloc();
            if (nc == 0) {
                return 0;
            }
            if (fat_set(c, nc) != 0) {
                return 0;
            }
            next = nc;
        }
        c = next;
    }
    return c;
}

/* 创建（或截断）一个文件。 */
static void handle_create(long reply, const struct fs_req *req, u64 who)
{
    g_write_denied = 0;
    (void)who;
    u32 parent = 0;
    char name[NAME_MAX];
    u8 name11[11];
    u32 status = 0;

    if (resolve_parent(req->path, &parent, name, sizeof(name)) != 0) {
        status = 1;         /* 父目录不存在 */
        goto done;
    }
    if (make_8_3(name, name11) != 0) {
        /* ★ 明确区分"名字不合规"与"别的失败" ★
         * 调用者（shell）要能告诉用户"这个文件名太长，只支持 8.3"，
         * 而不是一句笼统的失败——否则用户会以为磁盘满了或权限不对。 */
        status = 2;         /* 名字不是合法的 8.3 */
        goto done;
    }

    struct dir_slot slot;
    if (dir_lookup(parent, name, &slot) != 0) {
        status = 3;
        goto done;
    }
    if (slot.found) {
        /* 已存在：按 `>` 的语义截断——先释放旧簇链，再把首簇与大小清零 */
        static u8 sec[SECTOR];
        if (blk_read(slot.lba, 1, sec) != 0) {
            status = 3;
            goto done;
        }
        u32 fc = (u32)sec[slot.off + 26] | ((u32)sec[slot.off + 27] << 8) |
                 ((u32)sec[slot.off + 20] << 16) | ((u32)sec[slot.off + 21] << 24);
        if (clus_valid(fc) && clus_free_chain(fc) != 0) {
            status = 3;
            goto done;
        }
        if (dir_update_entry(&slot, 0, 0) != 0) {
            status = 3;
            goto done;
        }
    } else {
        if (dir_add(parent, name11, 0x20, 0, 0) != 0) {
            status = 3;
            goto done;
        }
    }
    if (blk_flush() != 0) {
        status = 4;
        goto done;
    }
    status = 0;

done:
    if (status != 0 && g_write_denied) {
        status = 10;        /* 被访问矩阵拒绝：与 I/O 错分开报 */
    }
    fs_reply(reply, &status, sizeof(status));
}

/* 写一段数据（从 offset 开始；offset == 当前大小就是追加）。 */
static void handle_write(long reply, const struct fs_req *req, const u8 *data,
                         u32 data_len, u64 who)
{
    g_write_denied = 0;
    (void)who;
    u32 status = 0;
    struct dirent_raw file;
    struct dir_slot slot;
    u32 parent = 0;
    char name[NAME_MAX];

    if (data_len == 0) {
        status = 5;
        goto done;
    }
    if (lookup_file(req->path, &file) != 0) {
        status = 1;         /* 文件不存在（这一版不隐式创建，让调用者决定） */
        goto done;
    }
    if (req->offset > file.size) {
        /* 不支持空洞：中间的空档要怎么填（零？保留旧数据？）规范上两种都见过，
         * 而我们的调用者（shell 追加）从不需要它。明确拒绝好过猜。 */
        status = 6;
        goto done;
    }
    if (resolve_parent(req->path, &parent, name, sizeof(name)) != 0 ||
        dir_lookup(parent, name, &slot) != 0 || !slot.found) {
        status = 3;
        goto done;
    }

    u32 clus_bytes = clus_size_bytes();
    u32 head = file.first_clus;
    u64 off = req->offset;
    u32 left = data_len;
    const u8 *p = data;
    static u8 sec[SECTOR];

    while (left > 0) {
        u32 idx = (u32)(off / clus_bytes);
        u32 in_clus = (u32)(off % clus_bytes);
        /* 需要这一簇时才分配：所以"创建一个空文件"不占任何簇，
         * 与 mkdir 的语义一致，也让 8.3 空文件不浪费空间。 */
        u32 c = clus_nth_or_extend(&head, idx, 1);
        if (c == 0) {
            status = 7;     /* 盘满 */
            break;
        }
        u32 lba = clus_to_lba(c);
        u32 sec_in_clus = in_clus / SECTOR;
        u32 in_sec = in_clus % SECTOR;
        u32 n = SECTOR - in_sec;
        if (n > left) {
            n = left;
        }
        if (in_sec == 0 && n == SECTOR) {
            /* 整扇区：直接写，不必先读 */
            int wr = blk_write(lba + sec_in_clus, 1, p);
            if (wr != 0) {
                /* -2 = 被访问矩阵拒绝（不是 I/O 错），要分开报：
                 * 用户看到"运行中的槽写不了"应该知道那是设计。 */
                status = (wr == -2) ? 10 : 8;
                break;
            }
        } else {
            /* 部分扇区：读-改-写。不先读的话会把这一扇区里原有的字节清掉。 */
            if (blk_read(lba + sec_in_clus, 1, sec) != 0) {
                status = 8;
                break;
            }
            for (u32 i = 0; i < n; i++) {
                sec[in_sec + i] = p[i];
            }
            {
                int wr = blk_write(lba + sec_in_clus, 1, sec);
                if (wr != 0) {
                    status = (wr == -2) ? 10 : 8;   /* 10 = 被写保护拒绝 */
                    break;
                }
            }
        }
        p += n;
        off += n;
        left -= n;
    }

    if (status != 0 && g_write_denied) {
        status = 10;        /* 被保护拒绝（数据段写不进去） */
    }
    if (status == 0) {
        u32 new_size = file.size;
        if (off > new_size) {
            new_size = (u32)off;
        }
        if (dir_update_entry(&slot, head, new_size) != 0) {
            status = 3;
        } else if (blk_flush() != 0) {
            status = 4;
        }
    }
    /* 写了多少字节也回给调用者：调用者据此知道"是不是全写进去了" */
    {
        u32 rep[2];
        rep[0] = status;
        rep[1] = (status == 0 || status == 7 || status == 8)
                 ? (u32)(off - req->offset) : 0;
        fs_reply(reply, rep, sizeof(rep));
        return;
    }

done:
    {
        u32 rep[2];
        rep[0] = status;
        rep[1] = 0;
        fs_reply(reply, rep, sizeof(rep));
    }
}

/* 删除一个文件：释放簇链 + 把目录项首字节标成 0xE5。 */
static void handle_unlink(long reply, const struct fs_req *req, u64 who)
{
    g_write_denied = 0;
    (void)who;
    u32 status = 0;
    u32 parent = 0;
    char name[NAME_MAX];
    struct dir_slot slot;
    static u8 sec[SECTOR];

    if (resolve_parent(req->path, &parent, name, sizeof(name)) != 0 ||
        dir_lookup(parent, name, &slot) != 0 || !slot.found) {
        status = 1;
        goto done;
    }
    if (blk_read(slot.lba, 1, sec) != 0) {
        status = 3;
        goto done;
    }
    if (sec[slot.off + 11] & 0x10) {
        status = 9;         /* 是目录：这一版不删目录（要递归，风险更大） */
        goto done;
    }
    u32 fc = (u32)sec[slot.off + 26] | ((u32)sec[slot.off + 27] << 8) |
             ((u32)sec[slot.off + 20] << 16) | ((u32)sec[slot.off + 21] << 24);
    if (clus_valid(fc) && clus_free_chain(fc) != 0) {
        status = 3;
        goto done;
    }
    sec[slot.off] = 0xE5;       /* 已删除标记 */
    if (blk_write(slot.lba, 1, sec) != 0) {
        status = 3;
        goto done;
    }
    if (blk_flush() != 0) {
        status = 4;
    }

done:
    if (status != 0 && g_write_denied) {
        status = 10;        /* 被访问矩阵拒绝：与 I/O 错分开报 */
    }
    fs_reply(reply, &status, sizeof(status));
}

/* ------------------------------------------------------------------ */

static int fs_mount(void)
{
    /* 分区表解析属于文件系统层，块设备驱动不该知道什么叫分区 */
    u8 mbr[SECTOR];
    if (blk_read(0, 1, mbr) != 0 || mbr[510] != 0x55 || mbr[511] != 0xAA) {
        return -1;
    }
    u32 part_lba = 0;
    for (int i = 0; i < 4; i++) {
        const u8 *e = &mbr[446 + i * 16];
        u32 start = (u32)(e[8] | (e[9] << 8) | (e[10] << 16) | (e[11] << 24));
        if (e[4] != 0 && start != 0) {
            part_lba = start;
            break;
        }
    }
    if (part_lba == 0) {
        return -1;
    }
    g_fs.part_lba = part_lba;

    u8 bs[SECTOR];
    if (blk_read(part_lba, 1, bs) != 0 || bs[510] != 0x55 || bs[511] != 0xAA) {
        return -1;
    }
    g_fs.bytes_per_sec = (u16)(bs[11] | (bs[12] << 8));
    g_fs.sec_per_clus = bs[13];
    g_fs.reserved_secs = (u16)(bs[14] | (bs[15] << 8));
    g_fs.num_fats = bs[16];
    u32 total16 = (u16)(bs[19] | (bs[20] << 8));
    u32 total32 = (u32)(bs[32] | (bs[33] << 8) | (bs[34] << 16) | (bs[35] << 24));
    g_fs.total_secs = total16 ? total16 : total32;
    g_fs.fat_size_secs = (u32)(bs[36] | (bs[37] << 8) | (bs[38] << 16) | (bs[39] << 24));
    g_fs.root_clus = (u32)(bs[44] | (bs[45] << 8) | (bs[46] << 16) | (bs[47] << 24));

    if (g_fs.bytes_per_sec != SECTOR || g_fs.sec_per_clus == 0 ||
        g_fs.fat_size_secs == 0 || g_fs.num_fats == 0 || g_fs.root_clus < 2) {
        return -1;
    }
    g_fs.fat_start_lba = part_lba + g_fs.reserved_secs;
    g_fs.data_start_lba = g_fs.fat_start_lba + (u32)g_fs.num_fats * g_fs.fat_size_secs;
    if (g_fs.data_start_lba >= part_lba + g_fs.total_secs) {
        return -1;
    }
    g_fs.cluster_count = (part_lba + g_fs.total_secs - g_fs.data_start_lba) /
                         g_fs.sec_per_clus;

    for (int i = 0; i < 11; i++) {
        g_fs.label[i] = (char)bs[71 + i];
    }
    g_fs.label[11] = '\0';
    for (int i = 0; i < 8; i++) {
        g_fs.oem[i] = (char)bs[3 + i];
    }
    g_fs.oem[8] = '\0';

    /* FAT 类型按**簇数**判定（微软规范的口径），而不是信分区类型字节——
     * 那个字节是"声称"，簇数才是"实际"，两者不一致的镜像很常见。 */
    u32 ft = (g_fs.cluster_count < 4085) ? 12
           : (g_fs.cluster_count < 65525) ? 16 : 32;
    if (ft != 32) {
        say("  [fsd] 按簇数判定这是 FAT");
        num(ft);
        say("，本服务只实现 FAT32\n");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 请求处理                                                            */
/* ------------------------------------------------------------------ */

static void handle_info(long reply)
{
    struct fs_info info;
    info.total_bytes = (u64)g_fs.total_secs * g_fs.bytes_per_sec;
    info.free_bytes = 0;        /* 要扫整张 FAT 才算得准，本轮不做 */
    info.cluster_size = clus_size_bytes();
    info.clusters = g_fs.cluster_count;
    info.fat_type = 32;
    for (u32 i = 0; i < sizeof(info.label); i++) {
        info.label[i] = (i < 11) ? g_fs.label[i] : '\0';
    }
    for (u32 i = 0; i < sizeof(info.oem); i++) {
        info.oem[i] = (i < 8) ? g_fs.oem[i] : '\0';
    }
    fs_reply(reply, &info, sizeof(info));
}

static void handle_list(long reply, const struct fs_req *req)
{
    struct dirent_raw dir;
    if (!path_resolve(req->path, &dir) || !(dir.attr & 0x10)) {
        fs_reply(reply, 0, 0);
        return;
    }
    static struct fs_list_reply out;
    out.count = 0;
    out.next_index = 0;

    u32 idx = req->index;
    int io_err = 0;
    for (u32 n = 0; n < LIST_MAX; n++) {
        struct dirent_raw e;
        int r = dir_get(dir.first_clus, idx, &e);
        if (r < 0) {
            /* ★ "读不到" 和 "到这里就没有了" 必须分开 ★
             * 第一版把两者一起 break 了，于是**枚举被拒绝**表现为
             * "这个目录是空的"。这一条直接害我误判过一次：A/B 块级拷贝之后
             * 目录项指向另一个槽，读被访问矩阵拒掉，而日志里看到的是
             * 「共 0 项」——看起来像目录真的空了，完全指不到真正的原因。
             * 错误必须能被调用者看见，否则它就不是错误，只是一条错的信息。 */
            io_err = 1;
            break;
        }
        if (r == 0) {
            break;                  /* 真的到末尾了 */
        }
        const char *nm = e.has_long ? e.long_name : e.short_name;
        u32 k = 0;
        for (const char *p = nm; *p && k < NAME_MAX - 1; p++) {
            out.entries[n].name[k++] = *p;
        }
        out.entries[n].name[k] = '\0';
        out.entries[n].size = e.size;
        out.entries[n].attr = e.attr;
        out.entries[n].is_dir = (e.attr & 0x10) ? 1 : 0;
        out.entries[n]._pad[0] = 0;
        out.entries[n]._pad[1] = 0;
        out.count++;
        idx++;
    }
    if (out.count == LIST_MAX) {
        out.next_index = idx;       /* 还有更多，告诉客户端从哪继续 */
    }
    /* 中途读失败：一项都没列出来时**报错**（空应答 = 客户端认为出错），
     * 已经列出来的部分照常返回，但 next_index 置 0 让客户端知道到此为止。 */
    if (io_err && out.count == 0) {
        fs_reply(reply, 0, 0);
        return;
    }
    fs_reply(reply, &out, sizeof(out));
}

/* 路径元数据。目录与文件都要能回答，不存在的路径返回空应答（客户端译为 ENOENT）。 */
static void handle_stat(long reply, const struct fs_req *req)
{
    struct dirent_raw f;
    if (!path_resolve(req->path, &f)) {
        fs_reply(reply, 0, 0);
        return;
    }
    struct fe_fs_stat_reply out;
    int is_dir = (f.attr & 0x10) != 0;
    /* 模式位用 POSIX 的取值：目录 0755、文件 0644。
     * 只读挂载下这是事实（写路径由访问矩阵挡，不靠权限位）。 */
    out.mode = (is_dir ? 0040000u : 0100000u) | (is_dir ? 0755u : 0644u);
    out.size = is_dir ? 0 : f.size;
    out.attr = f.attr;
    out._pad = 0;
    fs_reply(reply, &out, sizeof(out));
}

static void handle_read(long reply, const struct fs_req *req)
{
    struct dirent_raw f;
    if (!path_resolve(req->path, &f) || (f.attr & 0x10)) {
        fs_reply(reply, 0, 0);
        return;
    }
    static struct fs_read_reply out;
    out.len = 0;
    if (req->offset >= f.size) {
        fs_reply(reply, &out, 8);       /* 到末尾：返回 0 字节 */
        return;
    }
    u32 want = req->len;
    if (want > READ_MAX) {
        want = READ_MAX;
    }
    u32 remain = f.size - (u32)req->offset;
    if (want > remain) {
        want = remain;
    }

    /* 顺链跳到 offset 所在簇 */
    u32 cs = clus_size_bytes();
    u32 clus = f.first_clus;
    for (u32 i = 0; i < (u32)req->offset / cs; i++) {
        clus = fat_next(clus);
        if (!clus_valid(clus)) {
            fs_reply(reply, &out, 8);
            return;
        }
    }
    u32 in_clus = (u32)req->offset % cs;

    u32 got = 0;
    while (got < want) {
        if (!clus_valid(clus)) {
            break;
        }
        /* 只读这一簇里需要的那一个扇区——不把整簇读进来，
         * 因为簇最大可以到 32 KiB，栈上放不下，放静态又白读很多。 */
        u32 sec_in = in_clus / SECTOR;
        u32 off_in = in_clus % SECTOR;
        u8 secbuf[SECTOR];
        if (blk_read(clus_to_lba(clus) + sec_in, 1, secbuf) != 0) {
            break;
        }
        u32 can = SECTOR - off_in;
        if (can > want - got) {
            can = want - got;
        }
        for (u32 i = 0; i < can; i++) {
            out.data[got + i] = (char)secbuf[off_in + i];
        }
        got += can;
        in_clus += can;
        if (in_clus >= cs) {
            clus = fat_next(clus);
            in_clus = 0;
        }
    }
    out.len = got;
    fs_reply(reply, &out, 8 + got);
}

static void fs_reply(long reply_ep, const void *data, u32 len)
{
    if (reply_ep <= 0) {
        return;
    }
    struct fe_msg_header rh;
    rh.protocol = 0x46530000u;      /* "FS" */
    rh.opcode = 0;
    rh.payload_len = len;
    rh.handle_count = 0;
    rh.request_id = 0;
    fe_endpoint_send(reply_ep, &rh, data, NULL, 0);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    say("\n[fsd] FAT32 文件系统服务启动（ring 3，可读可写）\n");

    /* 依赖 blkd：**按路径找**，而不是等谁把句柄塞过来。
     * init 保证 blkd 先起来（单元表里 fsd 声明 require /dev/blk0）。 */
    g_blk = fe_devfs_open("/dev/blk0");
    step("通过 devfs 找到 /dev/blk0", g_blk > 0);
    if (g_blk <= 0) {
        say("  [fsd] 没有块设备，退出\n");
        return 1;
    }

    struct blk_req breq;
    breq.op = BLK_OP_INFO;
    breq.count = 0;
    breq.lba = 0;
    struct blk_info binfo;
    u32 got = 0;
    long r = fe_endpoint_call(g_blk, &breq, sizeof(breq), &binfo, sizeof(binfo), &got);
    step("问块设备要信息", r == FE_OK && got == sizeof(binfo));
    if (r == FE_OK && got == sizeof(binfo)) {
        say("  [fsd] 底层设备 \"");
        say(binfo.model);
        say("\"，");
        num(binfo.sectors);
        say(" 扇区\n");
    }

    step("挂载 FAT32（MBR → 分区 → BPB）", fs_mount() == 0);
    if (g_fail) {
        say("  [fsd] 挂载失败，退出\n");
        return 1;
    }
    say("  [fsd] 卷标 \"");
    say(g_fs.label);
    say("\"，OEM \"");
    say(g_fs.oem);
    say("\"\n");
    say("  [fsd] 每簇 ");
    num(g_fs.sec_per_clus);
    say(" 扇区，");
    num(g_fs.cluster_count);
    say(" 簇（");
    num((u64)g_fs.cluster_count * clus_size_bytes() / 1024);
    say(" KiB），根目录簇 ");
    num(g_fs.root_clus);
    say("\n");

    /* 挂载自检：根目录必须能列出来且非空。
     * 这一条比"BPB 字段看着对"强得多——它要求整条链真的能读出数据。 */
    u32 n = 0;
    struct dirent_raw e;
    for (u32 i = 0;; i++) {
        int rr = dir_get(g_fs.root_clus, i, &e);
        if (rr <= 0) {
            break;
        }
        n++;
    }
    say("  [fsd] 根目录可列出 ");
    num((u64)n);
    say(" 项\n");
    step("根目录非空（证明 FAT 链与目录解析都能工作）", n > 0);

    long ep = fe_endpoint_create(0);
    step("创建服务端点", ep > 0);
    if (ep <= 0) {
        return 1;
    }
    r = fe_devfs_publish("/dev/fs0", ep);
    step("发布 /dev/fs0", r == FE_OK);

    say("  [fsd] 初始化结束，失败项 ");
    num((u64)g_fail);
    say("；进入请求服务循环\n");

    for (;;) {
        struct fe_msg_header hdr;
        /* ★ 收进整块载荷缓冲，而不是只收请求结构体 ★
         * 写请求的数据紧跟在 fs_req 之后；只收 sizeof(req) 会把数据截掉，
         * 而截掉的后果不是报错，是"写进去的内容少了一截"。
         * 接收上限用 FE_MSG_MAX_PAYLOAD（与内核的内联载荷上限一致）。 */
        static u8 payload[FE_MSG_MAX_PAYLOAD];
        long reply = fe_endpoint_recv(ep, &hdr, payload, sizeof(payload), NULL, NULL);
        if (reply <= 0) {
            continue;
        }
        if (hdr.payload_len < 8) {
            fs_reply(reply, 0, 0);
            fe_handle_close(reply);
            continue;
        }
        struct fs_req req;
        memcpy(&req, payload, sizeof(req));
        u64 who = hdr.sender_task;
        if (req.op == FS_OP_INFO) {
            handle_info(reply);
        } else if (req.op == FS_OP_LIST) {
            handle_list(reply, &req);
        } else if (req.op == FS_OP_READ) {
            handle_read(reply, &req);
        } else if (req.op == FS_OP_CREATE) {
            handle_create(reply, &req, who);
        } else if (req.op == FS_OP_WRITE) {
            u32 dlen = 0;
            if (hdr.payload_len > sizeof(req)) {
                dlen = hdr.payload_len - (u32)sizeof(req);
            }
            handle_write(reply, &req, payload + sizeof(req), dlen, who);
        } else if (req.op == FS_OP_UNLINK) {
            handle_unlink(reply, &req, who);
        } else if (req.op == FS_OP_STAT) {
            handle_stat(reply, &req);
        } else {
            fs_reply(reply, 0, 0);
        }
        fe_handle_close(reply);
    }
}
