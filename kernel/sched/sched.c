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
#include <kernel/elf.h>
#include <ipc/port.h>

#define KSTACK_SIZE  16384
#define MAX_TASKS       256
/* M7 修复：内核栈底守卫哨兵。任务内核栈从高地址向下增长，栈底写入哨兵；
 * 若向下溢出破坏相邻堆块，哨兵会被覆盖。每次调度前校验当前任务栈底哨兵。 */
#define KSTACK_CANARY   0xCDC1FEEDDEADBEEFUL

/* 用户程序装载布局 */
#define USER_CODE_BASE   0x0000000000400000UL
#define USER_STACK_TOP   0x00007FFFFFFFF000UL
/* 32 页 = 128KiB 用户栈：playaudio 的 minimp3 解码在栈上使用较大的
 * scratch（grbuf/syn/qmf 等约数十 KB），4 页会栈溢出触发 #PF。 */
#define USER_STACK_PAGES 32
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
static task_t  *g_all_tasks = NULL;   /* 全局任务表（含 zombie），供 pid 查找 */

/* 指向“当前运行任务”的 scr_rip/scr_rsp 字段的指针（syscall_entry.S 用）。
 * 必须在每次上下文切换到某任务后指向该任务的 scr_rip，以保证 syscall
 * 返回帧使用的 RIP/RSP 属于正确任务（避免全局暂存被其它任务覆盖）。 */
uint64_t *g_scratch = NULL;

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
    g_scratch = &t0->scr_rip;      /* 初始指向 idle 的返回暂存 */
    g_task_count = 1;
    kprintf("[sched] scheduler initialized, task0='idle' pid=%lu\n",
            (unsigned long)t0->id);
}

