/*
 * kernel/mm/vma.c
 * -----------------------------------------------------------------------------
 * VMA（虚拟内存区域）管理 + 按需分页填充（见 include/mm/vma.h）。
 *
 * 调用关系：
 *   idt.c::page_fault_handler（用户缺页）-> vma_populate()
 *   syscall.c::user_access_ok（copy_*_user 预校验）-> vma_populate()
 *   syscall.c::sys_mmap/sys_munmap -> vma_insert()/vma_unmap_range()
 *   sched.c::task_create_user_args -> vma_insert()（栈自动增长区）
 *   sched.c::task_exit_current、syscall.c::sys_execve -> vma_destroy_all()
 *   kmain.c::mm_selftest -> vma_selftest()
 */
#include <mm/vma.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/kmalloc.h>
#include <kernel/task.h>
#include <kernel/syscall.h>      /* USER_SPACE_TOP */
#include <kernel/spinlock.h>
#include <kernel/console.h>
#include <kernel/string.h>

/* 全局 VMA 锁：保护所有任务的 vma_list 链表结构（读改写皆持锁）。
 * 锁序：位于 kmalloc 之外层（持本锁期间会调用 kmalloc/kfree/pmm_*）——
 * 与既有锁序（kmalloc -> pmm -> console）兼容，无 ABBA 环。 */
static spinlock_t g_vma_lock = SPINLOCK_INIT("vma");

bool vma_insert(struct task *t, uint64_t start, uint64_t end,
                uint64_t prot, uint32_t type)
{
    if (!t || start >= end || (start & (PAGE_SIZE - 1)) ||
        (end & (PAGE_SIZE - 1))) {
        return false;
    }
    vm_area_t *n = (vm_area_t *)kmalloc(sizeof(vm_area_t));
    if (!n) {
        return false;
    }
    n->start = start;
    n->end   = end;
    n->prot  = prot;
    n->type  = type;

    uint64_t f = spin_lock_irqsave(&g_vma_lock);
    /* 重叠检查 + 找升序插入点 */
    vm_area_t **pp = (vm_area_t **)&t->vma_list;
    while (*pp && (*pp)->start < start) {
        pp = &(*pp)->next;
    }
    /* 前驱不得越过 start；后继不得早于 end */
    if (*pp && (*pp)->start < end) {
        spin_unlock_irqrestore(&g_vma_lock, f);
        kfree(n);
        return false;
    }
    if (pp != (vm_area_t **)&t->vma_list) {
        /* 取前驱做尾部重叠检查 */
        vm_area_t *prev = (vm_area_t *)t->vma_list;
        while (prev->next && prev->next != *pp) {
            prev = prev->next;
        }
        if (prev->end > start) {
            spin_unlock_irqrestore(&g_vma_lock, f);
            kfree(n);
            return false;
        }
    }
    n->next = *pp;
    *pp = n;
    spin_unlock_irqrestore(&g_vma_lock, f);
    return true;
}

vm_area_t *vma_find(struct task *t, uint64_t addr)
{
    if (!t) {
        return NULL;
    }
    uint64_t f = spin_lock_irqsave(&g_vma_lock);
    vm_area_t *v = (vm_area_t *)t->vma_list;
    while (v) {
        if (addr >= v->start && addr < v->end) {
            break;
        }
        if (v->start > addr) {          /* 升序链表：已越过，提前止损 */
            v = NULL;
            break;
        }
        v = v->next;
    }
    spin_unlock_irqrestore(&g_vma_lock, f);
    return v;
}

