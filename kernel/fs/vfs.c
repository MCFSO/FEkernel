/* SPDX-License-Identifier: 0BSD */
/* VFS 接口层的实现（见 fe/vfs.h 的分层说明）。
 *
 * 这一层刻意很薄：挂载表 + 路径归一化 + 走查 + 属性转发。
 * 任何「文件系统具体怎么存」的知识都不许出现在这里。
 */
#include <fe/vfs.h>
#include <fe/string.h>
#include <fe/kprintf.h>
#include <fe/boot/bootinfo.h>

struct fe_mount {
    char point[FE_PATH_MAX];
    const struct fe_fs_ops *ops;
    struct fe_inode *root;
    bool used;
};

static struct fe_mount g_mounts[FE_VFS_MAX_MOUNTS];

void fe_vfs_init(void)
{
    memset(g_mounts, 0, sizeof(g_mounts));
}

fe_status_t fe_vfs_mount(const char *mountpoint, const struct fe_fs_ops *ops,
                         struct fe_inode *root)
{
    if (!mountpoint || mountpoint[0] != '/' || !ops || !root) {
        return FE_ERR_INVAL;
    }
    if (strlen(mountpoint) >= FE_PATH_MAX) {
        return FE_ERR_NAMETOOLONG;
    }
    for (u32 i = 0; i < FE_VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].used) {
            continue;
        }
        if (strcmp(g_mounts[i].point, mountpoint) == 0) {
            return FE_ERR_EXIST;        /* 同一挂载点不能挂两次 */
        }
    }
    for (u32 i = 0; i < FE_VFS_MAX_MOUNTS; i++) {
        if (g_mounts[i].used) {
            continue;
        }
        strlcpy(g_mounts[i].point, mountpoint, FE_PATH_MAX);
        g_mounts[i].ops = ops;
        g_mounts[i].root = root;
        g_mounts[i].used = true;
        return FE_OK;
    }
    return FE_ERR_NOSPC;
}

/* ------------------------------------------------------------------ */
/* 路径归一化                                                          */
/* ------------------------------------------------------------------ */

/* 就地解析 "." 与 ".."，产出以 '/' 开头、不以 '/' 结尾（根除外）的规范路径。
 * 越出根部的 ".." 被夹在根上（"/.." == "/"），这是 Unix 的既有语义。 */
