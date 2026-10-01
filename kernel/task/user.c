/* SPDX-License-Identifier: 0BSD */
#include <fe/user.h>
#include <fe/task.h>
#include <fe/sched/sched.h>
#include <fe/sched/thread.h>
#include <fe/mm.h>
#include <fe/mm/vmm.h>
#include <fe/mm/pmm.h>
#include <fe/mm/vma.h>
#include <fe/mm/kheap.h>
#include <fe/kprintf.h>
#include <fe/string.h>
#include <fe/panic.h>
#include <fe/boot/bootinfo.h>
#include <fe/time.h>
#include <fe/idt.h>

/* ------------------------------------------------------------------ */
/* 用户态相关的内核侧支持                                              */
/*                                                                     */
/* 注意这里**没有**「运行某个用户程序」的代码了。学 Linux：内核的最后一件事  */
/* 是 exec /init，之后所有用户程序都由 init 用 spawn/wait 拉起。          */
/* 内核里保留的只有「用户态出了事，内核看见了什么」这一项证据。            */
/* ------------------------------------------------------------------ */

/* init 跑完之后核对：drvdeny 那个故意越权的程序，必须是被 #GP 打死的。
 *
 * 内核能看到的证据比用户态强：用户态只看得见退出码 -1，而别的异常碰巧
 * 也会返回 -1；内核直接看异常向量，13 就是 13。 */
u32 fe_selftest_user(void)
{
    u32 fail = 0;
    if (fe_user_fault_count() == 0) {
        fe_kprintf("        没有任何用户态异常——drvdeny 应当触发一次 #GP\n");
        return 1;
    }
    u64 vec = fe_last_user_fault_vector();
    if (vec != 13) {
        fe_kprintf("        最近一次用户态异常向量是 %llu，期望 13 (#GP)\n",
                   (unsigned long long)vec);
        fail++;
    } else {
        fe_kprintf("        用户态异常向量 = 13 (#GP)，累计 %llu 次 —— 未认领端口的 in "
                   "被 CPU 逐条指令拒绝\n",
                   (unsigned long long)fe_user_fault_count());
    }
    return fail;
}

/* 由 task.c 提供：分配一个任务对象（不含地址空间） */
struct fe_task *fe_task_alloc(const char *name);

struct fe_task *fe_task_create_user(const char *name)
{
    struct fe_task *t = fe_task_alloc(name);
    if (!t) {
        return NULL;
    }
    t->space = fe_vmm_space_create();
    if (!t->space) {
        fe_object_unref(&t->hdr);
        return NULL;
    }
    /* 区间表：用户任务才有。分配失败就直接失败——
     * 没有区间表的用户任务是**残缺**的（#PF 永远无法解析、栈永远不能长），
     * 放它跑起来只会让问题在更晚、更难查的地方出现。 */
    t->vmas = (struct fe_vma_table *)fe_kzalloc(sizeof(struct fe_vma_table));
    if (!t->vmas) {
        fe_object_unref(&t->hdr);
        return NULL;
    }
    fe_vma_table_init(t->vmas);
    t->user_map_next = FE_USER_MMAP_BASE;
    t->user_stack_next = FE_USER_STACK_AREA_TOP;
    t->stack_low = 0;
    return t;
}

/* ------------------------------------------------------------------ */
/* 按需分页与栈增长                                                    */
/* ------------------------------------------------------------------ */

/* 把一页按 VMA 的权限补上映射。返回 0 成功。
 *
 * 物理帧的来源按区间类型决定：
 *   frames != NULL → 用区间准备好的那一帧（预映射）；
 *   否则（ANON）   → 现分配一帧并清零。
 * ★ 清零不是可选的 ★ 新分配的物理帧带着**上一个使用者**的数据，
 * 不 zero 就等于把内核/别人的内存内容泄漏给用户态。 */
