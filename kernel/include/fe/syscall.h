/* SPDX-License-Identifier: 0BSD */
/* FEKernel 系统调用 ABI（自定义，刻意区别于 Linux/POSIX）。
 *
 * 调用约定：
 *   rax = 调用号
 *   rdi, rsi, rdx, r10, r8, r9 = 参数 1..6
 *   返回: rax = fe_status_t（0 或正整数为成功，负数为错误码）
 *   rcx 与 r11 由 syscall/sysret 指令本身破坏，不作为参数。
 *
 * 本头文件同时被内核与用户态 libfe 使用，因此只允许依赖 fe/types.h 与 fe/status.h。
 */
#ifndef FE_SYSCALL_H
#define FE_SYSCALL_H

#include <fe/types.h>
#include <fe/status.h>

/* 句柄：进程内索引，0 恒为无效句柄 */
typedef u32 fe_handle_t;
#define FE_HANDLE_INVALID 0u

/* 权限位（用于内存映射与句柄能力） */
#define FE_PROT_READ   (1u << 0)
#define FE_PROT_WRITE  (1u << 1)
#define FE_PROT_EXEC   (1u << 2)
#define FE_PROT_USER   (1u << 3)

/* 线程创建标志 */
#define FE_THREAD_FLAG_SUSPENDED (1u << 0)

/* 内存对象标志 */
#define FE_MEM_FLAG_DMA          (1u << 0)  /* 物理连续，可交给设备做 DMA */
#define FE_MEM_FLAG_DEVICE       (1u << 1)  /* 映射 MMIO 物理区间，不可被换出 */

/* 端点标志 */
#define FE_ENDPOINT_FLAG_ONEWAY  (1u << 0)  /* 只发不收（日志类） */

enum fe_syscall_num {
    FE_SYS_DEBUG_WRITE      = 0x00,
    FE_SYS_THREAD_EXIT      = 0x01,
    FE_SYS_THREAD_YIELD     = 0x02,
    FE_SYS_THREAD_CREATE    = 0x03,
    FE_SYS_SLEEP            = 0x04,
    FE_SYS_CLOCK_MONOTONIC  = 0x05,

    FE_SYS_ENDPOINT_CREATE  = 0x10,
    FE_SYS_ENDPOINT_SEND    = 0x11,
    FE_SYS_ENDPOINT_RECV    = 0x12,
    FE_SYS_ENDPOINT_CALL    = 0x13,

    /* 通知对象：中断投递的落点。驱动拿它等中断，而不是让内核回调驱动代码。 */
    FE_SYS_NOTIFICATION_CREATE = 0x14,
    FE_SYS_NOTIFICATION_WAIT   = 0x15,
    FE_SYS_NOTIFICATION_SIGNAL = 0x16,

    FE_SYS_MEM_ALLOC        = 0x20,
    FE_SYS_MEM_MAP          = 0x21,
    /* 撤销一段**由 FE_SYS_MEM_MAP 建立**的用户映射。
     *
     * ★ 它不释放物理帧 ★ 帧的所有者是内存对象，不是映射：对象可能还被别的
     * 任务映射着、可能还被句柄持有。想真正释放内存就去关最后一个句柄——
     * 那是对象生命周期的事。让 UNMAP"顺手把内存还回去"会释放一块别人正在用的
     * 内存，症状是对方数据随机损坏（见 docs/09-handle-transfer.md §4.1）。 */
    FE_SYS_MEM_UNMAP        = 0x22,
    FE_SYS_MEM_INFO         = 0x23,

