/* SPDX-License-Identifier: 0BSD */
/* 进程模型的实现（见 process.h 的设计说明）。
 *
 * 这个文件承担三件事：
 *   1. 把 Linux 的初始栈约定摆出来（argc/argv/envp 全在用户栈上）；
 *   2. 按路径装载程序并造进程（等价于 clone + execve）；
 *   3. wait 的同步（等价于 wait4，但认句柄不认 pid）。
 */
#include <fe/process.h>
#include <fe/vfs.h>
#include <fe/ramfs.h>
#include <fe/task.h>
#include <fe/user.h>
#include <fe/object.h>
#include <fe/ipc.h>
#include <fe/sched/sched.h>
#include <fe/sched/thread.h>
#include <fe/mm.h>
#include <fe/mm/vmm.h>
#include <fe/mm/kheap.h>
#include <fe/string.h>
#include <fe/boot/bootinfo.h>
#include <fe/kprintf.h>
#include <fe/resource.h>        /* fe_resource_note_init：谁是引导者 */
#include <fe/io.h>
#include <fe/protect.h>

/* ------------------------------------------------------------------ */
/* 初始栈（Linux x86-64 约定）                                          */
/* ------------------------------------------------------------------ */

/* 往新任务的栈上压一个 8 字节值。这里不能直接用 memcpy 到 &sp —— 
 * sp 是内核栈上的局部变量，要写的是**用户**地址空间。 */
static fe_status_t push8(struct fe_address_space *as, u64 *sp, u64 value)
{
    *sp -= 8;
    return fe_user_write_space(as, *sp, &value, 8);
}

static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') {
            b = p + 1;
        }
    }
    return (*b) ? b : path;
}

/* 构造初始栈，返回用户态的初始 rsp。
 *
 * 布局（从高地址到低地址）：
 *     "argv[0]\0" "argv[1]\0" ...        ← 字符串在最高处
 *     （16 字节对齐 + 可能的 8 字节填充）
 *     NULL                               ← envp 终止（目前环境为空）
 *     NULL                               ← argv 终止
 *     argv[n-1] ... argv[0]
 *     argc                               ← rsp 指向这里
 *
 * 最终 rsp 一定 16 字节对齐：用户态 _start 直接 and rsp,-16 也不会破坏布局，
 * 而按 SysV ABI，call 之前 rsp 必须 16 字节对齐。 */
static fe_status_t build_initial_stack(struct fe_address_space *as, const char *path,
                                       char *const argv[], u32 argc, u64 *out_sp)
{
    if (argc > FE_ARGV_MAX) {
        return FE_ERR_RANGE;
    }
    u64 sp = FE_USER_STACK_TOP;
    u64 ptr[FE_ARGV_MAX + 1];
    u32 n = 0;

    for (u32 i = 0; i < argc; i++) {
        const char *s = (argv && argv[i]) ? argv[i] : "";
        u64 len = strlen(s) + 1;
        if (len > FE_ARG_MAX) {
            return FE_ERR_NAMETOOLONG;
        }
        sp -= len;
        fe_status_t st = fe_user_write_space(as, sp, s, len);
        if (fe_failed(st)) {
            return st;
        }
        ptr[n++] = sp;
    }
    /* 与 Linux 一致：没给 argv 时，程序自己的路径充当 argv[0] */
    if (n == 0) {
        u64 len = strlen(path) + 1;
        if (len > FE_ARG_MAX) {
            return FE_ERR_NAMETOOLONG;
        }
        sp -= len;
        fe_status_t st = fe_user_write_space(as, sp, path, len);
        if (fe_failed(st)) {
            return st;
        }
        ptr[n++] = sp;
    }

    sp &= ~0xFull;
    /* 指针区一共 n + 3 个 8 字节字（argv 指针 + 两个 NULL + argc），
     * 要让它也落在 16 字节边界上：n + 3 为偶数即可，即 n 为奇数。 */
    if ((n & 1u) == 0) {
        sp -= 8;
    }
    fe_status_t st = push8(as, &sp, 0);      /* envp 终止符（环境目前为空） */
    if (fe_failed(st)) {
        return st;
    }
    st = push8(as, &sp, 0);                  /* argv 终止符 */
    if (fe_failed(st)) {
        return st;
    }
    for (u32 i = n; i-- > 0;) {
        st = push8(as, &sp, ptr[i]);
        if (fe_failed(st)) {
            return st;
        }
    }
    st = push8(as, &sp, (u64)n);             /* argc */
    if (fe_failed(st)) {
        return st;
    }
    *out_sp = sp;
    return FE_OK;
}

/* ------------------------------------------------------------------ */
/* 候选现场：把"装载一个映像"与"把它挂到任务上"分开                    */
/* ------------------------------------------------------------------ */

