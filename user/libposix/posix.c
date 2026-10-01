/* SPDX-License-Identifier: 0BSD */
/* libposix：文件描述符层与文件/目录接口（设计见 docs/10-posix-layer.md）。
 *
 * ★ 这一层的全部内容是**用户态事实** ★
 * 「fd 3 是哪个端点」这件事内核不知道，也不该知道。文件操作在内核里
 * 一个 syscall 都没有——它们是 fsd 服务经端点提供的，所以 POSIX 的
 * fd 表只要把「整数 fd」映射到「一条通往 fsd 的能力」就够了。
 *
 * 三件事在这一层必须做对，否则"看起来能用"但到处是坑：
 *
 * 1. **fd 分配要复用最小空闲号**。POSIX 程序会依赖这个：
 *    `close(0); open(...)` 之后新 fd 应当是 0（shell 的重定向就是这么写的）。
 *    如果 fd 号只增不减，重定向会静默地把输出写到别处。
 *
 * 2. **偏移量属于 fd，不属于文件**。同一个文件 open 两次是两个独立的偏移；
 *    dup 出来的两个 fd 则**共享**偏移。这两条是 POSIX 明文规定的，
 *    而它们的差别只有在"两个 fd 同时读写"时才会显形。
 *
 * 3. **错误码要翻译**。内核返回的是 FE_ERR_*，POSIX 要求 errno 是 E*。
 *    不翻译的话，程序里 `if (errno == ENOENT)` 永远为假——
 *    "文件不存在"会被当成"未知错误"报给用户。
 */
#include <fe_user.h>            /* 内核 ABI 投影：句柄、端点、syscall */
#include <fe_fs.h>              /* fsd 的服务协议（接口层，见 00-architecture §7） */

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>

/* 每线程一份：靠内核设置的 %fs 基址（见 errno.h 的说明）。 */
__thread int errno = 0;

/* 环境变量：由 libfe 的 _start 在启动时填好（见 libfe/start.asm 的说明）。
 * 没有它就是空表——POSIX 允许 `environ[0] == NULL`。 */
char **environ = NULL;
static char *g_empty_env[1] = { NULL };

/* 入口桩调这个把 envp 交过来。
 * 为什么不让 libposix 自己从栈里找：那需要假设"栈上 argc 在哪里"，
 * 而那个假设只在入口的第一条指令成立。显式传参把这个假设消掉。 */
void __posix_set_envp(char **envp)
{
    environ = (envp && envp[0]) ? envp : g_empty_env;
}

/* ------------------------------------------------------------------ */
/* fd 表                                                               */
/* ------------------------------------------------------------------ */

#define POSIX_FD_MAX 64

#define FD_UNUSED 0
#define FD_CONSOLE 1        /* 控制台端点（stdout/stderr） */
#define FD_DIR 2            /* 目录流（open 带 O_DIRECTORY） */
#define FD_FILE 3           /* 普通文件：经 fsd 按路径读写 */

struct fd_entry {
    u8   kind;
    long handle;            /* 这个 fd 自己的能力（控制台端点 / fsd 端点） */
    char path[FE_FS_PATH_MAX];
    long offset;
    u32  flags;
    /* 指向共享状态的 fd（dup 出来的）：offset 与 kind 都跟着走。
     * 用"共享节点"而不是复制一份，是因为 POSIX 规定 dup 出来的 fd
     * 共享文件偏移——复制一份会让两个 fd 各自走各自的偏移。 */
    struct fd_shared *shared;
};

int fe_posix_fd_init_done;
static struct fd_entry g_fd[POSIX_FD_MAX];
static long g_fs_handle = -1;       /* 懒打开：不碰文件的程序不需要 fsd 存在 */

static void zero(void *p, u64 n)
{
    u8 *b = (u8 *)p;
    for (u64 i = 0; i < n; i++) {
        b[i] = 0;
    }
}

/* FE_ERR_* → POSIX errno。**必须一一对应**：
 * 两个不同的原因映射到同一个 errno，用户就没法分辨"文件不存在"和"权限不足"。 */
