/* SPDX-License-Identifier: 0BSD */
/* 用户态（ring 3）支持：地址空间、ELF 加载、用户线程、用户内存访问。
 *
 * 安全前提：内核**永不信任用户指针**。所有来自用户态的系统调用参数里的地址，
 * 都必须经过 fe_copy_from_user / fe_copy_to_user 校验（范围合法 + 逐页确实映射了且允许用户访问）。
 */
#ifndef FE_USER_H
#define FE_USER_H

#include <fe/types.h>
#include <fe/status.h>
#include <fe/task.h>

/* 用户地址空间布局 */
#define FE_USER_BASE        0x400000ull              /* 程序映像加载基址 */
#define FE_USER_MMAP_BASE   0x0000000010000000ull    /* mmap 区起始（256 MiB） */
#define FE_USER_MMAP_END    0x0000000040000000ull    /* mmap 区结束（1 GiB） */
#define FE_USER_STACK_TOP   0x00007fff00000000ull    /* 用户栈顶（向下增长） */
#define FE_USER_STACK_SIZE  (64ull * 1024)

/* 用户线程栈区：每个线程一段，从上往下分配。
 * 必须给每个线程独立栈——共用同一个栈会让两个线程互相踩栈，
 * 症状是"返回地址被改成不可执行页里的地址"从而触发指令取指异常。 */
#define FE_USER_STACK_AREA_TOP 0x0000700000000000ull

/* ---------------- 用户态线程局部存储（TLS）----------------
 *
 * ★ 布局必须与用户态的 TLS 访问序列一致，所以这里把它写成一个明确的约定 ★
 *
 * 访存序列是编译器生成的，我们改不了：`__thread int errno;` 编译成
 * `mov %fs:0x0, %rcx; movl $2, (%rcx,%rax)`（实测地址，见 docs/10 §9）。
 * 也就是说 `%fs` 基址指向的那个位置必须**存着一个指针**，
 * 该指针指向"数据段起始"，`__thread` 变量按各自偏移落在它后面。
 * 这就是 x86-64 上 GOTTPOFF 风格的变体 II 的变体：
 *
 *     %fs:0   →  指向 TLS 数据块起始（编译器取它，再按偏移访问）
 *     数据块    → .tdata（来自 PT_TLS 的初始化映像）+ .tbss（清零）
 *
 * 所以内核要做三件事：分配一块内存、把初始化映像拷进去、
 * 把块的地址写在 `%fs:0`，然后把 `%fs` 基址设成"这块内存的地址"。
 *
 * 这一条约定在两边各写一次（这里与 user/include/fe_user.h 的
 * FE_TLS_SELF_OFFSET），**数值必须一致**——不一致的症状是
 * "线程局部变量读到别人的值"，而那种错极难定位，所以两边都写了注释。 */
#define FE_TLS_SELF_OFFSET 0

/* PT_TLS 段缺失时，仍给每个线程一个小 TCB（libc 可能要用）。
 * 64 字节足够放一个自我指针 + 少量每线程状态。 */
#define FE_TLS_MIN_TCB 64

/* 创建一个带独立地址空间的用户任务 */
struct fe_task *fe_task_create_user(const char *name);

/* ---------------- 装载的产物：候选现场 ----------------
 *
 * ★ 为什么装载只认"地址空间 + 区间表"，不再认任务对象 ★
 *
 * 这三个函数（fe_elf_load / fe_user_map_stack / build_initial_stack）原来是
 * "顺手改任务对象"的：fe_elf_load 把 tls_init/tls_size/tls_align 写进任务，
 * fe_user_map_stack 写 t->vmas 与 t->stack_low。
 *
 * 这在"新造一个进程"里没问题——任务刚生出来、还没有任何人能看见它，
 * 改坏了就销毁。但 `exec` 的准备阶段**不能**这样：一旦准备失败，
 * 原程序还得接着跑，而它的 `tls_init` 已经指向了另一个程序的映像。
 * 症状是离原因最远的那种：**原程序安然无恙，等它下一次创建线程时**，
 * 新线程的 TLS 块是从别人的映像里拷出来的 → 线程局部变量读到垃圾，
 * 而那个时刻可能离这次失败的 exec 几百万条指令。
 *
 * 所以装载的产物放进这个结构，由调用者在**提交**时一次性挂到任务上
 * （`fe_task_attach_space`）。这样做还有个附带好处：装载路径不再需要
 * "任务"这个概念，自检可以直接对一个裸的地址空间做。
 *
 * ★ tls_init 指向 **ELF 缓冲内部** ★ 调用者必须保证那份缓冲活得比任务久。
 * 今天两个来源都满足：引导模块（Limine 加载、常驻）与 ramfs（同一批模块）。
 * 哪天映像能从磁盘按需读进来，这一条就是第一个要重新想的地方。 */
struct fe_elf_image_info {
    u64 entry;                  /* e_entry */
    const void *tls_init;       /* PT_TLS 初始化映像；NULL = 这个程序没有 TLS */
    u64 tls_size;               /* p_memsz：含 .tbss（必须按 0 初始化） */
    u64 tls_align;              /* 已夹到 <= 64 */
};

