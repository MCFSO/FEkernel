<!-- SPDX-License-Identifier: 0BSD -->
# 路线 B：POSIX 层（让 FEKernel 能跑普通 C 程序）

> **这是一次定位变更，必须先说明白。**
>
> `00-architecture.md` 的铁律是「内核只放机制、运行库不含设备语义」。
> 路线 B 不违反它——但会**拉伸**它：POSIX 是一层"语义很厚"的接口
> （fd 的共享语义、信号、进程组、`fork`），其中一部分不可避免地要内核配合。
> 所以本文的第一件事不是设计接口，而是**划清楚哪些能留在用户态**。
>
> ★ 2026-09-26 的后续 ★ 这条路线后来又**再变更过一次定位**：终点从
> "能编 FEKernel"扩展到"**能在 FEKernel 上编出 `qtbase`**"（提出者：用户，
> 2026-09-26）。设计、缺口与路线见 `16-selfhost-path.md`；本文 §1 的"不是"
> 一行已按新口径改写，旧表述保留在下面。

## 1. 先说目标是什么，不是什么

| | 内容 |
|---|---|
| **是** | 让**普通 C 程序**（用 `open`/`read`/`mmap`/`pthread`/`printf` 的那些）**不改一行源码**就能在 FEKernel 上编译并运行 |
| **不是** | Linux 二进制兼容（ELF 里没那个必要）、不是抄 glibc；桌面本身仍然不是目标，但**裁剪过的 Qt 5.15 LTS `qtbase` 至少要能在 FEKernel 上被编译出来**（**2026-09-26，提出者：用户**；理由：**Qt 至少要能在这个平台上编译出来**，也就是本机自举，不是交叉编译——路线与缺口见 `16-selfhost-path.md`） |
| **验收** | 一个**非平凡的真实程序**在 FEKernel 上跑对：读文件、算东西、写输出、报告错误码 |

> **原先写的是**（2026-09-26 之前，同一次定位变更下被替换）：
>
> > \| **不是** \| Linux 二进制兼容（ELF 里没那个必要）、不是跑 KDE、不是抄 glibc \|
>
> 替换掉的是"**不是跑 KDE**"这半句。**"不是 Linux 二进制兼容"与"不是抄 glibc"
> 两条一个字都没动**——它们仍然是这条路线的前提（ELF 兼容没必要；实现全部自研，
> 原型与常量按 POSIX 规范写，见 §5 的目录说明）。
> 改口径不等于放松边界：**"能编译 `qtbase`"与"跑一个 Qt 桌面"是两件不同的事**。
> 前者有可证伪的判据（`16-selfhost-path.md` §4 的 S10），后者需要显示服务，
> 不在本次口径里。

**为什么这条路线值得做**：它把"这个系统能干什么"从"我写的服务"扩展到
"别人写的程序"。而且它有一个自带的、可验证的终点——**自举**（在 FEKernel
上编译 FEKernel）。自举需要的是 libc + 一个 C 编译器，不需要 KDE。

★ 2026-09-26 之后，"自举"这个终点的**验收物**换了 ★ 从"能编 FEKernel"
换成"能编 `qtbase`"。理由是前者证明不了 libc：**FEKernel 自己是 freestanding
的（`-ffreestanding -nostdlib`），它压根不用 libc**，所以"能编它"与
"这个平台的 POSIX 层能用"是两件事。完整的论证、代价与路线见
`16-selfhost-path.md` §1.2。

## 2. 一个关键的实地勘察结论：fd 层可以整个放在用户态

本轮核实：**内核里没有任何文件相关的系统调用**。文件、目录、读、写
全部是用户态服务（`fsd`）通过端点 IPC 提供的，内核只提供端点与句柄。

这意味着 POSIX 层的绝大多数**不需要碰内核**：

