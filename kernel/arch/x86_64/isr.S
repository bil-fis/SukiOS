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

/* IPI 向量（P0-3 SMP）：0xF0 重调度 / 0xF1 TLB 刷新 / 0xF2 panic 停机
 * 0xF3 GDB 冻结（P0-R3：gdbstub 会话期间其它 CPU 自旋等待释放） */
ISR_NOERR 240
ISR_NOERR 241
ISR_NOERR 242
ISR_NOERR 243

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

/* MSI/MSI-X 向量池（P0-2/R6）：48..127 为消息信号中断复用 isr\num 存根。
 * 设备经 MSI 把中断投递到这些向量，由驱动通过 register_interrupt_handler 注册。
 * 用 .altmacro + .rept 批量生成，避免手工罗列 80 条。 */
.altmacro
.macro GEN_MSI_STUBS first, last
.set _m, \first
.rept (\last - \first + 1)
ISR_NOERR %_m
.set _m, _m + 1
.endr
.endm
GEN_MSI_STUBS 48, 127
.noaltmacro

/* 公共存根：保存寄存器 -> KPTI 切内核页表 -> 调用 C -> KPTI 还原 -> iretq
 *
 * P0-R2 KPTI 设计（KAISER 式）：
 *   入口：压完 GPR 后读 CR3 压栈（registers_t.saved_cr3，帧首字段）。若
 *   bit12=1（来自 Ring3 影子视图），清 bit12 切到本地址空间内核视图——
 *   在此之前 CPU 压 iretq 帧 / 本存根压 GPR 全部落在「已映射进影子」的
 *   任务内核栈或 IST 栈上，不会缺页。
 *   出口：弹出 saved_cr3，与当前 CR3 不同才写回（避免每次中断白刷 TLB）。
 *   「原样还原」而非「按目标 CS 推断」的关键收益：NMI 若命中 syscall 出口
 *   「已切影子、尚未 iretq/sysret」的 1~2 条指令微窗口（CPL0 + 影子 CR3），
 *   出口会如实还原影子视图继续退出——不会把内核视图带回 Ring3 造成 KPTI
 *   旁路（Linux 用跳板页解决的同一竞态，这里以保存-还原语义闭合）。
 *   任务切换安全性：schedule() 在 isr_dispatch 内切走后，本帧连同 saved_cr3
 *   一起“停泊”在旧任务内核栈上；旧任务恢复时 CR3 已由 schedule 恢复为其
 *   内核视图，弹出的 saved_cr3 也属于同一地址空间对，语义自洽。 */
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

    /* KPTI 入口：保存 CR3 原值到帧首（registers_t.saved_cr3），必要时切内核视图。
     * 必须先查 g_kpti_enabled：CR3 bit12 属 PML4 物理基址（osdev CPU_Registers_
     * x86-64），只有 KPTI 启用时「成对 8KB 分配 + 内核 PML4 8KB 对齐」才保证
     * bit12=1 ⇔ 影子视图；KPTI 未启用时任务 PML4 为单页分配，物理地址 bit12
     * 可为 1，无条件清位会把合法 CR3 改成垃圾页表 -> 取指 #PF(RSVD) -> 三重
     * 故障（实测：boot p4_table 落在 0x101000 时 AP 首个中断即崩）。 */
    movq %cr3, %rax
    pushq %rax
    movabsq $g_kpti_enabled, %rcx
    cmpb $0, (%rcx)
    je 1f                    /* KPTI 未启用：CR3 原样使用 */
    testq $0x1000, %rax      /* bit12=1 -> 影子视图（来自 Ring3） */
    jz 1f
    andq $-4097, %rax        /* 清 bit12（-4097 = ~0x1000，32 位符号扩展立即数） */
    movq %rax, %cr3
1:
    cld
    movq %rsp, %rdi          /* 参数：registers_t *（首字段 saved_cr3） */
    call isr_dispatch

    /* KPTI 出口：还原入口时的 CR3 视图（不同才写，省去无谓 TLB 冲刷） */
    popq %rax                /* saved_cr3 */
    movq %cr3, %rcx
    cmpq %rax, %rcx
    je 2f
    movq %rax, %cr3
2:
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
