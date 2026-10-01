/* SPDX-License-Identifier: 0BSD */
/* x86_64 底层指令封装：端口 I/O、控制寄存器、MSR、中断开关。
 * 所有内联汇编集中在此，其它内核代码只调用这些函数。
 */
#ifndef FE_IO_H
#define FE_IO_H

#include <fe/types.h>
#include <fe/compiler.h>

/* ---------------- 端口 I/O ---------------- */

FE_INLINE void fe_outb(u16 port, u8 val)
{
    __asm__ volatile("outb %0, %1" ::"a"(val), "Nd"(port));
}

FE_INLINE void fe_outw(u16 port, u16 val)
{
    __asm__ volatile("outw %0, %1" ::"a"(val), "Nd"(port));
}

FE_INLINE void fe_outl(u16 port, u32 val)
{
    __asm__ volatile("outl %0, %1" ::"a"(val), "Nd"(port));
}

FE_INLINE u8 fe_inb(u16 port)
{
    u8 r;
    __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

FE_INLINE u16 fe_inw(u16 port)
{
    u16 r;
    __asm__ volatile("inw %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

FE_INLINE u32 fe_inl(u16 port)
{
    u32 r;
    __asm__ volatile("inl %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

/* 写 0x80 端口制造约 1us 延迟，给慢速老设备（如 8259/8254）留出反应时间 */
FE_INLINE void fe_io_wait(void)
{
    fe_outb(0x80, 0);
}

/* ---------------- CPU 状态 ---------------- */

FE_INLINE void fe_hlt(void)      { __asm__ volatile("hlt"); }
FE_INLINE void fe_cli(void)      { __asm__ volatile("cli" ::: "memory"); }
FE_INLINE void fe_sti(void)      { __asm__ volatile("sti" ::: "memory"); }
FE_INLINE void fe_pause(void)    { __asm__ volatile("pause"); }
FE_INLINE void fe_cld(void)      { __asm__ volatile("cld"); }

/* 关中断并返回先前的 RFLAGS，用于成对恢复 */
FE_INLINE u64 fe_irq_save(void)
{
    u64 flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

FE_INLINE void fe_irq_restore(u64 flags)
{
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

FE_INLINE u64 fe_read_rflags(void)
{
    u64 flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    return flags;
}

/* ---------------- 控制寄存器 ---------------- */

FE_INLINE u64 fe_read_cr0(void) { u64 v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
FE_INLINE u64 fe_read_cr2(void) { u64 v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
FE_INLINE u64 fe_read_cr3(void) { u64 v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
FE_INLINE u64 fe_read_cr4(void) { u64 v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }
FE_INLINE void fe_write_cr0(u64 v) { __asm__ volatile("mov %0, %%cr0" ::"r"(v) : "memory"); }
FE_INLINE void fe_write_cr3(u64 v) { __asm__ volatile("mov %0, %%cr3" ::"r"(v) : "memory"); }
FE_INLINE void fe_write_cr4(u64 v) { __asm__ volatile("mov %0, %%cr4" ::"r"(v) : "memory"); }
FE_INLINE void fe_invlpg(u64 va)   { __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory"); }

FE_INLINE void fe_wbinvd(void) { __asm__ volatile("wbinvd" ::: "memory"); }

/* ---------------- MSR ---------------- */

FE_INLINE void fe_wrmsr(u32 msr, u64 value)
{
    __asm__ volatile("wrmsr" ::"c"(msr), "a"((u32)value), "d"((u32)(value >> 32)));
}

FE_INLINE u64 fe_rdmsr(u32 msr)
{
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

FE_INLINE u64 fe_rdtsc(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

/* ---------------- 常用 MSR 编号 ---------------- */

#define FE_MSR_APIC_BASE     0x0000001Bu
#define FE_MSR_EFER           0xC0000080u
#define FE_MSR_STAR           0xC0000081u
#define FE_MSR_LSTAR          0xC0000082u
#define FE_MSR_CSTAR          0xC0000083u
#define FE_MSR_SFMASK         0xC0000084u
#define FE_MSR_FS_BASE        0xC0000100u
#define FE_MSR_GS_BASE        0xC0000101u
#define FE_MSR_KERNEL_GS_BASE 0xC0000102u
#define FE_MSR_TSC_AUX        0xC0000103u

#define FE_EFER_SCE (1ull << 0)
#define FE_EFER_NXE (1ull << 11)

#endif /* FE_IO_H */
