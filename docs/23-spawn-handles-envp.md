<!-- SPDX-License-Identifier: 0BSD -->
# spawn/exec 的初始句柄组与 `envp`（K13）

> 本文把 `20-posix-kernel-requirements.md` §3.4 里 K13 那一行
> （"**spawn/exec 的初始句柄组 + `envp`**（`file_actions`/重定向/环境变量传子进程）"）
> 变成一份可评审、可实现的设计。
>
> ★ 口径（照 `20` §1.2，本文一条都不放宽）★
> **支持 POSIX 标准 ≠ 向 Linux 靠拢**：依据是公开标准（IEEE 1003 / ISO 9945）
> 的规范条文，不是某个实现的行为。**内核里不出现 POSIX 的名字**——
> 内核不认识 `fd`、`dup2`、`FD_CLOEXEC`、`environ`；它只知道
> **句柄**（能力）与**初始栈上的一块字节**。
>
> ★ 写作范围 ★ **本文一行代码都不改、不跑构建、不跑虚拟机。**
> 每一条事实性陈述都对应一次真实的代码阅读，引用写成 `文件:行号`；
> 没读到的一律写"待查"（§8）并写明怎么查。
> 写作时另有执行体在改 `kernel/`、`user/`、`tools/`，所以行号取自**写作当时**，
> 以**函数名 + 代码片段**为准。

---

## 1. 判据与今天的缺口

### 1.1 K13 是两件事，不是一个

| # | 要什么 | 谁做不到 | 标准出处（接口形状） |
|---|---|---|---|
| **(i)** | **初始句柄组**：新任务在**跑第一条用户指令之前**就持有父进程指定的一组句柄 | 用户态**单独**做不到（见 §1.4） | `posix_spawn_file_actions_adddup2/addclose/addopen`（`<spawn.h>`） |
| **(ii)** | **`envp`**：把一组环境字符串放进新映像的初始栈 | 用户态做不到（初始栈是内核摆的） | `execve(path, argv, envp)` 的**第三个参数** |

两件事在实现上**共用同一条路**（都要经过 `fe_image_prepare` 那段"构造初始栈 +
装载映像"的准备阶段），但**判据与代价完全不同**，所以本文分开写、分刀做（§6）。

### 1.2 标准要求什么（按 `20` §2.0 的三分法判定）

★ 三分法（`20:230-241`）：**标准要求** → 做；**不是标准（某个实现的附加行为）**
→ 不做并明写；**我们的实现形态 / 实现限制** → 做了也不算"像 Linux"，
但要写清代价。

#### 1.2.1 `posix_spawn` 的 `file_actions` —— **标准要求**

标准（`<spawn.h>`）规定三个 `add*`，它们的**语义**是"**在子进程里**做一次
描述符操作，而且这些操作在**子进程执行任何用户代码之前**全部完成"：

| 标准接口 | 标准语义（可观察的那一半） | 落到内核的中性动作（不出现 POSIX 名字） |
|---|---|---|
| `posix_spawn_file_actions_adddup2(fa, fd, newfd)` | 子进程里 `newfd` 变成 `fd` 的**副本**（共享同一个打开文件描述） | **把句柄 H 装进子任务的句柄表**（占用槽位 `newfd`），H 的权限按"只能收窄"从父进程那份算 |
| `posix_spawn_file_actions_addclose(fa, fd)` | 子进程里 `fd` 被关闭（**即使父进程还开着**） | 子任务的槽位 `fd` **保持空**（今天的默认行为，见下一条） |
| `posix_spawn_file_actions_addopen(fa, fd, path, oflag, mode)` | 子进程里 `open(path,…)` 的结果落在 `fd` 上，**且发生在任何用户代码之前** | 内核**不做这件事**：文件是**用户态服务**（`fsd`）提供的（`posix.c:5-7` 把这条分层写死了）。正确形态是"**父进程先打开**，拿到端点句柄后**随初始句柄组装进去**"（§2.6） |

★ 一条**可能反直觉**但标准明确的事实 ★ 上面三条的语义是
"**子进程的哪些 fd 号上有什么**"，而标准**没有**规定"没有出现在
`file_actions` 里的 fd 该怎样"。`POSIX_SPAWN_CLOEXEC_DEFAULT` 之类的扩展
（那是各实现加的，见 §4）会影响它。所以本文的默认行为必须**显式选定**：
见 §1.3 第 (2) 条与 §2.1 的 `FE_INHERIT_OP_KEEP`。

#### 1.2.2 `execve` 的 `envp` —— **标准要求**

`execve` 的第三个参数就是环境（`char *const envp[]`，以 NULL 结尾）。
标准要求的是**可观察语义**：新映像里的 `getenv("K")` 能看到 `K=V`，
且 `environ` 反映这个数组。**内核用什么形状把它交过去，是实现自由**
（`20:1047-1049` 已经就此写过一句：`environ` 由 `_start` 从初始栈装是
**我们的做法**）。

### 1.3 今天各自断在哪一步

#### (1) 初始句柄组：**子任务的句柄表是空的，而且没有任何路径能往里放东西**

| 事实 | 出处 |
|---|---|
| `fe_process_spawn(parent, path, argv, argc, out_handle)` —— **签名里没有任何句柄数组** | `kernel/include/fe/process.h:127-128` |
| spawn 的实现只做两件事：`fe_process_create(...)` + 把**子任务句柄**装进**父进程**的句柄表 | `kernel/task/process.c:344-372`（安装那一段在 `:362-365`） |
| 那个句柄装给的是**父进程**（`&parent->handles`），不是子进程 | 同上 `:362` |
| 子任务的句柄表由 `fe_task_alloc` 里 `fe_handle_table_init` 初始化，**从此到子进程跑起来之间没有任何一处往里装东西** | `kernel/task/task.c:125`（`fe_task_alloc` 里那一句）；`fe_process_create` 全文 `process.c:240-342` 没有第二处 `fe_handle_install` |
| 内核侧 `fe_handle_install` 收的是**目标表指针**（`struct fe_handle_table *t`），所以"往别人表里装"在内核里是**天然可行**的 | `kernel/object/object.c:61-80`；表大小与"0 恒为无效句柄"在 `:68` |
| 用户态 fd 表是**进程内静态数组**，且 **0/1/2 是懒初始化的**：第一次用 fd 时才 `fe_devfs_open("/dev/console")` | `user/libposix/posix.c:75`（`static struct fd_entry g_fd[POSIX_FD_MAX]`，`POSIX_FD_MAX 64` 在 `:55`）、`:114-133`（`fd_ensure_init`） |
| `file_actions` 的三个 `add*` **明确返回 `ENOSYS`**；`posix_spawn` 收到非 NULL 的 `file_actions` 就返回 `ENOSYS` | `user/libposix/spawn.c:81-101`、`:122-125` |
| 理由写在用户态注释里："内核的 `fe_process_spawn` 不认识 fd，而用户态的 fd 表**不跨任务继承**（子进程是全新的地址空间与句柄表）" | `user/libposix/spawn.c:72-80` |

→ **断点是这样一条链**：`posix_spawn` 收到非 NULL 的 `file_actions` → `ENOSYS`
（`spawn.c:122-125`）→ 即使把这一句去掉，**内核也没有参数可以接收**那些句柄
（`process.h:203-204`）→ 于是子进程只可能是一张空句柄表（`task.c:111`）。
**三处各自都"诚实"，但合起来就是"重定向做不到"。**

★ 一条容易被忽略的相邻事实（它决定了"空句柄表"到底有多空）★
子进程虽然**没有任何句柄**，但它的 libposix 会在**第一次用到 fd 时**
自己去 `fe_devfs_open("/dev/console")`（`posix.c:124`）。也就是说
"子进程的 stdout 能出字"**不是继承来的**，是它自己打开控制台得到的。
这一条对 §2.5 的设计有直接影响：**继承 0/1/2 会与这段懒初始化撞车**。

#### (2) `envp`：**不是"环境为空"，而是"环境无法跨 spawn/exec 传递"**

