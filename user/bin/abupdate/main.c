/* SPDX-License-Identifier: 0BSD */
/* abupdate —— A/B 更新器：把当前槽逐段拷到另一个槽，然后翻转引导控制块。
 *
 * ★ 它是**唯一**被内核指定、能碰另一个槽的进程。★
 * 依据是清单里的 `updater bin/abupdate` 那一行：内核在创建进程时看到这个路径，
 * 就把该任务的 id 记为更新器。其它任何进程去读/写另一个槽都会被拒绝。
 * ——"谁能突破保护"只能由拿着清单的那一方决定，不能由进程自报。
 *
 * 更新分三步，顺序不能反：
 *   1. **预扫描**：先只读地比一遍两个槽。全都一样就什么都不做。
 *   2. **把内容写完并刷盘**（blkd 的 PIO 写带 FLUSH CACHE）；
 *   3. **再翻引导控制块**。翻控制块是提交点：翻了之后下次启动就用新槽。
 * 如果反过来先翻再写，中途断电就会启动一个半成品。
 *
 * ★ 为什么第 1 步不是多余的 ★
 * "更新"的语义是**让另一个槽变成和运行槽一样**。已经一样的时候，
 * 拷贝 + 翻转控制块的结果是：两个槽内容本来就相同，却白翻了一次——
 * 下次启动换了个槽，什么也没变，而"切换"这个动作被消费掉了。
 * 更糟的是它不可重入：同一个请求执行两次会来回翻。
 * 加上预扫描之后，这个程序是**幂等**的，而幂等正是"可以安全重试"的前提。
 *
 * 本轮是**逐段块拷贝**（与 Android 拷分区镜像同一思路），不是文件级差异更新。
 * 之所以够用：mkfat 已经保证两个槽的区间**逐段对齐**（段数相同、长度相同）。
 * 文件级差异更新要等 fsd 的写路径。
 *
 * 退出码 = 失败项数。 */
#include <fe_user.h>
#include <fe_blk.h>

static u32 g_fail;
static long g_blk = -1;
static struct fe_ab_info g_ab;

static void say(const char *s) { fe_puts(s); }
static void num(u64 v) { fe_print_u64(v); }

static void check(int cond, const char *what)
{
    say(cond ? "    OK   " : "    失败 ");
    say(what);
    say("\n");
    if (!cond) {
        g_fail++;
    }
}

