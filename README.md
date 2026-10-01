<!-- SPDX-License-Identifier: 0BSD -->
# FEKernel

一个从零实现的 **x86_64 操作系统内核（微内核）**。

> ★ 本项目做的是**内核**，不是操作系统。★ 内核只提供机制（内存、调度、IPC、对象、
> 中断、能力），设备驱动、文件系统、协议栈、shell 全部是**内核之外**的用户态服务。
> `user/` 下的程序在项目里的身份是**内核的测试客户端**——它们的作用是把内核缺什么照出来，
> 不是"我们要做的系统"。

> 实现边界：内核、驱动、文件系统、协议栈的代码全部自研。
> 汇编部分（GDT/IDT/上下文切换/系统调用入口）按 x86-64 的既有惯例编写；
> Limine 引导协议、PCI/USB/FAT32 等属于公开规范，按规范实现。**不抄任何现有内核。**

---

## 一、今天它是什么

| 维度 | 事实 |
|---|---|
| 内核规模 | 约 2.2 万行 C/ASM，`kernel/**/*.{c,h,asm}` |
| 系统调用 | 约 50 个（`docs/11` §2 有清单口径） |
| 内核自检 | 启动时按组打印汇总行，**每组都带会失败的反向对照**；当前 24 条汇总行在双环境下全为 0 |
| 验证环境 | **QEMU + WHPX** 与 **VirtualBox 7.2 (UEFI)** 双跑；走硬件前提的单元按 `needs` 如实跳过，不算失败 |
| 里程碑 | M0–M19 已完成（清单见 `docs/01-milestones.md`），每一项都有实测记录 |

**已实测的能力**（挑关键的）：

- **执行模型**：ring 3 + 自定义 syscall ABI、ELF64 装载、句柄而非 pid、`spawn`/`wait`/`terminate`/任务枚举；
- **内存**：4 级页表 + HHDM、按需分页 + 栈自动增长、映射撤销、**区间属性变更（`mprotect`）**、W^X；
- **调度**：32 级优先队列 + 时间片轮转 + 睡眠 + 抢占；**每线程终止闸门**（可"只杀别人、留下自己"）；
- **IPC/对象**：端点、通知对象、共享内存对象、**句柄传递**；多对象等待（`wait_any`）内核机制；
- **能力模型**：每任务 I/O 位图（TSS）、MMIO 映射、IRQ 登记、硬件资源池（独占 + 共享 + 协作锁）；
- **中断**：电平/边沿语义、**共享中断线**、触发方式契约、**MSI / MSI-X**（表项由内核代写）；
- **替换映像（`exec`）**：同一任务换掉当前映像（身份不变、映像变），身份相关的授权显式处理；
- **块设备与 DMA**：ATA PIO 与 virtio-blk 两条后端，总线主控 DMA + 零拷贝批量（实测比内联路径快两个数量级）；
- **文件系统**：devfs（服务发现）、ramfs、**FAT32 只读 + 写路径**、A/B 无缝更新 + 扇区保护；
- **时间**：TSC 真实时钟（纳秒分辨率），节拍只作调度单位；
- **用户态**：`libfe`（syscall 投影）、`libdrv`（设备语义）、一套最小的 POSIX 层——**普通 C 程序不改一行源码**即可编译运行。

## 二、还没做的（如实列，按"挡谁"排序）

| 缺 | 挡什么 | 状态 |
|---|---|---|
| **K5 用户态异常处理者** | C1 的最后一项：今天一个 `#PF` 就直接杀线程，程序自己兜不住 | 设计完成（`docs/18`），未实现 |
| **K11 "等一个用户地址"** | 任何线程运行时（`pthread_cond_wait`/`std::condition_variable`）的底座；单核自旋会锁死 | 新发现的硬前置，未做 |
| **SMP（K10）** | 多核；今天全是单核假设（"杀别的线程"、MSI 目的 CPU 固定给 BSP） | 设计完成（`docs/06`），未实现 |
| **USB（D8）** | 真机可用性：今天键鼠**只有 PS/2**，很多机器没有 PS/2 口 | 立项 + 三刀切分（`docs/19`），前置机制已全部满足 |
| **网络（M9）** | 没有任何网络栈 | 未开始 |
| **真机（M11）** | 只在两台虚拟机里验证过 | 未开始 |
| **C++/Qt 链** | 让裁剪过的 Qt 5.15 `qtbase` 能在本平台上被编译出来 | 路线与缺口已成文（`docs/16`/`docs/17`） |

