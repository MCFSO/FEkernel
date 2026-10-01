<!-- SPDX-License-Identifier: 0BSD -->
# K8：内核事实来源（`sysconf` / `uname` / `getrlimit` 该问谁）

> **这是一份设计文档，不是实施记录。** 它回答一个问题：
>
> **"内存总量、CPU 数、页大小、句柄上限……这些用户态猜不出来的事实，
> 内核要怎么给出去；给哪几个；哪些**不该**由内核给。"**
>
> ★ 本文一行代码都不动 ★ 本文的全部结论来自**读文件**，没有跑构建、没有跑虚拟机。
> 每一条"从哪读"都给了 `文件:行号`，行号按本文写作时的工作区状态。
>
> 与前序文档的关系：
> - K8 的**登记**在 `10-posix-layer.md` §3（那一行的原话是"内存总量、CPU 数、
>   页大小——内核知道，用户态猜不出来"，代价写的是"小。一个 `fe_sysinfo` syscall 就够"）；
> - 需求侧的**清单**在 `20-posix-kernel-requirements.md` §2.11（本文是它的施工图）；
> - `20` §6 第 12 条把"`sysinfo` 该暴露哪几个字段的完整清单"留作待查——
>   **本文就是消掉那一条的**（§2 从内核侧的量反推，不是从 `sysconf` 的名字正推）；
> - `20` §6 第 15 条把"`uname()` 要不要与 K8 同一刀做"留作待查——
>   本文 §1.3 给判定、§5 给分刀。

## 1. 判据：先定"标准要求什么"，再定"内核给什么"

### 1.1 三层，不要混

★ 这一节的方法与 `20` §2.11 一致，但**顺序反过来** ★
`20` 是从 `sysconf` 的名字正推"要哪些数"；本文从**内核里真实存在的量**反推
"能给哪些数"，再回到标准去对名字。理由是 `20` §6 第 12 条自己写的那句：
**边界是"内核只回答它真的知道的"，所以清单必须从内核侧的量反推。**

三层必须分开写，混起来就会出现"内核被迫回答 libc 的常量"这种错：

| 层 | 例子 | 谁回答 | 为什么 |
|---|---|---|---|
| **标准要求的接口** | `sysconf` / `uname` / `getrlimit` | **用户态**（libposix） | 内核里不许出现 POSIX 的名字（`10-posix-layer.md` §5 末尾那段；`20` §1.4 重申） |
| **标准要求的"名字"** | `_SC_PAGESIZE`、`struct utsname.sysname` | **用户态** | 名字是标准的，取值才是事实 |
| **事实** | 页大小、帧数、CPU 数、句柄表容量 | ★ **内核** ★ | 用户态**猜不出来**（这一句就是 K8 的原始判据） |

★ 反过来的边界同样硬 ★ `_SC_OPEN_MAX` / `_SC_ARG_MAX` / `_SC_LINE_MAX`
**不是内核的事实**，是 libc 的常量。它们在内核里确实有限制值
（`FE_ARGV_MAX 16` / `FE_ENVP_MAX 8` / `FE_ARG_MAX 128`，
`kernel/include/fe/process.h:40-42`），但"内核的 argv 上限"与
"libc 自称的 `_SC_ARG_MAX`"是**两个不同的问题**：前者是机制，后者是 ABI 承诺。
**内核不回答后者。**

### 1.2 逐条判：标准要什么 → 我们要不要回答

判定只有三种：**标准要求**（必须回答，且取值必须是真的）、
**标准要求但取值可以合理退化**（回答"没有/不限"而不是编一个数）、
**不是标准 → 不做**（写清为什么）。