```
POSIX 的 open/read/write/close/lseek/stat/opendir/...
        ↓
   libposix（用户态）
   fd 表：整数 fd → { 对象类型, 端点句柄 / 内存对象 / 缓冲区, 偏移, 标志 }
        ↓
   现有机制：fe_endpoint_call（问 fsd）+ 句柄 + 共享内存对象
```

★ **这就是微内核该有的样子** ★
POSIX 兼容性是一种**策略**，不是机制。把它放在用户态库里的好处不只是
"内核不变胖"：它意味着**换一个 POSIX 实现不需要改内核**（同一套内核上
可以并存一个极简 libc 和一个完整 libc），也意味着 libc 里的 bug 不会变成
内核 panic。这条与 `00-architecture.md` 的 P1 完全一致。

## 3. 内核必须补的东西（尽量少，且每条都要理由）

| # | 需要什么 | 为什么用户态做不到 | 代价 |
|---|---|---|---|
| K1 | **`execve` 暴露成 syscall** | 内核里已经有 `fe_process_create`（装载 ELF、建初始栈），但它只被用来启动 init。"把自己换成另一个映像、**保留** fd 与地址空间之外的身份"必须由内核做——用户态没有换页表的能力 | 小。内核已有全部零件，主要是**参数与环境变量的传递**（`envp`）与"失败必须原子：要么换成功，要么原样返回" |
| K2 | **`fork` / `posix_spawn` 的支持** | 复制地址空间要动页表。**注意 `process.h` 里早就写明"没有 fork"是设计决定** | 见 §4——这一步我建议**先不做**，用 `posix_spawn` 语义顶上 |
| K3 | **信号的投递点** | "用户态异常不要杀线程，交给注册的处理者"只能内核做（`#PF`/`#GP` 的现场在内核手里） | 中。内核要加"每任务异常处理端点"+ 修改 `user_fault` 路径 |
| K4 | **`wait_any`（多路等待）** | 一个线程同时等"端点消息"与"通知置位"——现在做不到，所以每个服务都得开两条线程 + 自己写唤醒逻辑 | 中。机制清楚（等待对象集合），但要小心死锁与公平性 |
| K5 | **进程终止原语（`kill`）** | 没有它，系统管不住自己（一个跑飞的服务只能等它自己退）；也是 `waitpid` 语义的前提 | 中。见 `docs/08` 已知限制 #8 |
| K6 | **VMA / 按需分页** | `mmap` 的完整语义（文件映射、`PROT_*` 变更、栈增长）需要"区间表 + 缺页补页" | 大。**独立里程碑**，不属于本层，但 POSIX 层越完整、对它的依赖越强 |
| K7 | **`clock_gettime` 的真实刻度** | 现在的单调时钟是节拍制（`time.c` 注释自己写着"应以 TSC 为准"） | 小。改 `fe_time_ms` 走 TSC |
| K8 | **`sysconf`/`getrlimit` 之类的事实来源** | 内存总量、CPU 数、页大小——内核知道，用户态猜不出来 | 小。一个 `fe_sysinfo` syscall 就够 |

**明确的成本纪律**：K1/K3/K4/K5/K7/K8 都是"小到中"的改动，合起来仍然让内核
保持在"只放机制"的范围内。**K2（fork）与 K6（VMA）是真正的分水岭**，
本文不把它们塞进第一里程碑（见 §6）。

## 4. `fork` 的取舍（单独说，因为它是定位问题的核心）

`kernel/include/fe/process.h` 里已经写下了这个决定：

> **没有 fork**。Linux 自己也在往 `posix_spawn` 上靠（glibc 就是用
> `clone(CLONE_VM|CLONE_VFORK)+execve` 实现的），而微内核里复制整个地址空间……

这个理由在路线 B 下依然成立，而且有实证支持：**`make` 需要的是
`posix_spawn`，不是 `fork`**（它要的是"起一个子进程并等它"，
不是"复制我自己再换映像"）。所以：

