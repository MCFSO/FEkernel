/* SPDX-License-Identifier: 0BSD */
/* 自定义系统调用 ABI 的内核侧实现（M4）。
 *
 * 调用约定（与 include/fe/syscall.h 一致）：
 *   rax = 调用号，rdi/rsi/rdx/r10/r8/r9 = 参数 1..6，返回值为 rax。
 *   rcx 与 r11 被 syscall 指令本身破坏，用户态不得依赖。
 *
 * 所有接收用户指针的系统调用都必须经过 uaccess 校验：
 * 内核不信任用户指针，越界或未映射一律返回 FE_ERR_FAULT 而不是崩溃。
 */
#include <fe/syscall.h>
#include <fe/regs.h>
#include <fe/vectors.h>
#include <fe/idt.h>
#include <fe/gdt.h>
#include <fe/io.h>
#include <fe/kprintf.h>
#include <fe/panic.h>
#include <fe/string.h>
#include <fe/time.h>
#include <fe/mm.h>
#include <fe/mm/vmm.h>
#include <fe/mm/vma.h>      /* fe_vma_protect_range（MEM_PROTECT 的机制层） */
#include <fe/mm/kheap.h>
#include <fe/mm/pmm.h>
#include <fe/sched/sched.h>
#include <fe/sched/thread.h>
#include <fe/task.h>
#include <fe/object.h>
#include <fe/ipc.h>
#include <fe/user.h>
#include <fe/resource.h>
#include <fe/irq.h>
#include <fe/vfs.h>
#include <fe/devfs.h>
#include <fe/protect.h>
#include <fe/reset.h>
#include <fe/boot/bootinfo.h>
#include <fe/process.h>

void fe_syscall_msr_init(u64 entry, void *percpu);
extern void fe_syscall_entry(void);

/* 每个 CPU 的系统调用暂存区（见 syscall.asm 的说明） */
struct fe_syscall_percpu {
    u64 user_rsp;       /* gs:0 */
    u64 kernel_rsp;     /* gs:8 */
};
static struct fe_syscall_percpu g_syscall_percpu;

void fe_syscall_set_kernel_stack(u64 rsp)
{
    g_syscall_percpu.kernel_rsp = rsp;
}

void fe_syscall_init(void)
{
    g_syscall_percpu.user_rsp = 0;
    g_syscall_percpu.kernel_rsp = 0;
    fe_syscall_msr_init((u64)(uptr)fe_syscall_entry, &g_syscall_percpu);
    u64 star = fe_rdmsr(FE_MSR_STAR);
    u64 lstar = fe_rdmsr(FE_MSR_LSTAR);
    u64 sfmask = fe_rdmsr(FE_MSR_SFMASK);
    u64 efer = fe_rdmsr(FE_MSR_EFER);
    fe_kprintf("[初始化] syscall/sysret 已启用\n");
    fe_kprintf("        STAR=%#llx -> syscall 用 CS=%#llx SS=%#llx, sysret 用 CS=%#llx SS=%#llx\n",
               (unsigned long long)star,
               (unsigned long long)((star >> 32) & 0xFFFF),
               (unsigned long long)(((star >> 32) & 0xFFFF) + 8),
               (unsigned long long)(((star >> 48) & 0xFFFF) + 16),
               (unsigned long long)(((star >> 48) & 0xFFFF) + 8));
    fe_kprintf("        LSTAR=%#llx (入口 %#llx) SFMASK=%#llx EFER=%#llx(SCE=%llu)\n",
               (unsigned long long)lstar, (unsigned long long)(uptr)fe_syscall_entry,
               (unsigned long long)sfmask, (unsigned long long)efer,
               (unsigned long long)(efer & 1));
}

/* ------------------------------------------------------------------ */
/* 各系统调用的实现                                                     */
/* ------------------------------------------------------------------ */

static i64 sys_debug_write(const char *user_buf, u64 len)
{
    if (!user_buf) {
        return FE_ERR_INVAL;
    }
    if (len > 4096) {
        len = 4096;
    }
    char tmp[256];
    u64 done = 0;
    while (done < len) {
        u64 chunk = len - done;
        if (chunk > sizeof(tmp)) {
            chunk = sizeof(tmp);
        }
        fe_status_t s = fe_copy_from_user(tmp, user_buf + done, chunk);
        if (fe_failed(s)) {
            return s;
        }
        for (u64 i = 0; i < chunk; i++) {
            fe_console_putc(tmp[i]);
        }
        done += chunk;
    }
    return (i64)len;
}

static i64 sys_thread_exit(i32 code)
{

    fe_thread_exit(code);
    return FE_OK;   /* 不会到达 */
}

static i64 sys_thread_create(u64 entry, u64 arg, u64 stack, u64 stack_size, u64 flags)
{
    return fe_user_thread_create((void (*)(void *))(uptr)entry, (void *)(uptr)arg,
                                 stack, stack_size, (u32)flags);
}

static i64 sys_sleep_ns(u64 ns)
{
    u64 ms = ns / 1000000ull;
    if (ms == 0 && ns > 0) {
        ms = 1;
    }
    fe_thread_sleep_ms(ms);
    return FE_OK;
}

static i64 sys_clock_monotonic(void)
{
    /* ★ K7：返回**TSC 制**的纳秒，不再是"毫秒 × 1e6" ★
     * 原来那个写法把时钟量化到 1 ms，于是任何亚毫秒的测量都只会得到
     * 0 或 1000000 的倍数（D4 的吞吐对照里两个数字都是正好 5000 us）。
     * 引导器没给 TSC 频率时 fe_time_ns 会自己退回节拍制——
     * 调用者可以用 FE_SYS_CLOCK_INFO 问清楚精度。 */
    return (i64)fe_time_ns();
}

/* 时钟的**自述**：分辨率与来源。给用户态一个"我能不能信这个时钟"的判据。
 *
 * ★ 为什么要专门一个 syscall 而不是"文档里写一句" ★
 * D4 那次的教训是：一个不知道自身精度的时钟会让测试写出**看起来通过、
 * 实际什么都没测**的断言（两个 5000 us 就是那样来的）。
 * 与其在文档里承诺"时钟够细"，不如让调用者能当场问出来并据此选工具
 * （例如"粒度不够就自己用 rdtsc"）。 */
static i64 sys_clock_info(u64 user_out)
{
    if (!user_out) {
        return FE_ERR_INVAL;
    }
    struct fe_clock_info info;
    info.hz = fe_time_clock_hz();
    info.flags = fe_time_clock_is_tsc() ? FE_CLOCK_FLAG_TSC : 0;
    info._pad = 0;
    /* 分辨率 = 一个计数对应多少纳秒（TSC 制下约 0.33 ns；
     * 节拍制下就是 1 ms = 1000000 ns）。取整到至少 1，避免报 0——
     * "分辨率 0"会被读成"无限精确"。 */
    u64 res = (info.hz > 1000000000ull) ? 1ull : (1000000000ull / info.hz);
    info.resolution_ns = res ? res : 1ull;
    return fe_copy_to_user((void *)(uptr)user_out, &info, sizeof(info));
}

static i64 sys_mem_alloc(u64 size, u64 flags)
{
    struct fe_task *t = fe_task_current();
    fe_handle_t h = FE_HANDLE_INVALID;
    fe_status_t s = fe_memory_create(t, size, (u32)flags, &h);
    if (fe_failed(s)) {
        return s;
    }
    return (i64)h;
}

static i64 sys_mem_map(u64 handle, u64 hint, u64 size, u64 prot)
{
    struct fe_task *t = fe_task_current();
    void *virt = NULL;
    fe_status_t s = fe_memory_map_user(t, (fe_handle_t)handle, hint, size, (u32)prot, &virt);
    if (fe_failed(s)) {
        return s;
    }
    return (i64)(uptr)virt;
}

static i64 sys_handle_close(u64 handle)
{
    struct fe_task *t = fe_task_current();
    return fe_handle_close(&t->handles, (fe_handle_t)handle);
}

/* 句柄复制：权限只能收窄（内核的 fe_handle_dup 已经强制这条）。
 *
 * new_rights == 0 表示"沿用原权限"。为什么用 0 而不是 ~0：
 * 让"我只要再来一份同样的能力"这个最常见的诉求写成最自然的那个值，
 * 而不是让调用者去猜一个全 1 的掩码（猜错就是权限被意外收窄，
 * 症状是"复制出来的句柄干活时报 ACCESS"，离原因隔着几层）。
 *
 * 注意 DUP 与 TRANSFER 是两位不同的权限，别混：
 *   DUP      = 我可以在**自己**的句柄表里再装一份；
 *   TRANSFER = 我可以把它交给**别人**。
 * 所以复制本身要 DUP，而被复制出来的句柄**不含** TRANSFER 也能正常工作。 */
static i64 sys_handle_dup(u64 handle, u64 new_rights)
{
    struct fe_task *t = fe_task_current();
    if (!t) {
        return FE_ERR_ACCESS;
    }
    u32 rights = (u32)new_rights;
    if (rights == 0) {
        fe_status_t sr = fe_handle_rights(&t->handles, (fe_handle_t)handle, &rights);
        if (fe_failed(sr)) {
            return sr;
        }
    }
    fe_handle_t out = FE_HANDLE_INVALID;
    fe_status_t s = fe_handle_dup(&t->handles, (fe_handle_t)handle, rights, &out);
    return fe_failed(s) ? s : (i64)out;
}

