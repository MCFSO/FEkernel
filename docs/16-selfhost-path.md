<!-- SPDX-License-Identifier: 0BSD -->
# 在 FEKernel 上编译 Qt（本机自举路线与缺口）

> ★ **本文不属于本项目的交付物。** ★ 它是一份"**外部需求记录**"：用途有两个——
> (1) 记录**内核必须提供什么机制**（例如 K11「等一个用户地址」就是被 Qt/libc++ 这条线
> 逼出来的，`21-user-address-wait.md` 是它的设计）；(2) 给拿这个内核去做系统的第三方
> 当路线参考。**不要把它当成本项目的待办清单。**（**2026-10-01 范围决定，提出者：用户**）
>
> ★ 指向具体产物 ★ 本文逼出来的内核项：**K11「等一个用户地址」**（设计见
> `21-user-address-wait.md`）与 **`20-posix-kernel-requirements.md` §2** 的主表；
> 本文 §4 的 S1–S10 是**外部路线**，不是本项目的里程碑表（那一份是
> `01-milestones.md`）。★

> **这是一次定位变更。** 提出者：**用户**；日期：**2026-09-26**。
> 新口径只有一句：**裁剪过的 Qt 5.15 LTS `qtbase` 必须能在 FEKernel 上被编译出来，
> 而且走的是"在 FEKernel 里跑编译器"这条路——本机（native），不是交叉编译。**
>
> 本文**只做路线与缺口，不动一行代码**。理由在 §5：用户已经决定**先把 `exec` 做完**
> （`15-exec.md` 的 K6）再开 Qt。在 `exec` 之前动 Qt 相关的任何东西都是在沙地上盖楼。
>
> 第三方代码的处理方式也已定：**放 `third_party/`、用它们自己的 SPDX 标识、
> 由 `tools/add_spdx.py` 对那个目录豁免**。豁免已在 2026-09-26 落地（§2.6）。

## 1. 这条要求的含义

### 1.1 "本机编译"与"交叉编译"不是同一件事

今天这个项目的构建是**交叉编译**，而且是最纯粹的一种：宿主机是 Windows，
目标机是 `x86_64-unknown-none-elf`，编译器是 Windows 上的 `clang.exe`（§2.1）。

| | 交叉编译（今天） | 本机编译（新要求） |
|---|---|---|
| 编译器在哪跑 | Windows | **FEKernel 里** |
| 编译器的宿主环境需要什么 | Windows 的 CRT 与文件系统 | **FEKernel 的 libc、fd、进程、管道、shell** |
| 谁提供头文件 | `toolchain/llvm/` 与 `user/include/posix/` | 同一份，但**必须能被目标机上的编译器找到** |
| 谁提供 C++ 标准库 | **没有**（今天根本没有 C++ 这条路，§2.1） | 必须先有 libc++ 之类的实现，且能编到目标机上 |
| 谁来驱动编译 | `tools/build.py`（Python，跑在 Windows 上） | **目标机上的一个构建系统**（make/cmake/ninja），由 shell 驱动 |
| 谁来链接 | Windows 上的 `ld.lld.exe` | 目标机上的链接器（或让 `clang` 直接吐可执行文件） |
| 失败时会暴露什么 | 只有编译/链接错误 | **外加**解释器缺失、文件系统写不了、进程起不来、管道是 `ENOSYS` |

★ 一句话总结这个差别 ★ 交叉编译只考验**编译器本身**；本机编译考验**整个平台**（内核 + 用户态运行库 + 工具链）。
今天的 `tools/build.py` 之所以能跑，是因为 Windows 替我们做了进程、管道、shell、
动态加载、文件系统这五件事——这五件事在 FEKernel 上**一件都还没通**（§2.4）。

### 1.2 它实际上等于推进到 `10-posix-layer.md` §1 写下的"自举"终点

`10-posix-layer.md` 开头那条验收标准里写着：

> 而且它有一个自带的、可验证的终点——**自举**（在 FEKernel
> 上编译 FEKernel）。自举需要的是 libc + 一个 C 编译器，不需要 KDE。

于是这次的要求可以精确地写成：

**自举的终点从"能编 FEKernel"扩展到"能编 qtbase"。**

这不是换终点，是**把同一条路的验收物换成一个更难、也更值钱的东西**，理由有三条：

1. **FEKernel 自己是 freestanding 的**（`-ffreestanding -nostdlib`，§2.1）。
   它能被编出来，**证明不了 libc 可用**——它压根不用 libc。
   而 `qtbase` 是一个"把宿主环境用满"的正常 C++ 程序：它要 libc、libm、libc++、
   线程、动态加载探测、文件系统写入、shell 脚本、构建系统。
   **用它做终点，"自举"才第一次真的证明了"这个平台的 POSIX 层能用"。**
2. **它是别人写的代码。** `10-posix-layer.md` §6 那条验收（"源码不改"）在
   Qt 上会被推到极限：Qt 的源码不会为我们让步，任何"改一改就能编"都是失败。
3. **它有一个不含形容词的判据**：`qtbase` 的构建产物存在、能被目标机上的程序加载
   并跑出一段正确输出（§4 的 S8/S10）。

**代价（如实说）**：这条路线比"自举 FEKernel"贵得多，而且贵在一个不显眼的地方——
**FEKernel 是短命的小程序，qtbase 是几百个翻译单元的长期构建**。
今天内核里所有"进程生命周期短，所以这样也行"的取舍（`10-posix-layer.md` 已知边界
最后一行"堆只增不减"、`15-exec.md` §8 的"多线程 `exec` = 先杀掉调用者之外的
其它线程，而那一步**不可回滚**"——机制与代价见 `13-tasks-and-kill.md` §6）在 Qt 上
**全部会咬人**。这不是缺陷，是这条要求第一次把这些取舍摆到台面上。

## 2. 现状盘点（每一项都亲自读过文件，出处写在行内）

> ★ **本文 §1.2 列出的那条"libc++ 链"（libc++ / libc++abi / libunwind /
> compiler-rt builtins）的实施清单已经独立成文**：`docs/17-libcxx-build.md`。
> 本文（`16`）只保留**路线与现状盘点**；那份文档回答的是"要做哪几件具体的事、
> 每件怎么验收"，并带着自己的待查项（`17` §6 / §6.1）。
> ★ 两份文档口径不一致时**以 `17` 为准**——它是后写的，逐条复核过本文的证据。
> 到第二轮为止，本文有**五处说法已被它按文件或按官方文档核实推翻**：
> ① §2.2 的头文件计数（15 → 12，见本节）；
> ② §2.4 的 `O_APPEND`（"返回 `ENOSYS`" → "被接受、被记住、然后被静默忽略"，见 §2.4）；
> ③ §2.5 的 `.eh_frame`（**一道闸 → 两道闸**，见 §2.5）；
> ④ §2.3 的线程替代方案（"libc++ 允许 `_LIBCPP_ABI_*` 之类的开关" → 官方机制是
> **`LIBCXX_HAS_EXTERNAL_THREAD_API=ON` + 自己写 `<__external_threading>`**，见 §2.3）；
> ⑤ §4 的 **S1 口径**（S1 的目标**不再是多线程**：`std::thread` 移到 S3，
> 见 §4 的 S1/S3 与"顺序理由"第 2 条；依据 `17` §5.1/§5.4/§7.1）。

### 2.1 工具链：`tools/build.py` 的实际口径

| 项 | 实际值 | 出处 |
|---|---|---|
| 目标三元组 | `x86_64-unknown-none-elf` | `tools/build.py` 的 `KERNEL_CFLAGS` / `USER_CFLAGS` 第一行 |
| C 标准 | `-std=c17` | 同上 |
| freestanding | `-ffreestanding -nostdlib -fno-builtin` | 同上 |
| 代码模型 | 内核 `-mcmodel=kernel`；用户态 `-mcmodel=small` | 同上 |
| PIC/PIE | `-fno-pic -fno-pie`（**用户态也是**） | 同上 |
| SIMD | 内核 `-mno-sse -mno-sse2 -mgeneral-regs-only`；用户态 `-msse2 -mfpmath=sse` | 同上 |
| 用户态加载基址 | `USER_BASE = 0x400000`，非 PIE 静态 ELF | `tools/build.py` + `user/linker.ld` |
| 链接器 | `ld.lld`（`tc.find_ld_lld()`），**只有 lld，没有任何 `ld`/`ld.bfd`** | `tools/build.py:build_kernel` / `build_user` |
| 汇编器 | NASM（`tc.find_nasm()`） | 同上 |
| 构建编排 | **Windows 上的 Python**（`tools/build.py`）——不是目标机上的 make | `tools/build.py:main` |

**没有一条 C++ 编译路径**：两个 `*_CFLAGS` 里只有 `-std=c17`，构建编排也只
`rglob("*.c")` / `rglob("*.asm")`（`build_user`）。今天往 `user/bin/` 放一个
`.cpp`，它**不会被构建**——这是事实，不是推测。

#### ★ 核实结论：`toolchain/llvm/` 里没有 C++ 运行时，也没有目标侧编译器运行时 ★

逐项核实（`toolchain/llvm/`，LLVM 23.1.2 的 `clang+llvm-*-x86_64-pc-windows-msvc` 包）：

| 期待有 | 实际 | 核实方式 |
|---|---|---|
| libc++ 头 `include/c++/v1/` | **没有**。`toolchain/llvm/include/` 下只有 `clang`、`clang-c`、`clang-tidy`、`lld`、`llvm`、`llvm-c` | 列目录 |
| libc++abi / libunwind | **没有**。整个 `toolchain/llvm/` 递归搜 `libc++`/`libcxx`/`libunwind`/`cxxabi`/`libstdc++`，只命中 clang 自己的两个定义文件 `include/clang/Basic/TargetCXXABI.{def,h}` | 递归文件名搜索 |
| 目标侧 compiler-rt builtins | **没有**。递归搜 `*.a` / `*.so` / `*.dylib` → **0 个命中** | 递归扩展名搜索 |
| 有的那一个 | 只有 Windows 版：`lib/clang/23/lib/windows/clang_rt.builtins-x86_64.lib`（外加 `asan*`/`ubsan*`/`fuzzer*`/`profile*`，**全是 windows 版**） | 列 `lib/clang` 递归 |
| clang 内建头 | 有，且是 freestanding 版本：`lib/clang/23/include/` 里有 `stddef.h`、`stdint.h`、`stdarg.h`、`limits.h`、`float.h`、`stdbool.h`、`unwind.h` 等 | 列目录 + 逐个 `Test-Path` |
| `lib/clang/23/include/unwind.h` | **有**——但它是**接口声明**（供 `__Unwind_*` 调用），没有实现 | 同上 |

这解释了 `user/include/posix/` 里为什么只放了 12 个头（§2.2。★ 这一句原先写的是"15 个"，
计数已按实测改正，见 §2.2 的说明）：
**标准里"编译器自带"的那几个（`stddef.h` / `stdint.h` / `stdarg.h` / `limits.h`）
确实不用自己写**，clang 的资源目录里就有，而且 freestanding 形态是对的。
这条是本项目"我们自己的头 vs 抄来的头"边界的一个正面例子，应当保持。

#### ★ 这与内核里那次 `undefined symbol: __udivti3` 是同一个根因 ★

`kernel/time/time.c:272-287` 记着那次实测：

> `(unsigned __int128)delta * 1e9 / hz` 是最直观的写法，但它会引出
> `__udivti3` —— 编译器运行时库里的 128 位除法辅助函数，而内核是
> **freestanding** 链接的（`-nostdlib`），那个符号不存在，链接直接失败
> （实测：`ld.lld: error: undefined symbol: __udivti3`）。
> 内核里没有 libgcc，所以 128 位除法这条路是堵死的。

**根因一模一样**：`clang` 在"没有原生指令"时会生成对编译器运行时库的调用
（128 位除法 → `__udivti3`，64 位除法 → `__udivdi3`，浮点转换 → `__floatundidf` ……），
而我们的包里**只有 Windows 版的那一份**（`clang_rt.builtins-x86_64.lib`，
MSVC `.lib` 格式，且是为 Windows 目标编的）。
目标三元组 `x86_64-unknown-none-elf` 一份都没有。

于是今天有两个后果：

1. **内核**：靠"不写会引出这些符号的代码"绕过（`time.c` 把 128 位除法拆成两段
   64 位）。这是一条**纪律**，不是一条保证——下一个人写一行 `__int128` 除法就会
   把构建打回原形，而且报错发生在链接期、离那一行很远。
2. **用户态 / C++**：绕不过去。C++ 的标准库、异常展开、原子操作、`__int128`
   到处都是。**"把 compiler-rt builtins 编到目标机"是这条路线上一件必须先做的事**，
   它的位置在 §4 的 S5，不是"以后有空再说"。

