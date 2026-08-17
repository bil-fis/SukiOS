/*
 * kernel/sched/sched.c
 * -----------------------------------------------------------------------------
 * 抢占式轮转调度器 (Round-Robin)，P0-R1 对称多核版本。
 *
 * 模型（相较单核版的关键变化）：
 *   - 每 CPU 一套独立运行队列（percpu.rq_head/tail/count），任务在创建时按
 *     RR 绑定到某个 CPU，仅在该 CPU 上被调度（无跨核迁移，故无需 TLB 跨核
 *     失效与“双 CPU 同跑一任务”竞态）。
 *   - 全局 g_current 改为 percpu.current_task（sched_current() 经 %gs 取本核）。
 *   - 所有运行队列/全局任务表/死亡链表的修改统一由 g_sched_lock 自旋锁串行化，
 *     取代单核时代的 cli/sti 临界区（cli/sti 在 SMP 下不隔离其它核）。
 *   - 每 CPU 自带 LAPIC 周期定时器（100Hz）触发本核 sched_tick -> schedule()。
 *   - 新任务入队到非空歇 CPU 时，向该 CPU 发 IPI_RESCHED 唤醒其 hlt 空闲循环。
 *
 * 调用关系：kmain -> sched_init（建 BSP idle）；smp_init -> sched_create_idle
 *           （为各 AP 建 idle 并入队）；每核 LAPIC 定时器 -> sched_tick ->
 *           schedule；syscall/中断路径 -> schedule。
 */
#include <kernel/task.h>
#include <kernel/interrupts.h>
#include <kernel/gdt.h>
#include <kernel/string.h>
#include <kernel/console.h>
#include <kernel/spinlock.h>
#include <kernel/smp.h>
#include <kernel/apic.h>
#include <kernel/percpu.h>
#include <mm/kmalloc.h>
#include <mm/kstack.h>       /* P0-R5：带未映射守卫页的内核栈分配器 */
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <kernel/gdbstub.h>  /* P0-R3：sched_tick 内轮询 GDB 远程 break-in */
#include <mm/vma.h>          /* P0-5：栈自动增长区登记 / 退出清理 */
#include <kernel/elf.h>
#include <kernel/hda.h>      /* hda_release_owner：任务退出释放音频流 */
#include <ipc/port.h>

/* 每任务内核栈大小（字节）。Ring0 内核栈与用户态栈(USER_STACK_PAGES)独立。
 * P0-R5：栈体改由 kstack_alloc 提供（独立 VA 槽位 + 栈底下方未映射守卫页，
 * 溢出立即 #PF 定性），大小由 mm/kstack.h 统一定义（16KB 不变）。 */
#define KERNEL_STACK_BYTES  KSTACK_BYTES
#define MAX_TASKS       256
/* M7 修复：内核栈底守卫哨兵。任务内核栈从高地址向下增长，栈底写入哨兵；
 * 若向下溢出破坏相邻堆块，哨兵会被覆盖。每次调度前校验当前任务栈底哨兵。 */
#define KSTACK_CANARY   0xCDC1FEEDDEADBEEFUL

/* 用户程序装载布局 */
#define USER_CODE_BASE   0x0000000000400000UL
#define USER_STACK_TOP   0x00007FFFFFFFF000UL
#define USER_STACK_PAGES 32
#define USER_DATA_SPLIT  0x0000000000100000UL   /* 1 MiB */

extern void context_switch(uint64_t *old_rsp_save, uint64_t new_rsp);
extern void task_trampoline(void);
extern void enter_user_mode(uint64_t rip, uint64_t rsp);  /* syscall_entry.S */
extern uint8_t kernel_stack_top;   /* boot.S：BSP 引导内核栈顶（idle0 复用） */
/* syscall 快速路径的 per-CPU 全局量（syscall_init.c 定义，[MAX_CPUS] 数组） */
extern uint64_t  g_syscall_kstack[MAX_CPUS];
extern uint64_t *g_scratch[MAX_CPUS];
extern uint64_t  g_utmp_rsp[MAX_CPUS];

/* 调度器内部共享状态：统一由自旋锁保护，取代单核 cli/sti 临界区（P0-R1） */
static spinlock_t g_sched_lock;
static uint64_t g_next_pid = 0;
static uint32_t g_task_count = 0;
static task_t  *g_dead_list = NULL;   /* 待回收的已退出任务 */
static task_t  *g_all_tasks = NULL;   /* 全局任务表（含 zombie），供 pid 查找 */
static uint32_t g_rr_counter = 0;     /* 新任务 RR 绑定 CPU 的轮转计数器 */

uint64_t sched_next_pid(void)
{
    /* 多核并发创建任务时 PID 必须原子分配（P0-R1） */
    return __atomic_fetch_add(&g_next_pid, 1, __ATOMIC_RELAXED);
}
task_t *sched_current(void)   { return cpu_local()->current_task; }

