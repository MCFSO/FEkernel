/* SPDX-License-Identifier: 0BSD */
/* 物理内存管理器（PMM）：基于位图的 4KiB 帧分配器。
 *
 * 设计要点：
 *   - 位图本身不占静态内存，而是紧跟在**内核映像末尾**的物理内存之后（由 PMM 自己定位），
 *     因此可以支持任意大小的物理内存，没有写死的上限。
 *   - 初始时把所有帧标记为「已占用」，再把引导器报告为可用的区间标记为空闲，
 *     这样保留区、ACPI 区、帧缓冲、内核与模块区天然就是不可分配的。
 *   - 额外强制保留：物理地址最低 1 MiB（BIOS/IVT/VGA 遗留区）与位图自身所占的帧。
 */
#ifndef FE_MM_PMM_H
#define FE_MM_PMM_H

#include <fe/types.h>
#include <fe/status.h>
#include <fe/mm.h>

struct fe_pmm_stats {
    u64 total_frames;    /* 位图覆盖的总帧数 */
    u64 usable_frames;   /* 引导器报告为可用的帧数 */
    u64 reserved_frames; /* 其中被内核强制保留的帧数 */
    u64 free_frames;     /* 当前空闲 */
    u64 allocated_frames;/* 当前已分配 */
    u64 bitmap_frames;   /* 位图自身占用的帧数（含 1 MiB 以下保留区） */
    phys_addr_t bitmap_phys;
    u64 bitmap_bytes;
};

void fe_pmm_init(void);

/* 分配/释放单个物理帧。失败返回 0（物理地址 0 永不作为有效帧返回）。 */
phys_addr_t fe_pmm_alloc_frame(void);
void        fe_pmm_free_frame(phys_addr_t frame);

/* 分配/释放连续物理帧（DMA 用；M1 用朴素的首次适配）。 */
phys_addr_t fe_pmm_alloc_frames(u64 count);
void        fe_pmm_free_frames(phys_addr_t base, u64 count);

/* 标记某段物理内存为已占用（用于保留内核数据结构、引导器遗留区等） */
void fe_pmm_reserve(phys_addr_t base, u64 size);

/* 这段物理区间是否与引导器报告的**可用内存**相交。
 * 用来挡住「把普通内存当 MMIO 映射给驱动」：那样驱动就能绕过页表保护
 * 直接改内核数据结构，比任何权限检查都致命。 */
bool fe_pmm_is_usable(phys_addr_t base, u64 size);

/* 可用内存区间数（诊断用） */
u32 fe_pmm_usable_region_count(void);

void fe_pmm_get_stats(struct fe_pmm_stats *out);
u64  fe_pmm_free_frame_count(void);

/* 位图占用的字节数（诊断用） */
u64 fe_pmm_bitmap_bytes(void);

#endif /* FE_MM_PMM_H */
