/* SPDX-License-Identifier: 0BSD */
/* 编译器相关宏：把 GCC/Clang 扩展收拢到一处，内核其它代码不直接写 __attribute__。 */
#ifndef FE_COMPILER_H
#define FE_COMPILER_H

#define FE_NORETURN      __attribute__((noreturn))
#define FE_PACKED        __attribute__((packed))
#define FE_ALIGNED(n)    __attribute__((aligned(n)))
#define FE_UNUSED        __attribute__((unused))
#define FE_MAYBE_UNUSED  __attribute__((unused))
#define FE_USED          __attribute__((used))
#define FE_SECTION(s)    __attribute__((section(s)))
#define FE_WEAK          __attribute__((weak))
#define FE_NODISCARD     __attribute__((warn_unused_result))
#define FE_PRINTF(f, a)  __attribute__((format(printf, f, a)))
#define FE_NONNULL(...)  __attribute__((nonnull(__VA_ARGS__)))

#define FE_INLINE static inline __attribute__((always_inline))

#define FE_LIKELY(x)   __builtin_expect(!!(x), 1)
#define FE_UNLIKELY(x) __builtin_expect(!!(x), 0)

#define FE_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define FE_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

#define FE_STRINGIFY_(x) #x
#define FE_STRINGIFY(x)  FE_STRINGIFY_(x)
#define FE_CONCAT_(a, b) a##b
#define FE_CONCAT(a, b)  FE_CONCAT_(a, b)

/* 编译器屏障：告诉编译器内存可能被外部（中断/DMA/其它 CPU）改变 */
#define FE_BARRIER() __asm__ volatile("" ::: "memory")

#define FE_UNREACHABLE() __builtin_unreachable()

/* 对齐到缓存行，避免伪共享 */
#define FE_CACHELINE 64
#define FE_CACHE_ALIGNED FE_ALIGNED(FE_CACHELINE)

#endif /* FE_COMPILER_H */
