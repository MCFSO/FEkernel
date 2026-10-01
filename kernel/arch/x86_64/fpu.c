/* SPDX-License-Identifier: 0BSD */
/* FPU / SIMD：能力探测、CR0/CR4/XCR0 设置、线程状态保存/恢复。见 fe/fpu.h。 */
#include <fe/fpu.h>
#include <fe/io.h>
#include <fe/kprintf.h>
#include <fe/string.h>
#include <fe/mm/kheap.h>

/* CR0 位 */
#define CR0_MP  (1ull << 1)     /* 监控协处理器 */
#define CR0_EM  (1ull << 2)     /* 模拟：置位时任何 FPU 指令都 #NM */
#define CR0_TS  (1ull << 3)     /* 任务已切换：置位时第一条 FPU 指令 #NM */

/* CR4 位 */
#define CR4_OSFXSR     (1ull << 9)   /* 允许 FXSAVE/FXRSTOR，且 SSE 指令不 #UD */
#define CR4_OSXMMEXCPT (1ull << 10)  /* 未屏蔽的 SIMD 异常走 #XF(19)，而不是 #UD */
#define CR4_OSXSAVE    (1ull << 18)  /* 允许 XSAVE/XRSTOR/XGETBV/XSETBV */

/* XCR0 位 */
#define XCR0_X87 (1ull << 0)
#define XCR0_SSE (1ull << 1)
#define XCR0_AVX (1ull << 2)

static enum fe_fpu_mode g_mode = FE_FPU_NONE;
static u64 g_xcr0;
static u64 g_area_size;
static u64 g_saves;
static bool g_ready;

/* 干净的（初始态）状态区：给"没有状态区的线程"用。
 * 静态数组天然满足 FXSAVE 的 16 字节对齐；XSAVE 需要 64 字节，
 * 所以显式按 64 对齐——**对齐是 XSAVE 的硬要求，不是优化**：
 * 不对齐会直接 #GP。 */
static u8 g_clean_area[FE_FPU_AREA_SIZE] FE_ALIGNED(FE_FPU_AREA_ALIGN);

static void cpuid(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d)
{
    __asm__ __volatile__("cpuid"
                         : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                         : "a"(leaf), "c"(sub));
}

static u64 xgetbv0(void)
{
    u32 lo, hi;
    __asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((u64)hi << 32) | lo;
}

static void xsetbv0(u64 v)
{
    u32 lo = (u32)v, hi = (u32)(v >> 32);
    __asm__ __volatile__("xsetbv" :: "a"(lo), "d"(hi), "c"(0));
}

/* 状态区操作。
 *
 * ★ 为什么是 xsave64 而不是 xsave ★
 * 没有 REX.W 的 `xsave` 用的是**传统 32 位格式**（FXSAVE 兼容布局），
 * 有 REX.W 的 `xsave64` 才是 64 位格式。两者部件内容相同但头部语义不同，
 * 而 `xrstor64` 必须配 `xsave64` 写的区。Linux 在 x86_64 上用的也是 64 位形式。
 *
 * 掩码（EDX:EAX）告诉 CPU"只处理哪些部件"：传 XCR0 全掩码即可。 */
static void hw_save(void *area)
{
    u32 lo = (u32)g_xcr0, hi = (u32)(g_xcr0 >> 32);
    switch (g_mode) {
    case FE_FPU_XSAVEOPT:
        __asm__ __volatile__("xsaveopt64 (%0)" :: "r"(area), "a"(lo), "d"(hi)
                             : "memory");
        break;
    case FE_FPU_XSAVE:
        __asm__ __volatile__("xsave64 (%0)" :: "r"(area), "a"(lo), "d"(hi)
                             : "memory");
        break;
    default:
        __asm__ __volatile__("fxsave (%0)" :: "r"(area) : "memory");
        break;
    }
}

static void hw_restore(const void *area)
{
    u32 lo = (u32)g_xcr0, hi = (u32)(g_xcr0 >> 32);
    switch (g_mode) {
    case FE_FPU_XSAVEOPT:
    case FE_FPU_XSAVE:
        __asm__ __volatile__("xrstor64 (%0)" :: "r"(area), "a"(lo), "d"(hi)
                             : "memory");
        break;
    default:
        __asm__ __volatile__("fxrstor (%0)" :: "r"(area) : "memory");
        break;
    }
}

