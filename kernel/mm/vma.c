/* SPDX-License-Identifier: 0BSD */
/* VMA 区间表的实现（设计说明见 fe/mm/vma.h）。 */
#include <fe/mm/vma.h>
#include <fe/mm/vmm.h>
#include <fe/mm/kheap.h>    /* 自检自己造地址空间/区间表要用到 */
#include <fe/task.h>        /* fe_task_create_kernel（自检的一次性任务） */
#include <fe/object.h>      /* fe_object_unref */
#include <fe/user.h>        /* FE_PTE_* 的 USER 位与用户地址空间布局 */
#include <fe/kprintf.h>
#include <fe/string.h>

void fe_vma_table_init(struct fe_vma_table *t)
{
    if (!t) {
        return;
    }
    memset(t->items, 0, sizeof(t->items));
    t->count = 0;
}

struct fe_vma *fe_vma_find(struct fe_vma_table *t, virt_addr_t addr)
{
    if (!t) {
        return NULL;
    }
    for (u32 i = 0; i < t->count; i++) {
        struct fe_vma *v = &t->items[i];
        if (addr >= v->base && addr < v->end) {
            return v;
        }
    }
    return NULL;
}

/* 下标版：指针会被 memmove 失效，跨插入/删除的调用要用下标（见 vma.h 的说明）。 */
int fe_vma_find_index(struct fe_vma_table *t, virt_addr_t addr)
{
    if (!t) {
        return -1;
    }
    for (u32 i = 0; i < t->count; i++) {
        if (addr >= t->items[i].base && addr < t->items[i].end) {
            return (int)i;
        }
    }
    return -1;
}

struct fe_vma *fe_vma_at(struct fe_vma_table *t, int index)
{
    if (!t || index < 0 || (u32)index >= t->count) {
        return NULL;
    }
    return &t->items[index];
}

/* 与 [base,end) 是否有交叠 */
static bool overlaps(const struct fe_vma *v, virt_addr_t base, virt_addr_t end)
{
    return (base < v->end) && (end > v->base);
}

fe_status_t fe_vma_add(struct fe_vma_table *t, virt_addr_t base, virt_addr_t end,
                       u32 flags, phys_addr_t *frames)
{
    if (!t) {
        return FE_ERR_INVAL;
    }
    if (!fe_is_aligned(base, FE_FRAME_SIZE) || !fe_is_aligned(end, FE_FRAME_SIZE) ||
        base >= end) {
        return FE_ERR_INVAL;
    }
    if (t->count >= FE_VMA_MAX) {
        return FE_ERR_NOSPC;
    }
    for (u32 i = 0; i < t->count; i++) {
        if (overlaps(&t->items[i], base, end)) {
            /* ★ 重叠必须拒绝 ★ 见 vma.h 的说明：两个区间覆盖同一段地址时
             * "该按谁的权限"没有正确答案，而放过去会让权限检查依赖插入顺序。 */
            return FE_ERR_EXIST;
        }
    }
    /* 按地址升序插入：查找可以提前退出，dump 也可读。
     * 定长数组上的插入是 memmove，代价可以忽略（n ≤ 32）。 */
    u32 at = t->count;
    for (u32 i = 0; i < t->count; i++) {
        if (base < t->items[i].base) {
            at = i;
            break;
        }
    }
    if (at < t->count) {
        memmove(&t->items[at + 1], &t->items[at],
                (t->count - at) * sizeof(t->items[0]));
    }
    struct fe_vma *v = &t->items[at];
    v->base = base;
    v->end = end;
    v->flags = flags;
    v->frames = frames;
    v->_pad = 0;
    t->count++;
    return FE_OK;
}

u32 fe_vma_remove_range(struct fe_vma_table *t, virt_addr_t base, virt_addr_t end)
{
    if (!t || base >= end) {
        return 0;
    }
    u32 touched = 0;
    for (u32 i = 0; i < t->count; ) {
        struct fe_vma *v = &t->items[i];
        if (!overlaps(v, base, end)) {
            i++;
            continue;
        }
        touched++;
        /* ★ 两个"是否保留两端"的锚点必须是 v 自己，不能是移除范围的端点 ★
         *
         * 第一版写的是 `base > v->base`（切头）与 `end < v->end`（切尾）。
         * 后者错了：当**移除范围完全落在区间内部**时（base > v->base 且
         * end < v->end），它给出"切头且切尾"→ 走挖洞分支，那是对的；
         * 但当移除范围**恰好从 v->base 开始、在内部结束**时（例如
         * v=[0x102000,0x104000)、移除 [0x103000,0x104000) 其实是
         * ……见下），锚点用错会得到"应当只切头"而实际要保留头部。
         *
         * 正确的判据是：**区间在移除范围左侧有没有剩、右侧有没有剩**：
         *   has_head = v->base < base      （有 [v->base, base) 要保留）
         *   has_tail = v->end  > end       （有 [end, v->end) 要保留）
         * 三者组合正好覆盖四种情况，且不会把"要保留的部分"当成"要删的"。 */
        bool has_head = (v->base < base);
        bool has_tail = (v->end > end);
        if (has_head && has_tail) {
            /* ★ 从中间挖洞：必须先**收前半段**、再插后半段 ★
             *
             * 第一版写的是"先插入后半段，再收前半段"，理由是"否则插入时
             * 前半段还覆盖着整段，会被重叠检查挡住"。实际正好相反：
             * 前半段**此刻确实覆盖着整段**，所以那次插入必然报
             * `FE_ERR_EXIST` —— 于是函数在"收尾"之前就返回了，
             * 洞根本没挖出来。实测症状：`remove_range` 返回 1、
             * 而 `count` 仍是 1（本该 2）。
             *
             * 正确顺序：先把前半段的 end 收到 base（此时数组里没有区间
             * 覆盖 [base,end) 了），再插入后半段；插入失败（表满）就把
             * end 还原——**不留一个被悄悄截短的区间**，
             * 因为区间表是权限判断的依据，边界不清比多一块映射危险。 */
            virt_addr_t old_end = v->end;
            v->end = base;
            if (t->count < FE_VMA_MAX) {
                fe_status_t s = fe_vma_add(t, end, old_end, v->flags, v->frames);
                if (fe_ok(s)) {
                    i++;
                    continue;       /* 两半都在：洞挖成功 */
                }
            }
            /* 表满：还原成整段，让调用者看到"这次裁剪没做成" */
            v->end = old_end;
            i++;
        } else if (has_head) {
            /* 只有左侧有剩：保留 [v->base, base)，砍掉右边那截 */
            v->end = base;
            i++;
        } else if (has_tail) {
            /* 只有右侧有剩：保留 [end, v->end)，砍掉左边那截 */
            v->base = end;
            i++;
        } else {
            /* 整段包含：删掉它（把后面的往前挪） */
            if (i + 1 < t->count) {
                memmove(&t->items[i], &t->items[i + 1],
                        (t->count - i - 1) * sizeof(t->items[0]));
            }
            t->count--;
            /* 不 i++：挪过来的那个还要检查 */
        }
    }
    return touched;
}