/* 按路径找到可执行的映像。三种错因给三种错误码——混成一个的话，
 * 用户态排查只能靠猜（「文件不存在」「那是个目录」「不是可执行映像」）。
 * create 与 exec 共用它，所以这条规矩只有一份。 */
fe_status_t fe_image_lookup(const char *path, struct fe_inode **out_ino)
{
    if (!path || !out_ino) {
        return FE_ERR_INVAL;
    }
    struct fe_inode *ino = fe_vfs_lookup(path);
    if (!ino) {
        return FE_ERR_NOENT;
    }
    if (fe_vfs_is_dir(ino)) {
        return FE_ERR_ISDIR;
    }
    if (!fe_vfs_is_exec(ino)) {
        /* 与 Linux 的 ENOEXEC 同义：文件在，但不是可执行的映像 */
        return FE_ERR_NOTSUP;
    }
    *out_ino = ino;
    return FE_OK;
}

/* 准备一个候选现场（见 fe/process.h 的设计说明）。
 *
 * ★ 它不碰任何任务对象 ★ as/vmas 由调用者给：新建进程用新任务自己的那一对
 * （那个任务还没被任何人看见，改坏了销毁即可）；exec 自己新建一对，
 * 好让失败时能整个扔掉、原程序毫发无损。
 *
 * ★ 失败时半成品留在 as/vmas 里 ★ 由调用者按自己的所有权规则回收
 * （新建进程：销毁那个任务；exec：销毁候选空间与区间表）。 */
fe_status_t fe_image_prepare(struct fe_new_image *img,
                             struct fe_address_space *as, struct fe_vma_table *vmas,
                             struct fe_inode *ino, const char *path,
                             char *const argv[], u32 argc)
{
    if (!img || !as || !ino || !path) {
        return FE_ERR_INVAL;
    }
    img->as = as;
    img->vmas = vmas;
    img->stack_low = 0;
    img->user_map_next = FE_USER_MMAP_BASE;
    img->user_stack_next = FE_USER_STACK_AREA_TOP;
    img->entry = 0;
    img->sp = 0;
    img->tls_init = NULL;
    img->tls_size = 0;
    img->tls_align = 0;
    strlcpy(img->name, base_name(path), FE_TASK_NAME_MAX);

    fe_status_t st = fe_user_map_stack(as, vmas, FE_USER_STACK_TOP,
                                       FE_USER_STACK_SIZE, &img->stack_low);
    if (fe_failed(st)) {
        return st;
    }

    struct fe_elf_image_info ei;
    st = fe_elf_load(as, vmas, fe_vfs_data(ino), fe_vfs_size(ino), &ei);
    if (fe_failed(st)) {
        return st;
    }
    img->entry = ei.entry;
    img->tls_init = ei.tls_init;
    img->tls_size = ei.tls_size;
    img->tls_align = ei.tls_align;

    st = build_initial_stack(as, path, argv, argc, &img->sp);
    if (fe_failed(st)) {
        return st;
    }
    return FE_OK;
}

/* 提交：把候选现场挂到任务上。
 *
 * ★ 它是一个"赋值"而不是"换一个" ★ 旧空间/旧区间表的释放由调用者自己做，
 * 而且必须在**换完 CR3 之后**做（exec 那边顺序错了就是"释放正在跑的页表"，
 * 症状见 sched.c 里那段 K2/K3 的注释）。这里只赋值，正是为了让那个顺序
 * 由调用者显式掌握、而不是藏在函数里。
 *
 * ★ 一次挂全，不许逐项写 ★ 漏掉一项的症状各不相同且都很难查：
 *   vmas            → 新程序第一次 #PF 时区间表是旧的（该补的不补）
 *   stack_low       → 栈长不动，或一长就越过保留区
 *   user_map_next   → 新程序第一次 mmap 落进旧程序的地址范围
 *   tls_*           → 新建线程从别人的映像里拷 TLS 模板 */
void fe_task_attach_space(struct fe_task *t, const struct fe_new_image *img)
{
    if (!t || !img) {
        return;
    }
    t->space = img->as;
    t->vmas = img->vmas;
    t->stack_low = img->stack_low;
    t->user_map_next = img->user_map_next;
    t->user_stack_next = img->user_stack_next;
    t->tls_init = img->tls_init;
    t->tls_size = img->tls_size;
    t->tls_align = img->tls_align;
}

/* ------------------------------------------------------------------ */
/* 创建与等待                                                          */
/* ------------------------------------------------------------------ */

