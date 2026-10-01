/* SPDX-License-Identifier: 0BSD */
/* ELF64 加载器：把可执行映像装进**指定的地址空间**。
 *
 * 做法：先按**最终权限**建立映射，再通过 HHDM 按物理地址写入内容。
 * 这样就不需要「先映射成可写、加载完再改成只读」这种会留下时间窗的两步走法。
 *
 * ★ 它不认识"任务"，只认地址空间与区间表 ★ 产物写进 fe_elf_image_info。
 * 这样 exec 的准备阶段就能往一个**还没挂到任何任务上**的候选空间里装载，
 * 失败了整个扔掉、原程序毫发无损。理由见 fe/user.h 里那段说明。
 */
#include <fe/user.h>
#include <fe/elf.h>
#include <fe/mm.h>
#include <fe/mm/vmm.h>
#include <fe/mm/pmm.h>
#include <fe/mm/vma.h>
#include <fe/kprintf.h>
#include <fe/string.h>

fe_status_t fe_elf_load(struct fe_address_space *as, struct fe_vma_table *vmas,
                        const void *elf, u64 size, struct fe_elf_image_info *out)
{
    if (!as || !out || !elf || size < sizeof(struct fe_elf64_ehdr)) {
        return FE_ERR_INVAL;
    }
    const struct fe_elf64_ehdr *eh = (const struct fe_elf64_ehdr *)elf;
    if (!fe_elf_check(eh)) {
        return FE_ERR_INVAL;
    }
    if (eh->e_type != FE_ET_EXEC) {
        return FE_ERR_NOTSUP;       /* M4 只支持静态可执行文件，不支持 PIE */
    }
    if (eh->e_phoff + (u64)eh->e_phnum * eh->e_phentsize > size) {
        return FE_ERR_INVAL;
    }

    u64 hhdm = fe_vmm_hhdm_offset();
    u32 loaded = 0;
    u32 tls_seen = 0;
    /* 产物先攒在局部变量里，最后**一次性**写进 out。
     * 这样"装载失败"时调用者的候选现场一个字都没被改过——
     * 这条纪律是 exec 的"先准备后提交"能成立的前提。 */
    struct fe_elf_image_info img;
    img.entry = 0;
    img.tls_init = NULL;
    img.tls_size = 0;
    img.tls_align = 0;

    for (u32 i = 0; i < eh->e_phnum; i++) {
        const struct fe_elf64_phdr *ph =
            (const struct fe_elf64_phdr *)(const void *)((const u8 *)elf +
                                                         eh->e_phoff +
                                                         (u64)i * eh->e_phentsize);

        /* ★ PT_TLS：线程局部存储的初始化映像 ★
         *
         * 它**不是**要映射进地址空间的一段（所以不能当 PT_LOAD 处理）：
         * 它是"每个线程开始时该有什么"。内核在这里只记下映像的位置与大小，
         * 新建线程时再复制一份到那个线程自己的 TLS 块里。
         *
         * 注意 tls_size 用 p_memsz 而不是 p_filesz：多出来的那部分
         * 是 .tbss（未初始化的线程局部变量），必须按 0 初始化。
         * 用 p_filesz 的后果是"某个 __thread 变量带着上一个线程的值"。 */
        if (ph->p_type == FE_PT_TLS) {
            if (ph->p_offset + ph->p_filesz > size) {
                return FE_ERR_INVAL;
            }
            img.tls_init = (const void *)((const u8 *)elf + ph->p_offset);
            img.tls_size = ph->p_memsz;
            img.tls_align = ph->p_align ? ph->p_align : 16;
            if (img.tls_align > 64) {
                img.tls_align = 64;     /* 异常大的对齐要求：挡住，别把分配算爆 */
            }
            tls_seen = 1;
            continue;
        }

        if (ph->p_type != FE_PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        if (ph->p_offset + ph->p_filesz > size) {
            return FE_ERR_INVAL;
        }
        if (ph->p_vaddr < FE_USER_BASE) {
            return FE_ERR_INVAL;    /* 不允许加载到低地址 */
        }

        u64 vstart = FE_FRAME_ALIGN_DOWN(ph->p_vaddr);
        u64 vend = FE_FRAME_ALIGN_UP(ph->p_vaddr + ph->p_memsz);

        u64 flags = FE_PTE_USER | FE_PTE_NX;
        if (ph->p_flags & FE_PF_W) {
            flags |= FE_PTE_WRITE;
        }
        if (ph->p_flags & FE_PF_X) {
            flags &= ~FE_PTE_NX;
        }

        for (u64 va = vstart; va < vend; va += FE_FRAME_SIZE) {
            /* 同一段内可能多个页共享一个物理帧的边界情况不存在（我们逐页分配） */
            phys_addr_t frame = fe_pmm_alloc_frame();
            if (frame == 0) {
                return FE_ERR_NOMEM;
            }
            fe_status_t s = fe_vmm_map(as, va, frame, FE_FRAME_SIZE, flags);
            if (fe_failed(s)) {
                fe_pmm_free_frame(frame);
                return s;
            }
            u8 *dst = (u8 *)(uptr)(frame + hhdm);
            memset(dst, 0, FE_FRAME_SIZE);

            /* 把该页落在文件里的部分拷进去 */
            u64 page_file_start = (va > ph->p_vaddr) ? va : ph->p_vaddr;
            u64 page_file_end = ph->p_vaddr + ph->p_filesz;
            if (page_file_end > va + FE_FRAME_SIZE) {
                page_file_end = va + FE_FRAME_SIZE;
            }
            if (page_file_end > page_file_start) {
                u64 n = page_file_end - page_file_start;
                memcpy(dst + (page_file_start - va),
                       (const u8 *)elf + ph->p_offset + (page_file_start - ph->p_vaddr),
                       n);
            }
            /* 其余部分保持为零（.bss 语义） */
        }
        loaded++;
        /* ★ 把这一段登记成区间，但**故意不标 ANON** ★
         *
         * 映像段是**预映射**的：这里已经把每一页都映射好并填了内容。
         * 登记区间的意义是让地址空间有"意图"记录（诊断、将来的
         * mprotect/munmap 都要它），而不是要按需分页——
         * 它没有后备存储：缺页时没有"从哪里取数据"的答案，
         * 所以那种缺页必须仍然是错误（`fe_user_resolve_fault` 会拒绝
         * 没有 ANON 也没有后备帧的区间）。
         *
         * 两段的权限从 ELF 的 p_flags 来，与页表用的是同一个判断，
         * 所以 VMA 与页表在同一次装载里天然一致。 */
        if (vmas) {
            u32 vflags = 0;
            if (ph->p_flags & FE_PF_R) {
                vflags |= FE_VMA_READ;
            }
            if (ph->p_flags & FE_PF_W) {
                vflags |= FE_VMA_WRITE;
            }
            if (ph->p_flags & FE_PF_X) {
                vflags |= FE_VMA_EXEC;
            }
            /* 段与段之间可能有页对齐造成的空档；登记失败（重叠/表满）
             * **不阻止装载**——映像已经映射好、能跑；区间表少一项的后果
             * 只是"这段地址缺页时不会被补页"，而预映射段本来也不该缺页。 */
            (void)fe_vma_add(vmas, vstart, vend, vflags, NULL);
        }
    }

    if (loaded == 0) {
        return FE_ERR_INVAL;
    }
    if (!tls_seen) {
        /* 没有 TLS 段：产物显式留零，别让调用者拿到上一个程序留下的映像指针。
         * （今天这个是局部变量，本来就是零；写出来是为了让"为什么必须有这一步"
         *   留在代码里：换成复用结构的那天，忘了清零就会去复制一段
         *   不属于本程序的内存——那是最难查的一类错。） */
        img.tls_init = NULL;
        img.tls_size = 0;
        img.tls_align = 0;
    }
    img.entry = eh->e_entry;
    *out = img;
    return FE_OK;
}
