/* SPDX-License-Identifier: 0BSD */
/* 硬件资源池（M6）。
 *
 * 微内核里「谁能碰硬件」必须由内核裁决：端口位图、MMIO 页表项、IRQ 路由这三样
 * 都是内核直接掌握的手段，所以资源池也在内核里。
 *
 * 为什么做成「池」而不是「开关」：资源是**有限且互斥**的。
 * 同一段端口区间不能被两个驱动同时认领——这才叫能力。如果只是「申请就给」，
 * 那不过是权限检查，两个驱动会互相踩，而且踩了之后没有任何一方能发现。
 *
 * 池子的内容在引导期由内核放入；将来设备管理器服务出现后，由它决定把哪一段
 * 授予哪个驱动，内核仍然握有最终否决权（见 docs/03-protection.md 的分层说明）。
 */
#ifndef FE_RESOURCE_H
#define FE_RESOURCE_H

#include <fe/types.h>
#include <fe/status.h>
#include <fe/syscall.h>

/* 资源类别常量由 ABI 头 fe/syscall.h 提供（FE_RES_IOPORT / FE_RES_MMIO / FE_RES_IRQ）：
 * 用户态也要用同一组数值调用 RESOURCE_LOCK，而 syscall.h 是唯一两边共享的头文件。
 * 在这里另立一个 enum 会和那组宏撞名。 */

/* 空闲区间表与已认领表各这么大。区间会被切分，所以实际能容纳的认领数
 * 比这个数字小——但 64 个条目对「几十个驱动服务」的规模绰绰有余。 */
#define FE_RES_MAX 64

/* 一个共享区间最多允许几个持有者。
 * 为什么需要「共享」这一类：有些硬件的寄存器**物理上就分不开**。
 * 典型例子是 PS/2 的 8042 控制器：键盘的中断是 IRQ1、鼠标是 IRQ12（天然分开），
 * 但两者的数据口都是 0x60、命令口都是 0x64——同一个寄存器对。
 * 所以「键盘服务和鼠标服务各自认领自己的设备」这件事，只能落到
 * 「中断各自独占、端口共享」这个组合上。 */
#define FE_RES_SHARERS_MAX 4

void fe_resource_init(void);

/* 把一段硬件资源放进可分配池（独占：认领后从池中移除，同一时刻只有一个持有者）。
 * 区间重叠会被拒绝（池子永远是一组互不相交的区间）。 */
fe_status_t fe_resource_pool_add(u32 kind, u64 base, u64 len);

/* 放进池子但标记为**可共享**：认领后仍留在池中，允许多个持有者。
 * 只应该用在硬件本身就无法切分的区间上——共享意味着内核不再保证互斥，
 * 用错的代价是两个驱动互相踩（PS/2 那种必须靠控制器锁来串行化）。 */
fe_status_t fe_resource_pool_add_shared(u32 kind, u64 base, u64 len);

/* 认领 [base, base+len)。成功即从池中扣掉这一段（必要时把池中的大区间切成两段），
 * 并记在 owner 名下。失败：
 *   FE_ERR_BUSY  该区间已被独占认领（别人先拿到了）
 *   FE_ERR_EXIST 自己已经持有这一段
 *   FE_ERR_NOENT 该区间不在池子里（内核从未打算把它交给任何驱动）
 *   FE_ERR_NOSPC 条目用尽
 *   FE_ERR_RANGE 超出该类资源的合法范围 */
fe_status_t fe_resource_claim(u32 kind, u64 base, u64 len, u64 owner_id);

/* ---- 共享区间上的控制器锁 ----
 *
 * 用途：把「读状态 → 写命令 → 写数据」这一串端口访问串起来。
 * PS/2 控制器要求这套序列不能被打断，而两个服务各自持有端口权限时
 * 调度器随时可能把其中一个换下去。
 *
 * ★ 这是**协作锁**，不是安全边界。★
 * 内核无法让一串用户态的端口访问变成原子操作；一个不守规矩的驱动
 * 完全可以不申请锁就动手。它防的是「两个都守规矩的服务撞在一起」，
 * 也就是实际会发生的那类问题。安全边界是 I/O 权限位图，那是另一回事。 */
fe_status_t fe_resource_lock(u32 kind, u64 base, u64 len, u64 owner_id);
fe_status_t fe_resource_unlock(u32 kind, u64 base, u64 len, u64 owner_id);