/* 撤销一段由 MEM_MAP 建立的用户映射。
 *
 * ★ 它不释放物理帧 ★（帧归内存对象所有，见 fe/syscall.h 里这一项的说明）。
 * ★ 只允许撤销 mmap 区内的地址 ★（见下）。
 *
 * 页表项的清理是原子的（`*pte = 0` 一条 store）且**在 TLB 失效之前**完成：
 * 先清 PTE 再 flush，所以不存在"页表说没映射、TLB 还说有"的窗口。
 * 反过来（先 flush 再清 PTE）就会出现那种窗口——另一条线程可能读到已释放映射。
 *
 * ★ 重复撤销返回成功（幂等）★
 * 调用者把 UNMAP 写在清理路径上时，往往并不知道自己有没有映射过。
 * 让第二次返回 NOENT 会把"清理"变成需要判断返回值的操作，
 * 而这里的后置条件（"这段地址上没有映射"）在第二次调用后同样成立。 */
static i64 sys_mem_unmap(u64 addr, u64 size)
{
    struct fe_task *t = fe_task_current();
    if (!t || !t->space) {
        return FE_ERR_ACCESS;       /* 内核线程不走这条路 */
    }
    if (size == 0 || (addr & (FE_FRAME_SIZE - 1)) != 0) {
        return FE_ERR_INVAL;
    }
    u64 end = addr + FE_FRAME_ALIGN_UP(size);
    if (end < addr) {
        return FE_ERR_OVERFLOW;
    }
    /* 只撤销 mmap 区：程序映像与栈也在同一个地址空间里，但它们不是
     * MEM_MAP 建立的。允许撤销它们等于提供一个"把自己的代码页拆掉"的接口
     * ——那不是机制，是陷阱。 */
    if (addr < FE_USER_MMAP_BASE || end > FE_USER_MMAP_END) {
        return FE_ERR_INVAL;
    }
    fe_status_t s = fe_vmm_unmap(t->space, (virt_addr_t)addr, end - addr);
    return (s == FE_ERR_NOENT) ? FE_OK : (i64)s;
}

/* 改一段**调用者自己地址空间里**页面的访问权限（C2 的最后一条）。
 *
 * ★ 参数形状与 MEM_MAP/MEM_UNMAP 同族 ★ addr / len / prot 三个参数，
 * 权限位复用既有的 `FE_PROT_*`（`fe_user.h` 与内核这一份是同一套值）。
 *
 * ★ 边界：只认自己的地址空间 ★
 * 目标恒为 `fe_task_current()`，接口里没有"指定别的任务"这个参数——
 * 与 `MEM_UNMAP` 同一条纪律。所以"改到别人的内存"在**形状上**就不可能，
 * 而不是靠一句运行期比对（那种判据会漂，而且漏一处就是一个洞）。
 *
 * ★ 与 MEM_UNMAP 不同的一点：这里**不限制在 mmap 区** ★
 * UNMAP 限制在 mmap 区，是因为"把自己的代码页拆掉"只是陷阱、没有用处；
 * 而**改权限**对映像段与栈恰恰是正当用途（`libposix` 的 `mprotect`
 * 就要能给任意自己映射过的段松紧权限）。安全边界因此落在**别处**：
 * 范围必须完全落在调用者自己的某个 VMA 内（`fe_vma_protect_range` 判），
 * 而 VMA 是内核在装载映像/建栈/做映射时建立的——用户态无法凭空造一个
 * 覆盖别人内存的区间。
 *
 * ★ 权限位到 VMA 标志的翻译放在这一层 ★
 * 上层的 `fe_vma_protect_range` 只认 `FE_VMA_*`；`FE_PROT_*` 是 ABI 那一侧
 * 的名字。两套值**今天恰好同序但语义不同**（`FE_PROT_USER` 在 VMA 侧没有
 * 对应物），所以必须显式翻译一遍，不能指望数值相等就把掩码直接传下去。 */
static i64 sys_mem_protect(u64 addr, u64 len, u64 prot)
{
    struct fe_task *t = fe_task_current();
    if (!t || !t->space || !t->vmas) {
        return FE_ERR_ACCESS;       /* 内核线程不走这条路 */
    }
    if (len == 0 || (addr & (FE_FRAME_SIZE - 1)) != 0) {
        return FE_ERR_INVAL;
    }
    /* 用户指针必须是用户地址：用户空间之上的地址一律拒绝。
     * （`addr` 是调用者给的，不是内核算出来的，所以要自己判一次。） */
    if (addr >= FE_USER_SPACE_END || addr + len < addr ||
        addr + len > FE_USER_SPACE_END) {
        return FE_ERR_INVAL;
    }
    /* 只认这三位；多给别的位（比如 FE_PROT_USER）一律拒绝，
     * 而不是"忽略多余位"——后者会让"用户态以为自己设了什么"与
     * "内核实际做了什么"不一致。 */
    if (prot & ~(u64)(FE_PROT_READ | FE_PROT_WRITE | FE_PROT_EXEC)) {
        return FE_ERR_INVAL;
    }
    u32 vflags = 0;
    if (prot & FE_PROT_READ)  { vflags |= FE_VMA_READ; }
    if (prot & FE_PROT_WRITE) { vflags |= FE_VMA_WRITE; }
    if (prot & FE_PROT_EXEC)  { vflags |= FE_VMA_EXEC; }

    /* 范围要按页对齐地覆盖 [addr, addr+len)：与 UNMAP 一样向上取整，
     * 并且把"没覆盖到任何页"这种空范围挡掉。 */
    u64 end = addr + FE_FRAME_ALIGN_UP(len);
    if (end <= addr) {
        return FE_ERR_OVERFLOW;
    }
    return (i64)fe_vma_protect_range(t->vmas, t->space, (virt_addr_t)addr,
                                     (virt_addr_t)end, vflags);
}

static i64 sys_endpoint_create(u64 flags)
{
    struct fe_task *t = fe_task_current();
    fe_handle_t h = FE_HANDLE_INVALID;
    fe_status_t s = fe_endpoint_create(t, (u32)flags, &h);
    return fe_failed(s) ? s : (i64)h;
}

/* 消息头与载荷都在用户内存里；句柄数组紧随其后（M11：句柄传递）。
 *
 * ★ hcount 与 hdr.handle_count 必须一致，不一致就拒绝 ★
 * 两个来源分别决定"内核要拷几个句柄"和"内核要查几个句柄"。
 * 允许它们不一致，就是允许"内核按 A 读、用户按 B 理解"——
 * 症状是"有时传过去的句柄是垃圾"，而根因在几千行之外。 */
static i64 sys_endpoint_send(u64 ep, u64 user_hdr, u64 user_payload,
                             u64 hcount, u64 user_handles)
{
    struct fe_task *t = fe_task_current();
    struct fe_msg_header hdr;
    fe_status_t s = fe_copy_from_user(&hdr, (const void *)(uptr)user_hdr, sizeof(hdr));
    if (fe_failed(s)) {
        return s;
    }
    if (hdr.payload_len > FE_MSG_MAX_PAYLOAD) {
        return FE_ERR_INVAL;
    }
    if (hdr.handle_count > FE_MSG_MAX_HANDLES || hcount > FE_MSG_MAX_HANDLES) {
        return FE_ERR_INVAL;        /* 不截断：见 docs/09-handle-transfer.md §2 */
    }
    if (hcount != hdr.handle_count) {
        return FE_ERR_INVAL;
    }
    u8 payload[FE_MSG_MAX_PAYLOAD];
    if (hdr.payload_len) {
        s = fe_copy_from_user(payload, (const void *)(uptr)user_payload, hdr.payload_len);
        if (fe_failed(s)) {
            return s;
        }
    }
    /* hcount == 0 时**一次用户内存访问都不多做**：这是最常见的路径
     * （所有服务调用都是它），句柄通道不能给它带来任何额外开销。 */
    fe_handle_t handles[FE_MSG_MAX_HANDLES];
    const fe_handle_t *hp = NULL;
    if (hcount) {
        if (!user_handles) {
            return FE_ERR_INVAL;
        }
        s = fe_copy_from_user(handles, (const void *)(uptr)user_handles,
                              hcount * sizeof(fe_handle_t));
        if (fe_failed(s)) {
            return s;
        }
        hp = handles;
    }
    return fe_endpoint_send(t, (fe_handle_t)ep, &hdr, payload, hp);
}

