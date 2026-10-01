/* SPDX-License-Identifier: 0BSD */
#include <fe/mm/vmm.h>
#include <fe/mm/pmm.h>
#include <fe/boot/bootinfo.h>
#include <fe/elf.h>
#include <fe/kprintf.h>
#include <fe/panic.h>
#include <fe/string.h>
#include <fe/io.h>

/* 内核共享的 PML4 起始索引：256 以上属于内核半区 */
#define KERNEL_PML4_FIRST 256

static struct fe_address_space g_kernel_space;
static u64 g_hhdm_offset;

u64 fe_vmm_hhdm_offset(void)
{
    return g_hhdm_offset;
}

/* 物理帧 ↔ 页表内容的虚拟访问（页表帧都在 HHDM 覆盖范围内） */
FE_INLINE u64 *pt_from_phys(phys_addr_t p)
{
    return (u64 *)(uptr)(p + g_hhdm_offset);
}

/* ------------------------------------------------------------------ */
/* 页表遍历                                                            */
/* ------------------------------------------------------------------ */

/* 取 table[idx] 指向的下一级表；create 为真时按需分配中间层。
 * 遇到大页映射（或分配失败）返回 NULL。extra_flags 会加到新建的中间层项上
 * （用户地址空间需要给中间层也加上 USER 位）。 */
static u64 *next_table(u64 *table, u32 idx, bool create, u64 extra_flags)
{
    u64 e = table[idx];
    if (e & FE_PTE_PRESENT) {
        if (e & FE_PTE_HUGE) {
            return NULL;    /* 该层已是大页，无法继续向下 */
        }
        return pt_from_phys(e & FE_PTE_ADDR_MASK);
    }
    if (!create) {
        return NULL;
    }
    phys_addr_t np = fe_pmm_alloc_frame();
    if (np == 0) {
        return NULL;
    }
    u64 *nt = pt_from_phys(np);
    memset(nt, 0, FE_FRAME_SIZE);
    /* 中间层必须可写（更下层的只读权限由叶子项控制） */
    table[idx] = np | FE_PTE_PRESENT | FE_PTE_WRITE | extra_flags;
    return nt;
}

/* 定位到某虚拟地址对应的页表项（PT 层）。create=false 时缺失返回 NULL。 */
static u64 *pte_for(struct fe_address_space *as, virt_addr_t virt, bool create)
{
    u64 user = FE_PTE_USER;     /* 中间层一律允许用户访问，叶子项再收紧 */
    u64 *pdpt = next_table(as->pml4, FE_PML4_INDEX(virt), create, user);
    if (!pdpt) {
        return NULL;
    }
    u64 *pd = next_table(pdpt, FE_PDPT_INDEX(virt), create, user);
    if (!pd) {
        return NULL;
    }
    u64 *pt = next_table(pd, FE_PD_INDEX(virt), create, user);
    if (!pt) {
        return NULL;
    }
    return &pt[FE_PT_INDEX(virt)];
}

/* ------------------------------------------------------------------ */
/* 映射                                                                */
/* ------------------------------------------------------------------ */

fe_status_t fe_vmm_map(struct fe_address_space *as, virt_addr_t virt,
                       phys_addr_t phys, u64 size, u64 flags)
{
    if (size == 0) {
        return FE_ERR_INVAL;
    }
    if (!fe_is_aligned(virt, FE_FRAME_SIZE) || !fe_is_aligned(phys, FE_FRAME_SIZE) ||
        !fe_is_aligned(size, FE_FRAME_SIZE)) {
        return FE_ERR_INVAL;
    }

    for (u64 off = 0; off < size; off += FE_FRAME_SIZE) {
        u64 *pte = pte_for(as, virt + off, true);
        if (!pte) {
            return FE_ERR_NOMEM;
        }
        if (*pte & FE_PTE_PRESENT) {
            return FE_ERR_EXIST;
        }
        *pte = (phys + off) | flags | FE_PTE_PRESENT;
        as->mapped_pages++;
    }
    fe_vmm_flush_tlb_page(virt);
    return FE_OK;
}

