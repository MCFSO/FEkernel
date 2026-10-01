/* SPDX-License-Identifier: 0BSD */
#include <fe/mm/pmm.h>
#include <fe/mm/vmm.h>
#include <fe/boot/bootinfo.h>
#include <fe/io.h>
#include <fe/boot/limine.h>
#include <fe/kprintf.h>
#include <fe/panic.h>
#include <fe/string.h>

/* 链接脚本在映像末尾定义的符号：位图就放在它后面的物理内存里 */
extern char __kernel_end[];

#define BITMAP_USED  1
#define BITMAP_FREE  0

/* 物理地址最低 1 MiB 强制保留 */
#define RESERVED_LOW_MEMORY (1ull * 1024 * 1024)

static u64 *g_bitmap;
static u64  g_bitmap_bytes;
static u64  g_total_frames;
static u64  g_usable_frames;
static u64  g_reserved_frames;
static u64  g_free_frames;
static u64  g_search_hint;          /* 下次分配的起始帧号，避免每次从头扫描 */
static phys_addr_t g_bitmap_phys;
static u64  g_bitmap_frames;

/* 引导器报告的可用内存区间。MMIO 映射时要用它做重叠检查，
 * 所以必须在初始化时留档——位图只能回答「这帧有没有被分配」，
 * 回答不了「这帧本来是内存还是设备空间」（内核映像本身也是「已分配的内存」）。 */
#define FE_PMM_USABLE_MAX 64
static struct {
    phys_addr_t base;
    u64 size;
} g_usable_ranges[FE_PMM_USABLE_MAX];
static u32  g_usable_count;
static bool g_usable_overflow;

/* 位为 1 = 已占用，位为 0 = 空闲 */
FE_INLINE bool frame_is_used(u64 frame)
{
    return (g_bitmap[frame >> 6] >> (frame & 63)) & 1ull;
}

FE_INLINE void frame_mark_used(u64 frame)
{
    g_bitmap[frame >> 6] |= (1ull << (frame & 63));
}

FE_INLINE void frame_mark_free(u64 frame)
{
    g_bitmap[frame >> 6] &= ~(1ull << (frame & 63));
}

u64 fe_pmm_bitmap_bytes(void)
{
    return g_bitmap_bytes;
}

u64 fe_pmm_free_frame_count(void)
{
    return g_free_frames;
}

/* 为位图选址。
 *
 * 教训（真实踩过的坑）：位图**不能**紧跟在内核映像后面随意放置。内核映像之后往往
 * 是「引导器可回收」内存，而引导器在里面放着自己的页表——此时我们还没切换到自建
 * 页表，写坏它们会立刻导致页错误（而且是保留位违例这种难懂的形态）。
 * 因此必须从引导器明确标为「可用」的内存里取：那块内存里不可能有活着的引导器结构。 */
static phys_addr_t pick_bitmap_location(const struct fe_boot_info *bi, u64 bytes)
{
    for (u64 i = 0; i < bi->memmap_count; i++) {
        const struct fe_memmap_region *r = &bi->memmap[i];
        if (r->type != FE_LIMMAP_USABLE) {
            continue;
        }
        phys_addr_t start = FE_FRAME_ALIGN_UP(r->base);
        if (start < RESERVED_LOW_MEMORY) {
            start = FE_FRAME_ALIGN_UP(RESERVED_LOW_MEMORY);
        }
        phys_addr_t end = FE_FRAME_ALIGN_DOWN(r->base + r->length);
        if (end > start && end - start >= bytes) {
            return start;
        }
    }
    return 0;
}

static u64 mark_range_used(phys_addr_t base, u64 size)
{
    if (size == 0) {
        return 0;
    }
    u64 first = FE_FRAME_INDEX(FE_FRAME_ALIGN_DOWN(base));
    u64 last = FE_FRAME_INDEX(FE_FRAME_ALIGN_UP(base + size) - 1);
    if (last >= g_total_frames) {
        last = g_total_frames - 1;
    }
    u64 newly = 0;
    for (u64 f = first; f <= last; f++) {
        if (!frame_is_used(f)) {
            frame_mark_used(f);
            g_free_frames--;
            newly++;
        }
    }
    return newly;
}