| 标准里的东西 | 判定 | 谁来回答，怎么回答 |
|---|---|---|
| `sysconf(_SC_PAGESIZE)` | ★ **标准要求** ★ | 内核给 `FE_FRAME_SIZE`（`kernel/include/fe/mm.h:30`，4 KiB）；这**不是**"1 页 = 多少字节"的猜测，是页表的粒度 |
| `sysconf(_SC_PHYS_PAGES)` | ★ **标准要求** ★ | 内核给帧数（§2 的 `total_pages` / `usable_pages`）。★ 注意标准说的是"**物理**内存"，不是"可用内存"：两个数都给，让调用者自己选（`meminfo` 那种只给可用量的做法会让人算错容量规划） |
| `sysconf(_SC_AVPHYS_PAGES)` | ★ **标准要求** ★ | 内核给"当前空闲帧数"（`free_pages`） |
| `sysconf(_SC_NPROCESSORS_CONF)` / `_SC_NPROCESSORS_ONLN` | ★ **标准要求** ★ | 内核给 CPU 数（§2 的 `cpu_count`）。★ 今天这个数**恒为 1**（K10/SMP 未落地）：**如实返回 1 与"写死一个 1"是两回事**——前者在 SMP 落地之后自己变对，后者不会（`20` §2.11 的同一句）★ |
| `sysconf(_SC_OPEN_MAX)` | **标准要求，但值不是内核的事实** | **用户态**给常量。内核的知识是"**句柄表**容量 256"（`kernel/include/fe/object.h:47`）——它是一个**机制**（一个任务最多持多少句柄），不是 `_SC_OPEN_MAX` 的答案 |
| `sysconf(_SC_ARG_MAX)` / `_SC_LINE_MAX` / `_SC_CHILD_MAX` … | **同上** | **用户态**给常量（内核的 argv 限制见 `process.h:40-42`，别把它当成 `_SC_ARG_MAX`） |
| `sysconf(_SC_CLK_TCK)` | **标准要求** | ★ **不要**把它接到"内核的节拍"上 ★ 我们的 `cpu_ticks` 是**调度节拍**（`user/include/fe_user.h:344`），与时基是两件事；`_SC_CLK_TCK` 是"`times()` 的每秒滴答数"，而 `times()` 今天不存在。**回答了才是骗人**：现在应当返回 -1（"这个限制不确定"），而不是给一个会让 `times()` 算出错误秒数的数 |
| `uname()` 的 `sysname` / `release` / `version` / `machine` | ★ **标准要求** ★ | 内核给版本三元组与代号（`kernel/main.c:50-53`）；`machine` = `x86_64`（**编译目标**，不是运行期探测） |
| `uname()` 的 `nodename` | ★ **标准要求，但值可以合理退化** ★ | ★ **这个字段没有内核事实可给** ★ 主机名是 VFS / 名字空间的事，我们没有那个概念。正确的做法是**给一个固定的、明显的占位**（例如 `"fekernel"`），并在文档里写明"这是占位、不是配置项"——**不是**去造一个 hostname 子系统 |
| `getrlimit(RLIMIT_STACK)` | ★ **标准要求** ★ | 内核**确实知道**用户栈的默认大小（`FE_USER_STACK_SIZE`）；`setrlimit` 要改它就得有承载体，今天没有 → **`setrlimit` 如实返回 `ENOSYS`**，`getrlimit` 给真实默认值 |
| `getrlimit(RLIMIT_NOFILE)` | ★ **标准要求** ★ | 两个数分属两层：内核句柄表 256（`object.h:47`）、用户态 fd 上限 64（`user/libposix/posix.c:55`）。★ 回答**哪个**必须写死 ★——`getrlimit` 的语义是"**这个进程**能开多少"，所以答案是**用户态那个 64**；内核的 256 是能力上限，不是这个答案 |
| `getrlimit(RLIMIT_AS)` / `RLIMIT_DATA` / `RLIMIT_MEMLOCK` … | **标准要求；今天没有承载体** | 如实给"不限"（`RLIM_INFINITY`）或 `ENOSYS`，**不要编一个数**。★ `RLIMIT_AS` 尤其危险：给一个假上限会让分配器提前失败 ★ |
| `getrusage()` | ★ **标准要求** ★ | ★ **今天做不到** ★ 它要"本进程的用户态/内核态 CPU 时间"，而内核只有**节拍计数**（`user/include/fe_user.h:344` 的 `cpu_ticks`，"该线程累计占用的节拍"），**没有**用户态/内核态的**时间**拆分。→ 如实返回 `ENOSYS`，**不要**用节拍数除以一个猜的频率去凑秒数（那正是"编一个数字"） |
| `sysconf(_SC_MQ_*` / `_SC_SEM_*` / `_SC_THREAD_*` 一族 | **标准要求** | 分别依赖消息队列 / 信号量 / 线程：今天**都没有**。如实回答"不支持"（-1），**不要**回答 0（0 会被当成"这个限制是 0"——`20` §2.11 判据②的同一形状） |
| `statvfs()` / `fpathconf()` | **标准要求** | ★ **不做** ★ 它们问的是**文件系统**的事实，不是"内核"的事实；而我们的文件系统住在用户态服务（`fsd`）里。**让内核回答文件系统的事会立刻破坏分层**（`20` §1.4 那张图）。归属写清：将来由 `fsd` 自述，与 K8 无关 |