uint64_t vma_find_free(struct task *t, uint64_t base, uint64_t top,
                       uint64_t len)
{
    if (!t || len == 0 || (len & (PAGE_SIZE - 1)) || base + len > top) {
        return 0;
    }
    uint64_t f = spin_lock_irqsave(&g_vma_lock);
    uint64_t cand = base;
    for (vm_area_t *v = (vm_area_t *)t->vma_list; v; v = v->next) {
        if (v->end <= cand) {
            continue;                    /* 整段在候选点之前，不相干 */
        }
        if (v->start >= cand + len) {
            break;                       /* 升序：候选洞已足够大 */
        }
        cand = v->end;                   /* 与候选段相交：推到该 VMA 之后 */
        if (cand + len > top) {
            spin_unlock_irqrestore(&g_vma_lock, f);
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_vma_lock, f);
    return (cand + len <= top) ? cand : 0;
}

bool vma_populate(struct task *t, uint64_t addr, bool write)
{
    if (!t || !t->is_user || addr > USER_SPACE_TOP) {
        return false;
    }
    uint64_t va = addr & ~((uint64_t)PAGE_SIZE - 1);
    uint64_t pte = vmm_pte(t->cr3, va);

    if (pte & PTE_PRESENT) {
        /* 已映射页的故障：只可能是 COW 写故障可救；其余（NX 取指、
         * 只读页写、reserved 位）属真违规，交调用方处理。 */
        if (write && !(pte & PTE_WRITE) && (pte & PTE_COW)) {
            return vmm_cow_break(t->cr3, va);
        }
        return false;
    }

    /* 未映射：查 VMA 补零页（demand zero-fill） */
    vm_area_t *v = vma_find(t, va);
    if (!v) {
        return false;
    }
    if (write && !(v->prot & PTE_WRITE)) {
        return false;                    /* 区间只读，写故障不可救 */
    }
    void *page = pmm_alloc_page();       /* 已清零 */
    if (!page) {
        return false;
    }
    uint64_t flags = PTE_PRESENT | PTE_USER
                   | (v->prot & (PTE_WRITE | PTE_NX));
    if (!vmm_map_page(t->cr3, va, (uint64_t)page, flags)) {
        pmm_free_page(page);
        return false;
    }
    return true;
}

bool vma_unmap_range(struct task *t, uint64_t start, uint64_t end)
{
    if (!t || start >= end || (start & (PAGE_SIZE - 1)) ||
        (end & (PAGE_SIZE - 1))) {
        return false;
    }
    uint64_t f = spin_lock_irqsave(&g_vma_lock);
    vm_area_t **pp = (vm_area_t **)&t->vma_list;
    while (*pp) {
        vm_area_t *v = *pp;
        if (v->start >= end) {
            break;                       /* 升序：后面不再相交 */
        }
        if (v->end <= start) {
            pp = &v->next;
            continue;
        }
        /* 相交。四种关系：完全覆盖 / 切头 / 切尾 / 中间挖洞 */
        if (start <= v->start && end >= v->end) {
            *pp = v->next;               /* 整段移除 */
            kfree(v);
            continue;
        }
        if (start <= v->start) {
            v->start = end;              /* 切头 */
            pp = &v->next;
            continue;
        }
        if (end >= v->end) {
            v->end = start;              /* 切尾 */
            pp = &v->next;
            continue;
        }
        /* 中间挖洞：拆成 [v->start,start) + [end,v->end) */
        vm_area_t *hi = (vm_area_t *)kmalloc(sizeof(vm_area_t));
        if (!hi) {
            spin_unlock_irqrestore(&g_vma_lock, f);
            return false;
        }
        hi->start = end;
        hi->end   = v->end;
        hi->prot  = v->prot;
        hi->type  = v->type;
        hi->next  = v->next;
        v->end    = start;
        v->next   = hi;
        pp = &hi->next;
    }
    spin_unlock_irqrestore(&g_vma_lock, f);

    /* 解除已填充页的映射并释放物理页（共享页由 pmm 引用计数守卫降级）。
     * OOL 页不会出现在 VMA 管理区间（mmap 区与 OOL 窗口分离）。 */
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t pte = vmm_pte(t->cr3, va);
        if (pte & PTE_PRESENT) {
            uint64_t phys = pte & PTE_ADDR_MASK;
            vmm_unmap_page(t->cr3, va);
            pmm_decref((void *)phys);
        }
    }
    return true;
}

void vma_destroy_all(struct task *t)
{
    if (!t) {
        return;
    }
    uint64_t f = spin_lock_irqsave(&g_vma_lock);
    vm_area_t *v = (vm_area_t *)t->vma_list;
    t->vma_list = NULL;
    spin_unlock_irqrestore(&g_vma_lock, f);
    while (v) {
        vm_area_t *n = v->next;
        kfree(v);
        v = n;
    }
}