**判据口径**（`docs/11` §1）：C1 执行模型 / C2 内存模型 / C3 时间与可见性。
今天 **C2 齐、C3 齐，C1 还差 K5**。

## 三、明确"不做"的（这是设计决定，不是欠债）

- **内核里不放任何驱动**——端口 / MMIO / 中断都由用户态服务通过资源池**认领**，内核只负责"认领 + 检查"；
- **不做 Linux 二进制兼容**，**不追求 POSIX 完整性**，**不抄任何 libc/glibc**；
- **没有 `fork`/COW**（服务之间共享数据走 IPC 与内存对象，不靠继承）；
- 内核里不认识 `open`/`errno`/`SIGKILL` 这类 POSIX 名字——那是一层用户态策略。

## 四、快速开始（纯 Windows 原生，不需要 WSL / Docker / MSYS）

```powershell
# 1. 工具链（便携 clang/lld + Limine，解到 toolchain/，删目录即卸载）
python tools/fetch_toolchain.py
#    NASM 需自行安装（winget install NASM.NASM）

# 2. 构建
python tools/build.py            # 内核 + 用户态 + 组装可启动目录
python tools/build.py kernel     # 只编内核
python tools/build.py clean

# 3. 运行（★ 必须带 --headless，否则会开图形界面）
python tools/mkfat.py build/disk.img build/esp --label FEKERNEL   # 磁盘镜像（build.py 不会重建它）
python tools/run_qemu.py --headless 100
python tools/run_vbox.py --headless 130

# 4. 串口日志：build/serial.log（QEMU）/ build/vbox-serial.log（VBox）
#    判据看这几行：各组 `失败项: 0`、`用户态失败项: 0`、`init 结束，失败项 0`
```

产物：`build/kernel.elf`（内核映像）、`build/kernel.map`、`build/esp/`（EFI 可启动目录树）、
`build/user/*.elf`（各用户态程序）、`build/disk.img`、`build/compile_commands.json`。

## 五、目录结构

```
kernel/
  entry.asm          内核入口（切到自建栈后调用 C 入口）
  linker.ld          链接脚本（内核位于 top 2GiB，段权限分开声明）
  boot/              Limine 协议请求 + 引导信息归一化
  arch/x86_64/       GDT/TSS、IDT/ISR、LAPIC/IOAPIC/PIC/PIT、串口、syscall 入口、FPU
  mm/                PMM 位图、4 级页表、slab 堆、区间表（VMA）、按需分页、uaccess
  sched/             线程、32 级优先队列、时间片、睡眠、每线程终止闸门
  task/              任务/进程模型、用户线程、ELF 装载、任务快照、终止
  ipc/               端点、通知对象、共享内存对象、句柄传递、多对象等待
  resource.c         硬件资源池（认领/授予/共享/协作锁）+ irq.c（中断投递、MSI/MSI-X）
  fs/                VFS、devfs（服务发现）、ramfs
  time/              时钟（TSC）与节拍源（TSC-deadline → LAPIC → 8254）
  protect.c          磁盘区间保护 + A/B 更新
user/
  libfe/             syscall ABI 的投影（不含设备语义）
  libdrv/            设备语义层（ATA / PS2 / virtio），只有 servers 链它
  libposix/          最小 POSIX 层（stdio/stdlib/string/heap/fd 表/spawn）
  servers/           驱动与系统服务：blkd、fsd、consoled、kbd、mouse、flaky
  bin/               测试客户端与工具：init、sh、ps、fs、pcid、devmgr…
  include/           头文件（含与内核共享的 ABI 定义）
tools/               纯 Python：构建编排、镜像打包、虚拟机启动、SPDX 检查
docs/                设计、里程碑、教训（见下）
```

## 六、设计要点（为什么是这样）

- **机制与策略分离**：内核只放机制；"谁能用哪块硬件"由**能力**表达——每个任务一张句柄表，
  权限是句柄上的位（`FE_RIGHT_*`），**认句柄不认 pid**。
