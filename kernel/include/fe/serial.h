/* SPDX-License-Identifier: 0BSD */
/* 16550 串口驱动（内核早期调试输出用）。
 *
 * 注意：这是**内核内部**用于引导期日志的最小驱动；完整串口服务（seriald）
 * 将作为用户态驱动服务实现，供整个系统使用。两者互不冲突：内核始终占用
 * COM1 做崩溃诊断，seriald 面向应用提供日志汇聚。
 */
#ifndef FE_SERIAL_H
#define FE_SERIAL_H

#include <fe/types.h>

#define FE_COM1 0x3F8
#define FE_COM2 0x2F8
#define FE_COM3 0x3E8
#define FE_COM4 0x2E8

void fe_serial_init(u16 port);
bool fe_serial_present(u16 port);
void fe_serial_putc(u16 port, char c);
void fe_serial_write(u16 port, const char *s, size_t n);
void fe_serial_puts(u16 port, const char *s);

/* 当前内核控制台使用的串口 */
u16  fe_serial_console_port(void);
void fe_serial_set_console_port(u16 port);

#endif /* FE_SERIAL_H */
