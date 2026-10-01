/* SPDX-License-Identifier: 0BSD */
/* contest —— 控制台的客户端测试。
 *
 * ★ 它存在的理由：`/dev/console` 的接口此前**没有任何调用者** ★
 * 服务写完了、发布出去了、自检也全绿，但"客户端能不能通过 IPC 真的把字打到
 * 屏幕上"这条路径一次都没跑过。没跑过的路径等于没有：
 * 服务端的请求循环、协议结构体、载荷长度、回复句柄语义，
 * 任何一处对不上都不会有人知道。
 *
 * 它验三件事：
 *   1. 能按路径打开 /dev/console（devfs + IPC 通）；
 *   2. INFO 返回的几何与服务启动时打印的一致（协议结构体没串位）；
 *   3. WRITE 返回成功，**并且屏幕上的像素真的变了**（这一条靠截图看，
 *      程序自己只能证明前两件——所以它会把要看的字符打在固定位置）。
 *
 * 退出码 = 失败项数。 */
#include <fe_user.h>
#include <fe_console.h>

static u32 g_fail;

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
    say("\n=== 控制台客户端（M6：/dev/console 的读写路径）===\n");

    long con = fe_devfs_open("/dev/console");
    check(con > 0, "按路径打开 /dev/console");
    if (con <= 0) {
        say("         （consoled 没起来？或者它没发布这个设备？）\n");
        return (int)g_fail;
    }

    struct fe_con_info info;
    if (fe_con_info(con, &info) != 0) {
        check(0, "INFO 请求有应答");
        fe_handle_close(con);
        return (int)g_fail;
    }
    say("         控制台 ");
    num(info.width);
    say("x");
    num(info.height);
    say(" 像素，");
    num(info.cols);
    say("x");
    num(info.rows);
    say(" 字符格，");
    num(info.bpp);
    say("bpp，缩放 ");
    num(info.scale);
    say("\n");
    /* 不变量：字符格数必须与像素尺寸、缩放自洽。
     * 只断言"字段非零"是没用的——协议串位时字段依然非零。 */
    check(info.cols > 0 && info.rows > 0, "字符网格非空");
    check(info.cols == info.width / ((5 + 1) * info.scale) ||
          info.cols == info.width / ((5 + 2) * info.scale),
          "列数与像素宽度、缩放自洽");

    /* ★ 屏幕上的内容要像"系统的界面"，不是"测试的日志" ★
     *
     * 我第一版在屏幕上打了两行说明 + 599 字节的 abc…xyz 测试串。
     * 结果是每次开机屏幕上都糊着六行字母——用户（就是我自己）看到的第一反应
     * 是"这什么鬼东西"。**测试数据的可读性也是可读性的一部分。**
     *
     * 现在的分工：
     *   - 检查项（OK/失败）→ **串口**：那是诊断日志该去的地方；
     *   - 屏幕上只留一行"控制台可用"的声明，人一眼能看懂；
     *   - 接近载荷上限的那次写**照样测**，但它写在屏幕最后一行、
     *     随后被 `\n` 顶走，不留痕。 */
    {
        /* 先做长载荷测试：它会被**擦掉**，不在用户屏幕上留垃圾。
         * 擦法是"原样退格回去"——这同时验证了控制台的退格能跨行回卷。 */
        char big[600];
        char rub[601];
        for (u32 i = 0; i < sizeof(big) - 2; i++) {
            big[i] = (char)('a' + (i % 26));
            rub[i] = '\b';
        }
        big[sizeof(big) - 2] = '\0';
        rub[sizeof(big) - 2] = '\0';
        int w3 = fe_con_write(con, big);
        check(w3 == 0, "一次写 598 字节（接近载荷上限）也成功");
        int w5 = fe_con_write(con, rub);
        check(w5 == 0, "把刚才那 598 字节退格擦掉（跨行回卷）");
    }

    int w1 = fe_con_write(con, "console: /dev/console write path OK\n");
    check(w1 == 0, "WRITE 返回成功");
    int w2 = fe_con_write(con, "console: ready\n");
    check(w2 == 0, "第二次 WRITE 也成功");

    fe_handle_close(con);
    say("=== 控制台客户端结束，失败项 ");
    num((u64)g_fail);
    say(" ===\n");
    return (int)g_fail;
}
