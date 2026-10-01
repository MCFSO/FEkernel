/* SPDX-License-Identifier: 0BSD */
#include <fe/idt.h>
#include <fe/gdt.h>
#include <fe/io.h>
#include <fe/kprintf.h>
#include <fe/panic.h>
#include <fe/string.h>
#include <fe/sched/thread.h>
#include <fe/task.h>            /* 缺页解析需要当前任务与它的区间表 */
#include <fe/process.h>         /* K5：fe_fault_deliver 与 FE_FAULT_* */
#include <fe/user.h>
#include <fe/mm/vma.h>

#define IDT_ENTRY_COUNT 256

/* 门类型 */
#define GATE_INTERRUPT 0x8E   /* P=1, DPL=0, 64 位中断门 */
#define GATE_TRAP      0x8F
#define GATE_USER      0xEE   /* P=1, DPL=3，供用户态 int 指令使用 */

struct fe_idt_entry {
    u16 offset_low;
    u16 selector;
    u8  ist;        /* 低 3 位为 IST 索引 */
    u8  type_attr;
    u16 offset_mid;
    u32 offset_high;
    u32 zero;
} FE_PACKED;

struct fe_idtr {
    u16 limit;
    u64 base;
} FE_PACKED;

static struct fe_idt_entry g_idt[IDT_ENTRY_COUNT];
static struct fe_idtr      g_idtr;
static fe_isr_handler_t    g_handlers[IDT_ENTRY_COUNT];

/* 异常使用的独立栈（double fault / NMI / machine check）。
 * 这三种异常可能在栈已经损坏时发生，必须用独立栈才能打印出诊断信息。 */
#define IST_STACK_SIZE 8192
static u8 g_ist_stacks[3][IST_STACK_SIZE] FE_ALIGNED(16);

extern void *fe_isr_stub_table[IDT_ENTRY_COUNT];

const char *fe_exception_name(u64 vector)
{
    static const char *const names[32] = {
        "除零错误 (#DE)",            "调试异常 (#DB)",
        "不可屏蔽中断 (NMI)",        "断点 (#BP)",
        "溢出 (#OF)",                "越界 (#BR)",
        "非法操作码 (#UD)",          "设备不可用 (#NM)",
        "双重错误 (#DF)",            "协处理器段越界",
        "非法 TSS (#TS)",            "段不存在 (#NP)",
        "栈段错误 (#SS)",            "通用保护错误 (#GP)",
        "页错误 (#PF)",              "保留",
        "x87 浮点错误 (#MF)",        "对齐检查 (#AC)",
        "机器检查 (#MC)",            "SIMD 浮点错误 (#XM)",
        "虚拟化异常 (#VE)",          "控制流保护 (#CP)",
        "保留", "保留", "保留", "保留", "保留", "保留", "保留",
        "超管调用 (#VC)",            "安全异常 (#SX)",
        "保留",
    };
    if (vector < 32) {
        return names[vector];
    }
    return "未知异常";
}

void fe_idt_set_handler(u8 vector, fe_isr_handler_t handler)
{
    g_handlers[vector] = handler;
}

void fe_idt_init(void)
{
    memset(g_idt, 0, sizeof(g_idt));
    memset(g_handlers, 0, sizeof(g_handlers));

    for (int i = 0; i < IDT_ENTRY_COUNT; i++) {
        u64 addr = (u64)(uptr)fe_isr_stub_table[i];
        g_idt[i].offset_low = (u16)(addr & 0xFFFF);
        g_idt[i].selector = FE_SEL_KCODE;
        g_idt[i].ist = 0;
        g_idt[i].type_attr = GATE_INTERRUPT;
        g_idt[i].offset_mid = (u16)((addr >> 16) & 0xFFFF);
        g_idt[i].offset_high = (u32)((addr >> 32) & 0xFFFFFFFF);
        g_idt[i].zero = 0;
    }

    /* 给致命异常分配 IST 栈 */
    fe_tss_set_ist(1, (u64)(uptr)&g_ist_stacks[0][IST_STACK_SIZE]);
    fe_tss_set_ist(2, (u64)(uptr)&g_ist_stacks[1][IST_STACK_SIZE]);
    fe_tss_set_ist(3, (u64)(uptr)&g_ist_stacks[2][IST_STACK_SIZE]);
    g_idt[2].ist = 2;   /* NMI */
    g_idt[8].ist = 1;   /* #DF */
    g_idt[18].ist = 3;  /* #MC */

    g_idtr.limit = (u16)(sizeof(g_idt) - 1);
    g_idtr.base = (u64)(uptr)&g_idt;

    __asm__ volatile("lidt %0" ::"m"(g_idtr));
}