struct fe_task *fe_process_create(const char *path, char *const argv[], u32 argc,
                                  fe_status_t *out_status, struct fe_thread **out_thread,
                                  bool boot_init)
{
    fe_status_t st = FE_OK;
    if (out_thread) {
        *out_thread = NULL;
    }

    struct fe_inode *ino = NULL;
    st = fe_image_lookup(path, &ino);
    if (fe_failed(st)) {
        goto fail;
    }

    struct fe_task *t = fe_task_create_user(base_name(path));
    if (!t) {
        st = FE_ERR_NOMEM;
        goto fail;
    }

    /* 准备 → 提交。这里两步紧挨着（没有可能失败的第三方介入），
     * 但结构上与 exec 完全一致：装载的产物先落进 img，再由 attach 挂上。 */
    struct fe_new_image img;
    st = fe_image_prepare(&img, t->space, t->vmas, ino, path, argv, argc);
    if (fe_failed(st)) {
        goto fail_task;
    }
    fe_task_attach_space(t, &img);

    u64 entry = img.entry;
    u64 sp = img.sp;
    const char *name = img.name;

    /* ★ 「谁是引导者 init」必须在**线程存在之前**记下 ★
     *
     * 这一条原来写在调用方（boot_init_task）里，位置是 fe_process_create
     * 返回之后——而新线程在返回之前就已经在就绪队列上了（rq_push），
     * 所以那是一个竞态。QEMU 上恰好从没撞上，VirtualBox 上**每次**撞上：
     * 新任务先跑，认领设备管理器身份时内核还不知道它是 init，被拒；
     * 接着它去读 PCI 配置空间端口（还没认领）→ #GP → 整个用户态没起来。
     *
     * 记在这里，是因为这里正是"接下来要跑的就是它、而且它还没开始跑"
     * 的那个时刻——与上面设 %fs 是同一条理由。 */
    if (boot_init) {
        fe_resource_note_init(t->id);
    }

    struct fe_thread *th = fe_thread_create_ring3(t, entry, sp, NULL, 32 * 1024,
                                                  FE_PRIO_NORMAL, name);
    if (!th) {
        st = FE_ERR_NOMEM;
        goto fail_task;
    }
    t->main_thread = th;
    /* ★ 谁来给这个进程的主线程收尸 ★
     *
     * 不是 init 的进程：它的退出由**父进程的 `fe_process_wait`** 观察
     * （看 task->exited 那一位），**没有任何人会去 join 那个线程**。
     * 所以它的僵尸可以、也必须由调度器回收——否则那次对任务的引用
     * 永远不放，任务对象永远不销毁，端口 / MMIO / IRQ / devfs 名字 /
     * 地址空间全部泄漏（K2/K3 那一轮实测：跑完的程序全都留在任务表里）。
     *
     * init 自己不行：引导线程就在 `fe_thread_join` 里等它，回收器
     * 先一步释放会让 join 踩空。所以 boot_init 那条路保持默认（不回收）。 */
    th->reap_ok = !boot_init;
    /* ★ 主线程的 TLS 基址也要立刻生效 ★
     *
     * 调度器会在**切换**时设 `%fs`，但主线程是被 `fe_thread_join` 等待、
     * 由当前执行流"交出去"才第一次跑的——在它跑起来之前的这段窗口里，
     * `%fs` 还是上一个线程的值。而这里（新任务刚建好）正是我们唯一
     * 能确定"接下来跑的就是它"的时刻，所以顺手设一次：
     * 设成 0 也行，但设成正确值能消掉一类"第一次访问 TLS 用了别人的基址"
     * 的间歇性错误（它只在恰好有别的用户线程在跑时才会出现）。
     *
     * 注意：写 MSR 是**当前 CPU** 的操作，而此刻它即将去跑这个线程——
     * 这个前提成立，因为创建者与将要执行者是同一个执行流（spawn 是同步的）。 */
    fe_wrmsr(FE_MSR_FS_BASE, th->user_fs_base);
    /* 告诉保护模块：又有一个进程起来了。
     * 如果它的路径正好是清单里指定的更新器，内核就此记下它的任务 id——
     * **"谁能突破保护"只能由拿着清单的那一方决定**，不能由进程自报。 */
    fe_protect_note_process(path, t->id);
    if (out_thread) {
        *out_thread = th;
    }
    if (out_status) {
        *out_status = FE_OK;
    }
    return t;

fail_task:
    /* 记了就要撤回：上面提前记下的那个 id 属于一个马上要被销毁的任务，
     * 留着它会让"引导者"指向一个不存在的任务（而 init 的 id 会被复用）。 */
    if (boot_init) {
        fe_resource_note_init(0);
    }
    fe_object_unref(&t->hdr);       /* 失败路径不能把半成品任务留在全局任务链上 */
fail:
    if (out_status) {
        *out_status = st;
    }
    return NULL;
}

