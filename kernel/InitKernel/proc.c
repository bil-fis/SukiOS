#include "kernel/proc.h"
#include "userland/elf64.h"
#include "kernel/vmm.h"
#include "kernel/heap.h"
#include "kernel/pmm.h"
#include "kernel/gdt.h"
#include "kernel/tty.h"

/* ---- 全局变量 ---- */
pcb_t *g_current_proc = NULL;
static pcb_t *ready_queue_head = NULL;
static pcb_t *ready_queue_tail = NULL;
static uint64_t next_pid = 1;
static volatile uint64_t ticks = 0;
static pcb_t idle_proc;
static bool scheduler_ready = false;

/* ---- 外部符号 ---- */
extern void switch_to_proc(pcb_t *next);

/* ---- 辅助 ---- */
void increment_ticks(void) { ticks++; }
uint64_t get_ticks(void) { return ticks; }

static void enqueue(pcb_t *p) {
    p->next = NULL;
    p->state = PROC_READY;
    if (ready_queue_tail) {
        ready_queue_tail->next = p;
        ready_queue_tail = p;
    } else {
        ready_queue_head = ready_queue_tail = p;
    }
}

static pcb_t *dequeue(void) {
    if (!ready_queue_head) return NULL;
    pcb_t *ret = ready_queue_head;
    ready_queue_head = ret->next;
    if (!ready_queue_head) ready_queue_tail = NULL;
    return ret;
}

/* ---- 为用户进程准备内核栈（含 interrupt_frame） ---- */
static void prepare_user_stack(pcb_t *proc, uint64_t entry, uint64_t user_stack_top) {
    uint8_t *kstack = kmalloc(KERNEL_STACK_SIZE);
    if (!kstack) return;
    proc->kernel_stack = kstack;

    uint64_t *sp = (uint64_t *)((uint64_t)(kstack + KERNEL_STACK_SIZE) & ~0xFULL);
    sp -= 22;  /* 20 个寄存器 + SS + RSP */
    for (int i = 0; i < 15; i++) sp[i] = 0;       /* r15 .. rax */
    sp[15] = 0;                                    /* int_no */
    sp[16] = 0;                                    /* err_code */
    sp[17] = entry;                                /* RIP (user) */
    sp[18] = GDT_SELECTOR_USER_CS | 3;             /* CS (0x1B) */
    sp[19] = 0x202;                                /* RFLAGS (IF=1) */
    sp[20] = user_stack_top;                       /* RSP */
    sp[21] = GDT_SELECTOR_USER_DS | 3;             /* SS */

    proc->rsp = (uint64_t)sp;
    proc->cr3 = 0;  /* 当前共用内核页表 */
}

/* ---- 从 ELF 文件在内存中的镜像创建进程 ---- */
pcb_t *proc_create_from_elf(const void *elf_data, size_t size) {
    elf_load_result_t elf = elf64_load((const uint8_t *)elf_data, size);
    if (!elf.success) {
        tty_print("[ELF] load failed: ");
        tty_print(elf.error);
        tty_print("\n");
        return NULL;
    }

    pcb_t *p = (pcb_t *)kmalloc(sizeof(pcb_t));
    if (!p) return NULL;

    for (int i = 0; i < MAX_FD; i++) p->fds[i].vnode = NULL;
    p->pid         = next_pid++;
    p->state       = PROC_READY;
    p->next        = NULL;

    /* 分配用户栈（映射在 USER_STACK_TOP 下方） */
    uint64_t stack_start = USER_STACK_TOP - USER_STACK_SIZE;
    p->user_stack  = USER_STACK_TOP;
    for (uint64_t va = stack_start; va < USER_STACK_TOP; va += VMM_PAGE_SIZE) {
        if (!vmm_is_mapped(va)) {
            vmm_alloc_map(va, VMM_PRESENT | VMM_USER | VMM_WRITE);
        }
    }

    p->entry_point = elf.entry_point;
    p->cr3         = 0; /* 共用内核页表 */

    prepare_user_stack(p, elf.entry_point, USER_STACK_TOP);

    enqueue(p);
    return p;
}

/* ---- 调度器 ---- */
void schedule(void) {
    if (!scheduler_ready) {          // 未就绪时禁止上下文切换
        return;
    }

    pcb_t *prev = g_current_proc;
    pcb_t *next = dequeue();

    if (!next) {
        next = &idle_proc;           // idle 现在已正确初始化
    }

    if (prev == next) return;

    if (prev->state == PROC_RUNNING) {
        enqueue(prev);
        prev->state = PROC_READY;
    }

    g_current_proc = next;
    next->state = PROC_RUNNING;
    switch_to_proc(next);
}

/* ---- idle 进程 ---- */
static pcb_t idle_proc;

static void idle_entry(void) {
    for (;;) __asm__ volatile ("sti; hlt");
}

void scheduler_init(void) {
    uint8_t *kstack = kmalloc(KERNEL_STACK_SIZE);
    if (!kstack) {
        tty_setcolor(VGA_RED, VGA_BLACK);
        tty_print("FATAL: idle stack alloc failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    uint64_t *sp = (uint64_t *)((uint64_t)(kstack + KERNEL_STACK_SIZE) & ~0xFULL);
    sp -= 20;
    for (int i = 0; i < 15; i++) sp[i] = 0;
    sp[15] = 0;
    sp[16] = 0;
    sp[17] = (uint64_t)idle_entry;
    sp[18] = GDT_SELECTOR_KERNEL_CS;
    sp[19] = 0x202;

    idle_proc.kernel_stack = kstack;
    idle_proc.rsp          = (uint64_t)sp;
    idle_proc.pid          = 0;
    idle_proc.state        = PROC_RUNNING;
    idle_proc.cr3          = 0;
    idle_proc.entry_point  = 0;
    idle_proc.user_stack   = 0;

    g_current_proc = &idle_proc;
//    enqueue(&idle_proc);
    scheduler_ready = true;
    tty_print("[OK] Scheduler initialized\n");
}

void scheduler_start(void) {
    tty_print("[OK] Scheduler running\n");
    schedule();
    for (;;) __asm__ volatile ("sti; hlt");
}