<!-- SPDX-License-Identifier: 0BSD -->
# K12：可等待的线程对象（`pthread_join` 的底座）

> **这是 K12 的设计文档。** 它回答四件事：**今天缺在哪一步**、
> **两条候选与取舍**、**语义边界（标准要求 vs 实现选择）**、**怎么证伪**。
>
> ★ **本文一行代码都不动。** 此刻有两个代理分别在改内核代码与别的文档，
> 所以本文的全部结论都来自**读文件**，没有跑构建、没有跑 QEMU/VBox。★
>
> 定位（`20-posix-kernel-requirements.md` §1.2 的口径）：
> **支持 POSIX 标准 ≠ 向 Linux 靠拢。** 本文按规范条文判定"哪些是标准要求、
> 哪些是我们的实现形态"，**不抄任何实现的做法**；内核里也**不出现 POSIX 名字**
> （`10-posix-layer.md:185-188`：★ 一条硬规则 ★ "内核里不出现 POSIX 的名字。
> 内核不认识 `open`、`errno`、`SIGKILL`；它只认识句柄、端点、通知、任务"，
> 紧接着一句"一旦内核里出现 `#define O_CREAT`，这条路线就变成了'把 Linux 抄进来'"）。
>
> 编号归属：K12 登记在 `20-posix-kernel-requirements.md` §3.4（本文**引用**
> 那个编号，不自立编号——与 `11-kernel-next.md` 的"编号归那份文档"是同一条纪律）。

## 1. 判据与今天的缺口

### 1.1 `pthread_join` 到底要求内核给什么

标准要求的是三件事，它们**不能拆开**：

| # | 标准要求 | 为什么"用户态做不到" |
|---|---|---|
| 1 | **等一个具体线程退出**（阻塞到它终止） | 用户态手里今天只有一个整数 id（`kernel/task/user.c:577` 的 `return th ? th->id : 0;`），而**没有任何能等它的东西**：等待登记是内核对象上的槽，id 不是对象 |
| 2 | **取回它的退出码** | 退出码存在内核的 `t->exit_code`（`kernel/include/fe/sched/thread.h:111`），用户态读不到；而线程一旦被回收，那个字段连同对象一起消失 |
| 3 | **join 之后该线程的资源确实归还**（栈 / TLS / FPU 区 / 对象本身） | 这些由内核分配（`kernel/sched/sched.c:1304-1317` 的 `thread_free`），用户态没有释放它们的能力 |

★ 一句话 ★ **`pthread_join` 缺的不是实现，是原语。**
这与 `20-posix-kernel-requirements.md:1303-1306` 的结论一致，
但那一节只写到"缺一个可等待的线程对象"；**本文的任务是把它拆到"缺在哪一行"**。

### 1.2 今天的完整链条（每一步都给了行号）

```
① 用户态   fe_thread_create(entry, arg, stack, size)
           user/libfe/libfe.c:100-104
             → fe_syscall(FE_SYS_THREAD_CREATE, entry, arg, stack, size, 0, 0)
             ★ 第 5 个参数固定传 0：用户态**没有**表达"我要 join 它"的位置 ★
                （头文件里其实有一个 FE_THREAD_FLAG_SUSPENDED，
                   kernel/include/fe/syscall.h:29 —— 见 §1.3 缺陷 D4）

② syscall  kernel/arch/x86_64/syscall.c:1553-1555（分发表里那一个 case）
             → sys_thread_create()，kernel/arch/x86_64/syscall.c:116-120
             ★ 五个参数一路透传，最后一个变成 (u32)flags ★

③ 内核     kernel/task/user.c:529-577   fe_user_thread_create
             :532  (void)flags;                    ← ★ 参数被显式丢掉　缺陷 D4
             :537-568  没给栈就自己分配（默认 FE_USER_STACK_SIZE = 64 KiB，
                       kernel/include/fe/user.h:19），并 fe_vma_add 一个区间
             :569-570  fe_thread_create_ring3(t, entry, stack, arg,
                                              16 * 1024, FE_PRIO_NORMAL, "user")
             :574-576  th->reap_ok = true;         ← ★ 缺陷 D3 的落点
             :577      return th ? th->id : 0;

④ 线程对象 kernel/sched/sched.c:428   t->id = g_next_id++;
             ★ 这个 id 是**软件计数器**，不是句柄、不是对象引用 ★

⑤ 线程跑   线程从 entry 开始跑（初始栈的伪造见 kernel/task/user.c:550-567）

⑥ 线程结束 fe_exit → FE_SYS_THREAD_EXIT（kernel/include/fe/syscall.h:40）
             user/libfe/libfe.c:85-93 → 内核 fe_thread_exit
             （kernel/sched/sched.c:1242-1250）
             → exit_isr（kernel/sched/sched.c:1226-1240）
             → thread_mark_dead(t, t->exit_code)（kernel/sched/sched.c:1178-1221）
                 :1214  rq_remove(t)
                 :1215  t->exit_code = code;
                 :1216  t->state = FE_THREAD_DEAD;
                 :1217-1218  挂进僵尸链 g_zombies
                 :1220  fe_process_on_thread_exit(t)

⑦ 收尸     空闲线程里调 fe_sched_reap()（kernel/sched/sched.c:1658 附近的调用点）
             → kernel/sched/sched.c:1325-1354  遍历僵尸链
                 :1333-1336  reap_ok == false 的**跳过**（"有人在 join 它的不许碰"）
                 :1350       thread_free(z)
             → thread_free（kernel/sched/sched.c:1291-1323）
                 释放栈(:1304-1307)、TLS(:1310-1315)、FPU 区(:1316-1317)、
                 对象本身(:1319)，并放掉对任务的那次引用(:1320-1322)
```

### 1.3 ★ 缺在哪一步：五个可指名的缺陷 ★

**(D1) 内核返回给用户态的是 id，不是句柄——而且这个 id 没有任何能力附着。**
`kernel/task/user.c:577` 返回 `th->id`；`th->id` 由 `kernel/sched/sched.c:428`
的 `g_next_id++` 分配，是一个**单调递增的软件计数器**。
句柄表里**没有线程对象**（`kernel/include/fe/object.h:73-82` 的
`struct fe_handle_entry` 只在创建端点/通知/内存对象/任务时被填），
所以用户态手里那个整数**既不能用来等、也不能用来验证"它还是不是原来那条线程"**。

★ 关键对照 ★ 任务侧是**有**这套的：`fe_process_wait` 先做
`fe_handle_lookup(&parent->handles, task_handle, FE_RIGHT_WAIT, &obj)`，
再断言 `obj->type == FE_OBJ_TASK`（`kernel/task/process.c:380-388`）。
线程侧一行都没有。**能力模型在任务上有，在线程上没有——这就是 D1。**

**(D2) 线程退出码的**唯一**存身之处是一个会被回收的对象字段。**
`t->exit_code`（`kernel/include/fe/sched/thread.h:111`）由
`thread_mark_dead` 写入（`kernel/sched/sched.c:1215`），
随 `thread_free` 一起消失（`kernel/sched/sched.c:1319` 的 `fe_kfree(t)`）。
而用户线程默认 `reap_ok = true`（`kernel/task/user.c:574-576`）
⇒ **空闲线程会在任意一次让出之后把它回收**
⇒ 用户态即使拿到了 id，也**没有任何时刻能保证读到"退出码 + 线程对象"这一对**。
★ 这是一个竞态，不是"以后优化" ★

