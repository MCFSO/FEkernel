/* SPDX-License-Identifier: 0BSD */
/* /dev/console 的请求协议（接口层）。
 *
 * 与 fe_blk.h 同一个理由：设备语义放接口层，不放 libfe（libfe 是运行时，
 * 不含设备语义），也不让每个客户端各抄一份（抄一份的代价是三份会各自漂移）。
 *
 * 这里只有"写"和"读一行"两件事——**控制台不是终端模拟器**：
 * 行编辑（退格、回显、光标）由 consoled 做（它才拥有屏幕），
 * 客户端拿到的永远是完整的一行。这个分工让 shell 不必碰按键，
 * 也不会与控制台的渲染打架。 */
#ifndef FE_CONSOLE_H
#define FE_CONSOLE_H

#include <fe_user.h>

#define FE_CON_OP_INFO  1
#define FE_CON_OP_WRITE 2
#define FE_CON_OP_READ  3      /* 阻塞直到有一行（或以 '\0' 表示 EOF/无输入） */

struct fe_con_req {
    u32 op;
    u32 len;        /* WRITE：后面跟的字节数 */
};

struct fe_con_info {
    u32 width;
    u32 height;
    u32 cols;
    u32 rows;
    u32 bpp;
    u32 scale;
};

/* 写字符串（不含结尾 '\0'）。返回 0 成功。 */
static inline int fe_con_write(long ep, const char *s)
{
    static char buf[sizeof(struct fe_con_req) + 512];
    struct fe_con_req req;
    u32 n = 0;
    u32 status = 0;
    u32 got = 0;
    long r;

    while (s[n] != '\0' && n < 512) {
        n++;
    }
    req.op = FE_CON_OP_WRITE;
    req.len = n;
    memcpy(buf, &req, sizeof(req));
    memcpy(buf + sizeof(req), s, n);
    r = fe_endpoint_call(ep, buf, (u32)(sizeof(req) + n),
                         &status, sizeof(status), &got);
    return (r == FE_OK && got == sizeof(status) && status == 1) ? 0 : -1;
}

/* 取控制台几何。返回 0 成功。 */
static inline int fe_con_info(long ep, struct fe_con_info *out)
{
    struct fe_con_req req;
    u32 got = 0;
    long r;
    req.op = FE_CON_OP_INFO;
    req.len = 0;
    r = fe_endpoint_call(ep, &req, (u32)sizeof(req), out, (u32)sizeof(*out), &got);
    return (r == FE_OK && got == sizeof(*out)) ? 0 : -1;
}

/* 读一行（阻塞）。返回读到的字节数（不含结尾 '\0'），失败 -1。 */
static inline int fe_con_readline(long ep, char *out, u32 cap)
{
    struct fe_con_req req;
    u32 got = 0;
    long r;
    if (cap == 0) {
        return -1;
    }
    req.op = FE_CON_OP_READ;
    req.len = cap;
    r = fe_endpoint_call(ep, &req, (u32)sizeof(req), out, cap, &got);
    if (r != FE_OK || got == 0) {
        return -1;
    }
    if (got >= cap) {
        got = cap - 1;
    }
    out[got] = '\0';
    return (int)got;
}

#endif /* FE_CONSOLE_H */