static fe_status_t demand_map_page(struct fe_task *t, struct fe_vma *v, u64 va)
{
    u64 pte = FE_PTE_USER;
    if (v->flags & FE_VMA_WRITE) {
        pte |= FE_PTE_WRITE;
    }
    if (!(v->flags & FE_VMA_EXEC)) {
        pte |= FE_PTE_NX;       /* 不可执行的区间一律加 NX：W^X 的默认方向 */
    }

    phys_addr_t frame = 0;
    if (v->frames) {
        /* 区间自带帧表：这是预映射区间（映像/显式映射），
         * 缺页说明"区间里有一页没映射"——那是兜底路径，不是常规路径。 */
        u64 idx = (va - v->base) / FE_FRAME_SIZE;
        frame = v->frames[idx];
        if (frame == 0) {
            return FE_ERR_NOENT;
        }
    } else if (v->flags & FE_VMA_ANON) {
        frame = fe_pmm_alloc_frame();
        if (frame == 0) {
            return FE_ERR_NOMEM;
        }
        memset((void *)(uptr)(frame + fe_vmm_hhdm_offset()), 0, FE_FRAME_SIZE);
    } else {
        /* 既没有后备帧也不是匿名区间：这个区间**不该**有缺页可补。
         * 明确拒绝，而不是"顺手给它一页"——后者会把
         * "区间描述与页表不一致"这种真问题掩盖掉。 */
        return FE_ERR_NOTSUP;
    }

    fe_status_t s = fe_vmm_map(t->space, va, frame, FE_FRAME_SIZE, pte);
    if (fe_failed(s)) {
        if (!v->frames) {
            fe_pmm_free_frame(frame);   /* 自己分配的才由自己回收 */
        }
        return s;
    }
    return FE_OK;
}

bool fe_user_resolve_fault(struct fe_task *t, u64 fault_addr, u64 error_code)
{
    if (!t || !t->space || !t->vmas) {
        return false;
    }
    u64 va = FE_FRAME_ALIGN_DOWN(fault_addr);
    bool is_write = (error_code & 2) != 0;

    /* ★★ 判断顺序：先"栈的紧邻下方"，再"落在某个区间内" ★★
     *
     * 这里有一个**设计缺陷**值得写清楚，因为第一版就是错的：
     *
     *   第一版先查区间（fe_vma_find），查不到才考虑栈增长——理由是
     *   "落在区间内 = 本来就有权访问，直接补页即可"。但栈的**保留区**
     *   （允许长到哪儿）本来就覆盖了要增长的那些地址，所以
     *   "区间查不到"与"这是栈增长"**永远不可能同时成立**：
     *   栈增长那条分支是死代码，而每个页都在走"区间内按需补页"，
     *   于是 `stack_low` 永不前移、`fe_vma_grow_down` 永不被调用。
     *
     *   症状很有欺骗性：页照样补上了、`#PF` 照样被消化，
     *   看起来"按需分页在工作"——只是"跳过保留区一大段"这种越界
     *   也被静默接受了（因为它们在保留区内），保护弱了一层而没有报错。
     *
     * 正确顺序：**先问"这是不是紧挨着已映射栈底的下一格"**——
     * 是，就走栈增长（唯一会改 `stack_low` 的路径）；
     * 不是，再按区间权限判断（匿名映射按需补页 / 只读与 NX 的拒绝）。
     * 于是"跳过一大段"落在保留区内、又不紧邻 → 被拒。
     *
     * 判据用 `t->stack_low`（**已映射**到的最低页）而不是 VMA 的 base：
     * base 是保留下界，按它判就等于"保留区 = 任意分配入口"，
     * `#PF` 不再是保护机制。只认紧邻一页，正是 Linux
     * expand_downwards 的语义。 */
    u64 probe = t->stack_low;
    if (probe != 0 && va + FE_FRAME_SIZE == probe) {
        struct fe_vma *stack = fe_vma_find(t->vmas, probe);
        if (!stack) {
            /* 已映射的最低页不在任何区间里：不变量被破坏，拒绝补页。
             * 补下去只会让它继续跑，而问题出在别处。 */
            return false;
        }
        if (!(stack->flags & FE_VMA_GROWSDOWN)) {
            return false;           /* 不是栈，不许往下长 */
        }
        if (is_write && !(stack->flags & FE_VMA_WRITE)) {
            return false;
        }
        if ((error_code & 0x10) && !(stack->flags & FE_VMA_EXEC)) {
            return false;           /* 栈不可执行：取指必须拒绝 */
        }
        if (fe_failed(fe_vma_grow_down(t->vmas, stack, va))) {
            return false;           /* 撞上别的区间：如实失败（栈的边界） */
        }
        if (fe_failed(demand_map_page(t, stack, va))) {
            /* 补页失败要把区间改回去，否则 VMA 说"这段归我"、
             * 页表说"没有"——两者不一致会在下一次缺页时
             * 被当成"已经增长了"从而不再尝试。 */
            stack->base = probe;
            return false;
        }
        /* ★ 顺序：先确认映射成功，再更新"已映射到哪" ★
         * 反过来写的话，补页失败时 stack_low 已经改小、区间却改回去了——
         * 下一次缺页不再满足"紧邻"，栈从此再也长不动。 */
        t->stack_low = va;
        return true;
    }

    struct fe_vma *v = fe_vma_find(t->vmas, va);
    if (!v) {
        /* 不紧邻、也不在任何区间内：越界访问 */
        return false;
    }

    /* 落在区间内：权限检查先做。写一个只读区间是**保护违例**，
     * 不能因为"页不存在"就先给它一页——那会把只读区变成可写的。 */
    if (is_write && !(v->flags & FE_VMA_WRITE)) {
        return false;
    }
    if (!is_write && !(v->flags & FE_VMA_READ)) {
        return false;
    }
    /* error_code 的 bit4 = 取指（instruction fetch）。执行一个不可执行区间
     * 同样必须拒绝，否则 NX 形同虚设。 */
    if ((error_code & 0x10) && !(v->flags & FE_VMA_EXEC)) {
        return false;
    }
    return fe_ok(demand_map_page(t, v, va));
}