- **硬件认领制**：驱动先向资源池**认领**端口/MMIO/中断，认领前执行 `in` 会被 CPU 的 I/O 位图当场拦下（**实测过**）。
- **中断即一位**：一根中断线（或一条 MSI 消息）变成通知对象上的一位；电平线"屏蔽到确认"，
  共享线的欠账**逐家记**（谁还没确认就还不放行）。
- **时间分两件事**：节拍只做**调度单位**，读时间一律走 TSC（纳秒）；睡眠用**绝对纳秒**到期
  （用"节拍个数"计时在虚拟机上会提前几倍返回——实测过）。
- **身份 = 任务，映像 = 地址空间**：`exec` 换掉的是地址空间与 TLS/FPU/入口，句柄表、设备认领、
  devfs 名字、任务 id 全部保留——这是"服务换一版还叫同一个名字"能成立的原因。
  凡是**按身份钉死**的授权（例如保护模块的更新器豁免），换映像时必须显式处理。

## 七、验证方式（这部分和代码一样重要）

1. **每条正向断言都配一条会失败的反向对照**。"什么缺页都补"也能让栈增长测试通过，
   所以真正要证的是"**不该做的绝对不做**"。
2. **先证明缺陷存在，再修**。修之前先写出**在今天的代码上会失败**的断言，把红留成证据。
   ——"把修复和判定同时做掉"等于没验证过修复前的行为（这条栽过，见 `docs/08`）。
3. **单一变量**：一次只加一件事、单独跑一次、单独看判据；机制、修复、自检不混在一轮里改。
4. **双环境**：QEMU + VirtualBox 都要跑。只有一个环境的样本，分不清是内核的问题还是虚拟机的问题
   （这一条真的抓出过一个节拍源 bug）。
5. **验收看产物，不看退出码**：构建"成功"可能只是链接了旧目标文件（mtime 陷阱，见 `docs/08`）。
6. **提交信息如实写判据达成与否**，WIP 就写 WIP——git 历史是这个项目的一等证据。

`docs/08` 收着八条"工具/测量自己错了"的教训（含"判据可能建立在错前提上"）。

## 八、文档地图

| 文档 | 内容 |
|---|---|
| `docs/00-architecture.md` | 架构与不变量（机制/策略边界、地址空间布局） |
| `docs/01-milestones.md` | 里程碑与实测记录（M0–M19） |
| `docs/03` / `docs/04` | 保护模型 / 磁盘写保护与 A/B 更新 |
| `docs/05-ab-update.md` | 无缝更新与槽状态机 |
| `docs/06-smp.md` | SMP 设计（未实现） |
| `docs/08-*.md` | 内核完成判据与路线 + **测试/工具教训表** |
| `docs/10-posix-layer.md` | 让普通 C 程序跑起来的路线与边界 |
| `docs/11-kernel-next.md` | 判据 C1/C2/C3、差距清单、顺序表 |
| `docs/12-drivers.md` | 驱动路线（D1–D8，含 USB 立项） |
| `docs/13` / `docs/14` / `docs/15` | 终止与枚举 / 中断语义 / 替换映像（`exec`） |
| `docs/16` / `docs/17` | 在 FEKernel 上编译 Qt 的路线 / libc++ 运行库链 |
| `docs/18` / `docs/19` | 用户态异常处理者设计 / USB 三刀 |

## 九、许可证

**0BSD**（BSD Zero Clause License，见 `LICENSE`）——最宽松的许可证之一，连保留版权声明的义务都没有。

选它和本项目的结构有关：计划**让其他人来编写用户态驱动服务**，0BSD 不附加任何义务，
因此那些服务采用什么许可证（包括 GPL-2.0）都不会与内核冲突。微内核架构下驱动是内核之外的
独立进程、通过 IPC 通信——这就是"内核保持宽松、各驱动自选许可证"在技术上的实现方式。

第三方代码（如将来的 libc++/Qt）放在 `third_party/`，**保留它们自己的许可证**，规则见
`third_party/README.md`。镜像中随附的 Limine 引导器采用 BSD-2-Clause（`toolchain/limine/LICENSE`）。

---

**致谢与边界**：x86-64 的汇编级约定（GDT/IDT/页表/Limine 协议）遵循公开规范；
其余全部自研。本项目**不复制任何现有内核的代码**。
