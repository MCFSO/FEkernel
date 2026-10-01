<!-- SPDX-License-Identifier: 0BSD -->
# FEKernel 架构设计

> 目标：x86_64 微内核操作系统。内核只做「机制」，所有「策略」与设备驱动都在用户态服务进程里。
> 全部源码为本项目自研；汇编部分（GDT/IDT/上下文切换/syscall 入口）按 BSD/Linux 的既有惯例编写。

---

## 1. 硬性设计原则

| 编号 | 原则 | 含义 |
|------|------|------|
| P1 | **内核最小化** | 内核不含任何设备驱动、文件系统、网络协议栈。 |
| P2 | **一切皆对象 + 句柄** | 进程持有的能力用句柄（handle）表示，句柄表由内核维护，越权访问返回 `FE_ERR_ACCESS`。 |
| P3 | **数据不共享，消息共享** | 进程间默认只能通过 IPC 端点通信；共享内存必须显式创建内存对象并映射。 |
| P4 | **内核不信任用户指针** | 内核访问用户内存一律走 `copy_from_user`/`copy_to_user`，逐页校验并捕获缺页。 |
| P5 | **中断只做最短路径** | ISR 只 EOI + 投递通知；真正的设备处理在驱动服务进程里完成。 |
| P6 | **可静态验证的启动** | 内核启动只依赖 Limine 协议给出的信息，不依赖 BIOS/VGA/ACPI 的隐式状态。 |
| P7 | **纯 Windows 原生工具链** | clang + lld + NASM，无 WSL / 无 Docker / 无 MSYS。 |

---

## 2. 引导链与内存布局

```
UEFI 固件 (OVMF / 主板)
   └─ EFI/BOOT/BOOTX64.EFI      ← Limine 引导器
        └─ limine.conf           ← 引导配置（内核路径、模块、超时）
             └─ boot/kernel.elf  ← FEKernel 内核（本仓库产出）
                  └─ 用户态服务 ELF（作为 Limine module 内嵌）
```

### 虚拟地址空间（每进程）

```
0x0000_0000_0000_0000 ─ 0x0000_7FFF_FFFF_FFFF   用户空间（低半区，128 TiB）
0x0000_7FFF_FFFF_F000                            用户栈顶（guard page 之下不可访问）
0xFFFF_8000_0000_0000 ─ 0xFFFF_8800_0000_0000   HHDM：物理内存直接映射（Limine 提供偏移）
0xFFFF_FFFF_8000_0000 ─ 0xFFFF_FFFF_FFFF_FFFF   内核代码/数据（top 2GiB，-mcmodel=kernel）
```

- 内核在所有进程地址空间中的映射完全相同（共享同一份内核 PML4 上半区），进入内核不需要换页表 → syscall 极快。
- HHDM 让内核用 `hhdm_offset + phys` 直接访问任意物理页，物理内存管理器（PMM）因此可以极简。

### 启动时的寄存器/环境约定

- CPU：long mode，分页已开启（Limine 的页表），中断关闭。
- 栈：Limine 提供，16 字节对齐；内核初始化自己的栈后切换过去。
- 信息传递：**只通过 `.limine_requests` 段**（不用寄存器传参），请求结构见 `kernel/boot/limine.h`。

---

## 3. 微内核提供的机制（内核态全部功能清单）

这是内核的**完整**功能列表，超出此列表的一律属于用户态服务：

1. **物理内存管理（PMM）**：按 4KiB 帧分配，位图 + 空闲链表。
2. **虚拟内存管理（VMM）**：4 级页表、`mmap`/`munmap`/`mprotect` 语义的映射对象、按需分页、COW。
3. **线程与调度**：内核线程 + 用户线程统一表示；抢占式优先级调度 + 时间片轮转；SMP-ready（每 CPU 运行队列）。
4. **进程/地址空间**：地址空间对象、线程组、ELF 加载由内核完成（也可由用户态 loader 服务完成，见里程碑）。
5. **IPC**：
   - 端点（endpoint）：同步消息传递，`call`/`send`/`recv`。
   - 共享内存对象（memory object）：物理帧的引用计数容器。
   - 通知（notification）：轻量信号量/事件，用于中断投递与异步完成。