    /* 0x91 (addr, len, prot) —— 改一段**自己地址空间里**页面的访问权限。
     *
     * 参数形状与 MEM_MAP/MEM_UNMAP **同族**（addr/len + 权限位），
     * 权限位复用既有的 `FE_PROT_READ`/`FE_PROT_WRITE`/`FE_PROT_EXEC`，
     * 不另造一套。目标永远是**调用者自己**的地址空间——
     * 接口里没有"指定别的任务"这个参数，所以"改到别人的内存"在形状上
     * 就不可能（与 MEM_UNMAP 同一条纪律）。
     *
     * ★ 三条边界（每一条都有自检）★
     *   1. **范围必须完全落在同一个 VMA 内**：跨区间 `FE_ERR_INVAL`、
     *      端点不在任何区间 `FE_ERR_NOENT`。不扩展、不拆分——
     *      猜意图猜错的后果是把一块调用者没打算改的内存改成只读；
     *   2. **W^X**：一次调用同时给 W 与 X 返回 `FE_ERR_INVAL`。
     *      本接口是用户态唯一能改页面权限的入口，所以 W^X 在这里是可强制
     *      的（`MEM_MAP` 今天允许申请 W|X，那是既有行为，本次不动它）；
     *   3. **写保护清单**（docs/04-write-protection.md）：那份清单的区间是
     *      **磁盘 LBA**，与虚拟地址没有交集，所以这里没有可判的交集——
     *      详见 kernel/mm/vma.c 里 `fe_vma_protect_range` 的说明。挡住
     *      "绕过写保护"的是边界 1（只能改自己 VMA 内的页）。
     *
     * ★ 它同时改两处：VMA 的 flags 与已映射页的 PTE，并**逐页刷 TLB** ★
     * 只改一处都有坏结局（见 fe/mm/vma.h 里 fe_vma_protect_range 的说明）；
     * TLB 不刷则"页表说只读、TLB 还说可写"，用户态照写不误。 */
    FE_SYS_MEM_PROTECT      = 0x91,

    FE_SYS_HANDLE_CLOSE     = 0x30,
    /* 句柄复制（权限只能收窄，不能放大）。
     * 用途：把同一个能力给同一个任务的另一个线程用。
     * 不用它而"把句柄号写进共享变量"是错的——句柄号是**进程内索引**，
     * 一个线程 close 掉，另一个线程的号就指向别的东西了。 */
    FE_SYS_HANDLE_DUP       = 0x31,

    /* 驱动能力（M6）：内核直接掌握的三样硬件手段——端口、MMIO、中断。
     * 它们不是「权限检查后放行」，而是从内核的资源池里**认领**：
     * 同一段资源只有一个持有者，认领不到就是 FE_ERR_BUSY。 */
    FE_SYS_IRQ_REGISTER     = 0x40,
    FE_SYS_IOPORT_REQUEST   = 0x41,
    FE_SYS_MMIO_MAP         = 0x42,
    FE_SYS_IRQ_ACK          = 0x43,
    /* 共享区间上的控制器锁（见 fe/resource.h：这是协作锁，不是安全边界） */
    FE_SYS_RESOURCE_LOCK    = 0x44,
    FE_SYS_RESOURCE_UNLOCK  = 0x45,

    FE_SYS_PROCESS_SPAWN    = 0x50,
    FE_SYS_PROCESS_WAIT     = 0x51,

    /* devfs：服务发现。发布/打开的都是**端点句柄**，
     * 名字空间本身是 VFS 上的一个后端（挂载在 /dev），不是平行的注册表。 */
    FE_SYS_DEVFS_PUBLISH    = 0x60,
    FE_SYS_DEVFS_OPEN       = 0x61,

    /* 写保护（WRP/WFP 第 2 道防线 + A/B 访问矩阵）。
     *
     * ADD 只能加不能减——保护是单调的，所以它开放给任何任务都是安全的；
     * CHECK 由块设备驱动在**每次读/写之前**调用，并把**请求方的任务 id**
     * 一起带上（那个 id 由内核在 IPC 投递时填写，不可伪造）。
     * 内核按自己的表回答：不看调用者是谁，只看"请求方是否被豁免"。
     * 0x71 留给将来的 REMOVE（需要签名更新包，本轮不做）。 */
    FE_SYS_PROTECT_ADD      = 0x70,
    FE_SYS_PROTECT_CHECK    = 0x72,
    FE_SYS_PROTECT_LIST     = 0x73,
    FE_SYS_PROTECT_STAT     = 0x74,

    /* A/B 的派生事实（槽状态位置、引导控制块位置、更新器身份、当前槽）。
     * 从清单算出来的，让用户态自己再解析一遍清单只会引入两边不一致的风险。 */
    FE_SYS_AB_INFO          = 0x80,

    /* 引导器给的命令行，原样交给用户态。
     * 内核**不解释**它（唯一的例外是 slot=，那是内核自己启动 init 要用的）。
     * 理由：命令行是引导器与用户态之间的通道；内核一旦开始解释它，
     * 就等于把"启动策略"搬进了内核。A/B 的"本次启动要不要执行更新"
     * 就是走这条通道的（fek.update=1，由 init 解释）。 */
    FE_SYS_CMDLINE          = 0x81,

    /* 机器复位。A/B 的切换点就是"重启"，所以这不是调试功能而是流程的一环：
     * 更新写完之后必须重启才能进新槽，试用启动失败也必须重启才能回滚。 */
    FE_SYS_REBOOT           = 0x82,

