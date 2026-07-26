/*
 * kernel/ipc/port.c
 * -----------------------------------------------------------------------------
 * Mach 式 IPC 端口路由（手册第 7 章）。
 *
 * - 全局端口表 g_ports[PORT_MAX]，端口号即索引。
 * - 小消息：sys_mach_msg 用 copy_from_user 拷入内核 kernel_msg_t，
 *   投递到目的端口队列；接收方 copy_to_user 拷出（两次拷贝）。
 * - OOL 大消息：只搬运物理页号。发送时按发送方页表翻译 VA->PA 并
 *   引用计数 +1；接收时映射进接收方地址空间的 OOL 窗口（零拷贝）。
 * - 阻塞接收：接收任务置 WAITING 并让出；消息到达即被唤醒 (READY)。
 *
 * 调用关系：Ring3 syscall -> sys_mach_msg（强符号，覆盖 syscall.c weak）
 *           内核服务线程 -> ipc_send_kernel / ipc_recv_kernel。
 */
#include <ipc/port.h>
#include <kernel/syscall.h>
#include <kernel/console.h>
#include <kernel/string.h>
#include <kernel/interrupts.h>
#include <mm/kmalloc.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

static kernel_port_t g_ports[PORT_MAX];

/* OOL 接收窗口：映射进接收方用户空间的基址（优先复用空闲区间） */
#define OOL_RECV_BASE  0x0000600000000000UL
static uint64_t g_ool_bump = 0;

/* OOL 区域空闲链表：任务退出后回收映射区间，避免 g_ool_bump 单调递增耗尽（A3 项） */
typedef struct ool_free { uint64_t base; uint64_t size; struct ool_free *next; } ool_free_t;
static ool_free_t *g_ool_free = NULL;

/* ---- 中断开关辅助（单核临界区） ---- */
static inline uint64_t irq_save(void)
{
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f)
{
    if (f & (1UL << 9)) {
        __asm__ volatile("sti" ::: "memory");
    }
}

void ipc_init(void)
{
    memset(g_ports, 0, sizeof(g_ports));
    /* 预留知名端口 */
    for (uint32_t p = DISK_PORT; p <= FS_REPLY_PORT; p++) {
        g_ports[p].in_use = true;
        g_ports[p].name = p;
    }
    kprintf("[ipc] port table ready (%d slots, well-known 1..7)\n", PORT_MAX);
}

kernel_port_t *port_lookup(uint32_t name)
{
    if (name == PORT_NULL || name >= PORT_MAX || !g_ports[name].in_use) {
        return NULL;
    }
    return &g_ports[name];
}

/* 授权某任务向内核端口发送（如 fs-server -> DISK_PORT）。send_owner==NULL
 * 表示任何任务均可发送。A2 项。 */
void port_grant_send(uint32_t name, task_t *owner)
{
    kernel_port_t *p = port_lookup(name);
    if (p) {
        p->send_owner = owner;
    }
}

/* 用户态认领端口的接收权：仅当端口无人认领或已归当前任务所有时成功。A2 项。 */
uint64_t port_claim(uint32_t name)
{
    kernel_port_t *p = port_lookup(name);
    if (!p) {
        return (uint64_t)-1;
    }
    if (p->owner && p->owner != sched_current()) {
        return (uint64_t)-1;
    }
    p->owner = sched_current();
    return 0;
}

/* 任务退出时回收其持有的 OOL 映射：递减共享物理页引用并归还映射区间到
 * 空闲链表（A3 项）。由调度器在回收死任务时调用。 */
void port_reap_ool(task_t *t)
{
    ool_map_node_t *n = (ool_map_node_t *)t->ool_maps;
    while (n) {
        ool_map_node_t *nx = n->next;
        for (uint32_t i = 0; i < n->count; i++) {
            pmm_decref((void *)n->pages[i]);
        }
        ool_free_t *fr = kmalloc(sizeof(ool_free_t));
        if (fr) {                       /* 回收映射区间供后续复用 */
            fr->base = n->va;
            fr->size = (uint64_t)(n->count + 1) * PAGE_SIZE;
            fr->next = g_ool_free;
            g_ool_free = fr;
        }
        kfree(n);
        n = nx;
    }
    t->ool_maps = NULL;
}

