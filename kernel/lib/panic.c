/* SPDX-License-Identifier: 0BSD */
#include <fe/panic.h>
#include <fe/kprintf.h>
#include <fe/io.h>
#include <fe/status.h>
#include <stdarg.h>

void fe_dump_regs(const struct fe_regs *r)
{
    fe_kprintf("  RAX=%016llx RBX=%016llx RCX=%016llx RDX=%016llx\n",
               (unsigned long long)r->rax, (unsigned long long)r->rbx,
               (unsigned long long)r->rcx, (unsigned long long)r->rdx);
    fe_kprintf("  RSI=%016llx RDI=%016llx RBP=%016llx RSP=%016llx\n",
               (unsigned long long)r->rsi, (unsigned long long)r->rdi,
               (unsigned long long)r->rbp, (unsigned long long)r->rsp);
    fe_kprintf("  R8 =%016llx R9 =%016llx R10=%016llx R11=%016llx\n",
               (unsigned long long)r->r8, (unsigned long long)r->r9,
               (unsigned long long)r->r10, (unsigned long long)r->r11);
    fe_kprintf("  R12=%016llx R13=%016llx R14=%016llx R15=%016llx\n",
               (unsigned long long)r->r12, (unsigned long long)r->r13,
               (unsigned long long)r->r14, (unsigned long long)r->r15);
    fe_kprintf("  RIP=%016llx CS =%04llx RFLAGS=%016llx SS=%04llx\n",
               (unsigned long long)r->rip, (unsigned long long)(r->cs & 0xFFFF),
               (unsigned long long)r->rflags, (unsigned long long)(r->ss & 0xFFFF));
}

FE_NORETURN void fe_panic_halt(const char *reason)
{
    fe_cli();
    fe_kprintf("\n*** FEKernel 已停机 (panic) ***\n");
    if (reason) {
        fe_kprintf("原因: %s\n", reason);
    }
    fe_kprintf("系统已停止，需重启。\n");

    /* 停机前用 NMI 之外的方式让 CPU 彻底静止：cli + hlt 循环。
     * NMI 仍可能唤醒 CPU，因此循环里反复 cli。 */
    for (;;) {
        fe_cli();
        fe_hlt();
    }
}

FE_NORETURN void fe_panic(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    fe_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fe_panic_halt(buf);
}

FE_NORETURN void fe_assert_fail(const char *expr, const char *file, int line,
                                const char *func)
{
    fe_cli();
    fe_kprintf("\n*** 断言失败 ***\n");
    fe_kprintf("  表达式: %s\n", expr);
    fe_kprintf("  位置  : %s:%d\n", file, line);
    fe_kprintf("  函数  : %s\n", func);
    fe_panic_halt("断言失败");
}

const char *fe_status_name(fe_status_t s)
{
    switch (s) {
    case FE_OK:              return "成功";
    case FE_ERR_INVAL:       return "参数非法";
    case FE_ERR_NOMEM:       return "内存不足";
    case FE_ERR_NOENT:       return "不存在";
    case FE_ERR_EXIST:       return "已存在";
    case FE_ERR_AGAIN:       return "资源暂不可用";
    case FE_ERR_BUSY:        return "忙";
    case FE_ERR_FAULT:       return "内存访问越界";
    case FE_ERR_ACCESS:      return "权限不足";
    case FE_ERR_BADHANDLE:   return "句柄无效";
    case FE_ERR_NOTSUP:      return "未实现";
    case FE_ERR_IO:          return "设备 I/O 错误";
    case FE_ERR_TIMEOUT:     return "超时";
    case FE_ERR_RANGE:       return "越界";
    case FE_ERR_NOSPC:       return "空间不足";
    case FE_ERR_NAMETOOLONG: return "名称过长";
    case FE_ERR_NOTDIR:      return "不是目录";
    case FE_ERR_ISDIR:       return "是目录";
    case FE_ERR_PIPE:        return "对端已关闭";
    case FE_ERR_PROTO:       return "协议错误";
    case FE_ERR_OVERFLOW:    return "溢出";
    case FE_ERR_CANCELED:    return "已取消";
    case FE_ERR_KILLED:      return "被终止";
    case FE_ERR_DEADLOCK:    return "死锁";
    case FE_ERR_INTERNAL:    return "内核内部错误";
    default:                 return "未知状态";
    }
}