static i64 sys_endpoint_recv(u64 ep, u64 user_hdr, u64 user_payload, u64 cap,
                             u64 user_hbuf, u64 user_nbuf)
{
    struct fe_task *t = fe_task_current();
    struct fe_msg_header hdr;
    u8 payload[FE_MSG_MAX_PAYLOAD];
    fe_handle_t reply_ep = FE_HANDLE_INVALID;
    fe_handle_t transferred[FE_MSG_MAX_HANDLES];

    /* ★ 先把两个输出指针校验掉，再去收 ★
     *
     * fe_endpoint_recv 是阻塞的，而且成功时**已经在接收方句柄表里装好了新句柄**。
     * 若那时才发现句柄数组拷不回去，用户态就拿到一堆自己不知道句柄号的
     * 已安装句柄——不可回收的泄漏。所以这里在进入 IPC 之前就把
     * "写不写得进去"问清楚：未映射 → FE_ERR_FAULT，且不消耗任何消息。 */
    if (user_hbuf && !fe_user_range_ok(fe_user_current_space(), user_hbuf,
                                       sizeof(transferred), true)) {
        return FE_ERR_FAULT;
    }
    if (user_nbuf && !fe_user_range_ok(fe_user_current_space(), user_nbuf,
                                       sizeof(u32), true)) {
        return FE_ERR_FAULT;
    }

    fe_status_t s = fe_endpoint_recv(t, (fe_handle_t)ep, &hdr, payload, sizeof(payload),
                                     &reply_ep, user_hbuf ? transferred : NULL);
    if (fe_failed(s)) {
        return s;
    }
    u32 n = hdr.payload_len;
    if (n > cap) {
        n = (u32)cap;
    }
    hdr.payload_len = n;
    s = fe_copy_to_user((void *)(uptr)user_hdr, &hdr, sizeof(hdr));
    if (fe_failed(s)) {
        return s;
    }
    if (n) {
        s = fe_copy_to_user((void *)(uptr)user_payload, payload, n);
        if (fe_failed(s)) {
            return s;
        }
    }
    if (user_hbuf) {
        u32 installed = 0;
        for (u32 i = 0; i < FE_MSG_MAX_HANDLES; i++) {
            if (transferred[i] != FE_HANDLE_INVALID) {
                installed++;
            }
        }
        s = fe_copy_to_user((void *)(uptr)user_hbuf, transferred, sizeof(transferred));
        if (fe_failed(s)) {
            return s;
        }
        if (user_nbuf) {
            s = fe_copy_to_user((void *)(uptr)user_nbuf, &installed, sizeof(installed));
            if (fe_failed(s)) {
                return s;
            }
        }
    }
    return (i64)reply_ep;       /* 非 0 表示可以回复 */
}

static i64 sys_endpoint_call(u64 ep, u64 req, u64 req_len, u64 rep, u64 rep_cap, u64 out_len)
{
    struct fe_task *t = fe_task_current();
    if (req_len > FE_MSG_MAX_PAYLOAD) {
        return FE_ERR_INVAL;
    }
    u8 reqbuf[FE_MSG_MAX_PAYLOAD];
    if (req_len) {
        fe_status_t s = fe_copy_from_user(reqbuf, (const void *)(uptr)req, req_len);
        if (fe_failed(s)) {
            return s;
        }
    }
    u8 repbuf[FE_MSG_MAX_PAYLOAD];
    u32 reply_len = 0;
    fe_status_t s = fe_endpoint_call(t, (fe_handle_t)ep, reqbuf, (u32)req_len,
                                     repbuf, sizeof(repbuf), &reply_len);
    if (fe_failed(s)) {
        return s;
    }
    u32 n = reply_len;
    if (n > rep_cap) {
        n = (u32)rep_cap;
    }
    if (n) {
        s = fe_copy_to_user((void *)(uptr)rep, repbuf, n);
        if (fe_failed(s)) {
            return s;
        }
    }
    if (out_len) {
        s = fe_copy_to_user((void *)(uptr)out_len, &n, sizeof(n));
        if (fe_failed(s)) {
            return s;
        }
    }
    return FE_OK;
}

static i64 sys_notification_create(void)
{
    struct fe_task *t = fe_task_current();
    fe_handle_t h = FE_HANDLE_INVALID;
    fe_status_t s = fe_notification_create(t, &h);
    return fe_failed(s) ? s : (i64)h;
}

static i64 sys_notification_wait(u64 handle, u64 mask, u64 user_out)
{
    u64 bits = 0;
    fe_status_t s = fe_notification_wait((fe_handle_t)handle, mask, &bits);
    if (fe_failed(s)) {
        return s;
    }
    if (user_out) {
        s = fe_copy_to_user((void *)(uptr)user_out, &bits, sizeof(bits));
        if (fe_failed(s)) {
            return s;
        }
    }
    return FE_OK;
}

/* ------------------------------------------------------------------ */
/* M6：驱动能力                                                        */
/* ------------------------------------------------------------------ */

/* 申请一段端口区间的使用权。
 *
 * 两步走，缺一不可：
 *   1. 从内核资源池里**认领**——保证同一段端口只有一个驱动持有；
 *   2. 把这段端口在**本任务**的 TSS I/O 位图里放开——这是真正让 in/out 不触发
 *      #GP 的东西，由 CPU 在每次执行 in/out 时检查。
 * 只做第 1 步是空头支票，只做第 2 步则两个驱动会互相踩。 */
static i64 sys_ioport_request(u64 base, u64 count)
{
    struct fe_task *t = fe_task_current();
    if (!t || !t->space) {
        return FE_ERR_ACCESS;       /* 内核线程不走这条路 */
    }
    if (count == 0 || base > 0xFFFFull || base + count > 0x10000ull) {
        return FE_ERR_RANGE;
    }

    u64 flags = fe_irq_save();

    /* 位图槽位是惰性分配的：没申请过端口能力的任务不占槽位（一共只有 6 个） */
    if (t->iopb_slot < 0) {
        int slot = fe_iopb_alloc_slot();
        if (slot < 0) {
            fe_irq_restore(flags);
            return FE_ERR_NOSPC;
        }
        t->iopb_slot = slot;
    }

    fe_status_t s = fe_resource_claim(FE_RES_IOPORT, base, count, t->id);
    if (fe_failed(s)) {
        fe_irq_restore(flags);
        return s;
    }

    fe_iopb_allow_range(t->iopb_slot, (u32)base, (u32)count, true);
    /* 申请者就是当前线程，立刻让新位图生效，不必等到下次切换 */
    fe_tss_set_iopb_slot(t->iopb_slot);
    fe_irq_restore(flags);

    t->granted_ports++;
    fe_kprintf("[能力] 任务 %s 获得端口 %#llx..%#llx\n", t->name,
               (unsigned long long)base, (unsigned long long)(base + count - 1));
    return FE_OK;
}

/* 把一个 MMIO 物理区间映射进本任务的地址空间。
 *
 * 两道闸门：
 *   - 区间必须来自资源池（内核打算交给驱动的那部分）；
 *   - 区间**不得与可用内存相交**。第二道是关键：如果允许把普通内存当 MMIO 映射，
 *     驱动就能拿到一段可写映射直接改内核数据结构，任何权限检查都形同虚设。 */
static i64 sys_mmio_map(u64 phys, u64 size, u64 prot)
{
    struct fe_task *t = fe_task_current();
    if (!t || !t->space) {
        return FE_ERR_ACCESS;
    }
    if (size == 0) {
        return FE_ERR_INVAL;
    }
    u64 raw_end = phys + size;
    if (raw_end < phys) {
        return FE_ERR_OVERFLOW;
    }
    u64 base = FE_FRAME_ALIGN_DOWN(phys);
    u64 end = FE_FRAME_ALIGN_UP(raw_end);
    u64 map_size = end - base;

    if (fe_pmm_is_usable(base, map_size)) {
        fe_kprintf("[能力] 拒绝把可用内存映射为 MMIO: %#llx + %#llx\n",
                   (unsigned long long)base, (unsigned long long)map_size);
        return FE_ERR_ACCESS;
    }

    u64 flags = fe_irq_save();
    fe_status_t s = fe_resource_claim(FE_RES_MMIO, base, map_size, t->id);
    fe_irq_restore(flags);
    if (fe_failed(s)) {
        return s;
    }

    u64 va = FE_FRAME_ALIGN_UP(t->user_map_next ? t->user_map_next : FE_USER_MMAP_BASE);
    if (va < FE_USER_MMAP_BASE || va + map_size > FE_USER_MMAP_END) {
        return FE_ERR_NOSPC;
    }

    /* MMIO 三个固定属性：用户可访问、关闭缓存、不可执行。
     * 关缓存是硬要求——带缓存的设备寄存器读写会丢写、读到陈旧值。
     * 外加 NOFREE：**那不是 RAM，是设备寄存器**，任务销毁时绝不能
     * 把它当成"自己的帧"还给 PMM（见 FE_PTE_NOFREE 的说明）——
     * 那样等于把设备的寄存器窗口变成了可分页内存。 */
    u64 pte = FE_PTE_USER | FE_PTE_PCD | FE_PTE_NX | FE_PTE_NOFREE;
    if (prot & FE_PROT_WRITE) {
        pte |= FE_PTE_WRITE;
    }
    u64 done = 0;
    for (; done < map_size; done += FE_FRAME_SIZE) {
        s = fe_vmm_map(t->space, va + done, base + done, FE_FRAME_SIZE, pte);
        if (fe_failed(s)) {
            /* 失败要回滚已映射的部分：半截映射对调用者毫无用处，
             * 而它拿不到那段地址（返回值是负数），既用不了也收不回。 */
            if (done > 0) {
                fe_vmm_unmap(t->space, va, done);
            }
            return s;
        }
    }
    t->user_map_next = va + map_size;
    fe_kprintf("[能力] 任务 %s 映射 MMIO %#llx + %#llx -> 虚拟 %#llx\n", t->name,
               (unsigned long long)base, (unsigned long long)map_size,
               (unsigned long long)va);
    return (i64)va;
}

/* 登记一个中断，投递到指定的通知对象。
 * 通知对象由驱动的句柄表提供，内核只持有它的引用——驱动退出时中断自动掐断。
 *
 * ★ 参数是**追加**的，老的两参数调用仍然合法（D5b）★
 * a3 = require_mode（0 = 不要求），a4 = out_info 用户指针（0 = 不要）。
 * 追加而不是新开一个 syscall，是因为"登记"这件事只该有一个入口——
 * 两个入口会让"我到底登记成什么样"有两个答案。 */
