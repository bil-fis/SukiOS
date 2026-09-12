/*
 * kernel/mm/pmm.c
 * -----------------------------------------------------------------------------
 * 位图式物理内存管理器。
 *
 * 初始化流程：
 *   1. 由 Multiboot2 内存映射得出最高物理地址 -> 计算总页数。
 *   2. 在内核末尾之后放置位图与引用计数数组（物理连续）。
 *   3. 先将全部页标记为"已用"，再依据 mmap 的可用区域(type=1)标记为"空闲"。
 *   4. 重新占用：低端 1MB、内核映像、PMM 元数据区。
 *
 * 调用关系：kmain() -> pmm_init(); vmm/kmalloc -> pmm_alloc_page()。
 */
#include <mm/pmm.h>
#include <kernel/string.h>
#include <kernel/spinlock.h>
#include <kernel/console.h>

/* P0-3（销 H8/M2 单核假设）：位图与引用计数的全部读改写路径由自旋锁
 * 串行化。锁序：pmm < console（pmm 持锁期间允许 kprintf）。 */
static spinlock_t g_pmm_lock = SPINLOCK_INIT("pmm");

extern char __kernel_phys_end[];   /* 链接脚本：内核物理结束地址 */

static uint8_t  *g_bitmap;          /* 虚拟指针：每 bit 一页 */
static uint32_t *g_refcount;        /* 每页引用计数 */
static uint64_t  g_total_pages;
static uint64_t  g_used_pages;
static uint64_t  g_meta_end_phys;   /* PMM 元数据物理结束（含内核） */

#define BIT_IDX(pg)   ((pg) >> 3)
#define BIT_OFF(pg)   ((pg) & 7)

/* M2 审计：引用计数饱和哨兵。计数一旦到达该值即"永久钉住"（pinned）：
 * 封顶时刻曾有 incref 未被计入，意味着存在未计数的持有者；此后任何 decref
 * 都不得再递减（否则计数会先于真实持有者归零，页被提前释放 → use-after-free）。
 * 粘滞语义 = 宁可泄漏 1 页，绝不悬空引用。正常值域 [0, SATURATED-1]。 */
#define PMM_REF_SATURATED  0xFFFFFFFFu

static inline void bm_set(uint64_t pg)   { g_bitmap[BIT_IDX(pg)] |=  (1u << BIT_OFF(pg)); }
static inline void bm_clear(uint64_t pg) { g_bitmap[BIT_IDX(pg)] &= ~(1u << BIT_OFF(pg)); }
static inline bool bm_test(uint64_t pg)  { return g_bitmap[BIT_IDX(pg)] & (1u << BIT_OFF(pg)); }

static uint64_t align_up(uint64_t x, uint64_t a) { return (x + a - 1) & ~(a - 1); }

