/*
 * kernel/mm/kmalloc.c
 * -----------------------------------------------------------------------------
 * 内核堆分配器：首次适配空闲链表，相邻空闲块合并。堆区为高半区连续虚拟
 * 地址空间 [KHEAP_BASE, KHEAP_MAX)，按需通过 VMM 映射物理页。
 *
 * 关键：kheap_init 必须在任何 vmm_create_address_space() 之前调用，以便
 * 内核 PML4 中堆区对应的顶层项被建立并被后续用户地址空间共享。
 *
 * 调用关系：kmain() -> kheap_init(); 全内核 -> kmalloc()/kfree()。
 */
#include <mm/kmalloc.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <kernel/string.h>
#include <kernel/console.h>
#include <kernel/spinlock.h>
#include <kernel/ksym.h>          /* EXPORT_SYMBOL 内核符号导出 */
EXPORT_SYMBOL(kmalloc);          /* 供 .kdr 内核模块调用 */
EXPORT_SYMBOL(kfree);            /* 供 .kdr 内核模块调用 */

/* P0-3：内核堆全局锁。锁序（外层->内层）：kmalloc -> pmm -> console，
 * 即持本锁期间可再拿 pmm 锁（kheap_grow）与 console 锁（诊断打印），
 * 反向（pmm/console 持锁时拿本锁）不存在，无 ABBA 环。 */
static spinlock_t g_kheap_lock = SPINLOCK_INIT("kheap");

typedef struct block {
    uint64_t      size;     /* 可用负载字节数（不含头部） */
    uint64_t      canary;   /* 完整性哨兵：分配时写入，释放时校验（B3 项） */
    bool          free;
    struct block *next;
    struct block *prev;
} block_t;

/* 头部大小向上取整到 16，保证 kmalloc 返回值 16 字节对齐
 * （task_t.fpu_state 依赖此对齐执行 fxsave64，未对齐会 #GP）。 */
#define HDR_SIZE   ((sizeof(block_t) + 15) & ~15UL)
#define ALIGN16(x) (((x) + 15) & ~15UL)

/* 哨兵：块指针与常数异或，释放时校验可发现越界写破坏链表头 */
#define KHEAP_CANARY 0x9E3779B97F4A7C15UL
static inline uint64_t blk_canary(block_t *b)
{
    return ((uint64_t)(void *)b) ^ KHEAP_CANARY;
}

static block_t *g_head;         /* 空闲/占用块链表头 */
static uint64_t g_heap_base;    /* P0-8：随机化后的堆起点（KASLR-lite） */
static uint64_t g_heap_end;     /* 已映射堆末尾（虚拟，独占） */

/* 将堆映射扩展到至少覆盖 need_end（虚拟地址） */
static bool kheap_grow(uint64_t need_end)
{
    need_end = (need_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (need_end > KHEAP_MAX) {
        return false;
    }
    while (g_heap_end < need_end) {
        void *phys = pmm_alloc_page();
        if (!phys) {
            return false;
        }
        if (!vmm_map_page(vmm_kernel_pml4(), g_heap_end,
                          (uint64_t)phys, PTE_PRESENT | PTE_WRITE)) {
            pmm_free_page(phys);
            return false;
        }
        g_heap_end += PAGE_SIZE;
    }
    return true;
}

void kheap_init(void)
{
    /* P0-8 KASLR-lite：堆基址随机滑移 0..4095 页（0..16MB，页对齐）。
     * TSC 低位混合乘散列做熵源（无 CSPRNG 的自由环境下的务实选择）。
     * 攻击者失去「内核堆对象地址可静态预测」这一前提，堆喷/UAF 利用
     * 成本显著上升。KHEAP_MAX 上限不变（1GB 窗口远大于滑移量）。 */
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t x = ((uint64_t)hi << 32) | lo;
    x ^= x >> 33; x *= 0xFF51AFD7ED558CCDUL; x ^= x >> 33;
    g_heap_base = KHEAP_BASE + (x & 0xFFF) * PAGE_SIZE;

    g_heap_end = g_heap_base;
    /* 预留初始 64KB，并确保建立顶层 PML4 项 */
    if (!kheap_grow(g_heap_base + 0x10000)) {
        kprintf("[kheap] FATAL: cannot map initial heap\n");
        return;
    }
    g_head = (block_t *)g_heap_base;
    g_head->size = (g_heap_end - g_heap_base) - HDR_SIZE;
    g_head->free = true;
    g_head->canary = blk_canary(g_head);
    g_head->next = NULL;
    g_head->prev = NULL;
    kprintf("[kheap] heap @ %p (KASLR slide +%lu KiB), initial %u KiB\n",
            (void *)g_heap_base,
            (unsigned long)((g_heap_base - KHEAP_BASE) / 1024),
            (unsigned)((g_heap_end - g_heap_base) / 1024));
}

/* 在链表尾部追加一个新块以扩容 */
static block_t *kheap_extend(uint64_t payload)
{
    block_t *last = g_head;
    while (last->next) {
        last = last->next;
    }
    uint64_t new_block_addr = (uint64_t)last + HDR_SIZE + last->size;
    uint64_t need_end = new_block_addr + HDR_SIZE + payload;
    if (!kheap_grow(need_end)) {
        return NULL;
    }
    block_t *nb = (block_t *)new_block_addr;
    nb->size = (g_heap_end - new_block_addr) - HDR_SIZE;
    nb->free = true;
    nb->canary = blk_canary(nb);
    nb->next = NULL;
    nb->prev = last;
    last->next = nb;
    return nb;
}

static void split_block(block_t *b, uint64_t want)
{
    /* 若剩余空间足够放下新头部 + 至少 16 字节，则拆分 */
    if (b->size >= want + HDR_SIZE + 16) {
        block_t *nb = (block_t *)((uint64_t)b + HDR_SIZE + want);
        nb->size = b->size - want - HDR_SIZE;
        nb->free = true;
        nb->canary = blk_canary(nb);
        nb->next = b->next;
        nb->prev = b;
        if (b->next) {
            b->next->prev = nb;
        }
        b->next = nb;
        b->size = want;
    }
    b->canary = blk_canary(b);   /* 更新本块哨兵（size 已变） */
}

void *kmalloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }
    uint64_t want = ALIGN16(size);
    uint64_t irqf = spin_lock_irqsave(&g_kheap_lock);

    for (block_t *b = g_head; b; b = b->next) {
        if (b->free && b->size >= want) {
            split_block(b, want);
            b->free = false;
            spin_unlock_irqrestore(&g_kheap_lock, irqf);
            return (void *)((uint64_t)b + HDR_SIZE);
        }
    }
    /* 无合适块，扩容 */
    block_t *nb = kheap_extend(want);
    if (!nb) {
        spin_unlock_irqrestore(&g_kheap_lock, irqf);
        return NULL;
    }
    split_block(nb, want);
    nb->free = false;
    spin_unlock_irqrestore(&g_kheap_lock, irqf);
    return (void *)((uint64_t)nb + HDR_SIZE);
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p) {
        memset(p, 0, size);
    }
    return p;
}