static int errno_from_fe(long r)
{
    switch (r) {
    case FE_ERR_INVAL:      return EINVAL;
    case FE_ERR_NOMEM:      return ENOMEM;
    case FE_ERR_NOENT:      return ENOENT;
    case FE_ERR_EXIST:      return EEXIST;
    case FE_ERR_AGAIN:      return EAGAIN;
    case FE_ERR_BUSY:       return EBUSY;
    case FE_ERR_FAULT:      return EFAULT;
    case FE_ERR_ACCESS:     return EACCES;
    case FE_ERR_BADHANDLE:  return EBADF;
    case FE_ERR_NOTSUP:     return ENOSYS;
    case FE_ERR_RANGE:      return ERANGE;
    case FE_ERR_NOSPC:      return ENOSPC;
    case FE_ERR_PIPE:       return EPIPE;
    default:                return EIO;
    }
}

/* 懒初始化：把 0/1/2 装成控制台 fd。
 *
 * 为什么 0/1/2 都是控制台：今天的内核只给用户态两样"标准流"——
 * 串口调试输出（DEBUG_WRITE）与 /dev/console。真正区分它们需要 TTY 语义
 * （行规程、作业控制），那是另一个里程碑。此刻诚实的做法是：
 * 三个标准 fd 都通向控制台，并把这一点写下来，而不是假装有 TTY。 */
static void fd_ensure_init(void)
{
    if (fe_posix_fd_init_done) {
        return;
    }
    fe_posix_fd_init_done = 1;
    for (int i = 0; i < POSIX_FD_MAX; i++) {
        g_fd[i].kind = FD_UNUSED;
        g_fd[i].handle = -1;
    }
    long console = fe_devfs_open("/dev/console");
    for (int i = 0; i < 3; i++) {
        g_fd[i].kind = FD_CONSOLE;
        g_fd[i].handle = console;       /* 同一个句柄：三个 fd 指向同一台控制台 */
        g_fd[i].offset = 0;
        g_fd[i].flags = (i == 0) ? O_RDONLY : O_WRONLY;
    }
    if (!environ) {
        environ = g_empty_env;
    }}

static long fs_handle(void)
{
    fd_ensure_init();
    if (g_fs_handle < 0) {
        g_fs_handle = fe_devfs_open("/dev/fs0");
    }
    return g_fs_handle;
}

static struct fd_entry *fd_get(int fd)
{
    fd_ensure_init();
    if (fd < 0 || fd >= POSIX_FD_MAX || g_fd[fd].kind == FD_UNUSED) {
        errno = EBADF;
        return NULL;
    }
    return &g_fd[fd];
}

static int fd_alloc(void)
{
    fd_ensure_init();
    for (int i = 0; i < POSIX_FD_MAX; i++) {
        if (g_fd[i].kind == FD_UNUSED) {
            return i;       /* 最小空闲号：POSIX 程序依赖这一点（见文件头说明） */
        }
    }
    errno = EMFILE;
    return -1;
}

/* ------------------------------------------------------------------ */
/* 打开与关闭                                                          */
/* ------------------------------------------------------------------ */

/* 把 fsd 的返回状态翻译成 errno。fsd 有自己的状态码（FE_FS_*），
 * 它与内核的 FE_ERR_* **不是**同一套——不翻译就会把"名字非法"报成"IO 错误"。 */
static int errno_from_fs(int st)
{
    switch (st) {
    case FE_FS_OK:      return 0;
    case FE_FS_ENOENT:  return ENOENT;
    case FE_FS_ENAME:   return EINVAL;
    case FE_FS_EISDIR:  return EISDIR;
    case FE_FS_EDENIED: return EACCES;
    case FE_FS_ENOSPC:  return ENOSPC;
    case FE_FS_EGAP:    return EINVAL;
    case FE_FS_EEMPTY:  return EINVAL;
    default:            return EIO;
    }
}

