/* SPDX-License-Identifier: 0BSD */
/* 受保护扇区区间的实现（设计说明见 fe/protect.h）。
 *
 * 数据结构是定长数组 + 线性扫描：区间数量是"几十到几百"量级，
 * 而检查发生在每次块 I/O 之前——线性扫描换来的，是
 * "没有动态分配、没有失败路径、没有锁"。
 */
#include <fe/protect.h>
#include <fe/string.h>
#include <fe/kprintf.h>
#include <fe/time.h>
#include <fe/io.h>
#include <fe/task.h>
#include <fe/sched/thread.h>

struct fe_prot_extent {
    u64 lba;
    u64 count;
    u64 exempt_task;
    u32 mode;
};

static struct fe_prot_extent g_extents[FE_PROT_MAX_EXTENTS];
static u32 g_extent_count;

static struct fe_protect_violation g_viol[FE_PROT_VIOLATION_LOG];
static u32 g_viol_next;
static u64 g_viol_total;
static u64 g_updater_task;      /* 被指定为更新器的任务 id；0 = 还没有 */

/* 打印限速：一次违规要留下痕迹，但不许被刷屏刷掉有用的日志 */
static u64 g_last_print_tick;
static u32 g_printed;

/* 清单派生出来的事实 */
static struct fe_ab_info g_ab;
static char g_updater_path[64];     /* 例如 "/slot_a/bin/abupdate" */

void fe_ab_get_info(struct fe_ab_info *out)
{
    if (out) {
        *out = g_ab;
        out->updater_task = g_updater_task;
    }
}

void fe_protect_note_process(const char *path, u64 task_id)
{
    if (!path || !g_updater_path[0] || g_updater_task != 0) {
        return;
    }
    if (strcmp(path, g_updater_path) != 0) {
        return;
    }
    g_updater_task = task_id;
    fe_kprintf("[保护] 更新器已指定: 任务 %llu（%s）—— "
               "只有它能碰另一个槽\n", (unsigned long long)task_id, path);
}

void fe_protect_init(void)
{
    memset(g_extents, 0, sizeof(g_extents));
    g_extent_count = 0;
    memset(g_viol, 0, sizeof(g_viol));
    g_viol_next = 0;
    g_viol_total = 0;
    g_last_print_tick = 0;
    g_printed = 0;
    memset(&g_ab, 0, sizeof(g_ab));
    g_updater_path[0] = '\0';
    g_updater_task = 0;
}

fe_status_t fe_protect_add(u64 lba, u64 count, u32 mode, u64 exempt_task)
{
    if (count == 0 || mode == 0) {
        return FE_ERR_INVAL;
    }
    u64 end = lba + count;
    if (end < lba) {
        return FE_ERR_OVERFLOW;
    }
    /* 重叠就拒绝：表必须是一组互不相交的区间。
     * 允许重叠的话，检查仍然是对的，但表会长出重复项、迟早撑满，
     * 而且"哪些扇区受保护"会变得难以推理。 */
    for (u32 i = 0; i < g_extent_count; i++) {
        u64 a0 = g_extents[i].lba;
        u64 a1 = a0 + g_extents[i].count;
        if (lba < a1 && a0 < end) {
            return FE_ERR_EXIST;
        }
    }
    if (g_extent_count >= FE_PROT_MAX_EXTENTS) {
        return FE_ERR_NOSPC;
    }
    g_extents[g_extent_count].lba = lba;
    g_extents[g_extent_count].count = count;
    g_extents[g_extent_count].mode = mode;
    g_extents[g_extent_count].exempt_task = exempt_task;
    g_extent_count++;
    return FE_OK;
}

/* 记一笔违规并（限速地）打一条日志。
 * 抽出来是因为它有好几个调用点：命中区间、参数回绕。
 * **每一条"内核拒绝了这次访问"的路径都必须留下痕迹**——
 * 我第一版只在命中区间那条路径上记录，回绕那条直接返回，
 * 于是有一部分被拒绝的访问查不到。 */