/* ------------------------------------------------------------------ */
/* 自检：按需分页与栈增长                                              */
/* ------------------------------------------------------------------ */

#define CHECK(cond) do { if (!(cond)) { fail++; } } while (0)

/* ★ 这一组的重点是**反向对照** ★
 *
 * 正向（"栈能长下去"）很容易做到——把任何缺页都补一页也能让它通过。
 * 真正要证明的是另一条：**不该补的绝对不补**。因为一个"什么缺页都补"的
 * 实现会把段错误变成静默的内存增长：空指针解引用会拿到一页零然后继续跑，
 * 直到别处出错——那时离真正的原因已经很远了。所以每一条正向都配一条反向：
 *
 *   栈下方紧邻一页     → 补（正向）
 *   跳过保留区一大段   → 不补（否则保留区等于任意分配入口）
 *   区间外             → 不补
 *   往只读区间写       → 不补（否则只读形同虚设）
 *   执行不可执行区间   → 不补（否则 NX 形同虚设）
 *   栈往上越界         → 不补（栈只往下长）
 *   撞上相邻区间       → 不补（增长的边界） */
u32 fe_selftest_demand_paging(void)
{
    u32 fail = 0;
    struct fe_task *t = fe_task_create_user("demand-probe");
    if (!t || !t->space || !t->vmas) {
        fe_kprintf("        探针任务创建失败\n");
        return 1;
    }
    u64 top = FE_USER_STACK_TOP;
    if (fe_failed(fe_user_map_stack(t->space, t->vmas, top, FE_USER_STACK_SIZE,
                                    &t->stack_low))) {
        fe_kprintf("        栈建立失败\n");
        fe_object_unref(&t->hdr);
        return 1;
    }
    u64 low = t->stack_low;
    fe_kprintf("        栈区间 %#llx-%#llx，预映射到 %#llx（其余按需）\n",
               (unsigned long long)(top - FE_USER_STACK_SIZE),
               (unsigned long long)top, (unsigned long long)low);

    /* 顶部必须**已经**映射：初始栈帧（argc/argv/envp）在装载时就写好了，
     * 而那是经 HHDM 写的、不会触发缺页。 */
    CHECK(fe_vmm_translate(t->space, top - FE_FRAME_SIZE) != 0);
    /* 而保留区底部**必须还没有**映射——否则"按需"只是说法。 */
    CHECK(fe_vmm_translate(t->space, top - FE_USER_STACK_SIZE) == 0);

    /* ---- 正向 1：紧邻下方一页 → 补上，且是清零的、用户可写 ---- */
    u64 fa = low - FE_FRAME_SIZE;
    bool r1 = fe_user_resolve_fault(t, fa, 0x6 /* 写、用户、页不存在 */);
    phys_addr_t got = fe_vmm_translate(t->space, fa);
    u64 fl1 = fe_vmm_query_flags(t->space, fa);
    /* ★ 断言失败时把原始值打出来 ★
     * 第一版只报"失败"，于是我花了很久在几个可能的分支之间猜——
     * 而这里需要的只是"哪一项不成立"这个事实。自检的输出必须能直接
     * 指出失败的**具体条件**，否则它只是在说"有问题"。 */
    if (!r1) {
        fe_kprintf("        正向1失败: resolve 返回 false（fa=%#llx low=%#llx）\n",
                   (unsigned long long)fa, (unsigned long long)low);
        fail++;
    }
    if (got == 0) {
        fe_kprintf("        正向1失败: 补页后 translate 仍为 0\n");
        fail++;
    }
    if (!(fl1 & FE_PTE_WRITE)) {
        fe_kprintf("        正向1失败: 补的页不可写（flags=%#llx）\n",
                   (unsigned long long)fl1);
        fail++;
    }
    if (!(fl1 & FE_PTE_USER)) {
        fe_kprintf("        正向1失败: 补的页不是用户页（flags=%#llx）\n",
                   (unsigned long long)fl1);
        fail++;
    }
    if (!(fl1 & FE_PTE_NX)) {
        fe_kprintf("        正向1失败: 栈页竟然是可执行的（flags=%#llx）\n",
                   (unsigned long long)fl1);
        fail++;
    }
    if (t->stack_low != fa) {
        fe_kprintf("        正向1失败: stack_low 没更新（=%#llx，期望 %#llx）\n",
                   (unsigned long long)t->stack_low, (unsigned long long)fa);
        fail++;
    }
    if (got) {
        /* 新页必须全零：物理帧来自全局池，留着上一个使用者的数据就是信息泄漏 */
        const u8 *p = (const u8 *)(uptr)(got + fe_vmm_hhdm_offset());
        u32 nz = 0;
        for (u32 i = 0; i < FE_FRAME_SIZE; i++) {
            if (p[i]) {
                nz++;
            }
        }
        if (nz) {
            fe_kprintf("        新补的栈页不是零（%u 字节非零）—— 泄漏上一个使用者的数据\n", nz);
            fail++;
        }
    }
    fe_kprintf("        正向：紧邻栈下方一页被补上（零填充、可写、NX、stack_low 前移）: OK\n");

    /* ---- 正向 2：再往下连续补两页（栈连续增长） ---- */
    {
        u32 before = fail;
        bool r2 = fe_user_resolve_fault(t, fa - FE_FRAME_SIZE, 0x6);
        bool r3 = fe_user_resolve_fault(t, fa - 2 * FE_FRAME_SIZE, 0x6);
        CHECK(r2);
        CHECK(r3);
        CHECK(t->stack_low == fa - 2 * FE_FRAME_SIZE);
        fe_kprintf("        正向：连续三页按需增长（r2=%d r3=%d stack_low=%#llx）: %s\n",
                   (int)r2, (int)r3, (unsigned long long)t->stack_low,
                   (fail == before) ? "OK" : "失败");
    }

    /* ---- 反向 1：跳过保留区一大段 → 必须拒绝 ---- */
    {
        u32 before = fail;
        u64 far = top - FE_USER_STACK_SIZE + FE_FRAME_SIZE * 4;    /* 离栈底还很远 */
        CHECK(!fe_user_resolve_fault(t, far, 0x6));
        CHECK(fe_vmm_translate(t->space, far) == 0);
        fe_kprintf("        反向：跳过保留区一大段**不**补页: %s\n",
                   (fail == before) ? "OK" : "失败");
    }
    /* ---- 反向 2：区间外（远低于保留区底） → 必须拒绝 ---- */
    {
        u32 before = fail;
        u64 outside = top - FE_USER_STACK_SIZE - FE_FRAME_SIZE * 8;
        CHECK(!fe_user_resolve_fault(t, outside, 0x6));
        CHECK(fe_vmm_translate(t->space, outside) == 0);
        fe_kprintf("        反向：区间外**不**补页: %s\n", (fail == before) ? "OK" : "失败");
    }
    /* ---- 反向 3：栈**上方**越界（栈只往下长） → 必须拒绝 ---- */
    {
        u32 before = fail;
        CHECK(!fe_user_resolve_fault(t, top, 0x6));
        CHECK(!fe_user_resolve_fault(t, top + FE_FRAME_SIZE * 2, 0x6));
        fe_kprintf("        反向：栈上方越界**不**补页（栈只向下长）: %s\n",
                   (fail == before) ? "OK" : "失败");
    }

    /* ---- 反向 4：往只读区间写 → 必须拒绝 ---- */
    {
        u32 before = fail;
        CHECK(fe_ok(fe_vma_add(t->vmas, 0x30000000ull, 0x30001000ull,
                               FE_VMA_READ | FE_VMA_ANON, NULL)));
        /* 区间在、标了 ANON，但**只读**：写它必须被拒 */
        CHECK(!fe_user_resolve_fault(t, 0x30000000ull, 0x2 /* 写 */));
        CHECK(fe_vmm_translate(t->space, 0x30000000ull) == 0);
        /* 而读它应当被补页（同一条区间的正向对照） */
        CHECK(fe_user_resolve_fault(t, 0x30000000ull, 0x0 /* 读 */));
        CHECK(fe_vmm_translate(t->space, 0x30000000ull) != 0);
        fe_kprintf("        反向：只读区间写被拒、读被补（同一区间的正反对照）: %s\n",
                   (fail == before) ? "OK" : "失败");
    }
    /* ---- 反向 5：执行不可执行区间 → 必须拒绝 ---- */
    {
        u32 before = fail;
        CHECK(fe_ok(fe_vma_add(t->vmas, 0x30001000ull, 0x30002000ull,
                               FE_VMA_READ | FE_VMA_ANON, NULL)));
        CHECK(!fe_user_resolve_fault(t, 0x30001000ull, 0x10 /* 取指 */));
        CHECK(fe_vmm_translate(t->space, 0x30001000ull) == 0);
        fe_kprintf("        反向：不可执行区间取指被拒（NX 不是摆设）: %s\n",
                   (fail == before) ? "OK" : "失败");
    }
    /* ---- 反向 6：没有 ANON 也没有后备帧的区间 → 不补 ----
     * 这一条防的是"区间描述与页表不一致"被掩盖：预映射段（映像）缺页
     * 说明装载出了问题，补一页零只会让它带着坏映像继续跑。 */
    {
        u32 before = fail;
        CHECK(fe_ok(fe_vma_add(t->vmas, 0x30002000ull, 0x30003000ull,
                               FE_VMA_READ | FE_VMA_WRITE, NULL)));
        CHECK(!fe_user_resolve_fault(t, 0x30002000ull, 0x2));
        fe_kprintf("        反向：无后备也无 ANON 的区间不补页（不掩盖不一致）: %s\n",
                   (fail == before) ? "OK" : "失败");
    }
    /* ---- 反向 7：栈增长撞上相邻区间 → 必须拒绝 ---- */
    {
        u32 before = fail;
        /* 在栈保留区正下方放一个区间，然后把栈长到贴着它 */
        u64 blk_end = top - FE_USER_STACK_SIZE;
        CHECK(fe_ok(fe_vma_add(t->vmas, blk_end - FE_FRAME_SIZE, blk_end,
                               FE_VMA_READ | FE_VMA_ANON, NULL)));
        /* 一直补到栈底的前一页 */
        u64 va = t->stack_low;
        while (va > blk_end) {
            if (!fe_user_resolve_fault(t, va - FE_FRAME_SIZE, 0x6)) {
                break;
            }
            va -= FE_FRAME_SIZE;
        }
        /* 再往下一格就是邻居：必须拒绝 */
        u64 collide = va - FE_FRAME_SIZE;
        CHECK(!fe_user_resolve_fault(t, collide, 0x6));
        fe_kprintf("        反向：栈增长撞上相邻区间被拒（%#llx）: %s\n",
                   (unsigned long long)collide, (fail == before) ? "OK" : "失败");
    }

    fe_vma_dump(t->vmas, "demand-probe");
    fe_object_unref(&t->hdr);
    return fail;
}