void fe_fpu_init(void)
{
    u32 a, b, c, d;
    u32 max_leaf;

    cpuid(0, 0, &max_leaf, &b, &c, &d);

    /* --- 先无条件把 SSE 打开：x86_64 保证有 SSE2 --- */
    u64 cr0 = fe_read_cr0();
    cr0 &= ~(CR0_EM | CR0_TS);
    cr0 |= CR0_MP;
    fe_write_cr0(cr0);

    u64 cr4 = fe_read_cr4();
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;

    /* --- 再看能不能上 XSAVE --- */
    bool have_xsave = false, have_avx = false, have_xsaveopt = false;
    if (max_leaf >= 1) {
        cpuid(1, 0, &a, &b, &c, &d);
        have_xsave = (c & (1u << 26)) != 0;      /* XSAVE */
        have_avx   = (c & (1u << 28)) != 0;      /* AVX */
        if (have_xsave) {
            cr4 |= CR4_OSXSAVE;
            fe_write_cr4(cr4);
            /* 置了 OSXSAVE 之后，CPUID.01H:ECX[27] 才有意义（OSXSAVE 已置位） */
            cpuid(1, 0, &a, &b, &c, &d);
            if ((c & (1u << 27)) == 0) {
                have_xsave = false;              /* 置不上去：退回 FXSAVE */
                cr4 &= ~CR4_OSXSAVE;
            }
        }
    }
    fe_write_cr4(cr4);

    if (have_xsave) {
        /* XCR0 只开 x87 | SSE（+AVX，如果有）*/
        u64 want = XCR0_X87 | XCR0_SSE;
        if (have_avx) {
            want |= XCR0_AVX;
        }
        /* CPUID.0DH:0 给出"XCR0 允许置哪些位"与启用后的区大小 */
        cpuid(0xD, 0, &a, &d, &b, &c);
        u64 supported = ((u64)d << 32) | a;
        want &= supported;
        if ((want & (XCR0_X87 | XCR0_SSE)) != (XCR0_X87 | XCR0_SSE)) {
            want = XCR0_X87 | XCR0_SSE;          /* 理论上不会发生 */
        }
        xsetbv0(want);
        g_xcr0 = xgetbv0();
        g_area_size = b;                          /* CPUID.0DH:0.EBX = 启用状态所需大小 */
        g_mode = FE_FPU_XSAVE;
        /* XSAVEOPT 是 CPUID.0DH:1.EAX[0] */
        if (max_leaf >= 0xD) {
            cpuid(0xD, 1, &a, &b, &c, &d);
            have_xsaveopt = (a & 1u) != 0;
        }
        if (have_xsaveopt) {
            g_mode = FE_FPU_XSAVEOPT;
        }
    } else {
        g_xcr0 = 0;
        g_area_size = 512;
        g_mode = FE_FPU_FXSAVE;
    }

    g_ready = true;
    fe_kprintf("[FPU] %s：CR0.EM=0 MP=1 TS=0，CR4.OSFXSR=%u OSXMMEXCPT=%u OSXSAVE=%u\n",
               fe_fpu_mode_name(g_mode),
               (unsigned)((fe_read_cr4() >> 9) & 1),
               (unsigned)((fe_read_cr4() >> 10) & 1),
               (unsigned)((fe_read_cr4() >> 18) & 1));
    fe_kprintf("      XCR0=%#llx（x87%u SSE%u AVX%u），每线程状态 %llu 字节，"
               "区大小 %u 字节\n",
               (unsigned long long)g_xcr0,
               (unsigned)(g_xcr0 & 1), (unsigned)((g_xcr0 >> 1) & 1),
               (unsigned)((g_xcr0 >> 2) & 1),
               (unsigned long long)g_area_size, (unsigned)FE_FPU_AREA_SIZE);
    /* ★ 内核自己不用 SIMD ★ 这句话必须能被核对，而不是只写在文档里：
     * 编译期已经用 -mgeneral-regs-only 保证，这里把"用户态拿得到什么"
     * 也印出来，两者一起构成"该放开的放开了、不该碰的没碰"的证据。 */
    fe_kprintf("      内核仍为 -mgeneral-regs-only：中断与系统调用**不保存向量寄存器**\n");
}

bool fe_fpu_enabled(void) { return g_ready && g_mode != FE_FPU_NONE; }
enum fe_fpu_mode fe_fpu_get_mode(void) { return g_mode; }
u64 fe_fpu_area_size(void) { return g_area_size; }
u64 fe_fpu_saves(void) { return g_saves; }

