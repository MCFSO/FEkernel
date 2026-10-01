<!-- SPDX-License-Identifier: 0BSD -->
# 在 `x86_64-unknown-none-elf` 上把 libc++ 这条运行库链建起来

> **这是一份实施清单，不是路线图。** 路线在 `16-selfhost-path.md`（§2.1 工具链、§2.5、
> §4 的 S1–S10）。本文只回答一个问题：
>
> **"把 libc++ / libc++abi / libunwind / compiler-rt builtins 编到
> `x86_64-unknown-none-elf`"这件事，今天到底要做哪几件具体的事，每件怎么验收。**
>
> ★ **本文一行代码都不动。** 此刻有另一个代理正在改内核源码并独占构建与虚拟机，
> 所以本文的全部结论都来自**读文件**，没有跑过构建、没有跑过虚拟机。★
>
> 与 `16` 的关系：`16` 的 S1 说"一个 C++ 程序要能跑"，S4 说"libc++ 要能编出来"。
> 本文是**S1 与 S4 之间的那份施工图**——把"要有 libc++"这一个句子，拆成
> "哪几个构件 / 宿主必须提供哪些 C 函数 / 谁来构建 / 三个已知的硬开关 /
> 怎么证伪"。

## 1. 需要哪几个构件，各自解决什么

### 1.1 四个构件，四件事（不是一个库的四个部分）

| 构件 | 它解决什么 | 典型符号 | 缺了它会怎样（可观测的症状） |
|---|---|---|---|
| **libc++** | C++ **标准库**：容器、字符串、iostream、算法、`<chrono>` | `std::string`、`std::vector`、`std::cout` | `#include <string>` 直接 `file not found`。今天 `toolchain/llvm/include/` 下只有 `clang`/`clang-c`/`clang-tidy`/`lld`/`llvm`/`llvm-c` **六个目录**，没有 `c++/v1`（`16` §2.1 已核实，本文复核一致） |
| **libc++abi** | **Itanium C++ ABI 的运行时**：异常对象的分配与销毁、`throw` 的入口、RTTI 的支撑、`__cxa_guard_*`（函数级 static 的线程安全初始化） | `__cxa_throw`、`__cxa_begin_catch`、`__cxa_allocate_exception`、`__cxa_atexit`、`__cxa_guard_acquire`、`__gxx_personality_v0` | ① `throw` 编不过（`__cxa_throw` 未定义）；② `dynamic_cast`/`typeid` 链接失败；③ **函数内 `static` 的初始化没有守卫**——多线程下会构造两次 |
| **libunwind** | **栈展开**：读 `.eh_frame`/DWARF CFI，边走栈边跑 `cleanup`/`catch` | `_Unwind_RaiseException`、`_Unwind_Resume`、`_Unwind_Backtrace` | 异常**抛出后找不到 catch**，直接落到 `std::terminate`；且**析构不会被调用**（栈上对象的析构靠展开时的 cleanup landing pad）。★ 注意 `toolchain/llvm/lib/clang/23/include/unwind.h` **有**，但它只有**接口声明**，没有实现（`16` §2.1 已核实） |
| **compiler-rt builtins** | **编译器自己生成的辅助函数**：没有原生指令时的一切（128 位除法、64 位除法、浮点↔整数转换、软件浮点、`__stack_chk_fail`） | `__udivti3`、`__umodti3`、`__udivdi3`、`__floatundidf`、`__fixdfdi` | **链接期直接失败**，见 §1.2。这是四个里**唯一已经有本项目实测证据**的一个 |

★ 这四件事**不能互相顶替** ★
- 有 libc++、没有 libc++abi：`#include <string>` 能过，用 `std::string` 也能过（
  `std::string` 不抛），但 `throw` 一行就断链。
- 有 libc++abi、没有 libunwind：`throw` 能编、能链，**抛出去之后找不到 catch**——
  最坏的一类症状（"看起来编译成功了"）。
- 有前三者、没有 builtins：**编译期一切正常，链接期报符号缺失**（§1.2）。

### 1.2 ★ 为什么必须有 builtins：本项目已经实测过一次 ★

这不是推测，是 `kernel/time/time.c:272-287` 里记着的一次真实失败。原文（逐字引用）：

> `(unsigned __int128)delta * 1e9 / hz` 是最直观的写法，但它会引出
> `__udivti3` —— 编译器运行时库里的 128 位除法辅助函数，而内核是
> **freestanding** 链接的（`-nostdlib`），那个符号不存在，链接直接失败
> （实测：`ld.lld: error: undefined symbol: __udivti3`）。
> 内核里没有 libgcc，所以 128 位除法这条路是堵死的。

同一件事在 `docs/11-kernel-next.md:348-351` 有第二条独立记录：

> **不能用 128 位除法**：`(u128)delta * 1e9 / hz` 会引出 `__udivti3`，
> 而内核是 freestanding 链接的，那个符号不存在（实测链接失败）。

**当时的处置是改写，不是补库**：`time.c` 把 128 位除法拆成两段 64 位
（`ns = (delta / hz) * 1e9 + ((delta % hz) * 1e9) / hz`，`kernel/time/time.c:294-295`）。
这是**一条纪律，不是一条保证**——下一个人写一行 `__int128` 除法，
构建就会被打回原形，而且报错发生在链接期、离那一行很远。

同一族还有一个更早的记录，`docs/01-milestones.md:123`：

> 链接报 `__floatundidf` / `__muldf3` 未定义 | 内核里用了浮点打印 MiB |
> **内核禁用浮点**：改用整数定点打印；这也符合 `-mno-80387` 的编译约束

★ 两条实测连起来说明一件事 ★
`__udivti3`（整数）与 `__floatundidf`/`__muldf3`（浮点）是**同一个根因的两种表现**：
clang 在没有原生指令时会生成对编译器运行时库的调用，而我们的工具链包里
**目标三元组那一份一个都没有**：

```
toolchain/llvm/lib/clang/23/lib/windows/   ← 只有 windows 版
    clang_rt.builtins-x86_64.lib           （MSVC .lib 格式，为 Windows 目标编的）
    clang_rt.asan_dynamic-x86_64.dll/.lib
    clang_rt.fuzzer-x86_64.lib  clang_rt.profile-x86_64.lib
    clang_rt.ubsan_standalone-x86_64.lib  clang_rt.stats-x86_64.lib  …
```

（核实方式：列 `toolchain/llvm/lib/clang/23/lib/` 递归，只有 `windows` 一个子目录，
其中共 16 个文件，**没有任何 `x86_64-unknown-none-elf` 的产物**。
`toolchain/llvm/include/c++/v1` 不存在，`toolchain/llvm/lib` 下没有 `libc++*`/`libunwind*`/
`libc++abi*`。这与 `16` §2.1 的核实结论**逐条一致**。）

**收益怎么写**：把"不许写 `__int128` 除法"这条**隐性纪律**，换成"符号存在"这条
**显性事实**。后者可以被链接器验证，前者只能靠人记。

### 1.3 ★ 还有第五件事，它藏在构建参数里 ★

上表四件是"库"。但今天的 `USER_CFLAGS` 里有两行**主动关掉异常展开信息**：

```
tools/build.py:102        "-fno-asynchronous-unwind-tables",
tools/build.py:103        "-fno-unwind-tables",
```

（`KERNEL_CFLAGS` 的同两行在 `tools/build.py:56-57`。内核关掉是对的——内核不用异常。）

这两行的含义是：**编译器根本不生成 `.eh_frame`**。所以今天即使把 libunwind 链进来，
它也没东西可读——**`.eh_frame` 在编译阶段就没被生成**，
而 `user/linker.ld:30` 的 `/DISCARD/` 只是"就算生成了也丢掉"这第二道闸。
两道闸都要开，见 §4.2。

这条今天没有任何症状，因为**用户态全是 C**（`tools/build.py` 的 `USER_CFLAGS` 是
`-std=c17`，而且 `build_user` 只 `rglob("*.c")` / `rglob("*.asm")`，
`tools/build.py:284/299`：往 `user/bin/` 放一个 `.cpp` **不会被构建**）。

## 2. 与宿主 C 库之间的接口面（ABI 清单）

### 2.1 怎么读这张表

- **判定**只有三种：**有**（头与实现都在，给了 `文件:行号`）、
  **缺**（我逐项搜过，0 处命中）、**只有头**（`#include` 能过、链接会失败）。
- 依据是 `user/include/posix/`（头）与 `user/libposix/`（实现），加上 `user/libfe/libfe.c`
  （syscall 投影层）。
- ★ 这张表**不是"libc++ 文档里列的清单"**，而是"我在这棵树里逐个名字核对的结果" ★
  理由与 `16` §2.2.1 相同：防"我以为有"。项目的 `10-posix-layer.md` §9 那张表里
  "声明了却没实现"出现过 **4 次**。

### 2.2 内存分配

| 需要 | 判定 | 证据 / 缺口 |
|---|---|---|
| `malloc` / `free` / `calloc` / `realloc` | **有** | `user/libposix/heap.c:214/237/261/278`；声明 `user/include/posix/stdlib.h:22-25`。arena 建立在 `fe_mem_alloc`+`fe_mem_map` 上（`heap.c:112-154`），块头 32 字节、`HEAP_ALIGN 16`（`heap.c:39/50`） |
| `aligned_alloc` | **缺** | 全项目搜 → 0 处。C11 函数，libc++ 在 `align_val_t` 路径下会用到 |
| `posix_memalign` | **缺** | 全项目搜 → 0 处 |
| 过对齐的 `operator new(size_t, align_val_t)` | **缺（依赖上面的两个）** | 见 §6 待查第 4 条：需要核实 libc++ 在没有这两个函数时的降级路径 |

★ 为什么这一组是"改得动、但必须改" ★
`heap.c` 的 arena **只增不减**（`heap.c:22-25` 的注释自己写着"释放不还内存给内核……
进程生命周期短，今天是对的"）。libc++ 的容器在 `std::string` 增长、`std::vector` 扩容时
会大量走 `malloc`/`free`；这条边界在"编译几百个翻译单元"的场景下会第一次咬人
（`16` §1.2，那句逐字是"在 Qt 上**全部会咬人**"）。

### 2.3 进程终止与退出钩子

| 需要 | 判定 | 证据 / 缺口 |
|---|---|---|
| `abort` | **有（但语义弱）** | `user/libposix/stdio.c:770-775`：`fflush(NULL)` 之后 `_exit(134)`。**不是** POSIX 的 "SIGABRT + core"，是"退出码 134"。`stdio.c:774` 的注释写明这是刻意的："128 + SIGABRT，与 shell 的约定一致" |
| `_exit` | **有** | `user/libposix/stdio.c:765-768` → `fe_exit`（`user/libfe/libfe.c:85-93`，先 `fe_flush()` 再 `FE_SYS_THREAD_EXIT`） |
| `exit` | **有** | `user/libposix/stdio.c:757-763`：`fflush(NULL)` → `posix_run_atexit()` → `_exit`。顺序是对的（`stdio.c:748-754` 记着"不做这一条的症状是程序跑了却没输出"） |
| `atexit` | **有，但只有 16 个槽** | `user/libposix/stdlib.c:17-28`：`#define ATEXIT_MAX 16`，满了返回 -1 |
| `__cxa_atexit` | ★ **缺** | 全项目搜 `__cxa` → 0 处。**这是 libc++abi 的标准宿主钩子**，见下 |
| `__cxa_finalize` / `__dso_handle` | ★ **缺** | 同上 |

★ `__cxa_atexit` 不是一个可以省的东西 ★
它的签名是 `int __cxa_atexit(void (*)(void *), void *, void *dso)`，
与 `atexit(void (*)(void))` **语义不同**（带参数、带 dso、按注册逆序、需要按 dso 过滤）。
libc++abi 在**没有 `__cxa_atexit` 的宿主**上会退到 `atexit`——但那时
**全局析构与 `std::cout` 的刷新顺序不受控**。
更要紧的是那个 **16 槽**：`ATEXIT_MAX 16` 意味着
**第 17 个静态析构注册会静默返回 -1**。C++ 的静态析构注册是"每个翻译单元的
全局对象一次"，`qtbase` 的翻译单元数以百计——**16 必然不够**，而且失败方式是
"析构不跑"，不是报错。这条要作为 §4.4 的判据之一。

### 2.4 ★ 线程与同步（`std::mutex` / `std::call_once` / `std::thread` 都走它）★

| 需要 | 判定 | 证据 / 缺口 |
|---|---|---|
| `pthread.h` 本身 | ★ **缺** | `user/include/` 全树 0 处 |
| 线程创建的**底座** | **有** | `user/libfe/libfe.c:100-104` 包 `FE_SYS_THREAD_CREATE`（`user/include/fe_user.h:417`）；内核侧 `kernel/task/user.c:498-547` |
| `pthread_create` | **缺** | 全项目搜 → 0 处 |
| `pthread_join` | ★ **缺，且缺的是内核 ABI** | 见下 |
| `pthread_mutex_*` | ★ **缺** | 全项目搜 → 0 处 |
| `pthread_cond_*` | ★ **缺** | 同上 |
| `pthread_once` | **缺** | 同上。理论上可由 `pthread_mutex` + 原子变量拼出来，但要先有 mutex |
| `pthread_key_*`（`thread_local` 的析构） | **缺** | 同上 |
| TLS 基址（`thread_local` 的前提） | **有** | K9 已完成（`docs/11-kernel-next.md:56`）；`docs/08-os-completion.md:189` 记着内核从 `PT_TLS` 取初始化映像、每线程复制一份、切换时 `wrmsr(IA32_FS_BASE)` |

★ `pthread_join` 缺的不是实现，是原语 ★
`fe_thread_create` 返回的是**线程 id**（`kernel/task/user.c:546` 的 `return th ? th->id : 0;`），
**不是句柄**。用户态拿到的这个整数**没有任何"等它"的能力**：

- 内核里确实有一个 `fe_thread_join`（`kernel/sched/sched.c:851`），但它的参数是
  `struct fe_thread *`——**内核对象的裸指针**，收 `FE_ERR_INVAL`（`sched.c:853-855`）；
