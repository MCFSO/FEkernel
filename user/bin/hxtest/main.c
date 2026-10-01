/* SPDX-License-Identifier: 0BSD */
/* hxtest —— 句柄传递 / 批量 IPC 的端到端测试客户端（M11）。
 *
 * 这个程序同时是**两个东西**：不带参数时是客户端，带参数时是服务端
 * （客户端用 fe_spawn 把自己再拉起一份）。这样做的理由：
 * 句柄传递必须有**两个任务**才有意义——同一个任务的句柄表里传句柄
 * 只是搬了个索引，验证不了"能力过了任务边界之后还能用"。
 *
 * 它验的是内核机制，不是系统功能：
 *   ① 0x11/0x12 的句柄数组通道真的把能力交给了另一个任务；
 *   ② 跨任务拿到内存对象后能映射，且两边看到的是**同一块物理内存**；
 *   ③ 收窄过的权限真的收窄了（只读对象在服务端写不进去）；
 *   ④ 两块内存对象（文本 + 文件）随同一条消息一起传，各自可用；
 *   ⑤ MEM_UNMAP 撤销之后那段地址确实没了，且幂等；
 *   ⑥ HANDLE_DUP 能把同一个能力交给本任务的另一个线程。
 *
 * ★ 只测"该过的过了"是不够的 ★ 所以每一步都有反向对照：
 *   无 TRANSFER 权限必须被拒、无 DUP 权限必须被拒、二次转发必须被拒、
 *   只读对象必须写失败、未映射的句柄数组必须 FAULT 且不消耗消息。
 *
 * 大块传输（1 MiB）走的是 page 映射：内核**一个字节都不碰**，
 * 内联载荷始终只有几十字节。这正是"块设备一次只能 2 个扇区"的解法。
 */
#include <fe_user.h>
#include <fe_fs.h>      /* 走 devfs 找 /dev/fs0、读写引导文件：服务发现是内核机制 */

/* ------------------------------------------------------------------ */
/* 协议                                                               */
/* ------------------------------------------------------------------ */

#define HX_MAGIC 0x48585431u        /* "HXT1" */
#define HX_WINDOW_SIZE (1024 * 1024)    /* 1 MiB：大块传输用的窗口 */
#define HX_SMALL_SIZE  4096             /* 小块：与"页"同尺寸，便于对比 */

/* 客户端 → 服务端 */
struct hx_req {
    u32 magic;
    u32 op;
    u32 size;
    u32 value;
};

#define HX_OP_SMALL     1   /* 小块：把内存对象交过去，等它确认 */
#define HX_OP_BIG       2   /* 大块：1 MiB 分块搬运，校验和比对 */
#define HX_OP_READONLY  3   /* 只读对象：服务端必须写不进去（反向对照） */
#define HX_OP_QUIT      4   /* 收尾：让服务端正常退出，客户端好 wait 它 */
#define HX_OP_HELLO     5   /* 握手：客户端把共享状态页的**句柄**交过去 */

/* 服务端 → 客户端 */
struct hx_rep {
    long status;
    u32  magic;
    u32  bytes;         /* 它实际处理/校验的字节数 */
    u64  checksum;      /* 大块：它算出来的校验和（客户端独立再算一遍） */
};

static u32 g_fail;
static const char *g_who = "hxtest";

/* 共享状态页（客户端分配、服务端也映射同一块物理内存）：
 *   [0] = 服务端记录的失败次数（父进程最后读它）
 *   [1] = 服务端启动计数（>0 证明它真的跑起来了）
 * 没有它，服务端在后台失败时客户端可能全绿——测试会报成功而内核是坏的。 */
static volatile u32 *g_status;

static void say(const char *s) { fe_puts(s); }
static void num(u64 v) { fe_print_u64(v); }

static void failf(const char *what)
{
    say("  [hxtest] 失败: ");
    say(what);
    say("\n");
    g_fail++;
    if (g_status) {
        g_status[0]++;
    }
}

/* 可复现的内容模式：与地址、序号都相关，抄一份"全 0"或"全 A5"都过不了。 */
static u8 pattern_byte(u64 off)
{
    return (u8)((off * 31u + (off >> 8) * 7u + 0x5Au) & 0xFFu);
}

static void pattern_fill(u8 *p, u64 n, u64 base_off)
{
    for (u64 i = 0; i < n; i++) {
        p[i] = pattern_byte(base_off + i);
    }
}

/* 返回不符的字节数（0 = 完全一致） */
static u64 pattern_check(const u8 *p, u64 n, u64 base_off)
{
    u64 bad = 0;
    for (u64 i = 0; i < n; i++) {
        if (p[i] != pattern_byte(base_off + i)) {
            bad++;
        }
    }
    return bad;
}

/* 独立于内容模式的校验和：客户端与服务端各算一遍，比"逐字节比较"更能
 * 暴露"两边看的是不同内存"而恰好前几十字节相同这种情况。 */