/*
 * vma_protect —— mprotect 语义：修改 [start, end) 的访问权限。
 * ---------------------------------------------------------------------------
 * 两步缺一不可：
 *   1) VMA 登记层：必须按需【拆分】区间两端（只改中间段），保持链表升序，
 *      否则一次调用会误改整块 VMA 的权限（越权放宽相邻内存的保护）；
 *   2) 页表层：对【已映射】的页必须用新权限重写 PTE——只改 VMA 不影响
 *      现存映射，用户立刻就能以旧权限继续访问，mprotect 形同虚设。
 *      未映射的页无需处理：首次触碰时由 vma_populate 按新 VMA 权限补页。
 *
 * 安全红线：prot 由调用方（sys_mprotect）过滤，此处不再做 W^X 判定，
 * 但绝不允许出现「可执行」位——内核侧 PTE_NX 恒定，见 vma_populate 与
 * sys_mmap 的一贯约定。
 *
 * 返回 false 表示参数非法、区间未被任何 VMA 覆盖，或节点分配失败（此时
 * 链表保持原状，绝不留下半改状态）。
 */
bool vma_protect(struct task *t, uint64_t start, uint64_t end, uint64_t prot)
{
    if (!t || start >= end) {
        return false;
    }
    if (start & (PAGE_SIZE - 1) || end & (PAGE_SIZE - 1)) {
        return false;
    }
    if (end > USER_SPACE_TOP + 1) {
        return false;
    }

    uint64_t f = spin_lock_irqsave(&g_vma_lock);

    /* 先做「全覆盖」校验：区间必须完全落在已有 VMA 的并集内，
     * 中途失败不留副作用（先扫描再改）。 */
    {
        uint64_t cover = start;
        vm_area_t *v = (vm_area_t *)t->vma_list;
        while (v && cover < end) {
            if (v->start <= cover && v->end > cover) {
                cover = v->end;
            }
            v = v->next;
        }
        if (cover < end) {
            spin_unlock_irqrestore(&g_vma_lock, f);
            return false;
        }
    }

    /* 拆分 + 改权限。遍历期间会插入新节点，用「先取 next 再处理」避免踩空。 */
    vm_area_t *v = (vm_area_t *)t->vma_list;
    vm_area_t *prev = NULL;
    while (v) {
        vm_area_t *next = v->next;
        uint64_t vs = v->start, ve = v->end;
        if (ve <= start || vs >= end) {
            prev = v;
            v = next;
            continue;
        }
        uint64_t os = (vs > start) ? vs : start;
        uint64_t oe = (ve < end) ? ve : end;

        if (os > vs) {
            /* 前段：保留原权限 */
            vm_area_t *a = (vm_area_t *)kmalloc(sizeof(vm_area_t));
            if (!a) {
                spin_unlock_irqrestore(&g_vma_lock, f);
                return false;
            }
            a->start = vs; a->end = os; a->prot = v->prot;
            a->type = v->type; a->next = v;
            if (prev) {
                prev->next = a;
            } else {
                t->vma_list = a;
            }
            prev = a;
        }
        if (oe < ve) {
            /* 后段：保留原权限 */
            vm_area_t *b = (vm_area_t *)kmalloc(sizeof(vm_area_t));
            if (!b) {
                spin_unlock_irqrestore(&g_vma_lock, f);
                return false;
            }
            b->start = oe; b->end = ve; b->prot = v->prot;
            b->type = v->type; b->next = v->next;
            v->next = b;
        }
        /* 中段：应用新权限 */
        v->start = os;
        v->end = oe;
        v->prot = prot;

        prev = v;
        v = v->next;
    }

    /* 页表层：更新区间内已映射页的 PTE 权限（未映射页交给按需补页） */
    for (uint64_t a = start; a < end; a += PAGE_SIZE) {
        uint64_t pte = vmm_pte(t->cr3, a);
        if (!(pte & PTE_PRESENT)) {
            continue;                       /* 尚未补页，VMA 权限已足够 */
        }
        uint64_t phys = pte & PTE_ADDR_MASK;
        uint64_t newflags = PTE_PRESENT | PTE_USER
                          | (prot & PTE_WRITE) | PTE_NX;
        if (pte & PTE_OOL) {
            newflags |= PTE_OOL;            /* OOL 共享页标记不得丢失 */
        }
        vmm_map_page(t->cr3, a, phys, newflags);
    }
    /* 改页表后必须刷 TLB：否则 CPU 仍按旧权限翻译，mprotect 不生效。
     * 重载 CR3 是最彻底的全量失效（等价于 Linux 的 flush_tlb_mm）；此处
     * CR3 已是本任务的内核视图，写回同值不会改变地址空间语义。 */
    {
        uint64_t cr3;
        __asm__ volatile("movq %%cr3, %0" : "=r"(cr3) :: "memory");
        __asm__ volatile("movq %0, %%cr3" :: "r"(cr3) : "memory");
    }

    spin_unlock_irqrestore(&g_vma_lock, f);
    return true;
}