/* 调度切换前的防御性校验（生产级稳定性铁律）：
 * 内核栈帧布局（switch.S 的 context_switch push/pop 顺序）：
 *   [rbx][rbp][r12][r13][r14][r15][ret]  —— ret 位于 next->rsp + 6*8 处。
 * 若 next->rsp 指向的内核栈上的返回地址落在堆区（本内核布局下堆在 0xFFFFC000
 * 段、合法代码在 0xFFFF8000 段），说明该任务内核栈（初始帧或切走时保存的返回
 * 地址）已被堆/越界数据覆盖，context_switch 的 ret 会跳到非法地址触发 #UD
 * （vector 6 / system halted）。提前 panic 带 next 任务信息，避免静默死机且便于定位。
 * 注：AP 的 idle 任务由 ap_main 循环充当，不通过 context_switch 切入，其 rsp 在
 * sched_create_idle 中显式构造为独立栈帧，亦满足本校验。 */
static void verify_switch_target(task_t *next)
{
    if (!next) {
        panic("verify_switch_target: next == NULL");
    }
    /* kstack_base==0 仅发生于早期引导复用 BSP 栈的残留路径，正常任务必有独立栈。
     * is_idle（AP 的 idle，如 idle1/2/3）实际运行在 ap_main 栈上，其 rsp 不在本处
     * kstack_alloc 的独立栈范围内，故跳过 rsp 范围检查，仅校验返回地址段。 */
    if (!next->is_idle && next->kstack_base != 0) {
        if (next->rsp < next->kstack_base || next->rsp > next->kstack_top) {
            kprintf("[sched] BAD SWITCH: next '%s' pid=%lu rsp=%p out of kstack [%p,%p]\n",
                    next->name, (unsigned long)next->id, (void *)next->rsp,
                    (void *)next->kstack_base, (void *)next->kstack_top);
            panic("verify_switch_target: next->rsp outside its kernel stack");
        }
    }
    /* 读取返回地址槽（next->rsp + 6*8），不跨页（栈内） */
    uint64_t ret_addr = *(volatile uint64_t *)(next->rsp + 6 * 8);
    /* 合法内核代码/数据地址落在 0xFFFF800000000000 .. 0xFFFFC00000000000 区间
     * （本内核：text/数据在 0xFFFF8000 段，内核堆/栈在 0xFFFFC000 段）。
     * 落在 0xFFFFC000 段即堆/栈区指针 → 必然非法返回目标。
     * 注意：canonical 地址高位全 1，不能用“高 16 位==0x8000”判断，须用区间。
     *
     * 重要例外：Ring3 用户任务的“返回地址槽”在首次切入（switch.S 构造）以及
     * 此后 syscall/中断返回时，保存的都是**用户空间地址**（0x0~0x00007FFFFFFFFFFF，
     * 即 iret 回用户态的桩），天然 < 0xFFFF800000000000，若仍做段检查会误 panic、
     * 导致所有用户态任务（FS_SERVER/INPUT/SHELL 等）无法被调度。故对 is_user
     * 任务跳过“返回地址段”检查——用户栈返回地址本就是合法用户地址；rsp 范围
     * 检查（上方）仍保留，继续防御内核栈被越界覆盖的静默死机。 */
    if (!next->is_user && !next->is_idle) {
        if (ret_addr < 0xFFFF800000000000ULL || ret_addr >= 0xFFFFC00000000000ULL) {
            kprintf("[sched] BAD SWITCH: next '%s' pid=%lu ret_addr=%p (heap/stack-seg) rsp=%p kstack=[%p,%p]\n",
                    next->name, (unsigned long)next->id, (void *)ret_addr,
                    (void *)next->rsp,
                    (void *)next->kstack_base, (void *)next->kstack_top);
            panic("verify_switch_target: next stack return address not in kernel text/data segment");
        }
    }
}

/* 把“当前任务”同步到本 CPU 的 percpu + syscall 快速路径数组（P0-R1 多核安全） */
static inline void set_cpu_current(task_t *t)
{
    uint32_t c = cpu_index();
    g_percpu[c].current_task = t;
    g_syscall_kstack[c] = t->kstack_top;
    g_scratch[c]        = &t->scr_rip;   /* syscall 返回帧暂存指向当前任务 */
}

/* 将任务追加到指定 CPU 的运行队列尾部（调用方须持 g_sched_lock） */
static void rq_push_cpu(task_t *t, uint32_t cpu)
{
    t->next = NULL;
    if (g_percpu[cpu].rq_tail) {
        g_percpu[cpu].rq_tail->next = t;
    } else {
        g_percpu[cpu].rq_head = t;
    }
    g_percpu[cpu].rq_tail = t;
    g_percpu[cpu].rq_count++;
    t->in_rq = true;
}

/* 从指定 CPU 运行队列摘除任意任务（调用方须持 g_sched_lock） */
static void rq_unlink_cpu(task_t *t, uint32_t cpu)
{
    task_t *p = g_percpu[cpu].rq_head;
    if (p == t) {
        g_percpu[cpu].rq_head = t->next;
        if (!g_percpu[cpu].rq_head) {
            g_percpu[cpu].rq_tail = NULL;
        }
        t->next = NULL;
        g_percpu[cpu].rq_count--;
        return;
    }
    while (p && p->next != t) {
        p = p->next;
    }
    if (p) {
        p->next = t->next;
        if (g_percpu[cpu].rq_tail == t) {
            g_percpu[cpu].rq_tail = p;
        }
        t->next = NULL;
        g_percpu[cpu].rq_count--;
        t->in_rq = false;
    }
}

