/* SPDX-License-Identifier: 0BSD */
/* FEKernel 内存管理公共定义：物理帧、虚拟地址布局、页表项标志。
 *
 * 地址空间布局（每个进程的内核部分完全相同，切换进程不需要换内核映射）：
 *
 *   0x0000_0000_0000_0000 ┌──────────────────────────────┐
 *                         │ 用户空间（M4 起使用）         │
 *   0x0000_8000_0000_0000 └──────────────────────────────┘
 *   0xffff_8000_0000_0000 ┌──────────────────────────────┐
 *                         │ HHDM：物理内存直接映射        │
 *                         │ （偏移由引导器给出）          │
 *   0xffff_ffff_8000_0000 ├──────────────────────────────┤
 *                         │ 内核映像（-mcmodel=kernel）   │
 *   0xffff_ffff_a000_0000 ├──────────────────────────────┤
 *                         │ 内核堆（按需映射 4KiB 页）    │
 *   0xffff_ffff_c000_0000 ├──────────────────────────────┤
 *                         │ MMIO 窗口（设备寄存器映射）   │
 *   0xffff_ffff_e000_0000 ├──────────────────────────────┤
 *                         │ 内核栈 / 每 CPU 区域          │
 *   0xffff_ffff_ffff_ffff └──────────────────────────────┘
 */
#ifndef FE_MM_H
#define FE_MM_H

#include <fe/types.h>

/* ---------------- 物理帧 ---------------- */

#define FE_FRAME_SHIFT 12
#define FE_FRAME_SIZE  (1ull << FE_FRAME_SHIFT)   /* 4 KiB */
#define FE_FRAME_MASK  (FE_FRAME_SIZE - 1)
#define FE_FRAME_ALIGN_UP(x)   (((x) + FE_FRAME_MASK) & ~FE_FRAME_MASK)
#define FE_FRAME_ALIGN_DOWN(x) ((x) & ~FE_FRAME_MASK)
#define FE_FRAME_INDEX(phys)   ((phys) >> FE_FRAME_SHIFT)

/* 大页 */
#define FE_HUGE_2M_SHIFT 21
#define FE_HUGE_2M_SIZE  (1ull << FE_HUGE_2M_SHIFT)
#define FE_HUGE_1G_SHIFT 30
#define FE_HUGE_1G_SIZE  (1ull << FE_HUGE_1G_SHIFT)

/* ---------------- 内核虚拟地址布局 ---------------- */

#define FE_KERNEL_IMAGE_BASE  0xffffffff80000000ull
#define FE_KERNEL_HEAP_BASE   0xffffffffa0000000ull
#define FE_KERNEL_HEAP_SIZE   (512ull * 1024 * 1024)
#define FE_KERNEL_HEAP_END    (FE_KERNEL_HEAP_BASE + FE_KERNEL_HEAP_SIZE)
/* 用户空间上界（低半区）：内核绝不接受指向这之外的"用户指针" */
#define FE_USER_SPACE_END     0x0000800000000000ull

#define FE_MMIO_BASE          0xffffffffc0000000ull
#define FE_MMIO_SIZE          (256ull * 1024 * 1024)
#define FE_MMIO_END           (FE_MMIO_BASE + FE_MMIO_SIZE)
#define FE_OBJMAP_BASE        0xffffffffd0000000ull
#define FE_OBJMAP_SIZE        (256ull * 1024 * 1024)
#define FE_OBJMAP_END         (FE_OBJMAP_BASE + FE_OBJMAP_SIZE)
#define FE_PERCPU_BASE        0xffffffffe0000000ull
#define FE_PERCPU_SIZE        (256ull * 1024 * 1024)
#define FE_PERCPU_END         (FE_PERCPU_BASE + FE_PERCPU_SIZE)

/* HHDM 覆盖的物理地址上限（自建页表时按此建立直接映射） */
#define FE_HHDM_MAX_PHYS FE_GIB(64)

/* ---------------- 页表项标志 ---------------- */

#define FE_PTE_PRESENT  (1ull << 0)
#define FE_PTE_WRITE    (1ull << 1)
#define FE_PTE_USER     (1ull << 2)
#define FE_PTE_PWT      (1ull << 3)
#define FE_PTE_PCD      (1ull << 4)   /* 关闭缓存：MMIO 必须置位 */
#define FE_PTE_ACCESSED (1ull << 5)
#define FE_PTE_DIRTY    (1ull << 6)
#define FE_PTE_HUGE     (1ull << 7)   /* 在 PD 层表示 2MiB 页，在 PDPT 层表示 1GiB 页 */
#define FE_PTE_GLOBAL   (1ull << 8)
#define FE_PTE_NX       (1ull << 63)
/* ★ 「这一页的帧不归本地址空间所有」★
 *
 * 软件位（bit 9，硬件不用它）。没有这一位就没法正确销毁一个地址空间：
 * 销毁要遍历用户半区、把叶子项背后的帧还给 PMM——但**并不是每个叶子项
 * 背后的帧都是这个地址空间的**：
 *   - 内存对象映射（MEM_MAP）：帧归那个对象所有，可能有别的任务也在映射它；
 *   - MMIO 映射：那根本不是 RAM，是设备寄存器。
 * 一律归还的后果是把借来的内存还进池子：PMM 随后把它分配给别人，
 * 于是两个使用者互相踩——实测症状是"销毁一个刚跑完的程序，另一个
 * 正在跑的进程在自己的代码上取指 #PF"（它的页表帧被回收并重用了）。
 *
 * 这一位让"拥有"这件事**记在页表里**，而不是靠销毁代码去猜。
 * 代价：映射非独占内存的路径都要记得置它（今天两处：MEM_MAP 与 MMIO_MAP），
 * 漏置的症状是"那次销毁多还了别人的帧"——所以两处都写在注释里互相指认。 */
#define FE_PTE_NOFREE   (1ull << 9)
#define FE_PTE_ADDR_MASK 0x000ffffffffff000ull

/* 页表层级索引 */
#define FE_PML4_INDEX(v) ((u32)(((u64)(v) >> 39) & 0x1ff))
#define FE_PDPT_INDEX(v) ((u32)(((u64)(v) >> 30) & 0x1ff))
#define FE_PD_INDEX(v)   ((u32)(((u64)(v) >> 21) & 0x1ff))
#define FE_PT_INDEX(v)   ((u32)(((u64)(v) >> 12) & 0x1ff))

/* ---------------- ELF64（内核自映像解析用，最小子集） ---------------- */

#define FE_ELF_MAGIC 0x464c457fu
#define FE_PT_LOAD   1
#define FE_PF_X      1
#define FE_PF_W      2
#define FE_PF_R      4

/* ---------------- 内存子系统初始化顺序 ---------------- */

void fe_pmm_init(void);    /* 1. 物理帧分配器 */
void fe_vmm_init(void);    /* 2. 自建页表并切换（此后内核跑在自己的页表上） */
void fe_kheap_init(void);  /* 3. 内核堆（slab） */

#endif /* FE_MM_H */
