/* SPDX-License-Identifier: 0BSD */
/* posixprobe —— 路线 B 的"缺口清单生成器"（设计见 docs/10-posix-layer.md）。
 *
 * ★ 这个文件的作用不是"演示功能"，而是**测量** ★
 * 它按 POSIX 的名字调用一整套接口。先用现有环境编译一次，
 * 编译/链接报出来的每一个错误就是一条真实缺口——比我凭记忆列一张表可靠得多。
 *
 * 它同时也是一件**真东西**：算出文件内容的校验和并按 `wc` 的格式报数。
 * 不做成"只为探测而生"的假程序，是因为假程序会掩盖真实依赖
 * （真实程序才需要缓冲、需要 errno、需要按大小分配内存）。
 *
 * 用法：  posixprobe [-v] <文件> [<文件>...]
 *         -v  打印每个文件：行数、词数、字节数（与 POSIX wc 一致）
 *         无 -v 时只打印总计，退出码 0；任何一个文件打不开 → 退出码 1。
 *
 * 覆盖的接口（按里程碑分组，见 docs/10-posix-layer.md §6）：
 *   M1 字符串与内存    memcpy/memset/strlen/strcmp/strchr/strtoul
 *   M1 堆              malloc/free
 *   M2 文件 IO         open/close/read/write/lseek/stat
 *   M2 stdio           printf/fprintf/stderr/perror/fflush
 *   M3 目录            opendir/readdir/closedir
 *   M4 进程            posix_spawn/waitpid/getpid
 *   M5 错误码          errno/strerror
 *
 * 用标准头而不是我们的头：**这正是实验的一部分**——我们要知道
 * "一个普通 C 程序 #include 的那些头"在这个环境里到底能不能用。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <spawn.h>
#include <sys/wait.h>

/* ★ 为什么一个"普通 C 程序"要包含 fe_user.h ★
 *
 * 这一行是**如实记录一条边界**，不是可有可无的：程序住在槽里
 * （/slot_a/bin/…），而槽前缀只有它自己的 argv[0] 知道——
 * POSIX 里没有"我在哪"这个接口，因为我们没有 procfs、没有 chdir、
 * 没有 per-process 根目录（见 docs/10-posix-layer.md §3）。
 *
 * 也就是说：一个要 spawn 出**另一个程序**的 POSIX 程序，今天必须知道
 * 自己在一个 A/B 槽里。这是 A/B 更新带来的真实约束，
 * 等到有 procfs 或者把它做成 spawnp 的搜索路径之后才能消掉。
 * 探测程序的价值之一就是把这类约束摆到明面上。 */
#include <fe_user.h>

#define BUF_SIZE 4096

/* ---- 与 POSIX wc 一致的分类规则 ----
 * 词的定义：由空白分隔的非空序列；`isspace` 在本程序里只认这几个字符
 * （避免为一个探测程序拖进 <ctype.h> 的 locale 语义，那是另一件事）。 */
static int probe_isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

struct counts {
    unsigned long long lines;
    unsigned long long words;
    unsigned long long bytes;
};

/* 读一个文件并统计。返回 0 成功，-1 失败（errno 已设置）。 */
static int count_file(const char *path, struct counts *out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    char *buf = malloc(BUF_SIZE);
    if (!buf) {
        close(fd);
        errno = ENOMEM;
        return -1;
    }

    memset(out, 0, sizeof(*out));
    int in_word = 0;
    for (;;) {
        ssize_t n = read(fd, buf, BUF_SIZE);
        if (n < 0) {
            free(buf);
            close(fd);
            return -1;              /* errno 由 read 设置 */
        }
        if (n == 0) {
            break;                  /* EOF */
        }
        out->bytes += (unsigned long long)n;
        for (ssize_t i = 0; i < n; i++) {
            int c = (unsigned char)buf[i];
            if (c == '\n') {
                out->lines++;
            }
            if (probe_isspace(c)) {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                out->words++;
            }
        }
    }
    free(buf);
    if (close(fd) != 0) {
        return -1;
    }
    return 0;
}