**(D3) `reap_ok` 编码的是一个**假设**，而这个假设正是 K12 要推翻的那一条。**
`kernel/include/fe/sched/thread.h:66-85` 把这个字段的语义写得很清楚：
> `reap_ok = true` → 僵尸由调度器回收（**默认**，进程主线程属此类……）；
> `reap_ok = false` → 有人在 join 它，回收器不许碰

而 `kernel/task/user.c:571-576` 给用户线程**无条件**设 `true`，注释逐字是
"用户自己建的线程：内核里**没有人 join 它**（用户态的 join 语义还没有）"。
★ 所以 K12 要改的不是"加一个字段"，而是**把这一条假设换成一个真判断**：
"这条线程还有人要 join 吗"——而"有人要"这件事**必须由用户态表达出来**（§2）。★

**(D4) `FE_THREAD_FLAG_SUSPENDED` 是一个**被静默忽略**的 ABI 位。**
`kernel/include/fe/syscall.h:29` 定义了 `#define FE_THREAD_FLAG_SUSPENDED (1u << 0)`，
全项目**只有这一处**（全树搜 `FE_THREAD_FLAG_SUSPENDED` → 1 处命中），
而 `kernel/task/user.c:532` 是 `(void)flags;`。
★ 也就是说：用户态今天传**任何** flag 位进来，内核都接受、都忽略。★
这与本项目自己立过的规矩正面冲突——`user/include/posix/fcntl.h:4-6` 逐字：
> 我们**不支持**的位必须能被识别出来并报错，不能当成"没给"——
> 那会让"要追加写"静默变成"覆盖写"。

**K12 必须顺手把这个口堵上**：要么真的实现（线程创建后先挂起、等一个"启动"动作），
要么**拒绝未知位**。两种都比"静默忽略"好。

**(D5) 内核里的 `fe_thread_join` 是一条**不可用于用户态**的路，而且它自己承认有 UAF 风险。**
`kernel/sched/sched.c:1356` 的签名是 `i32 fe_thread_join(struct fe_thread *t)`
——收的是**内核对象的裸指针**（用户态不可能构造），
`kernel/include/fe/sched/thread.h:258-259` 的声明与之一致。
它的行为：

| 事实 | 出处 |
|---|---|
| 有界自旋：让出 + 计轮数，上限 **20,000,000** 轮 | `kernel/sched/sched.c:1386-1397`（`spins >= 20000000u` 在 `:1390`） |
| 超时后打印一行，然后**放弃等待** | `kernel/sched/sched.c:1391-1395` |
| 放弃之后仍然读退出码、摘两条链、**`thread_free(t)`** | `kernel/sched/sched.c:1398`、`:1399-1417`、`:1418` |
| ★ 注释自己承认这条路出过 use-after-free ★ | `kernel/sched/sched.c:1363-1381`：`2000000` 轮的上限是按"让出很贵"估的，让出修快之后"两百万轮只要零点几秒"，于是 join **在被等的线程还没死透时就超时放弃，放弃之后还把它 `thread_free` 掉**——"实测：`init` 的线程对象被释放之后它再也走不到收尾" |

★ 两条连带事实（都要记进 K12 的账上）★
1. **它今天唯一的调用者都是内核自己**：`kernel/task/process.c:644`（引导线程等 init 主线程）
   与 `kernel/ipc/ipctest.c:205`/`:556`/`:857`/`:937`（自检）。
   这与 `kernel/include/fe/sched/thread.h:81-84` 的说明一致：
   "`reap_ok=false` ……今天只有引导线程 join 的 init 主线程属于此类"。
2. ★ **一处注释里的数字已经过期** ★ `kernel/include/fe/process.h:154` 写着
   "与 fe_thread_join 的 2,000,000（sched.c）同族的保守值"，
   而 `kernel/sched/sched.c:1390` 的实现是 **20,000,000**。
   `FRONT` 那一句是"轮数上限跟着让出语义改过"这件事的**遗留**。
   ★ 本文只**如实记下**这个不一致，不改任何文件 ★

**(D6) ★ K12 会新增第六处等待登记，而"漏加取消路径"的教训已经写在代码注释里 ★**
`kernel/ipc/ipc.c:1103` 起是 `fe_thread_cancel(task, victim)`，它今天要摘**五条**路径：

| # | 路径 | 位置 |
|---|---|---|
| 1 | `wait_any` 的节点（线程自己身上的 `wait_node`） | `kernel/ipc/ipc.c:1088`（注释）、`kernel/include/fe/sched/thread.h:115-134` |
| 2 | 端点的 `receiver` 单槽 | `kernel/ipc/ipc.c:1128-1132` |
| 3 | 通知对象的 `waiter` 单槽 | `kernel/ipc/ipc.c:1133-1149` |
| 4 | 共享区间锁上的等待登记 | `kernel/ipc/ipc.c:1151-1163`（`fe_resource_forget_thread_locks`） |
| 5 | 目标任务上的 `waiter` 槽（`fe_process_wait` 的登记） | `kernel/ipc/ipc.c:1164-1176`（`fe_task_clear_waiter`，实现在 `kernel/task/task.c:217-227`） |

而 `kernel/ipc/ipc.c:1101-1102` 逐字写着：

> ★ 将来若有第四种等待登记，必须同时加一条取消路径 ★
> 这一条写在 `docs/13-tasks-and-kill.md` §3.5 的边界里。

（`docs/13-tasks-and-kill.md:186` 就是那一行，它自己标注"**已被 §6.3 证伪：实际是五处登记**"。
`docs/21-user-address-wait.md:143` 也说这五处是"权威清单"。）

★ 所以 K12 的每一刀都必须回答：**新加的那处登记，在哪一刻被摘掉？** ★
漏掉它的症状与路径 5 的缺陷**一模一样**（`kernel/ipc/ipc.c:1166-1173` 记着实测）：
线程被取消 → 返回 CANCELED → 走闸门 → 死 → 僵尸被回收，
而槽里留着指向已释放内存的指针，下一次"被等的那一方"动作时拿它去
`fe_sched_wake`（读 `t->state`）——一次 use-after-free。

### 1.4 判据（本文要证伪什么）

> **K12 完成的判据是**：用户态能拿到一个**线程句柄**，对它可以
> ① 阻塞到目标线程退出、② 取回退出码、③ 令目标线程退出后资源真的归还，
> 而且 ④ 这处新的等待登记在**取消 / 终止 / 任务销毁**三条路径上都被摘掉。

**不满足任一条都不算完成**——尤其 ④，它是本项目已经栽过五次的那一类。

## 2. 设计：候选与取舍

### 2.1 候选 (a)：线程变成句柄对象（`FE_OBJ_THREAD` + 父线程持句柄）

**做法**：

```
1. struct fe_thread 最前面放 struct fe_object_header（16 字节：
   u32 type + volatile u32 refcount + u64 id — kernel/include/fe/object.h:30-34）
   ★ 必须在最前面 ★ FE_OBJ_OF(ptr, type) 是纯指针转换（object.h:41），
     没有偏移调整；放后面就要求每个使用者自己算偏移。
2. 新建线程时把它的对象装进**调用者**的句柄表：
   fe_handle_install(&fe_task_current()->handles, &th->hdr, FE_RIGHT_JOIN)
   （fe_handle_install 的语义见 kernel/object/object.c:61-80：扫描第一个空槽、
      ref 一次、返回索引；0 恒为无效句柄）
   ★ 装进**调用者**的表而不是新线程自己的表 ★ —— 线程自己的表在被 join
     之前不存在（它还没有"自己的"句柄表语义），而"谁能 join 谁"是标准的
     "创建者获得能力"形状，与 fe_process_spawn 把子任务句柄装进父进程
     （kernel/include/fe/process.h:125-128）**完全同构**。
3. reap_ok 的写法改成：reap_ok = detached（见 §2.3 的语义澄清）
4. 线程退出时（thread_mark_dead 之后）：若有人正在 join，唤醒它；
   否则按 detached 或不 detached 决定"等 join 还是等回收器"。
5. 生命周期归引用计数：thread_free 里的 fe_kfree(t) 换成 fe_object_unref，
   并在 fe_object_destroy 里补一个 case FE_OBJ_THREAD
   （kernel/task/task.c:21-116 那个 switch 今天没有它）。
```