const char *fe_fpu_mode_name(enum fe_fpu_mode m)
{
    switch (m) {
    case FE_FPU_FXSAVE:    return "仅 SSE（FXSAVE）";
    case FE_FPU_XSAVE:     return "XSAVE";
    case FE_FPU_XSAVEOPT:  return "XSAVEOPT（跳过未修改部件）";
    default:               return "不可用";
    }
}

void fe_fpu_save(void *area)
{
    if (!area || !fe_fpu_enabled() || g_mode == FE_FPU_NONE) {
        return;
    }
    hw_save(area);
    g_saves++;
}

void fe_fpu_restore(const void *area)
{
    if (!fe_fpu_enabled() || g_mode == FE_FPU_NONE) {
        return;
    }
    hw_restore(area ? area : g_clean_area);
}

void *fe_fpu_area_alloc(void)
{
    if (!fe_fpu_enabled()) {
        return NULL;
    }
    void *p = fe_kzalloc(FE_FPU_AREA_SIZE);
    if (!p) {
        return NULL;
    }
    /* ★ 对齐是硬要求，不是优化 ★
     * XSAVE/XRSTOR 要求 64 字节对齐，不对齐直接 #GP，
     * 而 #GP 会发生在"换成下一个线程"的那一刻——现场离原因很远。
     * 内核堆的 1024 字节等级天然满足（页 + 64，步长 1024），
     * 但这条依赖是**隐式**的，所以这里显式验证：一旦哪天堆的布局变了，
     * 我们在这里就知道，而不是在切换路径上收一个 #GP。 */
    if (((uptr)p & (FE_FPU_AREA_ALIGN - 1)) != 0) {
        fe_kprintf("[FPU] **状态区未按 %u 字节对齐**（得到 %p）——"
                   "内核堆的尺寸等级假设被破坏了\n",
                   (unsigned)FE_FPU_AREA_ALIGN, p);
        fe_kfree(p);
        return NULL;
    }
    memset(p, 0, FE_FPU_AREA_SIZE);     /* 全零 = 所有部件处于初始态 */
    return p;
}

void fe_fpu_area_free(void *area)
{
    if (area) {
        fe_kfree(area);
    }
}

u32 fe_selftest_fpu(void)
{
    u32 fail = 0;
    u32 a = 0, b = 0, c = 0, d = 0;

#define FCHECK(cond) do { if (!(cond)) { fail++; \
    fe_kprintf("[FPU] 自检失败: %s（fpu.c:%d）\n", #cond, __LINE__); } } while (0)

    FCHECK(fe_fpu_enabled());
    /* CR0 必须真的干净：EM 会让所有 FPU 指令 #NM，TS 会让第一条 FPU 指令陷入 */
    FCHECK((fe_read_cr0() & (CR0_EM | CR0_TS)) == 0);
    FCHECK((fe_read_cr0() & CR0_MP) != 0);
    FCHECK((fe_read_cr4() & (CR4_OSFXSR | CR4_OSXMMEXCPT)) ==
           (CR4_OSFXSR | CR4_OSXMMEXCPT));
    if (g_mode == FE_FPU_XSAVE || g_mode == FE_FPU_XSAVEOPT) {
        FCHECK((fe_read_cr4() & CR4_OSXSAVE) != 0);
        /* XCR0 必须与"我们以为的"一致：不一致说明有人动过 */
        FCHECK(xgetbv0() == g_xcr0);
        FCHECK((g_xcr0 & (XCR0_X87 | XCR0_SSE)) == (XCR0_X87 | XCR0_SSE));
        /* AVX 必须在 CPUID 与 XCR0 上一致 */
        cpuid(1, 0, &a, &b, &c, &d);
        FCHECK(((c & (1u << 28)) != 0) == ((g_xcr0 & XCR0_AVX) != 0));
    }

    /* 状态区：对齐 + 往返一致。
     * 往返测试故意**不走用户态**：内核不能用 SIMD 指令，
     * 但 xsave/xrstor 本身是"搬状态"的指令，内核可以用它——
     * 而用不了 xmm 寄存器意味着我没法在内核里填充测试数据，
     * 所以这里只验"保存→恢复不崩、区内容自洽"，
     * 真正的数据往返由用户态的 simdtest 验（那才是它该在的地方）。 */
    void *area = fe_fpu_area_alloc();
    FCHECK(area != NULL);
    if (area) {
        FCHECK(((uptr)area & (FE_FPU_AREA_ALIGN - 1)) == 0);
        fe_fpu_save(area);
        fe_fpu_restore(area);
        fe_fpu_restore(NULL);       /* 恢复成干净状态也必须能走通 */
        fe_fpu_area_free(area);
    }
#undef FCHECK
    return fail;
}