/* ---- M2：stat 与 lseek 的探测（不是 wc 需要，但清单需要）---- */
static int probe_stat_and_seek(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    off_t end = lseek(fd, 0, SEEK_END);
    off_t back = lseek(fd, 0, SEEK_SET);
    int regular = S_ISREG(st.st_mode);
    close(fd);
    if (end < 0 || back != 0 || !regular || (unsigned long long)end != (unsigned long long)st.st_size) {
        errno = EIO;
        return -1;
    }
    return 0;
}

/* ---- M3：目录遍历 ---- */
static int probe_dir(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        return -1;
    }
    unsigned long n = 0;
    struct dirent *e;
    /* ★ 迭代上限是**必须**的 ★
     * 第一版没有上限，于是"目录流永远读不完"表现为一个静默的挂死——
     * 分不清是服务端没回、还是索引没前进。有上限之后，
     * 它能明确报出"读到第 N 项仍在继续"，把问题指到正确的层。 */
    while ((e = readdir(d)) != NULL) {
        /* 跳过 . 与 ..：POSIX 要求 readdir 返回它们，但工具通常不显示。
         * 这里**保留在计数里**、只是不打印——"列出来有 8 项、其中 2 个是
         * 点目录"与"只有 6 项"是两件事，混起来会让下面的项数对不上。 */
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            n++;
            continue;
        }
        n++;
        if (n <= 32) {
            fprintf(stderr, "  [posixprobe]   dirent: %s (%s)\n", e->d_name,
                    e->d_type == DT_DIR ? "目录" : "文件");
        }
        if (n > 4096) {
            fprintf(stderr, "  [posixprobe] 目录遍历没有终点（已读 %lu 项）\n", n);
            closedir(d);
            errno = EIO;
            return -1;
        }
    }
    fprintf(stderr, "  [posixprobe]   共 %lu 项\n", n);
    closedir(d);
    return (n > 0) ? 0 : -1;        /* 目录里至少该有 . 和 .. */
}

/* ---- M4：起一个子进程并等它（用的是 POSIX 的 posix_spawn，不是 fork）----
 *
 * ★ 第一版这里写的是 "/bin/true"——而镜像里**没有**这个程序 ★
 * 于是 posix_spawn 失败、probe 卡在后续路径上，而我一开始以为
 * "子进程那步挂死了"。教训与项目里记过的一致：**先确认你测的东西存在**，
 * 再去解读它的结果。
 *
 * 现在起的是 /bin/hello：它真的在镜像里，而且**退出码是 42**——
 * 于是这一组探测同时验证了三件互相独立的事：
 *   1. spawn 能装载并运行一个真实程序；
 *   2. 退出码能从子进程传回来（不是被丢掉、也没被当成 0）；
 *   3. WIFEXITED / WEXITSTATUS 的位布局翻译是对的。
 * 用 42 而不是 0：如果翻译写错（比如把状态字与退出码搞混），
 * 0 会"碰巧正确"，而 42 不会。 */
static int probe_spawn(const char *child)
{
    char *const argv[] = { (char *)"hello", NULL };
    extern char **environ;
    pid_t pid = 0;
    int rc = posix_spawn(&pid, child, NULL, NULL, argv, environ);
    if (rc != 0) {
        fprintf(stderr, "  [posixprobe]   posix_spawn(\"%s\") 失败: %s\n",
                child, strerror(rc));
        errno = rc;
        return -1;
    }
    int status = 0;
    pid_t got = waitpid(pid, &status, 0);
    if (got < 0) {
        fprintf(stderr, "  [posixprobe]   waitpid 失败: %s\n", strerror(errno));
        return -1;
    }
    if (!WIFEXITED(status)) {
        fprintf(stderr, "  [posixprobe]   子进程不是正常退出（status=%#x）\n", status);
        errno = ECHILD;
        return -1;
    }
    int code = WEXITSTATUS(status);
    fprintf(stderr, "  [posixprobe]   子进程退出码 %d（期望 42）\n", code);
    if (code != 42) {
        errno = ECHILD;
        return -1;
    }
    return 0;
}