/* 任务销毁时把它名下的资源全部归还，并合并相邻空闲区间 */
void fe_resource_release_owner(u64 owner_id);

/* ------------------------------------------------------------------ */
/* 设备管理器（DM）能力                                                */
/* ------------------------------------------------------------------ */

/* ★ 为什么是"指定的那一个任务"，而不是一个权限位 ★
 *
 * `RESOURCE_POOL_ADD` 能把硬件放进池子——如果任何任务都能调，资源池就没了。
 * 而"谁能当设备管理器"这个问题不该由内核里的规则回答（比如"名字叫 pcid 的
 * 任务"），那样内核就开始理解策略了。
 *
 * 做法与 A/B 更新器完全一致（见 fe_protect_note_process）：**内核在启动流程里
 * 指定一次**，之后：
 *   - 只有这个任务能往池子里加硬件；
 *   - 任务退出时这个身份随之失效（不留悬空授权）。
 * 谁来当由引导链决定（今天是 init 拉起 pcid/devmgr 时指定），
 * 内核只记"是它"，不问"为什么是它"。
 *
 * 注意它与"认领"的区别：DM 加资源不等于持有资源——它只是**申报存在**，
 * 之后仍要按常规路径认领（或授权给别人）。 */
void fe_resource_set_devmgr(u64 task_id);
bool fe_resource_is_devmgr(u64 task_id);
void fe_resource_clear_devmgr(u64 task_id);

/* ★ 设备管理器的身份由**引导者自己认领** ★
 *
 * 内核不认识"devmgr"这个名字（那是策略）；它只需要回答一个更小的问题：
 * **谁是引导者**。答案是 init —— 内核 exec 的第一个用户任务，
 * 这一点内核本来就知道，所以 `fe_resource_note_init` 由
 * `fe_process_start_init` 调一次，之后：
 *   - 只有 init 自己能认领设备管理器身份（`fe_resource_claim_devmgr`）；
 *   - 身份唯一，认领过就再认领不到；
 *   - init 退出时随任务销毁自动失效（走 fe_resource_clear_devmgr）。
 *
 * 这样"谁来申报 BAR"这件事仍然是**引导链的策略**（今天由 init 自己认领
 * 再把活交给 pcid，将来换独立 devmgr 服务只改 init 的表），而内核里
 * 没有多出任何一条需要维护的"特殊名字"规则——与 05-ab-update 的
 * 更新器身份（fe_protect_note_process）是同一条做法。 */
void fe_resource_note_init(u64 task_id);
bool fe_resource_claim_devmgr(u64 task_id);


/* 把 owner 名下的 [base, base+len) 转给 new_owner。
 * 用于设备管理器把设备交给驱动：转移之后只有新持有者能继续使用这段资源，
 * 原持有者失去它（**不是复制**——硬件的所有权唯一）。 */
fe_status_t fe_resource_grant(u32 kind, u64 base, u64 len,
                              u64 from_owner, u64 to_owner);

/* 诊断 */
u32  fe_resource_owned_count(u64 owner_id);
u32  fe_resource_pool_count(void);
bool fe_resource_is_shared(u32 kind, u64 base, u64 len);
void fe_resource_dump(void);

/* ---- 只读访问器：共享区间控制器锁的持有者 ----
 *
 * ★ 为什么需要它（而不是看 fe_resource_dump 的打印）★
 * `g_pool` 是 kernel/resource.c 里的 static，池外读不到；而 dump 只打印，
 * 打印出来的东西**不能当断言判据**。D4 的断言①（持锁线程死透之后 holder
 * 必须是 NULL）与 D2① 的反向对照都要把 `lock_holder` 读成一个**可读的事实**。
 * 它只看不写：不改变锁的状态、不唤醒任何人。
 *
 * 区间不属于共享条目（或根本不在池子里）时返回 NULL。 */
struct fe_thread;
struct fe_thread *fe_resource_lock_holder(u32 kind, u64 base, u64 len);

/* 自检：返回失败项数（0 = 全部通过） */
u32 fe_selftest_resource(void);

/* 自检：设备管理器（申报硬件 / 授权分发 / 越权被拒）。返回失败项数。 */
u32 fe_selftest_devmgr(void);

/* 自检：PCI 配置空间（机制 #1 的端口对：认领、独占、真读一次）。 */
u32 fe_selftest_pci(void);

#endif /* FE_RESOURCE_H */