**代价**：多一个"自己从 `compiler-rt` 源码编 builtins 到 `x86_64-unknown-none-elf`"
的构建步骤（vendor 进 `third_party/`），并让链接命令行显式带上它。
**收益**：把那条"不许写 `__int128` 除法"的隐性纪律变成"符号存在"的显性事实——
后者可以被链接器验证，前者只能靠人记。

### 2.2 用户态 C 库：`user/include/posix/` 与 `user/libposix/` 的现状

**已有的头（12 个，`user/include/posix/`）**：
`dirent.h`、`errno.h`、`fcntl.h`、`spawn.h`、`stddef.h`、`stdio.h`、`stdlib.h`、
`string.h`、`sys/stat.h`、`sys/types.h`、`sys/wait.h`、`unistd.h`
（另有 `user/include/` 下的 `fe_*.h`，那是内核 ABI 投影，不是 POSIX）。

> ★ 这一句原先写的是"**已有的头（15 个，`user/include/posix/`）**"；本节开头另一处
> 也写过"`user/include/posix/` 里为什么只放了 15 个头"。**实测是 12 个**：
> `user/include/posix/` 下**递归列出的 `.h` 恰好就是上面这 12 个**
> （其中 3 个在 `sys/` 下：`sys/stat.h`、`sys/types.h`、`sys/wait.h`）。
> **这只错在计数，清单本身对得上**——12 个名字逐个都能在目录里找到，没有多写也没有漏写。
> 至于那 3 个是怎么多出来的，**没有实证，这里不猜**（可能是把 clang 内建的同名头
> `stddef.h`/`stdint.h`/`stdarg.h`/`limits.h` 一并算了进去，也可能是别的；
> 这条留给下一个人，**不要拿它当结论**）。
> ★ 与计数相邻的另一个数字要分清：§2.2.1 结论那段里的 "**25** 行 / **5** 行判'有' /
> **17** 个头 / 缺 **13** 个"，是按**行**数出来的，与"12 个文件"不是同一把尺子
> （同一行里可以列好几个头）。**换个尺子必须重新数，不能拿 15、12、13 互相加减。**

**已有的实现（`user/libposix/`，7 个 .c）**：
`fmt.c`（完整 `printf` 族）、`heap.c`（arena + 合并 + 自检）、`posix.c`（fd 表、
文件/目录/stat/access/unlink）、`spawn.c`（`posix_spawn` + `waitpid`）、
`stdio.c`（FILE 流、`sscanf`、`perror`、`exit` 契约）、`stdlib.c`
（`atexit`/`strtol`/`strtod`/`getenv`/`setenv`/`qsort`/`bsearch`）、
`string.c`（`memchr`/`memmove`/`strstr`/`strtok_r`/`strdup`/`strerror`…）。

**缺的头 / 函数**（★ 这一层是自举最重的一层，而且它**完全不依赖内核** ★）：
按 Qt 与 libc++ 真正会碰到的顺序列：

| 缺什么 | Qt 里谁碰它 | 难度与说明 |
|---|---|---|
| `time.h`（`time`/`clock_gettime`/`localtime`/`mktime`/`strftime`） | **QtCore 的日期时间是本命**（`QDateTime`/`QElapsedTimer`/`QTimer`）；构建系统也用 `time()` 判新旧 | 中。内核已有 K7 的 TSC 时钟与 `fe_clock_ns()`，缺的是**日历换算**（闰年、时区）；没有 RTC，`time()` 只能给"自启动以来的秒数"，这一点必须**如实写下来**而不是编一个纪元 |
| `sys/mman.h`（`mmap`/`munmap`/`mprotect`/`madvise`） | 所有分配器（libc++ 的 `operator new` 大块走 `mmap`）；`qtbase` 的 `QArrayData` 大规模分配 | 中偏大。`mmap` 今天只有**内核 ABI**（`fe_mem_map(handle,...)`，`user/include/fe_user.h:441`），**没有 POSIX 包装**——`user/` 下搜 `mmap` 只命中注释与 `hxtest` 的说明文字，没有任何实现。`mprotect` **内外都没有**（§2.3） |
| `pthread.h`（`pthread_create/join/mutex_*/cond_*/once/key_*`） | **QtCore 的 `QThread`/`QMutex`/`QWaitCondition`/`QThreadStorage`**；构建期 `moc` 也起线程 | 中。**底座已经有了**：`fe_thread_create`（`fe_user.h:417`）、内核 TLS 基址（K9 ✅）、`fe_notification_*`、`fe_wait_any`（内核侧 ✅）。但是 **`pthread_*` 一个都没有**，而且 `pthread_join` 需要"等线程退出"这个今天不存在的 ABI（内核只有 `wait` 等**任务**句柄，没有等线程） |
| `signal.h`（`signal`/`sigaction`/`raise`/`sigprocmask`/`kill`） | QtCore 的崩溃处理、`QProcess`、`qFatal` | 大，且**依赖内核 K5**（§2.3）。`kill` 的底座有了（K2 ✅ `TASK_TERMINATE`），但"把异常交给用户注册的处理者"今天**完全没有** |
| `sys/socket.h` + `sys/un.h` + `netinet/in.h` + `netdb.h` | `QLocalSocket`（Unix socket 是 Qt 的进程间通信默认路径）、`QHostAddress`、`QSslSocket` | **很大，且今天明确不做**（`10-posix-layer.md` §7 第一行："网络（socket/DNS/TLS）—— 没有 TCP/IP 栈"）。★ 但 Qt 的构建系统**会探测它们**（`configure` 的 feature 检测），所以"缺"与"探测时优雅降级"是两件事，见 §4 的 S9 |
| `poll.h` / `sys/epoll.h` / `sys/select.h` | `QEventDispatcherUNIX` 的核心（`poll` 是它的默认后端） | 中。内核有 `fe_wait_any`（K4 ✅），把它投影成 `poll` 是**接口工作**；`epoll` 可以先用 `poll` 顶上（如实记下"这是模拟"） |
| `sys/eventfd.h` / `sys/inotify.h` | `QEventDispatcherUNIX` 的自唤醒管道；`QFileSystemWatcher` | 小（eventfd：内核有通知对象，包装即可）/ 中（inotify：**没有**任何底座，要新建机制）。Qt 对两者都有"没有就换实现"的分支 |
| `dlfcn.h`（`dlopen`/`dlsym`/`dlclose`/`dlerror`） | `QPluginLoader`、`QLibrary`，以及 **Qt 自身的模块化加载** | **很大**（`10-posix-layer.md` §7：动态链接是**独立项目**）。★ 但 `qtbase` **可以在 `-static` 下构建**，那时 `dlopen` 只需返回错误——**"能编出来"与"能加载插件"是两条不同的要求**，这一点要在 §6 的待查清单里定死 |
| `pwd.h` / `grp.h` | `QFileInfo` 的属主查询、`QDir` 的 home 路径 | 小。没有用户概念，如实返回固定值或 `ENOSYS`——**但不能返回假数据**（`posix.c` 的 `stat` 注释已经立了这条规矩："报 0 而不是编一个时间"） |
| `sched.h`（`sched_yield`/`sched_getaffinity`） | `QThread::yieldCurrentThread`、`QThread::idealThreadCount` | 小。`fe_yield()` 已有；`idealThreadCount` 可以从 `TASK_LIST`/`CLOCK_INFO` 那类自述接口取 CPU 数（今天没有"CPU 数"的自述接口，见下一行） |
| `sys/auxv.h`（`getauxval`） | glibc 特有的东西，Qt 用它做少量探测 | 小但**语义特殊**：FEKernel 没有 auxv，正确做法是**返回 0 并让调用者走降级分支**，而不是假装有一个 |
| `sys/ioctl.h` | `QFile` 的 `ioctl`、终端查询 | 小。没有 TTY 语义（`posix.c:114` 的注释写明"三个标准 fd 都通向控制台……而不是假装有 TTY"） |
| `sys/random.h` | `QRandomGenerator` 的系统熵源 | 小，但**注意**：今天连"系统熵"都没有。要么加一个内核随机源，要么**如实**退回到"不安全"的降级分支——Qt 允许后者，但我们必须把它写在文档里 |
| `sys/resource.h`（`getrlimit`/`setrlimit`） | Qt 的栈大小探测 | 小。判据在 `10-posix-layer.md` K8："内存总量、CPU 数、页大小——内核知道，用户态猜不出来" |
| `semaphore.h` | QtCore 的 `QSemaphore`（在 `pthread` 之上，所以排在 `pthread` 后面） | 小（纯用户态，建在 `pthread_mutex`+`cond` 上） |
| `libm`（`math.h` + 实现） | **QtCore 与 QtGui 大量使用**（`qmath.h`、几何、`QTransform`、`qdrawhelper`） | **中偏大，且容易被忽略**。今天 `user/libposix/` 里**没有任何数学函数**，`math.h` 也不存在。`fmt.c` 里的 `%f` 是自己算的。这是"看起来只差头文件、实际差一个库"的典型 |
| C++ 侧：`libc++`（或替代）、`libc++abi`、`libunwind`、`<new>`、`<typeinfo>`、`<exception>`、`<atomic>`、`<thread>` | Qt **全部** | **最大的一块**，见 §2.1 与 §4 的 S1–S5 |

#### 2.2.1 逐个头文件核对（"要求里的那一份清单" vs 我实际读到的）

上面那张表是按**依赖顺序**写的；下面这张是按**头文件名**逐个核对的结果，
用来防止"我以为有"这类错误（`10-posix-layer.md` §9 那个"声明了却没实现"
的教训 ×4 就是这么来的）。判定列只有三种：**有**（文件与实现都在）、
**只有头**（`#include` 能过，链接会失败）、**没有**。

| 头 | 判定 | 证据 |
|---|---|---|
| `<pthread.h>` | **没有** | `user/include/` 全树搜 `pthread` → 0 处 |
| `<dlfcn.h>` | **没有** | 同上搜 `dlfcn` → 0 处 |
| `<poll.h>` | **没有** | 同上搜 `poll` → 0 处（只有 `blkd` 里"轮询"这两个汉字） |
| `<signal.h>` | **没有** | 同上搜 `signal`/`sigaction` → 0 处 |
| `<time.h>` | **没有** | 同上搜 `time.h`/`clock_gettime` → 0 处。★ 但内核侧的 `fe_clock_ns()`（K7 ✅）与 `FE_SYS_CLOCK_INFO` 都在，缺的是**用户态包装 + 日历换算** |
| `<sys/mman.h>` | **没有** | 同上搜 `mmap` → 只命中 `fe_user.h:446` 的注释与 `hxtest/main.c:620` 的测试说明。内核 ABI 是 `fe_mem_map`/`fe_mem_unmap`（`fe_user.h:441/447`），**没有 POSIX 包装** |
| `<sys/time.h>` | **没有** | 同上 |
| `<sys/socket.h>` | **没有** | 同上搜 `socket` → 0 处 |
| `<sys/un.h>` | **没有** | 同上搜 `un.h`/`sockaddr_un` → 0 处 |
| `<sys/eventfd.h>` | **没有** | 同上搜 `eventfd` → 0 处 |
| `<sys/inotify.h>` | **没有** | 同上搜 `inotify` → 0 处 |
| `<sys/ioctl.h>` | **没有** | 同上搜 `ioctl` → 0 处 |
| `<sys/random.h>` | **没有** | 同上搜 `random` → 无相关实现 |
| `<sys/syscall.h>` | **没有，且不应该照 Linux 的做** | FEKernel 的号空间是自己的（`fe_user.h:402` 的 `fe_syscall(n, …)`）。★ 提供这个头**必须**连号表一起提供并注明"这不是 Linux 的号"，否则 `SYS_gettid` 会静默变成另一个系统调用 |
| `<sys/auxv.h>` | **没有** | 同上搜 `auxv`/`getauxval` → 0 处 |
| `<sys/resource.h>` | **没有** | 同上搜 `rlimit` → 0 处 |
| `<sys/uio.h>` | **没有** | 同上搜 `iovec`/`readv`/`writev` → 0 处 |
| `<pwd.h>` | **没有** | 同上搜 `pwd.h`/`getpwuid` → 0 处 |
| `<grp.h>` | **没有** | 同上 |
| `<sched.h>` | **没有** | 同上搜 `sched_` → 0 处（`fe_yield` 在 `fe_user.h` 里，不是 POSIX 形态） |
| `<semaphore.h>` | **没有** | 同上搜 `sem_`/`semaphore` → 0 处 |
| `<netinet/in.h>` | **没有** | 同上搜 `netinet`/`in_addr` → 0 处 |
| `<math.h>` + `libm` | **没有** | 同上搜 `math.h`/`sin(`/`sqrt(` → 0 处。★ 这一条最容易被漏掉：Qt 的几何与绘制大量用它 |
| `<wchar.h>` / `<locale.h>` | **没有** | 同上。Qt5 在多数配置下不依赖 `wchar_t`，但 `configure` 会探测 |
| `<stddef.h>` `<stdint.h>` `<stdarg.h>` `<limits.h>` `<float.h>` `<stdbool.h>` | ★ **有**（clang 内建） | `toolchain/llvm/lib/clang/23/include/` 下逐个 `Test-Path` 为真，且是 freestanding 形态 |
| `<stdio.h>` `<stdlib.h>` `<string.h>` `<errno.h>` `<dirent.h>` `<fcntl.h>` `<unistd.h>` `<spawn.h>` | **有** | `user/include/posix/` 下实有 |
| `<sys/types.h>` `<sys/stat.h>` `<sys/wait.h>` | **有** | 同上 |