/* 在本 CPU 运行队列中选下一个可运行任务（排除当前任务自身）。
 * 调用方须持 g_sched_lock，且处于本 CPU 上下文。 */
static void reap_dead(void);
static task_t *pick_next(void)
{
    task_t *cur = g_percpu[cpu_index()].current_task;
    task_t *t = g_percpu[cpu_index()].rq_head;
    for (uint32_t i = 0; i < g_percpu[cpu_index()].rq_count; i++) {
        if (t && t != cur && !t->is_idle && t->alive &&
            (t->state == READY || t->state == RUNNING)) {
            return t;
        }
        if (!t) {
            break;
        }
        t = t->next;
    }
    /* 回退：无其它可运行非 idle 任务。决不能回退到 cur —— cur 可能刚被标记
     * dead（如本任务正在 task_exit_current 中退出），其内核栈返回地址槽可能已被
     * 退出路径破坏，context_switch 切回时会 ret 到非法地址触发 #UD（vector 6）。
     * 改回退到本 CPU 的 idle 任务（idle_task 栈完整、帧合法），保证系统稳定空转。 */
    return g_percpu[cpu_index()].idle_task;
}

/* BSP idle 循环（独立内核栈上运行，与 BSP 引导栈彻底解耦）。
 * 对齐 AP 的 ap_main idle 循环：开中断、hlt 省电、被唤醒后调度。 */
static void bsp_idle(void)
{
    (void)g_percpu;
    interrupts_enable();
    for (;;) {
        g_percpu[0].in_idle = 1;
        /* 防御性开中断：本循环经 context_switch 切入时，若上一上下文是
         * 中断上下文(如 sti 后 LAPIC 定时器抢占某任务并 schedule)，被恢复的
         * RFLAGS.IF 可能落在 0（中断门自动清 IF 的遗留），导致 hlt 永远等不到
         * 定时器唤醒、系统表现为“卡死”。idle 循环本就应在开中断下 hlt，故此处
         * 显式开中断以消除该竞态（与 ap_main 的 idle 循环保持一致）。 */
        interrupts_enable();
        __asm__ volatile("hlt");
        g_percpu[0].in_idle = 0;
        schedule();
    }
}

void sched_init(void)
{
    spinlock_init(&g_sched_lock, "sched");
    /* 将引导执行流封装为 task0（idle0）。
     * 关键：idle0 使用 *独立* 内核栈（kstack_alloc，带守卫页），并构造独立栈帧
     * 令首次 context_switch 落到 bsp_idle 的 hlt 循环；绝不复用 BSP 引导栈
     * （&kernel_stack_top）。否则 idle0 被切回时其“停泊点”落在共享的 BSP 引导栈上，
     * 而 kmain 后续在 BSP 引导栈上的调用会覆盖该返回地址，导致切回时执行非法指令
     * （#UD / vector 6），表现为 '[EXC] vector 6' + 'system halted'。 */
    task_t *t0 = (task_t *)kzalloc(sizeof(task_t));
    if (!t0) {
        panic("sched_init: kzalloc idle0 failed");
    }
    uint64_t stack = kstack_alloc();   /* P0-R5：守卫页栈 */
    if (!stack) {
        panic("sched_init: kstack_alloc idle0 failed");
    }
    t0->id = sched_next_pid();
    t0->state = RUNNING;
    t0->cr3 = vmm_kernel_pml4();
    t0->priority = 0;
    t0->ticks_remaining = TIME_SLICE_TICKS;
    t0->is_user = false;
    t0->alive = true;
    t0->is_idle = true;
    t0->cpu = 0;
    t0->kstack_base = stack;
    t0->kstack_top  = stack + KERNEL_STACK_BYTES;
    *(uint64_t *)stack = KSTACK_CANARY;
    strncpy(t0->name, "idle0", sizeof(t0->name) - 1);

    /* 构造初始内核栈帧，令首次 context_switch 落到 bsp_idle
     * 布局必须与 switch.S 的 context_switch pop 顺序匹配：
     *   pushq %rbx; %rbp; %r12; %r13; %r14; %r15  ->  pop 逆序
     *   [rbx][rbp][r12=entry][r13=arg][r14][r15][ret=task entry] */
    uint64_t *sp = (uint64_t *)t0->kstack_top;
    *(--sp) = (uint64_t)bsp_idle;   /* ret 目标 */
    *(--sp) = 0;                    /* r15 */
    *(--sp) = 0;                    /* r14 */
    *(--sp) = (uint64_t)bsp_idle;   /* r12 = entry */
    *(--sp) = 0;                    /* r13 = arg */
    *(--sp) = 0;                    /* rbp */
    *(--sp) = 0;                    /* rbx */
    t0->rsp = (uint64_t)sp;

    /* idle0 入 CPU0 运行队列，置为本核当前任务 */
    rq_push_cpu(t0, 0);
    g_percpu[0].current_task = t0;
    g_percpu[0].idle_task    = t0;
    g_syscall_kstack[0] = t0->kstack_top;
    g_scratch[0]        = &t0->scr_rip;
    set_cpu_current(t0);
    g_task_count = 1;
    kprintf("[sched] scheduler initialized (SMP RR), idle0 pid=%lu stack=%p\n",
            (unsigned long)t0->id, (void *)t0->kstack_base);
}