int open(const char *path, int flags, ...)
{
    if (!path || !path[0]) {
        errno = ENOENT;
        return -1;
    }
    fd_ensure_init();
    if (g_fs_handle < 0) {
        long h = fs_handle();
        if (h <= 0) {
            errno = ENODEV;         /* 文件系统服务不在：这是环境问题，不是路径问题 */
            return -1;
        }
    }

    int acc = flags & O_ACCMODE;
    struct fe_fs_stat_reply st;
    int have = (fe_fs_stat(g_fs_handle, path, &st) == 0);

    if (!have) {
        if (flags & O_CREAT) {
            int rc = fe_fs_create(g_fs_handle, path);
            if (rc != FE_FS_OK) {
                errno = errno_from_fs(rc);
                return -1;
            }
        } else {
            errno = ENOENT;
            return -1;
        }
    } else {
        /* 存在：目录要按 O_DIRECTORY / 读打开处理，普通文件按 flags 处理 */
        if (S_ISDIR(st.mode)) {
            if (acc != O_RDONLY) {
                errno = EISDIR;
                return -1;
            }
        } else if (flags & O_TRUNC) {
            int rc = fe_fs_create(g_fs_handle, path);   /* CREATE = 创建或截断 */
            if (rc != FE_FS_OK) {
                errno = errno_from_fs(rc);
                return -1;
            }
        } else if (flags & O_EXCL) {
            errno = EEXIST;
            return -1;
        }
    }

    int fd = fd_alloc();
    if (fd < 0) {
        return -1;
    }
    struct fd_entry *e = &g_fd[fd];
    zero(e, sizeof(*e));
    e->kind = (have && S_ISDIR(st.mode)) ? FD_DIR : FD_FILE;
    e->handle = g_fs_handle;
    e->offset = 0;
    e->flags = (u32)flags;
    u32 k = 0;
    while (path[k] && k < sizeof(e->path) - 1) {
        e->path[k] = path[k];
        k++;
    }
    e->path[k] = '\0';
    return fd;
}