★ 结论 ★ 上面 **25** 行里，标"**有**"的只有 **5** 行，而摊开是 **17 个头**：
其中 **6 个是 clang 自带的**（本来就不该我们自己写），**11 个是我方已有的**
（`user/include/posix/`）。
也就是说"缺的头"是 **13 个**，而且它们**没有一条需要新的内核机制**
（除 `sys/mman.h` 的 `mprotect` 与 `signal.h`）。这就是 §4 把 S1–S5 排在 S6 之前的原因。

★ 但"有头"不等于"能用" ★ 上表判"有"的每一行，**只保证 `#include` 能过**。
真正的可用性在 `2.2` 那张按函数列的表里——例如 `<unistd.h>` 声明了 `pipe`，
而 `pipe` 的实现返回 `ENOSYS`（`posix.c:760`）。
这正是 `10-posix-layer.md` §9 那条教训的同一形状：
**头文件描述的是意图，链接器只在有人调用时才报错。**

★ 这一层为什么值得先做 ★ 它**完全不依赖内核的新机制**（除了 `mprotect` 与信号），
而且它有一个不会骗人的验收方式：**一个真实程序在 FEKernel 上跑对**——
这正是 `10-posix-layer.md` §6 已经建立的模式。先把 `libc++` 跑起来，
Qt 的失败才会聚焦到"Qt 特有的东西"上，而不是混在"libc 还没有"里。

### 2.3 内核：自举前置要哪些机制

`11-kernel-next.md` §1 的三条判据（C1 执行模型 / C2 内存模型 / C3 时间与可见性）
今天的状态（§2 的表述，我核对了 §3 的 K 列表与 §2 的现状表）：

| 判据 | 今天 | 自举需要它出哪一份力 |
|---|---|---|
| **C2** 内存模型 | 按需分页 ✅ + 栈增长 ✅，**缺区间属性变更 `mprotect`** | 见下 |
| **C1** 执行模型 | 创建/终止/等待/多路等待 ✅，**缺 K5 异常处理者与 K6 `exec`** | 见下 |
| **C3** 时间与可见性 | 真实时钟 ✅、任务枚举 ✅（`ps`）、诊断 ✅ | 编译器的驱动要 `_exit` 后的退出码（有）、要计时（有）、要"我在哪"（**没有**，见 §2.4 的 `/proc/self/exe` 一行） |

#### `mprotect`（C2 缺的那一条）：**今天一条都找不到**

核实：全项目搜 `mprotect` / `MPROTECT` / `vma_protect`，**内核里 0 处**，
`user/` 下 0 处（只有 `toolchain/` 里 clang 自带的 sanitizer 头提到它）。
`kernel/ld/elf.c:119` 的注释写着"`mprotect`/`munmap` 都要它"，也就是
**当初就知道会需要，只是没做**。

谁需要它：

| 谁 | 要 `mprotect` 干什么 | 不做会怎样 |
|---|---|---|
| **C++ 运行时（libc++ / libc++abi）** | ①把 `.data.rel.ro`（重定位后的只读表）在启动时**降权成只读**——这是 RELRO 的机制；②`operator new` 的大块分配走 `mmap` 后可能要 `madvise`/`mprotect` | ①不做只是"少一层加固"（可以接受，但必须写下来）；②**做不了就是硬失败** |
| **Qt 的 QML/V4 引擎** | JIT 需要"先写后执行"——**必须先 `mprotect(PROT_WRITE)` 再 `mprotect(PROT_EXEC)`** | ★ **今天完全不可能**：内核是 W^X（`11-kernel-next.md` §2 的 VMM 行写着 W^X），一条区间不能同时可写可执行，而"先写后执行"必须靠两次属性变更。**没有 `mprotect` 就没有 JIT** |
| **栈保护（guard page）** | 在栈下方放一页 `PROT_NONE` 把"栈溢出"变成可捕获的信号 | 依赖 K5（没有处理者的话，`#PF` 就是杀线程，guard page 只是"死得更整齐"） |
| **编译器自己** | `clang` 的 `-fsanitize` 与 JIT（`clang-repl`）都要它；**我们把它们关掉**（§6） | 关掉即可，代价写进文档 |

**代价**：`mprotect` 在本内核里**不是加一个 syscall 那么简单**。今天的模型是
"VMA 是意图、页表是事实"（`11-kernel-next.md` §6），属性变更必须**同时**改
区间表与**已经映射的那些页**的 PTE，而且方向不能反：
区间先改、页表后改，中间那一刻"意图与事实不一致"，`#PF` 解析会读到旧意图——
这正好是 `11-kernel-next.md` §7 里已经踩过的那类坑的形状。
另外 `FE_VMA_MAX` **只有 32**（`kernel/include/fe/mm/vma.h:65`）——见下。

#### ★ `FE_VMA_MAX = 32`：一个今天不痛、Qt 上必痛的边界 ★

`kernel/include/fe/mm/vma.h:65` 写死 `#define FE_VMA_MAX 32`。
今天够用：一个用户程序就是一个 ELF（几个段）+ 一段栈 + 一两段 `mmap`。
**Qt 上不够**，而且不够得非常快：

- `libc++` + `libc++abi` + `libm` + `libQt6Core`/`libQt5Core` 每一个都带若干段；
- 编译期 `clang` 的每一份临时分配、每一个 `malloc` arena 的 `mmap` 都占区间；
- `mprotect` 一旦实现，**按页改属性会天然产生碎片**（相邻区间属性一旦不同就不能合并）。

**这不是"以后优化"，是"跑到一半报 `FE_ERR_NOSPC`"**——而 `vma.c` 里表满确实
返回 `NOSPC`（`11-kernel-next.md` §6 的实测行："表满 NOSPC"）。
所以要写进路线：**`FE_VMA_MAX` 要么调大、要么改成动态表**，判据是
"§4 的 S4/S5 里区块间的正反向对照"。

#### K5（用户态异常处理者）：**今天完全没有**

`11-kernel-next.md` §3 的 K5 行情是"挡容错：一个 `#PF` 就死一个线程"，状态 ⬜。

谁需要它：

| 谁 | 要它干什么 |
|---|---|
| **C++ 的异常与终止** | `throw` 本身靠 `libunwind` + `.eh_frame`（`user/linker.ld` 今天**主动 `*(.eh_frame*)` 丢弃**，见 §2.4）。但 `std::terminate` / 未捕获异常 / `abort()` 的**栈回溯打印**，Linux 上走的是"信号 + `backtrace()`"。没有 K5，编译器崩溃时我们只拿到一个退出码 |
| **Qt** | `Q_ASSERT`/`qFatal` 的现场、`QProcess` 的子进程死亡通知、崩溃处理器 |
| **所有"跑飞了"的诊断** | 今天的 `#PF` 一律杀线程——在"要编译几百个翻译单元"的场景下，这意味着**第一个错误就是最后一个错误** |

#### K6（`exec`）：见 §5

`15-exec.md` 已经为它写了完整设计。这里只说它与自举的关系：
**编译器驱动会 spawn `clang -cc1` 与链接器**，而 `make`/`cmake` 会用
`exec` 语义启动每一个编译动作（`posix_spawn` 在没有 `fork` 时是标准替身，
但它撑不住"驱动自己变成 `cc1`"这条路径）。详见 §5。

#### ★ "等一个用户地址"这类同步原语：今天完全没有 ★

这是最容易漏掉、也最难绕的一条。它要解决的是：**A 线程要等 B 线程把某个
用户内存里的值改掉**。

| 谁需要它 | 为什么 `fe_notification_*` 顶不上 |
|---|---|
| **`pthread_cond_wait` / `std::condition_variable`** | 条件变量是"用户内存里的一个条件 + 一个等待队列"。`fe_wait_any`（K4 ✅）等的是**内核对象**（端点/通知），不是用户地址。没有它，`pthread_cond_wait` 只能写成**自旋**——而 Qt 的线程池、`QWaitCondition`、`moc` 的并发都用它 |
| **`std::mutex` 的公平唤醒** | 自旋锁在多 vCPU 上会把 CPU 烧光；今天 QEMU 默认 `-smp 1`（`tools/run_qemu.py`），**自旋在单核上会直接把系统锁死**（等的人占着 CPU，被等的人永远跑不到） |
| **futex 语义的正确实现** | Linux 的 `futex` 是"内核检查用户地址上的值，不等于期望就立刻返回"。这条语义**只能内核做**（要原子地"检查用户内存 + 入队睡眠"），用户态拼不出来：自拼的版本在"检查完、还没睡下"的窗口里会**丢唤醒** |

**真实难度**：这是一条**新的等待对象类型**（"用户地址"作为等待目标），
要接进 `fe_wait_any` 的等待节点机制（`11-kernel-next.md` §8 那套）。
★ 它今天**完全没有**，连"待办"都没有列在 K 清单里 ★——
这是本次盘点发现的**最大缺口**，比 `mprotect` 更靠前，因为**没有它，多线程
C++ 程序在单核上跑不起来**。
（★ 这一句原先接着写"而 §4 的 S1 目标就是多线程"——**那句已经被推翻**：
`docs/17` §5.1/§7.1 把 `std::thread` **移出了 S1**，线程归 §4 的 **S3**，
理由见下一条。缺口本身仍然成立，而且**更硬**：它现在是 S3 的硬前置。★）

替代方案（必须在 §6 里定死，不能含糊）：把 `sizeof(std::mutex)` 换成一个
**内核通知对象句柄**的封装，或者先用 `fe_wait_any` + 通知对象实现
**"通知对象版"的条件变量**并把"不满足 POSIX 的 `pthread_cond_t` 尺寸/语义"
写在明处。两条路都要付代价，**不能假装 `pthread_cond_t` 就是 48 字节**。

> ★ **上面这条替代方案里的机制说法被后续核实推翻了** ★
>
> **原先写的是**："把 `sizeof(std::mutex)` 换成一个**内核通知对象句柄**的封装
> （libc++ 允许 `_LIBCPP_ABI_*` 之类的开关，但那是 ABI 层面的取舍）……"
>
> ★ **libc++ 并不允许你用 `_LIBCPP_ABI_*` 之类的开关去换线程实现** ★
> `_LIBCPP_ABI_*` 管的是 ABI（版本、命名空间、`type_info` 比较方式一类），
> **不是"线程 API 由谁来提供"**。官方为这件事准备的口子是**另一个**：
>
> | 机制 | 怎么用 | 依据 |
> |---|---|---|
> | **`LIBCXX_HAS_EXTERNAL_THREAD_API=ON` + 自己写一个 `<__external_threading>` 头** | 定义 `_LIBCPP_HAS_THREAD_API_EXTERNAL` 时，`<__thread/support.h>` **直接转发**给 `<__external_threading>`，而那个头**必须存在**、必须提供与 `<__thread/support.h>` 完全相同的接口 | libc++ 的 `ThreadingSupportAPI` 设计文档（逐字引文与 URL 见 `docs/17-libcxx-build.md` §7.1） |
>
> 同页还写着两条同向的约束：`_LIBCPP_HAS_NO_THREADS`
> "**should not be manually defined by the user**"；而改**未文档化**的
> `_LIBCPP_ABI_*` 会撞官方 User 文档的警告（"Configuration macros that are not
> documented here are not intended to be customized by developers"）。
>
> ★ 换机制**不改变代价**，只改变"谁来写" ★ `docs/17` §7.1 的结论值得原样搬过来：
> 这条路（它编号 T3）**今天照样卡在同一处**——那个头**必须实现
> `__libcpp_condvar_*`**，而它的底座（"等一个用户地址"）不存在。
> **它换的是"谁来写"，不是"能不能跑"。**

### 2.4 构建系统：qtbase 构建期真正会用到、而今天明确不行的东西