**为什么它最贴合本项目**：
- **对象类型已经预留在那里**：`kernel/include/fe/object.h:24` 有
  `FE_OBJ_THREAD`，而 `kernel/object/object.c:19` 已经给了它名字"线程"。
  ★ 全项目搜 `FE_OBJ_THREAD` → **只有这 2 处**（枚举定义 + 名字表），
  没有任何逻辑用它。**这是一个已经挖好、还没插线的槽位。** ★
- 权限位体系现成（`kernel/include/fe/object.h:50-71`），join 只是再加一位。
- 与任务侧**同构**：调用者拿句柄、内核按句柄查对象、等一个"已退出"标志、
  取退出码。K12 不需要发明第二种等待形状。

**代价（逐条说清）**：

| 代价 | 具体是什么 |
|---|---|
| **`fe_thread` 的偏移全变** | 头部插在最前面，`state`/`exit_code`/`task` 等字段全部后移 16 字节。★ 今天**没有任何代码依赖裸偏移**（调度切换用的是 `t->rsp`，见 `kernel/include/fe/sched/thread.h:46-48`），但要加一条静态断言把"头部在 0 偏移"钉住 ★ |
| **销毁路径必须收敛到一处** | 今天 `thread_free` 直接 `fe_kfree(t)`（`kernel/sched/sched.c:1319`）。加了头部之后，句柄表持有一份引用，**再直接 free 就是双重释放**；反过来，如果 join 之后不放引用，线程对象**永不销毁**。★ 这两个方向的错都不是崩溃，是"慢漏"或"随机崩" ★ |
| **句柄表是 256 项** | `kernel/include/fe/object.h:47` 的 `FE_HANDLE_TABLE_SIZE = 256`。创建 N 条线程占 N 个句柄槽。★ `docs/18-user-fault-handler.md:161` 已经为"每线程一个端点"这条**拒绝过**同一个形状（"每个线程一个端点句柄是纯浪费"）——所以这里必须说清为什么不同：**join 需要的是"每条线程一个能力"，而端点那份能力对每个线程都一样，是可以共享的** ★ |
| **`FE_OBJ_THREAD` 的 id 与 `t->id` 是两套编号** | `fe_object_init` 会分配自己的 `g_next_object_id`（`kernel/object/object.c:9`、`:28`），而线程已有 `t->id`（`kernel/sched/sched.c:428` 的 `g_next_id++`）。★ 两套编号并存就必须回答"诊断打印用哪个"——建议保留 `t->id`（`ps` 与 `fe_thread_dump_all` 都在用它）并把对象 id 只当对象层的账 ★ |

### 2.2 候选 (b)：用通知对象表达"线程结束"

**做法**：不动线程对象，用户态创建一个通知对象（`fe_notification_create`，
`user/include/fe_user.h:589`），把句柄交给内核；线程退出时内核替它置一位；
`pthread_join` 在用户态等那一位（`fe_notification_wait`，`user/include/fe_user.h:590`），
退出码经一块共享内存传回。

**它的吸引力**：**不新增系统调用号、不动 `fe_thread` 的结构**。
今天所有零件都在（通知对象 + `wait_any` 的内核机制 + 内存对象）。

**代价（四条，每条都是"用户态补不回来"的那一类）**：

| # | 代价 | 为什么用户态补不回来 |
|---|---|---|
| 1 | **通知对象是要给内核的新参数**，而线程创建今天只有 5 个参数、第 5 个还被忽略（`kernel/arch/x86_64/syscall.c:116-120`、`kernel/task/user.c:532`） | "新参数"就是"改 ABI"，代价与新增一个 syscall 同量级——**它并没有省下那一刀** |
| 2 | ★ **竞态：线程可能在 join 之前就退出了** ★ | 通知对象只能记住"置过位"（位是粘性的），但**"置位"与"某条具体线程的退出码"之间没有绑定**：两条线程共用一个通知对象时，谁置的那一位分不出来。要分开就得每条线程一个通知对象，于是回到"对象数 = 线程数"，只是对象类型换了一个 |
| 3 | **退出码不是位信息** | 通知对象传的是位（`user/include/fe_user.h:590` 的 `mask`），退出码要另开一块共享内存。★ 而"写共享内存"与"置位"**不是原子的** ⇒ join 醒来时可能读到还没写的退出码 ★ |
| 4 | **它不解决 D2/D3，只挪了位置** | 线程对象照样默认 `reap_ok = true`（`kernel/task/user.c:574-576`），退出码照样随对象消失；`detach` 照样是个标志；**"谁负责收尸"从一个答案变成两个**（回收器 + 取退出码的人）。而 `20-posix-kernel-requirements.md:1201` 的纪律逐字是："K12 的'谁负责收尸'**必须只有一处答案**" |

★ 还有一条属于**判定**而不是代价 ★ 用通知对象表达"线程结束"，
等于把 `pthread_join` 的**语义**（谁等谁、退出码怎么取、detach 怎么算）
推到用户态。而 `20-posix-kernel-requirements.md` §1.1 把本项目的交付物写成
"**一个内核**"，§1.2 又把"用户态做不到的那一半"定义成内核的义务。
"等一个具体线程并拿它的退出码"**是用户态做不到的那一半**（§1.1 的判据 1/2/3），
所以它不该被推到用户态。★

### 2.3 ★ 推荐：候选 (a)，并且**只做 (a)** ★

**理由三条**：

1. **它把"谁负责收尸"保持成一处的答案。** `reap_ok` 的语义从
   "内核里没有人 join 它"（一个**假设**）换成"这条线程有人要 join"（一个**事实**），
   而这个事实由**句柄存在与否**表达——句柄一关，回收器就接手，不需要第二处判断。
   ★ 这与 `20-posix-kernel-requirements.md:1199-1203` 的纪律正面吻合。★
2. **它让"取消路径"这件事仍然只有一处。** 新的等待登记是"线程对象的
   `join_waiter` 槽"，它在 `fe_thread_cancel` 里加**一条**摘除路径（§4.4 的 C1），
   而如果选 (b)，登记落在通知对象上（路径 3 已经存在）**但退出码那块共享内存
   仍然需要一个"谁在等"的位置**——于是等待登记变成两处，漏一处的风险翻倍。
3. **它不需要发明新机制。** 句柄 / 引用计数 / FE_OBJ_* / 权限位 /
   "检查 → 登记 waiter → 阻塞"这套循环**全部已存在**，且都在真实路径上跑过
   （`kernel/task/process.c:374-428` 的 `fe_process_wait` 就是它）。

**★ 一处必须澄清的语义（否则实现时一定漂）★**
`reap_ok` 今天编码的是"内核里**有没有人** join 它"（`kernel/include/fe/sched/thread.h:66-85`）。
K12 之后要区分**两件不同的事**：

