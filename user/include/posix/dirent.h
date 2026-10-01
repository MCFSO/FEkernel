/* SPDX-License-Identifier: 0BSD */
/* <dirent.h> —— 目录遍历（见 docs/10-posix-layer.md）。
 *
 * ★ d_name 的长度上限与 fsd 的 FE_FS_NAME_MAX 一致，但要显式写出来 ★
 * POSIX 只保证 NAME_MAX ≥ 14；程序会按它开缓冲。
 * 我们把上限对齐到服务端能给的最大名字，短了会截断（症状是"某些文件读不到"），
 * 长了则白占每个目录流的内存。 */
#ifndef FE_POSIX_DIRENT_H
#define FE_POSIX_DIRENT_H

#include <sys/types.h>

#define NAME_MAX 64

struct dirent {
    ino_t d_ino;
    off_t d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[NAME_MAX + 1];
};

/* d_type 取值（POSIX 允许文件系统不支持时给 DT_UNKNOWN） */
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK    10

typedef struct __posix_dir DIR;

DIR           *opendir(const char *path);
struct dirent *readdir(DIR *d);
int            closedir(DIR *d);
void           rewinddir(DIR *d);

#endif /* FE_POSIX_DIRENT_H */