    /* 帧缓冲的几何与物理地址。
     *
     * 内核知道这些（引导器告诉它的），而**控制台服务在用户态**——
     * 它要做的第一件事就是 fe_mmio_map(phys, size) 把帧缓冲映射进来。
     * 没有这个查询，用户态就只能靠"约定"或命令行去猜地址，
     * 而那两样都会在换机器/换分辨率时静默失效。
     *
     * 注意它返回的是**物理**地址：帧缓冲必须走 MMIO 映射（关缓存），
     * 不能用内核的 HHDM 线性地址——那是给内核用的。 */
    FE_SYS_FB_INFO          = 0x83,

    /* 按需分页的累计解析次数（诊断）。
     *
     * ★ 为什么要专门给它一个 syscall，而不是"打印到串口自己看" ★
     * "栈能长"这件事有两个完全不同的实现：
     *   (a) 真的按需补页（缺页被解析）；
     *   (b) 干脆一开始就把整个保留区映射好（永远不缺页）。
     * 从用户态看这两者**行为一模一样**——程序都不崩。
     * 只有这个计数能把它们分开：>0 才说明走的是 (a)。
     * 这正是本项目反复用的那条原则：一个断言必须能被证伪，
     * 而"没崩"这种断言什么都证明不了。 */
    FE_SYS_PF_STAT          = 0x85,

    /* 任务/线程的**只读快照**（K3）。
     *
     * ★ 为什么不在"设备管理器"那一节里，而是单独一条 ★
     * 它的用途比设备管理宽：`ps` 这类运维面、诊断、以及"设备管理器要按
     * 名字找到驱动现在是哪个 id"（D4 的授予需要任务 id）都用它。
     * 上一次它被"声明了但没实现"挂了一个坑（docs/08 的限制清单里记着），
     * 这一轮把它补上——因为 D4 真的需要"按名字找任务"。
     *
     * (buf, buf_len, task_cap, thread_cap)：buf 的布局按
     * `struct fe_task_list`（表头 + 定长任务记录 + 紧凑线程记录），
     * 见本文件下面的定义与 fe_user.h 的同一份约定。 */
    FE_SYS_TASK_LIST        = 0x84,

    /* ---- 设备管理：用户态发现硬件 → 内核裁决归属 ----
     *
     * ★ 为什么"发现"在用户态、"裁决"在内核 ★
     *
     * 发现硬件（读 PCI 配置空间、遍历总线）是**策略与协议**：它要知道设备类、
     * 要处理桥、要认厂商 ID。那属于用户态的设备管理器（`pcid`/`devmgr`）。
     *
     * 但"这段硬件可以给谁"是**安全边界**：I/O 位图、MMIO 页表项、IRQ 路由
     * 都是内核直接掌握的手段。如果任何任务都能往资源池里塞东西，
     * 资源池就不存在了——那正是它要防的事。
     *
     * 所以分成两步，各自在自己该在的地方：
     *   1. **设备管理器**（持有 DM 能力的那个任务）把发现的区间交给内核入池；
     *   2. 内核裁决后，由它把区间**授予**某个驱动任务；驱动再按常规的
     *      `IOPORT_REQUEST` / `MMIO_MAP` / `IRQ_REGISTER` 去认领。
     *
     * 与 M6 的引导期入池相比，这条链把"哪段硬件存在"从**内核硬编码**
     * 变成**用户态发现 + 内核批准**——加一个设备不再需要改内核，
     * 而内核仍然握有否决权（它检查对齐、范围、是否与可用内存相交）。 */

    /* 0x86 (kind, base, len, shared) —— 只有设备管理器能调。
     * kind 取 FE_RES_*；shared 非 0 表示放进池子但标记为可共享。 */
    FE_SYS_RESOURCE_POOL_ADD = 0x86,

    /* 0x87 (kind, base, len, target_handle) —— 把自己持有的资源转给别的任务。
     * 用途：设备管理器把设备真正交给驱动（否则"给哪个驱动"这件事
     * 又变成了驱动自己去抢，谁先认领谁得）。 */
    FE_SYS_RESOURCE_GRANT    = 0x87,