void pmm_init(const boot_info_t *bi)
{
    uint64_t highest = bi->mem_highest;
    if (highest == 0) {
        highest = 0x8000000;   /* 回退 128MB */
    }
    g_total_pages = highest / PAGE_SIZE;

    uint64_t bitmap_bytes   = align_up(g_total_pages / 8, 8);
    uint64_t refcount_bytes = g_total_pages * sizeof(uint32_t);

    /* 元数据放在内核末尾之后（物理连续，均 < 4GB，可经 PHYS_TO_VIRT 访问） */
    uint64_t meta_phys = align_up((uint64_t)__kernel_phys_end, PAGE_SIZE);
    g_bitmap   = (uint8_t  *)PHYS_TO_VIRT(meta_phys);
    g_refcount = (uint32_t *)PHYS_TO_VIRT(meta_phys + bitmap_bytes);
    g_meta_end_phys = align_up(meta_phys + bitmap_bytes + refcount_bytes, PAGE_SIZE);

    /* 全部标记为已用 */
    memset(g_bitmap, 0xFF, bitmap_bytes);
    memset(g_refcount, 0, refcount_bytes);
    g_used_pages = g_total_pages;

    /* 依据 mmap 释放可用 RAM */
    if (bi->mmap) {
        const struct mb2_tag_mmap *mm = bi->mmap;
        uint32_t esz = mm->entry_size;
        const uint8_t *e  = (const uint8_t *)mm->entries;
        const uint8_t *me = (const uint8_t *)mm + mm->size;
        while (e + esz <= me) {
            const struct mb2_mmap_entry *ent = (const struct mb2_mmap_entry *)e;
            if (ent->type == 1) {   /* 可用 */
                uint64_t start = align_up(ent->addr, PAGE_SIZE);
                uint64_t end   = (ent->addr + ent->len) & ~(PAGE_SIZE - 1);
                for (uint64_t a = start; a < end; a += PAGE_SIZE) {
                    uint64_t pg = a / PAGE_SIZE;
                    if (pg < g_total_pages && bm_test(pg)) {
                        bm_clear(pg);
                        g_used_pages--;
                    }
                }
            }
            e += esz;
        }
    }

    /* 重新占用低端 1MB + 内核 + PMM 元数据区 [0, g_meta_end_phys) */
    for (uint64_t a = 0; a < g_meta_end_phys; a += PAGE_SIZE) {
        uint64_t pg = a / PAGE_SIZE;
        if (pg < g_total_pages && !bm_test(pg)) {
            bm_set(pg);
            g_used_pages++;
        }
    }

    /* 保留引导模块（GRUB 加载的内核模块/配置）占用的物理页，防止被 PMM 回收
     * 而覆盖其内容（kdr 加载器、display 配置解析都需长期读取原物理页）。
     * 模块页一旦标为已用即永不归还，与「模块常驻内核」语义一致。 */
    for (int i = 0; i < bi->nmods; i++) {
        uint64_t p = bi->mods[i].phys;
        uint64_t s = bi->mods[i].size;
        for (uint64_t a = p & ~(PAGE_SIZE - 1); a < p + s; a += PAGE_SIZE) {
            uint64_t pg = a / PAGE_SIZE;
            if (pg < g_total_pages && !bm_test(pg)) {
                bm_set(pg);
                g_used_pages++;
            }
        }
    }

    kprintf("[pmm] total=%u MiB, pages=%lu, used=%lu, free=%lu, meta_end=%p\n",
            (unsigned)(g_total_pages * PAGE_SIZE / (1024 * 1024)),
            (unsigned long)g_total_pages,
            (unsigned long)g_used_pages,
            (unsigned long)(g_total_pages - g_used_pages),
            (void *)g_meta_end_phys);
}