| 事实 | 出处 |
|---|---|
| 内核在初始栈上压的是**空环境的终止符**：`push8(as, &sp, 0); /* envp 终止符（环境目前为空） */` | `kernel/task/process.c:109`（布局说明在 `:57-62`） |
| 指针区对齐推导按 `n + 3` 个字算（`n` = argv 指针个数；`+3` = 两个 NULL + argc） | `kernel/task/process.c:104-108` |
| **但环境变量在用户态是齐的**：`environ` + `__posix_set_envp` | `user/libposix/posix.c:40-49` |
| `_start` **已经**在算 `envp` 并交给 libposix：`lea rdx, [rsi + rdi*8 + 8]` | `user/libfe/start.asm:34`、`:37-38` |
| `getenv` 有实现（`setenv` 也在，但今天**只在本进程内生效**） | `user/libposix/stdlib.c:217`（`getenv`）、`:235-238`（`setenv`，注释里那句"今天只在**本进程内**生效"正是 K13 要改的事） |
| `posix_spawn` **显式忽略** `envp`：`(void)envp; /* 环境变量还不能传给子进程（见 K1） */` | `user/libposix/spawn.c:121` |
| 内核侧的上限常量：`FE_ARGV_MAX 16`、**`FE_ENVP_MAX 8`**、`FE_ARG_MAX 128` | `kernel/include/fe/process.h:40-42` |

★ **`FE_ENVP_MAX` 今天的真实状态：定义了，但内核里一个引用都没有** ★
我在 `kernel/` 全目录 grep 三个常量，命中如下（这是"有没有真在用"的直接证据）：

| 常量 | `kernel/` 里的引用 | 结论 |
|---|---|---|
| `FE_ARGV_MAX` | `syscall.c:1074/1079/1130/1131/1171/1172`（`copy_user_argv` 与两个 syscall 的栈缓冲）、`process.c:69/73/492/1030/1031/1034` | **在真用**：既限个数（`> FE_ARGV_MAX → FE_ERR_RANGE`），也定缓冲大小 |
| `FE_ARG_MAX` | `syscall.c:1130/1171`（`char arena[FE_ARGV_MAX * FE_ARG_MAX]`）、`process.c:79/92`（单条字符串长度上限） | **在真用** |
| **`FE_ENVP_MAX`** | **0 处** | ★ **只被定义，从未被任何代码读过** ★（`grep FE_ENVP_MAX kernel/` 只有 `process.h:41` 那一行定义本身） |

→ 所以"复用既有常量"这句话今天只对了一半：**`FE_ENVP_MAX` 是个还没兑现的承诺**，
K13 是它的第一个消费者。这不是缺口，但**要在实现清单里显式认领它**
（否则它会像 `18-user-fault-handler.md` §6.1.1 那对计数器一样，
"文档里写着、代码里没人用"）。

★ 精确的表述（本文采用这一句）★
**不是"环境变量为空"，而是"环境变量无法跨 `spawn`/`exec` 传递"。**
一个进程内部的 `getenv`/`setenv` 完全可用（`environ` 由 `_start` 从栈上装，
`posix.c:46-49`），只是那个栈上的 `envp` **永远是 `{NULL}`**——
因为**没有任何路径能往里放东西**。

#### (3) `exec` 这一侧的缺口与 spawn 不同：**句柄天然保留，环境仍然传不进去**

| 事实 | 出处 |
|---|---|
| `FE_SYS_EXEC = 0x90`，参数 `(path, argv, argc)` —— **没有第三个参数给环境** | `kernel/include/fe/syscall.h:340-353` |
| exec 换的是**地址空间与映像**；句柄表 / 设备认领 / devfs 名字 / 任务 id / 父子关系**一个字不动** | `kernel/include/fe/syscall.h:342-343`；`kernel/task/process.c:475-476`（`fe_exec` 只拿 `path/argv/argc`） |
| 用户态包装同样是三个参数 | `user/include/fe_user.h:705`（声明 `fe_exec(path, argv, argc)`）、`user/libfe/libfe.c:306`（实现）、`user/include/fe_user.h:116`（`FE_SYS_EXEC 0x90`） |

→ 于是 **`exec` 这条路上"fd 继承"本来就成立**（父进程不会因为 exec 丢掉句柄），
缺的只有环境。这与 spawn 那条路**方向相反**，见 §3。

### 1.4 为什么"初始句柄组"必须由内核做

判据是 `20:217` 那一条：**用户态能不能单独做到（不依赖"内核恰好没提供的那件事"）**。

- **候选 (b)（子进程收 IPC 后再装）今天"几乎"能做**：句柄传递机制是齐的——
  发送方要有 `FE_RIGHT_TRANSFER`（`kernel/ipc/ipc.c:194-197`），
  接收方在 `fe_endpoint_recv` 里把句柄装进**自己的**句柄表，
  权限按"发送方权限 **减掉** TRANSFER"算（`ipc.c:444-453`，尤其是 `:448`）。
- **但它做不到标准的时序要求**：标准要求"在子进程执行任何用户代码之前"完成重定向，
  而 IPC 方案里**子进程必须先跑用户代码**（去 `recv`）。
  这不是"慢一点"，是**语义不同**：在那段窗口里子进程的 fd 表是它自己的初始状态，
  它可能已经 `open` 了别的东西占掉了号（`posix.c:154-164` 的 `fd_alloc`
  复用**最小空闲号**，所以"占掉"是必然会发生的）。

→ **归属：内核（机制）+ 用户态（语义）**，与 `20:346-347` 的判定一致。

---

## 2. 设计（候选 + 取舍）

### 2.0 两条候选

| 候选 | 形状 | 谁提供句柄 | "在任何用户代码之前"能保证吗 |
|---|---|---|---|
| **(a) spawn 带一组"句柄 → 目标槽位"** ★ 本文推荐 ★ | `FE_SYS_PROCESS_SPAWN_H`：父进程给一组 `(handle, number, op)`，内核在**线程被创建之前**装进子任务的句柄表 | **父进程自己的句柄表** | ✅ 线程在安装**之后**才 `rq_push`（见 §2.2） |
| **(b) 子进程先跑 stub，父进程经 IPC 传句柄** | 子进程入口先指向一段 stub：它 `recv` 到句柄组 → 装进自己的 fd 表 → 再 `exec`/跳到真入口 | 父进程（经端点） | ❌ **做不到**（stub 本身就是用户代码）；只能做到"尽早" |

**推荐 (a)**，理由**不是性能**，而是候选 (b) 有一个无法回避的语义差：
标准要求的是"**之前**"，(b) 只能给"**尽早**"（`20:336-338` 已经就这一点下过
同样的结论，本文复核了它引用的代码事实，见 §2.7）。

★ 候选 (b) 的第二条代价（我自己读出来的，`20` 没提）★
(b) 若想用"子进程先挂起"来补时序，需要 `FE_THREAD_FLAG_SUSPENDED`——
而这个标志**在用户态头文件里根本不存在**（`user/include/fe_user.h` 只有
`FE_THREAD_FLAG_MAIN 1u`，`:374`），内核侧的 `FE_THREAD_FLAG_SUSPENDED`
（`kernel/include/fe/syscall.h:29`）**也没有任何代码读它**：
`fe_user_thread_create` 第一句就是 `(void)flags;`（`kernel/task/user.c:530-532`）。
也就是说 **(b)+挂起 = 还要先在 K12 那条路上把"线程挂起"做出来**，
而"挂起"会引入一整套新状态（被挂起的线程在闸门/取消点上算什么）。
**这是 (b) 被否掉的最硬一条。**

### 2.1 候选 (a) 的机制：内核只认三件事——装、留空、复制

★ 内核不认 fd，只认"**槽位号**"（一个 `u32`）。它做的动作只有三种，
名字都是中性的：

```c
/* 内核侧的一个条目（用户态 ABI 见 §2.4）。三个动作，没有第四个。 */
#define FE_INHERIT_OP_KEEP   0u   /* 槽位 number 保持**空**（父进程开着也照样空）*/
#define FE_INHERIT_OP_INSTALL 1u  /* 把 handle 装进槽位 number */
#define FE_INHERIT_OP_COPY   2u   /* 把槽位 src 的句柄复制一份装进槽位 number */
```

三个动作到标准的映射（**映射表在用户态那一层**，内核里不出现 `dup2` 这个名字）：

