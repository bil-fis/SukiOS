/* =============================================================================
 * SukiOS - 内核堆分配器实现
 *
 * 使用隐式空闲链表（first-fit），每个块结构：
 *
 *   +-------------------+-------------------+
 *   | size (uint64_t)   | next (uint64_t)   |
 *   +-------------------+-------------------+
 *   |        data payload area              |
 *   +---------------------------------------+
 *
 * size 字段最高位 (bit 63) 标记块是否已分配：
 *   0 = 空闲，1 = 已分配
 *
 * 堆使用 VMM 动态映射物理页实现按需扩展。
 *
 * 参考：OSDev Kernel Memory Management, K&R malloc (8.7)
 * ============================================================================= */

#include "kernel/heap.h"
#include "kernel/vmm.h"
#include "kernel/tty.h"

/* ---- 块头部大小（16 字节，对齐到 16 字节） ---- */
#define BLOCK_HEADER_SIZE  16
#define BLOCK_ALIGN        16

/* ---- 最小块大小（头部 + 最小 16 字节 payload） ---- */
#define BLOCK_MIN_SIZE     (BLOCK_HEADER_SIZE + BLOCK_ALIGN)

/* ---- 块大小标志位 ---- */
#define BLOCK_ALLOCATED    (1ULL << 63)   /* 最高位标记已分配 */
#define BLOCK_SIZE_MASK    ~(BLOCK_ALLOCATED)  /* 取实际大小 */

/* ---- 堆状态 ---- */
static uint64_t heap_start;       /* 堆起始虚拟地址 */
static size_t  heap_max_size;     /* 堆最大容量 */
static size_t  heap_used_size;    /* 已使用容量 */
static size_t  heap_mapped_size;  /* 已映射的物理页容量 */

/* ---- 统计信息 ---- */
static size_t stat_total_allocs = 0;
static size_t stat_total_frees  = 0;

/* =========================================================================
 * 辅助函数
 * ========================================================================= */

/* 获取块的实际大小 */
static inline uint64_t block_size(uint64_t *header)
{
    return header[0] & BLOCK_SIZE_MASK;
}

/* 设置块的大小和分配状态 */
static inline void block_set(uint64_t *header, uint64_t size, bool allocated)
{
    header[0] = (size & BLOCK_SIZE_MASK) | (allocated ? BLOCK_ALLOCATED : 0);
}

/* 块是否已分配 */
static inline bool block_is_allocated(uint64_t *header)
{
    return (header[0] & BLOCK_ALLOCATED) != 0;
}

/* 获取下一个块的头指针 */
static inline uint64_t *block_next(uint64_t *header)
{
    uint64_t size = block_size(header);
    if (size == 0) return NULL;
    return (uint64_t *)((uint8_t *)header + size);
}

/* 返回对齐后的块大小 */
static inline uint64_t align_up(uint64_t val, uint64_t align)
{
    return (val + align - 1) & ~(align - 1);
}

/* =========================================================================
 * 堆扩展
 *
 * 当需要更多内存时，通过 VMM 分配新的物理页并映射到堆区域。
 * ========================================================================= */
static bool heap_expand_to(uint64_t needed_end)
{
    /* 需要确保从 heap_start 到 needed_end 都已映射 */
    uint64_t current_end = heap_start + heap_mapped_size;

    while (current_end < needed_end) {
        if (vmm_alloc_map(current_end, VMM_KERN_RW) != 0) {
            return false;
        }
        /* 清零新映射的页（vmm_alloc_map 已清零） */
        current_end += VMM_PAGE_SIZE;
        heap_mapped_size += VMM_PAGE_SIZE;
    }

    return true;
}

/* =========================================================================
 * 核心接口
 * ========================================================================= */

void heap_init(uint64_t start, size_t size)
{
    heap_start      = start;
    heap_max_size   = size;
    heap_used_size  = 0;
    heap_mapped_size = 0;

    /* 初始映射第一页（4KB 足够堆头部的初始化） */
    if (vmm_alloc_map(start, VMM_KERN_RW) != 0) {
        tty_setcolor(VGA_RED, VGA_BLACK);
        tty_print("  ERROR: Failed to map initial heap page!\n");
        return;
    }
    heap_mapped_size = VMM_PAGE_SIZE;

    /* 初始化：整个堆作为一个大空闲块 */
    uint64_t *first_block = (uint64_t *)(uintptr_t)start;
    block_set(first_block, VMM_PAGE_SIZE, false);
    first_block[1] = 0; /* next = NULL（隐式链表，不需要 next 指针） */
}