- **第一里程碑**：提供 `posix_spawn`（= 现有 `spawn` + fd 继承 + `envp`）
- **`fork` 本身**：等出现一个真的需要它的程序（例如某个 shell 的管道实现）再评估。
  到那时再决定"实现 COW fork"还是"给那个程序打补丁用 `posix_spawn`"——
  **用需求驱动，而不是预先实现一个大机制**。

★ 这条与项目一贯的做法一致 ★ 不做"以后可能需要"的机制；
但要把**不做**的理由写清楚（这里是：`fork` 会带来 COW、进程组、信号语义
一整套连锁需求，而当前没有任何程序要求它）。

## 5. 分层：谁负责什么（目录与边界）

```
user/include/posix/     POSIX 头（<stdio.h> <stdlib.h> <unistd.h> <fcntl.h>
                        <sys/stat.h> <dirent.h> <errno.h> <pthread.h> …）
                        ★ 这些是**我们的**实现，不是抄的：函数原型与常量按
                          POSIX 规范写（公开标准），实现全部自研
user/libposix/          libposix.a：fd 表、stdio 缓冲、目录流、错误码翻译、
                        malloc（已有）、字符串（已有）、格式化（已有）
user/libfe/             保持不变：**syscall ABI 的投影**，不含 POSIX 语义
kernel/                 只加 §3 里那几条机制
```

**边界怎么判**（用来防止这层慢慢长歪）：

| 问题 | 归属 |
|---|---|
| "fd 3 是哪个端点" | libposix（用户态事实） |
| "这段内存能不能被用户访问" | 内核 |
| "`errno` 该设成什么" | libposix（POSIX 的约定，不是内核的） |
| "这个句柄能不能转交" | 内核（能力模型） |
| "`printf` 的 `%5.2f` 怎么排" | libposix |
| "缺页时补哪一页" | 内核（K6） |

★ 一条硬规则 ★ **内核里不出现 POSIX 的名字**。
内核不认识 `open`、`errno`、`SIGKILL`；它只认识句柄、端点、通知、任务。
一旦内核里出现 `#define O_CREAT`，这条路线就变成了"把 Linux 抄进来"，
而那正是这个项目一开始就拒绝的事。

## 6. 第一个里程碑（可验收，不含形容词）

**目标**：一个**非平凡的真实程序**在 FEKernel 上跑对。

具体做法：拿一个真实的小 C 程序（读文件、算内容、写输出、按 `errno` 报错），
**源码不改**地对着 `libposix` 编译、在 FEKernel 上运行并核对输出。

验收三条：

1. **源码不改**：程序本体一个字符都不动（只允许提供头文件与库）
2. **正向**：对已知内容的文件算出正确结果（与主机上的同一个程序逐字节比对）
3. **反向对照**：文件不存在 → 明确的 `errno` + 非零退出码（不是崩溃、不是静默成功）

第一条为什么重要：**"改一改就能编"证明不了任何事**。
如果为了编过而修改程序源码，那测的是"我们的 API 长得像不像"，
而不是"别人的代码能不能用"。

## 7. 明确不做的事（第一里程碑范围内）

| 不做 | 理由 |
|---|---|
| 网络（socket/DNS/TLS） | 没有 TCP/IP 栈；而且它不挡"普通 C 程序"这一类 |
| 动态链接（`.so`/`dlopen`） | 静态链接足以覆盖第一里程碑；动态链接是**独立项目** |
| 完整信号语义（进程组、作业控制） | 只需要 K3 那一小块（异常交给处理者）；其余等有程序要 |
| 线程取消 / `pthread_cancel` | 需要 K5（终止原语），排在它后面 |
| 完整 `mmap` 文件映射 | 需要 K6（VMA），是独立里程碑 |
| 抄任何 libc 的代码 | 与项目铁律冲突；原型与常量按规范写，实现自研 |

## 8. 这一步先做什么（本次执行）

不是先写 libc，而是先**把缺口变成可核对的清单**：