- 它的等待方式是**有界自旋**：`while (t->state != FE_THREAD_DEAD) { fe_thread_yield(); if (++spins >= 2000000u) { 打印超时并放弃 } }`
  （`kernel/sched/sched.c:866-874`）；
- `docs/13-tasks-and-kill.md:357` 明写："而用户态的 `join` 语义今天不存在"。

所以 `pthread_join` 需要的是一条**新的内核 ABI**（把"线程"变成一种可等待的对象），
不是"在用户态包一层"。`16` §2.2 的同一结论在本文被独立复核为真。

### 2.5 ★★ 两个硬缺口之一：`math.h` / `libm` 完全不存在 ★★

核实：全项目搜 `math.h`、`libm`、`sin(`、`sqrt(` → 用户态 **0 处命中**。

唯一一次沾边是 `user/libposix/fmt.c:122` 的一句注释：

```
if (v != v) {                       /* NaN：自己判，不用 <math.h> */
```

也就是 **`%f` 是手算的，连 `isnan` 都没用库**。

| 需要 | 判定 |
|---|---|
| `math.h` | ★ **缺** |
| `libm`（`sin`/`cos`/`exp`/`log`/`sqrt`/`pow`/`floor`/`fmod`…） | ★ **缺** |
| `float.h` / `stdint.h` / `limits.h` 里的浮点常量宏 | **有**（clang 内建，`toolchain/llvm/lib/clang/23/include/`） |

**它挡谁**：libc++ 本身对 `libm` 的依赖很窄（`<cmath>` 的转发、`std::to_chars` 的浮点路径），
所以 **S1 大概率不需要 `libm`**；但 `16` §4 的 S5 已经把"`libm` + 完整 POSIX 头覆盖"
列成独立阶段，理由是 Qt 的几何与绘制大量用它（`16` §2.2 的 `libm` 行）。

★ 判据要分开写，别混 ★
- S1 的验收（§5）**允许不依赖 `libm`**；
- 但 S1 的程序里**不能出现任何 `#include <cmath>`**，否则它测的是 `libm` 而不是 libc++。
  这条如果不写死，"S1 过了"会变成一个含义模糊的句子。

### 2.6 ★★ 两个硬缺口之二："等一个用户地址"的等待原语不存在 ★★

这是 `16` §2.3 盘点出的最大缺口，本文独立复核为真。

核实：全项目搜 `futex` / `wait_addr` / `condvar` → **内核 0 处、用户态 0 处**。

| 今天有什么 | 它等的是什么 | 为什么顶不上 |
|---|---|---|
| `fe_wait_any`（K4 ✅，`docs/11-kernel-next.md:153` 是它的完成节标题） | **内核对象**（端点 / 通知） | 条件变量的条件是**用户内存里的一个值**，不是内核对象 |
| `fe_notification_wait`（`user/include/fe_user.h:590`） | 一个通知对象的置位掩码 | 同上。而且它不能原子地"检查用户内存的值，不等于期望就返回"——那正是 futex 语义的定义 |
| `fe_sleep_ms` / `fe_yield`（`user/libfe/libfe.c:106/95`） | 时间 / 让出 | 睡眠是"睡够时间"，不是"等某个值变化"。让出是自旋的组件，见下 |

**为什么自旋在这个内核上不是"慢"，而是"死"**：

- `tools/run_qemu.py` 的 QEMU 默认是 **`-smp 1`**（`16` §2.3 已核实）；
- 单核上，一个自旋的等待者**占着唯一的 CPU**，被等的线程永远跑不到。
  症状不是"慢"，是**卡死**。

**真实难度（`16` §2.3 的结论，本文复核同意）**：这是一条**新的等待对象类型**，
要接进 `fe_wait_any` 的等待节点机制。★ 它今天**连"待办"都没有列在 K 清单里** ★
（`docs/11-kernel-next.md:48-56` 的 K1…K9 里，最接近的是 K4 `wait_any`，
但它等的是内核对象。K 清单里没有这一项。）

**代价必须先写死一条**（`16` §2.3 的替代方案，S4 会在这里收账）：

> **不能假装 `pthread_cond_t` 就是 48 字节。**

两条候选出路（都不许含糊）：

| 候选 | 做法 | 代价 |
|---|---|---|
| **A. 内核加"等一个用户地址"** | 新等待对象 = 用户地址；内核原子地"检查值 + 入队睡眠" | 内核新增机制；要接进 `fe_wait_any` 的等待节点，处理丢唤醒窗口 |
| **B. 用通知对象伪装** | `pthread_mutex_t`/`pthread_cond_t` 里放一个**内核通知对象句柄**的封装 | **ABI 与 libc++ 假设的尺寸/语义不一致**，必须写在明处；`sizeof(pthread_cond_t)` 不再是平台无关的常量 |

### 2.7 时间

| 需要 | 判定 | 证据 / 缺口 |
|---|---|---|
| `fe_clock_ns()`（单调、纳秒、TSC 制） | **有** | `user/libfe/libfe.c:111-114`；K7 完成（`docs/11-kernel-next.md:54`） |
| `fe_clock_info()`（自述精度） | **有** | `user/libfe/libfe.c:118-124`；`user/include/fe_user.h:430-438` |
| `clock_gettime` | ★ **缺** | 全项目搜 → 0 处。**可包**：`CLOCK_MONOTONIC` ← `fe_clock_ns()` |
| `CLOCK_REALTIME` | ★ **缺，且不该假装有** | 没有 RTC。`posix.c:476-501` 的注释立了规矩："时间戳一律 0：没有 RTC……**报 0 而不是编一个时间**"（`docs/08-os-completion.md` 已知限制 #6 同条） |
| `nanosleep` | **缺** | 但底座在：`usleep` 已实现（`user/libposix/posix.c:750-758`），**粒度是 1 ms**（`posix.c:753-755`：不足 1 ms 也要真的让出一次） |
| `sleep` | **有** | `user/libposix/posix.c:744-748` |
| `time.h` / `sys/time.h` | ★ **缺** | 全项目搜 → 0 处 |

★ 时区与 `std::chrono` 的坑要提前记 ★
`localdate`/`tzdb` 需要 IANA 时区库。libc++ 有一个厂商开关可以关掉它
（`LIBCXX_ENABLE_TIME_ZONE_DATABASE`，见 §3.4）。**这条今天不会痛，S4 会。**

### 2.8 文件 IO

| 需要 | 判定 | 证据 / 缺口 |
|---|---|---|
| `write` | **有** | `user/libposix/posix.c:335`；stdout/stderr 通向控制台 |
| `read` | **有（但读控制台是 `ENOSYS`）** | `user/libposix/posix.c:292`；★ `console_read` 明确 `errno = ENOSYS; return -1;`（`posix.c:285-290`），注释："读控制台没有意义……明确返回 ENOSYS 而不是读到 0 字节" |
| `open` | **有** | `user/libposix/posix.c:187-253` |
| `close` | **有** | `posix.c:261` |
| `fstat` | **有** | `posix.c:506`；`struct stat` 见 `user/include/posix/sys/stat.h:43-57` |
| `stat` | **有** | `posix.c:504` |
| `lseek` | **有** | `posix.c:389` |
| `isatty` | **有** | `posix.c:723-730` |
| `fsync` | **有** | `posix.c:460` |
| `O_APPEND` 语义 | ★ **缺，而且失败方式是静默的** | 见下 |
| `stdin`（真正可读的） | ★ **缺** | `read(0, …)` 走 `console_read` → `ENOSYS` |

★ 关于 `O_APPEND`：本文与 `docs/16` 的表述**不一致**，以下是我读到的 ★
- `16` §2.4 的表里写的是："`O_APPEND` | `10-posix-layer.md` §9 边界表：
  **返回 `ENOSYS`（不假装支持）**；`fcntl.h` 里 `O_APPEND` 常量有，fsd 没有语义"。
- 我读到的**不是**返回 `ENOSYS`：
  - `user/include/posix/fcntl.h:21` 有 `#define O_APPEND 00002000`；
  - `user/libposix/posix.c:245` 只是把它**存进 fd 表**：`e->flags = (u32)flags;`
  - `user/libposix/posix.c:373` 的写路径按**当前偏移**写：
    `fe_fs_write(g_fs_handle, e->path, (u64)(e->offset + (long)done), …)`，
    然后 `e->offset += (long)done;`（`posix.c:385`）。
  - **全文件搜 `O_APPEND` 只命中 `fcntl.h` 的定义与 `posix.c:245` 存 flags 这一行，
    没有任何一处读取它。**
- 也就是说：**`O_APPEND` 被接受、被记住、但被忽略**——打开不会失败，
  写入从偏移 0 开始**覆盖**已有内容。这比"返回 `ENOSYS`"严重一档：
  它正是 `fcntl.h` 头注释自己警告的那种失败（`user/include/posix/fcntl.h:4-6`：
  "我们**不支持**的位必须能被识别出来并报错，不能当成'没给'——
  那会让'要追加写'静默变成'覆盖写'"）。
  **这一条应作为一条独立的缺口记录，而不是"已经如实报错"。**

（`user/libposix/posix.c:100` 确实有 `case FE_ERR_NOTSUP: return ENOSYS;`——那是
**错误码翻译**，不是 `O_APPEND` 的路径。`posix.c` 里真正 `ENOSYS` 的是
`chdir:719`、`pipe:763`、`mkdir:770`、`chmod:777`、`rmdir:784`、`rename:791`、
`console_read:288`。）

### 2.9 内存映射与保护

| 需要 | 判定 | 证据 / 缺口 |
|---|---|---|
| 内核 ABI：`fe_mem_alloc` / `fe_mem_map` / `fe_mem_unmap` / `fe_mem_info` | **有** | `user/libfe/libfe.c:126-145`；声明 `user/include/fe_user.h:439-449` |
| 用户态已在用 | **有** | `heap.c` 的 arena 就是 `fe_mem_alloc` + `fe_mem_map`（`heap.c:112-154`） |
| `sys/mman.h` + `mmap` / `munmap` | ★ **缺** | 全项目搜 `mmap` → 只命中 `fe_user.h:446` 的注释与 `hxtest/main.c:620` 的测试说明 |
| `madvise` | ★ **缺** | 0 处 |
| `mprotect` | ★ **缺（内外都没有）** | 内核 0 处、用户态 0 处。`kernel/ld/elf.c:119` 的注释写着"`mprotect`/`munmap` 都要它"——**当初就知道会需要，只是没做**（`16` §2.3 已核实，本文复核一致） |
| `mmap` 的**分配器**角色 | **有替身** | libc++ 的 `operator new` 大块分配走宿主 `malloc`；而我们的 `malloc` 在 arena 不够时自己 `fe_mem_alloc`+`fe_mem_map`（`heap.c:113-129`）。**所以 S1 不需要 `mmap`** |

★ 但 `mprotect` 缺了不是只有 JIT 受影响 ★
`16` §2.3 那张表里有一行容易被忽略：**`operator new` 的大块分配走 `mmap` 后
可能要 `madvise`/`mprotect`**。今天的 arena 只增不减，所以"大块还回去"这条路
本来就没走通；`mprotect` 的缺席在这一层**暂时不产生新症状**，
但它会在 libc++ 的 `std::pmr` / 大块分配路径上第一次显形。**这一条是"待实测"**（§6）。

### 2.10 环境变量、locale、字符串与杂项

| 需要 | 判定 | 证据 / 缺口 |
|---|---|---|
| `getenv` / `setenv` / `environ` | **有** | `user/libposix/stdlib.c:217`（`getenv`）；`extern char **environ` 声明在 `user/include/posix/stdlib.h:52`；`__posix_set_envp` 由 `user/libfe/start.asm:38` 在 `main` 之前调用 |
| `strerror` | **有** | `user/libposix/string.c:199`（该文件紧跟其上的注释逐字："★ 每个 errno 一句人话，而不是 \"error 2\" ★"） |
| `setlocale` / `locale.h` | ★ **缺** | 全项目搜 → 0 处。`16` §2.2.1 已核实 `<wchar.h>`/`<locale.h>` 都没有 |
| `strcoll` | **有（退化成字节序）** | `user/include/posix/string.h:26`，注释逐字："无 locale：等同 `strcmp`" |
| `strdup` | **有** | `user/libposix/string.c:147` |
| `memcpy`/`memset`/`memmove`/`strlen`/`strcmp` | **有**（两份，libfe 那份优先） | libfe：`user/libfe/libfe.c:666`（`memcpy`，注释说明用 `__builtin_memcpy` 搬 16 字节块让编译器选 `movups`/`movaps`）、`:689`（`memset`）、`:949`（`strlen`）、`:958`（`strcmp`）；libposix：`user/libposix/string.c:34`（`memmove`）。★ 谁赢由链接顺序决定，`tools/build.py:310-311` 写明"重复符号由先出现的那个胜出，而 libfe 里的 `memcpy`/`malloc` 是经过 SIMD 与自检的那一份，应当优先" ★；`tools/build.py:330` 的 `--undefined` 清单点名了 `malloc`/`free`/`memmove`/`memset`/`write`/`read`/… |
| `errno` | **有，但是进程级一份** | `docs/10-posix-layer.md:264` 已知边界："`errno` 是进程级……多线程会串"。★ K9 只解决 TLS 基址，**`errno` 自己还是全局的**——多线程下 libc++ 的错误报告会互相覆盖 |
| `__errno_location`（glibc 形态） | **缺，也不需要** | libc++ 用 `errno` 宏，不走 `__errno_location` |

★ locale 这一组为什么可以缺 ★
libc++ 对 locale 相关的 C 函数（`setlocale`、`localeconv`、`strftime`…）走的是
**弱符号/条件调用**，所以在没有它们的宿主上应当优雅降级。**但"应当"是文档说法，
不是实测**——§6 待查第 5 条把它列成必须核实的项。
（`16` §4 的 S4 也把这一条列为"最先看哪里"：`libc++` 的 `__config` 对宿主环境的假设、
`<features.h>` 不存在、locale 相关。）

### 2.11 内核侧要补的（与本清单的关系）

上面标"缺"的项里，**只有三条的根在内核**，其余全在用户态：

