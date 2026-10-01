/* SPDX-License-Identifier: 0BSD */
/* 虚拟内存管理器（VMM）：4 级页表管理。
 *
 * 设计要点：
 *   - 所有页表帧都从 PMM 分配，通过 HHDM 访问其内容（物理帧永远有直接映射）。
 *   - 内核地址空间（HHDM + 内核映像 + MMIO + 堆）在所有进程空间中的映射完全一致，
 *     因此进程切换只需要换 CR3，内核代码与数据始终可访问。
 *   - 建立自有页表后立刻切换 CR3，从此不再依赖引导器的页表。
 *   - 内核映像按 ELF 程序头逐段映射，并据此设置 W/NX 权限，实现内核自身的 W^X。
 */
#ifndef FE_MM_VMM_H
#define FE_MM_VMM_H

#include <fe/types.h>
#include <fe/status.h>
#include <fe/mm.h>

struct fe_address_space {
    phys_addr_t pml4_phys;
    u64        *pml4;        /* 通过 HHDM 访问的页表虚拟地址 */
    u32         refcount;
    bool        is_kernel;
    u64         mapped_pages;/* 已映射的 4KiB 页计数（诊断用） */
};

void fe_vmm_init(void);

struct fe_address_space *fe_vmm_kernel_space(void);

/* 新建/销毁进程地址空间（内核部分自动共享） */
struct fe_address_space *fe_vmm_space_create(void);
void fe_vmm_space_destroy(struct fe_address_space *as);

/* 切换当前地址空间 */
void fe_vmm_switch(struct fe_address_space *as);

/* 映射与解除映射。virt/phys/size 均须页对齐。 */
fe_status_t fe_vmm_map(struct fe_address_space *as, virt_addr_t virt,
                       phys_addr_t phys, u64 size, u64 flags);
fe_status_t fe_vmm_unmap(struct fe_address_space *as, virt_addr_t virt, u64 size);
/* 解除映射并把物理帧归还 PMM */
fe_status_t fe_vmm_unmap_free(struct fe_address_space *as, virt_addr_t virt, u64 size);

/* 分配新物理帧并映射到指定虚拟地址区间 */
fe_status_t fe_vmm_map_alloc(struct fe_address_space *as, virt_addr_t virt,
                             u64 size, u64 flags);

/* 2MiB 大页映射（用于 HHDM 与 MMIO 这类大范围直映射） */
fe_status_t fe_vmm_map_huge_2m(struct fe_address_space *as, virt_addr_t virt,
                               phys_addr_t phys, u64 size, u64 flags);

/* 查询：返回物理地址（0 表示未映射）与页表项标志 */
phys_addr_t fe_vmm_translate(struct fe_address_space *as, virt_addr_t virt);
u64         fe_vmm_query_flags(struct fe_address_space *as, virt_addr_t virt);

void fe_vmm_flush_tlb_page(virt_addr_t virt);
void fe_vmm_flush_tlb_all(void);

/* 把 MMIO 物理区间映射进 MMIO 窗口（关闭缓存），返回可访问的虚拟地址 */
void *fe_vmm_map_mmio(phys_addr_t phys, u64 size);

/* 逐级打印虚拟地址的页表项（诊断用） */
void fe_vmm_debug_walk(struct fe_address_space *as, virt_addr_t virt);

/* M11 自检：撤销映射的语义（页表项已清 / 幂等 / 撤销后重映射看不到旧内容 /
 * 撤销不释放帧）。返回失败项数。 */
u32 fe_selftest_vmm_unmap(void);

u64 fe_vmm_hhdm_offset(void);

#endif /* FE_MM_VMM_H */
