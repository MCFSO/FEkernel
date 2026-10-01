/* SPDX-License-Identifier: 0BSD */
/* 内核堆：slab 分配器。
 *
 *   - 小对象（≤ 2048 字节）走 8 个尺寸等级的 slab 缓存，每个 slab 占 1 个 4KiB 页；
 *   - 大对象走「虚拟连续、物理离散」的页组分配，头部页记录页数；
 *   - 堆的虚拟地址取自 FE_KERNEL_HEAP_BASE..FE_KERNEL_HEAP_END 这段保留区，
 *     物理帧按需从 PMM 分配（因此 kmalloc 的内存永远是零初始化的）。
 *
 * M1 阶段运行在单核上、且中断上下文中不会调用堆，所以暂不加锁；
 * M2 引入 SMP 后会在这三个入口处加自旋锁。
 */
#ifndef FE_MM_KHEAP_H
#define FE_MM_KHEAP_H

#include <fe/types.h>

void  fe_kheap_init(void);

void *fe_kmalloc(size_t size);
void *fe_kzalloc(size_t size);
void *fe_krealloc(void *ptr, size_t new_size);
void  fe_kfree(void *ptr);

/* 分配并返回物理地址（DMA 缓冲用；返回的内存同时可通过 HHDM 访问） */
void *fe_kmalloc_dma(size_t size, phys_addr_t *out_phys);

struct fe_kheap_stats {
    u64 va_total_pages;     /* 堆虚拟区总页数 */
    u64 va_used_pages;      /* 已占用的虚拟页数（含空闲槽位） */
    u64 va_free_pages;      /* 空闲虚拟页数 */
    u64 mapped_pages;       /* 已映射物理帧的页数 */
    u64 slab_count;         /* 当前 slab 页数 */
    u64 slab_free_objects;  /* slab 中空闲对象数 */
    u64 slab_live_objects;  /* slab 中已分配对象数 */
    u64 large_count;        /* 大对象块数 */
    u64 large_bytes;        /* 大对象占用字节数 */
    u64 total_allocs;
    u64 total_frees;
};

void fe_kheap_get_stats(struct fe_kheap_stats *out);

/* 堆的自检：分配/写入/校验/释放一整轮，返回不通过的项数（0 表示全部通过） */
u32 fe_kheap_selftest(void);

#endif /* FE_MM_KHEAP_H */
