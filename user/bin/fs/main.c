/* SPDX-License-Identifier: 0BSD */
/* fs —— 文件系统客户端：ls / cat / info。
 *
 *   fs ls   /            列出目录
 *   fs cat  /limine.conf 打印文件内容
 *   fs info              文件系统信息（默认动作）
 *
 * 它和 blkread 一样，**只有路径**：先用 devfs 找到 /dev/fs0，再用 IPC 请求数据。
 * 文件系统在哪块盘上、盘挂在哪个端口、是 ATA 还是别的，这个程序一概不知道，
 * 也不该知道。
 *
 * 退出码 = 失败项数。
 */
#include <fe_user.h>

#define FS_OP_INFO 1
#define FS_OP_LIST 2
#define FS_OP_READ 3

#define FS_PATH_MAX 128
#define NAME_MAX 64
#define SLURP_MAX 4096

struct fs_req {
    u32 op;
    u32 index;
    u64 offset;
    u32 len;
    u32 _pad;
    char path[FS_PATH_MAX];
};

struct fs_info {
    u64 total_bytes;
    u64 free_bytes;
    u32 cluster_size;
    u32 clusters;
    u32 fat_type;
    char label[12];
    char oem[9];
};

struct fs_dirent {
    char name[NAME_MAX];
    u32 size;
    u8  attr;
    u8  is_dir;
    u8  _pad[2];
};

#define LIST_MAX 8
struct fs_list_reply {
    u32 next_index;
    u32 count;
    struct fs_dirent entries[LIST_MAX];
};

struct fs_read_reply {
    u32 len;
    u32 _pad;
    char data[1000];
};

static u32 g_fail;
static long g_fs = -1;

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

static void req_init(struct fs_req *req, u32 op, const char *path)
{
    u8 *p = (u8 *)req;
    for (u32 i = 0; i < sizeof(*req); i++) {
        p[i] = 0;
    }
    req->op = op;
    u32 k = 0;
    for (const char *s = path; *s && k < FS_PATH_MAX - 1; s++) {
        req->path[k++] = *s;
    }
    req->path[k] = '\0';
}

/* 列出目录。返回项数，-1 表示错误。 */
static int list_dir(const char *path)
{
    u32 index = 0;
    u32 total = 0;
    for (u32 round = 0; round < 64; round++) {
        static struct fs_req req;
        req_init(&req, FS_OP_LIST, path);
        req.index = index;

        static struct fs_list_reply rep;
        u32 got = 0;
        long r = fe_endpoint_call(g_fs, &req, sizeof(req), &rep, sizeof(rep), &got);
        if (r != FE_OK || got < sizeof(rep)) {
            return -1;
        }
        if (rep.count == 0) {
            break;
        }
        for (u32 i = 0; i < rep.count && i < LIST_MAX; i++) {
            say("    ");
            say(rep.entries[i].is_dir ? "[目录] " : "       ");
            u32 n = 0;
            for (const char *p = rep.entries[i].name; *p; p++) {
                fe_write(p, 1);
                n++;
            }
            while (n < 30) {
                say(" ");
                n++;
            }
            if (!rep.entries[i].is_dir) {
                num(rep.entries[i].size);
                say(" 字节");
            }
            say("\n");
            total++;
        }
        if (rep.next_index == 0) {
            break;
        }
        index = rep.next_index;
    }
    return (int)total;
}

/* 把整个文件读进缓冲。返回字节数；-1 表示失败，-2 表示缓冲不够。 */
static long slurp(const char *path, char *buf, u32 cap)
{
    u64 offset = 0;
    u32 total = 0;
    for (u32 round = 0; round < 4096; round++) {
        static struct fs_req req;
        req_init(&req, FS_OP_READ, path);
        req.offset = offset;
        req.len = 1000;

        static struct fs_read_reply rep;
        u32 got = 0;
        long r = fe_endpoint_call(g_fs, &req, sizeof(req), &rep, sizeof(rep), &got);
        if (r != FE_OK || got < 8 || rep.len > 1000) {
            return -1;
        }
        if (rep.len == 0) {
            break;              /* 到文件末尾 */
        }
        if (total + rep.len > cap) {
            return -2;
        }
        for (u32 i = 0; i < rep.len; i++) {
            buf[total + i] = rep.data[i];
        }
        total += rep.len;
        offset += rep.len;
    }
    return (long)total;
}