/*
 * vma_clone_all —— fork 用：把父进程的 VMA 链表深拷贝到子进程。
 * ---------------------------------------------------------------------------
 * 必要性：fork 的地址空间由 vmm_fork_cow 建立（用户半区页表级 COW 共享），
 * 但 VMA 是「登记型元数据」（区间/权限/类型），不随页表复制。子进程若没有
 * 自己的 VMA 链表，则：
 *   - mmap 区域与栈增长区的按需补页（#PF -> vma_populate）在子进程中失效，
 *     首次访问即被当作非法访问而杀任务；
 *   - munmap/mprotect 找不到区间，误返回 EINVAL。
 * 故必须与 COW 页表同步克隆，二者共同构成完整的 fork 地址空间语义。
 *
 * 实现：按升序单链表逐节点 kmalloc + 复制（保持升序，vma_find_free/
 * vma_populate 的首次适配与查找逻辑依赖该顺序）。任一节点分配失败即回滚
 * 全部已分配节点并返回 false，绝不留下半截链表（半截链表会让子进程在
 * 部分区间上“看起来合法”却无法补页，属隐蔽故障）。
 */
bool vma_clone_all(struct task *dst, const struct task *src)
{
    if (!dst || !src) {
        return false;
    }
    uint64_t f = spin_lock_irqsave(&g_vma_lock);

    vm_area_t *s = (vm_area_t *)src->vma_list;
    vm_area_t *head = NULL, *tail = NULL;
    while (s) {
        vm_area_t *n = (vm_area_t *)kmalloc(sizeof(vm_area_t));
        if (!n) {
            /* 回滚：释放本函数已分配的全部节点，dst->vma_list 保持原样 */
            while (head) {
                vm_area_t *nx = head->next;
                kfree(head);
                head = nx;
            }
            spin_unlock_irqrestore(&g_vma_lock, f);
            return false;
        }
        n->start = s->start;
        n->end = s->end;
        n->prot = s->prot;
        n->type = s->type;
        n->next = NULL;
        if (tail) {
            tail->next = n;
        } else {
            head = n;
        }
        tail = n;
        s = s->next;
    }
    dst->vma_list = head;
    spin_unlock_irqrestore(&g_vma_lock, f);
    return true;
}