fe_status_t fe_vma_grow_down(struct fe_vma_table *t, struct fe_vma *v,
                             virt_addr_t new_base)
{
    if (!t || !v) {
        return FE_ERR_INVAL;
    }
    if (!(v->flags & FE_VMA_GROWSDOWN)) {
        return FE_ERR_NOTSUP;       /* 这个区间不是栈：不许长 */
    }
    if (!fe_is_aligned(new_base, FE_FRAME_SIZE) || new_base >= v->base) {
        return FE_ERR_INVAL;
    }
    /* 新区间不能撞上别的区间。这是**增长的安全边界**：
     * 栈一直长下去就会撞到映像或别的映射——那样两边都会静默写坏。
     * 这里明确拒绝，于是"栈用超"表现为一次可控的失败（#PF 杀线程），
     * 而不是把相邻映射踩坏。 */
    for (u32 i = 0; i < t->count; i++) {
        struct fe_vma *o = &t->items[i];
        if (o == v) {
            continue;
        }
        if (overlaps(o, new_base, v->base)) {
            return FE_ERR_NOSPC;
        }
    }
    v->base = new_base;
    return FE_OK;
}

void fe_vma_dump(const struct fe_vma_table *t, const char *who)
{
    fe_kprintf("        VMA[%s]: %u 个区间\n", who ? who : "?", t ? t->count : 0);
    if (!t) {
        return;
    }
    for (u32 i = 0; i < t->count; i++) {
        const struct fe_vma *v = &t->items[i];
        fe_kprintf("          %#llx-%#llx %s%s%s%s%s%s\n",
                   (unsigned long long)v->base, (unsigned long long)v->end,
                   (v->flags & FE_VMA_READ) ? "r" : "-",
                   (v->flags & FE_VMA_WRITE) ? "w" : "-",
                   (v->flags & FE_VMA_EXEC) ? "x" : "-",
                   (v->flags & FE_VMA_GROWSDOWN) ? " 栈式增长" : "",
                   (v->flags & FE_VMA_ANON) ? " 匿名" : "",
                   v->frames ? " 预映射" : "");
    }
}

/* ------------------------------------------------------------------ */
/* 区间属性变更（mprotect 的 VMA 层）                                  */
/* ------------------------------------------------------------------ */

/* ★ 改一个区间里一段页面的权限：VMA 的 flags **与**已映射页的 PTE 权限 ★
 *
 * 两者缺一不可，而且缺哪一个都有具体的坏结局：
 *   - 只改 PTE 不改 VMA ⇒ 撤销映射之后（或缺页重新补页时）权限**退回旧的**：
 *     `demand_map_page` 是按 `v->flags` 建映射的，于是"mprotect 成只读"
 *     在下一轮缺页后静默失效；
 *   - 只改 VMA 不改 PTE ⇒ 已经映射的页保持旧权限，用户态照样写得进去
 *     （这正是本刀的正面判据）。
 *
 * ★ 边界一：范围必须**完全落在同一个 VMA 内** ★
 * "部分覆盖"一律拒绝（`FE_ERR_INVAL`），不做隐式扩展、也不跨区间拆分：
 *   - 扩展等于内核替调用者猜意图，而猜错的后果是**把一块它没打算改的内存
 *     改成了只读**——那是一次随机的、很难归因的崩溃；
 *   - 跨两个区间更糟：调用者以为改了一段，实际改了两段语义不同的内存。
 * 所以判据写成"起点与终点减一落在**同一个**区间里"，而不是"起点落在某个
 * 区间里"——后者会放过跨区间的范围。
 *
 * ★ 边界二：W^X ★
 * 一次调用同时给出 W 与 X 直接拒绝（`FE_ERR_INVAL`）。这里**不存在**
 * "先建映射后改权限"的绕法：本接口就是用户态唯一能改权限的入口。
 * 注意这条只作用于**本接口**——既有的 `fe_memory_map_user` 今天允许
 * 申请 W|X（那是历史行为，本次不动它，改了会波及既有判据）。
 *
 * ★ 边界三：写保护清单（`docs/04-write-protection.md`）★
 * ★ 如实说明：那份清单的区间是**磁盘 LBA**（`kernel/protect.c` 的
 * `fe_protect_add` 收的是 lba/count），与虚拟地址是**两个不同的地址空间**，
 * 今天不存在"内存地址 ∈ 保护清单"这种关系，所以这里**没有可判的交集**。
 * 本函数不去"顺手"发明一条策略（那会变成第二个会漂的判据）；
 * 真正挡住"绕过写保护"的是上面两条边界加 VMA 的归属：
 * 用户态**只能改自己地址空间里、自己 VMA 内的页**，
 * 而保护清单管的那块盘根本不是任何任务的虚拟内存。
 * 这一条写在这里是为了让下一个读的人不必再查一遍。 */
