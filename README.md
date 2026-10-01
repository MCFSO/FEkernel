<!-- SPDX-License-Identifier: 0BSD -->
# FEKernel

一个从零实现的 **x86_64 微内核操作系统**。内核只提供机制（内存、调度、IPC、对象），
所有设备驱动、文件系统与网络协议栈都以**用户态服务进程**的形式用 C 实现。

> 实现边界：内核、驱动、文件系统、协议栈的代码全部为本项目自研。
> 汇编部分（GDT/IDT/上下文切换/系统调用入口）按 BSD/Linux 的既有惯例编写；
> Limine 引导协议、FAT32 磁盘格式、TCP/IP RFC 属于公开规范，按规范实现。

---

## 许可证

**0BSD**（BSD Zero Clause License，见 `LICENSE`）。这是最宽松的许可证之一 —— 连保留版权
声明的义务都没有，等于把代码放进公有领域并附免责声明。

选它的原因与本项目的结构有关：计划**让其他人来编写用户态驱动服务**，而 0BSD 不附加任何义务，
因此那些服务无论采用什么许可证（包括 GPL-2.0）都不会与内核冲突。微内核架构下驱动是内核之外的
独立进程、通过 IPC 通信 —— 这就是「内核保持宽松、各驱动自选许可证」在技术上的实现方式。

镜像中随附的 Limine 引导器采用 BSD-2-Clause（见 `toolchain/limine/LICENSE`），
属宽松许可证，不影响本项目的授权。

---

## 快速开始（纯 Windows 原生，无需 WSL / Docker / MSYS）

### 1. 准备工具链

```powershell
python tools/fetch_toolchain.py            # 下载并解出便携 clang/lld 与 Limine 到 toolchain/
```

工具链会被解到 `toolchain/` 目录内（便携、不写注册表、不需要管理员权限，删目录即卸载）。
NASM 请自行安装（本机已有 `C:\Program Files\NASM\nasm.exe`；`winget install NASM.NASM` 亦可）。

| 组件 | 来源 | 用途 |
|------|------|------|
| clang / ld.lld / llvm-objcopy | LLVM 官方 Windows 版（优先清华 TUNA 镜像） | 交叉编译 `x86_64-unknown-none-elf` 裸机目标 |
| NASM 3.x | 系统安装 | 汇编入口桩、上下文切换 |
| Limine 12.x | Limine 官方发布 | 引导器（UEFI + BIOS） |
| QEMU / VirtualBox | 见下 | 运行与调试 |

### 2. 构建

```powershell
python tools/build.py            # 内核 + 用户态服务 + 组装可启动目录
python tools/build.py kernel     # 只编内核
python tools/build.py clean      # 清理
python tools/build.py -v         # 打印完整命令行
```

产物：

```
build/kernel.elf            内核映像（Limine 加载）
build/kernel.map            链接映射
build/esp/                  EFI 可启动目录树
build/compile_commands.json 编辑器用的编译数据库
```

### 3. 运行

```powershell
# VirtualBox（已实测可用）：自动生成 FAT32 磁盘镜像 → 转 VDI → UEFI 启动 → 回收串口日志与截图
python tools/run_vbox.py --headless 25 --reset

# QEMU（需先装 QEMU；会优先使用 WHPX 硬件加速，避免纯软件模拟的性能损失）
python tools/run_qemu.py --headless 15
python tools/run_qemu.py                    # 带窗口，串口接终端
```

### 4. 单独生成磁盘镜像

```powershell
python tools/mkfat.py build/disk.img build/esp --label FEKERNEL
```

---

## 目录结构

```
kernel/
  entry.asm            内核入口（切换到自建栈后调用 C 入口）
  linker.ld            链接脚本（内核位于 top 2GiB，四个段分开声明权限）
  limine.conf          引导菜单配置
  boot/                Limine 协议请求实例 + 引导信息归一化层
  arch/x86_64/         GDT/TSS、IDT/ISR、8259 PIC、8254 PIT、16550 串口
  lib/                 kprintf、字符串、panic/断言
  include/fe/          内核公共头（含与用户态共享的 syscall ABI）
user/                  用户态：libfe 运行库 + 各驱动服务（M4 起填充）
tools/                 纯 Python 构建编排、镜像打包、虚拟机启动
docs/                  架构设计与里程碑
```

