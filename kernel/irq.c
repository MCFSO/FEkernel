/* SPDX-License-Identifier: 0BSD */
/* 外部中断投递的实现（见 irq.h 的设计说明）。
 *
 * 中断到达时的处理顺序是有讲究的，错一步就会出现「中断风暴」或「丢中断」：
 *   电平触发：先屏蔽中断线 → 再 EOI → 再置位通知
 *   边沿触发：直接 EOI → 再置位通知
 * 电平线之所以要先屏蔽：EOI 只是告诉 LAPIC「这次中断处理完了」，设备那条线还拉着，
 * 若此时线是通的，LAPIC 会立刻再投递一次，驱动还没跑到就被淹没了。
 */
#include <fe/irq.h>
#include <fe/vectors.h>
#include <fe/ipc.h>
#include <fe/idt.h>
#include <fe/regs.h>
#include <fe/io.h>
#include <fe/string.h>
#include <fe/kprintf.h>
#include <fe/object.h>
#include <fe/task.h>
#include <fe/acpi.h>
#include <fe/arch/ioapic.h>
#include <fe/arch/lapic.h>
#include <fe/pic.h>
#include <fe/mm.h>
#include <fe/mm/pmm.h>
#include <fe/mm/vmm.h>

/* ★ 一根线可以有多个登记者（D5a）★
 *
 * 为什么：两台 PCI 设备接在同一条 INTx 上时，第二个驱动今天起不来
 * （`FE_ERR_BUSY`）。QEMU 上第二台 virtio 设备**就是**同一条 IRQ11。
 *
 * 三条约束决定了这个结构的形状：
 *   1. **线本身不携带"是哪台设备"**——所以内核无法挑一个登记者，
 *      只能**全都置位**，由驱动去问自己的设备（PCI 共享中断的标准做法）；
 *   2. **只有电平线能共享**：边沿线在"正在处理 A 的中断"期间 B 的跳变
 *      无法被检测到，那一次中断就永久丢了。这是边沿语义本身的性质，
 *      不是实现问题。所以边沿线的第二个登记者一律被拒（见 §2.1）；
 *   3. **电平线要"所有人都确认"才放行**：先确认的那个若立刻放行，
 *      而线还高着（另一台设备没被服务），就会立刻再投递一次——
 *      中断风暴。所以 `pending` 是**计数**，不是布尔。 */
#define FE_IRQ_SHARERS_MAX 4

struct fe_irq_entry {
    u32  irq;               /* 中断号（不是 GSI） */
    u32  gsi;
    u64  count;             /* 投递次数（整条线） */
    u8   vector;
    bool level;
    bool active_low;
    bool masked;            /* 因等待确认而屏蔽 */
    bool used;
    /* --- 共享（D5a）--- */
    u32  nsharer;
    u32  pending;           /* 还欠几次确认；归零才放行 */
    struct {
        u64 owner;
        struct fe_notification *nt;
        u64 count;          /* 这家自己收到多少次（诊断：能看出谁没被叫到） */
        /* ★ 这家还欠不欠一次确认 ★
         * 必须**逐家**记，不能只看总数：退出的人若已经确认过，
         * 它的离开不该再从 pending 里减一次——那会让线在"还有人在欠账"
         * 的情况下被错误放行，而线还高着 → 立刻重投 → 中断风暴。
         * 这个 bug 是自检抓出来的（见 fe_selftest_irq 的"欠账的那家退出"）。 */
        bool owes;
        /* 这只一种"这家要求屏蔽到确认吗"的记账 */
        bool need_ack;
    } sh[FE_IRQ_SHARERS_MAX];
    /* ---- MSI / MSI-X（D5c）----
     * MSI 条目没有 IOAPIC 路由，所以 gsi 记 FE_IRQ_NONE——
     * 屏蔽/放行那些动作对它一律**不适用**（见 release 里的判据）。
     * 设备位置要记下来：任务退出时得替它关掉设备的 MSI 使能位。 */
    bool is_msi;
    u32  msi_bus, msi_dev, msi_fn, msi_cap;
};

static struct fe_irq_entry g_irq[FE_IRQ_MAX];
static u8 g_vec_slot[256];      /* 向量 → g_irq 下标；0xFF = 未分配 */

/* MSI 用到它们，而定义在文件后面（归还路径要替设备关掉使能位）。 */
static u32 msi_irq_alloc(void);
static void msi_disable_device(u32 bus, u32 dev, u32 fn, u32 cap);

