/* SPDX-License-Identifier: 0BSD */
/* 硬件资源池的实现。
 *
 * 数据结构刻意做得极简：两个定长数组（空闲区间 / 已认领区间）+ 线性扫描。
 * 驱动认领资源的频率是「启动时几次」，不值得为它引入红黑树；
 * 定长数组还顺带保证了内核代码里没有动态分配的失败路径。
 */
#include <fe/resource.h>
#include <fe/mm/pmm.h>     /* 自检要取一帧真内存来验证"可用内存不能被申报为 MMIO" */
#include <fe/mm.h>
#include <fe/string.h>
#include <fe/kprintf.h>
#include <fe/sched/thread.h>
#include <fe/sched/sched.h>
#include <fe/io.h>
#include <fe/task.h>
#include <fe/process.h>     /* D 组要用 fe_task_kill_other_threads / 等待死透 */

#define FE_RES_FLAG_SHARED 1u

struct fe_res_entry {
    u64 base;
    u64 len;
    u64 owner;                          /* 独占持有者；共享条目恒为 0 */
    u64 sharers[FE_RES_SHARERS_MAX];    /* 共享持有者列表 */
    struct fe_thread *lock_holder;      /* 共享区间上的控制器锁 */
    struct fe_thread *lock_waiter;
    u32 sharer_count;
    u32 kind;
    u32 flags;
    u32 used;
};

static struct fe_res_entry g_pool[FE_RES_MAX];      /* 空闲区间 */
static struct fe_res_entry g_owned[FE_RES_MAX];     /* 已认领区间，用于归还 */

/* ------------------------------------------------------------------ */
/* 条目管理                                                            */
/* ------------------------------------------------------------------ */

static struct fe_res_entry *entry_alloc(struct fe_res_entry *tab)
{
    for (u32 i = 0; i < FE_RES_MAX; i++) {
        if (!tab[i].used) {
            memset(&tab[i], 0, sizeof(tab[i]));
            tab[i].used = 1;
            return &tab[i];
        }
    }
    return NULL;
}

static void entry_free(struct fe_res_entry *e)
{
    memset(e, 0, sizeof(*e));
}

static u32 free_slots(const struct fe_res_entry *tab)
{
    u32 n = 0;
    for (u32 i = 0; i < FE_RES_MAX; i++) {
        if (!tab[i].used) {
            n++;
        }
    }
    return n;
}

static u32 used_count(const struct fe_res_entry *tab)
{
    u32 n = 0;
    for (u32 i = 0; i < FE_RES_MAX; i++) {
        if (tab[i].used) {
            n++;
        }
    }
    return n;
}

/* 两个区间是否重叠。空区间（len=0）永不重叠。 */
static bool overlap(u64 a0, u64 a1, u64 b0, u64 b1)
{
    return a0 < b1 && b0 < a1;
}

static const char *kind_name(u32 kind)
{
    switch (kind) {
    case FE_RES_IOPORT: return "端口";
    case FE_RES_MMIO:   return "MMIO";
    case FE_RES_IRQ:    return "IRQ";
    default:            return "?";
    }
}

static bool kind_valid(u32 kind)
{
    return kind == FE_RES_IOPORT || kind == FE_RES_MMIO || kind == FE_RES_IRQ;
}

/* ------------------------------------------------------------------ */
/* 对外的池子操作                                                      */
/* ------------------------------------------------------------------ */

void fe_resource_init(void)
{
    memset(g_pool, 0, sizeof(g_pool));
    memset(g_owned, 0, sizeof(g_owned));
}

static fe_status_t pool_add_common(u32 kind, u64 base, u64 len, u32 flags)
{
    if (!kind_valid(kind) || len == 0) {
        return FE_ERR_INVAL;
    }
    if (kind == FE_RES_IOPORT && (base + len) > 0x10000ull) {
        return FE_ERR_RANGE;        /* 端口号只有 16 位 */
    }
    if (kind == FE_RES_IRQ && (len != 1 || base > 255)) {
        return FE_ERR_INVAL;
    }
    if (kind == FE_RES_MMIO) {
        /* MMIO 一律按页对齐记录：映射是按页做的，池子里的区间若与页边界错开，
         * 驱动按页对齐认领时就永远匹配不上池子里的条目。 */
        u64 a0 = FE_FRAME_ALIGN_DOWN(base);
        u64 a1 = FE_FRAME_ALIGN_UP(base + len);
        base = a0;
        len = a1 - a0;

        /* ★ 可用内存绝不能当 MMIO ★
         *
         * 这是整个资源池最重要的一条护栏：放过去的话，把这段"设备寄存器"
         * 映射给驱动就是给它一个**可写映射到内核/别人的内存**——
         * 内存保护于是变成"取决于设备管理器守不守规矩"。
         *
         * ★ 它必须在**机制层**，不能只在 syscall 入口 ★
         * 第一版把它写在 `sys_resource_pool_add` 里，机制函数一点没查。
         * 自检直接调机制函数，于是一条都没挡住——实测打印出了
         * "可用内存 0x14f000 竟然能被申报为 MMIO"。
         * 这暴露的不是"少写一个检查"，而是**分层放错了**：
         * 机制的不变式必须由机制自己维持，否则每一个新调用路径
         * （引导期入池、自检、将来的设备管理器、以及我还没写的代码）
         * 都要各自记得再查一遍，**漏一个就等于没有**。
         * 入口层该管的是"谁能调"与"报错准不准"，那是策略与体验。 */
        if (fe_pmm_is_usable(base, len)) {
            fe_kprintf("[资源] 拒绝：%#llx+%#llx 是可用内存，不能入池为 MMIO\n",
                       (unsigned long long)base, (unsigned long long)len);
            return FE_ERR_ACCESS;
        }
    }
    u64 end = base + len;
    if (end < base) {
        return FE_ERR_OVERFLOW;
    }
    /* 池子必须保持「一组互不相交的区间」，否则认领语义会变得含混：
     * 同一个区间出现在两个空闲条目里，两个驱动就都能认领成功了。 */
    for (u32 i = 0; i < FE_RES_MAX; i++) {
        if (!g_pool[i].used || g_pool[i].kind != kind) {
            continue;
        }
        if (overlap(g_pool[i].base, g_pool[i].base + g_pool[i].len, base, end)) {
            return FE_ERR_EXIST;
        }
    }
    struct fe_res_entry *e = entry_alloc(g_pool);
    if (!e) {
        return FE_ERR_NOSPC;
    }
    e->kind = kind;
    e->base = base;
    e->len = len;
    e->flags = flags;
    return FE_OK;
}

fe_status_t fe_resource_pool_add(u32 kind, u64 base, u64 len)
{
    return pool_add_common(kind, base, len, 0);
}

fe_status_t fe_resource_pool_add_shared(u32 kind, u64 base, u64 len)
{
    return pool_add_common(kind, base, len, FE_RES_FLAG_SHARED);
}

/* 在池子里找到完全包含 [base, end) 的条目；pool_only=false 时也允许返回共享条目的
 * 判据由调用方决定。找不到返回 NULL。 */
static struct fe_res_entry *pool_find_containing(u32 kind, u64 base, u64 end)
{
    for (u32 i = 0; i < FE_RES_MAX; i++) {
        struct fe_res_entry *p = &g_pool[i];
        if (!p->used || p->kind != kind) {
            continue;
        }
        if (p->base <= base && end <= p->base + p->len) {
            return p;
        }
    }
    return NULL;
}

bool fe_resource_is_shared(u32 kind, u64 base, u64 len)
{
    struct fe_res_entry *p = pool_find_containing(kind, base, base + len);
    return p && (p->flags & FE_RES_FLAG_SHARED);
}