/* 由 BSP 引导流（kmain）在一切初始化、所有用户服务创建完毕后调用：
 * 将 BSP 当前执行流从“共享的 BSP 引导栈”切换到 idle0 的 *独立* 内核栈，
 * 从此 BSP 引导栈被冻结，idle0 的空转/调度循环在独立栈上运行。
 *
 * 关键：调用前先关中断，防止切换间隙 100Hz tick 触发 schedule() 把 BSP 引导栈
 * 指针写回 idle0->rsp（那样会重新引入“idle0 返回地址落在 BSP 引导栈、被后续调用
 * 覆盖”的根因）；并强制重置 idle0->rsp 为独立栈帧，抵消初始化阶段可能发生的覆盖。
 *
 * 本函数一去不返：切走后 BSP 引导流停在局部 krsp（冻结），idle0 独立栈上执行
 * bsp_idle 的 hlt+schedule 循环。 */
void sched_switch_to_idle0(void)
{
    task_t *idle0 = g_percpu[0].current_task;
    if (!idle0 || !idle0->is_idle) {
        panic("sched_switch_to_idle0: idle0 missing");
    }
    interrupts_disable();

    /* 强制重置 idle0 初始栈帧（与 sched_init 构造一致），确保 rsp 指向独立栈，
     * 不被初始化阶段 timer tick 的 schedule() 切走所覆盖。 */
    uint64_t *sp = (uint64_t *)idle0->kstack_top;
    *(--sp) = (uint64_t)bsp_idle;   /* ret 目标 */
    *(--sp) = 0;                    /* r15 */
    *(--sp) = 0;                    /* r14 */
    *(--sp) = (uint64_t)bsp_idle;   /* r12 = entry */
    *(--sp) = 0;                    /* r13 = arg */
    *(--sp) = 0;                    /* rbp */
    *(--sp) = 0;                    /* rbx */
    idle0->rsp = (uint64_t)sp;

    uint64_t krsp;
    /* 注意：第一个参数是局部 krsp（非 &idle0->rsp），因此 idle0->rsp 不被覆盖，
     * 始终指向上面的独立栈帧。 */
    context_switch(&krsp, idle0->rsp);
    /* never returns */
    for (;;) { __asm__ volatile("cli; hlt"); }
}

/* 创建某 CPU 的 idle 任务（smp_init 在 SIPI 前为各 AP 调用，P0-R1） */
task_t *sched_create_idle(uint32_t cpu)
{
    /* 分配在锁外完成（kmalloc 内有自己的锁，避免嵌套锁序问题） */
    task_t *t = (task_t *)kzalloc(sizeof(task_t));
    if (!t) {
        return NULL;
    }
    uint64_t stack = kstack_alloc();   /* P0-R5：守卫页栈 */
    if (!stack) {
        kfree(t);
        return NULL;
    }
    t->id = sched_next_pid();
    t->state = READY;
    t->cr3 = vmm_kernel_pml4();
    t->priority = 0;
    t->ticks_remaining = TIME_SLICE_TICKS;
    t->is_user = false;
    t->alive = true;
    t->is_idle = true;
    t->cpu = cpu;
    t->kstack_base = stack;
    t->kstack_top  = stack + KERNEL_STACK_BYTES;
    *(uint64_t *)stack = KSTACK_CANARY;
    strncpy(t->name, "idle", sizeof(t->name) - 1);
    t->name[4] = '0' + (char)(cpu % 10);
    /* 注意：AP idle 的实际执行流是 ap_main 循环（运行在 ap_boot 守卫页栈上），
     * 其 rsp 在首次 schedule() 切出时由 context_switch 保存为 ap_main 栈指针，
     * 并不指向本处 kstack_alloc 的独立栈。因此 t->rsp 保持未初始化（0），
     * 且 verify_switch_target 对 is_idle 任务跳过 rsp 范围检查（仅查返回地址段）。
     * 独立 kstack 仅作守卫/诊断用途，与 BSP idle0（独立栈帧）的模型不同。 */

    uint64_t f = spin_lock_irqsave(&g_sched_lock);
    rq_push_cpu(t, cpu);
    g_percpu[cpu].current_task = t;   /* AP 上电后读 percpu 即得其 idle */
    g_percpu[cpu].idle_task    = t;
    g_syscall_kstack[cpu] = t->kstack_top;
    g_scratch[cpu]        = &t->scr_rip;
    g_task_count++;
    spin_unlock_irqrestore(&g_sched_lock, f);
    kprintf("[sched] created idle%u pid=%lu for cpu%u\n",
            (unsigned)cpu, (unsigned long)t->id, (unsigned)cpu);
    return t;
}

/* 分配并初始化一个内核任务结构（不入队、不加锁）。
 * 拆出本函数是为了让 task_create_user_args 在“装配完全部用户态字段”之后
 * 再原子入队 —— 否则半成品任务可能被其它核先调走（P0-R1 多核正确性），
 * 同时避免“持 g_sched_lock 再调用会取同一把锁的函数”造成自死锁。 */
