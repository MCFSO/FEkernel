/* SPDX-License-Identifier: 0BSD */
/* /dev/blk0 的请求协议（接口层）。
 *
 * ★ 它为什么不在 libfe 里 ★
 * libfe 是**运行时库**：它只做"把系统调用包成 C 函数"这件事，不含任何设备语义
 * （分层规则见 docs/00-architecture.md §7）。而这里每一样都是块设备语义：
 * 操作码、单次扇区上限、IPC 载荷怎么摆。把它塞进 libfe，libfe 就变成了
 * "运行时 + 块设备协议"的混合体，下一个设备（网卡、显示）也会照抄这个先例。
 *
 * ★ 它为什么也不该在各客户端里各抄一份 ★
 * 抄一份的代价不是那二十行，而是**三份"我以为协议是这样的"会各自漂移**：
 * 改协议时改了两处漏了第三处，表现为"某个程序偶发读不到数据"。
 * 所以协议定义只有这一份，客户端共享它。
 *
 * 注意这里是 `static inline`：它不需要链接任何库，也不需要额外的 .c 文件。
 * 这层薄到"没有实现"，正是接口层该有的样子。 */
#ifndef FE_BLK_H
#define FE_BLK_H

#include <fe_user.h>

#define FE_BLK_SECTOR    512

/* 单次请求的扇区数上限。★ 读和写**不一样**，这不是笔误 ★
 *
 * IPC 内联载荷上限是 1024 字节，而：
 *   读：请求头 16 字节，回包是纯数据 → 1024/512 = 2 个扇区；
 *   写：请求是"头 + 数据"，头占 16 字节 → 只剩 1008 字节 = 1 个扇区。
 *
 * 我第一版把两者都当成 2，症状很有代表性：**只有大于 1 个扇区的区间失败**，
 * 而 1 个扇区的区间全都成功（因为 chunk 会被夹到 count）。
 * 于是"拷贝失败"看起来像是权限问题，其实是被载荷大小卡住的。
 * 把上限拆成两个名字，就是为了让这个区别写在接口上而不是靠人记住。 */
#define FE_BLK_MAX_READ  2
#define FE_BLK_MAX_WRITE 1
#define FE_BLK_MAX_CHUNK FE_BLK_MAX_READ   /* 读侧的旧名字，保留 */

#define FE_BLK_OP_INFO  1
#define FE_BLK_OP_READ  2
#define FE_BLK_OP_WRITE 3
#define FE_BLK_OP_FLUSH 4

/* 刷新的结果。**"刷了"和"有屏障保证"是两件事**，所以分开报：
 * 有的驱动器不实现 FLUSH CACHE，这时数据已经交给它了，只是拿不到
 * "掉电也不会丢"的保证。把它当失败会让调用者去重试整个拷贝，
 * 而重试并不能让盘变得支持 flush。 */
#define FE_BLK_FLUSH_OK       1   /* 屏障成立 */
#define FE_BLK_FLUSH_NOGUARD  2   /* 写下去了，但没有屏障保证 */

struct fe_blk_req {
    u32 op;
    u32 count;
    u64 lba;
};

struct fe_blk_geometry {
    u64 sectors;
    u32 sector_size;
    u32 _pad;
};

/* 读 count 个扇区到 out（最多 FE_BLK_MAX_CHUNK）。返回 0 成功。
 *
 * 失败**不区分原因**地返回 -1，因为客户端能做的处置是同一个（放弃/报错）：
 * 内核返回 FE_ERR_ACCESS 说明这次访问被访问矩阵拒了，blkd 回一个空载荷；
 * 两者对客户端都只是"没拿到数据"。要区分原因得看内核的违规计数
 * （protcheck 就是这么做的）。 */
static inline int fe_blk_read(long ep, u64 lba, u32 count, void *out)
{
    struct fe_blk_req req;
    u32 got = 0;
    req.op = FE_BLK_OP_READ;
    req.count = count;
    req.lba = lba;
    long r = fe_endpoint_call(ep, &req, (u32)sizeof(req), out,
                              count * FE_BLK_SECTOR, &got);
    return (r == FE_OK && got == count * FE_BLK_SECTOR) ? 0 : -1;
}

