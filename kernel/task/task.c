/* SPDX-License-Identifier: 0BSD */
#include <fe/task.h>
#include <fe/ipc.h>
#include <fe/sched/thread.h>
#include <fe/mm/kheap.h>
#include <fe/kprintf.h>
#include <fe/string.h>
#include <fe/panic.h>
#include <fe/mm/vmm.h>
#include <fe/gdt.h>
#include <fe/resource.h>
#include <fe/irq.h>
#include <fe/devfs.h>

static struct fe_task *g_kernel_task;
static struct fe_task *g_all_tasks;
static u64 g_next_task_id = 1;

/* 由 object.c 在引用计数归零时调用 */
void fe_object_destroy(struct fe_object_header *hdr)
{
    if (!hdr) {
        return;
    }
    switch (hdr->type) {
    case FE_OBJ_ENDPOINT:
        fe_endpoint_destroy(FE_OBJ_OF(hdr, struct fe_endpoint));
        break;
    case FE_OBJ_NOTIFICATION:
        fe_notification_destroy(FE_OBJ_OF(hdr, struct fe_notification));
        break;
    case FE_OBJ_MEMORY:
        fe_memory_destroy(FE_OBJ_OF(hdr, struct fe_memory_object));
        break;
    case FE_OBJ_TASK: {
        struct fe_task *t = FE_OBJ_OF(hdr, struct fe_task);
        /* ★ 销毁一个**还有存活线程**的任务 = 引用计数出错 ★
         * 每个存活线程都持有一个任务引用（fe_thread_create_ring3 /
         * fe_thread_create 里各 ref 一次），所以引用计数归零**必然**
         * 意味着没有存活线程。反过来说：如果这里看到还有活线程，
         * 那就是有谁多放了一次引用——后果是地址空间被拆掉，
         * 而那个线程还在自己的代码上跑（实测症状：用户线程在
         * `fe_endpoint_recv` 里取指 #PF，错误码 0x14）。
         * 这一条不是"防御性代码"，它是一次**证据收集**：
         * 这种错平时只表现为随机的用户态崩溃，指不到真正的原因。 */
        u32 live = fe_task_thread_count(t);
        if (live) {
            fe_kprintf("[对象] **任务 %s(%llu) 在还有 %u 个存活线程时被销毁**"
                       "——引用计数出错\n",
                       t->name, (unsigned long long)t->id, live);
        }
        /* ★ 销毁之前必须把还指着这个任务对象的线程摘干净 ★
         * 见 fe_task_detach_threads 的说明：有些线程（内核线程僵尸）活得比
         * 任务久，而它们对任务的引用要到 thread_free 才放——也就是永远不放。
         * 不摘就是悬空指针（实测：QEMU 上 `fe_sched_maybe_switch` 读
         * `next->task->iopb_slot` 页错误，CR2 = 任务地址 + 0x1060）。
         * 必须在下面 `fe_kfree(t)` **之前**做。 */
        fe_task_detach_threads(t);
        fe_handle_table_clear(&t->handles);
        /* 归还硬件资源。顺序要紧：先掐中断线，再还资源池，最后放位图槽位。
         * 反过来的话，中断可能还在往一个正在销毁的对象里投递。 */
        fe_irq_release_owner(t->id);
        fe_resource_release_owner(t->id);
        /* 设备管理器身份也要收回：id 复用之后，另一个任务不该**继承**它。
         * 这类"身份跟着 id 走"的漏洞在 id 单调递增时看不出来，
         * 一旦 id 复用就是提权。 */
        fe_resource_clear_devmgr(t->id);
        /* devfs 上的名字也要收回：否则服务重启后注册不回同一个名字，
         * 而名字背后的端点是旧任务的，所有已打开的客户端都会静默失联。 */
        fe_devfs_release_owner(t->id);
        if (t->iopb_slot >= 0) {
            fe_iopb_free_slot(t->iopb_slot);
            t->iopb_slot = -1;
        }
        /* 地址空间也要还。内核任务是共享的内核页表，fe_vmm_space_destroy
         * 会认出来并直接返回；用户任务的页表与地址空间对象在这里回收。 */
        if (t->space) {
            fe_vmm_space_destroy(t->space);
            t->space = NULL;
        }
        /* 区间表也是任务持有的内存：不放就是每跑一个程序漏 1.3 KiB。
         * 放在地址空间之后释放——区间表里的 frames 指针指向的内存对象
         * 由对象自己的引用计数管，不归这里管。 */
        if (t->vmas) {
            fe_kfree(t->vmas);
            t->vmas = NULL;
        }
        /* 从全局任务链摘除 */
        struct fe_task **pp = &g_all_tasks;
        while (*pp && *pp != t) {
            pp = &(*pp)->next;
        }
        if (*pp) {
            *pp = t->next;
        }
        fe_kfree(t);
        break;
    }
    default:
        fe_kfree(hdr);
        break;
    }
}

