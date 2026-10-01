/* SPDX-License-Identifier: 0BSD */
/* devfs 的实现（设计说明见 fe/devfs.h）。
 *
 * 数据结构是一个定长表：每一条是一个已发布的名字 + 它背后端点的引用。
 * 没有目录层级（/dev 下面一层，够用）——真需要 /dev/pci/0000:00:1f.2
 * 那种层级时，再给条目加一个 parent 指针即可，不影响现在的接口。
 */
#include <fe/devfs.h>
#include <fe/vfs.h>
#include <fe/ipc.h>
#include <fe/object.h>
#include <fe/task.h>
#include <fe/string.h>
#include <fe/kprintf.h>

struct devfs_entry {
    struct fe_inode ino;                /* 必须在最前面：VFS 句柄就是它 */
    char name[FE_FS_NAME_MAX];
    char path[FE_PATH_MAX];
    struct fe_endpoint *ep;             /* devfs 持有引用 */
    u64 owner;                          /* 发布者任务 id */
    /* ★ 发布时发布方持有的权限 ★ —— 打开者拿到的能力以它为上界。
     *
     * 为什么必须存下来：不存的话，任何按名字打开的人都拿到
     * SEND|RECV|DUP|TRANSFER 全权限，于是"发布时只给收"这个动作是**假的**：
     * 客户端照样能从这个端点收消息，把本该属于服务端的请求抢走。
     * 有了它，发布方给的权限就是打开方能用到的全部（只能更窄）。 */
    u32 rights;
    bool used;
};

/* 根目录自己也是一个 inode（/dev 这个目录项） */
struct devfs_root {
    struct fe_inode ino;
    char path[FE_PATH_MAX];
};

static struct devfs_entry g_entries[FE_DEVFS_MAX_ENTRIES];
static struct devfs_root  g_root;

#define ENTRY(ino) ((struct devfs_entry *)(void *)(ino))

FE_STATIC_ASSERT(__builtin_offsetof(struct devfs_entry, ino) == 0,
                 "struct fe_inode 必须位于 devfs 条目结构的最前面");
FE_STATIC_ASSERT(__builtin_offsetof(struct devfs_root, ino) == 0,
                 "struct fe_inode 必须位于 devfs 根结构的最前面");

/* ------------------------------------------------------------------ */
/* 后端操作表                                                          */
/* ------------------------------------------------------------------ */

static struct fe_inode *devfs_lookup(struct fe_inode *dir, const char *name)
{
    if (!dir) {
        return &g_root.ino;             /* NULL 目录 = 取本后端的根（即 /dev） */
    }
    if (dir != &g_root.ino || !name) {
        return NULL;
    }
    for (u32 i = 0; i < FE_DEVFS_MAX_ENTRIES; i++) {
        if (g_entries[i].used && strcmp(g_entries[i].name, name) == 0) {
            return &g_entries[i].ino;
        }
    }
    return NULL;
}

static struct fe_inode *devfs_readdir(struct fe_inode *dir, struct fe_inode *prev)
{
    if (dir != &g_root.ino) {
        return NULL;
    }
    /* prev 为 NULL 时从表头开始；否则从它后面一条继续 */
    u32 start = 0;
    if (prev) {
        struct devfs_entry *p = ENTRY(prev);
        start = (u32)(p - g_entries) + 1;
    }
    for (u32 i = start; i < FE_DEVFS_MAX_ENTRIES; i++) {
        if (g_entries[i].used) {
            return &g_entries[i].ino;
        }
    }
    return NULL;
}

static bool devfs_is_dir(const struct fe_inode *ino)
{
    return ino == &g_root.ino;          /* 只有 /dev 自己是目录 */
}

static bool devfs_is_exec(const struct fe_inode *ino)
{
    (void)ino;
    return false;                       /* 设备节点不是可执行文件 */
}

static u64 devfs_size(const struct fe_inode *ino)
{
    (void)ino;
    return 0;
}

static const void *devfs_data(const struct fe_inode *ino)
{
    /* 关键的一处「不支持」：设备节点没有内存内容可给。
     * VFS 契约允许后端返回 NULL，上层必须处理——
     * 这正是 VFS 存在的意义：exec 路径看到 NULL 就知道「这个文件不能直接取内容」，
     * 而不是去摸一个它不该知道的字段。 */
    (void)ino;
    return NULL;
}

static const char *devfs_name(const struct fe_inode *ino)
{
    if (ino == &g_root.ino) {
        return "dev";
    }
    return ENTRY(ino)->name;
}

