/* SPDX-License-Identifier: 0BSD */
/* sh —— 交互式 shell。
 *
 * 它是这个系统上**第一个真正的应用**：在此之前所有用户态程序都是自检或服务，
 * 没有一个"给人用"的东西。有了它，"文件能不能写"这种问题可以手打一条命令来验，
 * 不必再写一次性测试程序——**验证工具本身也是一种基础设施**。
 *
 * 分工（这条边界是整个交互面的关键）：
 *   - **行编辑、回显、光标**在 consoled 里（它拥有屏幕）；
 *   - **命令解析、执行**在这里。
 * 所以 sh 读到的永远是完整的一行，它不需要知道任何按键的事，
 * 也不会与控制台的渲染打架。
 *
 * 命令形式故意简单（空白分隔，不支持引号与重定向）：
 * 这一版的目标是"能用"，不是"能跑脚本"。管道的价值要等到有多个
 * 值得串联的程序时才体现，现在加进来只是给自己找活干。
 */
#include <fe_user.h>
#include <fe_console.h>
#include <fe_fs.h>

#define LINE_CAP 256
#define ARG_MAX  8

static long g_con = -1;
static long g_fs = -1;
static char g_prefix[32];       /* 槽前缀，如 "/slot_a" */
static u32  g_fail;

static void out(const char *s)
{
    /* 控制台单次写入有载荷上限，所以分块发。
     * ★ 不能直接 fe_con_write 一次了事 ★ 它在 512 字节处**静默截断**，
     * 而 `cat` 一个稍大的文件就会超过——症状是"文件明明有内容却只打出一半"。 */
    while (*s) {
        char chunk[480];
        u32 n = 0;
        while (s[n] && n < sizeof(chunk) - 1) {
            chunk[n] = s[n];
            n++;
        }
        chunk[n] = '\0';
        fe_con_write(g_con, chunk);
        s += n;
    }
}

