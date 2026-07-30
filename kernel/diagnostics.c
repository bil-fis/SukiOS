/*
 * kernel/diagnostics.c
 * -----------------------------------------------------------------------------
 * 内核诊断实现（见 include/kernel/diagnostics.h）。
 *
 * 调用关系：idt.c 的异常/内核缺页路径 -> kernel_oops()；console.c 的 panic()
 *           -> diag_dump_self()。
 *
 * 栈回溯说明：x86_64 调用约定下，进入函数后通常 `push %rbp; mov %rsp,%rbp`，
 * 故 [rbp] = 上一层 rbp，[rbp+8] = 返回地址。我们沿此链向上最多走
 * DIAG_MAX_FRAMES 层，每层地址做内核范围合法性校验，越界即停（避免读坏栈引发
 * 三重故障）。返回地址为内核虚拟地址，可经 objdump -d kernel.elf 离线映射函数。
 */
#include <kernel/diagnostics.h>
#include <kernel/console.h>
#include <kernel/types.h>
#include <kernel/smp.h>       /* kernel_oops 停机前广播 IPI_HALT */
#include <mm/vmm.h>           /* 回溯前逐页校验帧地址已映射，防二次 #PF */

#define DIAG_MAX_FRAMES 32

/* 返回地址合法域：内核映像/物理直映窗口 [KERNEL_BASE, KHEAP_BASE)。
 *
 * 历史 bug（P0-R3 修复）：原上界写作 KERNEL_BASE + 0x1000000000000（2^48），
 * 但 KERNEL_BASE=0xFFFF800000000000 距 2^64 仅 2^47，相加发生无符号回绕
 * 得 0x0000800000000000，整个区间判断恒为假——编译器据此把回溯循环折叠成
 * 「无条件 break」，导致自引入以来所有 backtrace 输出皆为空且无人察觉。
 * 现改用不溢出的显式上界 0xFFFFC00000000000（KHEAP_BASE）：内核代码只可能
 * 位于映像/直映区（W^X 保证堆不可执行），返回地址落在堆/栈区即为链尾。 */
static inline bool kernel_addr(uint64_t v)
{
    return v >= KERNEL_BASE && v < 0xFFFFC00000000000UL;
}

/* 栈帧地址合法域：整个内核高半区——任务内核栈位于 kstack 区
 * (0xFFFFC00080000000+)、BSP 引导栈位于 0xFFFFC00000800000 附近、
 * 早期路径也可能在内核映像内，统一放行到高半区再逐页验映射。 */
static inline bool kernel_half_addr(uint64_t v)
{
    return v >= KERNEL_BASE;
}

/* 帧两个 qword（[rbp]=上一层 rbp、[rbp+8]=返回地址）所在页是否已映射。
 * 回溯常在异常/panic 现场执行，读未映射地址会引发二次 #PF 把首个现场
 * 冲掉——先查当前 CR3 的页表再解引用。 */
static bool frame_mapped(uint64_t frame)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t root = cr3 & PTE_ADDR_MASK;
    if (!vmm_translate(root, frame & ~(PAGE_SIZE - 1))) {
        return false;
    }
    /* rbp+8 可能跨页 */
    if (!vmm_translate(root, (frame + 8) & ~(PAGE_SIZE - 1))) {
        return false;
    }
    return true;
}

void diag_dump_registers(registers_t *r)
{
    kprintf("  RAX=%p RBX=%p RCX=%p RDX=%p\n",
            (void *)r->rax, (void *)r->rbx, (void *)r->rcx, (void *)r->rdx);
    kprintf("  RSI=%p RDI=%p RBP=%p RSP=%p\n",
            (void *)r->rsi, (void *)r->rdi, (void *)r->rbp, (void *)r->rsp);
    kprintf("  R8 =%p R9 =%p R10=%p R11=%p\n",
            (void *)r->r8, (void *)r->r9, (void *)r->r10, (void *)r->r11);
    kprintf("  R12=%p R13=%p R14=%p R15=%p\n",
            (void *)r->r12, (void *)r->r13, (void *)r->r14, (void *)r->r15);
    kprintf("  RIP=%p CS=0x%lx RFLAGS=0x%lx SS=0x%lx\n",
            (void *)r->rip, (unsigned long)r->cs,
            (unsigned long)r->rflags, (unsigned long)r->ss);
    kprintf("  ERR=0x%lx INT=0x%lx\n", (unsigned long)r->err_code,
            (unsigned long)r->int_no);

    uint64_t cr0, cr2, cr3, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    kprintf("  CR0=%p CR2=%p CR3=%p CR4=%p\n",
            (void *)cr0, (void *)cr2, (void *)cr3, (void *)cr4);
}

void diag_dump_trace(uint64_t rbp, uint64_t rip)
{
    kprintf("  backtrace (rbp=%p):\n", (void *)rbp);
    if (rip != 0) {
        kprintf("    #0  %p\n", (void *)rip);
    }
    uint64_t frame = rbp;
    for (int i = (rip ? 1 : 0); i < DIAG_MAX_FRAMES; i++) {
        /* 帧指针在内核高半区（内核映像 / 堆 / kstack 区均可）、8 字节
         * 对齐、且两个 qword 所在页真实已映射，才允许解引用。 */
        if (!kernel_half_addr(frame) || (frame & 0x7) || !frame_mapped(frame)) {
            break;   /* 非对齐/非内核地址/未映射：链结束 */
        }
        uint64_t *slot = (uint64_t *)frame;
        uint64_t prev = slot[0];          /* 上一层 rbp */
        uint64_t ret  = slot[1];          /* 返回地址 */
        if (!kernel_addr(ret) || ret == 0) {
            break;   /* 返回地址必须落在内核映像/直映窗口 */
        }
        kprintf("    #%d %p\n", i, (void *)ret);
        if (prev <= frame || !kernel_half_addr(prev)) {
            break;   /* 栈向下生长，上一层帧必须更高；防环/链结束 */
        }
        frame = prev;
    }
}

void kernel_oops(const char *msg, registers_t *r)
{
    kprintf("\n[KERNEL OOPS] %s\n", msg ? msg : "(no message)");
    diag_dump_registers(r);
    diag_dump_trace(r->rbp, r->rip);
    /* P0-R3：停掉其它 CPU——半死状态的旁核继续调度会覆写现场/写坏磁盘 */
    smp_halt_others();
    kprintf("[KERNEL OOPS] system halted.\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

void diag_dump_self(void)
{
    uint64_t rbp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    kprintf("[backtrace]\n");
    diag_dump_trace(rbp, 0);
}

/* P0-R3b：WARN 实现体——打印文件:行号:条件 + 当前栈回溯后返回继续运行。
 * 单独成帧（不 inline）使回溯首层稳定指向 kernel_warn，第二层即告警点。 */
void kernel_warn(const char *file, int line, const char *cond)
{
    kprintf("\n[WARN] %s:%d: WARN_ON(%s)\n", file ? file : "?", line,
            cond ? cond : "?");
    diag_dump_self();
}

/* P0-R3b：启动自检——故意触发一次 WARN_ON 验证全链路（打印+回溯+继续）。
 * 用 volatile 防编译器把条件折叠掉。 */
void diag_selftest(void)
{
    volatile int trigger = 1;
    kprintf("[diag] selftest: expecting one intentional WARN below\n");
    if (WARN_ON(trigger == 1)) {
        kprintf("[diag] selftest OK: WARN_ON fired, backtrace printed, "
                "execution continued\n");
    }
    BUG_ON(trigger != 1);   /* 同时验证 BUG_ON 的「不触发」路径编译/求值 */
}