| 缺口 | 根在哪一层 | 为什么用户态做不到 |
|---|---|---|
| 等一个用户地址（§2.6） | **内核**（新等待对象） | 要原子地"检查用户内存 + 入队睡眠"，用户态拼不出来（丢唤醒窗口） |
| `pthread_join`（§2.4） | **内核**（线程要变成可等待对象） | 今天的 `fe_thread_create` 只给一个 id；等线程的内核函数收的是内核指针 |
| `mprotect`（§2.9） | **内核**（VMA 属性变更） | 要**同时**改区间表与已映射页的 PTE，方向不能反（`16` §2.3 已写清代价） |

其余（`__cxa_atexit`、`aligned_alloc`、`clock_gettime`、`nanosleep`、`mmap` 包装、
`math.h`/`libm`、`pthread_mutex_*` 的用户态那一半）**全是 `user/libposix/` 的事**，
与 `10-posix-layer.md` §5 的分层一致：**内核里不出现 POSIX 的名字。**

## 3. 构建形态：两条候选路线的代价对比

### 3.1 先确认一件事：libc++ **没有**正式支持我们的目标

`libcxx.llvm.org` 首页的"Platform and Compiler Support"表里，Embedded 一栏只有
**`arm` + `picolibc`** 一项；Linux 一栏写着
"Only glibc-2.24 and later and **no other libc is officially supported**"。
同一页紧接着的一句很重要：

> Generally speaking, libc++ should work on any platform that provides a fairly
> complete C Standard Library. It is also possible to turn off parts of the
> library for use on systems that provide incomplete support.

来源：<https://libcxx.llvm.org/>（正文"Platform and Compiler Support"一节）。

★ 这句话要当成**待证伪的假设**，不是保证 ★
"should work"不是"works"。所以 §5 的验收里**必须有一条反向对照**，
用来区分"真的 libc++ 在跑"与"恰好编过、恰好跑出一样的输出"。

### 3.2 两条路线

| | **(a) 宿主侧交叉编出目标格式的静态库** | **(b) 等 FEKernel 自举出编译器后在机器上构建** |
|---|---|---|
| 编译器在哪跑 | Windows（今天的 `toolchain/llvm/bin/clang.exe`） | FEKernel 里（S7 的产物） |
| 谁来驱动 | 宿主上的 CMake + Ninja | 目标机上的 CMake/make（S6 的产物） |
| 今天能不能做 | ★ **今天就能做**（前提见 §3.3） | 不能。S6/S7 都还没做 |
| 需要先写什么 | **一个 CMake toolchain file**（`x86_64-unknown-none-elf`） | 整个 S1–S7 |
| 暴露什么 | 只暴露"这套运行库配不配得起来" | **外加**整个操作系统（进程、管道、shell、文件系统、内存） |
| 失败时的诊断成本 | 低（宿主上有完整的工具与日志） | 高（失败可能出在任何一层） |
| 与"本机自举终局"的关系 | ★ **不冲突**，见 §3.3 | 终局本身 |

### 3.3 ★ 明确推荐先做 (a)，并说明为什么它不违背"本机自举终局" ★

**理由一：口径已经写死在 `16` §6.1 里。**

> ★ 口径必须写死 ★ **"本机自举"指的是"在 FEKernel 上编译 `qtbase`"，
> 而不是"从零开始在 FEKernel 上长出 `clang`"。** 前者有明确判据（S10），
> 后者是一条无限长的路。

(a) 造的**不是**编译器，是**编译器的被编译物**（libc++ 的静态库）。
S10 的判据是"构建过程中没有调用宿主机"（`16` §4 的 S10 终点行）——
那条判据管的是 **`qtbase` 那一次构建**，不是"每一个 .a 都必须长在目标机上"。

**理由二：这与 GCC 的 stage 0、LLVM 的 host tools 是同一件事。**
`16` §6.1 的候选 A 已经把这条理由写过：**它不违反本机自举的口径——判据是
"`qtbase` 在 FEKernel 上被编译"，不是"`clang` 在 FEKernel 上被编译"**，
并推荐了这一条。

**理由三：它把"未知"和"已知"分开。**
S1 的整条路上有两个独立的未知：
① **这套运行库在 freestanding 的 `x86_64-unknown-none-elf` 上配不配得起来**；
② **这个内核能不能让一个 C++ 程序活着跑完**。
(b) 把两个未知叠在一起；(a) 只留 ②。★ 这正是 `16` §4"顺序的四条理由"第 1 条
的同一逻辑：把未知放前面，是为了让"要不要继续"这个决定在花掉大量工作之前做出。★

**理由四：(a) 的产物是 (b) 的输入，不是替代品。**
即使将来 S7 做出了目标侧 `clang`，libc++ 的**头文件**（`include/c++/v1`）
仍然需要一次"谁生成它"的过程；(a) 产出的 `install/` 目录
（`include/c++/v1` + `lib/libc++.a` + …）是 (b) 拿来就用的东西。
**先做 (a) 不会产生任何要在 (b) 阶段丢掉的工作。**

★ 一句口径 ★
**(a) 造的是货，(b) 造的是工厂。先有货，工厂才验得出来自己在造什么。**

### 3.4 形态：CMake 工程与必须显式声明的开关

libc++ 的官方构建方式是在 monorepo 的 `runtimes` 目录上 root 一次 CMake，
官方给的默认形态是（逐字引用官方文档的命令）：

```
$ cmake -G Ninja -S runtimes -B build -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;libunwind"
$ ninja -C build cxx cxxabi unwind
$ ninja -C build install-cxx install-cxxabi install-unwind
```

来源：<https://libcxx.llvm.org/VendorDocumentation.html>（"The default build"一节）。

**与本项目相关的厂商选项**（全部来自上面那一页的"Vendor Configuration Options"，
以及 19.1.0 版同名页面
<https://releases.llvm.org/19.1.0/projects/libcxx/docs/BuildingLibcxx.html>）：

| 选项 | 官方默认 | 我们要设成 | 为什么 |
|---|---|---|---|
| `LIBCXX_ENABLE_SHARED` | `ON` | **`OFF`** | 内核**没有动态链接**（`10-posix-layer.md` §7：独立项目，不做）。`USER_CFLAGS` 是 `-fno-pic -fno-pie`（`tools/build.py:100-101`） |
| `LIBCXX_ENABLE_STATIC` | `ON` | `ON`（保持） | 这是我们要的那一个。官方原话："Either `LIBCXX_ENABLE_SHARED` or `LIBCXX_ENABLE_STATIC` has to be enabled" |
| `LIBCXX_CXX_ABI` | `libcxxabi` | `libcxxabi`（保持） | 只有它配 `LIBCXXABI_USE_LLVM_UNWINDER` |
| `LIBCXXABI_USE_LLVM_UNWINDER` | `ON` | `ON`（保持） | 我们要的就是 libunwind（§1.1） |
| `LIBCXX_ENABLE_STATIC_ABI_LIBRARY` | `OFF` | **`ON`** | 静态链接下把 ABI 库并进来，避免"多一个 .a 的顺序问题" |
| `LIBCXX_ENABLE_FILESYSTEM` | `ON` | **待定**（见 §6 第 6 条） | `<filesystem>` 需要 `stat`/`mkdir`/`rename`/`opendir`；`mkdir`/`rename` 今天 `ENOSYS`（`posix.c:770/791`）。**但如果关掉它，`qtbase` 会用到 `QFileSystemEngine` 的哪条路就变了**——这是"能编出来"与"跑得对"的边界，不能顺手关 |
| `LIBCXX_ENABLE_WIDE_CHARACTERS` | `ON` | **候选 `OFF`** | 官方原话："allows the library to work on top of a C Standard Library that does not provide support for `wchar_t`"。我们要核实 `user/include/posix/` 里有没有 `wchar.h`（§6 第 7 条） |
| `LIBCXX_ENABLE_TIME_ZONE_DATABASE` | `ON` | **`OFF`** | 没有 RTC、没有 IANA tzdata（§2.7） |
| `LIBCXX_ENABLE_EXCEPTIONS` | `ON` | **`ON`** | S1 要 `throw`/`catch` |
| `LIBCXX_ENABLE_RTTI` | `ON` | **`ON`** | 官方原话："This option may only be set to `OFF` when `LIBCXX_ENABLE_EXCEPTIONS=OFF`"。S2 要 `dynamic_cast`/`typeid` |
| `LIBCXX_INCLUDE_TESTS` / `LIBCXX_INCLUDE_BENCHMARKS` | `ON` | **`OFF`** | 我们跑不了 libc++ 的测试套件（它需要宿主 POSIX）；开着只是拖构建 |
| `LIBCXX_ENABLE_ABI_LINKER_SCRIPT` | UNIX 上默认 `ON` | `OFF` | 没有动态链接，没有 `libc++.so` 可写脚本 |
| `CMAKE_CXX_COMPILER_TARGET` | — | `x86_64-unknown-none-elf` | 官方文档在"CMake + ninja (MSVC)"一节明确用这个变量指定三元组 |
| `CMAKE_C_COMPILER` / `CMAKE_CXX_COMPILER` | — | `toolchain/llvm/bin/clang.exe` / `clang++.exe` | 见 §3.5 |
| `LIBCXX_ADDITIONAL_COMPILE_FLAGS` / `LIBCXXABI_…` / `LIBUNWIND_…` | `""` | 见下 | 官方文档："Additional compile flags to use when building the runtimes" |

★ `LIBCXX_ADDITIONAL_COMPILE_FLAGS` 上必须放什么：**把 `tools/build.py` 那两行反过来** ★

```
-fno-asynchronous-unwind-tables   ← tools/build.py:102，必须**去掉**
-fno-unwind-tables                ← tools/build.py:103，必须**去掉**
```

理由：libunwind 的输入就是 `.eh_frame`（§1.3）。build.py 的 C 构建可以继续关着，
**C++ 运行库与 C++ 程序不能关**。这一条是本文件里最容易漏掉的一个开关，
因为它藏在**另一个文件的另一组变量的另外两行**里。

（其余当继承的：`-ffreestanding`、`-nostdlib`、`-fno-stack-protector`、
`-fno-pic -fno-pie`、`-mno-red-zone`、`-mcmodel=small`、`-msse2 -mfpmath=sse`，
见 `tools/build.py:93-115`。★ 但**不要**继承 `-fno-builtin`（`build.py:98`）：
那是给内核的，用在标准库自己的构建上没有好处。★）

### 3.5 ★ 今天做 (a) 会缺的一件工具：宿主上没有 CMake ★

本文实测（只查 PATH，没有跑构建）：

```
cmake      -> (不在 PATH)
ninja      -> C:\Users\hjcdu\AppData\Local\Programs\Python\Python310\Scripts\ninja.exe
clang      -> (不在 PATH)
clang++    -> (不在 PATH)
ld.lld     -> (不在 PATH)
python     -> C:\Users\hjcdu\AppData\Local\Programs\Python\Python310\python.exe
```

- **Ninja 有**（Python 环境里带了一个）；
- **clang / clang++ / ld.lld「不在 PATH」但项目内有**：
  `tools/toolchain.py:50-74` 的三级回退是"项目内便携版 → PATH → `C:\Program Files\LLVM`"，
  而 `toolchain/llvm/bin/` 下 `clang.exe`/`clang++.exe`/`ld.lld.exe`/`llvm-ar.exe` 都在；
- ★ **CMake 没有** ★：`tools/fetch_toolchain.py` 只下载三样东西
  （`llvm` / `qemu` / `limine`，`tools/fetch_toolchain.py:57-88`），**不含 CMake**。

所以 (a) 的第一步不是写 toolchain file，而是**先决定 CMake 从哪来**：

| 候选 | 代价 |
|---|---|
| 手工下载 CMake 到 `toolchain/cmake/`（照 `fetch_toolchain.py` 的既有形态加一项） | 小。但要改 `tools/fetch_toolchain.py`——**本文不改任何工具**，这一条留给下一次开工 |
| 用 `pip install cmake`（Python 环境里已有 `ninja` 的先例） | 小。但引入一条"工具来自 pip"的依赖，与项目"工具链在 `toolchain/` 下可复现"的既有做法不一致 |
| 不用 CMake，手工列出 libc++ 的源文件编译 | ★ 不推荐：那是把 libc++ 的构建配置**抄一份进我们仓库**，而它每个版本都会变。这与"不抄现有内核"同一条纪律 |

★ 这一条要写进路线，不能等撞上再说 ★ 它是 (a) 的第一个真实阻塞点，
而且**与内核无关**（纯宿主侧）。

### 3.6 产物怎么进今天的构建

今天 `tools/build.py` 的链接方式是**逐个 `.o` 文件**，不是 `.a`：

- `build_group()` 返回 `list[Path]`（`tools/build.py:190-198`）；
- 链接命令行是 `-nostdlib` + `*objs`（`tools/build.py:334-346`）；
- 库不是一个归档，是"一堆目标文件按顺序传进去"。

所以 libc++ 的静态库接进来有两种形态（都要在动手前定死）：

| 形态 | 做法 | 代价 |
|---|---|---|
| **摊平** | libc++ 的编译产物也摊成一堆 `.o` 传进去 | 需要把 CMake 的构建产物**逐个文件列举**出来。`llvm-ar`/`llvm-ranlib` 都在（`toolchain/llvm/bin/`），但"把 `.a` 摊平"这件事今天没有工具做 |
| **归档** | 改成 `.a` + `--whole-archive`/`-u` | ★ 会碰到 `--gc-sections` 的既有坑：`tools/build.py:322-333` 那段注释记着"按目标文件链接 + `--gc-sections` 会把没有任何节引用的编译单元整个丢掉"，症状是 `undefined symbol: malloc`。**libc++ 的 `.a` 里可能有几百个 .o，同一个坑会以更大的规模重演** |

★ 本文不给结论 ★ 这是"实测之后才知道哪个便宜"的一类决定，
判据写在 §6 待查第 8 条。

### 3.7 CMake 从哪来：三个候选与取舍

§3.5 只说了"宿主上没有 CMake"。这一节把它**变成可执行的决策**。

#### 3.7.0 先核实：今天的工具链机制到底是什么样（不许猜）

**(1) `tools/fetch_toolchain.py` 的下载清单与校验方式**