int main(void)
{
    say("\n=== A/B 更新器 ===\n");

    long r = fe_ab_get_info(&g_ab);
    check(r == FE_OK, "取得 A/B 布局信息");
    if (r != FE_OK) {
        return (int)g_fail;
    }
    say("         当前槽: ");
    say(g_ab.boot_slot == 0 ? "A" : "B");
    say("；引导控制块 LBA ");
    num(g_ab.bootsel_lba);
    say(" 偏移 ");
    num(g_ab.bootsel_offset);
    say("\n");

    /* 身份核对：内核没把我认成更新器的话，后面的写一定会被拒。
     * 提前报出来，比等到"写失败"再猜原因清楚得多。 */
    check(g_ab.updater_task != 0, "内核已把本进程指定为更新器");
    if (g_ab.updater_task == 0) {
        say("         （清单里没有 updater 行，或本进程的路径不匹配）\n");
        say("=== A/B 更新器结束，失败项 ");
        num((u64)g_fail);
        say(" ===\n");
        return (int)g_fail;
    }

    g_blk = fe_devfs_open("/dev/blk0");
    check(g_blk > 0, "找到块设备服务");
    if (g_blk <= 0) {
        say("=== A/B 更新器结束，失败项 ");
        num((u64)g_fail);
        say(" ===\n");
        return (int)g_fail;
    }

    /* 从内核取区间表，按模式分成"源槽"与"目标槽"。
     * 源 = 正在运行的槽（禁写）；目标 = 另一个槽（禁读禁写 + 更新器豁免）。
     *
     * ★ 缓冲按内核上限开，并核对没有被截断 ★
     * 这里原来是 ext[64] + src/dst[32]，而内核最多登记 256 段。
     * 区间数超过容量时拿到的是**前缀**，而前缀与全表看起来一样——
     * 对更新器来说那不是"少检查几段"，是**拷贝的段少了**，
     * 也就是更新出一个不完整的槽。所以两处都要能发现截断：
     *   - 先问总数，断言"我拿到的 == 总数"；
     *   - 不再用 `nsrc < 32` 这类**悄悄丢段**的写法：宁可让下面的
     *     对齐检查失败，也不要静默地少拷一段。 */
    static struct fe_protect_info ext[FE_PROT_MAX_EXTENTS];
    long total = fe_protect_list(NULL, 0);
    long n = fe_protect_list(ext, FE_PROT_MAX_EXTENTS);
    check(n > 0, "取得受保护区间表");
    check(total == n, "区间表没有被缓冲容量截断（段数完整）");
    if (n <= 0 || total != n) {
        goto done;
    }

    static struct fe_protect_info src[FE_PROT_MAX_EXTENTS];
    static struct fe_protect_info dst[FE_PROT_MAX_EXTENTS];
    u32 nsrc = 0, ndst = 0;
    for (long i = 0; i < n; i++) {
        if (ext[i].mode == FE_PROT_DENY_WRITE) {
            src[nsrc++] = ext[i];
        } else if ((ext[i].mode & FE_PROT_DENY_ALL) == FE_PROT_DENY_ALL) {
            dst[ndst++] = ext[i];
        }
    }
    say("         源槽 ");
    num(nsrc);
    say(" 段，目标槽 ");
    num(ndst);
    say(" 段\n");
    check(nsrc > 0 && nsrc == ndst, "两个槽区间逐段对齐（段数相同）");
    if (nsrc == 0 || nsrc != ndst) {
        goto done;
    }

    /* 核对每段长度也相同——只对段数不够，长度不同就是错位拷贝。 */
    u32 mismatch = 0;
    u64 total_sectors = 0;
    for (u32 i = 0; i < nsrc; i++) {
        if (src[i].count != dst[i].count) {
            mismatch++;
        }
        total_sectors += src[i].count;
    }
    check(mismatch == 0, "对应区间长度相同");
    say("         待比较 ");
    num(total_sectors);
    say(" 扇区（");
    num(total_sectors / 2);
    say(" KiB）\n");
    if (mismatch) {
        goto done;
    }

    /* ---- 先证明访问矩阵确实在起作用 ---- */
    say("  [1] 先确认我自己处在什么位置\n");
    {
        static u8 tmp[FE_BLK_SECTOR];
        /* 正向对照：读**运行中的槽**必须成功。
         * 没有这条，"后面的读全都失败"和"整条读路径是坏的"就分不出来。 */
        check(fe_blk_read(g_blk, src[0].lba, 1, tmp) == 0, "运行中的槽：读放行");
        /* 负向对照：写运行中的槽必须被拒——它正在跑，谁都不能改它。 */
        check(fe_blk_write(g_blk, src[0].lba, 1, tmp) < 0, "运行中的槽：写被拒绝");

        /* ★ 另一边的读，在我这里**必须成功** ★
         * 这不是"少验了一条"，而是两个身份看到的东西本就不同：
         *   protcheck（普通进程）对另一个槽读/写都被拒；
         *   我（内核按清单指定的更新器）被豁免，因为更新就是"读源、写目标"。
         * 如果这里也期望被拒，那更新根本没法完成——测试就会把正确行为报成失败。 */
        check(fe_blk_read(g_blk, dst[0].lba, 1, tmp) == 0,
              "另一个槽：读**放行**（我是被指定的更新器，这正是豁免的意义）");
    }

    /* ---- 预扫描：先只读地比一遍，决定要不要更新 ---- */
    say("  [2] 预扫描（只读比较两个槽）\n");
    static u8 bs[FE_BLK_MAX_CHUNK * FE_BLK_SECTOR];
    static u8 bd[FE_BLK_MAX_CHUNK * FE_BLK_SECTOR];
    u32 diff_sectors = 0;
    u32 scanned = 0;
    u32 scan_err = 0;
    for (u32 i = 0; i < nsrc; i++) {
        for (u64 off = 0; off < src[i].count; off += FE_BLK_MAX_CHUNK) {
            u32 chunk = FE_BLK_MAX_CHUNK;
            if (off + chunk > src[i].count) {
                chunk = (u32)(src[i].count - off);
            }
            if (fe_blk_read(g_blk, src[i].lba + off, chunk, bs) != 0 ||
                fe_blk_read(g_blk, dst[i].lba + off, chunk, bd) != 0) {
                scan_err++;
                break;
            }
            for (u32 s = 0; s < chunk; s++) {
                for (u32 k = 0; k < FE_BLK_SECTOR; k++) {
                    if (bs[s * FE_BLK_SECTOR + k] != bd[s * FE_BLK_SECTOR + k]) {
                        diff_sectors++;
                        break;
                    }
                }
            }
            scanned += chunk;
        }
        if (scan_err) {
            break;
        }
    }
    say("         扫描 ");
    num(scanned);
    say(" 扇区，其中不同的 ");
    num(diff_sectors);
    say(" 个\n");
    check(scan_err == 0, "预扫描完成（读源槽允许、读目标槽走豁免）");
    if (scan_err) {
        goto done;
    }

    if (diff_sectors == 0) {
        say("  [3] 两个槽内容一致 → **本次没有可更新的东西**，不动引导控制块\n");
        say("         这就是幂等：同一个更新请求执行两次，第二次什么都不做，\n");
        say("         而不会白翻一次控制块、把「切换」这个动作浪费掉。\n");
        goto done;
    }

    /* ---- 逐段拷贝 ---- */
    say("  [3] 逐段拷贝到另一个槽\n");
    u32 bad = 0;
    u64 copied = 0;
    u64 t0 = fe_clock_ns();
    for (u32 i = 0; i < nsrc; i++) {
        for (u64 off = 0; off < src[i].count; off += FE_BLK_MAX_WRITE) {
            /* 写侧一次只能 1 个扇区（请求要带头，见 fe_blk.h 的上限说明），
             * 所以拷贝的步长按**写**的上限走，而不是按读的。 */
            u32 chunk = FE_BLK_MAX_WRITE;
            if (off + chunk > src[i].count) {
                chunk = (u32)(src[i].count - off);
            }
            if (fe_blk_read(g_blk, src[i].lba + off, chunk, bs) != 0) {
                say("         区间 ");
                num(i);
                say(" 偏移 ");
                num(off);
                say(" **读失败**\n");
                bad++;
                break;
            }
            if (fe_blk_write(g_blk, dst[i].lba + off, chunk, bs) != 0) {
                say("         区间 ");
                num(i);
                say(" 偏移 ");
                num(off);
                say(" **写失败**\n");
                bad++;
                break;
            }
            copied += chunk;
            /* 进度是长任务的基本礼貌，也是**定位手段**：
             * 卡住时"最后打出来的那个数字"直接指出卡在哪一段。
             * 拷完之后这些行还能给出真实吞吐，比"很快"这种话有用。 */
            if (copied % 512 == 0) {
                say("         ... 已拷贝 ");
                num(copied);
                say("/");
                num(total_sectors);
                say(" 扇区，用时 ");
                num((fe_clock_ns() - t0) / 1000000ull);
                say(" ms\n");
            }
        }
    }
    say("         拷贝结束：");
    num(copied);
    say(" 扇区，用时 ");
    num((fe_clock_ns() - t0) / 1000000ull);
    say(" ms\n");
    check(bad == 0, "所有区间拷贝完成（读源槽允许、写目标槽走豁免）");
    if (bad) {
        say("         失败区间数: ");
        num(bad);
        say("\n");
        goto done;
    }

    /* ---- 屏障：在提交之前把数据真正推上盘 ----
     * ★ 位置就是全部意义 ★
     * 刷新必须发生在"翻引导控制块"**之前**：翻了控制块就等于宣布
     * "新槽可用了"，而如果那时数据还在写缓存里，掉电就会启动一个半成品。
     * 反过来，刷新也不需要出现在别的任何地方——每次写后面都刷一遍
     * 只是把 14 秒花在一件只需要做一次的事情上。 */
    say("  [4] 刷写缓存（提交前的屏障）\n");
    int fr = fe_blk_flush(g_blk);
    if (fr == FE_BLK_FLUSH_OK) {
        check(1, "写缓存已刷到盘上（屏障成立）");
    } else if (fr == FE_BLK_FLUSH_NOGUARD) {
        say("    **提示** 驱动器不支持 FLUSH CACHE：数据已交出，但没有屏障保证。\n");
        say("             这里**不当作失败**——重试不能让一个不支持 flush 的盘变得支持。\n");
        check(1, "刷新已发出（无屏障保证）");
    } else {
        check(0, "刷写缓存失败");
        goto done;
    }

    /* ---- 回读核对：拷贝必须逐字节可信 ---- */
    say("  [5] 回读核对\n");
    u32 diff = 0;
    u32 checked = 0;
    for (u32 i = 0; i < nsrc && diff == 0; i++) {
        for (u64 off = 0; off < src[i].count; off += FE_BLK_MAX_CHUNK) {
            u32 chunk = FE_BLK_MAX_CHUNK;
            if (off + chunk > src[i].count) {
                chunk = (u32)(src[i].count - off);
            }
            if (fe_blk_read(g_blk, src[i].lba + off, chunk, bs) != 0 ||
                fe_blk_read(g_blk, dst[i].lba + off, chunk, bd) != 0) {
                diff++;
                break;
            }
            for (u32 k = 0; k < chunk * FE_BLK_SECTOR; k++) {
                if (bs[k] != bd[k]) {
                    diff++;
                    break;
                }
            }
            checked += chunk;
        }
    }
    say("         核对 ");
    num(checked);
    say(" 扇区，不一致 ");
    num(diff);
    say(" 处\n");
    check(diff == 0, "目标槽与源槽逐字节一致");
    if (diff) {
        goto done;
    }

    /* ---- 提交：翻引导控制块 ---- */
    say("  [6] 提交（翻转引导控制块）\n");
    u8 want = (g_ab.boot_slot == 0) ? FE_AB_BOOTSEL_B : FE_AB_BOOTSEL_A;
    int pr = fe_blk_patch_byte(g_blk, g_ab.bootsel_lba, g_ab.bootsel_offset, want);
    check(pr == 0, "引导控制块已写入");
    if (pr == 0) {
        static u8 sec[FE_BLK_SECTOR];
        if (fe_blk_read(g_blk, g_ab.bootsel_lba, 1, sec) == 0) {
            say("         控制块现在是 '");
            fe_write((const char *)&sec[g_ab.bootsel_offset], 1);
            say("'（下次启动槽 ");
            say(sec[g_ab.bootsel_offset] == FE_AB_BOOTSEL_A ? "A" : "B");
            say("）\n");
            check(sec[g_ab.bootsel_offset] == want, "回读确认控制块已生效");
        }
    }
    say("         注：槽状态记录由 init 负责改写（更新器只管数据与控制块），\n");
    say("             所以「下次启动进新槽」这件事是两边各写一半、合起来才成立的。\n");

done:
    if (g_blk > 0) {
        fe_handle_close(g_blk);
    }
    say("=== A/B 更新器结束，失败项 ");
    num((u64)g_fail);
    say(" ===\n");
    return (int)g_fail;
}