| 概念 | 谁决定 | 含义 |
|---|---|---|
| **detached** | **用户态**（`pthread_detach`） | "我不打算 join 它"——这是标准的语义 |
| **有人持 join 句柄** | **内核**（句柄表里有没有那个句柄） | "还能被 join"——这是实现的事实 |

★ 两者不能合成一个 bool ★ 一个线程可以被 `detach`（用户态声明不要了），
也可以"没人 detach、但用户态忘了 join"（句柄还开着）。
**两种情况下回收器都该接手**，但**理由不同**：前者是语义，后者是句柄关了。
把两者合成一个字段的症状是"detach 过的线程仍然不被回收"（漏一个僵尸，可见）
或者更糟——"句柄还开着就被回收"（join 踩空，不可见）。

**候选被否掉的那一条也写在这里**：(b) **不做**，理由见 §2.2 的四条，
以及它违反 `20` §3.5 的第一条纪律。

## 3. 与既有机制的关系

### 3.1 任务句柄（`FE_OBJ_TASK`）已有 join 语义：**照它做，不复用**

`fe_process_wait`（`kernel/task/process.c:374-428`）是现成的模板，逐条对照：

| 步骤 | 任务侧（已实现） | 线程侧（K12 要做的） |
|---|---|---|
| 取句柄 | `fe_handle_lookup(&parent->handles, task_handle, FE_RIGHT_WAIT, &obj)`（`:381`） | 同一套；权限位见下 |
| 断类型 | `if (obj->type != FE_OBJ_TASK) return FE_ERR_INVAL;`（`:385-387`） | `obj->type != FE_OBJ_THREAD` |
| 取对象 | `FE_OBJ_OF(obj, struct fe_task)`（`:388`） | `FE_OBJ_OF(obj, struct fe_thread)` |
| 取消点 | `if (fe_thread_should_die(fe_thread_current())) return FE_ERR_CANCELED;`（`:412-414`） | 同一句，**必须放在循环顶** |
| 检查条件 + 登记 waiter | 关中断区间里一起做（`:415-425`），注释在 `:390-393` 解释为什么（"检查完发现没退出，正准备阻塞"与"对方刚好在这一刻退出并唤醒"会互相错过） | 同一套 |
| 阻塞 | `fe_sched_block_current()`（`:426`） | 同一个 |
| 唤醒方 | `fe_process_on_thread_exit`（`kernel/task/process.c:430-448`） | 在 `thread_mark_dead` 之后做（§3.4） |
| 摘登记 | `fe_task_clear_waiter`（`kernel/task/task.c:217-227`） | 需要对称的 `fe_thread_clear_joiner`（§4.4 C1） |

★ **为什么"不复用"★** 任务侧等的是 `task->exited` + `task->exit_code`
（`kernel/include/fe/task.h:63-65` 的 `waiter`/`exit_code`/`exited`），
那三个字段属于**任务**。线程要的是**线程级**的"它退出了吗 + 它的退出码"，
而线程没有 `exited` 字段——它有的是 `state == FE_THREAD_DEAD`
（`kernel/include/fe/sched/thread.h:41`）与 `exit_code`（`:111`）。
★ 把任务的 `exited` 借给线程用，会让"主线程退出"与"某条线程退出"变成同一个标志，
而 `fe_process_on_thread_exit` 恰恰是靠"只看主线程"来区分这两件事的
（`kernel/task/process.c:436-440`）★ —— **复用会把这个区分拆掉。**

### 3.2 权限位：新增 `FE_RIGHT_JOIN`，还是复用 `FE_RIGHT_WAIT`？

| 候选 | 好处 | 代价 |
|---|---|---|
| **复用 `FE_RIGHT_WAIT`**（`kernel/include/fe/object.h:56`，"等待通知"） | 不加常量；`fe_process_wait` 已经在用它查任务句柄（`kernel/task/process.c:381`） | ★ 语义漂移：那一句注释写的是"**等待通知**"。一旦它同时表示"等任务"与"等线程"，下一个人读权限位表时无法从名字判断它能等什么 |
| **新增 `FE_RIGHT_JOIN`** | 名字即语义；`FE_RIGHT_ALL = 0x3FFu`（`kernel/include/fe/object.h:71`）是**手工枚举的掩码**，所以"加一位"必须**同时改它**——而改了它就会被 `fe_process_spawn` 那条路径自动带上（`fe_handle_install(..., FE_RIGHT_ALL)` 之类），**这一点必须核实**（§8 待查 J3） | 多一个常量；多一处"新句柄默认给哪些权限"的决定 |

★ **推荐新增 `FE_RIGHT_JOIN`** ★，理由与 `FE_RIGHT_TERMINATE` 当初单列一位
（`kernel/include/fe/object.h:59-70`）是同一条：**"杀"是一位权限而不是一个特殊接口**，
它的理由写得很清楚——"名字可以改、id 会复用，所以只能是'句柄 + 权限位'"。
join 是**破坏性**操作（它会让线程对象被回收、句柄失效），
放进一个叫"等待通知"的位里，等于把两件不同强度的事合成一票。

### 3.3 `pthread_detach` 对应哪一步

★ **`detach` 不是"加一个标志"，它是"放弃 join 能力"** ★
最贴合能力模型的做法是：**`pthread_detach(t)` = `fe_handle_close(线程句柄)`
+ 置 detached 位**。

- 关句柄这一步是**用户态可做的**（`FE_SYS_HANDLE_CLOSE` 已有，
  `kernel/arch/x86_64/syscall.c:1583`），
- 但"以后没人能 join 它了"这个**事实**只能由内核在"最后一个 join 句柄被关掉"
  的那一刻知道——而句柄表**没有回收回调**（`fe_handle_close`，
  `kernel/object/object.c:116-134`，只做 `e->object = NULL` + `fe_object_unref`）。
  ★ 所以"最后一个句柄关掉 ⇒ 回收器接手"这件事**不能**靠句柄表实现，
  必须靠**引用计数归零**（`fe_object_unref` 那条路，`kernel/object/object.c:42-50`）。
  这正是 §2.3 说的"生命周期归引用计数"的含义。★

**代价**：`pthread_t` 在用户态**仍然是一个整数**（句柄号），而"线程已经 detach 过"
这件事要么存在用户态（一个 id→标志的表），要么内核再给一个 syscall 去置位。
★ 本文推荐前者（用户态存 detach 标志，内核只认句柄）：这样内核里
**只有一处**回答"这条线程还能不能被等"（句柄在不在），
不需要第二个状态位。**代价如实写**：用户态必须自己维护"哪些 id 已 detach"，
而这正是 POSIX 允许的（标准只规定 `pthread_detach` 之后 `pthread_join` 必须失败，
**没有**规定失败由谁判定）。★

### 3.4 与"被杀线程"的闸门共存（`docs/13` §6）

**今天的机制**（逐条给行号）：

| 事实 | 出处 |
|---|---|
| 闸门的**唯一**判据是 `t->kill_pending \|\| t->task->dying` | `kernel/sched/sched.c:532-535`（`fe_thread_should_die`） |
| 闸门本身在**中断返回的必经之路**上，判据在早退之前 | `kernel/sched/sched.c:539-562`（`:559-562` 是那句"必须在 `g_need_resched` 的早退**之前**判"） |
| 闸门判定死亡时用的退出码是 **`FE_ERR_KILLED`**（= -22，`kernel/include/fe/errno.h:42`） | `kernel/sched/sched.c:560`（`thread_mark_dead(g_current, FE_ERR_KILLED)`） |
| `kill_pending` **只有置位、没有清除** | `kernel/include/fe/sched/thread.h:87-105`（语义三条，第 2 条逐字） |
| 线程级标记由 `fe_task_kill_other_threads` 发出（`exec` 用） | `kernel/include/fe/process.h:159-170`；实现在 `kernel/task/process.c:645-680`（`:680` 是 `t->kill_pending = true;`） |