/* 把引导器报告的可用区间「放出」为空闲。
 * 注意：本函数只在初始化阶段调用，此时位图刚刚被整体置为「已占用」，
 * 所以这里看到置位的帧一律是要放出的——强制保留是在放完之后才做的，
 * 不存在「已经被保留、必须跳过」的帧。 */
static void release_usable_range(phys_addr_t base, u64 size)
{
    if (size == 0) {
        return;
    }
    u64 region_end = base + size;
    u64 first = FE_FRAME_INDEX(FE_FRAME_ALIGN_UP(base));      /* 起点向上取整 */
    u64 end = FE_FRAME_ALIGN_DOWN(region_end);                /* 终点向下取整 */
    if (end <= base) {
        return;                                               /* 区间不足一整帧 */
    }
    u64 last = (end >> FE_FRAME_SHIFT) - 1;                   /* 最后一帧的帧号 */
    if (last >= g_total_frames) {
        last = g_total_frames - 1;
    }
    for (u64 f = first; f <= last && f < g_total_frames; f++) {
        if (frame_is_used(f)) {
            frame_mark_free(f);
            g_free_frames++;
        }
    }
}

void fe_pmm_init(void)
{
    const struct fe_boot_info *bi = fe_boot_info();
    FE_ASSERT(bi->memmap_count > 0);

    g_total_frames = FE_FRAME_INDEX(FE_FRAME_ALIGN_UP(bi->highest_address));
    g_bitmap_bytes = (g_total_frames + 7) / 8;
    /* 8 字节对齐，位图按 u64 数组访问 */
    g_bitmap_bytes = fe_align_up(g_bitmap_bytes, 8);

    /* 位图选址：必须放在**可用**内存里（见 pick_bitmap_location 的说明） */
    g_bitmap_phys = pick_bitmap_location(bi, g_bitmap_bytes);
    if (g_bitmap_phys == 0) {
        fe_panic("找不到足够大的可用内存区间放置 PMM 位图（需要 %llu 字节）",
                 (unsigned long long)g_bitmap_bytes);
    }
    g_bitmap = (u64 *)(uptr)(g_bitmap_phys + bi->hhdm_offset);

    /* 第一步：全部标记为已占用（保留区/ACPI/帧缓冲/内核模块自动落入此状态） */
    memset(g_bitmap, 0xFF, g_bitmap_bytes);
    g_free_frames = 0;

    /* 第二步：放出引导器报告的可用区间 */
    g_usable_count = 0;
    g_usable_overflow = false;
    for (u64 i = 0; i < bi->memmap_count; i++) {
        const struct fe_memmap_region *r = &bi->memmap[i];
        if (r->type == FE_LIMMAP_USABLE) {
            g_usable_frames += r->length / FE_FRAME_SIZE;
            release_usable_range(r->base, r->length);
            /* 记下来：fe_pmm_is_usable 要用它挡住「把普通内存当 MMIO 映射」 */
            if (r->length >= FE_FRAME_SIZE) {
                if (g_usable_count < FE_PMM_USABLE_MAX) {
                    g_usable_ranges[g_usable_count].base = r->base;
                    g_usable_ranges[g_usable_count].size = r->length;
                    g_usable_count++;
                } else if (!g_usable_overflow) {
                    g_usable_overflow = true;
                    fe_kprintf("[内存] 警告：可用区间超过 %u 段，MMIO 重叠检查将不完整\n",
                               (u32)FE_PMM_USABLE_MAX);
                }
            }
        }
    }

    /* 第三步：强制保留 */
    g_reserved_frames = 0;
    g_reserved_frames += mark_range_used(0, RESERVED_LOW_MEMORY);
    g_reserved_frames += mark_range_used(g_bitmap_phys, g_bitmap_bytes);
    g_bitmap_frames = g_reserved_frames;

    g_search_hint = FE_FRAME_INDEX(RESERVED_LOW_MEMORY);

    if (g_free_frames == 0) {
        fe_panic("物理内存管理器初始化后没有任何空闲帧");
    }
}

