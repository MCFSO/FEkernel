/* SPDX-License-Identifier: 0BSD */
#include <fe/user.h>
#include <fe/task.h>
#include <fe/sched/thread.h>
#include <fe/mm.h>
#include <fe/mm/vmm.h>
#include <fe/mm/pmm.h>
#include <fe/kprintf.h>
#include <fe/string.h>
#include <fe/io.h>

struct fe_address_space *fe_user_current_space(void)
{
    struct fe_task *t = fe_task_current();
    return t ? t->space : NULL;
}

bool fe_user_range_ok(struct fe_address_space *as, u64 addr, u64 len, bool write)
{
    if (!as || len == 0) {
        return false;
    }
    /* 必须完全落在用户半区（低半区），且不能跨到 0 附近以外的非规范地址 */
    u64 end = addr + len;
    if (end < addr) {
        return false;                       /* 溢出 */
    }
    if (end > FE_USER_SPACE_END) {          /* 必须落在低半区（用户空间） */
        return false;
    }
    if (addr < 0x1000) {
        return false;                       /* 空指针页永远不允许 */
    }

    /* 逐页确认：确实映射了、且允许用户访问、写操作还要求可写 */
    u64 page = FE_FRAME_ALIGN_DOWN(addr);
    u64 last = FE_FRAME_ALIGN_DOWN(end - 1);
    for (; page <= last; page += FE_FRAME_SIZE) {
        u64 flags = fe_vmm_query_flags(as, page);
        if (!(flags & FE_PTE_PRESENT) || !(flags & FE_PTE_USER)) {
            return false;
        }
        if (write && !(flags & FE_PTE_WRITE)) {
            return false;
        }
    }
    return true;
}

fe_status_t fe_copy_from_user(void *dst, const void *user_src, u64 len)
{
    if (len == 0) {
        return FE_OK;
    }
    struct fe_address_space *as = fe_user_current_space();
    if (!fe_user_range_ok(as, (u64)(uptr)user_src, len, false)) {
        return FE_ERR_FAULT;
    }
    /* 内核半区在所有地址空间里共享，因此当前 CR3 下直接读用户虚拟地址即可 */
    memcpy(dst, user_src, len);
    return FE_OK;
}

fe_status_t fe_copy_to_user(void *user_dst, const void *src, u64 len)
{
    if (len == 0) {
        return FE_OK;
    }
    struct fe_address_space *as = fe_user_current_space();
    if (!fe_user_range_ok(as, (u64)(uptr)user_dst, len, true)) {
        return FE_ERR_FAULT;
    }
    memcpy(user_dst, src, len);
    return FE_OK;
}

/* 以 '\0' 结尾的用户字符串：逐页校验、遇终止符即停（见头文件里的说明）。 */
fe_status_t fe_copy_str_from_user(char *dst, const char *user_src, u64 cap)
{
    if (!dst || !user_src || cap == 0) {
        return FE_ERR_INVAL;
    }
    struct fe_address_space *as = fe_user_current_space();
    const char *src = user_src;
    u64 n = 0;

    while (n + 1 < cap) {
        /* 一次处理"当前页内还剩多少字节"，于是永远不会跨页读未映射的部分 */
        u64 page_off = (u64)(uptr)src & (FE_FRAME_SIZE - 1);
        u64 chunk = FE_FRAME_SIZE - page_off;
        if (chunk > cap - 1 - n) {
            chunk = cap - 1 - n;
        }
        if (!fe_user_range_ok(as, (u64)(uptr)src, chunk, false)) {
            return FE_ERR_FAULT;
        }
        for (u64 i = 0; i < chunk; i++) {
            char c = src[i];
            dst[n++] = c;
            if (c == '\0') {
                return FE_OK;
            }
        }
        src += chunk;
    }
    dst[n] = '\0';      /* 便于诊断，但返回的是错误：调用者不得使用这个串 */
    return FE_ERR_NAMETOOLONG;
}

/* 往**别的**地址空间里写。
 *
 * 为什么不能像上面两个那样直接 memcpy：目标不是当前任务，CR3 里没有它的映射。
 * 做法是逐页查出物理帧，经 HHDM（物理内存的全局直映射）写进去。
 * 装载器构造初始栈时要用这个——那时新进程还没被调度过，不能切 CR3
 * （在内核中间换地址空间，稍有不慎就会踩到自己的栈）。 */
fe_status_t fe_user_write_space(struct fe_address_space *as, u64 va,
                               const void *src, u64 len)
{
    if (!as || !src) {
        return FE_ERR_INVAL;
    }
    const u8 *s = (const u8 *)src;
    u64 hhdm = fe_vmm_hhdm_offset();

    while (len > 0) {
        u64 page = FE_FRAME_ALIGN_DOWN(va);
        u64 off = va - page;
        u64 chunk = FE_FRAME_SIZE - off;
        if (chunk > len) {
            chunk = len;
        }
        phys_addr_t phys = fe_vmm_translate(as, page);
        if (phys == 0) {
            return FE_ERR_FAULT;
        }
        memcpy((void *)(uptr)(phys + hhdm + off), s, chunk);
        va += chunk;
        s += chunk;
        len -= chunk;
    }
    return FE_OK;
}
