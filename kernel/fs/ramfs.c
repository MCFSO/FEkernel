/* SPDX-License-Identifier: 0BSD */
/* ramfs —— 由引导模块构成的内存文件系统（initramfs 等价物），**VFS 的一个后端**。
 *
 * 它现在只做后端该做的事：维护自己的目录树、实现一组 inode 操作。
 * 路径解析、挂载、"." / ".." 归一化全部归 VFS —— 这个文件里没有一行
 * 「这条路径怎么写」的逻辑，将来换成 FAT32 时也不需要。
 *
 * 数据结构是「父/子/兄弟」三叉链，节点来自定长静态池。刻意的选择：
 * 只读、无分配、无锁。引导模块数量在编译期就有上限（FE_BOOTINFO_MAX_MODULES），
 * 路径分量也不会无限深，所以这里根本不需要动态分配——
 * 也就不存在「构建根文件系统时内存不足」这种失败路径。
 */
#include <fe/vfs.h>
#include <fe/ramfs.h>
#include <fe/string.h>
#include <fe/kprintf.h>
#include <fe/boot/bootinfo.h>

#define FE_RAMFS_MAX_NODES 128

/* 节点结构：struct fe_inode **必须放在最前面**，VFS 拿到的句柄就是它。
 * 这样两边共用一个指针，既不需要包装分配，也不需要 priv 字段。 */
struct ramfs_node {
    struct fe_inode ino;
    char name[FE_FS_NAME_MAX];
    char path[FE_PATH_MAX];
    struct ramfs_node *parent;
    struct ramfs_node *first_child;
    struct ramfs_node *next_sibling;
    const void *data;
    u64 size;
    u32 mode;
};

static struct ramfs_node g_nodes[FE_RAMFS_MAX_NODES];
static u32 g_node_count;
static struct ramfs_node *g_root;

FE_STATIC_ASSERT(sizeof(struct fe_inode) <= sizeof(struct ramfs_node),
                 "ramfs 节点必须能容纳 VFS 的 inode 头");
FE_STATIC_ASSERT(__builtin_offsetof(struct ramfs_node, ino) == 0,
                 "struct fe_inode 必须位于 ramfs 节点结构的最前面");

/* ------------------------------------------------------------------ */
/* 树构造（只在初始化时用）                                            */
/* ------------------------------------------------------------------ */

/* 按名字升序插入，好让目录遍历的输出稳定（自检与文档引用它） */
static void link_child(struct ramfs_node *dir, struct ramfs_node *node)
{
    node->parent = dir;
    struct ramfs_node **pp = &dir->first_child;
    while (*pp && strcmp((*pp)->name, node->name) < 0) {
        pp = &(*pp)->next_sibling;
    }
    node->next_sibling = *pp;
    *pp = node;
}

static struct ramfs_node *node_alloc(const char *name, u32 mode)
{
    if (g_node_count >= FE_RAMFS_MAX_NODES) {
        return NULL;
    }
    struct ramfs_node *n = &g_nodes[g_node_count++];
    memset(n, 0, sizeof(*n));
    strlcpy(n->name, name, FE_FS_NAME_MAX);
    n->mode = mode;
    return n;
}

static struct ramfs_node *node_find_child(struct ramfs_node *dir, const char *name)
{
    for (struct ramfs_node *c = dir->first_child; c; c = c->next_sibling) {
        if (strcmp(c->name, name) == 0) {
            return c;
        }
    }
    return NULL;
}

/* 依次创建路径上的中间目录（mkdir -p 语义），返回最后一级目录 */
static struct ramfs_node *node_mkdir_p(struct ramfs_node *dir, const char *name)
{
    struct ramfs_node *c = node_find_child(dir, name);
    if (c) {
        return (c->mode & FE_S_IFDIR) ? c : NULL;
    }
    struct ramfs_node *n = node_alloc(name, FE_S_IFDIR | 0755u);
    if (!n) {
        return NULL;
    }
    link_child(dir, n);
    return n;
}

/* 递归填好 path 字段。后端自己维护完整路径是为了诊断——
 * VFS 不需要它，但出问题时日志里能看到 "/sbin/kbd" 而不是 "kbd"，
 * 值回这点代码。 */
static void fill_paths(struct ramfs_node *n)
{
    for (struct ramfs_node *c = n->first_child; c; c = c->next_sibling) {
        u32 k = 0;
        if (n->parent == NULL) {
            c->path[k++] = '/';
        } else {
            for (const char *s = n->path; *s && k < FE_PATH_MAX - 1; s++) {
                c->path[k++] = *s;
            }
            if (k + 1 < FE_PATH_MAX) {
                c->path[k++] = '/';
            }
        }
        for (const char *s = c->name; *s && k < FE_PATH_MAX - 1; s++) {
            c->path[k++] = *s;
        }
        c->path[k] = '\0';
        fill_paths(c);
    }
}