fe_status_t fe_vmm_map_alloc(struct fe_address_space *as, virt_addr_t virt,
                             u64 size, u64 flags)
{
    if (size == 0 || !fe_is_aligned(virt, FE_FRAME_SIZE) ||
        !fe_is_aligned(size, FE_FRAME_SIZE)) {
        return FE_ERR_INVAL;
    }
    for (u64 off = 0; off < size; off += FE_FRAME_SIZE) {
        phys_addr_t frame = fe_pmm_alloc_frame();
        if (frame == 0) {
            /* 回滚已映射的部分，避免留下半截映射 */
            if (off > 0) {
                fe_vmm_unmap(as, virt, off);
            }
            return FE_ERR_NOMEM;
        }
        u64 *pte = pte_for(as, virt + off, true);
        if (!pte) {
            fe_pmm_free_frame(frame);
            if (off > 0) {
                fe_vmm_unmap(as, virt, off);
            }
            return FE_ERR_NOMEM;
        }
        *pte = frame | flags | FE_PTE_PRESENT;
        /* 新分配的帧必须清零：内核堆的对象可能被当作指针使用，绝不能含脏数据。
         * 注意必须通过 HHDM 按**物理帧**清零，不能用 virt —— virt 只在 as 这个
         * 地址空间里才有映射，而此刻我们通常正运行在另一个地址空间上。 */
        memset((void *)(uptr)(frame + g_hhdm_offset), 0, FE_FRAME_SIZE);
        as->mapped_pages++;
    }
    fe_vmm_flush_tlb_page(virt);
    return FE_OK;
}

fe_status_t fe_vmm_map_huge_2m(struct fe_address_space *as, virt_addr_t virt,
                               phys_addr_t phys, u64 size, u64 flags)
{
    const u64 huge = FE_HUGE_2M_SIZE;
    if (size == 0 || !fe_is_aligned(virt, huge) || !fe_is_aligned(phys, huge) ||
        !fe_is_aligned(size, huge)) {
        return FE_ERR_INVAL;
    }
    for (u64 off = 0; off < size; off += huge) {
        u64 *pdpt = next_table(as->pml4, FE_PML4_INDEX(virt + off), true, 0);
        if (!pdpt) {
            return FE_ERR_NOMEM;
        }
        u64 *pd = next_table(pdpt, FE_PDPT_INDEX(virt + off), true, 0);
        if (!pd) {
            return FE_ERR_NOMEM;
        }
        u32 idx = FE_PD_INDEX(virt + off);
        if (pd[idx] & FE_PTE_PRESENT) {
            continue;   /* 已映射则跳过（HHDM 区间可能被分多次建立） */
        }
        pd[idx] = (phys + off) | flags | FE_PTE_PRESENT | FE_PTE_HUGE;
        as->mapped_pages += huge / FE_FRAME_SIZE;
    }
    fe_vmm_flush_tlb_page(virt);
    return FE_OK;
}

fe_status_t fe_vmm_unmap(struct fe_address_space *as, virt_addr_t virt, u64 size)
{
    if (size == 0 || !fe_is_aligned(virt, FE_FRAME_SIZE) ||
        !fe_is_aligned(size, FE_FRAME_SIZE)) {
        return FE_ERR_INVAL;
    }
    for (u64 off = 0; off < size; off += FE_FRAME_SIZE) {
        u64 *pte = pte_for(as, virt + off, false);
        if (!pte || !(*pte & FE_PTE_PRESENT)) {
            return FE_ERR_NOENT;
        }
        *pte = 0;
        if (as->mapped_pages > 0) {
            as->mapped_pages--;
        }
    }
    fe_vmm_flush_tlb_page(virt);
    return FE_OK;
}

/* 解除映射并把背后的物理帧归还 PMM（堆与进程地址空间回收时使用） */
fe_status_t fe_vmm_unmap_free(struct fe_address_space *as, virt_addr_t virt, u64 size)
{
    if (size == 0 || !fe_is_aligned(virt, FE_FRAME_SIZE) ||
        !fe_is_aligned(size, FE_FRAME_SIZE)) {
        return FE_ERR_INVAL;
    }
    for (u64 off = 0; off < size; off += FE_FRAME_SIZE) {
        phys_addr_t phys = fe_vmm_translate(as, virt + off);
        u64 *pte = pte_for(as, virt + off, false);
        if (!pte || !(*pte & FE_PTE_PRESENT)) {
            return FE_ERR_NOENT;
        }
        *pte = 0;
        if (as->mapped_pages > 0) {
            as->mapped_pages--;
        }
        if (phys != 0) {
            fe_pmm_free_frame(FE_FRAME_ALIGN_DOWN(phys));
        }
    }
    fe_vmm_flush_tlb_page(virt);
    return FE_OK;
}

/* ------------------------------------------------------------------ */
/* 权限变更（mprotect 的页表层）                                       */
/* ------------------------------------------------------------------ */