fe_status_t fe_process_spawn(struct fe_task *parent, const char *path,
                             char *const argv[], u32 argc, fe_handle_t *out_handle)
{
    if (!parent || !out_handle) {
        return FE_ERR_INVAL;
    }
    fe_status_t st = FE_OK;
    struct fe_task *child = fe_process_create(path, argv, argc, &st, NULL, false);
    if (!child) {
        return st;
    }
    /* 把任务对象装进父进程的句柄表。装句柄会加一次引用，
     * 因此随后要放掉「创建者引用」，否则任务永远不会被销毁。
     *
     * ★ 权限里带 TERMINATE ★ 拉起者可以终止它拉起的东西——
     * 这就是"系统自管"需要的形状：init 拉起服务，服务跑飞了就终止它。
     * 不给的话，"杀"这个能力只能由内核或某个特权服务代发，
     * 而那等于把"谁该被杀"这件事从拉起者手里拿走。 */
    fe_handle_t h = fe_handle_install(&parent->handles, &child->hdr,
                                      FE_RIGHT_READ | FE_RIGHT_WAIT |
                                      FE_RIGHT_DUP | FE_RIGHT_TRANSFER |
                                      FE_RIGHT_TERMINATE);
    fe_object_unref(&child->hdr);
    if (h == FE_HANDLE_INVALID) {
        return FE_ERR_NOMEM;
    }
    *out_handle = h;
    return FE_OK;
}

fe_status_t fe_process_wait(struct fe_task *parent, fe_handle_t task_handle,
                            i32 *out_status)
{
    if (!parent) {
        return FE_ERR_INVAL;
    }
    struct fe_object_header *obj = NULL;
    fe_status_t s = fe_handle_lookup(&parent->handles, task_handle, FE_RIGHT_WAIT, &obj);
    if (fe_failed(s)) {
        return s;
    }
    if (obj->type != FE_OBJ_TASK) {
        return FE_ERR_INVAL;
    }
    struct fe_task *child = FE_OBJ_OF(obj, struct fe_task);

    /* 与通知对象的等待同一套模式：检查 → 登记 waiter → 阻塞。
     * 条件检查与登记 waiter 必须在同一个关中断区间里完成，
     * 否则「检查完发现没退出，正准备阻塞」与「对方刚好在这一刻退出并唤醒」
     * 会互相错过，调用者就永远睡下去了。 */
    for (;;) {
        u64 flags = fe_irq_save();
        if (child->exited) {
            i32 code = child->exit_code;
            fe_irq_restore(flags);
            if (out_status) {
                *out_status = code;
            }
            return FE_OK;
        }
        child->waiter = fe_thread_current();
        fe_irq_restore(flags);
        fe_sched_block_current();
    }
}

void fe_process_on_thread_exit(struct fe_thread *t)
{
    if (!t || !t->task) {
        return;
    }
    struct fe_task *task = t->task;
    /* 只有主线程退出才代表进程结束。多线程服务里某个工作线程退出
     * 不该让父进程的 wait 返回。 */
    if (t != task->main_thread) {
        return;
    }
    task->exit_code = t->exit_code;
    task->exited = true;
    struct fe_thread *w = task->waiter;
    if (w) {
        task->waiter = NULL;
        fe_sched_wake(w);
    }
}

/* ------------------------------------------------------------------ */
/* 终止一个任务（K2）                                                  */
/* ------------------------------------------------------------------ */

/* ★ 叫停本任务里除 keep 之外的每一个线程（2a：闸门扩到线程级）★
 *
 * `docs/13-tasks-and-kill.md` §6.2。它做两件事，然后立刻返回
 * （**异步**，与 fe_task_terminate 同一套语义）：
 *   1. 遍历本任务的线程，跳过 keep，对每一个：`kill_pending = true`
 *      → 它在 BLOCKED/SLEEPING 就调既有的 `fe_thread_cancel`；
 *   2. 返回被标记的线程数。
 *
 * ★ 遍历与"置位 + 摘登记 + 唤醒"必须在同一个关中断区间里做完 ★
 * 原来的 fe_task_terminate 是 `restore → 调 fe_thread_cancel → save`，
 * 也就是循环中途把中断放开。那在这类循环里是一个**真实的窗口**：
 * 被 fe_sched_wake 叫醒的受害者一旦被抢占，就可能跑到闸门、变成僵尸、
 * 被空闲线程回收，而循环手里还握着它的指针（`fe_thread_next_of(t)`
 * 要读 `t->all_next`）——use-after-free。
 * 关中断区间可以嵌套：fe_thread_cancel / fe_sched_wake 自己都是
 * `fe_irq_save` + `fe_irq_restore`，恢复的是"进来时的 IF"，不会提前开中断。
 *
 * ★ 就绪（或在跑）的线程什么都不用做 ★ 它们下次回用户态时会被闸门拦下
 * （`fe_sched_maybe_switch` 里的 `fe_thread_should_die`）——这是唯一安全的
 * 方式：它可能正握着内核里一半的状态（比如刚做完一次拷贝还没返回）。
 *
 * ★ keep 必须是本任务的主线程（task->main_thread）★
 * 它正是"换完映像之后接着跑"的那个线程。把它标记了，它下一次经过闸门
 * 就死了（闸门对谁都成立），而 exec 的提交要在**它自己的返回帧**上写新入口。
 * 今天的保证来自 `15-exec.md` §7 的 `FE_ERR_INVAL`（调用者不是主线程就拒绝）；
 * 要不要放宽这条限制，取决于"主线程退出即进程结束"这条语义要不要动
 * （`fe_process_on_thread_exit` 只看主线程）——那是同一件事的两面。 */