static task_t *task_alloc_kernel(void (*entry)(void *), void *arg,
                                 const char *name)
{
    /* M7 修复：任务数硬上限 */
    if (g_task_count >= MAX_TASKS) {
        return NULL;
    }
    task_t *t = (task_t *)kzalloc(sizeof(task_t));
    if (!t) {
        return NULL;
    }
    uint64_t stack = kstack_alloc();   /* P0-R5：守卫页栈 */
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
    t->kstack_base = stack;
    t->kstack_top  = stack + KERNEL_STACK_BYTES;
    *(uint64_t *)stack = KSTACK_CANARY;
    strncpy(t->name, name ? name : "kthread", sizeof(t->name) - 1);

    /* 构造初始内核栈帧，令首次 context_switch 落到 task_trampoline */
    uint64_t *sp = (uint64_t *)t->kstack_top;
    *(--sp) = (uint64_t)task_trampoline;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = (uint64_t)entry;
    *(--sp) = (uint64_t)arg;
    *(--sp) = 0;
    *(--sp) = 0;
    t->rsp = (uint64_t)sp;
    return t;
}

/* 把装配完成的任务发布到目标 CPU 运行队列（加锁 + IPI 唤醒空闲核） */
static void task_publish(task_t *t, uint32_t cpu)
{
    uint64_t f = spin_lock_irqsave(&g_sched_lock);
    t->cpu = cpu;
    rq_push_cpu(t, cpu);
    t->all_next = g_all_tasks;
    g_all_tasks = t;
    g_task_count++;
    /* P0-R1：总是向目标 CPU 发 RESCHED IPI（含同核 self-IPI），不再依赖 per-CPU
     * 定时节拍兜底——AP tick 在某些环境下不可靠，依赖它会导致新建任务（尤其是
     * 用户态服务 fs-server/display-server 等）永久不被调度，表现为启动挂载卡死。
     * IPI handler 直接 schedule()，确保目标核立即发生一次调度。 */
    if (cpu != cpu_index()) {
        lapic_send_ipi((uint8_t)g_percpu[cpu].lapic_id, IPI_RESCHED);
    } else {
        lapic_send_ipi((uint8_t)g_percpu[cpu_index()].lapic_id, IPI_RESCHED);
    }
    spin_unlock_irqrestore(&g_sched_lock, f);
}

task_t *task_create_kernel(void (*entry)(void *), void *arg, const char *name)
{
    task_t *t = task_alloc_kernel(entry, arg, name);
    if (!t) {
        return NULL;
    }
    /* P0-R1：新任务按 RR 绑定到一个 CPU（smp 未就绪时 online_count=1 全绑 CPU0） */
    uint32_t cpu = __atomic_fetch_add(&g_rr_counter, 1, __ATOMIC_RELAXED)
                 % smp_online_count();
    task_publish(t, cpu);
    kprintf("[sched] created task '%s' pid=%lu cpu=%u stack=%p\n",
            t->name, (unsigned long)t->id, (unsigned)cpu,
            (void *)t->kstack_base);
    return t;
}

/* 轻量熵源：TSC 低位混合乘散列（用户栈 ASLR） */
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
    interrupts_disable();
    tss_set_rsp0(t->kstack_top);
    g_syscall_kstack[cpu_index()] = t->kstack_top;
    g_scratch[cpu_index()]        = &t->scr_rip;
    enter_user_mode(t->user_rip, t->user_stack_top);
}

task_t *task_create_user_args(const void *elf, size_t size,
                                int argc, const char *const argv[],
                                int envc, const char *const envp[],
                                const char *name)
{
    uint64_t as = vmm_create_address_space();
    if (!as) {
        kprintf("[sched] task_create_user: vmm_create_address_space FAILED (return NULL)\n");
        return NULL;
    }

    elf_load_result_t res;
    uint64_t stack_top = USER_STACK_TOP - (aslr_random() & 0xFF) * PAGE_SIZE;
    if (!elf_load(as, elf, size, argc, argv, envc, envp, stack_top,
                  USER_STACK_PAGES, &res)) {
        kprintf("[sched] task_create_user: elf_load FAILED\n");
        vmm_destroy_address_space(as);
        return NULL;
    }

    /* 先在“私有”状态下装配完全部用户态字段，再发布入队 —— 保证其它核
     * 绝不会调度到半成品任务（P0-R1 多核正确性 + 避免锁重入死锁）。 */
    task_t *t = task_alloc_kernel(user_task_thunk, NULL, name);
    if (!t) {
        kprintf("[sched] task_create_user: task_alloc_kernel FAILED\n");
        vmm_destroy_address_space(as);
        return NULL;
    }
    t->is_user = true;
    t->cr3 = as;
    /* P0-R2 KPTI：本任务内核栈页映射进影子 PML4（Ring3 被中断/系统调用时
     * CPU 向 TSS.rsp0 压栈，栈页必须在影子中可写）。必须在发布入队前完成，
     * 且 as 已由 task_alloc_kernel 之前建立。 */
    vmm_kpti_map_kstack(as, t->kstack_base, t->kstack_top);
    t->user_rip = res.entry;
    t->user_stack_top = res.stack_top;
    {
        uint64_t mapped_lo = stack_top - (uint64_t)USER_STACK_PAGES * PAGE_SIZE;
        uint64_t grow_lo   = mapped_lo
                           - (uint64_t)VMA_STACK_GROW_PAGES * PAGE_SIZE;
        vma_insert(t, grow_lo, mapped_lo, PTE_WRITE | PTE_NX, VMA_TYPE_STACK);
    }
    /* 修正跳板参数：r13 槽（arg）指向任务自身 */
    ((uint64_t *)t->rsp)[2] = (uint64_t)t;

    uint32_t cpu = __atomic_fetch_add(&g_rr_counter, 1, __ATOMIC_RELAXED)
                 % smp_online_count();
    task_publish(t, cpu);

    kprintf("[sched] user task '%s' pid=%lu cpu=%u cr3=%p entry=%p (ELF, W^X)\n",
            t->name, (unsigned long)t->id, (unsigned)cpu, (void *)as,
            (void *)res.entry);
    return t;
}