void *kmalloc(size_t size)
{
    if (size == 0) return NULL;

    /* 对齐请求大小并加上头部 */
    uint64_t total = align_up(size + BLOCK_HEADER_SIZE, BLOCK_ALIGN);
    if (total < BLOCK_MIN_SIZE)
        total = BLOCK_MIN_SIZE;

    /* 遍历空闲链表（隐式），first-fit */
    uint64_t *current = (uint64_t *)(uintptr_t)heap_start;
    uint64_t *best = NULL;
    uint64_t heap_end = heap_start + heap_mapped_size;

    while ((uint64_t)current < heap_end) {
        uint64_t bsize = block_size(current);

        if (bsize == 0) break; /* 到达堆尾 */

        if (!block_is_allocated(current) && bsize >= total) {
            best = current;
            break; /* first-fit */
        }

        current = (uint64_t *)((uint8_t *)current + bsize);
    }

    if (!best) {
        /* 没有找到合适的空闲块，尝试扩展堆 */
        uint64_t expand_end = (uint64_t)current + total;
        if (expand_end > heap_start + heap_max_size) {
            return NULL; /* 堆已满 */
        }

        if (!heap_expand_to(expand_end)) {
            return NULL;
        }

        /* 扩展成功，current 就是我们需要的块 */
        best = current;
        block_set(best, total, false);
    }

    uint64_t best_size = block_size(best);

    /* 如果剩余空间足够大，分裂块 */
    if (best_size >= total + BLOCK_MIN_SIZE) {
        uint64_t *remaining = (uint64_t *)((uint8_t *)best + total);
        block_set(remaining, best_size - total, false);

        block_set(best, total, true);
        heap_used_size += total;
    } else {
        /* 不分裂，整块分配 */
        block_set(best, best_size, true);
        heap_used_size += best_size;
    }

    stat_total_allocs++;
    return (void *)(best + 2); /* 跳过 16 字节头部 */
}

void kfree(void *ptr)
{
    if (!ptr) return;

    uint64_t *header = (uint64_t *)ptr - 2;

    if ((uint64_t)header < heap_start ||
        (uint64_t)header >= heap_start + heap_mapped_size) {
        return; /* 无效指针 */
    }

    if (!block_is_allocated(header)) {
        return; /* 双重释放 */
    }

    uint64_t size = block_size(header);
    block_set(header, size, false);
    heap_used_size -= size;
    stat_total_frees++;

    /* 合并相邻的空闲块 */
    uint64_t *next = block_next(header);
    uint64_t heap_end = heap_start + heap_mapped_size;

    if (next && (uint64_t)next < heap_end && !block_is_allocated(next)) {
        /* 合并当前块和下一个块 */
        uint64_t next_size = block_size(next);
        block_set(header, size + next_size, false);
    }
}

void *krealloc(void *ptr, size_t size)
{
    if (!ptr) return kmalloc(size);
    if (size == 0) { kfree(ptr); return NULL; }

    uint64_t *header = (uint64_t *)ptr - 2;
    uint64_t old_size = block_size(header) - BLOCK_HEADER_SIZE;

    if (size <= old_size) return ptr; /* 缩小或不变 */

    /* 分配新块 */
    void *new_ptr = kmalloc(size);
    if (!new_ptr) return NULL;

    /* 复制旧数据 */
    uint8_t *src = (uint8_t *)ptr;
    uint8_t *dst = (uint8_t *)new_ptr;
    for (size_t i = 0; i < old_size; i++)
        dst[i] = src[i];

    kfree(ptr);
    return new_ptr;
}

void *kcalloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void *ptr = kmalloc(total);
    if (!ptr) return NULL;

    /* 清零 */
    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0; i < total; i++)
        p[i] = 0;

    return ptr;
}

void heap_get_stats(size_t *total, size_t *used, size_t *free)
{
    if (total) *total = heap_mapped_size;
    if (used)  *used  = heap_used_size;
    if (free)  *free  = heap_mapped_size - heap_used_size;
}
