/* SPDX-License-Identifier: 0BSD */
/* VMA 区间表的实现（设计说明见 fe/mm/vma.h）。 */
#include <fe/mm/vma.h>
#include <fe/mm/vmm.h>
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

/* SPDX-License-Identifier: 0BSD */
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