/* 可执行位判定与 Linux 的 binfmt 一致：看内容是不是 ELF，而不是看扩展名。 */
static bool looks_like_elf(const void *p, u64 size)
{
    if (!p || size < 4) {
        return false;
    }
    const u8 *b = (const u8 *)p;
    return b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F';
}

/* 把一条绝对路径（如 /sbin/hello）变成树里的一张叶子 */
static void add_module(const char *path, const void *data, u64 size)
{
    if (!path || path[0] != '/' || !g_root) {
        return;
    }
    struct ramfs_node *dir = g_root;
    const char *p = path;
    char comp[FE_FS_NAME_MAX];

    while (*p) {
        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        u32 k = 0;
        while (*p && *p != '/' && k < FE_FS_NAME_MAX - 1) {
            comp[k++] = *p++;
        }
        comp[k] = '\0';
        while (*p && *p != '/') {
            p++;        /* 分量过长时丢弃剩余部分，而不是写越界 */
        }
        if (*p == '\0') {
            struct ramfs_node *f = node_alloc(comp, FE_S_IFREG | 0644u);
            if (!f) {
                return;
            }
            f->data = data;
            f->size = size;
            if (looks_like_elf(data, size)) {
                f->mode |= FE_S_IXUSR;
            }
            link_child(dir, f);
            return;
        }
        struct ramfs_node *next = node_mkdir_p(dir, comp);
        if (!next) {
            return;
        }
        dir = next;
    }
}

/* ------------------------------------------------------------------ */
/* 后端操作表                                                          */
/* ------------------------------------------------------------------ */

#define NODE(ino) ((struct ramfs_node *)(void *)(ino))

static struct fe_inode *ramfs_lookup(struct fe_inode *dir, const char *name)
{
    if (!dir) {
        return g_root ? &g_root->ino : NULL;    /* NULL 目录 = 取本后端的根 */
    }
    if (!(NODE(dir)->mode & FE_S_IFDIR) || !name) {
        return NULL;
    }
    struct ramfs_node *c = node_find_child(NODE(dir), name);
    return c ? &c->ino : NULL;
}

static struct fe_inode *ramfs_readdir(struct fe_inode *dir, struct fe_inode *prev)
{
    struct ramfs_node *d = NODE(dir);
    if (!d || !(d->mode & FE_S_IFDIR)) {
        return NULL;
    }
    struct ramfs_node *c = prev ? NODE(prev)->next_sibling : d->first_child;
    return c ? &c->ino : NULL;
}

static bool ramfs_is_dir(const struct fe_inode *ino)
{
    return (NODE(ino)->mode & FE_S_IFDIR) != 0;
}

static bool ramfs_is_exec(const struct fe_inode *ino)
{
    u32 m = NODE(ino)->mode;
    return (m & FE_S_IFREG) != 0 && (m & FE_S_IXUSR) != 0;
}

static u64 ramfs_size(const struct fe_inode *ino)
{
    return NODE(ino)->size;
}

static const void *ramfs_data(const struct fe_inode *ino)
{
    return NODE(ino)->data;
}

static const char *ramfs_name(const struct fe_inode *ino)
{
    return NODE(ino)->name;
}

static const char *ramfs_path(const struct fe_inode *ino)
{
    return NODE(ino)->path;
}

static const struct fe_fs_ops g_ramfs_ops = {
    .fs_name = "ramfs（引导模块）",
    .lookup = ramfs_lookup,
    .readdir = ramfs_readdir,
    .is_dir = ramfs_is_dir,
    .is_exec = ramfs_is_exec,
    .size = ramfs_size,
    .data = ramfs_data,
    .name = ramfs_name,
    .path = ramfs_path,
};

/* ------------------------------------------------------------------ */
/* 初始化：建树 + 把自己挂到 "/"                                       */
/* ------------------------------------------------------------------ */

fe_status_t fe_ramfs_init(void)
{
    memset(g_nodes, 0, sizeof(g_nodes));
    g_node_count = 0;

    g_root = node_alloc("", FE_S_IFDIR | 0755u);
    if (!g_root) {
        return FE_ERR_NOMEM;
    }
    strlcpy(g_root->path, "/", FE_PATH_MAX);

    const struct fe_boot_info *bi = fe_boot_info();
    for (u32 i = 0; i < bi->module_count; i++) {
        add_module(bi->modules[i].path, bi->modules[i].address, bi->modules[i].size);
    }
    fill_paths(g_root);

    /* 整棵树建好后统一填 ops：这样 node_alloc 不必依赖静态初始化顺序。 */
    for (u32 i = 0; i < g_node_count; i++) {
        g_nodes[i].ino.ops = &g_ramfs_ops;
    }

    return fe_vfs_mount("/", &g_ramfs_ops, &g_root->ino);
}