struct fe_task *fe_task_alloc(const char *name)
{
    struct fe_task *t = (struct fe_task *)fe_kzalloc(sizeof(*t));
    if (!t) {
        return NULL;
    }
    fe_object_init(&t->hdr, FE_OBJ_TASK);
    fe_handle_table_init(&t->handles);
    /* I/O 位图槽位是**惰性分配**的：只有真正申请过端口能力的任务才占用一个槽位。
     * kzalloc 给的是 0，而 0 是合法槽位号，所以必须显式写成 -1。 */
    t->iopb_slot = -1;
    t->id = g_next_task_id++;
    strlcpy(t->name, name ? name : "task", FE_TASK_NAME_MAX);
    t->next = g_all_tasks;
    g_all_tasks = t;
    return t;
}

struct fe_task *fe_task_create_kernel(const char *name)
{
    return fe_task_alloc(name);
}

void fe_task_init(void)
{
    g_kernel_task = fe_task_create_kernel("kernel");
    if (!g_kernel_task) {
        fe_panic("无法创建内核任务");
    }
    /* 内核任务也持有一个地址空间对象（就是内核页表本身）。
     * 这一点很关键：否则从用户任务切回内核线程时，`space` 为 NULL 会让调度器
     * 认为"不需要换 CR3"，内核线程就会继续跑在**用户任务的页表**上——
     * 而内核之后新建的页表项未必会出现在那份（创建时浅拷贝的）页表里。 */
    g_kernel_task->space = fe_vmm_kernel_space();
}

struct fe_task *fe_task_kernel(void)
{
    return g_kernel_task;
}

struct fe_task *fe_task_current(void)
{
    struct fe_thread *th = fe_thread_current();
    if (th && th->task) {
        return th->task;
    }
    return g_kernel_task;
}

/* 按 id 找任务。id 只在任务对象还挂在全局链上时才成立——
 * 任务销毁的最后一步就是从这条链上摘除（见 fe_object_destroy），
 * 所以"链上有它"与"它还活着"是同一件事，不需要额外的存活标志。 */
struct fe_task *fe_task_by_id(u64 id)
{
    for (struct fe_task *t = g_all_tasks; t; t = t->next) {
        if (t->id == id) {
            return t;
        }
    }
    return NULL;
}

/* 按名字找任务。
 *
 * ★ 名字是**不唯一**的（多个用户线程都叫 "user"），所以这里返回第一个匹配 ★
 * 这没问题，因为调用者（设备管理器）问的是"那个叫 blkd 的驱动现在是哪个 id"
 * ——服务名在引导链上是唯一的，而线程名不是。把这一点写在这里，
 * 免得将来有人拿它当"唯一标识"用。 */
struct fe_task *fe_task_by_name(const char *name)
{
    if (!name) {
        return NULL;
    }
    for (struct fe_task *t = g_all_tasks; t; t = t->next) {
        if (strcmp(t->name, name) == 0) {
            return t;
        }
    }
    return NULL;
}

/* 所有任务都挂在这条链上（TASK_LIST 快照按它遍历）。 */
struct fe_task *fe_task_first(void)
{
    return g_all_tasks;
}

void fe_task_dump_all(void)
{
    fe_kprintf("  任务 ID  名称             句柄数\n");
    for (struct fe_task *t = g_all_tasks; t; t = t->next) {
        fe_kprintf("  %-8llu %-16s %u\n", (unsigned long long)t->id, t->name,
                   fe_handle_table_used(&t->handles));
    }
}

u32 fe_task_live_count(void)
{
    u32 n = 0;
    for (struct fe_task *t = g_all_tasks; t; t = t->next) {
        n++;
    }
    return n;
}