`10-posix-layer.md` §7/§9 的"已知边界"是这一节的地基。我把每一条**标注它是
"Qt 源码需要"还是"Qt 的构建系统/工具需要"**——这两类难度完全不同：
前者要等 Qt 自己被移植过去，后者要在**第一批 Qt 代码编译之前**就通。

| 缺什么 | 今天的实际状态（出处） | **谁需要** | 难度为什么不同 |
|---|---|---|---|
| `fork` | `10-posix-layer.md` §4 与 §9 边界表：**设计决定，不做**；`posix.h` 里没有；`posix_spawn` 是替身 | **构建系统/工具**（`configure` 的 `AC_FORK` 式探测、部分 Makefile 的 `$(shell)`） | ★ 这一类里最难的一条。"不做 fork"是**写在设计里的决定**（`kernel/include/fe/process.h`），要改的是定位而不是实现。**候选出路：让 `configure` 走 `posix_spawn` 分支**——但那要求先证明它真的能走通，见 §6 |
| `pipe` | `user/libposix/posix.c:760`：**明确 `errno = ENOSYS`** | **构建系统/工具**（`configure \| tee`、`make` 的子进程输出、`grep` 的管道） | 中。底座有（内存对象 + 端点，`09-handle-transfer.md` 已经实测 1 MiB 往返），缺的是"两个 fd 共享一个偏移 + `dup2` 接得上"的语义层 |
| `chdir` / `mkdir` / `rename` / `rmdir` / `chmod` | `posix.c:716/767/774/781/788`：**全部 `ENOSYS`**；`getcwd` 恒返回 `"/"` | **构建系统/工具**（`configure` 的第一件事就是 `mkdir` 一个临时目录、`cd` 进去试编译） | 中。`mkdir`/`rename` 需要 fsd 新操作；`chdir` 需要**每进程工作目录**（今天"进程的当前位置就是根"，`posix.c:703` 的注释）。**这一条挡在所有构建之前**：`configure` 起不来，后面全白做 |
| `O_APPEND` | `user/include/posix/fcntl.h:21` 有 `#define O_APPEND 00002000`，但 `user/libposix/posix.c:245` 只把它**存进 fd 表**（`e->flags = (u32)flags;`）——**全项目搜 `O_APPEND`，除了定义与这一行存 flags，没有任何一处读它** | **构建系统/工具**（日志、`.d` 依赖文件、"追加一行到 config.h"） | 中。要在 fsd 里加"追加"语义，或在打开时把偏移定位到文件尾（后者有竞态，且 Makefile 并发时会写坏——**要如实选一个并写下来**） |
| 动态链接（`.so` / `dlopen`） | `10-posix-layer.md` §7：**独立项目，第一里程碑范围内不做**；`USER_CFLAGS` 里是 `-fno-pic -fno-pie` | **两者都要**（Qt 的 `QPluginLoader` 是源码需要；`configure` 的"能不能编动态库"是构建系统需要） | ★ 最大的一条。**但 `qtbase` 可以 `-static` 构建**（§6 待查）。差别是"能编出来" vs "能加载插件"，**必须把验收口径定死** |
| `/proc/self/exe` | 没有 procfs；`10-posix-layer.md` §9 边界表里那条"程序要 spawn 别的程序时得知道自己在哪个槽 → 从 `argv[0]` 推前缀"就是它的替身 | **构建系统/工具**（`cmake`/`ninja` 定位自己、`qmake` 找 `qt.conf`） | 中。本项目已经有 `fe_slot_prefix`（`fe_user.h:682`）。**候选出路：把"我在哪"做成一个自述 syscall**，而不是造一个 procfs |
| `pthread_*` | **一个都没有**（§2.2） | **Qt 源码需要**（`QThread`/`QMutex`/`QWaitCondition`） | 中，但**它比构建系统那一类更靠前**：`moc` 自己就起线程 |
| Unix socket / socket | `10-posix-layer.md` §7：**没有 TCP/IP 栈**；`sys/socket.h`/`sys/un.h` 都不存在 | **Qt 源码需要**（`QLocalSocket`） | 大。但 Qt 有"没有就关掉这个 feature"的开关（`configure -no-feature-*`），**关键是别让 `configure` 因为探测失败而整体中止**——那是构建系统那一类的问题 |
| `inotify` / `eventfd` | 都没有（§2.2） | **Qt 源码需要**（`QFileSystemWatcher` / 事件派发的自唤醒） | 小（eventfd，有通知对象底座）/ 中（inotify，无底座） |
| `getauxval` / `sys/auxv.h` | 不存在 | **Qt 源码需要**（少量探测） | 小，见 §2.2 |
| `syscall(SYS_gettid)` | **直接没有意义**：FEKernel 没有 Linux 系统调用号表，`fe_syscall` 的第一个参数是 FEKernel 自己的号（`fe_user.h:402`） | **Qt 源码需要**（`QThread::currentThreadId` 在 Linux 上就是 `gettid`） | 小但**容易踩**：任何硬编码 Linux syscall 号的代码在 FEKernel 上会拿到**另一个 FEKernel 系统调用的语义**。★ 这一类必须靠"`configure` 探测时如实拒绝"挡住，而不是靠"恰好没用到" |
| 没有 RTC | `posix.c:476-501` 的注释："时间戳一律 0：没有 RTC……**报 0 而不是编一个时间**"；`08-kernel-completion.md` 已知限制 #6 | **两者都要**（`configure` 拿 `time()` 判文件新旧） | 中。★ **`configure` 会依赖"时间在走"**：它用时间戳判断"这次编译出的产物比上次新"。如果 `time()` 恒定，"缓存"逻辑会退化——**要实测**（§3 的待实测清单） |
| shell | `user/bin/sh/main.c:14`：**"命令形式故意简单（空白分隔，不支持引号与重定向）"**；只有 `help`/`echo`/`rm`/`ls`/`cat`/`fs`/`clear`/`exit` **八个**内建（★ 这一格原先写的是"**七个**"且列了七个名字、漏了 `rm`；实测 `sh/main.c:450-505` 是 **8** 个分支，与 §3.3 的"8 个内建命令"一致 ★） | **构建系统/工具** | 见 §3 的"shell"专段 |
| `make` / `cmake` / `ninja` / `sed` / `grep` / `awk` / `pkg-config` | 一个都没有 | **构建系统/工具** | ★ 见 §3 与 §4 的 S6 |
| `clang` 本身 | **没有目标侧编译器**（只有 Windows 上的 `clang.exe`） | **两者都要** | ★ 见 §6 的鸡生蛋 |

★ 读这张表的正确姿势 ★ 它按"谁需要"分成两栏之后，**顺序就自己出来了**：
"构建系统/工具需要"的那一栏（`fork`/`pipe`/`chdir`/`mkdir`/`O_APPEND`/shell/工具链）
**全部在"Qt 源码需要"的那一栏之前**——因为 Qt 源码要能被编译，
先得有一个能跑的构建系统。而今天这一栏**几乎全是 `ENOSYS`**。

★ 但 `O_APPEND` 那一格**不是 `ENOSYS`——这一条被后续核实推翻了** ★

> **原先写的是**："`O_APPEND` | `10-posix-layer.md` §9 边界表：**返回 `ENOSYS`
> （不假装支持）**；`fcntl.h` 里 `O_APPEND` 常量有，fsd 没有语义"。

**实测不是这样**（逐条复核写在 `docs/17-libcxx-build.md` §2.8，本文照它改正）：

- `open()` **接受**这个标志：`user/libposix/posix.c:245` 只把它存进 fd 表
  （`e->flags = (u32)flags;`），打开**不会失败**；
- 写路径（`posix.c:373`）按 `e->offset` 写，写完 `e->offset += (long)done;`（`posix.c:385`）——
  **从头写、顺序覆盖**；
- **全项目搜 `O_APPEND`：除了 `user/include/posix/fcntl.h:21` 的定义与 `posix.c:245`
  那一行存 flags，没有任何一处读它**（`posix.c`、`stdio.c` 都没有）。

所以 `O_APPEND` 是**被接受、被记住、然后被静默忽略**：打开成功，写入从偏移 0
**覆盖**已有内容。这正好是 `user/include/posix/fcntl.h:4-6` 自己的头注释警告要防的那种失败——
"我们**不支持**的位必须能被识别出来并报错，不能当成'没给'——那会让'要追加写'静默变成'覆盖写'"。
**它比"返回 `ENOSYS`"严重一档：从"已如实报错"降级成"静默做错事"。**

★ 一条必须一起记下的例外，否则下一处又会写错 ★
`stdio.c:127-132` 的 `fopen(path, "a")` **确实**是 `errno = ENOSYS; return NULL;`——
所以"追加"这件事有**两条入口**，它们的行为**不同**：

| 入口 | 今天的行为 |
|---|---|
| `fopen(path, "a")`（`stdio.c:127`） | **明确报错**（`ENOSYS`，返回 `NULL`）——注释写着"假装支持会更糟……所以明确报错" |
| `open(path, O_APPEND\|…)` + `write()`（`posix.c:245/373`） | ★ **静默覆盖** ★ |

`fopen` 报错是对的，但**没有拒绝 `open()` 那条路**：直接用 `open` + `write` 的程序
（构建工具、`moc`/`rcc` 那类写临时文件的代码，正是 §3.4/§4 的 S6/S9）会拿到
"成功但写坏"的结果。**这一条作为一条独立缺口记下**，而不是"已经如实报错"。

（对照：`posix.c:100` 的 `case FE_ERR_NOTSUP: return ENOSYS;` 是**错误码翻译**，
不是 `O_APPEND` 的路径。`posix.c` 里真正返回 `ENOSYS` 的是 `chdir:719`、`pipe:763`、
`mkdir:770`、`chmod:777`、`rmdir:784`、`rename:791`、`console_read:288`——**没有 `open`**。）

### 2.5 附：一个我没有预期到的事实

`user/linker.ld` 的 `/DISCARD/` 段**主动丢弃 `*(.eh_frame*)`**：

```
/DISCARD/ : {
    *(.eh_frame*)
    ...
}
```

今天这是对的（用户态全是 C，没有异常），但它意味着
**"把 `.eh_frame` 丢掉"是一条已经写进构建配置的决定**。

> ★ **但"丢 `.eh_frame`"不是只有这一道闸——下面这一条被后续核实补全了** ★
>
> **本节原先只写了上面这一段**（链接脚本的 `/DISCARD/` 是唯一一处），给人一种
> "改链接脚本就够了"的印象。**实测是两道闸，而且第一道在编译器那一侧**：
>
> 1. **`tools/build.py:102-103`** 的 `-fno-asynchronous-unwind-tables` /
>    `-fno-unwind-tables`（`KERNEL_CFLAGS` 的同样两行在 `build.py:56-57`）
>    → **编译器根本不生成 `.eh_frame`**；
> 2. `user/linker.ld:30` 的 `*(.eh_frame*)` `/DISCARD/` → 就算生成了也丢。
>
> ★ **只改第 2 道闸（改链接脚本）什么都不会发生**，因为第 1 道闸让输入是空的 ★
> 顺序不能反，必须**先去掉编译开关**。
>
> ★ 还有第二件容易只做一半的事：**链接脚本里没有 `.eh_frame` 输出段** ★
> `user/linker.ld` 全文只有 `.text`（`:10-12`）、`.rodata`（`:15-17`）、
> `.data`（`:20-22`）、`.bss`（`:24-27`）**四个输出段**。所以修法是**两步**：
> ① 去掉 `build.py:102-103` 的两行；② 把 `*(.eh_frame*)` 从 `/DISCARD/` 里拿掉，
> **并新增一个 `.eh_frame` 输出段**。只做"不在 `/DISCARD/` 里"、
> 不给它输出段，`ld.lld` 会按默认规则安置它——**那正是"看起来改对了、实际位置未定义"**。
> （配套还涉及 `.eh_frame_hdr` 与 `__eh_frame_start`/`__eh_frame_end` 一类的边界符号，
> 见 `docs/17-libcxx-build.md` §4.2。）
>
> ★ 顺带一条彼处新核实的事实 ★ **内核的链接脚本也丢 `.eh_frame`**：
> `kernel/linker.ld:69` 同样在 `/DISCARD/` 里写着 `*(.eh_frame*)`。
> 这不是问题（内核不用异常，`KERNEL_CFLAGS:56-57` 同样关掉了 unwind tables），
> 但读本节的人可能以为"只有用户在丢"。**两块链接脚本在这个点上一致、都是有意的**，
> 记在这里免得下一个人误以为内核里还留着 `.eh_frame` 可以借。

