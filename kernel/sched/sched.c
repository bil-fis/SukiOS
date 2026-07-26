/*
 * kernel/sched/sched.c
 * -----------------------------------------------------------------------------
 * 抢占式轮转调度器 (Round-Robin)。
 *
 * 模型：所有可运行任务组成循环链表。PIT (IRQ0) 每个节拍调用 sched_tick()，
 * 递减当前任务时间片；耗尽即切换到下一个就绪任务。任务也可 task_yield()
 * 主动让出。上下文切换由 switch.S 的 context_switch() 完成。
 *
 * 注意：schedule() 在关中断下执行上下文切换。IRQ 路径中 EOI 已由
 * isr_dispatch 在调用处理器之前发送（见 idt.c），避免切走后 EOI 丢失。
 *
 * 调用关系：kmain -> sched_init/task_create_kernel; IRQ0 -> sched_tick -> schedule。
 */
#include <kernel/task.h>
#include <kernel/interrupts.h>
#include <kernel/gdt.h>
#include <kernel/string.h>
#include <kernel/console.h>
#include <mm/kmalloc.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <ipc/port.h>

#define KSTACK_SIZE  16384

/* 用户程序装载布局 */
#define USER_CODE_BASE   0x0000000000400000UL
#define USER_STACK_TOP   0x00007FFFFFFFF000UL
#define USER_STACK_PAGES 4
/* 代码/数据 W^X 边界：链接脚本把 .text/.rodata 放在 [0, USER_DATA_SPLIT)，
 * 把 .data/.bss 放在 [USER_DATA_SPLIT, ...)。内核据此分别映射为 RX 与 RW+NX。 */
#define USER_DATA_SPLIT  0x0000000000100000UL   /* 1 MiB */

extern void context_switch(uint64_t *old_rsp_save, uint64_t new_rsp);
extern void task_trampoline(void);
extern void enter_user_mode(uint64_t rip, uint64_t rsp);  /* syscall_entry.S */
extern uint64_t g_syscall_kstack;                          /* syscall_init.c */

static task_t  *g_current;
static uint64_t g_next_pid = 0;
static uint32_t g_task_count = 0;
static task_t  *g_dead_list = NULL;   /* 待回收的已退出任务（B1 项） */