static void out_num(u64 v)
{
    char b[24];
    u32 n = 0;
    char rev[24];
    u32 r = 0;
    if (v == 0) {
        out("0");
        return;
    }
    while (v && r < sizeof(rev)) {
        rev[r++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (r) {
        b[n++] = rev[--r];
    }
    b[n] = '\0';
    out(b);
}

/* ---- 命令 ---- */

static const char *fs_err(int st)
{
    switch (st) {
    case FE_FS_OK:     return "ok";
    case FE_FS_ENOENT: return "no such file or directory";
    case FE_FS_ENAME:  return "bad name (only 8.3 names can be created)";
    case FE_FS_EIO:    return "I/O error";
    case FE_FS_EFLUSH: return "flush failed";
    case FE_FS_EEMPTY: return "empty write";
    case FE_FS_EGAP:   return "cannot write past end of file (no holes)";
    case FE_FS_ENOSPC: return "no space left";
    case FE_FS_EISDIR: return "is a directory";
    case FE_FS_EDENIED: return "blocked: the running slot is write-protected (use /home)";
    default:           return "failed";
    }
}

/* 把一段文字写进文件（等价于 `echo text > path`）。 */
static void write_file(const char *path, const char *text, int append)
{
    if (g_fs <= 0) {
        out("write: no filesystem\n");
        g_fail++;
        return;
    }
    u64 off = 0;
    int st;
    if (append) {
        struct fe_fs_read_reply probe;
        /* 追加：先问出当前大小。用一个 0 字节读拿不到大小，
         * 所以直接读到末尾把偏移累出来——文件不大时这样最简单可靠。 */
        off = 0;
        for (u32 i = 0; i < 64; i++) {
            int n = fe_fs_read(g_fs, path, off, &probe);
            if (n <= 0) {
                break;
            }
            off += (u64)n;
        }
    } else {
        st = fe_fs_create(g_fs, path);
        if (st != FE_FS_OK) {
            out("write: "); out(fs_err(st)); out("\n");
            g_fail++;
            return;
        }
    }

    u32 len = 0;
    while (text[len]) {
        len++;
    }
    if (len == 0) {
        return;
    }
    u32 done = 0;
    while (done < len) {
        u32 chunk = len - done;
        if (chunk > 480) {
            chunk = 480;        /* 留出请求头的空间，别顶到载荷上限 */
        }
        u32 wrote = 0;
        st = fe_fs_write(g_fs, path, off + done, text + done, chunk, &wrote);
        if (st != FE_FS_OK) {
            out("write: "); out(fs_err(st)); out("\n");
            g_fail++;
            return;
        }
        if (wrote == 0) {
            out("write: short write\n");
            g_fail++;
            return;
        }
        done += wrote;
    }
    out("wrote ");
    out_num(done);
    out(" bytes to ");
    out(path);
    out("\n");
}

static void cmd_rm(const char *path)
{
    if (g_fs <= 0) {
        out("rm: no filesystem\n");
        return;
    }
    if (!path || !*path) {
        out("rm: missing path\n");
        return;
    }
    int st = fe_fs_unlink(g_fs, path);
    if (st != FE_FS_OK) {
        out("rm: "); out(fs_err(st)); out("\n");
        g_fail++;
    } else {
        out("removed "); out(path); out("\n");
    }
}

static void cmd_help(void)
{
    out("commands:\n");
    out("  help               this list\n");
    out("  echo <text>        print text\n");
    out("  echo <text> > <f>  write text to a file (creates/truncates)\n");
    out("  echo <text> >> <f> append to a file\n");
    out("  ls [path]          list a directory (default /)\n");
    out("  cat <path>         print a file\n");
    out("  rm <path>          delete a file\n");
    out("  fs                 filesystem info\n");
    out("  clear              clear the screen\n");
    out("  exit               leave the shell\n");
    out("  <name> [args]      run /bin/<name> from the booted slot\n");
    out("notes: line editing and echo are done by consoled;\n");
    out("       paths are FAT paths (absolute), e.g. /home/a.txt or /slot_a/bin/hello\n");
    out("       file creation supports 8.3 names only (long names can be read)\n");
}

static void cmd_echo(const char *args)
{
    out(args);
    out("\n");
}

static void cmd_ls(const char *path)
{
    if (g_fs <= 0) {
        out("ls: no filesystem (/dev/fs0 not open)\n");
        g_fail++;
        return;
    }
    if (!path || !*path) {
        path = "/";
    }
    u32 index = 0;
    u32 total = 0;
    for (u32 page = 0; page < 64; page++) {
        static struct fe_fs_list_reply rep;
        if (fe_fs_list(g_fs, path, index, &rep) != 0) {
            out("ls: cannot read directory\n");
            g_fail++;
            return;
        }
        if (rep.count == 0) {
            break;
        }
        for (u32 i = 0; i < rep.count && i < FE_FS_LIST_MAX; i++) {
            if (rep.entries[i].is_dir) {
                out("[dir]  ");
            } else {
                out("       ");
                out_num(rep.entries[i].size);
                out("  ");
            }
            out(rep.entries[i].name);
            out("\n");
            total++;
        }
        if (rep.next_index == 0) {
            break;
        }
        index = rep.next_index;
    }
    out("(");
    out_num(total);
    out(" entries)\n");
}

static void cmd_cat(const char *path)
{
    if (g_fs <= 0) {
        out("cat: no filesystem\n");
        g_fail++;
        return;
    }
    if (!path || !*path) {
        out("cat: missing path\n");
        g_fail++;
        return;
    }
    static struct fe_fs_read_reply rep;
    u64 off = 0;
    u32 total = 0;
    char last = '\n';
    for (u32 round = 0; round < 64; round++) {
        int n = fe_fs_read(g_fs, path, off, &rep);
        if (n < 0) {
            out("cat: read error\n");
            g_fail++;
            return;
        }
        if (n == 0) {
            break;
        }
        if (n > FE_FS_CHUNK) {
            n = FE_FS_CHUNK;
        }
        last = rep.data[n - 1];
        /* 追加一个终止符再整体输出：控制台按字节写，
         * 所以这里只是给 out() 一个可以扫描的 C 字符串。 */
        rep.data[n] = '\0';
        out(rep.data);
        off += (u64)n;
        total += (u32)n;
    }
    if (total == 0) {
        out("cat: empty or not found\n");
    } else if (last != '\n') {
        out("\n");      /* 文件末尾没有换行时补一个，免得提示符贴在内容后面 */
    }
}

static void cmd_fs(void)
{
    if (g_fs <= 0) {
        out("fs: no filesystem\n");
        return;
    }
    static struct fe_fs_info info;
    if (fe_fs_info(g_fs, "/", &info) != 0) {
        out("fs: info failed\n");
        return;
    }
    out("label    ");
    out(info.label);
    out("\n");
    out("fat      ");
    out(info.fat_type == 32 ? "FAT32" : "FAT16");
    out("\n");
    out("cluster  ");
    out_num(info.cluster_size);
    out(" bytes\n");
    out("clusters ");
    out_num(info.clusters);
    out("\n");
    out("total    ");
    out_num(info.total_bytes / 1024);
    out(" KiB\n");
}

/* 跑一个外部程序。找不到就明确说找不到，而不是静默无反应。 */
static int run_external(char *argv[], int argc)
{
    char path[96];
    u32 k = 0;
    const char *parts[3];
    parts[0] = g_prefix;
    parts[1] = "/bin/";
    parts[2] = argv[0];
    for (u32 p = 0; p < 3; p++) {
        for (const char *s = parts[p]; *s && k < sizeof(path) - 1; s++) {
            path[k++] = *s;
        }
    }
    path[k] = '\0';

    long h = fe_spawn(path, argv, (u32)argc);
    if (h < 0) {
        out(argv[0]);
        out(": not found (looked for ");
        out(path);
        out(")\n");
        return -1;
    }
    int status = 0;
    fe_wait(h, &status);
    fe_handle_close(h);
    out("[");
    out(argv[0]);
    out(" exit ");
    if (status < 0) {
        out("-");
        out_num((u64)(-status));
    } else {
        out_num((u64)status);
    }
    out("]\n");
    return status;
}

/* 把一行拆成参数。空白分隔，原地改 g_line。 */
static int split_args(char *line, char *argv[], int max)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') {
            p++;
        }
        if (*p) {
            *p++ = '\0';
        }
    }
    return argc;
}

