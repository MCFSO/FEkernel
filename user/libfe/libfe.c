/* SPDX-License-Identifier: 0BSD */
/* libfe：系统调用封装与最小运行库实现。 */
#include <fe_user.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* 系统调用                                                            */
/* ------------------------------------------------------------------ */

long fe_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = a6;
    long ret;
    /* rcx 与 r11 被 syscall 指令本身破坏，已在 ABI 中声明 */
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

/* ------------------------------------------------------------------ */
/* 控制台输出：行缓冲                                                  */
/* ------------------------------------------------------------------ */

/* 控制台是**多个服务共用**的，而每次 fe_write 是一次独立的系统调用。
 * 逐段打印的一行日志会被另一个服务的输出从中间劈开——实测出现过
 *   [mouse] 设采样率 100Hz (0xF3 0x64) -> fe  （无 ACK）
 * 其中那个 `fe` 其实是键盘服务打印的应答字节。
 *
 * 做法：把一行攒齐，遇到换行（或缓冲满）才真正发一次系统调用，
 * 于是**行内是原子的**。这一层放在用户态而不是内核，是因为
 * 「什么算一行」是输出方的事，内核不该替应用猜。 */
#define LINE_BUF_SIZE 160
static char  g_line[LINE_BUF_SIZE];
static usize g_line_len;

static void raw_write(const char *buf, usize len)
{
    fe_syscall(FE_SYS_DEBUG_WRITE, (long)buf, (long)len, 0, 0, 0, 0);
}

void fe_flush(void)
{
    if (g_line_len) {
        raw_write(g_line, g_line_len);
        g_line_len = 0;
    }
}

/* 缓冲满了就先发出去再继续写，所以**不会丢字符**——
 * 只有「行内原子」这一条在超长行上退化成「分几次发」。 */
static void line_putc(char c)
{
    if (c == '\n') {
        if (g_line_len >= LINE_BUF_SIZE) {
            fe_flush();
        }
        g_line[g_line_len++] = '\n';
        fe_flush();
        return;
    }
    if (g_line_len >= LINE_BUF_SIZE) {
        fe_flush();
    }
    g_line[g_line_len++] = c;
}

/* 公开的 fe_write 也走行缓冲，这样顺序不会乱 */
long fe_write(const char *buf, usize len)
{
    if (!buf) {
        return FE_ERR_INVAL;
    }
    for (usize i = 0; i < len; i++) {
        line_putc(buf[i]);
    }
    return (long)len;
}

void fe_flush(void);

__attribute__((noreturn)) void fe_exit(int code)
{
    /* 退出前必须把行缓冲吐出去：否则最后一行（很可能是「失败项 N」这种结论）
     * 会随着进程一起消失，看起来就像程序没跑完。 */
    fe_flush();
    fe_syscall(FE_SYS_THREAD_EXIT, code, 0, 0, 0, 0, 0);
    for (;;) {
    }       /* 正常情况下不会到达 */
}

void fe_yield(void)
{
    fe_syscall(FE_SYS_THREAD_YIELD, 0, 0, 0, 0, 0, 0);
}

u64 fe_thread_create(void (*entry)(void *), void *arg, void *stack, usize stack_size)
{
    return (u64)fe_syscall(FE_SYS_THREAD_CREATE, (long)entry, (long)arg, (long)stack,
                           (long)stack_size, 0, 0);
}

void fe_sleep_ms(u64 ms)
{
    fe_syscall(FE_SYS_SLEEP, (long)(ms * 1000000ull), 0, 0, 0, 0, 0);
}

u64 fe_clock_ns(void)
{
    return (u64)fe_syscall(FE_SYS_CLOCK_MONOTONIC, 0, 0, 0, 0, 0, 0);
}

/* 时钟自述（K7）：让调用者能判断"这个时钟够不够细"，而不是假设它够细。
 * 见 fe_user.h 里 fe_clock_ns 的说明——D4 就是栽在这个假设上的。 */
long fe_clock_info(struct fe_clock_info *out)
{
    if (!out) {
        return FE_ERR_INVAL;
    }
    return fe_syscall(FE_SYS_CLOCK_INFO, (long)out, 0, 0, 0, 0, 0);
}

long fe_mem_alloc(u64 size)
{
    return fe_syscall(FE_SYS_MEM_ALLOC, (long)size, 0, 0, 0, 0, 0);
}

void *fe_mem_map(long handle, void *hint, u64 size, u32 prot)
{
    long r = fe_syscall(FE_SYS_MEM_MAP, handle, (long)hint, (long)size, (long)prot, 0, 0);
    return (r < 0) ? (void *)0 : (void *)(usize)r;
}