fe_status_t fe_vma_protect_range(struct fe_vma_table *vt, struct fe_address_space *as,
                                 virt_addr_t base, virt_addr_t end, u32 new_vma_flags)
{
    if (!vt || !as) {
        return FE_ERR_INVAL;
    }
    /* 对齐与空范围：与 fe_mem_map/MEM_UNMAP 同一套判据。 */
    if (end <= base || !fe_is_aligned(base, FE_FRAME_SIZE) ||
        !fe_is_aligned(end, FE_FRAME_SIZE)) {
        return FE_ERR_INVAL;
    }
    /* 只认区间标志里的三位；别的位（GROWSDOWN/ANON）由区间自己说了算，
     * 不许通过 mprotect 改——那两位描述"这段内存**是什么**"，不是"允许怎么访问"。 */
    if (new_vma_flags & ~(FE_VMA_READ | FE_VMA_WRITE | FE_VMA_EXEC)) {
        return FE_ERR_INVAL;
    }
    /* 边界二：W^X。 */
    if ((new_vma_flags & FE_VMA_WRITE) && (new_vma_flags & FE_VMA_EXEC)) {
        return FE_ERR_INVAL;
    }

    /* 边界一：两端必须落在**同一个**区间里。 */
    int i0 = fe_vma_find_index(vt, base);
    int i1 = fe_vma_find_index(vt, end - 1);
    if (i0 < 0 || i1 < 0) {
        return FE_ERR_NOENT;        /* 有端点不在任何区间里 */
    }
    if (i0 != i1) {
        return FE_ERR_INVAL;        /* 跨区间：拒绝，不拆分 */
    }
    /* ★ 用下标取，不用先前拿到的指针 ★
     * `fe_vma_find_index` 不插入/删除，所以这里其实安全；但按 vma.h 的纪律
     * 统一用下标版本——那条纪律的代价（一次实测踩过的越界读）比这点别扭大。 */
    struct fe_vma *v = fe_vma_at(vt, i0);
    if (!v) {
        return FE_ERR_INVAL;
    }

    /* PTE 层：只碰已映射的页，逐页刷 TLB（见 fe_vmm_protect）。 */
    u64 pte_flags = FE_PTE_USER;
    if (new_vma_flags & FE_VMA_WRITE) {
        pte_flags |= FE_PTE_WRITE;
    }
    if (!(new_vma_flags & FE_VMA_EXEC)) {
        pte_flags |= FE_PTE_NX;
    }
    fe_status_t s = fe_vmm_protect(as, base, end - base, pte_flags);
    if (fe_failed(s)) {
        return s;   /* PTE 没改成功就绝不改 VMA：两份状态不许不一致 */
    }

    /* VMA 层：只换权限三位，GROWSDOWN/ANON 与 frames 原样保留。 */
    v->flags = (v->flags & ~(FE_VMA_READ | FE_VMA_WRITE | FE_VMA_EXEC)) |
               (new_vma_flags & (FE_VMA_READ | FE_VMA_WRITE | FE_VMA_EXEC));
    return FE_OK;
}

/* ------------------------------------------------------------------ */
/* 自检                                                                */
/* ------------------------------------------------------------------ */

/* ★ 这一组自检的写法值得说两句 ★
 *
 * 第一版用 `CHECK(...)` 累加失败数、每**组**打一行结论。结果我拿到
 * "裁剪删除: 失败"之后无从下手——因为一组里有七八条断言，
 * 而打印出来的只有总数。于是我只能回去读断言、猜是哪条不成立，
 * 中间有两条其实是**我自己的断言写错了**（那个删除范围同时碰到两个
 * 区间，返回 2 才是对的）。
 *
 * 现在改成：每条性质**独立**判定、失败时把实际值打出来。
 * 自检的读者不应该是"记得当初为什么这么写的人"，而是"几分钟后
 * 拿着日志的另一个人（或另一个我）"。一条说不出**哪个条件不成立**的
 * 自检，价值只有它的一半。
 */
#define CHECK(cond) do { if (!(cond)) { fail++; } } while (0)
#define CHECK_EQ(a, b, what)                                                  \
    do {                                                                      \
        unsigned long long _a = (unsigned long long)(a);                       \
        unsigned long long _b = (unsigned long long)(b);                       \
        if (_a != _b) {                                                        \
            fail++;                                                            \
            fe_kprintf("          [%s] 实际 %#llx，期望 %#llx\n", what, _a, _b); \
        }                                                                      \
    } while (0)

