/* SPDX-License-Identifier: 0BSD */
#include <fe/idt.h>
#include <fe/gdt.h>
#include <fe/io.h>
#include <fe/kprintf.h>
#include <fe/panic.h>
#include <fe/string.h>
#include <fe/sched/thread.h>
#include <fe/task.h>            /* 缺页解析需要当前任务与它的区间表 */
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
static u64 g_pf_resolved;           /* 被按需分页消化掉的缺页次数（诊断） */

u64 fe_last_user_fault_vector(void)
{
    return g_last_user_fault_vector;
}

u64 fe_user_fault_count(void)
{
    return g_user_fault_count;
}

u64 fe_pf_resolved_count(void)
{
    return g_pf_resolved;
}

/* 注意：**不是** FE_NORETURN —— 缺页可能被按需分页消化掉，
 * 那时它正常返回，异常路径会 iretq 回原现场继续跑。 */
static void user_fault(struct fe_regs *r)
{
    const char *tname = "?";
    struct fe_thread *t = fe_thread_current();
    if (t) {
        tname = t->name;
    }
    u64 cr2 = fe_read_cr2();

    /* ★ 缺页的第一条出路：按需分页 / 栈增长 ★
     * 判定逻辑全部在 fe_user_resolve_fault 里（它与 VMA 表在一起，
     * 因为"这块地址该不该有映射"是区间表的问题，不是异常处理器的问题）。 */
    if (r->vector == 14) {
        struct fe_task *task = fe_task_current();
        if (fe_user_resolve_fault(task, cr2, r->error)) {
            g_pf_resolved++;
            return;         /* 回到原现场，那条指令会重新执行并成功 */
        }
    }

    g_last_user_fault_vector = r->vector;
    g_user_fault_count++;
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
