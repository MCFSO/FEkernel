/* SPDX-License-Identifier: 0BSD */
/* 虚拟文件系统接口层（VFS）。
 *
 * 分层的分界线画在这里：
 *
 *   VFS 负责 —— 路径解析、**挂载表**、inode 公共属性的统一入口
 *   后端负责 —— 目录项查找与遍历、文件内容（ramfs 是内存树，FAT32 是簇链，
 *               将来的 devfs 是「名字 → 端点能力」）
 *   上层 —— kernel/task/process.c 的 exec 路径**只认这一层**，
 *           不认识任何具体后端。换成 FAT32 时它一行都不用改。
 *
 * 为什么操作表是**每个后端一张**、而不是每个 inode 一套：
 * 一个挂载里的所有 inode 都来自同一个后端，逐 inode 挂一份不同的 ops 是指针
 * 开销换来的零收益。（Linux 需要 per-inode ops 是因为同一个目录树里可以混入
 * 不同后端——挂载跨越、overlay、联合挂载。我们暂时不需要那种灵活性，
 * 真需要时再往上加，比一开始就背着一套用不上的间接层要好。）
 *
 * inode 是**不透明句柄**：调用方拿到的 `struct fe_inode *` 只能交给下面这些
 * fe_vfs_* 访问器，不许自己去摸字段。后端把 struct fe_inode 放在自己节点结构的
 * **最前面**，于是两边用的是同一个指针，不需要任何包装分配。
 */
#ifndef FE_VFS_H
#define FE_VFS_H

#include <fe/types.h>
#include <fe/status.h>

#define FE_PATH_MAX     128
#define FE_FS_NAME_MAX  32
#define FE_VFS_MAX_MOUNTS 8

/* 模式位（与 Linux 一致，将来接权限检查时不用做语义翻译） */
#define FE_S_IFREG 0x8000u
#define FE_S_IFDIR 0x4000u
#define FE_S_IXUSR 0x0040u
#define FE_S_IRUSR 0x0100u

/* 不透明 inode 句柄。后端的节点结构必须把它放在第一个成员，
 * 于是两边的指针是同一个，不需要任何包装分配。
 *
 * ops 指向**后端**的操作表：同一后端的所有 inode 共用一张表（静态常量），
 * 每个 inode 只是捎带一个指针。它不是「每个 inode 一套操作」——
 * 我们不需要 Linux 那种同一目录树里混入不同后端的灵活性。 */
struct fe_inode {
    const struct fe_fs_ops *ops;
};

/* 后端操作表。除 name/path 外都可能返回「不支持」（NULL / false）。 */
struct fe_fs_ops {
    const char *fs_name;

    /* 在 dir 里查找一个路径分量。dir 为 NULL 表示取本挂载的根。 */
    struct fe_inode *(*lookup)(struct fe_inode *dir, const char *name);

    /* 目录遍历：prev 为 NULL 表示取第一个子项，返回 NULL 表示到头。 */
    struct fe_inode *(*readdir)(struct fe_inode *dir, struct fe_inode *prev);

    bool (*is_dir)(const struct fe_inode *ino);
    bool (*is_exec)(const struct fe_inode *ino);
    u64  (*size)(const struct fe_inode *ino);

    /* 内容指针。**内存文件系统**可以直接给（ramfs 的模块映像就在内存里）；
     * 需要 I/O 的后端（FAT32）返回 NULL，让上层走将来的 read 接口。
     * 调用方必须处理 NULL——这不是错误，是「这个后端不支持直接取内容」。 */
    const void *(*data)(const struct fe_inode *ino);

    const char *(*name)(const struct fe_inode *ino);
    const char *(*path)(const struct fe_inode *ino);
};

/* 初始化挂载表。必须在任何 mount 之前调用。 */
void fe_vfs_init(void);

/* 把一个后端挂到挂载点。mountpoint 必须是绝对路径（"/" 或 "/mnt" 之类）。
 * root 是该后端自己的根 inode（一般是 ops->lookup(NULL, "") 的返回值）。 */
fe_status_t fe_vfs_mount(const char *mountpoint, const struct fe_fs_ops *ops,
                         struct fe_inode *root);

/* 绝对路径 → inode。不存在、或不是绝对路径、或落在没有挂载的子树里 → NULL。
 *
 * 路径里的 "." 与 ".." 会先做**文本归一化**再走查表，因此 "/a/../b" 等价于 "/b"。
 * 这一手在有符号链接之后就不够了（".." 必须按真实目录走），
 * 到时候再换成走查期处理——现在没有符号链接，文本归一化是正确且最省的。 */
struct fe_inode *fe_vfs_lookup(const char *path);

/* 公共属性访问：调用方只认这些，不碰 ops，也不碰后端的私有结构。 */
bool        fe_vfs_is_dir(const struct fe_inode *ino);
bool        fe_vfs_is_exec(const struct fe_inode *ino);
u64         fe_vfs_size(const struct fe_inode *ino);
const void *fe_vfs_data(const struct fe_inode *ino);
const char *fe_vfs_path(const struct fe_inode *ino);
const char *fe_vfs_name(const struct fe_inode *ino);
struct fe_inode *fe_vfs_readdir(struct fe_inode *dir, struct fe_inode *prev);

/* 诊断：打印挂载表与整棵树 */
void fe_vfs_dump(void);

/* 自检：返回失败项数（0 = 全部通过） */
u32 fe_selftest_fs(void);

#endif /* FE_VFS_H */
