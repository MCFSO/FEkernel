/* SPDX-License-Identifier: 0BSD */
/* 外部中断投递（M6）：把硬件中断变成「通知对象上的一位」。
 *
 * 为什么是通知对象而不是回调函数：微内核里**内核不执行驱动的代码**。
 * 中断到达时内核只做三件事——置位、唤醒、决定要不要暂时屏蔽——剩下的全部由
 * 驱动服务在自己的线程里处理。回调式（Linux 的 request_irq）会让驱动代码
 * 在内核栈上跑，那是宏内核的做法，我们不抄。
 *
 * 电平触发的中断必须「屏蔽到确认」：设备拉住中断线之后如果没人处理，
 * 中断会立刻重来，CPU 会被同一个中断淹没。边沿触发没有这个问题。
 * 所以电平线投递后立刻在 IOAPIC 里屏蔽，驱动处理完调用 fe_irq_ack 才放行。
 */
#ifndef FE_IRQ_H
#define FE_IRQ_H

#include <fe/types.h>
#include <fe/status.h>

struct fe_task;
struct fe_notification;

#define FE_IRQ_NONE 0xFFFFFFFFu

/* 最多同时登记的 IRQ 数；与 FE_VEC_GSI_COUNT 对齐 */
#define FE_IRQ_MAX 32

/* ---- 触发方式与极性（D5b）----
 *
 * ★ 为什么要让调用者能**声明**要求，而不只是"事后能查" ★
 * 报告是事后知情，声明是事前拒绝。对中断来说，带着错的触发方式跑比直接失败
 * 更糟：它不会立刻崩，只会在负载高的时候**偶发丢事件**——而那种症状会被
 * 归因到驱动自己的逻辑上，排查代价极高。 */
#define FE_IRQ_MODE_EDGE        1u
#define FE_IRQ_MODE_LEVEL       2u
#define FE_IRQ_MODE_ACTIVE_LOW  4u
#define FE_IRQ_MODE_ANY         0u      /* 不要求（默认） */

/* ★ "不要为我屏蔽这条线"（bit 3）★
 *
 * 电平线投递后会**屏蔽到所有需要确认的登记者都确认**为止——那是防中断
 * 风暴的（线还高着就会被立刻重投）。但那个保护**不是每个驱动都需要**：
 * 一个"用轮询判定完成、只把中断当提示"的驱动（今天的 `blkd` 就是：
 * 它读 used ring 判完成、顺手读 ISR 寄存器清设备条件，但从不 ack）
 * 加上屏蔽之后会**再也收不到提示**——第一次投递之后线就永远屏蔽着。
 *
 * 所以把"要不要这个保护"交给登记者自己声明：
 *   - 默认（不带这一位）：我在中断处理里清设备，**必须** ack；
 *   - 带这一位：我靠轮询判定完成，**不要**因为我而屏蔽这条线。
 *
 * 一条线上只要**有任何一个**登记者需要屏蔽，线就会被屏蔽——保护是
 * 线级别的，声明是登记者级别的。没人需要时线从不屏蔽，
 * 那时的语义就与边沿一致（安全的责任落在"设备条件会不会被清掉"上）。 */
#define FE_IRQ_F_NO_MASK        8u

/* 一条线的实情（登记成功后回填，也可随时查询）。 */
struct fe_irq_info {
    u32 gsi;
    u8  vector;
    u8  _pad[3];
    u32 mode;               /* FE_IRQ_MODE_* */
    u32 sharers;            /* 当前有几个登记者 */
};

void fe_irq_init(void);

/* 登记一个 IRQ，交给 nt 投递。
 *
 * irq 是**中断号**而不是 GSI：IRQ0..15 是传统 ISA 编号，注册时按 ACPI 的
 * 中断源覆盖表换算成 GSI 与极性/触发方式（这是踩过的坑：系统里有 IOAPIC 时
 * IRQ0 走的是 GSI 2，直接按号对应是错的）。
 *
 * 投递时置位通知对象的第 (1 << irq) 位，因此 irq 必须 < 64。
 * owner_id 非 0 时，该资源可以在任务销毁时自动归还。
 *
 * ★ 共享（D5a）★ 同一个 irq 可以被最多 FE_IRQ_SHARERS_MAX 个**不同 owner**
 * 登记——但**只有电平触发**的线允许，边沿线共享会丢中断（见
 * docs/14-interrupt-semantics.md §2.1）。共享者必须接受**伪唤醒**：
 * 线不携带"是哪台设备"，所以每次被叫醒都要问自己的设备"是你吗"。
 *
 * require_mode 非 0 时，触发方式不匹配就拒绝登记。 */
