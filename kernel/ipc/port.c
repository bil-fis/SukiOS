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
#include <kernel/spinlock.h>
#include <kernel/percpu.h>
#include <kernel/smp.h>
#include <kernel/apic.h>
#include <mm/kmalloc.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

static kernel_port_t g_ports[PORT_MAX];

/* P0-R1：端口表自旋锁，取代单核 cli/sti 临界区（cli/sti 在 SMP 下不隔离
 * 其它核）。原 irq_save/irq_restore 包裹接口保留，内部改为 spin_lock_irqsave。 */
static spinlock_t g_port_lock = SPINLOCK_INIT("port");

/* OOL 接收窗口：映射进接收方用户空间的基址（优先复用空闲区间） */
#define OOL_RECV_BASE  0x0000600000000000UL
/* OOL 窗口上界（H4 修复）：g_ool_bump 单调递增到此处即视为耗尽，
 * 不再允许新的映射，强制复用空闲链表或返回失败，杜绝无限向上爬升
 * 撞入内核/其它区域导致 #PF panic。1TiB 窗口对单条 <=16 页(64KiB) 的
 * OOL 消息而言足以支撑千万级映射，纯属安全网。 */
#define OOL_RECV_LIMIT 0x0000700000000000UL
static uint64_t g_ool_bump = 0;

/* OOL 区域空闲链表：任务退出后回收映射区间，避免 g_ool_bump 单调递增耗尽（A3 项） */
typedef struct ool_free { uint64_t base; uint64_t size; struct ool_free *next; } ool_free_t;
static ool_free_t *g_ool_free = NULL;

/* ---- 端口表临界区：自旋锁 irqsave 包装（P0-R1） ---- */
static inline uint64_t irq_save(void)
{
    return spin_lock_irqsave(&g_port_lock);
}
static inline void irq_restore(uint64_t f)
{
    spin_unlock_irqrestore(&g_port_lock, f);
}

/* 阻塞让出（P0-R1 关键正确性）：登记等待者之后必须【先释放端口自旋锁】
 * 再 schedule() —— 单核 cli/sti 时代持"锁"（即关中断）切换是安全的，但
 * 自旋锁时代持锁切走会让其它核/任务的所有 IPC 在关中断下永久自旋（死锁）。
 * 顺序：解锁（保持关中断，杜绝本核中断重入端口路径）-> schedule ->
 * 被唤醒后按进入时的 RFLAGS.IF 恢复中断。潜在的"解锁后、调度前被唤醒"
 * 不丢事件：唤醒仅置 state=READY，任务仍在运行队列，稍后必被重新调度。 */
static void port_block_and_yield(uint64_t f)
{
    spin_unlock(&g_port_lock);
    schedule();
    if (f & (1UL << 9)) {
        __asm__ volatile("sti" ::: "memory");
    }
}