static const char *devfs_path(const struct fe_inode *ino)
{
    if (ino == &g_root.ino) {
        return g_root.path;
    }
    return ENTRY(ino)->path;
}

static const struct fe_fs_ops g_devfs_ops = {
    .fs_name = "devfs（服务命名空间）",
    .lookup = devfs_lookup,
    .readdir = devfs_readdir,
    .is_dir = devfs_is_dir,
    .is_exec = devfs_is_exec,
    .size = devfs_size,
    .data = devfs_data,
    .name = devfs_name,
    .path = devfs_path,
};

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */

fe_status_t fe_devfs_init(void)
{
    memset(g_entries, 0, sizeof(g_entries));
    memset(&g_root, 0, sizeof(g_root));
    g_root.ino.ops = &g_devfs_ops;
    strlcpy(g_root.path, FE_DEVFS_MOUNT_POINT, FE_PATH_MAX);
    return fe_vfs_mount(FE_DEVFS_MOUNT_POINT, &g_devfs_ops, &g_root.ino);
}

/* ------------------------------------------------------------------ */
/* 发布与打开                                                          */
/* ------------------------------------------------------------------ */

/* 把 /dev/xxx 拆成 "xxx"。只接受**恰好一层**：/dev/a/b 一律拒绝，
 * 宁可现在报错，也不要让「路径看着能用、其实匹配不上」这种半吊子状态存在。 */
static bool split_dev_path(const char *path, char *name_out)
{
    const char *p = FE_DEVFS_MOUNT_POINT;
    if (strncmp(path, p, strlen(p)) != 0) {
        return false;
    }
    const char *rest = path + strlen(p);
    if (*rest != '/') {
        return false;
    }
    rest++;
    if (*rest == '\0') {
        return false;                   /* "/dev/" 本身不是设备名 */
    }
    u32 k = 0;
    for (; *rest; rest++) {
        if (*rest == '/') {
            return false;               /* 不接受更深的层级 */
        }
        if (k >= FE_FS_NAME_MAX - 1) {
            return false;
        }
        name_out[k++] = *rest;
    }
    name_out[k] = '\0';
    return true;
}

fe_status_t fe_devfs_publish(struct fe_task *owner, const char *path,
                             fe_handle_t endpoint)
{
    if (!owner || !path) {
        return FE_ERR_INVAL;
    }
    char name[FE_FS_NAME_MAX];
    if (!split_dev_path(path, name)) {
        return FE_ERR_INVAL;
    }

    /* 端点必须真的在这个任务的句柄表里，而且带「可转交」权限。
     * 能把它交出去是一种权限，不是默认就有的。 */
    struct fe_object_header *obj = NULL;
    fe_status_t s = fe_handle_lookup(&owner->handles, endpoint,
                                     FE_RIGHT_TRANSFER, &obj);
    if (fe_failed(s)) {
        return s;
    }
    if (obj->type != FE_OBJ_ENDPOINT) {
        return FE_ERR_INVAL;
    }

    /* 同名不能重复发布：否则任何服务都能抢注 /dev/blk0 把真服务顶掉 */
    for (u32 i = 0; i < FE_DEVFS_MAX_ENTRIES; i++) {
        if (g_entries[i].used && strcmp(g_entries[i].name, name) == 0) {
            return FE_ERR_EXIST;
        }
    }
    for (u32 i = 0; i < FE_DEVFS_MAX_ENTRIES; i++) {
        struct devfs_entry *e = &g_entries[i];
        if (e->used) {
            continue;
        }
        memset(e, 0, sizeof(*e));
        e->ino.ops = &g_devfs_ops;
        strlcpy(e->name, name, FE_FS_NAME_MAX);
        strlcpy(e->path, path, FE_PATH_MAX);
        e->ep = FE_OBJ_OF(obj, struct fe_endpoint);
        fe_object_ref(&e->ep->hdr);     /* devfs 自己持一份引用 */
        e->owner = owner->id;
        /* 记下发布方**当时**持有的权限：打开者能拿到的不会比它多。
         * 记快照而不是实时查发布方句柄表：发布方可能已经关掉那个句柄
         * （甚至退出），而名字还在、别人还要打开它。 */
        {
            u32 rights = 0;
            if (fe_failed(fe_handle_rights(&owner->handles, endpoint, &rights))) {
                rights = FE_RIGHT_ALL;
            }
            e->rights = rights;
        }
        e->used = true;
        return FE_OK;
    }
    return FE_ERR_NOSPC;
}