u32 fe_selftest_vma(void)
{
    u32 fail = 0;
    struct fe_vma_table tab;

    /* ---- 1. 插入与查找（右开区间） ---- */
    fe_vma_table_init(&tab);
    CHECK_EQ(fe_vma_add(&tab, 0x100000, 0x102000, FE_VMA_READ | FE_VMA_WRITE, NULL),
             0, "插入应成功");
    CHECK_EQ(tab.count, 1, "区间数");
    CHECK(fe_vma_find(&tab, 0x100000) != NULL);     /* base 在内 */
    CHECK(fe_vma_find(&tab, 0x101FFF) != NULL);     /* 最后一字节在内 */
    CHECK(fe_vma_find(&tab, 0x102000) == NULL);     /* end 本身**不在**内 */
    CHECK(fe_vma_find(&tab, 0x0FFFFF) == NULL);     /* base 之前不在内 */
    {
        u32 before = fail;
        fe_kprintf("        插入与查找（右开区间：base 在内、end 不在内）: %s\n",
                   (fail == before) ? "OK" : "失败");
    }

    /* ---- 2. 重叠必须被拒（三种形式），且被拒的插入不得改变表 ---- */
    {
        u32 before = fail;
        CHECK_EQ(fe_vma_add(&tab, 0x101000, 0x103000, FE_VMA_READ, NULL),
                 FE_ERR_EXIST, "尾部相交应被拒");
        CHECK_EQ(fe_vma_add(&tab, 0x0FF000, 0x101000, FE_VMA_READ, NULL),
                 FE_ERR_EXIST, "首部相交应被拒");
        CHECK_EQ(fe_vma_add(&tab, 0x100000, 0x102000, FE_VMA_READ, NULL),
                 FE_ERR_EXIST, "完全重合应被拒");
        CHECK_EQ(fe_vma_add(&tab, 0x0FF000, 0x103000, FE_VMA_READ, NULL),
                 FE_ERR_EXIST, "完全包含应被拒");
        CHECK_EQ(tab.count, 1, "被拒后区间数应不变");
        fe_kprintf("        重叠被拒（4 种形式，且表长度不变）: %s\n",
                   (fail == before) ? "OK" : "失败");
    }

    /* ---- 3. 相邻不算重叠 ---- */
    {
        u32 before = fail;
        CHECK_EQ(fe_vma_add(&tab, 0x102000, 0x104000, FE_VMA_READ, NULL), 0,
                 "相邻区间应可共存");
        CHECK_EQ(tab.count, 2, "区间数");
        fe_kprintf("        相邻区间（end == 下一个 base）可共存: %s\n",
                   (fail == before) ? "OK" : "失败");
    }

    /* ---- 4. 裁剪删除 ----
     * 表的初始状态：[0x100000,0x102000) 与 [0x102000,0x104000)。 */
    /* 4a. 切尾：删 [0x103000,0x104000) —— 只命中后一个区间，把它的尾巴切掉。
     * 结果：后一个变成 [0x102000,0x103000)。
     * ★ 断言在这里写错过一次 ★ 我原来检查 "0x102000 还在区间内" ——
     * 对，但它同时检查了 "0x101FFF 还在"，而那个地址属于**前**一个区间，
     * 与前一个断言混在一组里，读的人（我自己）分不清在验哪一段。
     * 现在逐条写清是哪个区间的哪一端。 */
    {
        u32 before = fail;
        u32 n = fe_vma_remove_range(&tab, 0x103000, 0x104000);
        CHECK_EQ(n, 1, "切尾应命中 1 个区间");
        /* 逐条打印，避免"只知道不成、不知道哪一条不成" */
        struct {
            u64 addr;
            int want;       /* 1 = 应在区间内，0 = 不应在 */
            const char *what;
        } probes[] = {
            { 0x102000, 1, "被切区间的 base 仍在" },
            { 0x102FFF, 1, "保留部分的最后一字节仍在" },
            { 0x103000, 0, "切掉的部分没了" },
            { 0x103FFF, 0, "切掉的部分末尾也没了" },
            { 0x100000, 1, "邻居区间的 base 不受影响" },
            { 0x101FFF, 1, "邻居区间的末尾不受影响" },
        };
        for (u32 k = 0; k < sizeof(probes) / sizeof(probes[0]); k++) {
            int got = fe_vma_find(&tab, probes[k].addr) ? 1 : 0;
            if (got != probes[k].want) {
                fail++;
                fe_kprintf("          [4a] %#llx %s：实际 %d，期望 %d\n",
                           (unsigned long long)probes[k].addr, probes[k].what,
                           got, probes[k].want);
            }
        }
        CHECK_EQ(tab.count, 2, "切尾不该改变区间个数");
        fe_kprintf("        裁剪-切尾（被切区间的头还在、尾没了；邻居不受影响）: %s\n",
                   (fail == before) ? "OK" : "失败");
    }
    /* 4b. 整段删 */
    {
        u32 before = fail;
        u32 n = fe_vma_remove_range(&tab, 0x100000, 0x102000);
        CHECK_EQ(n, 1, "整段删应命中 1 个区间");
        CHECK_EQ(tab.count, 1, "剩下的区间数");
        CHECK(fe_vma_find(&tab, 0x100000) == NULL);
        fe_kprintf("        裁剪-整段删除: %s\n", (fail == before) ? "OK" : "失败");
    }
    /* 4c. 中间挖洞：区间被切成两半，两半都还在 */
    {
        u32 before = fail;
        fe_vma_table_init(&tab);
        CHECK_EQ(fe_vma_add(&tab, 0x200000, 0x206000,
                            FE_VMA_READ | FE_VMA_WRITE, NULL), 0, "建立大区间");
        u32 n = fe_vma_remove_range(&tab, 0x202000, 0x203000);
        CHECK_EQ(n, 1, "挖洞应命中 1 个区间");
        CHECK_EQ(tab.count, 2, "挖洞后应剩两半");
        CHECK(fe_vma_find(&tab, 0x201000) != NULL);     /* 前半 */
        CHECK(fe_vma_find(&tab, 0x202000) == NULL);     /* 洞里 */
        CHECK(fe_vma_find(&tab, 0x203000) != NULL);     /* 后半 */
        fe_kprintf("        裁剪-中间挖洞（前半与后半都还在）: %s\n",
                   (fail == before) ? "OK" : "失败");
    }
    /* 4d. 一次删掉两半 */
    {
        u32 before = fail;
        u32 n = fe_vma_remove_range(&tab, 0x200000, 0x206000);
        CHECK_EQ(n, 2, "全包含式删除应命中两半");
        CHECK_EQ(tab.count, 0, "删空后区间数");
        fe_kprintf("        裁剪-全包含删除: %s\n", (fail == before) ? "OK" : "失败");
    }

    /* ---- 5. 栈式增长：一条正向 + 三条反向 ----
     * ★ 这一段全部用**下标**而不是指针 ★
     * 因为中间会插入邻居，而定长数组的插入会 memmove 元素——
     * 拿旧指针继续用就是读别人的 flags。这个坑实测踩到过
     * （见 vma.h 里 fe_vma_find_index 的说明）。 */
    {
        u32 before = fail;
        const u64 sb = 0x7FFFFFF00000ull;
        const u64 se = 0x7FFFFFF10000ull;
        fe_vma_table_init(&tab);
        CHECK_EQ(fe_vma_add(&tab, sb, se, FE_VMA_READ | FE_VMA_WRITE |
                            FE_VMA_ANON | FE_VMA_GROWSDOWN, NULL), 0, "建立栈区间");
        int vi = fe_vma_find_index(&tab, sb);
        CHECK(vi >= 0);
        /* 5a. 正向：向下长一页 */
        fe_status_t g1 = fe_vma_grow_down(&tab, fe_vma_at(&tab, vi), sb - 0x1000);
        CHECK_EQ(g1, 0, "向下长一页应成功");
        CHECK(fe_vma_find(&tab, sb - 0x1000) != NULL);
        /* 5b. 反向：向上不是"增长"（栈只往下长） */
        CHECK_EQ(fe_vma_grow_down(&tab, fe_vma_at(&tab, vi), se + 0x1000),
                 FE_ERR_INVAL, "向上应被拒");
        /* 5c. 反向：没标 GROWSDOWN 的区间不许长 */
        {
            struct fe_vma_table t2;
            fe_vma_table_init(&t2);
            CHECK_EQ(fe_vma_add(&t2, 0x300000, 0x301000, FE_VMA_READ, NULL), 0,
                     "建立非栈区间");
            int ni = fe_vma_find_index(&t2, 0x300000);
            CHECK_EQ(fe_vma_grow_down(&t2, fe_vma_at(&t2, ni), 0x2FF000),
                     FE_ERR_NOTSUP, "无 GROWSDOWN 应被拒");
        }
        /* 5d. 反向：撞上邻居必须被拒——这是栈的安全边界。
         * 邻居插在栈**下方**，于是"再往下长"会撞上它。 */
        CHECK_EQ(fe_vma_add(&tab, sb - 0x2000, sb - 0x1000, FE_VMA_READ, NULL), 0,
                 "在栈下方放一个邻居");
        /* 插入可能移动了元素：重新取下标，而不是复用旧指针 */
        vi = fe_vma_find_index(&tab, sb - 0x1000);
        CHECK(vi >= 0);
        fe_status_t g2 = fe_vma_grow_down(&tab, fe_vma_at(&tab, vi), sb - 0x3000);
        CHECK_EQ(g2, FE_ERR_NOSPC, "撞邻居应被拒");
        fe_kprintf("        栈式增长（向下可以 / 向上不行 / 无标记不行 / 撞邻居不行）: %s\n",
                   (fail == before) ? "OK" : "失败");
    }

    /* ---- 6. 表满：明确报错，而不是静默丢弃 ---- */
    {
        u32 before = fail;
        fe_vma_table_init(&tab);
        u32 added = 0;
        for (u32 i = 0; i < FE_VMA_MAX; i++) {
            if (fe_ok(fe_vma_add(&tab, 0x1000000 + i * 0x1000,
                                 0x1001000 + i * 0x1000, FE_VMA_READ, NULL))) {
                added++;
            }
        }
        CHECK_EQ(added, FE_VMA_MAX, "应能装满");
        CHECK_EQ(fe_vma_add(&tab, 0x2000000, 0x2001000, FE_VMA_READ, NULL),
                 FE_ERR_NOSPC, "第 33 个应返回 NOSPC");
        fe_kprintf("        表满（%u 项）返回 NOSPC 而不是静默丢弃: %s\n",
                   (u32)FE_VMA_MAX, (fail == before) ? "OK" : "失败");
    }
    return fail;
}