void fe_interrupts_enable(void)
{
    fe_sti();
}

void fe_interrupts_disable(void)
{
    fe_cli();
}

/* ------------------------------------------------------------------ */
/* 异常处理                                                            */
/* ------------------------------------------------------------------ */

static void dump_regs(const struct fe_regs *r)
{
    fe_kprintf("  RAX=%016llx RBX=%016llx RCX=%016llx RDX=%016llx\n",
               (unsigned long long)r->rax, (unsigned long long)r->rbx,
               (unsigned long long)r->rcx, (unsigned long long)r->rdx);
    fe_kprintf("  RSI=%016llx RDI=%016llx RBP=%016llx RSP=%016llx\n",
               (unsigned long long)r->rsi, (unsigned long long)r->rdi,
               (unsigned long long)r->rbp, (unsigned long long)r->rsp);
    fe_kprintf("  R8 =%016llx R9 =%016llx R10=%016llx R11=%016llx\n",
               (unsigned long long)r->r8, (unsigned long long)r->r9,
               (unsigned long long)r->r10, (unsigned long long)r->r11);
    fe_kprintf("  R12=%016llx R13=%016llx R14=%016llx R15=%016llx\n",
               (unsigned long long)r->r12, (unsigned long long)r->r13,
               (unsigned long long)r->r14, (unsigned long long)r->r15);
    fe_kprintf("  RIP=%016llx CS =%04llx RFLAGS=%016llx SS=%04llx\n",
               (unsigned long long)r->rip, (unsigned long long)(r->cs & 0xFFFF),
               (unsigned long long)r->rflags, (unsigned long long)(r->ss & 0xFFFF));
}

/* 异常处理器：目前策略是「打印尽可能多的信息后停机」。
 * 内核进入稳定期后，页错误会被内存管理器接管（按需分页/写时复制）。 */
static FE_NORETURN void exception_fatal(struct fe_regs *r)
{
    fe_kprintf("\n");
    fe_kprintf("================ 内核异常 ================\n");
    fe_kprintf("异常: %s (向量 %llu, 错误码 %#llx)\n",
               fe_exception_name(r->vector), (unsigned long long)r->vector,
               (unsigned long long)r->error);
    if (r->vector == 14) {
        fe_kprintf("CR2 (出错地址) = %#llx\n", (unsigned long long)fe_read_cr2());
        fe_kprintf("  判定: %s, %s, %s\n",
                   (r->error & 1) ? "保护违例" : "页不存在",
                   (r->error & 2) ? "写操作" : "读操作",
                   (r->error & 4) ? "用户态" : "内核态");
        /* ★ 把栈顶附近的内存打出来之前，必须先确认**那内存可读** ★
         *
         * 这一段的代价是实测出来的：用户在 ring 3 栈越界时，`r->rsp` 就是
         * 那个越界的**用户**指针，而这里直接 `sp[i]` 去解引用 ——
         * 内核态访问未映射的用户地址会再触发一次 #PF，于是又回到本函数，
         * 递归到栈耗尽，最后打印出"发生不可恢复的内核异常"。
         *
         * 也就是说：**一个用户程序的栈溢出，能把整个内核带崩**。
         * 这正是 `docs/08` 那条"异常处理必须是终点的路径，不能自己再出异常"
         * 的实例——诊断代码也必须遵守同一条纪律。
         *
         * 修法：只在内核栈的地址范围内才 dump（内核栈必然已映射），
         * 否则明确打印"不可读"——**不尝试**、也不假装读到了。 */
        u64 sp_addr = r->rsp;
        if (sp_addr >= FE_KERNEL_BASE) {
            const u64 *sp = (const u64 *)(uptr)(sp_addr & ~0xFULL);
            fe_kprintf("  栈顶附近内存 (%p):\n", (const void *)sp);
            for (int i = -4; i < 10; i++) {
                fe_kprintf("    [%+5d] %p = %#llx\n", i * 8,
                           (const void *)(uptr)(sp + i),
                           (unsigned long long)sp[i]);
            }
        } else {
            fe_kprintf("  栈指针 %#llx 在用户空间——**不可读**（读它就是内核态 #PF），"
                       "跳过内存 dump\n", (unsigned long long)sp_addr);
        }
    }
    if (r->vector == 13) {
        fe_kprintf("提示: #GP 常见原因——非规范地址、特权指令、段选择子非法\n");
        /* 若是 iretq 触发的 #GP，错误码就是那个非法的段选择子 */
        if (r->error) {
            fe_kprintf("  非法选择子 = %#llx (索引 %llu, %s, RPL %llu)\n",
                       (unsigned long long)r->error,
                       (unsigned long long)(r->error >> 3),
                       (r->error & 4) ? "LDT" : "GDT",
                       (unsigned long long)(r->error & 3));
        }
        const u64 *sp = (const u64 *)(uptr)(r->rsp & ~0xFULL);
        fe_kprintf("  故障时栈附近内存 (%p):\n", (const void *)sp);
        for (int i = 0; i < 8; i++) {   /* 只取少量：走太远会越出内核栈触发二次异常 */
            fe_kprintf("    [%+5d] %p = %#llx\n", i * 8, (const void *)(uptr)(sp + i),
                       (unsigned long long)sp[i]);
        }
    }
    dump_regs(r);
    fe_kprintf("==========================================\n");
    fe_panic_halt("发生不可恢复的内核异常");
}