fe_status_t fe_devfs_open(struct fe_task *opener, const char *path,
                          fe_handle_t *out_handle)
{
    if (!opener || !path || !out_handle) {
        return FE_ERR_INVAL;
    }
    char name[FE_FS_NAME_MAX];
    if (!split_dev_path(path, name)) {
        return FE_ERR_INVAL;
    }
    struct devfs_entry *e = NULL;
    for (u32 i = 0; i < FE_DEVFS_MAX_ENTRIES; i++) {
        if (g_entries[i].used && strcmp(g_entries[i].name, name) == 0) {
            e = &g_entries[i];
            break;
        }
    }
    if (!e) {
        return FE_ERR_NOENT;
    }
    /* 装进打开者的句柄表：它拿到的是一条通往该服务的通道。
     *
     * ★ 权限 = 发布方权限 ∩ 客户端可用集合 ★
     * 「发布时收窄」必须在这里生效，否则它是假的：只给 RECV 发布的端点，
     * 任何人打开都能发/都能收，服务端赖以区分"谁在跟我说话"的那条线就没了
     * （更糟的是别人能从它那里收走本该属于服务端的请求）。
     * 客户端可能需要 DUP/TRANSFER 才能把这条能力转交或复制，所以它们仍在
     * 集合里；但只要发布方没给，就拿不到。 */
    u32 rights = e->rights & (FE_RIGHT_SEND | FE_RIGHT_RECV |
                              FE_RIGHT_DUP | FE_RIGHT_TRANSFER);
    fe_handle_t h = fe_handle_install(&opener->handles, &e->ep->hdr, rights);
    if (h == FE_HANDLE_INVALID) {
        return FE_ERR_NOMEM;
    }
    *out_handle = h;
    return FE_OK;
}