**被杀线程的 join 返回什么？** ★ 一条链推到底，答案是确定的 ★

```
被杀的线程有两条路走到死，但**两条路的退出码是同一个值**：
  路 A（它在"正要回用户态"的闸门上被判死）
    → thread_mark_dead(g_current, FE_ERR_KILLED)   sched.c:560
  路 B（它阻塞在某个取消点上）
    → 取消点返回 FE_ERR_CANCELED（-21）           ipc.c:1084-1085 / process.c:412-414
    → 用户态/内核调用者按错误处理，最终仍要退出
    → ★ 但**它退出时传的码**取决于调用者 ★
```

★ **这里有一个必须写下来的边界** ★ 路 B 的线程**不是**由内核直接给它
`FE_ERR_KILLED`——它在取消点上拿到 `CANCELED`，然后**由它正在跑的那段代码
决定接下来怎么办**。如果那是内核里的一次阻塞调用（如 `fe_process_wait`），
它返回 `-21` 到用户态；用户态的程序**可以**选择用 `-21` 退，也可以退别的码。
★ 所以 K12 能保证的是：**"被闸门判死的线程 join 得到 `FE_ERR_KILLED`"**；
**"被取消点唤醒的线程"的退出码由它自己决定**——两句话不能合并。★

**推荐**：`pthread_join` 把 `t->exit_code` **原样**返回（不做任何翻译），
因为它是内核里**唯一**存的退出码（`kernel/sched/sched.c:1215` 写入、`:1398` 读出）。
`FE_ERR_KILLED(-22)` 因此会原样出现在 join 的返回值里。
★ 对照 POSIX：`PTHREAD_CANCELED` 是标准定义的"被取消"返回值，
而**我们不做 `pthread_cancel`**（§4.5），所以这里**不需要**那个翻译 ★——
但要写清"为什么没有"，否则下一个人会以为漏了。

### 3.5 与 `exec`、任务销毁的关系

| 场景 | 今天 | K12 之后要保证什么 |
|---|---|---|
| **`exec` 杀其它线程** | `fe_task_kill_other_threads`（`kernel/include/fe/process.h:159-170`）标记它们，之后 `fe_task_wait_others_dead` 等死透（`:172-179`） | 被杀线程的 `join_waiter`（如果有）必须被**唤醒或摘除**——否则 join 者永远睡（§4.4 C1）。★ `exec` 的调用者**必须**在名单之外（`process.h:165-169`），所以"正在 join 的线程"被杀之后会走取消点返回 CANCELED，这是允许的 ★ |
| **任务被终止（K2）** | 所有线程走取消点（`kernel/ipc/ipc.c:1103` 的 `fe_thread_cancel`），任务销毁时 `fe_object_destroy` 的 `FE_OBJ_TASK` 分支做一长串收尾（`kernel/task/task.c:36-110`） | 同上一行：join 登记必须在同一处被摘。★ 并且**线程对象自己**也要能销毁：今天 `case FE_OBJ_TASK` 里没有 `FE_OBJ_THREAD` 的分支（`kernel/task/task.c:21-115`），K12 要补 ★ |
| **任务销毁之后线程还活着** | `fe_task_detach_threads`（`kernel/include/fe/sched/thread.h:281-300`；实现在 `kernel/sched/sched.c` 里）把僵尸的 `task` 摘成 NULL，防一条实测抓到的 use-after-free（`thread.h:286-298` 记着 CR2 = 任务地址 + 0x1060） | ★ 一个 join 者如果等的是一条**属于已销毁任务**的线程，必须得到一个明确错误，而不是永远睡 —— 这条要单独验（§5.3 反向 R6） |

## 4. 语义边界（标准要求 vs 实现选择）

★ 判定方法按 `20-posix-kernel-requirements.md` §1.2.4（逐条对规范，
不用"某个实现怎么做"当依据）。★

### 4.1 `pthread_join` 的返回值与错误码

| 项 | 判定 | 说明 |
|---|---|---|
| 成功时返回 **0**，退出状态写到 `*retval` | ★ **标准要求** ★ | 我们要加的 ABI 是"`join` 返回 `i32` 退出码"这个**内核形态**；把它翻成标准的 `int` + `void **retval` 是用户态的事（§1.1：内核给机制，用户态给语义） |
| `EINVAL`：目标线程**不可 join**（已 detached，或已被别人 join 过） | ★ **标准要求** ★ | 内核侧对应"句柄没带 `FE_RIGHT_JOIN`"或"对象已被回收" |
| `ESRCH`：**没有**这个线程 | ★ **标准要求** ★ | 内核侧对应 `FE_ERR_BADHANDLE`（句柄无效，`kernel/object/object.c:89-91`） |
| `EDEADLK`：**自己 join 自己** | ★ **标准要求** ★ | ★ 见 §4.3 ★ |
| `ETIMEDOUT`（`pthread_timedjoin_np`） | ⚠ **不是标准接口**（是某个实现的扩展） | 明确不做（§4.5）；★ 而且它需要"等待超时"，那是 K14 的形状 ★ |

★ **内核不出现 POSIX 名字**（`10-posix-layer.md:185-188`）★
所以内核侧返回的是 `FE_ERR_*`（`kernel/include/fe/errno.h:19-45`），
用户态再翻成 `EINVAL`/`ESRCH`/`EDEADLK`。
这套翻译**已经有一层现成的**：`user/libposix/posix.c:100` 的
`case FE_ERR_NOTSUP: return ENOSYS;`（以及它上面那一整段 `errno_from_fs`）。

### 4.2 `EDEADLK`（自己 join 自己）——今天**有常量但没用过**

核实：`kernel/include/fe/errno.h:43` 定义了 `FE_ERR_DEADLOCK = -23`，
而全项目搜 `FE_ERR_DEADLOCK` → **只有 2 处**：
定义处（`errno.h:43`）与 `kernel/lib/panic.c:91` 的名字表
（`case FE_ERR_DEADLOCK: return "死锁";`）。

★ 也就是说：**这个错误码从来没被返回过。** K12 会是它的第一个使用者。★
判据很简单：`join` 的目标线程 == `fe_thread_current()`
（`kernel/include/fe/sched/thread.h:261`）⇒ 返回 `FE_ERR_DEADLOCK`。
**必须判**，否则就是自旋到超时（今天 `fe_thread_join` 会等 2000 万轮，
`sched.c:1390`），而症状是"调用者卡住两秒然后拿到一个错"——
与"明确拒绝"差一个数量级。

### 4.3 自己 join 自己的检测位置

★ 放在**机制层**，不放在 syscall 入口 ★
理由与 `12-drivers.md:158-168`（"这暴露的不是'少写一个检查'，而是**分层放错了**"）
以及它下面那条"**机制的不变式必须由机制自己维持**"逐字相同："护栏只写在 syscall 入口、
机制函数一点没查，自检直接调机制函数就一条都没挡住"。
所以 `fe_thread_join_by_handle(obj, ...)` 这个机制函数自己就判 `self == target`，
syscall 层只负责取句柄、断类型、报错准不准。

### 4.4 明确不做（逐条给"为什么"，按 §1.2.4 的判定）

