#include "kernel/heap.h"
#include "kernel/vmm.h"
#include "kernel/pmm.h"
#include "kernel/tty.h"

/* ================= 内部常量 ================= */
#define MAGIC_VALUE        0xBEEFCAFE20161220ULL
#define BLOCK_HEADER_SIZE  32       /* size + prev_free + next_free + magic */
#define BLOCK_FOOTER_SIZE  16       /* footer_magic + footer_size */
#define BLOCK_META_SIZE    (BLOCK_HEADER_SIZE + BLOCK_FOOTER_SIZE)
#define BLOCK_MIN_SIZE     (BLOCK_META_SIZE + 16)   /* 至少留 16 字节数据区 */
#define BLOCK_ALIGN        16

#define BLOCK_ALLOCATED    (1ULL << 63)
#define BLOCK_SIZE_MASK    ~(BLOCK_ALLOCATED)

/* ================= 全局状态 ================= */
static uint64_t heap_start;
static size_t   heap_max_size;
static size_t   heap_mapped_size;
static size_t   heap_allocated_bytes;   /* 已分配给用户的有效载荷字节总数 */

static uint64_t *free_list_head = NULL;

/* ================= 中断保护（单核版本） ================= */
static inline void lock(void)   { __asm__ volatile("cli" ::: "memory"); }
static inline void unlock(void) { __asm__ volatile("sti" ::: "memory"); }

/* ================= 辅助函数 ================= */
/* 获取/设置块大小 */
static inline uint64_t blk_size(uint64_t *hdr) {
    return hdr[0] & BLOCK_SIZE_MASK;
}
static inline void blk_set_size(uint64_t *hdr, uint64_t sz, bool alloc) {
    hdr[0] = (sz & BLOCK_SIZE_MASK) | (alloc ? BLOCK_ALLOCATED : 0);
}
static inline bool blk_is_alloc(uint64_t *hdr) {
    return (hdr[0] & BLOCK_ALLOCATED) != 0;
}

/* 空闲链表指针 */
static inline uint64_t *blk_prev_free(uint64_t *hdr) { return (uint64_t *)hdr[1]; }
static inline void blk_set_prev_free(uint64_t *hdr, uint64_t *p) { hdr[1] = (uint64_t)p; }
static inline uint64_t *blk_next_free(uint64_t *hdr) { return (uint64_t *)hdr[2]; }
static inline void blk_set_next_free(uint64_t *hdr, uint64_t *n) { hdr[2] = (uint64_t)n; }

/* magic */
static inline uint64_t blk_magic(uint64_t *hdr) { return hdr[3]; }
static inline void blk_set_magic(uint64_t *hdr) { hdr[3] = MAGIC_VALUE; }

/* 从前一块尾部获取其大小（如果存在且非分配状态） */
static inline uint64_t *blk_prev_phys(uint64_t *hdr) {
    if ((uint64_t)hdr == heap_start) return NULL;
    /* 前一块的 footer 在 hdr - 2*8 的位置 (footer_magic + footer_size) */
    uint64_t *prev_footer_size = (uint64_t *)((uint8_t *)hdr - 16);
    /* 简单验证：前一个块的大小如果异常则返回 NULL */
    uint64_t prev_sz = *prev_footer_size;
    if (prev_sz == 0 || prev_sz > heap_max_size) return NULL;
    /* 前一块头部地址 = 当前头 - prev_sz */
    return (uint64_t *)((uint8_t *)hdr - prev_sz);
}

/* 下一个物理块头部 */
static inline uint64_t *blk_next_phys(uint64_t *hdr) {
    uint64_t sz = blk_size(hdr);
    return (uint64_t *)((uint8_t *)hdr + sz);
}

/* 读取/设置 footer（位于数据区末尾） */
static inline void set_footer(uint64_t *hdr) {
    uint64_t sz = blk_size(hdr);
    uint64_t *footer_magic_ptr = (uint64_t *)((uint8_t *)hdr + sz - 8);
    uint64_t *footer_size_ptr  = (uint64_t *)((uint8_t *)hdr + sz - 16);
    *footer_magic_ptr = MAGIC_VALUE;
    *footer_size_ptr  = sz;   /* 保存完整 size，含 allocated 标志 */
}
static inline bool check_footer(uint64_t *hdr) {
    uint64_t sz = blk_size(hdr);
    uint64_t *footer_magic_ptr = (uint64_t *)((uint8_t *)hdr + sz - 8);
    uint64_t *footer_size_ptr  = (uint64_t *)((uint8_t *)hdr + sz - 16);
    /* 轻度校验：magic 必须匹配，size 必须一致 */
    return (*footer_magic_ptr == MAGIC_VALUE) && (*footer_size_ptr == hdr[0]);
}