/* 保存/恢复 IF 的临界区原语：与无条件 sti 不同，嵌套调用安全 */
static inline uint64_t irq_save(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void irq_restore(uint64_t flags)
{
    if (flags & (1UL << 9)) {
        __asm__ volatile("sti" ::: "memory");
    }
}

uint64_t sched_next_pid(void) { return g_next_pid++; }
task_t *sched_current(void)   { return g_current; }

void sched_init(void)
{
    /* 将当前引导执行流封装为 task0（idle/boot 线程） */
    task_t *t0 = (task_t *)kzalloc(sizeof(task_t));
    t0->id = sched_next_pid();
    t0->state = RUNNING;
    t0->cr3 = vmm_kernel_pml4();
    t0->priority = 0;
    t0->ticks_remaining = TIME_SLICE_TICKS;
    t0->is_user = false;
    t0->alive = true;
    strncpy(t0->name, "idle", sizeof(t0->name) - 1);
    t0->next = t0;                 /* 循环链表自环 */
    g_current = t0;
    g_task_count = 1;
    kprintf("[sched] scheduler initialized, task0='idle' pid=%lu\n",
            (unsigned long)t0->id);
}

task_t *task_create_kernel(void (*entry)(void *), void *arg, const char *name)
{
    task_t *t = (task_t *)kzalloc(sizeof(task_t));
    if (!t) {
        return NULL;
    }
    void *stack = kmalloc(KSTACK_SIZE);
    if (!stack) {
        kfree(t);
        return NULL;
    }
    t->id = sched_next_pid();
    t->state = READY;
    t->cr3 = vmm_kernel_pml4();
    t->priority = 128;
    t->ticks_remaining = TIME_SLICE_TICKS;
    t->is_user = false;
    t->alive = true;
    t->kstack_base = (uint64_t)stack;
    t->kstack_top  = (uint64_t)stack + KSTACK_SIZE;
    strncpy(t->name, name ? name : "kthread", sizeof(t->name) - 1);

    /* 构造初始内核栈帧，令首次 context_switch 落到 task_trampoline */
    uint64_t *sp = (uint64_t *)t->kstack_top;
    *(--sp) = (uint64_t)task_trampoline;   /* ret 地址 */
    *(--sp) = 0;                           /* rbx */
    *(--sp) = 0;                           /* rbp */
    *(--sp) = (uint64_t)entry;             /* r12 = entry */
    *(--sp) = (uint64_t)arg;               /* r13 = arg */
    *(--sp) = 0;                           /* r14 */
    *(--sp) = 0;                           /* r15 */
    t->rsp = (uint64_t)sp;

    /* 插入循环就绪链表（g_current 之后）。
     * 用 irq_save/restore 而非无条件 sti：task_create_user 需要在关中断
     * 下调用本函数并在补写 cr3/user_rip 等字段后才允许调度器看到新任务，
     * 否则定时器可能在字段就绪前抢占并以垃圾 user_rip 进入 Ring3（竞态）。 */
    uint64_t f = irq_save();
    t->next = g_current->next;
    g_current->next = t;
    g_task_count++;
    irq_restore(f);

    kprintf("[sched] created task '%s' pid=%lu stack=%p\n",
            t->name, (unsigned long)t->id, stack);
    return t;
}

/* 轻量熵源：TSC 低位混合乘散列（C3 项：用户栈 ASLR）。
 * 注：平坦二进制按固定 VA 链接，代码基址无法随机化；栈顶可随机下移
 * 0..255 页（最多 ~1MiB），提升 ROP/栈喷射攻击成本。 */
static uint64_t aslr_random(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t x = ((uint64_t)hi << 32) | lo;
    x ^= x >> 33; x *= 0xFF51AFD7ED558CCDUL; x ^= x >> 33;
    return x;
}

/* Ring3 任务的内核侧入口：设置本任务内核栈后经 iretq 落入用户态 */
static void user_task_thunk(void *arg)
{
    task_t *t = (task_t *)arg;
    interrupts_disable();              /* iretq 前的窗口保持原子 */
    tss_set_rsp0(t->kstack_top);
    g_syscall_kstack = t->kstack_top;
    /* RFLAGS.IF=1 由 enter_user_mode 的 iretq 帧恢复 */
    enter_user_mode(t->user_rip, t->user_stack_top);
    /* 不可达 */
}

task_t *task_create_user(const void *blob, size_t size, const char *name)
{
    /* 1. 独立地址空间（共享内核高半区） */
    uint64_t as = vmm_create_address_space();
    if (!as) {
        return NULL;
    }

    /* 2. 按 W^X 拆分装载：.text/.rodata -> RX；.data/.bss -> RW+NX。
     * 链接脚本已将两部分以 USER_DATA_SPLIT(1MiB) 为界分开（C1 项）。 */
    size_t split = (size < USER_DATA_SPLIT) ? size : USER_DATA_SPLIT;

    /* 代码/只读区：RX */
    for (size_t off = 0; off < split; off += PAGE_SIZE) {
        void *phys = pmm_alloc_page();
        if (!phys) {
            goto fail;                        /* 回滚：销毁已建地址空间（B2 项） */
        }
        uint8_t *kva = (uint8_t *)PHYS_TO_VIRT(phys);
        size_t chunk = (size - off) > PAGE_SIZE ? PAGE_SIZE : (size - off);
        if (chunk) {
            memcpy(kva, (const uint8_t *)blob + off, chunk);
        }
        if (chunk < PAGE_SIZE) {
            memset(kva + chunk, 0, PAGE_SIZE - chunk);
        }
        vmm_map_page(as, USER_CODE_BASE + off, (uint64_t)phys,
                     PTE_PRESENT | PTE_USER);          /* 可执行、不可写 */
    }

    /* 数据/BSS 区：RW + NX。
     * 链接脚本把 .data/.bss 放到 1MiB 边界之后；objcopy -O binary 若 .data
     * 为空则 bin 只含 text/rodata（size < 1MiB），此时数据区完全靠零页。
     * 无论哪种情况，都必须为 [USER_DATA_SPLIT, USER_DATA_SPLIT+data+bss)
     * 建立映射，否则用户程序访问静态缓冲即缺页。bss/堆保留 256KiB。 */
    {
        size_t data_bytes = (size > split) ? (size - split) : 0;
        size_t region = data_bytes + 256 * 1024;             /* data + bss/heap */
        for (size_t off = 0; off < region; off += PAGE_SIZE) {
            void *phys = pmm_alloc_page();
            if (!phys) {
                goto fail;
            }
            uint8_t *kva = (uint8_t *)PHYS_TO_VIRT(phys);
            if (off < data_bytes) {                          /* 拷贝 .data 内容 */
                size_t chunk = (data_bytes - off) > PAGE_SIZE
                               ? PAGE_SIZE : (data_bytes - off);
                memcpy(kva, (const uint8_t *)blob + split + off, chunk);
                if (chunk < PAGE_SIZE) {
                    memset(kva + chunk, 0, PAGE_SIZE - chunk);
                }
            } else {
                memset(kva, 0, PAGE_SIZE);                   /* .bss/堆零页 */
            }
            vmm_map_page(as, USER_CODE_BASE + USER_DATA_SPLIT + off,
                         (uint64_t)phys,
                         PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_NX);
        }
    }

    /* 3. 用户栈：RW + NX，栈顶 ASLR 随机下移 0..255 页（C3 项） */
    uint64_t stack_top = USER_STACK_TOP - (aslr_random() & 0xFF) * PAGE_SIZE;
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        void *phys = pmm_alloc_page();
        if (!phys) {
            goto fail;
        }
        memset(PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
        vmm_map_page(as, stack_top - (uint64_t)(i + 1) * PAGE_SIZE,
                     (uint64_t)phys,
                     PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_NX);
    }

    /* 4. 复用内核任务骨架，入口为 user_task_thunk。
     * 整段关中断：task_create_kernel 将任务插入就绪链表后，若定时器在
     * cr3/user_rip 补写完成前抢占并调度该任务，thunk 将以 arg=NULL 运行，
     * 从物理页 0（IVT）读出垃圾 user_rip 直接三重故障。 */
    uint64_t f = irq_save();
    task_t *t = task_create_kernel(user_task_thunk, NULL, name);
    if (!t) {
        irq_restore(f);
        goto fail;
    }
    t->is_user = true;
    t->cr3 = as;
    t->user_rip = USER_CODE_BASE;
    t->user_stack_top = stack_top;
    /* 修正跳板参数：r13 槽（arg）指向任务自身（见 task_create_kernel 栈帧布局） */
    ((uint64_t *)t->rsp)[2] = (uint64_t)t;   /* [r15,r14,r13,...] 从栈顶起序 2 = r13 */
    irq_restore(f);

    kprintf("[sched] user task '%s' pid=%lu cr3=%p entry=%p (W^X)\n",
            t->name, (unsigned long)t->id, (void *)as, (void *)USER_CODE_BASE);
    return t;

fail:
    /* B2 项：中途失败，销毁已建地址空间（其页表遍历会释放已映射的全部自有页） */
    vmm_destroy_address_space(as);
    return NULL;
}

/* 选择 g_current 之后的下一个存活可运行任务 */
static task_t *pick_next(void)
{
    task_t *t = g_current->next;
    for (uint32_t i = 0; i < g_task_count + 1; i++) {
        if (t->alive && (t->state == READY || t->state == RUNNING)) {
            return t;
        }
        t = t->next;
    }
    return g_current;   /* 回退：无其它可运行任务 */
}

/* 回收所有已退出任务的 task 结构与内核栈（B1 项）。资源（地址空间、
 * OOL 页）已在 task_exit_current 中释放，这里仅释放无法在自身栈上释放的部分。
 * 必须在上下文切换离开该任务后进行（此时其内核栈已不再使用）。 */
static void reap_dead(void)
{
    task_t *t = g_dead_list;
    g_dead_list = NULL;
    while (t) {
        task_t *nx = t->dead_next;
        kfree((void *)t->kstack_base);
        kfree(t);
        t = nx;
    }
}

/* 执行一次调度（调用时须处于关中断状态） */
void schedule(void)
{
    task_t *prev = g_current;
    task_t *next = pick_next();
    if (next == prev) {
        prev->ticks_remaining = TIME_SLICE_TICKS;
        return;
    }

    /* FPU/SSE 上下文保存与恢复（D2 项） */
    fpu_fxsave(&prev->fpu_state);
    if (next->fpu_valid) {
        fpu_fxrstor(&next->fpu_state);
    } else {
        fpu_fninit();
        next->fpu_valid = true;
    }

    if (prev->state == RUNNING) {
        prev->state = READY;
    }
    next->state = RUNNING;
    next->ticks_remaining = TIME_SLICE_TICKS;
    g_current = next;

    /* 为可能的 Ring3->Ring0 切换设置内核栈；必要时切换地址空间 */
    tss_set_rsp0(next->kstack_top);
    g_syscall_kstack = next->kstack_top;
    if (next->cr3 != prev->cr3) {
        vmm_switch(next->cr3);
    }

    context_switch(&prev->rsp, next->rsp);

    /* 切换完成后，回收此前已退出任务的残留资源（B1 项） */
    reap_dead();
}

/* 由 PIT IRQ0 调用（覆盖 pit.c 的弱符号） */
void sched_tick(registers_t *r)
{
    (void)r;
    if (!g_current) {
        return;
    }
    if (g_current->ticks_remaining > 0) {
        g_current->ticks_remaining--;
    }
    if (g_current->ticks_remaining == 0) {
        schedule();   /* 中断上下文，IF 已关 */
    }
}

void task_yield(void)
{
    interrupts_disable();
    schedule();
    interrupts_enable();
}

/* 从就绪环移除一个任务 */
static void unlink_task(task_t *t)
{
    task_t *p = t->next;
    while (p->next != t) {
        p = p->next;
    }
    p->next = t->next;
    g_task_count--;
}

__attribute__((noreturn)) void task_exit_current(void)
{
    interrupts_disable();
    task_t *t = g_current;
    kprintf("[sched] task '%s' pid=%lu exited\n",
            t->name, (unsigned long)t->id);

    /* 释放本任务持有的 IPC OOL 共享页引用与映射区间（A3 项） */
    port_reap_ool(t);
    /* 释放地址空间：用户自有物理页 + 全部页表结构（B1 项）。
     * 关键顺序：必须先把 CR3 切回内核 PML4，再销毁旧地址空间——
     * 否则 destroy 会释放 CR3 正指向的 PML4 页（悬空根页表）。
     * OOL 共享页仅解除映射（PTE_OOL），其物理页由上面的 decref 释放。 */
    if (t->is_user && t->cr3 && t->cr3 != vmm_kernel_pml4()) {
        vmm_switch(vmm_kernel_pml4());
        vmm_destroy_address_space(t->cr3);
    }
    t->cr3 = 0;

    /* 入死亡链表，待下次调度由 reap_dead() 回收 task 结构与内核栈 */
    t->alive = false;
    t->state = BLOCKED;
    t->dead = true;
    t->dead_next = g_dead_list;
    g_dead_list = t;

    task_t *next = pick_next();
    unlink_task(t);
    next->state = RUNNING;
    next->ticks_remaining = TIME_SLICE_TICKS;
    g_current = next;
    tss_set_rsp0(next->kstack_top);
    g_syscall_kstack = next->kstack_top;

    /* FPU/SSE 保存退出任务、恢复下一任务（D2 项） */
    fpu_fxsave(&t->fpu_state);
    if (next->fpu_valid) {
        fpu_fxrstor(&next->fpu_state);
    } else {
        fpu_fninit();
        next->fpu_valid = true;
    }

    if (next->cr3) {
        vmm_switch(next->cr3);
    }

    context_switch(&t->rsp, next->rsp);   /* 一去不返 */
    for (;;) { __asm__ volatile("hlt"); }
}