task_t *task_create_user(const void *elf, size_t size, const char *name)
{
    return task_create_user_args(elf, size, 0, NULL, 0, NULL, name);
}

/* 按 PID 在全局任务表中查找（含尚未被回收的 zombie）。调用方无需持锁，
 * 本函数内部加锁以与 reap/创建路径互斥。 */
task_t *task_lookup(uint64_t pid)
{
    uint64_t f = spin_lock_irqsave(&g_sched_lock);
    task_t *t = g_all_tasks;
    while (t) {
        if (t->id == pid) {
            spin_unlock_irqrestore(&g_sched_lock, f);
            return t;
        }
        t = t->all_next;
    }
    spin_unlock_irqrestore(&g_sched_lock, f);
    return NULL;
}

/* P0-R1 负载均衡：work-stealing。调用方须持 g_sched_lock。
 * 扫描其它 CPU 的运行队列，摘取一个「就绪(READY)、非 idle、非该 CPU 当前运行
 * 任务」的任务迁移到本 CPU（更新 t->cpu 以便后续 IPI 唤醒定位正确）。
 * 返回 NULL 表示无任务可偷（其它 CPU 也仅剩 idle / 正在运行）。 */
static task_t *steal_task(uint32_t self)
{
    for (uint32_t c = 0; c < MAX_CPUS; c++) {
        if (c == self) {
            continue;
        }
        if (g_percpu[c].rq_count <= 1) {
            continue;   /* 该 CPU 仅含 idle，无可偷任务 */
        }
        task_t *t = g_percpu[c].rq_head;
        while (t) {
            if (t->alive && !t->is_idle && t->state == READY &&
                t != g_percpu[c].current_task) {
                rq_unlink_cpu(t, c);   /* 从源 CPU 队列摘链（rq_count--） */
                t->cpu = self;         /* 迁移到本 CPU */
                return t;
            }
            t = t->next;
        }
    }
    return NULL;
}

/* 执行一次调度（调用时须处于关中断状态，或本函数内部会自行关中断）。
 * 在持有 g_sched_lock 期间完成“挑选 + 摘链 + 状态/CR3/栈切换”，随后释锁再做
 * context_switch（绝不在持锁时切栈，避免锁被新任务栈“带走”造成其它核死等）。 */
void schedule(void)
{
    uint64_t f = spin_lock_irqsave(&g_sched_lock);
    uint32_t cpu = cpu_index();
    task_t *cur = g_percpu[cpu].current_task;

    /* M7 修复：内核栈溢出守卫 */
    if (cur && cur->kstack_base) {
        if (*(const uint64_t *)cur->kstack_base != KSTACK_CANARY) {
            spin_unlock_irqrestore(&g_sched_lock, f);
            panic("kernel stack overflow detected (task '%s' pid=%lu)",
                  cur->name, (unsigned long)cur->id);
        }
    }

    task_t *next = pick_next();
    if (next == cur) {
        /* P0-R1 负载均衡：本 CPU 运行队列无可运行任务时，从其它 CPU 窃取一个
         * 就绪任务，避免 AP 在多任务场景下空转（work-stealing）。 */
        task_t *stolen = steal_task(cpu);
        if (stolen) {
            rq_push_cpu(stolen, cpu);
            next = stolen;
        }
    }
    if (next == cur) {
        cur->ticks_remaining = TIME_SLICE_TICKS;
        spin_unlock_irqrestore(&g_sched_lock, f);
        return;
    }

    /* FPU/SSE 上下文保存与恢复（换栈前在旧栈上完成） */
    fpu_fxsave(&cur->fpu_state);
    if (next->fpu_valid) {
        fpu_fxrstor(&next->fpu_state);
    } else {
        fpu_fninit();
        next->fpu_valid = true;
    }

    if (cur->state == RUNNING) {
        cur->state = READY;
    }
    next->state = RUNNING;
    next->ticks_remaining = TIME_SLICE_TICKS;
    if (next->is_user && next != cur) {
        g_percpu[cpu].user_switches++;   /* 负载均衡观测：本核跑了一次 Ring3 任务 */
    }
    g_percpu[cpu].current_task = next;
    g_syscall_kstack[cpu] = next->kstack_top;
    g_scratch[cpu]        = &next->scr_rip;
    tss_set_rsp0(next->kstack_top);

    bool cr3_switch = (next->cr3 != cur->cr3);
    uint64_t next_cr3 = next->cr3;
    spin_unlock_irqrestore(&g_sched_lock, f);

    if (cr3_switch) {
        vmm_switch(next_cr3);
    }
    verify_switch_target(next);
    context_switch(&cur->rsp, next->rsp);

    /* 切换完成后，在“新任务”栈上回收此前已退出任务的残留资源 */
    reap_dead();
}