uint32_t port_allocate(task_t *owner)
{
    uint64_t f = irq_save();
    for (uint32_t i = PORT_FIRST_DYN; i < PORT_MAX; i++) {
        if (!g_ports[i].in_use) {
            g_ports[i].in_use = true;
            g_ports[i].name = i;
            g_ports[i].owner = owner;
            g_ports[i].queue_head = g_ports[i].queue_tail = NULL;
            g_ports[i].queue_len = 0;
            g_ports[i].waiter_head = g_ports[i].waiter_tail = NULL;
            irq_restore(f);
            return i;
        }
    }
    irq_restore(f);
    return PORT_NULL;
}

void port_set_owner(uint32_t name, task_t *owner)
{
    kernel_port_t *p = port_lookup(name);
    if (p) {
        p->owner = owner;
    }
}

/* ---- 队列操作（调用方须持临界区） ---- */
static void enqueue(kernel_port_t *p, kernel_msg_t *m)
{
    m->next = NULL;
    if (p->queue_tail) {
        p->queue_tail->next = m;
    } else {
        p->queue_head = m;
    }
    p->queue_tail = m;
    p->queue_len++;

    /* 唤醒 FIFO 队首等待者（A4 项：取代单 waiter 指针，避免丢失唤醒） */
    if (p->waiter_head) {
        task_t *w = p->waiter_head;
        p->waiter_head = w->wait_next;
        if (!p->waiter_head) {
            p->waiter_tail = NULL;
        }
        w->wait_next = NULL;
        w->state = READY;
    }
}

/* 将当前任务加入端口等待队列尾部（调用方须持临界区且随后 schedule()） */
static void port_wait_enqueue(kernel_port_t *p)
{
    task_t *t = sched_current();
    t->wait_next = NULL;
    if (p->waiter_tail) {
        p->waiter_tail->wait_next = t;
    } else {
        p->waiter_head = t;
    }
    p->waiter_tail = t;
    t->state = WAITING;
}

static kernel_msg_t *dequeue(kernel_port_t *p)
{
    kernel_msg_t *m = p->queue_head;
    if (m) {
        p->queue_head = m->next;
        if (!p->queue_head) {
            p->queue_tail = NULL;
        }
        p->queue_len--;
    }
    return m;
}

/* ---- 投递一条已构造好的内核消息 ---- */
static uint64_t deliver(uint32_t dest, kernel_msg_t *m)
{
    uint64_t f = irq_save();
    kernel_port_t *p = port_lookup(dest);
    if (!p) {
        irq_restore(f);
        kfree(m);
        return MACH_SEND_INVALID_DEST;
    }
    enqueue(p, m);
    irq_restore(f);
    return MACH_MSG_SUCCESS;
}

/* ---- 内核侧 API ---- */

uint64_t ipc_send_kernel(uint32_t dest, const void *msg, uint32_t size)
{
    if (size < sizeof(mach_msg_header_t) ||
        size > MACH_MSG_INLINE_MAX + sizeof(mach_msg_header_t)) {
        return MACH_SEND_TOO_LARGE;
    }
    kernel_msg_t *m = (kernel_msg_t *)kmalloc(sizeof(kernel_msg_t) + size);
    if (!m) {
        return MACH_SEND_TOO_LARGE;
    }
    memset(m, 0, sizeof(kernel_msg_t));
    memcpy(m->data, msg, size);
    m->size = size;
    return deliver(dest, m);
}