task_t *task_create_kernel(void (*entry)(void *), void *arg, const char *name)
{
    /* M7 修复：任务数硬上限。无上限时 PID/物理页耗尽无保护，恶意/失控 spawn
     * 会拖垮整系统。达到上限即拒绝创建。 */
    if (g_task_count >= MAX_TASKS) {
        return NULL;
    }
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
    *(uint64_t *)stack = KSTACK_CANARY;   /* M7：内核栈底守卫哨兵 */
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
    t->all_next = g_all_tasks;       /* 链入全局表，供 sys_wait 按 PID 查找 */
    g_all_tasks = t;
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

task_t *task_create_user_args(const void *elf, size_t size,
                                int argc, const char *const argv[],
                                int envc, const char *const envp[],
                                const char *name)
{
    /* 1. 独立地址空间（共享内核高半区） */
    uint64_t as = vmm_create_address_space();
    if (!as) {
        return NULL;
    }

    /* 2. 用 ELF 加载器装载段 + 构造初始栈（W^X，栈 ASLR，含 argc/argv/envp）。
     * 取代旧的“平坦二进制按 1MiB 拆分”装载：现在内核直接解析 ELF64，
     * 支持 ET_EXEC / ET_DYN(PIE+ASLR)、清零 BSS、按 W^X 设权限、建立
     * 含 argc/argv/envp/auxv 的初始用户栈。 */
    elf_load_result_t res;
    uint64_t stack_top = USER_STACK_TOP - (aslr_random() & 0xFF) * PAGE_SIZE;
    if (!elf_load(as, elf, size, argc, argv, envc, envp, stack_top,
                  USER_STACK_PAGES, &res)) {
        vmm_destroy_address_space(as);     /* 回滚（B2 项） */
        return NULL;
    }

    /* 3. 复用内核任务骨架，入口为 user_task_thunk。
     * 整段关中断：task_create_kernel 将任务插入就绪链表后，若定时器在
     * cr3/user_rip 补写完成前抢占并调度该任务，thunk 将以 arg=NULL 运行，
     * 从物理页 0（IVT）读出垃圾 user_rip 直接三重故障。 */
    uint64_t f = irq_save();
    task_t *t = task_create_kernel(user_task_thunk, NULL, name);
    if (!t) {
        irq_restore(f);
        vmm_destroy_address_space(as);
        return NULL;
    }
    t->is_user = true;
    t->cr3 = as;
    t->user_rip = res.entry;
    t->user_stack_top = res.stack_top;
    /* 修正跳板参数：r13 槽（arg）指向任务自身（见 task_create_kernel 栈帧布局） */
    ((uint64_t *)t->rsp)[2] = (uint64_t)t;   /* [r15,r14,r13,...] 从栈顶起序 2 = r13 */
    irq_restore(f);

    kprintf("[sched] user task '%s' pid=%lu cr3=%p entry=%p (ELF, W^X)\n",
            t->name, (unsigned long)t->id, (void *)as, (void *)res.entry);
    return t;
}

task_t *task_create_user(const void *elf, size_t size, const char *name)
{
    /* 启动期装载：无参数（argc=0） */
    return task_create_user_args(elf, size, 0, NULL, 0, NULL, name);
}

/* 按 PID 在全局任务表中查找（含尚未回收的 zombie）。单核：调用方须自保证
 * 在关中断窗口内访问以避免与 reap_dead 的摘除产生竞态。 */
task_t *task_lookup(uint64_t pid)
{
    task_t *t = g_all_tasks;
    while (t) {
        if (t->id == pid) {
            return t;
        }
        t = t->all_next;
    }
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

/* 立即回收一个已退出（zombie）任务：从死亡链表与全局任务表摘除并释放其
 * task 结构与内核栈。调用方须处于关中断临界区且持有有效指针。供 sys_wait
 * 在父任务首次回收 zombie 时调用，避免二次 wait 命中残留结构而永久阻塞，
 * 也避免僵尸一直占用内存。 */
void task_reap(task_t *t)
{
    /* 从死亡链表摘除 */
    if (g_dead_list == t) {
        g_dead_list = t->dead_next;
    } else {
        task_t *p = g_dead_list;
        while (p && p->dead_next != t) {
            p = p->dead_next;
        }
        if (p) {
            p->dead_next = t->dead_next;
        }
    }
    /* 从全局任务表摘除 */
    if (g_all_tasks == t) {
        g_all_tasks = t->all_next;
    } else {
        task_t *q = g_all_tasks;
        while (q && q->all_next != t) {
            q = q->all_next;
        }
        if (q) {
            q->all_next = t->all_next;
        }
    }
    kfree((void *)t->kstack_base);
    kfree(t);
}

/* 回收所有已退出任务的 task 结构与内核栈（B1 项）。资源（地址空间、
 * OOL 页）已在 task_exit_current 中释放，这里仅释放无法在自身栈上释放的部分。
 * 必须在上下文切换离开该任务后进行（此时其内核栈已不再使用）。
 * 同时把任务从全局表 g_all_tasks 摘除，避免 sys_wait 后续查到悬空指针。 */
static void reap_dead(void)
{
    task_t *t = g_dead_list;
    g_dead_list = NULL;
    while (t) {
        task_t *nx = t->dead_next;
        /* 从全局任务表摘除（本函数运行于关中断的 schedule() 内，单核安全） */
        if (g_all_tasks == t) {
            g_all_tasks = t->all_next;
        } else {
            task_t *p = g_all_tasks;
            while (p && p->all_next != t) {
                p = p->all_next;
            }
            if (p) {
                p->all_next = t->all_next;
            }
        }
        kfree((void *)t->kstack_base);
        kfree(t);
        t = nx;
    }
}

/* 执行一次调度（调用时须处于关中断状态） */
void schedule(void)
{
    /* M7 修复：内核栈溢出守卫。任务内核栈从高地址向下增长，栈底 8 字节写入
     * 哨兵；若向下溢出破坏了相邻堆块，哨兵会被覆盖。每次调度前校验当前任务
     * 栈底哨兵，遭破坏即 panic，把"静默内存损坏"转为可诊断的崩溃，而非任其
     * 蔓延污染其它任务。idle 任务(t0)无独立内核栈(kstack_base==0)，跳过。 */
    if (g_current && g_current->kstack_base) {
        if (*(const uint64_t *)g_current->kstack_base != KSTACK_CANARY) {
            panic("kernel stack overflow detected (task '%s' pid=%lu)",
                  g_current->name, (unsigned long)g_current->id);
        }
    }
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
    g_scratch = &next->scr_rip;   /* 当前任务切换：暂存指针同步 */

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

__attribute__((noreturn)) void task_exit_current(uint64_t code)
{
    interrupts_disable();
    task_t *t = g_current;
    kprintf("[sched] task '%s' pid=%lu exited (code=%lu)\n",
            t->name, (unsigned long)t->id, (unsigned long)code);

    /* 唤醒所有阻塞在 sys_wait 本任务的父任务（及等待者）：把退出码写入
     * 各自的 wait_result 并置 READY。必须在 t->waiters 仍有效时（尚未释放
     * task 结构）完成；task 结构在后续 reap_dead 才释放，故等待者读取
     * wait_result（自身字段）绝不触发悬空访问。 */
    {
        task_t *w = t->waiters;
        while (w) {
            task_t *wn = w->wait_link;
            w->wait_result = code;
            w->wait_link   = NULL;
            w->state       = READY;
            w = wn;
        }
        t->waiters = NULL;
    }
    t->exit_code = code;
    t->zombie    = true;     /* 等待父任务 sys_wait 回收（仍留在 g_all_tasks） */

    /* 释放本任务认领的端口所有权（避免下个 app 认领同名端口被悬空 owner 拒绝） */
    port_release_owner(t);
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
    g_scratch = &next->scr_rip;   /* 当前任务切换：暂存指针同步 */
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