static void record_violation(u64 lba, u64 count, bool write, u32 mode)
{
    u64 irq = fe_irq_save();
    struct fe_protect_violation *v = &g_viol[g_viol_next];
    struct fe_task *t = fe_task_current();
    v->task_id = t ? t->id : 0;
    v->lba = lba;
    v->count = count;
    v->tick = fe_time_ticks();
    v->mode = mode;
    v->write = write ? 1 : 0;
    g_viol_next = (g_viol_next + 1) % FE_PROT_VIOLATION_LOG;
    g_viol_total++;
    u64 now = fe_time_ticks();
    bool do_print = (now - g_last_print_tick > 100) && (g_printed < 20);
    if (do_print) {
        g_last_print_tick = now;
        g_printed++;
    }
    fe_irq_restore(irq);

    if (do_print) {
        fe_kprintf("[保护] **拒绝%s** LBA %llu + %llu（调用者 %s）\n",
                   write ? "写入" : "读取",
                   (unsigned long long)lba, (unsigned long long)count,
                   t ? t->name : "?");
    }
}

fe_status_t fe_protect_check(u64 lba, u64 count, bool is_write, u64 requester)
{
    if (count == 0) {
        return FE_OK;
    }
    u64 end = lba + count;
    if (end < lba) {
        record_violation(lba, count, is_write, FE_PROT_DENY_ALL);
        return FE_ERR_ACCESS;       /* 回绕：一律拒绝，不给"擦边"留口子 */
    }
    u32 want = is_write ? FE_PROT_DENY_WRITE : FE_PROT_DENY_READ;
    for (u32 i = 0; i < g_extent_count; i++) {
        u64 a0 = g_extents[i].lba;
        u64 a1 = a0 + g_extents[i].count;
        if (!(lba < a1 && a0 < end)) {
            continue;               /* 不相交 */
        }
        if (!(g_extents[i].mode & want)) {
            continue;               /* 这一段不禁这类访问 */
        }
        /* 豁免只看"请求方是不是被内核指定的更新器"，不看**调用者**是谁。
         * blkd 是可信的执行者，但它报上来的 requester 必须是内核填的那个。 */
        if ((g_extents[i].mode & FE_PROT_EXEMPT_UPDATER) &&
            g_updater_task != 0 && g_updater_task == requester) {
            continue;
        }
        if (g_extents[i].exempt_task != 0 &&
            g_extents[i].exempt_task == requester) {
            continue;
        }
        record_violation(lba, count, is_write, g_extents[i].mode);
        fe_kprintf("[保护]   （落在受保护区间 %llu + %llu，模式 %s）\n",
                   (unsigned long long)a0, (unsigned long long)a1 - a0,
                   (g_extents[i].mode & FE_PROT_DENY_READ) ? "禁读禁写" : "禁写");
        return FE_ERR_ACCESS;
    }
    return FE_OK;
}

bool fe_protect_active(void)
{
    return g_extent_count > 0;
}

u64 fe_protect_extent_count(void)
{
    return g_extent_count;
}

u64 fe_protect_violation_count(void)
{
    return g_viol_total;
}

u32 fe_protect_get_violations(struct fe_protect_violation *out, u32 cap)
{
    if (!out || cap == 0) {
        return 0;
    }
    u32 n = (g_viol_total < FE_PROT_VIOLATION_LOG) ? (u32)g_viol_total
                                                   : FE_PROT_VIOLATION_LOG;
    if (n > cap) {
        n = cap;
    }
    for (u32 i = 0; i < n; i++) {
        u32 idx = (g_viol_next + FE_PROT_VIOLATION_LOG - 1 - i) % FE_PROT_VIOLATION_LOG;
        out[i] = g_viol[idx];
    }
    return n;
}

u32 fe_protect_list(struct fe_protect_info *out, u32 cap)
{
    if (!out || cap == 0) {
        return 0;
    }
    u32 n = g_extent_count < cap ? g_extent_count : cap;
    for (u32 i = 0; i < n; i++) {
        out[i].lba = g_extents[i].lba;
        out[i].count = g_extents[i].count;
        out[i].mode = g_extents[i].mode;
        out[i].exempt_task = g_extents[i].exempt_task;
        out[i]._pad = 0;
    }
    return n;
}

void fe_protect_dump(void)
{
    if (g_extent_count == 0) {
        fe_kprintf("  **没有登记任何受保护区间——写保护未生效**\n");
        return;
    }
    fe_kprintf("  受保护扇区区间 %u 段（只读，无解除路径）:\n", g_extent_count);
    for (u32 i = 0; i < g_extent_count && i < 8; i++) {
        fe_kprintf("    LBA %-10llu + %-8llu  %s%s\n",
                   (unsigned long long)g_extents[i].lba,
                   (unsigned long long)g_extents[i].count,
                   (g_extents[i].mode & FE_PROT_DENY_READ) ? "禁读禁写" : "禁写",
                   g_extents[i].exempt_task ? "（有豁免者）" : "");
    }
    if (g_extent_count > 8) {
        fe_kprintf("    …（其余 %u 段略）\n", g_extent_count - 8);
    }
    fe_kprintf("  累计拒绝访问 %llu 次\n", (unsigned long long)g_viol_total);
}

