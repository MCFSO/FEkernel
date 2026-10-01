/* SPDX-License-Identifier: 0BSD */
/* 内核对象模型。
 *
 * 微内核的一切能力都通过「对象 + 句柄」表达：
 *   - 内核对象（端点/通知/内存对象/任务）都带一个统一的头部，提供类型标签与引用计数；
 *   - 进程只能通过句柄表的间接引用访问对象，且句柄带有**权限位**；
 *   - 对象被引用时引用计数加一，句柄关闭时减一，归零即销毁。
 *
 * 这样「谁能对谁做什么」就完全由句柄表决定，内核不需要理解任何策略。
 */
#ifndef FE_OBJECT_H
#define FE_OBJECT_H

#include <fe/types.h>
#include <fe/compiler.h>
#include <fe/syscall.h>

enum fe_object_type {
    FE_OBJ_NONE = 0,
    FE_OBJ_ENDPOINT,
    FE_OBJ_NOTIFICATION,
    FE_OBJ_MEMORY,
    FE_OBJ_TASK,
    FE_OBJ_THREAD,
};

const char *fe_object_type_name(u32 type);

/* 所有内核对象的公共头部，必须位于结构体最前面 */
struct fe_object_header {
    u32 type;
    volatile u32 refcount;
    u64 id;                 /* 全局唯一编号，诊断用 */
};

void fe_object_init(struct fe_object_header *hdr, u32 type);
void fe_object_ref(struct fe_object_header *hdr);
void fe_object_unref(struct fe_object_header *hdr);     /* 归零时按类型销毁 */

/* 把头部还原成具体对象（仅做类型断言，不做运行期检查） */
#define FE_OBJ_OF(ptr, type) ((type *)(void *)(ptr))

/* ------------------------------------------------------------------ */
/* 句柄表                                                              */
/* ------------------------------------------------------------------ */

#define FE_HANDLE_TABLE_SIZE 256

/* 句柄权限位 */
#define FE_RIGHT_READ     (1u << 0)
#define FE_RIGHT_WRITE    (1u << 1)
#define FE_RIGHT_EXEC     (1u << 2)
#define FE_RIGHT_SEND     (1u << 3)   /* 向端点发消息 */
#define FE_RIGHT_RECV     (1u << 4)   /* 从端点收消息 */
#define FE_RIGHT_SIGNAL   (1u << 5)   /* 置位通知 */
#define FE_RIGHT_WAIT     (1u << 6)   /* 等待通知 */
#define FE_RIGHT_DUP      (1u << 7)   /* 允许复制该句柄 */
#define FE_RIGHT_TRANSFER (1u << 8)   /* 允许随消息传递给别的任务 */
/* 终止一个任务（K2）。
 *
 * ★ 为什么"杀"是一位权限而不是一个特殊接口 ★
 * 它是**破坏性**操作，所以判据必须是不可伪造的能力。名字可以改、id 会复用
 * （见 05-ab-update 里"身份随退出失效"要防的同一件事），
 * 所以只能是"句柄 + 权限位"。
 *
 * 谁天然拥有它：`fe_process_spawn` 把子任务句柄装进父进程句柄表时带上它
 * ——**拉起者可以终止它拉起的东西**，这正是"系统自管"需要的形状
 * （init 拉起服务，服务跑飞了，init 终止它）。
 * 它也能经 TRANSFER 转交（那是一条正常的授权），但不能凭空获得。 */
#define FE_RIGHT_TERMINATE (1u << 9)
#define FE_RIGHT_ALL      0x3FFu

struct fe_handle_entry {
    struct fe_object_header *object;
    u32 rights;
    u32 pad;
};

struct fe_handle_table {
    struct fe_handle_entry entries[FE_HANDLE_TABLE_SIZE];
    u32 used;
};

void fe_handle_table_init(struct fe_handle_table *t);

/* 安装一个新句柄；失败返回 FE_HANDLE_INVALID */
fe_handle_t fe_handle_install(struct fe_handle_table *t, struct fe_object_header *obj,
                              u32 rights);

/* 按权限要求查找对象；权限不足返回 FE_ERR_ACCESS，句柄无效返回 FE_ERR_BADHANDLE */
fe_status_t fe_handle_lookup(struct fe_handle_table *t, fe_handle_t h, u32 required_rights,
                             struct fe_object_header **out);

/* 只取权限位（不要求任何权限）。
 *
 * 用途：把「某个句柄当前有什么权限」作为一个**事实**读出来——
 * 例如随消息转交能力时，要按发送方手里的权限去收窄接收方的权限。
 * 它不构成绕过：拿到权限位不等于拿到对象，任何实际使用仍要走 lookup。 */
fe_status_t fe_handle_rights(struct fe_handle_table *t, fe_handle_t h, u32 *out_rights);

/* 关闭句柄（释放引用） */
fe_status_t fe_handle_close(struct fe_handle_table *t, fe_handle_t h);

/* 复制句柄（权限只能收窄，不能放大） */
fe_status_t fe_handle_dup(struct fe_handle_table *t, fe_handle_t h, u32 new_rights,
                          fe_handle_t *out);

/* 关闭整张表（任务退出时调用） */
void fe_handle_table_clear(struct fe_handle_table *t);

u32 fe_handle_table_used(const struct fe_handle_table *t);

#endif /* FE_OBJECT_H */