| 标准动作 | 用户态怎么翻译成内核动作 |
|---|---|
| `posix_spawn_file_actions_adddup2(fa, fd, newfd)` | 用户态查自己的 fd 表拿到 `fd` 的**句柄** → `INSTALL(handle=H, number=newfd)` |
| `posix_spawn_file_actions_addclose(fa, fd)` | `KEEP(number=fd)`（默认就是空，显式写出来是为了**让意图可读**：见 §1.2.1 那条"标准没规定默认"） |
| `posix_spawn_file_actions_addopen(fa, fd, path, …)` | 用户态**先打开**（经 fsd 拿到端点句柄）→ `INSTALL(handle=H, number=fd)` |

★ `COPY` 存在的理由 ★ 用户态要"把 0 号槽复制到 3 号"时，
它手里可能**只有一个 fd 号**（没有裸句柄）；有了 COPY，
它不必为了拿到句柄而先做一次 `HANDLE_DUP`。**代价**：内核多一个分支、
自检多一条判据。**它也可以不做**（第一刀里可以先只做 `INSTALL`/`KEEP`，见 §6），
这一条在 §8 留了一个待查项。

### 2.2 ★ 时序：安装必须在"线程存在之前"，而这一点今天是可证的 ★

这是整个设计的**承重点**，而它有一条已经写在代码里的先例：

| 事实 | 出处 |
|---|---|
| 新线程在 `fe_thread_create_ring3` 里被 `rq_push` 挂上就绪队列，**在 `fe_process_create` 返回之前就已经可运行** | `kernel/task/process.c:274-286` 那段注释（"新线程在这个函数返回之前就已经可运行了"）；线程创建调用在 `:288-289` |
| 因此 `boot_init` 必须在**线程创建之前**记下（`if (boot_init) fe_resource_note_init(t->id);` 在 `:284-286`），否则 VirtualBox 上每次复现一个竞态 | 同上；详细实测见 `12-drivers.md` §9.3（`process.c:278-283` 复述了症状） |

→ **结论：初始句柄组的安装点与 `fe_resource_note_init` 是同一个位置**——
"接下来要跑的就是它、而它还没开始跑"的那个时刻。
**这不是"顺手放这里"，而是唯一安全的位置**；放到 `fe_process_spawn` 返回之后
就是**同一个竞态**（`20:1201-1203` 那条"机制层的不变式必须由机制自己维持"
要求的正是"在 `fe_process_create` 那条路上就能被自检调到"）。

**落点（实现清单的形状，不是最终代码）**：

```c
/* kernel/task/process.c：fe_process_create 多两个参数（与 boot_init 同一族） */
struct fe_task *fe_process_create(const char *path, char *const argv[], u32 argc,
                                  const struct fe_inherit_item_x *inherit, u32 n_inherit,
                                  fe_status_t *out_status, struct fe_thread **out_thread,
                                  bool boot_init);
/*                       ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ 新增这一段
 * 结构体名带 `_x`：内核侧那一份（与 `struct fe_task_info_x` 同一套命名，
 * kernel/include/fe/syscall.h:475；用户侧那一份**不带** `_x`，
 * user/include/fe_user.h:337 —— 这是项目已有的镜像约定，照抄它）。
 * 安装点：在 `if (boot_init) fe_resource_note_init(...)`（process.c:284-286）之后、
 *         `fe_thread_create_ring3(...)`（process.c:288）**之前**。 */
```

**失败必须原子**：任一 entry 失败（坏句柄 / 缺 `FE_RIGHT_TRANSFER` /
目标槽位非法 / 槽位冲突 / 只剩空表的句柄表满）→ **返回错误，子任务销毁**，
一个句柄都不留在半成品任务上。今天 `fe_process_create` 的失败路径只有一条
（`fail_task:` → `fe_object_unref(&t->hdr)`，`process.c:330-336`），
所以"销毁子任务"这件事**已经有现成的出口**，代价是安装循环要能**回滚已经装进去的**。

★ 回滚为什么不能靠"反正任务要销毁" ★
任务销毁会 `fe_handle_table_clear`（`kernel/task/task.c:58`）把引用放掉，
所以**确实**不需要手工关闭——但**只有当"失败 ⇒ 一定走到 `fail_task`"成立时**。
这一条要写进实现清单：**安装循环里禁止 `return` 一个成功**，
所有失败都必须 `goto fail_task`（否则就是"装了一半的子任务跑起来"）。

### 2.3 能力面：谁能把句柄塞给谁（安全）

| 问题 | 答案（本文的选择） | 依据 / 代价 |
|---|---|---|
| 句柄从哪来 | **父进程自己的句柄表**（`struct fe_task *parent` 的那个） | 与句柄传递同一套能力模型：`fe_handle_lookup(&t->handles, handles[i], FE_RIGHT_TRANSFER, &obj)`（`kernel/ipc/ipc.c:196-197`） |
| 凭什么能转交 | **必须有 `FE_RIGHT_TRANSFER`**，缺它返回 `FE_ERR_ACCESS`**（不是 `BADHANDLE`）** | `ipc.c:196-201`；`docs/09-handle-transfer.md:75` 把这条错误码的区分写死了——"这条错误码的区别是判据的一部分" |
| 子进程拿到的权限是多少 | **父进程那份权限减掉 `TRANSFER`** | 与 IPC 那条路径**逐字相同**：`u32 rights = m->transferred_rights[i] & ~FE_RIGHT_TRANSFER;`（`ipc.c:448`）。★ 不减的话，一条能力会像病毒一样扩散（`ipc.c:437-439` 的原文）★ |
| 谁能把句柄塞给谁 | **只有"拉起者能给它拉起的东西"** | 这是项目已有的形状：`fe_process_spawn` 把 `TERMINATE` 随子任务句柄给父进程（`process.c:358-365`），而 spawn 的调用者**就是**那个父进程。**没有"任意两个任务之间塞句柄"的新面** |
| 子进程能用它做什么 | 与父进程那份**同一组能力**，只是少了转发权 | 能力模型的核心约束（只能收窄） |

★ **一条必须写下来的禁止**（否则它会被"顺手加上"）★
**不要把父进程的"子任务句柄"当成初始句柄塞进子进程。**
理由：那个句柄带 `FE_RIGHT_READ | FE_RIGHT_WAIT | FE_RIGHT_DUP |
FE_RIGHT_TRANSFER | FE_RIGHT_TERMINATE`（`process.c:362-365`），
子进程拿到它之后**能终止自己**（`TERMINATE`），也能 `WAIT` 自己
（`fe_process_wait` 的实现只看"句柄指向的是不是任务"，`process.c:380-388`）。
本文不新增任何"禁止"代码——**它是使用契约**：父进程不把它放进初始句柄组。
代价如实说：**内核不会拦这件事**（拦它需要一条"这个句柄指向调用者自己"的判断，
而它今天不存在）。什么时候会咬人：一个把 `fe_spawn` 的返回句柄无脑塞进
inherited 数组的测试程序，会得到一个能自杀的子进程——而那**看起来**像内核 bug。

### 2.4 ABI 草案（内核按偏移填、两边各钉一次）

```c
/* ---- 共享 ABI：初始句柄组（K13）----
 * ★ 与 TASK_LIST / MSI_INFO 同一套做法 ★ 契约用**数字**写，
 * 一致性用 _Static_assert 守（docs/13-tasks-and-kill.md §2 那次的教训：
 * 两个头文件各写一遍步长宏、两处都写成 48、实际 64，而没有任何东西会报错）。 */
#define FE_INHERIT_MAX      8u      /* 一次 spawn 最多带几个条目 */
#define FE_INHERIT_STRIDE   24u     /* struct fe_inherit_item_x 的字节数 */
#define FE_INHERIT_STRIDE_X 24u     /* 用户态镜像（同一数值，两边各写一次） */

struct fe_inherit_item_x {
    u64 handle;     /* 父进程句柄表里的句柄；COPY 时是源槽位里的句柄 */
    u32 number;     /* 目标槽位号（内核只当它是个下标，不解释它） */
    u32 op;         /* FE_INHERIT_OP_* */
    u64 _pad;       /* 显式补齐到 24：**内核按偏移填，不靠编译器填充** */
};
_Static_assert(sizeof(struct fe_inherit_item_x) == FE_INHERIT_STRIDE,
               "FE_INHERIT_STRIDE 与 struct fe_inherit_item_x 不一致");
```

