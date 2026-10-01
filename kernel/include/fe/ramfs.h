/* SPDX-License-Identifier: 0BSD */
/* ramfs —— 由引导模块构成的内存文件系统（VFS 的一个后端）。
 *
 * 为什么要有这一层：还没有磁盘文件系统时，根文件系统由引导器交进来的文件构成，
 * 这正是 Linux 的 initramfs。等 FAT32 就绪，把它挂到 /mnt 即可，
 * 「按路径 exec」这套语义完全不用改——因为上层只认 VFS，不认后端。
 */
#ifndef FE_RAMFS_H
#define FE_RAMFS_H

#include <fe/types.h>
#include <fe/status.h>

/* 从引导模块构建内存文件树，并把自己挂到 "/"。
 * 必须在 fe_vfs_init() 之后调用。 */
fe_status_t fe_ramfs_init(void);

#endif /* FE_RAMFS_H */