/* ================================================================== */
/* 自检：区间属性变更（mprotect 的机制层 + 三条安全边界）              */
/* ================================================================== */

/* ★ 这一组为什么落在**真的页表**上，而不是只用一张假 VMA 表 ★
 *
 * 本刀的核心是"VMA 的意图"与"页表的权限"**两处都要改**。如果自检只用一张
 * 内部的 `fe_vma_table` 而不碰真页表，那么"忘了改 PTE"这个缺陷**照样全绿**
 * ——而那正是这一刀最可能出的错（也是用户态能观察到的唯一后果）。
 * 所以这里在自检自己任务的地址空间里造真区间、真映射，然后断言
 * `fe_vmm_query_flags()` 读到的 PTE 权限真的变了。
 *
 * ★ 为什么检查的是 PTE 而不是"用户态写它会不会 #PF" ★
 * 自检跑在内核里（ring 0），无法触发用户态的 #PF。但"用户态写它会不会
 * 失败"这件事在硬件上**完全由那一页的 PTE.W 位决定**，所以断言 PTE 那一位
 * 与断言 #PF 是同一个事实的两个观察面。反过来，"忘了 invlpg"（TLB 陈旧）
 * 这一条**在内核里观察不到**——内核从不通过用户虚拟地址访问这些页
 * （它走 HHDM 别名），所以 TLB 里那条用户 VA 的表项对内核不可见。
 * 这一条只能靠代码正确性 + 实机验证，自检里如实说明，不假装测到了。
 *
 * ★ 地址选择 ★ 用一片"自检专用"高地址（远在映像 0x400000、mmap 区
 * 0x10000000 之上），末尾把整份地址空间销毁，不给后面的自检留状态。
 *
 * ★ 为什么自检**自己造一个任务对象 + 地址空间**，而不是用当前任务 ★
 * 两个理由，第二个是硬性的：
 *   1. 当前执行流是 main 线程，它属于**内核任务**（`fe_task_kernel()`），
 *      而内核任务**没有区间表**（`vmas == NULL`）——它跑在内核页表上；
 *   2. 更要紧的是：验"只读"必须真的改到**用户可访问**的页表项，而 main 跑在
 *      **内核地址空间**里，往里插用户页既不合语义、也会污染内核页表。
 * 所以这里建一次性的任务/地址空间/区间表，验完连同页表一起销毁。
 * 这条做法与 `fe_selftest_vmm_unmap` 完全一致（那一组也是
 * "新地址空间 → 切 CR3 → 读写 → 销毁 → 核对帧数复原"）。 */