void kfree(void *ptr)
{
    if (!ptr) {
        return;
    }
    block_t *b = (block_t *)((uint64_t)ptr - HDR_SIZE);
    uint64_t irqf = spin_lock_irqsave(&g_kheap_lock);
    /* B3 项：校验哨兵，发现堆头被越界写破坏则停止，避免链表进一步崩坏 */
    if (b->canary != blk_canary(b)) {
        spin_unlock_irqrestore(&g_kheap_lock, irqf);
        kprintf("[kheap] CORRUPTION: bad canary at %p, aborting kfree\n", (void *)b);
        return;
    }
    /* M1 修复：双重释放防护。原代码仅校验 canary（防堆头被覆写），但未检测
     * 块是否已进入 free 状态；重复 kfree 会让下方合并逻辑再次执行，破坏链表
     * next/prev 指针（释放后哨兵虽在，但 free 标志已被置位）。此处显式拒绝
     * 对已释放块的二次释放。 */
    if (b->free) {
        spin_unlock_irqrestore(&g_kheap_lock, irqf);
        kprintf("[kheap] DOUBLE FREE at %p, ignoring\n", (void *)b);
        return;
    }
    b->free = true;

    /* 与后继合并 */
    if (b->next && b->next->free &&
        (uint64_t)b->next == (uint64_t)b + HDR_SIZE + b->size) {
        if (b->next->canary != blk_canary(b->next)) {
            spin_unlock_irqrestore(&g_kheap_lock, irqf);
            kprintf("[kheap] CORRUPTION: bad canary in next block\n");
            return;
        }
        b->size += HDR_SIZE + b->next->size;
        b->next = b->next->next;
        if (b->next) {
            b->next->prev = b;
        }
        b->canary = blk_canary(b);
    }
    /* 与前驱合并 */
    if (b->prev && b->prev->free &&
        (uint64_t)b == (uint64_t)b->prev + HDR_SIZE + b->prev->size) {
        b->prev->size += HDR_SIZE + b->size;
        b->prev->next = b->next;
        if (b->next) {
            b->next->prev = b->prev;
        }
        b->prev->canary = blk_canary(b->prev);
    }
    spin_unlock_irqrestore(&g_kheap_lock, irqf);
}

/*
 * krealloc：重新分配内核堆块（供内核能力 miniz 等使用）。
 * - ptr==NULL 等价 kmalloc(size)；size==0 等价 kfree(ptr) 并返回 NULL。
 * - 缩小时原地返回；扩大时新分配 + 拷贝旧负载 + 释放旧块。
 * 不持 g_kheap_lock 调用 kmalloc/kfree（二者内部各自加锁），避免嵌套死锁。
 */
void *krealloc(void *ptr, size_t size)
{
    if (!ptr) {
        return kmalloc(size);
    }
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }
    block_t *b = (block_t *)((uint64_t)ptr - HDR_SIZE);
    uint64_t old = b->size;              /* 旧负载字节数（块头记录，读取合法） */
    if (size <= old) {
        return ptr;                      /* 原地复用，无需搬移 */
    }
    void *np = kmalloc(size);
    if (!np) {
        return NULL;
    }
    uint8_t *s = (uint8_t *)ptr, *d = (uint8_t *)np;
    for (uint64_t i = 0; i < old; i++) {
        d[i] = s[i];
    }
    kfree(ptr);
    return np;
}