static i64 sys_irq_register(u64 irq, u64 nt_handle, u64 require_mode, u64 out_info)
{
    struct fe_task *t = fe_task_current();
    if (!t) {
        return FE_ERR_ACCESS;
    }
    struct fe_object_header *obj = NULL;
    fe_status_t s = fe_handle_lookup(&t->handles, (fe_handle_t)nt_handle,
                                     FE_RIGHT_SIGNAL, &obj);
    if (fe_failed(s)) {
        return s;
    }
    if (obj->type != FE_OBJ_NOTIFICATION) {
        return FE_ERR_INVAL;
    }
    if (irq > 255) {
        return FE_ERR_INVAL;
    }
    if (require_mode & ~(u64)(FE_IRQ_MODE_EDGE | FE_IRQ_MODE_LEVEL |
                              FE_IRQ_MODE_ACTIVE_LOW | FE_IRQ_F_NO_MASK)) {
        return FE_ERR_INVAL;
    }
    /* ★ 触发方式的契约**先于**资源认领检查 ★
     * 反过来的话，"我声明的模式写错了"会先撞上资源池的错误
     * （共享线上同一个任务再认领一次拿到 EXIST），报出来的是
     * "你已经持有它"——而调用者真正写错的是那个参数。
     * 参数错误不依赖任何状态，就该先报。 */
    if (!fe_irq_mode_ok((u32)irq, (u32)require_mode)) {
        fe_kprintf("[IRQ] 拒绝：任务 %s 要求%s触发，而 IRQ%llu 实测不是\n",
                   t->name,
                   (require_mode & FE_IRQ_MODE_LEVEL) ? "电平" : "边沿",
                   (unsigned long long)irq);
        return FE_ERR_INVAL;
    }

    /* ★ 资源池这一步是**共享能不能成立**的关键（D5a）★
     * 池子里那条 IRQ 若被声明为 shared，`fe_resource_claim` 允许多个
     * owner 同时持有；若是独占，第二个驱动在这里就被拒——而那是
     * **设备管理器**的决定（"这条线接了几台设备"是拓扑，是策略），
     * 内核只如实执行。 */
    u64 flags = fe_irq_save();
    s = fe_resource_claim(FE_RES_IRQ, irq, 1, t->id);
    fe_irq_restore(flags);
    if (fe_failed(s)) {
        return s;
    }

    struct fe_irq_info info;
    info.gsi = 0;
    info.vector = 0;
    info.mode = 0;
    info.sharers = 0;
    s = fe_irq_register_ex((u32)irq, FE_OBJ_OF(obj, struct fe_notification), t->id,
                           (u32)require_mode, &info);
    if (fe_failed(s)) {
        /* 登记失败就把资源还回去，否则这根中断线就永远没人能用了 */
        fe_resource_release_owner(t->id);
        return s;
    }
    if (out_info) {
        /* 把实情拷给调用者：GSI、向量、触发方式、共享者数。
         * 拷不回去**不算登记失败**（登记已经生效了），但必须让调用者
         * 知道它没拿到——所以返回 FAULT 而不是假装成功。 */
        fe_status_t cs = fe_copy_to_user((void *)(uptr)out_info, &info, sizeof(info));
        if (fe_failed(cs)) {
            return cs;
        }
    }
    return FE_OK;
}

/* 确认一次中断（D5a：共享时所有登记者都确认才放行）。
 * 从当前任务的身份去确认——这样"谁在确认"不需要由调用者自称。 */
static i64 sys_irq_ack(u64 irq)
{
    struct fe_task *t = fe_task_current();
    if (!t) {
        return FE_ERR_ACCESS;
    }
    return fe_irq_ack_owner((u32)irq, t->id);
}

/* 使能一台设备的 MSI / MSI-X 并把中断绑到通知对象（D5c）。
 * 签名：a1=nt_handle a2=bus a3=dev a4=fn a5=cap_off a6=out_info */
static i64 sys_irq_msi_alloc(u64 nt_handle, u64 bus, u64 dev, u64 fn,
                            u64 cap_off, u64 out_info)
{
    struct fe_task *t = fe_task_current();
    if (!t) {
        return FE_ERR_ACCESS;
    }
    if (!out_info) {
        return FE_ERR_INVAL;    /* MSI 的信息（伪中断号、地址、数据）必须能回传，
                                 * 否则调用者拿到 FE_OK 却不知道等哪一位 */
    }
    struct fe_object_header *obj = NULL;
    fe_status_t s = fe_handle_lookup(&t->handles, (fe_handle_t)nt_handle,
                                     FE_RIGHT_SIGNAL, &obj);
    if (fe_failed(s)) {
        return s;
    }
    if (obj->type != FE_OBJ_NOTIFICATION) {
        return FE_ERR_INVAL;
    }
    struct fe_msi_info info;
    info.irq = 0;
    info.vector = 0;
    info.kind = 0;
    info._pad[0] = info._pad[1] = 0;
    info.table_bar = 0xFF;
    info.table_offset = 0;
    info.data = 0;
    info.addr = 0;
    s = fe_irq_msi_alloc(bus, dev, fn, cap_off, 0,
                         FE_OBJ_OF(obj, struct fe_notification), t->id, &info);
    if (fe_failed(s)) {
        return s;
    }
    fe_status_t cs = fe_copy_to_user((void *)(uptr)out_info, &info, sizeof(info));
    if (fe_failed(cs)) {
        /* 拷不回去就**把它撤掉**：调用者拿不到伪中断号就永远等不到中断，
         * 而这个向量会一直占着——那是一个只有重启才能清掉的泄漏。 */
        (void)fe_irq_msi_free(info.irq, t->id);
        return cs;
    }
    return FE_OK;
}

static i64 sys_irq_msi_free(u64 irq)
{
    struct fe_task *t = fe_task_current();
    if (!t) {
        return FE_ERR_ACCESS;
    }
    return fe_irq_msi_free((u32)irq, t->id);
}

/* 共享资源上的控制器锁。只有标记为可共享的区间才有锁——
 * 独占区间本来就只有一个持有者，加锁没有意义。 */
static i64 sys_resource_lock(u64 kind, u64 base, u64 len)
{
    struct fe_task *t = fe_task_current();
    if (!t) {
        return FE_ERR_ACCESS;
    }
    return fe_resource_lock((u32)kind, base, len, t->id);
}

static i64 sys_resource_unlock(u64 kind, u64 base, u64 len)
{
    struct fe_task *t = fe_task_current();
    if (!t) {
        return FE_ERR_ACCESS;
    }
    return fe_resource_unlock((u32)kind, base, len, t->id);
}

/* 设备管理器把发现的硬件申报给内核（放入资源池）。
 *
 * ★ 这道"是不是设备管理器"的检查是整条链的关键 ★
 * 资源池存在的意义就是**内核裁决"哪段硬件可以给谁"**。如果任何任务都能
 * 往池子里加，那池子就没了——一个任务可以先把任意端口段加进去再认领，
 * 于是 `fe_ioport_request` 的"认领不到就是没有"这句话不再成立。
 * 所以这里不是简单的参数校验，是**能力边界**。
 *
 * 至于"发现"本身（读 PCI 配置空间、走桥、认设备类）留在用户态：
 * 那是协议与策略。内核只做三件事：**谁在说**（授权）、
 * **说的合不合法**（对齐/范围/不与可用内存相交）、**记下来**（入池）。 */
static i64 sys_resource_pool_add(u64 kind, u64 base, u64 len, u64 shared)
{
    struct fe_task *t = fe_task_current();
    if (!t) {
        return FE_ERR_ACCESS;
    }
    if (!fe_resource_is_devmgr(t->id)) {
        fe_kprintf("[资源] 拒绝：任务 %s 不是设备管理器，不能向资源池申报硬件\n",
                   t->name);
        return FE_ERR_ACCESS;
    }
    if (len == 0) {
        return FE_ERR_INVAL;
    }
    /* 范围校验按类别分开，因为三者的合法空间完全不同：
     *   IOPORT：16 位端口空间
     *   IRQ   ：GSI 号（今天用 0..255；APIC 的实际上限由 MADT 决定）
     *   MMIO  ：物理地址，且**绝不能是可用的普通内存**——否则设备管理器
     *           可以把内核的内存当成 MMIO 送给驱动，那等于把内存保护
     *           交给了用户态。
     * 这一条是本 syscall 最重要的护栏：`sys_mmio_map` 里也有一道，
     * 但两道都要有——入口挡住比出口挡住便宜，而且错误信息更准。 */
    if (kind == FE_RES_IOPORT) {
        if (base > 0xFFFFull || base + len > 0x10000ull) {
            return FE_ERR_RANGE;
        }
    } else if (kind == FE_RES_IRQ) {
        if (base > 255ull || base + len > 256ull) {
            return FE_ERR_RANGE;
        }
    } else if (kind == FE_RES_MMIO) {
        if (len & (FE_FRAME_SIZE - 1)) {
            return FE_ERR_INVAL;        /* 必须页对齐，否则池与映射会对不上 */
        }
        if (fe_pmm_is_usable(base, len)) {
            fe_kprintf("[资源] 拒绝：设备管理器试图把**可用内存** %#llx+%#llx "
                       "当作 MMIO 申报\n", (unsigned long long)base,
                       (unsigned long long)len);
            return FE_ERR_ACCESS;
        }
    } else {
        return FE_ERR_INVAL;
    }
    u64 flags = fe_irq_save();
    fe_status_t s = shared ? fe_resource_pool_add_shared((u32)kind, base, len)
                           : fe_resource_pool_add((u32)kind, base, len);
    fe_irq_restore(flags);
    if (fe_ok(s)) {
        fe_kprintf("[资源] 设备管理器 %s 申报 %u 类资源 %#llx+%#llx%s\n",
                   t->name, (u32)kind, (unsigned long long)base,
                   (unsigned long long)len, shared ? "（共享）" : "");
    }
    return s;
}

