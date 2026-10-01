/* SPDX-License-Identifier: 0BSD */
/* FPU / SIMD 支持（x86_64）。
 *
 * ★ 为什么必须有这一层 ★
 * 内核是 `-mgeneral-regs-only` 编译的——它**从不碰** XMM/YMM/ZMM。
 * 这个选择换来一个极有价值的性质：**中断与系统调用不需要保存任何向量寄存器**，
 * 于是中断延迟不被 FPU 状态拖累。（Intel 的手册也把"内核不用 SIMD"
 * 列为降低中断延迟的常规手段。）
 *
 * 代价是：用户态默认也拿不到 SIMD（编译选项 + CR4 没开）。
 * 而"用不了 SIMD"不是"慢一点"，是**一整类工作做不了**：
 * 向量化的 memcpy/校验、图像音频、数值计算，以及**任何真正的密码学库**
 * （OpenSSL/BoringSSL 要求 SSE2/AVX）。
 *
 * 所以本层只做一件事：**把用户态该有的 SIMD 打开，同时不让内核沾上它。**
 *
 * 具体是四件事：
 *   1. CR0：清 EM、置 MP、清 TS（不做惰性切换，见下）；
 *   2. CR4：置 OSFXSR / OSXMMEXCPT（SSE），有能力时再置 OSXSAVE（XSAVE）；
 *   3. XCR0：只开 x87 | SSE | AVX —— **AVX-512 故意不开**（没有目标硬件，
 *      而它会把每线程状态从 832 字节抬到 2688 字节，收益为负）；
 *   4. 线程切换时 XSAVE/XRSTOR（有能力时用 XSAVEOPT，它能跳过未修改的部件）。
 *
 * ★ 为什么不做惰性切换（CR0.TS + #NM）★
 * 惰性切换是"切出去时置 TS，等它真用 FPU 时再陷入内核保存"。
 * 好处是"不用 FPU 的线程完全零成本"，坏处是**第一次用到 FPU 时要吃一次异常**，
 * 而异常路径比 XSAVE 贵得多。性能优先的口径下选**急切保存**：
 * 谁都没用 FPU 时，XSAVEOPT 只写一个头部（几十周期），代价可预测、
 * 没有异常带来的抖动。 */
#ifndef FE_FPU_H
#define FE_FPU_H

#include <fe/types.h>

/* 每线程的 FPU 状态区大小。
 *
 * 实际需要：FXSAVE 512；XSAVE(x87+SSE) 576；XSAVE(+AVX) 832。
 * 取整到 **1024**，因为内核堆的尺寸等级是 16/32/…/2048，
 * 1024 这个等级的对象**天然 64 字节对齐**（对象基址 = 页 + 64，步长 1024），
 * 而 XSAVE 要求 64 字节对齐。这不是巧合，是刻意挑的：见 fpu_area_alloc()。 */
#define FE_FPU_AREA_SIZE   1024
#define FE_FPU_AREA_ALIGN  64

enum fe_fpu_mode {
    FE_FPU_NONE = 0,        /* 连 FXSAVE 都不可用（不该发生） */
    FE_FPU_FXSAVE,          /* 只有 SSE（FXSAVE/FXRSTOR，512 字节格式） */
    FE_FPU_XSAVE,           /* XSAVE/XRSTOR */
    FE_FPU_XSAVEOPT,        /* XSAVEOPT：跳过未修改的部件 */
};

/* 探测能力并设置本 CPU 的 CR0/CR4/XCR0。
 *
 * ★ 这些寄存器都是**每 CPU 的** ★：CR0/CR4/XCR0 属于 CPU 而不属于线程，
 * 所以将来多核启动 AP 时，每个 AP 都要各自调用一次本函数。
 * 函数是幂等的，重复调用没有副作用。 */
void fe_fpu_init(void);

bool fe_fpu_enabled(void);
enum fe_fpu_mode fe_fpu_get_mode(void);
const char *fe_fpu_mode_name(enum fe_fpu_mode m);
u64  fe_fpu_area_size(void);
u64  fe_fpu_saves(void);        /* 统计：发生过多少次保存（诊断与性能核对） */

/* 分配一块 64 字节对齐的 FPU 状态区（来自内核堆），并把它初始化成"干净状态"。
 * 失败返回 NULL——调用者必须容忍 NULL（那样该线程就没有 FPU 上下文，
 * 切换时会被恢复成干净状态，**不会看到别人的数据**）。 */
void *fe_fpu_area_alloc(void);
void  fe_fpu_area_free(void *area);

/* 保存到（非 NULL 的）区；从区恢复。
 * ★ 传 NULL 表示"恢复成干净状态" ★ —— 这不是"什么都不做"：
 * 如果什么都不做，新线程会看到上一个线程留在寄存器里的数据（信息泄漏）。
 * 切换路径上新线程没有状态区时就走这条，保证隔离性不依赖"分配一定成功"。 */
void fe_fpu_save(void *area);
void fe_fpu_restore(const void *area);

/* 自检：能力位与 CR4 是否一致、状态区是否真的按 64 字节对齐、
 * 保存→恢复是否真的往返一致。返回失败项数。 */
u32 fe_selftest_fpu(void);

#endif /* FE_FPU_H */