### 1.3 `uname()` 到底是"标准要求"还是"实现形态"

★ 这是 `20` §6 第 15 条点名要回答的问题 ★ **答案是：接口是标准要求，
但它的五个字段里只有四个有内核事实，第五个（`nodename`）是实现形态。**

| `struct utsname` 的字段 | 判定 | 内核里有没有事实 |
|---|---|---|
| `sysname` | 标准要求 | 有：它就是这个内核（"FEKernel"） |
| `release` | 标准要求 | 有：`0.3.0`（`kernel/main.c:50-52`） |
| `version` | 标准要求 | 有：代号 `"Genesis"`（`kernel/main.c:53`）★ 见 §7 待查：要不要带构建标识 ★ |
| `machine` | 标准要求 | 有：`x86_64`（**编译目标**，与 `tools/build.py` 的 `--target` 同一个事实） |
| `nodename` | ★ **实现形态** ★ | ★ **没有** ★ 主机名是 VFS / 名字空间的事（§1.2 那一行） |

★ 为什么这条必须单独写 ★ `20` §1.2.5 第 (3) 条用"`sysconf`/`uname` 这一类"
指过它，但主表里**从来没有列它**；而 `user/include/posix/` 的 12 个头里
**没有 `sys/utsname.h`**（`16-selfhost-path.md` §2.2 已核实为 12 个）。
**"讨论里提过"不等于"清单里有"**——这正是 `20` §6 第 15 条存在的理由。

## 2. 一个 syscall：字段清单

### 2.1 设计约束（先写死，再填字段）

1. ★ **内核里不出现 POSIX 的名字** ★ 这个 syscall 的名字、字段名、位名全部是
   机制侧的（`FE_SYS_SYSINFO`、`page_size`、`cpu_count`），
   **不许**出现 `sysconf` / `_SC_` / `rlimit`（`20` §1.4）。
2. ★ **号从 `FE_SYS_MAX` 起** ★ 今天 `FE_SYS_MAX = 0x94`（`kernel/include/fe/syscall.h:363`），
   所以新号是 **`0x94`**，同时把 `FE_SYS_MAX` 改成 `0x95`。
   ★ 不要顺延、不要复用 0x91 ★：`0x91` 已被 `FE_SYS_MEM_PROTECT` 占用
   （`kernel/include/fe/syscall.h:90`），而"`18` 原写 0x90/0x91 结果都已被占"那次教训
   就记在 `syscall.h:357-359`。**下号之前先读 `FE_SYS_MAX`，不要数 `case`。**
3. ★ **共享 ABI 结构要有 `_Static_assert`，且不要照 `fe_clock_info` 抄** ★
   `18-user-fault-handler.md:289-290` 指出 `fe_clock_info` 的用户态那份
   **今天没有 `_Static_assert`**。要照抄的是 `struct fe_task_info` /
   `struct fe_thread_info` 那两处（`user/include/fe_user.h:378-381`）。
4. ★ **未知调用号今天已经有正确行为** ★ 分发表的 `default` 打印一行并返回
   `FE_ERR_NOTSUP`（`kernel/arch/x86_64/syscall.c:1707-1712`），所以旧内核上
   调新号是"明确的失败"，不是静默错值。

### 2.2 字段清单（★ 每个字段都要能回答"从哪读" ★）

★ 这张表的形状是刻意的：**一个字段一行，四列缺一不可** ★
"从哪读"要能落到 `文件:行号`；写不出行号的字段**不许进这张表**。