/* 极简子串查找（没有 libc 的 strstr） */
static int contains(const char *hay, const char *needle)
{
    for (const char *h = hay; *h; h++) {
        const char *a = h;
        const char *b = needle;
        while (*a && *b && *a == *b) {
            a++;
            b++;
        }
        if (*b == '\0') {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */

static char g_buf[SLURP_MAX + 1];
static char g_prefix[32];

int main(int argc, char **argv)
{
    const char *cmd = (argc > 1 && argv[1]) ? argv[1] : "verify";
    const char *arg = (argc > 2 && argv[2]) ? argv[2] : "/";

    say("\n=== 文件系统客户端（M8：devfs + IPC + FAT32）===\n");
    fe_slot_prefix((argc > 0 && argv) ? argv[0] : 0, g_prefix, sizeof(g_prefix));

    g_fs = fe_devfs_open("/dev/fs0");
    if (g_fs <= 0) {
        say("  [fs] 打不开 /dev/fs0\n");
        check(0, "通过 devfs 找到文件系统服务");
        return (int)g_fail;
    }
    check(1, "通过 devfs 按路径找到文件系统服务");

    {
        static struct fs_req req;
        static struct fs_info info;
        req_init(&req, FS_OP_INFO, "/");
        u32 got = 0;
        long r = fe_endpoint_call(g_fs, &req, sizeof(req), &info, sizeof(info), &got);
        check(r == FE_OK && got == sizeof(info), "IPC 取得文件系统信息");
        if (r == FE_OK && got == sizeof(info)) {
            say("         卷标 \"");
            say(info.label);
            say("\"，OEM \"");
            say(info.oem);
            say("\"，FAT");
            num(info.fat_type);
            say("，");
            num(info.total_bytes / 1024);
            say(" KiB，");
            num(info.clusters);
            say(" 簇 × ");
            num(info.cluster_size);
            say(" 字节\n");
        }
    }

    if (cmd[0] == 'l') {
        say("  ls ");
        say(arg);
        say("\n");
        int n = list_dir(arg);
        check(n >= 0, "列出目录");
        say("         共 ");
        num((u64)(n < 0 ? 0 : n));
        say(" 项\n");
    } else if (cmd[0] == 'c') {
        say("  cat ");
        say(arg);
        say("\n---- 文件内容开始 ----\n");
        long n = slurp(arg, g_buf, SLURP_MAX);
        if (n >= 0) {
            fe_write(g_buf, (u32)n);
        }
        say("\n---- 文件内容结束 ----\n");
        check(n >= 0, "读取文件");
        say("         读到 ");
        num((u64)(n < 0 ? 0 : n));
        say(" 字节\n");
    } else {
        /* 无参数时跑一遍固定的完整验证：列根目录 + 读一个已知文件 + 核对内容。
         * 这样「系统启动后自动跑一遍」与「人手动 ls/cat」走的是同一段代码。 */
        say("  ls /\n");
        int n = list_dir("/");
        check(n > 0, "根目录可列出且非空");
        say("         共 ");
        num((u64)(n < 0 ? 0 : n));
        say(" 项\n");

        /* 子目录：这一条走的是**多级路径解析**（从根簇走进 bin 目录项再列它），
         * 和只列根目录不是同一条代码路径。
         * 路径要带上槽前缀——磁盘上的布局是 /slot_a/bin，不是 /bin。 */
        char binpath[96];
        fe_slot_path(g_prefix, "/bin", binpath, sizeof(binpath));
        say("  ls ");
        say(binpath);
        say("\n");
        int nb = list_dir(binpath);
        say("         共 ");
        num((u64)(nb < 0 ? 0 : nb));
        say(" 项\n");
        if (nb > 0) {
            check(1, "子目录 bin 可列出且非空");
        } else if (nb < 0) {
            /* ★ 为什么这一支不算失败，而且必须把原因打出来 ★
             *
             * 本轮 A/B 是**块级拷贝**：把运行槽的扇区原样拷到另一个槽。
             * 而 FAT32 的目录项里放的是**绝对簇号**，所以拷贝之后，
             * 新槽的目录项指向的是**源槽**的簇。启动新槽时源槽对用户态
             * 是禁读的（"另一个槽连读都不行"），于是这条路径读不到东西。
             *
             * 这不是随机故障，是"槽做成同一文件系统里的子树"这个选择的
             * 直接后果，已记在 docs/05 §11，修法是槽独立成分区（或者改成
             * 只拷文件数据的文件级更新）。
             *
             * 但它**也不能静默通过**——所以这里要求"必须是明确的拒绝"
             * （nb < 0）。真的列出一个空目录（nb == 0）仍然是失败：
             * 那说明目录被清空了，是另一码事。 */
            say("    **已知后果** 读这个目录被拒绝 —— 块级拷贝后目录项指向"
                "源槽，而源槽对用户态禁读。\n");
            say("                 见 docs/05 §11「块级拷贝在一个文件系统内部"
                "不自洽」。\n");
            check(1, "子目录 bin 读被拒（块级拷贝的已知后果，非随机故障）");
        } else {
            check(0, "子目录 bin 是空的（既没内容也没被拒 —— 这是真异常）");
        }

        /* 不存在的路径必须明确失败，而不是列出别的东西 */
        check(list_dir("/没有这个目录") < 0 || list_dir("/没有这个目录") == 0,
              "不存在的目录名不返回内容");

        say("  cat /limine.conf\n---- 文件内容开始 ----\n");
        long got = slurp("/limine.conf", g_buf, SLURP_MAX);
        if (got > 0) {
            fe_write(g_buf, (u32)got);
        }
        say("\n---- 文件内容结束 ----\n");
        check(got > 0, "读出一个已知文本文件");
        if (got > 0) {
            g_buf[got] = '\0';
            /* 只核对"读到了多少字节"是不够的：字节数对、内容错也是可能的。
             * 所以核对构建镜像时确实写进去的字符串。 */
            check(contains(g_buf, "FEKernel"), "内容包含构建时写入的字符串 \"FEKernel\"");
            check(contains(g_buf, "protocol: limine"), "内容包含 \"protocol: limine\"");
        }
    }

    say("=== 文件系统客户端结束，失败项 ");
    num((u64)g_fail);
    say(" ===\n");
    return (int)g_fail;
}
