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
#include <kernel/apic.h>
#include <kernel/diagnostics.h>
#include <kernel/task.h>
#include <mm/vma.h>          /* P0-5：#PF 按需分页救援 vma_populate */
#include <mm/kstack.h>       /* P0-R5：守卫页命中定性（内核栈溢出诊断） */

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
/* IPI 向量存根（P0-3 SMP + P0-R3 GDB 冻结，见 isr.S） */
extern void isr240(void); extern void isr241(void); extern void isr242(void);
extern void isr243(void);

/* MSI/MSI-X 向量池存根（P0-2/R6）：48..127，X 宏一次生成声明与数组 */
#define MSI_ISR_LIST \
    X(48)  X(49)  X(50)  X(51)  X(52)  X(53)  X(54)  X(55) \
    X(56)  X(57)  X(58)  X(59)  X(60)  X(61)  X(62)  X(63) \
    X(64)  X(65)  X(66)  X(67)  X(68)  X(69)  X(70)  X(71) \
    X(72)  X(73)  X(74)  X(75)  X(76)  X(77)  X(78)  X(79) \
    X(80)  X(81)  X(82)  X(83)  X(84)  X(85)  X(86)  X(87) \
    X(88)  X(89)  X(90)  X(91)  X(92)  X(93)  X(94)  X(95) \
    X(96)  X(97)  X(98)  X(99)  X(100) X(101) X(102) X(103) \
    X(104) X(105) X(106) X(107) X(108) X(109) X(110) X(111) \
    X(112) X(113) X(114) X(115) X(116) X(117) X(118) X(119) \
    X(120) X(121) X(122) X(123) X(124) X(125) X(126) X(127)
#define X(n) extern void isr##n(void);
MSI_ISR_LIST
#undef X
static void (*const g_msi_stubs[])(void) = {
#define X(n) isr##n,
MSI_ISR_LIST
#undef X
};

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
        /* P0-5：先尝试按需分页救援——命中 VMA 的缺页补零页 / COW 写故障
         * 拷贝断开，成功则 iretq 原地重试指令（对用户完全透明）。 */
        if (vma_populate(t, cr2, write)) {
            return;
        }
        kprintf("[pf] user #PF: cr2=%p write=%d pid=%lu '%s' rip=%p -> killing task\n",
                (void *)cr2, write, (unsigned long)t->id, t->name,
                (void *)r->rip);
        task_exit_current(139);          /* noreturn：隔离故障任务（139≈SIGSEGV） */
    }
    kprintf("\n[KPF] KERNEL page fault! cr2=%p write=%d rip=%p cs=0x%lx rflags=0x%lx\n",
            (void *)cr2, write, (void *)r->rip,
            (unsigned long)r->cs, (unsigned long)r->rflags);
    /* P0-R5：CR2 落在某条在用内核栈的守卫页 => 定性为内核栈溢出（大局部
     * 数组/深递归写穿栈底），直接给出结论性诊断而非神秘 #PF。 */
    if (kstack_guard_hit(cr2)) {
        kprintf("[KPF] CR2 hits a kernel-stack GUARD PAGE => "
                "KERNEL STACK OVERFLOW (task '%s')\n",
                sched_current() ? sched_current()->name : "?");
        kernel_oops("Kernel stack overflow (guard page hit)", r);
    }
    kernel_oops("Kernel page fault (copy_from_user should have pre-validated)",
                r);
}

void idt_init(void)
{
    for (int i = 0; i < 256; i++) {
        g_handlers[i] = NULL;
    }
    /* 0x8E = present, DPL0, 中断门（自动关中断）。异常 8/双故障走 IST1。 */
    for (int i = 0; i < 48; i++) {
        /* P0-8：双重错误(8)=IST1，NMI(2)=IST2——两者都可能在「当前栈
         * 不可信」时到来（栈溢出触发 #DF；NMI 可打断任意瞬间含栈切换
         * 中途），必须走独立已知良好栈。 */
        uint8_t ist = (i == 8) ? 1 : ((i == 2) ? 2 : 0);
        idt_set_gate(i, (uint64_t)g_stubs[i], ist, 0x8E);
    }

    /* IPI 向量（P0-3）：0xF0 重调度 / 0xF1 TLB 刷新 / 0xF2 停机
     * P0-R3：0xF3 GDB 冻结（gdbstub 会话期间其它 CPU 自旋等待） */
    idt_set_gate(240, (uint64_t)isr240, 0, 0x8E);
    idt_set_gate(241, (uint64_t)isr241, 0, 0x8E);
    idt_set_gate(242, (uint64_t)isr242, 0, 0x8E);
    idt_set_gate(243, (uint64_t)isr243, 0, 0x8E);

    /* MSI/MSI-X 向量池（P0-2/R6）：48..127，消息信号中断专用存根 */
    for (int i = 48; i <= 127; i++) {
        idt_set_gate(i, (uint64_t)g_msi_stubs[i - 48], 0, 0x8E);
    }

    register_interrupt_handler(14, page_fault_handler);   /* #PF 隔离处理器 */

    g_idtr.limit = sizeof(g_idt) - 1;
    g_idtr.base  = (uint64_t)&g_idt;
    idt_load(&g_idtr);

    kprintf("[idt] IDT loaded (48+80(MSI)+IPI vectors installed, #PF handler)\n");
}

/* P0-3：AP 加载与 BSP 相同的 IDT（handler 表共享，per-CPU 行为由向量决定） */
void idt_load_ap(void)
{
    idt_load(&g_idtr);
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
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        if (vec == 14) {
            kprintf("  CR2 (fault addr) = %p\n", (void *)cr2);
        }
        /* P0-R5：#DF（vec 8，走 IST1）最常见成因就是 push/call 推进守卫页
         * ——#PF 帧无处可压升级 #DF，CR2 仍保留首次故障地址。命中守卫区间
         * 即可给出「内核栈溢出」结论，而非无线索的 Double Fault。 */
        if ((vec == 8 || vec == 14) && kstack_guard_hit(cr2)) {
            kprintf("  CR2=%p hits a kernel-stack GUARD PAGE => "
                    "KERNEL STACK OVERFLOW\n", (void *)cr2);
            kernel_oops("Kernel stack overflow (guard page hit, via #DF/#PF)",
                        r);
        }
        kernel_oops("Unhandled CPU exception", r);
    }

    if (vec >= 32 && vec != 0xFF) {
        /* 硬件 IRQ 与 IPI：先发送 LAPIC EOI，再调用处理器——定时器处理器
         * 可能触发上下文切换而长时间不返回，若 EOI 滞后会导致 LAPIC 停止
         * 投递后续中断。0xFF 为伪中断（spurious），规范要求不发 EOI。 */
        lapic_eoi();
        if (g_handlers[vec]) {
            g_handlers[vec](r);
        }
        return;
    }
}
