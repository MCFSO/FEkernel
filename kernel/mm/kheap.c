/* SPDX-License-Identifier: 0BSD */
#include <fe/mm/kheap.h>
#include <fe/mm/vmm.h>
#include <fe/mm/pmm.h>
#include <fe/kprintf.h>
#include <fe/panic.h>
#include <fe/string.h>
#include <fe/status.h>

/* ------------------------------------------------------------------ */
/* 堆虚拟区域分配器                                                    */
/* ------------------------------------------------------------------ */

/* 为了不做「分配描述符要用堆」的鸡生蛋问题，描述符来自静态池 */
#define VA_POOL_SIZE 256

struct fe_va_region {
    virt_addr_t base;
    u64 pages;
    struct fe_va_region *next;
};

static struct fe_va_region  g_va_pool[VA_POOL_SIZE];
static struct fe_va_region *g_va_pool_free;
static struct fe_va_region *g_va_free_list;      /* 按地址升序的空闲区间 */
static u64 g_va_used_pages;

static struct fe_va_region *va_desc_get(void)
{
    if (!g_va_pool_free) {
        return NULL;
    }
    struct fe_va_region *r = g_va_pool_free;
    g_va_pool_free = r->next;
    r->next = NULL;
    return r;
}

static void va_desc_put(struct fe_va_region *r)
{
    r->next = g_va_pool_free;
    g_va_pool_free = r;
}

static virt_addr_t va_alloc(u64 pages)
{
    if (pages == 0) {
        return 0;
    }
    for (struct fe_va_region *r = g_va_free_list; r; r = r->next) {
        if (r->pages < pages) {
            continue;
        }
        virt_addr_t base = r->base;
        if (r->pages == pages) {
            /* 整块用掉：从链表摘除 */
            if (g_va_free_list == r) {
                g_va_free_list = r->next;
            } else {
                struct fe_va_region *prev = g_va_free_list;
                while (prev && prev->next != r) {
                    prev = prev->next;
                }
                if (prev) {
                    prev->next = r->next;
                }
            }
            va_desc_put(r);
        } else {
            r->base += pages * FE_FRAME_SIZE;
            r->pages -= pages;
        }
        g_va_used_pages += pages;
        return base;
    }
    return 0;
}

static void va_free(virt_addr_t base, u64 pages)
{
    if (pages == 0) {
        return;
    }
    struct fe_va_region *desc = va_desc_get();
    if (!desc) {
        fe_panic("堆虚拟区域描述符耗尽");
    }
    desc->base = base;
    desc->pages = pages;

    /* 按地址插入并合并相邻区间 */
    struct fe_va_region **pp = &g_va_free_list;
    while (*pp && (*pp)->base < base) {
        pp = &(*pp)->next;
    }
    desc->next = *pp;
    *pp = desc;

    if (desc->next && desc->base + desc->pages * FE_FRAME_SIZE == desc->next->base) {
        struct fe_va_region *nxt = desc->next;
        desc->pages += nxt->pages;
        desc->next = nxt->next;
        va_desc_put(nxt);
    }
    /* 与前一个区间合并 */
    if (pp != &g_va_free_list) {
        struct fe_va_region *prev = g_va_free_list;
        while (prev && prev->next != desc) {
            prev = prev->next;
        }
        if (prev && prev->base + prev->pages * FE_FRAME_SIZE == desc->base) {
            prev->pages += desc->pages;
            prev->next = desc->next;
            va_desc_put(desc);
        }
    }
    g_va_used_pages -= pages;
}

/* 分配 pages 个虚拟页并映射新物理帧，返回虚拟基址（失败返回 0） */
static virt_addr_t heap_map_pages(u64 pages, u64 flags)
{
    virt_addr_t base = va_alloc(pages);
    if (!base) {
        return 0;
    }
    fe_status_t s = fe_vmm_map_alloc(fe_vmm_kernel_space(), base, pages * FE_FRAME_SIZE, flags);
    if (fe_failed(s)) {
        va_free(base, pages);
        return 0;
    }
    return base;
}

static void heap_unmap_pages(virt_addr_t base, u64 pages)
{
    fe_vmm_unmap_free(fe_vmm_kernel_space(), base, pages * FE_FRAME_SIZE);
    va_free(base, pages);
}

/* ------------------------------------------------------------------ */
/* slab 缓存                                                           */
/* ------------------------------------------------------------------ */

#define SLAB_MAGIC  0x534c414246454b31ull   /* "SLABFEK1" */
#define LARGE_MAGIC 0x4c41524746454b31ull   /* "LARGFEK1" */

