/* SPDX-License-Identifier: 0BSD */
/* 文件系统客户端协议（接口层）。
 *
 * 从 `user/bin/fs/main.c` 里抽出来的——shell 也要 ls/cat，
 * 而"每个客户端各抄一份协议"正是 fe_blk.h 里记过的那个坑：
 * 抄一份的代价不是那几十行，而是**几份"我以为协议是这样的"会各自漂移**。
 *
 * 用法：
 *     long fs = fe_devfs_open("/dev/fs0");
 *     struct fs_list_reply lr;
 *     fe_fs_list(fs, "/", 0, &lr);
 *     ...
 */
#ifndef FE_FS_H
#define FE_FS_H

#include <fe_user.h>

#define FE_FS_OP_INFO 1
#define FE_FS_OP_LIST 2
#define FE_FS_OP_READ 3
#define FE_FS_OP_CREATE 4
#define FE_FS_OP_WRITE 5
#define FE_FS_OP_UNLINK 6
/* 路径元数据（POSIX 的 stat 靠它落地）。
 *
 * ★ 为什么要有它、而不是让客户端用 LIST 去凑 ★
 * LIST 是"列一个目录的一页"，它回答的是"这个目录里有什么"；
 * 而 stat 问的是"**这个**路径是什么"。前者凑不出后者：
 * 对根目录、对文件、对不存在的路径，语义全都不同，
 * 而按页遍历去判断"某个名字在不在"还会随目录大小退化。
 * 语义不同就该是两个操作——这是接口层的判断，不是实现细节。 */
#define FE_FS_OP_STAT 7

/* 写操作的返回状态。**把失败原因分开报**，调用者才能给出有用的提示：
 * "名字太长"和"磁盘满了"对用户的含义完全不同。 */
#define FE_FS_OK          0
#define FE_FS_ENOENT      1
#define FE_FS_ENAME       2    /* 不是合法的 8.3 名字（这一版只支持 8.3） */
#define FE_FS_EIO         3
#define FE_FS_EFLUSH      4
#define FE_FS_EEMPTY      5
#define FE_FS_EGAP        6    /* 试图跳过空洞写（不支持） */
#define FE_FS_ENOSPC      7
#define FE_FS_EISDIR      9
#define FE_FS_EDENIED     10   /* 被访问矩阵拒绝（写受保护的区域：设计如此） */

#define FE_FS_PATH_MAX 128
#define FE_FS_NAME_MAX 64
#define FE_FS_LIST_MAX 8
#define FE_FS_CHUNK   1000      /* 单次读回的数据字节数（受内联载荷上限约束） */

struct fe_fs_req {
    u32 op;
    u32 index;
    u64 offset;
    u32 len;
    u32 _pad;
    char path[FE_FS_PATH_MAX];
};

struct fe_fs_info {
    u64 total_bytes;
    u64 free_bytes;
    u32 cluster_size;
    u32 clusters;
    u32 fat_type;
    char label[12];
    char oem[9];
};

struct fe_fs_dirent {
    char name[FE_FS_NAME_MAX];
    u32 size;
    u8  attr;
    u8  is_dir;
    u8  _pad[2];
};

struct fe_fs_list_reply {
    u32 next_index;         /* 0 = 没有更多了 */
    u32 count;
    struct fe_fs_dirent entries[FE_FS_LIST_MAX];
};

struct fe_fs_read_reply {
    u32 len;                /* 本次读到的字节数；0 = 到文件末尾 */
    u32 _pad;
    char data[FE_FS_CHUNK];
};

/* STAT 的应答。
 *
 * `mode` 用 **POSIX 的 S_IF* 取值**（内核的 ramfs 也沿用同一套，
 * 见 kernel/include/fe/vfs.h）：这样 POSIX 层不用做一次翻译，
 * 而"两套模式位"正是漂移的老来源。
 * 只读文件系统上权限位是常量：目录 0755、文件 0644——
 * 报出来的是**事实**（谁都能读、只有那个身份能写），不是猜的。 */
struct fe_fs_stat_reply {
    u32 mode;               /* S_IFMT | 权限位 */
    u32 size;               /* 目录时为 0 */
    u32 attr;               /* 原始 FAT 属性（诊断用） */
    u32 _pad;
};

static inline void fe_fs_req_init(struct fe_fs_req *req, u32 op, const char *path)
{
    u8 *p = (u8 *)req;
    u32 k = 0;
    for (u32 i = 0; i < sizeof(*req); i++) {
        p[i] = 0;
    }
    req->op = op;
    if (path) {
        for (const char *s = path; *s && k < FE_FS_PATH_MAX - 1; s++) {
            req->path[k++] = *s;
        }
    }
}