void ipc_init(void)
{
    memset(g_ports, 0, sizeof(g_ports));
    /* H3 修复：将 PORT_NULL(0) 永久保留为"空端口"哨兵——标记为 in_use 使
     * 其永远不可被 port_allocate 分配（分配循环本就从 PORT_FIRST_DYN 起，
     * 此处置位是双保险），且 port_lookup(0) 仍因 name==PORT_NULL 短路返回
     * NULL，故 0 在任何上下文都只表示"无端口/失败"，与合法端口号 1..63
     * 彻底分离，杜绝"耗尽返回 0 与空端口语义混淆"导致的泄漏/静默失败。 */
    g_ports[PORT_NULL].in_use = true;
    g_ports[PORT_NULL].name   = PORT_NULL;
    g_ports[PORT_NULL].owner  = NULL;
    /* 预留知名端口（含 APP_PORT，供独立 app 认领应答） */
    for (uint32_t p = DISK_PORT; p <= APP_PORT; p++) {
        g_ports[p].in_use = true;
        g_ports[p].name = p;
    }
    kprintf("[ipc] port table ready (%d slots, well-known 1..8, 0=sentinel)\n",
            PORT_MAX);
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

/* 任务退出时释放其认领的所有端口所有权（owner/send_owner 置空），避免
 * 端口 owner 变成悬空指针导致下一个同名端口认领者被永久拒绝（A2 修复）。
 * 知名端口（<PORT_FIRST_DYN）仅清 owner 保留槽位；动态端口不在此处理
 * （由 port_free 显式回收）。单核：由 task_exit_current 关中断下调用。 */
void port_release_owner(task_t *t)
{
    for (uint32_t i = DISK_PORT; i < PORT_MAX; i++) {
        if (g_ports[i].owner == t) {
            g_ports[i].owner = NULL;
        }
        if (g_ports[i].send_owner == t) {
            g_ports[i].send_owner = NULL;
        }
    }
}

/* 任务退出时回收其持有的 OOL 映射：递减共享物理页引用并归还映射区间到
 * 空闲链表（A3 项）。由调度器在回收死任务时调用。 */
void port_reap_ool(task_t *t)
{
    /* M4 修复：OOL 共享物理页的引用释放已统一移交给
     * vmm_destroy_address_space()（其销毁接收方地址空间时对 PTE_OOL 页执行
     * pmm_decref，仅释放本地址空间的引用，杜绝误释放他任务页）。此处仅回收
     * 映射区间（VA 区域）到空闲链表供后续复用，不再对物理页做 decref
     * （避免与 vmm_destroy 重复计数导致双重释放）。 */
    ool_map_node_t *n = (ool_map_node_t *)t->ool_maps;
    while (n) {
        ool_map_node_t *nx = n->next;
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
    /* H3 修复：端口表耗尽。返回 PORT_NULL(0) 现在是明确的"失败"语义
     * （0 号槽已被永久保留为哨兵，绝不可能是合法端口号），调用方（如
     * exec_read_file）据 `if (!rp)` 判定失败并清理，不再产生歧义。
     * 此处留日志便于观测资源泄漏型长稳问题。 */
    kprintf("[ipc] port_allocate: table exhausted (max=%u)\n", PORT_MAX);
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
        /* P0-R1：等待者绑定在其它核且该核正 hlt 空闲时，发 IPI 立即唤醒，
         * 否则最坏要等对核下一个 10ms 定时节拍才被调度（IPC 延迟激增）。 */
        if (w->cpu != cpu_index() && g_percpu[w->cpu].in_idle) {
            lapic_send_ipi((uint8_t)g_percpu[w->cpu].lapic_id, IPI_RESCHED);
        }
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
    /* M5 修复：消息队列长度上限，避免恶意/失控任务狂发耗尽内核堆。
     * 超出则拒绝投递并返回 NO_BUFFER，形成背压（发送方收到错误后可重试/
     * 限流），而非无界增长。 */
    if (p->queue_len >= PORT_QUEUE_MAX) {
        irq_restore(f);
        kfree(m);
        return MACH_SEND_NO_BUFFER;
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
        /* 阻塞：登记等待 -> 解锁（保持关中断）-> 调度让出（见 helper 注释） */
        port_wait_enqueue(p);
        port_block_and_yield(f);
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
    /* H4 修复：单调分配前先检查是否越过窗口上界；越界则返回 0（失败哨兵，
     * 因合法映射基址恒 >= OOL_RECV_BASE 非 0），由调用方 sys_mach_msg 释放
     * 消息并返回错误，而非让 g_ool_bump 无限增长撞区。 */
    base = OOL_RECV_BASE + g_ool_bump;
    if (base + need > OOL_RECV_LIMIT) {
        irq_restore(f);
        kprintf("[ipc] OOL recv window exhausted (bump=%p limit=%p)\n",
                (void *)(uintptr_t)g_ool_bump, (void *)(uintptr_t)OOL_RECV_LIMIT);
        return 0;
    }
    g_ool_bump += need;
    irq_restore(f);

got:
    for (uint32_t i = 0; i < m->ool_page_count; i++) {
        vmm_map_page(cr3, base + (uint64_t)i * PAGE_SIZE, m->ool_pages[i],
                     PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_NX | PTE_OOL);
    }
    return base;
}

/* ---- 内核侧 OOL 接收（供 execve 等内核客户端读文件） ---- */
uint64_t ipc_recv_ool_kernel(uint32_t port_name, void *inline_buf,
                             uint32_t inline_cap, uint32_t *inline_out,
                             void *ool_buf, uint32_t ool_cap,
                             uint32_t *ool_out, bool block)
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
            uint32_t n = m->size < inline_cap ? m->size : inline_cap;
            if (inline_buf) {
                memcpy(inline_buf, m->data, n);
            }
            if (inline_out) {
                *inline_out = n;
            }
            uint32_t got = 0;
            if (m->has_ool) {
                for (uint32_t i = 0; i < m->ool_page_count; i++) {
                    uint64_t pa = m->ool_pages[i];
                    uint8_t *kva = (uint8_t *)PHYS_TO_VIRT(pa);
                    uint32_t rem = ool_cap - got;
                    if (rem == 0) {
                        break;
                    }
                    uint32_t chunk = PAGE_SIZE < rem ? PAGE_SIZE : rem;
                    if (ool_buf) {
                        memcpy(ool_buf + got, kva, chunk);
                    }
                    got += chunk;
                    pmm_decref((void *)pa);     /* 内核消费，释放 OOL 引用 */
                }
            }
            if (ool_out) {
                *ool_out = got;
            }
            kfree(m);
            return MACH_MSG_SUCCESS;
        }
        if (!block) {
            irq_restore(f);
            return MACH_RCV_TIMED_OUT;
        }
        port_wait_enqueue(p);
        port_block_and_yield(f);
    }
}