| 字段 | 类型/单位 | 今天内核里从哪读 | 用户态拿它做什么 |
|---|---|---|---|
| `abi_size` | `u32`，字节 | `sizeof(struct fe_sysinfo)`（**结构自身的常量**） | 版本协商：旧程序读旧结构，新内核加字段时靠它判断"我能读多少"（`FE_TASK_LIST` 没有这一格，是它的一个短板） |
| `page_size` | `u64`，字节 | `FE_FRAME_SIZE`（`kernel/include/fe/mm.h:30`，`1 << 12`） | `sysconf(_SC_PAGESIZE)`；`mmap` 的对齐；`getpagesize()` |
| `total_pages` | `u64`，帧数 | `fe_pmm_get_stats()` 的 `total_frames`（`kernel/include/fe/mm/pmm.h:19`，由 `pmm.c:306` 填） | `sysconf(_SC_PHYS_PAGES)`。★ "位图覆盖的总帧数"= 最高物理地址折算出来的量，**不等于**可用量（见下一行）★ |
| `usable_pages` | `u64`，帧数 | 同上，`usable_frames`（`pmm.h:20`） | 容量规划：这是"引导器说这些是内存"，**扣掉内核自己与环境保留之后**才是能用的（再用 `free_pages`） |
| `free_pages` | `u64`，帧数 | 同上，`free_frames`（`pmm.h:22`）；也有现成的单值访问器 `fe_pmm_free_frame_count()`（`pmm.h:51`） | `sysconf(_SC_AVPHYS_PAGES)`；分配器决定"要不要现在就放弃" |
| `cpu_count` | `u64`，个 | `fe_boot_info()->cpu_count`（`kernel/include/fe/boot/bootinfo.h:96`，由 `bootinfo.c:78` 从 Limine 的 MP 响应填入） | `sysconf(_SC_NPROCESSORS_ONLN)`、`sched_getaffinity`、`QThread::idealThreadCount`。★ **今天恒为 1**，但它是**读出来的 1**，不是写死的 1（K10 落地后自动变对）★ |
| `handle_table_size` | `u32`，槽位 | `FE_HANDLE_TABLE_SIZE`（`kernel/include/fe/object.h:47`，256） | 用户态的**能力上限**认知（不是 `_SC_OPEN_MAX` 的答案，见 §1.2） |
| `version_major` / `version_minor` / `version_patch` | `u16` ×3 | `FE_VERSION_MAJOR/MINOR/PATCH`（`kernel/main.c:50-52`） | `uname().release`；程序自述"我在哪个内核上跑" |
| `version_name` | `char[12]` | `FE_CODENAME`（`kernel/main.c:53`，`"Genesis"`） | `uname().version`。★ 定长数组而不是指针（跨进程传指针没有意义）★ |
| `boot_timestamp` | `i64`，**Unix 秒** | `fe_boot_info()->boot_timestamp`（`bootinfo.h:103`，由 `bootinfo.c:65` 从 Limine 的 `date_at_boot` 填入） | ★ 这是**今天唯一的墙钟来源** ★ 见 §3 与 §7：它有价值（`ls -l` 至少有个参考点），但**必须**与"我们没有 RTC"这件事一起写清楚 |
| `boot_lapic_id` | `u32`，APIC ID | `fe_boot_info()->bsp_lapic_id`（`bootinfo.h:95`）；运行期也有 `fe_lapic_id()`（`kernel/include/fe/arch/lapic.h:50`） | 排障与"我在哪个核上"；**不是** `uname` 的字段 |
| `flags` | `u32` 位集 | 见 §2.3 | ★ 让"这个值今天只是占位"这件事**可被程序判断**，而不是靠人读文档 ★ |

### 2.3 `flags` 位：把"别信我"变成可以判断的事实

★ 这一节是本文与"随手加个 syscall"的分界线 ★
`_SC_NPROCESSORS_ONLN` 今天返回 1 是**对的**（如实），
但调用者没法区分"真的只有 1 个核"与"我们还没实现 SMP"。
差别不在数字，在**它会不会自己变对**。