---

## 当前状态

| 里程碑 | 状态 |
|--------|------|
| M0 引导 + 串口 + GDT/IDT + 异常 + 定时器 + 帧缓冲 | ✅ 已在 VirtualBox(UEFI) 实测通过 |
| M1 物理内存 + 虚拟内存 + 内核堆 | ✅ 已在 VirtualBox(UEFI) 实测通过 |
| M2 ACPI/LAPIC/IOAPIC + 抢占式调度 + 睡眠 | ✅ 已在 VirtualBox(UEFI) 实测通过 |
| M3 对象/句柄 + 端点 IPC + 通知 + 共享内存 | ✅ 已在 VirtualBox(UEFI) 实测通过 |
| M4 ring 3 + 自定义 syscall ABI + ELF 加载 + libfe | ✅ 已在 VirtualBox(UEFI) 实测通过 |
| M5 驱动能力 ABI + 硬件资源池 + 进程模型 + initramfs | ✅ 已在 VirtualBox(UEFI) 实测通过 |
| M6 驱动服务：PS/2 键鼠（两个独立用户态进程） | ✅ 已在 VirtualBox(UEFI) 实测通过（宿主机注入按键） |
| 双环境验证：VirtualBox + QEMU(WHPX) | ✅ 两个环境均通过；并借此修复了一个节拍源 bug |
| M7 devfs 服务发现 + 块设备（ATA PIO） | ✅ 两个环境对照通过（读到的磁盘数据逐字一致） |
| M7 续 init 依赖声明服务表（BSD rcorder 那一步） | ✅ 含负向测试（依赖未就绪必须跳过） |
| M8 前半 fsd：FAT32 只读文件系统服务 | ✅ 双环境对照通过（`ls` / `cat` 读出真实镜像） |
| M8 后半：写路径 + WRP/WFP 保护层 | 下一步（写与强制点是同一件事，必须一起做） |
| M6 后半 帧缓冲控制台服务 | 计划中 |
| M9~M11 网络栈 / SMP / 真机 | 计划中 |

### M7 交付内容

```
kernel/fs/devfs.c   「名字 → 端点能力」的文件系统，挂在 /dev —— 服务发现的全部实现
kernel/fs/vfs.c     VFS 接口层：挂载表 + 路径归一化 + 属性转发（与后端分离）
kernel/fs/ramfs.c   initramfs 后端（已退化为纯后端，不再做路径解析）
user/servers/blkd   块设备服务（ATA PIO，轮询式），发布 /dev/blk0
user/bin/blkread    客户端：devfs 找服务 → IPC 读盘 → 自己解析 MBR/BPB
user/include/fe_drv.h  libdrv 接口 —— 构建系统按目录分层，只有 servers 链它
```

**服务发现学 BSD：不建注册表。** 命名空间就是文件系统——
`devfs` 是 VFS 的一个后端（和 ramfs、fat32 并列），服务把端点发布到一个名字，
客户端按路径打开。这样 `/dev/blk0` 与 `/mnt/disk` 由同一个路径解析器处理，
将来 WRP 的权限也能挂在同一套 inode 上。同名发布被拒绝，
所以没有服务能被别的服务顶替。

**第一块块设备走 ATA PIO 而不是 AHCI**：端口 0x1F0/0x3F6 是固定的，
不需要 PCI 枚举就能用，PIO 轮询的行为也完全确定。
先打通「能读磁盘 → 能解析文件系统 → 保护机制有附着点」这条链。

实测：客户端只用路径 `/dev/blk0`，经由 devfs 找到服务、用 IPC 读回 512 字节扇区、
自己解析出 MBR（类型 `ef`，LBA 2048）与 FAT32 BPB（96736 簇，OEM/卷标 `FEKERNEL`）——
**QEMU 与 VirtualBox 读到的数据逐字一致**。

**服务编排学 BSD 的 rc.d**：init 持有一张单元表，每条声明 `provides`（发布哪个 devfs 名字）
与 `require` / `want`，init 做静态完整性检查 + 拓扑排序，**等到依赖方真的 publish
成功才启动下游**。表里的顺序是故意打乱的，日志同时打出"声明顺序"与"解析出的启动顺序"。