/* ================= 空闲链表操作 ================= */
static void list_remove(uint64_t *hdr) {
    uint64_t *prev = blk_prev_free(hdr);
    uint64_t *next = blk_next_free(hdr);
    if (prev) blk_set_next_free(prev, next);
    else      free_list_head = next;
    if (next) blk_set_prev_free(next, prev);
    blk_set_prev_free(hdr, NULL);
    blk_set_next_free(hdr, NULL);
}

static void list_insert_head(uint64_t *hdr) {
    uint64_t *old = free_list_head;
    blk_set_prev_free(hdr, NULL);
    blk_set_next_free(hdr, old);
    if (old) blk_set_prev_free(old, hdr);
    free_list_head = hdr;
}

/* ================= 物理合并 ================= */
/* 与下一个物理块合并，前提是 next 未分配 */
static uint64_t *merge_next(uint64_t *hdr) {
    uint64_t sz = blk_size(hdr);
    uint64_t *next = (uint64_t *)((uint8_t *)hdr + sz);
    if ((uint64_t)next >= heap_start + heap_mapped_size) return hdr;
    if (blk_is_alloc(next)) return hdr;

    /* 从空闲链表移除 next */
    list_remove(next);
    /* 合并大小 */
    uint64_t new_sz = sz + blk_size(next);
    blk_set_size(hdr, new_sz, false);
    blk_set_magic(hdr);
    set_footer(hdr);
    return hdr;
}

/* 与前一个物理块合并（需要 footer 支持） */
static uint64_t *merge_prev(uint64_t *hdr) {
    uint64_t *prev = blk_prev_phys(hdr);
    if (!prev) return hdr;
    if (blk_is_alloc(prev)) return hdr;

    /* 从空闲链表移除 prev */
    list_remove(prev);
    uint64_t new_sz = blk_size(prev) + blk_size(hdr);
    blk_set_size(prev, new_sz, false);
    blk_set_magic(prev);
    set_footer(prev);
    return prev;  /* 返回合并后的块头部 */
}

/* ================= 堆扩展 ================= */
static bool expand_to(uint64_t needed) {
    uint64_t cur_end = heap_start + heap_mapped_size;
    while (cur_end < needed) {
        if (cur_end >= heap_start + heap_max_size) return false;
        if (vmm_alloc_map(cur_end, VMM_KERN_RW) != 0) return false;
        cur_end += VMM_PAGE_SIZE;
        heap_mapped_size += VMM_PAGE_SIZE;
    }
    return true;
}

/* ================= 初始化 ================= */
void heap_init(uint64_t start, size_t size) {
    heap_start          = start;
    heap_max_size       = size;
    heap_mapped_size    = 0;
    heap_allocated_bytes = 0;
    free_list_head      = NULL;

    /* 映射第一页 */
    if (vmm_alloc_map(start, VMM_KERN_RW) != 0) {
        tty_setcolor(VGA_RED, VGA_BLACK);
        tty_print("  HEAP: initial page map failed\n");
        return;
    }
    heap_mapped_size = VMM_PAGE_SIZE;

    /* 创建一个覆盖整页的空闲块 */
    uint64_t *block = (uint64_t *)(uintptr_t)start;
    blk_set_size(block, VMM_PAGE_SIZE, false);
    blk_set_magic(block);
    set_footer(block);
    list_insert_head(block);
}

/* ================= 分配 ================= */
void *kmalloc(size_t size) {
    if (size == 0 || size > HEAP_MAX_ALLOC) return NULL;

    uint64_t total = size + BLOCK_META_SIZE;
    /* 向上对齐到 16 字节 */
    total = (total + BLOCK_ALIGN - 1) & ~((uint64_t)BLOCK_ALIGN - 1);
    if (total < BLOCK_MIN_SIZE) total = BLOCK_MIN_SIZE;

    lock();

    /* first‑fit */
    uint64_t *best = free_list_head;
    while (best) {
        if (blk_size(best) >= total) break;
        best = blk_next_free(best);
    }

    if (!best) {
        /* 需要扩展堆 */
        uint64_t cur_end = heap_start + heap_mapped_size;
        if (!expand_to(cur_end + total)) {
            unlock();
            return NULL;
        }
        /* 新扩展出来的空间是连续的空闲区域，创建一个新块并插入 */
        uint64_t *new_block = (uint64_t *)(uintptr_t)cur_end;
        uint64_t expand_sz = (heap_start + heap_mapped_size) - cur_end;
        blk_set_size(new_block, expand_sz, false);
        blk_set_magic(new_block);
        set_footer(new_block);
        list_insert_head(new_block);
        best = free_list_head;  /* 现在链表头就是新块 */
    }

    /* 分裂 */
    uint64_t bsize = blk_size(best);
    if (bsize >= total + BLOCK_MIN_SIZE) {
        uint64_t *rest = (uint64_t *)((uint8_t *)best + total);
        /* 设置剩余块 */
        blk_set_size(rest, bsize - total, false);
        blk_set_magic(rest);
        set_footer(rest);
        /* 在链表中用 rest 替换 best */
        uint64_t *prev = blk_prev_free(best);
        uint64_t *next = blk_next_free(best);
        if (prev) blk_set_next_free(prev, rest); else free_list_head = rest;
        if (next) blk_set_prev_free(next, rest);
        blk_set_prev_free(rest, prev);
        blk_set_next_free(rest, next);
        blk_set_magic(rest);

        /* 设置分配的块 */
        blk_set_size(best, total, true);
        blk_set_magic(best);
        set_footer(best);
    } else {
        /* 整个块分配 */
        list_remove(best);
        blk_set_size(best, bsize, true);
        blk_set_magic(best);
        set_footer(best);
    }

    heap_allocated_bytes += total;
    unlock();

    return (void *)(best + 4);   /* 跳过 32 字节头部 */
}

