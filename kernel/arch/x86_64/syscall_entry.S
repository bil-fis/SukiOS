/*
 * kernel/arch/x86_64/syscall_entry.S
 * -----------------------------------------------------------------------------
 * syscall 指令入口 与 Ring0->Ring3 进入函数。
 *
 * syscall 进入时的 CPU 状态：
 *   RCX = 用户 RIP，R11 = 用户 RFLAGS，RSP = 仍是用户栈！
 *   IF 已被 IA32_FMASK 清零（进入即关中断）。
 * 本入口：切到当前任务内核栈(g_syscall_kstack) -> 构造 iretq 帧 ->
 *          调 C 分发器 -> iretq 返回 Ring3。
 * 寄存器约定：仅 rax(返回值)/rcx/r11 允许被破坏，其余全部保存恢复。
 *
 * 调用关系：Ring3 syscall -> syscall_entry -> syscall_dispatch(C) -> iretq。
 */

.section .text
.code64

.extern syscall_dispatch
.extern g_syscall_kstack

.global syscall_entry
.type syscall_entry, @function
syscall_entry:
    /* 此刻仍在用户栈上，先把用户 RSP 存入暂存槽，换到内核栈 */
    movq  %rsp, g_user_rsp_scratch(%rip)
    movq  g_syscall_kstack(%rip), %rsp

    /* 构造 iretq 返回帧（自底向上：SS,RSP,RFLAGS,CS,RIP） */
    pushq $0x23                          /* 用户 SS = USER_DS */
    pushq g_user_rsp_scratch(%rip)       /* 用户 RSP */
    pushq %r11                           /* 用户 RFLAGS（syscall 存于 r11） */
    pushq $0x1B                          /* 用户 CS = USER_CS */
    pushq %rcx                           /* 用户 RIP（syscall 存于 rcx） */

    /* 保存除 rax/rcx/r11 外可能被 C 调用破坏的用户寄存器 */
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %r10
    pushq %r8
    pushq %r9

    /* 参数重排：用户(rax,rdi,rsi,rdx,r10,r8) -> C(rdi,rsi,rdx,rcx,r8,r9)
     * syscall_dispatch(num, a1, a2, a3, a4, a5) */
    movq  %r8,  %r9          /* a5 = 用户 r8 */
    movq  %r10, %r8          /* a4 = 用户 r10 */
    movq  %rdx, %rcx         /* a3 = 用户 rdx */
    movq  %rsi, %rdx         /* a2 = 用户 rsi */
    movq  %rdi, %rsi         /* a1 = 用户 rdi */
    movq  %rax, %rdi         /* num = 用户 rax */
    call  syscall_dispatch   /* 返回值留在 rax */

    popq  %r9
    popq  %r8
    popq  %r10
    popq  %rdx
    popq  %rsi
    popq  %rdi

    /* 栈顶现在正是 iretq 帧；iretq 恢复用户 RIP/CS/RFLAGS/RSP/SS */
    iretq

/*
 * enter_user_mode(rdi=用户RIP, rsi=用户RSP)
 * 首次从 Ring0 进入 Ring3：构造 iretq 帧并跳转。永不返回。
 * RFLAGS = 0x202（IF=1，保证用户任务可被 PIT 抢占）。
 */
.global enter_user_mode
.type enter_user_mode, @function
enter_user_mode:
    movw  $0x23, %ax         /* 数据段选择子 = USER_DS（DPL=3） */
    movw  %ax, %ds
    movw  %ax, %es
    pushq $0x23              /* SS */
    pushq %rsi               /* RSP */
    pushq $0x202             /* RFLAGS: IF=1 */
    pushq $0x1B              /* CS */
    pushq %rdi               /* RIP */
    /* 清零通用寄存器，避免向用户态泄漏内核数据 */
    xorq  %rax, %rax
    xorq  %rbx, %rbx
    xorq  %rcx, %rcx
    xorq  %rdx, %rdx
    xorq  %rsi, %rsi
    xorq  %rdi, %rdi
    xorq  %rbp, %rbp
    xorq  %r8,  %r8
    xorq  %r9,  %r9
    xorq  %r10, %r10
    xorq  %r11, %r11
    xorq  %r12, %r12
    xorq  %r13, %r13
    xorq  %r14, %r14
    xorq  %r15, %r15
    iretq

.section .bss
.align 8
g_user_rsp_scratch: .skip 8

.section .note.GNU-stack, "", @progbits
