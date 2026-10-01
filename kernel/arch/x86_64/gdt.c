/* SPDX-License-Identifier: 0BSD */
#include <fe/gdt.h>
#include <fe/string.h>
#include <fe/kprintf.h>
#include <fe/panic.h>

/* 索引: 0 空  1 内核代码  2 内核数据  3 用户数据  4 用户代码  5/6 TSS */
#define GDT_ENTRY_COUNT 7

static struct fe_gdt_entry g_gdt[GDT_ENTRY_COUNT];
static struct fe_gdtr       g_gdtr;

/* TSS 与它的 I/O 位图槽位必须连续存放：iopb_offset 是 16 位，只能指到 TSS 之后
 * 64 KiB 以内。对齐到 64 字节免得描述符跨缓存行。 */
static struct fe_tss_block  g_tss_block FE_ALIGNED(64);
#define g_tss (g_tss_block.tss)

static u8  g_iopb_used[FE_IOPB_SLOTS];
static int g_iopb_current = -2;     /* -2 表示「还没初始化」，避免与 -1 混淆 */

/* 访问权限字节 */
#define ACC_PRESENT  0x80
#define ACC_DPL(n)   ((u8)((n) << 5))
#define ACC_S        0x10   /* 代码/数据段（非系统段） */
#define ACC_EXEC     0x08
#define ACC_RW       0x02
#define ACC_ACCESSED 0x01

/* 标志字节高 4 位 */
#define FLAG_GRAN_4K 0x80   /* 粒度 4KiB */
#define FLAG_LONG    0x20   /* 64 位代码段 */
#define FLAG_DB      0x40   /* 32 位默认操作数（64 位下代码段必须为 0） */

static void set_entry(int i, u32 base, u32 limit, u8 access, u8 flags)
{
    g_gdt[i].limit_low = (u16)(limit & 0xFFFF);
    g_gdt[i].base_low = (u16)(base & 0xFFFF);
    g_gdt[i].base_mid = (u8)((base >> 16) & 0xFF);
    g_gdt[i].access = access;
    g_gdt[i].flags_limit_high = (u8)(((limit >> 16) & 0x0F) | (flags & 0xF0));
    g_gdt[i].base_high = (u8)((base >> 24) & 0xFF);
}

/* 槽位 k 的位图相对 TSS 基址的偏移。 */
static u16 iopb_slot_offset(int slot)
{
    if (slot < 0 || slot >= FE_IOPB_SLOTS) {
        /* 指到 TSS 界限之外：CPU 视为「所有端口都禁止」。
         * 这正是 x86 手册给的用法——偏移超出 TSS 界限即拒绝一切 I/O。 */
        return (u16)sizeof(struct fe_tss_block);
    }
    return (u16)(__builtin_offsetof(struct fe_tss_block, iopb) +
                 (u32)slot * (FE_IOPB_SIZE + 1));
}

int fe_iopb_alloc_slot(void)
{
    for (int i = 0; i < FE_IOPB_SLOTS; i++) {
        if (!g_iopb_used[i]) {
            g_iopb_used[i] = 1;
            /* 新槽位一律「全禁止」，由能力授予时逐位放开 */
            memset(g_tss_block.iopb[i], 0xFF, FE_IOPB_SIZE + 1);
            return i;
        }
    }
    return -1;
}

void fe_iopb_free_slot(int slot)
{
    if (slot < 0 || slot >= FE_IOPB_SLOTS) {
        return;
    }
    /* 归还前先清干净：否则下一个拿到该槽位的任务会继承上一个任务的端口权限 */
    memset(g_tss_block.iopb[slot], 0xFF, FE_IOPB_SIZE + 1);
    g_iopb_used[slot] = 0;
}

void fe_iopb_allow_port(int slot, u16 port, bool allow)
{
    if (slot < 0 || slot >= FE_IOPB_SLOTS) {
        return;
    }
    u16 byte_index = (u16)(port >> 3);
    u8 bit = (u8)(port & 7);
    if (byte_index >= FE_IOPB_SIZE) {
        return;
    }
    if (allow) {
        g_tss_block.iopb[slot][byte_index] &= (u8)~(1u << bit);  /* 0 = 允许 */
    } else {
        g_tss_block.iopb[slot][byte_index] |= (u8)(1u << bit);
    }
}

void fe_iopb_allow_range(int slot, u32 base, u32 count, bool allow)
{
    for (u32 p = base; p < base + count && p <= 0xFFFF; p++) {
        fe_iopb_allow_port(slot, (u16)p, allow);
    }
}