fe_status_t fe_resource_claim(u32 kind, u64 base, u64 len, u64 owner_id)
{
    if (!kind_valid(kind) || len == 0 || owner_id == 0) {
        return FE_ERR_INVAL;
    }
    u64 end = base + len;
    if (end < base) {
        return FE_ERR_OVERFLOW;
    }

    /* 已经被别人拿走？先查已认领表，好给出「忙」而不是含混的「不存在」。 */
    for (u32 i = 0; i < FE_RES_MAX; i++) {
        const struct fe_res_entry *o = &g_owned[i];
        if (!o->used || o->kind != kind) {
            continue;
        }
        if (overlap(o->base, o->base + o->len, base, end)) {
            /* ★ BUSY 必须说清楚"被谁、哪一段占了" ★
             * 只回一个 -6 的话，调用者（和排查的人）无法区分：
             *   - 池子里那条资源根本没被标成可共享（申报方的问题）；
             *   - 别人拿的是**相邻**的一段（范围算错）；
             *   - 自己已经持有（那是 EXIST，不是 BUSY）。
             * 实测吃过这个亏：irqtest 认领共享的 IRQ11 拿到 BUSY，
             * 而池子里明明写着"（共享）"——因为冲突来自**已认领表**里
             * 另一段区间。没有这三行就只能靠猜。 */
            fe_kprintf("[资源] BUSY：%u 类 %#llx+%#llx 与任务 %llu 持有的 "
                       "%#llx+%#llx 重叠\n",
                       kind, (unsigned long long)base, (unsigned long long)len,
                       (unsigned long long)o->owner,
                       (unsigned long long)o->base, (unsigned long long)o->len);
            return (o->owner == owner_id) ? FE_ERR_EXIST : FE_ERR_BUSY;
        }
    }

    /* 共享条目：不从池中移除，只是把持有者记进列表。
     * 判据是「池子里有这么一段、且它被标记为可共享」——独占条目走下面的切分路径。 */
    {
        struct fe_res_entry *sh = pool_find_containing(kind, base, end);
        if (sh && (sh->flags & FE_RES_FLAG_SHARED)) {
            for (u32 i = 0; i < sh->sharer_count; i++) {
                if (sh->sharers[i] == owner_id) {
                    return FE_ERR_EXIST;        /* 自己已经持有 */
                }
            }
            if (sh->sharer_count >= FE_RES_SHARERS_MAX) {
                return FE_ERR_NOSPC;
            }
            sh->sharers[sh->sharer_count++] = owner_id;
            return FE_OK;
        }
    }

    /* 在池子里找一段**完全包含**请求的空闲区间 */
    for (u32 i = 0; i < FE_RES_MAX; i++) {
        struct fe_res_entry *p = &g_pool[i];
        if (!p->used || p->kind != kind) {
            continue;
        }
        u64 p0 = p->base, p1 = p->base + p->len;
        if (p0 > base || end > p1) {
            continue;
        }
        /* ★ 切分一个**共享**条目是错的，必须报出来 ★
         * 走到这里说明上面的共享分支没匹配上（旗标丢了、或者查表没找到）。
         * 后果很隐蔽：共享条目被切走一段进独占表，于是第二家来认领时
         * 拿到的是 BUSY——而池子里明明写着"（共享）"。 */
        if (p->flags & FE_RES_FLAG_SHARED) {
            fe_kprintf("[资源] **正在切分一个共享条目**：%u 类 %#llx+%#llx "
                       "flags=%#x（认领 %#llx+%#llx）——共享分支没匹配上\n",
                       kind, (unsigned long long)p0, (unsigned long long)p->len,
                       p->flags, (unsigned long long)base,
                       (unsigned long long)(end - base));
        }
        u64 left_len = base - p0;
        u64 right_len = p1 - end;

        /* 先把槽位凑齐再动手：否则切到一半失败会丢掉整段资源。 */
        u32 need = 1 + (left_len ? 1u : 0u) + (right_len ? 1u : 0u);
        if (free_slots(g_pool) + free_slots(g_owned) < need) {
            return FE_ERR_NOSPC;
        }

        u32 saved_kind = p->kind;
        entry_free(p);

        if (left_len) {
            struct fe_res_entry *l = entry_alloc(g_pool);
            l->kind = saved_kind;
            l->base = p0;
            l->len = left_len;
        }
        if (right_len) {
            struct fe_res_entry *r = entry_alloc(g_pool);
            r->kind = saved_kind;
            r->base = end;
            r->len = right_len;
        }
        struct fe_res_entry *own = entry_alloc(g_owned);
        own->kind = saved_kind;
        own->base = base;
        own->len = len;
        own->owner = owner_id;
        return FE_OK;
    }
    return FE_ERR_NOENT;
}

/* 归还之后把同一个 kind 的相邻/重叠空闲区间合并，避免反复认领归还把池子切碎。
 *
 * ★ 共享条目**不参与合并**（D5a 期间查出来的 bug）★
 *
 * 这个函数原来只看 kind 与区间，从不看 `flags`。于是：
 *   - init 把 IRQ11 申报成**可共享**（`[11,1) flags=SHARED`），
 *     而引导期池子里本来就有相邻的 `[12,1)`（鼠标）——
 *     `fe_resource_release_owner` 一调用就把两条并成 `[11,2) flags=0`，
 *     **共享标记被抹掉**；
 *   - 后果不是"合并得不好看"，而是"第二个驱动认领同一条线时拿到 BUSY"，
 *     而池子里明明写着"（共享）"。查这个现象要一路读到这里才行。
 *
 * 还有第二个原因不能合并共享条目：共享条目的身份不只是"一段区间"，
 * 还有 `sharers[]`（谁持有它）。`entry_free` 会把被并掉那一条的
 * 持有者列表**丢掉**——那些任务于是从"已认领"变成了"没记录"，
 * 归还时再也摘不掉自己。
 *
 * 代价：相邻的共享条目会各占一格，池子碎一点。共享区间很少
 * （今天只有 PS/2 端口与 IRQ），碎一点完全可以接受；
 * 而"合并之后含义变了"是不可接受的。 */
static void pool_merge(void)
{
    bool again = true;
    while (again) {
        again = false;
        for (u32 i = 0; i < FE_RES_MAX && !again; i++) {
            if (!g_pool[i].used || (g_pool[i].flags & FE_RES_FLAG_SHARED)) {
                continue;
            }
            for (u32 j = i + 1; j < FE_RES_MAX; j++) {
                if (!g_pool[j].used || g_pool[j].kind != g_pool[i].kind) {
                    continue;
                }
                if (g_pool[j].flags & FE_RES_FLAG_SHARED) {
                    continue;       /* 同上：不把共享条目并进独占条目 */
                }
                u64 i0 = g_pool[i].base, i1 = i0 + g_pool[i].len;
                u64 j0 = g_pool[j].base, j1 = j0 + g_pool[j].len;
                if (!overlap(i0, i1, j0, j1) && i1 != j0 && j1 != i0) {
                    continue;       /* 既不相邻也不重叠 */
                }
                g_pool[i].base = (i0 < j0) ? i0 : j0;
                g_pool[i].len = ((i1 > j1) ? i1 : j1) - g_pool[i].base;
                entry_free(&g_pool[j]);
                again = true;
                break;
            }
        }
    }
}

/* 共享区间上的控制器锁的释放（供归还路径与 unlock 共用）。
 * 持锁线程所在的任务消失时必须自动放锁，否则这把锁就永远锁死了。 */