/* 改一段**已映射**页面的权限位，帧一个字节都不动。
 *
 * ★ 它是什么、不是什么 ★
 *   - 是：把**已存在**的那些叶子项的 W 与 NX 两位改成 `new_flags` 指定的
 *     样子。帧地址、USER、NOFREE、PCD/GLOBAL 这些位**原样保留**——它们描述的是
 *     "这一页是谁的、从哪来"，不是"允许怎么访问"。把它们一起覆盖掉，
 *     后果比权限改错更糟：NOFREE 被清 ⇒ 任务销毁时把借来的帧还给 PMM
 *     （见 FE_PTE_NOFREE 的说明，实测症状是另一个进程在自己的代码上取指 #PF）；
 *     USER 被清 ⇒ 用户态再也碰不到这一页，而 VMA 还在。
 *   - 不是：不是"给整段区间建映射"。区间里没映射的页是**正常状态**
 *     （按需分页的全部意义就在这），所以这里对不存在的页**直接跳过**、
 *     返回成功，由调用者去改 VMA 的 flags——那一页将来缺页时
 *     `demand_map_page` 会按新的 flags 建映射。
 *
 * ★ 为什么必须逐个叶子项改、不能只看 VMA ★
 * 一个只改 VMA 的实现会让**已经映射的页保持旧权限**：用户态照样能写一个
 * 刚刚被 mprotect 成只读的页（PTE 里 W 还是 1）。这正是本刀的正面判据。
 *
 * ★ TLB：改完必须逐页刷（这是本刀最容易漏的一步）★
 * PTE 改了不等于 TLB 知道。x86-64 上软件改页表**硬件不知情**，于是
 * "页表说只读、TLB 还说可写"这个窗口会一直存在到那条表项自己被换出——
 * 用户态在此期间照写不误。修法只有一条：`invlpg`。
 * 这里**对区间里的每一页都刷**（不管它当时有没有映射）：
 *   - 没映射的页刷一次是空操作，代价可以忽略；
 *   - 而**漏刷**的代价是这条安全属性静默失效。两者不对称，所以选宽的那边。
 * x86-64 没有"按范围失效"的指令，只能逐页 `invlpg`；单核（`--smp 1`）下
 * 这就够了——SMP 还需要给其它核发 IPI，见 docs/13-tasks-and-kill.md §6.5
 * 的同一条边界。 */
fe_status_t fe_vmm_protect(struct fe_address_space *as, virt_addr_t virt,
                           u64 size, u64 new_flags)
{
    if (!as) {
        return FE_ERR_INVAL;
    }
    if (size == 0 || !fe_is_aligned(virt, FE_FRAME_SIZE) ||
        !fe_is_aligned(size, FE_FRAME_SIZE)) {
        return FE_ERR_INVAL;
    }
    /* 只允许改这两位：调用者传别的位一律忽略，而不是让它们生效。
     * 这是接口的一部分——理由见上面"不是什么"那一段。 */
    const u64 touch = FE_PTE_WRITE | FE_PTE_NX;

    for (u64 off = 0; off < size; off += FE_FRAME_SIZE) {
        virt_addr_t va = virt + off;
        u64 *pte = pte_for(as, va, false);
        if (pte && (*pte & FE_PTE_PRESENT) && !(*pte & FE_PTE_HUGE)) {
            u64 keep = *pte & ~touch;
            *pte = keep | (new_flags & touch);
        }
        /* 刷 TLB 在叶子项处理之后、且对每一页都做（理由见上）。 */
        fe_vmm_flush_tlb_page(va);
    }
    return FE_OK;
}

/* ------------------------------------------------------------------ */
/* 查询                                                                */
/* ------------------------------------------------------------------ */

phys_addr_t fe_vmm_translate(struct fe_address_space *as, virt_addr_t virt)
{
    u64 *pdpt = next_table(as->pml4, FE_PML4_INDEX(virt), false, 0);
    if (!pdpt) {
        return 0;
    }
    u64 *pd = next_table(pdpt, FE_PDPT_INDEX(virt), false, 0);
    if (!pd) {
        return 0;
    }
    if (pd[FE_PD_INDEX(virt)] & FE_PTE_HUGE) {
        u64 e = pd[FE_PD_INDEX(virt)];
        if (!(e & FE_PTE_PRESENT)) {
            return 0;
        }
        return (e & FE_PTE_ADDR_MASK) + (virt & (FE_HUGE_2M_SIZE - 1));
    }
    u64 *pt = next_table(pd, FE_PD_INDEX(virt), false, 0);
    if (!pt) {
        return 0;
    }
    u64 e = pt[FE_PT_INDEX(virt)];
    if (!(e & FE_PTE_PRESENT)) {
        return 0;
    }
    return (e & FE_PTE_ADDR_MASK) + (virt & FE_FRAME_MASK);
}

u64 fe_vmm_query_flags(struct fe_address_space *as, virt_addr_t virt)
{
    u64 *pdpt = next_table(as->pml4, FE_PML4_INDEX(virt), false, 0);
    if (!pdpt) {
        return 0;
    }
    u64 *pd = next_table(pdpt, FE_PDPT_INDEX(virt), false, 0);
    if (!pd) {
        return 0;
    }
    if (pd[FE_PD_INDEX(virt)] & FE_PTE_HUGE) {
        return pd[FE_PD_INDEX(virt)];
    }
    u64 *pt = next_table(pd, FE_PD_INDEX(virt), false, 0);
    if (!pt) {
        return 0;
    }
    return pt[FE_PT_INDEX(virt)];
}

