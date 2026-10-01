/* SPDX-License-Identifier: 0BSD */
/* exectest —— 替换映像（K6 / fe_exec）的**用户态运行期**验证（2c）。
 *
 * ★ 为什么必须有它（这一刀补的是 2b 留下的空白）★
 * `sys_exec` 的提交路径（换 CR3 / 挂现场 / 重建 TLS 与 FPU / 放旧空间）
 * 在 2b 之后**只有编译级的保证**——内核自检 E1–E5 验的是"叫停其它线程"
 * 那一半，而"调用者带着新映像回到用户态"这一半**只能由真实客户端验**
 * （docs/15-exec.md §10.2 写明）。这个二进制就是那个客户端。
 *
 * 同一个可执行文件分饰父与子（子模式由 argv[1] 认出），三种模式：
 *
 *   A 成功（默认）  fe_exec 自己 + {"exectest","child"} → 子模式 exit(0x5A)
 *   B 回滚          先 fe_exec 一个**不存在**的路径 → 必须返回错误，
 *                   然后**原程序继续干活** → exit(0x5B)
 *   C 连换          连续 exec 自己两次、第二次 argv 不同 → 0x5C
 *
 * ★ 模式 B 是三种里最关键的一条 ★ 它是"先准备、后提交"的端到端对照：
 * 如果提交发生在准备之前，这里会崩、或者静默变成别的程序——
 * 两种都不是"返回一个错误码然后继续跑"。
 *
 * ★ 模式 A 一次覆盖的机制 ★ 路径装载、新栈构造、argv 传递、入口重置、
 * 换 CR3、挂现场、**重建 TLS**（libfe 的 errno 是 __thread，它能用就说明
 * 新 TLS 生效）、换 FPU 状态区、放旧空间。
 *
 * ──────────────────────────────────────────────────────────────────
 * 除了 exec，本程序还带一条 **mprotect 的 ring-3 端到端探针**（2c 的另一半）：
 * 上一刀（区间属性变更）的自检跑在 ring 0，只能断言 PTE 位，
 * **观察不到"用户态写只读页真的会 #PF"**。typing 在这里补上。
 *
 * ★ 探针为什么用 fe_spawn 的孩子，而不是自己撞 #PF ★
 * K5（缺页容错/信号）还没实现，一个 #PF 就直接杀掉线程——所以撞的人
 * 必须是**可以被牺牲**的那个。父进程观察孩子的死，本身就是证据。
 *
 * ★ 三条设计约束（都是这套内核的真实形状，不是风格偏好）★
 *   1. **被保护的页必须落在某个 VMA 里**（`mprotect` 的机制层要求
 *      范围完全落在同一个区间内）。程序映像的段由 ELF 装载器登记成 VMA
 *      （`kernel/ld/elf.c`），所以用**本程序自己的 .data 里一个页对齐的
 *      全局缓冲**最稳——它一定在数据段的 VMA 内，而且一定已经被映射。
 *   2. **不能拿"父进程的页"去给孩子写**。`fe_spawn` 会 `fe_vmm_space_create()`
 *      建一个**新地址空间**，父子不共享内存——所以"父进程 mprotect 一页、
 *      子进程写它"这种写法在这套内核上根本不成立（写的是孩子自己那份
 *      同名地址，与父进程的页表无关）。
 *      因此改成：**孩子在 spawn 之前先由父进程改好父进程自己的页**——
 *      等等，这也不行。正确的形状是"被写的那一页必须与调用 mprotect 的
 *      那一方在同一个地址空间里"，所以探针做成：
 *      **孩子进程里，父与子都是同一个 exectest**——不，父子不同空间。
 *      最终采用的是最简单也最扎实的一种：**父进程不许诺孩子的内存**，
 *      而是让**子进程自己**在自己的地址空间里跑一遍完整的
 *      "protect → write"，父进程只观察它的死活。
 *   3. 因此探针分两半，跑两次、对照着看：
 *      - **正向**：子进程先 `fe_mem_protect(只读)` 再写 → 必须被 #PF 杀死；
 *      - **反向对照**：同一个子进程**不调用** `fe_mem_protect` 直接写
 *        → 必须写成功并正常退出。
 *      两条一起才能排除"孩子本来就因为别的原因死"这种解释。
 *
 * ★ 孩子死没死怎么观察 ★ 用户态 #PF 走 `user_fault` → `fe_thread_exit(-1)`
 * （主线程退出即进程结束 → `task->exited` 置位），所以
 * `fe_wait` 拿到的退出码是 **-1**。孩子正常跑完则给一个我们自己约定的码。
 *
 * 退出码约定（init 校验）：
 *   0x5A 模式 A 成功（子模式）
 *   0x5B 模式 B 回滚成功
 *   0x5C 模式 C 连换成功
 *   0x00 父模式成功
 *   1    任一断言失败（并打印是哪一条）
 */

