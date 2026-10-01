/* SPDX-License-Identifier: 0BSD */
/* <sys/stat.h> —— 元数据（见 docs/10-posix-layer.md）。
 *
 * ★ 模式位的取值直接采用内核 VFS 那一套（S_IF*）★
 * 这不是"顺手"，是刻意的：内核的 ramfs/VFS 已经用 POSIX 的 S_IF* 取值
 * （见 kernel/include/fe/vfs.h），fsd 的 STAT 应答也照抄同一套。
 * 如果这里再翻译一次，就会出现"两套模式位"，而那种漂移的症状是
 * `S_ISDIR` 偶尔为假——查起来要跨内核/服务/库三层。 */
#ifndef FE_POSIX_SYS_STAT_H
#define FE_POSIX_SYS_STAT_H

#include <sys/types.h>

#define S_IFMT   0170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)

#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100
#define S_IRWXG 00070
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010
#define S_IRWXO 00007
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001

struct stat {
    dev_t    st_dev;
    ino_t    st_ino;
    mode_t   st_mode;
    nlink_t  st_nlink;
    uid_t    st_uid;
    gid_t    st_gid;
    dev_t    st_rdev;
    off_t    st_size;
    blksize_t st_blksize;
    blkcnt_t st_blocks;
    time_t   st_atime;      /* 没有 RTC：一切时间戳都是 0（见 docs/08 已知限制） */
    time_t   st_mtime;
    time_t   st_ctime;
};

int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);
int mkdir(const char *path, mode_t mode);
int chmod(const char *path, mode_t mode);

#endif /* FE_POSIX_SYS_STAT_H */
