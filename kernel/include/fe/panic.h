/* SPDX-License-Identifier: 0BSD */
/* 内核致命错误处理：断言、panic、停机。 */
#ifndef FE_PANIC_H
#define FE_PANIC_H

#include <fe/types.h>
#include <fe/compiler.h>
#include <fe/regs.h>

/* 打印原因并永久停机（关闭中断后 hlt 循环） */
FE_NORETURN void fe_panic_halt(const char *reason);
FE_NORETURN void fe_panic(const char *fmt, ...) FE_PRINTF(1, 2);

/* 断言失败：打印表达式、文件、行号、调用点，然后停机 */
FE_NORETURN void fe_assert_fail(const char *expr, const char *file, int line,
                                const char *func);

#define FE_ASSERT(expr)                                                        \
    do {                                                                       \
        if (FE_UNLIKELY(!(expr))) {                                            \
            fe_assert_fail(#expr, __FILE__, __LINE__, __func__);               \
        }                                                                      \
    } while (0)

/* 内核内部一致性检查：与断言相同，但语义上表示「不可能发生」 */
#define FE_BUG_ON(expr) FE_ASSERT(!(expr))

/* 打印寄存器现场（异常处理与调试用） */
void fe_dump_regs(const struct fe_regs *r);

#endif /* FE_PANIC_H */
