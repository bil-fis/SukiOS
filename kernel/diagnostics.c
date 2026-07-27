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

#define DIAG_MAX_FRAMES 32

/* 判断指针是否落在内核高半区（粗略，防回溯读到用户/空洞地址） */
static inline bool kernel_addr(uint64_t v)
{
    return v >= KERNEL_BASE && v < (KERNEL_BASE + 0x1000000000000ULL);
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
    kprintf("  backtrace:\n");
    if (rip != 0) {
        kprintf("    #0  %p\n", (void *)rip);
    }
    uint64_t frame = rbp;
    for (int i = (rip ? 1 : 0); i < DIAG_MAX_FRAMES; i++) {
        if (!kernel_addr(frame) || (frame & 0x7)) {
            break;   /* 非对齐或非内核地址：链结束 */
        }
        uint64_t *slot = (uint64_t *)frame;
        uint64_t prev = slot[0];          /* 上一层 rbp */
        uint64_t ret  = slot[1];          /* 返回地址 */
        if (!kernel_addr(ret) || ret == 0) {
            break;
        }
        kprintf("    #%d %p\n", i, (void *)ret);
        if (prev == frame || !kernel_addr(prev)) {
            break;   /* 防环 / 链结束 */
        }
        frame = prev;
    }
}

void kernel_oops(const char *msg, registers_t *r)
{
    kprintf("\n[KERNEL OOPS] %s\n", msg ? msg : "(no message)");
    diag_dump_registers(r);
    diag_dump_trace(r->rbp, r->rip);
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