| 不做 | 判定 | 为什么 |
|---|---|---|
| **`pthread_cancel`** | ★ **标准要求**，但**本项不做** ★ | 它要的是一整套新语义：取消状态（enabled/disabled/deferred）、取消类型、**清理处理器**（`pthread_cleanup_push/pop`）、以及"取消点"这个概念在**用户态**的投影。★ 内核今天有的是"线程必须死"的闸门（`kernel/sched/sched.c:532-535`）与"取消点返回 `-21`"（`kernel/ipc/ipc.c:1084-1085`）——那是**终止**语义，不是**可推迟的取消**语义。★ ★ 这是一条**要如实声明的缺口**，不许用"标准没要求"抹掉（`20-posix-kernel-requirements.md:234` 那一行的纪律）★ |
| **`pthread_attr_*` 的全部成员** | 混合 | `setstacksize`/`getstacksize`：**标准要求**，而且**内核侧今天已经有形状**——`fe_thread_create` 的第 5 个参数 `flags` 与 `stack_size` 都在（`kernel/arch/x86_64/syscall.c:116-120`），只是 `stack_size` 只能"没给栈"时才生效（`kernel/task/user.c:537-568`）。★ `setinheritsched`/`setschedpolicy`/`setprioceiling`：**标准要求接口**，但内核**没有**"用户态设优先级"的机制（`kernel/task/user.c:569-570` 把优先级写死成 `FE_PRIO_NORMAL`）——见 `20-posix-kernel-requirements.md:1004-1005` 的同族判定 ★ 归 K12 之后的独立项 |
| **`pthread_timedjoin_np` / `pthread_clockjoin_np`** | ⚠ **不是标准**（扩展） | 需要的"等待超时"是 K14（到点置位通知对象）的形状，见 `20-posix-kernel-requirements.md` §3.4 的 K14 行 |
| **`pthread_detach` 的完整语义** | ★ **标准要求** ★，**本项做**（但形态不同） | 标准规定的是"之后 `pthread_join` 必须失败、资源自动回收"。本条**做**——但形态是"关句柄 + 用户态记一个标志"（§3.3），**不是**内核加一个 detach syscall。★ 差别要写清：**"detach 之后 join 必须失败"由用户态保证**（它自己的标志），内核保证的是"没有任何 join 句柄时回收器接手" ★ |
| **线程 id 的全局唯一与可枚举** | ⚠ **不是标准要求，是实现形态** | 与 `20-posix-kernel-requirements.md:553` 对 pid 的判定同一条：标准只要求"有标识、能按它找到目标"，**没有**要求全局可枚举。★ 我们保留 `t->id`（诊断用，`kernel/task/tasklist.c` 的快照在用它）但**不把它当能力** ★ |
| **`FE_THREAD_FLAG_SUSPENDED` 的悬空位** | ⚠ **我们的实现形态** | 见 §1.3 D4。★ K12 必须选一条：实现它、或者拒绝未知位。**推荐"拒绝未知位"**（最小改动、与 `fcntl.h:4-6` 的规矩一致），实现它归将来的独立项 ★ |

### 4.5 一条必须写清楚的"不做"的理由

`pthread_cancel` 不做，**不等于**"线程杀不掉"：
`fe_task_terminate`（`kernel/include/fe/process.h:139-149`）与
`fe_task_kill_other_threads`（`:159-170`）今天都能让线程在取消点上结束。
★ 区别是：**那是"整条任务/一批线程"级的动作，不是"点名取消某一条线程并可被推迟"**。
POSIX 的 `pthread_cancel` 是后者。**把前者说成后者是"用一个更强的动作冒充一个更弱的语义"**
——超出的那部分（不可推迟、波及全任务）恰恰是标准不许可的。★

## 5. 判据（可证伪 + 反向对照）

写法仿 `11-kernel-next.md` §8 的纪律：**"自检里绝不能用'本该阻塞'的接口
去验证它没立刻返回"**——所以下面每一条反向都必须**会失败**，
而且都要写清"它证明的是哪一件事"。

### 5.1 正向

**P1**：主线程创建一条线程（入口里 `fe_exit(7)`），`join` 它。
**判据**：join 返回 **7**（不是 0），且返回后
`fe_thread_live_count()`（`kernel/include/fe/sched/thread.h:263`）与句柄表
`used`（`kernel/include/fe/object.h:111` 的 `fe_handle_table_used`）
都回到 join 之前的水平。

★ 为什么用 **7** 而不是 0 ★ `10-posix-layer.md` §9 已经用过这个手法
（`posixprobe` 的子进程退出 **42**）：**0 会"碰巧正确"**——
`exit_code` 没被搬对时，0 与"成功"无法区分。

**P2**：多线程版本——4 条线程各算一段、主线程 join 全部，
**总和精确等于**期望值（这条是 `20-posix-kernel-requirements.md:625` 的判据 ①，
本文同意并复用）。

**P3**：join 之后目标线程的**栈 / TLS 块 / FPU 区**确实被释放。
**判据**：`fe_handle_table_used` 与线程计数都回落，且新线程还能建起来
（如果栈没还，建到第 N 条会失败——★ 这是一条**容量型**判据，
它比"看计数器"更难伪造 ★）。

### 5.2 ★ 反向（至少三条；这里给六条）★

| # | 反向对照 | 期望现象 | 它证明什么 |
|---|---|---|---|
| **R1** | ★ **没退出时 join 必须阻塞** ★ 做法：目标线程 `fe_sleep_ms(300)` 之后才 `fe_exit(1)`；join 前后各读一次 `fe_clock_ns()`（`user/include/fe_user.h` 的 `fe_clock_ns`，实现在 `user/libfe/libfe.c:111-114`） | 差值 **≥ 300 ms**，且目标线程在这段时间里 **`state != FE_DEAD`** | ★ 这是最重要的一条 ★ 没有它，"join 立刻返回 7"与"join 真的等了"在输出上无法区分。**判据必须是两件事一起看**（时间差 + 目标状态），只看时间差会被"调度抖动"糊过去，只看状态则测不到"等"这个动作 |
| **R2** | **detach 过的线程 join 必须失败** 做法：`join` 之前先关掉句柄（`FE_SYS_HANDLE_CLOSE`） | 明确的错误（`FE_ERR_BADHANDLE`），**不是**一个看似合理的退出码 | 证明"可 join"这件事**确实由句柄承载**，不是"内核永远记得所有线程" |
| **R3** | ★ **join 一个已被回收的线程不许踩空** ★ 做法：对同一个句柄 join 两次 | 第二次起返回明确错误，且**不 panic、不静默返回上一次的退出码** | 防的是"join 把对象释放了、句柄表里那根指针还在"⇒ 第二次 `fe_handle_lookup` 拿到已释放内存。★ 这一类错在本项目已经出现过五次（§1.3 D6），**这一条就是第六处的守门人** ★ |
| **R4** | ★ **取消路径：join 者在等、被等的线程被 kill** ★ 做法：线程 A join 线程 B；在 B 还没退出时，让 A 所属任务被终止（或 B 被 `fe_task_kill_other_threads` 标记） | A 必须**返回**（`FE_ERR_CANCELED` 或目标已死的退出码），**不许永远睡**；且 B 的 `join_waiter` 槽**必须为空** | ★ 这一条证明"新加的第六处等待登记确实有摘除路径"（§1.3 D6）★。没有它，实现里漏掉 `fe_thread_clear_joiner` 也能通过 P1/P2/P3 与 R1/R2/R3——**而这正是 `kernel/ipc/ipc.c:1166-1173` 记着的那次实测缺陷的形状** |
| **R5** | **被杀的线程 join 回来的不是 0** | 返回值是 `FE_ERR_KILLED`（**-22**，`kernel/include/fe/errno.h:42`）；如果是被取消点唤醒的，则按 §3.4 的边界如实登记它自己的码 | 证明退出码**原样传播**，没有被"没搬对就默认 0"掩盖 |
| **R6** | ★ **目标线程所属任务已被销毁** ★ 做法：join 一条属于已退出任务的线程（句柄是经 `dup`/消息传递留下来的） | 明确的错误，**不是**永远睡、也不是解引用已释放的任务对象 | 与 `kernel/include/fe/sched/thread.h:281-300` 记的那条同类（僵尸的 `task` 悬空）在同一族；K12 不得把它复活 |