/* fe_user_map_stack：把 [top-size, top) 建成一个**栈区间**。
 *
 * ★ 这里改成了"只映射一部分 + 其余按需" ★
 * 原来是把整段 64 KiB 全部映射（每一页都占一个物理帧）。改法：
 *   - VMA 覆盖整个保留范围（一直向下留出增长空间）；
 *   - 只预映射**顶部若干页**（够放初始栈帧），其余留给 #PF 补。
 * 为什么不一页都不映射：初始栈是内核在装载时就写好的（argc/argv/envp），
 * 而那些写入走的是 HHDM、不触发缺页——所以顶部必须在装载时就存在。
 * 只留最上面几页，正是"栈按需增长"该有的起点：程序一用到更深处，
 * 就会真的走一次 #PF 增长路径（而不是"预先都映射好了，看起来像按需"）。 */
#define STACK_PREMAP_PAGES 4

fe_status_t fe_user_map_stack(struct fe_address_space *as, struct fe_vma_table *vmas,
                              u64 top, u64 size, u64 *out_stack_low)
{
    if (!as || size == 0) {
        return FE_ERR_INVAL;
    }
    u64 end = FE_FRAME_ALIGN_UP(top);
    u64 base = FE_FRAME_ALIGN_DOWN(top - size);
    u64 premap_from = end - STACK_PREMAP_PAGES * FE_FRAME_SIZE;
    if (premap_from < base) {
        premap_from = base;
    }

    /* 1. 区间：**只覆盖"已映射"的那一段**，不是整个保留范围。
     *
     * ★ 这里是第一版的设计缺陷，值得写清楚 ★
     *
     * 原来把区间建成整个保留范围 [top-size, top)，理由是"这段地址允许
     * 被栈使用"。但那样一来，要增长的那一页**本来就在区间里**——
     * `fe_vma_find` 会命中它，于是"紧邻栈底的下一格"这条判据永远轮不到
     * （严格说：它排在 `hit` 之后，永远不会被执行）；就算轮到，
     * `fe_vma_grow_down` 也会因为"new_base 落在区间内部"而判非法。
     * 两个条件互相矛盾，症状是"页照样补上了、栈却永远长不动"。
     *
     * 正确的模型是把两者分开：
     *   区间 base = **已映射**到哪儿（缺页时向下移动），
     *   end       = 栈顶（不变），
     *   保留下界   = 由 `stack_low` 的下移与"撞上邻居必须被拒"共同约束
     *              （见 fe_vma_grow_down 的碰撞检查）。
     * 这样"命中区间"= 在已映射区域内（补页即可），
     *     "紧邻下方"= 栈增长（要移动 base），两者不再重叠。 */
    if (vmas) {
        fe_status_t s = fe_vma_add(vmas, premap_from, end,
                                   FE_VMA_READ | FE_VMA_WRITE | FE_VMA_ANON |
                                   FE_VMA_GROWSDOWN, NULL);
        if (fe_failed(s)) {
            return s;
        }
    }

    /* 2. 只预映射顶部几页（初始栈帧落在那里） */
    u32 mapped = 0;
    for (u64 va = premap_from; va < end; va += FE_FRAME_SIZE) {
        phys_addr_t frame = fe_pmm_alloc_frame();
        if (frame == 0) {
            return FE_ERR_NOMEM;
        }
        /* 栈：可读可写、不可执行——栈上的数据永远不该被执行 */
        fe_status_t s = fe_vmm_map(as, va, frame, FE_FRAME_SIZE,
                                   FE_PTE_USER | FE_PTE_WRITE | FE_PTE_NX);
        if (fe_failed(s)) {
            fe_pmm_free_frame(frame);
            return s;
        }
        /* 经 HHDM 按**物理帧**清零（不能用 va：此刻可能正跑在别的地址空间上） */
        memset((void *)(uptr)(frame + fe_vmm_hhdm_offset()), 0, FE_FRAME_SIZE);
        mapped++;
    }
    (void)mapped;
    if (out_stack_low) {
        *out_stack_low = premap_from;
    }
    return FE_OK;
}

