/* SPDX-License-Identifier: 0BSD */
/* libposix：进程（见 docs/10-posix-layer.md §4）。
 *
 * ★ 这里提供的是 posix_spawn，不是 fork ★
 * `kernel/include/fe/process.h` 里写明了"没有 fork"是设计决定，
 * 而需要它的程序（make、shell）真正要的是"起子进程并等它"。
 * POSIX 里那件事的名字就是 posix_spawn——所以我们实现的是它，
 * 不是"用 spawn 冒充 fork"。
 *
 * ★ pid ↔ 任务句柄的映射在本进程内 ★
 * 内核用**任务句柄**而不是 pid（不可伪造的能力）。POSIX 程序要一个整数 pid，
 * 于是这里维护一张进程内的表：pid → 句柄。这张表**不需要**全局唯一，
 * 因为一个进程只可能 wait 自己 fork/spawn 出来的孩子——
 * 那正是能力模型比 pid 强的地方（不需要"谁都能 kill 谁"）。
 */
#include <spawn.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <fe_user.h>

#define SPAWN_MAX 16

struct child {
    pid_t pid;
    long  handle;           /* fe_spawn 返回的任务句柄 */
    int   used;
    int   reaped;
    int   status;
};

static struct child g_children[SPAWN_MAX];
static pid_t g_next_pid = 100;      /* 从 100 起：避开 shell 的保留值，也便于辨认 */

static struct child *child_by_pid(pid_t pid)
{
    for (int i = 0; i < SPAWN_MAX; i++) {
        if (g_children[i].used && g_children[i].pid == pid) {
            return &g_children[i];
        }
    }
    return NULL;
}

static struct child *child_alloc(void)
{
    for (int i = 0; i < SPAWN_MAX; i++) {
        if (!g_children[i].used) {
            memset(&g_children[i], 0, sizeof(g_children[i]));
            g_children[i].used = 1;
            return &g_children[i];
        }
    }
    return NULL;
}

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa)
{
    (void)fa;
    return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa)
{
    (void)fa;
    return 0;
}

/* ★ file_actions 今天就返回 ENOSYS，而且是**明确**的 ★
 *
 * 它的用途是"在子进程里先做 dup2/open/close 再 exec"——也就是重定向。
 * 我们做不到，原因很具体：内核的 `fe_process_spawn` 不认识 fd，
 * 而用户态的 fd 表**不跨任务继承**（子进程是全新的地址空间与句柄表）。
 * 要做对，需要内核提供"spawn 时带上初始 fd 映射"的能力（见 §3 的 K1/K2）。
 *
 * 假装成功会更糟：程序以为重定向生效了，输出却打到了控制台，
 * 而脚本里 `prog > out.txt` 之后 out.txt 是空的——查起来要跨三层。 */
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa,
                                     int fd, const char *path, int oflag, mode_t mode)
{
    (void)fa; (void)fd; (void)path; (void)oflag; (void)mode;
    errno = ENOSYS;
    return ENOSYS;
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd)
{
    (void)fa; (void)fd;
    errno = ENOSYS;
    return ENOSYS;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd, int newfd)
{
    (void)fa; (void)fd; (void)newfd;
    errno = ENOSYS;
    return ENOSYS;
}

int posix_spawnattr_init(posix_spawnattr_t *attr)
{
    (void)attr;
    return 0;
}

int posix_spawnattr_destroy(posix_spawnattr_t *attr)
{
    (void)attr;
    return 0;
}

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[], char *const envp[])
{
    (void)attrp;
    (void)envp;                 /* 环境变量还不能传给子进程（见 K1） */
    if (file_actions) {
        errno = ENOSYS;
        return ENOSYS;
    }
    if (!path || !argv) {
        errno = EINVAL;
        return EINVAL;
    }
    struct child *c = child_alloc();
    if (!c) {
        errno = EAGAIN;
        return EAGAIN;
    }

    /* 数一下 argv 的长度（fe_spawn 需要显式个数，不是 NULL 结尾） */
    u32 argc = 0;
    while (argv[argc] && argc < 32) {
        argc++;
    }

    long h = fe_spawn(path, (char *const *)argv, argc);
    if (h <= 0) {
        c->used = 0;
        errno = ENOENT;         /* 内核只给了"失败"这一个粒度；按最常见的原因报 */
        return errno;
    }
    c->handle = h;
    c->pid = g_next_pid++;
    if (pid) {
        *pid = c->pid;
    }
    return 0;
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[], char *const envp[])
{
    /* PATH 查找：我们的程序都在 /bin 与 /sbin（见 build.py 的布局）。
     * 没有 PATH 环境变量，所以这里是一张固定的搜索表——
     * 而不是"猜一个路径"或者"静默失败"。 */
    if (!file) {
        errno = EINVAL;
        return EINVAL;
    }
    if (file[0] == '/') {
        return posix_spawn(pid, file, file_actions, attrp, argv, envp);
    }
    static const char *dirs[] = { "/bin/", "/sbin/", NULL };
    char buf[128];
    for (int i = 0; dirs[i]; i++) {
        size_t dl = strlen(dirs[i]);
        size_t fl = strlen(file);
        if (dl + fl + 1 > sizeof(buf)) {
            continue;
        }
        memcpy(buf, dirs[i], dl);
        memcpy(buf + dl, file, fl);
        buf[dl + fl] = '\0';
        int rc = posix_spawn(pid, buf, file_actions, attrp, argv, envp);
        if (rc == 0) {
            return 0;
        }
    }
    errno = ENOENT;
    return ENOENT;
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    if (options & WNOHANG) {
        /* 非阻塞等待：内核没有 wait(nohang) 语义，所以这一条**如实不支持**。
         * 假装支持（立即返回 0 = "没有孩子退出"）会让轮询循环永远转下去。 */
        errno = ENOSYS;
        return -1;
    }
    struct child *c;
    if (pid <= 0) {
        /* waitpid(-1) / waitpid(0)：等任意孩子。取第一个还没收尸的。 */
        c = NULL;
        for (int i = 0; i < SPAWN_MAX; i++) {
            if (g_children[i].used && !g_children[i].reaped) {
                c = &g_children[i];
                break;
            }
        }
        if (!c) {
            errno = ECHILD;
            return -1;
        }
    } else {
        c = child_by_pid(pid);
        if (!c) {
            errno = ECHILD;
            return -1;
        }
        if (c->reaped) {
            errno = ECHILD;     /* 已经收过了：POSIX 要求报 ECHILD */
            return -1;
        }
    }

    int code = 0;
    long r = fe_wait(c->handle, &code);
    if (r != FE_OK) {
        errno = ECHILD;
        return -1;
    }
    c->reaped = 1;
    c->status = W_EXITCODE(code & 0xFF);
    fe_handle_close(c->handle);
    c->handle = 0;
    if (status) {
        *status = c->status;
    }
    return c->pid;
}

pid_t wait(int *status)
{
    return waitpid(-1, status, 0);
}