uint64_t ipc_recv_kernel(uint32_t port_name, void *buf, uint32_t buf_size,
                         uint32_t *out_size, bool block)
{
    kernel_port_t *p = port_lookup(port_name);
    if (!p) {
        return MACH_RCV_INVALID_NAME;
    }

    for (;;) {
        uint64_t f = irq_save();
        kernel_msg_t *m = dequeue(p);
        if (m) {
            irq_restore(f);
            uint32_t n = m->size < buf_size ? m->size : buf_size;
            memcpy(buf, m->data, n);
            if (out_size) {
                *out_size = n;
            }
            kfree(m);
            return MACH_MSG_SUCCESS;
        }
        if (!block) {
            irq_restore(f);
            return MACH_RCV_TIMED_OUT;
        }
        /* 阻塞：登记到等待队列并让出 CPU（关中断下切换是安全的） */
        port_wait_enqueue(p);
        schedule();
        irq_restore(f);
        /* 被唤醒后重试出队 */
    }
}

/* ---- OOL 支持 ---- */

/* 发送方：把 [uva, uva+size) 的物理页记录到消息（引用计数+1，零拷贝） */
static uint64_t ool_capture(kernel_msg_t *m, uint64_t uva, uint64_t size)
{
    if (size == 0 || size > MACH_MSG_OOL_MAX_PAGES * PAGE_SIZE) {
        return MACH_SEND_TOO_LARGE;
    }
    if (uva & (PAGE_SIZE - 1)) {
        return MACH_INVALID_ARGUMENT;      /* 要求页对齐 */
    }
    uint64_t cr3 = sched_current()->cr3;
    uint32_t pages = (uint32_t)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    for (uint32_t i = 0; i < pages; i++) {
        uint64_t pa = vmm_translate(cr3, uva + (uint64_t)i * PAGE_SIZE);
        if (!pa) {
            return MACH_INVALID_ARGUMENT;
        }
        pmm_incref((void *)pa);
        m->ool_pages[i] = pa;
    }
    m->ool_page_count = pages;
    m->ool_size = size;
    m->has_ool = true;
    return MACH_MSG_SUCCESS;
}

/* 接收方：把 OOL 页映射进接收方地址空间，返回映射后的虚拟地址。
 * 优先从空闲链表复用区间；映射带 PTE_OOL 标记，vmm_destroy 时只解除映射
 * 不释放物理页（引用计数由 port_reap_ool 在任务退出时递减，A3 项）。 */
static uint64_t ool_map_into_receiver(kernel_msg_t *m)
{
    uint64_t cr3 = sched_current()->cr3;
    uint64_t need = (uint64_t)(m->ool_page_count + 1) * PAGE_SIZE;  /* 含守护页 */
    uint64_t base = 0;

    /* 尝试复用已释放的 OOL 区域（单核，关中断保护空闲链表） */
    uint64_t f = irq_save();
    ool_free_t **pp = &g_ool_free;
    while (*pp) {
        if ((*pp)->size >= need) {
            base = (*pp)->base;
            ool_free_t *fr = *pp;
            *pp = fr->next;
            irq_restore(f);
            kfree(fr);
            goto got;
        }
        pp = &(*pp)->next;
    }
    base = OOL_RECV_BASE + g_ool_bump;
    g_ool_bump += need;
    irq_restore(f);

got:
    for (uint32_t i = 0; i < m->ool_page_count; i++) {
        vmm_map_page(cr3, base + (uint64_t)i * PAGE_SIZE, m->ool_pages[i],
                     PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_NX | PTE_OOL);
    }
    return base;
}

/* ---- syscall 强符号实现（覆盖 syscall.c 中的 weak 占位） ----
 * sys_mach_msg(msg_uptr, option, send_size, recv_limit, port)
 *   MACH_SEND_MSG: msg_uptr 指向 [header|<ool_desc>|payload]，send_size=总大小
 *   MACH_RECV_MSG: 从 port 收一条到 msg_uptr（容量 recv_limit）
 */