u64 fe_user_thread_create(void (*entry)(void *), void *arg,
                          u64 stack, u64 stack_size, u32 flags)
{
    (void)flags;
    struct fe_task *t = fe_task_current();
    if (!t || !t->space || !entry) {
        return 0;       /* 只有用户任务能创建用户线程 */
    }
    /* 没给栈就自己分配一段：每个线程必须有独立栈 */
    if (stack == 0) {
        u64 size = stack_size ? stack_size : FE_USER_STACK_SIZE;
        size = FE_FRAME_ALIGN_UP(size);
        if (t->user_stack_next < size + 0x1000) {
            return 0;
        }
        u64 top = t->user_stack_next;
        t->user_stack_next -= size;
        fe_status_t s = fe_user_map_stack(t->space, t->vmas, top, size, &t->stack_low);
        if (fe_failed(s)) {
            return 0;
        }
        /* ★ 线程入口是"被调用"的，初始 rsp 必须模拟一次 call 的压栈 ★
         *
         * SysV ABI：**函数第一条指令处 rsp % 16 == 8**（call 压了返回地址）。
         * 栈顶是页对齐的，所以减 24（=16+8）既留出余量又满足这个同余式。
         *
         * 这里原本是 `top - 16`（≡ 0 mod 16），也就是把线程入口当成了
         * **进程入口**——进程入口才是 ≡ 0（没有返回地址）。这个错一直存在，
         * 但在用户态不能用 SIMD 时看不出来：只有编译器用对齐的 SSE 搬移
         * （movaps）往栈上溢出时才会 #GP。
         *
         * 我们一打开 SIMD，bench 的 RPC 服务线程就 #GP 了（出错指令在用户代码里、
         * 错误码 0）——查下去才发现是这个 8 字节。**潜伏了一年的 ABI 错误，
         * 被"打开 SIMD"这件事照出来了**：它不会因为不用 SIMD 而不存在，
         * 只是没有症状。 */
        stack = top - 24;
        if ((stack & 0xF) != 8) {
            return 0;       /* 不变量：进不去就明确失败，别让线程带着错栈启动 */
        }
    }
    struct fe_thread *th = fe_thread_create_ring3(t, (u64)(uptr)entry, stack, arg,
                                                  16 * 1024, FE_PRIO_NORMAL, "user");
    /* 用户自己建的线程：内核里**没有人 join 它**（用户态的 join 语义还没有），
     * 所以它的僵尸由调度器回收。不打开这一位，每个退出过的用户线程都会
     * 一直占着线程栈、TLS 块、FPU 区，并让所属任务永不销毁。 */
    if (th) {
        th->reap_ok = true;
    }
    return th ? th->id : 0;
}