### 5.3 每条断言失败时最先看哪里

| 症状 | 最先看哪里 | 为什么 |
|---|---|---|
| `join` **立刻**返回、且码是 0 | ①`thread_mark_dead` 是否真的把 `exit_code` 抄进去了（`kernel/sched/sched.c:1215`）；②唤醒方是不是在**抄退出码之前**就唤醒了 join 者（顺序：先写 `exit_code` 再 `fe_sched_wake`，与 `fe_process_on_thread_exit` 的 `kernel/task/process.c:441-447` 同序） | ★ 顺序反了的症状就是"退出码永远是 0"，而且它看起来像"join 没等"★ |
| join **永远不返回**（卡死） | ①`fe_thread_should_die` 的取消点有没有放在循环顶（`kernel/include/fe/sched/thread.h:265-272` 是唯一判据）；②唤醒方是不是漏了"有人等就唤醒"这一步；③★ 目标线程是不是**已经死了但没唤醒任何人** ★（`thread_mark_dead` 是唯一登记死亡的地方，`kernel/sched/sched.c:1178`） | 这四种都表现为"卡住"，但修法完全不同——所以判据要把"目标状态"一起打出来 |
| 随机的 `#PF` / panic，且 `CR2` 指向堆地址 | 先查 §4.4 的 C1（取消路径）与 R3（重复 join），再查 F2（引用计数） | 与 `kernel/sched/sched.c:1194-1196` 记的"`rq_pick` 选中已释放内存"是同族；★ 这一族在本项目已经出现过五次，症状永远是"随机崩"而不是"报错"★ |
| 建到第 N 条线程就失败 | ①句柄表满（`FE_HANDLE_TABLE_SIZE = 256`，`kernel/include/fe/object.h:47`）；②栈没还（`thread_free` 的 `kernel/sched/sched.c:1304-1307`）；③VMA 区间满（`FE_VMA_MAX`，见 `17-libcxx-build.md` §4.1） | 三条路都会"建不出新线程"，**必须分开报**（★ 否则又是一个"报错指向错误的原因"★） |
| 任务销毁时打印"**在还有 N 个存活线程时被销毁**" | `kernel/task/task.c:47-52` 那句诊断——它意味着**引用计数出错**（每个存活线程持有一个任务引用，`kernel/sched/sched.c:441-443`） | ★ 这条诊断是 K12 最容易碰响的一条：线程对象多了一份句柄引用之后，"谁放掉对任务的那次引用"变成了一个必须回答的问题 ★ |

## 6. 分刀

★ 三刀，每刀一个**可证伪的终点**，而且**都能在 QEMU 里验**（不必等真机）。
分刀的理由与 `20-posix-kernel-requirements.md:1158-1163` 一致：
**改的面越小，出错时的归因越清楚**。★

### K12a：线程变成句柄对象（**不新增任何等待语义**）

| 项 | 内容 |
|---|---|
| **终点** | `fe_thread_create` 返回一个**线程句柄**（放进调用者的句柄表），`FE_OBJ_THREAD` 真正被用起来；线程退出后对象**不再由调度器无条件回收**，而是"最后一个引用放掉时销毁"；新增 `FE_RIGHT_JOIN` |
| **判据** | ① 句柄能被 `FE_SYS_HANDLE_CLOSE` 关掉，关掉之后线程照常跑完、照常被回收（**不泄漏**）；② 建 N 条线程再用掉 N 个句柄，句柄表 `used` 的增删**精确对应**；③ 既有判据零退化（★ 这是硬要求：`01-milestones.md` 的 25 条汇总行必须仍然全 0 ★） |
| **反向** | 未知 `flags` 位必须被**拒绝**（§1.3 D4）；不关句柄而线程跑完 ⇒ **不回收到"漏僵尸"**（这正是今天 `reap_ok=true` 在防的那件事，`kernel/task/user.c:571-576`） |
| **失败时最先看哪里** | `thread_free` / `fe_object_destroy` 两处（销毁路径双份 = 双重释放；缺一处 = 永不销毁） |

### K12b：`join` / `detach`

| 项 | 内容 |
|---|---|
| **终点** | 用户态能阻塞到一个线程退出并取回**原样**的退出码；`detach`（= 关句柄）之后 join 明确失败 |
| **判据** | P1 / P2 / P3 + R1 / R2 / R3 / R5 / R6 |
| **反向** | R1（阻塞）、R2（detach 后失败）、R3（重复 join 不踩空） |
| **失败时最先看哪里** | 检查-登记-阻塞三件事是否在**同一个关中断区间**里（`kernel/task/process.c:390-393` 的注释就是这条的说明）；`FE_ERR_DEADLOCK` 是否判在机制层（§4.2/§4.3） |
| **依赖** | K12a |

### K12c：取消路径与销毁路径的收口

| 项 | 内容 |
|---|---|
| **终点** | 新加的等待登记在**取消 / 终止 / 任务销毁**三条路径上都被摘掉；`fe_thread_cancel` 从五条路径变成**六条**（`kernel/ipc/ipc.c:1103` 那个函数的注释必须跟着改——★ 它自己写着"将来若有第四种等待登记，必须同时加一条取消路径"，`kernel/ipc/ipc.c:1101-1102` ★） |
| **判据** | **R4**（join 者在等、被等的线程被 kill）+ 任务销毁路径上的"没有死指针残留"断言 |
| **反向** | 故意**不**加那条摘除路径 ⇒ 该断言必须**红**（★ 这是本项目一贯的"先红后绿"做法，`01-milestones.md:27` 记着 M19 的四刀都是这么验的 ★） |
| **失败时最先看哪里** | `fe_thread_cancel` 的五条现有路径（`kernel/ipc/ipc.c:1128` / `:1133` / `:1151` / `:1164` 以及路径 1）逐条对照，看第六条有没有落在同一个关中断区间里、且在唤醒**之前**做（`:1082` 的纪律："**先摘除登记，再唤醒**"） |
| **依赖** | K12b |

★ 为什么 K12c 单独一刀 ★ 它与 K12a/K12b 的**失败症状完全不同**：
a 的错是"泄漏或双重释放"，b 的错是"卡住或拿到错码"，c 的错是**随机 use-after-free**。
合成一刀会让"失败时最先看哪里"失效——★ 这与 `20-posix-kernel-requirements.md:1158-1163`
把 K11/K12 拆开的理由是同一条 ★。

## 7. 代价清单（一句话版）

- **一个已经挖好、还没插线的槽位被用起来**：`FE_OBJ_THREAD` 已在枚举里
  （`kernel/include/fe/object.h:24`），但全项目只有枚举定义与名字表两处
  （`kernel/object/object.c:19`）。K12a 是把它插上线。