long fe_mem_unmap(void *addr, u64 size)
{
    return fe_syscall(FE_SYS_MEM_UNMAP, (long)addr, (long)size, 0, 0, 0, 0);
}

long fe_mem_protect(void *addr, u64 size, u32 prot)
{
    return fe_syscall(FE_SYS_MEM_PROTECT, (long)addr, (long)size, (long)prot,
                      0, 0, 0);
}

long fe_handle_close(long handle)
{
    return fe_syscall(FE_SYS_HANDLE_CLOSE, handle, 0, 0, 0, 0, 0);
}

long fe_handle_dup(long handle, u32 new_rights)
{
    return fe_syscall(FE_SYS_HANDLE_DUP, handle, (long)new_rights, 0, 0, 0, 0);
}

long fe_endpoint_create(u32 flags)
{
    return fe_syscall(FE_SYS_ENDPOINT_CREATE, (long)flags, 0, 0, 0, 0, 0);
}

long fe_endpoint_send(long ep, const struct fe_msg_header *hdr, const void *payload,
                      const u32 *handles, u32 handle_count)
{
    return fe_syscall(FE_SYS_ENDPOINT_SEND, ep, (long)hdr, (long)payload,
                      (long)handle_count, (long)handles, 0);
}

long fe_endpoint_recv(long ep, struct fe_msg_header *hdr, void *payload, u32 cap,
                      u32 *out_handles, u32 *out_count)
{
    return fe_syscall(FE_SYS_ENDPOINT_RECV, ep, (long)hdr, (long)payload, (long)cap,
                      (long)out_handles, (long)out_count);
}

long fe_endpoint_call(long ep, const void *req, u32 req_len, void *rep, u32 rep_cap,
                      u32 *out_len)
{
    return fe_syscall(FE_SYS_ENDPOINT_CALL, ep, (long)req, (long)req_len, (long)rep,
                      (long)rep_cap, (long)out_len);
}

/* ------------------------------------------------------------------ */
/* 驱动能力（M6）                                                      */
/* ------------------------------------------------------------------ */

long fe_ioport_request(u64 base, u64 count)
{
    return fe_syscall(FE_SYS_IOPORT_REQUEST, (long)base, (long)count, 0, 0, 0, 0);
}

void *fe_mmio_map(u64 phys, u64 size, u32 prot)
{
    long r = fe_syscall(FE_SYS_MMIO_MAP, (long)phys, (long)size, (long)prot, 0, 0, 0);
    return (r < 0) ? (void *)0 : (void *)(usize)r;
}

long fe_irq_register(u64 irq, long notif_handle)
{
    return fe_syscall(FE_SYS_IRQ_REGISTER, (long)irq, notif_handle, 0, 0, 0, 0);
}

/* 带触发方式要求与实情回填的版本（D5b）。见 fe_user.h 里那段说明。 */
long fe_irq_register_ex(u64 irq, long notif_handle, u32 require_mode,
                        struct fe_irq_info *out_info)
{
    return fe_syscall(FE_SYS_IRQ_REGISTER, (long)irq, notif_handle,
                      (long)require_mode, (long)out_info, 0, 0);
}

long fe_irq_ack(u64 irq)
{
    return fe_syscall(FE_SYS_IRQ_ACK, (long)irq, 0, 0, 0, 0, 0);
}

/* MSI / MSI-X（D5c）。见 fe_user.h 里那段说明（用法、分工、安全边界）。 */
long fe_irq_msi_alloc(long notif_handle, u32 bus, u32 dev, u32 fn, u32 cap_off,
                      struct fe_msi_info *out)
{
    return fe_syscall(FE_SYS_IRQ_MSI_ALLOC, notif_handle, (long)bus, (long)dev,
                      (long)fn, (long)cap_off, (long)out);
}

long fe_irq_msi_free(u32 irq)
{
    return fe_syscall(FE_SYS_IRQ_MSI_FREE, (long)irq, 0, 0, 0, 0, 0);
}

long fe_mem_alloc_dma(u64 size)
{
    return fe_syscall(FE_SYS_MEM_ALLOC, (long)size, FE_MEM_FLAG_DMA, 0, 0, 0, 0);
}

long fe_notification_create(void)
{
    return fe_syscall(FE_SYS_NOTIFICATION_CREATE, 0, 0, 0, 0, 0, 0);
}

long fe_notification_wait(long handle, u64 mask, u64 *out_bits)
{
    return fe_syscall(FE_SYS_NOTIFICATION_WAIT, handle, (long)mask, (long)out_bits,
                      0, 0, 0);
}

