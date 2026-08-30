/*
 * user/lib/stdlib.c
 * 动态内存分配（基于内核 sys_brk 的堆区）+ 进程退出。
 *
 * 分配器设计（first-fit + 立即合并，真正可用，非 stub）：
 *   - 堆底由 sys_brk(0) 取得（内核已登记整块匿名 VMA，按需补页）。
 *   - 每个块头部 16 字节：{ size_t total; size_t free; }（total 含头部，
 *     free 为 1 表示该块空闲）。空闲块以单向链表串接（free 链）。
 *   - malloc：先扫描空闲链找 first-fit；找不到则向内核扩展 brk 开辟新块。
 *   - free：标记空闲，并与相邻（前向/后向）空闲块合并，避免碎片累积。
 */
#include "libc.h"

typedef struct block {
    size_t total;          /* 整块字节数（含头部） */
    size_t free;           /* 1 = 空闲，0 = 占用 */
    struct block *next;    /* 空闲链 next（仅空闲时有意义） */
} block_t;

#define BLK_HDR  (sizeof(block_t))
#define ALIGN8(n) (((n) + 7) & ~(size_t)7)
#define HEAP_PAGE 4096UL   /* 与内核页大小一致，仅用于初始化时确保基址页映射 */

static block_t *g_heap_base = NULL;   /* 第一个块 */
static block_t *g_free_head = NULL;   /* 空闲链头 */
static uint8_t  g_heap_init = 0;

static void heap_init(void)
{
    /* 取得当前 brk 作为堆底。注意：本进程之前可能已被其它代码（如测试
     * 套件的 sbrk 收缩）把堆基址所在的页 unmap 过，因此 base 这一页此刻
     * 未必在 VMA 内。必须先把 brk 向前扩展至少一页，让内核重新把
     * [base, base+PAGE) 纳入堆 VMA 并允许首次触碰补页，否则下面写
     * g_heap_base->total 会触发 #PF 杀进程。 */
    uintptr_t base = (uintptr_t)suki_syscall1(SYS_BRK, 0);
    if (!base) return;
    uintptr_t first_top = (base + HEAP_PAGE);
    uintptr_t got = (uintptr_t)suki_syscall1(SYS_BRK, first_top);
    if (got != first_top) return;   /* 内核拒绝扩展（OOM）：堆不可用 */
    g_heap_base = (block_t *)base;
    /* 第一个块覆盖整个 [base, base+PAGE)，全部空闲，供后续切分/合并 */
    g_heap_base->total = (size_t)(first_top - base);
    g_heap_base->free  = 1;
    g_heap_base->next  = NULL;
    g_free_head = g_heap_base;
    g_heap_init = 1;
}

/* 向内核申请至少 need 字节的新堆块，返回新块指针（已设为占用） */
static block_t *heap_extend(size_t need)
{
    uintptr_t cur = (uintptr_t)suki_syscall1(SYS_BRK, 0);
    size_t want = ALIGN8(need);
    uintptr_t new_brk = cur + want;
    uintptr_t got = (uintptr_t)suki_syscall1(SYS_BRK, new_brk);
    if (got != new_brk) {
        return NULL;    /* 内核拒绝扩展（OOM） */
    }
    block_t *b = (block_t *)cur;
    b->total = want;
    b->free  = 0;
    b->next  = NULL;
    return b;
}

void *malloc(size_t size)
{
    if (!size) return NULL;
    if (!g_heap_init) heap_init();
    if (!g_heap_init) return NULL;

    size_t need = ALIGN8(size) + BLK_HDR;

    /* first-fit 扫描空闲链 */
    block_t *prev = NULL;
    for (block_t *b = g_free_head; b; prev = b, b = b->next) {
        if (b->free && b->total >= need) {
            if (b->total >= need + BLK_HDR + 16) {
                /* 切分：剩余部分作为新空闲块放入空闲链 */
                block_t *split = (block_t *)((uintptr_t)b + need);
                split->total = b->total - need;
                split->free  = 1;
                split->next  = b->next;
                b->total = need;
                b->next = split;
            }
            b->free = 0;
            /* 从空闲链摘除（仅当 b 是链头时才需更新 head；切分后可能影响，统一处理） */
            if (prev) prev->next = b->next;
            else g_free_head = b->next;
            b->next = NULL;
            return (void *)((uintptr_t)b + BLK_HDR);
        }
    }

    /* 无合适空闲块：扩展堆 */
    block_t *nb = heap_extend(need);
    if (!nb) { errno = ENOMEM; return NULL; }
    return (void *)((uintptr_t)nb + BLK_HDR);
}

void free(void *ptr)
{
    if (!ptr) return;
    block_t *b = (block_t *)((uintptr_t)ptr - BLK_HDR);
    b->free = 1;
    b->next = NULL;

    uintptr_t brk = (uintptr_t)suki_syscall1(SYS_BRK, 0);

    /* 线性扫描整堆，合并相邻空闲块（前后向均处理） */
    block_t *cur = g_heap_base;
    while ((uintptr_t)cur < brk && cur->total >= BLK_HDR) {
        if (cur->free) {
            block_t *nxt = (block_t *)((uintptr_t)cur + cur->total);
            while ((uintptr_t)nxt > (uintptr_t)cur &&
                   (uintptr_t)nxt < brk &&
                   nxt->total >= BLK_HDR && nxt->free) {
                cur->total += nxt->total;
                nxt = (block_t *)((uintptr_t)cur + cur->total);
            }
        }
        cur = (block_t *)((uintptr_t)cur + cur->total);
    }

    /* 重建空闲链（单向，首插） */
    g_free_head = NULL;
    cur = g_heap_base;
    while ((uintptr_t)cur < brk && cur->total >= BLK_HDR) {
        if (cur->free) {
            cur->next = g_free_head;
            g_free_head = cur;
        } else {
            cur->next = NULL;
        }
        cur = (block_t *)((uintptr_t)cur + cur->total);
    }
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    if (nmemb && size && total / nmemb != size) { errno = ENOMEM; return NULL; }
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr) return malloc(size);
    if (!size) { free(ptr); return NULL; }
    block_t *b = (block_t *)((uintptr_t)ptr - BLK_HDR);
    size_t old_usable = b->total - BLK_HDR;
    if (old_usable >= size) return ptr;
    void *np = malloc(size);
    if (!np) return NULL;
    memcpy(np, ptr, old_usable);
    free(ptr);
    return np;
}

void exit(int code)
{
    suki_syscall1(SYS_EXIT_GROUP, (uint64_t)code);
    for (;;) { }
}

void _exit(int code)
{
    suki_syscall1(SYS_EXIT_GROUP, (uint64_t)code);
    for (;;) { }
}

void abort(void)
{
    suki_syscall1(SYS_EXIT_GROUP, 134);
    for (;;) { }
}
