/* SPDX-License-Identifier: 0BSD */
/* FEKernel 统一返回状态码。
 *
 * 约定（对内核与用户态一致）：
 *   0            成功
 *   负数         失败，取值为下面的 FE_ERR_*
 * 系统调用返回时，rax 直接承载该值。
 */
#ifndef FE_STATUS_H
#define FE_STATUS_H

#include <fe/types.h>
#include <fe/errno.h>       /* 错误码只有一份定义（内核与用户态共享） */

typedef i32 fe_status_t;

static inline bool fe_failed(fe_status_t s) { return s < 0; }
static inline bool fe_ok(fe_status_t s) { return s == FE_OK; }

/* 内核内部断言/错误上报用：把状态码转成可读名字 */
const char *fe_status_name(fe_status_t s);

#endif /* FE_STATUS_H */