void fe_vmm_flush_tlb_page(virt_addr_t virt)
{
    fe_invlpg(virt);
}

/* 逐级打印某个虚拟地址的页表项。诊断「映射看起来建好了但访问仍出错」这类问题专用。 */
void fe_vmm_debug_walk(struct fe_address_space *as, virt_addr_t virt)
{
    u64 *t = as->pml4;
    u32 i4 = FE_PML4_INDEX(virt);
    u64 e4 = t[i4];
    fe_kprintf("  [walk] %#llx PML4[%u] = %#llx\n", (unsigned long long)virt, i4,
               (unsigned long long)e4);
    if (!(e4 & FE_PTE_PRESENT)) {
        return;
    }
    t = pt_from_phys(e4 & FE_PTE_ADDR_MASK);
    u32 i3 = FE_PDPT_INDEX(virt);
    u64 e3 = t[i3];
    fe_kprintf("         PDPT[%u] = %#llx\n", i3, (unsigned long long)e3);
    if (!(e3 & FE_PTE_PRESENT) || (e3 & FE_PTE_HUGE)) {
        return;
    }
    t = pt_from_phys(e3 & FE_PTE_ADDR_MASK);
    u32 i2 = FE_PD_INDEX(virt);
    u64 e2 = t[i2];
    fe_kprintf("         PD[%u]   = %#llx %s\n", i2, (unsigned long long)e2,
               (e2 & FE_PTE_HUGE) ? "(2MiB 页)" : "");
    if (!(e2 & FE_PTE_PRESENT) || (e2 & FE_PTE_HUGE)) {
        return;
    }
    t = pt_from_phys(e2 & FE_PTE_ADDR_MASK);
    u32 i1 = FE_PT_INDEX(virt);
    fe_kprintf("         PT[%u]   = %#llx\n", i1, (unsigned long long)t[i1]);
}

void fe_vmm_flush_tlb_all(void)
{
    fe_write_cr3(fe_read_cr3());
}

/* ------------------------------------------------------------------ */
/* 地址空间生命周期                                                    */
/* ------------------------------------------------------------------ */

struct fe_address_space *fe_vmm_kernel_space(void)
{
    return &g_kernel_space;
}

struct fe_address_space *fe_vmm_space_create(void)
{
    phys_addr_t pml4_phys = fe_pmm_alloc_frame();
    if (pml4_phys == 0) {
        return NULL;
    }
    /* ★ 第二个帧的分配失败**必须单独判**，不能只判指针 ★
     *
     * `pt_from_phys(0)` 不是 NULL —— 它是 HHDM 里物理地址 0 的视图，
     * 也就是一个**看起来完全合法**的指针。所以原来那句 `if (!as)`
     * 在"帧分配失败"时永远不会成立，返回出去的 `as` 指向物理 0。
     * 之后 memset 会去写低端内存，而 `fe_vmm_space_destroy` 里
     * `as_phys = (uptr)as - g_hhdm_offset` 算出来正好是 **0**，
     * 于是 `fe_pmm_free_frame(0)` → panic。
     *
     * 也就是说：**内存不足被伪装成了一次内核 panic**（而且中间还写了
     * 低端物理内存）。这类"用 NULL 检查一个经过地址转换的指针"的错误，
     * 在这个项目里是通用形状——凡 `pt_from_phys` 出来的东西都要单独查
     * 它的**物理来源**是否为 0。 */
    phys_addr_t as_frame = fe_pmm_alloc_frame();
    if (as_frame == 0) {
        fe_pmm_free_frame(pml4_phys);
        return NULL;
    }
    struct fe_address_space *as = (struct fe_address_space *)pt_from_phys(as_frame);
    memset(as, 0, FE_FRAME_SIZE);

    as->pml4_phys = pml4_phys;
    as->pml4 = pt_from_phys(pml4_phys);
    memset(as->pml4, 0, FE_FRAME_SIZE);

    /* 共享内核半区：把内核 PML4 的上半部分原样复制过来。
     * 页表本身是共享的（只读共享给进程），进程只会在下半区创建自己的表。 */
    for (u32 i = KERNEL_PML4_FIRST; i < 512; i++) {
        as->pml4[i] = g_kernel_space.pml4[i];
    }
    as->refcount = 1;
    as->is_kernel = false;
    return as;
}

/* 递归释放用户半区的所有页表与数据帧。
 * level 表示当前正在遍历的表所处的层级：4=PML4, 3=PDPT, 2=PD, 1=PT。 */