/* 把 ELF 映像加载进**指定地址空间**；产物写进 out（不允许为 NULL）。 */
fe_status_t fe_elf_load(struct fe_address_space *as, struct fe_vma_table *vmas,
                        const void *elf, u64 size, struct fe_elf_image_info *out);

/* 在指定地址空间里建立用户栈，返回"栈已映射到的最低地址"（栈增长的下界判据）。
 * out_stack_low 可以为 NULL（调用者不关心时）。 */
fe_status_t fe_user_map_stack(struct fe_address_space *as, struct fe_vma_table *vmas,
                              u64 top, u64 size, u64 *out_stack_low);

/* ---------------- 按需分页与栈增长 ----------------
 *
 * `#PF` 处理器的落点：判断这次缺页是不是"某人合法地碰了一块还没给页的
 * 地址"（栈往下长、匿名映射还没分配），是就补一页并返回真值让异常路径
 * iretq 回去继续跑；不是就返回假，由调用者走"杀线程"那条路。
 *
 * ★ 为什么它是**非致命路径**而必须严格判断 ★
 * 一个把非法访问也"补页"的实现会让段错误变成静默的内存增长：
 * 空指针解引用会分配到一页零、然后程序继续跑，直到别处出错——
 * 那时离真正的原因已经很远了。所以这个函数只承认三种情况：
 *   1. 地址落在标了 GROWSDOWN 的栈区间下方**紧邻**一页处；
 *   2. 地址落在标了 ANON 的区间内（按需给零页）；
 *   3. 地址落在有后备帧的区间内（预映射漏了一页——不该发生，兜底）。
 * 其余一律拒绝，包括"落在区间内但没有 ANON/后备"（那说明区间描述与
 * 页表不一致，补页只会掩盖它）。
 *
 * 返回 true = 已解决（异常处理器应当 iretq 回原现场）。 */
bool fe_user_resolve_fault(struct fe_task *t, u64 fault_addr, u64 error_code);

/* 内核异常处理器看到的原始信息（供 fe_user_resolve_fault 判断与诊断） */
u64 fe_fault_cr2(void);
u64 fe_fault_error(void);

/* 在当前任务里创建一个 ring 3 线程，返回线程 id（0 表示失败） */
u64 fe_user_thread_create(void (*entry)(void *), void *arg,
                          u64 stack, u64 stack_size, u32 flags);

/* ---------------- 用户内存访问 ---------------- */

/* 检查用户区间是否合法（范围内且每页都映射了；write=true 时要求可写） */
bool fe_user_range_ok(struct fe_address_space *as, u64 addr, u64 len, bool write);

fe_status_t fe_copy_from_user(void *dst, const void *user_src, u64 len);
fe_status_t fe_copy_to_user(void *user_dst, const void *src, u64 len);

/* 拷贝一个**以 '\0' 结尾**的用户字符串，最多 cap-1 个字符 + 终止符。
 *
 * ★ 为什么不能用 fe_copy_from_user(dst, p, cap) 代替 ★
 * 那种写法要求从 p 开始的**整整 cap 个字节**都映射着。而用户字符串可能恰好
 * 落在映射区的末尾：字符串本身完全有效（比如 "/slot_a/bin/hxtest"），
 * 但往后读 256 字节就跨进了未映射页 → 整个系统调用返回 FAULT。
 * 这个 bug 在用户态很难复现（栈里的字符串通常离页边界很远），
 * 直到有个程序的 argv[0] 正好贴着栈页末尾才显形——症状是
 * "spawn 一个真实存在的程序却报 EFAULT"，离原因隔着好几层。
 *
 * 本函数逐页校验、遇到 '\0' 立刻停，因此**从不读越过终止符的字节**。
 * 没找到终止符（超过 cap）返回 FE_ERR_NAMETOOLONG，不截断——
 * 截断会把 "/a/b" 变成 "/a/bc" 这种另一个真实存在的路径。 */
fe_status_t fe_copy_str_from_user(char *dst, const char *user_src, u64 cap);

/* 往指定地址空间（不一定是当前任务）的用户虚拟地址写入。
 * 逐页查物理帧、经 HHDM 写；构造新进程的初始栈时使用。 */
fe_status_t fe_user_write_space(struct fe_address_space *as, u64 va,
                                const void *src, u64 len);

/* 取当前任务（用于判断用户指针属于谁） */
struct fe_address_space *fe_user_current_space(void);

/* M5：核对用户态异常证据（init 跑完之后由 main.c 调用）。
 * 返回失败项数。 */
u32 fe_selftest_user(void);

/* 自检：按需分页与栈增长（含 7 条反向对照——“不该补的绝对不补”）。
 * 返回失败项数。 */
u32 fe_selftest_demand_paging(void);

/* 被按需分页消化掉的缺页次数（诊断：>0 说明按需路径真的被走过） */
u64 fe_pf_resolved_count(void);

#endif /* FE_USER_H */