long fe_notification_signal(long handle, u64 bits)
{
    return fe_syscall(FE_SYS_NOTIFICATION_SIGNAL, handle, (long)bits, 0, 0, 0, 0);
}

long fe_mem_info(long handle, struct fe_mem_info *out)
{
    return fe_syscall(FE_SYS_MEM_INFO, handle, (long)out, 0, 0, 0, 0);
}

/* 端口 I/O：权限由内核写在 TSS 的 I/O 位图里，CPU 逐次检查。
 * 这里不做任何软件检查——软件检查可以被绕过，位图不行。 */
unsigned char fe_inb(unsigned short port)
{
    unsigned char v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

void fe_outb(unsigned short port, unsigned char val)
{
    __asm__ volatile("outb %0, %1" ::"a"(val), "Nd"(port));
}

unsigned short fe_inw(unsigned short port)
{
    unsigned short v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

void fe_outw(unsigned short port, unsigned short val)
{
    __asm__ volatile("outw %0, %1" ::"a"(val), "Nd"(port));
}

unsigned int fe_inl(unsigned short port)
{
    unsigned int v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

void fe_outl(unsigned short port, unsigned int val)
{
    __asm__ volatile("outl %0, %1" ::"a"(val), "Nd"(port));
}

/* ------------------------------------------------------------------ */
/* 进程模型（M5）                                                      */
/* ------------------------------------------------------------------ */

long fe_spawn(const char *path, char *const argv[], u32 argc)
{
    return fe_syscall(FE_SYS_PROCESS_SPAWN, (long)path, (long)argv, (long)argc, 0, 0, 0);
}

/* ★ 成功时**不返回**（内核改的是这次 syscall 的返回帧）★
 * 走到 `return` 那一行只可能是失败。包装形式与 fe_spawn 一样，
 * 语义完全不同，所以把"不返回"写在代码旁边而不是只写在头文件里。 */
long fe_exec(const char *path, char *const argv[], u32 argc)
{
    return fe_syscall(FE_SYS_EXEC, (long)path, (long)argv, (long)argc, 0, 0, 0);
}

long fe_wait(long task_handle, int *out_status)
{
    return fe_syscall(FE_SYS_PROCESS_WAIT, task_handle, (long)out_status, 0, 0, 0, 0);
}

long fe_resource_lock(u32 kind, u64 base, u64 len)
{
    return fe_syscall(FE_SYS_RESOURCE_LOCK, (long)kind, (long)base, (long)len, 0, 0, 0);
}

long fe_resource_unlock(u32 kind, u64 base, u64 len)
{
    return fe_syscall(FE_SYS_RESOURCE_UNLOCK, (long)kind, (long)base, (long)len, 0, 0, 0);
}

/* 设备管理器专用：向资源池申报一段硬件 / 把名下的资源授予某个任务。
 * 非设备管理器调用会被内核拒绝（FE_ERR_ACCESS）——这是能力边界，不是参数检查。 */
long fe_resource_pool_add(u32 kind, u64 base, u64 len, int shared)
{
    return fe_syscall(FE_SYS_RESOURCE_POOL_ADD, (long)kind, (long)base, (long)len,
                      (long)shared, 0, 0);
}

long fe_resource_grant(u32 kind, u64 base, u64 len, long target_task_handle)
{
    return fe_syscall(FE_SYS_RESOURCE_GRANT, (long)kind, (long)base, (long)len,
                      target_task_handle, 0, 0);
}

/* 同上，但按**任务 id** 授予。用于"驱动不是设备管理器拉起的"那种引导链
 * （句柄在 init 手里）。详见 kernel/include/fe/syscall.h 的 0x88。 */
long fe_resource_grant_id(u32 kind, u64 base, u64 len, u64 target_task_id)
{
    return fe_syscall(FE_SYS_RESOURCE_GRANT_ID, (long)kind, (long)base, (long)len,
                      (long)target_task_id, 0, 0);
}

/* 把自己标记为设备管理器 / 解除。
 *
 * ★ 为什么这件事由**任务自己**声明，而不是内核按名字指定 ★
 * 内核不认识"devmgr"这个名字（那是策略）。引导链的决定是：
 * **init 是引导者，所以它有权把设备管理器身份交给自己**
 * ——只允许在 init 还没有拉起任何服务时调用（由 init 自己保证顺序），
 * 内核只记"是它"。这与 A/B 更新器身份（fe_protect_note_process）同构。 */
long fe_devmgr_claim(void)
{
    return fe_syscall(FE_SYS_DEVMGR_CLAIM, 0, 0, 0, 0, 0, 0);
}

long fe_devmgr_release(void)
{
    return fe_syscall(FE_SYS_DEVMGR_RELEASE, 0, 0, 0, 0, 0, 0);
}

/* 释放本任务名下的**全部**硬件资源所有权（区间重新回到池子里）。
 *
 * ★ 它为什么有用（而不是"等任务退出自动回收"就够了）★
 * 任务退出时确实会自动回收，但**引导链上有中间点**：init 探测完 BAR4 之后
 * 就不再需要 PCI 配置空间端口，而 pcid 单元随后要拿它做枚举。
 * 不放回去，pcid 就会失败——而"发现"这条链不能因为"init 顺手多用了一会儿"
 * 而断掉。语义与"任务退出"完全一致（走同一条回收路径），只是提前发生。
 *
 * 注意它**不影响设备管理器身份**：身份是"谁能申报"，所有权是"谁拿着硬件"，
 * 两者是两件事（见 fe/resource.h）。 */
long fe_resource_release_owner(void)
{
    return fe_syscall(FE_SYS_RESOURCE_RELEASE, 0, 0, 0, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* 服务发现（M7）                                                      */
/* ------------------------------------------------------------------ */

long fe_devfs_publish(const char *path, long endpoint_handle)
{
    return fe_syscall(FE_SYS_DEVFS_PUBLISH, (long)path, endpoint_handle, 0, 0, 0, 0);
}

long fe_devfs_open(const char *path)
{
    return fe_syscall(FE_SYS_DEVFS_OPEN, (long)path, 0, 0, 0, 0, 0);
}

/* 终止一个任务（K2）。语义见 fe_user.h 那段说明：异步、可能需要 wait 收尾。 */
long fe_task_terminate(long task_handle)
{
    return fe_syscall(FE_SYS_TASK_TERMINATE, task_handle, 0, 0, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* 任务/线程快照（TASK_LIST）                                          */
/* ------------------------------------------------------------------ */

long fe_task_list(void *buf, u32 buf_len, u32 task_cap, u32 thread_cap)
{
    return fe_syscall(FE_SYS_TASK_LIST, (long)buf, (long)buf_len,
                      (long)task_cap, (long)thread_cap, 0, 0);
}

/* 取一次快照并做**结构自洽检查**。
 *
 * ★ 为什么用户态要自己再检查一遍长度 ★
 * 内核返回的 task_count/thread_count 是"实际写进去的"，
 * 而"能读多少条记录"由**我们自己要了多少**决定。两者不一致时按较小的
 * 那个走——否则一个被截断的快照会让 at() 越界读到缓冲之外，
 * 而那种越界在用户态是 #PF，不是错误码。
 *
 * 步长用编译期常量（FE_TASK_STRIDE / FE_THREAD_STRIDE）而不是
 * sizeof(struct ...)：这两者必须相等，而不相等时**宁可按常量走**
 * ——常量是与内核约定的那一个，sizeof 只反映本地编译器。 */
long fe_task_view_get(struct fe_task_view *out, void *buf, u32 buf_len,
                      u32 task_cap, u32 thread_cap)
{
    if (!out || !buf || task_cap == 0) {
        return FE_ERR_INVAL;
    }
    u64 need = (u64)sizeof(struct fe_task_list) +
               (u64)task_cap * FE_TASK_STRIDE +
               (u64)thread_cap * FE_THREAD_STRIDE;
    if ((u64)buf_len < need) {
        return FE_ERR_NOSPC;
    }
    long n = fe_task_list(buf, buf_len, task_cap, thread_cap);
    if (n < 0) {
        return n;
    }
    const struct fe_task_list *h = (const struct fe_task_list *)buf;
    out->hdr = h;
    out->raw = (const u8 *)buf;
    out->raw_len = buf_len;
    return n;
}

const struct fe_task_info *fe_task_at(const struct fe_task_view *v, u32 i)
{
    if (!v || !v->hdr || i >= v->hdr->task_count) {
        return NULL;
    }
    /* 记录在表头之后，步长是**约定常量**；越界由调用方用 task_count 挡住 */
    return (const struct fe_task_info *)(const void *)
           (v->raw + sizeof(struct fe_task_list) + (usize)i * FE_TASK_STRIDE);
}

const struct fe_thread_info *fe_thread_at(const struct fe_task_view *v, u32 i)
{
    if (!v || !v->hdr || i >= v->hdr->thread_count) {
        return NULL;
    }
    const u8 *base = v->raw + sizeof(struct fe_task_list) +
                     (usize)v->hdr->tasks_cap * FE_TASK_STRIDE;
    return (const struct fe_thread_info *)(const void *)
           (base + (usize)i * FE_THREAD_STRIDE);
}

const struct fe_task_info *fe_task_find(const struct fe_task_view *v, const char *name)
{
    if (!v || !v->hdr || !name) {
        return NULL;
    }
    for (u32 i = 0; i < v->hdr->task_count; i++) {
        const struct fe_task_info *t = fe_task_at(v, i);
        if (t && strcmp(t->name, name) == 0) {
            return t;
        }
    }
    return NULL;
}

/* 线程状态的可读名字。
 *
 * ★ 为什么用户态这份是 ASCII，而内核那份是中文 ★
 * 内核的 `fe_thread_state_name` 打在串口日志上，读的是我；
 * 而这份会**画到屏幕上**，屏幕用的是自绘的 5x7 点阵字体（95 个字形，
 * 只有可打印 ASCII）。所以这一份必须是 ASCII——这不是风格选择，
 * 是"屏幕画不出别的"这个事实。
 *
 * ★ 为什么表里要带编号注释 ★
 * 数值来自共享 ABI（fe_user.h 的 FE_THREAD_* 常量，与内核
 * enum fe_thread_state 一一对应）。写成"按数组下标取"最省事，
 * 但那会让"内核加了一个状态、用户态没跟上"表现成**读到别人的名字**
 * ——所以这里显式列出编号，漏了就是显式的空洞。 */
const char *fe_thread_state_str(u32 state)
{
    switch (state) {
    case FE_THREAD_UNUSED:   return "unused";
    case FE_THREAD_READY:    return "ready";
    case FE_THREAD_RUNNING:  return "running";
    case FE_THREAD_SLEEPING: return "sleep";
    case FE_THREAD_BLOCKED:  return "blocked";
    case FE_THREAD_DEAD:     return "dead";
    default:                 return "?";
    }
}

/* ------------------------------------------------------------------ */
/* 扇区访问矩阵                                                        */
/* ------------------------------------------------------------------ */

long fe_protect_add(u64 lba, u64 count, u32 mode, u64 exempt_task)
{
    return fe_syscall(FE_SYS_PROTECT_ADD, (long)lba, (long)count,
                      (long)mode, (long)exempt_task, 0, 0);
}

long fe_protect_check(u64 lba, u64 count, int is_write, u64 requester_task)
{
    return fe_syscall(FE_SYS_PROTECT_CHECK, (long)lba, (long)count,
                      (long)is_write, (long)requester_task, 0, 0);
}

/* cap = 0 → 查询真实段数（见 fe_user.h 的说明：这是发现"被截断"的唯一手段）。 */
long fe_protect_list(struct fe_protect_info *out, u32 cap)
{
    return fe_syscall(FE_SYS_PROTECT_LIST, (long)out, (long)cap, 0, 0, 0, 0);
}

u64 fe_protect_violations(void)
{
    return (u64)fe_syscall(FE_SYS_PROTECT_STAT, 0, 0, 0, 0, 0, 0);
}

long fe_ab_get_info(struct fe_ab_info *out)
{
    return fe_syscall(FE_SYS_AB_INFO, (long)out, 0, 0, 0, 0, 0);
}

long fe_cmdline(char *out, u32 cap)
{
    return fe_syscall(FE_SYS_CMDLINE, (long)out, (long)cap, 0, 0, 0, 0);
}

long fe_fb_get_info(struct fe_fb_info *out)
{
    return fe_syscall(FE_SYS_FB_INFO, (long)out, 0, 0, 0, 0, 0);
}

/* 按需分页累计解析的缺页次数（诊断）。
 * ★ 它是"栈真的在按需增长"的唯一证据 ★ 只看"程序没崩"分不出
 * "缺页被补上了"与"一开始就全映射好了"——两者行为完全一样。 */
u64 fe_pf_resolved_count(void)
{
    return (u64)fe_syscall(FE_SYS_PF_STAT, 0, 0, 0, 0, 0, 0);
}

void fe_reboot(void)
{
    fe_syscall(FE_SYS_REBOOT, 0, 0, 0, 0, 0, 0);    /* 走到这里说明复位没有生效。**不要装作成功**：
     * 调用者（init 的更新/回滚路径）依赖"不返回"这个性质，
     * 所以这里也不能 return 一个"看起来像成功"的结果。
     * 打印并停机，让人能看到发生了什么。 */
    fe_puts("\n[libfe] **重启系统调用返回了**，说明复位没有生效；停机\n");
    fe_flush();
    for (;;) {
        fe_yield();
    }
}

/* CRC-32（IEEE 802.3），按位算。
 *
 * 只用来校验槽状态记录那 12 个字节，调用频率是"每次启动一次"，
 * 所以不需要查表版本——**为了一次调用去背一张 1 KiB 的表不值**。
 * 这不是密码学哈希：它只防"写入被打断"，不防"有人故意改"。
 * 防后者要靠签名，那是另一件事（见 docs/04-write-protection.md）。 */
u32 fe_crc32(const void *data, u32 len)
{
    const u8 *p = (const u8 *)data;
    u32 crc = 0xFFFFFFFFu;
    for (u32 i = 0; i < len; i++) {
        crc ^= p[i];
        for (u32 b = 0; b < 8; b++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ */
/* 槽前缀（A/B）                                                       */
/* ------------------------------------------------------------------ */

/* 从 argv[0] 推出"我在哪个槽"的前缀，例如 "/slot_a"。
 *
 * 为什么不问内核：**答案就写在程序自己的名字里**。内核 exec 的是
 * "/slot_<x>/..."，那个路径原样进了 argv[0]。
 *
 * ★ 规则是"第一个以 slot_ 开头的路径分量"，不是"去掉最后一段" ★
 * 我第一版用的是后者，结果 init（在 /slot_a/init，一层深）得到 "/slot_a"，
 * 而 fs（在 /slot_a/bin/fs，两层深）得到 "/slot_a/bin"，
 * 于是它去列 "/slot_a/bin/bin" ——路径被拼重了。
 * 槽根在目录树里的**深度是固定的**（永远是顶层），所以要按名字找，不能按深度截。
 *
 * 这也是"槽做成路径前缀"这个选择带来的直接好处：
 * 一个需要"问别人我在哪"的设计，在别人不可信时就没有答案了；
 * 而路径不用问——它是我自己名字的一部分。
 * 代价是这里有了一条命名约定（顶层目录名以 slot_ 开头），仅此而已。 */
void fe_slot_prefix(const char *arg0, char *out, u32 cap)
{
    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (!arg0 || arg0[0] != '/') {
        return;                     /* 没有槽（单槽布局）：前缀为空 */
    }
    const char *p = arg0 + 1;
    while (*p) {
        const char *start = p;
        while (*p && *p != '/') {
            p++;
        }
        u32 n = (u32)(p - start);
        if (n >= 5 && start[0] == 's' && start[1] == 'l' && start[2] == 'o' &&
            start[3] == 't' && start[4] == '_') {
            u32 k = 0;
            if (k < cap - 1) {
                out[k++] = '/';
            }
            for (u32 i = 0; i < n && k < cap - 1; i++) {
                out[k++] = start[i];
            }
            out[k] = '\0';
            return;
        }
        while (*p == '/') {
            p++;
        }
    }
}

/* 把槽内相对路径补上前缀："/bin/x" + "/slot_a" → "/slot_a/bin/x" */
void fe_slot_path(const char *prefix, const char *rel, char *out, u32 cap)
{
    if (!out || cap == 0) {
        return;
    }
    u32 k = 0;
    if (prefix) {
        for (const char *p = prefix; *p && k < cap - 1; p++) {
            out[k++] = *p;
        }
    }
    if (rel) {
        for (const char *p = rel; *p && k < cap - 1; p++) {
            out[k++] = *p;
        }
    }
    out[k] = '\0';
}

/* ------------------------------------------------------------------ */
/* 最小运行库                                                          */
/* ------------------------------------------------------------------ */

/* 拷贝：SSE2 版本（16 字节/次）。
 *
 * 历史：第一版逐字节（约 2.3 字节/周期），第二版按 8 字节字长（约 6 倍）。
 * 现在用户态能执行 SIMD 了（见 kernel/arch/x86_64/fpu.c），
 * 步长提到 16 字节——**这不是"顺手优化"，而是把之前被迫放弃的宽度拿回来**：
 * 当时 `-mgeneral-regs-only` 让 8 字节成为实际天花板。
 *
 * 仍然不假设**源**对齐（只要求目的 16 字节对齐后走对齐路径），
 * 因为 IPC 载荷、字符串这类源地址本来就可能是任意的。
 * 用 __builtin_memcpy 搬 16 字节块，让编译器选 movups/movaps——
 * 手写 _mm_loadu_si128 也等价，但内置函数在编译器眼里是"一次访问"，
 * 别名分析更准确。 */
void *memcpy(void *dst, const void *src, usize n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;

    /* 先把目的地址对齐到 16 字节（源可能仍未对齐，所以这一段用未对齐搬） */
    while (n && ((usize)d & 15u)) {
        *d++ = *s++;
        n--;
    }
    /* 主体：一次 16 字节。用 builtin 让编译器生成 movups（源未对齐时安全） */
    while (n >= 16) {
        __builtin_memcpy(d, s, 16);
        d += 16;
        s += 16;
        n -= 16;
    }
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memset(void *dst, int c, usize n)
{
    u8 *d = (u8 *)dst;
    u8 v = (u8)c;

    while (n && ((usize)d & 15u)) {
        *d++ = v;
        n--;
    }
    if (n >= 16) {
        /* 16 字节的重复模式：SSE2 下编译器会把它变成一条广播 + 定宽存储 */
        u8 pattern[16];
        for (u32 i = 0; i < 16; i++) {
            pattern[i] = v;
        }
        while (n >= 16) {
            __builtin_memcpy(d, pattern, 16);
            d += 16;
            n -= 16;
        }
    }
    while (n--) {
        *d++ = v;
    }
    return dst;
}

int memcmp(const void *a, const void *b, usize n)
{
    const u8 *x = (const u8 *)a;
    const u8 *y = (const u8 *)b;
    for (usize i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return (int)x[i] - (int)y[i];
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 数值解析与格式化                                                    */
/* ------------------------------------------------------------------ */

/* ★ 这两个函数曾经"只存在于头文件里" ★
 *
 * fe_user.h 宣告了 atoi / fe_snprintf，而 libfe.c 里从来没有实现——
 * 于是任何调用它们的程序都能编过、到链接时才炸（undefined symbol）。
 * 声明而没有实现比"没有这个函数"更糟：读头文件的人会以为它可用。
 * 所以这里补齐实现；**要么有实现，要么把声明删掉**，不允许中间状态。
 *
 * fe_snprintf 的返回语义按 C 标准：返回**本该写入**的长度（不含 '\0'），
 * 于是调用者能检测截断。返回值被"修正"成实际写入长度的实现，
 * 会让所有检查截断的代码永远看不到截断——那是更坏的一种错。 */
/* ★ atoi 已移交给 libposix ★
 *
 * 这里原本有一份 atoi 实现。路线 B 引入 libposix 之后出现了**两份**，
 * 链接器报 duplicate symbol —— 这正是"重复实现"该有的样子：
 * 它至少在链接期就炸了，而不是等到运行期两份行为不一致才显形。
 *
 * atoi 现在住在 libposix/stdlib.c（与 strtol 同一处，含 endptr 与 ERANGE），
 * 因为 POSIX 语义该住在 POSIX 层。libfe 仍然是"syscall ABI 的投影 +
 * 最小运行库"，不该拥有 POSIX 定义的函数。
 *
 * fe_snprintf 则**保留**在这个文件里：它的名字与 POSIX 的 snprintf 不同，
 * 是 libfe 自己的日志格式化器（无精度、无浮点，够内核风格用），
 * 不构成"两份同名实现"那种问题。
 */

struct fe_fmt_out {
    char  *buf;
    usize  cap;
    usize  len;         /* 本该写入的长度（不含 '\0'） */
};

static void fmt_putc(struct fe_fmt_out *o, char c)
{
    if (o->buf && o->len + 1 < o->cap) {
        o->buf[o->len] = c;
    }
    o->len++;
}

static void fmt_puts(struct fe_fmt_out *o, const char *s)
{
    if (!s) {
        s = "(null)";
    }
    while (*s) {
        fmt_putc(o, *s++);
    }
}

/* 按 base 输出无符号数，带最小宽度、左对齐与零填充 */
static void fmt_unum(struct fe_fmt_out *o, u64 v, u32 base, bool upper,
                     int width, bool left, bool zero)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    u32 n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    }
    while (v) {
        tmp[n++] = digits[v % base];
        v /= base;
    }
    int pad = width - (int)n;
    if (!left) {
        for (int i = 0; i < pad; i++) {
            fmt_putc(o, zero ? '0' : ' ');
        }
    }
    while (n > 0) {
        fmt_putc(o, tmp[--n]);
    }
    if (left) {
        for (int i = 0; i < pad; i++) {
            fmt_putc(o, ' ');
        }
    }
}

int fe_vsnprintf(char *buf, usize cap, const char *fmt, va_list ap)
{
    struct fe_fmt_out o;
    o.buf = buf;
    o.cap = cap;
    o.len = 0;

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            fmt_putc(&o, *p);
            continue;
        }
        p++;
        bool left = false, zero = false, plus = false, space = false;
        for (;; p++) {
            if (*p == '-') {
                left = true;
            } else if (*p == '0') {
                zero = true;
            } else if (*p == '+') {
                plus = true;
            } else if (*p == ' ') {
                space = true;
            } else {
                break;
            }
        }
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }
        bool is64 = false;
        while (*p == 'l' || *p == 'z' || *p == 'h') {
            if (*p == 'l') {
                is64 = true;
            }
            p++;
        }
        switch (*p) {
        case 'd':
        case 'i': {
            i64 v = is64 ? va_arg(ap, i64) : (i64)va_arg(ap, int);
            u64 mag = (v < 0) ? (u64)(-v) : (u64)v;
            const char *sign = (v < 0) ? "-" : (plus ? "+" : (space ? " " : ""));
            int w = width - (int)strlen(sign);
            if (zero && !left && *sign) {
                fmt_putc(&o, *sign++);      /* 符号在零填充之前 */
            }
            if (*sign) {
                fmt_putc(&o, *sign);
            }
            fmt_unum(&o, mag, 10, false, w, left, zero);
            break;
        }
        case 'u':
            fmt_unum(&o, is64 ? va_arg(ap, u64) : (u64)va_arg(ap, u32),
                     10, false, width, left, zero);
            break;
        case 'x':
            fmt_unum(&o, is64 ? va_arg(ap, u64) : (u64)va_arg(ap, u32),
                     16, false, width, left, zero);
            break;
        case 'X':
            fmt_unum(&o, is64 ? va_arg(ap, u64) : (u64)va_arg(ap, u32),
                     16, true, width, left, zero);
            break;
        case 'p':
            fmt_puts(&o, "0x");
            fmt_unum(&o, (u64)(usize)va_arg(ap, void *), 16, false, 0, false, false);
            break;
        case 'c': {
            char c = (char)va_arg(ap, int);
            int pad = width - 1;
            if (!left) {
                for (int i = 0; i < pad; i++) {
                    fmt_putc(&o, ' ');
                }
            }
            fmt_putc(&o, c);
            if (left) {
                for (int i = 0; i < pad; i++) {
                    fmt_putc(&o, ' ');
                }
            }
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) {
                s = "(null)";
            }
            int pad = width - (int)strlen(s);
            if (!left) {
                for (int i = 0; i < pad; i++) {
                    fmt_putc(&o, ' ');
                }
            }
            fmt_puts(&o, s);
            if (left) {
                for (int i = 0; i < pad; i++) {
                    fmt_putc(&o, ' ');
                }
            }
            break;
        }
        case '%':
            fmt_putc(&o, '%');
            break;
        default:
            fmt_putc(&o, '%');
            if (*p) {
                fmt_putc(&o, *p);
            }
            break;
        }
        if (*p == '\0') {
            break;
        }
    }

    /* cap > 0 时缓冲区永远是合法的 C 字符串 */
    if (buf && cap > 0) {
        usize end = (o.len < cap) ? o.len : cap - 1;
        buf[end] = '\0';
    }
    return (int)o.len;
}

int fe_snprintf(char *buf, usize cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = fe_vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

usize strlen(const char *s)
{
    usize n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(u8)*a - (int)(u8)*b;
}

void fe_puts(const char *s)
{
    fe_write(s, strlen(s));
}

void fe_print_u64(u64 v)
{
    char buf[24];
    int i = 24;
    if (v == 0) {
        fe_write("0", 1);
        return;
    }
    while (v && i > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    fe_write(&buf[i], (usize)(24 - i));
}

void fe_print_hex(u64 v)
{
    static const char digits[] = "0123456789abcdef";
    char buf[18];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        buf[2 + i] = digits[(v >> ((15 - i) * 4)) & 0xF];
    }
    fe_write(buf, sizeof(buf));
}

/* 极简 printf：只支持 %s %u %d %x */
void fe_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            fe_write(p, 1);
            continue;
        }
        p++;
        switch (*p) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            fe_puts(s ? s : "(null)");
            break;
        }
        case 'u': {
            fe_print_u64(va_arg(ap, unsigned int));
            break;
        }
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) {
                fe_write("-", 1);
                v = -v;
            }
            fe_print_u64((u64)v);
            break;
        }
        case 'l': {
            p++;
            if (*p == 'u') {
                fe_print_u64(va_arg(ap, u64));
            } else {
                fe_print_hex(va_arg(ap, u64));
            }
            break;
        }
        case 'x': {
            fe_print_hex(va_arg(ap, unsigned int));
            break;
        }
        case '%': {
            fe_write("%", 1);
            break;
        }
        default:
            fe_write("%", 1);
            fe_write(p, 1);
            break;
        }
    }
    va_end(ap);
}
