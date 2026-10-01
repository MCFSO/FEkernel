/* SPDX-License-Identifier: 0BSD */
/* 受保护扇区区间（WRP/WFP 第 2 道防线的**策略**部分）。
 *
 * ★ 强制点是 blkd，不是内核。★
 * ATA 的端口读写同体，内核没办法让一个任务"能读设备但不能写它"——
 * blkd 要发读命令就必须能写命令寄存器。所以内核提供策略（哪些扇区、允许什么），
 * blkd 提供执行（每次读/写之前问一次）。内核拦不住一个不问的 blkd。
 *
 * 挡得住：被替换的 fsd、被替换的客户端、任何软件层的高权限程序。
 * 挡不住：被替换的 blkd、改写了清单的人、离线改盘。
 * 详见 docs/04-write-protection.md 与 docs/05-ab-update.md。
 *
 * ★ 保护是**单调**的：只有 add，没有 remove。★
 * 这带来一个意外的好处——"谁能登记保护"这个问题消失了：
 * 只能加不能减的接口，开放给任何任务都是安全的（最坏情况是某个任务
 * 多保护了一段扇区，那是让它自己更难写，不是攻击）。
 * 于是不需要为它设计能力对象，也就少了一层"谁是可信的"的判断。
 */
#ifndef FE_PROTECT_H
#define FE_PROTECT_H

#include <fe/types.h>
#include <fe/status.h>

#define FE_PROT_MAX_EXTENTS 256

/* 访问模式。可以按位或。 */
#define FE_PROT_DENY_WRITE (1u << 0)    /* 禁止写入（正在运行的那个槽） */
#define FE_PROT_DENY_READ  (1u << 1)    /* 连读都禁止（待更新的那个槽） */
#define FE_PROT_DENY_ALL   (FE_PROT_DENY_WRITE | FE_PROT_DENY_READ)
/* 只有**被内核指定的更新器**能突破这一段。
 *
 * ★ 豁免者必须由内核指定，不能由用户态自称 ★
 * 否则任何进程都能给自己发一张豁免令，整张表就形同虚设。
 * 指定的依据是清单里的 `updater <槽内相对路径>`：
 * 内核在创建进程时看到那个路径，就把该任务的 id 记为更新器。 */
#define FE_PROT_EXEMPT_UPDATER (1u << 2)

/* 清单里派生出来的 A/B 事实，供 init 与更新器使用。
 * 它们都来自同一份清单，所以不会出现"内核按 A 保护、init 以为在 B"这种不一致。 */
struct fe_ab_info {
    u64 misc_lba;       /* 槽状态记录所在 */
    u64 misc_count;
    u64 bootsel_lba;    /* "下次启动哪个槽"那个字节所在 */
    u64 bootsel_offset;
    u64 updater_task;   /* 被指定为更新器的任务 id；0 = 还没有进程匹配上 */
    u32 boot_slot;      /* 0 = A, 1 = B */
    u32 flags;          /* FE_AB_FLAG_*：与 user/include/fe_user.h 的值必须一致
                         * （这个结构体会原样交给用户态，见 FE_SYS_AB_INFO） */
};

#define FE_AB_FLAG_SLOT_STATE 0x1u  /* misc 位置有效：能读写槽状态记录 */
#define FE_AB_FLAG_BOOTSEL    0x2u  /* 引导控制块位置有效：能切槽 */
void fe_ab_get_info(struct fe_ab_info *out);

/* 违规记录的环形缓冲大小 */
#define FE_PROT_VIOLATION_LOG 16

struct fe_protect_violation {
    u64 task_id;    /* 是谁尝试的 */
    u64 lba;
    u64 count;
    u64 tick;       /* 发生时刻（节拍） */
    u32 mode;
    u32 write;      /* 1 = 写，0 = 读 */
};

/* 对外的区间描述（供 PROTECT_LIST 系统调用把表交给用户态检查） */
struct fe_protect_info {
    u64 lba;
    u64 count;
    u64 exempt_task;    /* 0 = 谁都不豁免 */
    u32 mode;
    u32 _pad;
};

void fe_protect_init(void);

/* 登记一段受保护的扇区区间 [lba, lba+count)。
 *
 * exempt_task 非 0 时，只有该任务能在这一段上突破 mode 的限制。
 * 与已有区间重叠返回 FE_ERR_EXIST（表里永远是一组互不相交的区间，
 * 检查因此可以是简单的线性扫描）。表满返回 FE_ERR_NOSPC。 */
fe_status_t fe_protect_add(u64 lba, u64 count, u32 mode, u64 exempt_task);

/* 访问检查。is_write 为真表示写、假表示读；requester 是**请求方任务 id**
 * （由内核在 IPC 投递时填写，服务不可伪造）。
 *
 * 返回 FE_OK 或 FE_ERR_ACCESS。与**调用者**是谁无关——
 * 不看谁在问，只看"请求方是否被豁免"。这样"谁能碰哪些扇区"
 * 就完全由表决定，没有第二条判断路径可以被绕过。 */
fe_status_t fe_protect_check(u64 lba, u64 count, bool is_write, u64 requester);

/* 解析 A/B 清单文本并登记。
 *
 * 清单由 mkfat.py 在建镜像时生成（见 docs/05-ab-update.md），
 * 语法：`slot <a|b> <lba> <count>` / `misc <lba> <count>` / `bootsel <lba> <off>`。
 *
 * 规则由 boot_slot 决定（内核从 cmdline 里读到的槽）：
 *   - **正在运行的那个槽 → DENY_WRITE**（读允许）
 *   - **另一个槽 → DENY_READ|DENY_WRITE**（连读都不行，见访问矩阵）
 * misc 与 bootsel **不保护**：它们是"下次启动谁"的投票，必须可写。
 *
 * 注意这里**不是在内核里发明策略**：策略来自清单（一份随引导映像来的文件），
 * 内核只负责把它翻成表。清单未认证这件事是已知边界，见文档。 */
u32 fe_protect_load_manifest(const char *text, u64 len, char boot_slot);

/* 内核在创建进程时调用：路径正好是被指定的更新器路径，就把该任务记为更新器。
 * 放在这里而不是让用户态自报，是因为"谁能突破保护"这件事
 * 只能由拿着清单的那一方决定。 */
void fe_protect_note_process(const char *path, u64 task_id);

bool fe_protect_active(void);

u64 fe_protect_extent_count(void);
u64 fe_protect_violation_count(void);
u32 fe_protect_get_violations(struct fe_protect_violation *out, u32 cap);
/* 导出区间表（供用户态的检查程序核对访问矩阵） */
u32 fe_protect_list(struct fe_protect_info *out, u32 cap);

void fe_protect_dump(void);
u32 fe_selftest_protect(void);

#endif /* FE_PROTECT_H */