| 位 | 名字 | 含义 |
|---|---|---|
| bit 0 | `FE_SYSINFO_F_CPU_COUNT_IS_COMPILE_TIME` | ★ `cpu_count` 是**编译期/引导期的占位**，不是运行期探测 ★ K10 落地后这一位应当清掉（清掉这个动作本身就是一条可验证的判据） |
| bit 1 | `FE_SYSINFO_F_BOOT_TIMESTAMP_VALID` | `boot_timestamp` 可用（引导器给了）；★ 清掉时那个字段是 0，**不是** 1970 ★ |
| bit 2 | `FE_SYSINFO_F_NO_REALTIME_CLOCK` | ★ 系统**没有 RTC**：`boot_timestamp` 是"引导那一刻"，此后**时间戳不会再前进**（`posix.c:476-501` 的注释立了规矩："时间戳一律 0……**报 0 而不是编一个时间**"）★ |

★ 为什么值得为这个多花 4 个字节 ★ 没有它，`configure` 那种
"用时间戳判新旧"的逻辑会在**没有报错**的情况下退化（`16-selfhost-path.md` §3.1
与 `20` §2.12 都记着这条）；有了它，用户态可以**主动**选择"时间不可用就不要做缓存"
而不是"猜一个"。

### 2.4 明确**不**进的字段（写下来，免得下一个人补进去）

| 不进 | 为什么 |
|---|---|
| "哪个任务在用多少内存" | ★ 这是**新的信息面** ★ `TASK_LIST` 已经暴露"有哪些任务"；再加"谁占多少"就把只读快照变成资源画像（`20` §2.11 代价表最后一行已经定了："今天不做"） |
| `_SC_OPEN_MAX` / `_SC_ARG_MAX` / `_SC_LINE_MAX` / `_SC_CLK_TCK` | 不是内核的事实（§1.2） |
| 文件系统容量 / 块大小 | 住在 `fsd`，不是内核（§1.2 最后一行） |
| 主机名 | 没有那个概念（§1.3） |
| 用户态 fd 上限（64） | 那是 **libposix 的**常量（`posix.c:55`）；内核不知道也不该知道 |
| `getrusage` 的那几个时间 | 内核只有节拍计数，没有用户态/内核态时间（§1.2） |

## 3. 与既有 ABI 的关系：能复用就复用，不要重复

★ 这一节是"先去看已经有什么"的清单 ★
K8 很容易变成"再问一遍内核已经说过的事"。

| 已经有的 | 它已经回答了 | K8 该怎么做 |
|---|---|---|
| `FE_SYS_CLOCK_INFO`（`0x8D`，`syscall.h:294`；用户态 `fe_clock_info`，`user/include/fe_user.h:469-475`） | 时基频率、**一格多少纳秒**、是不是 TSC | ★ **一个字都不要重复** ★ `sysinfo` **不**给 `hz`、**不**给 `resolution_ns`。想拿这些的调用者去问 `fe_clock_info()`（它的注释里写着为什么必须有这个口子：粒度不够时调用者要能自己决定"别用它"） |
| `FE_SYS_TASK_LIST`（`0x84`，`syscall.h:179`；用户态 `fe_task_list`，`fe_user.h:412`） | ★ **存活的任务数与线程数** ★ `struct fe_task_list.total_tasks` / `total_threads`（`fe_user.h:396-397`） | ★ 不要进 `sysinfo` ★ 已经有的事实不进第二个 ABI（两处都要维护 = 两处会漂）。要"有多少线程"就去取一次快照 |
| `FE_SYS_MEM_INFO`（`0x23`，`syscall.h:65`） | **一个内存对象**的物理地址/大小/页数 | ★ 它不是系统自述 ★ `20` §2.11 已经指出这件事（"那是**针对一个内存对象**的，不是全系统的"）。两者名字像、语义不同，**文档里必须并排写一句**，否则下一个人会拿 `fe_mem_info` 当 `sysconf(_SC_PHYS_PAGES)` 用 |
| `FE_SYS_PF_STAT`（`0x85`） | 缺页解析计数 | 不同族，不动 |
| `FE_SYS_AB_INFO`（`0x80`）/ `FE_SYS_CMDLINE`（`0x81`）/ `FE_SYS_FB_INFO`（`0x83`） | A/B 槽、命令行、帧缓冲 | 都是"自述"家族，但**各自有自己的 syscall**——K8 不把它们收编（收编 = 改既有 ABI = 与 `20` §1.3 的"不新造编号"同一条纪律的反面） |
| `FE_SYS_RESOURCE_*` / `FE_SYS_DEVMGR_*` | 资源池与设备认领 | 无关，不动 |