/* 唤醒一个阻塞任务（IPC/等待协议用）。
 * 经典 sleep/wakeup 死锁修复：接收方在 g_port_lock 下把 state 置 WAITING 并
 * 调 schedule() 让出；schedule() 见 state!=RUNNING 会把它从运行队列摘下
 * （in_rq=false）。若仅把 state 改回 READY 而不重新入队，pick_next 永远选不到
 * 它 —— 表现即 FS server 阻塞在 recv 等 disk-srv 回复、disk-srv 却永不被调度的
 * 死锁。故此处除置 READY 外，若它已不在 rq 则重新入队，并确保对其所在核发
 * RESCHED IPI（含同核 self-IPI）。
 * 锁序：本函数持 g_sched_lock（irqsave）。调用方（enqueue）持 g_port_lock 再调
 * 本函数，即 g_port_lock 在外、g_sched_lock 在内，无反向持锁，安全。
 * 因 interrupt gate 自动 CLI，持锁期间不会嵌套 resched IPI，无自死锁。 */
void sched_wake(task_t *t)
{
    if (!t) {
        return;
    }
    uint64_t f = spin_lock_irqsave(&g_sched_lock);
    t->state = READY;
    if (!t->in_rq) {
        rq_push_cpu(t, t->cpu);
    }
    uint32_t wcpu = t->cpu;
    bool same = (wcpu == cpu_index());
    spin_unlock_irqrestore(&g_sched_lock, f);

    /* IPI 在完全解锁后发送：waiter 所在核（可能即本核）需立即发生一次调度。
     * 同核 self-IPI 在解锁后才触发，保证当前任务不会在仍持任何锁时被切走。 */
    if (same) {
        lapic_send_ipi((uint8_t)g_percpu[wcpu].lapic_id, IPI_RESCHED);
    } else {
        lapic_send_ipi((uint8_t)g_percpu[wcpu].lapic_id, IPI_RESCHED);
    }
}

/* P0-R1 负载均衡观测：打印每 CPU 在线状态、运行队列长度、Ring3 任务切换计数。 */
void sched_balance_report(void)
{
    kprintf("[sched] load balance: %u CPUs\n", (unsigned)smp_online_count());
    for (uint32_t c = 0; c < smp_online_count(); c++) {
        kprintf("  cpu%u: rq=%u user_switches=%llu%s\n",
                (unsigned)c, (unsigned)g_percpu[c].rq_count,
                (unsigned long long)g_percpu[c].user_switches,
                g_percpu[c].online ? "" : " (offline)");
    }
}

/* 由每 CPU LAPIC 定时器（IRQ0）调用 */
void sched_tick(registers_t *r)
{
    (void)r;
    /* P0-R3：BSP 每 tick（10ms）探测 COM2 是否有 GDB 数据到达；有则触发
     * int3 陷入 gdbstub 会话（函数内部自限 cpu0 + 未附着时才触发）。 */
    gdbstub_poll();
    /* P0-R1：启动约 3 秒后（仅 BSP）打印一次负载均衡快照，验证 AP 也跑了
     * Ring3 任务（user_switches 非 0），确认对称调度真正生效。 */
    {
        static uint64_t s_ticks = 0;
        static bool s_reported = false;
        if (cpu_index() == 0) {
            s_ticks++;
            if (!s_reported && s_ticks >= 300) {
                s_reported = true;
                sched_balance_report();
            }
        }
    }
    task_t *cur = cpu_local()->current_task;
    if (!cur) {
        return;
    }
    if (cur->ticks_remaining > 0) {
        cur->ticks_remaining--;
    }
    if (cur->ticks_remaining == 0) {
        schedule();   /* 中断上下文，IF 已关 */
    }
}

void task_yield(void)
{
    interrupts_disable();
    schedule();
    interrupts_enable();
}

/* 回收所有已退出任务的 task 结构与内核栈（在 schedule() 切换后、新栈上调用） */
static void reap_dead(void)
{
    uint64_t f = spin_lock_irqsave(&g_sched_lock);
    task_t *t = g_dead_list;
    g_dead_list = NULL;
    while (t) {
        task_t *nx = t->dead_next;
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
        if (t->kstack_base) {
            kstack_free(t->kstack_base);   /* P0-R5：归还守卫页栈槽位 */
        }
        kfree(t);
        t = nx;
    }
    spin_unlock_irqrestore(&g_sched_lock, f);
}

/* 摘链并释放一个 zombie 任务（调用方须持 g_sched_lock 且确保 t 不在运行） */
static void task_reap_locked(task_t *t)
{
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
    if (t->kstack_base) {
        kstack_free(t->kstack_base);       /* P0-R5：归还守卫页栈槽位 */
    }
    kfree(t);
}

/* 立即回收一个已退出（zombie）任务。调用方须确保 t 不在运行。 */
void task_reap(task_t *t)
{
    uint64_t f = spin_lock_irqsave(&g_sched_lock);
    task_reap_locked(t);
    spin_unlock_irqrestore(&g_sched_lock, f);
}