C++ 的异常展开（`libunwind`）**必须**有 `.eh_frame`（或 DWARF CFI），
而且链接器脚本里**没有 `.init_array` / `.ctors` / `.fini_array` 段定义**。

★ 后果 ★ 即使我们把 libc++ 编出来了，**全局对象的构造函数也不会被调用**
（`start.asm` 里只调 `__posix_set_envp` 和 `main`，没有任何 `__libc_start_main`
或 `_init` 循环；全项目搜 `init_array`/`ctors`/`__libc` → **0 处命中**）。
Qt 的静态对象（`Q_GLOBAL_STATIC`、元类型注册表）**全靠这个**。
所以这是 §4 的 S2 的判据，也是"看起来编过了、跑起来莫名其妙"这类症状的根源。

### 2.6 附：`third_party/` 的 SPDX 豁免**已经落地**（2026-09-26）

★ 这一节原先写的是"豁免还没做"★ 它在本节写下之后**当天就被做掉了**，
所以这里改成记录**结果**与**为什么它必须排在任何第三方代码之前**。

当时的风险是这样来的：`tools/add_spdx.py` 只跳过三个目录

```python
SKIP_DIRS = {"build", "toolchain", ".git"}
```

`third_party` 不在其中，而 `collect()` 用的是 `ROOT.rglob("*")`（全树遍历）。
于是把 Qt 源码放进去的那一刻：`--check` 会列出几万个缺标识的文件并返回 1；
而**不带 `--check`** 跑一次，会把 `/* SPDX-License-Identifier: 0BSD */`
插到 Qt 的每一个 `.c`/`.h` 头上——**那是许可污染，比检查失败严重得多**。

现在的事实（`tools/add_spdx.py`）：

- 新增 `VENDORED_DIRS = {"third_party"}`，与 `SKIP_DIRS` 并列、**同一种匹配语义**
  （路径分段精确相等，不做"路径里带 third_party 就放过"这类模糊匹配）；
- 豁免落在 `collect()` 这个 `--check` 与自动补头**共用**的唯一入口上，
  所以补头模式**结构上不可能**写进豁免目录（这条有隔离实验为证，见下）；
- `third_party/README.md` 写明了这个目录的规则：保留上游许可证文件与 SPDX 头、
  在登记表登记（名字/版本/许可证/来源/为什么需要/是否被我们改过）、
  **本目录不使用 0BSD、也不受 `add_spdx.py` 管理**。

可证伪的证据（做豁免时跑的）：`third_party/` 下临时放一个**没有** SPDX 头的 `.c`
→ `--check` 仍报"所有源文件都有 SPDX 标识"、退出码 0；`docs/` 下放同样一个文件
→ `--check` 报 `缺少 SPDX 标识的文件 1 个`、退出码 1；隔离根目录下跑自动补头模式，
`docs/p.c` 被加了头而 `third_party/p.c` 的 SHA256 前后一致。

★ 顺带照出一件事，值得记在这里 ★ 那个工具**自己一直没有 SPDX 首行**，
它之所以通过自己的检查，只是因为判据是"前 400 个字符里出现该子串"，
而它第 18 行的常量字面量 `SPDX = "SPDX-License-Identifier: 0BSD"` 正好落在 400 之内
（偏移 353）。给它的文档字符串加三行之后，它立刻把自己报成缺失文件。
**"检查通过"不等于"检查在做你以为的那件事"**——这是本项目第二次栽在
"判据比意图宽松"上（第一次是 `check_init.py` 在报告不符之后仍然打印"一致"）。

## 3. 硬阻塞（必须先解决，否则后面全白做）

### 3.1 内存：可用 462 MiB，"编 qtbase 需要多少"是**待实测**

实测事实（引用，不是新测）：

| 项 | 值 | 出处 |
|---|---|---|
| QEMU 默认内存 | `-m 512M` | `tools/run_qemu.py:212` 的 `--mem` 默认值 |
| VirtualBox 默认内存 | 512 MB | `tools/run_vbox.py:170` 的 `--memory` 默认值 |
| 内核看到的可用内存 | **462 MiB**（映射条目 25） | `01-milestones.md:59`（`[内存] 可用内存 462 MiB / 映射条目 25`）、`:90`（`PMM 就绪: 462 MiB 可用`）、`:95`（`当前空闲 118320 (462.18 MiB)`） |
| PMM 空闲帧 | 118320 帧 × 4 KiB | `01-milestones.md:95` |

★ **"编 qtbase 需要多少内存"是待实测，本文不写任何数字** ★
理由：我没有在 FEKernel 上跑过任何编译器，宿主机上编 qtbase 的内存曲线
**不能直接搬**（宿主机有 8~64 GiB、有 swap、有页缓存；FEKernel 没有 swap、
没有页缓存回收、`heap.c` 的 arena **只增不减**——`10-posix-layer.md` 已知边界
最后一行明写"堆只增不减"）。把它当成"大概够了"是最典型的一类编造。

**打算怎么测（四级，逐级加严）**：

| 级 | 怎么测 | 它回答什么 | 什么时候能做 |
|---|---|---|---|
| **0** | 在宿主机的 `clang` 上对**同一份源码、同一组翻译单元**逐个编，记录 RSS 峰值与 `-ftime-trace` 的峰值内存 | "单个翻译单元的量级是多少"（例如最常见的 `qstring.cpp` 是几十 MB 还是几百 MB） | **今天就能做**，只写脚本，不碰内核 |
| **1** | 把单个翻译单元的峰值 × 并发度（`make -jN`，N 从 1 开始） | "峰值需求"与"并发度"的关系。★ 这一条决定"能不能 `-j1`"——**如果 `-j1` 都要超过 462 MiB，那就得先加内存，而不是先写代码** | 级 0 之后 |
| **2** | 用 `--mem` 逐级抬（512M → 1G → 2G → 4G），记录**内核能起来且自检全过**的上限；同时用 `FE_SYS_MEM_INFO`/`TASK_LIST` 观察内核自身开销 | "这台虚拟机最多能给多少"——`01-milestones.md` 的所有数字都是 512M 下的，**更大的内存没有实测过** | 需要跑 QEMU，**与当前内核工作冲突，等 §5 的时机** |
| **3** | 目标侧编译器上去之后，用同一个 §4 的 S1 程序做"编译一个真实翻译单元"的端到端测量 | 真实数字。★ 在它之前，所有"够了/不够"都是猜测 | S1–S7 之后 |

★ 顺带一条今天就该记下的账 ★ 今天的磁盘尺寸全是"按 MiB 计"的：
`tools/run_qemu.py:44` 的基准数据盘 `VDISK_BYTES = 64 * 1024 * 1024`（64 MiB，**只读**）；
可写的 FAT32 镜像由 `mkfat.py` 按 payload 算出来，并有硬下界
`mkfat.py:883` 的 `volume_bytes = max(int(payload * 2) + 8 MiB, 48 MiB)`。
而 `qtbase` 的源码树是**几百 MiB**、构建中间产物按**GiB** 计。
也就是说：**内存可能不是第一个撞墙的，"盘放不下"可能才是。**
这一条同样**待实测**（源码树与中间产物的实际大小），但它决定了 §4 的 S6
"构建系统要不要支持增量/分段构建"——**这条必须写进路线，不能等撞上再说**。

### 3.2 进程 / 管道 / 作业控制

| 需要 | 今天 | 为什么是硬阻塞 |
|---|---|---|
| `fork` | §2.4：设计决定"不做" | 构建系统与 `configure` 的默认假设。**但它有替身**（`posix_spawn`），所以它是"必须先证明替身够用"的阻塞，不是"必须实现 `fork`"的阻塞 |
| `pipe` | `ENOSYS`（`posix.c:760`） | ★ 这条**没有替身**：`configure | tee`、`make` 收集子进程输出、`grep` 的输入，都要它。而且 `dup2` **今天就有**（`posix.c:440`），所以缺的只是"两端" |
| 作业控制 / 进程组 | 完全没有（`10-posix-layer.md` §7："完整信号语义（进程组、作业控制）……其余等有程序要"） | `make -jN` 与 `configure` 会**杀进程组**（`kill(-pgid, SIGTERM)`）。没有它，一个失败的构建会留下**跑飞的编译器**，而今天唯一的止血方式是 `killtest` 那条"按任务句柄杀"的路 |
| `waitpid` 的 `WUNTRACED`/`WIFSIGNALED` | `waitpid` 有（`spawn.c:191`），但状态字只有退出码（`sys/wait.h` 只定义了 `W_EXITCODE`） | `make` 要区分"编译失败"与"被信号杀死"。**没有它，诊断会撒谎**（"失败"与"被杀"看起来一样） |

### 3.3 shell（这一条要单独说，因为它有连锁需求）

Qt 5.15 的 `configure` **是一个 sh 脚本**（`configure` 本身是一段很长的
POSIX sh，会层层调用 `config.tests/` 下的小脚本）。而本机自举意味着：

> **机器上要先有一个能跑它的 shell，以及它依赖的那一整套基本工具。**

连锁需求（每一层都被上一层需要）：

```
configure (POSIX sh 脚本)
  ├─ 需要: sh 支持 引号 / 变量替换 / $(...) / 反引号 / case / for / if / 函数 / 重定向 / 管道
  ├─ 需要: 一组外部命令  mkdir, cd(builtin), rm, mv, cp, cat, sed, grep, tr, sort, uniq, expr, printf, test
  │         └─ 它们各自又要  libc + argv/envp + stdin/stdout/stderr + 退出码
  ├─ 需要: 一个"能编译一个 .c 文件并运行它"的动作（config.tests 的每个测试都是这样）
  │         └─ 这就是 §2.1 的目标侧编译器与 compiler-rt builtins
  └─ 需要: 一个 make（configure 的最后一步是生成 Makefile）
            └─ make 需要: fork/exec、pipe、时间戳、shell、以及"文件比另一个文件新"
```

**今天这一条上有什么**：一个**交互式**小 shell——`user/bin/sh/main.c:14`
自己写着"命令形式故意简单（空白分隔，**不支持引号与重定向**）"，
只有 **8** 个内建命令（`help`/`echo`/`rm`/`ls`/`cat`/`fs`/`clear`/`exit`，
`sh/main.c:450-505`），**重定向只做在 `echo` 这一个内建里**（`sh/main.c:453`
起的一段"重定向：`echo text > file` / `>> file`"），不是 shell 级的语法；
而且它**不能跑脚本文件**（它从控制台端点读整行，没有"执行一个脚本文件"的入口）。
`ARG_MAX` 是 **8**（`sh/main.c:23`）。

★ 所以"缺一个 shell"这句话是**严重低估** ★ 缺的是：

1. **一个能读脚本文件的 POSIX sh**（引号、展开、重定向、管道、函数）；
2. **十来个基本工具**（它们本身是可执行文件，要各自被移植/自研）；
3. **一个能把"编译一个 .c 并运行它"表达出来的构建系统**。

代价（如实说）：这一条**与 Qt 无关也照样要付**——任何"在 FEKernel 上构建"
的路线都要这些。所以它**不是 Qt 的额外负担**，而是自举的入场券。
值得单独说的是：**这三样东西里没有一样需要 Qt**，所以它们可以、也应该
排在 Qt 之前做（§4 的 S6/S7）。

### 3.4 `moc` / `rcc`：**"能编 C++"不等于"能编 Qt"**

这是本次要求里最容易被忽略的一条，必须单独说清楚。

Qt 的构建期要在**目标机**上编译出 `moc`（Meta-Object Compiler）与 `rcc`
（Resource Compiler），然后**运行它们**，用它们的输出再编译真正的 Qt 源码。

```
Qt 的构建流程（自举时序）
  1. 编译 QtCore 的一小部分        ← 需要 C++ 编译器 + libc++
  2. 用第 1 步的产物编译 moc/rcc   ← 需要 QtCore 的这部分**已经能跑**
  3. 运行 moc/rcc                  ← ★ 在目标机上"运行"一个刚编出来的 C++ 程序
  4. moc 的输出 (.moc/.cpp) 再参与编译 → 生成完整 Qt
  5. 步骤 4 又会产生新的 moc 输入 → 回到步骤 3（多轮）
```

★ 这条链上"能编 C++"只解决了第 1 步 ★ 后面每一步都要：
进程能起来（有）、能写文件到磁盘（有，fsd 写路径 ✅）、
**能执行一个刚生成的可执行文件**（`exec` 或 `spawn`：spawn 有，`exec` 没有）、
以及 `moc` 自己**是多线程的**（`pthread`，没有）并且**用 `QFile`/`QDir`
做大量文件 IO**（有，但 `chdir`/`mkdir`/`rename` 没有）。

