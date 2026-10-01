/* SPDX-License-Identifier: 0BSD */
/* 全局描述符表 + 任务状态段（TSS）。
 *
 * 段选择子的排列顺序是**刻意**的，必须与 syscall/sysret 的硬件约定一致：
 *   SYSCALL:  CS = STAR[47:32]      , SS = CS + 8
 *   SYSRET :  CS = STAR[63:48] + 16 , SS = STAR[63:48] + 8
 * 因此要求「用户数据段」紧挨在「用户代码段」之前：
 *   0x08 内核代码  0x10 内核数据  0x18 用户数据  0x20 用户代码  0x28 TSS
 * 这样 STAR = (0x10 << 48) | (0x08 << 32) 即可让 SYSCALL/SYSRET 正常工作。
 */
#ifndef FE_GDT_H
#define FE_GDT_H

#include <fe/types.h>
#include <fe/compiler.h>

#define FE_SEL_KCODE 0x08
#define FE_SEL_KDATA 0x10
#define FE_SEL_UDATA 0x18
#define FE_SEL_UCODE 0x20
#define FE_SEL_TSS   0x28

/* TSS 的 I/O 权限位图容量：65536 个端口 = 8192 字节。
 * 位为 1 表示「禁止访问」，因此默认全 1；驱动服务申请端口后由内核清位。
 *
 * 为什么需要多个槽位：端口权限是**每任务**的能力。TSS 里的 iopb_offset 是相对
 * TSS 基址的 16 位偏移，所以所有位图必须紧跟 TSS 之后（64 KiB 以内）。这里预置
 * 若干槽位，任务创建时分配一个，任务切换时只改 iopb_offset —— 不必每次切换都
 * 拷贝 8 KiB，也不会让一次切换带上几千个周期的开销。 */
#define FE_IOPB_SIZE  8192
#define FE_IOPB_SLOTS 6

struct fe_tss {
    u32 reserved0;
    u64 rsp0;           /* ring0 栈：从 ring3 进入内核时使用 */
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist[7];         /* 中断栈表：double fault / NMI / MCE 等使用独立栈 */
    u64 reserved2;
    u16 reserved3;
    u16 iopb_offset;    /* I/O 权限位图相对 TSS 基址的偏移 */
} FE_PACKED;

/* TSS 与它的位图槽位必须连续存放（见上面的说明），
 * 且 GDT 里 TSS 描述符的界限要覆盖整个块，否则 CPU 读不到位图。 */
struct fe_tss_block {
    struct fe_tss tss;
    u8 iopb[FE_IOPB_SLOTS][FE_IOPB_SIZE + 1];   /* 末尾一个哨兵字节，恒为 0xFF */
} FE_PACKED;

struct fe_gdt_entry {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  flags_limit_high;
    u8  base_high;
} FE_PACKED;

struct fe_gdtr {
    u16 limit;
    u64 base;
} FE_PACKED;

void fe_gdt_init(void);

/* 设置 ring0 栈顶（每次线程切换时更新） */
void fe_tss_set_rsp0(u64 rsp0);

/* 设置 IST 栈顶，index 取 1..7 */
void fe_tss_set_ist(int index, u64 top);

/* ---- 每任务的 I/O 权限位图 ----
 * 槽位是全局资源，任务销毁时必须归还。slot < 0 表示「全部端口禁止」，
 * 实现方式是把 iopb_offset 指到 TSS 界限之外。 */
int  fe_iopb_alloc_slot(void);
void fe_iopb_free_slot(int slot);
void fe_iopb_allow_port(int slot, u16 port, bool allow);
void fe_iopb_allow_range(int slot, u32 base, u32 count, bool allow);
/* 切换当前生效的位图槽位，线程切换时调用 */
void fe_tss_set_iopb_slot(int slot);

/* 由 gdt_flush.asm 实现 */
void fe_gdt_load(const struct fe_gdtr *gdtr);
void fe_tss_load(void);

#endif /* FE_GDT_H */