/* 正在销毁的地址空间（**仅用于诊断**）：让 free_table_level 里发现的
 * 异常项能立刻对上"这是哪个地址空间"。定位完这个 bug 就删掉。 */
static struct fe_address_space *g_destroying_as;

static void free_table_level(u64 *table, int level)
{
    for (u32 i = 0; i < 512; i++) {
        u64 e = table[i];
        if (!(e & FE_PTE_PRESENT)) {
            continue;
        }
        if (level == 1) {
            /* PT 层：叶子项指向数据帧。
             * ★ 只还"确实属于本地址空间"的帧 ★ 见 FE_PTE_NOFREE 的说明：
             * 内存对象映射与 MMIO 映射背后不是本空间的帧，还回去就是
             * 把借来的内存送给 PMM，然后两个使用者互相踩。
             * PCD 也一并挡住：MMIO 一定关缓存，这是第二道保险。 */
            phys_addr_t f = e & FE_PTE_ADDR_MASK;
            if (e & (FE_PTE_NOFREE | FE_PTE_PCD)) {
                continue;
            }
            if (f == 0) {
                /* 收集证据：present 却说地址是 0 的项不该存在。
                 * 打印它是哪张表、第几项、原值——没有这三样就只能猜。 */
                fe_kprintf("[vmm] **PT 项 present 但物理地址为 0**：表=%p 项=%u 值=%#llx\n",
                           (void *)table, i, (unsigned long long)e);
            }
            fe_pmm_free_frame(f);
            continue;
        }
        if (e & FE_PTE_HUGE) {
            /* 大页：释放其覆盖的 4KiB 帧。level==2 是 2MiB 页，level==3 是 1GiB 页 */
            u64 pages = (level == 3) ? (FE_HUGE_1G_SIZE / FE_FRAME_SIZE)
                                     : (FE_HUGE_2M_SIZE / FE_FRAME_SIZE);
            phys_addr_t base = e & FE_PTE_ADDR_MASK;
            if (e & (FE_PTE_NOFREE | FE_PTE_PCD)) {
                continue;       /* 同上：借来的整段也不还 */
            }
            if (base == 0) {
                fe_kprintf("[vmm] **大页项 present 但物理地址为 0**：表=%p 项=%u "
                           "层级=%d 值=%#llx\n",
                           (void *)table, i, level, (unsigned long long)e);
            }
            for (u64 k = 0; k < pages; k++) {
                fe_pmm_free_frame(base + k * FE_FRAME_SIZE);
            }
            continue;
        }
        u64 *child = pt_from_phys(e & FE_PTE_ADDR_MASK);
        if ((e & FE_PTE_ADDR_MASK) == 0) {
            /* ★ "present 但地址为 0"的非叶项：**不能**照着走 ★
             *
             * 它没有任何合法来源——中间层的项要么不存在，要么指向一个真实的
             * 页表帧。走到这里说明这张表被写坏了（或者这张表本身就是一帧
             * 已被回收、又被当成数据用的内存）。
             *
             * 而照着它走一次就是灾难：`pt_from_phys(0)` 是 HHDM 视图下的
             * **物理 0**，把低端内存当成页表念一遍，会释放掉一大堆
             * 毫不相干的帧——实测症状是另一个进程（fsd）的代码页被释放，
             * 它在自己的取指上 #PF，接着 `fe_pmm_free_frames(0)` panic。
             * 一次"表被写坏"就这样变成了"到处崩"。
             *
             * 所以这里**只报告、不释放**：把损坏限制在它本来的范围内，
             * 让根因还能被查。代价是那一棵子树（如果真的存在）会泄漏，
             * 但那比拆掉别人的内存好得多——而且它本来就不该存在。 */
            fe_kprintf("[vmm] **非叶项 present 但物理地址为 0**：表=%p 项=%u "
                       "层级=%d 值=%#llx（销毁中的 as=%p pml4=%#llx mapped=%llu）"
                       "—— 跳过该子树，不释放\n",
                       (void *)table, i, level, (unsigned long long)e,
                       (void *)g_destroying_as,
                       g_destroying_as ? (unsigned long long)g_destroying_as->pml4_phys : 0,
                       g_destroying_as ? g_destroying_as->mapped_pages : 0);
            g_destroying_as = NULL;
            continue;
        }
        free_table_level(child, level - 1);
        fe_pmm_free_frame(e & FE_PTE_ADDR_MASK);
    }
}