u32 fe_task_kill_other_threads(struct fe_task *task, struct fe_thread *keep)
{
    if (!task) {
        return 0;
    }
    u32 marked = 0;
    u64 flags = fe_irq_save();
    for (struct fe_thread *t = fe_task_thread_first(task); t;
         t = fe_thread_next_of(t)) {
        if (t == keep) {
            continue;
        }
        t->kill_pending = true;
        marked++;
        if (t->state == FE_THREAD_BLOCKED || t->state == FE_THREAD_SLEEPING) {
            /* 阻塞/睡眠中的线程自己走不到闸门，必须把它们摘下来 + 叫醒。 */
            fe_thread_cancel(task, t);
        }
    }
    fe_irq_restore(flags);
    return marked;
}

/* 等"别的线程都死透"（§6.4）。返回 FE_OK = 死透了，FE_ERR_TIMEOUT = 超时。
 *
 * ★ 判据是 `fe_task_thread_count(task) <= 1`，不是"我手里那个指针的 state" ★
 * 受害者的 `reap_ok` 是 true（用户建的线程），空闲线程会在任意一次让出
 * 之后把它 free —— 裸指针立刻悬空。`fe_task_thread_count` 每轮从链头
 * 重新扫（DEAD 的被跳过、已回收的被摘链），所以只要不在两轮之间持有指针，
 * 就没有 use-after-free。
 *
 * ★ 为什么 `<= 1` 而不是 `== 0` ★ 调用者（keep）自己还活着，它就是那一个。
 *
 * ★ 为什么让出（yield）而不是睡眠 ★ 等待的是"别人被调度"：受害者会被选中
 * →走到取消点/闸门→死；空闲线程也会被选中→顺手收尸。让出就是"把 CPU
 * 交出去一次"，不需要时钟参与。
 *
 * ★ 超时**绝不 panic** ★ 这条路径用户态可触发（只要有一个线程卡在
 * 没有取消点的等待上，或者睡在一个很长的 SLEEP 上），内核 panic 等于
 * 把它变成 DoS。代价必须说清：**kill_pending 不撤回**——已经发出的
 * "必须死"标记没有回滚接口，进程会带着"少了一部分线程"继续跑。 */
u32 fe_task_wait_others_dead(struct fe_task *task, u32 max_rounds)
{
    if (!task) {
        return FE_OK;
    }
    for (u32 i = 0; i < max_rounds; i++) {
        if (fe_task_thread_count(task) <= 1) {
            return FE_OK;
        }
        fe_thread_yield();
    }
    return FE_ERR_TIMEOUT;
}

/* ★ 超时日志：直接指认凶手（只在上面那条超时路径上跑）★
 *
 * 内容是 `name` / `id` / `state`（**数值**！`fe_thread_state_name()` 返回
 * 中文，而屏幕上打印的字符串只能用 ASCII —— 这是本项目的铁律）/
 * 它登记的等待点（`fe_thread_wait_site`）。 */
void fe_task_dump_stuck_threads(struct fe_task *task, struct fe_thread *keep)
{
    if (!task) {
        return;
    }
    u32 n = fe_task_thread_count(task);
    fe_kprintf("[exec] kill timeout: %u thread(s) still alive\n",
               n ? n - 1 : 0);
    for (struct fe_thread *t = fe_task_thread_first(task); t;
         t = fe_thread_next_of(t)) {
        if (t == keep) {
            continue;
        }
        fe_kprintf("[exec]   stuck: name=%s id=%llu state=%u wait=%s\n",
                   t->name, (unsigned long long)t->id, t->state,
                   fe_thread_wait_site(t));
    }
    /* ★ 最后一行是这段日志存在的理由 ★ 返回 FE_ERR_TIMEOUT 之后进程已经
     * **残缺**：线程数少了一部分，而且被标记的那些会在下一次经过闸门时
     * 接着死。调用者不能再把它当完整的用。 */
    fe_kprintf("[exec]   the process is now INCOMPLETE (marked must-die,"
               " no rollback)\n");
}