清单是一个 `ARTIFACTS` 列表，**三项**（`tools/fetch_toolchain.py:55-90`）：

| key | 文件名 | 版本来源（常量） | `expect_bytes` | `kind` | 标志文件（`produce`） |
|---|---|---|---|---|---|
| `llvm` | `clang+llvm-23.1.2-x86_64-pc-windows-msvc.tar.zst` | `LLVM_VERSION = "23.1.2"`（`:38`） | `276469918`（`:65`） | `tar.zst` | `toolchain/llvm/bin/clang.exe`（`:67`） |
| `qemu` | `qemu-w64-setup-20260811.exe` | `QEMU_INSTALLER`（`:41`） | `0`（`:73`，**不校验**） | `7z-nsis` | `toolchain/qemu/qemu-system-x86_64.exe`（`:75`） |
| `limine` | `limine-binary.zip` | `LIMINE_VERSION = "12.9.0"`（`:40`，**只用在 URL 里**） | `2020779`（`:86`） | `7z-zip` | `toolchain/limine/BOOTX64.EFI`（`:88`） |

★ 校验方式是**字节数**，不是摘要 ★
- `download()` 的跳过判据是 `dest.stat().st_size == art.expect_bytes`（`:104`）；
- 下载后也是比字节数，不符就换源续传（`:123-125`）；
- ★ **全文件搜 `hash`/`sha`/`md5`/`digest` → 0 处命中。没有哈希校验，没有签名校验。** ★

这与 `third_party/README.md` 里"保留上游许可证"的严谨程度不在一个量级上，
但它是**既有事实**，不是本文要改的东西。写在这里是因为：
如果候选 (a) 要把 CMake 加进这份清单，就必须知道**这份清单的安全模型是"字节数"**。

其余机制（选 (a) 时要照着做的）：
- 下载用系统 `curl.exe`（`CURL`，`:34`），参数含 `--retry 20`、`-C -`（断点续传）、
  `--speed-limit 4096 --speed-time 30`（`:111-115`）；下载目录 `toolchain/downloads/`（`:32`）；
- 解包用 `C:\Program Files\7-Zip\7z.exe`（`SEVENZIP`，`:35`）；
- 解包后要求"标志文件存在"，否则报错（`:176-181`）；
- `--only` 的合法取值就是清单里的 key（`:187`，`choices=[a.key for a in ARTIFACTS]`）。

**(2) 构建是怎么发现工具链的：三级回退**

`tools/build.py:28` 导入 `toolchain as tc`；所有工具都走 `tools/toolchain.py` 的同一形状：

```
_first_file([ 项目内便携版 ,  PATH 上找  ,  C:\Program Files\... ])
```

| 工具 | 候选顺序（`tools/toolchain.py`） |
|---|---|
| clang | `:52` `toolchain/llvm/bin/clang.exe` → `:53` `…/clang` → `:54` `shutil.which("clang.exe")`/`("clang")` → `:55` `C:\Program Files\LLVM\bin\clang.exe` |
| ld.lld | `:68` `toolchain/llvm/bin/ld.lld.exe` → `:69` `…/ld.lld` → `:70` `which` → `:71` `C:\Program Files\LLVM\bin\ld.lld.exe` |
| llvm 其他工具 | `:81` `toolchain/llvm/bin/<name>.exe` → `:82` `which` → `:83` `C:\Program Files\LLVM\bin\<name>.exe` |
| nasm | `:92` `toolchain/nasm/nasm.exe` → `:93` `which` → `:94/95` `C:\Program Files[(x86)]\NASM\nasm.exe` |
| qemu / limine / ovmf / VBoxManage | `:124-141` / `:106-114` / `:149-157` / `:168-173` |

★ 语义写死在文件开头（`tools/toolchain.py:6`）★
> \* 优先使用项目内便携工具链 `toolchain/`，其次才是系统 PATH。

而**回退是静默的**，所以 `build.py` 专门做了一次告警
（`tools/build.py:205-222`，`report_toolchain()`）：

```
tools/build.py:219-222
    if local not in clang.parents and clang.parent != local:
        print("[build] ⚠ clang 不是项目内便携版！可能被别的工具链抢走了。")
        print("[build]   修复: python tools/fetch_toolchain.py")
        print("[build]   注意: 混用不同工具链的目标文件会在链接期报出误导性的错误。")
```

★ 这一条**决定了 (a) 与 (b) 的取舍形状** ★
`toolchain/` 优先于 PATH 是这个项目**已经写下来的**机制。
把它用在 CMake 上，得到的是"项目内那份赢了"的**同一个**语义，
而且 `report_toolchain()` 已经把"被 PATH 抢走"这件事当成一类要喊出来的故障。

**(3) `tools/add_spdx.py` 的 `SKIP_DIRS` 里到底有没有 `toolchain`**

有，逐字：

```
tools/add_spdx.py:35
SKIP_DIRS = {"build", "toolchain", ".git"}
```

判定发生在唯一的收集入口 `collect()` 里，**按路径分段精确相等**：

```
tools/add_spdx.py:64-65
        if any(part in SKIP_DIRS for part in path.relative_to(ROOT).parts):
            continue
```

★ 两条连带结论 ★
1. 往 `toolchain/` 里放**任何**文件（包括 CMake）都**不需要**动 `add_spdx.py`，
   也不会有"给别人的代码补 0BSD 头"的许可证风险——
   `SKIP_DIRS` 的语义是"既不检查也不加头"，与 `VENDORED_DIRS`（`:51/67`）同一种形状。
2. 但 `SKIP_DIRS` 同时**跳过了 `toolchain/` 的源码管理**：
   `third_party/README.md:76-77` 明确区分过这两者——
   > 和 `toolchain/` 的区别：`toolchain/`（下载下来的 LLVM/Limine 等）是被既有的
   > `SKIP_DIRS` 跳过的（**下载物、不是我们要维护的源码**），和本目录的许可证豁免是两回事。

   所以 `toolchain/cmake/` 放的是**下载物**，不是我们要维护的源码。
   （许可上没问题：CMake 是 BSD-3-Clause，见 §6 待查第 1 条的核实项。）

#### 3.7.1 ★ 候选 (c) 的裁决：它今天**根本走不通** ★

先把最容易被当成"省钱"的那条路判掉，因为它有一个**编译期硬错误**做判据。

上游 `libcxx/include/__config` 的开头（逐字，来自
<https://raw.githubusercontent.com/llvm/llvm-project/main/libcxx/include/__config>）：

```cpp
#ifndef _LIBCPP___CONFIG
#define _LIBCPP___CONFIG

#include <__config_site>
```

★ 这三行里没有任何 `__has_include` 保护 ★
`__config_site` 不是一个"有就用、没有就走默认值"的可选头。
它是**无条件 `#include`**。没有它，`#include <string>` 的**第一步**就
`fatal error: '__config_site' file not found`。

而且 `__config` 一路往下（同一文件 `#include <__configuration/hardening.h>`）
会要求另外两个宏，`hardening.h` 里是两条 `#error`：

```cpp
// libcxx/include/__configuration/hardening.h
#  ifndef _LIBCPP_HARDENING_MODE_DEFAULT
#    error _LIBCPP_HARDENING_MODE_DEFAULT is not defined. This definition should be set at configuration time in the \
`__config_site` header, please make sure your installation of libc++ is not broken.
#  endif
...
#  ifndef _LIBCPP_ASSERTION_SEMANTIC_DEFAULT
#    error _LIBCPP_ASSERTION_SEMANTIC_DEFAULT is not defined. ...
#  endif
```

**这条路的真实工作量（逐项）**：

| 要手工产出什么 | 上游由谁产出 | 不做的后果 |
|---|---|---|
| `__config_site` | CMake `configure_file` 从 `libcxx/include/__config_site.in` 生成 | ★ **编译期立即失败**（上面那三行） |
| `__assertion_handler` | CMake `configure_file(…/vendor/llvm/default_assertion_handler.in …)` | 只有非 default 的 hardening 模式才需要 |
| `module.modulemap` | CMake `configure_file` 从 `module.modulemap.in` | 只在用 Clang modules 时需要（我们不用） |
| `libcxx.imp` | CMake 跑 `utils/generate_iwyu_mapping.py` | 只给 IWYU 用（我们不用） |
| **源文件清单** | `libcxx/src/CMakeLists.txt` 的 `LIBCXX_SOURCES` | 漏一个 → 链接期 `undefined symbol` |
| **头文件清单** | `libcxx/include/CMakeLists.txt` 的 `set(files …)` | 漏一个 → 编译期 `file not found` |

**代价（三句话）**：

1. **配置逻辑没法"只抄一点点"**。那两份 `CMakeLists.txt` 加起来是几百行，
   而且它们不是"文件列表"——里面有条件逻辑，例如
   （`libcxx/src/CMakeLists.txt`，逐字）：

   ```cmake
   if (LIBCXX_ENABLE_THREADS)
     list(APPEND LIBCXX_SOURCES
       atomic.cpp barrier.cpp condition_variable_destructor.cpp condition_variable.cpp
       future.cpp mutex_destructor.cpp mutex.cpp shared_mutex.cpp thread.cpp )
   endif()
   ```

   抄一份进我们仓库 = **把 libc++ 的构建配置变成我们的长期维护物**，
   与"不抄现有内核"是同一条纪律的两种表现。
2. **升级一次就全过期**。上游每个版本都在动这两份清单
   （本文对比过：`libcxx/include/CMakeLists.txt` 的当前版里已有
   `__new/global_new_delete.h`、`__stop_token/*`、一整套 `__cxx03/` 冻结头，
   而 19 版的结构完全不同）。过期的症状是"某个头没被复制过去"，
   报错位置在**用户代码的 `#include`** 而不是清单里——最难查的一类。
3. **它换不到任何东西**。省下的是"装一个 CMake"，赔上的是上面两条。

★ 本文的裁决 ★ **候选 (c) 不做。**
但留一条**有意义的窄版本**：如果将来只需要**验证 header-only 的一部分**
（例如只想知道 `#include <string>` 能不能过），可以**手工写一个二十行的 `__config_site`**
配 `-I libcxx/include` 做烟测。那是"探针"，不是"构建形态"——
**它不能产出可链接的 `libc++.a`**，所以不解决 S1。

#### 3.7.2 候选对比

| | **(a) 放进 `toolchain/cmake/`** | **(b) `pip install cmake`** | **(c) 不用 CMake** |
|---|---|---|---|
| **为什么** | 与 portable LLVM 同一套路；`toolchain/` 优先于 PATH 已经是写下来的机制（§3.7.0 (2)） | Python 3.10 + pip 23.0.1 已经在本机（实测），`ninja` 也已经在 PATH 上（Python Scripts 下） | 想省掉一个宿主工具 |
| **代价** | ① `tools/fetch_toolchain.py` 要加第四项 `Artifact`（含 `expect_bytes`，而**这份清单的安全模型只是字节数**，§3.7.0 (1)）；② 要确认 CMake 官方 zip **自带 Ninja 还是不带**（待实测，§6 第 1 条） | ① 装在 Python 环境里，**与 repo 的 `toolchain/` 无关**：换机器/换 Python 就没了；② 版本随 pip 漂（今天拿到的不是项目钉住的）；③ ★ **pip 23.0.1 + Python 3.10 是否在 PyPI `cmake` 包的支持窗口内，本文没有核实**（§6 第 1 条）★ | 见 §3.7.1：**编译期硬错误**，不是一个选项 |
| **可复现性** | 高（版本可钉死、可校验、可离网重建） | 低（依赖 Python 环境状态） | — |
| **今天能不能用** | 能（但先要有人改 `fetch_toolchain.py`——**本文不改**） | ★ **能，且是唯一"今天零改动就能用"的** ★ | 不能 |
| **什么时候会咬人** | 加完就没事了 | ① 换机器/重装 Python；② `pip install --upgrade` 悄悄换 CMake 版本，而 **LLVM 24 起 CMake 最低版本会升到 3.31.0**（上游 `libcxx/CMakeLists.txt` 开头就为此打印警告："Starting with LLVM 24, the minimum version of CMake required to build LLVM will become 3.31.0, and using an older CMake will become an error."）；③ 与 `report_toolchain()` 的"被别的工具链抢走"是**同一类故障**，但 pip 那条路**没有对应的告警** | — |

**共同事实（两条候选都要知道）**：

- libc++ 自己的最低要求是 `cmake_minimum_required(VERSION 3.20.0)`
  （上游 `libcxx/CMakeLists.txt` 第一行，release/23.x 分支）；
- 生成器用 Ninja：`ninja 1.13.0` 已实测在本机 PATH 上
  （`C:\Users\hjcdu\AppData\Local\Programs\Python\Python310\Scripts\ninja.exe`）；
- Python 3.10.11 + pip 23.0.1（实测）。★ 注意：`libcxx/include/CMakeLists.txt` 会用
  `Python3_EXECUTABLE` 跑 `utils/generate_iwyu_mapping.py`
  （同文件里 `add_custom_command(... COMMAND "${Python3_EXECUTABLE}" ...)`），
  所以 Python 是**构建期的必需项**，不是可选项 ★。

#### 3.7.3 推荐：**(a) 为终局，(b) 为今天**

**口径（引 `docs/16-selfhost-path.md` 的定位变更与 §6.1）**：

> ★ 口径必须写死 ★ **"本机自举"指的是"在 FEKernel 上编译 `qtbase`"，
> 而不是"从零开始在 FEKernel 上长出 `clang`"。**

按这个口径，判据是"**被编译物**（`qtbase` / libc++）在哪台机器上被编"，而不是
"每个宿主工具长在哪台机器上"。★ 所以三个候选里**没有一个**因为"CMake 跑在 Windows 上"
而违反自举口径 ★——CMake 是**宿主构建工具**，与被编译物是两件事。
（换句话说：本节讨论的**不是自举原则问题，而是"宿主工具的可复现性"问题**。）

**真正的判据是四条：**

1. **可复现性**：换一台 Windows 机器，`git clone` + `python tools/fetch_toolchain.py`
   能不能把环境重建出来？→ (a) **能**，(b) **不能**。