void fe_vmm_space_destroy(struct fe_address_space *as)
{
    if (!as || as->is_kernel) {
        return;
    }
    if (as->refcount > 1) {
        as->refcount--;
        return;
    }
    /* ★ 拆掉一个**当前正装载在 CR3 里**的地址空间是致命的 ★
     * 释放之后 CPU 仍拿着那一帧做页表遍历，而 PMM 可以立刻把它分配给别人。
     * 症状是"代码自己消失"（用户线程在自己的取指上 #PF），
     * 以及随后的二次销毁（读回已释放的结构体，pml4_phys 读到 0）。
     * 正确的做法是**切换路径保证内核线程也换回内核空间**（见
     * fe_sched_maybe_switch），这里只把违规的那一次大声报出来——
     * 修的是根，不是在这里兜。 */
    if (fe_read_cr3() == as->pml4_phys) {
        fe_kprintf("[vmm] **正在销毁当前 CR3 指向的地址空间**（pml4=%#llx）"
                   "——切换路径没有换回内核空间\n",
                   (unsigned long long)as->pml4_phys);
    }
    g_destroying_as = as;    /* 只回收用户半区；内核半区是共享的，不能释放 */
    for (u32 i = 0; i < KERNEL_PML4_FIRST; i++) {
        u64 e = as->pml4[i];
        if (!(e & FE_PTE_PRESENT)) {
            continue;
        }
        u64 *pdpt = pt_from_phys(e & FE_PTE_ADDR_MASK);
        free_table_level(pdpt, 3);
        fe_pmm_free_frame(e & FE_PTE_ADDR_MASK);
        as->pml4[i] = 0;
    }
    phys_addr_t as_phys = (phys_addr_t)((uptr)as - g_hhdm_offset);
    fe_pmm_free_frame(as->pml4_phys);
    fe_pmm_free_frame(as_phys);
}

void fe_vmm_switch(struct fe_address_space *as)
{
    fe_write_cr3(as->pml4_phys);
}

/* 把一段 MMIO 物理区间映射到 MMIO 窗口。
 *
 * 映射地址取 FE_MMIO_BASE + (phys % FE_MMIO_SIZE)：确定性映射，同一物理区间
 * 永远得到同一虚拟地址，重复调用不会泄漏虚拟空间。必须带 PCD（关闭缓存），
 * 否则 CPU 会缓存设备寄存器读值。 */
void *fe_vmm_map_mmio(phys_addr_t phys, u64 size)
{
    if (size == 0) {
        return NULL;
    }
    virt_addr_t virt = FE_MMIO_BASE + (phys % FE_MMIO_SIZE);
    u64 pages = (size + FE_FRAME_SIZE - 1) / FE_FRAME_SIZE;
    if (virt + pages * FE_FRAME_SIZE > FE_MMIO_END) {
        return NULL;
    }
    for (u64 i = 0; i < pages; i++) {
        if (fe_vmm_translate(&g_kernel_space, virt + i * FE_FRAME_SIZE) != 0) {
            continue;   /* 已映射（同一设备被多次请求） */
        }
        fe_status_t s = fe_vmm_map(&g_kernel_space, virt + i * FE_FRAME_SIZE,
                                   (phys & ~FE_FRAME_MASK) + i * FE_FRAME_SIZE,
                                   FE_FRAME_SIZE,
                                   FE_PTE_WRITE | FE_PTE_PCD | FE_PTE_NX | FE_PTE_GLOBAL);
        if (fe_failed(s)) {
            return NULL;
        }
    }
    return (void *)(uptr)(virt + (phys & FE_FRAME_MASK));
}

/* ------------------------------------------------------------------ */
/* 内核地址空间建立                                                    */
/* ------------------------------------------------------------------ */

/* 按 ELF 程序头映射内核映像本身：可执行段给 X，可写段给 W，其余给 NX，
 * 从而实现内核自身的 W^X（可写页不可执行）。
 * 物理布局与虚拟布局线性对应：phys = kernel_phys_base + (vaddr - kernel_virt_base)。 */