void port_free(uint32_t name)
{
    kernel_port_t *p = port_lookup(name);
    if (p) {
        memset(p, 0, sizeof(*p));
        p->in_use = false;
    }
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
            port_block_and_yield(f);
        }
        /* M6 修复：接收侧防御性校验。
         * - recv_limit 至少须容纳消息头，否则无法安全拷出 → 拒绝。
         * - 若消息携带 OOL，重验 ool_page_count 在合法区间内，防止 SEND 侧
         *   ool_capture 之外的任何伪造/损坏导致越界映射或越界访问 ool_pages[]。
         *   （ool_pages[] 定容 MACH_MSG_OOL_MAX_PAGES，越界即数组越界读。） */
        if (recv_limit < sizeof(mach_msg_header_t)) {
            kfree(m);
            return MACH_RCV_INVALID_NAME;
        }
        if (m->has_ool) {
            uint32_t pc = (m->ool_page_count > MACH_MSG_OOL_MAX_PAGES)
                          ? MACH_MSG_OOL_MAX_PAGES : m->ool_page_count;
            if (m->ool_page_count == 0 ||
                m->ool_page_count > MACH_MSG_OOL_MAX_PAGES) {
                for (uint32_t i = 0; i < pc; i++) {
                    pmm_decref((void *)m->ool_pages[i]);
                }
                kfree(m);
                return MACH_RCV_NO_SPACE;
            }
        }
        if (m->size > recv_limit) {
            kfree(m);                      /* 超限即丢弃 */
            return MACH_RCV_TOO_LARGE;
        }
        /* OOL：映射进接收方并改写描述符为接收方 VA；登记到任务 OOL 链表（A3） */
        if (m->has_ool) {
            uint64_t va = ool_map_into_receiver(m);
            if (va == 0) {                       /* H4：窗口耗尽，映射失败 */
                kfree(m);
                return MACH_RCV_NO_SPACE;
            }
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