6. **对象与句柄**：进程句柄表、能力（capability）授权、对象生命周期（引用计数）。
7. **中断路由**：IDT 归内核；申请了 IRQ 能力的驱动服务，中断发生时收到内核投递的通知消息。
8. **时间**：TSC/HPET 校准、单调时钟、定时器队列。
9. **系统调用入口**：`syscall`/`sysret` 指令对，自定义 ABI（见 §5）。

---

## 4. 用户态服务（全部用 C 写）

| 服务 | 职责 | 依赖 |
|------|------|------|
| `init` | 第一个用户进程；拉起其它服务；崩溃重启策略 | 内核 |
| `seriald` | 16550 串口：接收内核/其它服务的日志并输出 | I/O 端口能力 |
| `consoled` | Framebuffer 图形控制台：自研 8x16 点阵字体、光标、滚屏、转义序列 | 帧缓冲内存对象 |
| `pcid` | PCI/PCIe 总线枚举：配置空间读写、BAR 分配、设备树广播 | I/O 端口 + MMIO 能力 |
| `timerd` | 定时器服务：为其它服务提供 `sleep`/`timeout`/`periodic` | 内核通知 |
| `inputd` | PS/2 键盘与鼠标：扫描码 → 按键事件 → 订阅者广播 | I/O 端口 + IRQ |
| `blkd` | 块设备：ATA PIO → AHCI → virtio-blk，统一块接口 | PCI + IRQ + DMA |
| `fsd` | FAT32 文件系统服务：挂载、路径解析、读写、目录遍历 | `blkd` |
| `netd` | 网卡驱动：e1000 + virtio-net，收发帧 | PCI + IRQ + DMA |
| `ipd` | TCP/IP 协议栈：ARP/IPv4/ICMP/UDP/TCP + socket 服务 | `netd` |

**驱动如何碰硬件**：驱动服务启动时向内核申请能力（I/O 端口区间、MMIO 物理区间、IRQ 号、DMA 物理内存），内核校验后：
- I/O 端口：写 TSS 的 I/O 权限位图，只放行申请到的区间，驱动可直接 `in`/`out`。
- MMIO：把物理区间映射进该服务地址空间（用户态可访问页）。
- IRQ：注册后内核在中断到来时向该服务的通知对象投递事件。

---

## 5. 自定义系统调用 ABI

调用约定（我们自己的，刻意区别于 Linux/POSIX）：

```
rax = 系统调用号
rdi, rsi, rdx, r10, r8, r9 = 参数 1..6
返回值 = rax（成功为 0 或结果值，失败为负的错误码 FE_ERR_*）
rflags.IF 在 syscall 入口由内核关闭，返回前恢复
syscall 指令进入内核栈（每线程独立内核栈），sysret 返回
```

早期 syscall 集合（编号见 `kernel/include/fe/syscall.h`）：

```
0x00 fe_debug_write(const char *buf, size_t len)
0x01 fe_thread_exit(int code)
0x02 fe_thread_yield(void)
0x03 fe_thread_create(fe_thread_entry entry, void *arg, void *stack, size_t stack_size, unsigned flags)
0x04 fe_sleep(uint64_t nanoseconds)
0x05 fe_clock_monotonic(void)              -> ns
0x10 fe_endpoint_create(unsigned flags)    -> handle
0x11 fe_endpoint_send(handle ep, const void *msg, size_t len)
0x12 fe_endpoint_recv(handle ep, void *buf, size_t *len)
0x13 fe_endpoint_call(handle ep, const void *msg, size_t len, void *reply, size_t *reply_len)
0x20 fe_mem_alloc(size_t size, unsigned flags) -> handle（内存对象）
0x21 fe_mem_map(handle mo, void *hint, size_t size, unsigned prot) -> void *
0x22 fe_mem_unmap(void *addr, size_t size)
0x30 fe_handle_close(handle h)
0x31 fe_handle_dup(handle h, unsigned flags) -> handle
0x40 fe_irq_register(unsigned irq) -> handle（通知对象）
0x41 fe_ioport_request(uint16_t base, uint16_t count)
0x42 fe_mmio_map(uint64_t phys, size_t size) -> void *
0x50 fe_process_spawn(handle image_mo, const char *name) -> handle
0x51 fe_process_wait(handle proc, int *exit_code)
```