/* 把一行切成「命令词」与「其余部分（原样）」。
 *
 * ★ 为什么不能只靠 split_args ★
 * 它会把每个参数就地截断，于是 `echo hello world` 只剩下 `hello`——
 * 因为 args[1] 指向的那个字符串已经被 '\0' 截在第一个空格处。
 * 我第一版就是这么写的。echo 要的是"后面那一整段"，所以这里必须在
 * 切分**之前**把剩余部分的指针取出来。 */
static char *split_cmd(char *line, char **out_rest)
{
    char *p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    char *cmd = p;
    while (*p && *p != ' ' && *p != '\t') {
        p++;
    }
    if (*p) {
        *p++ = '\0';
        while (*p == ' ' || *p == '\t') {
            p++;
        }
    }
    *out_rest = p;
    return cmd;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int main(int argc, char **argv)
{
    char line[LINE_CAP];
    char *args[ARG_MAX];

    fe_slot_prefix((argc > 0 && argv) ? argv[0] : 0, g_prefix, sizeof(g_prefix));

    g_con = fe_devfs_open("/dev/console");
    if (g_con <= 0) {
        fe_puts("[sh] **打不开 /dev/console**，shell 无法交互\n");
        return 1;
    }
    g_fs = fe_devfs_open("/dev/fs0");   /* 没有文件系统也要能进 shell */

    out("\n");
    out("FEKernel shell.  slot = ");
    out(g_prefix[0] ? g_prefix : "(none)");
    out(", fs = ");
    out(g_fs > 0 ? "yes" : "NO");
    out(".\n");

    for (;;) {
        out("> ");
        int n = fe_con_readline(g_con, line, LINE_CAP);
        if (n < 0) {
            out("\n[sh] console read failed, exiting\n");
            break;
        }
        if (n == 0) {
            continue;               /* 空行：再给一个提示符 */
        }

        char *rest = NULL;
        char *cmd = split_cmd(line, &rest);
        if (!*cmd) {
            continue;
        }

        if (streq(cmd, "help")) {
            cmd_help();
        } else if (streq(cmd, "echo")) {
            /* ★ 重定向：`echo text > file` / `>> file` ★
             * 这是"文件系统可写"最直接的手动验证方式：用户打一条命令就能
             * 创建文件、再 cat 回来核对。也是 C5/C6 的验收手段。
             *
             * 找最后一个 '>'：文本里本来就可能含 '>'，取最后一个更符合直觉
             * （与 shell 的既有习惯一致）。 */
            char *redir = 0;
            int append = 0;
            for (char *p = rest ? rest : line; *p; p++) {
                if (*p == '>') {
                    redir = p;
                    append = (p[1] == '>') ? 1 : 0;
                }
            }
            if (redir) {
                *redir = '\0';
                char *path = redir + 1 + append;
                while (*path == ' ') {
                    path++;
                }
                /* 去掉文本尾部的空格，免得写进去一堆空格 */
                char *end = redir;
                while (end > (rest ? rest : line) && end[-1] == ' ') {
                    end--;
                }
                *end = '\0';
                write_file(path, rest ? rest : "", append);
            } else {
                cmd_echo(rest ? rest : "");
            }
        } else if (streq(cmd, "rm")) {
            args[0] = cmd;
            int ac = 1 + split_args(rest ? rest : "", args + 1, ARG_MAX - 1);
            cmd_rm(ac > 1 ? args[1] : "");
        } else if (streq(cmd, "ls")) {
            args[0] = cmd;
            int ac = 1 + split_args(rest ? rest : "", args + 1, ARG_MAX - 1);
            cmd_ls(ac > 1 ? args[1] : "/");
        } else if (streq(cmd, "cat")) {
            args[0] = cmd;
            int ac = 1 + split_args(rest ? rest : "", args + 1, ARG_MAX - 1);
            cmd_cat(ac > 1 ? args[1] : "");
        } else if (streq(cmd, "fs")) {
            cmd_fs();
        } else if (streq(cmd, "clear")) {
            /* 控制台没有"清屏"操作码：用足够多的换行把内容顶出去。
             * 这比新加一个操作码诚实——清屏是终端的事，
             * 而我们的控制台只有"写字符"这一种能力。 */
            for (int i = 0; i < 44; i++) {
                out("\n");
            }
        } else if (streq(cmd, "exit")) {
            out("bye\n");
            break;
        } else {
            args[0] = cmd;
            int ac = 1 + split_args(rest ? rest : "", args + 1, ARG_MAX - 1);
            run_external(args, ac);
        }
    }

    fe_handle_close(g_con);
    if (g_fs > 0) {
        fe_handle_close(g_fs);
    }
    return (int)g_fail;
}
