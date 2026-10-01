/* SPDX-License-Identifier: 0BSD */
/* libposix：用户态堆（见 docs/10-posix-layer.md）。
 *
 * ★ 为什么这是"补上一个从没实现的承诺"，而不是新功能 ★
 *
 * user/include/fe_user.h 里从早期就一直写着 malloc/free/calloc/realloc，
 * 还配了一大段说明（arena、统计、fe_malloc_check 的结构性自检）——
 * 而 libfe.c 里**一行都没有**。因为在此之前没有任何用户态程序用过它，
 * 所以从来没有人发现：声明了却没有实现，编译器不报错，
 * 直到有程序真的调用它才在链接期炸出来（POSIX 探测程序就是第一个）。
 *
 * 这个项目反复记的教训是"没有症状不等于没有问题"——这是又一个例子：
 * 一个**从未被使用**的接口，它的文档描述的是意图，不是事实。
 *
 * ---- 实现方式 ----
 *
 * arena 建立在**内存对象**上（fe_mem_alloc + fe_mem_map），
 * 块头里放幻数与边界，空闲块双向链表、分配时按需切分、释放时向前后合并。
 * 为什么不用"每次 malloc 都开一个内存对象"：一次 IPC-free 的分配要
 * 走 syscall + 页表操作，而 malloc 会被调用几十万次（每个 strdup、每个
 * 临时缓冲）。arena 让 malloc 变成纯用户态的指针运算。
 *
 * 代价是**释放不还内存给内核**——arena 只增不减（除 realloc 缩小）。
 * 对今天的用途是对的（进程生命周期短），但这是一个明确的边界，
 * 写在这里而不是假装它有完整的 free 语义。
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <fe_user.h>

/* ------------------------------------------------------------------ */
/* 块结构                                                              */
/* ------------------------------------------------------------------ */

#define HEAP_MAGIC_FREE  0x46524545u    /* "FREE" */
#define HEAP_MAGIC_USED  0x55534544u    /* "USED" */
#define HEAP_ALIGN       16

struct blk {
    u32 magic;
    u32 size;               /* 可用负载字节数（不含头部） */
    struct blk *prev;
    struct blk *next;
    u32 used;
    u32 _pad;
};
/* 24 字节头 → 为了负载 16 字节对齐，头本身需要补齐到 32 */
#define HDR_SIZE 32
#define MIN_BLK  64             /* 最小块（含头），避免碎片到无法复用 */

struct chunk {
    struct chunk *next;
    u64 size;               /* 内存对象字节数 */
    long handle;
    u8 *base;               /* 映射后的虚拟地址 */
};

#define CHUNK_MIN   (64u * 1024u)
#define CHUNK_MAX   (8u * 1024u * 1024u)

static struct chunk *g_chunks;
static u64 g_next_chunk_size = CHUNK_MIN;

static u64 g_live_bytes;
static u64 g_live_blocks;
static u64 g_total_blocks;
static u64 g_free_blocks;
static u64 g_alloc_calls;
static u64 g_free_calls;
static u64 g_peak_live;
static u64 g_arena_bytes;

static struct blk *blk_of(void *p)
{
    return (struct blk *)((u8 *)p - HDR_SIZE);
}

static void *payload_of(struct blk *b)
{
    return (void *)((u8 *)b + HDR_SIZE);
}

/* 把两个相邻的空闲块合成一个。**不合并**是分配器最常见的错，
 * 而症状是"明明有空闲却分配不出来"——所以这里不做"差不多就行"，
 * 每次 free 都尝试向前后合并。 */
static void coalesce(struct blk *b)
{
    if (b->next && !b->next->used &&
        (u8 *)b + HDR_SIZE + b->size == (u8 *)b->next) {
        struct blk *n = b->next;
        b->size += HDR_SIZE + n->size;
        b->next = n->next;
        if (n->next) {
            n->next->prev = b;
        }
        g_free_blocks--;
    }
    if (b->prev && !b->prev->used &&
        (u8 *)b->prev + HDR_SIZE + b->prev->size == (u8 *)b) {
        struct blk *p = b->prev;
        p->size += HDR_SIZE + b->size;
        p->next = b->next;
        if (b->next) {
            b->next->prev = p;
        }
        g_free_blocks--;
    }
}