/* 把自己持有的资源转给另一个任务（设备管理器把设备真正交给驱动）。
 *
 * ★ 为什么需要"授予"而不是让驱动自己去认领 ★
 * 如果只有"认领"，那"哪个驱动拿哪块硬件"就由**谁先跑**决定——
 * 服务启动顺序一变，硬件归属就变了。设备管理器的价值恰恰在于
 * **它来分**，所以必须有一个能表达"这块给那个任务"的原语。
 *
 * 目标用**任务句柄**而不是 id：句柄是不可伪造的能力（id 可以猜），
 * 与项目里"用句柄而不是 pid"的决定一致。 */
static i64 sys_resource_grant(u64 kind, u64 base, u64 len, u64 target_handle)
{
    struct fe_task *t = fe_task_current();
    if (!t) {
        return FE_ERR_ACCESS;
    }
    if (!fe_resource_is_devmgr(t->id)) {
        return FE_ERR_ACCESS;       /* 只有设备管理器能分发硬件 */
    }
    struct fe_object_header *obj = NULL;
    fe_status_t s = fe_handle_lookup(&t->handles, (fe_handle_t)target_handle,
                                     FE_RIGHT_TRANSFER, &obj);
    if (fe_failed(s)) {
        return s;                   /* 传进来的不是一个有效的任务句柄 */
    }
    if (obj->type != FE_OBJ_TASK) {
        return FE_ERR_INVAL;
    }
    struct fe_task *target = FE_OBJ_OF(obj, struct fe_task);
    if (target->id == t->id) {
        return FE_ERR_INVAL;
    }
    return fe_resource_grant((u32)kind, base, len, t->id, target->id);
}

/* 同 0x87，但目标是**任务 id** 而不是句柄。
 *
 * 存在理由与代价写在 fe/syscall.h 的 0x88 上。这里只有一件事必须强调：
 * **目标必须现在活着**。按 id 授予一个还不存在的任务，等于给"将来某个
 * 任务"预留了一个位置，而 id 会被复用（这与 05-ab-update 里
 * "身份随退出失效"要防的是同一件事）。所以这里查一次存活，
 * 查不到就是 FE_ERR_NOENT——不是"先记下来以后再说"。 */
static i64 sys_resource_grant_id(u64 kind, u64 base, u64 len, u64 target_id)
{
    struct fe_task *t = fe_task_current();
    if (!t) {
        return FE_ERR_ACCESS;
    }
    if (!fe_resource_is_devmgr(t->id)) {
        return FE_ERR_ACCESS;       /* 与 0x87 同一道能力边界 */
    }
    if (target_id == 0) {
        return FE_ERR_INVAL;
    }
    struct fe_task *target = fe_task_by_id(target_id);
    if (!target) {
        fe_kprintf("[资源] 拒绝：按 id 授予的目标任务 %llu 不存在\n",
                   (unsigned long long)target_id);
        return FE_ERR_NOENT;
    }
    if (target->id == t->id) {
        return FE_ERR_INVAL;
    }
    return fe_resource_grant((u32)kind, base, len, t->id, target->id);
}

/* ------------------------------------------------------------------ */
/* 任务可见性（TASK_LIST，K3 的内核侧）                                */
/* ------------------------------------------------------------------ */

/* ★ 为什么是"只读快照"而不是"内核给一个任务表句柄" ★
 *
 * 运维面（ps、设备管理器找驱动）需要的是**全系统的状态**，
 * 而不是"我持有的能力"。做成句柄就得回答"谁有权持有它"，
 * 而在能力模型里那个答案会一路膨胀：拿到任务表句柄的进程等于能看见
 * （如果再有写口就能干预）所有进程。
 *
 * 只读快照既满足"看得见"，又不产生任何新的权限——**看不见的能力
 * 也就无法被滥用**。反过来这也意味着它**不能**用来杀进程：
 * 这里只有 id，没有句柄，而"杀"需要句柄（那才是不可伪造的能力）。
 *
 * ★ 输出布局是"表头 + 定长任务记录 + 紧凑线程记录" ★
 * 不是"C 结构体数组"：内核必须能在**不知道用户态结构体布局**的前提下
 * 按固定步长填表，否则两边任何一次填充差异都会变成"字段莫名其妙是 0"
 * （这个项目已经栽过好几次，见 fe_user.h 的 FE_TASK_STRIDE 说明）。
 *
 * ★ 上限是**硬上限**，超了如实报错而不是静默截断 ★
 * 用户态传 task_cap = 64 时内核不会只填 32 条就不说话——那会让调用者
 * 以为"系统里只有 32 个任务"。header 里 total_tasks 是**真实总数**，
 * 调用者据此就知道自己看到的是不是全部。
 *
 * ★ 拼表本身在 kernel/task/tasklist.c，不在这里 ★
 * 见那个文件开头的说明：拆出去是为了让内核自检能直接测到它
 * （自检只有内核地址，走不到"往用户指针拷"这一层）。 */
#define FE_TASK_LIST_MAX   64
#define FE_THREAD_LIST_MAX 64
#define FE_TASK_INFO_BYTES  FE_TASK_STRIDE_X
#define FE_THREAD_INFO_BYTES FE_THREAD_STRIDE_X
#define FE_LIST_HDR_BYTES   FE_TASK_LIST_HDR_X

static i64 sys_task_list(u64 user_buf, u64 buf_len, u64 task_cap, u64 thread_cap)
{
    struct fe_task *self = fe_task_current();
    if (!self || !user_buf) {
        return FE_ERR_INVAL;
    }

    /* 先在**内核缓冲**里把整张表拼好，再一次性拷给用户态。
     *
     * ★ 为什么不直接往用户缓冲里写 ★
     * 那要写十几次用户内存，每次都可能失败（FAULT），而那时表已经写了一半
     * ——调用者拿到一张"一半是这次、一半是上次"的表，而它看不出来。
     * 内核缓冲 + 一次拷贝把这个窗口消掉：要么整张表过去，要么什么都不动。
     *
     * 拼表本身在 kernel/task/tasklist.c（fe_task_snapshot_build）——
     * 拆出去是为了让**内核自检能测到它**：自检手里只有内核地址，
     * 走到这个函数会被 uaccess 拒掉，于是那份代码永远没有东西跑过它。 */
    static u8 kbuf[FE_LIST_HDR_BYTES +
                    FE_TASK_LIST_MAX * FE_TASK_INFO_BYTES +
                    FE_THREAD_LIST_MAX * FE_THREAD_INFO_BYTES];

    i64 n = fe_task_snapshot_build(kbuf, buf_len, (u32)task_cap,
                                   (u32)thread_cap, self->id);
    if (n < 0) {
        return n;
    }
    u64 need = FE_LIST_HDR_BYTES +
               (u64)task_cap * FE_TASK_INFO_BYTES +
               (u64)thread_cap * FE_THREAD_INFO_BYTES;

    /* 一次拷贝。uaccess 会逐页校验：用户缓冲没映射时这里返回 FAULT，
     * 而此时内核缓冲被丢弃、用户内存一个字节都没动。 */
    fe_status_t s = fe_copy_to_user((void *)(uptr)user_buf, kbuf,
                                    (usize)(buf_len < need ? buf_len : need));
    if (fe_failed(s)) {
        return s;
    }
    return n;
}

/* 认领 / 交还设备管理器身份。只有引导者（init）能认领，且只能认领一次；
 * 规则与理由写在 fe/syscall.h 的 0x89/0x8A 与 fe/resource.h 上。 */
static i64 sys_devmgr_claim(void)
{
    struct fe_task *t = fe_task_current();
    if (!t) {
        return FE_ERR_ACCESS;
    }
    if (!fe_resource_claim_devmgr(t->id)) {
        fe_kprintf("[资源] 拒绝：任务 %s(%llu) 不能认领设备管理器"
                   "（只有引导者 init 能，且只能一次）\n",
                   t->name, (unsigned long long)t->id);
        return FE_ERR_ACCESS;
    }
    return FE_OK;
}

static i64 sys_devmgr_release(void)
{
    struct fe_task *t = fe_task_current();
    if (!t) {
        return FE_ERR_ACCESS;
    }
    if (!fe_resource_is_devmgr(t->id)) {
        return FE_ERR_ACCESS;       /* 不是设备管理器，谈不上交还 */
    }
    fe_resource_clear_devmgr(t->id);
    fe_kprintf("[资源] 任务 %s 交还设备管理器身份\n", t->name);
    return FE_OK;
}

/* 释放本任务名下的全部硬件资源所有权（区间回到池子里）。
 * 与"任务退出自动回收"走同一条路径，只是提前发生——理由见 fe/syscall.h。 */
static i64 sys_resource_release_owner(void)
{
    struct fe_task *t = fe_task_current();
    if (!t) {
        return FE_ERR_ACCESS;
    }
    fe_resource_release_owner(t->id);
    return FE_OK;
}

/* 取内存对象的物理地址（DMA 用）。
 * 只对 FE_MEM_FLAG_DMA 开放，见 fe/syscall.h 里 struct fe_mem_info 的说明。 */