/* 每个 slab 页的头部（对象从头部之后开始，因此由对象指针取页基址即可读到头部） */
#define SLAB_HDR_SIZE 64

struct fe_slab {
    u64 magic;
    struct fe_slab_cache *cache;
    struct fe_slab *next;
    void *freelist;
    u32 obj_size;
    u32 free_count;
    u32 total_count;
    u32 pad;
};

struct fe_slab_cache {
    u32 obj_size;
    u32 pad;
    struct fe_slab *partial;    /* 有空闲对象的 slab */
    struct fe_slab *full;
    u64 slab_count;
    u64 live_objects;
};

struct fe_large_hdr {
    u64 magic;
    u64 data_pages;
    u64 va_pages;
    u64 pad;
};

#define KHEAP_MIN_SIZE 16
#define KHEAP_MAX_SLAB 2048
#define KHEAP_CLASSES 8             /* 16,32,...,2048 */

static struct fe_slab_cache g_caches[KHEAP_CLASSES];
static u64 g_large_count;
static u64 g_large_bytes;
static u64 g_total_allocs;
static u64 g_total_frees;
static bool g_heap_ready;

static u32 size_class_index(size_t size)
{
    u32 idx = 0;
    size_t s = KHEAP_MIN_SIZE;
    while (s < size && idx < KHEAP_CLASSES - 1) {
        s <<= 1;
        idx++;
    }
    return idx;
}

static bool slab_is_valid(const struct fe_slab *s)
{
    if (s->magic != SLAB_MAGIC) {
        return false;
    }
    const struct fe_slab_cache *c = s->cache;
    return c >= &g_caches[0] && c < &g_caches[KHEAP_CLASSES];
}

static struct fe_slab *slab_create(struct fe_slab_cache *cache)
{
    virt_addr_t page = heap_map_pages(1, FE_PTE_WRITE | FE_PTE_GLOBAL);
    if (!page) {
        return NULL;
    }
    struct fe_slab *s = (struct fe_slab *)page;
    s->magic = SLAB_MAGIC;
    s->cache = cache;
    s->next = NULL;
    s->obj_size = cache->obj_size;

    u8 *obj_base = (u8 *)page + SLAB_HDR_SIZE;
    u32 count = (u32)((FE_FRAME_SIZE - SLAB_HDR_SIZE) / cache->obj_size);
    if (count == 0) {
        count = 1;
    }
    /* 把空闲对象串成链（空闲对象的头 8 字节存下一个空闲对象地址） */
    for (u32 i = 0; i < count; i++) {
        void **slot = (void **)(void *)(obj_base + (u64)i * cache->obj_size);
        *slot = (i + 1 < count) ? (void *)(obj_base + (u64)(i + 1) * cache->obj_size)
                                : NULL;
    }
    s->freelist = obj_base;
    s->free_count = count;
    s->total_count = count;
    cache->slab_count++;
    return s;
}

static void *slab_alloc(struct fe_slab_cache *cache)
{
    struct fe_slab *s = cache->partial;
    if (!s) {
        s = slab_create(cache);
        if (!s) {
            return NULL;
        }
        s->next = cache->partial;
        cache->partial = s;
    }
    void *obj = s->freelist;
    if (!obj) {
        fe_panic("slab 空闲链为空但仍在 partial 链上（缓存 %u 字节）", cache->obj_size);
    }
    s->freelist = *(void **)obj;
    s->free_count--;
    cache->live_objects++;

    if (s->free_count == 0) {
        /* 摘出 partial，挂到 full */
        if (cache->partial == s) {
            cache->partial = s->next;
        } else {
            struct fe_slab *prev = cache->partial;
            while (prev && prev->next != s) {
                prev = prev->next;
            }
            if (prev) {
                prev->next = s->next;
            }
        }
        s->next = cache->full;
        cache->full = s;
    }
    return obj;
}