2. **与既有机制一致**：项目的工具链纪律已经写成"项目内便携版优先于 PATH"
   （`tools/toolchain.py:6`），并且**专门为"被抢走"写了告警**
   （`tools/build.py:219-222`）。→ (a) 直接落进这条纪律；(b) 落在纪律**之外**。
3. **今天可执行**：S1 不能等 `fetch_toolchain.py` 被改。→ **(b) 今天就能做**。
4. **不新建长期债务**：把 CMake 藏在某个 Python 环境里，等于给这条路线加一条
   "★ 只有这台机器上跑得通"的隐含前提——这正是 §5.2 反向对照要防的那类假设。

**★ 结论（两句话，不矛盾）★**

> **今天：用 (b) 把 CMake 装上，只为把 S1 走通。**
> **原则性修复：把 CMake 加进 `tools/fetch_toolchain.py` 的 `ARTIFACTS`（= (a)）。**
>
> 两者不冲突：(b) 是**今天**的手段，(a) 是**这台机器之外**的保证。
> ★ 但必须记下一条 ★ **`ninja` 已经在 PATH 上这一条同样是"运气"，不是设计**——
> 它是随 Python 环境来的。走 (a) 时**要一起决定 `ninja` 从哪来**，
> 否则会出现"CMake 钉住了、Ninja 没钉住"的半通状态，
> 而 `tools/toolchain.py:147-148` 已经为 QEMU 记过同一类故障的形状：
> "否则会出现「QEMU 找到了、固件找不到」的半通状态"。

**判据（可证伪，等能跑构建时执行）**：

| # | 判据 | 反向对照 |
|---|---|---|
| D1 | 在**没有** Python 的环境语义下（把 `Scripts` 从 PATH 里去掉），`cmake --version` 仍然可用 | 去掉 PATH 之后**必须失败**——证明测的是 `toolchain/` 那份，不是 PATH 上别的 |
| D2 | `cmake --version` 与 `fetch_toolchain.py` 钉住的版本**逐字相符** | 故意改一个补丁号 → 必须**不一致** |
| D3 | 用 (a) 装出来的 CMake + PATH 上的 Ninja 能配出 libc++ 的 `build.ninja` | 把 Ninja 从 PATH 去掉 → 配置必须**明确失败**，不是静默换一个生成器 |

★ D3 的第二条尤其重要 ★ 它证明的是"Ninja 是从哪来的"这件事
**要么被钉住、要么会被发现**。

## 4. 与 `FE_VMA_MAX=32`、`user/linker.ld` 的关系

### 4.1 `FE_VMA_MAX = 32`（核实：**真**）

`16` §2.3 说"`kernel/include/fe/mm/vma.h:65` 写死 `#define FE_VMA_MAX 32`"。
本文核实结果：**行号与内容都相符**。

```
kernel/include/fe/mm/vma.h:61-70
/* 一个任务的区间表。定长数组而不是链表：
 * 真实程序（我们见过的）区间数在个位数；定长让插入/查找没有分配失败路径，
 * 也让"表满了"这件事有一个明确、可测的边界。
 * 上限取 32：够用，且整个结构 32*40 = 1.3 KiB，挂在任务上不心疼。 */
#define FE_VMA_MAX 32

struct fe_vma_table {
    struct fe_vma items[FE_VMA_MAX];
    u32 count;
};
```

**它怎么会让"libc++ 编出来了但跑不对"**：

1. **每个线程一段栈区间**。`fe_user_map_stack` 给每个栈 `fe_vma_add` 一个区间
   （`kernel/task/user.c:464-471`），而 `fe_user_thread_create` 在没给栈时
   自己调它（`kernel/task/user.c:507-518`）。
   默认栈 64 KiB（`FE_USER_STACK_SIZE`，`kernel/include/fe/user.h:19`）。
   ★ 也就是说：**`std::thread` 每起一个线程就吃掉一个 VMA 槽** ★。
   32 个槽里还要放 ELF 的代码/数据/BSS 段、主栈、`fe_mem_alloc` 的每个 arena 段。
2. **`fe_mem_alloc`+`fe_mem_map` 的每个 arena 段也是一个区间**。
   `heap.c` 的 chunk 链是**增长式**的（`CHUNK_MIN 64 KiB` → 翻倍 → `CHUNK_MAX 8 MiB`，
   `heap.c:60-61/145-147`），每个 chunk 一次 `fe_mem_map`。
   libc++ 的容器在编译期大量分配 → chunk 数增长 → 区间数增长。
3. **表满不是"退化"，是 `FE_ERR_NOSPC`**。`fe_vma_add` 的重叠/满表判定在
   `kernel/mm/vma.c`；`16` §2.3 引的是"表满 NOSPC"的实测行。
   症状是**跑到一半报错**，不是"启动就失败"——这一类最难查。

★ 修它属于哪一层：**内核** ★
它是 K6（VMA/按需分页）那一轮的**实现常量**，不是一条待办（`16` §5.3）。
两条候选：**调大**（`FE_VMA_MAX` 从 32 到 256，结构从 1.3 KiB 到 10 KiB/task）
或**改成动态表**（代价是引入分配失败路径——而 `vma.h:61-63` 那段注释
明确说"定长让插入/查找没有分配失败路径"是**刻意的**）。
★ 这个取舍必须写清代价才能动，不能顺手改大。★

**判据**：`16` §6.5 待实测第 5 条"`FE_VMA_MAX=32` 在真实 C++ 程序上到底够不够"，
时间点是 S1/S2。★ **本条已被 §7.1/§5.1 修订过一次** ★ 原文写的是
"S1 的程序要同时起 4 个以上 `std::thread`"；但 §7.1 核实 `_LIBCPP_HAS_THREADS`
是**全有或全无**且今天**跑不了**，所以 `std::thread` 移出了 S1。
**修订后的判据**：S1 **静态地**数段数（ELF 段 + 主栈 + 每个 arena chunk + 每个线程栈），
写成一张表与 32 比；**"4 个以上线程"这条移交给 `16` §4 的 S3**——
到那时它才是可跑的，也才测得准。

### 4.2 `user/linker.ld` 丢弃 `.eh_frame`（核实：**真**）

```
user/linker.ld:29-34
    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
        *(.llvm_addrsig)
    }
```

**它怎么会让"libc++ 编出来了但跑不对"**：
`throw` 的机制是 "libc++abi 分配异常对象 → `_Unwind_RaiseException`（libunwind）
→ libunwind 读 **`.eh_frame`** 里的 CFI 逐帧回退，在每一帧查
personality routine（`__gxx_personality_v0`），命中就跳到 landing pad"。
**`.eh_frame` 一丢，libunwind 走不出第一帧**：

- `catch` 分支不执行 → 落到 `std::terminate`；
- **栈上对象的析构不跑** → 资源泄漏，且不会有任何提示。

★ 而且今天有**两道闸**（比 `16` §2.5 写的多一道）★
1. `tools/build.py:102-103` 的 `-fno-asynchronous-unwind-tables -fno-unwind-tables`
   → **编译器根本不生成** `.eh_frame`；
2. `user/linker.ld:30` 的 `/DISCARD/` → 就算生成了也丢。

只开第 2 道闸（改链接脚本）**什么都不会发生**，因为第 1 道闸让输入是空的。
这条顺序必须在动手时写在最前面。

★ 顺带一条本文新核实的事实，`16` 里没有 ★
**内核的链接脚本也丢 `.eh_frame`**：

```
kernel/linker.ld:68-73
    /DISCARD/ : {
        *(.eh_frame*)
        …
```

这不是问题（内核不用异常，而且 `KERNEL_CFLAGS:56-57` 同样关掉了 unwind tables），
但读 `16` §2.5 的人可能以为"只有用户在丢"。**两块链接脚本在这个点上是一致的、
都是有意的**，写在这里免得下一个人误以为内核里还留着 `.eh_frame` 可以借。

★ 修它属于哪一层：**链接脚本 + 构建参数**（不是内核）★
`user/linker.ld` 要**同时**做两件容易只做一件的事：
① 把 `*(.eh_frame*)` 从 `/DISCARD/` 里拿掉；
② 给它一个**输出段**（今天脚本里只有 `.text`/`.rodata`/`.data`/`.bss` 四个输出段，
`user/linker.ld:10-27`）。★ `.eh_frame` 不放进某个输出段、只是"不在 `/DISCARD/` 里"，
它会被 `ld.lld` 按默认规则安置——**那正是"看起来改对了、实际位置未定义"** ★。
配套还要加 `.eh_frame_hdr`（libunwind 的二分查找表，可选但强烈建议）
和 `__eh_frame_start`/`__eh_frame_end` 一类的边界符号（如果要自己实现 `_Unwind_Find_FDE`）。

### 4.3 链接脚本里没有 `.init_array`（核实：**真**）

`16` §2.5 说"链接器脚本里**没有** `.init_array` / `.ctors` / `.fini_array` 段定义"。
本文核实结果：**真**，而且比 `16` 写的更强：

- `user/linker.ld` 全文 35 行，只有 `.text`（:10-12）、`.rodata`（:15-17）、
  `.data`（:20-22）、`.bss`（:24-27）四个输出段 + `/DISCARD/`；
- ★ **全项目搜 `.init_array` / `.ctors` / `__init_array` → 0 处命中**，
  不只是"用户态里没有"，是**整棵树都没有** ★
  （包括 `kernel/`：内核自己也没有构造函数的调用链）。

**它怎么会让"libc++ 编出来了但跑不对"**：
C++ 的全局对象构造不是"编译器随手插一段"——它被编译器放进
**`.init_array` 节**（老式是 `.ctors`），由**启动代码**在 `main` 之前遍历调用。
没有这个节、或者没人遍历它，后果是：

- **静态注册全失效**（`16` §2.5 的原话）；
- libc++ 自己的静态对象（`std::cout` 的 `ios_base::Init`、异常类的 `type_info`
  注册、locale 的 `classic_locale`）**全部不初始化**;
- 症状极具误导性：`std::cout` 能编能链，**第一次用它就崩或什么都不打印**，
  而"什么都没打印"看起来像缓冲问题。

★ 修它属于哪一层：**链接脚本 + 启动代码（`user/libfe/start.asm`）**★
两处必须成对改，缺一个都不生效：

```
user/libfe/start.asm:29-45（今天全文只有两个 call）
_start:
    xor rbp, rbp
    mov r12, rsp
    mov rdi, [r12]              ; argc
    lea rsi, [r12 + 8]          ; argv
    lea rdx, [rsi + rdi*8 + 8]  ; envp
    and rsp, -16
    mov rdi, rdx
    call __posix_set_envp       ; ← call #1
    mov rdi, [r12]              ; argc
    lea rsi, [r12 + 8]          ; argv
    call main                   ; ← call #2
    mov edi, eax
    call fe_exit
```

要加的是三样（顺序有讲究）：
1. `main` **之前**：遍历 `.init_array`（`[__init_array_start, __init_array_end)`）调用；
2. `main` **之后**、`fe_exit` **之前**：遍历 `.fini_array`；
3. `__cxa_atexit` 与 `atexit` **共用同一个注册表**（§2.3），
   并且那个表的容量要从 16 提上去——否则第 17 个静态析构静默丢失。

★ 一个必须提前写死的判据 ★
`start.asm` 今天是**纯汇编、不链接 libposix 之外的东西**（它 `extern main` /
`fe_exit` / `__posix_set_envp`，`start.asm:25-27`）。加了 `.init_array` 循环之后，
**它仍然不应该调用 libc++abi 的任何符号**——那会把 `libfe` 变成"C++ 依赖"，
而 `libfe` 的定位是"syscall ABI 的投影，不含 POSIX 语义"（`10-posix-layer.md` §5）。
正确的做法是：**`.init_array` 的遍历只做"取地址 + 间接调用"**，
`__cxa_atexit` 的实现留在 libposix 里。

### 4.4 三条修法的层次汇总（一句话各一条）

| 修什么 | 层次 | 一句话 |
|---|---|---|
| `FE_VMA_MAX = 32` | **内核**（K6 的实现常量） | 调大（空间换）或改动态表（引入分配失败路径）——**取舍要写代价，不能顺手改大** |
| `.eh_frame` | **构建参数 + 用户链接脚本** | 先去掉 `build.py:102-103` 的两行 `-fno-*-unwind-tables`，再改 `linker.ld` 的 `/DISCARD/`，**并给它一个输出段** |
| `.init_array` | **用户链接脚本 + `start.asm`** | 两侧成对改；同时把 `ATEXIT_MAX 16` 提上去，否则静态析构第 17 个就丢 |

## 5. S1 的可证伪验收

### 5.1 正向：一个 C++ 程序跑出**逐字节**正确的输出

★ **本节已被 §7.1 修订过一次**：`std::thread` **移出 S1**。
理由不是"太难"，而是 §7.1 核实的那两条硬事实——
`_LIBCPP_HAS_THREADS` 是全有或全无，而关掉线程就必然关掉单调时钟；
打开线程则要求 `__libcpp_condvar_wait`，其底座（"等一个用户地址"）今天不存在。★

**程序要碰的四样东西（S1 口径）**：

| # | 碰什么 | 具体要出什么 |
|---|---|---|
| 1 | `std::string` | 拼接、`substr`、`size()`、`c_str()`；长度要**超过 SSO 阈值**（64 位 libc++ 上短串内联，不超过就测不到堆） |
| 2 | `std::vector` | 扩容（迫使 `malloc`/`realloc` 真的走 arena）、`push_back`、迭代、`size()` |
| 3 | `throw` / `catch` | 抛一个自定义类型（带析构，验"栈上对象的析构真的跑了"）、`catch (const T&)` 收到正确内容 |
| 4 | **全局对象的构造函数** | 一个全局对象在构造时往一张表里写一个标记；`main` 里读它 |

**`std::thread` 的去处（不许含糊）**：它属于 `docs/16` §4 的 **S3**，
和"等一个用户地址"的内核原语一起验收。S1 通过之后、S3 之前，
**任何"多线程 C++ 已经能跑"的说法都是没有证据的**。