static i64 sys_mem_info(u64 handle, u64 user_out)
{
    struct fe_task *t = fe_task_current();
    if (!t || !user_out) {
        return FE_ERR_INVAL;
    }
    struct fe_object_header *obj = NULL;
    fe_status_t s = fe_handle_lookup(&t->handles, (fe_handle_t)handle, FE_RIGHT_READ, &obj);
    if (fe_failed(s)) {
        return s;
    }
    if (obj->type != FE_OBJ_MEMORY) {
        return FE_ERR_INVAL;
    }
    struct fe_memory_object *mo = FE_OBJ_OF(obj, struct fe_memory_object);
    if (!(mo->flags & FE_MEM_FLAG_DMA) || mo->page_count == 0) {
        return FE_ERR_ACCESS;       /* 非 DMA 对象不暴露物理地址 */
    }
    struct fe_mem_info info;
    info.phys = mo->frames[0];
    info.size = mo->size;
    info.flags = mo->flags;
    info.page_count = (u32)mo->page_count;
    return fe_copy_to_user((void *)(uptr)user_out, &info, sizeof(info));
}

/* ------------------------------------------------------------------ */
/* M5：进程模型（学 Linux）                                            */
/* ------------------------------------------------------------------ */

/* 把用户态的 argv 数组搬进内核缓冲。
 *
 * 必须搬：字符串内容在用户地址空间里，而装载器要在**新任务**的地址空间里
 * 构造初始栈，中间还要解析路径、读 ELF——这些事都可能睡眠（将来会有），
 * 因此不能像 Linux 的 copy_strings 那样直接边算边拷用户内存。
 * 先整体拷进内核，后面就完全不碰用户内存了。
 * 上限是硬限制，超了返回 FE_ERR_RANGE 而不是截断——截断会让程序
 * 收到一个自己没传过的参数，那种 bug 比直接报错难查得多。 */
static fe_status_t copy_user_argv(u64 user_argv, u32 argc,
                                  char *buf, u64 buf_size, char *out[FE_ARGV_MAX + 1])
{
    if (argc == 0) {
        return FE_OK;
    }
    if (argc > FE_ARGV_MAX) {
        return FE_ERR_RANGE;
    }
    u64 off = 0;
    for (u32 i = 0; i < argc; i++) {
        u64 uptr_i = 0;
        fe_status_t s = fe_copy_from_user(&uptr_i, (const void *)(uptr)(user_argv + i * 8),
                                          sizeof(uptr_i));
        if (fe_failed(s)) {
            return s;
        }
        if (uptr_i == 0) {
            out[i] = NULL;
            continue;
        }
        /* 逐字节拷到 '\0'：不知道长度，只能让用户内存访问逐次校验 */
        u64 k = 0;
        for (;;) {
            if (off + k >= buf_size) {
                return FE_ERR_NAMETOOLONG;
            }
            char c = 0;
            s = fe_copy_from_user(&c, (const void *)(uptr)(uptr_i + k), 1);
            if (fe_failed(s)) {
                return s;
            }
            buf[off + k] = c;
            k++;
            if (c == '\0') {
                break;
            }
        }
        out[i] = &buf[off];
        off += k;
    }
    out[argc] = NULL;
    return FE_OK;
}

static i64 sys_process_spawn(u64 user_path, u64 user_argv, u64 argc)
{
    struct fe_task *t = fe_task_current();
    if (!t || !user_path) {
        return FE_ERR_INVAL;
    }
    char path[FE_PATH_MAX];
    fe_status_t s = fe_copy_str_from_user(path, (const char *)(uptr)user_path, sizeof(path));
    if (fe_failed(s)) {
        return s;
    }

    char arena[FE_ARGV_MAX * FE_ARG_MAX];
    char *argv[FE_ARGV_MAX + 1];
    memset(argv, 0, sizeof(argv));
    if (argc) {
        s = copy_user_argv(user_argv, (u32)argc, arena, sizeof(arena), argv);
        if (fe_failed(s)) {
            return s;
        }
    }

    fe_handle_t h = FE_HANDLE_INVALID;
    s = fe_process_spawn(t, path, argv, (u32)argc, &h);
    if (fe_failed(s)) {
        return s;
    }
    return (i64)h;
}

/* ★ K6：替换当前映像（`FE_SYS_EXEC`）★
 *
 * ★ 它是本内核第一个要**改返回帧**的系统调用 ★ 所以签名带 `struct fe_regs *r`
 * ——"哪个系统调用会改现场"必须在调用点就看得见（docs/15-exec.md §3）。
 * 参数校验与 `sys_process_spawn` **逐字复用同一套**：
 * `fe_copy_str_from_user` + `copy_user_argv`，于是 `FE_ARGV_MAX` /
 * `FE_ARG_MAX` / `FE_PATH_MAX` 只有一份定义（§5 那条"两次写同一件事必然出错"）。
 *
 * 成功时不返回：返回帧已经被 `fe_exec` 写成新映像的入口/栈/rflags。
 * 这里返回的那个 0 只会落进 `r->rax`，而新程序**不看** rax（§6.1）。 */
static i64 sys_exec(struct fe_regs *r, u64 user_path, u64 user_argv, u64 argc)
{
    struct fe_task *t = fe_task_current();
    if (!t || !user_path) {
        return FE_ERR_INVAL;
    }
    char path[FE_PATH_MAX];
    fe_status_t s = fe_copy_str_from_user(path, (const char *)(uptr)user_path,
                                          sizeof(path));
    if (fe_failed(s)) {
        return s;
    }

    char arena[FE_ARGV_MAX * FE_ARG_MAX];
    char *argv[FE_ARGV_MAX + 1];
    memset(argv, 0, sizeof(argv));
    if (argc) {
        s = copy_user_argv(user_argv, (u32)argc, arena, sizeof(arena), argv);
        if (fe_failed(s)) {
            return s;
        }
    }
    s = fe_exec(r, path, argv, (u32)argc);
    if (fe_failed(s)) {
        return s;
    }
    return FE_OK;
}

static i64 sys_process_wait(u64 task_handle, u64 user_status)
{
    struct fe_task *t = fe_task_current();
    if (!t) {
        return FE_ERR_INVAL;
    }
    i32 status = 0;
    fe_status_t s = fe_process_wait(t, (fe_handle_t)task_handle, &status);
    if (fe_failed(s)) {
        return s;
    }
    if (user_status) {
        s = fe_copy_to_user((void *)(uptr)user_status, &status, sizeof(status));
        if (fe_failed(s)) {
            return s;
        }
    }
    return FE_OK;
}

/* ------------------------------------------------------------------ */
/* devfs：服务发现                                                     */
/* ------------------------------------------------------------------ */

/* 路径字符串来自用户态，必须先拷进内核再解析。
 * 与进程装载同样的理由：拷进来之后我们不再碰用户内存，后面怎么用都安全。
 *
 * ★ 用带终止符的有界拷贝，不是"拷满 256 字节" ★
 * 后者要求路径起点之后的 256 字节全部映射着——路径字符串落在映射区末尾时
 * 明明有效却返回 FAULT（栈里的 argv[0] 就会这样）。见 fe_copy_str_from_user。 */
static fe_status_t copy_user_path(u64 user_path, char *out)
{
    if (!user_path) {
        return FE_ERR_INVAL;
    }
    return fe_copy_str_from_user(out, (const char *)(uptr)user_path, FE_PATH_MAX);
}

static i64 sys_devfs_publish(u64 user_path, u64 endpoint)
{
    struct fe_task *t = fe_task_current();
    if (!t) {
        return FE_ERR_ACCESS;
    }
    char path[FE_PATH_MAX];
    fe_status_t s = copy_user_path(user_path, path);
    if (fe_failed(s)) {
        return s;
    }
    return fe_devfs_publish(t, path, (fe_handle_t)endpoint);
}

static i64 sys_devfs_open(u64 user_path)
{
    struct fe_task *t = fe_task_current();
    if (!t) {
        return FE_ERR_ACCESS;
    }
    char path[FE_PATH_MAX];
    fe_status_t s = copy_user_path(user_path, path);
    if (fe_failed(s)) {
        return s;
    }
    fe_handle_t h = FE_HANDLE_INVALID;
    s = fe_devfs_open(t, path, &h);
    return fe_failed(s) ? s : (i64)h;
}

/* ------------------------------------------------------------------ */
/* 写保护                                                              */
/* ------------------------------------------------------------------ */

static i64 sys_protect_add(u64 lba, u64 count, u64 mode, u64 exempt)
{
    /* 不检查调用者是谁：这个接口只能让保护更严，开放给任何任务都无害。
     * 为它设计能力对象反而会引入一层"谁可信"的判断——那才是容易出错的地方。 */
    return fe_protect_add(lba, count, (u32)mode, exempt);
}

/* 访问检查。requester 是**请求方的任务 id**，由 blkd 从消息头里取来
 * （那个字段是内核在投递时填的，发送方伪造不了）。
 *
 * 为什么把 requester 当参数传、而不是内核自己推：
 * 内核看不到 IPC 的语义流向——它只知道"blkd 在问"，
 * 不知道"blkd 在为谁问"。执行者是 blkd，所以这个信息只能由它给。
 * 这也正是强制点在 blkd 的代价：一个不问的 blkd 就绕过了整张表。 */
static i64 sys_protect_check(u64 lba, u64 count, u64 is_write, u64 requester)
{
    return fe_protect_check(lba, count, is_write != 0, requester);
}

/* 把区间表交给用户态。检查程序要靠它核对访问矩阵，
 * 而不是把布局硬编码进测试——硬编码的测试只能验证"我以为的布局"。
 *
 * ★ cap = 0 是**查询真实段数**，不是错误 ★
 * 之前 cap 被悄悄夹到 64，而内核最多登记 256 段：区间表比 64 段多时，
 * 调用者拿到的是**前缀**，而它看不出来——`protcheck` 就据此算出过一个
 * "在所有受保护区间之外"的 LBA，而那一段其实在第 65 段之后。
 * 症状是"正向对照需要一个可读的探测扇区"这种看起来与写保护无关的失败。
 * 现在 cap = 0 直接回答"一共有多少段"，调用者据此判断自己有没有被截断。 */