void fe_tss_set_iopb_slot(int slot)
{
    if (slot == g_iopb_current) {
        return;         /* 切到同一个槽位就别写了，省一次内存写 */
    }
    g_tss.iopb_offset = iopb_slot_offset(slot);
    g_iopb_current = slot;
}

void fe_tss_set_rsp0(u64 rsp0)
{
    g_tss.rsp0 = rsp0;
}

void fe_tss_set_ist(int index, u64 top)
{
    if (index >= 1 && index <= 7) {
        g_tss.ist[index - 1] = top;
    }
}

void fe_gdt_init(void)
{
    memset(g_gdt, 0, sizeof(g_gdt));
    memset(&g_tss_block, 0, sizeof(g_tss_block));

    /* 0x00 空描述符（硬件要求第 0 项恒为空） */
    set_entry(0, 0, 0, 0, 0);

    /* 0x08 内核代码：DPL0，可执行可读，64 位，4KiB 粒度 */
    set_entry(1, 0, 0xFFFFF, ACC_PRESENT | ACC_S | ACC_EXEC | ACC_RW,
              FLAG_GRAN_4K | FLAG_LONG);

    /* 0x10 内核数据：DPL0，可读写 */
    set_entry(2, 0, 0xFFFFF, ACC_PRESENT | ACC_S | ACC_RW,
              FLAG_GRAN_4K | FLAG_DB);

    /* 0x18 用户数据：DPL3（必须排在用户代码之前，见头文件说明） */
    set_entry(3, 0, 0xFFFFF, ACC_PRESENT | ACC_DPL(3) | ACC_S | ACC_RW,
              FLAG_GRAN_4K | FLAG_DB);

    /* 0x20 用户代码：DPL3，64 位 */
    set_entry(4, 0, 0xFFFFF, ACC_PRESENT | ACC_DPL(3) | ACC_S | ACC_EXEC | ACC_RW,
              FLAG_GRAN_4K | FLAG_LONG);

    /* 0x28 TSS：系统段，DPL0，type=0x9（64 位可用 TSS）。
     * 界限必须覆盖整个块（含位图槽位），否则 CPU 读 I/O 位图时会当成越界。 */
    u64 tss_base = (u64)(uptr)&g_tss_block;
    u32 tss_limit = (u32)(sizeof(struct fe_tss_block) - 1);
    set_entry(5, (u32)tss_base, tss_limit, ACC_PRESENT | 0x09, 0x00);
    /* 64 位 TSS 描述符占 16 字节：高 8 字节存放基址的高 32 位 */
    g_gdt[6].limit_low = (u16)((tss_base >> 32) & 0xFFFF);
    g_gdt[6].base_low = (u16)((tss_base >> 48) & 0xFFFF);
    g_gdt[6].base_mid = 0;
    g_gdt[6].access = 0;
    g_gdt[6].flags_limit_high = 0;
    g_gdt[6].base_high = 0;

    /* 所有 I/O 位图槽位默认全禁止；哨兵字节恒为 0xFF
     * （CPU 可能按 2 字节粒度读位图的最后一个字节之后的那个字节） */
    for (int i = 0; i < FE_IOPB_SLOTS; i++) {
        memset(g_tss_block.iopb[i], 0xFF, FE_IOPB_SIZE + 1);
    }
    memset(g_iopb_used, 0, sizeof(g_iopb_used));
    g_iopb_current = -2;
    fe_tss_set_iopb_slot(-1);       /* 默认：谁都不许碰端口 */
    g_tss.rsp0 = 0;

    g_gdtr.limit = (u16)(sizeof(g_gdt) - 1);
    g_gdtr.base = (u64)(uptr)&g_gdt;

    fe_gdt_load(&g_gdtr);
    fe_tss_load();

    /* 位图必须落在 TSS 基址之后 64 KiB 以内（iopb_offset 只有 16 位） */
    FE_STATIC_ASSERT(sizeof(struct fe_tss_block) <= 65536,
                     "TSS 块必须能放进 16 位偏移");
    FE_STATIC_ASSERT(__builtin_offsetof(struct fe_tss_block, iopb) +
                     (FE_IOPB_SLOTS - 1) * (FE_IOPB_SIZE + 1) < 65536,
                     "最后一个 I/O 位图槽位偏移必须能放进 16 位");
}