/* 写 count 个扇区（最多 FE_BLK_MAX_CHUNK）。返回 0 成功。
 *
 * 载荷是"请求头 + 数据"拼在一起的一段：用户态没有 scatter/gather，
 * 而拼一次的代价远小于让内核加一个"两段载荷"的机制。 */
static inline int fe_blk_write(long ep, u64 lba, u32 count, const void *in)
{
    static u8 buf[sizeof(struct fe_blk_req) + FE_BLK_MAX_WRITE * FE_BLK_SECTOR];
    struct fe_blk_req req;
    u32 status = 0;
    u32 got = 0;
    const u8 *src = (const u8 *)in;
    const u8 *rh = (const u8 *)&req;

    if (count == 0 || count > FE_BLK_MAX_WRITE) {
        return -1;
    }
    req.op = FE_BLK_OP_WRITE;
    req.count = count;
    req.lba = lba;
    for (u32 i = 0; i < sizeof(req); i++) {
        buf[i] = rh[i];
    }
    for (u32 i = 0; i < count * FE_BLK_SECTOR; i++) {
        buf[sizeof(req) + i] = src[i];
    }
    long r = fe_endpoint_call(ep, buf, (u32)(sizeof(req) + count * FE_BLK_SECTOR),
                              &status, sizeof(status), &got);
    return (r == FE_OK && got == sizeof(status) && status == 1) ? 0 : -1;
}

/* 读一个扇区、改一个字节、写回去。
 *
 * 这就是"翻转引导控制块"的全部动作。那个字节的**位置**是 mkfat.py 建镜像时
 * 算好写进清单的，所以这里完全不需要懂 FAT：否则更新器就得实现目录项解析、
 * 簇链遍历、以及"改动会不会跨簇"——全是文件系统该干的活，
 * 而这段代码的目的只是改一个字节。 */
static inline int fe_blk_patch_byte(long ep, u64 lba, u64 offset, u8 value)
{
    static u8 sec[FE_BLK_SECTOR];
    if (offset >= FE_BLK_SECTOR) {
        return -1;
    }
    if (fe_blk_read(ep, lba, 1, sec) != 0) {
        return -1;
    }
    if (sec[offset] == value) {
        return 0;               /* 已经是目标值：不写，省掉一次可失败的写 */
    }
    sec[offset] = value;
    return fe_blk_write(ep, lba, 1, sec);
}

/* 把写缓存刷到盘上。返回 FE_BLK_FLUSH_OK / FE_BLK_FLUSH_NOGUARD，失败 -1。
 *
 * ★ 为什么它必须是一个独立操作，而不是跟在每次写后面 ★
 * 第一版在驱动的写路径里每个扇区都 FLUSH，理由是"写完就落盘更安全"。
 * 结果 2717 个扇区的拷贝用了 14.3 秒——每次 FLUSH 都是一次真实的落盘屏障，
 * 虚拟化下就是一次 fsync。**慢得像卡死**。
 *
 * 而屏障的意义本来就是"在某个提交点之前，之前写的都得在盘上"，
 * 那个提交点只有调用方知道：对 A/B 更新来说就是**翻引导控制块之前**，
 * 一次就够。所以刷新的粒度属于调用方，不属于驱动。 */
static inline int fe_blk_flush(long ep)
{
    struct fe_blk_req req;
    u32 status = 0;
    u32 got = 0;
    req.op = FE_BLK_OP_FLUSH;
    req.count = 0;
    req.lba = 0;
    long r = fe_endpoint_call(ep, &req, (u32)sizeof(req), &status,
                              sizeof(status), &got);
    if (r != FE_OK || got != sizeof(status)) {
        return -1;
    }
    return (int)status;
}

#endif /* FE_BLK_H */