static i64 sys_protect_list(u64 user_out, u64 cap)
{
    /* 查询模式：cap = 0 时只回答"一共有多少段"，不写任何用户内存。
     * ★ 为什么不做成"user_out 为空也算查询" ★
     * 那样两个参数就有两种含义，而"我传错了"与"内核不支持"会混在一起。
     * 一个参数表达一件事：cap = 0 是查询。 */
    if (cap == 0) {
        return (i64)fe_protect_extent_count();
    }
    if (user_out == 0) {
        return FE_ERR_INVAL;
    }
    if (cap > FE_PROT_MAX_EXTENTS) {
        cap = FE_PROT_MAX_EXTENTS;
    }
    static struct fe_protect_info info[FE_PROT_MAX_EXTENTS];
    u32 n = fe_protect_list(info, (u32)cap);
    if (n == 0) {
        return 0;
    }
    fe_status_t s = fe_copy_to_user((void *)(uptr)user_out, info,
                                    (u64)n * sizeof(info[0]));
    if (fe_failed(s)) {
        return s;
    }
    return (i64)n;
}

/* 终止一个任务（K2）。
 *
 * ★ 权限判据只有一条：句柄上有 FE_RIGHT_TERMINATE ★
 * 不用 id、不用名字——两者都可伪造（名字能改、id 会复用），
 * 而句柄 + 权限位是不可伪造的能力。谁天然有它：拉起这个任务的那个人
 * （见 fe_process_spawn）。
 *
 * ★ 为什么这里**不**禁止"终止自己" ★
 * 一个工作线程想让**整个任务**结束时需要它（它自己退只能结束一个线程）。
 * 代价要说清楚：返回值观察不到——它在返回用户态之前就被闸门拦下了。
 * 这是语义的一部分，不是缺陷；只想结束当前线程请用 FE_SYS_THREAD_EXIT。
 *
 * ★ 为什么不能终止内核任务 ★
 * 内核任务没有句柄表，也没有"被杀"这个状态该有的收尾路径
 * （它的地址空间是共享的内核页表）。给它一个 FE_ERR_ACCESS 比
 * 让它进入一个没被设计过的状态诚实。 */
static i64 sys_task_terminate(u64 task_handle)
{
    struct fe_task *self = fe_task_current();
    if (!self) {
        return FE_ERR_ACCESS;
    }
    struct fe_object_header *obj = NULL;
    fe_status_t s = fe_handle_lookup(&self->handles, (fe_handle_t)task_handle,
                                     FE_RIGHT_TERMINATE, &obj);
    if (fe_failed(s)) {
        return s;
    }
    if (obj->type != FE_OBJ_TASK) {
        return FE_ERR_INVAL;
    }
    struct fe_task *target = FE_OBJ_OF(obj, struct fe_task);
    if (!target->space && target != self) {
        /* 内核任务：不给杀。自我终止那条路仍然允许（上面那个条件把它排除掉了），
         * 因为调用者自己显然不是"别的内核任务"。 */
        return FE_ERR_ACCESS;
    }
    fe_task_terminate(target);
    return FE_OK;
}

static i64 sys_protect_stat(void)
{
    return (i64)fe_protect_violation_count();
}

/* A/B 的派生事实：槽状态位置、引导控制块位置、更新器身份、当前槽。
 * init 与更新器要写 misc / 翻控制块，但那些**位置**是从清单算出来的——
 * 让用户态自己再解析一遍清单既是重复劳动，也会引入
 * "两边解析结果不一致"这种极难查的风险。 */
static i64 sys_ab_info(u64 user_out)
{
    if (!user_out) {
        return FE_ERR_INVAL;
    }
    struct fe_ab_info info;
    fe_ab_get_info(&info);
    return fe_copy_to_user((void *)(uptr)user_out, &info, sizeof(info));
}

/* 引导命令行原样交给用户态。
 *
 * 内核只从中取过 slot=（那是它自己启动 init 要用的），其余一概不解释。
 * 这不是"偷懒"：命令行是引导器与用户态之间的通道，内核一旦开始解释它，
 * "启动策略"就搬进内核了——而 A/B 的更新触发（fek.update=1）
 * 正是一条**策略**，它该由 init 解释。
 *
 * 拷贝带上结尾的 '\0'：用户态拿到的是一个 C 字符串，而不是一段要自己
 * 补终止符的字节。装不下时明确报 NOSPC，**不截断**——
 * 截断会把"fek.update=1"变成"fek.upda"，于是更新静默不发生。 */
static i64 sys_cmdline(u64 user_out, u64 cap)
{
    if (!user_out || cap < 2) {
        return FE_ERR_INVAL;
    }
    const char *cl = fe_boot_info()->cmdline;
    u64 len = 0;
    while (cl[len] != '\0' && len < 255) {
        len++;
    }
    if (len + 1 > cap) {
        return FE_ERR_NOSPC;
    }
    return fe_copy_to_user((void *)(uptr)user_out, cl, len + 1);
}

/* 机器复位。A/B 的切换点就是重启：更新器写完另一个槽、翻完控制块之后，
 * 只有重启才会真的进新槽；试用启动失败的回滚同理。 */
static i64 sys_reboot(void)
{
    fe_kprintf("[系统] 任务 %s 请求重启\n",
               fe_task_current() ? fe_task_current()->name : "?");
    fe_machine_reset();
    return FE_OK;   /* 到不了这里 */
}

/* 帧缓冲的几何与物理地址，交给用户态的控制台服务。
 *
 * HHDM 地址减掉 HHDM 偏移就是物理地址——这里必须做这个换算，
 * 而不是把内核用的线性地址直接给出去：用户态要的是"能用 fe_mmio_map 映射的
 * 物理区间"，帧缓冲必须走关缓存的 MMIO 映射，用线性地址是错的。 */
static i64 sys_fb_info(u64 user_out)
{
    if (!user_out) {
        return FE_ERR_INVAL;
    }
    const struct fe_boot_info *bi = fe_boot_info();
    struct fe_fb_info info;
    if (!bi->has_framebuffer) {
        return FE_ERR_NOTSUP;
    }
    u64 phys = (u64)(uptr)bi->fb.address - fe_vmm_hhdm_offset();
    memset(&info, 0, sizeof(info));
    info.phys = FE_FRAME_ALIGN_DOWN(phys);
    info.size_bytes = FE_FRAME_ALIGN_UP(bi->fb.size_bytes +
                                        (phys - info.phys));
    info.width = (u32)bi->fb.width;
    info.height = (u32)bi->fb.height;
    info.pitch = (u32)bi->fb.pitch;
    info.bpp = bi->fb.bpp;
    info.memory_model = bi->fb.memory_model;
    info.red_size = bi->fb.red_mask_size;
    info.red_shift = bi->fb.red_mask_shift;
    info.green_size = bi->fb.green_mask_size;
    info.green_shift = bi->fb.green_mask_shift;
    info.blue_size = bi->fb.blue_mask_size;
    info.blue_shift = bi->fb.blue_mask_shift;
    return fe_copy_to_user((void *)(uptr)user_out, &info, sizeof(info));
}

/* ------------------------------------------------------------------ */
/* 用户态异常处理者（K5）：登记 / 注销 / 回复                            */
/* ------------------------------------------------------------------ */

/* 登记 / 注销。语义与理由见 fe/syscall.h 里那一段共享 ABI 注释。
 *
 * ★ 这个函数只做"校验用户指针 + 转发"★ 判据全在 `kernel/task/fault.c`
 * 里（与 `sys_task_terminate` 同一层）。放这里的只有两件必须在这一层做的事：
 *   1. **登记时当场校验缓冲区**：`fe_user_range_ok(space, buf, 200, true)`
 *      要求它"已映射 + 可写"。留到投递时再发现的话，异常上下文里那次
 *      `fe_copy_to_user` 会返回 FAULT，而那时能做的只有杀线程——
 *      用户态会看到"我登记成功了，但一出事就被杀"，根因在几千行之外。
 *      （这正是 docs/18 §5.3 里"用户程序取指 #PF 之后整个系统崩"那一行的
 *      前置条件：登记时的指针校验必须当场做。）
 *   2. `ep_handle == 0` 走注销，并把"本来有没有"翻译成
 *      `FE_OK` / `FE_ERR_NOENT` —— 那正是 exec 之后要问的那一句
 *      （docs/18 §6.1.5：`NOENT` 说"登记不在了"，`INVAL` 说"你参数写错了"）。 */
static i64 sys_fault_handler(u64 ep_handle, u64 user_regs)
{
    struct fe_task *t = fe_task_current();
    if (!t) {
        return FE_ERR_INVAL;
    }
    if (ep_handle == 0) {
        /* 注销：返回值回答"注销掉了一个吗"。 */
        bool had = (t->handler_ep != NULL);
        fe_fault_clear_handler(t);
        return had ? FE_OK : FE_ERR_NOENT;
    }
    if (!user_regs) {
        return FE_ERR_INVAL;
    }
    struct fe_object_header *obj = NULL;
    /* ★ 只要求"句柄指着一个端点"，**不要求任何权限位** ★
     * 登记之后内核持有的是**端点对象指针**加一次引用，投递走
     * `fe_endpoint_send_obj`（不查句柄权限，见 kernel/ipc/ipc.c 那段说明）。
     * 所以这里查权限等于发明一条"以后投递要不要权限"的假判据：
     * 用户态随后把句柄权限收窄，投递照样工作。 */
    fe_status_t s = fe_handle_lookup(&t->handles, (fe_handle_t)ep_handle, 0, &obj);
    if (fe_failed(s)) {
        return s;
    }
    if (obj->type != FE_OBJ_ENDPOINT) {
        return FE_ERR_INVAL;
    }
    /* 缓冲区必须**当场**可写。只查不拷：这一页的内容等投递时才写。 */
    if (!fe_user_range_ok(t->space, (virt_addr_t)user_regs,
                          FE_FAULT_REGS_SIZE, true)) {
        return FE_ERR_FAULT;
    }
    return fe_fault_set_handler(t, FE_OBJ_OF(obj, struct fe_endpoint),
                                user_regs);
}

