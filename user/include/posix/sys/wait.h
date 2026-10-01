/* SPDX-License-Identifier: 0BSD */
/* <sys/wait.h> —— 等子进程（见 docs/10-posix-layer.md §4）。 */
#ifndef FE_POSIX_SYS_WAIT_H
#define FE_POSIX_SYS_WAIT_H

#include <sys/types.h>

/* ★ 状态字必须用 POSIX 的**位布局**，不能自定义 ★
 * 程序里到处是 `WIFEXITED(status)`、`WEXITSTATUS(status)`，
 * 还有直接 `status == 0` 的写法（shell 脚本里尤其多）。
 * 位布局一旦不同，"退出码 0"与"被信号杀死"就会互相伪装。 */
#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) & 0xFF00) >> 8)
#define WIFSIGNALED(s) (((s) & 0x7F) != 0 && ((s) & 0x7F) != 0x7F)
#define WTERMSIG(s)    ((s) & 0x7F)
#define WIFSTOPPED(s)  (((s) & 0xFF) == 0x7F)
#define WSTOPSIG(s)    WEXITSTATUS(s)
#define WCOREDUMP(s)   ((s) & 0x80)

/* 把"退出码"包成状态字：WIFEXITED 为真、WEXITSTATUS 等于 code。
 * 反过来说，从 waitpid 拿到的状态字也要能这样拆开——
 * 我们的内核 fe_wait 直接给退出码，所以这一层做的是**翻译**，
 * 而翻译必须是一一对应的：两个不同的退出码不能映射到同一个状态字。 */
#define W_EXITCODE(code) ((((code) & 0xFF) << 8))

#define WNOHANG   1
#define WUNTRACED 2

pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);

#endif /* FE_POSIX_SYS_WAIT_H */