失败路径有专门的负向测试：`flaky`（起来但永不发布名字）与 `depmissing`
（硬依赖那个名字，**必须被跳过**；真跑起来会大声报错并返回 99）。
正常路径随便写写也能过，容易退化的是"依赖没就绪却照跑"这条。

### M8 交付内容（前半：只读 FAT32）

```
user/servers/fsd/main.c   FAT32 只读服务：MBR/BPB、簇链、目录 + VFAT 长名、路径解析、文件读
user/bin/fs/main.c        客户端：fs ls / fs cat / 无参数时跑固定完整验证
user/servers/blkd/main.c  支持多扇区读
```

四层栈，每层只做自己那一层的事，每层之间都是"按 devfs 路径找服务"：

```
客户端 ──IPC──▶ fsd ──IPC──▶ blkd ──端口 I/O──▶ ATA
       (路径/文件)   (扇区/LBA)
```

实测（QEMU 与 VirtualBox 完全一致）：`ls /` 列出 6 项、`ls /bin` 列出 9 项
（含 FAT 里真实存在的 `.` 与 `..`）、`cat /limine.conf` 读出完整内容并**核对
构建时写入的字符串**——只核对字节数是不够的，字节数对、内容错也可能。

**本轮刻意只做读**：写路径和保护层是同一件事，一旦能写就必须同时决定
"谁能写哪些扇区"。分两步做的结果一定是先落地一个**没有任何约束的写路径**，
然后再去补强制点——那时候依赖它的代码都已经写完了。

### 为什么坚持要有第二个虚拟化环境

此前所有验证都在 VirtualBox 里做，而 VBox 的节拍投递实测只有 49~169 Hz（声明 1000 Hz），
调度器自检还出现过一次「睡眠 100 节拍实际 546 节拍」。**只有一个环境的样本，
无法判断这是内核的问题还是虚拟机的问题。**

QEMU + WHPX 一接上就分明了：**同一份内核，QEMU 下节拍率是 999 Hz**——
VBox 的低节拍率确实是它的中断投递限流。

但同一轮也揪出了一个**真 bug**：调度器自检为了诊断，直接操作 LAPIC 寄存器测量计数速率，
而它**测完不恢复**——把系统节拍源按一个算错 10 倍的窗口（`tsc_freq/10` 是 100ms 不是 10ms）
重编了一遍，节拍率被静默改成 100 Hz；在已经退到 8254 的 VBox 上还会把 LAPIC 定时器
重新启动，造成**两个节拍源同时跑**——那个 546 节拍的离群值就是这么来的。

修法是把「改完必须恢复」和「破坏」写进同一个函数：时间子系统提供
`fe_time_restart_tick()` 与 `fe_time_measure_lapic_rate()`，自检只测量并报告，
不再碰 LAPIC 寄存器。修复后 QEMU 下三个独立测量（启动校验 / LAPIC 计数速率 / 中断投递速率）
全部收敛到 999~1000 Hz，VBox 下「投递速率」与「睡眠实测」也终于自洽。

详见 `docs/01-milestones.md` 的《双环境验证》一节。

### M6 交付内容（PS/2 键鼠）

```
user/kbd/main.c       键盘服务：认领 IRQ1 + 共享端口 0x60..0x64，解码 set 1 扫描码
user/mouse/main.c     鼠标服务：认领 IRQ12 + 同一段共享端口，解析 3 字节包
user/libfe/ps2.c      8042 控制器访问层（两个服务共用）：flush / RESEND 重发 / 设备命令
kernel/resource.c     资源池新增「可共享区间」与控制器锁
kernel/include/fe/gdt.h  （共享端口仍然落在各任务自己的 I/O 位图里，权限互不影响）
tools/run_vbox.py     新增 --keys：从宿主机注入 PS/2 扫描码，让键盘测试可全自动回归
```

**为什么需要「共享认领」**：键盘的中断是 IRQ1、鼠标是 IRQ12（天然分开），
但数据口 0x60 与命令口 0x64 是同一个 8042 寄存器对（物理上分不开）。
真正能分开的只有中断线，所以两个服务各自认领自己设备的落地形式就是
**中断各自独占 + 端口共享 + 一把控制器锁**（协作锁，防的是两个规矩的服务撞车）。