/* 处理者的决定。verdict 与"两个只读字段必须回显"见 fe/syscall.h。
 *
 * ★ 为什么这里要先把 `regs` 拷进内核栈 ★
 * 回复的校验要看 `thread_id`/`fault_count` 两个字段；直接从用户指针读
 * 会让校验与拷贝分成两次用户内存访问，中间用户态（另一条线程）可以改它。
 * 拷一次、校验与落盘都用内核里那一份，这个窗口就没有了。 */
static i64 sys_fault_reply(u64 verdict, u64 user_regs)
{
    struct fe_task *t = fe_task_current();
    if (!t) {
        return FE_ERR_INVAL;
    }
    if (!user_regs) {
        return FE_ERR_INVAL;
    }
    /* ★ "没有登记过处理者"要报 NOENT，而不是 INVAL ★
     * 这是 exec 之后唯一可观察的后果：新映像问一句"我还有处理者吗"。
     * 两者对用户的含义不同，不能混（docs/18 §6.1.5 第 2 条）。
     * 注意这一条必须排在 `fe_fault_reply` 之前：那个函数对
     * "没有正在等的投递"与"登记不在了"都只能返回 INVAL。 */
    if (!t->handler_ep) {
        return FE_ERR_NOENT;
    }
    struct fe_fault_regs regs;
    fe_status_t s = fe_copy_from_user(&regs, (const void *)(uptr)user_regs,
                                      sizeof(regs));
    if (fe_failed(s)) {
        return s;
    }
    return fe_fault_reply(t, fe_thread_current(), &regs, verdict);
}

/* ------------------------------------------------------------------ */
/* 分发                                                                */
/* ------------------------------------------------------------------ */

void fe_syscall_dispatch(struct fe_regs *r)
{
    u64 num = r->rax;
    u64 a1 = r->rdi, a2 = r->rsi, a3 = r->rdx;
    u64 a4 = r->r10, a5 = r->r8, a6 = r->r9;
    i64 ret;

    switch (num) {
    case FE_SYS_DEBUG_WRITE:
        ret = sys_debug_write((const char *)(uptr)a1, a2);
        break;
    case FE_SYS_THREAD_EXIT:
        ret = sys_thread_exit((i32)a1);
        break;
    case FE_SYS_THREAD_YIELD:
        fe_thread_yield();
        ret = FE_OK;
        break;
    case FE_SYS_THREAD_CREATE:
        ret = sys_thread_create(a1, a2, a3, a4, a5);
        break;
    case FE_SYS_SLEEP:
        ret = sys_sleep_ns(a1);
        break;
    case FE_SYS_CLOCK_MONOTONIC:
        ret = sys_clock_monotonic();
        break;
    case FE_SYS_CLOCK_INFO:
        ret = sys_clock_info(a1);
        break;
    case FE_SYS_IRQ_MSI_ALLOC:
        ret = sys_irq_msi_alloc(a1, a2, a3, a4, a5, a6);
        break;
    case FE_SYS_IRQ_MSI_FREE:
        ret = sys_irq_msi_free(a1);
        break;
    case FE_SYS_MEM_ALLOC:
        ret = sys_mem_alloc(a1, a2);
        break;
    case FE_SYS_MEM_MAP:
        ret = sys_mem_map(a1, a2, a3, a4);
        break;
    case FE_SYS_MEM_UNMAP:
        ret = sys_mem_unmap(a1, a2);
        break;
    case FE_SYS_MEM_PROTECT:
        ret = sys_mem_protect(a1, a2, a3);
        break;
    case FE_SYS_HANDLE_CLOSE:
        ret = sys_handle_close(a1);
        break;
    case FE_SYS_HANDLE_DUP:
        ret = sys_handle_dup(a1, a2);
        break;
    case FE_SYS_ENDPOINT_CREATE:
        ret = sys_endpoint_create(a1);
        break;
    case FE_SYS_ENDPOINT_SEND:
        ret = sys_endpoint_send(a1, a2, a3, a4, a5);
        break;
    case FE_SYS_ENDPOINT_RECV:
        ret = sys_endpoint_recv(a1, a2, a3, a4, a5, a6);
        break;
    case FE_SYS_ENDPOINT_CALL:
        ret = sys_endpoint_call(a1, a2, a3, a4, a5, a6);
        break;
    case FE_SYS_MEM_INFO:
        ret = sys_mem_info(a1, a2);
        break;
    case FE_SYS_NOTIFICATION_CREATE:
        ret = sys_notification_create();
        break;
    case FE_SYS_NOTIFICATION_WAIT:
        ret = sys_notification_wait(a1, a2, a3);
        break;
    case FE_SYS_NOTIFICATION_SIGNAL:
        ret = fe_notification_signal((fe_handle_t)a1, a2);
        break;
    case FE_SYS_IOPORT_REQUEST:
        ret = sys_ioport_request(a1, a2);
        break;
    case FE_SYS_MMIO_MAP:
        ret = sys_mmio_map(a1, a2, a3);
        break;
    case FE_SYS_IRQ_REGISTER:
        ret = sys_irq_register(a1, a2, a3, a4);
        break;
    case FE_SYS_IRQ_ACK:
        ret = sys_irq_ack(a1);
        break;
    case FE_SYS_PROCESS_SPAWN:
        ret = sys_process_spawn(a1, a2, a3);
        break;
    case FE_SYS_EXEC:
        /* ★ 唯一一个**拿到返回帧**的分发分支 ★ 它可能改 r->rip/rsp/rflags，
         * 于是这一次 syscall 的返回落在新映像的入口上（成功时不返回）。
         * 与其它分支的区别必须在这里看得见——这正是签名带 r 的理由。 */
        ret = sys_exec(r, a1, a2, a3);
        break;
    case FE_SYS_PROCESS_WAIT:
        ret = sys_process_wait(a1, a2);
        break;
    case FE_SYS_RESOURCE_LOCK:
        ret = sys_resource_lock(a1, a2, a3);
        break;
    case FE_SYS_RESOURCE_UNLOCK:
        ret = sys_resource_unlock(a1, a2, a3);
        break;
    case FE_SYS_RESOURCE_POOL_ADD:
        ret = sys_resource_pool_add(a1, a2, a3, a4);
        break;
    case FE_SYS_RESOURCE_GRANT:
        ret = sys_resource_grant(a1, a2, a3, a4);
        break;
    case FE_SYS_RESOURCE_GRANT_ID:
        ret = sys_resource_grant_id(a1, a2, a3, a4);
        break;
    case FE_SYS_DEVMGR_CLAIM:
        ret = sys_devmgr_claim();
        break;
    case FE_SYS_DEVMGR_RELEASE:
        ret = sys_devmgr_release();
        break;
    case FE_SYS_RESOURCE_RELEASE:
        ret = sys_resource_release_owner();
        break;
    case FE_SYS_TASK_LIST:
        ret = sys_task_list(a1, a2, a3, a4);
        break;
    case FE_SYS_TASK_TERMINATE:
        ret = sys_task_terminate(a1);
        break;
    case FE_SYS_DEVFS_PUBLISH:
        ret = sys_devfs_publish(a1, a2);
        break;
    case FE_SYS_DEVFS_OPEN:
        ret = sys_devfs_open(a1);
        break;
    case FE_SYS_PROTECT_ADD:
        ret = sys_protect_add(a1, a2, a3, a4);
        break;
    case FE_SYS_PROTECT_CHECK:
        ret = sys_protect_check(a1, a2, a3, a4);
        break;
    case FE_SYS_PROTECT_LIST:
        ret = sys_protect_list(a1, a2);
        break;
    case FE_SYS_PROTECT_STAT:
        ret = sys_protect_stat();
        break;
    case FE_SYS_AB_INFO:
        ret = sys_ab_info(a1);
        break;
    case FE_SYS_CMDLINE:
        ret = sys_cmdline(a1, a2);
        break;
    case FE_SYS_REBOOT:
        ret = sys_reboot();
        break;
    case FE_SYS_FB_INFO:
        ret = sys_fb_info(a1);
        break;
    case FE_SYS_PF_STAT:
        /* 按需分页累计解析次数。诊断用，无参数、无失败路径。 */
        ret = (i64)fe_pf_resolved_count();
        break;
    case FE_SYS_FAULT_HANDLER:
        ret = sys_fault_handler(a1, a2);
        break;
    case FE_SYS_FAULT_REPLY:
        ret = sys_fault_reply(a1, a2);
        break;
    default:
        fe_kprintf("[syscall] 未知调用号 %llu (线程 %s)\n",
                   (unsigned long long)num,
                   fe_thread_current() ? fe_thread_current()->name : "?");
        ret = FE_ERR_NOTSUP;
        break;
    }

    r->rax = (u64)ret;
}