/* ------------------------------------------------------------------ */
/* A/B 清单解析                                                        */
/* ------------------------------------------------------------------ */

static bool mparse_u64(const char *s, u32 len, u64 *out)
{
    u64 v = 0;
    u32 i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) {
        i++;
    }
    if (i >= len || s[i] < '0' || s[i] > '9') {
        return false;
    }
    for (; i < len && s[i] >= '0' && s[i] <= '9'; i++) {
        v = v * 10 + (u64)(s[i] - '0');
    }
    *out = v;
    return true;
}

u32 fe_protect_load_manifest(const char *text, u64 len, char boot_slot)
{
    if (!text || len == 0) {
        return 0;
    }
    u32 added = 0;
    u64 pos = 0;
    while (pos < len) {
        /* 取一行 */
        u64 start = pos;
        while (pos < len && text[pos] != '\n') {
            pos++;
        }
        u64 line_len = pos - start;
        if (pos < len) {
            pos++;                  /* 跳过 '\n' */
        }
        const char *line = text + start;
        /* 去掉尾部空白（占位文件用空格补齐） */
        while (line_len > 0 && (line[line_len - 1] == ' ' ||
                                line[line_len - 1] == '\r' ||
                                line[line_len - 1] == '\t')) {
            line_len--;
        }
        if (line_len == 0 || line[0] == '#') {
            continue;
        }

        /* slot <a|b> <lba> <count> */
        if (line_len > 5 && memcmp(line, "slot ", 5) == 0) {
            char s = line[5];
            if (s != 'a' && s != 'b') {
                continue;
            }
            /* 从 "slot <s> " 之后取两个数 */
            u32 p = 7;
            while (p < line_len && line[p] == ' ') {
                p++;
            }
            u64 lba = 0, count = 0;
            u32 q = p;
            while (q < line_len && line[q] != ' ') {
                q++;
            }
            if (!mparse_u64(line + p, q - p, &lba)) {
                continue;
            }
            while (q < line_len && line[q] == ' ') {
                q++;
            }
            u32 r = q;
            while (r < line_len && line[r] != ' ') {
                r++;
            }
            if (!mparse_u64(line + q, r - q, &count)) {
                continue;
            }

            /* ★ 访问矩阵就落在这里 ★
             * 正在运行的槽：读允许、写禁止。
             * 另一个槽：**连读都不行**——它是"下次要运行的一整份系统"，
             * 让它可读可写就等于把更新通道变成任何进程都能用的公共草稿纸，
             * "更新"与"破坏"在机制上就分不出来了。
             * 唯一的例外是被内核指定的更新器（清单里的 updater 行）。 */
            u32 mode = (s == boot_slot) ? FE_PROT_DENY_WRITE
                                        : (FE_PROT_DENY_ALL | FE_PROT_EXEMPT_UPDATER);
            if (fe_ok(fe_protect_add(lba, count, mode, 0))) {
                added++;
            }
            continue;
        }

        /* misc <lba> <count> —— 槽状态记录所在。**不保护**：
         * 它是"下次启动谁"的投票，必须可写；内容极小且带 CRC32，
         * 改坏了也只会退回另一份记录。 */
        if (line_len > 5 && memcmp(line, "misc ", 5) == 0) {
            u32 p = 5;
            u64 lba = 0, count = 0;
            u32 q = p;
            while (q < line_len && line[q] != ' ') {
                q++;
            }
            if (mparse_u64(line + p, q - p, &lba)) {
                while (q < line_len && line[q] == ' ') {
                    q++;
                }
                u32 r = q;
                while (r < line_len && line[r] != ' ') {
                    r++;
                }
                if (mparse_u64(line + q, r - q, &count)) {
                    g_ab.misc_lba = lba;
                    g_ab.misc_count = count;
                    g_ab.flags |= FE_AB_FLAG_SLOT_STATE;
                }
            }
            continue;
        }

        /* bootsel <lba> <offset> —— 决定下次启动哪个槽的那个字节。
         * **不保护**：切换它就是"更新完成"的标志。改错的最坏后果是
         * 启动了另一个槽，而那正是回滚要做的事。 */
        if (line_len > 8 && memcmp(line, "bootsel ", 8) == 0) {
            u32 p = 8;
            u64 lba = 0, off = 0;
            u32 q = p;
            while (q < line_len && line[q] != ' ') {
                q++;
            }
            if (mparse_u64(line + p, q - p, &lba)) {
                while (q < line_len && line[q] == ' ') {
                    q++;
                }
                u32 r = q;
                while (r < line_len && line[r] != ' ') {
                    r++;
                }
                if (mparse_u64(line + q, r - q, &off)) {
                    g_ab.bootsel_lba = lba;
                    g_ab.bootsel_offset = off;
                    g_ab.flags |= FE_AB_FLAG_BOOTSEL;
                }
            }
            continue;
        }

        /* updater <槽内相对路径> —— 指定唯一能碰另一个槽的进程。
         * 存成**绝对路径**（补上前缀），因为内核创建进程时拿到的是绝对路径。 */
        if (line_len > 8 && memcmp(line, "updater ", 8) == 0) {
            u32 k = 0;
            g_updater_path[k++] = '/';
            g_updater_path[k++] = 's';
            g_updater_path[k++] = 'l';
            g_updater_path[k++] = 'o';
            g_updater_path[k++] = 't';
            g_updater_path[k++] = '_';
            g_updater_path[k++] = boot_slot;
            g_updater_path[k++] = '/';
            for (u32 i = 8; i < line_len && k < sizeof(g_updater_path) - 1; i++) {
                g_updater_path[k++] = line[i];
            }
            g_updater_path[k] = '\0';
            continue;
        }
        /* 不认识的键直接忽略（清单将来会加东西，旧内核不该因此报错）。 */
    }
    g_ab.boot_slot = (boot_slot == 'b') ? 1u : 0u;
    return added;
}