static void lock_forget_owner(struct fe_res_entry *e, u64 owner_id)
{
    if (e->lock_holder && e->lock_holder->task && e->lock_holder->task->id == owner_id) {
        e->lock_holder = NULL;
        struct fe_thread *w = e->lock_waiter;
        if (w) {
            e->lock_waiter = NULL;
            fe_sched_wake(w);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 设备管理器能力                                                      */
/* ------------------------------------------------------------------ */

/* 谁是设备管理器。0 = 还没有（引导期入池阶段就是 0）。 */
static u64 g_devmgr_task;

void fe_resource_set_devmgr(u64 task_id)
{
    g_devmgr_task = task_id;
    if (task_id) {
        fe_kprintf("[资源] 任务 %llu 被指定为设备管理器（可向资源池申报硬件）\n",
                   (unsigned long long)task_id);
    }
}

bool fe_resource_is_devmgr(u64 task_id)
{
    return task_id != 0 && g_devmgr_task == task_id;
}

/* 任务退出时收回身份：否则任务 id 复用之后，另一个任务会**继承**这个能力。
 * 这类"身份跟着 id 走"的漏洞在 id 是单调递增时看不出来，
 * 但 id 一旦复用就是提权。 */
void fe_resource_clear_devmgr(u64 task_id)
{
    if (task_id && g_devmgr_task == task_id) {
        g_devmgr_task = 0;
    }
}

/* ---- 设备管理器身份由**引导者自己认领** ----
 *
 * ★ 为什么不是"内核按名字指定"★
 * 内核不认识 "devmgr" 这个名字（那是策略）。内核只需要回答一个更小的问题：
 * **谁是引导者**。答案是 init —— 它是内核 exec 的第一个用户任务
 * （`fe_process_start_init`），这一点内核本来就知道。
 *
 * 于是规则可以写得很紧：
 *   - 只有 init 自己能认领；
 *   - 认领之后身份唯一（再也认领不到）；
 *   - init 退出时随对象销毁自动失效（复用 fe_resource_clear_devmgr）。
 *
 * ★ 为什么需要它（而不是内核直接指定 pcid/devmgr）★
 * 引导链上"谁来申报 BAR"是**策略**，要能改而不动内核。今天由 init 自己
 * 认领、然后由它把"读配置空间 + 申报 + 授予"交给 pcid 做；
 * 将来换成独立的 devmgr 服务，改的是 init 的表，内核一行不动。 */
static u64 g_init_task;

void fe_resource_note_init(u64 task_id)
{
    g_init_task = task_id;
}

bool fe_resource_claim_devmgr(u64 task_id)
{
    if (task_id == 0 || task_id != g_init_task || g_devmgr_task != 0) {
        return false;
    }
    fe_resource_set_devmgr(task_id);
    return true;
}


fe_status_t fe_resource_grant(u32 kind, u64 base, u64 len,
                              u64 from_owner, u64 to_owner)
{
    if (!kind_valid(kind) || len == 0 || from_owner == 0 || to_owner == 0) {
        return FE_ERR_INVAL;
    }
    if (from_owner == to_owner) {
        return FE_ERR_INVAL;        /* 转给自己：多半是调用者的逻辑错 */
    }
    u64 flags = fe_irq_save();
    /* 独占资源：改 owner 即可 —— 但必须确认**确实是它的**。
     * 不检查的话，任何任务都能"把它人的资源转给自己"，那是提权。 */
    for (u32 i = 0; i < FE_RES_MAX; i++) {
        struct fe_res_entry *o = &g_owned[i];
        if (!o->used || o->kind != kind || o->base != base || o->len != len) {
            continue;
        }
        if (o->owner != from_owner) {
            fe_irq_restore(flags);
            return FE_ERR_ACCESS;
        }
        o->owner = to_owner;
        /* 控制器锁也跟着走：否则旧持有者退出时会把新持有者的锁放掉。
         * 锁属于线程而不是任务，这里只把"锁的持有者属于旧任务"这种情况清掉。 */
        lock_forget_owner(o, from_owner);
        fe_irq_restore(flags);
        return FE_OK;
    }
    /* 共享资源：把 sharers 里的 from 换成 to（保持共享语义） */
    for (u32 i = 0; i < FE_RES_MAX; i++) {
        struct fe_res_entry *e = &g_pool[i];
        if (!e->used || e->kind != kind || !(e->flags & FE_RES_FLAG_SHARED)) {
            continue;
        }
        if (base < e->base || base + len > e->base + e->len) {
            continue;
        }
        for (u32 k = 0; k < e->sharer_count; k++) {
            if (e->sharers[k] == from_owner) {
                e->sharers[k] = to_owner;
                fe_irq_restore(flags);
                return FE_OK;
            }
        }
    }
    fe_irq_restore(flags);
    return FE_ERR_NOENT;            /* 这段资源不在它名下 */
}

void fe_resource_release_owner(u64 owner_id)
{
    if (owner_id == 0) {
        return;
    }
    bool any = false;
    for (u32 i = 0; i < FE_RES_MAX; i++) {
        struct fe_res_entry *o = &g_owned[i];
        if (!o->used || o->owner != owner_id) {
            continue;
        }
        struct fe_res_entry *p = entry_alloc(g_pool);
        if (p) {
            p->kind = o->kind;
            p->base = o->base;
            p->len = o->len;
        } else {
            /* 池子满了：宁可泄漏这一段也不能让两个条目指向同一区间 */
            fe_kprintf("[资源] 警告：池子已满，%s %#llx+%#llx 未能归还\n",
                       kind_name(o->kind), (unsigned long long)o->base,
                       (unsigned long long)o->len);
        }
        entry_free(o);
        any = true;
    }

    /* 共享区间：从持有者列表里划掉自己，并放掉可能还握着的控制器锁 */
    for (u32 i = 0; i < FE_RES_MAX; i++) {
        struct fe_res_entry *e = &g_pool[i];
        if (!e->used || !(e->flags & FE_RES_FLAG_SHARED)) {
            continue;
        }
        lock_forget_owner(e, owner_id);
        for (u32 k = 0; k < e->sharer_count; k++) {
            if (e->sharers[k] != owner_id) {
                continue;
            }
            for (u32 m = k + 1; m < e->sharer_count; m++) {
                e->sharers[m - 1] = e->sharers[m];
            }
            e->sharer_count--;
            break;
        }
    }

    if (any) {
        pool_merge();
    }
}

/* ------------------------------------------------------------------ */
/* 共享区间上的控制器锁                                                */
/* ------------------------------------------------------------------ */

static struct fe_res_entry *shared_entry_find(u32 kind, u64 base, u64 len)
{
    struct fe_res_entry *e = pool_find_containing(kind, base, base + len);
    if (!e || !(e->flags & FE_RES_FLAG_SHARED)) {
        return NULL;
    }
    return e;
}

fe_status_t fe_resource_lock(u32 kind, u64 base, u64 len, u64 owner_id)
{
    struct fe_res_entry *e = shared_entry_find(kind, base, len);
    if (!e) {
        return FE_ERR_NOTSUP;       /* 锁只对共享区间有意义 */
    }
    struct fe_thread *self = fe_thread_current();
    if (!self) {
        return FE_ERR_INVAL;
    }

    for (;;) {
        u64 flags = fe_irq_save();
        if (!e->lock_holder) {
            e->lock_holder = self;
            fe_irq_restore(flags);
            return FE_OK;
        }
        if (e->lock_holder == self) {
            /* 同一个线程重复加锁：这是 bug，明说而不是静静放行 */
            fe_irq_restore(flags);
            return FE_ERR_BUSY;
        }
        e->lock_waiter = self;
        fe_irq_restore(flags);
        fe_sched_block_current();
    }
}

fe_status_t fe_resource_unlock(u32 kind, u64 base, u64 len, u64 owner_id)
{
    (void)owner_id;
    struct fe_res_entry *e = shared_entry_find(kind, base, len);
    if (!e) {
        return FE_ERR_NOTSUP;
    }
    u64 flags = fe_irq_save();
    if (!e->lock_holder) {
        fe_irq_restore(flags);
        return FE_ERR_NOENT;        /* 没锁却来解锁 */
    }
    if (e->lock_holder != fe_thread_current()) {
        fe_irq_restore(flags);
        return FE_ERR_ACCESS;       /* 不是自己锁的 */
    }
    e->lock_holder = NULL;
    struct fe_thread *w = e->lock_waiter;
    if (w) {
        e->lock_waiter = NULL;
    }
    fe_irq_restore(flags);
    if (w) {
        fe_sched_wake(w);
    }
    return FE_OK;
}

/* ---- 只读访问器：让"谁握着锁"成为**可读的事实** ----
 *
 * ★ 为什么必须有它（而不是靠 fe_resource_dump 打印）★
 * `g_pool` 是本文件的 static，池外读不到；而 `fe_resource_dump` 只打印，
 * 打印出来的东西不能当**断言判据**（"跑起来没崩"与"看到的事实对"是两回事）。
 * D4 的断言①（持锁线程死透之后 holder 必须是 NULL）与 D2① 的反向对照
 * 都要把 `lock_holder` 这个槽读出来。它**只看不写**，也不改变任何状态。 */
struct fe_thread *fe_resource_lock_holder(u32 kind, u64 base, u64 len)
{
    struct fe_res_entry *e = shared_entry_find(kind, base, len);
    return e ? e->lock_holder : NULL;
}

/* ------------------------------------------------------------------ */
/* 诊断与自检                                                          */
/* ------------------------------------------------------------------ */

u32 fe_resource_owned_count(u64 owner_id)
{
    u32 n = 0;
    for (u32 i = 0; i < FE_RES_MAX; i++) {
        if (g_owned[i].used && g_owned[i].owner == owner_id) {
            n++;
        }
    }
    return n;
}

u32 fe_resource_pool_count(void)
{
    return used_count(g_pool);
}

void fe_resource_dump(void)
{
    fe_kprintf("  空闲资源区间（%u 段）：\n", used_count(g_pool));
    for (u32 i = 0; i < FE_RES_MAX; i++) {
        if (!g_pool[i].used) {
            continue;
        }
        fe_kprintf("    %-5s %#010llx + %#llx%s\n", kind_name(g_pool[i].kind),
                   (unsigned long long)g_pool[i].base,
                   (unsigned long long)g_pool[i].len,
                   (g_pool[i].flags & FE_RES_FLAG_SHARED) ? "  [共享]" : "");
        if (g_pool[i].flags & FE_RES_FLAG_SHARED) {
            fe_kprintf("          持有者:");
            for (u32 k = 0; k < g_pool[i].sharer_count; k++) {
                fe_kprintf(" %llu", (unsigned long long)g_pool[i].sharers[k]);
            }
            if (g_pool[i].lock_holder) {
                fe_kprintf("   （控制器锁被线程 %s 持有）", g_pool[i].lock_holder->name);
            }
            fe_kprintf("\n");
        }
    }
    fe_kprintf("  已认领区间（%u 段）：\n", used_count(g_owned));
    for (u32 i = 0; i < FE_RES_MAX; i++) {
        if (g_owned[i].used) {
            fe_kprintf("    %-5s %#010llx + %#llx  任务 %llu\n",
                       kind_name(g_owned[i].kind),
                       (unsigned long long)g_owned[i].base,
                       (unsigned long long)g_owned[i].len,
                       (unsigned long long)g_owned[i].owner);
        }
    }
}

#define CHECK(cond) do { if (!(cond)) { fail++; } } while (0)

/* ================================================================== */
/* D 组：先证明这四条缺陷今天真的存在（docs/13-tasks-and-kill.md §6.7） */
/*                                                                    */
/* ★ 为什么落在这里而不是 fe_selftest_kill ★                           */
/* 资源池只有 fe_resource_init() 一种复位手段，而"造共享条目 → 最后复位" */
/* 这套纪律本来就在本文件里；main.c 在资源自检之后会重新 bootstrap 一次   */
/* 真实池子，所以这里造出来的假条目不会留给真实驱动。                    */
/* ★ 这一版**只有自检、没有任何修复** ★ 所以它们应当如实变红。           */
/* ================================================================== */

/* 有界轮询：等一个 volatile 标志变真（每轮真的睡 5 ms——让出不会让时间前进，
 * 而这里等的就是"另一次调度/另一个线程跑完"）。 */
static int wait_flag_ms(volatile u32 *flag, u32 rounds)
{
    for (u32 i = 0; i < rounds; i++) {
        if (*flag) {
            return 1;
        }
        fe_thread_sleep_ms(5);
    }
    return *flag ? 1 : 0;
}

/* ---- D2① 的探针：阻塞在共享区间的控制器锁上 ----
 *
 * ★ 这个线程身上有两个可观察的事实 ★
 *   - `e->lock_waiter == 它`（登记在锁上，§6.3 表里第 4 行的那个登记）；
 *   - `state == FE_THREAD_BLOCKED`（真的睡下去了）。
 * 缺了取消点的后果：被叫醒之后它回到循环顶、条件仍不成立，于是
 * **重新登记再睡**——状态从 BLOCKED 回到 BLOCKED；就算它最后死在闸门上，
 * 那次调用也**永远不会返回**，于是"谁握着这把锁 / 谁在等"在它死前一直是它自己。 */
static volatile u32 g_d2_entered;
static volatile u32 g_d2_done;
static volatile i32 g_d2_status;
static void probe_d2_lock(void *arg)
{
    (void)arg;
    g_d2_entered = 1;
    fe_status_t s = fe_resource_lock(FE_RES_IOPORT, 0x70, 4, 0);
    g_d2_status = s;
    g_d2_done = 1;
    fe_thread_exit(0);
}

/* ---- D4 的探针（按 arg 区分的三种角色）----
 *
 * ★ 为什么要有一个"持有者自己退出"的角色 ★
 * 走 fe_task_terminate 的话整个任务会被销毁，`fe_resource_release_owner`
 * 会顺带把锁放掉——那样断言会**假绿**。自己 fe_thread_exit(0) 时任务
 * 还活着，于是状态与"exec 杀掉线程"之后完全一样：
 * 锁卡住 + lock_holder 指向一个已经死掉的线程。 */
#define D4_ROLE_HOLD_AND_EXIT 1     /* 拿住 slot_d4 的锁，然后自己退出 */
#define D4_ROLE_HOLD_AND_STAY 2     /* 拿住 slot_a0 的锁，一直握着（反向对照②）*/
#define D4_ROLE_WAIT          3     /* 阻塞在 slot_d4 的锁上 */

static volatile u32 g_d4_holding;       /* 两个"持有"角色共用的到达标志 */
static volatile u32 g_d4_exited;

static void probe_d4(void *arg)
{
    long role = (long)arg;
    if (role == D4_ROLE_HOLD_AND_EXIT) {
        if (fe_ok(fe_resource_lock(FE_RES_IOPORT, 0x80, 4, 0))) {
            g_d4_holding = 1;
        }
        g_d4_exited = 1;
        fe_thread_exit(0);
    }
    if (role == D4_ROLE_HOLD_AND_STAY) {
        if (fe_ok(fe_resource_lock(FE_RES_IOPORT, 0xA0, 4, 0))) {
            g_d4_holding = 1;
        }
        for (;;) {
            fe_thread_yield();          /* 一直握着，直到自检收尾 */
        }
    }
    if (role == D4_ROLE_WAIT) {
        (void)fe_resource_lock(FE_RES_IOPORT, 0x80, 4, 0);
        fe_thread_exit(0);
    }
    fe_thread_exit(0);
}

/* 造一个探针任务并挂上探针线程。
 *
 * ★ 为什么要临时换当前线程的 task ★
 * `fe_thread_create` 把新线程的 task 取成"当前任务"的。要让探针线程属于
 * 探针任务（而不是调用者任务），只能在这个窗口里换一下——与 killtest.c
 * 里的做法一样（创建期间不会被抢占：rq_push 那一小段是关中断的）。 */
static struct fe_thread *res_spawn_probe(struct fe_task *task, const char *name,
                                         fe_thread_entry_t entry, void *arg)
{
    struct fe_thread *cur = fe_thread_current();
    struct fe_task *saved = cur ? cur->task : NULL;
    if (cur) {
        cur->task = task;
    }
    struct fe_thread *th = fe_thread_create(name, entry, arg, 16 * 1024,
                                            FE_PRIO_NORMAL);
    if (cur) {
        cur->task = saved;
    }
    if (th && !task->main_thread) {
        task->main_thread = th;
    }
    return th;
}

/* D2①：任务 P 的线程 B 阻塞在**另一个任务**的线程（这里是自检自己）所持有的
 * 共享区间控制器锁上；标记 + 杀之后 B 必须在有界轮次内 DEAD，
 * 而且它那次调用必须**返回 FE_ERR_CANCELED**。
 *
 * 反向对照 C：另一个任务的线程也阻塞在同一把锁上，但**没有被标记**——
 * 它在被杀线程死透的那一刻必须仍然 BLOCKED；锁一旦正常放掉，它必须能拿到。 */
static u32 selftest_d2_reslock(void)
{
    u32 fail = 0;
    fe_resource_init();
    CHECK(fe_ok(fe_resource_pool_add_shared(FE_RES_IOPORT, 0x70, 4)));
    /* 自检自己（内核任务的主线程）先拿住锁：探针一定能阻塞。 */
    CHECK(fe_ok(fe_resource_lock(FE_RES_IOPORT, 0x70, 4, 0)));

    struct fe_task *p = fe_task_create_kernel("d2-p");
    struct fe_task *r = fe_task_create_kernel("d2-r");
    if (!p || !r) {
        fe_kprintf("        探针任务创建失败\n");
        return fail + 1;
    }
    g_d2_entered = 0;
    g_d2_done = 0;
    g_d2_status = 0;
    struct fe_thread *b = res_spawn_probe(p, "d2-lock", probe_d2_lock, NULL);
    if (!b || !wait_flag_ms(&g_d2_entered, 20)) {
        fe_kprintf("        D2① 探针没有跑起来\n");
        fail++;
    } else {
        /* 反向对照 C：另一个任务里、同样阻塞在这把锁上的线程。 */
        struct fe_thread *c = res_spawn_probe(r, "d2-ctrl", probe_d2_lock, NULL);
        /* ★ 先证明探针处在预期状态：两个都真的 BLOCKED 在这把锁上 ★ */
        u32 blocked = 0;
        for (u32 i = 0; i < 40 && blocked < 2; i++) {
            fe_thread_sleep_ms(5);
            blocked = ((b->state == FE_THREAD_BLOCKED) ? 1u : 0u) +
                      ((c && c->state == FE_THREAD_BLOCKED) ? 1u : 0u);
        }
        if (blocked < 2) {
            fe_kprintf("        D2① 前置状态不成立：探针没有都进入阻塞"
                       "（%u/2，b=%u c=%u）\n", blocked, b->state,
                       c ? c->state : 0);
            fail++;
        } else {
            fe_kprintf("        D2① 前置状态：b/c 都 BLOCKED（%u），锁的 holder"
                       "=自检自己\n", FE_THREAD_BLOCKED);
            CHECK(fe_task_kill_other_threads(p, fe_thread_current()) == 1);
            /* 有界窗口：100 轮 × 5 ms = 500 ms。 */
            u32 dead_round = 0;
            for (u32 i = 0; i < 100 && b->state != FE_THREAD_DEAD; i++) {
                fe_thread_sleep_ms(5);
                dead_round = i + 1;
            }
            if (b->state != FE_THREAD_DEAD) {
                fe_kprintf("        **D2① 失败**：等控制器锁的线程在 500 ms 内"
                           "仍是状态 %u（杀不掉）\n", b->state);
                fail++;
            } else if (g_d2_done == 0) {
                /* ★ 缺陷期的形态：受害者死在闸门上，而这次调用**永远不返回**。
                 * 于是"谁握着这把锁 / 谁在等"在它死前一直是它自己——
                 * lock_waiter 那个槽留着它的指针，僵尸回收之后就是悬空。 */
                fe_kprintf("        **D2① 缺陷**：受害者已 DEAD，但它那次"
                           "fe_resource_lock **永远没有返回**（done=0）——"
                           "循环顶没有取消判据，锁的等待者登记一直留着它\n");
                fail++;
            } else {
                fe_kprintf("        D2① 修复后：取消判据生效，受害者返回 %d"
                           "（FE_ERR_CANCELED=%d），且已 DEAD（第 %u 轮 ≈%u ms）\n",
                           (int)g_d2_status, (int)FE_ERR_CANCELED,
                           dead_round, dead_round * 5u);
            }
            CHECK(b->state == FE_THREAD_DEAD);
            CHECK(g_d2_done == 1);
            CHECK(g_d2_status == FE_ERR_CANCELED);
            /* ★ 反向对照 C：这一刻必须**还在阻塞** ★ */
            CHECK(c && c->state == FE_THREAD_BLOCKED);
            if (!c || c->state != FE_THREAD_BLOCKED) {
                fe_kprintf("        **D2① 反向对照失效**：没被标记的锁等待者状态"
                           "变成了 %u（取消波及了同一个对象上的其它等待者，"
                           "或者锁被顺手放掉了）\n", c ? c->state : 0);
                fail++;
            } else {
                fe_kprintf("        D2① 反向对照：另一个任务里没被标记的锁等待者"
                           "仍 BLOCKED\n");
            }
            /* 正常放锁：等待者必须能拿到（证明取消判据没有打掉正常获取路径）。 */
            CHECK(fe_ok(fe_resource_unlock(FE_RES_IOPORT, 0x70, 4, 0)));
            struct fe_thread *took = NULL;
            for (u32 i = 0; i < 200 && !took; i++) {
                fe_thread_sleep_ms(5);
                took = fe_resource_lock_holder(FE_RES_IOPORT, 0x70, 4);
            }
            CHECK(took == c);
            if (took != c) {
                fe_kprintf("        **D2① 反向对照失效**：正常放锁之后等待者"
                           "没有拿到锁（holder=%p 期望 %p，c 状态=%u）\n",
                           (void *)took, (void *)c, c ? c->state : 0);
                fail++;
            } else {
                fe_kprintf("        D2① 反向对照：正常放锁之后等待者拿到了锁"
                           "（正常路径没被打掉）\n");
            }
        }
    }
    /* ★ 收尾（第 4 步的变量）★ 探针不许活得比自检久。
     * `d2-ctrl` 今天**清不掉**：它卡在 `fe_resource_lock` 上，而那处的
     * 取消点正是 D2 还没修的缺陷。如实点名，不算失败。 */
    fe_task_cleanup_probes(p, "D2① 受害者");
    fe_task_cleanup_probes(r, "D2① 对照");
    fe_object_unref(&p->hdr);
    fe_object_unref(&r->hdr);
    return fail;
}

/* D4：持锁线程死掉之后锁不放（lock_forget_owner 只在任务销毁/转交时跑）。
 *
 * 断言：
 *   ① 持锁线程死透之后 `fe_resource_lock_holder(slot_d4) == NULL`；
 *   ② 另一个任务的线程随后能在有限轮次内拿到同一把锁。
 *
 * 反向对照：
 *   ① keep（自检自己）握着**另外两把**锁（另两个共享条目）：这一轮之后
 *      那两把的 holder 必须**仍然等于 keep**——将来那个"按线程放锁"的
 *      函数不许按 owner_id 误放调用者自己的锁；
 *   ② 另一个任务里**没有被杀**的持锁线程，holder 必须仍然非 NULL。 */
static u32 selftest_d4_forget(void)
{
    u32 fail = 0;
    fe_resource_init();
    CHECK(fe_ok(fe_resource_pool_add_shared(FE_RES_IOPORT, 0x80, 4)));
    CHECK(fe_ok(fe_resource_pool_add_shared(FE_RES_IOPORT, 0x90, 4)));
    CHECK(fe_ok(fe_resource_pool_add_shared(FE_RES_IOPORT, 0x94, 4)));
    CHECK(fe_ok(fe_resource_pool_add_shared(FE_RES_IOPORT, 0xA0, 4)));

    struct fe_task *t = fe_task_create_kernel("d4-t");
    struct fe_task *v = fe_task_create_kernel("d4-v");
    struct fe_task *z = fe_task_create_kernel("d4-z");
    if (!t || !v || !z) {
        fe_kprintf("        探针任务创建失败\n");
        return fail + 1;
    }
    /* 反向对照①：keep 自己握着另外两把锁（两个不同的共享条目）。 */
    CHECK(fe_ok(fe_resource_lock(FE_RES_IOPORT, 0x90, 4, 0)));
    CHECK(fe_ok(fe_resource_lock(FE_RES_IOPORT, 0x94, 4, 0)));
    CHECK(fe_resource_lock_holder(FE_RES_IOPORT, 0x90, 4) == fe_thread_current());

    /* 反向对照②：另一个任务里没被杀的持锁者。 */
    g_d4_holding = 0;
    struct fe_thread *vh = res_spawn_probe(v, "d4-vhold", probe_d4,
                                           (void *)D4_ROLE_HOLD_AND_STAY);
    if (!vh || !wait_flag_ms(&g_d4_holding, 20)) {
        fe_kprintf("        D4 反向对照②的持锁探针没有拿到锁\n");
        fail++;
    }

    g_d4_holding = 0;
    g_d4_exited = 0;

    /* 任务 T：持锁者（受害者）与锁等待者（受害者）。 */
    struct fe_thread *a = res_spawn_probe(t, "d4-hold", probe_d4,
                                          (void *)D4_ROLE_HOLD_AND_EXIT);
    if (!a || !wait_flag_ms(&g_d4_holding, 20)) {
        fe_kprintf("        D4 持锁探针没有拿到锁\n");
        fail++;
    } else {
        struct fe_thread *b = res_spawn_probe(t, "d4-wait", probe_d4,
                                              (void *)D4_ROLE_WAIT);
        u32 blocked = 0;
        for (u32 i = 0; i < 20 && blocked < 1; i++) {
            fe_thread_sleep_ms(5);
            blocked = (b && b->state == FE_THREAD_BLOCKED) ? 1u : 0u;
        }
        if (blocked < 1) {
            fe_kprintf("        D4 等待探针没有进入阻塞（状态 %u）\n",
                       b ? b->state : 0);
            fail++;
        } else {
            /* 同步点：持锁者真的握着锁（holder 就是断言要读的那个事实）。 */
            CHECK(fe_resource_lock_holder(FE_RES_IOPORT, 0x80, 4) == a);
            /* ★ 走 fe_task_kill_other_threads——那才是 exec 走的路 ★
             * （反向对照的"持锁线程自己退出"是缺陷期的另一条复现路径，
             *   两条在"锁卡住"这个结局上是同一个状态。） */
            CHECK(fe_task_kill_other_threads(t, fe_thread_current()) == 2);
            CHECK(fe_ok(fe_task_wait_others_dead(t, 20000)));
            /* 有界窗口：把 CPU 让出去，让"死在闸门 / 走到取消点"发生完。 */
            for (u32 i = 0; i < 20 && (a->state != FE_THREAD_DEAD ||
                                       b->state != FE_THREAD_DEAD); i++) {
                fe_thread_sleep_ms(2);
            }
            CHECK(a->state == FE_THREAD_DEAD);
            CHECK(b->state == FE_THREAD_DEAD);
            /* ★ 被测的那一条 ★ */
            struct fe_thread *holder =
                fe_resource_lock_holder(FE_RES_IOPORT, 0x80, 4);
            CHECK(holder == NULL);
            if (holder) {
                fe_kprintf("        **D4 失败**：持锁线程已死，"
                           "fe_resource_lock_holder() 仍返回 %p（已死线程）"
                           "——后来者会一直等在这把锁上\n", (void *)holder);
            } else {
                fe_kprintf("        D4 修复后：持锁线程死透之后 holder == NULL\n");
            }
            /* ★ 反向对照① ★ keep 自己那两把锁不许被顺手放掉。 */
            struct fe_thread *h90 = fe_resource_lock_holder(FE_RES_IOPORT, 0x90, 4);
            struct fe_thread *h94 = fe_resource_lock_holder(FE_RES_IOPORT, 0x94, 4);
            /* ★ 反向对照② ★ 没被杀的持锁者仍然是 holder。 */
            struct fe_thread *ha0 = fe_resource_lock_holder(FE_RES_IOPORT, 0xA0, 4);
            CHECK(h90 == fe_thread_current());
            CHECK(h94 == fe_thread_current());
            CHECK(ha0 == vh);
            if (h90 != fe_thread_current() || h94 != fe_thread_current() ||
                ha0 != vh) {
                fe_kprintf("        **D4 反向对照失效**：放锁波及了不该放的锁"
                           "（keep: %p/%p 期望 %p，没被杀的持锁者: %p 期望 %p）\n",
                           (void *)h90, (void *)h94, (void *)fe_thread_current(),
                           (void *)ha0, (void *)vh);
                fail++;
            } else {
                fe_kprintf("        D4 反向对照：keep 自己的两把锁与另一个任务"
                           "没被杀的持锁者都**没有**被放掉\n");
            }
            /* ★ 断言② ★ 另一个任务的线程随后必须能拿到这把锁。
             * 判据用 holder 的转移（不依赖 B 与 Z 谁先跑）。 */
            res_spawn_probe(z, "d4-z", probe_d4, (void *)D4_ROLE_WAIT);
            struct fe_thread *took = NULL;
            for (u32 i = 0; i < 40 && !took; i++) {
                fe_thread_sleep_ms(5);
                took = fe_resource_lock_holder(FE_RES_IOPORT, 0x80, 4);
            }
            CHECK(took != NULL);
            if (!took) {
                fe_kprintf("        **D4 失败**：锁没被放掉，另一个任务的线程"
                           "拿不到（holder 仍为 NULL，B 仍 state=%u）\n",
                           b->state);
            } else {
                fe_kprintf("        D4 修复后：另一个任务的线程随后拿到了同一把锁"
                           "（holder=%p）\n", (void *)took);
            }
        }
    }
    /* 收尾：keep 自己放掉那两把锁，免得被后面的校验当成漏掉的。 */
    fe_resource_unlock(FE_RES_IOPORT, 0x90, 4, 0);
    fe_resource_unlock(FE_RES_IOPORT, 0x94, 4, 0);
    /* ★ 收尾（第 4 步的变量）★ 探针不许活得比自检久：
     *   - `d4-vhold` 是**永久自旋**的（`for(;;) fe_thread_yield();`）——
     *     它是这一刀最该被收掉的那个（一直在烧 CPU、还握着一把锁）；
     *   - `d4-z` 卡在 `fe_resource_lock` 上，与 `d2-ctrl` 一样**清不掉**
     *     （没有取消点），如实点名。 */
    fe_task_cleanup_probes(t, "D4 受害者");
    fe_task_cleanup_probes(v, "D4 对照持锁者");
    fe_task_cleanup_probes(z, "D4 后来者");
    fe_object_unref(&t->hdr);
    fe_object_unref(&v->hdr);
    fe_object_unref(&z->hdr);
    return fail;
}

u32 fe_selftest_resource(void)
{
    u32 fail = 0;

    /* 自检用独立的资源号段，避免与真实驱动抢（0xF000 起是没人用的端口区） */
    fe_resource_init();
    CHECK(fe_ok(fe_resource_pool_add(FE_RES_IOPORT, 0xF000, 16)));
    CHECK(fe_resource_pool_count() == 1);

    /* 重叠的池子条目必须被拒绝 */
    CHECK(fe_resource_pool_add(FE_RES_IOPORT, 0xF008, 8) == FE_ERR_EXIST);
    CHECK(fe_resource_pool_add(FE_RES_IOPORT, 0xEFFC, 8) == FE_ERR_EXIST);
    fe_resource_pool_add(FE_RES_IOPORT, 0xF010, 16);   /* 相邻但不重叠：允许 */
    CHECK(fe_resource_pool_count() == 2);

    /* 不在池子里的区间：不能认领 */
    CHECK(fe_resource_claim(FE_RES_IOPORT, 0xF100, 4, 1) == FE_ERR_NOENT);

    /* 从中间切一段：池子应裂成两段 + 一段已认领 */
    CHECK(fe_ok(fe_resource_claim(FE_RES_IOPORT, 0xF004, 4, 1)));
    CHECK(fe_resource_pool_count() == 3);
    CHECK(fe_resource_owned_count(1) == 1);

    /* 再认领同一段：必须是「忙」，这是能力互斥的关键 */
    CHECK(fe_resource_claim(FE_RES_IOPORT, 0xF004, 4, 2) == FE_ERR_BUSY);
    /* 与已认领区间重叠的一部分：也要挡住 */
    CHECK(fe_resource_claim(FE_RES_IOPORT, 0xF006, 4, 2) == FE_ERR_BUSY);

    /* 从头部对齐切一段 */
    CHECK(fe_ok(fe_resource_claim(FE_RES_IOPORT, 0xF000, 4, 2)));
    /* 池子里已经没有这一段了 */
    CHECK(fe_resource_claim(FE_RES_IOPORT, 0xF0F0, 4, 3) == FE_ERR_NOENT);

    /* 归还任务 1 的那段：0xF004..0xF008 应与右侧两段合并成 0xF004..0xF020 */
    fe_resource_release_owner(1);
    CHECK(fe_resource_owned_count(1) == 0);
    CHECK(fe_resource_pool_count() == 1);
    u32 found = 0;
    for (u32 i = 0; i < FE_RES_MAX; i++) {
        if (g_pool[i].used && g_pool[i].kind == FE_RES_IOPORT &&
            g_pool[i].base == 0xF004 && g_pool[i].len == 0x1C) {
            found = 1;
        }
    }
    CHECK(found == 1);

    /* 再归还任务 2：左侧 0xF000..0xF004 接上，池子恢复成完整的一段 */
    fe_resource_release_owner(2);
    CHECK(fe_resource_pool_count() == 1);
    found = 0;
    for (u32 i = 0; i < FE_RES_MAX; i++) {
        if (g_pool[i].used && g_pool[i].kind == FE_RES_IOPORT &&
            g_pool[i].base == 0xF000 && g_pool[i].len == 0x20) {
            found = 1;
        }
    }
    CHECK(found == 1);

    /* 端口范围检查：超过 0xFFFF 的区间不能入池 */
    CHECK(fe_resource_pool_add(FE_RES_IOPORT, 0xFFF0, 32) == FE_ERR_RANGE);

    /* ---- 共享认领：PS/2 那种「端口共享、中断分开」的真实情形 ----
     * 两个服务各自认领同一段端口，且都必须成功——这正是「允许键盘和鼠标
     * 分别认领自己的设备」在硬件寄存器分不开时的落地方式。 */
    fe_resource_init();
    CHECK(fe_ok(fe_resource_pool_add_shared(FE_RES_IOPORT, 0x60, 5)));
    CHECK(fe_resource_is_shared(FE_RES_IOPORT, 0x60, 5));
    CHECK(fe_ok(fe_resource_claim(FE_RES_IOPORT, 0x60, 5, 10)));        /* 键盘服务 */
    CHECK(fe_ok(fe_resource_claim(FE_RES_IOPORT, 0x60, 5, 11)));        /* 鼠标服务 */
    /* 自己重复认领要挡住，否则「认领」就没有记账意义了 */
    CHECK(fe_resource_claim(FE_RES_IOPORT, 0x60, 5, 10) == FE_ERR_EXIST);
    /* 共享区间留在池子里，所以条目数不减 */
    CHECK(fe_resource_pool_count() == 1);

    /* 控制器锁：串行化「读状态 → 写命令 → 写数据」这一串访问 */
    CHECK(fe_ok(fe_resource_lock(FE_RES_IOPORT, 0x60, 5, 10)));
    CHECK(fe_resource_lock(FE_RES_IOPORT, 0x60, 5, 10) == FE_ERR_BUSY); /* 同一线程重复加锁是 bug */
    CHECK(fe_ok(fe_resource_unlock(FE_RES_IOPORT, 0x60, 5, 10)));
    CHECK(fe_resource_unlock(FE_RES_IOPORT, 0x60, 5, 10) == FE_ERR_NOENT);
    /* 独占区间上没有锁这回事 */
    CHECK(fe_resource_lock(FE_RES_IOPORT, 0xF000, 4, 10) == FE_ERR_NOTSUP);

    /* 归还共享区间：只是把持有者从列表里划掉，条目本身留在池中 */
    fe_resource_release_owner(10);
    fe_resource_release_owner(11);
    CHECK(fe_resource_pool_count() == 1);
    found = 0;
    for (u32 i = 0; i < FE_RES_MAX; i++) {
        if (g_pool[i].used && g_pool[i].base == 0x60 && g_pool[i].sharer_count == 0) {
            found = 1;
        }
    }
    CHECK(found == 1);

    /* 清干净，别把自检的数据留给真实驱动 */
    fe_resource_init();
    CHECK(fe_resource_pool_count() == 0);

    /* ---- D 组：先证缺陷存在（docs/13-tasks-and-kill.md §6.7）----
     * ★ 为什么落在这里 ★ 资源池只有 fe_resource_init() 一种复位手段，
     * 而"造共享条目 → 最后复位"这套纪律本来就在本文件里；main.c 在这之后
     * 会重新 bootstrap 一次真实池子，所以这里造的假条目不会留给真实驱动。
     * ★ 这一版只有自检、没有修复 ⇒ 它们应当如实变红。 */
    fe_kprintf("        D2①：取消点缺失——阻塞在共享区间控制器锁上的线程\n");
    fail += selftest_d2_reslock();
    fe_kprintf("        D4：持锁线程死后锁不放（lock_holder 悬空）\n");
    fail += selftest_d4_forget();

    /* 两个 D 组都造了自己的假条目：再复位一次，保持"干净状态结束"。 */
    fe_resource_init();
    CHECK(fe_resource_pool_count() == 0);
    return fail;
}

/* ------------------------------------------------------------------ */
/* 自检：设备管理器（申报硬件 / 授权分发）                              */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* 自检：PCI 配置空间（机制 #1 的端口对）                              */
/* ------------------------------------------------------------------ */

/* ★ 为什么这一段"读硬件"的自检有意义 ★
 *
 * 端口入池本身没什么可测的（加一个区间而已）。真正要证明的是两件事：
 *
 *   1. **认领之后端口真的能用** —— `fe_ioport_request` 的两步走
 *      （资源池认领 + 写 TSS 位图）缺任何一步，用户态的 in/out 都会被
 *      CPU 挡成 #GP。"池子里有"与"CPU 放行"是两件独立的事，
 *      所以这里在**内核态**直接读一次 0xCFC，确认返回的不是全 1。
 *      （全 1 = 没有设备应答，与"端口不可用"是两种不同的失败。）
 *
 *   2. **它必须是独占的** —— 机制 #1 是"先写地址口、再读数据口"两步，
 *      两个持有者交错就会互相改到对方的窗口。所以第二个认领必须被拒。
 *
 * ★ 一台机器上必然存在的设备 ★
 * 0 号总线 0 号设备 0 号功能是**主机桥**，PCI 规范要求它存在。
 * 所以"读到非 0xFFFF 的厂商 ID"是一个跨环境都成立的断言——
 * 比"读到某个具体 ID"更稳（不同虚拟机不同）。
 * 顺带列出总线上前几个设备：那是"枚举"这件事本身的证据，
 * 而且 QEMU 与 VBox 的设备列表不同，正好当环境指纹。 */
u32 fe_selftest_pci(void)
{
    u32 fail = 0;
    const u64 cfg_base = 0xCF8;
    const u64 cfg_len = 8;

    struct fe_task *probe = fe_task_create_kernel("pci-probe");
    if (!probe) {
        fe_kprintf("        探针任务创建失败\n");
        return 1;
    }

    /* 正向：认领成功 */
    CHECK(fe_ok(fe_resource_claim(FE_RES_IOPORT, cfg_base, cfg_len, probe->id)));
    /* 反向 1：独占资源不能被第二个持有者认领 */
    {
        struct fe_task *thief = fe_task_create_kernel("pci-thief");
        if (thief) {
            fe_status_t s = fe_resource_claim(FE_RES_IOPORT, cfg_base, cfg_len,
                                              thief->id);
            if (s != FE_ERR_BUSY) {
                fe_kprintf("        第二个持有者竟然认领成功（返回 %s）\n",
                           fe_status_name(s));
                fail++;
            } else {
                fe_kprintf("        PCI 配置端口是独占的（第二个认领被拒: %s）\n",
                           fe_status_name(s));
            }
            fe_object_unref(&thief->hdr);
        }
    }

    /* 正向：真的读一次配置空间。
     *
     * 机制 #1 的地址格式（PCI 规范 3.2.2.3.2）：
     *   bit31 使能 | bit23..16 总线 | bit15..11 设备 | bit10..8 功能 | bit7..2 寄存器
     * 这里在**内核态**直接用 inl/outl：端口权限对内核态不受 TSS 位图限制，
     * 所以这一段验证的是"端口确实通、设备确实答"，而不是权限本身
     * （权限由 drvdeny 那个用户态反例验证）。 */
    {
        fe_outl((u16)cfg_base, 0x80000000u);
        u32 id = fe_inl((u16)(cfg_base + 4));
        u16 vendor = (u16)(id & 0xFFFF);
        u16 device = (u16)(id >> 16);
        if (vendor == 0xFFFF || vendor == 0) {
            fe_kprintf("        00:00.0 的厂商 ID = %#x —— 主机桥应当存在"
                       "（端口不通，或设备没应答）\n", vendor);
            fail++;
        } else {
            fe_kprintf("        PCI 配置空间可读：00:00.0 厂商 %#06x 设备 %#06x\n",
                       vendor, device);
        }

        u32 found = 0;
        for (u32 dev = 0; dev < 8; dev++) {
            u32 addr = 0x80000000u | (dev << 11);
            fe_outl((u16)cfg_base, addr);
            u32 d = fe_inl((u16)(cfg_base + 4));
            u16 v = (u16)(d & 0xFFFF);
            if (v == 0xFFFF || v == 0) {
                continue;
            }
            fe_outl((u16)cfg_base, addr + 8);       /* 类代码在偏移 8 */
            u32 cls = fe_inl((u16)(cfg_base + 4));
            fe_kprintf("          00:%02u.0 厂商 %#06x 设备 %#06x 类 %#06x\n",
                       dev, v, (u16)(d >> 16), (u16)(cls >> 16));
            found++;
        }
        if (found == 0) {
            fe_kprintf("        总线上一个设备都没枚举到\n");
            fail++;
        }
    }

    /* 收尾：归还之后应能被**另一个任务**认领——独占是暂时的，不是永久占用。
     * ★ 这里必须用一个真实任务 id ★ `owner_id == 0` 是共享条目的保留值
     * （共享条目的 owner 恒为 0），所以 0 不是合法的独占持有者。
     * 第一版传了 0、被 claim 拒掉——断言失败而机制是对的。 */
    fe_resource_release_owner(probe->id);
    {
        u32 before = fail;
        struct fe_task *next_owner = fe_task_create_kernel("pci-next");
        if (!next_owner) {
            fe_kprintf("        第二个认领者的任务创建失败\n");
            fail++;
        } else {
            CHECK(fe_ok(fe_resource_claim(FE_RES_IOPORT, cfg_base, cfg_len,
                                          next_owner->id)));
            fe_resource_release_owner(next_owner->id);
            fe_object_unref(&next_owner->hdr);
            fe_kprintf("        释放后可被另一个任务认领"
                       "（独占是暂时的，不是永久占用）: %s\n",
                       (fail == before) ? "OK" : "失败");
        }
    }
    fe_object_unref(&probe->hdr);
    return fail;
}

/* ★ 这一组的关键是**反向对照** ★
 *
 * "设备管理器能申报硬件"这件事很容易做到——把检查删掉也能通过。
 * 真正要证明的是另外几条**拒绝**：
 *   1. 不是设备管理器的任务不能申报（否则资源池就不存在了）；
 *   2. 设备管理器也**不能**把可用内存当 MMIO 申报（否则它能把内核的
 *      内存送给驱动，内存保护就交给用户态了）；
 *   3. 不是资源的主人不能把它"授予"给别人（否则任何任务都能提权）；
 *   4. 身份随任务退出失效（否则 id 复用就是提权）。
 * 每一条都对应一类真实的越权路径，而不是"多测一个分支"。 */
u32 fe_selftest_devmgr(void)
{
    u32 fail = 0;
    fe_resource_init();

    struct fe_task *dm = fe_task_create_kernel("dm-probe");
    struct fe_task *plain = fe_task_create_kernel("plain-probe");
    if (!dm || !plain) {
        fe_kprintf("        探针任务创建失败\n");
        return 1;
    }
    fe_resource_set_devmgr(dm->id);
    CHECK(fe_resource_is_devmgr(dm->id));
    CHECK(!fe_resource_is_devmgr(plain->id));
    CHECK(!fe_resource_is_devmgr(0));       /* id 0 永远不是 */

    /* ---- 正向：申报一段 MMIO，驱动能认领到 ---- */
    const u64 mmio_base = 0xFEBF0000ull;    /* 典型的设备 MMIO 窗口 */
    const u64 mmio_len = 0x1000;
    fe_status_t s = fe_resource_pool_add(FE_RES_MMIO, mmio_base, mmio_len);
    if (fe_failed(s)) {
        fe_kprintf("        申报 MMIO 失败: %s\n", fe_status_name(s));
        fail++;
    } else if (fe_failed(fe_resource_claim(FE_RES_MMIO, mmio_base, mmio_len,
                                           plain->id))) {
        fe_kprintf("        申报之后驱动认领不到\n");
        fail++;
    } else {
        fe_kprintf("        申报 MMIO %#llx+%#llx → 驱动认领成功\n",
                   (unsigned long long)mmio_base, (unsigned long long)mmio_len);
    }

    /* ---- 反向 1：**可用内存**不能当 MMIO 申报 ----
     * 整组里最重要的一条：放过去的话，设备管理器就能把内核的内存当成
     * 设备寄存器交给驱动，而驱动拿到的是**可写映射**——内存保护于是变成
     * "取决于设备管理器守不守规矩"。 */
    {
        phys_addr_t usable = fe_pmm_alloc_frame();
        if (usable == 0) {
            fe_kprintf("        取一帧可用内存失败\n");
            fail++;
        } else {
            fe_status_t r = fe_resource_pool_add(FE_RES_MMIO, usable, FE_FRAME_SIZE);
            if (fe_ok(r)) {
                fe_kprintf("        **可用内存 %#llx 竟然能被申报为 MMIO**\n",
                           (unsigned long long)usable);
                fail++;
            } else {
                fe_kprintf("        可用内存不能申报为 MMIO（%s）\n",
                           fe_status_name(r));
            }
            fe_pmm_free_frame(usable);
        }
    }
    /* ---- 反向 2：端口 / IRQ / MMIO 的范围与对齐校验 ---- */
    {
        u32 before = fail;
        CHECK(fe_failed(fe_resource_pool_add(FE_RES_IOPORT, 0x10000, 1)));
        CHECK(fe_failed(fe_resource_pool_add(FE_RES_IOPORT, 0xFFF8, 16)));
        CHECK(fe_failed(fe_resource_pool_add(FE_RES_IRQ, 256, 1)));
        /* MMIO **未页对齐不是错误**：机制会把它规范化到页边界
         * （真实的 PCI BAR 常常不是页对齐的，拒绝它等于把麻烦推给每个驱动）。
         * 所以这里验的是"规范化真的发生了"：入池后按**对齐后的**区间能认领。 */
        CHECK(fe_ok(fe_resource_pool_add(FE_RES_MMIO, 0xFEC00000, 0x1800)));
        CHECK(fe_ok(fe_resource_claim(FE_RES_MMIO, 0xFEC00000, 0x2000, plain->id)));
        fe_kprintf("        范围校验（端口越界 / IRQ 越界）+ MMIO 自动页对齐: %s\n",
                   (fail == before) ? "OK" : "失败");
    }

    /* ---- 正向 2：把资源授予另一个任务，所有权**唯一**（不是复制） ---- */
    {
        fe_status_t g = fe_resource_grant(FE_RES_MMIO, mmio_base, mmio_len,
                                          plain->id, dm->id);
        if (fe_failed(g)) {
            fe_kprintf("        授予失败: %s\n", fe_status_name(g));
            fail++;
        } else {
            /* 转移之后，新持有者不该再"认领"到同一段（它已经持有了），
             * 而旧持有者不该再持有 —— 这两条一起证明"所有权唯一"。 */
            fe_status_t again = fe_resource_claim(FE_RES_MMIO, mmio_base, mmio_len,
                                                  dm->id);
            if (fe_ok(again)) {
                fe_kprintf("        转移之后同一段还能被再次认领（所有权不唯一）\n");
                fail++;
            } else {
                fe_kprintf("        资源已从 %llu 转给 %llu（所有权唯一）\n",
                           (unsigned long long)plain->id, (unsigned long long)dm->id);
            }
        }
    }
    /* ---- 反向 3：不是主人就不能授予 ---- */
    {
        u32 before = fail;
        /* 一段从没入池的区间：谁都不是主人 */
        CHECK(fe_failed(fe_resource_grant(FE_RES_IOPORT, 0x300, 8,
                                          plain->id, dm->id)));
        /* 已转走的区间：旧主人在授予时仍然是 owner（资源在 dm 名下），
         * 但这里 from 给的是 plain —— 不该成功 */
        CHECK(fe_failed(fe_resource_grant(FE_RES_MMIO, mmio_base, mmio_len,
                                          plain->id, plain->id)));
        fe_kprintf("        非主人不能授予（凭空转移被拒）: %s\n",
                   (fail == before) ? "OK" : "失败");
    }

    /* ---- 反向 4：身份随任务退出失效 ---- */
    fe_resource_clear_devmgr(dm->id);
    if (fe_resource_is_devmgr(dm->id)) {
        fe_kprintf("        清除之后身份还在\n");
        fail++;
    } else {
        fe_kprintf("        设备管理器退出后身份失效（id 复用不会继承能力）: OK\n");
    }

    /* ---- 正向 3 / 反向 5：**按任务 id 授予**（0x88 的机制侧） ----
     *
     * ★ 为什么要单独测这一条 ★
     * "按 id 授予"比"按句柄授予"弱：id 是可猜的。所以它的安全性完全
     * 落在两道检查上——调用者是设备管理器、且**目标任务现在活着**。
     * 这两条不测，"弱的那条路"就没人看着了。
     *
     * 机制函数 fe_resource_grant 只认 owner id（它不知道句柄是什么），
     * 所以这里能直接测；syscall 入口那一层另外做存活检查，见 syscall.c。 */
    {
        u32 before = fail;
        fe_resource_set_devmgr(dm->id);
        const u64 port_base = 0xF200ull;
        const u64 port_len = 8;

        /* 正向：DM 自己认领一段，再按 id 转给另一个任务 */
        CHECK(fe_ok(fe_resource_pool_add(FE_RES_IOPORT, port_base, port_len)));
        CHECK(fe_ok(fe_resource_claim(FE_RES_IOPORT, port_base, port_len, dm->id)));
        if (fe_failed(fe_resource_grant(FE_RES_IOPORT, port_base, port_len,
                                        dm->id, plain->id))) {
            fe_kprintf("        按 id 授予失败\n");
            fail++;
        }
        /* 反向：转走之后 DM 不再是主人，再转一次必须失败 */
        CHECK(fe_failed(fe_resource_grant(FE_RES_IOPORT, port_base, port_len,
                                          dm->id, plain->id)));
        /* 反向：目标 id 不存在时不能"先记下来"——
         * 机制层不知道任务存不存在，所以"不存在"必须被入口层挡住；
         * 机制层能验的是"不是主人就转不走"，上面那条已经验了。
         * 这里补一条：把自己名下没有的区间转给任意 id 都要失败。 */
        CHECK(fe_failed(fe_resource_grant(FE_RES_IOPORT, 0x330, 8,
                                          dm->id, plain->id)));
        fe_kprintf("        按任务 id 授予（所有权转移 + 二次转移被拒）: %s\n",
                   (fail == before) ? "OK" : "失败");
    }

    fe_resource_release_owner(dm->id);
    fe_resource_release_owner(plain->id);
    fe_object_unref(&dm->hdr);
    fe_object_unref(&plain->hdr);

    /* 清干净，别把自检的数据留给真实驱动 */
    fe_resource_init();
    CHECK(fe_resource_pool_count() == 0);
    return fail;
}