★ 两条与 `TASK_LIST` 共享同一条教训的注意 ★
（依据：`kernel/include/fe/syscall.h:343-382` 那段"步长曾经是错的"的注释）

1. **`_pad` 是显式的**，不是让编译器填：内核按**偏移常量**填表，
   两边编译器的填充差异会变成静默错位（`syscall.h:329-332` 原文）。
2. **`number` 是用户态的事实**（"0 号是标准输入"），内核**不解释**它——
   它只用来当"装到哪个槽位"（`fe_handle_install` 今天**不接受**指定槽位，
   它总是找第一个空槽，`object.c:68-77`；所以本项需要一个新的机制函数
   `fe_handle_install_at(table, slot, obj, rights)`——见 §5 的实现清单）。

### 2.5 ★ 与用户态懒初始化的撞车（我自己读出来的一条，`20` 没提）★

**今天的 `fd_ensure_init`（`user/libposix/posix.c:114-133`）会无条件把 0/1/2
覆盖成它自己打开的 `/dev/console`：**

```c
for (int i = 0; i < 3; i++) {
    g_fd[i].kind = FD_CONSOLE;
    g_fd[i].handle = console;      /* posix.c:124-130 */
```

→ 一旦子进程**继承了** 0/1/2（例如 shell 把 `prog > out.txt` 落成
"把文件端点装到 1 号"），而它的 libposix 从来没被初始化过，
那么**第一次用到 fd 时这段代码会把继承来的 1 号覆盖掉**，
而 `handle` 指向的那个继承句柄**再也回不来了**（`fd_entry` 里没有别的地方存它）
——**重定向静默失效**，输出又回到控制台。

**修法（落在用户态，不在内核）**：`fd_ensure_init` 的 0/1/2 那段加一条
"**已经有条目的号不碰**"：

```c
for (int i = 0; i < 3; i++) {
    if (g_fd[i].kind != FD_UNUSED) { continue; }   /* 继承来的优先 */
    ...
}
```

★ 为什么这条必须写进本文而不是留给实现 ★ 它与 `20:409-411` 记的
`dup` 偏移不共享是**同一族**：都属于"用户态的一处实现与它自己的注释/标准不符"。
区别是这一条**还没有发生**（它要等 K13 落地才会显形），所以**现在写下来最便宜**。
代价：`posix.c` 里 3 行；**不写它的症状是"重定向在第一次用到 fd 之前有效，
之后失效"**——一种时序相关的、极难复现的错。

### 2.6 `addopen` 的归属：内核**不做**文件打开

`file_actions` 的第三个动作是 `addopen`。内核**不能**做它：
文件是用户态服务（`fsd`）通过端点提供的（`user/libposix/posix.c:5-7` 把这条
分层写死了；`20:209-210` 记着内核连 `open` 这个名字都不该认识）。

**正确形态**（一行内核都不加）：

```
父进程: open(path, ...) → 拿到 fsd 端点句柄 H（或者直接拿到 /dev/xxx 的句柄）
      → 把 H 作为 inherited 数组里的一个 INSTALL(handle=H, number=fd) 传下去
子进程: 从 fd 号 fd 上直接读写（它**从不调用 open**）
```

**这顺带解决了一个语义问题**：标准里 `addopen` 的 `oflag` 与 `mode`
是**子进程**的权限语义，而"父进程先打开"会让子进程拿到的是**父进程当时的权限**。
★ 代价如实说 ★ 两者在标准下的可观察差别只在"父进程与子进程的权限不同"
这种场景（例如 `O_CLOEXEC` + 不同 uid 的场景）；**本项目今天没有 uid**
（`20` 没有任何 uid/权限模型的条目），所以这个差别**今天不可观察**。
什么时候会咬人：有了凭据模型之后，`addopen` 必须回到"**子进程执行、父进程提供
路径**"的形状（那需要一个"在子进程里先跑一段内核代做的动作"的原语，
今天没有，见 §4）。

### 2.7 与候选 (b) 的正面比较（把 `20` 的判断复核一遍）

| 维度 | (a) spawn 带句柄组 | (b) 子进程 stub + IPC |
|---|---|---|
| 时序 | ✅ 线程创建之前装好（§2.2 有先例与落点） | ❌ 子进程已经跑过用户代码 |
| 内核改动 | 中：一个新 syscall、一个新机制函数（按槽位装）、`fe_process_create` 多两个参数 | 小：**内核可以一行不改**（今天就能做） |
| 需要的前置 | 无 | ❌ **需要"线程挂起"**，而它今天不存在（`user.c:530-532` 的 `(void)flags;`；`FE_THREAD_FLAG_SUSPENDED` 在用户态头里没有） |
| fd 号归谁 | 用户态（`number` 只是一个下标） | 用户态（完全在 stub 里） |
| 失败原子性 | ✅ 与 spawn 同一把伞（§2.2） | ❌ 句柄可能已经发过去了、子进程可能已经装了 |
| 安全面 | 只新增"拉起者给它拉起的东西"这一条（已有） | 同样只需要 TRANSFER；但要**多开一条端点**并约定协议 |

★ 结论 ★ **(a) 的代价是内核多一个 syscall；(b) 的代价是"标准语义做不到"
加上"要先做线程挂起"。标准要求的是可观察语义，"尽早"不等于"之前"。**

---

## 3. `envp` 的形态

### 3.1 内核只做一件事：**把字符串数组写进初始栈**（与 argv 同一套）

今天的 `build_initial_stack`（`kernel/task/process.c:66-129`）已经在做这件事，
只是第二段永远是空表。要加的东西**形状上只有三处**：

| 要改的地方 | 今天 | 改成 |
|---|---|---|
| 字符串拷贝循环 | 只遍历 `argv`（`:76-88`） | 遍历 `argv` **与** `envp`，两段共用同一个"写字符串、记下用户态地址"的动作 |
| **写入时机** | `argv` 字符串写在 `FE_USER_STACK_TOP` 往下、**并且在 `sp &= ~0xF` 之前**（`:72-103`） | ★ 环境字符串必须写在**同一个区间里、同样在 `&= ~0xF` 之前** ★——写在对齐之后就会**压掉刚摆好的指针区**（见下面那段） |
| 指针区 | `push8(0)` 当 envp 终止符（`:109`） | 先压 `envp` 的字符串指针（逆序），再压那个 NULL |
| **对齐推导** | `n + 3` 为偶数即可，即 `n` 为奇数（`:103-108`） | ★ **`n + 3` 变成 `argc + envc + 3`** ★ 判据跟着变成"`argc + envc` 为奇数" |

★ **本节的第一个坑：写入时机**（我读 `process.c:72-108` 的顺序读出来的）★
今天的顺序是：`sp = FE_USER_STACK_TOP`（`:72`）→ 逐条 `sp -= len; fe_user_write_space(...)`
写 argv 字符串（`:76-88`）→ 最后 `sp &= ~0xFull`（`:103`）→ 才开始 `push8` 指针区。
★ 所以**字符串区与指针区是靠"对齐那一句"分开的** ★：字符串在 `&= ~0xF` **之上**，
指针区在它**之下**。环境字符串如果写在对齐**之后**，`sp` 已经低于指针区，
后写的字符串会**覆盖刚压进去的 argv 指针**——症状是"程序启动后 argv 里是乱码"，
而它**只在传了环境时出现**（不传环境时这段代码根本走不到）。
正确做法：**环境字符串紧接在 argv 字符串之后、在 `&= ~0xF` 之前**写
（顺序可以两段分开，但都必须在那一刻之前）。

★ **本节的第二个坑：对齐推导**（这一处才会"看起来在工作"）★
`process.c:104-108` 那段推导本身是对的，但它**写死了 `n` 的语义**
（= 指针个数）。加了环境之后总指针数 = `argc + envc`，而**字符串区的对齐要求没变**。
改错的症状是"**某些程序在某些参数个数下 `#GP`**"——`20:1031` 已经就此提过一次，
而同一族的一次真实事故写在 `kernel/task/user.c`：线程入口栈少减 8 字节
（`stack = top - 24` 与 `(stack & 0xF) != 8` 的判据，`user.c:519-536`），
注释里那句"**潜伏了一年的 ABI 错误，被'打开 SIMD'这件事照出来**"就是它。