#include <fe_user.h>

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static char g_prefix[32];

/* 拼一个本槽里的程序路径：<prefix>/bin/<name>
 * （与 killtest 的写法一致：路径是"我自己名字的一部分"，不用问别人） */
static void slot_path(const char *name, char *out, u32 cap)
{
    u32 k = 0;
    const char *parts[3];
    parts[0] = g_prefix;
    parts[1] = "/bin/";
    parts[2] = name;
    for (u32 p = 0; p < 3; p++) {
        for (const char *s = parts[p]; *s && k < cap - 1; s++) {
            out[k++] = *s;
        }
    }
    out[k] = '\0';
}

/* 失败项的语义用**数值**打印：屏幕上只允许 ASCII，而且数值比措辞更精确。 */
static void fail(const char *what, long code)
{
    fe_printf("  [exectest] **失败** %s（返回 %ld）\n", what, code);
}

/* ------------------------------------------------------------------ */
/* K5：exec 必须清掉异常处理者的登记（docs/18 §6.1.5 对 K6 的要求）      */
/* ------------------------------------------------------------------ */

/* 与内核 fe/errno.h 的 FE_ERR_NOENT 是同一个数。用户态今天没有 errno 头，
 * 所以写死一个数——但写成符号，好让"这个 -3 是什么"一眼可见。 */
#define EXEC_ERR_NOENT  (-3)

/* 登记用的现场缓冲区。**必须是已映射且可写的用户地址**（登记时内核当场
 * 校验，见 sys_fault_handler）；ELF 装载器把每个 PT_LOAD 段整段映射好
 * （含 .bss 尾部，与下面 g_probe_page 那条理由相同），所以一个静态变量够用。 */
static struct fe_fault_regs g_fault_regs;

/* exec **之前**登记一个异常处理者。
 * 收件线程 = 调用线程（main）——本模式不打算真的出错，登记存在的唯一意义
 * 是给 exec 之后的查询留一个"没被清掉就会看得见"的东西。
 * 返回失败项数（0 = 成功）。 */