uint64_t sys_mach_msg(uint64_t msg_uptr, uint64_t option,
                      uint64_t send_size, uint64_t recv_limit, uint64_t port)
{
    if (option & MACH_SEND_MSG) {
        if (send_size < sizeof(mach_msg_header_t) ||
            send_size > sizeof(mach_msg_header_t) + MACH_MSG_INLINE_MAX) {
            return MACH_SEND_TOO_LARGE;
        }
        kernel_msg_t *m = (kernel_msg_t *)kmalloc(sizeof(kernel_msg_t) + send_size);
        if (!m) {
            return MACH_SEND_TOO_LARGE;
        }
        memset(m, 0, sizeof(kernel_msg_t));
        /* 红线：用户数据一律 copy_from_user */
        if (copy_from_user(m->data, (const void *)msg_uptr, send_size) != send_size) {
            kfree(m);
            return MACH_INVALID_ARGUMENT;
        }
        m->size = (uint32_t)send_size;

        mach_msg_header_t *h = (mach_msg_header_t *)m->data;
        uint32_t dest = h->msgh_remote_port;

        if (h->msgh_bits & MACH_MSGH_BITS_OOL) {
            if (m->size < sizeof(mach_msg_header_t) + sizeof(mach_ool_desc_t)) {
                kfree(m);
                return MACH_INVALID_ARGUMENT;
            }
            mach_ool_desc_t *d =
                (mach_ool_desc_t *)(m->data + sizeof(mach_msg_header_t));
            uint64_t rc = ool_capture(m, d->address, d->size);
            if (rc != MACH_MSG_SUCCESS) {
                kfree(m);
                return rc;
            }
        }
        /* A2 项：发送权校验。send_owner==NULL 表示任何人可发；
         * 否则仅授权任务可发（防止用户态伪造内核端口请求）。 */
        kernel_port_t *dp = port_lookup(dest);
        if (!dp || (dp->send_owner && dp->send_owner != sched_current())) {
            if (m->has_ool) {                    /* OOL 发送失败：递减引用（A3） */
                for (uint32_t i = 0; i < m->ool_page_count; i++)
                    pmm_decref((void *)m->ool_pages[i]);
            }
            kfree(m);
            return MACH_SEND_INVALID_DEST;
        }
        uint64_t rc = deliver(dest, m);
        if (rc != MACH_MSG_SUCCESS) {
            if (m->has_ool) {                    /* OOL 投递失败：递减引用（A3） */
                for (uint32_t i = 0; i < m->ool_page_count; i++)
                    pmm_decref((void *)m->ool_pages[i]);
            }
            kfree(m);
            return rc;
        }
    }

    if (option & MACH_RECV_MSG) {
        /* A2 项：仅端口 owner 可接收，防止窃听/伪造应答 */
        kernel_port_t *p = port_lookup((uint32_t)port);
        if (!p || p->owner != sched_current()) {
            return MACH_RCV_INVALID_NAME;
        }
        kernel_msg_t *m = NULL;
        for (;;) {
            uint64_t f = irq_save();
            m = dequeue(p);
            if (m) {
                irq_restore(f);
                break;
            }
            port_wait_enqueue(p);
            schedule();
            irq_restore(f);
        }
        if (m->size > recv_limit) {
            kfree(m);                      /* 超限即丢弃 */
            return MACH_RCV_TOO_LARGE;
        }
        /* OOL：映射进接收方并改写描述符为接收方 VA；登记到任务 OOL 链表（A3） */
        if (m->has_ool) {
            uint64_t va = ool_map_into_receiver(m);
            mach_ool_desc_t *d =
                (mach_ool_desc_t *)(m->data + sizeof(mach_msg_header_t));
            d->address = va;
            d->size = m->ool_size;
            m->ool_mapped_va = va;
            ool_map_node_t *node = kmalloc(sizeof(ool_map_node_t));
            if (node) {
                node->va = va;
                node->count = m->ool_page_count;
                for (uint32_t i = 0; i < m->ool_page_count; i++)
                    node->pages[i] = m->ool_pages[i];
                node->next = sched_current()->ool_maps;
                sched_current()->ool_maps = node;
            }
        }
        if (copy_to_user((void *)msg_uptr, m->data, m->size) != m->size) {
            kfree(m);
            return MACH_INVALID_ARGUMENT;
        }
        kfree(m);
    }
    return MACH_MSG_SUCCESS;
}