static struct fe_irq_entry *find_irq(u32 irq)
{
    for (u32 i = 0; i < FE_IRQ_MAX; i++) {
        if (g_irq[i].used && g_irq[i].irq == irq) {
            return &g_irq[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 中断处理程序                                                        */
/* ------------------------------------------------------------------ */

static void gsi_isr(struct fe_regs *r)
{
    u32 vec = (u32)r->vector;
    if (vec > 255) {
        return;
    }
    u8 slot = g_vec_slot[vec];
    if (slot == 0xFF) {
        /* 向量上没有任何登记：屏蔽源头，否则会一直响。
         * 这里不去猜是谁，也不打印（中断里打印会拖垮系统）。 */
        return;
    }
    struct fe_irq_entry *e = &g_irq[slot];

    if (e->level) {
        /* 电平触发：必须在 EOI 之前屏蔽，见文件头说明。
         * 共享时**所有需要确认的登记者**都欠一次确认（pending = 这个数），
         * 归零才放行——先确认的那个若立刻放行，线还高着就会立刻重来。
         * 声明了 NO_MASK 的登记者不计入：它靠轮询判定完成、不会 ack，
         * 计入就等于这条线永远放不开（见 FE_IRQ_F_NO_MASK）。 */
        u32 need = 0;
        for (u32 i = 0; i < e->nsharer; i++) {
            if (e->sh[i].need_ack) {
                need++;
            }
        }
        if (need > 0) {
            if (fe_ioapic_present()) {
                fe_ioapic_mask(e->gsi);
            } else {
                fe_pic_mask((u8)e->irq);
            }
            e->masked = true;
            e->pending = need;
        }
    }

    if (fe_lapic_present()) {
        fe_lapic_eoi();
    } else {
        fe_pic_eoi((u8)e->irq);
    }

    e->count++;
    /* ★ 置位**所有**登记者（D5a）★
     * 线不携带"是哪台设备"，所以内核挑不了——让每个驱动去问自己的设备
     * 是唯一正确的做法。代价是共享者必须接受伪唤醒，这一条写在
     * docs/14-interrupt-semantics.md §2.2，也写在用户态的 API 说明里。 */
    for (u32 i = 0; i < e->nsharer; i++) {
        struct fe_notification *nt = e->sh[i].nt;
        e->sh[i].owes = e->sh[i].need_ack;  /* 只对要确认的那几家记账 */
        if (nt && e->irq < 64) {
            e->sh[i].count++;
            fe_notification_signal_obj(nt, 1ull << e->irq);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 登记与释放                                                          */
/* ------------------------------------------------------------------ */

void fe_irq_init(void)
{
    memset(g_irq, 0, sizeof(g_irq));
    memset(g_vec_slot, 0xFF, sizeof(g_vec_slot));
    /* GSI 向量区间一次性全部装上处理程序：向量分配是运行期动态的，
     * 每次登记再去改 IDT 会让「登记」这一步带上难以推理的时序。 */
    for (u32 v = FE_VEC_GSI_BASE; v <= FE_VEC_GSI_MAX; v++) {
        fe_idt_set_handler((u8)v, gsi_isr);
    }
}

/* require_mode：调用者对触发方式的要求（FE_IRQ_MODE_ANY = 不要求）。
 * 见 docs/14-interrupt-semantics.md §3：报告是"事后知情"，声明是"事前拒绝"；
 * 对中断来说带着错的触发方式跑比直接失败更糟（症状是负载高时偶发丢事件，
 * 而那种症状会被归因到驱动自己的逻辑上）。 */
static fe_status_t irq_install(u32 irq, struct fe_notification *nt, u64 owner,
                               bool route_hw, u32 require_mode,
                               struct fe_irq_info *out_info)
{
    if (!nt || irq > 255) {
        return FE_ERR_INVAL;
    }
    if (irq >= 64) {
        /* 通知对象只有 64 位，投递位是 (1 << irq)，超过就没法表达 */
        return FE_ERR_RANGE;
    }

    u32 gsi = irq;
    bool level = false, active_low = false;
    fe_acpi_irq_to_gsi((u8)irq, &gsi, &level, &active_low);

    /* ---- 触发方式的契约（D5b）：先核对，再改动任何状态 ---- */
    /* ★ 只有**真的声明了触发方式**才核对 ★
     * 第一版写成 `require_mode != 0`，于是 `FE_IRQ_F_NO_MASK`（只说
     * "不要屏蔽"、没说触发方式）被当成了"我要边沿"，在电平线上被拒——
     * 而日志还理直气壮地说"调用者要求边沿触发"。**把调用者没说的话
     * 说成它说了**，比不检查更糟：它会让人去改一个没写错的地方。 */
    if (require_mode & (FE_IRQ_MODE_EDGE | FE_IRQ_MODE_LEVEL)) {
        bool want_level = (require_mode & FE_IRQ_MODE_LEVEL) != 0;
        if (want_level != level) {
            fe_kprintf("[IRQ] 拒绝：调用者要求%s触发，而 IRQ%u 实测是%s触发\n",
                       want_level ? "电平" : "边沿", irq, level ? "电平" : "边沿");
            return FE_ERR_INVAL;
        }
    }

    /* ---- 已有登记者？共享还是拒绝（D5a）---- */
    struct fe_irq_entry *ex = find_irq(irq);
    if (ex) {
        /* ★ 边沿线的共享必须被拒 ★
         * 设备 A 的中断正在处理时，B 拉的那次边沿**没有第二次跳变可检测**，
         * 那一次就永久丢了。这不是实现缺陷，是边沿语义本身的性质——
         * 允许它只会把"偶发丢中断"变成一个查不出来的问题。
         * 想共享就用 MSI（每台设备一条独立消息），见 §4。 */
        if (!ex->level) {
            fe_kprintf("[IRQ] 拒绝：IRQ%u 是**边沿**触发，不能共享"
                       "（边沿共享会丢中断，要共享请走 MSI）\n", irq);
            return FE_ERR_BUSY;
        }
        if (ex->nsharer >= FE_IRQ_SHARERS_MAX) {
            return FE_ERR_NOSPC;
        }
        /* 同一个 owner 重复登记同一条线：拒绝（多半是它自己写错了） */
        for (u32 i = 0; i < ex->nsharer; i++) {
            if (ex->sh[i].owner == owner) {
                return FE_ERR_EXIST;
            }
        }
        u64 flags = fe_irq_save();
        ex->sh[ex->nsharer].owner = owner;
        ex->sh[ex->nsharer].nt = nt;
        ex->sh[ex->nsharer].count = 0;
        ex->sh[ex->nsharer].need_ack = (require_mode & FE_IRQ_F_NO_MASK) == 0;
        ex->nsharer++;
        fe_object_ref(&nt->hdr);
        /* ★ pending 要跟着一起长，但**只对要确认的那家** ★
         * 声明了 NO_MASK 的人不计入——它不会 ack，计进去就等于这条线
         * 永远放不开（而线屏蔽着时邻居也一起收不到中断）。 */
        if (ex->masked && ex->sh[ex->nsharer - 1].need_ack) {
            ex->pending++;
        }
        u32 n = ex->nsharer;
        fe_irq_restore(flags);
        fe_kprintf("[IRQ] IRQ%u（%s）**共享登记**第 %u 家：任务 %llu\n",
                   irq, ex->level ? "电平" : "边沿", n,
                   (unsigned long long)owner);
        if (out_info) {
            out_info->gsi = ex->gsi;
            out_info->vector = ex->vector;
            out_info->mode = (ex->level ? FE_IRQ_MODE_LEVEL : FE_IRQ_MODE_EDGE) |
                             (ex->active_low ? FE_IRQ_MODE_ACTIVE_LOW : 0);
            out_info->sharers = n;
        }
        return FE_OK;
    }

    u32 idx = FE_IRQ_MAX;
    for (u32 i = 0; i < FE_IRQ_MAX; i++) {
        if (!g_irq[i].used) {
            idx = i;
            break;
        }
    }
    if (idx == FE_IRQ_MAX) {
        return FE_ERR_NOSPC;
    }

    /* 选向量：有 IOAPIC 时用动态区间（一个中断一个向量，能分辨来源）；
     * 退回 8259 时只能用它固定的映射（0x20 + irq），因为向量是 8259 自己给的。 */
    bool dynamic = route_hw && fe_ioapic_present();
    u8 vector;
    if (dynamic) {
        u32 free_vec = FE_VEC_GSI_BASE;
        for (; free_vec <= FE_VEC_GSI_MAX; free_vec++) {
            if (g_vec_slot[free_vec] == 0xFF) {
                break;
            }
        }
        if (free_vec > FE_VEC_GSI_MAX) {
            return FE_ERR_NOSPC;
        }
        vector = (u8)free_vec;
    } else if (route_hw) {
        vector = (u8)FE_VEC_IRQ(irq & 0x0F);
        fe_idt_set_handler(vector, gsi_isr);
    } else {
        vector = (u8)(FE_VEC_GSI_BASE + idx);   /* 自检：借一个向量，不接硬件 */
    }

    if (dynamic) {
        fe_ioapic_route(gsi, vector, fe_lapic_id(), level, active_low, false);
        /* 8259 那条路必须掐断，否则同一个设备会经两条路各投递一次 */
        fe_pic_mask((u8)irq);
    } else if (route_hw) {
        fe_pic_unmask((u8)irq);
    }
    g_vec_slot[vector] = (u8)idx;

    struct fe_irq_entry *e = &g_irq[idx];
    e->used = true;
    e->irq = irq;
    e->gsi = gsi;
    e->vector = vector;
    e->level = level;
    e->active_low = active_low;
    e->masked = false;
    e->count = 0;
    /* 第一个登记者：nsharer 从 1 起（共享的人往后加），pending 归零 */
    e->nsharer = 1;
    e->pending = 0;
    e->sh[0].owner = owner;
    e->sh[0].nt = nt;
    e->sh[0].count = 0;
    e->sh[0].owes = false;
    e->sh[0].need_ack = (require_mode & FE_IRQ_F_NO_MASK) == 0;
    fe_object_ref(&nt->hdr);

    fe_kprintf("[IRQ] 登记 IRQ%u -> GSI %u 向量 %u（%s, %s）%s\n",
               irq, gsi, vector, level ? "电平" : "边沿",
               active_low ? "低有效" : "高有效",
               route_hw ? "" : "（自检，未接硬件）");
    if (out_info) {
        out_info->gsi = gsi;
        out_info->vector = vector;
        out_info->mode = (level ? FE_IRQ_MODE_LEVEL : FE_IRQ_MODE_EDGE) |
                         (active_low ? FE_IRQ_MODE_ACTIVE_LOW : 0);
        out_info->sharers = 1;
    }
    return FE_OK;
}

fe_status_t fe_irq_register_ex(u32 irq, struct fe_notification *nt, u64 owner_id,
                               u32 require_mode, struct fe_irq_info *out_info)
{
    u64 flags = fe_irq_save();
    fe_status_t s = irq_install(irq, nt, owner_id, true, require_mode, out_info);
    fe_irq_restore(flags);
    return s;
}

fe_status_t fe_irq_register(u32 irq, struct fe_notification *nt, u64 owner_id)
{
    return fe_irq_register_ex(irq, nt, owner_id, FE_IRQ_MODE_ANY, NULL);
}

/* 确认一次投递（D5a：共享时**所有人都确认**才放行）。
 *
 * ★ 为什么 pending 是计数而不是布尔 ★
 * 共享的电平线上，先确认的那个若立刻放行，而线还高着（另一台设备还没被
 * 服务），LAPIC 会立刻再投递一次——CPU 被同一个中断淹没。
 * 所以放行的条件是"欠的确认全部还清"。 */
fe_status_t fe_irq_ack_owner(u32 irq, u64 owner_id)
{
    u64 flags = fe_irq_save();
    struct fe_irq_entry *e = find_irq(irq);
    if (!e) {
        fe_irq_restore(flags);
        return FE_ERR_NOENT;
    }
    /* 只有登记者才能确认：否则任何人都能把一条线"提前放行" */
    i32 mine = -1;
    for (u32 i = 0; i < e->nsharer; i++) {
        if (e->sh[i].owner == owner_id) {
            mine = (i32)i;
            break;
        }
    }
    if (mine < 0) {
        fe_irq_restore(flags);
        return FE_ERR_ACCESS;
    }
    /* ★ 同一家确认两次不该把别人的欠账消掉 ★
     * 用 owes 逐家判：已经还过的人再确认一次是空操作。
     * 声明了 NO_MASK 的那家本来就没有欠账（owes 永远是 false），
     * 它调 ack 也只是空操作——**不报错**，因为"确认一下"是安全的动作，
     * 而为一个无害的多余调用报错只会让驱动多写一个分支。 */
    if (e->masked && e->sh[mine].owes) {
        e->sh[mine].owes = false;
        if (e->pending > 0) {
            e->pending--;
        }
        if (e->pending == 0) {
            if (fe_ioapic_present()) {
                fe_ioapic_unmask(e->gsi);
            } else {
                fe_pic_unmask((u8)e->irq);
            }
            e->masked = false;
        }
    }
    fe_irq_restore(flags);
    return FE_OK;
}

/* 兼容入口：不带 owner 的确认。
 * ★ 只有独占（一个登记者）时才语义明确 ★ 共享时"谁在确认"必须能回答，
 * 所以这个入口在共享线上返回 INVAL，逼调用者用带 owner 的那个。 */
fe_status_t fe_irq_ack(u32 irq)
{
    struct fe_irq_entry *e = find_irq(irq);
    if (!e) {
        return FE_ERR_NOENT;
    }
    if (e->nsharer > 1) {
        return FE_ERR_INVAL;
    }
    return fe_irq_ack_owner(irq, e->sh[0].owner);
}

/* ★ 触发方式的契约单独提出来，让调用者能**先校验再动状态** ★
 *
 * 原来这个检查只长在 irq_install 里，而系统调用是"先认领资源、再登记"
 * 的顺序。于是"我声明的触发方式不对"这件事会**先**撞上资源池的错误
 * （共享线上同一个任务再认领一次会拿到 EXIST），报出来的是
 * "你已经持有它"——**而调用者真正写错的是那个模式参数**。
 *
 * 参数错误应当先于状态检查报出来，这是通例：它不依赖任何状态，
 * 而且早报一次就不必去动资源池。 */
bool fe_irq_mode_ok(u32 irq, u32 require_mode)
{
    if ((require_mode & (FE_IRQ_MODE_EDGE | FE_IRQ_MODE_LEVEL)) == 0) {
        return true;        /* 没声明触发方式，无所谓匹配 */
    }
    u32 gsi = irq;
    bool level = false, active_low = false;
    fe_acpi_irq_to_gsi((u8)irq, &gsi, &level, &active_low);
    bool want_level = (require_mode & FE_IRQ_MODE_LEVEL) != 0;
    return want_level == level;
}

bool fe_irq_get_info(u32 irq, struct fe_irq_info *out){
    u64 flags = fe_irq_save();
    struct fe_irq_entry *e = find_irq(irq);
    if (!e || !out) {
        fe_irq_restore(flags);
        return false;
    }
    out->gsi = e->gsi;
    out->vector = e->vector;
    out->mode = (e->level ? FE_IRQ_MODE_LEVEL : FE_IRQ_MODE_EDGE) |
                (e->active_low ? FE_IRQ_MODE_ACTIVE_LOW : 0);
    out->sharers = e->nsharer;
    fe_irq_restore(flags);
    return true;
}

void fe_irq_release_owner(u64 owner_id)
{
    if (owner_id == 0) {
        return;
    }
    u64 flags = fe_irq_save();
    for (u32 i = 0; i < FE_IRQ_MAX; i++) {
        struct fe_irq_entry *e = &g_irq[i];
        if (!e->used) {
            continue;
        }
        /* ★ 共享线上只摘自己那一家（D5a）★
         * 原来一命中就把整条线掐掉——共享时那会把**邻居的中断一起打死**，
         * 而邻居根本不知道自己被牵连了（症状：某个驱动突然收不到中断，
         * 而它的登记还在）。 */
        u32 k = 0;
        bool found = false;
        for (; k < e->nsharer; k++) {
            if (e->sh[k].owner == owner_id) {
                found = true;
                break;
            }
        }
        if (!found) {
            continue;
        }
        struct fe_notification *nt = e->sh[k].nt;
        bool was_owing = e->sh[k].need_ack && e->sh[k].owes;
        /* 摘除：后面的往前挪 */
        for (u32 j = k; j + 1 < e->nsharer; j++) {
            e->sh[j] = e->sh[j + 1];
        }
        e->nsharer--;
        memset(&e->sh[e->nsharer], 0, sizeof(e->sh[0]));
        /* ★ 只有"真的还欠着"的那家退出才从 pending 里减 ★
         * 已经确认过的人离开时再减一次，会让线在"还有人在欠账"的情况下
         * 被错误放行——线还高着 → 立刻重投 → 中断风暴。
         * 反过来说，欠着账就跑的人必须被清掉，否则 pending 永远归不了零，
         * 整条线（连同邻居）就此卡死。两个方向都要对，所以判据是
         * **它自己那一格 owes**，而不是"有人退出就减一"。 */
        if (e->masked && was_owing && e->pending > 0) {
            e->pending--;
        }
        if (nt) {
            fe_object_unref(&nt->hdr);
        }
        if (e->nsharer == 0) {
            /* 最后一个走了：这才掐线、释放向量 */
            if (e->is_msi) {
                /* ★ MSI：先把**设备**的使能位关掉 ★
                 * 只释放向量是不够的——设备还会继续往一个已经没人处理的
                 * 向量发消息。那不是崩溃（没有登记者的向量直接返回），
                 * 但它是一个没人知道的泄漏，而且换了架构就未必无害。 */
                msi_disable_device(e->msi_bus, e->msi_dev, e->msi_fn, e->msi_cap);
            } else if (e->gsi != FE_IRQ_NONE) {
                /* ★ gsi == FE_IRQ_NONE 时不能写 IOAPIC ★
                 * IOAPIC 的重定向表只有 24 项，拿 0xFFFFFFFF 去写就是
                 * 越界写设备寄存器。MSI 条目正是这种情况。 */
                if (fe_ioapic_present()) {
                    fe_ioapic_mask(e->gsi);
                } else {
                    fe_pic_mask((u8)e->irq);
                }
            }
            g_vec_slot[e->vector] = 0xFF;
            memset(e, 0, sizeof(*e));
        } else if (e->masked && e->pending == 0) {
            /* 最后一个欠账的人走了，线可以放行了 */
            if (fe_ioapic_present()) {
                fe_ioapic_unmask(e->gsi);
            } else {
                fe_pic_unmask((u8)e->irq);
            }
            e->masked = false;
        }
    }
    fe_irq_restore(flags);
}

/* ------------------------------------------------------------------ */
/* MSI / MSI-X（D5c）                                                  */
/* ------------------------------------------------------------------ */

/* ★ 为什么 MSI 的编程必须在内核里 ★
 *
 * MSI 是"设备往一个固定的物理地址写一个 32 位数"，而那个地址是
 * **LAPIC 的 MSI 窗口**、那个数是**向量号**——两者都是内核的资源。
 * 让用户态填的话，它可以把中断投到任意向量上，那等于绕过了
 * "向量归内核分配"这条边界。所以地址与数据都由内核算。
 *
 * ★ MSI-X 的表项也由内核代写（这一段是实测之后改的）★
 * **原先写的是**："用户态只拿到结果（以及 MSI-X 的表项位置，好自己去写那 16
 * 字节）。"
 *
 * **实测推翻了它**（`docs/14-interrupt-semantics.md` §8.1）：QEMU 的
 * virtio-blk-pci 把 MSI-X 表放在 **BAR1**，而驱动只映射了 **BAR4**——
 * 驱动手里根本没有表项所在的那段映射，"让驱动自己写"在真实设备上走不通。
 * 现在由内核在 `fe_irq_msi_alloc` 里代写：映那 16 字节 MMIO、写
 * addr/data/控制字、再置 Enable、清 Function Mask，**整套在一次调用里做完**。
 *
 * ★ 不要在这里加"让用户态自己写表项"的路 ★ 两个理由：
 * ① **在真实设备上它不可行**——表所在的 BAR（实测 BAR1）驱动根本没映射；
 * ② **能力面反而更大**——让驱动写表，它就能往**别人的**表项里填别人的向量；
 * 内核代写则只写它自己分配的那一个。
 * `struct fe_msi_info` 里的 `table_bar` / `table_offset` **只是诊断信息**
 * （告诉驱动内核把表写在哪儿、好让它打印与排障），**不是"轮到你写"的信号**。
 *
 * ★ 表项号也由内核这一层定 ★ 用户态那层 ABI 没有"表项号"参数
 * （`fe_irq_msi_alloc(nt, bus, dev, fn, cap_off, &mi)`），
 * `kernel/arch/x86_64/syscall.c` 的 `sys_irq_msi_alloc` 固定传 `flags = 0`，
 * 所以今天写进去的**永远是第 0 项**（virtio 的队列 0 用它，正好对得上）。
 * 将来要支持多个表项，改的是这一条链上的**参数**，不是让驱动自己写表。
 *
 * ★ 那还需要用户态做什么 ★ 只剩两件：等 `1 << mi.irq` 那一位，
 * 以及**协议层**那个开关（例如 virtio 的 `queue_msix_vector`，默认 0xFFFF =
 * 不用 MSI-X）——那份知识只有驱动有，内核不该碰（§8.2）。 */

#define FE_PCI_CFG_ADDR 0xCF8u
#define FE_PCI_CFG_DATA 0xCFCu
#define FE_PCI_CAP_MSI    0x05u
#define FE_PCI_CAP_MSIX   0x11u

/* 伪中断号：MSI 没有 ISA 中断号，但通知位要用一个下标。
 * 取 32..63 —— 与 ISA（0..15）和 GSI（16..31）都不重叠，
 * 而且 < 64（通知对象是 64 位）。 */
#define FE_IRQ_MSI_FIRST 32u
#define FE_IRQ_MSI_LAST  63u

static u32 pci_cfg_read32(u32 bus, u32 dev, u32 fn, u32 off)
{
    u32 addr = 0x80000000u | ((bus & 0xFF) << 16) | ((dev & 0x1F) << 11) |
               ((fn & 0x07) << 8) | (off & 0xFC);
    fe_outl((u16)FE_PCI_CFG_ADDR, addr);
    return fe_inl((u16)FE_PCI_CFG_DATA);
}

static void pci_cfg_write32(u32 bus, u32 dev, u32 fn, u32 off, u32 val)
{
    u32 addr = 0x80000000u | ((bus & 0xFF) << 16) | ((dev & 0x1F) << 11) |
               ((fn & 0x07) << 8) | (off & 0xFC);
    fe_outl((u16)FE_PCI_CFG_ADDR, addr);
    fe_outl((u16)FE_PCI_CFG_DATA, val);
}

static u16 pci_cfg_read16(u32 bus, u32 dev, u32 fn, u32 off)
{
    u32 w = pci_cfg_read32(bus, dev, fn, off & ~3u);
    return (u16)((w >> ((off & 2u) * 8)) & 0xFFFFu);
}

static void pci_cfg_write16(u32 bus, u32 dev, u32 fn, u32 off, u16 val)
{
    u32 w = pci_cfg_read32(bus, dev, fn, off & ~3u);
    u32 sh = (off & 2u) * 8;
    w = (w & ~(0xFFFFu << sh)) | ((u32)val << sh);
    pci_cfg_write32(bus, dev, fn, off & ~3u, w);
}

/* 找一个空闲的伪中断号（MSI 用）。 */
static u32 msi_irq_alloc(void);

/* 关掉一台设备的 MSI（写能力结构的使能位）。 */
static void msi_disable_device(u32 bus, u32 dev, u32 fn, u32 cap);

/* 找一个空闲的伪中断号（MSI 用）。 */
static u32 msi_irq_alloc(void)
{    for (u32 n = FE_IRQ_MSI_FIRST; n <= FE_IRQ_MSI_LAST; n++) {
        if (!find_irq(n)) {
            return n;
        }
    }
    return FE_IRQ_NONE;
}

/* 分配一个向量并占住 g_vec_slot。MSI 不需要 IOAPIC 路由。 */
static i32 msi_vector_alloc(u32 *out_slot)
{
    u32 idx = FE_IRQ_MAX;
    for (u32 i = 0; i < FE_IRQ_MAX; i++) {
        if (!g_irq[i].used) {
            idx = i;
            break;
        }
    }
    if (idx == FE_IRQ_MAX) {
        return -1;
    }
    for (u32 v = FE_VEC_GSI_BASE; v <= FE_VEC_GSI_MAX; v++) {
        if (g_vec_slot[v] == 0xFF) {
            g_vec_slot[v] = (u8)idx;
            *out_slot = idx;
            return (i32)v;
        }
    }
    return -1;
}

/* x86 的 MSI 消息编码：往 LAPIC 的 MSI 窗口写一个 32 位数。
 *   address = 0xFEE00000 | (目的 APIC ID << 12)
 *   data    = 向量 | 边沿 | assert
 * 目的固定为**本 CPU**（今天单核；SMP 时这里要按 IRQ 亲和性选）。 */
static u64 msi_addr_for_cpu(void)
{
    return 0xFEE00000ull | ((u64)fe_lapic_id() << 12);
}

static u32 msi_data_for_vector(u8 vector)
{
    return (u32)vector | (1u << 14);    /* assert，边沿（bit15 = 0）*/
}

fe_status_t fe_irq_msi_alloc(u64 bus, u64 dev, u64 fn, u64 cap_off,
                             u32 flags, struct fe_notification *nt, u64 owner,
                             struct fe_msi_info *out)
{
    if (!nt || !out || bus > 0xFF || dev > 0x1F || fn > 0x07 ||
        cap_off < 0x40 || cap_off > 0xFC || (cap_off & 3)) {
        return FE_ERR_INVAL;
    }
    /* 先确认那里真的是一个 MSI / MSI-X 能力结构，再动任何状态。
     * ★ 这一条不能省 ★ 用户态给的偏移是它自己算出来的，内核不能假设
     * 它算对了——照着错的偏移写下去，可能改掉设备的 BAR 或命令寄存器。 */
    u16 cap_id = pci_cfg_read16(bus, dev, fn, (u32)cap_off);
    u8 id = (u8)(cap_id & 0xFF);
    if (id != FE_PCI_CAP_MSI && id != FE_PCI_CAP_MSIX) {
        fe_kprintf("[MSI] 拒绝：%02llx:%02llx.%llu 的偏移 %#llx 上是能力 %#x，"
                   "不是 MSI(0x05) 也不是 MSI-X(0x11)\n",
                   (unsigned long long)bus, (unsigned long long)dev,
                   (unsigned long long)fn, (unsigned long long)cap_off, id);
        return FE_ERR_INVAL;
    }

    u64 irq_flags = fe_irq_save();
    u32 pirq = msi_irq_alloc();
    if (pirq == FE_IRQ_NONE) {
        fe_irq_restore(irq_flags);
        return FE_ERR_NOSPC;
    }
    u32 idx = 0;
    i32 vec = msi_vector_alloc(&idx);
    if (vec < 0) {
        fe_irq_restore(irq_flags);
        return FE_ERR_NOSPC;
    }

    u16 ctrl = pci_cfg_read16(bus, dev, fn, (u32)cap_off + 2);
    u64 addr = msi_addr_for_cpu();
    u32 data = msi_data_for_vector((u8)vec);

    out->irq = pirq;
    out->vector = (u8)vec;
    out->kind = (id == FE_PCI_CAP_MSIX) ? FE_MSI_KIND_MSIX : FE_MSI_KIND_MSI;
    out->addr = addr;
    out->data = data;
    out->table_bar = 0xFF;
    out->table_offset = 0;

    if (id == FE_PCI_CAP_MSI) {
        /* MSI：地址/数据直接写在能力结构里，然后置使能位。
         * ★ 64 位地址能力（message control 的 bit 7）★ 有它时
         * 数据在 +12，没有时在 +8——写错位置就是把向量写进地址的高半部。 */
        pci_cfg_write32(bus, dev, fn, (u32)cap_off + 4, (u32)addr);
        if (ctrl & 0x0080) {
            pci_cfg_write32(bus, dev, fn, (u32)cap_off + 8, (u32)(addr >> 32));
            pci_cfg_write16(bus, dev, fn, (u32)cap_off + 12, (u16)data);
        } else {
            pci_cfg_write16(bus, dev, fn, (u32)cap_off + 8, (u16)data);
        }
        pci_cfg_write16(bus, dev, fn, (u32)cap_off + 2, (u16)(ctrl | 0x0001));
    } else {
        /* MSI-X：表在某个 BAR 里，内核**用自己已有的 MMIO 窗口**写第 index 项。
         *
         * ★ 这里与原设计不同，值得说明为什么改 ★
         * **原设计（`docs/14-interrupt-semantics.md` §4.4）是"驱动自己写那 16 个
         * 字节"**，本轮**实测推翻了它**（同文档 §8.1）：QEMU 的 virtio-blk-pci
         * 把 MSI-X 表放在 **BAR1**，而驱动只映射了 **BAR4**（virtio 四类结构
         * 所在的那一段）——于是驱动为了写 16 个字节还得再认领一个 BAR、
         * 再映射一次。那不是"更干净的分工"，是把一次写放大成一条资源链。
         *
         * 内核这边本来就要读那个 BAR 的**地址**（不然算不出表项在哪），
         * 所以"知道 MSI-X 表在哪"这件事它躲不掉。用 `fe_vmm_map_mmio`
         * 映射到内核自己的 MMIO 窗口写一次，**不产生任何所有权声明**：
         * 资源池里 BAR1 归谁完全不受影响。
         *
         * 代价：内核多知道一点 PCI 布局（表 = BAR + 偏移 + 项号 × 16）。
         * 好处：驱动不需要碰 BAR1，也没有"驱动能往别人的表项里填向量"
         * 这件事了——内核只写它自己分配的那个向量。
         *
         * ★ 下面填进 out 的两个字段只是诊断信息 ★ `table_bar`/`table_offset`
         * 是给驱动打印与排障用的（"内核把表写在哪儿"），**不是"请你（驱动）
         * 自己去写表项"的暗示**——表项在本函数返回之前就已经写完了。
         * 读到这两行注释的人**不要再加一条"让用户态写表项"的路**。 */
        u32 tbl = pci_cfg_read32(bus, dev, fn, (u32)cap_off + 4);
        u32 bir = tbl & 0x7u;
        u32 off = tbl & ~0x7u;
        out->table_bar = bir;
        out->table_offset = off;
        if (bir >= 6) {
            fe_irq_restore(irq_flags);
            return FE_ERR_NOTSUP;       /* BIR 6/7 在规范里是保留值 */
        }
        u32 bar = pci_cfg_read32(bus, dev, fn, 0x10u + 4u * bir);
        if ((bar & 0x1u) != 0) {
            /* I/O 空间的 BAR 装不下 MSI-X 表（规范要求内存 BAR） */
            fe_kprintf("[MSI] 拒绝：MSI-X 表指示的 BAR%u 是 I/O 空间\n", bir);
            fe_irq_restore(irq_flags);
            return FE_ERR_INVAL;
        }
        u64 bar_base = bar & 0xFFFFFFF0ull;
        if (bar_base == 0) {
            fe_kprintf("[MSI] 拒绝：BAR%u 没有分配地址（表写不进去）\n", bir);
            fe_irq_restore(irq_flags);
            return FE_ERR_NOENT;
        }
        u32 index = flags;              /* 表项号（virtio 的队列 0 用第 0 项） */
        u64 ent = bar_base + off + (u64)index * 16u;
        /* ★ 绝不能往可用内存里写 ★ 设备说表在哪是**设备说的**，
         * 而我们马上要往那个物理地址写数据。它与 sys_mmio_map 的护栏
         * 是同一条，也必须在这里再有一道（入口挡不住"设备撒谎"）。 */
        if (fe_pmm_is_usable(ent, 16)) {
            fe_kprintf("[MSI] 拒绝：MSI-X 表项落在**可用内存** %#llx 上\n",
                       (unsigned long long)ent);
            fe_irq_restore(irq_flags);
            return FE_ERR_ACCESS;
        }
        volatile u32 *tab = (volatile u32 *)fe_vmm_map_mmio(ent, 16);
        if (!tab) {
            fe_irq_restore(irq_flags);
            return FE_ERR_NOMEM;
        }
        tab[0] = (u32)addr;
        tab[1] = (u32)(addr >> 32);
        tab[2] = data;
        tab[3] = 0;                     /* 控制字 0 = 该项不屏蔽 */
        /* 使能位最后置：前面三项没写完就让设备开始发消息的话，
         * 它会拿一个地址/数据还不完整的表项去投递。 */
        pci_cfg_write16(bus, dev, fn, (u32)cap_off + 2,
                        (u16)((ctrl & ~0x4000u) | 0x8000u));
    }

    struct fe_irq_entry *e = &g_irq[idx];
    e->used = true;
    e->irq = pirq;
    e->gsi = FE_IRQ_NONE;           /* MSI 不经 IOAPIC：屏蔽/放行都不适用 */
    e->vector = (u8)vec;
    e->level = false;               /* MSI 天然是边沿（消息写），不需要 ack */
    e->active_low = false;
    e->masked = false;
    e->count = 0;
    e->nsharer = 1;
    e->pending = 0;
    e->sh[0].owner = owner;
    e->sh[0].nt = nt;
    e->sh[0].count = 0;
    e->sh[0].owes = false;
    e->sh[0].need_ack = false;
    /* 记下设备位置：任务退出时**替它把设备的 MSI 关掉**。
     * 不关的话设备会一直往一个已释放的向量发消息——那不是崩溃
     * （没有登记者的向量直接返回），但它是一个没人知道的泄漏。 */
    e->msi_bus = (u32)bus;
    e->msi_dev = (u32)dev;
    e->msi_fn = (u32)fn;
    e->msi_cap = (u32)cap_off;
    e->is_msi = true;
    fe_object_ref(&nt->hdr);
    fe_irq_restore(irq_flags);

    fe_kprintf("[MSI] %s 已使能：%02llx:%02llx.%llu cap %#llx → 伪中断 %u 向量 %u"
               "（addr %#llx data %#x）\n",
               (id == FE_PCI_CAP_MSIX) ? "MSI-X" : "MSI",
               (unsigned long long)bus, (unsigned long long)dev,
               (unsigned long long)fn, (unsigned long long)cap_off,
               pirq, (u32)vec, (unsigned long long)addr, data);
    return FE_OK;
}

/* 关掉一台设备的 MSI（写能力结构的使能位）。 */
static void msi_disable_device(u32 bus, u32 dev, u32 fn, u32 cap)
{
    u16 ctrl = pci_cfg_read16(bus, dev, fn, cap + 2);
    u16 id = (u16)(pci_cfg_read16(bus, dev, fn, cap) & 0xFF);
    if (id == FE_PCI_CAP_MSI) {
        pci_cfg_write16(bus, dev, fn, cap + 2, (u16)(ctrl & ~0x0001u));
    } else if (id == FE_PCI_CAP_MSIX) {
        /* 先置 function mask 再关使能：反过来的话，关的那一瞬间
         * 设备还可能往一个正在失效的表项发消息。 */
        pci_cfg_write16(bus, dev, fn, cap + 2, (u16)(ctrl | 0x4000u));
        pci_cfg_write16(bus, dev, fn, cap + 2, (u16)(ctrl & ~0x8000u));
    }
}

fe_status_t fe_irq_msi_free(u32 irq, u64 owner)
{
    struct fe_irq_entry *e = find_irq(irq);
    if (!e) {
        return FE_ERR_NOENT;
    }
    if (!e->is_msi) {
        return FE_ERR_INVAL;        /* 不是 MSI 条目：别用这个入口 */
    }
    if (e->sh[0].owner != owner) {
        return FE_ERR_ACCESS;
    }
    u64 flags = fe_irq_save();
    msi_disable_device(e->msi_bus, e->msi_dev, e->msi_fn, e->msi_cap);
    fe_irq_restore(flags);
    fe_irq_release_owner(owner);
    return FE_OK;
}

/* ------------------------------------------------------------------ */
/* 诊断与自检                                                          */
/* ------------------------------------------------------------------ */

u32 fe_irq_owned_count(u64 owner_id)
{
    u32 n = 0;
    for (u32 i = 0; i < FE_IRQ_MAX; i++) {
        if (!g_irq[i].used) {
            continue;
        }
        for (u32 k = 0; k < g_irq[i].nsharer; k++) {
            if (g_irq[i].sh[k].owner == owner_id) {
                n++;
            }
        }
    }
    return n;
}

u64 fe_irq_count(u32 irq)
{
    struct fe_irq_entry *e = find_irq(irq);
    return e ? e->count : 0;
}

void fe_irq_dump(void)
{
    fe_kprintf("  IRQ  向量  GSI  触发  共享  次数        认领者（各自收到）\n");
    for (u32 i = 0; i < FE_IRQ_MAX; i++) {
        struct fe_irq_entry *e = &g_irq[i];
        if (!e->used) {
            continue;
        }
        fe_kprintf("  %-4u %-5u %-4u %-5s %-5u %-11llu", e->irq, e->vector, e->gsi,
                   e->level ? "电平" : "边沿", e->nsharer,
                   (unsigned long long)e->count);
        for (u32 k = 0; k < e->nsharer; k++) {
            fe_kprintf(" %llu(%llu)", (unsigned long long)e->sh[k].owner,
                       (unsigned long long)e->sh[k].count);
        }
        fe_kprintf("\n");
    }
}

/* ★ 失败要报**行号** ★
 * 这一组有四十多条断言，而"失败项: 2"这种输出无法定位任何东西——
 * 排查的人只能一条条读代码去猜。一个说不出在哪失败的测试，
 * 与一个不存在的测试差不多（本轮就因此多绕了两步）。 */
#define CHECK(cond) do { \
        if (!(cond)) { \
            fe_kprintf("        **中断自检失败** irq.c:%d: %s\n", __LINE__, #cond); \
            fail++; \
        } \
    } while (0)

u32 fe_selftest_irq(void)
{
    u32 fail = 0;

    /* 自检不碰真实硬件：route_hw=false 只走登记/投递/确认的逻辑路径。
     * 这是刻意的——自检去路由一根真实的中断线，万一那根线上有设备在拉，
     * 就会在别人的机器上变成随机的中断风暴。 */
    struct fe_task *kt = fe_task_kernel();
    fe_handle_t h = FE_HANDLE_INVALID;
    CHECK(fe_ok(fe_notification_create(kt, &h)));

    struct fe_object_header *obj = NULL;
    CHECK(fe_ok(fe_handle_lookup(&kt->handles, h, FE_RIGHT_SIGNAL, &obj)));
    struct fe_notification *nt = FE_OBJ_OF(obj, struct fe_notification);

    /* 第二个通知对象：共享要"两家各自的位都被置上"，一个对象看不出这一点 */
    fe_handle_t h2 = FE_HANDLE_INVALID;
    CHECK(fe_ok(fe_notification_create(kt, &h2)));
    struct fe_object_header *obj2 = NULL;
    CHECK(fe_ok(fe_handle_lookup(&kt->handles, h2, FE_RIGHT_SIGNAL, &obj2)));
    struct fe_notification *nt2 = FE_OBJ_OF(obj2, struct fe_notification);

    /* ★ 判据从"ACPI 报出的实情"推导，不硬编码 ★
     *
     * 第一版把"IRQ15 是边沿"写死在断言里，结果这台机器上 ACPI 把未列出的
     * ISA 中断一律报成"**电平、低有效**"（那是 ISA 的正确默认值，见
     * fe_acpi_irq_to_gsi），于是 11 项断言一起红——**而内核完全正确**。
     *
     * 现在改成：先登记一次问清楚它是什么，再断言"声明成它本来的样子
     * 必须成功、声明成相反的样子必须失败"。这条判据与机器无关。 */
    const u32 LVL_IRQ = 11;     /* 这台机器上 ACPI 覆盖表说它是电平 */
    const u32 EDGE_IRQ = 20;    /* 没有覆盖条目且 ≥16 → 按默认是边沿 */
    struct fe_irq_info mi;

    u32 lvl_mode = 0, edge_mode = 0;
    if (irq_install(LVL_IRQ, nt, 1, false, FE_IRQ_MODE_ANY, &mi) == FE_OK) {
        lvl_mode = mi.mode;
        fe_irq_release_owner(1);
    }
    if (irq_install(EDGE_IRQ, nt, 1, false, FE_IRQ_MODE_ANY, &mi) == FE_OK) {
        edge_mode = mi.mode;
        fe_irq_release_owner(1);
    }
    fe_kprintf("        本机实情: IRQ%u=%s，IRQ%u=%s\n",
               LVL_IRQ, (lvl_mode & FE_IRQ_MODE_LEVEL) ? "电平" : "边沿",
               EDGE_IRQ, (edge_mode & FE_IRQ_MODE_LEVEL) ? "电平" : "边沿");

    /* ---- 触发方式的契约（D5b）：声明对的样子成功、相反的样子被拒 ---- */
    {
        u32 want_lvl = (edge_mode & FE_IRQ_MODE_LEVEL) ? FE_IRQ_MODE_EDGE
                                                       : FE_IRQ_MODE_LEVEL;
        /* EDGE_IRQ 的真实模式（按 ACPI 默认应当是边沿） */
        CHECK(irq_install(EDGE_IRQ, nt, 1, false, edge_mode, &mi) == FE_OK);
        CHECK(irq_install(EDGE_IRQ, nt2, 2, false, want_lvl, &mi) == FE_ERR_INVAL);
        /* ★ 边沿线**不能**共享（D5a 的核心物理约束）★
         * A 的中断正在处理时 B 拉的边沿没有第二次跳变可检测，
         * 那一次就永久丢了——拒绝比"允许然后偶发丢事件"诚实。
         * 注意这里期望的是 **BUSY**：模式声明是对的，被拒的原因是
         * "这条线不能共享"，两件事要能分开。 */
        CHECK(irq_install(EDGE_IRQ, nt2, 2, false, edge_mode, &mi) == FE_ERR_BUSY);
        CHECK(find_irq(EDGE_IRQ) && find_irq(EDGE_IRQ)->nsharer == 1);
        fe_irq_release_owner(1);
        CHECK(find_irq(EDGE_IRQ) == NULL);

        /* 电平线：声明"要求电平"必须成功、要求"边沿"必须被拒 */
        CHECK(irq_install(LVL_IRQ, nt, 1, false, lvl_mode, &mi) == FE_OK);
        u32 bad = (lvl_mode & FE_IRQ_MODE_LEVEL) ? FE_IRQ_MODE_EDGE : FE_IRQ_MODE_LEVEL;
        CHECK(irq_install(LVL_IRQ, nt2, 2, false, bad, &mi) == FE_ERR_INVAL);
        /* 同一 owner 重复登记同一条线：EXIST（与"我想再加一家"区分开） */
        CHECK(irq_install(LVL_IRQ, nt, 1, false, FE_IRQ_MODE_ANY, &mi) == FE_ERR_EXIST);
        /* 超过 64 的中断号无法用通知位表达 */
        CHECK(irq_install(70, nt, 1, false, FE_IRQ_MODE_ANY, &mi) == FE_ERR_RANGE);
        fe_irq_release_owner(1);
    }

    /* ---- 独占线的基本链路：置位通知、取出即清除 ---- */
    {
        CHECK(irq_install(EDGE_IRQ, nt, 1, false, FE_IRQ_MODE_ANY, &mi) == FE_OK);
        struct fe_irq_entry *e = find_irq(EDGE_IRQ);
        u8 vec = e ? e->vector : 0;

        struct fe_regs r;
        memset(&r, 0, sizeof(r));
        r.vector = vec;
        gsi_isr(&r);
        gsi_isr(&r);
        gsi_isr(&r);
        CHECK(nt->bits & (1ull << EDGE_IRQ));
        CHECK(fe_irq_count(EDGE_IRQ) == 3);

        u64 got = 0;
        CHECK(fe_ok(fe_notification_wait(h, ~0ull, &got)));
        CHECK(got == (1ull << EDGE_IRQ));
        CHECK((nt->bits & (1ull << EDGE_IRQ)) == 0);

        /* 边沿触发不需要屏蔽，ack 应是空操作 */
        CHECK(fe_ok(fe_irq_ack(EDGE_IRQ)));
        CHECK(fe_irq_ack(19) == FE_ERR_NOENT);

        fe_irq_release_owner(0);      /* owner=0 不归还，验证它确实不动手 */
        CHECK(fe_irq_count(EDGE_IRQ) == 3);
        fe_irq_release_owner(1);
        CHECK(find_irq(EDGE_IRQ) == NULL);
        CHECK(g_vec_slot[vec] == 0xFF);
    }

    /* ================= 共享一根**电平**线（D5a） ================= */
    {
        if (!(lvl_mode & FE_IRQ_MODE_LEVEL)) {
            fe_kprintf("        IRQ%u 在这台机器上不是电平触发，"
                       "共享自检无法进行（如实报出，不假装测过）\n", LVL_IRQ);
            fail++;
        } else {
            struct fe_irq_info i1, i2;
            CHECK(irq_install(LVL_IRQ, nt, 10, false, lvl_mode, &i1) == FE_OK);
            struct fe_irq_entry *e = find_irq(LVL_IRQ);
            u8 lvec = e ? e->vector : 0;

            /* 第二个 owner 登记同一条线：**必须成功**（D5a 全部的意义） */
            CHECK(irq_install(LVL_IRQ, nt2, 11, false, FE_IRQ_MODE_ANY, &i2) == FE_OK);
            CHECK(i2.sharers == 2);

            /* 合成一次中断：**两家都必须被置位**，各自计数都要加 */
            struct fe_regs r2;
            memset(&r2, 0, sizeof(r2));
            r2.vector = lvec;
            u64 c0 = fe_irq_count(LVL_IRQ);
            gsi_isr(&r2);
            CHECK(fe_irq_count(LVL_IRQ) == c0 + 1);
            CHECK(nt->bits & (1ull << LVL_IRQ));
            CHECK(nt2->bits & (1ull << LVL_IRQ));
            CHECK(e && e->sh[0].count == 1 && e->sh[1].count == 1);

            /* ---- ★ 核心反向对照：只确认一个**不能**放行 ★ ----
             * 先确认的那个若立刻放行，而线还高着（另一台设备没被服务），
             * LAPIC 会立刻再投递一次——中断风暴。 */
            CHECK(e && e->level);
            CHECK(e && e->masked);
            CHECK(e && e->pending == 2);
            CHECK(fe_ok(fe_irq_ack_owner(LVL_IRQ, 10)));
            CHECK(e && e->masked);              /* ← 仍然屏蔽着 */
            CHECK(e && e->pending == 1);
            CHECK(fe_ok(fe_irq_ack_owner(LVL_IRQ, 11)));
            CHECK(e && !e->masked);             /* ← 欠账还清才放行 */
            CHECK(e && e->pending == 0);

            /* 不在册的人不能确认（否则谁都能把线提前放行） */
            CHECK(fe_irq_ack_owner(LVL_IRQ, 999) == FE_ERR_ACCESS);
            /* 共享线上不带 owner 的确认语义不明 → 明确拒绝 */
            CHECK(fe_irq_ack(LVL_IRQ) == FE_ERR_INVAL);

            /* ---- ★ 反向对照：一家退出不能打死整条线 ★ ----
             * 原来的实现一命中就把整条线掐掉，共享时那会把邻居的中断
             * 一起打死，而邻居完全不知道自己被牵连了。 */
            fe_irq_release_owner(10);
            CHECK(find_irq(LVL_IRQ) != NULL);           /* 线还在 */
            CHECK(e && e->nsharer == 1);
            CHECK(e && e->sh[0].owner == 11);
            CHECK(g_vec_slot[lvec] != 0xFF);            /* 向量没被释放 */

            /* 邻居仍然收得到中断，退出的那家不再收到 */
            nt->bits = 0;
            nt2->bits = 0;
            gsi_isr(&r2);
            CHECK((nt2->bits & (1ull << LVL_IRQ)) != 0);
            CHECK((nt->bits & (1ull << LVL_IRQ)) == 0);

            /* 最后一家退出：这才释放向量 */
            CHECK(fe_ok(fe_irq_ack_owner(LVL_IRQ, 11)));
            fe_irq_release_owner(11);
            CHECK(find_irq(LVL_IRQ) == NULL);
            CHECK(g_vec_slot[lvec] == 0xFF);
        }
    }

    /* ===== 反向对照：欠着确认的那家退出，线不能就此卡死 ===== */
    {
        struct fe_irq_info i1;
        if (irq_install(LVL_IRQ, nt, 20, false, FE_IRQ_MODE_ANY, &i1) == FE_OK) {
            CHECK(irq_install(LVL_IRQ, nt2, 21, false, FE_IRQ_MODE_ANY, &i1) == FE_OK);
            struct fe_irq_entry *e = find_irq(LVL_IRQ);
            struct fe_regs r3;
            memset(&r3, 0, sizeof(r3));
            r3.vector = e ? e->vector : 0;
            gsi_isr(&r3);                       /* 两家都欠一次确认 */
            CHECK(e && e->pending == 2);
            CHECK(fe_ok(fe_irq_ack_owner(LVL_IRQ, 20)));   /* 20 还了，21 欠着 */
            CHECK(e && e->pending == 1);
            /* ★ 20 退出 → 它已经还清了，pending 不该再减 ★ */
            fe_irq_release_owner(20);
            CHECK(e && e->pending == 1);
            /* 21 还清 → 线必须放行（不能因为"退出的人被多减了一次"而卡住） */
            CHECK(fe_ok(fe_irq_ack_owner(LVL_IRQ, 21)));
            CHECK(e && !e->masked && e->pending == 0);
            fe_irq_release_owner(21);
            CHECK(find_irq(LVL_IRQ) == NULL);
        }
    }

    fe_status_t c1 = fe_handle_close(&kt->handles, h);
    fe_status_t c2 = fe_handle_close(&kt->handles, h2);
    if (fe_failed(c1) || fe_failed(c2)) {
        /* 说不清楚就等于没说：把句柄值与返回码一起打出来 */
        fe_kprintf("        **关闭通知句柄失败** h=%u(%s) h2=%u(%s)\n",
                   (unsigned)h, fe_status_name(c1),
                   (unsigned)h2, fe_status_name(c2));
        fail++;
    }

    /* ================= MSI 的参数校验（D5c 的反向对照） =================
     *
     * ★ 这一组不碰任何真实设备 ★ 全部走"拒绝"那几条路径：
     * 内核照着用户态给的偏移去写配置空间，而那个偏移是**用户态算出来的**——
     * 算错了就可能改掉设备的 BAR 或命令寄存器。所以"这到底是不是一个
     * MSI 能力结构"必须先问清楚。
     *
     * 正向（真的使能 MSI-X 并收到中断）由 blkd 在**真实设备**上验：
     * 那条路需要一台真设备，而自检**不该去动真设备的配置空间**——
     * 那会让自检依赖机器，也会在别人的机器上改掉设备状态。 */
    {
        struct fe_msi_info mi;
        /* 1. 不是 MSI/MSI-X 的偏移：0x40 上是 virtio 的 capability（0x09） */
        CHECK(fe_irq_msi_alloc(0, 4, 0, 0x40, 0, nt, 1, &mi) == FE_ERR_INVAL);
        /* 2. 设备不存在：配置空间读回全 1 → cap id 0xFF → 拒绝。
         *    ★ 这一条很重要 ★ "设备不存在"与"设备存在但没有 MSI"
         *    必须走同一条拒绝路径，否则会去写一个不存在设备的 BAR。 */
        CHECK(fe_irq_msi_alloc(0x1F, 0x1F, 7, 0x98, 0, nt, 1, &mi) == FE_ERR_INVAL);
        /* 3. 偏移越界（能力结构只能在 0x40..0xFF，且 4 字节对齐） */
        CHECK(fe_irq_msi_alloc(0, 4, 0, 0x30, 0, nt, 1, &mi) == FE_ERR_INVAL);
        CHECK(fe_irq_msi_alloc(0, 4, 0, 0x99, 0, nt, 1, &mi) == FE_ERR_INVAL);
        /* 4. 参数为空 */
        CHECK(fe_irq_msi_alloc(0, 4, 0, 0x98, 0, NULL, 1, &mi) == FE_ERR_INVAL);
        CHECK(fe_irq_msi_alloc(0, 4, 0, 0x98, 0, nt, 1, NULL) == FE_ERR_INVAL);
        /* 5. 释放一个不存在的 MSI 条目 */
        CHECK(fe_irq_msi_free(40, 1) == FE_ERR_NOENT);
        /* 6. 一个从没登记过的号，确认也是 NOENT */
        CHECK(fe_irq_ack(40) == FE_ERR_NOENT);
    }
    return fail;
}