### 3.2 上限：复用既有常量，并**认领** `FE_ENVP_MAX`

| 项 | 取值 | 出处 / 理由 |
|---|---|---|
| 环境条目数上限 | **`FE_ENVP_MAX` = 8** | `kernel/include/fe/process.h:41`——★ 今天 **0 引用**（§1.3(2) 的 grep 证据），K13 是第一个消费者 ★ |
| 单条字符串长度上限 | **`FE_ARG_MAX` = 128** | `process.h:42`；单条上限的检查方式照 `argv`（`process.c:78-81`：`len > FE_ARG_MAX → FE_ERR_NAMETOOLONG`） |
| 内核侧环境缓冲 | **`FE_ENVP_MAX * FE_ARG_MAX` = 1024 字节**（栈上） | 与 argv 那条**同一个乘法**：`char arena[FE_ARGV_MAX * FE_ARG_MAX];`（`syscall.c:1130`、`:1171`）。★ 取值是否照抄 argv 见 §8 待查第 1 条 ★ |
| 超限的行为 | **`FE_ERR_RANGE`（条数）/ `FE_ERR_NAMETOOLONG`（单条），绝不截断** | `process.h:37-39`："超限返回 `FE_ERR_RANGE` 而不是截断——静默截断会让人以为是程序自己的 bug" |

### 3.3 ★ 一条判定：内核**要不要**检查 `NAME=VALUE` 的形状 ★

（按 `20` §2.0 的三分法：这一条**不是标准要求**，是**我们的实现形态**，
所以要写清代价。）

**标准判定的结论**：`execve` 的 `envp` 是"字符串数组"，标准**没有**规定
内核必须校验每个字符串含 `=`，也**没有**规定遇到不含 `=` 的条目要失败。
"不含 `=` 的条目被忽略"是**某个实现的行为**（POSIX 里没有这条）。

**本文的选择：不检查，原样传递。** 理由三条：

1. **检查它会让内核开始解释环境的内容**——与 `10-posix-layer.md:128-131`
   那条硬规则（内核不认识 `errno`/`O_CREAT` 这一类语义）正面冲突：
   "含不含 `=`"正是 `getenv` 的语义，而 `getenv` 是用户态的（`stdlib.c`）。
2. `envp` 数组本身**可能故意含 NULL**（今天的 `copy_user_argv` 就把
   `uptr_i == 0` 记成 NULL 并继续，`syscall.c:1090-1093`）——内核没有
   "这一条是不是合法环境"的判据，硬造一个只会变成第二份会漂的规则。
3. **代价落在用户态，而且很小**：`getenv` 的实现按 `=` 切分，
   不含 `=` 的条目天然被忽略（这是标准的 `getenv` 行为），
   不需要内核参与。

★ 代价如实说 ★ 一个把 `{"K"}`（忘了 `=V`）传下来的调用者，
`getenv("K")` 会返回 NULL，而**没有任何一处报错**。什么时候会咬人：
第一次写 `posix_spawn` 的 `envp` 组装代码时。**缓解手段**：
把它写进 `user/libposix` 那一侧的文档与 `faulttest` 风格的客户端注释里
（用户态可以自己检查，而且**应该**在那里检查）。

### 3.4 `envp` 的 ABI 变更与向后兼容

今天的两个 syscall 都是三参数：

| syscall | 今天 | 出处 |
|---|---|---|
| `FE_SYS_PROCESS_SPAWN = 0x50` | `(path, argv, argc)` | `kernel/include/fe/syscall.h:110`；`sys_process_spawn` 在 `syscall.c:1118-1146` |
| `FE_SYS_EXEC = 0x90` | `(path, argv, argc)` | `kernel/include/fe/syscall.h:353`；`sys_exec` 在 `syscall.c:1158-1185` |
| 调用约定：`rdi, rsi, rdx, r10, r8, r9` = 参数 1..6 | 所以**前 6 个参数不需要新寄存器约定** | `kernel/include/fe/syscall.h:4-8` |

**推荐做法：不改旧号，新增两个号**（"加参数"而不是"改签名"）：

```c
FE_SYS_PROCESS_SPAWN_H = 0x52   /* (path, argv, argc, envp, envc, harr) —— 6 个参数正好用满 */
FE_SYS_EXEC_ENV        = 0x94   /* (path, argv, argc, envp, envc)      —— 5 个参数 */
FE_SYS_MAX             = 0x95
```

★ **为什么是"新增"而不是"改旧号"**（这是本文的一个明确取舍）★

| 方案 | 代价 | 本文的判断 |
|---|---|---|
| **改 `0x50`/`0x90` 的签名**（多加两个参数） | 旧调用者（`user/libposix/spawn.c:142` 的 `fe_spawn`、`user/libfe/libfe.c` 的包装）**静默**把 `r10`/`r8` 当垃圾传下去 → 内核把垃圾当"环境数组指针"去读 → `FE_ERR_FAULT` 或更糟 | ❌ **否决**：项目里已经有"两侧定义漂移、没有任何东西报错"的实测教训（`fe/syscall.h:343-351`） |
| **新增号，旧的保留** | 两个入口要做同样的事 → **两份参数校验必然漂移**（这正是 `sys_exec` 的注释特意强调的："参数校验与 `sys_process_spawn` **逐字复用同一套**"，`syscall.c:1152-1154`） | ✅ **选它**，但**必须把校验与拷贝抽成共用函数**（今天已经是共用的：`copy_user_argv`，`syscall.c:1073`） |

★ 抽共用函数的代价与收益（这条要写进实现清单）★
收益：`FE_ARGV_MAX` / `FE_ARG_MAX` / `FE_PATH_MAX` 仍然只有**一份定义**
（`syscall.c:1153-1154` 的原文）。代价：新的"环境拷贝"必须是
`copy_user_argv` 的**兄弟**（同名不同参数），而不是复制粘贴一份。

★ 另一条可选路（本文不推荐，但要写下来）★
**`exec` 只暴露 `FE_SYS_EXEC_ENV`，把 `0x90` 标成"等价于 `envc = 0`"**。
好处是只有一个入口；坏处是"旧号还在、但语义变成特例"，
而 `0x90` 的**唯一调用者**是 `user/libfe/libfe.c` 的 `fe_exec`（`20:1015` 记的是
`:306-309`）——**我们自己的用户态**。所以：
`0x90` **保留原语义**（`envc = 0`），`0x94` 是新的带环境版本，
`fe_exec()` 保持三参数、`fe_exec_env()` 是新的五参数。
**这样"旧 ABI 一个字不改"这条纪律可以原样保持。**

---

## 4. 与 `exec` 的关系：**别把两件事混成一件**

| | `spawn` | `exec` |
|---|---|---|
| 新任务的句柄表 | **新建的、空的** → 需要"初始句柄组" | **原任务的、原样保留** → **天然满足"fd 继承"** |
| 依据 | `fe_task_alloc` → `fe_handle_table_init`（`kernel/task/task.c:111`） | `kernel/include/fe/syscall.h:342-343`（"身份……一个字都不动"）；`process.c:475-476`（`fe_exec` 的签名里没有句柄相关参数） |
| 环境 | 传不进去 | 传不进去 |
| 标准上要什么 | 初始句柄组 + `envp` | 只需要 `envp`（句柄那半**已经成立**） |

★ 于是三件事必须分开做、分开验（§6 分刀的依据）★

1. **`envp`**：spawn 与 exec **都要**（一处机制、两个 syscall 入口）；
2. **初始句柄组**：**只有 spawn 要**——`exec` 这条路上它**不是缺口**；
3. **`FD_CLOEXEC`**：★ **本文不做** ★ 见 §5。

★ `FD_CLOEXEC` 为什么不做（`20:394` 已经给了两条候选，本文复核并选定）★
`20:394` 的两个选项是：① 内核里加一个"句柄的 close-on-exec 位"；
② 让 `exec` 带一个"**要关哪些句柄**"的参数（名字中性）。
**本文赞成 ②**（①"把标准里的语义搬进机制层"，与 §1.4 那条硬规则冲突）。
但 ②**今天没有调用者**：`user/libposix` 里没有 `fcntl`、没有 `FD_CLOEXEC`、
没有 `O_CLOEXEC` 的实现（`20:364-365` 的核实：全仓搜只在 `fcntl.h` 里
有别的常量）。★ 按 `11-kernel-next.md:324` 那条纪律——
**"不要为了兑现文档里的一句话而造一个调用者"**——它排在 K13 之后，
等 `fcntl` 落地时按"exec 的第二个可选参数"加。★