static int register_probe_handler(void)
{
    memset(&g_fault_regs, 0, sizeof(g_fault_regs));   /* 登记之前先碰一次 */
    long ep = fe_endpoint_create(0);
    if (ep < 0) {
        fail("K5/exec：endpoint_create 失败", ep);
        return 1;
    }
    long rc = fe_fault_set_handler(ep, &g_fault_regs);
    if (rc != 0) {
        fail("K5/exec：登记异常处理者失败", rc);
        return 1;
    }
    fe_puts("[exectest] K5/exec：exec 之前登记了一个异常处理者"
            "（exec 之后新映像必须查不到它）\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* mprotect 探针：本程序 .data 里一个页对齐的页                        */
/* ------------------------------------------------------------------ */

/* ★ 为什么必须是独立的、页对齐的、够大的全局缓冲 ★
 *   - 页对齐 ⇒ protect 的地址就是页首，范围恰好一页；
 *   - 在 .data 里 ⇒ ELF 装载器把数据段登记成 VMA（`kernel/ld/elf.c`），
 *     而 `mprotect` 要求范围完全落在同一个 VMA 内；
 *   - 已经映射 ⇒ ELF 装载器把每个 PT_LOAD 段整段映射好（含 .bss 尾部），
 *     所以这一页的 PTE 一开始就存在，"改权限"有东西可改。
 * 写成 4096 字节：正好一页，避免"跨页"把变量搅进来。 */
static volatile u8 g_probe_page[4096] __attribute__((aligned(4096)));

/* 孩子正常跑完时的退出码（与"被 #PF 杀死"的 -1 区分开）。 */
#define PROBE_OK_EXIT   0x37

/* 子模式：--probe-write <mode>
 *   mode 0 = 不调用 mprotect，直接写 → 必须成功
 *   mode 1 = 先 mprotect 成只读，再写 → 必须被 #PF 杀死
 *
 * 写完/写不成之后的痕迹写在页里（若页是只读则写不进去，那正是我们要的）。 */
static int probe_child(int mode)
{
    volatile u8 *p = (volatile u8 *)g_probe_page;
    u64 page = (u64)(usize)g_probe_page;

    /* 前置：先读一次，确认这一页**本来就在**（不是"因为缺页才失败"）。
     * 只读也照样读得动——这正是"改的是权限、不是存在性"的对照。 */
    u8 before = p[0];
    (void)before;

    if (mode == 0) {
        p[0] = 0xA5;                    /* 没被保护：必须成功 */
        p[1] = 0x5A;
        if (p[0] != 0xA5 || p[1] != 0x5A) {
            return PROBE_OK_EXIT + 1;   /* 写进去读回来不一样：内存有问题 */
        }
        return PROBE_OK_EXIT;
    }

    /* mode 1：改成只读。prot 只给 READ（不给 W、不给 X）。 */
    long r = fe_mem_protect((void *)(usize)page, 4096, FE_PROT_READ);
    if (r != 0) {
        /* 保护都设不上，这条探针就没意义了——用另一个码说出来，
         * 免得父进程把它误读成"页本来就不可写"。 */
        return PROBE_OK_EXIT + 2;
    }
    /* ★ 这一行就是判据：若 PTE 的 W 位真的掉了，它会 #PF 把这个线程杀掉；
     * 若只改了 VMA（上一刀的红测试形态），它会安然写进去并返回。★ */
    p[0] = 0x11;
    return PROBE_OK_EXIT + 3;           /* 走到这里 = 保护没生效 */
}

/* ------------------------------------------------------------------ */
/* 父模式：exec 三种模式 + mprotect 探针                               */
/* ------------------------------------------------------------------ */

static int run_exec_modes(const char *self)
{
    int fail_count = 0;

    /* ---------- 模式 A：fe_exec 自己，子模式退出 0x5A ---------- */
    fe_puts("[exectest] 模式 A：fe_exec 自己（期望子模式退出 0x5A）\n");
    /* ★ K5：exec **之前**登记一个异常处理者 ★ 子模式会问一次
     * "我还有处理者吗"，正确的答案是"没有"（提交阶段清掉了）。
     * 不先登记就没有东西可清，那条断言会**假通过**——
     * 这正是"每条正向都要配一条会失败的反向"的那条纪律。 */
    fail_count += register_probe_handler();
    {
        char *av[3];
        av[0] = (char *)self;
        av[1] = (char *)"child";
        av[2] = (char *)0;
        /* ★ 走到下一行 = 失败 ★ 成功时内核改了返回帧，根本回不到这里。 */
        long e = fe_exec(self, av, 2);
        fail("模式 A：fe_exec 返回了（应当不返回）", e);
        fail_count++;
    }
    /* 模式 A 若成功，下面这些代码**永远不会执行**；若失败，原程序继续跑，
     * 于是我们能继续把剩下的模式与探针跑完、如实报出失败项。 */
    return fail_count;
}

static int run_rollback_mode(void)
{
    int fail_count = 0;
    fe_puts("[exectest] 模式 B：fe_exec 一个不存在的路径（期望返回错误）\n");
    {
        char *av[2];
        av[0] = (char *)"/slot_a/bin/definitely-not-here";
        av[1] = (char *)0;
        long e = fe_exec(av[0], av, 1);
        if (e >= 0) {
            fail("模式 B：不存在的路径居然 exec 成功了", e);
            fail_count++;
        } else {
            fe_printf("  [exectest] 模式 B：如实返回了错误码 %ld\n", -e);
        }
    }
    /* ★ 这一行是模式 B 的真正判据 ★
     * 它证明"准备阶段失败之后**原程序完好无损地接着跑**"——
     * 若提交发生在准备之前，这里要么崩、要么已经不是原程序了。 */
    fe_puts("[exectest] 模式 B：原程序继续运行（这一行就是判据）\n");
    return fail_count;
}

static int run_chain_mode(const char *self)
{
    int fail_count = 0;
    fe_puts("[exectest] 模式 C：连换两次（第一次 exec 自己 + chain2）\n");
    {
        char *av[3];
        av[0] = (char *)self;
        av[1] = (char *)"chain2";
        av[2] = (char *)0;
        long e = fe_exec(self, av, 2);      /* 成功不返回 */
        fail("模式 C 第一跳：fe_exec 返回了", e);
        fail_count++;
    }
    return fail_count;
}

static int run_probe(const char *self, const char *tag, int mode)
{
    char *av[4];
    av[0] = (char *)self;
    av[1] = (char *)"--probe-write";
    av[2] = (mode == 0) ? (char *)"0" : (char *)"1";
    av[3] = (char *)0;
    long h = fe_spawn(self, av, 3);
    if (h <= 0) {
        fe_printf("  [exectest] %s：spawn 失败（%ld）\n", tag, h);
        return -1000;
    }
    int code = 0x7fffffff;
    long w = fe_wait(h, &code);
    if (w < 0) {
        fe_printf("  [exectest] %s：wait 失败（%ld）\n", tag, w);
        return -1000;
    }
    fe_handle_close(h);
    return code;
}

static int run_mprotect_probe(const char *self)
{
    int fail_count = 0;
    u64 page = (u64)(usize)g_probe_page;

    fe_puts("[exectest] mprotect 探针：ring-3 端到端（父进程只观察孩子的死活）\n");

    /* 前置：这一页在**保护之前**必须是可写的。用一次不受保护的写来证。 */
    g_probe_page[0] = 0x01;
    if (g_probe_page[0] != 0x01) {
        fail("探针前置：未受保护的页居然写不动", (long)g_probe_page[0]);
        fail_count++;
    } else {
        fe_puts("  [exectest] 前置：这一页在保护之前可写（父进程自己写通过）\n");
    }

    /* ---------- 反向对照：不调用 mprotect 的孩子必须能写成功 ---------- */
    int c0 = run_probe(self, "反向对照（不保护）", 0);
    if (c0 == PROBE_OK_EXIT) {
        fe_puts("  [exectest] 反向对照：同一个孩子**不调用** mprotect 时"
                "写成功并正常退出（0x37）\n");
    } else if (c0 == -1) {
        /* 孩子没保护就被 #PF 杀了 ⇒ 说明"死"与保护无关，pos 那一条作废 */
        fail("反向对照：孩子没被保护却被 #PF 杀死（前置不成立）", -1);
        fail_count++;
    } else {
        fail("反向对照：孩子既没写成功也没被杀死（退出码异常）", c0);
        fail_count++;
    }

    /* ---------- 正向：调用 mprotect 成只读的孩子必须被 #PF 杀死 ---------- */
    int c1 = run_probe(self, "正向（保护成只读）", 1);
    if (c1 == -1) {
        fe_puts("  [exectest] 正向：孩子 mprotect 成只读后写它 → 被 #PF 杀死"
                "（退出码 -1）——PTE 的 W 位真的掉了\n");
    } else if (c1 == PROBE_OK_EXIT + 2) {
        fail("正向：孩子连 mprotect 都没设置成功", c1);
        fail_count++;
    } else if (c1 == PROBE_OK_EXIT + 3) {
        fail("正向：孩子写了只读页却**活着回来了** —— 保护没生效", c1);
        fail_count++;
    } else {
        fail("正向：孩子以意外退出码结束", c1);
        fail_count++;
    }

    /* ---------- 边界：mprotect 自己的拒绝路径（ring-3 侧）----------
     *
     * ★ 判据是"返回码**等于**那个负错误码" ★
     * 这套 ABI 的约定是"失败时 rax 直接承载负值"（见 fe/errno.h 开头那三行），
     * 所以比较时**直接比 FE_ERR_***，不要写 `-FE_ERR_*`——后者等于把
     * "期望 -1" 变成"期望 +1"，于是"如实返回了 -1"会被判成失败。
     * （第一版就是这么写的，三条边界一起假失败。） */
    {
        long r1 = fe_mem_protect((void *)(usize)(page + 1), 4096, FE_PROT_READ);
        if (r1 != FE_ERR_INVAL) {
            fail("边界：未对齐地址应当被拒（期望 FE_ERR_INVAL）", r1);
            fail_count++;
        } else {
            fe_puts("  [exectest] 边界：未对齐地址被拒（-INVAL）\n");
        }
        /* 范围越出数据段 VMA（往后 16 MiB，那里没有任何区间） */
        long r2 = fe_mem_protect((void *)(usize)(page + 0x1000000ull), 4096,
                                 FE_PROT_READ);
        if (r2 != FE_ERR_NOENT) {
            fail("边界：不在任何区间内的地址应当被拒（期望 FE_ERR_NOENT）", r2);
            fail_count++;
        } else {
            fe_puts("  [exectest] 边界：不在任何区间内的地址被拒（-NOENT）\n");
        }
        /* W^X：一次同时给 W 与 X 必须被拒 */
        long r3 = fe_mem_protect((void *)(usize)page, 4096,
                                 FE_PROT_READ | FE_PROT_WRITE | FE_PROT_EXEC);
        if (r3 != FE_ERR_INVAL) {
            fail("边界：W|X 应当被拒（期望 FE_ERR_INVAL）", r3);
            fail_count++;
        } else {
            fe_puts("  [exectest] 边界：一次给 W|X 被拒（-INVAL）\n");
        }
    }

    /* ★ 收尾：把这一页改回可写，免得影响后面还用到它的代码 ★
     * （本程序用完就退，但"测试不留副作用"这条纪律在这里也照做：
     *   后面的模式 C 与收尾打印可能碰 .data 的其它页，
     *   万一将来这个缓冲与它们同页，留一个只读页会变成别人的假失败。） */
    long rb = fe_mem_protect((void *)(usize)page, 4096,
                             FE_PROT_READ | FE_PROT_WRITE);
    if (rb == 0) {
        g_probe_page[1] = 0x02;
        if (g_probe_page[1] != 0x02) {
            fail("收尾：改回可写之后仍然写不动", (long)g_probe_page[1]);
            fail_count++;
        } else {
            fe_puts("  [exectest] 收尾：改回可写之后又能写了（权限是可逆的）\n");
        }
    } else {
        fail("收尾：把页改回可写失败", rb);
        fail_count++;
    }
    return fail_count;
}

/* ------------------------------------------------------------------ */

/* ★ 为什么三种模式各占一次"进程调用"，而不是一次跑完 ★
 *
 * ★ 一个进程只能有一个退出码 ★ 模式 A 成功时进程**变成**了子模式、
 * 退出码是 0x5A；模式 C 成功时进程变成 chain3、退出码是 0x5C。
 * 两者都想让 init 校验自己的退出码，就**必须各占一次调用**——
 * 一次调用里"两个退出码"在物理上不可能同时成立。
 *
 * 于是三种模式做成三个可选的命令行开关，init 用三个单元分别拉起它：
 *   （默认）      模式 A：exec 自己 + "child"        → 0x5A
 *   --rollback    模式 B：exec 不存在的路径 → 必须失败；再跑 mprotect 探针 → 0x5B
 *   --chain       模式 C：连着 exec 自己两次          → 0x5C
 *
 * ★ mprotect 探针为什么搭在 --rollback 那一次里 ★
 * 探针与 exec 无关，但它需要一个"会正常跑完并给出退出码"的宿主。
 * 模式 A/C 的宿主退出码已被 exec 占用，只有模式 B 是"原程序跑到底"的形态，
 * 所以探针搭在它上面最自然——而且模式 B 的失败项会与探针的一起计进退出码，
 * 一条命令就能同时覆盖"回滚"与"区间保护"两件事。 */
#define MODE_DEFAULT   0
#define MODE_ROLLBACK  1
#define MODE_CHAIN     2

int main(int argc, char **argv)
{
    fe_slot_prefix((argc > 0 && argv) ? argv[0] : 0, g_prefix, sizeof(g_prefix));

    /* ---------- 子模式：probe 的孩子（两种 mode）---------- */
    if (argc >= 3 && argv[1] && strcmp(argv[1], "--probe-write") == 0) {
        int mode = (argv[2][0] == '1') ? 1 : 0;
        return probe_child(mode);
    }

    /* ---------- 子模式：exec 的孩子 ---------- */
    if (argc >= 2 && argv[1] && strcmp(argv[1], "child") == 0) {
        fe_puts("[exectest] 子模式：新映像已经跑起来了（这就是 exec 成功的证据）\n");

        /* ★ K5：exec 的提交阶段必须**清掉**异常处理者的登记 ★
         * 登记里"处理者端点"属**身份**（exec 不换句柄表，所以它本来会跟着
         * 任务活下来），而"现场缓冲区地址"与"收件线程"属**映像**——
         * 默认保留就等于让新映像被一段**不属于它**的代码接管
         * （docs/18 §6.1.5；与 `13-tasks-and-kill.md` §6.5 那条"按身份钉死的
         * 授权，换映像必须显式处理"是同一条纪律）。
         *
         * 判据用"注销"这一个动作，因为它同时回答"本来有没有"：
         *   返回  0        = 登记**还在** → 提交阶段没清（这条断言就红）
         *   返回 -3(NOENT) = 没有登记   → 正确
         * 为什么必须是 `NOENT` 而不是 `INVAL`：`NOENT` 说"登记不在了"，
         * `INVAL` 说"你参数写错了"——而这里不是参数错。
         *
         * ★ 这一条**不能**放在 `--rollback` 那条路上 ★ 那条路的 exec
         * **失败**（路径不存在），提交阶段根本没跑，登记理应还在。
         * docs/18 §6.1.5 第 2 条写的是"模式 B 之后必须是 NOENT"，
         * 那在本内核上不成立——成功换映像的才是模式 A/C。 */
        long had = fe_fault_set_handler(0, NULL);
        if (had == 0) {
            fail("K5/exec：exec 之后登记**还在**（提交阶段没清）", had);
            return 0x51;
        }
        if (had != EXEC_ERR_NOENT) {
            fail("K5/exec：exec 之后注销返回的不是 NOENT(-3)", had);
            return 0x52;
        }
        fe_printf("[exectest] K5/exec：新映像查询处理者 -> NOENT(%ld)，"
                  "登记已被提交阶段清掉\n", had);

        /* ★ 这里用了一次 printf 与一次 puts：它们都走 libfe，
         * 而 libfe 的 errno 是 __thread —— 能正常输出就说明**新 TLS 生效**。★ */
        fe_printf("[exectest] 子模式：argv[0]=%s argc=%d，即将 exit(0x5A)\n",
                  argv[0], argc);
        return 0x5A;
    }

    /* ---------- 子模式：模式 C 的第二跳 ---------- */
    if (argc >= 2 && argv[1] && strcmp(argv[1], "chain2") == 0) {
        fe_puts("[exectest] 模式 C 第二跳：再次 exec 自己（argv[1]=chain3）\n");
        char *av[3];
        av[0] = (char *)argv[0];
        av[1] = (char *)"chain3";
        av[2] = (char *)0;
        long e = fe_exec(argv[0], av, 2);
        /* 走到这里 = 第二跳失败：如实报出来（模式 C 的判据之一）。 */
        fail("模式 C 第二跳：fe_exec 返回了", e);
        return 1;
    }
    if (argc >= 2 && argv[1] && strcmp(argv[1], "chain3") == 0) {
        fe_puts("[exectest] 模式 C 第三跳：连换两次都成功，exit(0x5C)\n");
        return 0x5C;
    }

    /* ---------- 父模式：三种开关 ---------- */
    char self[96];
    slot_path("exectest", self, sizeof(self));

    int mode = MODE_DEFAULT;
    if (argc >= 2 && argv[1]) {
        if (strcmp(argv[1], "--rollback") == 0) {
            mode = MODE_ROLLBACK;
        } else if (strcmp(argv[1], "--chain") == 0) {
            mode = MODE_CHAIN;
        }
    }

    if (mode == MODE_ROLLBACK) {
        fe_puts("\n=== 替换映像（K6）：模式 B 回滚 + 区间保护（mprotect）端到端 ===\n");
        int fails = run_rollback_mode();          /* 这一条必然失败，但**必须返回** */
        fails += run_mprotect_probe(self);        /* 探针搭在这一次里 */
        if (fails) {
            fe_printf("=== 回滚/区间保护测试结束，失败项 %d ===\n", fails);
            return 1;
        }
        fe_puts("[exectest] 模式 B：两条都通过，exit(0x5B)\n");
        return 0x5B;
    }

    if (mode == MODE_CHAIN) {
        fe_puts("\n=== 替换映像（K6）：模式 C 连换两次 ===\n");
        int fails = run_chain_mode(self);
        fe_printf("=== 模式 C 未能接管，失败项 %d ===\n", fails);
        return 1;
    }

    fe_puts("\n=== 替换映像（K6）：模式 A 成功（exec 自己 + child）===\n");
    {
        int fails = run_exec_modes(self);
        fe_printf("=== 模式 A 未能接管，失败项 %d ===\n", fails);
        return 1;
    }
}