/* 来自 ring 3 的异常。
 *
 * ★ `#PF` 有两条出路，其余异常只有一条 ★
 *
 * 缺页不一定是错误：按需分页与栈增长都会以缺页的形式出现。所以这里先问
 * `fe_user_resolve_fault`："这是不是某人合法地碰了一块还没给页的地址？"
 *   - 是 → **直接 iretq 回原现场**，被中断的那条指令重新执行，这次能过；
 *   - 不是 → 走"杀线程"路径，与其它异常一样。
 *
 * 为什么必须"回来继续跑"而不是"杀线程后再来一次"：那条指令的状态
 * （寄存器、栈）都在异常帧里，回到原现场是最廉价的恢复方式——
 * 而且这正是硬件设计异常帧的用意。
 *
 * 为什么其余异常不能有第二条出路：`#GP`/`#UD`/除零是**指令本身**非法，
 * 与地址空间状态无关，重试一万次还是同样结果。给它们"修复后重试"的
 * 余地，就等于把一个明确的错误变成死循环。
 *
 * 最近一次用户异常的向量会被记下来：自检程序要靠它区分
 * 「被 #GP 挡下来了」和「因为别的原因碰巧也返回 -1」。 */
static u64 g_last_user_fault_vector = ~0ull;
static u64 g_user_fault_count;
static u64 g_user_fault_by_vec[32]; /* 按向量分别计数（见 fe_user_fault_count_of） */
static u64 g_pf_resolved;           /* 被按需分页消化掉的缺页次数（诊断） */
static u64 g_fault_handled;         /* 被用户态处理者消化掉的次数（K5） */

u64 fe_last_user_fault_vector(void)
{
    return g_last_user_fault_vector;
}

u64 fe_user_fault_count(void)
{
    return g_user_fault_count;
}

/* ★ 按向量分别计数（2c 加的）★
 *
 * ★ 为什么"最近一次向量"不够用了 ★
 * `fe_selftest_user` 原来只看 `fe_last_user_fault_vector()`，因为那时
 * 用户态**只该**有 drvdeny 那一次 #GP。2c 之后不是了：exectest 的
 * mprotect 探针**故意**让一个孩子写只读页，那是一次**预期之内**的 #PF。
 * 于是"最近一次"变成了 14，#GP 那条断言就假失败了——而它测的性质
 * （越权的 in 被 CPU 拒成 #GP）完全没变。
 *
 * 判据应该是"**发生过**一次 #GP"，不是"最后一次是 #GP"：
 * 后者把断言押在"异常发生的顺序"上，而顺序不是被验的性质。 */
u64 fe_user_fault_count_of(u32 vector)
{
    return (vector < 32u) ? g_user_fault_by_vec[vector] : 0;
}

u64 fe_pf_resolved_count(void)
{
    return g_pf_resolved;
}

/* ★ 被处理者**消化掉**的用户态异常次数（K5）★
 *
 * ★ 为什么它必须与 `fe_user_fault_count` 分开（docs/18 §6.1.1）★
 * K5 之前"用户态异常次数"只有一个含义。K5 之后被处理者接管的那些异常
 * 在出口③就 `return` 了，**走不到**出口④的那两个计数器——于是
 * `fe_user_fault_count()` 的含义悄悄变成了"**没被消化**的用户态异常次数"。
 *
 * 它的唯一消费者是 `fe_selftest_user`（判"drvdeny 那次 #GP 被看见了"），
 * 而那个判据依赖的是"全部"。**"看起来还通过"正是最坏的一种绿**：
 * 判据依赖的语义已经变了，但它仍然通过，因为恰好没有受害者
 * （drvdeny 不登记处理者）。
 *
 * 所以处置是：把旧接口的含义**明确定义成"全部"**（出口③也记一次），
 * 再**新增**这一个"被消化的"计数，而不是把旧接口改成"未消化"。 */