错误码风格：`FE_OK=0`，`FE_ERR_*` 为负值（-1 起），见 `kernel/include/fe/status.h`。

---

## 6. IPC 细节

**消息**：定长头 + 变长载荷（内联小消息 ≤ 64 字节走寄存器直传，大消息走内核拷贝，超大走共享内存对象传递句柄）。

**同步语义**：`send` 在接收方准备好之前阻塞；`call` = `send` + 等待一次性 reply 端点，构成天然 RPC。

**句柄传递**：消息可携带句柄（内核在接收方句柄表中安装副本），这是「能力传递」的基础。

**中断投递**：`irq` 通知对象可被 `wait`；驱动服务主循环通常是
`recv(命令端点)` 与 `wait(irq 通知)` 的合流（内核提供 `fe_wait_any`）。

---

## 7. 分层与目录结构

**分层的分界线不是按目录画的，是按「谁允许知道什么」画的。** 三条铁律：

1. **内核只放机制，策略一律上移。** 内核知道「这段端口只能有一个持有者」，
   不知道「这段端口应该给键盘服务」——后者是设备管理器的判断。
2. **运行库不含设备语义。** `libfe` 是系统调用 ABI 的投影；8042 时序、PCI 配置空间
   这类东西住在 `libdrv`，只链接进常驻服务。一旦设备代码住进运行库，
   它就会开始长出「反正 libfe 里有」的跨层调用——只有两个驱动时看不出问题，
   到十个驱动时收不回来。

   同样地，**设备协议住在接口层的头文件里**（`user/include/fe_blk.h`：
   `/dev/blk0` 的操作码、单次扇区上限、载荷怎么摆），而不是塞进 libfe，
   也不是让每个客户端各抄一份。抄一份的代价不是那二十行，而是
   **三份"我以为协议是这样的"会各自漂移**——改协议时改了两处漏了第三处，
   表现为"某个程序偶发读不到数据"。（这条不是推演出来的：给更新器加
   `FE_BLK_OP_FLUSH` 时，正是靠"只有一处协议定义"才一次改对。）
3. **接口层不认后端。** `kernel/task/process.c` 的 exec 路径只认 VFS，
   不认识 ramfs，也不认识将来的 FAT32。

```
┌─ 用户态服务（ring 3，各自独立进程，可各自选许可证）
│    init          用户态第一个进程：按依赖拉起服务、跑一次性程序
│    devmgr        服务编排 + 硬件分配**策略**              （M7 起）
│    kbd / mouse   PS/2 设备服务（IRQ 独占 + 端口共享 + 控制器锁）
│    consoled      帧缓冲控制台：帧缓冲 MMIO 的持有者        （M6 后半）
│    blkd / fsd    块设备 / 文件系统                        （M8）
│    netd / ipd    网卡 / 协议栈                            （M9）
├─ 用户态库
│    libfe         syscall ABI 投影 + 最小运行库（**无设备语义**）→ 所有程序
│    libdrv        驱动公共设施：8042、PCI 配置空间…        → **只给服务**
│    （M7 起）     libipc：服务发现客户端 / RPC 桩
├─ 内核机制（ring 0）
│    mm / sched / ipc / object / resource / irq / process
│    vfs           挂载表 + 路径归一化 + inode 属性访问   ← 接口层
│    fs/ramfs      initramfs 后端                        ← 后端之一
│    fs/fat32      FAT32 后端                            （M8）
│    fs/devfs      「名字 → 端点能力」的后端              （M7，见 §9）
│    blk           扇区写白名单的强制点                   （M8，见 03-protection.md）
└─ 硬件抽象
     arch/x86_64   GDT / IDT / ISR / TSS / LAPIC / IOAPIC / PIC / PIT / 串口
     acpi / boot   ACPI 表解析 / Limine 协议与启动信息归一化
```

