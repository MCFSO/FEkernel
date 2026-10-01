/* SPDX-License-Identifier: 0BSD */
/* VMA：用户地址空间的**区间表**（按需分页的基础）。
 *
 * ★ 为什么必须有它 ★
 *
 * 在它之前，"这块地址该不该有映射"这个问题没有答案：页表里有就是有、
 * 没有就是没有。于是 `#PF` 只有一条路——杀线程。后果是三件事做不了：
 *   1. **栈不能自动增长**（栈用超 = 死）；
 *   2. **大 BSS 必须一次映射完**（声明 64 MiB 就真的占 64 MiB 物理内存）；
 *   3. **任何 mmap 语义都不成立**（匿名映射、文件映射、属性变更都无从表达）。
 *
 * VMA 回答的正是那个问题："这个地址**被允许**存在映射吗？按什么权限？"
 * 有了它，`#PF` 才有第二种处理方式：落在某个区间内且权限允许 → 补一页，继续跑。
 *
 * ★ 它与页表的分工（这条不能混）★
 *   VMA    = **意图**：这段地址属于什么（代码 / 数据 / 栈 / 匿名映射），
 *            允许什么权限，能不能增长；
 *   页表   = **事实**：这一页现在有没有物理帧。
 * 一个区间只有一部分页有映射，是**正常状态**（按需分页的全部意义就在这），
 * 而不是"不一致"。反过来说：页表里有映射而 VMA 里没有对应的区间，
 * 那是**非法状态**——它意味着某处绕过了表直接改页表，而那种映射
 * 一旦要撤销（`MEM_UNMAP`、任务销毁）就会漏掉。
 */
#ifndef FE_MM_VMA_H
#define FE_MM_VMA_H

#include <fe/types.h>
#include <fe/status.h>
#include <fe/mm.h>

struct fe_address_space;
struct fe_task;

/* 区间标志（与页表权限位**不复用**：VMA 上还要表达"能不能增长"这种事，
 * 而那是意图，不是任何一页的属性）。 */
#define FE_VMA_READ      (1u << 0)
#define FE_VMA_WRITE     (1u << 1)
#define FE_VMA_EXEC      (1u << 2)
/* 栈式增长：缺页地址在区间下方紧邻处时，把区间**下界**向下扩一页。
 * 这是栈的语义（向下长），与"匿名映射按需给页"是两件事——
 * 后者只需要这个区间存在，前者还需要动态改边界。 */
#define FE_VMA_GROWSDOWN (1u << 3)
/* 匿名映射：缺页时分配清零的新页。
 * 没有它（例如文件映射）缺页该从别处取数据——本轮不做，所以标志必须显式，
 * 免得将来把"匿名"当成默认行为。 */
#define FE_VMA_ANON      (1u << 4)

struct fe_vma {
    struct fe_vma *next;
    virt_addr_t base;       /* 页对齐 */
    virt_addr_t end;        /* 页对齐，右开区间 */
    u32 flags;
    u32 _pad;
    /* 按需分页时该映射的物理源。本轮只有两种：
     *   frames == NULL  → 匿名：缺页时新分配一帧并清零；
     *   frames != NULL  → 来自内存对象：按页索引取帧（预映射用）。
     * 文件映射需要"缺页时去读文件"，那是另一个里程碑（要能睡眠）。 */
    phys_addr_t *frames;
};

/* 一个任务的区间表。定长数组而不是链表：
 * 真实程序（我们见过的）区间数在个位数；定长让插入/查找没有分配失败路径，
 * 也让"表满了"这件事有一个明确、可测的边界。
 * 上限取 32：够用，且整个结构 32*40 = 1.3 KiB，挂在任务上不心疼。 */
#define FE_VMA_MAX 32

struct fe_vma_table {
    struct fe_vma items[FE_VMA_MAX];
    u32 count;
};

void fe_vma_table_init(struct fe_vma_table *t);

/* 新增一个区间。要求页对齐、base < end、不与已有区间重叠。
 * 重叠一律**拒绝**（FE_ERR_EXIST）而不是合并或覆盖：
 * 两个区间覆盖同一段地址时，"这段地址该按谁的权限"没有正确答案，
 * 而放过去会让权限检查依赖插入顺序——那是最难查的一类不一致。 */