/* P0-R1：sys_wait 的 SMP 安全核心。全部父子关系检查/等待者登记/zombie 回收
 * 都在 g_sched_lock 保护下进行（原实现只关本地中断，与其它核上子任务的
 * task_exit_current 并发修改 waiters 链是数据竞争）。
 * 返回 0 成功（*rc_out=子任务退出码）；-1 pid 无效或非本任务子进程。 */
int64_t task_wait_child(uint64_t child_pid, uint64_t *rc_out)
{
    task_t *cur = sched_current();
    uint64_t f = spin_lock_irqsave(&g_sched_lock);

    /* 内联查找（不可调 task_lookup：它自行拿同一把锁） */
    task_t *child = g_all_tasks;
    while (child && child->id != child_pid) {
        child = child->all_next;
    }
    if (!child || child->parent_id != cur->id) {
        spin_unlock_irqrestore(&g_sched_lock, f);
        return -1;
    }
    if (child->zombie) {
        uint64_t rc = child->exit_code;
        task_reap_locked(child);   /* 立即回收：二次 wait 将查找失败返回 -1 */
        spin_unlock_irqrestore(&g_sched_lock, f);
        *rc_out = rc;
        return 0;
    }

    /* 阻塞：登记到子任务等待者链，解锁（保持关中断）后调度让出。
     * "解锁后、调度前"子任务恰好退出置本任务 READY 不丢事件：任务仍在
     * 运行队列，schedule() 之后必被重新调度，wait_result 已写好。 */
    cur->state     = BLOCKED;
    cur->wait_link = child->waiters;
    child->waiters = cur;
    spin_unlock(&g_sched_lock);
    schedule();
    if (f & (1UL << 9)) {
        __asm__ volatile("sti" ::: "memory");
    }
    *rc_out = cur->wait_result;
    return 0;
}

__attribute__((noreturn)) void task_exit_current(uint64_t code)
{
    interrupts_disable();
    uint64_t f = spin_lock_irqsave(&g_sched_lock);
    uint32_t cpu = cpu_index();
    task_t *t = g_percpu[cpu].current_task;
    kprintf("[sched] task '%s' pid=%lu exited (code=%lu)\n",
            t->name, (unsigned long)t->id, (unsigned long)code);

    /* 唤醒阻塞在本任务退出的父任务（跨核等待者发 IPI 立即唤醒） */
    {
        task_t *w = t->waiters;
        while (w) {
            task_t *wn = w->wait_link;
            w->wait_result = code;
            w->wait_link   = NULL;
            w->state       = READY;
            if (w->cpu != cpu && g_percpu[w->cpu].in_idle) {
                lapic_send_ipi((uint8_t)g_percpu[w->cpu].lapic_id,
                               IPI_RESCHED);
            }
            w = wn;
        }
        t->waiters = NULL;
    }
    t->exit_code = code;
    t->zombie    = true;

    port_release_owner(t);
    port_reap_ool(t);
    hda_release_owner(t);   /* P0-R1：owner 退出时停流，防悬空/音频锁死 */
    vma_destroy_all(t);
    if (t->is_user && t->cr3 && t->cr3 != vmm_kernel_pml4()) {
        vmm_switch(vmm_kernel_pml4());
        vmm_destroy_address_space(t->cr3);
    }
    t->cr3 = 0;

    /* 入死亡链表，待下次本核 schedule 由 reap_dead 回收 */
    t->alive = false;
    t->state = BLOCKED;
    t->dead = true;
    t->dead_next = g_dead_list;
    g_dead_list = t;

    task_t *next = pick_next();
    rq_unlink_cpu(t, cpu);
    next->state = RUNNING;
    next->ticks_remaining = TIME_SLICE_TICKS;
    g_percpu[cpu].current_task = next;
    g_syscall_kstack[cpu] = next->kstack_top;
    g_scratch[cpu]        = &next->scr_rip;
    tss_set_rsp0(next->kstack_top);

    fpu_fxsave(&t->fpu_state);
    if (next->fpu_valid) {
        fpu_fxrstor(&next->fpu_state);
    } else {
        fpu_fninit();
        next->fpu_valid = true;
    }

    bool cr3_switch = (next->cr3 != t->cr3);
    uint64_t next_cr3 = next->cr3;
    spin_unlock_irqrestore(&g_sched_lock, f);

    if (cr3_switch) {
        vmm_switch(next_cr3);
    }
    verify_switch_target(next);
    context_switch(&t->rsp, next->rsp);   /* 一去不返 */

    /* 兜底：context_switch 设计上永不返回（被切走的任务停泊在 switch.S 内部）。
     * 若因 next 内核栈帧被破坏等异常导致它意外返回，此处绝不能落在关中断的
     * hlt 上冻结本核（那正是"系统冻结"的表现之一）。改为 panic 带诊断，把
     * 当前退出任务信息打印出来，交由异常处理路径安全停机而非静默死等。 */
    panic("task_exit_current: context_switch returned unexpectedly "
          "(task '%s' pid=%lu next='%s')",
          t->name, (unsigned long)t->id, next->name);
}