    /* 0x88 (kind, base, len, target_task_id) —— 同上，但目标是**任务 id**。
     *
     * ★ 为什么在"用句柄不用 pid"的项目里还要一个 id 版本 ★
     *
     * 句柄版本（0x87）是**默认**做法，它是不可伪造的能力：只能把硬件交给
     * "我持有的那个任务"。这一条不动。
     *
     * 但设备管理器的现实处境是：**被授予硬件的驱动往往不是它拉起的**。
     * 引导链是 init → devmgr → pcid → blkd，而 blkd 的句柄在 init 手里，
     * devmgr 拿不到。它只有两条路：
     *   (a) 让驱动**反过来**把自己请求的那段资源带走（那就成了"谁请求谁得"，
     *       设备管理器退化成一个盖章的，正是 0x87 想避免的）；
     *   (b) 由**已经有身份的那个任务**（init）把任务句柄转交给 devmgr
     *       —— 这需要 init 记住"哪个句柄是哪个驱动的"，也就是把设备拓扑
     *       抄了一份到引导链里。
     * 两条都比"按 id 授予"更糟。所以补这一条，并把代价写在明处：
     *
     * 代价：目标 id 是**可猜的**。防护靠两件事，缺一不可：
     *   1. 调用者必须是设备管理器（这仍然是一道能力检查，不是权限位）；
     *   2. 目标任务**必须已经存在**——授予一个还不存在的 id，等于给
     *      "将来某个任务"留了一个位置，而 id 会被复用（这是 05-ab-update
     *      里"身份随退出失效"要防的同一件事）。
     * 也就是说：这个原语能表达的是"把硬件交给**现在正在跑的**那个任务"，
     * 而不是"交给 id 为 N 的任务"。 */
    FE_SYS_RESOURCE_GRANT_ID = 0x88,

    /* 0x89 () —— 认领设备管理器身份；只有**引导者**（内核 exec 的第一个
     * 用户任务，即 init）能成功，且只能认领一次。
     *
     * ★ 为什么"谁当设备管理器"不是内核里的规则 ★
     * 内核不认识"devmgr"这个名字——那是策略。它只需要回答一个更小的问题：
     * **谁是引导者**。这一点内核本来就知道（是它自己 exec 的），
     * 所以规则能写得很紧：只有 init 能认领、认领后唯一、init 退出即失效。
     * 引导链要换人（今天由 init 自己兼任，将来换成独立 devmgr 服务），
     * 改的是 init 的表，内核一行不动。 */
    FE_SYS_DEVMGR_CLAIM      = 0x89,

    /* 0x8A () —— 交还设备管理器身份。
     * 用途很具体：init 拉起所有服务之后不再需要它。**主动交还比等退出更紧**——
     * 身份在 init 活着的时候就不存在了，不留一段"还有人在用"的窗口。 */
    FE_SYS_DEVMGR_RELEASE    = 0x8A,

    /* 0x8B () —— 释放本任务名下的**全部**硬件资源所有权（区间回到池子里）。
     *
     * ★ 与"任务退出自动回收"是同一件事，只是提前发生 ★
     * 引导链上有中间点：init 探测完 BAR4 就不再需要 PCI 配置空间端口，
     * 而 pcid 单元随后要拿它做检测。不放回去，pcid 就会失败——
     * "发现"这条链不该因为"init 顺手多用了一会儿"而断掉。
     *
     * 它**不**影响设备管理器身份：身份是"谁能申报"，
     * 所有权是"谁拿着硬件"，两件事（见 fe/resource.h）。 */
    FE_SYS_RESOURCE_RELEASE  = 0x8B,

    /* 0x8C (task_handle) —— 终止一个任务（K2）。
     *
     * 需要句柄上有 **FE_RIGHT_TERMINATE**。`fe_process_spawn` 把它随
     * 子任务句柄一起给父进程，所以**拉起者可以终止它拉起的东西**——
     * 这正是"跑飞的服务只能等它自己退"要解决的那件事。
     *
     * ★ 语义是**异步**的 ★ 返回时只保证"目标的所有线程都已被标记"，
     * 不保证它们已经死透。想要"确定没了"就 FE_SYS_PROCESS_WAIT 那个句柄。
     * 被终止任务的主线程退出码是 FE_ERR_KILLED。
     *
     * ★ 目标不会再回到用户态 ★ 这是硬保证：唯一的闸门在
     * `fe_sched_maybe_switch`（isr 与 syscall 两条返回路径的必经之处）。
     * 但**卡在阻塞调用里的线程**要先被唤醒才走得到闸门，所以内核在
     * 阻塞重试点上检查它，并让那次调用返回 FE_ERR_CANCELED。
     *
     * 边界（为什么会这样设计、代价是什么）见 docs/13-tasks-and-kill.md §3：
     * 不能就地拔掉一个线程（它在对象的等待表上留着指针），
     * 所以终止必然是协作式的——线程只在取消点上死。 */
    FE_SYS_TASK_TERMINATE    = 0x8C,