/* ========================================================================= */
/*  启动自检：VMA 操作 + demand 填充 + COW fork/断开全链路                     */
/* ========================================================================= */
void vma_selftest(void)
{
    bool ok = true;
    uint64_t free0 = pmm_free_pages();

    /* 伪任务：只需要 cr3/vma_list/is_user 三个字段参与测试 */
    static task_t ft;
    memset(&ft, 0, sizeof(ft));
    ft.is_user = true;
    ft.cr3 = vmm_create_address_space();
    if (!ft.cr3) {
        kprintf("[vma] selftest FAIL: no address space\n");
        return;
    }

    /* T1：VMA 插入/查找/重叠拒绝 */
    ok &= vma_insert(&ft, 0x10000000, 0x10010000, PTE_WRITE | PTE_NX,
                     VMA_TYPE_ANON);                       /* 16 页 */
    ok &= !vma_insert(&ft, 0x1000F000, 0x10011000, PTE_WRITE, VMA_TYPE_ANON);
    ok &= (vma_find(&ft, 0x10008123) != NULL);
    ok &= (vma_find(&ft, 0x10010000) == NULL);             /* end 不含 */
    if (!ok) kprintf("[vma] T1 FAIL (insert/find/overlap)\n");

    /* T2：demand 填充——未映射页经 vma_populate 补零页并可写 */
    bool t2 = ok;
    if (vmm_pte(ft.cr3, 0x10004000) != 0) t2 = false;      /* 尚未填充 */
    t2 = t2 && vma_populate(&ft, 0x10004abc, true);
    uint64_t pte = vmm_pte(ft.cr3, 0x10004000);
    t2 = t2 && (pte & PTE_PRESENT) && (pte & PTE_WRITE) && (pte & PTE_USER);
    /* VMA 外地址不可救 */
    t2 = t2 && !vma_populate(&ft, 0x20000000, false);
    if (!t2) kprintf("[vma] T2 FAIL (demand fill)\n");
    ok &= t2;

    /* T3：COW fork + 写断开。父页写入图案 -> fork_cow -> 双方只读共享
     * -> 子写断开 -> 内容独立、父页不变、引用计数回归。 */
    bool t3 = ok;
    uint64_t child = vmm_create_address_space();
    if (child) {
        uint64_t ppa = vmm_translate(ft.cr3, 0x10004000) & ~0xFFFUL;
        memset(PHYS_TO_VIRT(ppa), 0xA5, 64);               /* 父页图案 */
        t3 = t3 && vmm_fork_cow(ft.cr3, child);

        uint64_t p_pte = vmm_pte(ft.cr3, 0x10004000);
        uint64_t c_pte = vmm_pte(child,  0x10004000);
        /* 双方均：只读 + COW + 指向同一物理页，引用计数=2 */
        t3 = t3 && !(p_pte & PTE_WRITE) && (p_pte & PTE_COW);
        t3 = t3 && !(c_pte & PTE_WRITE) && (c_pte & PTE_COW);
        t3 = t3 && ((p_pte & PTE_ADDR_MASK) == (c_pte & PTE_ADDR_MASK));
        t3 = t3 && (pmm_refcount((void *)ppa) == 2);

        /* 子空间写断开 */
        t3 = t3 && vmm_cow_break(child, 0x10004000);
        uint64_t c2 = vmm_pte(child, 0x10004000);
        uint64_t cpa = c2 & PTE_ADDR_MASK;
        t3 = t3 && (c2 & PTE_WRITE) && !(c2 & PTE_COW) && (cpa != ppa);
        /* 内容已拷贝且互相独立 */
        t3 = t3 && (memcmp(PHYS_TO_VIRT(cpa), PHYS_TO_VIRT(ppa), 64) == 0);
        ((uint8_t *)PHYS_TO_VIRT(cpa))[0] = 0x5A;
        t3 = t3 && (((uint8_t *)PHYS_TO_VIRT(ppa))[0] == 0xA5);
        t3 = t3 && (pmm_refcount((void *)ppa) == 1);

        /* 父空间最后持有者写断开（零拷贝快路径：原页直接改回可写） */
        t3 = t3 && vmm_cow_break(ft.cr3, 0x10004000);
        uint64_t p2 = vmm_pte(ft.cr3, 0x10004000);
        t3 = t3 && (p2 & PTE_WRITE) && !(p2 & PTE_COW)
                && ((p2 & PTE_ADDR_MASK) == ppa);

        vmm_destroy_address_space(child);
    } else {
        t3 = false;
    }
    if (!t3) kprintf("[vma] T3 FAIL (COW fork/break)\n");
    ok &= t3;

    /* T4：munmap 挖洞拆分 + 页回收 */
    bool t4 = ok;
    t4 = t4 && vma_unmap_range(&ft, 0x10004000, 0x10005000);  /* 挖 1 页 */
    t4 = t4 && (vma_find(&ft, 0x10004000) == NULL);
    t4 = t4 && (vma_find(&ft, 0x10003000) != NULL);
    t4 = t4 && (vma_find(&ft, 0x10005000) != NULL);
    t4 = t4 && (vmm_pte(ft.cr3, 0x10004000) == 0);
    if (!t4) kprintf("[vma] T4 FAIL (munmap split)\n");
    ok &= t4;

    /* 清理并验证无页泄漏 */
    vma_destroy_all(&ft);
    vmm_destroy_address_space(ft.cr3);
    if (pmm_free_pages() != free0) {
        kprintf("[vma] selftest FAIL: page leak (%lu -> %lu)\n",
                (unsigned long)free0, (unsigned long)pmm_free_pages());
        ok = false;
    }
    kprintf("[vma] selftest (T1 vma/T2 demand/T3 COW/T4 munmap): %s\n",
            ok ? "PASS" : "FAIL");
}