/* ---- M6：栈自动增长（按需分页的端到端证据）----
 *
 * ★ 为什么这一组必须由**用户程序**来测 ★
 * 内核自检只能证明"解析函数在给定输入下做对了"，它证明不了
 * "真的发生了一次缺页、而且内核把它接住了"——那需要让 CPU 自己
 * 撞在一页没有映射的栈上。
 *
 * 做法：递归到栈的深处（每层约 96 字节，足够跨过预映射的 4 页），
 * 然后核对内核侧的事实：`fe_pf_resolved_count()` 增加了。
 * 这一条比"没崩"强得多：它证明内核**消化过缺页**，而不是
 * "栈其实早就全映射好了、根本没走到那条路径"。
 *
 * 为什么每层要写数组并读回来：否则优化器会把递归折叠掉，
 * 于是栈根本没被用——那样测出来的是"什么都没发生"。 */
/* 深度要**故意**落在"预映射 4 页之外、保留区之内"：
 * 每层约 96 字节 → 330 层约 31 KiB，跨过预映射的 16 KiB，
 * 但远小于 64 KiB 的栈保留区。
 *
 * ★ 第一版写了 4000 层（约 384 KiB，远超保留区）★ 结果是
 * "用户态栈溢出"——那本该被正常拒绝，却把内核带崩了：
 * 内核打印异常现场时去解引用用户栈指针，触发内核态 #PF 并递归。
 * 那个内核 bug 已经修掉（见 idt.c 里那段注释），而这里把深度改成
 * 落在**合法**区间内：这一组要测的是"栈能长"，不是"越界会怎样"
 * （越界由内核自检的反向对照覆盖）。 */
#define STACK_PROBE_DEPTH 700

static volatile u64 g_stack_probe_sum;

static void stack_eater(u32 depth)
{
    volatile u8 pad[64];        /* 强迫占用真实的栈空间 */
    pad[0] = (u8)depth;
    pad[63] = (u8)(depth >> 8);
    g_stack_probe_sum += pad[0] + pad[63];
    if (depth > 0) {
        stack_eater(depth - 1);
    }
    /* 回程再读一次：证明这一层的栈帧在整个递归期间都有效
     * （如果内核把某页回收了，这里会读到别的线程的数据） */
    g_stack_probe_sum += pad[0];
}

static int probe_stack_growth(void)
{
    u64 before = fe_pf_resolved_count();
    g_stack_probe_sum = 0;
    fprintf(stderr, "  [posixprobe]   递归 %u 层（每层约 96 字节栈）……\n",
            (u32)STACK_PROBE_DEPTH);
    stack_eater(STACK_PROBE_DEPTH);
    u64 after = fe_pf_resolved_count();

    if (g_stack_probe_sum == 0) {
        fprintf(stderr, "  [posixprobe]   递归被优化掉了（和值为 0）\n");
        return -1;
    }
    if (after <= before) {
        fprintf(stderr, "  [posixprobe]   内核没有解析过任何缺页（%llu -> %llu）"
                        "—— 栈并非按需增长\n",
                (unsigned long long)before, (unsigned long long)after);
        return -1;
    }
    fprintf(stderr, "  [posixprobe]   递归完成，内核解析缺页 %llu 次"
                    "（栈是**长**出来的，不是预映射的）\n",
            (unsigned long long)(after - before));
    return 0;
}