    /* 0x8D (struct fe_clock_info *) —— 时钟自述：频率、分辨率、来源（K7）。
     *
     * ★ 为什么需要一个"问时钟精度"的口子 ★
     * D4 期间踩到的那次是：时钟粒度 1 ms，而被测代码只有几毫秒，
     * 于是两次独立测量都得到**正好 5000 us**——测出来的是时钟本身。
     * 更糟的是那种断言**看起来是通过的**。
     * 与其在文档里承诺"时钟够细"，不如让调用者能当场问出来，
     * 据此决定用不用它（粒度不够就自己读 TSC）。
     *
     * 分辨率不是猜的：它由实际使用的时基算出来（TSC 制约 0.3 ns，
     * 退回节拍制时是 1 ms）。**"不知道自己的精度"比"精度差"更危险。** */
    FE_SYS_CLOCK_INFO        = 0x8D,

    /* 0x8E (nt_handle, bus, dev, fn, cap_off, out_info) —— 为一台设备使能
     * MSI / MSI-X，并把中断绑到通知对象（D5c）。
     *
     * ★ 为什么"发现"与"使能"要分开 ★
     * 能力结构在哪（cap_off）是设备管理器读配置空间发现的——那是 PCI 协议
     * 知识，是策略。而"分配向量 + 算出消息地址/数据 + 让设备开始发消息"
     * 必须**原子**地由内核做：分两步会出现"设备已经会写 MSI 了，但那个向量
     * 还没有处理者"的窗口，而那个窗口里设备发的消息落到一个野向量上。
     *
     * ★ MSI-X 的表项**由内核代写**，不在这里交给驱动 ★
     * 内核把"表在哪个 BAR、偏移多少、消息地址/数据是什么"报回去**只是诊断**，
     * 那 16 个字节在 `fe_irq_msi_alloc` 返回之前就已经写好了
     * （映 BAR 里那一小段 MMIO、写 addr/data/控制字、再置 Enable、
     * 清 Function Mask），驱动**不要再写第二遍**。
     *
     * **原先写的是**："★ MSI-X 的表项不在这里写 ★ 表在设备的某个 BAR 里，
     * 而 BAR 的映射是驱动自己的事。内核只把……报回去，驱动自己写那 16 个字节。
     * 安全上这不新增能力：驱动本来就能给别人制造伪中断（共享 INTx 的语义如此）。"
     *
     * **实测推翻了它**（`docs/14-interrupt-semantics.md` §8.1）：QEMU 的
     * virtio-blk-pci 把 MSI-X 表放在 **BAR1**，而驱动只映射了 **BAR4**——
     * 驱动手里根本没有表项所在的那段映射，"让驱动自己写"在真实设备上走不通。
     * 内核本来就要读那个 BAR 的**地址**（不然算不出表项在哪），于是用
     * `fe_vmm_map_mmio` 映进**自己的 MMIO 窗口**写一次，**不产生任何所有权声明**。
     *
     * ★ 安全边界（跟着改准）★ 内核代写之后，驱动**没有**"给别人制造伪中断"
     * 这条路了——它只能等自己那一位，能力面比原设计**更小**。
     * 共享 INTx 下"内核无法分辨是哪台设备"那件事仍然在（见 §2.2 / §4.2），
     * 但那是 INTx 的性质，**不是**说驱动现在还能往别人的表项里填向量。
     *
     * ★ 表项号：用户态这层 ABI 没有 `flags` 参数 ★ 这个 syscall 的六个参数里
     * 没有"表项号"，`kernel/arch/x86_64/syscall.c` 的 `sys_irq_msi_alloc`
     * 固定传 `flags = 0`，所以今天写进去的**永远是第 0 项**
     * （virtio 的队列 0 用第 0 项，正好对得上）。要支持别的表项，
     * 改的是 **syscall 这一层**的参数，**不是**让驱动自己写表。
     *
     * 返回的 `irq` 是**伪中断号**（32..63），通知位是 (1 << irq)——
     * MSI 没有 ISA 中断号，但通知对象只有 64 位，所以要有一个下标。 */
    FE_SYS_IRQ_MSI_ALLOC     = 0x8E,

    /* 0x8F (irq) —— 关掉设备的 MSI 并释放向量与伪中断号。
     * 只认自己分配的那个；别人的会返回 FE_ERR_ACCESS。 */
    FE_SYS_IRQ_MSI_FREE      = 0x8F,

