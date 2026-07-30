/*
 * kernel/mm/kstack.c
 * -----------------------------------------------------------------------------
 * P0-R5：带未映射守卫页的内核栈分配器（设计见 include/mm/kstack.h）。
 *
 * 并发：位图与映射操作由自旋锁（关中断版）保护——task_create 可能并发于
 * 多核，reap 路径也会在持有调度锁之外调用 kstack_free。
 *
 * 守卫页命中的两种表现（均可被诊断层定性）：
 *   1) 数据访问越过栈底（如大局部数组写穿）→ 普通 #PF，CR2 ∈ 守卫页；
 *   2) push/call 把 RSP 推进守卫页 → #PF 帧本身无处可压 → #DF（IST1 栈），
 *      此时 CR2 仍保留首次故障地址，同样落在守卫区间。
 * idt.c 的 #PF/#DF 路径调用 kstack_guard_hit(CR2) 输出「stack overflow」。
 */
#include <mm/kstack.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <kernel/spinlock.h>
#include <kernel/console.h>

static uint64_t   g_slot_bitmap[KSTACK_MAX_SLOTS / 64];  /* 1=在用 */
static spinlock_t g_kstack_lock;
static bool       g_kstack_ready;

static void kstack_lazy_init(void)
{
    if (!g_kstack_ready) {
        spinlock_init(&g_kstack_lock, "kstack");
        g_kstack_ready = true;
    }
}

static uint64_t slot_va(size_t idx)
{
    return KSTACK_AREA_BASE + (uint64_t)idx * KSTACK_SLOT_BYTES;
}

uint64_t kstack_alloc(void)
{
    kstack_lazy_init();

    uint64_t fl = spin_lock_irqsave(&g_kstack_lock);
    size_t idx = (size_t)-1;
    for (size_t w = 0; w < KSTACK_MAX_SLOTS / 64; w++) {
        if (g_slot_bitmap[w] != ~0UL) {
            uint64_t inv = ~g_slot_bitmap[w];
            size_t bit = (size_t)__builtin_ctzll(inv);
            g_slot_bitmap[w] |= (1UL << bit);
            idx = w * 64 + bit;
            break;
        }
    }
    spin_unlock_irqrestore(&g_kstack_lock, fl);

    if (idx == (size_t)-1) {
        kprintf("[kstack] slot exhausted (%u in use)\n",
                (unsigned)KSTACK_MAX_SLOTS);
        return 0;
    }

    /* 槽内第 0 页为守卫页（保持未映射），第 1..4 页映射为 RW+NX 栈体 */
    uint64_t base = slot_va(idx) + (uint64_t)KSTACK_GUARD_PAGES * PAGE_SIZE;
    for (size_t p = 0; p < KSTACK_PAGES; p++) {
        void *phys = pmm_alloc_page();
        if (!phys) {
            /* 回滚已映射页并归还槽位 */
            for (size_t q = 0; q < p; q++) {
                uint64_t va = base + q * PAGE_SIZE;
                uint64_t pa = vmm_translate(vmm_kernel_pml4(), va);
                vmm_unmap_page(vmm_kernel_pml4(), va);
                if (pa) pmm_free_page((void *)pa);
            }
            fl = spin_lock_irqsave(&g_kstack_lock);
            g_slot_bitmap[idx / 64] &= ~(1UL << (idx % 64));
            spin_unlock_irqrestore(&g_kstack_lock, fl);
            return 0;
        }
        if (!vmm_map_page(vmm_kernel_pml4(), base + p * PAGE_SIZE,
                          (uint64_t)phys, PTE_WRITE | PTE_NX)) {
            pmm_free_page(phys);
            for (size_t q = 0; q < p; q++) {
                uint64_t va = base + q * PAGE_SIZE;
                uint64_t pa = vmm_translate(vmm_kernel_pml4(), va);
                vmm_unmap_page(vmm_kernel_pml4(), va);
                if (pa) pmm_free_page((void *)pa);
            }
            fl = spin_lock_irqsave(&g_kstack_lock);
            g_slot_bitmap[idx / 64] &= ~(1UL << (idx % 64));
            spin_unlock_irqrestore(&g_kstack_lock, fl);
            return 0;
        }
    }
    return base;
}

void kstack_free(uint64_t kstack_base)
{
    if (kstack_base < KSTACK_AREA_BASE || kstack_base >= KSTACK_AREA_END) {
        kprintf("[kstack] free of non-kstack VA %p ignored\n",
                (void *)kstack_base);
        return;
    }
    size_t idx = (size_t)((kstack_base - KSTACK_AREA_BASE) / KSTACK_SLOT_BYTES);
    uint64_t base = slot_va(idx) + (uint64_t)KSTACK_GUARD_PAGES * PAGE_SIZE;

    for (size_t p = 0; p < KSTACK_PAGES; p++) {
        uint64_t va = base + p * PAGE_SIZE;
        uint64_t pa = vmm_translate(vmm_kernel_pml4(), va);
        vmm_unmap_page(vmm_kernel_pml4(), va);   /* 内核 VA：自动 TLB shootdown */
        if (pa) {
            pmm_free_page((void *)pa);
        }
    }

    uint64_t fl = spin_lock_irqsave(&g_kstack_lock);
    g_slot_bitmap[idx / 64] &= ~(1UL << (idx % 64));
    spin_unlock_irqrestore(&g_kstack_lock, fl);
}

bool kstack_guard_hit(uint64_t addr)
{
    if (addr < KSTACK_AREA_BASE || addr >= KSTACK_AREA_END) {
        return false;
    }
    size_t idx = (size_t)((addr - KSTACK_AREA_BASE) / KSTACK_SLOT_BYTES);
    uint64_t off = (addr - KSTACK_AREA_BASE) % KSTACK_SLOT_BYTES;
    if (off >= (uint64_t)KSTACK_GUARD_PAGES * PAGE_SIZE) {
        return false;                 /* 落在栈体（已映射），不是守卫页 */
    }
    /* 仅对「在用」槽位报告；空闲槽的整个区间本就未映射 */
    return (g_slot_bitmap[idx / 64] >> (idx % 64)) & 1;
}
