/* SPDX-License-Identifier: 0BSD */
/* devfs —— 「名字 → 端点能力」的文件系统，VFS 的一个后端。
 *
 * 这是「服务发现」的**全部**实现方式：不建注册表服务，不搞查找协议，
 * 就是一个挂在 /dev 上的文件系统。BSD 的做法：命名空间就是文件系统
 * （FreeBSD 的 devfs 把设备暴露成 /dev 下的节点，Unix 的服务发布成命名套接字）。
 *
 * 为什么不做成平行的「名字 → 句柄」注册表：
 *   - devfs 节点是 VFS 里的一等公民，天然可列举（ls /dev 与 ls /mnt 走同一个 readdir）、
 *     可挂载、可被将来的 WRP 按 inode 施加权限；
 *   - 平行的注册表这三样都得重新造一遍，而且会多出一个与 VFS 语义不总一致的命名空间。
 *
 * 一条安全性质值得单独说：**同名发布会被拒绝（FE_ERR_EXIST）**。
 * 否则任何服务都能抢注 /dev/blk0 把真正的块设备服务顶掉，
 * 「按路径找服务」立刻退化成「谁先注册谁说了算」。
 * 认领硬件用的是资源池的互斥语义，认领名字用的是这里的互斥语义，两者一致。
 */
#ifndef FE_DEVFS_H
#define FE_DEVFS_H

#include <fe/types.h>
#include <fe/status.h>
#include <fe/syscall.h>

struct fe_task;

#define FE_DEVFS_MAX_ENTRIES 16
#define FE_DEVFS_MOUNT_POINT "/dev"

/* 建表并把自己挂到 /dev。必须在 fe_vfs_init() 之后调用。 */
fe_status_t fe_devfs_init(void);

/* 把一个端点发布到 path（形如 /dev/blk0）。
 * 端点必须是 owner 句柄表里一个带 FE_RIGHT_TRANSFER 的有效句柄——
 * 「能把它交出去」是一种权限，不是默认就有的。
 * 名字已被占用返回 FE_ERR_EXIST；名字不合法返回 FE_ERR_INVAL。 */
fe_status_t fe_devfs_publish(struct fe_task *owner, const char *path,
                             fe_handle_t endpoint);

/* 按路径打开：把该服务端点的句柄装进 opener 的句柄表。
 * 拿到的是**一条通往该服务的通道**，不是服务本身的端点对象。 */
fe_status_t fe_devfs_open(struct fe_task *opener, const char *path,
                          fe_handle_t *out_handle);

/* 发布者任务销毁时收回它的名字（否则名字会永远占着，服务重启就注册不回来） */
void fe_devfs_release_owner(u64 owner_task_id);

/* 诊断与自检 */
void fe_devfs_dump(void);
u32  fe_selftest_devfs(void);

#endif /* FE_DEVFS_H */