目录结构与上面的分层**一一对应**——这不是巧合，是刻意的：
目录看不见的分层，三个月后就不存在了。

```
kernel/
  boot/        Limine 协议实现（请求结构、启动信息解析）
  arch/x86_64/ GDT / IDT / ISR / TSS / LAPIC / IOAPIC / PIC / PIT / 串口 / syscall 入口
  mm/          PMM（位图帧分配器）/ VMM（页表）/ 内核堆（slab）/ 用户内存访问
  sched/       线程对象 / 调度器 / 调度自检
  ipc/         端点 / 通知 / 共享内存对象
  object/      句柄表 / 能力 / 对象生命周期
  resource.c   硬件资源池（端口 / MMIO / IRQ 的互斥与共享认领）
  irq.c        外部中断 → 通知对象的投递
  fs/vfs.c     VFS 接口层（挂载表 + 路径归一化 + 属性转发）
  fs/ramfs.c   initramfs 后端
  ld/          ELF64 加载器
  task/        任务对象 / 进程模型（spawn / wait / 初始栈）/ 用户态支持
  lib/         kprintf / 字符串 / panic
  include/fe/  内核公共头（其中 syscall.h 与用户态共享）
user/
  include/     fe_user.h（libfe 接口）/ fe_drv.h（libdrv 接口）
               fe_blk.h（/dev/blk0 的**接口层协议**：设备语义放这里，
               不放 libfe，也不让客户端各抄一份）
  libfe/       最小运行库 + syscall 封装 + 用户程序入口      → 链接进所有程序
  libdrv/      驱动公共设施                                 → 只链接进 servers
  servers/     常驻服务（拥有硬件资源、对外提供能力）        → 镜像里是 /sbin/<名字>
  bin/         一次性程序（工具与测试）                      → 镜像里是 /bin/<名字>
tools/         纯 Python 构建编排、镜像打包、A/B 状态工具（abtool.py）、启动脚本
```

镜像里的路径与目录分层一致（`servers/` → `/sbin/`，`bin/` → `/bin/`），
这是 Unix 的既有约定，两边一致就不必再解释一遍。

---

## 8. 与「不抄现有内核」的边界声明

- 内核、驱动、文件系统、协议栈的**实现代码 100% 本项目自研**。
- **允许**：汇编启动惯例（GDT/IDT/上下文切换/syscall 入口指令序列）、CPU 手册规定的数据结构（页表、TSS、描述符）、
  Limine 引导协议（外部引导器的公开接口，非内核代码）、FAT32 公开规范、TCP/IP RFC。
- **禁止**：复制 Linux/BSD/其它内核的任何 C 代码或数据结构设计。

---

## 9. 服务发现：学 BSD，不建注册表

**决策**：服务发现走 **VFS 上的 devfs 节点**，不引入独立的「名字 → 句柄」注册表服务。

BSD 的答案是现成的：**命名空间就是文件系统**。
FreeBSD 的 devfs 把设备暴露成 `/dev/*`，打开一个设备节点就得到一条通往驱动的通道；
Unix 的服务则发布成命名套接字（`/var/run/*.sock`）。
没有 D-Bus 那样的broker，也没有内核里的服务表。

对照到我们这里：