/* ================= 释放 ================= */
void kfree(void *ptr) {
    if (!ptr) return;

    lock();

    uint64_t *hdr = (uint64_t *)ptr - 4;

    /* 地址范围 */
    if ((uint64_t)hdr < heap_start || (uint64_t)hdr >= heap_start + heap_mapped_size) {
        unlock(); return;
    }

    /* 魔数检查 */
    if (blk_magic(hdr) != MAGIC_VALUE || !check_footer(hdr)) {
        unlock(); return;
    }

    if (!blk_is_alloc(hdr)) {
        unlock(); return;   /* 双重释放 */
    }

    uint64_t sz = blk_size(hdr);
    heap_allocated_bytes -= sz;

    /* 标记为空闲 */
    blk_set_size(hdr, sz, false);
    blk_set_magic(hdr);
    set_footer(hdr);

    /* 与相邻物理块合并 */
    hdr = merge_prev(hdr);       /* 先合并前一个 */
    hdr = merge_next(hdr);       /* 再合并后一个 */

    /* 插入空闲链表 */
    list_insert_head(hdr);

    unlock();
}

/* ================= realloc / calloc ================= */
void *krealloc(void *ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    if (size == 0) { kfree(ptr); return NULL; }

    uint64_t *hdr = (uint64_t *)ptr - 4;
    if (blk_magic(hdr) != MAGIC_VALUE) return NULL;

    size_t old_user = blk_size(hdr) - BLOCK_META_SIZE;
    if (size <= old_user) return ptr;

    void *new = kmalloc(size);
    if (!new) return NULL;

    /* 复制旧数据 */
    uint8_t *s = (uint8_t *)ptr;
    uint8_t *d = (uint8_t *)new;
    for (size_t i = 0; i < old_user; ++i) d[i] = s[i];
    kfree(ptr);
    return new;
}

void *kcalloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return NULL;
    if (size > HEAP_MAX_ALLOC / nmemb) return NULL;  /* 溢出防护 */
    size_t total = nmemb * size;
    void *p = kmalloc(total);
    if (!p) return NULL;

    uint8_t *byte = (uint8_t *)p;
    for (size_t i = 0; i < total; ++i) byte[i] = 0;
    return p;
}

/* ================= 统计 ================= */
void heap_get_stats(size_t *total, size_t *used, size_t *free) {
    if (total) *total = heap_mapped_size;
    if (used)  *used  = heap_allocated_bytes;
    if (free)  *free  = heap_mapped_size - heap_allocated_bytes;
}

/* ================= 调试 ================= */
void heap_dump(void) {
    lock();
    tty_print("\n--- Heap Free List ---\n");
    uint64_t *cur = free_list_head;
    int i = 0;
    while (cur) {
        tty_print("  ["); tty_print_dec(i++);
        tty_print("] addr:0x"); tty_print_hex64((uint64_t)cur);
        tty_print(" size:"); tty_print_dec(blk_size(cur));
        tty_print("\n");
        cur = blk_next_free(cur);
    }
    tty_print("--- End of Free List ---\n");
    unlock();
}

int heap_check(void) {
    lock();
    uint64_t *prev = NULL;
    uint64_t *cur = free_list_head;
    while (cur) {
        if ((uint64_t)cur < heap_start || (uint64_t)cur >= heap_start + heap_mapped_size) {
            unlock(); tty_print("HEAP CHECK FAIL: address out of range\n"); return -1;
        }
        if (blk_magic(cur) != MAGIC_VALUE || !check_footer(cur)) {
            unlock(); tty_print("HEAP CHECK FAIL: bad magic/footer\n"); return -1;
        }
        if (blk_is_alloc(cur)) {
            unlock(); tty_print("HEAP CHECK FAIL: allocated block in free list\n"); return -1;
        }
        if (blk_prev_free(cur) != prev) {
            unlock(); tty_print("HEAP CHECK FAIL: prev pointer mismatch\n"); return -1;
        }
        prev = cur;
        cur = blk_next_free(cur);
    }
    unlock();
    return 0;
}