/*
 * kernel/arch/x86_64/idt.c
 * -----------------------------------------------------------------------------
 * 中断描述符表 (IDT) 构建与加载，以及 C 语言中断分发器 isr_dispatch。
 *
 * 调用关系：kmain() -> idt_init()（内部安装 isr0..isr47 存根）。
 *           CPU 中断 -> isrN (isr.S) -> isr_common -> isr_dispatch()。
 */
#include <kernel/interrupts.h>
#include <kernel/console.h>
#include <kernel/pic.h>
#include <kernel/task.h>

/* 64 位 IDT 门描述符（16 字节） */
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry g_idt[256];
static struct idt_ptr   g_idtr;
static isr_handler_t     g_handlers[256];

/* isr.S 中的存根声明 */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);
extern void isr32(void); extern void isr33(void); extern void isr34(void); extern void isr35(void);
extern void isr36(void); extern void isr37(void); extern void isr38(void); extern void isr39(void);
extern void isr40(void); extern void isr41(void); extern void isr42(void); extern void isr43(void);
extern void isr44(void); extern void isr45(void); extern void isr46(void); extern void isr47(void);

static void (*const g_stubs[48])(void) = {
    isr0,isr1,isr2,isr3,isr4,isr5,isr6,isr7,isr8,isr9,isr10,isr11,
    isr12,isr13,isr14,isr15,isr16,isr17,isr18,isr19,isr20,isr21,isr22,isr23,
    isr24,isr25,isr26,isr27,isr28,isr29,isr30,isr31,isr32,isr33,isr34,isr35,
    isr36,isr37,isr38,isr39,isr40,isr41,isr42,isr43,isr44,isr45,isr46,isr47,
};

static const char *g_exc_names[32] = {
    "Divide-by-zero","Debug","NMI","Breakpoint","Overflow","BOUND Range",
    "Invalid Opcode","Device N/A","Double Fault","Coprocessor Overrun",
    "Invalid TSS","Segment Not Present","Stack-Segment Fault","General Protection",
    "Page Fault","Reserved","x87 FP","Alignment Check","Machine Check","SIMD FP",
    "Virtualization","Control Protection","Reserved","Reserved","Reserved",
    "Reserved","Reserved","Reserved","Hypervisor","VMM Comm","Security","Reserved",
};

static void idt_set_gate(int n, uint64_t handler, uint8_t ist, uint8_t type_attr)
{
    g_idt[n].offset_low  = handler & 0xFFFF;
    g_idt[n].selector    = KERNEL_CS;
    g_idt[n].ist         = ist & 0x7;
    g_idt[n].type_attr   = type_attr;
    g_idt[n].offset_mid  = (handler >> 16) & 0xFFFF;
    g_idt[n].offset_high = (handler >> 32) & 0xFFFFFFFF;
    g_idt[n].zero        = 0;
}

/* 特权指令隔离封装：lidt（手册 9.2） */
static __attribute__((noinline)) void idt_load(const struct idt_ptr *ptr)
{
    __asm__ volatile("lidt (%0)" : : "r"(ptr) : "memory");
}

void register_interrupt_handler(uint8_t vec, isr_handler_t handler)
{
    g_handlers[vec] = handler;
}

/* 缺页异常(#PF, vec 14)处理器（A1 项）：
 * - 用户态缺页：说明用户程序访问了非法地址（空指针/越界/未映射），
 *   直接终止该用户任务并切换到其他任务，避免整个内核宕机（故障隔离）。
 * - 内核态缺页：属内核 bug；copy_from_user/copy_to_user 已做逐页预校验，
 *   理论上不应再发生，打印关键信息后 panic。 */
static void page_fault_handler(registers_t *r)
{
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    bool user  = (r->cs & 0x3) != 0;
    bool write = (r->err_code & 0x2) != 0;

    if (user) {
        task_t *t = sched_current();
        kprintf("[pf] user #PF: cr2=%p write=%d pid=%lu '%s' rip=%p -> killing task\n",
                (void *)cr2, write, (unsigned long)t->id, t->name,
                (void *)r->rip);
        task_exit_current(139);          /* noreturn：隔离故障任务（139≈SIGSEGV） */
    }
    kprintf("\n[KPF] KERNEL page fault! cr2=%p write=%d rip=%p cs=0x%lx rflags=0x%lx\n",
            (void *)cr2, write, (void *)r->rip,
            (unsigned long)r->cs, (unsigned long)r->rflags);
    panic("Kernel page fault (copy_from_user should have pre-validated)");
}

void idt_init(void)
{
    for (int i = 0; i < 256; i++) {
        g_handlers[i] = NULL;
    }
    /* 0x8E = present, DPL0, 中断门（自动关中断）。异常 8/双故障走 IST1。 */
    for (int i = 0; i < 48; i++) {
        uint8_t ist = (i == 8) ? 1 : 0;   /* 双重错误使用 IST1 独立栈 */
        idt_set_gate(i, (uint64_t)g_stubs[i], ist, 0x8E);
    }

    register_interrupt_handler(14, page_fault_handler);   /* #PF 隔离处理器 */

    g_idtr.limit = sizeof(g_idt) - 1;
    g_idtr.base  = (uint64_t)&g_idt;
    idt_load(&g_idtr);

    kprintf("[idt] IDT loaded (48 vectors installed, #PF handler registered)\n");
}

/* C 语言中断分发器：由 isr_common 调用 */
void isr_dispatch(registers_t *r)
{
    uint64_t vec = r->int_no;

    if (vec < 32) {
        /* CPU 异常：若无注册处理器则 panic */
        if (g_handlers[vec]) {
            g_handlers[vec](r);
            return;
        }
        kprintf("\n[EXC] vector %u (%s) err=0x%lx\n",
                (unsigned)vec,
                g_exc_names[vec] ? g_exc_names[vec] : "?",
                (unsigned long)r->err_code);
        kprintf("  RIP=%p CS=0x%lx RFLAGS=0x%lx\n",
                (void *)r->rip, (unsigned long)r->cs, (unsigned long)r->rflags);
        kprintf("  RSP=%p RAX=%p RBX=%p\n",
                (void *)r->rsp, (void *)r->rax, (void *)r->rbx);
        if (vec == 14) {
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            kprintf("  CR2 (fault addr) = %p\n", (void *)cr2);
        }
        panic("Unhandled CPU exception %u", (unsigned)vec);
    }

    if (vec >= IRQ_BASE && vec < IRQ_BASE + 16) {
        /* 先发送 EOI，再调用处理器：定时器处理器可能触发上下文切换而长时间
         * 不返回，若 EOI 滞后会导致 PIC 停止投递后续中断。 */
        pic_send_eoi((uint8_t)(vec - IRQ_BASE));
        if (g_handlers[vec]) {
            g_handlers[vec](r);
        }
        return;
    }

    /* 其它向量（如软件 int）暂忽略 */
    if (g_handlers[vec]) {
        g_handlers[vec](r);
    }
}