/* ★ 自检的探针收尾：把一组探针叫停、等它们死透，并如实报告清不掉的那些 ★
 *
 * ★ 为什么自检必须做收尾（这是纪律，不是礼貌）★
 * 自检造出来的探针线程如果留在系统里继续跑，会同时污染两件事：
 *   1. **后续的自检**（线程表、节拍统计、就绪队列的形态全都被它们影响）；
 *   2. **"线程比任务活得久"这类场景**——一个永久自旋/永久阻塞的探针，
 *      在它所属的任务被销毁之后仍然活着，它的 task 字段就成了悬空指针。
 * 所以：跑完就把它们标记杀掉、等它们死透。
 *
 * ★ 为什么会有"清不掉"的 ★ 取消是**协作式**的（线程只在取消点上死，
 * 见 docs/13-tasks-and-kill.md §3.2/§3.3）。今天有两类等待**没有取消点**：
 *   - 睡在很长的 SLEEP 上（`fe_sched_wake` 还不叫醒 SLEEPING，§6.3（2））；
 *   - 卡在共享区间的控制器锁上（`fe_resource_lock` 循环顶没有判据，§6.3 表第 4 行）。
 * 它们要等对应的修复落地才清得掉。**如实点名打出来**，不要假装清干净了
 * ——"清不掉"本身是一条要被看见的事实。
 *
 * 返回清不掉的探针数（0 = 全清干净）。 */
u32 fe_task_cleanup_probes(struct fe_task *task, const char *tag)
{
    if (!task) {
        return 0;
    }
    /* ★ 不变式：**"清场"这个动作的语义里永远不该包含调用者** ★
     *
     * `keep` 传 `NULL` 在 `fe_task_terminate` 那里是**对的**（整个任务完蛋，
     * 连主线程一起走）；在这里是**错的**——我们只是清掉自检造出来的探针，
     * 调用者（`main`）还要接着把自检跑完。
     *
     * 为什么这一条必须写成断言而不是"记得传对参数"：调用者一旦被标记，
     * 它**下一次经过闸门**就会被就地判死、摘出就绪队列，而症状是
     * "一次 yield 切走、永远回不来"——那与我们追了三个小时的挂死
     * 一模一样。这个错误在这里犯过一次（原来传的是 `NULL`），
     * 所以留一条断言把它钉住。 */
    struct fe_thread *self = fe_thread_current();
    u32 marked = fe_task_kill_other_threads(task, self);
    if (self && self->kill_pending) {
        fe_kprintf("        **收尾把自己标记了**：调用者 %s(id=%llu) 处于"
                   " kill_pending —— 清场动作绝不该包含调用者，"
                   "它会在下一次经过闸门时被判死\n",
                   self->name, (unsigned long long)self->id);
    }
    /* 给被标记的探针几次调度机会：它们要么走到闸门、要么走进取消点。
     * 用"睡 2 ms"而不是"让出"——让出只在当前线程自己的时间片里转，
     * 被唤醒的线程可能一次都选不上（实测过）。 */
    for (u32 i = 0; i < 50 && fe_task_thread_count(task) > 0; i++) {
        fe_thread_sleep_ms(2);
    }
    u32 left = fe_task_thread_count(task);
    if (left == 0) {
        fe_kprintf("        收尾：%s 的 %u 个探针已清理\n", tag, marked);
        return 0;
    }
    fe_kprintf("        收尾：%s 标记了 %u 个，仍有 %u 个**清不掉**"
               "（卡在没有取消点的等待或长睡眠上，要等对应修复）:\n",
               tag, marked, left);
    for (struct fe_thread *t = fe_task_thread_first(task); t;
         t = fe_thread_next_of(t)) {
        fe_kprintf("          残留：name=%s id=%llu state=%u wait=%s\n",
                   t->name, (unsigned long long)t->id, t->state,
                   fe_thread_wait_site(t));
    }
    return left;
}