★ 还有一条 `exec` 侧的**相邻事实**要写在这里（否则会被当成 K13 的缺口）★
`exec` 已经要求调用者是**主线程**（`process.c:486-491` 的 `INVAL`），
并且会先杀掉其它线程（`process.c:543`）。所以"`exec` 之后 fd 表还在不在"
这个问题**没有多线程版本**：唯一活下来的是调用者，而它的 fd 表是进程内的静态数组
（`posix.c:75`），本来就不随线程变。

---

## 5. 明确不做（按 `20` §1.2 / §1.2.4 判定）

| 不做 | 判定（三分法） | 理由 / 什么时候会咬人 |
|---|---|---|
| **`posix_spawnattr_*` 全套**（`POSIX_SPAWN_SETSIGDEF` / `SETSIGMASK` / `SETSCHEDPARAM` / `SETPGROUP` / `RESETIDS` / `SETSCHEDULER`…） | ★ **标准要求** ★（它们在 `<spawn.h>` 里） | ★ 但它们的**底座今天全都不存在**：信号屏蔽与处置需要 K5 之后的信号机制（`20` §2.4(b) 未做）、`schedparam` 需要"设置另一个线程的优先级"（今天 `FE_SYS_THREAD_CREATE` 只接受**创建时**的优先级，`kernel/include/fe/syscall.h:42`；没有 `sched_setscheduler`，`20` §2.14）、`SETPGROUP` 需要进程组（`20` §2.5 未做）。★ **所以这不是"我们选择不做"，而是"做了也没有承载体"** ★——按 `20:1085-1087` 的措辞纪律，这一条要写成"**标准要求，但挡在别的机制后面**"，不能写成"附加行为所以不做"。★ 今天的 `posix_spawnattr_init/destroy` 已经存在且返回 0（`spawn.c:103-113`），那是对的：**"有一个空的 attr 对象"与"attr 里的字段生效"是两件事**，而前者不影响任何语义 |
| **`fork` / COW** | ★ **不是标准**（POSIX 里没有 `fork` 这个词——它是 XSI/历史接口；标准给的是 `posix_spawn` 一族）★ | `20` §2.1 已经判定"POSIX 完整性**不必须**它"（`:245`），并给出实证：`make` 要的是 `posix_spawn`（`20:91-93` 引的是 `10-posix-layer.md` §4）。★ 本文不改这个判定 ★ |
| **Linux 的 `CLONE_*` / `clone()` 号空间** | ⚠ ★ **不是标准**（Linux 专有）★ | 项目**不做 Linux 二进制兼容**（`20:105`：不实现 Linux 的 syscall 号空间、ELF 加载约定、`/proc` 布局）。`clone` 的那套标志位是"某个实现怎么切共享面"，与本文的"初始句柄组"不是同一层的东西 |
| **`POSIX_SPAWN_CLOEXEC_DEFAULT`** | ⚠ ★ **不是标准**（各实现加的扩展）★ | 它改变"没列进 `file_actions` 的 fd 怎么办"的默认值。★ 本文**不实现它**，但**必须显式选定默认行为** ★——本文选的是"**槽位默认保持空，除显式 `INSTALL`/`COPY` 之外一个句柄都不装**"（即"子进程的句柄表默认是空的"这条今天的语义**不变**）。理由：它是最保守的一条，而且与"能力只能显式授予"的项目纪律一致 |
| **`FD_CLOEXEC` / `fcntl`** | ★ **标准要求** ★ | 见 §4 末尾：**底座已有**（exec 不动句柄表），缺的是"exec 时关掉指定句柄集合"的那半，而**今天一个调用者都没有**（`20:364-365`）。排 K13 之后 |
| **`addopen` 由内核代做** | ★ **标准要求（`addopen` 本身）** ★ / ⚠ **但"内核代做"不是标准** ★ | 标准只要求"结果是那个 fd 上有一个打开的文件"；**内核根本不知道文件是什么**（`posix.c:5-7` 的分层）。本文选"父进程先打开"（§2.6），并写清了它的代价 |
| **环境的 `NAME=VALUE` 校验** | ⚠ ★ **不是标准**（标准没规定内核要校验）★ | 见 §3.3：本文选"不检查、原样传递"，代价是"忘了 `=V` 时静默无效" |
| **`envp` 的"清空环境"标志**（`execve` 之外的空环境表达） | ⚠ ★ **不是标准** ★ | 空环境今天**已经**可表达：`envc = 0`（或 `envp = NULL`），初始栈上就是一个 NULL（`process.c:109`）。加一个标志是多一个状态、零收益 |

★ 本节的写法纪律（照 `20:1099-1101`）★
这一节里的条目**只有三条**是"附加行为 → 不做"（`CLONE_*`、
`CLOEXEC_DEFAULT`、环境的 `NAME=VALUE` 校验），
其余四条要么是**标准要求但挡在别的机制后面**（`attr` 全套、`FD_CLOEXEC`、
`addopen`），要么是**已经判定过的别的项**（`fork`）。
**三种情况的说法不能混**——这正是 `20` §2.17 那条纪律。

---

## 6. 判据、分刀、代价

### 6.1 判据（可证伪；每条正向都配一条会失败的反向）

判据的写法照 `kernel/task/user.c:226-239` 那一组（"每一条正向都配一条反向"）
与 `13-tasks-and-kill.md:750-751`（"判据要能区分'真的生效了'与'恰好也对'"）。

#### A. 初始句柄组

| # | 正向断言 | 反向对照（必须失败 / 必须不变） |
|---|---|---|
| **A1** | 父进程把一个 `/dev/console` 端点句柄装到子进程的 **1 号槽位**；子进程**在跑任何自己的代码之前**读 1 号槽位 → 拿到的是**同一个对象**（判据：子进程把那个句柄 `fe_devfs_open` 出来的东西写进去，父进程能核到同一个端点；★ 不要用"能打印出字"当判据，那证明不了"是同一个对象" ★） | **不装**时同一个子进程的 1 号槽位必须是空 → 它对 1 号句柄做任何操作返回 `FE_ERR_BADHANDLE`（`object.c:85-91`）。两条一起才有约束力：它们互相排斥 |
| **A2** | 装进去的句柄权限**是父进程那份减掉 `TRANSFER`**（判据：父进程给一份带 `TRANSFER` 的句柄，子进程拿它再转交给第三个任务 → **必须 `FE_ERR_ACCESS`**） | **反向对照**：父进程手里那份**原句柄**转交同一个对象 → **必须成功**（证明减位减的是"这一份副本"，不是把发送方的权限也砍了）。★ 这一条与 `ipc.c:448` 是同一族判据，而 `docs/09-handle-transfer.md:126` 记着它的反向对照："接收方拿到的句柄再转发 → `FE_ERR_ACCESS`" ★ |
| **A3** | **时序**：子进程的第一条用户指令执行时，初始句柄**已经在表里**（判据：子进程入口的第一件事就是查 1 号槽位；★ 这条要在**多核/抢占**下反复跑，因为竞态只在被抢走时出现——`process.c:274-286` 记的正是这种"QEMU 上从不出现、VBox 上每次出现" ★） | 反向：把安装点**故意**挪到 `fe_thread_create_ring3` 之后（临时改一行）→ A3 必须**红**。★ 这一条是"判据先于修复"的落地：不先看到它红，就不知道它测到了东西 ★ |
| **A4** | **原子性**：一组 3 个条目里第 2 个 fail（给一个**没有 `TRANSFER`** 的句柄）→ spawn **整体失败**、返回 `FE_ERR_ACCESS`、**子任务不存在**（判据：`fe_task_live_count()` 回到基线；名字不出现在 `TASK_LIST` 里） | 反向：同样的 3 个条目里第 2 个换成**合法**句柄 → 三个都装进去了（证明"失败"来自那一个坏条目，不是"批量装了就不回滚"） |
| **A5** | **默认是空的**：不传 inherited 时，子进程的句柄表里**一个句柄都没有**（判据：`fe_handle_table_used(&child->handles) == 0`，用户态镜像见 `user/include/fe_user.h` 的 `handle_count` 字段） | 反向：传 1 个 `INSTALL` 时 `used == 1`（证明这个计数真的在动，而不是"永远是 0 所以看起来对"）。★ 这一族教训：`13-tasks-and-kill.md` §4 记的"`switches` 被写死 0"——**恒真的判据什么都证明不了** ★ |