- **一个假设要被换成事实**：`reap_ok` 今天编码"内核里没有人 join 它"
  （`kernel/include/fe/sched/thread.h:66-85`），而用户线程被**无条件**设成 `true`
  （`kernel/task/user.c:571-576`）。K12 之后"还能不能被 join"由**句柄在不在**回答。
- **一处销毁路径要收敛**：今天 `thread_free` 直接 `fe_kfree(t)`
  （`kernel/sched/sched.c:1319`），加了引用计数之后**必须**改走 `fe_object_unref`，
  并在 `fe_object_destroy` 里补 `case FE_OBJ_THREAD`
  （那个 switch 在 `kernel/task/task.c:21-115`，今天没有这一支）。
  ★ 两个方向的错都不报错：漏了是慢漏，多做一次是随机崩。★
- **一处等待登记要从五条变六条**：`fe_thread_cancel`（`kernel/ipc/ipc.c:1103`）
  今天摘五处（`:1128`/`:1133`/`:1151`/`:1164` 与路径 1），
  而它自己的注释（`:1101-1102`）就写着"将来若有第四种等待登记，必须同时加一条取消路径"。
  ★ `docs/13-tasks-and-kill.md:186` 那一行当时的预言**已经应验过一次**，K12 是第二次。★
- **两个系统调用号**：`docs/21-user-address-wait.md:243-247` 提议 K11 用
  `0x94`/`0x95`、`FE_SYS_MAX` → `0x96`。
  ★ **但 `0x94` 已经被占用了** ★ —— `kernel/include/fe/syscall.h:363` 今天就是
  `FE_SYS_MAX = 0x94`（`0x92`/`0x93` 归 K5，见 `:360-361`），
  所以 K11 实际能取的是 **`0x95`/`0x96`**，K12 顺延到 **`0x97`/`0x98`、
  `FE_SYS_MAX` → `0x99`**。
  ★ 本文**不替 `docs/21` 改号**（那不是我的文件），只把这条不一致如实记进 §8 待查 J2 ★
- **一条被顺手堵上的口子**：`FE_THREAD_FLAG_SUSPENDED` 今天定义了却没人读
  （`kernel/include/fe/syscall.h:29`，全树 1 处命中），
  而 `kernel/task/user.c:532` 是 `(void)flags;`——**用户态传什么 flag 都被静默接受**。
- **一条已经过期的注释要被顺手改掉**：`kernel/include/fe/process.h:154` 写
  "2,000,000"，而实现是 **20,000,000**（`kernel/sched/sched.c:1390`）。
- **明确不做的三件（标准里的）**：`pthread_cancel`（要一整套用户态取消语义）、
  `pthread_attr_*` 里与调度策略相关的部分（内核没有"用户态设优先级"的机制）、
  以及"线程 id 全局可枚举"（**不是标准要求**，是实现形态——同 `20` §2.5 对 pid 的判定）。
   ★ 前两条要如实记成**缺口**，不许用"标准没要求"抹掉（`20-posix-kernel-requirements.md:234`）★
- **本文一行代码都不动**，**没有一个实测数字**；§8 列了若干条待查，每条都写了"怎么查"。

## 8. 待查清单

★ 每一条都写"怎么查" ★ 查不到的**不猜、不编数字**。

| # | 待查 | 怎么查 |
|---|---|---|
| **J1** | ★ **`pthread_join` / `pthread_detach` 的返回值与错误码，逐条核对 POSIX 规范条文** ★ 本文 §4.1 的三条（`EINVAL`/`ESRCH`/`EDEADLK`）是**我按常识写的**，**没有打开规范原文核对**——它们必须逐条对上 | 查 POSIX（IEEE 1003.1 / ISO 9945）里 `pthread_join` 与 `pthread_detach` 的 **ERRORS** 一节。★ 判定要求是"规范条文怎么写的"，不是"某个实现怎么做"（`20-posix-kernel-requirements.md` §1.2.4/§1.2.5）★。**实现前必须做**，因为错误码是 ABI |
| **J2** | ★ **K12 的两个 syscall 号，以及 `docs/21` 的号与今天的 `FE_SYS_MAX` 不一致** ★ 今天 `kernel/include/fe/syscall.h:363` 是 `FE_SYS_MAX = 0x94`（`0x92`/`0x93` 归 K5，`:360-361`），而 `docs/21-user-address-wait.md:243-247` 提议 K11 用 `0x94`/`0x95`——**`0x94` 已被占用** | 等 K11 落地后读 `kernel/include/fe/syscall.h` 的**实际**枚举，再定 K12。★ 顺带把 `docs/21` 那个提议里的 `0x94` 改正（**本文不改别的文件**）★ 不许照抄本文的推测 |
| **J3** | ★ **`FE_RIGHT_ALL = 0x3FFu`（`kernel/include/fe/object.h:71`）是不是被某处当成"所有权限"用了** ★ 如果新增 `FE_RIGHT_JOIN`（bit 10）而 `ALL` 没跟着改，就会出现"某条路径给的句柄少了这一位"或反过来 | 全树搜 `FE_RIGHT_ALL` 的每一个使用点，逐个回答"这里该不该包含 JOIN"。★ 若某处是"父任务给子任务装句柄"，答案很可能是**不该**（子任务不该能 join 父任务创建的线程）★ |
| **J4** | **`fe_thread` 加对象头之后有没有"裸偏移"依赖** | 全树搜 `struct fe_thread` 的使用，找有没有 `+ N` / 强制转换 / 汇编里按偏移访问它的地方。★ 已知安全的两处：调度切换只用 `t->rsp`（`kernel/include/fe/sched/thread.h:46-48`）、`assembler` 不碰线程结构 ★ 但这一条要**逐一**确认，不能凭本文的两处就下结论 |
| **J5** | **`fe_object_destroy` 的 `FE_OBJ_THREAD` 分支该做什么** | 对照 `case FE_OBJ_TASK`（`kernel/task/task.c:36-110`）逐项问"线程有没有这一项"（句柄表？没有；地址空间？没有——那是任务的；栈/TLS/FPU？有；对任务的引用？有，`kernel/sched/sched.c:441-443`）。★ 漏掉"对任务的引用"那一项的症状是任务对象永不销毁 ★ |
| **J6** | **`pthread_t` 在用户态的形状**（句柄号？id？）与 `pthread_self`、`pthread_equal` 的语义 | 读 `10-posix-layer.md` §5 的分层（内核给机制、用户态给语义），再按 J1 的规范条文决定。★ 本文的建议是"句柄号"（句柄是唯一的能力载体），但**这是建议不是结论** ★ |
| **J7** | **`detach` 标志存在用户态之后，"已 detach 的 id 被复用"怎么办** | 这是纯用户态问题（一张 id→标志的表），但**必须写进用户态的文档**：`FE_HANDLE_INVALID = 0`、句柄 0 恒无效（`kernel/include/fe/syscall.h:18-20`），而句柄号会被复用（`fe_handle_install` 扫第一个空槽，`kernel/object/object.c:68-77`）⇒ **旧的 `pthread_t` 可能撞上新的线程句柄**。★ 判据：detach 过的 `pthread_t` 再 join **必须失败**（§5.2 R2）★ |
| **J8** | **K12 与 K11 的先后** | `20-posix-kernel-requirements.md:1127` 写的是"第四步：K12（`pthread_join`）"在"第三步 K11（等一个用户地址）"之后。★ 按本文的结论，K12**不依赖** K11（join 不需要用户地址等待）——所以这条顺序是**优先级**还是**依赖**，要回去核对 `20` §3.2 的原意 ★ |