/* ★ 这里做的是"标记 + 唤醒"，不是"就地拔掉" ★
 *
 * 一个线程可能正卡在内核里（阻塞在端点的等待表或 wait_any 的节点上）。
 * 就地把它标记为死亡会留下悬空指针（见 fe_thread_cancel 的说明），
 * 所以终止是**协作式**的：线程只在取消点上死。
 *
 * 于是这个函数做三件事，然后立刻返回：
 *   1. 置 `task->dying`——此后这个任务的线程再也回不到用户态
 *      （闸门在 fe_sched_maybe_switch，判据 fe_thread_should_die）；
 *   2. 把阻塞中的线程摘下来并唤醒，让它们能走到取消点；
 *   3. 已就绪的线程什么都不用做：它们下次回到用户态时会被闸门拦下。
 *
 * ★ 2a 起它与 fe_task_kill_other_threads 共用同一条实现 ★
 * 区别只有一个：终止是"整个任务"（连主线程一起，所以 keep 传 NULL），
 * 而 kill_other_threads 是"除调用者之外"。判据、摘登记、唤醒这三件事
 * 两处必须一模一样——写成两份必然漂移，而漂移的症状是"有时杀得掉、
 * 有时杀不掉"。
 *
 * ★ 语义：**异步** ★ 返回时只保证"所有线程都已被标记"，不保证它们已经死透。
 * 想要"确定没了"就用 fe_task_wait_others_dead（或 WAIT 那个任务句柄）。 */
void fe_task_terminate(struct fe_task *task)
{
    if (!task) {
        return;
    }
    task->dying = true;
    /* keep = NULL：连主线程一起叫停，这正是任务级终止的语义。
     * killed_threads 是"被叫停的线程数"（诊断用）。 */
    task->killed_threads = fe_task_kill_other_threads(task, NULL);
}

/* ------------------------------------------------------------------ */
/* 启动 init                                                           */
/* ------------------------------------------------------------------ */

/* 内核从 cmdline 解析出的引导槽：'a' 或 'b'。
 *
 * 这个值有两个用处，而且是同一件事的两面：
 *   1. 决定 exec 哪个路径的 init（/slot_a/init 还是 /slot_b/init）；
 *   2. 决定 A/B 访问矩阵里哪个槽"正在运行"（→ 只读），哪个"待更新"（→ 连读都不行）。
 * 让两处用同一个来源，它们就不会不一致——那种不一致会表现为
 * "保护的是 B 而跑的是 A"，查起来极其费劲。 */
char fe_boot_slot(void)
{
    const char *cl = fe_boot_info()->cmdline;
    const char *p = cl ? strstr(cl, "slot=") : NULL;
    if (p && (p[5] == 'a' || p[5] == 'b')) {
        return p[5];
    }
    return 'a';     /* 没给 slot= 时默认 A：单槽场景与它完全兼容 */
}

i32 fe_process_start_init(void)
{
    static char arg0[32];
    char *argv[2];
    argv[1] = NULL;

    fe_status_t st = FE_OK;
    char slot = fe_boot_slot();
    char init_path[32];
    strlcpy(init_path, "/slot_x/init", sizeof(init_path));
    init_path[6] = slot;
    strlcpy(arg0, init_path, sizeof(arg0));
    argv[0] = arg0;

    fe_kprintf("[AB] 引导槽 = %c（来自 cmdline: \"%s\"）\n", slot,
               fe_boot_info()->cmdline);

    struct fe_thread *th = NULL;
    struct fe_task *t = fe_process_create(init_path, argv, 1, &st, &th, true);
    if (!t) {
        /* Linux 在这里是 "No working init found" 然后 panic：确实没有别的办法，
         * 内核自己不会去干用户的活。我们至少要把原因打清楚。 */
        fe_kprintf("\n[init] 无法启动 %s: %s\n", init_path, fe_status_name(st));
        fe_kprintf("       文件系统里的内容：\n");
        fe_vfs_dump();
        return st;
    }
    /* "谁是引导者"已经在 fe_process_create 里记下了（必须是那里，
     * 见那段注释：等这里再记就是一个竞态）。 */
    fe_kprintf("[init] %s 已启动（任务 %llu, 主线程 %llu）—— 内核到此交棒\n",
               init_path, (unsigned long long)t->id, (unsigned long long)th->id);

    i32 code = fe_thread_join(th);
    fe_kprintf("[init] init 已退出，退出码 %d\n", code);
    fe_object_unref(&t->hdr);       /* 放掉创建者引用；句柄没了就自动销毁 */
    return code;
}

/* ------------------------------------------------------------------ */
/* 自检                                                                */
/* ------------------------------------------------------------------ */

#define CHECK(cond) do { if (!(cond)) { fail++; } } while (0)

/* 把刚构造好的初始栈读回来核对。这是最容易写错、又最难在运行期定位的一段：
 * 错了的表现是「用户程序一启动就崩」或者「argc 变成天文数字」，
 * 从那种症状反推布局几乎不可能，所以值得单独验一遍。 */