u64 fe_user_fault_handled_count(void)
{
    return g_fault_handled;
}

/* 注意：**不是** FE_NORETURN —— 缺页可能被按需分页消化掉，
 * 那时它正常返回，异常路径会 iretq 回原现场继续跑。
 *
 * ★ K5 之后这里有**四条**出口（docs/18-user-fault-handler.md §2.0）★
 *   ① 闸门判据成立 → 杀（**排在最前**，§2.7）
 *   ② 按需分页消化 → return（今天的路径，一个字没动）
 *   ③ 投递给处理者并回了 RESUME → 回填现场并 return
 *   ④ 其余一切 → 杀
 *
 * ★ 出口从两条变成四条是这一整套改动里**结构上最贵**的一处 ★
 * 因为它是异常分发里唯一一个多分支的地方，而这个文件里已经有过一次
 * 实测事故：§下方 `fe_isr_dispatch` 那段注释记着"漏一个 `return` 就把
 * 一次成功的缺页解析当成致命错误 → panic"。所以：**每一条 return 都要有
 * 一条自检堵它**（F2 堵 ①、F3 堵 ②、F1 堵 ③、F4/F5/F8 堵 ④）。 */
static void user_fault(struct fe_regs *r)
{
    const char *tname = "?";
    struct fe_thread *t = fe_thread_current();
    if (t) {
        tname = t->name;
    }
    /* ★ CR2 只有 #PF(14) 才会被 CPU 写 ★
     *
     * `fe_fault_regs.cr2` 的 ABI 是"出错线性地址；**只有 #PF 有意义，
     * 其余为 0**"（kernel/include/fe/regs.h 与 user/include/fe_user.h
     * 两边都这么写）。原先这里无条件读 CR2，于是 #UD/#DE 报给处理者的
     * 是**上一次缺页留下的地址**——一个陈旧值。
     *
     * ★ 这条不是推理出来的，是实测出来的 ★ `user/bin/faulttest` 的
     * `--ud`/`--de` 两个模式把 cr2 打出来，第一次跑就是
     * `cr2=0x0000000051000000`（正是同一个进程里 --segv 那个越界地址）。
     * 危害在于它**误导处理者**：处理者按 ABI 用"cr2 是否为 0"区分
     * "这是不是缺页"，而它拿到的是别人的地址。
     *
     * 所以这里按向量取值：只有 #PF 读 CR2，其余一律给 0。 */
    u64 cr2 = (r->vector == 14u) ? fe_read_cr2() : 0;

    /* ★★ 出口①：闸门判据在**最前面**（§2.7）★★
     *
     * 一个正在被终止的线程没必要再补页、也没必要再问处理者——补了它也回不去：
     * 闸门（`fe_sched_maybe_switch`）会在它下一次回用户态时把它杀掉。
     * 先判它有两个好处：不浪费一次投递往返，也不给处理者一个"我救活了它"
     * 的错觉。
     *
     * ★ 这一句的顺序需要一条负向对照来证明 ★ 见自检 F2：
     * `task->dying` 时端点里必须**一条消息都没有**，而同一时刻另一个
     * 没被终止的任务用同一个现场投递必须有消息。 */
    if (t && fe_thread_should_die(t)) {
        g_last_user_fault_vector = r->vector;
        g_user_fault_count++;
        if (r->vector < 32u) {
            g_user_fault_by_vec[r->vector]++;
        }
        fe_thread_exit(-1);         /* 不返回 */
    }

    /* ★★ 出口②：缺页的第一条出路：按需分页 / 栈增长 ★★
     * 判定逻辑全部在 fe_user_resolve_fault 里（它与 VMA 表在一起，
     * 因为"这块地址该不该有映射"是区间表的问题，不是异常处理器的问题）。
     *
     * ★ 为什么它必须排在投递**前面**（§2.5.1）★
     * 栈增长是内核的合法职责：先问处理者的话，**每一个** C 程序在栈长出
     * 预映射的那几页之后都要自己接管缺页，否则就是崩溃——那不是"更灵活"，
     * 是把内核该做的事推给每个程序。而且用户态**补不了页**：项目里没有
     * "按 VMA 的权限给我一页"这个原语（MEM_MAP 映射的是对象已有的帧）。
     * 两个判据不重叠（`fe_user_resolve_fault` 只承认三种情况，其余一律
     * false），所以"按需分页在前"不会吃掉处理者该看见的异常。 */
    if (r->vector == 14) {
        struct fe_task *task = fe_task_current();
        if (fe_user_resolve_fault(task, cr2, r->error)) {
            g_pf_resolved++;
            return;         /* 回到原现场，那条指令会重新执行并成功 */
        }
    }

    /* ★★ 出口③：问处理者（K5 新加的第三个选项）★★
     *
     * ★ 判据 `(r->cs & 3) == 3` 在调用点已经成立 ★
     * `fe_isr_dispatch` 只在"来自 ring 3 且 vector < 32"时才调到这里，
     * 所以这一段天然拿不到内核态的异常。**这一条是安全边界，不是可以
     * 顺手放宽的地方**：内核态异常的现场里 `cs` 是 ring 0，处理者改
     * `rip`/`rsp` 之后 iretq 就是任意内核代码执行——不需要"能改 cs"，
     * 把 `cs` 原样弹回去就够了（docs/18 §3.1）。 */
    {
        struct fe_fault_regs fres;
        memset(&fres, 0, sizeof(fres));
        if (fe_ok(fe_fault_deliver(t, r, cr2, &fres))) {
            if (fe_fault_apply_regs(r, &fres)) {
                /* ★ 记账：这两个计数器记的是"**全部**用户态异常"★
                 * 出口③也要记一笔，否则它们的语义会悄悄从"全部"变成
                 * "没被消化的"——而它们唯一的消费者依赖"全部"
                 * （docs/18 §6.1.1）。被消化的那部分另有一个计数。 */
                g_last_user_fault_vector = r->vector;
                g_user_fault_count++;
                if (r->vector < 32u) {
                    g_user_fault_by_vec[r->vector]++;
                }
                g_fault_handled++;
                return;
            }
            /* 现场非法（改了 cs/ss）：`fault_apply_regs` 已经点过名，照旧杀。 */
        }
    }

    /* ★★ 出口④：照旧杀（今天的行为，一个字没改）★★ */
    g_last_user_fault_vector = r->vector;
    g_user_fault_count++;
    if (r->vector < 32u) {
        g_user_fault_by_vec[r->vector]++;
    }
    fe_kprintf("\n[用户态异常] 线程 %s 触发 %s (向量 %llu, 错误码 %#llx)\n",
               tname, fe_exception_name(r->vector), (unsigned long long)r->vector,
               (unsigned long long)r->error);
    if (r->vector == 14) {
        fe_kprintf("             出错地址 %#llx (%s, %s)，不在任何可增长区间内\n",
                   (unsigned long long)cr2,
                   (r->error & 2) ? "写" : "读",
                   (r->error & 1) ? "保护违例" : "页不存在");
        /* 把区间表打出来：缺页是**唯一**能看到"用户地址空间长什么样"的
         * 时刻，而这张表正是判断"为什么这次缺页没能被消化"的依据。 */
        struct fe_task *task = fe_task_current();
        if (task && task->vmas) {
            fe_vma_dump(task->vmas, task->name);
        }
    }
    fe_kprintf("             出错指令 %#llx —— 该线程被终止，系统继续运行\n",
               (unsigned long long)r->rip);
    fe_thread_exit(-1);     /* 不返回 */
}