    /* 0x90 (path, argv, argc) —— 用新映像替换**当前程序**（K6）。
     *
     * ★ 身份不变、映像变 ★ 句柄表 / 设备认领 / devfs 名字 / 任务 id /
     * 父子关系一个字都不动，只换地址空间与"当前跑的那份程序"。
     *
     * 参数形状与 FE_SYS_PROCESS_SPAWN **完全一致**（path/argv/argc），
     * 因为两者干的是同一件事：把一份映像变成"正在跑的程序"。
     *
     * ★ 成功时**不返回** ★ 它改的是这次 syscall 的返回帧（rip/rsp/rflags），
     * 于是"回到用户态"直接落在新映像的入口上。失败时返回负错误码，
     * **原程序继续跑**（准备阶段的任何失败都不碰任务对象；唯一的例外是
     * FE_ERR_TIMEOUT，那时杀那一步已经发生、不可回滚）。
     * 设计与代价见 docs/15-exec.md。 */
    FE_SYS_EXEC              = 0x90,

    FE_SYS_MAX              = 0x92,
};

/* ---- 用户态异常处理者（K5）的共享 ABI ----
 * 设计与逐条语义见 docs/18-user-fault-handler.md。
 *
 * ★ 投递走消息、回复走系统调用（两条路各有职责）★
 * "现场怎么给用户态"是**拷贝**（内核侧 `fe_copy_to_user` 拷进用户在登记时
 * 给出的缓冲区），而"处理者现在被叫醒"必须走已有的端点唤醒机制——
 * 所以事件用一条消息、载荷是现场缓冲区的地址。
 *
 * ★ 回复**不走消息** ★ 内核在异常上下文里等的是一个**决定**，不是数据流；
 * 而 `fe_msg_reply` 要求处理者先 `recv` 拿到 `reply_ep`，那是"同步 RPC"的
 * 形状（内核并不在 recv 里等）。更硬的一条：系统调用天然携带"谁在回复"
 * （`fe_task_current()`），而消息的 `reply_ep` **可以转交**——
 * "谁能替我回复"必须不可伪造，否则另一个任务可以替处理者做决定。 */

#define FE_FAULT_PROTO      0x4641554Cu   /* 'FAUL' —— 消息头的 proto 字段 */
#define FE_FAULT_OP_EVENT   1u            /* 内核 → 处理者：出事了（载荷 = 现场）*/
/* ★ 这里**没有** FE_FAULT_OP_REPLY ★ 回复不走消息（理由见上），
 * 一个不存在的常量不值得留一行。 */

#define FE_FAULT_REGS_SIZE  200u          /* sizeof(struct fe_fault_regs)，内核侧 */
#define FE_FAULT_REGS_SIZE_X 200u         /* 同一数值的用户态镜像（两边各写一次，
                                           * 各自 _Static_assert 钉住——见
                                           * fe_clock_info 那次"宏与结构体对不上"
                                           * 的实测事故，docs/13 §2） */

/* 处理者的决定（`FE_SYS_FAULT_REPLY` 的第一个参数）。
 * 低 4 位是动作，高位是标志。 */
#define FE_FAULT_ACTION_MASK   0xFu
#define FE_FAULT_RESUME        1u    /* 已处理：按我给的现场继续跑 */
#define FE_FAULT_KILL          2u    /* 我不管：照旧杀线程（今天的行为） */
#define FE_FAULT_RETHROW       3u    /* 再抛一次：让本线程再走一轮投递 */
#define FE_FAULT_FLAG_KEEP_REGS 0x10u /* 与 RESUME 同用：现场用原来的，不改 */

/* 资源类别，供 RESOURCE_LOCK/UNLOCK 与诊断使用。
 * 数值与 fe/resource.h 的 enum fe_res_kind 一致。 */
#define FE_RES_IOPORT 1u
#define FE_RES_MMIO   2u
#define FE_RES_IRQ    3u

/* ---- 时钟自述（CLOCK_INFO，K7）的共享 ABI ----
 * 与 user/include/fe_user.h 的同一份定义逐字段一致。 */
#define FE_CLOCK_FLAG_TSC 1u        /* 时基是 TSC（分辨率约 0.3 ns） */
struct fe_clock_info {
    u64 hz;                 /* 时基计数频率（TSC 制的 TSC 频率） */
    u64 resolution_ns;      /* 一个计数 = 多少纳秒；**至少 1** */
    u32 flags;              /* FE_CLOCK_FLAG_* */
    u32 _pad;
};
_Static_assert(sizeof(struct fe_clock_info) == 24,
               "fe_clock_info 布局变了（用户态按 24 字节读）");