fe_status_t fe_vma_add(struct fe_vma_table *t, virt_addr_t base, virt_addr_t end,
                       u32 flags, phys_addr_t *frames);

/* 查找覆盖 addr 的区间；没有返回 NULL */
struct fe_vma *fe_vma_find(struct fe_vma_table *t, virt_addr_t addr);

/* 查找覆盖 addr 的区间并给出它的**下标**（没找到返回 -1）。
 *
 * ★ 为什么要有下标版：指针会被插入失效 ★
 * 区间表是定长数组，`fe_vma_add`/`fe_vma_remove_range` 会 memmove 元素——
 * 那之后任何先前拿到的 `struct fe_vma *` 都指向**别的区间**（或数组外）。
 * 这个坑我实测踩到过：测试里先拿到栈区间的指针、再在它下面插入一个邻居、
 * 然后拿着旧指针调 `fe_vma_grow_down` —— 读到的是邻居的 flags，
 * 于是"撞上邻居"该报 NOSPC 却报成了 NOTSUP（因为邻居没标 GROWSDOWN）。
 *
 * 所以跨"可能插入/删除"的调用之间，**用下标而不是指针**：
 * 下标在 memmove 之后仍然指向同一个逻辑区间。 */
int fe_vma_find_index(struct fe_vma_table *t, virt_addr_t addr);
struct fe_vma *fe_vma_at(struct fe_vma_table *t, int index);

/* 删除与 [base,end) 相交的全部区间（含部分相交时的裁剪）。
 * 返回被删除/裁剪的区间数。 */
u32 fe_vma_remove_range(struct fe_vma_table *t, virt_addr_t base, virt_addr_t end);

/* 把区间下界向下扩一页（仅 FE_VMA_GROWSDOWN 的区间允许）。
 * 返回 0 成功，负值为错误。 */
fe_status_t fe_vma_grow_down(struct fe_vma_table *t, struct fe_vma *v,
                             virt_addr_t new_base);

/* ★ 改一段页面的权限：VMA 的 flags **与**已映射页的 PTE 权限，并刷 TLB ★
 *
 * 这是 `mprotect` 的机制层。两条缺一不可（各自的坏结局见 vma.c 的说明）：
 *   - 只改 PTE ⇒ 下次缺页重新补页时权限退回旧的（`demand_map_page` 按 flags 建）；
 *   - 只改 VMA ⇒ 已经映射的页保持旧权限，用户态照样写得进去。
 *
 * 三条安全边界（每条都有自检）：
 *   1. **范围必须完全落在同一个 VMA 内**：跨区间返回 `FE_ERR_INVAL`、
 *      端点不在任何区间返回 `FE_ERR_NOENT`；不扩展、不拆分；
 *   2. **W^X**：同时给出 W 与 X 返回 `FE_ERR_INVAL`；
 *   3. **写保护清单**：那份清单的区间是**磁盘 LBA**，与虚拟地址没有交集，
 *      这里没有可判的交集——详见 vma.c 里那段如实说明。真正挡住
 *      "改到别人的内存"的是边界 1（只能改自己地址空间里、自己 VMA 内的页）。
 *
 * `new_vma_flags` 只认 `FE_VMA_READ`/`FE_VMA_WRITE`/`FE_VMA_EXEC` 三位，
 * 其余位传进来返回 `FE_ERR_INVAL`（GROWSDOWN/ANON 描述"这段内存是什么"，
 * 不是"允许怎么访问"，不许从这里改）。 */
fe_status_t fe_vma_protect_range(struct fe_vma_table *vt, struct fe_address_space *as,
                                 virt_addr_t base, virt_addr_t end, u32 new_vma_flags);

/* 诊断：把区间表打到串口（自检与排错用） */
void fe_vma_dump(const struct fe_vma_table *t, const char *who);

/* 自检：区间属性变更（mprotect 的机制层 + 三条安全边界）。返回失败项数。 */
u32 fe_selftest_protect_range(void);

/* 自检：区间表的插入/重叠拒绝/裁剪删除/增长。返回失败项数。 */
u32 fe_selftest_vma(void);

#endif /* FE_MM_VMA_H */