static void normalize(const char *in, char *out, u32 cap)
{
    u32 n = 0;
    out[n++] = '/';
    out[n] = '\0';
    if (cap < 2) {
        return;
    }

    const char *p = in;
    while (*p) {
        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        char comp[FE_FS_NAME_MAX];
        u32 k = 0;
        while (*p && *p != '/' && k < FE_FS_NAME_MAX - 1) {
            comp[k++] = *p++;
        }
        comp[k] = '\0';
        while (*p && *p != '/') {
            p++;                /* 分量过长：丢弃剩余部分，不越界 */
        }

        if (strcmp(comp, ".") == 0) {
            continue;
        }
        if (strcmp(comp, "..") == 0) {
            /* 退一级：删掉 out 里最后一个分量，但不越过开头那个 '/' */
            while (n > 1 && out[n - 1] != '/') {
                n--;
            }
            if (n > 1) {
                n--;            /* 连分隔符一起删掉 */
            }
            out[n] = '\0';
            continue;
        }
        u32 need = k + ((n > 1) ? 1u : 0u);
        if (n + need >= cap) {
            break;              /* 放不下就截断，宁可查不到也不要写越界 */
        }
        if (n > 1) {
            out[n++] = '/';
        }
        for (u32 i = 0; i < k; i++) {
            out[n++] = comp[i];
        }
        out[n] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/* 走查                                                                */
/* ------------------------------------------------------------------ */

/* 选出**最长**的、按分量对齐的挂载点前缀。
 * 取最长是关键："/mnt" 与 "/mnt/disk" 同时存在时，"/mnt/disk/a" 必须走后者。
 * 「按分量对齐」是指 "/mnt" 不能匹配 "/mntx"——只看字符串前缀会张冠李戴。 */
static const struct fe_mount *pick_mount(const char *path)
{
    const struct fe_mount *best = NULL;
    u32 best_len = 0;

    for (u32 i = 0; i < FE_VFS_MAX_MOUNTS; i++) {
        const struct fe_mount *m = &g_mounts[i];
        if (!m->used) {
            continue;
        }
        u32 n = strlen(m->point);
        if (strncmp(path, m->point, n) != 0) {
            continue;
        }
        bool is_root = (n == 1);        /* "/" */
        if (!is_root && path[n] != '/' && path[n] != '\0') {
            continue;
        }
        if (n > best_len) {
            best = m;
            best_len = n;
        }
    }
    return best;
}

/* 从某个挂载的根出发，按已归一化的路径走到底 */
static struct fe_inode *walk(const struct fe_mount *m, const char *path)
{
    struct fe_inode *cur = m->ops->lookup(NULL, "");    /* 根 */
    if (!cur) {
        return NULL;
    }
    const char *p = path + strlen(m->point);
    while (*p) {
        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        char comp[FE_FS_NAME_MAX];
        u32 k = 0;
        while (*p && *p != '/' && k < FE_FS_NAME_MAX - 1) {
            comp[k++] = *p++;
        }
        comp[k] = '\0';
        while (*p && *p != '/') {
            p++;
        }
        if (!m->ops->is_dir(cur)) {
            return NULL;        /* 不是目录却还有下一段 */
        }
        if (!m->ops->lookup) {
            return NULL;
        }
        struct fe_inode *next = m->ops->lookup(cur, comp);
        if (!next) {
            return NULL;
        }
        cur = next;
    }
    return cur;
}

struct fe_inode *fe_vfs_lookup(const char *path)
{
    if (!path || path[0] != '/') {
        return NULL;
    }
    char norm[FE_PATH_MAX];
    normalize(path, norm, sizeof(norm));

    const struct fe_mount *m = pick_mount(norm);
    if (!m) {
        return NULL;
    }
    return walk(m, norm);
}

/* ------------------------------------------------------------------ */
/* 属性转发                                                            */
/* ------------------------------------------------------------------ */

/* 每个访问器都要挡两种「没有」：没有这个属性（后端没实现）、没有这个 inode。
 * 上层因此永远不需要先判空再调用——少一处判空就少一处漏判。 */

#define OPS_OF(ino) ((ino) ? (ino)->ops : NULL)

bool fe_vfs_is_dir(const struct fe_inode *ino)
{
    const struct fe_fs_ops *o = OPS_OF(ino);
    return (o && o->is_dir) ? o->is_dir(ino) : false;
}

bool fe_vfs_is_exec(const struct fe_inode *ino)
{
    const struct fe_fs_ops *o = OPS_OF(ino);
    return (o && o->is_exec) ? o->is_exec(ino) : false;
}

u64 fe_vfs_size(const struct fe_inode *ino)
{
    const struct fe_fs_ops *o = OPS_OF(ino);
    return (o && o->size) ? o->size(ino) : 0;
}

const void *fe_vfs_data(const struct fe_inode *ino)
{
    const struct fe_fs_ops *o = OPS_OF(ino);
    return (o && o->data) ? o->data(ino) : NULL;
}

const char *fe_vfs_path(const struct fe_inode *ino)
{
    const struct fe_fs_ops *o = OPS_OF(ino);
    return (o && o->path) ? o->path(ino) : "(无路径)";
}

const char *fe_vfs_name(const struct fe_inode *ino)
{
    const struct fe_fs_ops *o = OPS_OF(ino);
    return (o && o->name) ? o->name(ino) : "(无名)";
}

struct fe_inode *fe_vfs_readdir(struct fe_inode *dir, struct fe_inode *prev)
{
    if (!dir) {
        return NULL;
    }
    const struct fe_fs_ops *o = dir->ops;
    if (!o || !o->readdir || !o->is_dir || !o->is_dir(dir)) {
        return NULL;
    }
    return o->readdir(dir, prev);
}

/* ------------------------------------------------------------------ */
/* 诊断与自检                                                          */
/* ------------------------------------------------------------------ */

static void dump_rec(struct fe_inode *dir, u32 depth)
{
    for (struct fe_inode *c = fe_vfs_readdir(dir, NULL); c;
         c = fe_vfs_readdir(dir, c)) {
        fe_kprintf("        ");
        for (u32 i = 0; i < depth; i++) {
            fe_kprintf("  ");
        }
        if (fe_vfs_is_dir(c)) {
            fe_kprintf("%s/\n", fe_vfs_name(c));
        } else {
            fe_kprintf("%s  %llu 字节%s\n", fe_vfs_name(c),
                       (unsigned long long)fe_vfs_size(c),
                       fe_vfs_is_exec(c) ? "  可执行" : "");
        }
        if (fe_vfs_is_dir(c)) {
            dump_rec(c, depth + 1);
        }
    }
}

void fe_vfs_dump(void)
{
    fe_kprintf("  挂载表:\n");
    for (u32 i = 0; i < FE_VFS_MAX_MOUNTS; i++) {
        if (g_mounts[i].used) {
            fe_kprintf("    %-16s ← %s\n", g_mounts[i].point,
                       g_mounts[i].ops->fs_name ? g_mounts[i].ops->fs_name : "?");
        }
    }
    for (u32 i = 0; i < FE_VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].used) {
            continue;
        }
        fe_kprintf("  %s 的内容:\n", g_mounts[i].point);
        dump_rec(g_mounts[i].root, 0);
    }
}

#define CHECK(cond) do { if (!(cond)) { fail++; } } while (0)