写一个 `posixprobe` 程序，它按 POSIX 名字调用一整套接口
（`open`/`read`/`write`/`malloc`/`stat`/`opendir`/`posix_spawn`/…），
先用**现有**头文件与环境编译一次。**编译与链接的错误列表就是缺口清单**——
它比"我列一张我认为缺什么的表"可靠得多（后者只能证明我的记忆）。

然后按"最短路径让 probe 跑起来"的顺序实现：头文件 → fd 表 → 文件 IO →
目录 → 进程 → 错误码。每实现一组，probe 就往前走一段，**每一步都有可跑的证据**。

★ 为什么用 probe 而不是直接上 `make` ★
`make` 会一次把所有缺口都砸出来（几十个符号、几百行错乱报错），
而 probe 让缺口**逐个**显形、逐个消掉——这是一个能被验证的收敛过程，
而不是"写完一个 libc 再说"。

## 9. 第一步的实测结果（已完成）

### 结论

**一个普通 C 程序已经能在 FEKernel 上编译、运行、读写真实文件、遍历目录、
拉起子进程并取回退出码——源码一行未改。**

它在 FEKernel 上的实际输出（`build/serial.log`，QEMU + WHPX）：

```
68     274    4096 /etc/protect.list     ← wc 的三项，算对了
  [posixprobe] 步骤: stat/lseek           ← stat 与 lseek 一致（end == st_size）
  [posixprobe] 步骤: 目录遍历
  [posixprobe]   dirent: EFI (目录)
  [posixprobe]   dirent: etc (目录)
  [posixprobe]   dirent: home (目录)
  [posixprobe]   dirent: slot_a (目录)
  [posixprobe]   dirent: slot_b (目录)
  [posixprobe]   dirent: limine.conf (文件)
  [posixprobe]   共 6 项
  [posixprobe] 步骤: 子进程
  [posixprobe]   子程序路径 /slot_a/bin/hello
  [posixprobe]   子进程退出码 42（期望 42）
  [posixprobe] 步骤: 完成，退出码 0
[init] posixprobe 退出码 = 0  期望 0  —— 相符
```

这一屏同时证明了六条链：stdio 缓冲与格式化、fd 表与 `open/read/close`、
`malloc/free`（arena）、`stat`/`lseek` 一致、`opendir/readdir` 分页、
`posix_spawn + waitpid` 的退出码翻译（用 42 而不是 0：状态字翻译写错时
0 会"碰巧正确"，42 不会）。

### 本轮交付

```
docs/10-posix-layer.md          本文（设计、边界、代价、缺口）
user/include/posix/             12 个 POSIX 头（自研，原型按规范）
user/libposix/posix.c           fd 表 + 文件/目录/stat/access/unlink
user/libposix/fmt.c             完整 printf：宽度/精度/%o/%#x/%f/%e/%g/%n
user/libposix/stdio.c           FILE 流（行缓冲/全缓冲）、fgets、sscanf、perror、exit 契约
user/libposix/stdlib.c          atexit、strtol/strtod、getenv/setenv、qsort/bsearch
user/libposix/string.c          memchr/memmove/strstr/strtok_r/strdup/strerror…
user/libposix/heap.c            **用户态堆**：arena + 切分 + 合并 + 结构性自检
user/libposix/spawn.c           posix_spawn/posix_spawnp/waitpid（pid ↔ 任务句柄表）
user/bin/posixprobe/main.c      探测程序（同时是一件真东西：wc 式的计数工具）
user/servers/fsd/main.c         新增 STAT 操作；协议定义改为**引用唯一那份头文件**
tools/build.py                  libposix 参与构建；链接加 --undefined 保住入口
```

### 撞出来的 6 个问题（全部是实测，不是推演）