void fe_devfs_release_owner(u64 owner_task_id)
{
    if (owner_task_id == 0) {
        return;
    }
    for (u32 i = 0; i < FE_DEVFS_MAX_ENTRIES; i++) {
        struct devfs_entry *e = &g_entries[i];
        if (!e->used || e->owner != owner_task_id) {
            continue;
        }
        /* 先放引用再清条目：反过来的话，条目已经不指向任何东西了，
         * 而引用还挂着，那个端点就永远回收不掉。 */
        struct fe_endpoint *ep = e->ep;
        memset(e, 0, sizeof(*e));
        if (ep) {
            fe_object_unref(&ep->hdr);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 诊断与自检                                                          */
/* ------------------------------------------------------------------ */

void fe_devfs_dump(void)
{
    fe_kprintf("  %s 下的服务（%s）:\n", FE_DEVFS_MOUNT_POINT, g_devfs_ops.fs_name);
    u32 n = 0;
    for (u32 i = 0; i < FE_DEVFS_MAX_ENTRIES; i++) {
        if (!g_entries[i].used) {
            continue;
        }
        fe_kprintf("    %-20s 发布者任务 %llu\n", g_entries[i].path,
                   (unsigned long long)g_entries[i].owner);
        n++;
    }
    if (n == 0) {
        fe_kprintf("    （暂无）\n");
    }
}

#define CHECK(cond) do { if (!(cond)) { fail++; } } while (0)

/* 自检用一个临时的内核端点和一个临时的发布者任务，不碰真实服务。
 * 这样它可以随时跑，不会因为「blkd 还没起来」而失败——自检不该依赖运行期状态。 */
u32 fe_selftest_devfs(void)
{
    u32 fail = 0;

    /* 1. /dev 是 VFS 里一个正常的目录，走的是同一套路径解析 */
    struct fe_inode *dev = fe_vfs_lookup(FE_DEVFS_MOUNT_POINT);
    CHECK(dev != NULL);
    CHECK(fe_vfs_is_dir(dev));
    CHECK(fe_vfs_lookup("/dev/绝对不存在") == NULL);

    /* 2. 设备节点没有内存内容 —— VFS 契约允许，上层必须处理 NULL */
    CHECK(fe_vfs_data(dev) == NULL);

    /* 3. 造一个内核任务与端点来试发布/打开 */
    struct fe_task *pub = fe_task_create_kernel("devfs-probe");
    struct fe_task *cli = fe_task_create_kernel("devfs-client");
    if (!pub || !cli) {
        fe_kprintf("        探针任务创建失败\n");
        return fail + 1;
    }
    fe_handle_t ep = FE_HANDLE_INVALID;
    CHECK(fe_ok(fe_endpoint_create(pub, 0, &ep)));
    CHECK(ep != FE_HANDLE_INVALID);

    /* 路径形状的拒绝：这些都不该被接受 */
    CHECK(fe_devfs_publish(pub, "dev/probe", ep) == FE_ERR_INVAL);      /* 不绝对 */
    CHECK(fe_devfs_publish(pub, "/dev", ep) == FE_ERR_INVAL);           /* 没有名字 */
    CHECK(fe_devfs_publish(pub, "/dev/", ep) == FE_ERR_INVAL);
    CHECK(fe_devfs_publish(pub, "/dev/a/b", ep) == FE_ERR_INVAL);       /* 层级过深 */
    CHECK(fe_devfs_publish(pub, "/mnt/probe", ep) == FE_ERR_INVAL);     /* 挂载点不对 */

    /* 正常发布 */
    CHECK(fe_ok(fe_devfs_publish(pub, "/dev/probe0", ep)));
    /* 同名再发布必须被拒 —— 这是「服务不能被顶替」的那条性质 */
    struct fe_task *pub2 = fe_task_create_kernel("devfs-thief");
    fe_handle_t ep2 = FE_HANDLE_INVALID;
    if (pub2 && fe_ok(fe_endpoint_create(pub2, 0, &ep2))) {
        CHECK(fe_devfs_publish(pub2, "/dev/probe0", ep2) == FE_ERR_EXIST);
    }

    /* 客户端按路径打开：拿到的是句柄，不是端点对象 */
    fe_handle_t got = FE_HANDLE_INVALID;
    CHECK(fe_ok(fe_devfs_open(cli, "/dev/probe0", &got)));
    CHECK(got != FE_HANDLE_INVALID);
    CHECK(fe_devfs_open(cli, "/dev/nothere", &got) == FE_ERR_NOENT);

    /* 打开者必须真的能用这个句柄（权限装对了） */
    struct fe_object_header *obj = NULL;
    CHECK(fe_ok(fe_handle_lookup(&cli->handles, got, FE_RIGHT_SEND, &obj)));
    CHECK(obj && obj->type == FE_OBJ_ENDPOINT);

    /* ★ 发布时收窄 = 打开时收窄 ★
     *
     * 负向对照：把句柄收窄成"只能收"再发布，客户端打开之后
     * **必须不能发**。没有这一条，"发布时收窄"就只是发布方那边的装饰：
     * 任何人按名字打开都拿到全权限，服务端赖以区分调用者的那条线就没了。 */
    {
        fe_handle_t recv_only = FE_HANDLE_INVALID;
        CHECK(fe_ok(fe_handle_dup(&pub->handles, ep,
                                  FE_RIGHT_RECV | FE_RIGHT_TRANSFER, &recv_only)));
        CHECK(fe_ok(fe_devfs_publish(pub, "/dev/probe1", recv_only)));
        fe_handle_close(&pub->handles, recv_only);

        fe_handle_t got1 = FE_HANDLE_INVALID;
        CHECK(fe_ok(fe_devfs_open(cli, "/dev/probe1", &got1)));
        CHECK(fe_handle_lookup(&cli->handles, got1, FE_RIGHT_SEND, NULL) == FE_ERR_ACCESS);
        CHECK(fe_ok(fe_handle_lookup(&cli->handles, got1, FE_RIGHT_RECV, NULL)));
        fe_handle_close(&cli->handles, got1);
        /* probe1 的释放留给下面统一的 release_owner（它会清掉这个任务的全部名字） */
    }

    /* VFS 也必须能看见它（devfs 是 VFS 后端，不是平行的表） */
    struct fe_inode *node = fe_vfs_lookup("/dev/probe0");
    CHECK(node != NULL);
    CHECK(!fe_vfs_is_dir(node));
    CHECK(strcmp(fe_vfs_name(node), "probe0") == 0);

    /* 收尾：发布者销毁时名字要跟着走 */
    fe_handle_close(&cli->handles, got);
    fe_devfs_release_owner(pub->id);
    CHECK(fe_vfs_lookup("/dev/probe0") == NULL);
    if (pub2) {
        fe_object_unref(&pub2->hdr);
    }
    fe_object_unref(&pub->hdr);
    fe_object_unref(&cli->hdr);
    return fail;
}