static u32 selftest_initial_stack(void)
{
    u32 fail = 0;
    struct fe_task *t = fe_task_create_user("stackprobe");
    if (!t) {
        fe_kprintf("        探针任务创建失败\n");
        return 1;
    }
    if (fe_failed(fe_user_map_stack(t->space, t->vmas, FE_USER_STACK_TOP,
                                    FE_USER_STACK_SIZE, &t->stack_low))) {
        fe_kprintf("        探针任务栈映射失败\n");
        fe_object_unref(&t->hdr);
        return 1;
    }

    static char a0[] = "/sbin/probe";
    static char a1[] = "第一个参数";
    static char a2[] = "second";
    char *argv[3];
    argv[0] = a0;
    argv[1] = a1;
    argv[2] = a2;

    u64 sp = 0;
    fe_status_t st = build_initial_stack(t->space, a0, argv, 3, &sp);
    if (fe_failed(st)) {
        fe_kprintf("        初始栈构造失败: %s\n", fe_status_name(st));
        fe_object_unref(&t->hdr);
        return 1;
    }

    CHECK((sp & 0xF) == 0);     /* SysV ABI：call 之前 rsp 必须 16 字节对齐 */

    /* 经 HHDM 把栈读回来 */
    u64 hhdm = fe_vmm_hhdm_offset();
    phys_addr_t phys = fe_vmm_translate(t->space, FE_FRAME_ALIGN_DOWN(sp));
    if (phys == 0) {
        fe_kprintf("        初始栈所在页未映射\n");
        fe_object_unref(&t->hdr);
        return fail + 1;
    }
    const u8 *base = (const u8 *)(uptr)(phys + hhdm + (sp & 0xFFF));

    u64 argc = 0;
    memcpy(&argc, base, 8);
    CHECK(argc == 3);

    u64 p[4];
    memcpy(p, base + 8, sizeof(p));         /* argv[0..2] + argv 终止符 */
    CHECK(p[3] == 0);

    /* 每个指针都要指向本页内的字符串，且内容与传入的一致 */
    const char *want[3] = { a0, a1, a2 };
    for (u32 i = 0; i < 3; i++) {
        CHECK(p[i] >= sp && p[i] < FE_USER_STACK_TOP);
        u64 off = p[i] - FE_FRAME_ALIGN_DOWN(sp);
        if (off < 0x1000) {
            CHECK(strcmp((const char *)(base + (p[i] - sp)), want[i]) == 0);
        }
    }

    fe_kprintf("        初始栈: rsp=%#llx argc=%llu argv[0]=\"%s\" argv[1]=\"%s\" "
               "16 字节对齐 %s\n",
               (unsigned long long)sp, (unsigned long long)argc,
               (const char *)(base + (p[0] - sp)), (const char *)(base + (p[1] - sp)),
               ((sp & 0xF) == 0) ? "OK" : "错误");

    /* 超过上限必须报错而不是截断 */
    char *too_many[FE_ARGV_MAX + 2];
    for (u32 i = 0; i < FE_ARGV_MAX + 2; i++) {
        too_many[i] = a2;
    }
    CHECK(build_initial_stack(t->space, a0, too_many, FE_ARGV_MAX + 2, &sp) == FE_ERR_RANGE);

    fe_object_unref(&t->hdr);
    return fail;
}

u32 fe_selftest_process(void)
{
    u32 fail = 0;

    /* 1. 装载器对坏路径的反应。三种错因必须给出三种不同的错误码——
     *    「文件不存在」「那是个目录」「不是可执行映像」混成一个的话，
     *    用户态排查起来只能靠猜。 */
    fe_status_t st = FE_OK;
    CHECK(fe_process_create("/绝对不存在的程序", NULL, 0, &st, NULL, false) == NULL);
    CHECK(st == FE_ERR_NOENT);
    if (fe_vfs_lookup("/sbin")) {
        st = FE_OK;
        CHECK(fe_process_create("/sbin", NULL, 0, &st, NULL, false) == NULL);
        CHECK(st == FE_ERR_ISDIR);
    }
    st = FE_OK;
    CHECK(fe_process_create(NULL, NULL, 0, &st, NULL, false) == NULL);
    CHECK(st == FE_ERR_INVAL);

    /* 2. 初始栈布局 */
    fail += selftest_initial_stack();

    /* 3. 任务引用计数：创建后立刻放掉创建者引用，对象必须销毁
     *    （否则每跑一个程序就漏一个任务对象，是那种跑一整天才会暴露的问题） */
    struct fe_task *probe = fe_task_create_user("refcheck");
    if (!probe) {
        fail++;
    } else {
        u32 before = fe_task_live_count();
        fe_object_unref(&probe->hdr);
        CHECK(fe_task_live_count() == before - 1);
    }
    return fail;
}