★ 一条必须写死的纪律 ★
**"自述"这件事今天已经是一族 syscall（`CLOCK_INFO` / `AB_INFO` / `CMDLINE` /
`FB_INFO` / `PF_STAT` / `TASK_LIST`），所以 K8 是**加一个兄弟**，
不是造一个"统一信息接口"去收编它们。**收编的代价是改 6 个既有 ABI 的调用者**，
而收益只是"少记几个号"。★

## 4. 判据（可证伪 + 反向对照）

★ 判据的形状照 `11-kernel-next.md` §4 与 `20` §5：每条正向都要配一条**会失败**的反向 ★

| # | 正向 | 反向对照（必须会失败） |
|---|---|---|
| **P1** | `sysconf(_SC_PAGESIZE)` == 内核实际页大小，**且这个数能被内核自检独立验证**（自检按 `FE_FRAME_SIZE` 走一遍页表） | 把 `sysinfo` 的 `page_size` **故意写死成 4096**（而不是读 `FE_FRAME_SIZE`），再改 `FE_FRAME_SHIFT` → `sysconf` 必须**跟着变**；不变就说明它是"同一个常量被抄了两遍"（`20` §2.11 判据①的原话） |
| **P2** | `sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE)` 与引导期日志里的可用内存量**同量级且关系写清**（`01-milestones.md` 的"可用内存 462 MiB"是 `usable`，不是 `total`） | ★ **谎报的字段必须能被独立测量对照** ★ 做法：拿 `total_pages` 与 `usable_pages` **两个都打出来**，如果两者恒等，说明有一格填错了（它们**不该**相等：`total` 是位图覆盖量、`usable` 是引导器报告的可用量） |
| **P3** | `uname()` 的 `machine` == `x86_64`、`release` == 内核横幅那三个数字（`main.c:67-68` 打印的同一组） | 把某个字段改成**不在内核里的值**（例如给 `nodename` 编一个真实主机名）→ 必须有测试断言它**等于那个占位常量**（否则"占位"会静默变成"配置"） |
| **P4** | `cpu_count` 与 `FE_SYSINFO_F_CPU_COUNT_IS_COMPILE_TIME` **成对**：位为 1 时 `cpu_count == 1` | SMP 落地后把位清掉而 `cpu_count` 仍是 1 → 判据必须**红**（这一条是为 K10 准备的；它让"改对"这件事有测试） |
| **P5** | 问一个**不存在**的 `sysconf` 名 → 明确返回 -1 且 `errno = EINVAL` | ★ 返回 **0** 必须被判据挡住 ★ 0 会被当成"这个限制是 0"（`20` §2.11 判据②） |
| **P6** | 旧程序在**新内核**上读到的字段仍然对（`abi_size` 协商） | 把 `sysinfo` 结构**加一个字段**、`abi_size` 跟着变 → 旧程序按旧 `abi_size` 截断读取，**不许读到越界内存**（反向：故意不改 `abi_size` → 大小自检必须红） |
| **P7** | `getrlimit(RLIMIT_NOFILE)` 给的是 **64**（用户态 fd 上限），不是 256 | 改成 256 → 必须红：256 是句柄表容量，不是"这个进程能开多少文件"（§1.2 那一行） |
| **P8** | `getrlimit(RLIMIT_STACK)` 给的是真实默认栈大小 | 反向：`setrlimit(RLIMIT_STACK)` **今天必须返回 `ENOSYS`**（不是"接受并忽略"——那正是 `O_APPEND` 那类"静默做错事"，`16-selfhost-path.md` §2.4） |

★ 一条贯穿全部判据的纪律 ★
**每个字段都要能回答"如果这一格填错了，哪个测试会红"。**
答不出来的字段说明它**不是事实、是装饰**——那就别放进 ABI。

## 5. 分刀（建议顺序，每刀一个可证伪的终点）