static void slab_free_object(struct fe_slab *s, void *obj)
{
    struct fe_slab_cache *cache = s->cache;
    *(void **)obj = s->freelist;
    s->freelist = obj;
    s->free_count++;
    cache->live_objects--;

    if (s->free_count == s->total_count) {
        /* 整页空闲：还给 PMM，避免内核堆只涨不落 */
        struct fe_slab **pp = &cache->partial;
        while (*pp && *pp != s) {
            pp = &(*pp)->next;
        }
        if (*pp) {
            *pp = s->next;
        } else {
            pp = &cache->full;
            while (*pp && *pp != s) {
                pp = &(*pp)->next;
            }
            if (*pp) {
                *pp = s->next;
            }
        }
        cache->slab_count--;
        heap_unmap_pages((virt_addr_t)(uptr)s, 1);
    } else if (s->free_count == 1 && s->total_count > 1) {
        /* 从 full 移回 partial，使其可再次分配 */
        struct fe_slab **pp = &cache->full;
        while (*pp && *pp != s) {
            pp = &(*pp)->next;
        }
        if (*pp) {
            *pp = s->next;
            s->next = cache->partial;
            cache->partial = s;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 公开接口                                                            */
/* ------------------------------------------------------------------ */

void fe_kheap_init(void)
{
    g_va_pool_free = NULL;
    for (u32 i = VA_POOL_SIZE; i > 0; i--) {
        g_va_pool[i - 1].next = g_va_pool_free;
        g_va_pool_free = &g_va_pool[i - 1];
    }
    g_va_free_list = va_desc_get();
    if (!g_va_free_list) {
        fe_panic("堆初始化失败：无描述符");
    }
    g_va_free_list->base = FE_KERNEL_HEAP_BASE;
    g_va_free_list->pages = FE_KERNEL_HEAP_SIZE / FE_FRAME_SIZE;
    g_va_free_list->next = NULL;
    g_va_used_pages = 0;

    for (u32 i = 0; i < KHEAP_CLASSES; i++) {
        g_caches[i].obj_size = (u32)(KHEAP_MIN_SIZE << i);
        g_caches[i].partial = NULL;
        g_caches[i].full = NULL;
        g_caches[i].slab_count = 0;
        g_caches[i].live_objects = 0;
    }
    g_heap_ready = true;
}

void *fe_kmalloc(size_t size)
{
    if (!g_heap_ready || size == 0) {
        return NULL;
    }
    g_total_allocs++;

    if (size <= KHEAP_MAX_SLAB) {
        struct fe_slab_cache *cache = &g_caches[size_class_index(size)];
        return slab_alloc(cache);
    }

    /* 大对象：数据页 + 1 个头部页，虚拟连续、物理离散 */
    u64 data_pages = (size + FE_FRAME_SIZE - 1) / FE_FRAME_SIZE;
    u64 va_pages = data_pages + 1;
    virt_addr_t base = heap_map_pages(va_pages, FE_PTE_WRITE | FE_PTE_GLOBAL);
    if (!base) {
        return NULL;
    }
    struct fe_large_hdr *hdr = (struct fe_large_hdr *)base;
    hdr->magic = LARGE_MAGIC;
    hdr->data_pages = data_pages;
    hdr->va_pages = va_pages;
    g_large_count++;
    g_large_bytes += data_pages * FE_FRAME_SIZE;
    return (void *)(base + FE_FRAME_SIZE);
}

void *fe_kzalloc(size_t size)
{
    /* 帧在映射时已经清零，但大对象的头部页之外的复用场景需要显式清零 */
    void *p = fe_kmalloc(size);
    if (p) {
        memset(p, 0, size);
    }
    return p;
}

void fe_kfree(void *ptr)
{
    if (!ptr) {
        return;
    }
    g_total_frees++;

    virt_addr_t page = FE_FRAME_ALIGN_DOWN((virt_addr_t)(uptr)ptr);
    struct fe_slab *s = (struct fe_slab *)(uptr)page;
    if (slab_is_valid(s)) {
        slab_free_object(s, ptr);
        return;
    }

    /* 大对象：数据页的前一页是头部页 */
    struct fe_large_hdr *hdr = (struct fe_large_hdr *)(uptr)(page - FE_FRAME_SIZE);
    if (hdr->magic == LARGE_MAGIC) {
        virt_addr_t base = (virt_addr_t)(uptr)hdr;
        g_large_count--;
        g_large_bytes -= hdr->data_pages * FE_FRAME_SIZE;
        u64 va_pages = hdr->va_pages;
        heap_unmap_pages(base, va_pages);
        return;
    }
    fe_panic("fe_kfree 收到非法指针 %p", ptr);
}

void *fe_krealloc(void *ptr, size_t new_size)
{
    if (!ptr) {
        return fe_kmalloc(new_size);
    }
    if (new_size == 0) {
        fe_kfree(ptr);
        return NULL;
    }

    /* 先判断原块能装多少：slab 对象按尺寸等级，大对象按数据页数 */
    size_t old_capacity;
    virt_addr_t page = FE_FRAME_ALIGN_DOWN((virt_addr_t)(uptr)ptr);
    struct fe_slab *s = (struct fe_slab *)(uptr)page;
    if (slab_is_valid(s)) {
        old_capacity = s->obj_size;
    } else {
        struct fe_large_hdr *hdr = (struct fe_large_hdr *)(uptr)(page - FE_FRAME_SIZE);
        if (hdr->magic != LARGE_MAGIC) {
            fe_panic("fe_krealloc 收到非法指针 %p", ptr);
        }
        old_capacity = hdr->data_pages * FE_FRAME_SIZE;
    }
    if (new_size <= old_capacity &&
        !(old_capacity > KHEAP_MAX_SLAB && new_size <= KHEAP_MAX_SLAB)) {
        return ptr;     /* 原地够用 */
    }

    void *n = fe_kmalloc(new_size);
    if (!n) {
        return NULL;
    }
    memcpy(n, ptr, old_capacity < new_size ? old_capacity : new_size);
    fe_kfree(ptr);
    return n;
}

void *fe_kmalloc_dma(size_t size, phys_addr_t *out_phys)
{
    if (size == 0 || !out_phys) {
        return NULL;
    }
    u64 pages = (size + FE_FRAME_SIZE - 1) / FE_FRAME_SIZE;
    phys_addr_t phys = fe_pmm_alloc_frames(pages);   /* 物理连续，便于设备 DMA */
    if (!phys) {
        return NULL;
    }
    void *virt = (void *)(uptr)(phys + fe_vmm_hhdm_offset());
    memset(virt, 0, pages * FE_FRAME_SIZE);
    *out_phys = phys;
    return virt;
}

void fe_kheap_get_stats(struct fe_kheap_stats *out)
{
    memset(out, 0, sizeof(*out));
    out->va_total_pages = FE_KERNEL_HEAP_SIZE / FE_FRAME_SIZE;
    out->va_used_pages = g_va_used_pages;
    out->va_free_pages = out->va_total_pages - g_va_used_pages;
    for (u32 i = 0; i < KHEAP_CLASSES; i++) {
        out->slab_count += g_caches[i].slab_count;
        out->slab_live_objects += g_caches[i].live_objects;
        for (struct fe_slab *s = g_caches[i].partial; s; s = s->next) {
            out->slab_free_objects += s->free_count;
        }
        for (struct fe_slab *s = g_caches[i].full; s; s = s->next) {
            out->slab_free_objects += s->free_count;
        }
    }
    out->mapped_pages = g_va_used_pages;
    out->large_count = g_large_count;
    out->large_bytes = g_large_bytes;
    out->total_allocs = g_total_allocs;
    out->total_frees = g_total_frees;
}

/* ------------------------------------------------------------------ */
/* 自检                                                                */
/* ------------------------------------------------------------------ */

#define ST_OBJECTS 48

u32 fe_kheap_selftest(void)
{
    u32 fail = 0;
    struct fe_kheap_stats s0, s1;

    fe_kheap_get_stats(&s0);
    fe_kprintf("[自检] 堆初始状态: 已用虚拟页 %llu, slab 页 %llu\n",
               (unsigned long long)s0.va_used_pages,
               (unsigned long long)s0.slab_count);

    /* --- 1. 各尺寸等级：分配 → 写模式 → 校验 → 释放 --- */
    for (u32 i = 0; i < KHEAP_CLASSES; i++) {
        size_t size = (size_t)KHEAP_MIN_SIZE << i;
        void *ptrs[ST_OBJECTS] = { 0 };
        u8 pattern = (u8)(0xA0 + i);

        for (u32 k = 0; k < ST_OBJECTS; k++) {
            ptrs[k] = fe_kmalloc(size);
            if (!ptrs[k]) {
                fe_kprintf("        尺寸 %4zu: 第 %u 个分配失败\n", size, k);
                fail++;
                break;
            }
            if (!fe_is_aligned((u64)(uptr)ptrs[k], 16)) {
                fail++;
            }
            memset(ptrs[k], pattern, size);
        }
        /* 隔一个释放，再确认剩下的内容没被破坏（验证空闲链没有踩到在用对象） */
        for (u32 k = 0; k < ST_OBJECTS; k += 2) {
            fe_kfree(ptrs[k]);
            ptrs[k] = NULL;
        }
        for (u32 k = 1; k < ST_OBJECTS; k += 2) {
            if (!ptrs[k]) {
                continue;
            }
            const u8 *p = (const u8 *)ptrs[k];
            for (size_t j = 0; j < size; j++) {
                if (p[j] != pattern) {
                    fe_kprintf("        尺寸 %4zu: 第 %u 个对象在偏移 %zu 处被破坏\n",
                               size, k, j);
                    fail++;
                    break;
                }
            }
        }
        /* 再分配一遍，复用刚释放的槽位 */
        for (u32 k = 0; k < ST_OBJECTS; k += 2) {
            ptrs[k] = fe_kmalloc(size);
            if (ptrs[k]) {
                memset(ptrs[k], (u8)(pattern ^ 0xFF), size);
            }
        }
        for (u32 k = 0; k < ST_OBJECTS; k++) {
            fe_kfree(ptrs[k]);
        }
    }

    /* --- 2. 大对象（虚拟连续、物理离散） --- */
    {
        size_t sizes[] = { 2049, 4096, 8192, 40000, 300000 };
        void *big[FE_ARRAY_LEN(sizes)] = { 0 };
        for (u32 i = 0; i < FE_ARRAY_LEN(sizes); i++) {
            big[i] = fe_kmalloc(sizes[i]);
            if (!big[i]) {
                fe_kprintf("        大对象 %zu 字节分配失败\n", sizes[i]);
                fail++;
                continue;
            }
            if (!fe_is_aligned((u64)(uptr)big[i], FE_FRAME_SIZE)) {
                fe_kprintf("        大对象 %zu 字节未按页对齐\n", sizes[i]);
                fail++;
            }
            memset(big[i], (int)(0x5A + i), sizes[i]);
        }
        /* 校验每个字节，特别是跨页边界处 */
        for (u32 i = 0; i < FE_ARRAY_LEN(sizes); i++) {
            if (!big[i]) {
                continue;
            }
            const u8 *p = (const u8 *)big[i];
            for (size_t j = 0; j < sizes[i]; j++) {
                if (p[j] != (u8)(0x5A + i)) {
                    fe_kprintf("        大对象 %zu 字节在偏移 %zu 处被破坏\n", sizes[i], j);
                    fail++;
                    break;
                }
            }
        }
        for (u32 i = 0; i < FE_ARRAY_LEN(sizes); i++) {
            fe_kfree(big[i]);
        }
    }

    /* --- 3. realloc：扩容后原内容必须保留 --- */
    {
        char *p = (char *)fe_kmalloc(100);
        if (!p) {
            fail++;
        } else {
            for (int i = 0; i < 100; i++) {
                p[i] = (char)i;
            }
            char *q = (char *)fe_krealloc(p, 9000);
            if (!q) {
                fail++;
            } else {
                for (int i = 0; i < 100; i++) {
                    if (q[i] != (char)i) {
                        fe_kprintf("        realloc 后第 %d 字节丢失\n", i);
                        fail++;
                        break;
                    }
                }
                fe_kfree(q);
            }
        }
    }

    /* --- 4. DMA 缓冲：物理地址合法且内容可写 --- */
    {
        phys_addr_t phys = 0;
        u8 *dma = (u8 *)fe_kmalloc_dma(8192, &phys);
        if (!dma || phys == 0 || !fe_is_aligned(phys, FE_FRAME_SIZE)) {
            fe_kprintf("        DMA 缓冲分配失败或物理地址非法\n");
            fail++;
        } else if ((u64)(uptr)dma != phys + fe_vmm_hhdm_offset()) {
            fe_kprintf("        DMA 缓冲虚拟地址不是 HHDM 映射\n");
            fail++;
        } else {
            for (u32 i = 0; i < 8192; i++) {
                dma[i] = (u8)(i * 7);
            }
            for (u32 i = 0; i < 8192; i++) {
                if (dma[i] != (u8)(i * 7)) {
                    fail++;
                    break;
                }
            }
            fe_pmm_free_frames(phys, 2);
        }
    }

    /* --- 5. 全部释放后必须回到**自检开始前**的状态（不能只涨不落）。
     * 注意基线不能假定为 0：调度器等子系统在自检之前就已经分配过常驻对象。 --- */
    fe_kheap_get_stats(&s1);
    if (s1.slab_count != s0.slab_count) {
        fe_kprintf("        释放后 slab 页数未回到基线: %llu -> %llu\n",
                   (unsigned long long)s0.slab_count, (unsigned long long)s1.slab_count);
        fail++;
    }
    if (s1.va_used_pages != s0.va_used_pages) {
        fe_kprintf("        释放后虚拟页占用未回到初始值: %llu -> %llu\n",
                   (unsigned long long)s0.va_used_pages,
                   (unsigned long long)s1.va_used_pages);
        fail++;
    }
    if (s1.va_free_pages + s1.va_used_pages != s1.va_total_pages) {
        fail++;
    }

    fe_kprintf("[自检] 堆累计分配 %llu 次 / 释放 %llu 次, 结束后 slab 页 %llu, "
               "已用虚拟页 %llu\n",
               (unsigned long long)s1.total_allocs, (unsigned long long)s1.total_frees,
               (unsigned long long)s1.slab_count, (unsigned long long)s1.va_used_pages);
    return fail;
}
