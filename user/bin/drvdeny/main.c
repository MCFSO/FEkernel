/* SPDX-License-Identifier: 0BSD */
/* 端口越权测试程序（ring 3）。
 *
 * 它**故意**不认领端口就去执行 in 指令。
 *
 * 这条测试是 M5 的承重证据：如果 I/O 权限位图真的生效，CPU 会在执行 in 的
 * 那一刻抛出 #GP，内核终止本线程（退出码 -1），下面第二行永远不会被打印。
 * 反过来，如果这个程序能打印出「竟然执行成功了」，那说明端口权限根本没拦住——
 * 而这种缺陷靠读代码是看不出来的，只有让 CPU 自己去拒绝才算数。
 *
 * 内核会核对两件事：退出码是 -1，且最近一次用户异常的向量正好是 13 (#GP)。
 * 只看退出码不够——别的异常碰巧也会给 -1。
 */
#include <fe_user.h>

#define COM2 0x2F8

int main(void)
{
    fe_puts("  [drvdeny] 未认领端口，接下来执行 in —— 预期被 #GP 终止\n");

    unsigned char v = fe_inb(COM2);

    /* 走到这里就是失败 */
    fe_puts("  [drvdeny] ！！！竟然执行成功了，读回 ");
    fe_print_hex(v);
    fe_puts(" —— I/O 权限位图没有生效！\n");
    return 7;
}