实测：宿主注入 10 个扫描码 → IRQ1 → 内核投递到通知对象 → 用户态服务读 0x60 →
解码打印，`a`/`d`/空格/Shift 全部正确；鼠标侧 A8/IRQ12/A9/0xF4 全部 ACK。
详见 `docs/01-milestones.md`（含未验证部分的如实记录）。

### M5 交付内容

```
kernel/resource.c    硬件资源池：空闲/已认领两组区间 + 切分 + 归还合并；同一段资源先到先得
kernel/irq.c         中断投递：GSI → 通知对象置位；电平线「屏蔽到确认」（屏蔽 → EOI → 置位）
kernel/fs/ramfs.c    根文件系统（initramfs）：由引导模块构建的只读目录树 + 路径解析
kernel/task/process.c 进程模型：Linux 初始栈布局、按路径装载、spawn/wait、启动 /init
kernel/include/fe/gdt.h  TSS 改成「TSS + 6 个 I/O 位图槽位」整块结构，切换时只改一个 16 位偏移
kernel/arch/x86_64/syscall.c  ioport_request / mmio_map / irq_register / mem_info / spawn / wait
user/init/main.c     init：用户态第一个进程，用 spawn/wait 拉起其余所有程序
user/drvtest/main.c  ring 3 驱动能力测试   user/drvdeny/main.c  故意越权，验证 #GP 真的会来
```

**进程模型学 Linux**：可执行文件由**路径**标识（没有"模块名/注册表 id"），
内核启动的最后一步是 exec 根文件系统的 `/init`；还没有磁盘文件系统时根文件系统由
引导模块构成——这正是 Linux 的 **initramfs**。内核里不再有任何"运行某个用户程序"的代码。

与 Linux 的两处刻意差异：`spawn` 返回新任务的**句柄**而不是 pid（不可伪造，只能 wait
自己持有的任务）；不提供 `fork`（Linux 自己也在往 `posix_spawn` 靠）。

三条硬性质（都有实测证据，见 `docs/01-milestones.md`）：

- **端口权限由 CPU 逐条指令检查**：未认领就 `in` 会当场 #GP 终止线程，认领后同一条指令跑通；
- **资源互斥**：同一段端口不能被两个驱动持有，内核自己占用的 COM1 谁都申请不到；
- **进程状态跨能力句柄传递**：子进程被 #GP 打死的结局，经句柄传回父进程的 `wait`。

### M2 交付内容

```
kernel/acpi/acpi.c         ACPI 表解析：RSDP → RSDT/XSDT → MADT
kernel/arch/x86_64/lapic.c LAPIC：MMIO 映射、EOI、定时器、TSC-deadline、IPI
kernel/arch/x86_64/ioapic.c IOAPIC：按 ACPI 极性/触发方式路由 GSI
kernel/sched/sched.c       线程 + 32 级优先队列 + 时间片轮转 + 睡眠队列
kernel/time/time.c         节拍源（TSC-deadline → LAPIC → 8254）含运行时自愈
```

所有上下文切换走同一条路径（中断返回路径换栈），抢占/让出/退出/首次启动共用一套代码。

### M1 交付内容

```
kernel/mm/pmm.c    物理帧分配器：位图跟在映像末尾，无静态上限；保留区/ACPI/帧缓冲自动排除
kernel/mm/vmm.c    4 级页表：HHDM 2MiB 大页直映射、内核按 ELF 段映射并设 W^X、独立地址空间
kernel/mm/kheap.c  slab 堆：8 个尺寸等级 + 大对象页组分配 + 虚拟区域分配器（空闲即归还 PMM）
kernel/include/fe/mm.h      地址空间布局与页表项标志
kernel/include/fe/elf.h     ELF64 结构（M4 的用户态加载器复用）
```

每项机制都带**启动期自检**（见 `kernel/main.c`），失败会在串口与屏幕上明确报出，而不是静默。

详见 `docs/01-milestones.md`，设计详见 `docs/00-architecture.md`。