u32 fe_selftest_fs(void)
{
    u32 fail = 0;

    /* 这一组测的是**分层本身**：VFS 必须在「后端是谁」这件事上保持无知，
     * 而上层（exec）必须能只靠 VFS 就把程序找出来。 */
    CHECK(pick_mount("/") != NULL);
    CHECK(fe_vfs_lookup("/") != NULL);
    CHECK(fe_vfs_is_dir(fe_vfs_lookup("/")));

    /* 路径归一化：这些等价关系与具体后端无关，是 VFS 自己的职责 */
    CHECK(fe_vfs_lookup("/") == fe_vfs_lookup("//"));
    CHECK(fe_vfs_lookup("/") == fe_vfs_lookup("/."));
    CHECK(fe_vfs_lookup("/") == fe_vfs_lookup("/.."));      /* 越出根被夹住 */
    CHECK(fe_vfs_lookup("/绝对不存在") == NULL);
    CHECK(fe_vfs_lookup("相对路径") == NULL);               /* 只接受绝对路径 */

    /* 每个引导模块都必须能按自己的路径查到，且内容与引导信息一致。
     * 这一条把「模块数组」与「VFS 树」两边钉在一起：
     * 任何一边改了而另一边没改，都会在这里而不是在 exec 时暴露。 */
    const struct fe_boot_info *bi = fe_boot_info();
    u32 checked = 0;
    for (u32 i = 0; i < bi->module_count; i++) {
        const char *p = bi->modules[i].path;
        struct fe_inode *n = fe_vfs_lookup(p);
        if (!n) {
            fe_kprintf("        模块 %s 在 VFS 里查不到\n", p ? p : "(空路径)");
            fail++;
            continue;
        }
        if (fe_vfs_data(n) != bi->modules[i].address ||
            fe_vfs_size(n) != bi->modules[i].size) {
            fe_kprintf("        %s 的内容与引导模块不一致\n", p);
            fail++;
        }
        /* 可执行位必须与**内容**一致：是 ELF 就该有，不是 ELF 就不该有。
         * 原来这里只检查"必须有可执行位"——那是建立在"所有引导模块都是 ELF"
         * 这个当时成立、现在不成立的前提上（/etc/protect.list 是文本清单）。
         * 把前提写进检查里，前提变了检查才会跟着报出来。 */
        {
            const u8 *d = (const u8 *)fe_vfs_data(n);
            u64 sz = fe_vfs_size(n);
            bool is_elf = d && sz >= 4 && d[0] == 0x7F && d[1] == 'E' &&
                          d[2] == 'L' && d[3] == 'F';
            if (is_elf != fe_vfs_is_exec(n)) {
                fe_kprintf("        %s 的可执行位与内容不符（内容%s ELF）\n",
                           p, is_elf ? "是" : "不是");
                fail++;
            }
        }
        /* 归一化必须对真实路径也成立：/./x 与 /x 是同一个 inode */
        checked++;
    }
    CHECK(checked == bi->module_count);

    if (bi->module_count > 0 && bi->modules[0].path) {
        char tricky[FE_PATH_MAX];
        u32 k = 0;
        tricky[k++] = '/';
        tricky[k++] = '.';
        for (const char *s = bi->modules[0].path; *s && k < FE_PATH_MAX - 6; s++) {
            tricky[k++] = *s;
        }
        tricky[k++] = '/';
        tricky[k++] = '.';
        tricky[k++] = '.';
        tricky[k] = '\0';
        CHECK(fe_vfs_lookup(tricky) != NULL);
    }

    /* 目录遍历必须能走通 */
    u32 listed = 0;
    for (struct fe_inode *n = fe_vfs_readdir(fe_vfs_lookup("/"), NULL); n;
         n = fe_vfs_readdir(fe_vfs_lookup("/"), n)) {
        listed++;
    }
    CHECK(listed > 0);

    /* ★ 注意 ramfs 里有什么 ★
     *
     * ramfs 的内容 = **引导模块**，而 Limine 只加载**它启动的那个条目**的模块。
     * 所以这里只有当前槽的文件；另一个槽的文件在磁盘上，要通过 fsd 才看得到。
     * 我第一版在这里断言 /slot_b/init 与 /home 存在——那是在测错层：
     * 前者不在 ramfs 里，后者根本不是模块（它是磁盘上的空目录）。
     *
     * 由此也划出一条有用的界线：
     *   - **启动当前这份系统所需的东西**必须在 ramfs 里（否则起不来）；
     *   - 其它一切（另一个槽、用户数据、不在模块表里的文件）都归磁盘文件系统管。
     */
    int have_init = (fe_vfs_lookup("/slot_a/init") != NULL) ||
                    (fe_vfs_lookup("/slot_b/init") != NULL);
    CHECK(have_init);
    CHECK(fe_vfs_is_exec(fe_vfs_lookup("/slot_a/init")) ||
          fe_vfs_is_exec(fe_vfs_lookup("/slot_b/init")));
    return fail;
}