static void map_kernel_image(struct fe_address_space *as)
{
    const struct fe_boot_info *bi = fe_boot_info();
    const u8 *elf = (const u8 *)bi->kernel_elf;
    if (!elf) {
        fe_panic("引导器未提供内核 ELF 映像，无法建立页表");
    }
    const struct fe_elf64_ehdr *eh = (const struct fe_elf64_ehdr *)(const void *)elf;
    if (!fe_elf_check(eh)) {
        fe_panic("内核 ELF 映像头非法");
    }

    u32 mapped_segments = 0;
    for (u32 i = 0; i < eh->e_phnum; i++) {
        const struct fe_elf64_phdr *ph =
            (const struct fe_elf64_phdr *)(const void *)(elf + eh->e_phoff + (u64)i * eh->e_phentsize);
        if (ph->p_type != FE_PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        virt_addr_t vstart = FE_FRAME_ALIGN_DOWN(ph->p_vaddr);
        virt_addr_t vend = FE_FRAME_ALIGN_UP(ph->p_vaddr + ph->p_memsz);

        u64 flags = FE_PTE_GLOBAL;
        if (ph->p_flags & FE_PF_W) {
            flags |= FE_PTE_WRITE;
        }
        if (!(ph->p_flags & FE_PF_X)) {
            flags |= FE_PTE_NX;
        }
        phys_addr_t phys = bi->kernel_phys_base + (vstart - bi->kernel_virt_base);
        fe_status_t s = fe_vmm_map(as, vstart, phys, vend - vstart, flags);
        if (fe_failed(s)) {
            fe_panic("映射内核段 %u 失败 (%#llx, %llu 字节): %s", i,
                     (unsigned long long)vstart,
                     (unsigned long long)(vend - vstart), fe_status_name(s));
        }
        mapped_segments++;
    }
    if (mapped_segments == 0) {
        fe_panic("内核 ELF 中没有可加载段");
    }
}

/* HHDM：把 [0, limit) 的物理内存以 2MiB 大页直接映射到高半区 */
static void map_hhdm(struct fe_address_space *as, u64 limit)
{
    u64 size = fe_align_up(limit, FE_HUGE_2M_SIZE);
    fe_status_t s = fe_vmm_map_huge_2m(as, g_hhdm_offset, 0, size,
                                       FE_PTE_WRITE | FE_PTE_GLOBAL);
    if (fe_failed(s)) {
        fe_panic("建立 HHDM 映射失败: %s", fe_status_name(s));
    }
}

/* ------------------------------------------------------------------ */
/* 自检：撤销映射（M11）                                               */
/* ------------------------------------------------------------------ */

/* 这一组测的是 MEM_UNMAP 的**机制**：撤销之后那段地址上到底还有什么。
 *
 * ★ 为什么"撤销"值得单独自检 ★
 * 撤销错了有两种都很安静的表现：
 *   1. 只清了页表项没清 TLB → 用户继续读写一块"已经不存在"的映射（读到旧数据，
 *      而页表看起来是对的）；
 *   2. 清了页表项却把帧还给了 PMM → 同一块物理内存被分给下一个用户，
 *      症状是**别人的数据随机损坏**（这个项目里最难查的一类）。
 * 两者都不会让任何程序崩溃在出错的地方，所以必须有断言盯着。
 *
 * ★ 反向对照 ★ 撤销之后在同一地址映射**另一帧**，那块内存必须是干净的：
 * 如果看得见旧内容，说明 PTE 根本没被清掉（只是看起来清了）。 */
u32 fe_selftest_vmm_unmap(void)
{
    u32 fail = 0;
    const virt_addr_t va = 0x30000000ull;

    u64 free_before = fe_pmm_free_frame_count();
    struct fe_address_space *as = fe_vmm_space_create();
    if (!as) {
        fe_kprintf("        地址空间创建失败\n");
        return 1;
    }

    phys_addr_t fa = fe_pmm_alloc_frame();
    phys_addr_t fb = fe_pmm_alloc_frame();
    if (fa == 0 || fb == 0) {
        fe_kprintf("        测试帧分配失败\n");
        fe_vmm_space_destroy(as);
        return 1;
    }
    /* 两帧都写上不同的标记，这样"看到谁的内容"是可判定的 */
    memset((void *)(uptr)(fa + g_hhdm_offset), 0xAA, FE_FRAME_SIZE);
    memset((void *)(uptr)(fb + g_hhdm_offset), 0x00, FE_FRAME_SIZE);

    if (fe_failed(fe_vmm_map(as, va, fa, FE_FRAME_SIZE,
                             FE_PTE_USER | FE_PTE_WRITE | FE_PTE_NX))) {
        fe_kprintf("        映射测试帧失败\n");
        fail++;
    }

    /* ---- 撤销 ---- */
    if (fe_failed(fe_vmm_unmap(as, va, FE_FRAME_SIZE))) {
        fe_kprintf("        撤销映射失败\n");
        fail++;
    } else if (fe_vmm_translate(as, va) != 0) {
        fe_kprintf("        撤销之后仍有物理映射: %#llx\n",
                   (unsigned long long)fe_vmm_translate(as, va));
        fail++;
    } else if (fe_vmm_query_flags(as, va) & FE_PTE_PRESENT) {
        fe_kprintf("        撤销之后页表项仍标为 present\n");
        fail++;
    } else {
        fe_kprintf("        撤销映射: 页表项已清、地址不再可翻译\n");
    }

    /* ---- 已撤销的地址再撤销：VMM 层报告"不存在" ----
     *
     * 这一条与用户可见的 MEM_UNMAP 语义**不同**，是刻意的分层：
     *   - VMM 是机制层，它如实回答"这个地址上有没有映射"；
     *   - MEM_UNMAP 是接口层，它承诺的是后置条件"这段地址上没有映射"，
     *     所以重复撤销返回成功（幂等）——调用者把清理写在收尾路径上时，
     *     不该为了"我到底映射过没有"去判断返回值。
     * 用户可见的那一半在 user/bin/hxtest 里断言（走真实系统调用）。 */
    fe_status_t again = fe_vmm_unmap(as, va, FE_FRAME_SIZE);
    if (again == FE_ERR_NOENT) {
        fe_kprintf("        已撤销的地址再次撤销: %s（VMM 如实报告；syscall 层把这一条"
                   "翻成成功）\n", fe_status_name(again));
    } else {
        fe_kprintf("        已撤销的地址再次撤销返回 %s（期望 FE_ERR_NOENT）\n",
                   fe_status_name(again));
        fail++;
    }

    /* ---- 反向对照 2：撤销后在同一地址映射另一帧，必须看不到旧内容 ---- */
    if (fe_failed(fe_vmm_map(as, va, fb, FE_FRAME_SIZE,
                             FE_PTE_USER | FE_PTE_WRITE | FE_PTE_NX))) {
        fe_kprintf("        重新映射失败\n");
        fail++;
    } else {
        fe_vmm_switch(as);
        u32 bad = 0;
        volatile u8 *p = (volatile u8 *)(uptr)va;
        for (u32 i = 0; i < FE_FRAME_SIZE; i++) {
            if (p[i] != 0x00) {
                bad++;
            }
        }
        fe_vmm_switch(fe_vmm_kernel_space());
        if (bad) {
            fe_kprintf("        重新映射后仍能看到旧映射的内容（%u 字节为 0xAA）\n", bad);
            fail++;
        } else {
            fe_kprintf("        重新映射: 旧映射的内容不可见（撤销真的生效了）\n");
        }
        /* ---- 反向对照 3：重复映射同一区间必须被拒，而不是静默覆盖 ---- */
        fe_status_t dup = fe_vmm_map(as, va, fa, FE_FRAME_SIZE,
                                     FE_PTE_USER | FE_PTE_WRITE | FE_PTE_NX);
        if (dup != FE_ERR_EXIST) {
            fe_kprintf("        对已映射区间再次映射返回 %s（期望 FE_ERR_EXIST）\n",
                       fe_status_name(dup));
            fail++;
        } else {
            fe_kprintf("        对已映射区间再次映射被拒: %s\n", fe_status_name(dup));
        }
    }

    /* ---- 撤销不释放帧：帧必须由调用者/对象归还 ----
     *
     * 两次撤销之后，两个帧仍然归我们（这就是"撤销 ≠ 释放"的定义）。
     * 断言放在最后复元时一起看，因为此刻地址空间自己的页表帧也还没还
     * ——把中间层的帧算进"帧计数异常"里是测试自己的错。 */
    fe_vmm_unmap(as, va, FE_FRAME_SIZE);
    fe_pmm_free_frame(fa);
    fe_pmm_free_frame(fb);
    fe_vmm_space_destroy(as);

    u64 free_after = fe_pmm_free_frame_count();
    if (free_after != free_before) {
        fe_kprintf("        自检结束后帧未复元: %llu -> %llu（差 %lld）\n",
                   (unsigned long long)free_before, (unsigned long long)free_after,
                   (long long)free_before - (long long)free_after);
        fail++;
    } else {
        fe_kprintf("        撤销**不**释放物理帧（帧归内存对象所有）：两个帧由调用者归还后"
                   "空闲计数复元 %llu\n",
                   (unsigned long long)free_after);
    }
    return fail;
}

void fe_vmm_init(void)
{
    const struct fe_boot_info *bi = fe_boot_info();
    g_hhdm_offset = bi->hhdm_offset;

    memset(&g_kernel_space, 0, sizeof(g_kernel_space));
    g_kernel_space.pml4_phys = fe_pmm_alloc_frame();
    if (g_kernel_space.pml4_phys == 0) {
        fe_panic("无法为内核地址空间分配 PML4");
    }
    g_kernel_space.pml4 = pt_from_phys(g_kernel_space.pml4_phys);
    memset(g_kernel_space.pml4, 0, FE_FRAME_SIZE);
    g_kernel_space.refcount = 1;
    g_kernel_space.is_kernel = true;

    u64 hhdm_limit = bi->highest_address;
    if (hhdm_limit > FE_HHDM_MAX_PHYS) {
        hhdm_limit = FE_HHDM_MAX_PHYS;
    }
    map_hhdm(&g_kernel_space, hhdm_limit);
    map_kernel_image(&g_kernel_space);

    /* 切换到自有页表：此后不再依赖引导器留下的页表。
     * 切换前必须保证「正在执行的代码、栈、HHDM、页表自身」都已映射好，
     * 上面几步已经覆盖了这些。 */
    fe_vmm_switch(&g_kernel_space);
    fe_vmm_flush_tlb_all();
}