| BSD | FEKernel |
|---|---|
| 打开 `/dev/ada0` 得到设备通道 | 打开 `/dev/blk0` 得到 `blkd` 的**端点句柄** |
| 命名套接字 `/var/run/*.sock` | 服务把自己的端点发布到 `/dev/<名字>` |
| rc 脚本里 `PROVIDE` / `REQUIRE` 声明依赖 | init 的服务表声明依赖，拓扑排序后依次拉起 |
| devd 处理热插拔与策略 | `devmgr` 决定「哪段资源给哪个驱动」 |

**为什么不做成内核注册表**：那会多出一个与 VFS 平行的命名空间。而 devfs 节点是
VFS 里的一等公民——它天然可列举（`ls /dev` 与 `ls /mnt` 走同一个 `readdir`）、
可挂载、可被将来的 WRP 保护按 inode 施加权限。平行的注册表这三样都得重新造一遍。

**接口形状**（M7 已实现）：

```c
/* 服务侧：把自己的端点发布到 VFS 上的一个名字 */
long fe_devfs_publish(const char *path, long endpoint_handle);
/* 客户端：按路径打开，拿到的是一条通往该服务的端点 */
long fe_devfs_open(const char *path);
```

`devfs` 是 VFS 的一个后端（和 ramfs、fat32 并列），
所以「服务发现」这件事对上层来说就是一次普通的路径查找——
`fsd` 找 `blkd` 和自己找一个文件用的是同一套代码。

两条已经落地的性质：

- **同名发布被拒绝**（`FE_ERR_EXIST`）。否则任何服务都能抢注 `/dev/blk0` 把真服务顶掉，
  「按路径找服务」就退化成「谁先注册谁说了算」。
  认领硬件用资源池的互斥语义，认领名字用 devfs 的互斥语义，两者一致。
- **发布者退出时名字自动收回**，否则服务重启后注册不回同一个名字，
  而旧名字背后的端点属于已销毁的任务，所有已打开的客户端会静默失联。

**启动顺序：学 BSD 的 rc.d / rcorder**（已实现）。

init 持有一张**单元表**，每条声明：

```c
{ .name = "blkread", .path = "/bin/blkread",
  .require = { "/dev/blk0", NULL } },          /* 硬依赖：没就绪就跳过本单位 */
{ .name = "blkd", .path = "/sbin/blkd",
  .provides = "/dev/blk0", .daemon = 1 },      /* 就绪标志：devfs 上出现这个名字 */
```

init 的职责是 BSD 的 `rcorder` 那一步：静态检查依赖完整性 → 拓扑排序 →
依次启动，**并等到依赖方真的 publish 成功才启动下游**。

为什么顺序必须是算出来的：手写顺序是"人脑里的一张图"，每加一个服务都要重新推一遍
谁在谁前面，漏一次就是偶发的启动失败——而失败现场（某服务打开某个名字失败）
离真正的原因（它被排到了提供者前面）隔着好几层。声明的依赖是**可检查的**：
能查出缺失的提供者、能查出环。

单元表里的顺序是**故意打乱**的，日志里会打出"声明顺序"与"解析出的启动顺序"，
两者不同本身就是排序生效的证据。

三条已经落地的性质：

- **`require` 未就绪 → 跳过本单位**，并说明是哪个提供者没发布出来。
  跳过要带原因，否则看到的只是"某程序没跑"，而不知道是上游没起来。
- **`want`（软依赖）只告警不阻塞**，也不参与定序——让软依赖参与定序会凭空造出环，
  而软依赖的本意恰恰是"没有也行"。
- **成环时明确报错并列出环上的单元**，然后按声明顺序退回（并标注这不是正确顺序），
  绝不静默地按某个顺序跑下去。

**失败路径必须专门测。** 正常路径（依赖都在）随便写写也能跑通，
而"依赖没就绪"这条路上很容易退化成"照跑不误，然后在内部莫名其妙地失败"。
所以有两个负向测试夹具：`flaky`（被拉起但永不发布名字，模拟"服务启动就挂"）
与 `depmissing`（依赖 `flaky` 提供、必然未就绪的名字，**必须被跳过**；
它一旦真的跑起来就会大声报错并返回 99）。