#### B. `envp`

| # | 正向断言 | 反向对照（必须失败 / 必须不变） |
|---|---|---|
| **B1** | 父进程 `setenv("K","V")` → `posix_spawn(..., envp)` → 子进程 `getenv("K")` 打印 `V` | **不传**环境（`envp == NULL`）时子进程的 `environ[0] == NULL`（`posix.c:48` 已经这么写：`environ = (envp && envp[0]) ? envp : g_empty_env`）。两条一起才排除了"恰好继承了父进程的环境" |
| **B2** | ★ **对齐判据**：`argc + envc` 的**奇偶各测一次**（例如 `(1,1)` 与 `(1,2)`），两次子进程都正常启动到 `main` | 反向：故意把 `process.c:103-108` 的推导按旧的 `n` 写（临时改一行）→ 奇偶之一必须**在 `_start` 里读到垃圾 argc**（这条正是"判据要能照出那个坑"）。★ `20:1036-1039` 已经提过这一条，本文把它写成**必须成对** ★ |
| **B3** | `envc > FE_ENVP_MAX` → `FE_ERR_RANGE`，**且子任务不存在** | 反向：`envc == FE_ENVP_MAX` 时成功（证明边界在 `>` 而不是 `>=`） |
| **B4** | 单条环境字符串长度 `== FE_ARG_MAX` 时成功、`> FE_ARG_MAX` 时 `FE_ERR_NAMETOOLONG` | 反向：同一个上限对 **argv** 也成立（证明两个上限是**同一份**判据，不是各写一遍） |
| **B5** | `envp` 里含一个**不含 `=`** 的条目 → 内核**原样传递**（不报错、不丢），子进程的 `environ` 里能读到它 | 反向：**这一条是负向判据本身**——它证明内核**没有**在解释环境的内容（§3.3 的选择）。★ 如果哪天有人加了 `NAME=VALUE` 校验，B5 会红，而那正是我们要它红的时刻 ★ |

#### C. `exec` 侧

| # | 正向断言 | 反向对照 |
|---|---|---|
| **C1** | `fe_exec_env(path, argv, argc, envp, envc)` 之后新映像的 `getenv("K")` 拿到 `V` | 反向：**旧号** `0x90`（三参数）仍然按 `envc = 0` 工作——新映像的 `environ[0] == NULL`（证明"旧 ABI 一个字不改"） |
| **C2** | **exec 不动句柄表**这条既有性质在 K13 之后仍然成立（判据：exec 前后 `fe_handle_table_used` 不变、某个已知句柄仍然可用） | 反向：spawn 出来的**新**任务在同样时刻 `used == 0`（证明"保留"是 exec 的特性，不是所有路径都这样） |
| **C3** | 环境准备失败（坏指针 / 超限）时**原程序完好**：exec 返回负错误码、原程序继续跑并打印一行 | 反向：同一个程序在**准备成功**时不再返回（`exec` 成功不返回，`user/include/fe_user.h:692` 明写"不要写成 `if (fe_exec(...) == 0)`"）。★ 这一条与 `15-exec.md` §10.2 的模式 B 是同一个形状 ★ |

#### D. 用户态那一层（`user/` 是测试客户端，判据要能在它身上跑）

| # | 正向 | 反向 |
|---|---|---|
| **D1** | `posix_spawn` 带一个 `adddup2(1, 1)`（把文件装到 1 号）→ 子进程 `write(1, ...)` 的内容落进**那个文件** | **不做 `file_actions`**时同一个子进程写到**控制台**（`20:342` 这一条已经定过；本文复核它的形状） |
| **D2** | ★ **§2.5 的撞车判据** ★ 子进程**继承** 1 号槽位之后，它**第一次**用到 fd 时（触发 `fd_ensure_init`）继承来的句柄**仍然有效** | 反向：如果 `fd_ensure_init` 无条件覆盖 0/1/2（今天的代码，`posix.c:124-130`），D2 **必须红**——★ 这是本文里唯一一条"在改动之前就能预测它会红"的判据 ★ 它也是 §2.5 那条修法的验收条件 |

### 6.2 分刀（三刀，每刀自带可证伪判据）

切法与 `13-tasks-and-kill.md` §6.8、`18-user-fault-handler.md` §4.2 同一原则：
**刀刃切在"出事时能不能单独归因"上**。

| 刀 | 内容 | 为什么这么切 | 自带判据 |
|---|---|---|---|
| **6a：`envp`（内核 + syscall + 两个入口共用校验）** | `build_initial_stack` 加第二段（含**对齐推导**）；`copy_user_argv` 的兄弟 `copy_user_env`；`FE_SYS_PROCESS_SPAWN_H (0x52)`、`FE_SYS_EXEC_ENV (0x94)`；`user/libposix/spawn.c:121` 的 `(void)envp` 换成真的用 | ★ 它**不依赖任何新机制** ★（不碰句柄、不碰任务生命周期），所以出问题时只可能是"栈摆错了"或"上限算错了" | **B1–B5** + **C1–C3**。★ 其中 **B2（对齐）** 是这一刀的承重判据 ★ |
| **6b：初始句柄组（内核机制 + ABI）** | `fe_handle_install_at`（按槽位装，`object.c` 那族）；`fe_process_create` 多 `inherit/n_inherit` 两个参数；安装点在 `fe_resource_note_init` 与 `fe_thread_create_ring3` **之间**；`FE_SYS_PROCESS_SPAWN_H` 或新增 `FE_SYS_PROCESS_SPAWN_XFDS` | ★ 它**不碰初始栈**（与 6a 正交），所以出问题时只可能是"权限/时序/原子性" | **A1–A5**。★ 其中 **A3（时序）** 与 **A4（原子性）** 是承重判据 ★ |
| **6c：用户态那一层（`posix_spawn` 的 `file_actions` + `fd_ensure_init` 的继承优先）** | `user/libposix/spawn.c` 的三个 `add*` 从 `ENOSYS` 变成真实现；`posix.c:124-130` 加"已有条目不覆盖"；`posix_spawn` 把 `envp` 传下去 | ★ 它**可以晚于 6a/6b 独立进行**，而且它的失败模式全在用户态（fd 表、重定向） | **D1–D2** |

★ 顺序是硬的：**6a 与 6b 可以并行，6c 必须在两者之后** ★
理由：6c 的判据（D1）同时用到环境与句柄两组能力，提前做会让"红"归因不清。
★ 而 6a 必须先于 6c 的原因更具体 ★ `posix_spawn` 的两个参数
（`envp` 与 `file_actions`）里，`envp` 是**纯传递**、`file_actions` 是**要新机制**——
把纯传递先做掉，6c 的失败就只可能是句柄那一半。

### 6.3 实现清单（要改/新增什么）

