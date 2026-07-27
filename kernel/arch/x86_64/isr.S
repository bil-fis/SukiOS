/*
 * kernel/arch/x86_64/isr.S
 * -----------------------------------------------------------------------------
 * 中断/异常汇编存根。为向量 0..47 生成入口，统一跳转到 isr_common，
 * 保存寄存器帧后调用 C 语言 isr_dispatch(registers_t *)。
 *
 * 压栈约定见 include/kernel/interrupts.h 的 registers_t。
 * 部分异常由 CPU 自动压入错误码，其余由存根压入哑值 0 以保持帧一致。
 */

.section .text
.code64

/* 无错误码的向量：压入哑错误码 0 + 向量号 */
.macro ISR_NOERR num
.global isr\num
isr\num:
    pushq $0
    pushq $\num
    jmp isr_common
.endm

/* 有错误码的向量：CPU 已压错误码，仅压向量号 */
.macro ISR_ERR num
.global isr\num
isr\num:
    pushq $\num
    jmp isr_common
.endm

/* CPU 异常 0..31 */
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

/* IPI 向量（P0-3 SMP）：0xF0 重调度 / 0xF1 TLB 刷新 / 0xF2 panic 停机 */
ISR_NOERR 240
ISR_NOERR 241
ISR_NOERR 242

/* 硬件 IRQ 32..47（PIC 重映射后） */
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

/* 公共存根：保存寄存器 -> 调用 C -> 恢复 -> iretq */
isr_common:
    /* 按 registers_t 约定顺序压入通用寄存器 */
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    cld
    movq %rsp, %rdi          /* 参数：registers_t * */
    call isr_dispatch

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    addq $16, %rsp           /* 弹出 int_no 与 err_code */
    iretq

.section .note.GNU-stack, "", @progbits