void fe_pmm_reserve(phys_addr_t base, u64 size)
{
    g_reserved_frames += mark_range_used(base, size);
}

u32 fe_pmm_usable_region_count(void)
{
    return g_usable_count;
}

bool fe_pmm_is_usable(phys_addr_t base, u64 size)
{
    if (size == 0) {
        return false;
    }
    u64 end = base + size;
    if (end < base) {
        return true;        /* 回绕：按「越界」处理，宁可拒绝映射 */
    }
    for (u32 i = 0; i < g_usable_count; i++) {
        u64 a0 = g_usable_ranges[i].base;
        u64 a1 = a0 + g_usable_ranges[i].size;
        if (base < a1 && a0 < end) {
            return true;
        }
    }
    return false;
}

static phys_addr_t alloc_frames_scan(u64 count, u64 start)
{
    if (count == 0) {
        return 0;
    }
    u64 run = 0;
    for (u64 f = start; f < g_total_frames; f++) {
        if (frame_is_used(f)) {
            run = 0;
            continue;
        }
        if (++run == count) {
            u64 first = f + 1 - count;
            for (u64 k = first; k <= f; k++) {
                frame_mark_used(k);
            }
            g_free_frames -= count;
            g_search_hint = f + 1;
            return (phys_addr_t)first << FE_FRAME_SHIFT;
        }
    }
    return 0;
}

phys_addr_t fe_pmm_alloc_frames(u64 count)
{
    if (count == 0 || count > g_free_frames) {
        return 0;
    }
    phys_addr_t p = alloc_frames_scan(count, g_search_hint);
    if (p == 0) {
        p = alloc_frames_scan(count, FE_FRAME_INDEX(RESERVED_LOW_MEMORY));  /* 回绕重试 */
    }
    return p;
}

phys_addr_t fe_pmm_alloc_frame(void)
{
    return fe_pmm_alloc_frames(1);
}

void fe_pmm_free_frames(phys_addr_t base, u64 count)
{
    if (base == 0 || count == 0 || !fe_is_aligned(base, FE_FRAME_SIZE)) {
        /* ★ 打印调用者地址再 panic ★
         * "某处释放了物理帧 0"这个信息指向不了任何具体代码——21 个调用点
         * 里任何一个把 0 传进来都会得到同一句话。而内核带调试信息，
         * 所以把返回地址打出来就能直接定位到那一行。
         * 少这一行的时候，只能靠一个个调用点去读（而这次读错了方向：
         * 我先怀疑的是地址空间销毁，实际原因在别处）。 */
        fe_kprintf("[pmm] 非法释放: base=%#llx count=%llu 调用者=%#llx\n",
                   (unsigned long long)base, (unsigned long long)count,
                   (unsigned long long)(uptr)__builtin_return_address(0));
        fe_panic("fe_pmm_free_frames 参数非法: base=%#llx count=%llu",
                 (unsigned long long)base, (unsigned long long)count);
    }
    u64 first = FE_FRAME_INDEX(base);
    for (u64 i = 0; i < count; i++) {
        u64 f = first + i;
        if (f >= g_total_frames) {
            break;
        }
        if (frame_is_used(f)) {
            frame_mark_free(f);
            g_free_frames++;
        }
    }
    if (first < g_search_hint) {
        g_search_hint = first;
    }
}

void fe_pmm_free_frame(phys_addr_t frame)
{
    fe_pmm_free_frames(frame, 1);
}

void fe_pmm_get_stats(struct fe_pmm_stats *out)
{
    out->total_frames = g_total_frames;
    out->usable_frames = g_usable_frames;
    out->reserved_frames = g_reserved_frames;
    out->free_frames = g_free_frames;
    out->allocated_frames = g_total_frames - g_free_frames;
    out->bitmap_frames = g_bitmap_frames;
    out->bitmap_phys = g_bitmap_phys;
    out->bitmap_bytes = g_bitmap_bytes;
}