/* ------------------------------------------------------------------ */

#define CHECK(cond) do { if (!(cond)) { fail++; \
    fe_kprintf("[保护] 自检失败: %s（protect.c:%d）\n", #cond, __LINE__); } } while (0)

/* 断言"这一段是不是禁读禁写"必须按**位**判，不能按相等判。
 *
 * mode 里除了 DENY_* 还装着别的标志（比如 FE_PROT_EXEMPT_UPDATER），
 * 所以 `mode == FE_PROT_DENY_ALL` 在"另一个槽"上永远不成立——
 * 而它在"运行中的槽"上恰好成立，于是症状是**只有一半断言失败**。
 * 这类"相等 vs 包含"的错在这里尤其贵：真正的语义是
 * "这一段的禁止集合里有没有读和写"，不是"它的模式字面量等于几"。 */
#define MODE_IS_DENY_ALL(m) (((m) & FE_PROT_DENY_ALL) == FE_PROT_DENY_ALL)

u32 fe_selftest_protect(void)
{
    u32 fail = 0;
    const u64 base = 0x00FFFFFFull;     /* 不会与真实布局重叠的高 LBA */

    /* ★ 先保存真实表，测完原样恢复 ★
     *
     * 自检要用一张干净的表才能验"没有条目时会怎样"，所以它必须能清空。
     * 但清空的是**真实状态**：一开始我直接在结尾调 fe_protect_init()，
     * 结果把引导期从 A/B 清单登记的区间全抹掉了，连违规计数也一起清零——
     * 症状是"内核说登记了 40 段，用户态查却是 0 段"。
     *
     * 用保存/恢复而不是"靠调用顺序避开"：顺序是不可靠的约定，
     * 谁把自检挪到前面就静默失效；保存/恢复无论顺序如何都成立。
     * （资源池自检也踩过同一个坑，那次用的是"测完重新 bootstrap"。）
     *
     * ★ 这个坑我踩了第二次，而且形式一模一样 ★
     * 第一次漏的是区间表本身；这一次漏的是 `g_ab`——清空它的时候没人用，
     * 所以"保存/恢复"看起来是完整的。等到 init 真的去问 A/B 布局时，
     * 症状变成"清单明明解析了 46 段，状态机却说没有 A/B 布局"。
     *
     * 教训不是"这次记得加上 g_ab"，而是：**fe_protect_init() 清掉什么，
     * 这里就必须保存什么**。所以下面把 init 清空的每一样都存下来，
     * 而不是挑"目前看起来会被用到的"那几个。 */
    static struct fe_prot_extent saved_ext[FE_PROT_MAX_EXTENTS];
    static struct fe_protect_violation saved_viol[FE_PROT_VIOLATION_LOG];
    static struct fe_ab_info saved_ab;
    static char saved_updater_path[sizeof(g_updater_path)];
    u32 saved_count = g_extent_count;
    u32 saved_next = g_viol_next;
    u64 saved_total = g_viol_total;
    u64 saved_tick = g_last_print_tick;
    u32 saved_printed = g_printed;
    u64 saved_updater_task = g_updater_task;
    saved_ab = g_ab;
    memcpy(saved_updater_path, g_updater_path, sizeof(saved_updater_path));
    for (u32 i = 0; i < FE_PROT_MAX_EXTENTS; i++) {
        saved_ext[i] = g_extents[i];
    }
    for (u32 i = 0; i < FE_PROT_VIOLATION_LOG; i++) {
        saved_viol[i] = g_viol[i];
    }

    fe_protect_init();
    CHECK(fe_protect_extent_count() == 0);
    CHECK(!fe_protect_active());

    /* 运行中的槽：禁写、可读 */
    CHECK(fe_ok(fe_protect_add(base, 100, FE_PROT_DENY_WRITE, 0)));
    CHECK(fe_protect_active());
    CHECK(fe_ok(fe_protect_check(base + 10, 1, false, 0)));          /* 读允许 */
    CHECK(fe_protect_check(base + 10, 1, true, 0) == FE_ERR_ACCESS); /* 写拒绝 */

    /* 另一个槽：禁读禁写 */
    CHECK(fe_ok(fe_protect_add(base + 200, 100, FE_PROT_DENY_ALL, 0)));
    CHECK(fe_protect_check(base + 210, 1, false, 0) == FE_ERR_ACCESS);  /* 读也拒 */
    CHECK(fe_protect_check(base + 210, 1, true, 0) == FE_ERR_ACCESS);

    /* **部分重叠也要拒**——只判"完全包含"的话，跨边界的访问就能
     * 把受保护数据读/改掉一半。这是最容易写错的地方。 */
    CHECK(fe_protect_check(base - 1, 2, true, 0) == FE_ERR_ACCESS);
    CHECK(fe_protect_check(base + 99, 2, true, 0) == FE_ERR_ACCESS);
    CHECK(fe_protect_check(base + 150, 100, false, 0) == FE_ERR_ACCESS);  /* 跨两段 */

    /* 区间外放行 */
    CHECK(fe_ok(fe_protect_check(base - 1, 1, true, 0)));
    CHECK(fe_ok(fe_protect_check(base + 100, 1, true, 0)));
    CHECK(fe_ok(fe_protect_check(0, 1, true, 0)));

    /* 回绕一律拒绝 */
    CHECK(fe_protect_check(~0ull, 2, true, 0) == FE_ERR_ACCESS);
    CHECK(fe_protect_check(~0ull, 2, false, 0) == FE_ERR_ACCESS);

    /* 豁免：只有被点名的那个任务能突破 */
    CHECK(fe_ok(fe_protect_add(base + 400, 100, FE_PROT_DENY_ALL, 0x1234)));
    CHECK(fe_ok(fe_protect_check(base + 410, 1, true, 0x1234)));
    CHECK(fe_ok(fe_protect_check(base + 410, 1, false, 0x1234)));
    CHECK(fe_protect_check(base + 410, 1, true, 0x9999) == FE_ERR_ACCESS);
    CHECK(fe_protect_check(base + 410, 1, true, 0) == FE_ERR_ACCESS);  /* 0 = 不豁免 */

    /* 重叠登记被拒 */
    CHECK(fe_protect_add(base + 50, 10, FE_PROT_DENY_WRITE, 0) == FE_ERR_EXIST);
    CHECK(fe_protect_add(base - 50, 100, FE_PROT_DENY_WRITE, 0) == FE_ERR_EXIST);
    /* 相邻可以（半开区间） */
    CHECK(fe_ok(fe_protect_add(base + 100, 10, FE_PROT_DENY_WRITE, 0)));
    CHECK(fe_protect_extent_count() == 4);

    /* LIST 导出与表一致 */
    struct fe_protect_info info[8];
    u32 n = fe_protect_list(info, 8);
    CHECK(n == 4);
    CHECK(info[0].lba == base && info[0].mode == FE_PROT_DENY_WRITE);
    CHECK(info[2].exempt_task == 0x1234);

    /* 违规都记下来了（每条拒绝路径都要有痕迹） */
    u64 n0 = fe_protect_violation_count();
    CHECK(n0 >= 8);
    struct fe_protect_violation v[16];
    u32 got = fe_protect_get_violations(v, 16);
    /* 能取回的是 min(总数, 环形缓冲格数) —— 不是"格数"。
     * 我第一版写 got == FE_PROT_VIOLATION_LOG，那是把"缓冲有多大"
     * 当成了"一定填满了"，而上面只制造了 10 次违规。 */
    u32 expect = (n0 < FE_PROT_VIOLATION_LOG) ? (u32)n0 : FE_PROT_VIOLATION_LOG;
    CHECK(got == expect);
    /* 最新一条在最前：上一条拒绝是"0 也想要豁免"那次 */
    CHECK(v[0].lba == base + 410);
    /* 记录里必须是**当时正在跑的那个任务**。
     * 不能写死 0：自检跑在内核主线程上，而内核任务的 id 是 1 不是 0。
     * 断言的对象应该是"记的是谁"，不是"我猜它是几"。 */
    struct fe_task *self = fe_task_current();
    CHECK(v[0].task_id == (self ? self->id : 0));
    int saw_read = 0;
    for (u32 i = 0; i < got; i++) {
        if (v[i].write == 0) {
            saw_read = 1;
        }
    }
    CHECK(saw_read);                    /* 读违规确实被记了 */

    /* ---- 清单解析 ---- */
    fe_protect_init();
    static const char manifest[] =
        "# 注释行\n"
        "misc 2080 32\n"
        "slot a 4000 100\n"
        "slot b 5000 100\n"
        "bootsel 3655 146\n";
    u32 added = fe_protect_load_manifest(manifest, sizeof(manifest) - 1, 'a');
    CHECK(added == 2);                  /* 只有 slot 两行进表 */
    CHECK(fe_protect_extent_count() == 2);

    n = fe_protect_list(info, 8);
    CHECK(n == 2);
    /* 槽 A 在跑 → 禁写可读 */
    CHECK(info[0].lba == 4000 && info[0].mode == FE_PROT_DENY_WRITE);
    /* 槽 B 没在跑 → 禁读禁写 */
    CHECK(info[1].lba == 5000 && MODE_IS_DENY_ALL(info[1].mode));
    CHECK(fe_ok(fe_protect_check(4000, 1, false, 0)));
    CHECK(fe_protect_check(4000, 1, true, 0) == FE_ERR_ACCESS);
    CHECK(fe_protect_check(5000, 1, false, 0) == FE_ERR_ACCESS);

    /* 换个槽启动，规则应当反过来 */
    fe_protect_init();
    added = fe_protect_load_manifest(manifest, sizeof(manifest) - 1, 'b');
    CHECK(added == 2);
    n = fe_protect_list(info, 8);
    CHECK(MODE_IS_DENY_ALL(info[0].mode));         /* A 不在跑了 */
    CHECK(info[1].mode == FE_PROT_DENY_WRITE);      /* B 在跑 */

    /* 收尾：把真实表原样放回去（原因见函数开头） */
    fe_protect_init();
    g_extent_count = saved_count;
    g_viol_next = saved_next;
    g_viol_total = saved_total;
    g_last_print_tick = saved_tick;
    g_printed = saved_printed;
    g_ab = saved_ab;
    g_updater_task = saved_updater_task;
    memcpy(g_updater_path, saved_updater_path, sizeof(g_updater_path));
    for (u32 i = 0; i < FE_PROT_MAX_EXTENTS; i++) {
        g_extents[i] = saved_ext[i];
    }
    for (u32 i = 0; i < FE_PROT_VIOLATION_LOG; i++) {
        g_viol[i] = saved_viol[i];
    }
    CHECK(fe_protect_extent_count() == saved_count);
    /* 自检要能证明"恢复"真的发生了，而不只是"我以为恢复了"：
     * 布局信息（flags）是这一轮新加的受害者，所以专门断言它。 */
    CHECK(g_ab.flags == saved_ab.flags);
    return fail;
}