/* 向内核申请一块新 arena 并挂到链尾 */
static int grow(u64 need)
{
    u64 size = g_next_chunk_size;
    if (size < need + HDR_SIZE + MIN_BLK) {
        size = need + HDR_SIZE + MIN_BLK;
    }
    size = (size + 4095u) & ~4095ull;

    long h = fe_mem_alloc(size);
    if (h <= 0) {
        return -1;
    }
    u8 *base = (u8 *)fe_mem_map(h, NULL, 0, FE_PROT_READ | FE_PROT_WRITE);
    if (!base) {
        fe_handle_close(h);
        return -1;
    }
    struct chunk *c = (struct chunk *)base;
    /* 块头就放在 arena 开头：多花 32 字节，换来"不用另外分配管理结构" */
    struct blk *b = (struct blk *)(base + sizeof(struct chunk));
    b->magic = HEAP_MAGIC_FREE;
    b->used = 0;
    b->prev = NULL;
    b->next = NULL;
    b->size = (u32)(size - sizeof(struct chunk) - HDR_SIZE);

    c->next = g_chunks;
    c->size = size;
    c->handle = h;
    c->base = base;
    g_chunks = c;
    g_arena_bytes += size;
    if (g_next_chunk_size < CHUNK_MAX) {
        g_next_chunk_size *= 2;         /* 翻倍增长：避免每加一个小对象就开一块 */
    }
    /* 新块挂到全局链尾（按地址顺序，合并才可能发生） */
    if (b->size >= MIN_BLK - HDR_SIZE) {
        g_total_blocks++;
        g_free_blocks++;
    }
    return 0;
}

/* 在已有的 arena 里找一块够大的空闲块（首次适配） */
static struct blk *find_free(u64 need)
{
    for (struct chunk *c = g_chunks; c; c = c->next) {
        u8 *start = c->base + sizeof(struct chunk);
        u8 *end = c->base + c->size;
        for (struct blk *b = (struct blk *)start; (u8 *)b + HDR_SIZE <= end;
             b = (struct blk *)((u8 *)b + HDR_SIZE + b->size)) {
            if (b->magic != HEAP_MAGIC_FREE && b->magic != HEAP_MAGIC_USED) {
                break;                  /* 链被写坏：停下来，别再往下走 */
            }
            if (!b->used && b->size >= need) {
                return b;
            }
            if (b->size == 0) {
                break;
            }
        }
    }
    return NULL;
}

static void *alloc_from(struct blk *b, u64 need)
{
    /* 够大就切分：留下至少 MIN_BLK 给下一次 */
    if (b->size >= need + MIN_BLK) {
        u8 *nb_addr = (u8 *)b + HDR_SIZE + need;
        nb_addr = (u8 *)(((usize)nb_addr + (HEAP_ALIGN - 1)) & ~(usize)(HEAP_ALIGN - 1));
        struct blk *nb = (struct blk *)nb_addr;
        u32 consumed = (u32)(nb_addr - ((u8 *)b + HDR_SIZE));
        nb->magic = HEAP_MAGIC_FREE;
        nb->used = 0;
        nb->size = b->size - consumed - HDR_SIZE;
        nb->prev = b;
        nb->next = b->next;
        if (b->next) {
            b->next->prev = nb;
        }
        b->next = nb;
        b->size = consumed;
        g_total_blocks++;
        g_free_blocks++;
    }
    b->magic = HEAP_MAGIC_USED;
    b->used = 1;
    g_free_blocks--;
    g_live_blocks++;
    g_live_bytes += b->size;
    if (g_live_bytes > g_peak_live) {
        g_peak_live = g_live_bytes;
    }
    return payload_of(b);
}

/* ------------------------------------------------------------------ */
/* POSIX 接口                                                          */
/* ------------------------------------------------------------------ */

void *malloc(size_t size)
{
    if (size == 0) {
        size = 1;               /* malloc(0) 可以返回可 free 的指针 */
    }
    u64 need = (size + (HEAP_ALIGN - 1)) & ~(u64)(HEAP_ALIGN - 1);
    g_alloc_calls++;

    struct blk *b = find_free(need);
    if (!b) {
        if (grow(need) != 0) {
            errno = ENOMEM;
            return NULL;
        }
        b = find_free(need);
        if (!b) {
            errno = ENOMEM;
            return NULL;
        }
    }
    return alloc_from(b, need);
}

