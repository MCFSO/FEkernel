/* SPDX-License-Identifier: 0BSD */
/* FEKernel 基础类型定义。
 * 不使用任何标准库头文件之外的依赖；stdint/stddef/stdbool 由 clang 的 freestanding 头提供。
 */
#ifndef FE_TYPES_H
#define FE_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

typedef uintptr_t uptr;
typedef intptr_t  iptr;

/* 物理地址与虚拟地址在 x86_64 上都是 64 位宽，但语义完全不同，故分开命名，避免混用。 */
typedef u64 phys_addr_t;
typedef u64 virt_addr_t;

#define FE_U8_MAX  0xFFu
#define FE_U16_MAX 0xFFFFu
#define FE_U32_MAX 0xFFFFFFFFu
#define FE_U64_MAX 0xFFFFFFFFFFFFFFFFull

#define FE_KIB(n) ((u64)(n) * 1024ull)
#define FE_MIB(n) ((u64)(n) * 1024ull * 1024ull)
#define FE_GIB(n) ((u64)(n) * 1024ull * 1024ull * 1024ull)

#define FE_PAGE_SIZE     4096ull
#define FE_PAGE_SHIFT    12
#define FE_PAGE_MASK     (FE_PAGE_SIZE - 1)
#define FE_PAGE_ALIGN_UP(x)   (((x) + FE_PAGE_MASK) & ~FE_PAGE_MASK)
#define FE_PAGE_ALIGN_DOWN(x) ((x) & ~FE_PAGE_MASK)

/* 内核所在区域的起点（顶层 2GiB），见 linker.ld 与 -mcmodel=kernel */
#define FE_KERNEL_BASE 0xffffffff80000000ull

static inline u64 fe_align_up(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }
static inline u64 fe_align_down(u64 v, u64 a) { return v & ~(a - 1); }
static inline bool fe_is_aligned(u64 v, u64 a) { return (v & (a - 1)) == 0; }

#endif /* FE_TYPES_H */