void fe_isr_dispatch(struct fe_regs *r)
{
    u64 vector = r->vector;

    /* 来自用户态的异常一律走「用户态异常」路径，而不是内核 panic。
     *
     * ★★ 这里必须有 return，而且理由值得写清楚 ★★
     *
     * `user_fault` 现在有**两种正常结局**：
     *   1. 缺页被按需分页消化 → **正常返回**，调用方必须直接返回，
     *      让异常路径 iretq 回原现场继续跑；
     *   2. 无法消化 → 它自己调 `fe_thread_exit`，永不返回。
     *
     * 第一版漏了这个 return（当时 `user_fault` 是 FE_NORETURN，
     * 不需要它）。加了按需分页之后，情形 1 就**落到下面的
     * `exception_fatal`** —— 于是"一次成功的缺页解析"被当成
     * 不可恢复的内核异常，系统直接 panic。
     *
     * 症状极具误导性：panic 里打印的现场是**用户态**的
     * （CS=0023、RIP 在用户代码里、CR2 是刚被补上的那一页），
     * 看起来像"用户程序把内核弄崩了"，而真相是内核自己把
     * 一次成功当成了致命错误。 */
    if ((r->cs & 3) == 3 && vector < 32) {
        user_fault(r);
        return;
    }

    if (vector < 32) {
        if (g_handlers[vector]) {
            g_handlers[vector](r);
            return;
        }
        exception_fatal(r);
    }

    fe_isr_handler_t h = (vector < IDT_ENTRY_COUNT) ? g_handlers[vector] : NULL;
    if (h) {
        h(r);
    }
}
