/* SPDX-License-Identifier: 0BSD */
/* <unistd.h> / <fcntl.h> / <sys/stat.h> / <dirent.h> / <spawn.h> / <sys/wait.h>
 * / <stdio.h> —— 文件与进程接口（实现见 user/libposix/）。
 *
 * 这一组是"路线 B 的承重墙"：普通 C 程序几乎只用这几组接口读写文件、
 * 遍历目录、起子进程。它们的共同点是**用户态完全可以实现**：
 * 内核里没有任何文件 syscall，文件操作全部是 fsd 服务经端点提供的，
 * 所以 libposix 只要维护一张 fd 表就够了（见 docs/10-posix-layer.md §2）。
 */
#ifndef FE_POSIX_UNISTD_H
#define FE_POSIX_UNISTD_H

#include <stddef.h>
#include <sys/types.h>

/* ---- 标准 fd ---- */
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* ---- SEEK_* ---- */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int     close(int fd);
off_t   lseek(int fd, off_t offset, int whence);
int     dup(int fd);
int     dup2(int oldfd, int newfd);
int     pipe(int fds[2]);
int     access(const char *path, int mode);
int     unlink(const char *path);
int     rmdir(const char *path);
int     chdir(const char *path);
char   *getcwd(char *buf, size_t size);
int     isatty(int fd);
pid_t   getpid(void);
pid_t   getppid(void);
unsigned sleep(unsigned seconds);
int     usleep(unsigned usec);
int     fsync(int fd);

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

#endif /* FE_POSIX_UNISTD_H */