★ 但 S1 仍然要**为 S3 探一条路**（今天就能做，不需要线程跑起来）★
把 §4.1 的 `FE_VMA_MAX` 判据改成**静态的**：数一数 S1 程序链接进去之后
它的地址空间**理论上**有多少段（ELF 段 + 主栈 + 每个 arena chunk + 每个线程栈），
写成一张表。★ 判据是"表里的段数 vs 32"，而不是"跑起来有没有撞 `NOSPC`" ★——
后者要等 S3。

**判据（不含形容词）**：
- 程序在 FEKernel 上输出一段**固定的期望文本**，与宿主机上同一个源码
  **逐字节相符**；
- 退出码 **0**；
- `init` 核对退出码与输出（照 `docs/10-posix-layer.md` §6 已经建立的模式）。

**额外判据（借 `16` §4 的 S1 那一条，本文同意）**：
走 `FE_SYS_PF_STAT`（`user/include/fe_user.h:92`，`fe_pf_resolved_count()` 在 `:264`）：
**C++ 程序的缺页解析次数必须 > 0**。理由与 `11-kernel-next.md` §6 相同：
"从用户态看不崩什么都证明不了"。C++ 映像比 C 大得多，只有 `>0`
才说明地址空间真的是按需建立的。

### 5.2 ★ 反向对照至少四条（每条都必须是"会失败"的那种）★

`11-kernel-next.md` §8 的纪律：**"自检里绝不能用'本该阻塞'的接口去验证它没立刻返回"**。
所以下面每一条都要写清"它证明的是哪一件事"。

| # | 反向对照 | 期望症状 | 它证明什么 |
|---|---|---|---|
| **R1** | **全局构造函数没被调用时那个标记读不到**。做法：把链接脚本的 `.init_array` 段**故意删掉**（或让 `start.asm` 不遍历），重跑同一个程序 | `main` 打印的标记是**空/0**，与正向输出**不一致** | 证明"标记读到了"这件事**确实来自构造函数**，不是"恰好有一个静态初始化的常量" |
| **R2** | **异常没被展开时 `catch` 分支不执行而是直接终止**。做法：把 `*(.eh_frame*)` **放回 `/DISCARD/`**，重跑 | `catch` 里的那行**不打印**，进程以 `std::terminate` 路径结束（今天落点是 `abort` → 退出码 **134**，`stdio.c:774`） | 证明 `throw`/`catch` 走的**真的是展开**，不是"编译器把 `catch` 优化成了 `if`" |
| **R3** | **正向那段输出不是硬编码的**。做法：让 S1 的 `std::string`/`std::vector` 部分**只依赖输入参数**（例如长度从 `argv` 取），换一个参数重跑 | 输出**必须变**，且与宿主机上同参数的结果仍然逐字节相符 | 证明比的是**真的算出来的结果**，不是"打印了一份我们写死的期望值"（这是 §5.1"逐字节相符"这条判据唯一能被伪造的方式） |
| **R4** | **`throw` 抛的对象是真的构造过的**。做法：让被抛对象的构造函数与析构函数各写一个标记，比较"构造次数"与"析构次数" | 正向：构造 == 析构 == 抛出次数；改坏后**析构数变少** | 证明"栈上对象的析构"这件事也走展开（R2 只验了 `catch` 分支，验不到 cleanup） |

★ 为什么 R1 和 R2 必须**物理上**把配置改坏，而不是"在代码里加个 if 分支" ★
在代码里加分支测的是**我们写的那条路**，不是**链接脚本/构建参数那条路**。
`16` §2.5 说得很准：这两条"看起来编过了、跑起来莫名其妙"的症状，
根源在**构建配置**里，而构建配置用运行期的 if 是测不到的。

★ **S1 不包含线程，所以"用并发证伪"的那条反向对照（原 R3）移到 S3** ★
它对应的断言是"4 个线程算出来的总数精确等于期望"——
那条断言在 S1 里**不存在**（§7.1 把 `std::thread` 移出了 S1），
所以它的反向对照也一并不该留在 S1 里。★ 一条没有对应正向断言的反向对照，
比没有反向对照更糟：它会让人以为"多线程验过了"。★

### 5.3 每条断言失败时最先看哪里

| 症状 | 最先看哪里 | 为什么 |
|---|---|---|
| `undefined symbol: __udivti3`（或 `__udivdi3`/`__floatundidf`/`__muldf3` 一族） | **§1.2 / `16` §2.1**：compiler-rt builtins 没链上 | 这是**链接期**报错，离"那一行 128 位除法"很远。★ 内核已经实测踩过一次（`kernel/time/time.c:272-287`），所以这条的排查路径是现成的 ★ |
| `undefined symbol: __cxa_*` / `_Unwind_*` | **§1.1 的构件表**：libc++abi / libunwind 没链上，或者链接顺序不对 | 三个 `.a`/一堆 `.o` 的顺序在静态链接下是有意义的 |
| `undefined symbol: __libcpp_*`（`__libcpp_mutex_lock` / `__libcpp_condvar_wait` / `__libcpp_thread_create` …） | **§7.1**：这是一个**信号**——说明 `_LIBCPP_HAS_THREADS` 是开的，而你既没给 pthread API 也没给 `<__external_threading>` | 这一族的符号名是 libc++ 内部线程接口，不是 POSIX。★ 看到它们就说明"线程被打开了"，与 §7.1 的推荐配置不符 ★ |
| `fatal error: '__config_site' file not found` 或 `_LIBCPP_HARDENING_MODE_DEFAULT is not defined` | **§7.2 / §3.7.1**：CMake 那一步没跑（或跑失败了），生成的头没在 include 路径里 | ★ 这是**编译第一步**就失败，与"某个源文件编不过"完全不同。它说明问题在配置阶段，不在代码 ★ |
| **全局对象的构造没跑**（标记读不到） | **§4.3**：① `user/linker.ld` 里没有 `.init_array` 输出段；② `user/libfe/start.asm` 只有两个 `call`（`:38` 与 `:42`），没有 `_init` 循环；③ `ATEXIT_MAX 16` 溢出（`stdlib.c:17`） | 三处**都可能**，按顺序查：链接脚本 → 启动代码 → 注册表容量 |
| **异常一抛就崩 / `catch` 不执行** | **§1.3 + §4.2**：先看 `tools/build.py:102-103` 的两行是否还在（编译器压根没生成 `.eh_frame`），再看 `user/linker.ld:30` | ★ 顺序不能反 ★ 先改链接脚本什么都不会发生 |
| `std::string`/`std::vector` 能编、一跑就崩 | **§4.1**：`FE_VMA_MAX = 32` 表满（`FE_ERR_NOSPC`）；`kernel/mm/vma.c` | 表满是"跑到一半报错"，与"启动就失败"完全不同 |
| `std::chrono::steady_clock` 行为不对 / `std::this_thread::sleep_for` 编不过 | **§7.1**：`_LIBCPP_HAS_MONOTONIC_CLOCK` 是**被线程开关绑住**的，关线程就必然关它 | 这两条是"关线程"的连带代价，不是独立故障 |
| 卡死而不是报错 | **§2.6 / §7.1**：`std::mutex`/`std::condition_variable` 没有"等一个用户地址"的原语，自旋在 `-smp 1` 下锁死；★ 这也是 S1 必须关线程的理由 ★ | `16` §2.3/§4 S3 的同一条 |
| 输出**少了最后一行** / 一个字符都没有 | **§2.3**：`exit` 契约（`stdio.c:748-763`）与 `fe_flush`（`libfe.c:85-93`）；或者全局析构没跑（§4.3） | `stdio.c:748-754` 记着"本地测试正常、重定向到文件就少了最后一行"这个坑 |
| 第一次用 `std::cout` 就崩 | **§4.3**：`ios_base::Init` 是全局构造，没跑 | 这条与"构造没跑"同源，但症状位置完全不同 |
| 读 `stdin` 得到 `-1`/`ENOSYS` | **§2.8**：`console_read` 明确 `ENOSYS`（`posix.c:285-290`） | 这是**设计**，不是 bug。测试程序不许读 `stdin` |

### 5.4 分段验收（不要把四件事一次全塞进一个程序）

★ 一次全上会让"失败时最先看哪里"失效：一个 `abort` 到底是构造没跑、
展开没通、还是 VMA 满了，从输出上看不出来。★ 建议的顺序：

| 步 | 程序内容 | 它单独证伪什么 |
|---|---|---|
| **A** | `std::string` + `std::vector` + 全局构造函数标记 | §4.3（`.init_array`）与 §2.2（堆） |
| **B** | A + `throw`/`catch` + 带析构的被抛对象 | §1.3/§4.2（`.eh_frame`）与 libunwind |
| **C（= `16` §4 的 S3，不属于 S1）** | B + 4 个 `std::thread` + `std::mutex` + `std::condition_variable` | §2.6（等一个用户地址）与 §4.1（`FE_VMA_MAX`）。★ 它的前置是 §7.1 里的 T2 或 T3 **任一先落地** ★ |

★ 附一条"今天就该做的准备"，不需要跑构建 ★
把 A 步那个程序**先在宿主机上编一遍**（`toolchain/llvm/bin/clang++.exe` 配
`--target=x86_64-unknown-none-elf` 与 §3.7 装出来的 CMake 生成的头目录，
只到 `-c` 为止），看它**需要哪些头**。
这一步不改任何文件、不跑虚拟机，却能把"libc++ 到底缺哪些 C 头"
从"我列的清单"变成"编译器的报错列表"——
这正是 `10-posix-layer.md` §8 用 `posixprobe` 而不是"一张我列的表"的同一条理由。

**S1 的完整档案（一句话）**：`-DLIBCXX_ENABLE_THREADS=OFF` +
`-DLIBCXX_ENABLE_MONOTONIC_CLOCK=OFF`（连带）+ CMake 生成的 `__config_site` 在
include 路径里 + `.eh_frame` 两道闸都开 + `.init_array` 三处都改。
★ 这五样缺任何一样，S1 都会以**一个与它自己无关的症状**失败。★

## 6. 待查清单

★ 每一条都写"怎么查" ★ 查不到的**不猜**。本文里凡是没实测的数字，一律没有出现。