| # | 症状 | 根因 | 性质 |
|---|---|---|---|
| 1 | `fatal error: 'stdio.h' file not found` | freestanding 目标**一个系统头都没有** | 基线（这就是路线 B 的起点） |
| 2 | 用户态用 `FE_ERR_PIPE` 编不过 | 错误码有**两份**：内核 24 个，用户态手抄 12 个（漏了 PIPE/IO/KILLED…） | **真 bug** → 统一到 `kernel/include/fe/errno.h`（唯一来源） |
| 3 | 加 STAT 操作时要改两个地方 | fsd 与客户端**各抄一份文件协议**（`FS_OP_*` vs `FE_FS_OP_*`） | **真 bug**（本项目最忌讳的"两份我以为"）→ 服务端改为引用唯一头文件 |
| 4 | `undefined symbol: atoi / memmove / malloc` | **声明了却没实现**：`atoi`、`fe_snprintf`、`memmove`，以及**整个用户态堆**（头文件里连统计与自检 API 都写了，实现一行都没有） | **真 bug ×4** → 全部补齐（堆是 arena + 合并 + `fe_malloc_check`） |
| 5 | 目录遍历永不终止（同一批条目刷了 4097 次） | 我按 `count == 0` 判结束，而服务端的约定是 **`next_index == 0`** 表示没有更多 | **我的 bug** → 按约定改，并把这条约定写进 `readdir` 的注释 |
| 6 | `posix_spawn` 失败路径上一写 `errno` 就 `#PF`（出错地址 0） | **内核没有给用户态设 TLS 基址**：`__thread` 编译成 `mov %fs:0x0`，而 fs 基址是 0 | **内核侧真缺口**（见下） |

★ 第 4 条值得单独说 ★
"声明了却没有实现"这件事在本轮出现了 **4 次**（`atoi`、`fe_snprintf`、
`memmove`、堆）。它们的共同点是：**编译器不报错，链接器只在有人调用时才报**，
而在那之前，头文件里的注释描述的是**意图**，不是事实——
`fe_user.h` 里那段关于堆的统计与自检的说明，写了很久，实现从来没有过。
这是"没有症状不等于没有问题"的又一例，而且是**文档本身在骗人**。

### 新发现的内核缺口：用户态 TLS 基址

`errno` 按 POSIX 必须**每线程一份**，而线程局部存储需要内核在上下文切换时
设置 `fs` 基址（用户的 TLS 块由装载器/内核分配并写入 `IA32_FS_BASE`）。
内核今天完全没做这件事，所以：

- `__thread int errno` 的代码**第一次写 errno 就崩**（实测：`#PF`，出错指令
  `movq %fs:0x0, %rcx`，出错地址 0x0）
- 现在的权宜之计是**进程级一份 errno**：单线程程序完全正确，
  多线程程序里会互相覆盖

这不是"实现风格"，而是一条真实的功能缺口，进 §3 的 K 列表：
**K9 用户态 TLS 基址**。它是"能跑普通 C 程序"的必要条件之一——
glibc/musl 的 `errno`、`__thread` 变量、C++ 的 `thread_local` 全都要它。

### 已知边界（第一步结束时的事实）

| 边界 | 说明 |
|---|---|
| `errno` 是进程级 | 见上（K9）。单线程正确，多线程会串 |
| `file_actions` 返回 `ENOSYS` | `posix_spawn` 的**重定向**做不了：fd 表不跨任务继承，需要内核支持"spawn 带初始 fd 映射"（K2） |
| 追加模式（`"a"`）返回 `ENOSYS` | fsd 没有 `O_APPEND` 语义。**不假装支持**——那会让日志文件只剩最后一条 |
| `fork` / `pipe` / `chdir` / `mkdir` / `rename` | 均明确返回 `ENOSYS`（不静默失败） |
| 程序要 spawn 别的程序时得知道自己在哪个槽 | 从 `argv[0]` 推前缀；POSIX 里没有"我在哪"的接口（没 procfs），已在 probe 里注明 |
| 没有 socket / DNS / 动态链接 / 线程 | 见 §7 |
| 堆只增不减 | arena 不还内存给内核（进程生命周期短，今天是对的；写在这里而不是假装有完整 free 语义） |