| 刀 | 内容 | 终点 | 依赖 |
|---|---|---|---|
| **K8-1** | 内核：`FE_SYS_SYSINFO = 0x94` + `struct fe_sysinfo` + 填表（§2.2 的字段）+ `FE_SYS_MAX → 0x95` | 一个内核自检：逐字段与**直接读同一个来源**对比（`fe_pmm_get_stats` / `fe_boot_info()` / `FE_FRAME_SIZE`），全等；反向：把某一格换成常量 → 红 | 无（**不依赖 SMP、不依赖 RTC**） |
| **K8-2** | 用户态 `libfe`：`fe_sysinfo()` 包装 + 用户态镜像结构 + `_Static_assert`（★ 照 `fe_task_info` 抄，**不照** `fe_clock_info` 抄） | 一个用户程序打印全部字段，与内核自检的输出**逐字段相符** | K8-1 |
| **K8-3** | 用户态 `libposix`：`sysconf` / `uname` 的**翻译层**（§1.2 的表逐行落地；`sys/utsname.h` + `unistd.h` 的 `_SC_*` 常量） | ★ 用户态**不改一行**的 `sysconf(_SC_PAGESIZE)` 等拿到正确值；问不存在的名字得到 -1/`EINVAL` | K8-2 |
| **K8-4** | `getrlimit` / `setrlimit`：`RLIMIT_STACK` / `RLIMIT_NOFILE` 给真值，其余如实"不限"或 `ENOSYS` | `16-selfhost-path.md` §4 的 S6 里 `configure` 的探测**不再因为"猜错"而继续** | K8-2 |
| **K8-5** | `getrusage`：★ **单列，很可能就是"如实 `ENOSYS`"** ★ | 判据是"它**明确**返回 `ENOSYS`"而不是"它返回 0 并且所有时间都是 0" | 无（可以先做"如实拒绝"这一半） |

★ 为什么 K8-1 与 K8-2 分开 ★
`CLOCK_INFO` 那次的经验：内核给对了、用户态镜像结构写错一格，
症状是"**字段莫名其妙是 0**"（`fe_user.h:328-331` 记着同一类事故：
两个步长宏都写成 48、而结构体是 64）。分刀之后，
"内核の值对不对"与"镜像结构对不对"是两个可以分开证伪的问题。

★ `uname` 与 `sysconf` 同一刀还是不同刀 ★（`20` §6 第 15 条）
**同一刀（K8-3）**，理由：`uname` 要的四个字段（`sysname`/`release`/`version`/`machine`）
**全部落在同一个 `struct fe_sysinfo` 里**，而第五个（`nodename`）是**不需要**任何内核事实的占位。
分开做等于把同一份结构的两次读取分成两个阶段，中间还要各写一遍镜像结构。

## 6. 代价

| 项 | 代价 |
|---|---|
| 一个 syscall 号 | `0x94`，`FE_SYS_MAX → 0x95`。★ 号是**跨文档标识符**，一旦发出去不能重排（`17-libcxx-build.md` §6.2 记过同一条纪律）★ |
| 一个共享 ABI 结构 | ~64 字节 + 两处镜像 + 两个 `_Static_assert`。★ 单价很低，**但它是长期的**：每一个字段都是"下一次改它要同时改两处"的债 ★ |
| `flags` 的 4 个字节 | 换到"占位值可被程序识别"（§2.3）。★ 这是本文唯一一处"多花空间换诚实"的地方 ★ |
| 内核侧新知识 | 零。所有字段都是**读已有变量**（§2.2 每一行都有 `文件:行号`），没有新机制、没有新权限、没有写路径 |
| 安全面 | 零（只读）。★ 但要守住 §2.4 那条：**不暴露 per-task 内存画像** ★ |
| 用户态 | `sysconf`/`uname` 的翻译层 + 一组 `_SC_*` 常量。★ 这一层会**长期存在**：标准有几百个 `_SC_*` 名字，而我们只回答少数几个；"回答哪些"必须写在头文件的注释里，不能靠人记 ★ |
| `uname` 的 `nodename` | 一个占位字符串。★ 代价不是代码，是**文档**：必须写明"这不是配置项"，否则下一个人会去实现它 ★ |

## 7. 待查

★ 每条都写"怎么查"；查不到的**不猜** ★