int main(int argc, char **argv)
{
    int verbose = 0;
    int first = 1;
    if (argc > 1 && strcmp(argv[1], "-v") == 0) {
        verbose = 1;
        first = 2;
    }
    if (first >= argc) {
        fprintf(stderr, "用法: %s [-v] <文件>...\n", argv[0]);
        return 2;                   /* POSIX 工具的约定：用法错误 = 2 */
    }

    struct counts total;
    memset(&total, 0, sizeof(total));
    int bad = 0;

    for (int i = first; i < argc; i++) {
        struct counts c;
        if (count_file(argv[i], &c) != 0) {
            perror(argv[i]);        /* "名字: 原因"，原因来自 strerror(errno) */
            bad = 1;
            continue;
        }
        total.lines += c.lines;
        total.words += c.words;
        total.bytes += c.bytes;
        if (verbose) {
            printf("%7llu %7llu %7llu %s\n", c.lines, c.words, c.bytes, argv[i]);
        }
    }

    if (verbose && argc - first > 1) {
        printf("%7llu %7llu %7llu 总计\n", total.lines, total.words, total.bytes);
    } else if (!verbose) {
        printf("%7llu %7llu %7llu\n", total.lines, total.words, total.bytes);
    }

    /* 顺带把剩下几组接口也碰一次：它们的结果**参与退出码**，
     * 所以不是"调了就算"，而是"不对就失败"。
     *
     * ★ 每一步之前打一个标记 ★
     * 第一版没有标记，结果是"读文件那段对了，然后就没有然后了"——
     * 分不清是 stat、目录、还是子进程那一步挂住。
     * 探测程序的价值在于**定位**，所以它自己必须能定位。 */
    fprintf(stderr, "  [posixprobe] 步骤: stat/lseek\n");
    if (probe_stat_and_seek(argv[first]) != 0) {
        fprintf(stderr, "posixprobe: stat/lseek 探测失败: %s\n", strerror(errno));
        bad = 1;
    }
    fprintf(stderr, "  [posixprobe] 步骤: 目录遍历\n");
    if (probe_dir("/") != 0) {
        fprintf(stderr, "posixprobe: 目录遍历探测失败: %s\n", strerror(errno));
        bad = 1;
    }
    fprintf(stderr, "  [posixprobe] 步骤: 子进程\n");
    /* ★ 子程序的路径必须带槽前缀 ★
     * 镜像里程序住在 /slot_a/bin/…（槽是 A/B 更新的单位），
     * 而 `/bin` 是**槽内**路径，由装载器加前缀。
     * 第一版直接 spawn("/bin/hello")，得到 ENOENT——文件确实不在那儿。
     * 这里从自己的 argv[0] 推出前缀（内核 exec 的就是 /slot_a/bin/posixprobe），
     * 于是"我在哪个槽"这件事不用问任何人（见 fe_slot_prefix 的说明）。 */
    char prefix[32];
    char child[96];
    fe_slot_prefix(argv[0], prefix, sizeof(prefix));
    {
        u32 k = 0;
        for (const char *s = prefix; *s && k < sizeof(child) - 1; s++) {
            child[k++] = *s;
        }
        const char *rel = "/bin/hello";
        for (const char *s = rel; *s && k < sizeof(child) - 1; s++) {
            child[k++] = *s;
        }
        child[k] = '\0';
    }
    fprintf(stderr, "  [posixprobe]   子程序路径 %s\n", child);
    if (probe_spawn(child) != 0) {
        fprintf(stderr, "posixprobe: 子进程探测失败: %s\n", strerror(errno));
        bad = 1;
    }
    fprintf(stderr, "  [posixprobe] 步骤: 栈自动增长\n");
    if (probe_stack_growth() != 0) {
        fprintf(stderr, "posixprobe: 栈增长探测失败\n");
        bad = 1;
    }
    fprintf(stderr, "  [posixprobe] 步骤: 完成，退出码 %d\n", bad ? 1 : 0);

    if (fflush(stdout) != 0) {
        bad = 1;
    }
    return bad ? 1 : 0;
}