static inline int fe_fs_info(long fs, const char *path, struct fe_fs_info *out)
{
    struct fe_fs_req req;
    u32 got = 0;
    long r;
    fe_fs_req_init(&req, FE_FS_OP_INFO, path ? path : "/");
    r = fe_endpoint_call(fs, &req, (u32)sizeof(req), out, (u32)sizeof(*out), &got);
    return (r == FE_OK && got >= 8) ? 0 : -1;
}

/* 列目录的一页。返回 0 成功。 */
static inline int fe_fs_list(long fs, const char *path, u32 index,
                             struct fe_fs_list_reply *out)
{
    struct fe_fs_req req;
    u32 got = 0;
    long r;
    fe_fs_req_init(&req, FE_FS_OP_LIST, path);
    req.index = index;
    r = fe_endpoint_call(fs, &req, (u32)sizeof(req), out, (u32)sizeof(*out), &got);
    return (r == FE_OK && got >= sizeof(*out)) ? 0 : -1;
}

/* 读一段。返回本次读到的字节数（0 = 末尾），失败 -1。 */
static inline int fe_fs_read(long fs, const char *path, u64 offset,
                             struct fe_fs_read_reply *out)
{
    struct fe_fs_req req;
    u32 got = 0;
    long r;
    fe_fs_req_init(&req, FE_FS_OP_READ, path);
    req.offset = offset;
    req.len = FE_FS_CHUNK;
    r = fe_endpoint_call(fs, &req, (u32)sizeof(req), out, (u32)sizeof(*out), &got);
    if (r != FE_OK || got < 8) {
        return -1;
    }
    if (out->len > FE_FS_CHUNK) {
        out->len = FE_FS_CHUNK;     /* 不信服务端给的越界长度 */
    }
    return (int)out->len;
}

/* 创建（或截断）文件。返回 FE_FS_* 状态。 */
static inline int fe_fs_create(long fs, const char *path)
{
    struct fe_fs_req req;
    u32 status = 0xFFFF;
    u32 got = 0;
    long r;
    fe_fs_req_init(&req, FE_FS_OP_CREATE, path);
    r = fe_endpoint_call(fs, &req, (u32)sizeof(req), &status, sizeof(status), &got);
    if (r != FE_OK || got != sizeof(status)) {
        return FE_FS_EIO;
    }
    return (int)status;
}

/* 从 offset 写一段数据。`*out_written` 收到实际写入的字节数（可为 NULL）。
 * 返回 FE_FS_* 状态。 */
static inline int fe_fs_write(long fs, const char *path, u64 offset,
                              const void *data, u32 len, u32 *out_written)
{
    /* 载荷 = 请求头 + 数据；上限由内联载荷决定（FE_MSG_MAX_PAYLOAD） */
    static char buf[FE_MSG_MAX_PAYLOAD];
    struct fe_fs_req req;
    u32 rep[2];
    u32 got = 0;
    long r;

    if (len > sizeof(buf) - sizeof(req)) {
        return FE_FS_EIO;       /* 调用者要自己分块 */
    }
    fe_fs_req_init(&req, FE_FS_OP_WRITE, path);
    req.offset = offset;
    req.len = len;
    memcpy(buf, &req, sizeof(req));
    memcpy(buf + sizeof(req), data, len);
    r = fe_endpoint_call(fs, buf, (u32)(sizeof(req) + len),
                         rep, sizeof(rep), &got);
    if (r != FE_OK || got != sizeof(rep)) {
        return FE_FS_EIO;
    }
    if (out_written) {
        *out_written = rep[1];
    }
    return (int)rep[0];
}

/* 删除文件。返回 FE_FS_* 状态。 */
static inline int fe_fs_unlink(long fs, const char *path)
{
    struct fe_fs_req req;
    u32 status = 0xFFFF;
    u32 got = 0;
    long r;
    fe_fs_req_init(&req, FE_FS_OP_UNLINK, path);
    r = fe_endpoint_call(fs, &req, (u32)sizeof(req), &status, sizeof(status), &got);
    if (r != FE_OK || got != sizeof(status)) {
        return FE_FS_EIO;
    }
    return (int)status;
}

/* 取路径元数据。返回 0 成功（out 已填），-1 失败（不存在 / 不是目录树里的路径）。
 *
 * ★ 定义放在文件末尾，是因为它要用到上面的 fe_fs_req_init ★
 * 第一版把它写在了 req_init 之前，于是编译报 "call to undeclared function"
 * ——头文件里的**顺序**是接口的一部分，不是排版偏好。 */
static inline int fe_fs_stat(long fs, const char *path, struct fe_fs_stat_reply *out)
{
    struct fe_fs_req req;
    u32 got = 0;
    fe_fs_req_init(&req, FE_FS_OP_STAT, path);
    long r = fe_endpoint_call(fs, &req, (u32)sizeof(req), out, (u32)sizeof(*out), &got);
    return (r == FE_OK && got == sizeof(*out)) ? 0 : -1;
}

#endif /* FE_FS_H */