| # | 待查 | 怎么查 | 为什么必须查 |
|---|---|---|---|
| 1 | `boot_timestamp` 的**真实可用性** | 读 `bootinfo.c:65` 的填入路径，再看 Limine 的 `date_at_boot` 在 **QEMU 与 VBox 两个环境**是否都给（`01-milestones.md` 里应有引导期日志）；再确认它是不是"Unix 秒"而不是别的纪元 | ★ 它是今天**唯一的墙钟来源** ★ 如果两个环境里有一个给不出来，`FE_SYSINFO_F_BOOT_TIMESTAMP_VALID` 就必须真的会被清掉（否则这一位是个摆设） |
| 2 | `usable_frames` 与"可用内存 462 MiB"的**关系** | 对一次真实引导：`total_frames` / `usable_frames` / `free_frames` 三个数都打出来，与 `01-milestones.md` 的 `462 MiB` 对账 | §4 的 P2 要求"谎报能被独立测量对照"。**没有这次对账，P2 就是纸面的** |
| 3 | `uname().version` 要不要带**构建标识** | 看 `tools/build.py` 今天有没有可用的构建 id（`--build-id=none` 是**关掉**的，`build.py:340`）；再看 `main.c:69-70` 横幅打了哪些 | 带构建标识 = 多一个"可复现构建"的变量；不带 = 两个不同构建的 `uname` 相同。★ 这是取舍，不是缺失，但必须选一个 ★ |
| 4 | `sysconf` 的 `_SC_*` 常量表**从哪来** | 读 `user/include/posix/unistd.h` 今天有什么（本文核实：`sysconf` 相关**一个都没有**）；再决定"照 POSIX 抄一份常量表"是否与"不抄现有实现"的纪律冲突 | ★ 常量表是**标准的一部分**，不是实现细节：抄它的风险与抄 Linux 头文件不同，但这条边界必须写清 ★ |
| 5 | `getrlimit(RLIMIT_STACK)` 的**真实默认值** | 读 `FE_USER_STACK_SIZE`（`kernel/include/fe/user.h`）与 `fe_user_thread_create` 的默认路径（`kernel/task/user.c:507-518` 一带） | 栈大小今天可能有两个来源（主线程 vs 新线程），★ 若两者不同，`getrlimit` 回答哪个必须写死 ★ |
| 6 | `cpu_count` 在 **VBox** 上的值 | 两个环境各跑一次并打印 `cpu_count` | ★ "如实返回 1"的前提是"引导器确实只说 1 个" ★ 如果 VBox 报 2 而我们只跑 1 个，那 `cpu_count` 的语义要改成"**在线** CPU 数"（`_SC_NPROCESSORS_ONLN` 与 `_SC_NPROCESSORS_CONF` 的区别就在这里） |
| 7 | `flags` 位与 K10 的**交接**判据 | 写进 `06-smp.md` 的验收：SMP 落地时哪一条测试负责"清掉 `CPU_COUNT_IS_COMPILE_TIME`" | ★ 位不加交接判据就会永远是 1 ★（`20` §2.11 那句"前者自己变对、后者不会"必须落成一条测试，否则它也只是个说法） |

## 8. 一句话代价（给代价清单用）

- **一个只读 syscall + 一个 ~64 字节共享 ABI**，换来"用户态不必猜页大小 / 内存量 / CPU 数"。
- ★ **内核侧零新机制** ★ 全部字段都是读已有变量；**代价全在用户态与文档**。
- **边界写死在两处**：内核只回答**它真的知道的**（§2.2 每行有出处）；
  `_SC_OPEN_MAX` 那一类**是 libc 的常量**，不进内核。
- **诚实成本**：`flags` 那 4 个字节用来让"这个值今天只是占位"**可被程序判断**，
  而不是靠人读文档——`cpu_count` 今天恒为 1，但它是**读出来的 1**。
- **`uname` 与 `sysconf` 同一刀**：五个字段里四个与 `sysinfo` 重合，
  第五个（`nodename`）是占位，不需要新机制。
- **`getrusage` 今天应当如实 `ENOSYS`**：内核只有节拍、没有用户态/内核态时间拆分，
  用节拍除以猜的频率就是**编数字**。
- **本文一行代码都不动**，也没有跑构建：所有结论来自读文件，出处写在行内。