| 文件 | 改动 | 为什么在这一处 |
|---|---|---|
| `kernel/include/fe/process.h` | `FE_INHERIT_*` 常量、`struct fe_inherit_item_x`、`fe_process_create` 的新签名、`FE_ENVP_MAX` 的注释补上"K13 起被真正使用" | `fe_process_create` 的声明在 `:121-123`；上限常量在 `:40-42` |
| `kernel/include/fe/object.h` | `fe_handle_install_at(struct fe_handle_table *, u32 slot, struct fe_object_header *, u32 rights)` 的声明 | 与 `fe_handle_install`（`object.h:87-88`）同一族：**机制层**，所以自检能直接调（`20:1201-1203` 那条纪律） |
| `kernel/object/object.c` | 实现 `fe_handle_install_at`（按槽位装；槽位占用 → 失败；槽位 0 → 失败） | 挨着 `fe_handle_install`（`object.c:61-80`）——"0 恒为无效句柄"这条不变式就在那里（`:68`） |
| `kernel/task/process.c` | ① `build_initial_stack` 加 `envp` 与**对齐推导**（`:103-108`）；② `fe_image_prepare` 多 `envp/envc`；③ `fe_process_create` 多 `inherit/n_inherit` 并在**线程创建之前**安装（`:284-289` 之间）；④ `fe_process_spawn` 转发；⑤ `fe_exec` 多 `envp/envc` 并转给 `fe_image_prepare`；⑥ 失败一律 `goto fail_task`（`:330-336`） | 全部集中在"准备/提交"两段已有的结构里（`:158-166` 的设计说明） |
| `kernel/arch/x86_64/syscall.c` | `copy_user_env`（`copy_user_argv` 的兄弟，`syscall.c:1073`）；`sys_process_spawn_h`；`sys_exec_env`；分发表两项；两个旧入口**不改** | "参数校验与 `sys_process_spawn` 逐字复用同一套"这条纪律在 `:1152-1154` |
| `kernel/include/fe/syscall.h` | `FE_SYS_PROCESS_SPAWN_H = 0x52`、`FE_SYS_EXEC_ENV = 0x94`、`FE_SYS_MAX = 0x95`；共享 ABI 注释（含 `FE_INHERIT_STRIDE`） | 号空间现状：`0x50/0x51` 是 spawn/wait（`:110-111`），**`0x52` 空着**；`FE_SYS_MAX` 今天是 `0x94`（`:363`） |
| `kernel/task/*test.c`（新或既有） | A/B/C 组的自检 | 自检注册点在 `kernel/main.c`（今天 28 条 `g_selftest_failures += f;`） |
| `user/include/fe_user.h` | `struct fe_inherit_item`（**不带 `_x`**，与 `struct fe_task_info` 同一套命名，`fe_user.h:337`）镜像 + `_Static_assert`、两个新 syscall 号、`fe_spawn_env` / `fe_exec_env` 原型（挨着 `fe_spawn:682` / `fe_exec:705`） | 用户侧 ABI 镜像的落点（今天有 13 个先例，从 `fe_msg_header:174` 到 `fe_malloc_stat:911`；★ `fe_clock_info:469`、`fe_task_view:416` ★） |
| `user/libfe/libfe.c` | 两个薄包装 | — |
| `user/libposix/spawn.c` | 三个 `add*` 真实现；`posix_spawn` 用 `envp`；把 `file_actions` 翻译成 inherited 数组 | `spawn.c:81-101`、`:121`、`:122-125` |
| `user/libposix/posix.c` | `fd_ensure_init` 的 0/1/2 加"已有条目不覆盖"（§2.5） | `posix.c:114-133` |
| `user/bin/`（新客户端） | 见 §6.1 的 D1/D2 | 一次性程序放 `user/bin/` |
| `tools/check_init.py` | 单元表若加客户端：`119` 行的期望值（**今天是 37**，我实测过）与参考日志 | `check_init.py:119-120` |

---

## 7. 代价清单（一句话版）

1. **两个新 syscall 号**（`0x52`/`0x94`）+ **两个旧入口一个字不改**——
   代价是"两条路要做同样的事"，缓解手段是**校验与拷贝共用同一批函数**
   （今天已经是这样：`copy_user_argv`）；
2. **一个共享 ABI 结构**（内核侧 `struct fe_inherit_item_x` / 用户侧 `struct fe_inherit_item`，
   24 字节 × 最多 8 条 = 192 字节），两边各一份镜像 + 各自 `_Static_assert`；
3. **`fe_process_create` 的签名再长两个参数**（它已经有 6 个）；
   ★ 这是本文**最不优雅**的一处代价 ★ 缓解手段：把 `inherit/n_inherit`
   打包成一个 `const struct fe_process_opts *`（可空），代价是多一层解引用、
   收益是"以后再加就只动结构体"。**两条都可以，本文倾向打包**（§8 待查第 3 条）；
4. **初始栈的构造多一段**（环境字符串 + 指针数组），并且**对齐推导必须一起改**
   ——这是"看起来在工作"风险最高的一处（§3.1）；
5. **`kernel/task/process.c` 的失败路径要保证**：安装循环里任何失败都不能
   返回成功（§2.2 的"禁止 `return` 成功"）；漏掉的症状是
   **"装了一半的子任务跑起来"**——它的 fd 表一半是新的一半是空的；
6. **用户态多一处必须改的地方**：`fd_ensure_init` 的"继承优先"（§2.5），
   漏掉的症状是"重定向在第一次用到 fd 之前有效、之后失效"；
7. **`FE_ENVP_MAX` 从"定义了没人用"变成"有人在用"**——这本身是收益，
   但要顺带把 `process.h:37-39` 那段注释里"argc/argv/envp 上限"的措辞
   落实（今天它说的三样里只有两样在用）；
8. **`addopen` 的语义在"有凭据模型之后"会需要重做**（§2.6）——
   这是**明确的债**，今天不可观察，但它是一处"将来要回来改"的地方。

---

## 8. 待查清单（写下来，不猜）

| # | 待查的问题 | 为什么它会影响设计 | 怎么查 |
|---|---|---|---|
| 1 | 内核侧环境缓冲取 **`FE_ENVP_MAX * FE_ARG_MAX` = 1024 字节**（照 argv 那条乘法）够不够 / 对不对 | 它决定"一次 spawn 的环境总字节上限"。今天 argv 的上限是 `16 × 128 = 2048`（`syscall.c:1130`），环境是它的**一半**。★ 这两个数今天都**没有实测依据**（它们是设计常量，`process.h:37-39` 的"取小值是有意的"是唯一理由）★ | 与 argv 一起量：真实程序（shell、`make`、`qtbase` 的构建）实际要多少环境字节；或直接问"谁在做 POSIX 层"（不是本项目）——**不许编数字** |
| 2 | `FE_INHERIT_OP_COPY` 要不要在第一刀里做 | 它只是"少一次 `HANDLE_DUP`"的便利；不做的话 6b 更小 | 看 6c 的实现：如果 `posix_spawn` 的 `adddup2` 在用户态已经能拿到句柄（`posix.c` 的 `fd_entry.handle` 就是句柄），那 `COPY` **可能根本不需要**（直接 `INSTALL`）→ 那就**不做**（`11-kernel-next.md:324` 那条纪律） |
| 3 | `fe_process_create` 是"加两个参数"还是"加一个 opts 结构" | 影响调用点数量与将来的可扩展性 | 数一遍调用点（`process.c` 内部 + 自检 + `fe_process_spawn`）。★ 注意 `15-exec.md` §5 记着一次"顺手改任务对象"的教训——签名变更是**显式**的，比隐式字段好，但参数太多也会让人漏传 |
| 4 | `addopen` 在**子进程**里执行的正确形态（§2.6 的债） | 有凭据模型之后必须回来改 | 等 `20` §2.11/§2.5 那一族（`sysconf`/`kill`/权限）落地后再评估；判据是"父进程与子进程的权限必须不同时才可观察" |
| 5 | `FE_THREAD_FLAG_SUSPENDED`（`syscall.h:29`）是**打算实现**还是**历史残留** | 它决定候选 (b) 到底"要补多少"；也决定"线程挂起"要不要单独立一项 | 读 `11-kernel-next.md`/`10-posix-layer.md` 里所有提到"挂起"的地方，确认它有没有被登记成一项能力。★ 我读到的现状是：**用户态头里没有这个宏、内核里没有任何代码读它**（`user.c:530-532` 直接 `(void)flags;`）★ |
| 6 | （**已复核，不再是待查**）`getenv` 的位置与它对"不含 `=`"的处理 | —— | 我实测：`user/libposix/stdlib.c:217`（`getenv`）、`:235-238`（`setenv`）。★ §3.3 的判据 B5 就落在这个函数上：**K13 落地后要回来确认它对不含 `=` 的条目是"忽略"而不是"崩"** ★ |
| 7 | `posix_spawnp` 的 PATH 查找（固定表 `/bin/`、`/sbin/`，`spawn.c:171`）要不要改成读 `PATH` 环境变量 | K13 落地后 `PATH` **才第一次可能非空**（今天没有环境，所以那张固定表是诚实的） | `spawn.c:161-163` 的注释自己写着"没有 PATH 环境变量，所以这里是一张固定的搜索表"。★ K13 **会**让这句话过时 ★——所以它要作为 K13 的**收尾项**记下来（否则就是"文档在骗人"的又一例） |