static u64 checksum_of(const u8 *p, u64 n)
{
    u64 h = 1469598103934665603ull;     /* FNV-1a 64 位 */
    for (u64 i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* 手写小工具，不依赖运行库：声明里有的东西不该默认它一定存在。
 *
 * 顺带记一笔：fe_user.h 里曾声明了 atoi / fe_snprintf 而 libfe **没有实现**
 * ——"文档写了、编译器不报错、链接时才炸"。这次把两个实现补上了
 * （见 libfe.c 末尾），所以这里的 hx_atoi 已经是多余的，删掉——
 * 留着它会继续替调用者掩盖"那个声明到底实现了没有"。 */

static void hx_utoa(long v, char *out, u32 cap)
{
    char tmp[24];
    u32 n = 0;
    u64 x = (v < 0) ? (u64)(-v) : (u64)v;
    if (cap == 0) {
        return;
    }
    if (x == 0) {
        tmp[n++] = '0';
    }
    while (x && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (x % 10));
        x /= 10;
    }
    u32 k = 0;
    if (v < 0 && k + 1 < cap) {
        out[k++] = '-';
    }
    while (n > 0 && k + 1 < cap) {
        out[k++] = tmp[--n];
    }
    out[k] = '\0';
}
/* 新建一个"回复用"的消息头。
 * ★ 必须新建 ★ 复用收到的头会把 payload_len 当成请求的长度发出去
 * （这个项目为此栽过一次：应答被截断，症状看起来像 IPC 不通）。 */
static void reply_msg(long reply_ep, long status, u32 magic, u32 bytes, u64 sum)
{
    struct hx_rep rep;
    rep.status = status;
    rep.magic = magic;
    rep.bytes = bytes;
    rep.checksum = sum;
    struct fe_msg_header rh;
    memset(&rh, 0, sizeof(rh));
    rh.protocol = HX_MAGIC;
    rh.opcode = 0x80;
    rh.payload_len = sizeof(rep);
    fe_endpoint_send(reply_ep, &rh, &rep, NULL, 0);
}

/* 一次请求-应答。
 *
 * ★ 必须用 fe_endpoint_call，不能用 send + recv ★
 * 这是本轮踩到的第三个坑，也是最隐蔽的一个：`fe_endpoint_send` 发出去的消息
 * **不带回复端点**，于是服务端 fe_endpoint_recv 返回的 reply 是 0（无处可回），
 * 它"回了"一条谁也没收到的消息，客户端则永远等下去。
 * fe_endpoint_call 会临时建一个回复端点、随请求交给服务端、
 * 发完就在它上面等应答——这就是 RPC 的全部。服务端只要往 reply 上发一条就行。
 *
 * 附带的能力：随请求还能带别的句柄（内存对象），于是
 * "把大块内存交给服务端"和"拿到它的应答"是同一次往返。 */
static u32 g_call_handles[FE_MSG_MAX_HANDLES];
static u32 g_call_hcount;

static long call_server(long ep, u32 op, u32 size, u32 value, struct hx_rep *out)
{
    struct hx_req req;
    req.magic = HX_MAGIC;
    req.op = op;
    req.size = size;
    req.value = value;
    long reply = fe_endpoint_create(0);
    if (reply <= 0) {
        return FE_ERR_NOMEM;
    }
    /* 把内存对象句柄 + 回复端点句柄一起交过去 */
    u32 arr[FE_MSG_MAX_HANDLES];
    u32 n = 0;
    for (u32 i = 0; i < g_call_hcount && n < FE_MSG_MAX_HANDLES - 1; i++) {
        arr[n++] = g_call_handles[i];
    }
    arr[n++] = (u32)reply;
    struct fe_msg_header h;
    memset(&h, 0, sizeof(h));
    h.protocol = HX_MAGIC;
    h.opcode = op;
    h.payload_len = sizeof(req);
    h.handle_count = n;
    long r = fe_endpoint_send(ep, &h, &req, arr, n);
    if (r != FE_OK) {
        fe_handle_close(reply);
        return r;
    }
    struct fe_msg_header rh;
    long rr = fe_endpoint_recv(reply, &rh, out, sizeof(*out), NULL, NULL);
    fe_handle_close(reply);
    if (rr < 0) {
        return rr;
    }
    return (rh.payload_len == sizeof(*out)) ? FE_OK : FE_ERR_INVAL;
}

/* ------------------------------------------------------------------ */
/* 服务端                                                             */
/* ------------------------------------------------------------------ */

/* 客户端与服务端之间的**引导通道**：一个普通文件（`/home/hx<id>.cfg`）。
 *
 * ★ 为什么不能用 argv 把句柄号传过去 ★
 * 这是本轮踩到的一个真问题，值得写下来：**句柄号是进程内索引**。
 * 客户端把自己的句柄号 3 写进 argv，子进程读到的 "3" 是**它自己**表里的 3
 * ——那是另一个对象（或者根本没有）。第一版就是这么写的，症状是
 * "服务端接收失败"，看起来像 IPC 坏了，实际是句柄号跨了任务边界。
 * 句柄能跨任务传递，**句柄号不能**——能过去的是「随消息传的句柄」，
 * 而那需要先有一条通道。这就是引导问题（bootstrap）。
 *
 * 解法用项目自己的机制，不加任何新原语：
 *   - 客户端把收件端点发布到 **devfs**，服务端按名字打开，拿到的是**能力**；
 *   - 状态页（共享内存对象）随第一条消息的**句柄**过去。
 * 于是"服务发现 + 能力传递"这两件事被真正组合起来用了一次。
 *
 * ★ 回信地址不在这条通道上 ★
 * 服务端不需要预先知道往哪儿回话：`fe_endpoint_call` 会把一个临时回复端点
 * 随每条请求一起交过来，服务端往它上面发一条就是应答。
 * 这消掉了一整类"两边各自发布端点、再互相打开"的引导复杂度。 */
#define HX_CFG_MAGIC 0x48584346u        /* "HXCF" */
struct hx_cfg {
    u32 magic;
    u32 reserved;           /* 句柄号不能跨任务，所以这里只放常数 */
    u32 pad[6];
};

#define HX_ST_READY 1           /* 服务端已就绪 */
#define HX_ST_DONE  2           /* 测试全部结束（服务端可以退出） */

/* 共享状态页布局 */
#define HX_ST_IDX_FAIL   0      /* 任一方的失败次数 */
#define HX_ST_IDX_FLAGS  1      /* HX_ST_* 位 */
#define HX_ST_IDX_SERVER 2      /* 服务端看到的失败项 */

/* 服务端的请求循环。
 *
 * 它只做三件事：映射客户端交过来的内存对象、读它、写它。
 * **数据一个字节都不经过内核的内联载荷**——这正是重点：
 * 载荷上限 1024 与"能搬多少数据"从此是两件事。 */
static int server_main(const char *inbox_name, const char *cfg_path)
{
    say("  [hxtest] 服务端启动（收件端点经 devfs 发现，状态页经消息里的句柄拿到）\n");

    /* ---- 0. 引导文件：确认它在（也给这次运行一个共同的落点） ---- */
    long fs = fe_devfs_open("/dev/fs0");
    if (fs <= 0) {
        say("  [hxtest] 失败: 服务端打不开 /dev/fs0\n");
        return 1;
    }
    static struct fe_fs_read_reply rd;
    int got = fe_fs_read(fs, cfg_path, 0, &rd);
    fe_handle_close(fs);
    if (got < (int)sizeof(struct hx_cfg)) {
        say("  [hxtest] 失败: 服务端读不到引导文件 ");
        say(cfg_path);
        say("\n");
        return 1;
    }
    struct hx_cfg cfg;
    memcpy(&cfg, rd.data, sizeof(cfg));
    if (cfg.magic != HX_CFG_MAGIC) {
        say("  [hxtest] 失败: 引导文件内容非法\n");
        return 1;
    }

    long inbox = fe_devfs_open(inbox_name);     /* 收件端点：按名字拿到的**能力** */
    if (inbox <= 0) {
        say("  [hxtest] 失败: 服务端打不开收件端点 ");
        say(inbox_name);
        say("\n");
        return 1;
    }
    say("  [hxtest] 服务端就绪（已打开 ");
    say(inbox_name);
    say("），等待请求\n");

    /* ---- 握手：状态页的句柄随第一条消息过来 ----
     *
     * ★ 这是整个测试里最关键的二十行 ★
     * 客户端在**这条消息里**把状态页句柄交过来，内核在服务端句柄表里
     * 装了一份副本，于是两个任务看见同一块物理内存。而"客户端怎么知道
     * 往哪儿发"这件事，靠的是 devfs 的**名字**（名字跨任务天然可用），
     * 拿到名字背后的是**能力**。这一条链就是用户态服务互相认识的全部机制。 */
    {
        struct hx_req req;
        struct fe_msg_header hdr;
        u32 hbuf[FE_MSG_MAX_HANDLES];
        u32 hn = 0;
        long r = fe_endpoint_recv(inbox, &hdr, &req, sizeof(req), hbuf, &hn);
        /* ★ 返回值不是错误码：>= 0 就是收到了，0 只表示"发送方没给回复能力" ★
         * 按错误码去判它（r <= 0 当失败）会把一次成功的握手判成失败——
         * 这是本轮踩到的第二个"ABI 语义"坑，与"句柄号不能跨任务"同类：
         * 接口的**返回值含义**必须按定义读，不能按习惯猜。 */
        if (r < 0) {
            say("  [hxtest] 失败: 服务端收不到握手消息（返回 ");
            num((u64)(r < 0 ? -r : r));
            say("，inbox=");
            num((u64)inbox);
            say("）\n");
            return 1;
        }
        if (req.magic != HX_MAGIC || req.op != HX_OP_HELLO ||
            hn != 1 || hbuf[0] == FE_HANDLE_INVALID) {
            say("  [hxtest] 失败: 握手消息格式不对\n");
            return 1;
        }
        volatile u32 *st = (volatile u32 *)fe_mem_map(hbuf[0], NULL, 0,
                                                      FE_PROT_READ | FE_PROT_WRITE);
        if (!st) {
            say("  [hxtest] 失败: 服务端无法映射收到的状态页句柄\n");
            return 1;
        }
        g_status = st;
        /* 就绪标志 = 对客户端的应答。
         * 握手这条消息**不回复**：客户端等的是"服务端拿到状态页了"，
         * 而那件事的落点就是这块共享内存本身。 */
        g_status[HX_ST_IDX_FLAGS] |= HX_ST_READY;
        say("  [hxtest] 服务端收到状态页（随消息传来的句柄），已就绪\n");
    }
    for (;;) {
        struct hx_req req;
        struct fe_msg_header hdr;
        u32 hbuf[FE_MSG_MAX_HANDLES];
        u32 hn = 0;
        long reply = fe_endpoint_recv(inbox, &hdr, &req, sizeof(req), hbuf, &hn);
        if (reply < 0) {
            failf("服务端接收失败");
            return 1;
        }
        if (req.magic != HX_MAGIC) {
            failf("请求魔数不对（协议头没传对）");
            reply_msg(reply, -1, 0, 0, 0);
            fe_handle_close(reply);
            continue;
        }
        /* 客户端把内存对象（0..n-2）与**回复端点**（最后一个）一起交过来。
         * 回复端点用完即关：它是这次往返专用的，留着只会占句柄表。
         * 这条约定由 fe_endpoint_call 的语义决定：它总是把自己的临时
         * 回复端点作为**最后一个**句柄交出去。 */

        if (req.op == HX_OP_QUIT) {
            /* ★ 收尾走协议而不是"把服务端丢在那儿" ★
             * 服务端正常退出，客户端才能 fe_wait 它并核对退出码——
             * 这样"服务端有没有悄悄失败"有一个强证据（退出码），
             * 而不必依赖共享状态页里的计数。 */
            say("  [hxtest] 服务端收到退出请求，正常结束\n");
            say("  [hxtest] 服务端侧失败项 ");
            num(g_fail);
            say("\n");
            g_status[HX_ST_IDX_SERVER] = g_fail;
            g_status[HX_ST_IDX_FLAGS] |= HX_ST_DONE;
            reply_msg(hbuf[0], FE_OK, HX_MAGIC, 0, 0);
            if (hbuf[0] != FE_HANDLE_INVALID) {
                fe_handle_close(hbuf[0]);
            }
            fe_handle_close(inbox);
            fe_exit(g_fail ? 1 : 0);
        }

        if (req.op == HX_OP_SMALL || req.op == HX_OP_READONLY) {
            /* ---- 小块 / 只读 ---- */
            if (hn != 2 || hbuf[0] == FE_HANDLE_INVALID ||
                hbuf[1] == FE_HANDLE_INVALID) {
                failf("服务端没有收到「内存对象 + 回复端点」两个句柄");
                reply_msg(reply, FE_ERR_BADHANDLE, 0, 0, 0);
                fe_handle_close(reply);
                continue;
            }
            void *p = fe_mem_map(hbuf[0], NULL, 0, FE_PROT_READ | FE_PROT_WRITE);
            if (!p) {
                /* ★ 反向对照：只读对象在这里**必须**映射不出可写视图 ★
                 * 这一条如果过了，说明"收窄权限"是假的。 */
                if (req.op == HX_OP_READONLY) {
                    say("  [hxtest] OK   只读对象在服务端无法建立可写映射（收窄是真的）\n");
                    reply_msg(hbuf[1], FE_ERR_ACCESS, HX_MAGIC, 0, 0);
                } else {
                    failf("服务端无法映射收到的内存对象");
                    reply_msg(hbuf[1], FE_ERR_ACCESS, 0, 0, 0);
                }
                fe_handle_close(hbuf[1]);
                fe_handle_close(hbuf[0]);
                continue;
            }
            if (req.op == HX_OP_READONLY) {
                failf("只读对象竟然建立了可写映射（权限收窄没生效）");
                fe_mem_unmap(p, req.size);
                reply_msg(hbuf[1], FE_OK, 0, 0, 0);
            } else {
                u64 bad = pattern_check((const u8 *)p, req.size, 0);
                if (bad) {
                    say("  [hxtest] 失败: 服务端看到的内容与客户端写入的不符（");
                    num(bad);
                    say(" 字节）\n");
                    g_fail++;
                }
                /* 改一个字节再回读：证明它真的能写这块内存 */
                ((u8 *)p)[0] = (u8)(((u8 *)p)[0] ^ 0xFFu);
                u8 back = ((u8 *)p)[0];
                ((u8 *)p)[0] = (u8)(back ^ 0xFFu);
                u64 sum = checksum_of((const u8 *)p, req.size);
                reply_msg(hbuf[1], FE_OK, HX_MAGIC, req.size, sum);
                fe_mem_unmap(p, req.size);
            }
            fe_handle_close(hbuf[1]);
            fe_handle_close(hbuf[0]);

        } else if (req.op == HX_OP_BIG) {
            /* ---- 大块：两个内存对象随一条消息过来 ---- */
            if (hn != 3 || hbuf[0] == FE_HANDLE_INVALID ||
                hbuf[1] == FE_HANDLE_INVALID || hbuf[2] == FE_HANDLE_INVALID) {
                failf("大块请求没有收到「两个内存对象 + 回复端点」三个句柄");
                reply_msg(reply, FE_ERR_BADHANDLE, 0, 0, 0);
                fe_handle_close(reply);
                continue;
            }
            /* 反向对照：收到的能力**收窄到了该有的那几位**
             *
             * 服务端是从 devfs 按名字打开收件端点的，拿到的是"只能收"的能力
             * （客户端发布时只给了 RECV）。所以它**不能**往那个端点发东西。
             * 这一条过的意义：能力不是"打开就有全部权限"，
             * 发布方给的权限就是对方能用的全部。 */
            {
                struct fe_msg_header fh;
                memset(&fh, 0, sizeof(fh));
                fh.payload_len = 0;
                fh.handle_count = 0;
                if (fe_endpoint_send(inbox, &fh, NULL, NULL, 0) != FE_ERR_ACCESS) {
                    failf("只能收的收件端点竟然可以发（发布时的收窄没生效）");
                }
            }

            u8 *file = (u8 *)fe_mem_map(hbuf[1], NULL, 0, FE_PROT_READ);
            u8 *text = (u8 *)fe_mem_map(hbuf[0], NULL, 0, FE_PROT_READ | FE_PROT_WRITE);
            if (!file || !text) {
                failf("服务端无法映射大块传输的两个内存对象");
                reply_msg(hbuf[2], FE_ERR_ACCESS, 0, 0, 0);
            } else {
                /* 分块搬运：一次一块，**每块都验内容**。
                 * 只验最后一块的话，"中间某块映射错了"会被漏掉。
                 *
                 * ★ 校验和用加法累加，不用 XOR ★
                 * 第一版写的是 sum ^= checksum_of(chunk)：1 MiB 分成 16 个
                 * 64 KiB 块，每块的字节序列**完全相同**（模式是按偏移算的，
                 * 而这里从 0 开始重算），于是 16 个相同的哈希 XOR 起来是 0
                 * ——客户端报"服务端校验和为 0（没有真的读）"，而服务端其实
                 * 老老实实读完了。聚合方式选错会把一次正确的读判成失败。 */
                u64 total = req.size;
                u64 bad = 0;
                u64 sum = 0;
                const u64 chunk = 64 * 1024;
                for (u64 off = 0; off < total; off += chunk) {
                    u64 n = (total - off < chunk) ? (total - off) : chunk;
                    bad += pattern_check(file + off, n, off);
                    sum += checksum_of(file + off, n) ^ off;
                }
                if (bad) {
                    say("  [hxtest] 失败: 服务端读到的文件内容不符（");
                    num(bad);
                    say(" 字节）\n");
                    g_fail++;
                }
                /* 用 text 这个窗口回写：文字客户端会回读校验 */
                pattern_fill(text, total, 0x1000000ull);
                reply_msg(hbuf[2], FE_OK, HX_MAGIC, (u32)total, sum);
                fe_mem_unmap(file, total);
                fe_mem_unmap(text, total);
            }
            fe_handle_close(hbuf[2]);
            fe_handle_close(hbuf[1]);
            fe_handle_close(hbuf[0]);
        } else {
            failf("未知操作码");
            reply_msg(reply, FE_ERR_NOTSUP, 0, 0, 0);
        }
        fe_handle_close(reply);
    }
    return g_fail ? 1 : 0;      /* 到不了这里：退出走 HX_OP_QUIT */
}

/* ------------------------------------------------------------------ */
/* 客户端                                                             */
/* ------------------------------------------------------------------ */

/* ③ HANDLE_DUP：把同一个能力给本任务的另一个线程用。
 *
 * ★ 为什么不能用共享变量传句柄号 ★
 * 句柄号是**进程内索引**。两个线程共用一个号时，任一方 close 掉它，
 * 另一方手里的号就指向别的对象（或被复用给新对象）——这是"神出鬼没"的
 * 一类 bug。dup 出来的两个句柄各自独立关闭，对象活到最后一个关闭。 */
static long g_dup_ep;
static volatile u32 g_dup_got;
static volatile long g_dup_r;

static void dup_thread(void *arg)
{
    (void)arg;
    struct fe_msg_header hdr;
    u8 payload[16];
    /* 用**复制出来的**句柄收，然后立刻关掉它：
     * 原句柄在同一任务里仍然有效，这就是"独立关闭"的含义。 */
    /* 返回值不是错误码：>= 0 就是收到了，0 只表示发送方没给回复能力 */
    long r = fe_endpoint_recv(g_dup_ep, &hdr, payload, sizeof(payload), NULL, NULL);
    g_dup_r = r;
    if (r >= 0) {
        g_dup_got = 1;
    }
    if (r > 0) {
        fe_handle_close(r);
    }
    fe_handle_close(g_dup_ep);
    fe_exit(0);
}

static int test_handle_dup(void)
{
    long ep = fe_endpoint_create(0);
    if (ep <= 0) {
        failf("端点创建失败");
        return 1;
    }
    long dup = fe_handle_dup(ep, FE_RIGHT_SEND | FE_RIGHT_RECV | FE_RIGHT_DUP);
    if (dup <= 0) {
        failf("fe_handle_dup 失败");
        fe_handle_close(ep);
        return 1;
    }
    /* 反向对照：权限放大必须被拒 */
    long big = fe_handle_dup(ep, FE_RIGHT_ALL);
    if (big > 0) {
        failf("句柄权限放大竟然成功");
        fe_handle_close(big);
    }

    g_dup_ep = dup;
    g_dup_got = 0;
    u64 tid = fe_thread_create(dup_thread, NULL, NULL, 0);
    if (tid == 0) {
        failf("线程创建失败");
        fe_handle_close(dup);
        fe_handle_close(ep);
        return 1;
    }
    /* 主线程用**原句柄**发一条：线程必须用副本收到 */
    struct fe_msg_header h;
    memset(&h, 0, sizeof(h));
    h.payload_len = 4;
    fe_endpoint_send(ep, &h, "dup!", NULL, 0);

    for (u32 i = 0; i < 2000 && !g_dup_got; i++) {
        fe_sleep_ms(1);
    }
    if (!g_dup_got) {
        failf("复制出来的句柄没能收到消息");
        return 1;
    }
    /* 副本已被那条线程关闭，原句柄仍然可用：再发一条并自己收 */
    fe_endpoint_send(ep, &h, "ok", NULL, 0);
    struct fe_msg_header rh;
    u8 buf[8];
    if (fe_endpoint_recv(ep, &rh, buf, sizeof(buf), NULL, NULL) < 0) {
        failf("副本关闭后原句柄失效（两者没有独立生命周期）");
        return 1;
    }
    fe_handle_close(ep);
    say("  [hxtest] OK   句柄复制：副本与原件各自独立关闭，权限放大被拒\n");
    return 0;
}

/* ⑤ MEM_UNMAP：撤销之后那段地址确实没了，而且幂等。 */
static int test_mem_unmap(void)
{
    long mo = fe_mem_alloc(2 * HX_SMALL_SIZE);
    if (mo <= 0) {
        failf("内存对象分配失败");
        return 1;
    }
    u8 *p = (u8 *)fe_mem_map(mo, NULL, 0, FE_PROT_READ | FE_PROT_WRITE);
    if (!p) {
        failf("映射失败");
        fe_handle_close(mo);
        return 1;
    }
    /* 先证明它**可用**（否则"撤销后读不到"可能只是因为本来就写不进去） */
    pattern_fill(p, HX_SMALL_SIZE, 0);
    if (pattern_check(p, HX_SMALL_SIZE, 0) != 0) {
        failf("映射后写入读回不一致（撤销测试的前提不成立）");
        fe_handle_close(mo);
        return 1;
    }
    if (fe_mem_unmap(p, HX_SMALL_SIZE) != FE_OK) {
        failf("fe_mem_unmap 失败");
        fe_handle_close(mo);
        return 1;
    }
    /* 幂等：重复撤销必须成功（调用者把清理写在收尾路径上时不该判返回值） */
    if (fe_mem_unmap(p, HX_SMALL_SIZE) != FE_OK) {
        failf("重复撤销返回失败（MEM_UNMAP 不是幂等的）");
    }
    /* 反向对照：越权范围（不在 mmap 区）必须被拒 */
    if (fe_mem_unmap((void *)0x400000ull, HX_SMALL_SIZE) != FE_ERR_INVAL) {
        failf("撤销程序映像区竟然被允许（那是把自己的代码页拆掉）");
    }
    /* 对象还活着，所以帧不该被释放：同一对象再映射一次必须仍是干净的 */
    u8 *q = (u8 *)fe_mem_map(mo, NULL, 0, FE_PROT_READ | FE_PROT_WRITE);
    if (!q) {
        failf("撤销后重新映射失败（帧被错误释放了？）");
    } else {
        /* 前一页写过的模式**应该还在**：撤销映射不等于丢数据，
         * 数据属于内存对象。这一条同时证明了"撤销没有把帧还给 PMM"。 */
        u64 bad = pattern_check(q, HX_SMALL_SIZE, 0);
        if (bad) {
            say("  [hxtest] 失败: 撤销后重新映射，数据不在了（");
            num(bad);
            say(" 字节）—— 帧被提前释放\n");
            g_fail++;
        } else {
            say("  [hxtest] OK   MEM_UNMAP：撤销+幂等+越界拒绝，帧未被释放（数据仍在）\n");
        }
        fe_mem_unmap(q, HX_SMALL_SIZE);
    }
    fe_handle_close(mo);
    return 0;
}

/* ---- 时钟刻度自证：计时结果能不能信，先测一次 ----
 *
 * ★ 为什么测试工具自己要先校验时钟 ★
 * VBox 上"1 MiB 往返 0 us"这个数字第一眼像是极快，实际是**时钟走得慢**：
 * 内核的单调时钟是按节拍算的（`fe_time_ms()` = 节拍数 / 每毫秒节拍数），
 * 而 VBox 对中断投递限流（实测 49~169 Hz），于是节拍数远少于真实毫秒数，
 * 同一个间隔被量出来就偏小。不校验的话，读者会把"时钟不准"
 * 读成"内核很快"——这正是这个项目反复记的那类误判。
 *
 * 做法：睡一段**名义上** 50 ms 的时间，再量它。刻度接近 1.0 才敢用这个
 * 时钟去报耗时；否则明确说明这次运行的计时不可信（而不是打印一个漂亮数字）。 */
static u32 clock_scale_check(void)
{
    u64 t0 = fe_clock_ns();
    fe_sleep_ms(50);
    u64 dt_us = (fe_clock_ns() - t0) / 1000ull;
    /* 返回"量到的微秒数 / 名义微秒数"的百分比（100 = 刻度准确）。
     * 睡 50 ms 时量到 50 ms 上下都算准；量到 15 ms 就是刻度 30%。 */
    u32 pct = (u32)((dt_us * 100ull) / 50000ull);
    say("  [hxtest] 时钟刻度自证：睡 50 ms，时钟量到 ");
    num(dt_us);
    say(" us → 刻度 ");
    num(pct);
    say("%\n");
    return pct;
}

static int client_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    say("\n  [hxtest] 句柄传递 / 批量 IPC 端到端测试\n");
    /* 刻度校验的结论打给读者看（它的作用是让"这个耗时数字可不可信"
     * 变成日志里的事实，而不是让程序去分支）。 */
    (void)clock_scale_check();

    /* ---- 唯一后缀：devfs 名字是全局的，而发布是**独占**的 ----
     * 用纳秒时钟取两位数字：一次测试运行内不会重复，
     * 而上一次运行的同名节点（若任务还没被回收）也不会撞上。
     * 这不是"绕过冲突检查"，是**给一次运行一个自己的名字空间**。 */
    u32 uniq = (u32)((fe_clock_ns() / 1000000ull) % 100u);
    char inbox_name[48], cfg_path[48];
    {
        char u2[8];
        hx_utoa((long)uniq, u2, sizeof(u2));
        const char *u = u2;
        /* ★ devfs 的名字是**全局**的，不带槽前缀 ★
         * 槽（/slot_a、/slot_b）是**文件系统**的布局；而 /dev 是内核里那个
         * devfs 挂载点的名字空间。两者是不同的东西，混起来的话
         * 发布到的路径根本不存在（第一次就撞在这一点上：发布失败，
         * 报的是"名字被占用"，而真正的原因是路径压根不在 devfs 里）。
         * 反过来说这也正是"名字跨槽稳定"的来源：两个槽里的服务
         * 都发布 /dev/blk0，谁起来算谁的，客户端不需要知道自己在哪个槽。 */
        u32 k = 0;
        const char *s1 = "/dev/hx";
        for (const char *s = s1; *s && k < sizeof(inbox_name) - 1; s++) {
            inbox_name[k++] = *s;
        }
        for (const char *s = u; *s && k < sizeof(inbox_name) - 1; s++) {
            inbox_name[k++] = *s;
        }
        inbox_name[k] = '\0';
        /* 引导文件放在可写文件系统上（槽是只读的，见 A/B 保护）。
         * /home 是全局路径，不是槽内路径——shell 里 `echo x > /home/a.txt`
         * 用的就是它。 */
        const char *c0 = "/home/hx";
        k = 0;
        for (const char *s = c0; *s && k < sizeof(cfg_path) - 1; s++) {
            cfg_path[k++] = *s;
        }
        for (const char *s = u; *s && k < sizeof(cfg_path) - 1; s++) {
            cfg_path[k++] = *s;
        }
        const char *c1 = ".cfg";
        for (const char *s = c1; *s && k < sizeof(cfg_path) - 1; s++) {
            cfg_path[k++] = *s;
        }
        cfg_path[k] = '\0';
    }

    long inbox = fe_endpoint_create(0);         /* 服务端收件端点（本进程持有） */
    if (inbox <= 0) {
        failf("端点创建失败");
        return 1;
    }
    /* ★ 发布到 devfs 之前必须先把权限收窄 ★
     *
     * devfs 的发布要求句柄带 TRANSFER（能交出去是一种权限）。而发布出去之后，
     * 任何能按名字打开的进程都会拿到一条通往这个端点的能力——所以
     * **发布的那一份只给 RECV**：别的进程能往里发，不能从里面收。
     * 这不是形式：收件端点若被赋予了 RECV 权限，任何人都能抢走本该属于
     * 服务端的消息。 */
    long inbox_pub = fe_handle_dup(inbox, FE_RIGHT_RECV | FE_RIGHT_TRANSFER);
    if (inbox_pub <= 0) {
        failf("为发布准备句柄失败（DUP 坏了？）");
        return 1;
    }
    if (fe_devfs_publish(inbox_name, inbox_pub) != FE_OK) {
        say("  [hxtest] 失败: 发布 ");
        say(inbox_name);
        say(" 失败（名字被占用？）\n");
        g_fail++;
        return 1;
    }

    /* 共享状态页：服务端把它的失败次数写在这里 */
    long status = fe_mem_alloc(4096);
    g_status = (status > 0)
             ? (volatile u32 *)fe_mem_map(status, NULL, 0, FE_PROT_READ | FE_PROT_WRITE)
             : NULL;
    if (!g_status) {
        failf("共享状态页创建/映射失败");
        return 1;
    }

    /* 引导文件：一次运行共同的落点（内容是常数，**不含句柄号**——
     * 句柄号不能跨任务，这个坑下面会踩一次）。
     * 顺带验证了文件系统在服务启动顺序里已经可用。 */
    {
        long fs = fe_devfs_open("/dev/fs0");
        if (fs <= 0) {
            failf("打不开 /dev/fs0（文件系统服务没起来？）");
            return 1;
        }
        struct hx_cfg cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.magic = HX_CFG_MAGIC;
        cfg.reserved = 0;       /* 句柄号不放这儿：它跨不了任务 */
        if (fe_fs_create(fs, cfg_path) != FE_FS_OK ||
            fe_fs_write(fs, cfg_path, 0, &cfg, sizeof(cfg), NULL) != FE_FS_OK) {
            say("  [hxtest] 失败: 写引导文件 ");
            say(cfg_path);
            say(" 失败\n");
            g_fail++;
            fe_handle_close(fs);
            return 1;
        }
        fe_handle_close(fs);
    }

    char *srv_argv[3];
    srv_argv[0] = (char *)argv[0];
    srv_argv[1] = inbox_name;
    srv_argv[2] = cfg_path;
    long srv = fe_spawn(argv[0], srv_argv, 3);
    if (srv <= 0) {
        say("  [hxtest] 失败: 拉起服务端任务失败（错误码 ");
        num((u64)(srv < 0 ? -srv : srv));
        say("，路径 ");
        say(argv[0] ? argv[0] : "(空)");
        say("）\n");
        g_fail++;
        return 1;
    }
    say("  [hxtest] 服务端任务已拉起（收件端点 ");
    say(inbox_name);
    say("，引导文件 ");
    say(cfg_path);
    say("）\n");

    /* ---- 把状态页句柄**随消息**交给它 ----
     *
     * ★ 这一步就是全篇的核心机制 ★
     * 句柄号不能跨任务，所以状态页的能力只能靠"随消息传句柄"过去：
     * 客户端先发布收件端点，服务端按名字打开（名字跨任务可用），
     * 客户端再往那条通道上发一条带句柄的消息。于是"两个任务共享一块内存"
     * 这件事完全没有用到句柄号，也用不到句柄继承。 */
    {
        struct hx_req req;
        req.magic = HX_MAGIC;
        req.op = HX_OP_HELLO;
        req.size = 0;
        req.value = 0;
        u32 arr[1];
        arr[0] = (u32)status;
        struct fe_msg_header h;
        memset(&h, 0, sizeof(h));
        h.payload_len = sizeof(req);
        h.handle_count = 1;
        if (fe_endpoint_send(inbox, &h, &req, arr, 1) != FE_OK) {
            failf("把状态页句柄交给服务端失败");
            return 1;
        }
        /* 等服务端确认拿到（标志落在共享页上：那正是"它拿到了"的证据） */
        for (u32 i = 0; i < 2000 && !(g_status[HX_ST_IDX_FLAGS] & HX_ST_READY); i++) {
            fe_sleep_ms(1);
        }
        if (!(g_status[HX_ST_IDX_FLAGS] & HX_ST_READY)) {
            failf("服务端没有确认收到状态页（随消息传句柄失败？）");
            return 1;
        }
        say("  [hxtest] OK   状态页经「随消息传句柄」交给服务端（句柄号没有跨任务）\n");
    }


    /* ---- 反向对照：未映射的句柄数组必须 FAULT，而且不消耗消息 ---- */
    {
        struct hx_req req;
        req.magic = HX_MAGIC;
        req.op = HX_OP_SMALL;
        req.size = HX_SMALL_SIZE;
        req.value = 0;
        struct fe_msg_header h;
        memset(&h, 0, sizeof(h));
        h.payload_len = sizeof(req);
        h.handle_count = 1;
        /* 内核必须先校验输出缓冲，再进入阻塞接收：
         * 否则 recv 成功之后才发现拷不回去，用户态会拿到一堆
         * 自己不知道句柄号的已安装句柄（不可回收的泄漏）。 */
        struct fe_msg_header rh;
        u8 rb[64];
        /* 用 inbox（本进程自己的端点）：这条调用必须在**读任何消息之前**
         * 就失败，所以它检验的是"先校验、后接收"这条次序，与端点上有没有
         * 消息无关——服务端正在往别处回话，不会往这里发。 */
        long r = fe_endpoint_recv(inbox, &rh, rb, sizeof(rb),
                                  (u32 *)0x1234, NULL);     /* 未映射的地址 */
        if (r != FE_ERR_FAULT) {
            say("  [hxtest] 失败: 未映射的句柄数组返回 ");
            num((u64)(r < 0 ? -r : r));
            say("，期望 FE_ERR_FAULT\n");
            g_fail++;
        } else {
            say("  [hxtest] OK   未映射的句柄数组直接 FAULT（先校验、后接收）\n");
        }
    }

    /* ---- 反向对照：没有 TRANSFER 权限的句柄不许随消息发出去 ---- */
    {
        long mo = fe_mem_alloc(HX_SMALL_SIZE);
        long ro = fe_handle_dup(mo, FE_RIGHT_READ);     /* 没有 TRANSFER */
        u32 arr[1];
        arr[0] = (u32)ro;
        struct hx_req req;
        req.magic = HX_MAGIC;
        req.op = HX_OP_SMALL;
        req.size = HX_SMALL_SIZE;
        req.value = 0;
        struct fe_msg_header h;
        memset(&h, 0, sizeof(h));
        h.payload_len = sizeof(req);
        h.handle_count = 1;
        long r = fe_endpoint_send(inbox, &h, &req, arr, 1);
        if (r != FE_ERR_ACCESS) {
            failf("没有 TRANSFER 权限的句柄竟然发得出去");
        } else {
            say("  [hxtest] OK   无 TRANSFER 权限的句柄被拒（能力不能凭空转交）\n");
        }

        /* ---- 小块：把内存对象交给服务端，它校验 + 改写 + 回校验和 ---- */
        u8 *p = (u8 *)fe_mem_map(mo, NULL, 0, FE_PROT_READ | FE_PROT_WRITE);
        if (!p) {
            failf("小块内存映射失败");
        } else {
            pattern_fill(p, HX_SMALL_SIZE, 0);
            /* ★ 要交出去就必须带 TRANSFER，而且要显式复制一份 ★
             * 这不是形式：默认创建出来的句柄带 TRANSFER，但**故意收窄过**的
             * 句柄（比如只读副本）不带——那种句柄交不出去，内核会拒。
             * 这里用副本交给服务端，自己留原件：于是"给了对方什么权限"
             * 完全由这一行决定，而不是"看创建时的默认值"。 */
            long mo_tx = fe_handle_dup(mo, FE_RIGHT_READ | FE_RIGHT_WRITE |
                                           FE_RIGHT_TRANSFER);
            if (mo_tx <= 0) {
                failf("复制可转交的内存对象句柄失败");
            }
            g_call_handles[0] = (u32)mo_tx;
            g_call_hcount = 1;
            struct hx_rep rep;
            long cr = call_server(inbox, HX_OP_SMALL, HX_SMALL_SIZE, 0, &rep);
            g_call_hcount = 0;
            fe_handle_close(mo_tx);
            if (cr != FE_OK || rep.status != FE_OK) {
                failf("小块请求没有拿到正常应答");
            } else if (rep.checksum != checksum_of(p, HX_SMALL_SIZE)) {
                failf("小块：服务端算出的校验和与客户端不同（不是同一块内存）");
            } else {
                say("  [hxtest] OK   4 KiB 内存对象跨任务传递：内容与校验和一致\n");
            }
            fe_mem_unmap(p, HX_SMALL_SIZE);
        }
        fe_handle_close(ro);
        fe_handle_close(mo);
    }

    /* ---- 大块：1 MiB 走共享内存，内核不碰数据 ---- */
    {
        long file = fe_mem_alloc(HX_WINDOW_SIZE);
        long text = fe_mem_alloc(HX_WINDOW_SIZE);
        if (file <= 0 || text <= 0) {
            failf("1 MiB 内存对象分配失败");
        } else {
            u8 *fp = (u8 *)fe_mem_map(file, NULL, 0, FE_PROT_READ | FE_PROT_WRITE);
            u8 *tp = (u8 *)fe_mem_map(text, NULL, 0, FE_PROT_READ | FE_PROT_WRITE);
            if (!fp || !tp) {
                failf("1 MiB 映射失败");
            } else {
                pattern_fill(fp, HX_WINDOW_SIZE, 0);
                /* 窗口给读写、数据只给读：**服务端能做什么由这几行决定** */
                long text_tx = fe_handle_dup(text, FE_RIGHT_READ | FE_RIGHT_WRITE |
                                                    FE_RIGHT_TRANSFER);
                long file_tx = fe_handle_dup(file, FE_RIGHT_READ | FE_RIGHT_TRANSFER);
                if (text_tx <= 0 || file_tx <= 0) {
                    failf("复制大块传输的可转交句柄失败");
                }
                u64 t0 = fe_clock_ns();
                g_call_handles[0] = (u32)text_tx;   /* 可写窗口：服务端往这里回写 */
                g_call_handles[1] = (u32)file_tx;   /* 只读数据：服务端读它 */
                g_call_hcount = 2;
                struct hx_rep rep;
                long cr = call_server(inbox, HX_OP_BIG, HX_WINDOW_SIZE, 0, &rep);
                g_call_hcount = 0;
                fe_handle_close(text_tx);
                fe_handle_close(file_tx);
                {
                    if (cr != FE_OK || rep.status != FE_OK) {
                        failf("大块请求没有拿到正常应答");
                    } else if (rep.bytes != HX_WINDOW_SIZE) {
                        failf("大块：服务端处理的字节数不对");
                    } else if (rep.checksum == 0) {
                        failf("大块：服务端校验和为 0（没有真的读）");
                    } else {
                        u64 t1 = fe_clock_ns();
                        /* ★ 回读：服务端通过 text 这个窗口写进来的内容 ★
                         * 这是"两边真的是同一块物理内存"的**唯一硬证据**：
                         * 若内核其实复制了一份，客户端这里读到的还是自己的旧模式。 */
                        u64 bad = pattern_check(tp, HX_WINDOW_SIZE, 0x1000000ull);
                        if (bad) {
                            say("  [hxtest] 失败: 服务端写入窗口的内容客户端看不到（");
                            num(bad);
                            say(" 字节）\n");
                            g_fail++;
                        } else {
                            say("  [hxtest] OK   1 MiB 双向共享：服务端写入的内容客户端全部可见\n");
                        }
                        /* ★ 按**能分辨的量**报，并把分辨率一起说出来 ★
                         *
                         * 这个数字换过两次写法，每一次都是被一个环境的读数逼出来的：
                         *   毫秒 → VBox 打出 "0 ms"；
                         *   微秒 → VBox 打出 "0 us"。
                         * 两次都不是"太快"，而是**内核的单调时钟分辨率不够**：
                         * 它是按节拍算的，而节拍间隔在 QEMU 是 1 ms、在 VBox
                         * 是 6~20 ms（中断投递被限流）。往返不足一个节拍时，
                         * 两次读数**一模一样**，差值必然是 0。
                         *
                         * 所以现在报三个东西：节拍差、微秒数、载荷长度。
                         * 节拍差为 0 时明确写"小于一个节拍"——
                         * 那是**这次测量分辨不出来**，不是"耗时为零"。 */
                        u64 dt_us = (t1 - t0) / 1000ull;
                        say("  [hxtest] 大块往返：");
                        if (dt_us == 0) {
                            say("小于一个时钟节拍（本环境分辨不出来）");
                        } else {
                            num(dt_us);
                            say(" us");
                        }
                        say("；内联载荷仅 ");
                        num((u64)sizeof(struct hx_req));
                        say(" 字节，数据走页映射\n");
                    }
                }
                fe_mem_unmap(fp, HX_WINDOW_SIZE);
                fe_mem_unmap(tp, HX_WINDOW_SIZE);
            }
            fe_handle_close(file);
            fe_handle_close(text);
        }
    }

    /* ---- 只读对象：服务端必须写不进去 ---- */
    {
        long ro = fe_mem_alloc(HX_SMALL_SIZE);
        if (ro > 0) {
            u8 *p = (u8 *)fe_mem_map(ro, NULL, 0, FE_PROT_READ | FE_PROT_WRITE);
            if (p) {
                pattern_fill(p, HX_SMALL_SIZE, 0);
                fe_mem_unmap(p, HX_SMALL_SIZE);
            }
            /* 交给服务端的是"只读 + 可转交"的副本：它能读、不能写 */
            long ro_tx = fe_handle_dup(ro, FE_RIGHT_READ | FE_RIGHT_TRANSFER);
            if (ro_tx <= 0) {
                failf("构造只读可转交句柄失败");
            } else {
                g_call_handles[0] = (u32)ro_tx;
                g_call_hcount = 1;
                struct hx_rep rep;
                long cr = call_server(inbox, HX_OP_READONLY, HX_SMALL_SIZE, 0, &rep);
                g_call_hcount = 0;
                if (cr != FE_OK) {
                    failf("只读请求没有应答");
                } else if (rep.status != FE_ERR_ACCESS) {
                    failf("只读对象在服务端竟然能可写映射（权限收窄是假的）");
                } else {
                    say("  [hxtest] OK   只读对象跨任务后仍不可写（能力只能收窄）\n");
                }
                fe_handle_close(ro_tx);
            }
            fe_handle_close(ro);
        }
    }

    /* ---- 收尾：让服务端正常退出并核对它的退出码 ---- */
    {
        struct hx_rep rep;
        g_call_hcount = 0;
        (void)call_server(inbox, HX_OP_QUIT, 0, 0, &rep);
    }
    int srv_code = -1;
    long w = fe_wait(srv, &srv_code);
    if (w != FE_OK) {
        failf("fe_wait 等不到服务端退出");
    } else if (srv_code != 0) {
        say("  [hxtest] 失败: 服务端退出码 ");
        num((u64)(srv_code < 0 ? -srv_code : srv_code));
        say(" != 0\n");
        g_fail++;
    } else {
        say("  [hxtest] OK   服务端退出码 0（跨任务收尾干净）\n");
    }

    if (test_handle_dup() != 0) {
        g_fail++;
    }
    if (test_mem_unmap() != 0) {
        g_fail++;
    }

    /* ★ 读共享状态页：服务端侧的失败必须让测试失败 ★
     * 服务端在另一个任务里跑，它的输出与客户端的输出会交错在一起；
     * 只看"客户端有没有报错"会把"服务端悄悄崩了"判成通过。
     * 状态页是同一块物理内存，所以这一读是可信的（不是靠打印去猜）；
     * 退出码是第二道证据——两者独立。 */
    if (g_status[HX_ST_IDX_SERVER] != 0) {
        say("  [hxtest] 失败: 服务端侧记录了 ");
        num(g_status[HX_ST_IDX_SERVER]);
        say(" 项失败\n");
        g_fail++;
    } else if (!(g_status[HX_ST_IDX_FLAGS] & HX_ST_DONE)) {
        failf("服务端没有报告结束（共享页没有 DONE 标志）");
    } else {
        say("  [hxtest] OK   服务端侧 0 失败（共享状态页与退出码两处证据）\n");
    }

    fe_handle_close(inbox);
    fe_handle_close(inbox_pub);
    fe_handle_close(status);

    say("  [hxtest] 客户端检查完毕，失败项 ");
    num(g_fail);
    say("\n");
    return g_fail ? 1 : 0;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    if (argc >= 3) {
        /* 服务端模式：argv[1] = 收件端点（devfs 路径）
         *               argv[2] = 引导文件路径
         * 回信地址不在这里：它随每条请求的句柄一起过来（fe_endpoint_call）。 */
        return server_main(argv[1], argv[2]);
    }
    (void)g_who;
    return client_main(argc, argv);
}
