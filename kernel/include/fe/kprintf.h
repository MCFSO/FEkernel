/* SPDX-License-Identifier: 0BSD */
/* 内核格式化输出：不依赖任何标准库实现。 */
#ifndef FE_KPRINTF_H
#define FE_KPRINTF_H

#include <fe/types.h>
#include <fe/compiler.h>
#include <stdarg.h>

/* 控制台字符输出。M0 阶段指向串口；后续 consoled 服务接管图形控制台后，
 * 内核自己的日志仍然只走串口，避免内核依赖用户态服务。 */
void fe_console_putc(char c);
void fe_console_write(const char *s, size_t n);
void fe_console_puts(const char *s);

void fe_kprintf(const char *fmt, ...) FE_PRINTF(1, 2);
void fe_kvprintf(const char *fmt, va_list ap);
int  fe_snprintf(char *buf, size_t size, const char *fmt, ...) FE_PRINTF(3, 4);
int  fe_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/* 支持的转换：%d %i %u %x %X %o %b(二进制) %c %s %p %%
 * 标志：- 左对齐, 0 补零, + 强制符号, 空格, # 前缀
 * 宽度：数字或 *
 * 精度：.数字 或 .*
 * 长度：hh h l ll z
 */
#endif /* FE_KPRINTF_H */