/* ---- MSI 分配结果（IRQ_MSI_ALLOC，D5c）的共享 ABI ----
 *
 * ★ 定义在 fe/irq.h，这里只做说明 ★
 * 它的字段既有"机制"含义（向量、伪中断号）又有 ABI 含义（设备要用的
 * addr/data、驱动要用的表位置），只能有一份定义。放 irq.h 是因为
 * 填它的代码在那里；用户态那份镜像在 user/include/fe_user.h。
 * 两边都用 _Static_assert 钉住大小。 */

/* ---- 任务/线程快照（TASK_LIST）的**共享 ABI** ----
 *
 * ★ 这一份必须与 user/include/fe_user.h 里的那一份逐字段一致 ★
 * 内核按**偏移常量**填这张表，用户态按同一批常量读它。两边各写一次是
 * 分层的必然（内核头不包含用户头），所以两边都要把"为什么"写清楚：
 * 任何一次单边改动都会表现成"某个字段莫名其妙是 0"。
 *
 * ★ 为什么是"定长记录 + 紧凑数组"而不是结构体数组 ★
 * 内核必须能在**不知道用户态结构体布局**的前提下填表（它只认字节偏移），
 * 否则两边编译器的填充差异会变成静默错位。步长写成宏而不是 sizeof：
 * sizeof 会随填充规则漂移，而宏不会。 */
#define FE_TASK_LIST_MAX     64
#define FE_TASK_NAME_MAX_X   16
#define FE_TASK_THREADS_MAX_X 16
#define FE_TASK_STRIDE_X     64     /* 每条任务记录的字节数 */
#define FE_THREAD_STRIDE_X   64     /* 每条线程记录的字节数 */
#define FE_TASK_LIST_HDR_X   48     /* struct fe_task_list 的字节数 */

/* 线程记录里的 flags 位（与 user/include/fe_user.h 的同一份约定）。 */
#define FE_THREAD_FLAG_MAIN  1u     /* 它是所属任务的主线程 */

/* ★ 这一对数字曾经是错的（两个都写成 48），而没有任何东西会报错 ★
 * 内核按**偏移常量**填表，偏移本身一直是对的；错的是"每条记录多长"。
 * 于是 `name` 被写到下一条记录的头 16 字节上，用户态按 48 读到的是
 * 别人的字节。整整一轮都没暴露，因为**这张表从来没有任何调用者**
 * ——没有调用者的 ABI 等于没有验过的 ABI（见 docs/13-tasks-and-kill.md）。
 *
 * 内核这边不能包含 fe_user.h（分层如此），所以它只能照抄一组镜像结构体，
 * 再用 _Static_assert 把"偏移常量"钉在自己的镜像上：
 * 下一次有人改字段而忘了改偏移，编译期就断。 */
struct fe_task_info_x {
    u64 id;
    u64 parent_id;
    u32 thread_count;
    u32 thread_base;
    u32 granted_ports;
    u32 handle_count;
    u64 cpu_ticks;
    u8  exited;
    u8  is_kernel;
    u8  _pad[6];
    char name[FE_TASK_NAME_MAX_X];
};

struct fe_thread_info_x {
    u64 id;
    u64 cpu_ticks;
    u64 switches;
    u32 priority;
    u32 state;
    i32 slice;
    i32 exit_code;
    u32 index;
    u32 flags;              /* FE_THREAD_FLAG_MAIN 等；原为 _pad（见 fe_user.h） */
    char name[FE_TASK_NAME_MAX_X];
};

_Static_assert(sizeof(struct fe_task_info_x) == FE_TASK_STRIDE_X,
               "FE_TASK_STRIDE_X 与内核侧的任务记录布局不一致");
_Static_assert(sizeof(struct fe_thread_info_x) == FE_THREAD_STRIDE_X,
               "FE_THREAD_STRIDE_X 与内核侧的线程记录布局不一致");

struct fe_task_list_hdr {
    u32 task_count;             /* 实际写入的任务数 */
    u32 thread_count;           /* 实际写入的线程数 */
    u64 total_tasks;            /* 内核里当前存活的任务总数（判断是否被截断） */
    u64 total_threads;
    u64 current_task;           /* 调用者自己的任务 id */
    u32 tasks_cap;
    u32 threads_cap;
    u32 _pad[2];
};