void free(void *ptr)
{
    if (!ptr) {
        return;                 /* free(NULL) 是合法的空操作（POSIX 明文） */
    }
    struct blk *b = blk_of(ptr);
    if (b->magic != HEAP_MAGIC_USED) {
        /* ★ 坏指针：**不要**试图修复，也不要静默忽略 ★
         * 继续跑下去会用错误的边界把堆链改乱，而症状会在很久以后
         * 以"随机崩溃"的形式出现。这里直接把事实喊出来并终止——
         * 一个能定位的崩溃远好过一个查不到的损坏。 */
        fe_puts("\n[libposix] free(): 无效指针（块头幻数不符）——堆已被写坏，终止\n");
        fe_flush();
        _exit(134);
    }
    b->magic = HEAP_MAGIC_FREE;
    b->used = 0;
    g_free_calls++;
    g_live_blocks--;
    g_live_bytes -= b->size;
    g_free_blocks++;
    coalesce(b);
}

void *calloc(size_t nmemb, size_t size)
{
    /* 乘法溢出必须挡：`calloc(n, 4)` 里 n = 2^62 会让 size 变成 0，
     * 于是返回一个"看起来成功"的小缓冲，而调用者会往里写 n*4 字节。 */
    if (nmemb != 0 && size > (size_t)-1 / nmemb) {
        errno = ENOMEM;
        return NULL;
    }
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (!p) {
        return NULL;
    }
    memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    struct blk *b = blk_of(ptr);
    if (b->magic != HEAP_MAGIC_USED) {
        errno = EINVAL;
        return NULL;
    }
    if (b->size >= size) {
        /* 缩小：就地返回。**不切分**——切分会让"缩一点再长回来"
         * 反复产生碎片，而这里省下的内存本来就还不到内核。
         * 这是明确的取舍，不是省事。 */
        return ptr;
    }
    void *np = malloc(size);
    if (!np) {
        return NULL;            /* 失败时**不动原块**（POSIX 要求） */
    }
    memcpy(np, ptr, b->size);
    free(ptr);
    return np;
}

/* ------------------------------------------------------------------ */
/* 诊断与自检（fe_user.h 里承诺过的那几个）                             */
/* ------------------------------------------------------------------ */

void fe_malloc_stat_get(struct fe_malloc_stat *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->arena_size = g_arena_bytes;
    out->arena_used = g_arena_bytes;        /* arena 只增不减（见文件头说明） */
    out->live_bytes = g_live_bytes;
    out->live_blocks = g_live_blocks;
    out->total_blocks = g_total_blocks;
    out->free_blocks = g_free_blocks;
    out->alloc_calls = g_alloc_calls;
    out->free_calls = g_free_calls;
    out->peak_live_bytes = g_peak_live;
}

u32 fe_malloc_live_blocks(void)
{
    return (u32)g_live_blocks;
}

/* 走一遍堆，返回发现的结构性错误数。
 *
 * 检查四件事，每一条都对应一类真实错误：
 *   1. 块头幻数（野指针写坏 / 双重释放）；
 *   2. 块边界自洽（size 与下一块地址对得上）；
 *   3. 相邻空闲块**没有被合并**（分配器最容易出的错，
 *      症状是"明明有空闲却分配不出来"）；
 *   4. 计数与链表一致（统计不能自己骗自己）。 */
u32 fe_malloc_check(void)
{
    u32 bad = 0;
    u64 counted_total = 0;
    u64 counted_free = 0;
    u64 counted_live = 0;

    for (struct chunk *c = g_chunks; c; c = c->next) {
        u8 *end = c->base + c->size;
        u8 *p = c->base + sizeof(struct chunk);
        struct blk *prev = NULL;
        while (p + HDR_SIZE <= end) {
            struct blk *b = (struct blk *)p;
            if (b->magic != HEAP_MAGIC_FREE && b->magic != HEAP_MAGIC_USED) {
                bad++;              /* 链断了：无法继续，换下一块 arena */
                break;
            }
            if (b->magic == HEAP_MAGIC_USED && !b->used) {
                bad++;
            }
            if (b->magic == HEAP_MAGIC_FREE && b->used) {
                bad++;
            }
            counted_total++;
            if (b->used) {
                counted_live++;
            } else {
                counted_free++;
                if (prev && !prev->used) {
                    bad++;          /* 相邻空闲块没合并 */
                }
            }
            if (p + HDR_SIZE + b->size > end) {
                bad++;              /* 越出 arena 边界 */
                break;
            }
            prev = b;
            p = p + HDR_SIZE + b->size;
            p = (u8 *)(((usize)p + (HEAP_ALIGN - 1)) & ~(usize)(HEAP_ALIGN - 1));
        }
    }
    if (counted_total != g_total_blocks) {
        bad++;
    }
    if (counted_free != g_free_blocks) {
        bad++;
    }
    if (counted_live != g_live_blocks) {
        bad++;
    }
    return bad;
}