**所以 `moc`/`rcc` 是这条路线上的第一个"真实 Qt 程序"**——
它比 `qtbase` 本身小得多，却把"编译出来的 Qt 程序能不能在 FEKernel 上跑"这件事
一次性问清楚了。**路线里必须把它当一个独立阶段（S9），不能混进 S10。**

## 4. 分阶段路线（S1…S10）

写法仿 `11-kernel-next.md` §4：每阶段一个**可证伪的终点**，
并写清"依赖什么"与"失败时最先看哪里"。

★ 贯穿全部阶段的纪律 ★ 每个"正向"都要配一条**反向对照**，
而且反向对照必须是**会失败**的那种（`11-kernel-next.md` §8 的教训：
"自检里绝不能用'本该阻塞'的接口去验证它没立刻返回"）。

### S1：一个 C++ 程序在 FEKernel 上跑出正确输出

| 项 | 内容 |
|---|---|
| **终点（可证伪）** | 一个 C++ 程序用 `std::string` / `std::vector` / `throw`-`catch` 与**全局对象的构造**，在 FEKernel 上打印固定的期望输出并以 0 退出；`init` 核对退出码与输出**逐字节相符**。★ **S1 不含 `std::thread` / `std::mutex` / `std::condition_variable`**：本节原先写的是"用 `std::string` / `std::vector` / `std::thread` / `throw`-`catch`"，**那句已被推翻**——线程**整体**归 S3，理由见"依赖"一行与顺序理由第 2 条（依据 `docs/17-libcxx-build.md` §5.1/§5.4/§7.1）★ |
| **反向对照** | ① 故意抛一个**没人接**的异常 → 必须是"明确的终止 + 可识别的退出码"，不是静默变成别的行为；② `std::vector` 越界读（`.at()`）必须抛而不是读出垃圾 |
| **额外判据** | 走 `FE_SYS_PF_STAT`：C++ 程序的**缺页解析次数必须 > 0**。理由同 `11-kernel-next.md` §6："从用户态看不崩什么都证明不了"——C++ 映像比 C 大得多，只有 `>0` 才说明地址空间真的是按需建立的 |
| **依赖** | 目标侧 compiler-rt builtins（§2.1）、一个最小 C++ 运行库（`operator new`/`delete`/静态构造/异常展开）、`time.h` 的最小可用集；★ **libc++ 的线程支持必须显式关掉**：`-DLIBCXX_ENABLE_THREADS=OFF`，**连带 `-DLIBCXX_ENABLE_MONOTONIC_CLOCK=OFF`**（`docs/17` §7.1：`_LIBCPP_HAS_THREADS` 与 `_LIBCPP_HAS_MONOTONIC_CLOCK` 双向绑定，CMake 侧对"关线程却不关单调时钟"直接 `FATAL_ERROR`）★。**本节原先写的是"`pthread_create` 或明确的'单线程版 `std::thread`'"，那句已被推翻**——libc++ 的线程开关**全有或全无**（mutex/condvar/thread_id/thread/TLS 一起开），**没有"只要 `std::thread` 不要 `mutex`"的粒度**；而打开线程就要求 `__libcpp_condvar_wait`，其底座（"等一个用户地址"，§2.3）今天**不存在**。自旋版在 `-smp 1` 下是**锁死**而不是变慢，所以"先随便写个自旋版"不是一条能走的路。★ **连带的另一条**：单调时钟被一并关掉，所以 **S1 连 `std::chrono::steady_clock` 都用不了**——`std::chrono` 的单调部分与 `std::this_thread::sleep_for` 都归 S3，S1 的"时间"只能走 C 侧那点最小可用集 |
| **失败时最先看哪里** | ① `undefined symbol: __udivti3` 一族 → §2.1（builtins 没链上）；② 全局对象的构造没跑 → §2.5（`.init_array` 没被链接脚本收录、`start.asm` 没有走 `_init` 循环）；③ 异常一抛就崩 → **先看 `tools/build.py:102-103` 的 `-fno-*-unwind-tables`（编译器根本没生成 `.eh_frame`），再看 `.eh_frame` 被 `/DISCARD/` 丢了**（§2.5，★ 顺序不能反 ★）；④ 冒出 `undefined symbol: __libcpp_mutex_lock` / `__libcpp_condvar_wait` / `__libcpp_thread_create` 一族 → **说明线程开关被打开了**（`docs/17` §7.1），与 S1 的 `LIBCXX_ENABLE_THREADS=OFF` 不符；⑤ `fatal error: '__config_site' file not found` → CMake 那一步没跑成（`17` §7.2） |

### S2：C++ 静态构造 / 析构 / 虚表 / RTTI / `typeinfo`

| 项 | 内容 |
|---|---|
| **终点** | 一个"注册表"模式：多个翻译单元各有一个全局对象，在 `main` 之前把名字注册进一张表；程序打印**注册顺序与条目数**，与期望逐字相符；`dynamic_cast` 与 `typeid` 在多重继承下给对结果 |
| **反向对照** | 故意让两个翻译单元的构造顺序被链接顺序调换 → 输出必须**变化**（证明测的是真实构造顺序，不是打印了一份硬编码的表） |
| **依赖** | 链接脚本加 `.init_array`/`.fini_array`；`start.asm` 在调 `main` 前跑构造、之后跑析构；`__cxa_atexit`。★ 与 S1 **不冲突**：`dynamic_cast`/`typeid` 走的是 **RTTI**（`LIBCXX_ENABLE_RTTI`），**不在线程开关那一组里**（官方原话："This option may only be set to `OFF` when `LIBCXX_ENABLE_EXCEPTIONS=OFF`"，见 `docs/17` §3.4），所以 S1 的 `LIBCXX_ENABLE_THREADS=OFF` 不影响 S2 的两条判据 |
| **失败时最先看哪里** | ★ 顺序不能反 ★ 先 `tools/build.py:102-103` 的两行编译开关，再 `user/linker.ld` 的 `/DISCARD/`（§2.5）；`user/libfe/start.asm`（只有两个 `call`） |

### S3：线程与同步（★ 这一阶段决定"单核能不能跑 Qt" ★）

| 项 | 内容 |
|---|---|
| **终点** | 4 个线程 + 一个互斥量 + 一个条件变量：生产者/消费者跑 N 轮，消费者收到的总数与生产者发出的**精确相等**；同时用 `std::thread::join` 在主线程回收 |
| **反向对照** | ① 把所有线程改成"不等待、纯自旋"的版本 → 在 `-smp 1` 下必须**卡死或超时**（证明"等一个用户地址"这条机制真的存在且必要，见 §2.3）；② 故意丢一次唤醒（改测试夹具）→ 总数必须对不上 |
| **依赖** | `pthread.h` / `std::thread` / `std::mutex` / `condition_variable`；**"等一个用户地址"的内核原语（今天完全没有，§2.3）**；`pthread_join` 需要"等线程退出"的内核 ABI（今天只有等**任务**句柄）。★ **它是 `std::thread` 的唯一归属阶段**：S1 用 `LIBCXX_ENABLE_THREADS=OFF` 把整套线程 API 关掉（`docs/17` §7.1），所以"多线程 C++ 能跑"这件事**只能在 S3 被证明**；S1 通过之后、S3 之前，任何"多线程 C++ 已经能跑"的说法都是没有证据的（`17` §5.1）。★ 另外记得 S3 要**同时**把 `LIBCXX_ENABLE_MONOTONIC_CLOCK` 打开——关线程时它是被连带关掉的（`17` §7.1） |
| **失败时最先看哪里** | 卡死而非报错 → §2.3 的"等一个用户地址"；`-smp 1` 下偶发卡死 → 自旋把 CPU 占光；`join` 返回后线程的 TLS 被复用 → K9 的 TLS 重建路径 |

### S4：`libc++` 与 `libc++abi` 在目标机上被编出来

| 项 | 内容 |
|---|---|
| **终点** | `libc++` 编出目标格式的库文件，且 S1/S2/S3 的程序**改成链接它**（而不是链接我们手写的替身）后仍然全过。★ 这才是"libc++ 可用"，不是"编译通过" |
| **反向对照** | 把 `libc++` 的某一个容器（比如 `std::map`）故意配置成"绝不分配"→ 必须在压力测试下失败（证明测的是真的 `libc++`，不是空壳） |
| **依赖** | S1–S3 全过；`libc++abi` 的"宿主 ABI 层"（`__cxa_*`、`operator new` 的宿主钩子）；compiler-rt builtins |
| **失败时最先看哪里** | `libc++` 的 `__config` 对宿主环境的假设（它探测 glibc/musl 的版本宏）；`<features.h>` 不存在；locale 相关（Qt 需要 `setlocale` 存在但可以是空实现） |
| **代价** | `libc++` 的 ABI 与我们的 `pthread_cond_t`/`pthread_mutex_t` **尺寸必须一致**——§2.3 那条"不能假装 `pthread_cond_t` 是 48 字节"在这里收账 |

### S5：`libm` + 完整的 POSIX 头覆盖

| 项 | 内容 |
|---|---|
| **终点** | `math.h` 的全部函数（含 `long double`）可用，且**精度判据**：`sin`/`cos`/`exp`/`log` 在一组已知输入上与宿主机的 `clang`/`glibc` 结果比对，误差在 1 ulp 量级内 |
| **反向对照** | 移除 `libm` 后链接必须失败（证明不是"恰好没用到"） |
| **依赖** | S1–S4；§2.2 的整个缺失头清单 |
| **失败时最先看哪里** | ★ 一个容易忽略的地方：`libm` 自己会引出 `__udivti3`/`__floatundidf` 等 builtins，**回到 §2.1** |

### S6：自己就能用的构建系统（shell + 工具 + make/cmake）

| 项 | 内容 |
|---|---|
| **终点** | 在 FEKernel 上跑一条**脚本文件**：它用 `mkdir`/`cd`/重定向/管道/`case`/`$(...)` 干完"配置 + 编译 + 链接 + 运行 + 比对输出"这一整套；判据是**脚本文件本身不被修改**且输出正确 |
| **反向对照** | ① 把 `pipe` 换回 `ENOSYS` → 脚本必须**明确失败**（不是静默给出半个结果）；② `O_APPEND` **今天不是"报错"，而是"静默覆盖"**（§2.4）——这条反向对照要按**今天的事实**写：脚本往日志里追加两条，今天的结果是**只剩最后一条、且没有任何错误**；修完之后，不支持的 `O_APPEND` 必须**明确失败**，不能继续静默降级。★ 本节原先按"报错而不是只剩最后一条"写，那是**修完之后**的要求，被当成今天的症状就是错的（见 §2.4） |
| **依赖** | §3.3 的整条链：POSIX sh（引号/展开/重定向/管道）、基本工具、`fork` 或 `posix_spawn` 的替身证明、`chdir`/`mkdir`/`rename`/`O_APPEND`、`pipe` |
| **失败时最先看哪里** | `posix.c` 里那一串 `ENOSYS`（`pipe`/`chdir`/`mkdir`/`chmod`/`rmdir`/`rename`）；`configure` 里第一个"探测失败"的地方（★ 探测失败必须**看得见**，不能是"猜错了但继续"） |

### S7：目标机上的 C 编译器（★ 鸡生蛋在这里被绕开 ★）

| 项 | 内容 |
|---|---|
| **终点** | 目标机上有一个能跑的 `clang` 驱动，它把**一个单文件 C 程序**编成 FEKernel 能执行的可执行文件，并且这个可执行文件跑出正确输出。★ **不要求它能编译自己**（那是 S10 的事） |
| **反向对照** | 用宿主机上的同一个源码编译出参照可执行文件，两者输出**逐字节比对**；再故意让目标侧编译器用错目标三元组 → 必须**明确拒绝**而不是产出一个格式对、跑不起来的二进制 |
| **依赖** | S1–S6；`llvm`/`clang`/`lld` 源码的移植（见 §6 的裁剪清单） |
| **失败时最先看哪里** | ① `clang` 驱动 spawn `clang -cc1` 失败 → **这就是 `exec` 的第一批真实调用者之一**（§5）；② 链接器找不到 → 用 `lld` 静态链接进驱动（避免"链接器是另一个程序"这一层）；③ 内存（§3.1） |

### S8：QtCore 的一个最小程序（真实 Qt 代码）

| 项 | 内容 |
|---|---|
| **终点** | 一段**不改一行**的 Qt 程序：`QCoreApplication` + `QByteArray` + `QString` + `QJsonDocument`（解析一段固定 JSON 并打印某个字段）+ `QTimer::singleShot` 触发退出。在 FEKernel 上输出与宿主机上的**逐字节相符** |
| **反向对照** | ① 故意给一段**非法 JSON** → 必须走到错误分支并给非零退出码（证明错误路径也通）；② 去掉 `QTimer` 的退出 → 程序必须真的**等在那儿**（证明事件循环不是"立刻跑完"） |
| **依赖** | S1–S7 + `qtbase` 的 `QtCore` 能在宿主机上**交叉**编出来（先证明"Qt 自己能编"，再证明"在 FEKernel 上能编"） |
| **失败时最先看哪里** | 静态初始化没跑（§2.5）；`QThread` 起不来（S3）；时间戳/时钟（§2.4 最后几行） |