_Static_assert(sizeof(struct fe_task_list_hdr) == FE_TASK_LIST_HDR_X,
               "FE_TASK_LIST_HDR_X 与 struct fe_task_list_hdr 不一致");

/* 用户线程入口签名 */
typedef void (*fe_thread_entry)(void *arg);

/* 一条 IPC 消息的固定头（紧跟变长载荷） */
#define FE_MSG_MAX_INLINE 64   /* 内联小消息上限：走寄存器直传，不经过内核拷贝 */

/* 一条消息最多携带几个句柄（能力）。
 *
 * ★ 它与 fe/ipc.h 的 FE_MSG_MAX_HANDLES 必须相等 ★ 这里重复定义是因为
 * 本头文件要能被用户态单独包含（用户态要知道"一次最多传几个"）。
 * 它直接决定 struct fe_message 的大小（消息池 64 条），所以提到 16 会多占
 * 3 KiB 消息池而今天没有任何协议需要——需要时再一起改。
 *
 * 用户态传的句柄数超过这个值一律**拒绝**，不截断：截断会让发送方以为
 * 5 个都过去了而接收方只看到 4 个。 */
#define FE_MSG_MAX_HANDLES_XFER 4

/* ---- 0x11 / 0x12 的新签名（句柄传递，见 docs/09-handle-transfer.md）----
 *
 *   FE_SYS_ENDPOINT_SEND(ep, hdr, payload, hcount, harr)
 *       hcount = 随消息传递的句柄个数，必须与 hdr->handle_count **一致**
 *       harr   = 用户态 u32[hcount]（hcount == 0 时可为 NULL）
 *
 *   FE_SYS_ENDPOINT_RECV(ep, hdr, payload, cap, hbuf, nbuf)
 *       hbuf = 用户态 u32[FE_MSG_MAX_HANDLES_XFER]，接收安装好的句柄（不足项填 0）
 *       nbuf = 用户态 u32*，接收实际安装的个数（可为 NULL）
 *
 * 两条约束值得在这里就说清楚，因为它们决定了怎么用：
 *   - 接收方拿到的权限 = **发送方的权限减去 TRANSFER**。能力只能收窄：
 *     不减这一位的话，A 交给 B 之后 B 能无限转发，一条能力会像病毒一样扩散。
 *   - 两个用户指针在**进入阻塞接收之前**就校验（未映射 → FE_ERR_FAULT）：
 *     recv 成功时句柄已经装进接收方句柄表了，若那时才拷不回去，
 *     用户态就拿到一堆自己不知道句柄号的已安装句柄——不可回收的泄漏。 */

struct fe_msg_header {
    u32 protocol;    /* 服务自定义的协议号，用于多路复用 */
    u32 opcode;      /* 请求/响应操作码 */
    u32 payload_len; /* 载荷字节数 */
    u32 handle_count;/* 随消息传递的句柄个数（载荷之后紧接 fe_handle_t 数组） */
    u64 request_id;  /* 用于匹配请求与响应 */
    /* 发送方任务 id。**由内核在投递时填写**，发送方写这个字段不算数。
     *
     * 为什么它必须在头里、且由内核填：服务要按客户端做策略
     * （例如"A 槽只读、B 槽连读都不行"）就必须知道是谁在请求，
     * 而放在载荷里的身份是可伪造的。这是任何 per-client 策略的前提，
     * 不是为某一个功能特设的。 */
    u64 sender_task;
};

/* MEM_INFO 的返回结构。
 *
 * 为什么必须走系统调用而不是让驱动自己算：物理地址是内核的信息，
 * 只有内核能保证「这段内存确实物理连续、确实归你」。
 * 只对 FE_MEM_FLAG_DMA 的对象开放——否则等于给驱动一个
 * 「任意内存的物理地址」查询接口，那是拿内存保护换方便。 */
struct fe_mem_info {
    u64 phys;        /* 物理基址（DMA 对象保证整段连续） */
    u64 size;        /* 字节数 */
    u32 flags;       /* FE_MEM_FLAG_* */
    u32 page_count;
};

/* ---- 以下仅供内核侧使用 ---- */

/* 初始化 syscall/sysret 的 MSR；内核启动时调用一次 */
void fe_syscall_init(void);
/* 更新「当前线程的内核栈顶」，线程切换时调用 */
void fe_syscall_set_kernel_stack(u64 rsp);

#endif /* FE_SYSCALL_H */