| # | 待查 | 怎么查 | 为什么必须查 |
|---|---|---|---|
| 1 | ~~**CMake 从哪来**~~ → ★ **本轮已决（§3.7）** ★ 候选 (a)/(b)/(c) 全部评估完；**推荐 (a) 为终局、(b) 为今天**。★ 但仍有**两条没核实** ★：① **CMake 官方 Windows zip 是否自带 Ninja**；② **pip 23.0.1 + Python 3.10 是否在 PyPI `cmake` 包的支持窗口内**；③ CMake 的许可证文本（**BSD-3-Clause** 是本文**没有核实**的记忆值，等真下载时读它的 `Copyright.txt` 确认） | ① 下 CMake 的 zip 之后列目录看有没有 `ninja.exe`；② `pip index versions cmake` 或直接 `pip download cmake --no-deps -d <tmp>` 看有没有匹配的 wheel（**别在没决定前真装**）；③ 读下载包里的许可证文件 | (a) 的清单里 `expect_bytes` 要填，且"Ninja 从哪来"与 CMake 是**两个**决定（§3.7.3 结尾） |
| 2 | **libc++ 的版本与 commit** | 决定用哪个 LLVM 版本（今天的工具链是 `clang+llvm-23.1.2-x86_64-pc-windows-msvc`，见 `toolchain/downloads/` 的文件名与 `tools/fetch_toolchain.py:39`）。**同版本优先**：混合版本的 C++ ABI 有隐性假设 | 版本决定 `LIBCXX_ENABLE_*` 的**可用集合**。19 与 24 的选项表已经不同（本文引用的两页文档就不同）。★ 本文已按 **release/23.x 分支**核实过选项名（§7.1 的来源），所以"用 23.x"这条能对上工具链 ★ |
| 3 | ~~`LIBCXX_ENABLE_THREADS` **是否存在**~~ → ★ **本轮已回答：是官方选项，且不是唯一的那一个** ★ 见 §7.1 与下面的"已回答"小节 | （已答）来源：上游 `libcxx/CMakeLists.txt`（release/23.x）的 `option(LIBCXX_ENABLE_THREADS "Build libc++ with support for threads." ON)` | 结论直接改写了 S1 的口径（§5.1/§5.4） |
| 4 | **`aligned_alloc`/`posix_memalign` 缺失时 libc++ 怎么降级** | 在 libc++ 源码里搜 `aligned_alloc`、`posix_memalign`、`__libcpp_aligned_alloc`（`libcxx/src/`）；看有没有"都没有就用 `malloc` + 手工对齐"的分支。★ 本轮已找到入口文件名 `libcxx/include/__memory/aligned_alloc.h`（出现在 `libcxx/include/CMakeLists.txt` 的 `set(files …)` 里），**但文件内容本文没有读**，所以仍列待查 ★ | 决定 §2.2 是"必须补"还是"可以推后"。**不要把"应该会降级"当成事实** |
| 5 | **libc++ 对 locale / `setlocale` 是不是弱符号** | `grep -rn "setlocale\|localeconv\|__libcpp_locale_guard" libcxx/src/ libcxx/include/` | §2.10 写了"应当是弱符号"——那是文档说法，不是实测。判据：**没有一个**非弱引用，否则链接期就会炸 |
| 6 | **`LIBCXX_ENABLE_FILESYSTEM` 该开还是关** | 先查 `qtbase` 在 `-static` 下会不会走 `QFileSystemEngine` 的 `<filesystem>` 路径；再看 libc++ 关掉它之后哪些头会消失（`<filesystem>`、`std::filesystem::*`） | 关掉可能让 `qtbase` 的某个翻译单元编不过；开着则要求 `mkdir`/`rename` 可用（今天 `ENOSYS`，`posix.c:770/791`）。**这是"能编出来"与"跑得对"的边界** |
| 7 | **`wchar_t` 相关**：`user/include/posix/` 里到底有没有 `wchar.h`/`wctype.h` | 直接列 `user/include/posix/`。本文实测：**12 个文件**（`dirent.h`、`errno.h`、`fcntl.h`、`spawn.h`、`stddef.h`、`stdio.h`、`stdlib.h`、`string.h`、`unistd.h`、`sys/stat.h`、`sys/types.h`、`sys/wait.h`）——**没有 `wchar.h`**。★ 原第 7 条写的是"本文与 `16` §2.2.1 的说法不一致：**那一节**写'已有的头（15 个……）'"——★ **节号指错了**：那句"15 个"的原文在 `16` §2.2（**已在 2026-09-26 修正为 12 个**），而 §2.2.1 从头到尾没写过 15（它写的是"25 行 / 5 行判'有'"那套按**行数**的口径）。★ 只错在"去哪儿找那句话"，**结论不变：12 个文件**。它列出的名字我都能对上，它说"缺的头是 13 个"我也能对上（25 行里 5 行判"有" = 6 个 clang 自带 + 11 个我方已有）——★ 但 **15 / 12 / 13 是三把不同的尺子**（**文件数** / **表格行数** / **摊开的头名数**），**不能互相加减** ★ | 决定 `LIBCXX_ENABLE_WIDE_CHARACTERS` 能不能关（§3.4） |
| 8 | **`.a` 还是摊平的 `.o`**（§3.6） | 在宿主机上做一次最小实验：把 2 个 `.o` 打进 `.a`，用今天的 `ld.lld -T user/linker.ld --gc-sections -nostdlib` 链一个程序，看 `--gc-sections` 会不会把 `main` 用到的符号所在的目标文件丢掉 | `tools/build.py:322-333` 已经栽过这个坑一次（症状 `undefined symbol: malloc`）。**libc++ 的规模会放大它** |
| 9 | **libc++ 的 `install/` 目录怎么被目标侧 `clang` 找到** | 查 `clang` 的 `-nostdinc++` / `-stdlib=libc++` 在 `x86_64-unknown-none-elf` 上的默认搜索路径（`clang -print-search-dirs --target=…`）；以及是否需要 `CMAKE_INSTALL_PREFIX` + `-isystem` | 官方文档的用法是 `clang++ -nostdinc++ -isystem <install>/include/c++/v1 -nostdlib++ -L <install>/lib -lc++`（<https://libcxx.llvm.org/VendorDocumentation.html>）。★ 今天 `tools/build.py` 的 `INCLUDES` 里**没有任何 C++ 头路径**（`build.py:117-126`）★ |
| 10 | **`mprotect` 的缺席在 libc++ 上到底有没有症状** | 待实测：先编出库，再看链接期与运行期有没有 `mprotect` 引用。今天**不许猜** | §2.9 写了"暂时不产生新症状"——那是**推理**，不是实测 |
| 11 | **`__cxa_thread_atexit` / `__cxa_thread_atexit_impl`** | 查 libc++abi 的 `src/cxa_thread_atexit.cpp`：确认在宿主**没有** `__cxa_thread_atexit_impl` 时是否有 weak fallback，以及 fallback 的语义（是不是直接 `abort`） | `thread_local` 对象的析构走它。相关上游讨论：libcxxabi 的 ["Provide a fallback `__cxa_thread_atexit()` implementation"](https://reviews.llvm.org/D21803)、源码 <https://llvm.googlesource.com/libcxxabi.git/+/2933bf8e793e69d882ad423a6de5af101620763f/src/cxa_thread_atexit.cpp> |
| 12 | ~~`_LIBCPP_HAS_NO_MONOTONIC_CLOCK` 之类的 ABI 宏该不该由我们定义~~ → ★ **本轮已回答：不该走 `LIBCXX_ABI_DEFINES`** ★ 见下面的"已回答"小节 | （已答）`_LIBCPP_HAS_MONOTONIC_CLOCK` 是 **CMake 生成的 `__config_site` 宏**，由 `LIBCXX_ENABLE_MONOTONIC_CLOCK` 控制；而 `LIBCXX_ABI_DEFINES` **只接受 `_LIBCPP_ABI_` 前缀**的宏（上游会 `message(SEND_ERROR "Invalid ABI macro …")`） | 用错口子会在**配置阶段**报错（比手改头文件好），但更重要的是它说明"没有 RTC / 没有线程"这件事**有正规口子** |
| 13 | **`libunwind` 的构建选项全表** | 官方文档 `libunwind/docs/BuildingLibunwind.rst`（本文尝试抓取时源站不可达，只拿到搜索结果里的镜像链接：<https://chromium.googlesource.com/external/github.com/llvm/llvm-project/libunwind/+/baf07ac16651a19395a05cd686355efa3963f67a/docs/BuildingLibunwind.rst>）。★ **本文没有核实它的选项名**，不要照抄本文的猜测 ★ | 至少要知道 `LIBUNWIND_ENABLE_SHARED/STATIC`、以及"没有 `dl_iterate_phdr` 时 `_Unwind_Find_FDE` 走哪条路" |
| 14 | **`compiler-rt` builtins 的目标侧构建形态** | 官方有 `compiler-rt/lib/builtins/CMakeLists.txt`；查 `COMPILER_RT_BAREMETAL_BUILD`、`COMPILER_RT_DEFAULT_TARGET_ONLY`、`COMPILER_RT_BUILD_BUILTINS` 这几个变量的语义（★ 本文只从搜索结果里见到这些名字，**没有核实语义** ★）。另有一条可参考的官方文档：[HowToCrossCompileBuiltinsOnArm](https://llvm.org.cn/docs/HowToCrossCompileBuiltinsOnArm.html)（架构不同，但流程与陷阱同族） | builtins 是**本文里唯一有实测证据的缺口**（§1.2），它的构建形态却最不清楚 |
| 15 | **`qtbase` 会不会真的需要 `libm` 才能编 `moc`** | 先只编 `moc` 的翻译单元，看链接期缺什么符号。★ 不许用"Qt 肯定要 libm"当结论 ★ | 决定 S5（`libm`）在 S9（`moc`）之前还是之后 |

### 6.1 ★ 本轮用静态阅读回答掉的（原第 3、12 条，外加第 1、7 条的进展）★

★ 这一小节的每条都给**上游文件的逐字依据**，不是"我记得" ★

**(A) `LIBCXX_ENABLE_THREADS` 是官方 CMake 选项名——原第 3 条，已答**

逐字来源：上游 `libcxx/CMakeLists.txt`（`release/23.x` 分支，
<https://raw.githubusercontent.com/llvm/llvm-project/release/23.x/libcxx/CMakeLists.txt>）：

```cmake
option(LIBCXX_ENABLE_THREADS "Build libc++ with support for threads." ON)
option(LIBCXX_ENABLE_MONOTONIC_CLOCK
  "Build libc++ with support for a monotonic clock.
  This option may only be set to OFF when LIBCXX_ENABLE_THREADS=OFF." ON)
...
option(LIBCXX_HAS_EXTERNAL_THREAD_API
  "Build libc++ with an externalized threading API.
   This option may only be set to ON when LIBCXX_ENABLE_THREADS=ON." OFF)
```

★ 为什么官方 Vendor 文档的选项表里找不到它 ★
那份表列的是"**厂商经常要调的**"子集，不是全表。`LIBCXX_ENABLE_THREADS`
在源码里是正经的 `option(...)`，而且**同一个文件里有跨选项校验**：

```cmake
if(LIBCXX_ENABLE_THREADS AND NOT LIBCXX_ENABLE_MONOTONIC_CLOCK)
  message(FATAL_ERROR "LIBCXX_ENABLE_MONOTONIC_CLOCK can only be set to OFF"
                      " when LIBCXX_ENABLE_THREADS is also set to OFF.")
endif()
```

★ 教训（值得单独记）★ **"官方文档里没有" ≠ "不存在"。**
原第 3 条就是因为只查了文档表才变成待查项——
正确做法是查**源码里的 `option(...)` 与 `cmake_dependent_option(...)`**。
这条与 `10-posix-layer.md` §9 那条"头文件描述的是意图，链接器只在有人调用时才报错"
是同一族：**文档描述的是意图，源码才是事实。**

它的全部后果已写进 §7.1 与 §5.1（`std::thread` 移出 S1）。

**(B) `_LIBCPP_HAS_MONOTONIC_CLOCK` 不该由 `LIBCXX_ABI_DEFINES` 定义——原第 12 条，已答**

三个逐字依据：

1. `LIBCXX_ABI_DEFINES` 的语义与**校验**（上游 `libcxx/CMakeLists.txt`）：
   ```cmake
   set(LIBCXX_ABI_DEFINES "" CACHE STRING "A semicolon separated list of ABI macros to define in the site config header.")
   ...
   foreach (abi_define ${LIBCXX_ABI_DEFINES})
     if (NOT abi_define MATCHES "^_LIBCPP_ABI_")
       message(SEND_ERROR "Invalid ABI macro ${abi_define} in LIBCXX_ABI_DEFINES")
     endif()
   ```
   ★ **它只接受 `_LIBCPP_ABI_` 前缀**，而 `_LIBCPP_HAS_MONOTONIC_CLOCK` 不匹配 ★
2. 正确的口子是 `config_define(${LIBCXX_ENABLE_MONOTONIC_CLOCK} _LIBCPP_HAS_MONOTONIC_CLOCK)`
   （同一文件，紧挨着 `config_define(${LIBCXX_ENABLE_THREADS} _LIBCPP_HAS_THREADS)`）。
3. 结果落在 `__config_site` 的 `#cmakedefine01 _LIBCPP_HAS_MONOTONIC_CLOCK`
   （`libcxx/include/__config_site.in`，逐字见 §7.2）。

**(C) 第 1 条（CMake）已从"待查"升级为"有推荐 + 三条残余待查"** → 见 §3.7 与第 1 条本身。

**(D) 第 7 条（`wchar_t` / POSIX 头计数）已被 `docs/16` 采纳** ★
★ 那一句"15 个"的原文在 `16` **§2.2**（不在 §2.2.1——§2.2.1 从头到尾没写过 15，
它写的是"25 行 / 5 行判'有'"那套**按行数**的口径）。本文早先的草稿把节号写成了
`§2.2.1`，**这里更正为 §2.2**；结论一个字都没变。★
`docs/16` 已改为 **12 个**（2026-09-26 修正），并在它的 §2.2 与 §6.5 表尾记录了三把**不同的尺子**
（**文件数** / **表格行数** / **摊开的头名数**，不能互相加减）。
本文结论（**12 个文件、没有 `wchar.h`**）不变。
它同时声明"两份文档口径不一致时**以 `17` 为准**"。
★ 这一条可以从待查里划掉了 ★（`user/include/posix/` 12 个头，没有 `wchar.h`，
所以 `LIBCXX_ENABLE_WIDE_CHARACTERS` **可以**关——但**要不要关**是另一个决定，
它取决于 `qtbase` 用不用宽字符，那属于 `16` §6.3 的裁剪口径。）

**(E) 仍然答不了的（保留待查，且不许猜）**：
第 5 条（locale 是否弱符号）、第 6 条（`ENABLE_FILESYSTEM` 开还是关）、
第 8 条（`.a` 还是摊平）、第 9 条（目标侧 `clang` 怎么找到 `install/`）、
第 10 条（`mprotect` 缺席的实际症状）、第 11 条（`__cxa_thread_atexit` 的 fallback 语义）、
第 13 条（libunwind 选项全表）、第 14 条（compiler-rt builtins 构建形态）、
第 15 条（`moc` 要不要 `libm`），以及第 1/4 条的各半。
★ 这些每一条都需要**打开对应的上游文件读**，而不是查文档表——
(A) 的教训已经说明了为什么。★

### 6.2 待查条数（本轮之后）

| | 本轮之前 | 本轮之后 |
|---|---|---|
| 待查项总数 | **15** | **15**（编号不变，便于对照） |
| 其中**已完全答掉** | — | **2**（第 3、12 条） |
| 其中**部分答掉 / 已有推荐** | — | **2**（第 1 条：有推荐 + 3 条残余；第 7 条：已被 `docs/16` 采纳） |
| 其中**完全未动** | — | **11**（第 2、4、5、6、8、9、10、11、13、14、15 条） |

★ 为什么保留编号而不重排 ★ 这份文档已经与 `docs/16` 互相引用
（`16` §2 声明了"以 `17` 为准"并点了本文的三处更正）。
**重排编号会让那些引用全部指错**——这与 `11-kernel-next.md` 里
"编号归那份文档"的纪律是同一条：编号是**跨文档的标识符**，不是行号。

## 7. 新增判据（本轮补的：线程配置与两个 `__config_site` 宏）

★ 这一节是 §3.7 与 §6 待查清单的产物：**两条静态阅读就能定死的事实**。
它们各自都改变了 S1 的做法，所以单独成节，不埋在待查表里。★

### 7.1 ★ `_LIBCPP_HAS_THREADS` 是全有或全无，而 S1 的 `std::thread` 在今天的 FEKernel 上**跑不了** ★

来源：上游 `libcxx/include/__thread/support.h`（逐字）：

```cpp
#if _LIBCPP_HAS_THREADS

#  if _LIBCPP_HAS_THREAD_API_EXTERNAL
#    include <__thread/support/external.h>
#  elif _LIBCPP_HAS_THREAD_API_PTHREAD
#    include <__thread/support/pthread.h>
#  elif _LIBCPP_HAS_THREAD_API_C11
#    include <__thread/support/c11.h>
#  elif _LIBCPP_HAS_THREAD_API_WIN32
#    include <__thread/support/windows.h>
#  else
#    error "No threading API was selected"
#  endif

#endif // _LIBCPP_HAS_THREADS
```

**三条结论，逐条都能证伪：**

1. **★★ 没有"只要 `std::thread`、不要 `std::mutex`"这个选项 ★★**
   打开 `_LIBCPP_HAS_THREADS` 就是**把整套线程 API 一次全打开**：
   mutex / recursive_mutex / condvar / execute_once / thread_id / thread / TLS。
   `support.h` 的注释块把这份**必须由实现提供的清单**逐条列了出来
   （同一文件，`/* … */` 里：`__libcpp_mutex_lock`、`__libcpp_condvar_wait`、
   `__libcpp_execute_once`、`__libcpp_thread_create`、`__libcpp_thread_join`、
   `__libcpp_tls_create` …）。**粒度不是按类分的。**
2. **★★ 不开线程，`_LIBCPP_HAS_MONOTONIC_CLOCK` 也必须关 ★★**
   `__config` 里有这条：
   `#if !_LIBCPP_HAS_MONOTONIC_CLOCK && _LIBCPP_HAS_THREADS → #error "_LIBCPP_HAS_MONOTONIC_CLOCK may only be false when _LIBCPP_HAS_THREADS is false."`
   （即两者**绑定**）。而 CMake 侧也强制：`LIBCXX_ENABLE_MONOTONIC_CLOCK` 只有在
   `LIBCXX_ENABLE_THREADS=OFF` 时才允许 OFF，否则 `FATAL_ERROR`。
   ★ 后果：`_LIBCPP_HAS_THREADS=0` **连带**让 `std::chrono` 丢掉单调时钟，
   而我们的 `fe_clock_ns()` 恰恰**只有**单调时钟（§2.7）。两条约束**同向**开火。 ★
3. **★★ 而"打开线程"在今天的 FEKernel 上是跑不了的 ★★**
   §2.6 已经核实：**"等一个用户地址"的等待原语内核里 0 处**。
   打开线程 = 必须提供 `__libcpp_condvar_wait`，它的语义就是"在用户内存的条件上睡下、
   被唤醒后重新检查"——今天**做不到**。
   ★ 更糟的是 §2.4/§2.6 那条：QEMU 默认 `-smp 1`，用自旋顶着写
   会**锁死而不是变慢**。★ 所以"先随便写个自旋版"不是一条能走的路。

**★ 于是 S1 必须改口径（这是本节最重要的产出）★** 三条候选，**代价都必须写在明处**：

| 候选 | 做法 | 代价 / 它测不到什么 |
|---|---|---|
| **T1（推荐给 S1）** | `-DLIBCXX_ENABLE_THREADS=OFF`（连带 `LIBCXX_ENABLE_MONOTONIC_CLOCK=OFF`） | S1 的 C++ 程序**不能有 `std::thread` / `std::mutex` / `std::condition_variable`**。原本写在 §5.1 表格里的"`std::thread`：≥ 4 线程 + mutex + 计数"这一项要**移出 S1**，线程归 S3（与 `docs/16` §4 的 S3 分工一致）。★ 好处：S1 的"唯一真未知"回到正确的位置——**运行库配不配得起来**，而不是**内核有没有 futex** ★ |
| **T2** | 打开线程 + 实现"等一个用户地址"的内核原语（§2.6 候选 A） | 这是**内核工作**，不是 S1 的构建工作；而且它是 `docs/16` §4 的 **S3 的硬前置**。把它塞进 S1 = 让 S1 同时依赖内核改动，违背"先证伪最便宜的未知" |
| **T3** | 打开线程 + `LIBCXX_HAS_EXTERNAL_THREAD_API=ON`，我们提供 `<__external_threading>` | ★ **这一条是官方为"自研线程"准备的口子，不是权宜之计** ★ 但今天它照样卡在同一处：那个头**必须实现 `__libcpp_condvar_*`**，而底座不存在。**它改变的是"谁来写"，不是"能不能跑"** |

**T3 的官方依据**（libc++ 的 `Threading Support API` 设计文档，逐字）：

> When `_LIBCPP_HAS_THREAD_API_EXTERNAL` is defined the `<__thread/support.h>`
> header simply forwards to the `<__external_threading>` header
> (**which must exist**). It is expected that the `<__external_threading>` header
> provide the exact interface normally provided by `<__thread/support.h>`.

来源：<https://raw.githubusercontent.com/llvm/llvm-project/7162fd750ee5f786f3b9b7a7b26b72ee36ce772e/libcxx/docs/DesignDocs/ThreadingSupportAPI.rst>
（同一页也写明 `_LIBCPP_HAS_NO_THREADS` "should not be manually defined by the user"。）

★ T3 与 `docs/16` §2.3 那段的关系 ★
`16` §2.3 写的候选是"把 `sizeof(std::mutex)` 换成一个**内核通知对象句柄**的封装，
libc++ 允许 `_LIBCPP_ABI_*` 之类的开关"。**本文核实到的口子比那段更准确**：
不是 `_LIBCPP_ABI_*`，而是 **`_LIBCPP_HAS_THREAD_API_EXTERNAL` +
一个我们自己写的 `<__external_threading>`**。★ 后者是官方文档明写的机制，
而"改 `_LIBCPP_ABI_*`"会撞回官方 User 文档的警告
（"Configuration macros that are not documented here are not intended to be
customized by developers"）。★ 这一条应当回填进 `16` §2.3。

★ 顺带一条 ★ **"c11 线程 API"不是出路**：`__thread/support/c11.h` 要求 C11 threads
（`thrd_create`/`mtx_lock`/`cnd_wait`），宿主一个都没有（§2.4）；
`windows` 那条更不相关；`pthread` 那条要求 §2.4 里今天不存在的 14 个函数。

### 7.2 ★ CMake 至少要**四个**生成文件，其中**两个**是硬依赖 ★

来源：上游 `libcxx/include/CMakeLists.txt`（逐字）：

```cmake
configure_file("__config_site.in" "${LIBCXX_GENERATED_INCLUDE_TARGET_DIR}/__config_site" @ONLY)
configure_file("${LIBCXX_ASSERTION_HANDLER_FILE}" "${LIBCXX_GENERATED_INCLUDE_DIR}/__assertion_handler" COPYONLY)
...
configure_file("module.modulemap.in" "${LIBCXX_GENERATED_INCLUDE_DIR}/module.modulemap" @ONLY)
...
add_custom_command(OUTPUT "${LIBCXX_GENERATED_INCLUDE_DIR}/libcxx.imp"
  COMMAND "${Python3_EXECUTABLE}" "${LIBCXX_SOURCE_DIR}/utils/generate_iwyu_mapping.py" "-o" "${LIBCXX_GENERATED_INCLUDE_DIR}/libcxx.imp"
  DEPENDS "${LIBCXX_SOURCE_DIR}/utils/libcxx/header_information.py"
  COMMENT "Generate the mapping file for include-what-you-use" )
```

`configure_file(... @ONLY)` 做的是 CMake 的变量替换，而模板 `__config_site.in`
用的宏是 CMake 专有的（逐字，节选）：

```
#cmakedefine _LIBCPP_ABI_VERSION @_LIBCPP_ABI_VERSION@
#cmakedefine _LIBCPP_ABI_NAMESPACE @_LIBCPP_ABI_NAMESPACE@
#cmakedefine01 _LIBCPP_ABI_FORCE_ITANIUM
#cmakedefine01 _LIBCPP_ABI_FORCE_MICROSOFT
#cmakedefine01 _LIBCPP_HAS_THREADS
#cmakedefine01 _LIBCPP_HAS_MONOTONIC_CLOCK
#cmakedefine01 _LIBCPP_HAS_MUSL_LIBC
#cmakedefine01 _LIBCPP_HAS_THREAD_API_PTHREAD
#cmakedefine01 _LIBCPP_HAS_THREAD_API_EXTERNAL
#cmakedefine01 _LIBCPP_HAS_THREAD_API_WIN32
#cmakedefine01 _LIBCPP_HAS_THREAD_API_C11
#cmakedefine _LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS
#cmakedefine01 _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS
#cmakedefine _LIBCPP_NO_VCRUNTIME
#cmakedefine _LIBCPP_TYPEINFO_COMPARISON_IMPLEMENTATION @_LIBCPP_TYPEINFO_COMPARISON_IMPLEMENTATION@
#cmakedefine01 _LIBCPP_HAS_FILESYSTEM
#cmakedefine01 _LIBCPP_HAS_RANDOM_DEVICE
#cmakedefine01 _LIBCPP_HAS_LOCALIZATION
#cmakedefine01 _LIBCPP_HAS_UNICODE
#cmakedefine01 _LIBCPP_HAS_WIDE_CHARACTERS
#cmakedefine01 _LIBCPP_HAS_TIME_ZONE_DATABASE
#cmakedefine01 _LIBCPP_INSTRUMENTED_WITH_ASAN
#cmakedefine _LIBCPP_PSTL_BACKEND_SERIAL
#cmakedefine _LIBCPP_PSTL_BACKEND_STD_THREAD
#cmakedefine _LIBCPP_PSTL_BACKEND_LIBDISPATCH
#cmakedefine _LIBCPP_HARDENING_MODE_DEFAULT @_LIBCPP_HARDENING_MODE_DEFAULT@
#cmakedefine _LIBCPP_ASSERTION_SEMANTIC_DEFAULT @_LIBCPP_ASSERTION_SEMANTIC_DEFAULT@
#cmakedefine01 _LIBCPP_LIBC_PICOLIBC
#cmakedefine01 _LIBCPP_LIBC_NEWLIB
#cmakedefine01 _LIBCPP_LIBC_LLVM_LIBC
@_LIBCPP_ABI_DEFINES@
@_LIBCPP_EXTRA_SITE_DEFINES@
```

★ `#cmakedefine` / `#cmakedefine01` / `@VAR@` 这三样**都不是 C 预处理语法** ★
它们只有 CMake 的 `configure_file` 认得。所以"不用 CMake"意味着**这三十来行要自己写**——
而它们的取值来自 CMake 的 `config_define(...)` 调用（同一份 `libcxx/CMakeLists.txt`），
也就是说**抄的不只是模板，还有模板的输入**。

（另一条独立的旁证：LLVM 自己的 GN 构建也知道这件事——
`llvm/utils/gn/secondary/libcxx/include/BUILD.gn` 里有
`write_cmake_config("write_config_site")`，`input = "__config_site.in"`、
`output = "$libcxx_generated_include_dir/__config_site"`。
★ 连 GN 都得专门写一个"跑 cmake 配置替换"的动作，而不是绕开它。★）

**本节的作用**：它把 §3.7 候选 (c) 的裁决从"判据"变成"结论"，
并且**同时**是 §6 待查第 1 条里 `__config_site` 那半条的答案。

## 8. 代价清单（一句话版）

- **四个构件，四件事**：libc++（标准库）、libc++abi（`__cxa_*` + 异常 + RTTI）、
  libunwind（读 `.eh_frame` 展开）、compiler-rt builtins（`__udivti3` 一族）——
  **缺任何一个的症状都不一样，不能互相顶替**。
- **builtins 是唯一已经有实测证据的缺口**：内核实测过
  `ld.lld: error: undefined symbol: __udivti3`（`kernel/time/time.c:272-287`），
  当时的处置是**改代码绕过**，而那条纪律对 C++ 不成立。
- **宿主接口面里，`user/libposix/` 已经有一半**：`malloc` 族 / `write`/`read`/`open`/`close`/
  `fstat` / `getenv` / `strerror` / `abort` 都在；**缺的是** `__cxa_atexit`、
  `aligned_alloc`/`posix_memalign`、整个 `pthread_*`、`clock_gettime`/`nanosleep`、
  `mmap` 的 POSIX 包装、`mprotect`、`setlocale`、以及**整个 `math.h`/`libm`**。
- **两个硬缺口**（`16` 已盘点，本文逐条复核为真）：**没有任何 `math.h`/`libm`**
  （用户态 0 处命中）；**"等一个用户地址"的等待原语不存在**（内核 0 处、用户态 0 处），
  而它卡住 `pthread_cond_wait`/`std::condition_variable`，
  在 `-smp 1` 下自旋不是"慢"而是**锁死**。
- **推荐 (a)：宿主侧交叉编出目标格式的静态库**。它不违背"本机自举终局"——
  口径已经写死在 `16` §6.1（自举 = 在 FEKernel 上编 `qtbase`，不是"长出 `clang`"），
  而且 (a) 的产物正是 (b) 的输入。
- **三个已知的硬开关**：`FE_VMA_MAX = 32`（内核层，每线程一个区间）、
  `.eh_frame` 被**两道闸**挡住（`build.py:102-103` 的编译参数 + `linker.ld:30` 的 `/DISCARD/`）、
  `.init_array` 在**整棵树里 0 处命中**（链接脚本 + `start.asm` 两侧都要改，
  还要把 `ATEXIT_MAX 16` 提上去）。
- **今天的第一个真实阻塞点不在内核**：宿主 PATH 上**没有 CMake**
  （Ninja 有、clang/lld 项目内有），而 `fetch_toolchain.py` 只下载 llvm/qemu/limine。
- **CMake 的取舍（§3.7）**：**今天用 `pip install cmake`（唯一零改动就能用的）**，
  **原则性修复是把 CMake 加进 `tools/fetch_toolchain.py` 的 `ARTIFACTS`**——
  它是"宿主工具的可复现性"问题，**不是自举原则问题**（CMake 跑在 Windows 上不违反任何口径）。
  候选 (c)"不用 CMake"被 §3.7.1 判死：`libcxx/include/__config` 无条件
  `#include <__config_site>`，没有它**编译期第一步就失败**，而那个头只有 CMake 能生成。
  ★ 顺带：`ninja` 在 PATH 上同样是运气，走 (a) 时要一起决定它从哪来 ★
- **★ S1 的口径被线程配置改写了（§7.1）★**：`_LIBCPP_HAS_THREADS` 是**全有或全无**，
  而且**关掉线程就必然关掉单调时钟**。今天 FEKernel 没有"等一个用户地址"，
  所以**打开线程一定跑不了**（`-smp 1` 下自旋 = 锁死）。
  **S1 应当 `-DLIBCXX_ENABLE_THREADS=OFF`**，把 `std::thread` 从 S1 移到 S3
  （与 `docs/16` §4 的 S3 分工一致）；想打开线程时的正路口子是
  **`LIBCXX_HAS_EXTERNAL_THREAD_API=ON` + 自写 `<__external_threading>`**，
  而不是改 `_LIBCPP_ABI_*`。
- **验收要分段，反向对照要改坏配置而不是加 if**：R1 删 `.init_array`、
  R2 把 `.eh_frame` 放回 `/DISCARD/`——这两条测的是**构建配置**，
  运行期的分支测不到它们。
- **本文一行代码都不动**：另一个代理正在改内核源码并独占构建与虚拟机，
  所以本文的全部结论来自读文件；**没有一个实测数字**，凡是没测的都写了"待实测"
  与怎么测（§5.4、§6 共 15 条）。