### S9：`moc` / `rcc` 自举（§3.4）

| 项 | 内容 |
|---|---|
| **终点** | 在 FEKernel 上编译 `moc`，**运行它**处理一个带 `Q_OBJECT` 的头文件，产出 `.moc` 文件；再把这个 `.moc` 编译进一个程序并跑对（信号槽真的连通：emit 之后槽被调用、参数正确） |
| **反向对照** | ① 头文件里**没有** `Q_OBJECT` → `moc` 必须明确报错或以约定的状态退出，不是静默产出空文件；② 故意写一个签名不匹配的 `connect` → 必须在运行期报告失败 |
| **依赖** | S8；`exec`/`spawn` 能跑刚生成的可执行文件；`moc` 用到的 `QFile`/`QDir`/`QTextStream` |
| **失败时最先看哪里** | `moc` 的线程（`pthread`）；`moc` 写文件（`O_APPEND`/`rename`——★ `moc` 的标准做法是"写临时文件再 rename"）；"刚编出来的程序立刻执行"这条路的文件系统一致性 |

### S10：在 FEKernel 上编译 `qtbase`（终点）

| 项 | 内容 |
|---|---|
| **终点** | 在 FEKernel 上（不是宿主机上）对**裁剪过的 `qtbase`** 跑完配置 + 构建，产出目标格式的库与至少一个可执行文件；该可执行文件在**同一台 FEKernel 上**加载并跑出与宿主机参照**逐字节相符**的输出。★ 判据里必须包含"**构建过程中没有调用宿主机**"这条——否则它退化成交叉编译 |
| **反向对照** | ① 把产物拿到**宿主机**上跑 → 必须是"格式对但跑不了"或明确报错（证明它真的是目标格式）；② 把某一次构建的输出删掉再重跑 → 必须（在增量逻辑下）**不做重复工作**，或（如果没做增量）**重新构建且结果一致** |
| **依赖** | S1–S9 全过；§3.1 的内存与磁盘实测；`FE_VMA_MAX`（§2.3） |
| **失败时最先看哪里** | 内存（§3.1）；磁盘（§3.1 结尾那条"盘可能先撞墙"）；VMA 表满 `NOSPC`（§2.3） |

### 顺序理由（仿 `11-kernel-next.md` §4 的写法）

```
S1  C++ 能跑（最小运行库）              ← 唯一"不知道能不能"的未知项
S2  静态构造/析构/vtable/RTTI           ← S1 的判据会被它证伪：很多"跑起来了"是巧合
S3  线程与同步                          ← 放大器：单核跑不通，后面每一步都在自旋里泡着
S4  libc++ 落地                         ← 从"我们的替身"换成"真的标准库"
S5  libm + POSIX 头覆盖                 ← Qt 的两个隐形大依赖
S6  构建系统（shell + 工具 + make）      ← ★ 所有"编 Qt"的前置，且完全不依赖 Qt
S7  目标机上的 C 编译器                  ← 鸡生蛋在这里被绕开（不要求自编译）
S8  最小 QtCore 程序                     ← 第一次跑真实 Qt 代码
S9  moc/rcc 自举                        ← 第一次"编译并运行一个刚生成的 Qt 程序"
S10 在 FEKernel 上编 qtbase             ← 终点
```

★ 顺序的四条理由 ★

1. **S1 排第一，因为它是唯一的真未知。** 别的阶段缺什么基本都能预判
   （缺头、缺函数、缺 syscall），而"我们的 freestanding 运行时 + 一个 C++
   运行库能不能在这个内核上让一个 C++ 程序活着跑完"**没有先例**。
   把未知放在最前面，是为了让"要不要继续"这个决定**在花掉大量工作之前**做出。
   ★ 第二轮补一条：按新的 S1 口径，**S1 也不许带"单线程版 `std::thread`"这种降级替身**
   （`docs/17` §7.1：线程开关是全有或全无，没有"半个线程支持"可开）。
   正确的写法是：**S1 = 关掉线程的最小可用配置**，它能独立通过、独立证伪，
   而不是"靠一个我们自己写的替身假装 `std::thread` 已经能用"。★
2. **S3 排在 S4 之前，因为它挡的是"能跑"而不是"完整"。** ★ 这一条的理由在第二轮被**改写过**，但**结论没变** ★ 原先写的是"一个自己的 `std::thread` 薄封装（S1 里就有了）足以支撑 S4 的构建"——
   **那句已被推翻**：`docs/17` §7.1 核实 libc++ 的线程开关是**全有或全无**（没有"只要 `std::thread` 不要 mutex"的粒度），所以 S1 **根本不能开着半个线程支持**，它必须 `LIBCXX_ENABLE_THREADS=OFF`；而只要线程是开的（S4 要为目标机产出一个 `qtbase` 能用的 libc++，**必然**要开），就必须有 `__libcpp_condvar_wait`，它的底座正是**"等一个用户地址"这条内核原语**——如果不通，S4 编出来的 `libc++` 一用 `std::mutex` 就会把单核系统锁死。**先证伪"多线程在这个内核上成立"，再去编标准库。**
   ★ 连带的档案变化 ★ S3 不是"S1 那个程序"+线程（S1 那个程序按定义**不含线程**），
   而是 S1/S2 的**同一套程序骨架**加上线程与同步（`docs/17` §5.4 的分段验收 A/B/C：
   其中 C 步=本文的 S3，明确标注"不属于 S1"）。S1 的判据**仍然有效且必须先过**——
   S3 的失败要能定位到"线程这一层"，而不是混在"运行库配不配得起来"里。
3. **S6 排在 S7 之前，因为"编辑器/构建系统"比"编译器"便宜得多，而且它是编译器的前置。**
   `clang` 的构建本身就要一个构建系统；如果我们在 S6 之前就去做 S7，
   会不得不在 Windows 上编 `clang`——那等于**把"本机自举"这条要求偷偷降级成
   "交叉编译一个编译器"**。这正是 `10-posix-layer.md` §6 反对的"改一改就能编"。
4. **S8 与 S9 分开，因为"能编 C++"与"能编 Qt"是两件事**（§3.4），
   而 S9（`moc`）是第一个能把这句话变成证据的东西。
   ★ 不要为了"看起来进度快"把 S9 并进 S10 ★ ——`moc` 失败时的症状
   （信号槽静默不通）与 Qt 编译失败的症状完全不同，混在一起会浪费大量时间。

## 5. 与当前工作的关系（为什么本文不动代码）

### 5.1 用户已决定：**先把 `exec` 做完，再开 Qt**

所以本文的定位是：**路线与缺口**。它不改内核、不改工具、不改 `user/`。
本轮的可见交付只有三份文档（`16` 新增，`10`/`11` 各改一处口径）。

### 5.2 `exec`（K6）与自举的关系：**它是自举的前置之一**

`15-exec.md` 已经为 `exec` 写了完整设计。这里只补"它为什么在自举路线上"：

| 编译器的行为 | 今天只有 `SPAWN` 时的后果 |
|---|---|
| `clang` 驱动 `main()` 读完参数后，**spawn 一个 `clang -cc1`**（或 `ld.lld`）去干活 | 用 `SPAWN` 也能做到——但那是**新任务**：新的句柄表、新的设备认领、新的 devfs 名字（`15-exec.md` 开头那段逐字列了这些） |
| 驱动要把 `-cc1` 的 stdout/stderr **原样接过来**（编译器诊断就是它的输出） | `SPAWN` 不继承 fd（`10-posix-layer.md` §9 边界表：`file_actions` 返回 `ENOSYS`，需要 K2 的"spawn 带初始 fd 映射"），于是驱动**看不到诊断**。★ 而"看不到诊断的编译器"在自举场景里等于没有 |
| 构建系统要"用另一个程序**替换**当前进程"（`make` 的 `exec` 语义、`configure` 的 `exec > log`） | 做不到。`15-exec.md` §1 第三条：`sh` 只能是"拉一个子进程然后等"的集合 |
| 编译一次要起 **几十到几百个** 进程 | 每个 `SPAWN` 都建新任务 + 新地址空间 + 新资源池身份；`13-tasks-and-kill.md` §5 记着"僵尸线程从不回收"是**修好之前一直在发生**的泄漏。`exec` 换掉的是同一份身份，**不产生新的外部引用** |

★ 一句话 ★ **`exec` 不是"Qt 需要的东西"，它是"任何编译器驱动都需要的东西"。**
把 `exec` 先做完，S7 才不会在"驱动怎么起 `cc1`"和"编译器本身对不对"
之间反复摇摆——这正是 `11-kernel-next.md` §4 把 K4 插到 D4 前面时用的同一条理由。

### 5.3 顺带确认：本文盘点出的两个"新"缺口**不在** `11-kernel-next.md` 的 K 清单里

| 缺口 | K 清单里有吗 | 它在 §4 的位置 |
|---|---|---|
| **"等一个用户地址"**（futex 语义的等待原语） | ★ **没有**。K 清单里最接近的是 K4 `wait_any`，但它等的是**内核对象**，不是用户地址（§2.3） | **S3 的硬前置**。建议在 `11-kernel-next.md` 的 K 列表里补一项（本文不代它编号，因为编号归那份文档） |
| **`FE_VMA_MAX = 32`** | ★ **没有**。它是 K6 那一轮的实现常量，不是一条待办 | **S4/S5/S10 会撞上**。建议记成"K6 的边界"而不是新 K 项 |

★ 这两条是本次盘点的**主要产出** ★ 它们都不是"Qt 带来的新需求"，
而是"Qt 这种规模的程序第一次让它们显形"——与 `11-kernel-next.md` §10 里
"K3 让'枚举任务'成立，于是僵尸线程泄漏第一次被看见"是同一族。

## 6. 明确不做 / 待查清单

★ 这一节的每条都写成**候选方案 + 代价**，不拍板 ★ 因为拍板需要实测，
而实测要等 `exec` 与 S1。

> ★ **libc++ 那一条链的待查项不在本节，在 `docs/17-libcxx-build.md` §6** ★
> 本节只保留本文自己盘点出的全局性条目。§6.5 那张表已经**逐条标注**了哪些条目
> 被 `17` 回答过、哪些仍然待查，并列出 `17` 新提出、本表没有的条目——
> **两张表要一起看**，否则"待查"会被当成"已经查完"。
> ★ `17` §6 **保持 15 个编号不重排**（`17` §6.2 写了理由：编号是**跨文档的标识符**），
> 到第二轮为止已完全答掉 2 条（第 3、12 条）、部分答掉 2 条（第 1、7 条）。

### 6.1 鸡生蛋：自举整个 `llvm` 需要先有一个 C++17 编译器

| 候选 | 怎么做 | 代价 | 今天能不能选 |
|---|---|---|---|
| **A. 宿主侧交叉编出"第一代" `clang`**，再把它拷进 FEKernel 用 | 用现在的 `toolchain/llvm` 交叉编 `clang`/`lld` 到 `x86_64-unknown-none-elf` | 需要先把 `libc++` 编到目标机（S4）。**它不违反"本机自举"的口径**——判据是"`qtbase` 在 FEKernel 上被编译"，不是"`clang` 在 FEKernel 上被编译" | ★ **推荐**。这是自举的标准做法（GCC 的"stage 0"、LLVM 的"host tools"） |
| **B. 一个更小的 C 编译器先自举，再用它编 `clang`** | 移植一个 C 子集编译器（chibicc/tcc 量级）先跑起来，用它编 `clang` | 要维护**两个**编译器；而 `clang` 的源码需要 C++17，那个小编译器**编不了 `clang`**，只能编"更小的 C 程序" | 不推荐（多一层且第一层不够用） |
| **C. 从头自研一个 C++ 编译器** | —— | 与项目铁律冲突（这是"再做一个项目"）；而且它**不解决任何自举问题**（仍然需要标准库） | 不做 |
| **D. 把 `clang` 裁剪成"不需要 C++17 也能编"** | 裁剪 `llvm`/`clang` | ★ **这不是选项**：那是把第三方源码改成"我们能编的样子"，正是 `10-posix-layer.md` §6 第一条判据（"源码不改"）禁止的 | 不做 |

★ 口径必须写死 ★ **"本机自举"指的是"在 FEKernel 上编译 `qtbase`"，
而不是"从零开始在 FEKernel 上长出 `clang`"。** 前者有明确判据（S10），
后者是一条无限长的路。**两者不能混为一谈，否则验收会永远做不完。**