fe_status_t fe_irq_register(u32 irq, struct fe_notification *nt, u64 owner_id);
fe_status_t fe_irq_register_ex(u32 irq, struct fe_notification *nt, u64 owner_id,
                               u32 require_mode, struct fe_irq_info *out_info);

/* 确认一个电平触发的中断：重新放行该中断线。
 *
 * ★ 共享时"谁在确认"必须能回答 ★ 放行的条件是**所有登记者都确认过**
 * （欠账计数归零），所以带 owner 的这个入口才是正路；
 * 不带 owner 的 `fe_irq_ack` 在共享线上返回 FE_ERR_INVAL。 */
fe_status_t fe_irq_ack(u32 irq);
fe_status_t fe_irq_ack_owner(u32 irq, u64 owner_id);

/* 查一条线的实情（GSI / 向量 / 触发方式 / 共享者数）。 */
bool fe_irq_get_info(u32 irq, struct fe_irq_info *out);

/* 声明的触发方式与这条线的实情相符吗。
 * 单独提出来是为了让调用者**先校验再动状态**——参数错误该先于状态
 * 检查报出来（见 irq.c 里的说明）。 */
bool fe_irq_mode_ok(u32 irq, u32 require_mode);

/* ---- MSI / MSI-X（D5c）----
 *
 * ★ 它解决的是 INTx 共享解决不了的那个问题 ★
 * 共享的 INTx 线上，"是哪台设备"内核答不出来，所以每个驱动都得接受
 * 伪唤醒、都得去问自己的设备。MSI 是**每台设备一条独立消息**，
 * 身份由向量本身携带——于是既没有共享、也没有电平/边沿的问题
 * （消息写天然是边沿语义，不需要 ack）。真实驱动默认走它。
 *
 * ★ 分工 ★ 发现能力结构是**用户态**的事（PCI 协议知识，是策略）；
 * "分配向量 + 算出消息地址/数据 + 绑定通知对象"是**内核**的事
 * （向量是内核的资源，而且分配向量与让设备会发消息必须原子）。
 * MSI-X 的表项由**驱动**自己写——表在它的 BAR 里，内核不该去映射
 * 一段属于别人的 BAR。
 *
 * ★ 安全边界 ★ 驱动拿到 (addr,data) 之后确实可以给别人制造伪中断，
 * 但共享 INTx 下它本来就能（内核无法分辨是哪台设备）。**这不是新能力。** */

#define FE_MSI_KIND_MSI   1u
#define FE_MSI_KIND_MSIX  2u

struct fe_msi_info {
    u32 irq;            /* 分配到的**伪中断号**：通知位是 (1 << irq)，取值 32..63 */
    u8  vector;
    u8  kind;           /* FE_MSI_KIND_* */
    u8  _pad[2];
    u32 table_bar;      /* MSI-X：表在哪个 BAR（0..5）；MSI 为 0xFF */
    u32 table_offset;   /* MSI-X：表相对该 BAR 的字节偏移 */
    u32 data;           /* 消息数据（= 向量 | 边沿 | assert） */
    u64 addr;           /* 消息地址（LAPIC 的 MSI 窗口 | 目的 APIC ID） */
};
/* 用户态按 32 字节读这一份（见 user/include/fe_user.h 的镜像定义）。 */
_Static_assert(sizeof(struct fe_msi_info) == 32,
               "fe_msi_info 布局变了：用户态的镜像定义也要改");

/* 为一台设备使能 MSI / MSI-X，并把中断绑到 nt。
 * cap_off 是能力结构在配置空间里的偏移（由设备管理器发现并传下来）；
 * flags 是 MSI-X 的**表项号**（virtio 的队列 0 用 0；MSI 忽略它）。 */
fe_status_t fe_irq_msi_alloc(u64 bus, u64 dev, u64 fn, u64 cap_off, u32 flags,
                             struct fe_notification *nt, u64 owner,
                             struct fe_msi_info *out);

/* 关掉一台设备的 MSI 并释放它占的向量与伪中断号。 */
fe_status_t fe_irq_msi_free(u32 irq, u64 owner);

/* 任务销毁时归还它登记的 IRQ（屏蔽中断线并释放对象引用） */
void fe_irq_release_owner(u64 owner_id);

/* 诊断 */
u32  fe_irq_owned_count(u64 owner_id);
u64  fe_irq_count(u32 irq);
void fe_irq_dump(void);

/* 自检：返回失败项数（0 = 全部通过） */
u32 fe_selftest_irq(void);

#endif /* FE_IRQ_H */