void *pmm_alloc_page(void)
{
    uint64_t f = spin_lock_irqsave(&g_pmm_lock);
    for (uint64_t pg = g_meta_end_phys / PAGE_SIZE; pg < g_total_pages; pg++) {
        if (!bm_test(pg)) {
            bm_set(pg);
            g_used_pages++;
            g_refcount[pg] = 1;
            void *phys = (void *)(pg * PAGE_SIZE);
            spin_unlock_irqrestore(&g_pmm_lock, f);
            /* 清零新页（经高半区映射访问；页已归本调用方独占，锁外安全） */
            memset(PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
            return phys;
        }
    }
    spin_unlock_irqrestore(&g_pmm_lock, f);
    return NULL;   /* 内存耗尽 */
}

/* P0-8 KPTI：连续对齐多页分配。位图线性扫描：从元数据区之后第一个满足
 * align_pages 对齐的页帧起，检查 count 个连续空闲页；命中即整体标记已用
 * （每页 refcount=1），锁外统一清零。失败返回 NULL。
 * 复杂度 O(total_pages)（与 pmm_alloc_page 同阶）；本函数仅在创建用户
 * 地址空间时调用（count=2），非热路径。 */
void *pmm_alloc_pages_aligned(size_t count, size_t align_pages)
{
    if (count == 0 || align_pages == 0 ||
        (align_pages & (align_pages - 1)) != 0) {
        return NULL;
    }
    uint64_t f = spin_lock_irqsave(&g_pmm_lock);
    uint64_t start = (g_meta_end_phys / PAGE_SIZE + align_pages - 1)
                     & ~((uint64_t)align_pages - 1);
    for (uint64_t pg = start; pg + count <= g_total_pages; pg += align_pages) {
        bool ok = true;
        for (size_t k = 0; k < count; k++) {
            if (bm_test(pg + k)) { ok = false; break; }
        }
        if (!ok) {
            continue;
        }
        for (size_t k = 0; k < count; k++) {
            bm_set(pg + k);
            g_refcount[pg + k] = 1;
        }
        g_used_pages += count;
        void *phys = (void *)(pg * PAGE_SIZE);
        spin_unlock_irqrestore(&g_pmm_lock, f);
        memset(PHYS_TO_VIRT(phys), 0, count * PAGE_SIZE);
        return phys;
    }
    spin_unlock_irqrestore(&g_pmm_lock, f);
    return NULL;
}

void pmm_free_page(void *phys_addr)
{
    uint64_t pg = (uint64_t)phys_addr / PAGE_SIZE;
    uint64_t f = spin_lock_irqsave(&g_pmm_lock);
    if (pg >= g_total_pages || !bm_test(pg)) {
        spin_unlock_irqrestore(&g_pmm_lock, f);
        return;
    }
    /* M2 审计修复：共享页直接释放守卫。此前 pmm_free_page 无视引用计数直接
     * 清零释放——若某路径对 refcount>1 的 OOL 共享页误调 free_page（而非
     * decref），其它持有者的映射立即指向已释放页。此处把误用降级为 decref
     * 语义：仅释放一份引用并告警，页保留给其余持有者；饱和页粘滞不递减。 */
    if (g_refcount[pg] > 1) {
        kprintf("[pmm] free_page on SHARED page %lu (ref=%u), demoted to decref\n",
                (unsigned long)pg, (unsigned)g_refcount[pg]);
        if (g_refcount[pg] != PMM_REF_SATURATED) {
            g_refcount[pg]--;
        }
        spin_unlock_irqrestore(&g_pmm_lock, f);
        return;
    }
    bm_clear(pg);
    g_refcount[pg] = 0;
    g_used_pages--;
    spin_unlock_irqrestore(&g_pmm_lock, f);
}

void pmm_incref(void *phys_addr)
{
    uint64_t pg = (uint64_t)phys_addr / PAGE_SIZE;
    uint64_t f = spin_lock_irqsave(&g_pmm_lock);
    if (pg < g_total_pages) {
        /* M2 修复：引用计数溢出防护。OOL 共享页的 refcount 若被无限 incref
         * 会回绕为 0，使释放逻辑误判页已无引用而提前回收（被他任务仍持有的
         * 共享页遭破坏）。此处封顶 PMM_REF_SATURATED，到顶即进入永久钉住态
         * （见 pmm_decref 的粘滞语义）并告警。
         * （pmm_alloc_page/位图非原子问题随 P0-3 锁体系销账。） */
        if (g_refcount[pg] < PMM_REF_SATURATED) {
            g_refcount[pg]++;
            if (g_refcount[pg] == PMM_REF_SATURATED) {
                kprintf("[pmm] refcount SATURATED at page %lu, pinned forever\n",
                        (unsigned long)pg);
            }
        }
    }
    spin_unlock_irqrestore(&g_pmm_lock, f);
}

uint64_t pmm_decref(void *phys_addr)
{
    uint64_t pg = (uint64_t)phys_addr / PAGE_SIZE;
    uint64_t f = spin_lock_irqsave(&g_pmm_lock);
    if (pg >= g_total_pages || g_refcount[pg] == 0) {
        spin_unlock_irqrestore(&g_pmm_lock, f);
        return 0;
    }
    /* M2 审计修复（饱和粘滞）：计数曾封顶意味着有 incref 未被计入——真实
     * 持有者数 >= 计数值。若此处照常递减，计数会先于真实持有者归零并释放页，
     * 造成 use-after-free。故饱和页永不递减、永不释放（钉住，宁泄漏不悬空）。 */
    if (g_refcount[pg] == PMM_REF_SATURATED) {
        spin_unlock_irqrestore(&g_pmm_lock, f);
        return PMM_REF_SATURATED;
    }
    g_refcount[pg]--;
    uint64_t rem = g_refcount[pg];
    if (rem == 0) {
        /* 直接走位图释放（不再递归经 pmm_free_page 的共享页守卫，此时
         * ref 已为 0，语义就是终局释放）。 */
        bm_clear(pg);
        g_used_pages--;
    }
    spin_unlock_irqrestore(&g_pmm_lock, f);
    return rem;
}

uint64_t pmm_refcount(void *phys_addr)
{
    uint64_t pg = (uint64_t)phys_addr / PAGE_SIZE;
    return (pg < g_total_pages) ? g_refcount[pg] : 0;
}

uint64_t pmm_total_pages(void) { return g_total_pages; }
uint64_t pmm_used_pages(void)  { return g_used_pages; }
uint64_t pmm_free_pages(void)  { return g_total_pages - g_used_pages; }

/*
 * M2 审计压测自检（boot 时由 mm_selftest 调用）。
 * 置于 pmm.c 内部：仅此处可直接操纵 g_refcount 构造饱和态（真实到达饱和需
 * 2^32 次 incref，QEMU 下不可行）。三组用例：
 *   T1 OOL 大规模共享压测：32 页 × 64 持有者 incref/decref 全循环，
 *      模拟 64 个接收方共享 32 页 OOL 缓冲的极端场景，验证计数与空闲页守恒。
 *   T2 饱和粘滞：人工置 SATURATED-1 后 incref 到顶，验证 decref 不递减、
 *      free_page 不释放（钉住语义），最后人工恢复计数并释放。
 *   T3 共享页守卫：ref=2 的页误调 pmm_free_page，验证被降级为 decref
 *      （页保留），再次 free 才真正释放。
 * 全部通过打印 PASS，任一失败打印 FAIL（不 panic，保留现场日志）。
 */
void pmm_selftest(void)
{
    uint64_t free0 = pmm_free_pages();
    bool ok = true;

    /* T1：OOL 大规模共享压测（32 页 × 64 持有者 × 8 轮 = 16384 次计数操作） */
    void *pages[32];
    for (int i = 0; i < 32; i++) {
        pages[i] = pmm_alloc_page();
        if (!pages[i]) { ok = false; }
    }
    for (int round = 0; ok && round < 8; round++) {
        for (int i = 0; i < 32; i++) {
            for (int h = 0; h < 64; h++) pmm_incref(pages[i]);   /* 64 接收方 */
        }
        for (int i = 0; i < 32; i++) {
            for (int h = 0; h < 64; h++) pmm_decref(pages[i]);   /* 逐个退出 */
        }
    }
    for (int i = 0; ok && i < 32; i++) {
        if (pmm_refcount(pages[i]) != 1) {
            kprintf("[pmm] T1 FAIL: page %d refcount=%u (expect 1)\n",
                    i, (unsigned)pmm_refcount(pages[i]));
            ok = false;
        }
    }
    for (int i = 0; i < 32; i++) {
        if (pages[i]) pmm_decref(pages[i]);       /* ref 1->0，终局释放 */
    }
    if (ok && pmm_free_pages() != free0) {
        kprintf("[pmm] T1 FAIL: free pages leak (%lu -> %lu)\n",
                (unsigned long)free0, (unsigned long)pmm_free_pages());
        ok = false;
    }

    /* T2：饱和粘滞（人工构造，绕过 2^32 次 incref） */
    void *sp = pmm_alloc_page();
    if (sp) {
        uint64_t spg = (uint64_t)sp / PAGE_SIZE;
        g_refcount[spg] = PMM_REF_SATURATED - 1;
        pmm_incref(sp);                            /* 到顶，应打印 pinned 告警 */
        if (g_refcount[spg] != PMM_REF_SATURATED) {
            kprintf("[pmm] T2 FAIL: incref did not saturate\n");
            ok = false;
        }
        if (pmm_decref(sp) != PMM_REF_SATURATED ||
            g_refcount[spg] != PMM_REF_SATURATED) {
            kprintf("[pmm] T2 FAIL: decref moved a SATURATED count\n");
            ok = false;
        }
        pmm_free_page(sp);                         /* 应被守卫拦截（粘滞不减） */
        if (!bm_test(spg) || g_refcount[spg] != PMM_REF_SATURATED) {
            kprintf("[pmm] T2 FAIL: free_page released a pinned page\n");
            ok = false;
        }
        g_refcount[spg] = 1;                       /* 人工解除钉住，归还页 */
        pmm_free_page(sp);
    }

    /* T3：共享页守卫（ref=2 误调 free_page 应降级为 decref） */
    void *gp = pmm_alloc_page();
    if (gp) {
        uint64_t gpg = (uint64_t)gp / PAGE_SIZE;
        pmm_incref(gp);                            /* ref = 2 */
        pmm_free_page(gp);                         /* 应降级：ref 2->1，页保留 */
        if (!bm_test(gpg) || g_refcount[gpg] != 1) {
            kprintf("[pmm] T3 FAIL: shared-page guard did not demote\n");
            ok = false;
        }
        pmm_free_page(gp);                         /* ref==1，真正释放 */
    }

    if (pmm_free_pages() != free0) {
        kprintf("[pmm] selftest FAIL: free pages %lu -> %lu\n",
                (unsigned long)free0, (unsigned long)pmm_free_pages());
        ok = false;
    }
    kprintf("[pmm] refcount selftest (T1 stress/T2 saturate/T3 guard): %s\n",
            ok ? "PASS" : "FAIL");
}