u32 fe_selftest_protect_range(void)
{
    u32 fail = 0;

    struct fe_task *own = fe_task_create_kernel("mprot-self");
    if (!own) {
        fe_kprintf("        自检任务创建失败\n");
        return 1;
    }
    own->vmas = (struct fe_vma_table *)fe_kzalloc(sizeof(struct fe_vma_table));
    own->space = fe_vmm_space_create();
    if (!own->vmas || !own->space) {
        fe_kprintf("        自检地址空间/区间表创建失败\n");
        if (own->vmas) { fe_kfree(own->vmas); own->vmas = NULL; }
        if (own->space) { fe_vmm_space_destroy(own->space); own->space = NULL; }
        fe_object_unref(&own->hdr);
        return 1;
    }
    fe_vma_table_init(own->vmas);
    struct fe_vma_table *vt = own->vmas;
    struct fe_address_space *as = own->space;

    /* 自检专用地址：三个相邻页做"目标"，另一段做"没被改过的邻居"。 */
    const virt_addr_t tgt = 0x30100000ull;      /* 目标区间 3 页 */
    const virt_addr_t tgt_end = tgt + 3 * FE_FRAME_SIZE;
    const virt_addr_t nb = 0x30200000ull;       /* 邻居区间 1 页 */
    const virt_addr_t nb_end = nb + FE_FRAME_SIZE;
    const virt_addr_t far_addr = 0x30300000ull; /* 不在任何区间里 */

    /* 目标区间：可写、匿名（ANON 让缺页时能按新 flags 补页） */
    fe_status_t a1 = fe_vma_add(vt, tgt, tgt_end,
                                FE_VMA_READ | FE_VMA_WRITE | FE_VMA_ANON, NULL);
    fe_status_t a2 = fe_vma_add(vt, nb, nb_end,
                                FE_VMA_READ | FE_VMA_WRITE | FE_VMA_ANON, NULL);
    CHECK(fe_ok(a1));
    CHECK(fe_ok(a2));

    /* 真映射三页 + 邻居一页（模拟"已经被碰过、页表里已经有映射"的状态——
     * 只改 VMA 不改 PTE 的缺陷只有在页**已经映射**时才显形）。 */
    int ti = fe_vma_find_index(vt, tgt);
    int ni = fe_vma_find_index(vt, nb);
    if (a1 != FE_OK || a2 != FE_OK || ti < 0 || ni < 0) {
        fe_kprintf("        自检区间建立失败（a1=%d a2=%d ti=%d ni=%d）\n",
                   (int)a1, (int)a2, ti, ni);
        fail++;
    }
    bool mapped = true;
    for (u64 off = 0; off < 3 * FE_FRAME_SIZE; off += FE_FRAME_SIZE) {
        if (fe_failed(fe_vmm_map_alloc(as, tgt + off, FE_FRAME_SIZE,
                                       FE_PTE_USER | FE_PTE_WRITE | FE_PTE_NX))) {
            mapped = false;
        }
    }
    if (fe_failed(fe_vmm_map_alloc(as, nb, FE_FRAME_SIZE,
                                   FE_PTE_USER | FE_PTE_WRITE | FE_PTE_NX))) {
        mapped = false;
    }
    if (!mapped) {
        fe_kprintf("        自检页映射失败（物理帧不足？）\n");
        fail++;
    }

    /* ---------- 正向：由可写改成只读 ---------- */
    {
        u32 before = fail;
        /* 前置事实：改之前三页的 PTE **确实**是可写的（否则下面的"变了"
         * 可能只是"本来就不是可写"，判据会恒真）。 */
        u64 f0 = fe_vmm_query_flags(as, tgt);
        u64 f1 = fe_vmm_query_flags(as, tgt + FE_FRAME_SIZE);
        u64 f2 = fe_vmm_query_flags(as, tgt + 2 * FE_FRAME_SIZE);
        CHECK(f0 & FE_PTE_WRITE);
        CHECK(f1 & FE_PTE_WRITE);
        CHECK(f2 & FE_PTE_WRITE);
        /* 帧地址先记下来：改权限**不许**动它。 */
        phys_addr_t p0 = fe_vmm_translate(as, tgt);

        fe_status_t s = fe_vma_protect_range(vt, as, tgt, tgt_end,
                                             FE_VMA_READ);
        CHECK(fe_ok(s));

        /* ① PTE 层：W 位必须掉了、NX 必须还在（只读 = 不可写不可执行）。
         *    这三条就是"忘了改 PTE"那个缺陷的直接判据。 */
        u64 g0 = fe_vmm_query_flags(as, tgt);
        u64 g1 = fe_vmm_query_flags(as, tgt + FE_FRAME_SIZE);
        u64 g2 = fe_vmm_query_flags(as, tgt + 2 * FE_FRAME_SIZE);
        CHECK(!(g0 & FE_PTE_WRITE));
        CHECK(!(g1 & FE_PTE_WRITE));
        CHECK(!(g2 & FE_PTE_WRITE));
        CHECK(g0 & FE_PTE_NX);
        CHECK(g1 & FE_PTE_NX);
        CHECK(g2 & FE_PTE_NX);
        /* ② 帧不许被换掉（改权限不是重映射）。 */
        CHECK(fe_vmm_translate(as, tgt) == p0);
        /* ③ 其余位不许被覆盖：USER 还在、NOFREE 的**不存在**也保持
         *    （自检映射是 fe_vmm_map_alloc 建的，本就不带 NOFREE——
         *     用"它仍然不带"来证明我们没有乱写这一位）。 */
        CHECK(g0 & FE_PTE_USER);
        CHECK(!(g0 & FE_PTE_NOFREE));
        /* ④ VMA 层：flags 必须跟着变（否则下次缺页会退回可写）。 */
        struct fe_vma *v = fe_vma_at(vt, fe_vma_find_index(vt, tgt));
        CHECK(v && !(v->flags & FE_VMA_WRITE));
        CHECK(v && (v->flags & FE_VMA_READ));
        /* ⑤ VMA 的"这段内存是什么"不许被碰：ANON 必须还在。 */
        CHECK(v && (v->flags & FE_VMA_ANON));
        fe_kprintf("        正向：可写→只读（3 页 PTE 的 W 位都掉了、NX 在、"
                   "帧没换、USER 在、VMA 的 ANON 保留）: %s\n",
                   (fail == before) ? "OK" : "失败");
    }

    /* ---------- 反向①：没被改过的邻居区间仍然可写 ---------- */
    {
        u32 before = fail;
        u64 nf = fe_vmm_query_flags(as, nb);
        CHECK(nf & FE_PTE_WRITE);       /* 邻居的 PTE 可写 */
        struct fe_vma *nv = fe_vma_at(vt, fe_vma_find_index(vt, nb));
        CHECK(nv && (nv->flags & FE_VMA_WRITE));    /* 邻居的 VMA 可写 */
        fe_kprintf("        反向①：没被改过的邻居区间仍然可写（PTE 与 VMA 都是）: %s\n",
                   (fail == before) ? "OK" : "失败");
    }

    /* ---------- 反向②：跨区间 / 部分覆盖 / 区间外 一律拒绝 ---------- */
    {
        u32 before = fail;
        /* (a) 跨两个区间：起点在目标区间、终点减一在邻居区间 */
        CHECK_EQ(fe_vma_protect_range(vt, as, tgt_end - FE_FRAME_SIZE,
                                      nb_end, FE_VMA_READ),
                 FE_ERR_INVAL, "跨两个区间应被拒");
        /* (b) 越出区间尾部（终点减一落在区间外） */
        CHECK_EQ(fe_vma_protect_range(vt, as, tgt,
                                      tgt_end + FE_FRAME_SIZE, FE_VMA_READ),
                 FE_ERR_NOENT, "越过区间尾部应被拒");
        /* (c) 越出区间头部（起点在区间之前） */
        CHECK_EQ(fe_vma_protect_range(vt, as, tgt - FE_FRAME_SIZE,
                                      tgt_end, FE_VMA_READ),
                 FE_ERR_NOENT, "越过区间头部应被拒");
        /* (d) 完全不在任何区间里 */
        CHECK_EQ(fe_vma_protect_range(vt, as, far_addr,
                                      far_addr + FE_FRAME_SIZE, FE_VMA_READ),
                 FE_ERR_NOENT, "不在任何区间里应被拒");
        /* (e) 未对齐 / 空范围：与 fe_mem_map/MEM_UNMAP 同一套判据 */
        CHECK_EQ(fe_vma_protect_range(vt, as, tgt + 1, tgt_end, FE_VMA_READ),
                 FE_ERR_INVAL, "未对齐应被拒");
        CHECK_EQ(fe_vma_protect_range(vt, as, tgt, tgt, FE_VMA_READ),
                 FE_ERR_INVAL, "空范围应被拒");
        /* (f) 不许借这个接口改 GROWSDOWN/ANON 这类"这段内存是什么"的位 */
        CHECK_EQ(fe_vma_protect_range(vt, as, tgt, tgt_end,
                                      FE_VMA_READ | FE_VMA_GROWSDOWN),
                 FE_ERR_INVAL, "不许改 ANON/GROWSDOWN 这类标志");
        /* (g) 上面这么多拒绝之后，目标区间的权限**一点都没变**：
         *     只读仍然是只读（说明拒绝是"什么都没做"，不是"做了一半"）。 */
        struct fe_vma *v = fe_vma_at(vt, fe_vma_find_index(vt, tgt));
        CHECK(v && !(v->flags & FE_VMA_WRITE));
        CHECK(!(fe_vmm_query_flags(as, tgt) & FE_PTE_WRITE));
        fe_kprintf("        反向②：跨区间/越界/区间外/未对齐/空范围/改结构标志"
                   "全部被拒，且拒绝后权限零变化: %s\n",
                   (fail == before) ? "OK" : "失败");
    }

    /* ---------- 反向③：另一个任务只影响它自己 ---------- */
    {
        u32 before = fail;
        struct fe_task *other = fe_task_create_kernel("mprot-other");
        bool ok = false;
        if (other) {
            other->vmas = (struct fe_vma_table *)fe_kzalloc(sizeof(struct fe_vma_table));
            other->space = fe_vmm_space_create();
            ok = (other->vmas && other->space);
        }
        if (!ok) {
            fe_kprintf("        反向③：另一个任务的地址空间创建失败，本项无法判定\n");
            fail++;
            if (other) {
                if (other->vmas) { fe_kfree(other->vmas); other->vmas = NULL; }
                if (other->space) { fe_vmm_space_destroy(other->space); other->space = NULL; }
                fe_object_unref(&other->hdr);
            }
        } else {
            fe_vma_table_init(other->vmas);
            /* 在**它**的表里放一个区间（模拟"别人的区间"）。 */
            bool added = fe_ok(fe_vma_add(other->vmas, tgt, tgt_end,
                                          FE_VMA_READ | FE_VMA_WRITE | FE_VMA_ANON,
                                          NULL));
            CHECK(added);
            /* (a) 用**它自己的表 + 它自己的地址空间**改：合法用法，必须成功
             *     ——证明判据不是"一律拒绝"。 */
            fe_status_t so = fe_vma_protect_range(other->vmas, other->space,
                                                  tgt, tgt_end, FE_VMA_READ);
            CHECK(fe_ok(so));
            struct fe_vma *ov = fe_vma_at(other->vmas,
                                          fe_vma_find_index(other->vmas, tgt));
            CHECK(ov && !(ov->flags & FE_VMA_WRITE));   /* 它自己的 VMA 变了 */
            /* (b) ★ "不许改到别人的区间"这一条的真正判据 ★
             *
             * 从**用户态**根本没有"指定别人的地址空间"这个参数——
             * `MEM_PROTECT` 只认 addr/len/prot，目标恒为 `fe_task_current()`
             * 的地址空间（与 `MEM_UNMAP` 同族）。所以边界不是靠"比对任务 id"
             * 实现的，而是靠**接口形状**：调用者根本没有办法说出别人的地址空间。
             * 这里断言的正是在这个形状下**本自检的地址空间不受影响**：
             * 上面改的是另一个任务的页表，我们自己的 PTE 与 VMA
             * 一个位都不许变。 */
            struct fe_vma *v = fe_vma_at(vt, fe_vma_find_index(vt, tgt));
            CHECK(v && !(v->flags & FE_VMA_WRITE));     /* 我的 VMA 仍是只读 */
            CHECK(!(fe_vmm_query_flags(as, tgt) & FE_PTE_WRITE));
            /* 收尾：把另一个任务的东西还干净。 */
            fe_kfree(other->vmas);
            other->vmas = NULL;
            fe_vmm_space_destroy(other->space);
            other->space = NULL;
            fe_object_unref(&other->hdr);
            fe_kprintf("        反向③：改另一个任务只影响它自己"
                       "（它的 VMA 变了；本地址空间的 PTE/VMA 零变化）: %s\n",
                       (fail == before) ? "OK" : "失败");
        }
    }

    /* ---------- 反向④：W^X —— 一次调用不许同时给 W 与 X ---------- */
    {
        u32 before = fail;
        CHECK_EQ(fe_vma_protect_range(vt, as, tgt, tgt_end,
                                      FE_VMA_READ | FE_VMA_WRITE | FE_VMA_EXEC),
                 FE_ERR_INVAL, "同时 W|X 应被拒");
        CHECK_EQ(fe_vma_protect_range(vt, as, tgt, tgt_end,
                                      FE_VMA_WRITE | FE_VMA_EXEC),
                 FE_ERR_INVAL, "只给 W|X 也应被拒");
        /* 反向的两条**合法**组合必须仍然通过（证明上面拒的是 W^X 本身，
         * 不是"凡带 X 就拒"）：R|X 与 R|W 各来一次。 */
        CHECK(fe_ok(fe_vma_protect_range(vt, as, tgt, tgt_end,
                                         FE_VMA_READ | FE_VMA_EXEC)));
        u64 e0 = fe_vmm_query_flags(as, tgt);
        CHECK(!(e0 & FE_PTE_NX));               /* 可执行 = NX 掉了 */
        CHECK(!(e0 & FE_PTE_WRITE));            /* 且仍然不可写 */
        CHECK(fe_ok(fe_vma_protect_range(vt, as, tgt, tgt_end,
                                         FE_VMA_READ | FE_VMA_WRITE)));
        u64 w0 = fe_vmm_query_flags(as, tgt);
        CHECK(w0 & FE_PTE_WRITE);               /* 可写回来了 */
        CHECK(w0 & FE_PTE_NX);                  /* 且不可执行（NX 加回去） */
        fe_kprintf("        反向④：W|X 两种写法都被拒；R|X 与 R|W 各自合法"
                   "（NX 与 W 位按请求翻转）: %s\n",
                   (fail == before) ? "OK" : "失败");
    }

    /* ---------- 收尾：把这一次性的地址空间整个还掉 ---------- */
    /* 区间表随这一次性对象一起 free；地址空间销毁会把它的页表帧还给 PMM
     * （自检映射的帧由 `fe_vmm_map_alloc` 分配、属于这个地址空间，
     *  所以不需要逐页 unmap_free）。与 fe_selftest_vmm_unmap 同一条纪律：
     * 自检不许漏帧、也不许把状态留给后面的自检。 */
    fe_kfree(own->vmas);
    own->vmas = NULL;
    fe_vmm_space_destroy(own->space);
    own->space = NULL;
    fe_object_unref(&own->hdr);
    return fail;
}