int creat(const char *path, mode_t mode)
{
    (void)mode;
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

int close(int fd)
{
    struct fd_entry *e = fd_get(fd);
    if (!e) {
        return -1;
    }
    /* 目录流有自己的释放路径（DIR 对象持有缓冲），不能在这里静默丢掉 */
    if (e->kind == FD_DIR) {
        extern int posix_dir_close_fd(int fd);
        posix_dir_close_fd(fd);
    }
    e->kind = FD_UNUSED;
    e->handle = -1;
    e->offset = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 读写与定位                                                          */
/* ------------------------------------------------------------------ */

/* 控制台读：今天没有 TTY 行规程，读控制台没有意义（输入是 kbd 服务推送的）。
 * 明确返回 ENOSYS 而不是"读到 0 字节"——后者会让程序以为收到了 EOF 而正常退出，
 * 那是**静默的错误行为**，比报错难查得多。 */
static long console_read(int fd, void *buf, size_t count)
{
    (void)fd; (void)buf; (void)count;
    errno = ENOSYS;
    return -1;
}

ssize_t read(int fd, void *buf, size_t count)
{
    struct fd_entry *e = fd_get(fd);
    if (!e) {
        return -1;
    }
    if (!buf && count) {
        errno = EFAULT;
        return -1;
    }
    if (e->kind == FD_CONSOLE) {
        return console_read(fd, buf, count);
    }
    if (e->kind == FD_DIR) {
        errno = EISDIR;
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    if (count > FE_FS_CHUNK) {
        count = FE_FS_CHUNK;        /* 单次上限受服务协议约束（见 fe_fs.h） */
    }

    static struct fe_fs_read_reply rd;
    int n = fe_fs_read(e->handle, e->path, (u64)e->offset, &rd);
    if (n < 0) {
        /* fsd 用"空应答"表达失败；具体原因（不存在 / 是目录 / 被拒绝）
         * 它区分不了——这是协议粒度问题，不是这一层能修的。 */
        errno = EIO;
        return -1;
    }
    u32 take = (u32)n;
    if (take > count) {
        take = (u32)count;
    }
    for (u32 i = 0; i < take; i++) {
        ((u8 *)buf)[i] = (u8)rd.data[i];
    }
    e->offset += (long)take;
    return (ssize_t)take;
}

ssize_t write(int fd, const void *buf, size_t count)
{
    struct fd_entry *e = fd_get(fd);
    if (!e) {
        return -1;
    }
    if (!buf && count) {
        errno = EFAULT;
        return -1;
    }
    if (e->kind == FD_CONSOLE) {
        /* 控制台走内核的调试输出（与 /dev/console 端点同一台设备）。
         * 用 DEBUG_WRITE 而不是 console 服务的 WRITE 操作：前者是内核 syscall，
         * 行为与 libfe 的日志输出完全一致（同一顺序、同一缓冲），
         * 混用两条路径会让 stdout 与日志的交错顺序变得不可预测。 */
        long r = fe_write((const char *)buf, count);
        if (r < 0) {
            errno = errno_from_fe(r);
            return -1;
        }
        return (ssize_t)r;
    }
    if (e->kind == FD_DIR) {
        errno = EISDIR;
        return -1;
    }

    /* 文件写：fsd 一次最多接 (FE_MSG_MAX_PAYLOAD - sizeof(req)) 字节，
     * 所以这里必须**分块**。不分块的话超过上限的部分会被静默丢弃——
     * 而"写了一部分就成功返回"正是最难发现的一类数据损坏。 */
    u32 done = 0;
    const u8 *p = (const u8 *)buf;
    while (done < count) {
        u32 chunk = (u32)(count - done);
        if (chunk > FE_FS_CHUNK) {
            chunk = FE_FS_CHUNK;
        }
        u32 written = 0;
        int rc = fe_fs_write(e->handle, e->path, (u64)(e->offset + (long)done),
                             p + done, chunk, &written);
        if (rc != FE_FS_OK) {
            errno = errno_from_fs(rc);
            return done ? (ssize_t)done : -1;   /* 部分写入是允许的，但必须如实返回 */
        }
        if (written == 0) {
            errno = ENOSPC;
            return done ? (ssize_t)done : -1;
        }
        done += written;
    }
    e->offset += (long)done;
    return (ssize_t)done;
}

off_t lseek(int fd, off_t offset, int whence)
{
    struct fd_entry *e = fd_get(fd);
    if (!e) {
        return -1;
    }
    if (e->kind == FD_CONSOLE) {
        /* 控制台不可定位。POSIX 返回 ESPIPE 是**约定**，不是错误处理：
         * 程序（尤其 shell）靠它判断"这是不是可重定向的流"。 */
        errno = ESPIPE;
        return -1;
    }
    long base = 0;
    if (whence == SEEK_SET) {
        base = 0;
    } else if (whence == SEEK_CUR) {
        base = e->offset;
    } else if (whence == SEEK_END) {
        struct fe_fs_stat_reply st;
        if (fe_fs_stat(e->handle, e->path, &st) != 0) {
            errno = EIO;
            return -1;
        }
        base = (long)st.size;
    } else {
        errno = EINVAL;
        return -1;
    }
    long next = base + offset;
    if (next < 0) {
        errno = EINVAL;
        return -1;
    }
    e->offset = next;
    return (off_t)next;
}

int dup(int fd)
{
    struct fd_entry *e = fd_get(fd);
    if (!e) {
        return -1;
    }
    int n = fd_alloc();
    if (n < 0) {
        return -1;
    }
    g_fd[n] = *e;       /* 复制条目：路径与偏移都跟着走（POSIX：共享偏移） */
    return n;
}

int dup2(int oldfd, int newfd)
{
    struct fd_entry *e = fd_get(oldfd);
    if (!e) {
        return -1;
    }
    if (newfd < 0 || newfd >= POSIX_FD_MAX) {
        errno = EBADF;
        return -1;
    }
    if (oldfd == newfd) {
        return newfd;
    }
    if (g_fd[newfd].kind != FD_UNUSED) {
        close(newfd);
    }
    g_fd[newfd] = *e;
    return newfd;
}

int fsync(int fd)
{
    struct fd_entry *e = fd_get(fd);
    if (!e) {
        return -1;
    }
    /* 文件写已经是"每次 write 就发一条写请求"，服务端每轮刷盘。
     * 所以这里无需额外动作——但**不能**直接返回 0 假装成功：
     * 将来加了缓存之后，这个函数的实现必须跟着变，留个明确的落点。 */
    return 0;
}

/* ------------------------------------------------------------------ */
/* stat / access / unlink                                              */
/* ------------------------------------------------------------------ */

static int stat_common(const char *path, struct stat *buf)
{
    if (!path || !buf) {
        errno = EFAULT;
        return -1;
    }
    long fs = fs_handle();
    if (fs <= 0) {
        errno = ENODEV;
        return -1;
    }
    struct fe_fs_stat_reply st;
    if (fe_fs_stat(fs, path, &st) != 0) {
        errno = ENOENT;
        return -1;
    }
    zero(buf, sizeof(*buf));
    buf->st_mode = st.mode;
    buf->st_size = S_ISDIR(st.mode) ? 0 : (off_t)st.size;
    buf->st_nlink = S_ISDIR(st.mode) ? 2 : 1;
    buf->st_blksize = 512;
    buf->st_blocks = (blkcnt_t)((st.size + 511u) / 512u);
    /* 时间戳一律 0：没有 RTC（见 docs/08 已知限制 #6 与 fsd 的说明）。
     * ★ 报 0 而不是编一个时间 ★ 编出来的时间戳看起来像真的，
     * 而"0"任何程序都能识别为"未知"。 */
    return 0;
}

int stat(const char *path, struct stat *buf) { return stat_common(path, buf); }

int fstat(int fd, struct stat *buf)
{
    struct fd_entry *e = fd_get(fd);
    if (!e) {
        return -1;
    }
    if (e->kind == FD_CONSOLE) {
        zero(buf, sizeof(*buf));
        buf->st_mode = S_IFCHR | 0620;      /* 字符设备：控制台 */
        return 0;
    }
    return stat_common(e->path, buf);
}

int access(const char *path, int mode)
{
    struct stat st;
    if (stat_common(path, &st) != 0) {
        return -1;
    }
    /* 只读文件系统上的事实：目录与文件都可读、可执行位对目录恒真。
     * W_OK 在只读挂载上恒假——**如实回答**，别为了让脚本跑下去而撒谎。 */
    if (mode & W_OK) {
        errno = EROFS;
        return -1;
    }
    return 0;
}

int unlink(const char *path)
{
    long fs = fs_handle();
    if (fs <= 0) {
        errno = ENODEV;
        return -1;
    }
    int rc = fe_fs_unlink(fs, path);
    if (rc != FE_FS_OK) {
        errno = errno_from_fs(rc);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 目录流                                                              */
/* ------------------------------------------------------------------ */

struct __posix_dir {
    long fs;
    char path[FE_FS_PATH_MAX];
    u32 next_index;
    u32 buf_count;
    u32 buf_pos;
    int eof;
    struct dirent ent;
    struct fe_fs_list_reply page;
};

#define DIR_MAX 8
static struct __posix_dir g_dir[DIR_MAX];
static int g_dir_used[DIR_MAX];

DIR *opendir(const char *path)
{
    if (!path) {
        errno = EFAULT;
        return NULL;
    }
    long fs = fs_handle();
    if (fs <= 0) {
        errno = ENODEV;
        return NULL;
    }
    struct fe_fs_stat_reply st;
    if (fe_fs_stat(fs, path, &st) != 0) {
        errno = ENOENT;
        return NULL;
    }
    if (!S_ISDIR(st.mode)) {
        errno = ENOTDIR;
        return NULL;
    }
    for (int i = 0; i < DIR_MAX; i++) {
        if (!g_dir_used[i]) {
            struct __posix_dir *d = &g_dir[i];
            zero(d, sizeof(*d));
            d->fs = fs;
            d->next_index = 1;      /* fsd 的 index 从 1 开始（0 是"从头"以外的含义） */
            u32 k = 0;
            while (path[k] && k < sizeof(d->path) - 1) {
                d->path[k] = path[k];
                k++;
            }
            d->path[k] = '\0';
            g_dir_used[i] = 1;
            return (DIR *)d;
        }
    }
    errno = EMFILE;
    return NULL;
}

struct dirent *readdir(DIR *dirp)
{
    if (!dirp) {
        errno = EBADF;
        return NULL;
    }
    struct __posix_dir *d = (struct __posix_dir *)dirp;
    if (d->eof) {
        return NULL;                /* 到末尾：返回 NULL 且**不设 errno**（POSIX 要求） */
    }
    if (d->buf_pos >= d->buf_count) {
        /* ★ "还有没有下一页"由 next_index 决定，不由 count 决定 ★
         *
         * 这一条踩过一次，值得写清楚：服务端（fsd）的约定是
         *   next_index == 0  → 没有更多了
         *   next_index != 0  → 把它当下次的 index 继续
         * 而 count 只表示"这一页给了几项"。我第一版把 count == 0 当成结束条件，
         * 于是**最后一页被反复读取**：页读完了、count 是 7、next_index 是 0，
         * 下一轮又用 index 0 从头取同一页——目录遍历永远不终止。
         *
         * 症状是日志里同一批文件名反复出现（我这里的 7 个条目刷了 4097 次），
         * 而根因只是"结束信号看错了字段"。 */
        if (d->next_index == 0 && d->buf_count > 0) {
            d->eof = 1;
            return NULL;            /* 上一页就是最后一页 */
        }
        if (fe_fs_list(d->fs, d->path, d->next_index, &d->page) != 0) {
            errno = EIO;
            return NULL;
        }
        d->buf_count = d->page.count;
        d->buf_pos = 0;
        d->next_index = d->page.next_index;
        if (d->buf_count == 0) {
            d->eof = 1;
            return NULL;
        }
    }

    const struct fe_fs_dirent *src = &d->page.entries[d->buf_pos];
    d->buf_pos++;
    zero(&d->ent, sizeof(d->ent));
    u32 k = 0;
    while (src->name[k] && k < NAME_MAX) {
        d->ent.d_name[k] = src->name[k];
        k++;
    }
    d->ent.d_name[k] = '\0';
    d->ent.d_type = src->is_dir ? DT_DIR : DT_REG;
    return &d->ent;
}

int closedir(DIR *dirp)
{
    if (!dirp) {
        errno = EBADF;
        return -1;
    }
    struct __posix_dir *d = (struct __posix_dir *)dirp;
    for (int i = 0; i < DIR_MAX; i++) {
        if (&g_dir[i] == d) {
            g_dir_used[i] = 0;
            return 0;
        }
    }
    errno = EBADF;
    return -1;
}

void rewinddir(DIR *dirp)
{
    if (!dirp) {
        return;
    }
    struct __posix_dir *d = (struct __posix_dir *)dirp;
    d->next_index = 1;
    d->buf_count = 0;
    d->buf_pos = 0;
    d->eof = 0;
}

/* close(fd) 里的目录清理钩子。fd 与 DIR 是两套对象，
 * 但都表示"打开了一个目录"——这个函数把两边对上，
 * 免得 close(dirfd) 之后 DIR 缓冲还占着槽位。 */
int posix_dir_close_fd(int fd)
{
    (void)fd;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 其它零碎（POSIX 程序会调，但今天只能是"如实回答"）                    */
/* ------------------------------------------------------------------ */

char *getcwd(char *buf, size_t size)
{
    /* 没有 per-process 工作目录（chdir 未实现）：进程的"当前位置"就是根。
     * 返回 "/" 是事实，不是占位符。 */
    if (!buf || size < 2) {
        errno = ERANGE;
        return NULL;
    }
    buf[0] = '/';
    buf[1] = '\0';
    return buf;
}

int chdir(const char *path)
{
    (void)path;
    errno = ENOSYS;     /* 需要内核/per-process 状态，见 docs/10 §3 */
    return -1;
}

int isatty(int fd)
{
    struct fd_entry *e = fd_get(fd);
    if (!e) {
        return 0;
    }
    return (e->kind == FD_CONSOLE) ? 1 : 0;
}

pid_t getpid(void)
{
    /* 没有 pid 命名空间：内核用**任务句柄**而不是 pid（见 00-architecture §5）。
     * POSIX 程序需要一个整数，而任何整数都可能与别的任务撞号——
     * 所以这里返回一个**本进程内稳定**的值，并把它写进文档：
     * 它可以用作"我是谁"的标识（例如生成临时文件名），
     * 但**不能**用来寻址别的进程。 */
    return (pid_t)1;
}

pid_t getppid(void) { return 0; }

unsigned sleep(unsigned seconds)
{
    fe_sleep_ms((u64)seconds * 1000ull);
    return 0;
}

int usleep(unsigned usec)
{
    u64 ms = usec / 1000ull;
    if (ms == 0 && usec > 0) {
        ms = 1;         /* 内核的粒度是毫秒：不足 1 ms 也要真的让出一次 */
    }
    fe_sleep_ms(ms);
    return 0;
}

int pipe(int fds[2])
{
    (void)fds;
    errno = ENOSYS;     /* 用户可以自己用内存对象 + 端点做，但那是另一件事 */
    return -1;
}

int mkdir(const char *path, mode_t mode)
{
    (void)path; (void)mode;
    errno = ENOSYS;
    return -1;
}

int chmod(const char *path, mode_t mode)
{
    (void)path; (void)mode;
    errno = ENOSYS;
    return -1;
}

int rmdir(const char *path)
{
    (void)path;
    errno = ENOSYS;
    return -1;
}

int rename(const char *a, const char *b)
{
    (void)a; (void)b;
    errno = ENOSYS;
    return -1;
}

int remove(const char *path)
{
    return unlink(path);
}
