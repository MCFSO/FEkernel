/* SPDX-License-Identifier: 0BSD */
/* <spawn.h> 与 <sys/wait.h> —— 进程（见 docs/10-posix-layer.md §4）。
 *
 * ★ 这里提供的是 posix_spawn，不是 fork ★
 * 理由写在 kernel/include/fe/process.h（"没有 fork"是设计决定）与
 * docs/10-posix-layer.md §4：需要的程序要的是"起个子进程并等它"，
 * 而不是"复制我自己再换映像"。先把前者做对，后者等有真实需求再评估。
 *
 * posix_spawn 的 file_actions 是我们**必须**支持的：
 * 没有它就没法做重定向（`prog > file`），而那是任何 shell/脚本的日常。
 */
#ifndef FE_POSIX_SPAWN_H
#define FE_POSIX_SPAWN_H

#include <sys/types.h>

typedef struct {
    int   __unused;
} posix_spawnattr_t;

typedef struct {
    int   __unused;
} posix_spawn_file_actions_t;

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[], char *const envp[]);
int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[], char *const envp[]);

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa,
                                     int fd, const char *path, int oflag, mode_t mode);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd, int newfd);

int posix_spawnattr_init(posix_spawnattr_t *attr);
int posix_spawnattr_destroy(posix_spawnattr_t *attr);

#endif /* FE_POSIX_SPAWN_H */