### 6.2 `clang`/`llvm` 的裁剪清单（候选，待实测哪一项真的省得下来）

| 关掉 | 理由 | 风险 |
|---|---|---|
| 动态链接 / `-shared` / 插件 | 内核没有动态链接（§2.4） | 无（本来就是静态构建） |
| sanitizer（ASan/UBSan/TSan/MSan） | 需要大量运行时与 `mprotect` | 无（自举不需要） |
| OpenMP / libomp | 需要线程与大量运行时 | 无 |
| 多目标后端（只留 X86） | 体积与构建时间 | 无（只跑 x86_64） |
| `clang-repl` / JIT / `lli` | 需要 JIT 内存（W^X + `mprotect`，§2.3） | 无 |
| 静态分析器 / `clang-tidy` / clangd | 体积 | 无 |
| LTO / ThinLTO | 内存与链接时间（§3.1） | 低（自举不需要 LTO） |
| `-fPIC` 代码生成（保留 `-fno-pic` 路径） | 没有动态链接 | ★ 中：某些构建系统**硬要** `-fPIC`（`qtbase` 的 `configure` 会探测）。**待实测**：`-static` 下它是否放行 |

### 6.3 `qtbase` 的裁剪口径**必须定死**（否则"编出来了"无法验收）

待查清单（全部**待实测**，本文不给结论）：

1. `-static` 构建下 `QPluginLoader`/`QLibrary` 的编译期路径是否真的自洽；
2. `configure -no-feature-*` 的完整清单（network / printsupport / sql / dbus / icu / glib / xcb / opengl / sql / testlib …）；
3. ★ 裁剪的**边界**在哪：如果裁到 `QtCore` 只剩 `QString`，那"qtbase 能编出来"
   就成了一句空话。**建议的口径**：裁剪只允许发生在"**依赖 Linux 特有接口**"
   的模块上（socket/dbus/xcb/glib），**不允许**裁掉 `QtCore` 的
   `QObject`/`QString`/`QByteArray`/容器/事件循环/线程/`QFile`/`QJson`——
   判据是 S8 那段程序必须**一行不改**地跑对；
4. `qtbase` 的**版本**锁到 5.15 LTS 的哪个补丁版（影响 `configure` 的形态）；
5. 构建系统的形态：Qt5 用 `qmake` + 递归 `Makefile`，这是**大量 `fork`/`exec`**
   的形态。**待实测**：在 S6 的构建系统下它到底要什么。

### 6.4 明确不做（本轮与可预见的下一轮）

| 不做 | 理由 |
|---|---|
| 改任何内核/用户态/工具代码 | 用户已定"先做完 `exec`"（§5.1） |
| `third_party/` 的实际引入 | 许可豁免**已落地**（§2.6，2026-09-26），但引入本身要等到 S1 之后：今天没有任何东西需要 vendor 进来 |
| 动态链接 / `dlopen` | `10-posix-layer.md` §7 已定为独立项目 |
| 网络栈 / Unix socket 的**实现** | 同上（"没有 TCP/IP 栈"）；本轮只记录"`configure` 会探测它们" |
| `fork` 的实现 | `10-posix-layer.md` §4：**用需求驱动**。S6 会给出第一个真实需求（或证明 `posix_spawn` 够用） |
| 任何"预计需要多少内存/多少磁盘"的数字 | **待实测**（§3.1）。没有实测的写"待实测" |
| 桌面 / 图形（`xcb`/`wayland`/`opengl`） | 超出"编译 `qtbase`"这条要求；`QGuiApplication` 的平台插件需要真正的显示服务，那是另一个项目 |

### 6.5 待实测清单（汇总，方便下次开工时逐条消掉）

| # | 待实测 | 状态（★ 标注由 `docs/17-libcxx-build.md` 回答的部分 ★） | 什么时候能测 |
|---|---|---|---|
| 1 | 单个 `qtbase` 翻译单元的编译峰值内存（宿主侧先测量级） | **仍未回答**（`17` §5.4 的"今天就该做的准备"是同一条，它也只给了做法，**没有数字**） | **今天**（§3.1 级 0） |
| 2 | `make -j1` 与 `-jN` 的峰值内存差 | **仍未回答** | 级 0 之后 |
| 3 | QEMU/VBox 在本机的可用内存上限（512M 以上没测过） | **仍未回答**（要跑虚拟机，`17` 同样回避了） | 需跑虚拟机 |
| 4 | `qtbase` 源码树与构建中间产物的实际磁盘占用（§3.1 结尾） | **仍未回答** | 级 0 之后 |
| 5 | `FE_VMA_MAX=32` 在真实 C++ 程序上到底够不够 | ★ **已被 `17` 回答（按文件核实，仍未实测）**：`17` §4.1 独立复核 `kernel/include/fe/mm/vma.h:65` **行号与内容都相符**，并补一条本文没有的事实——`std::thread` 每起一个线程就吃掉一个 VMA 槽。★★ **怎么测在本轮被改过一次** ★：原先写的是"S1 的程序要同时起 4 个以上 `std::thread`"，但 `std::thread` 已移出 S1，所以现在 **S1 改成"静态地数段数"**（ELF 段 + 主栈 + 每个 arena chunk + 每个线程栈，写成表与 32 比），**"≥4 个线程"移交 §4 的 S3**（`17` §4.1/§5.1） | S1（静态数）/ S3（实测） |
| 6 | `-static` 下 `qtbase` 是否真的不需要 `dlopen` | **仍未回答**（`17` §6 第 6 条改成问 `LIBCXX_ENABLE_FILESYSTEM` 该开还是关，**不是同一条**） | S8 之前 |
| 7 | `time()` 恒定（无 RTC）对 `configure`/`make` 增量判断的影响 | ★ **已被 `17` 回答一半**：`17` §2.7 核实**没有 RTC**、`clock_gettime`/`time.h`/`sys/time.h` 全缺，`CLOCK_REALTIME` "缺，且不该假装有"；**没有 RTC 时"增量判断会怎么退化"仍未实测**。★ 本轮追加两条**同族**且已被 `17` 定死的事实：① `nanosleep` 缺，但 `usleep` 在（粒度 **1 ms**）；② 关掉 libc++ 的线程**就连带关掉单调时钟** `std::chrono::steady_clock`（`17` §7.1），而我们的 `fe_clock_ns()` **只有**单调时钟 ★ | S6 |
| 8 | `posix_spawn` 能否顶替 `fork` 跑通 `configure` | **仍未回答** | S6 |
| 9 | `configure` 探测失败时是"降级"还是"中止" | **仍未回答**（这是 Qt 构建系统的行为，`17` 不涉及） | S6 |
| 10 | 目标侧 `clang` 的体积与启动内存 | **仍未回答**（`17` §3.5 只补了一条**与内核无关**的宿主侧阻塞：PATH 上没有 CMake） | S7 |
| 11 | ★ **本轮新加（来自 `17` §3.7/§7.1）** ★ **宿主工具的可复现性**：CMake 与 Ninja **分别**从哪来、版本怎么钉死 | ★ **已被 `17` 定出推荐**：终局走 `toolchain/cmake/`（与便携 LLVM 同一套路），今天可以先用 `pip install cmake` 把 S1 走通；**残余待实测**：CMake 官方 zip **是否自带 Ninja**、`pip` 那条路在今天的 Python/pip 组合下**能不能装上**。★ **Ninja 与 CMake 是两个决定**，只钉一个就是半通状态 ★ | S1 开工前 |

★ **`docs/17` 新提出、而这张表里没有的条目**（不要在下次开工时漏掉）★
`17` §6 的清单是 **15 条**（编号没有重排——`17` §6.2 明确写了理由：编号是
**跨文档的标识符**）。到第二轮为止：**完全答掉 2 条**（第 3 条
`LIBCXX_ENABLE_THREADS` 是否存在、第 12 条 ABI 宏该不该由我们定义），
**部分答掉 / 已有推荐 2 条**（第 1 条 CMake 从哪来、第 7 条 `wchar.h` / 头计数），
**完全未动 11 条**（第 2、4、5、6、8、9、10、11、13、14、15 条）。
上面第 11 行是 `17` 第 1 条里**剩下的那一半**，所以它同时出现在两张表里。
**两份表要一起看**，否则"待查"会被当成"已经查完"。

★ 另外，`17` §6.1 (D) 记着一条**指错了节**的引用，本轮发现并在这里记下 ★
它写"本文与 `16` §2.2.1 的说法不一致：那一节写'已有的头（15 个……）'"——
**"15 个"那句原文在 `16` §2.2，不在 §2.2.1**（§2.2.1 从头到尾没写过 15，
它写的是"25 行 / 5 行判'有'"那一套按行数的口径）。
`16` 本轮已把"15 个"改成 **12 个**，**顺带说明为什么不能拿 15、12、13 互相加减**：
它们是三把不同的尺子（**文件数** vs **表格行数** vs **摊开的头名数**）。
★ 这条只影响"去哪儿找那句话"，不影响 `17` 的结论（12 个文件、没有 `wchar.h` 都是对的）★

## 7. 代价清单（一句话版）

- **一个新终点**：自举的验收物从"能编 FEKernel"变成"能编 `qtbase`"——
  FEKernel 是 freestanding 的，它证明不了 libc；`qtbase` 能。
- **一条新纪律的前提**：先有目标侧 compiler-rt builtins，才能不再靠
  "不许写 `__int128` 除法"这种口头纪律躲 `__udivti3`。
- **两条本文盘点出的新缺口**：**"等一个用户地址"的等待原语**（今天完全没有，
  且**不在 K 清单里**）与 **`FE_VMA_MAX = 32`**——都会在 S3/S4 之前咬人。
- **一层最重的库**：`libc++`/`libc++abi`/`libm` + 十几个缺失的 POSIX 头，
  没有任何一样是 Qt 特有的——**它们全部是"自举入场券"**。
- **一条今天几乎全是 `ENOSYS` 的前置链**：`fork`/`pipe`/`chdir`/`mkdir`/`rename`/
  动态链接/`/proc/self/exe` + 一个**不支持引号与重定向**的 shell。
  ★ 唯一的例外是 `O_APPEND`：它**不返回 `ENOSYS`，而是被静默忽略**（接受标志、
  从偏移 0 覆盖写；只有 `fopen("a")` 那条入口如实报错）——见 §2.4。
  原先这一行把 `O_APPEND` 与那串 `ENOSYS` 并列，是错的。★
- **一个今天就能动手、且与内核无关的前置**：`docs/17` 实测宿主 PATH 上**没有 CMake**
  （`clang`/`lld` 项目内有），而 `tools/fetch_toolchain.py` 只下载
  llvm/qemu/limine——**这是"宿主侧交叉编出 libc++ 静态库"那条路线的第一个阻塞点**，
  本文原先没有把它记成一条（`17` §3.5 补上了）。
  ★ 第二轮追加两条，都是"同一件事的另一半" ★
  1. **`ninja` 在 PATH 上同样是"运气"，不是设计**——它是随 Python 环境装进来的
     （`…\Python310\Scripts\ninja.exe`）。走"把 CMake 放进 `toolchain/`"这条路时，
     **Ninja 从哪来必须一起决定**，否则是"CMake 钉住了、Ninja 没钉住"的**半通状态**
     （`17` §3.7.3；`tools/toolchain.py:147-148` 已经为 QEMU 记过同一类故障的形状）。
  2. ★ **与"工具链可复现"直接相关的一条** ★ `tools/fetch_toolchain.py` 的校验方式是
     **字节数，不是摘要**：跳过判据是 `dest.stat().st_size == art.expect_bytes`
     （`:104`），下载后也只比大小（`:123-125`），而 **全文件搜
     `hash`/`sha`/`md5`/`digest` → 0 处命中**（`17` §3.7.0 (1) 核实）。
     也就是说：**换源续传能挡住"下载不完整"，挡不住"内容被换过"**。
     往这份清单里加第四项（CMake）之前必须先知道这一点——它决定"加一项"是
     照着现有形态加，还是**先把摘要校验补上**。
- **`moc`/`rcc` 的独立阶段**：能编 C++ ≠ 能编 Qt；这一条必须单独验收。
- **两个待实测的物理边界**：内存（462 MiB 是 512M 配置下的实测值，
  编 `qtbase` 要多少**不知道**）与磁盘（今天的镜像按 MiB 计，`qtbase` 按 GiB 计）。
- **本文一行代码都不动**：用户已定"先做完 `exec`"，而 `exec` 是编译器驱动的
  前置之一（驱动要 spawn `cc1`，还要能看到它的诊断）。
