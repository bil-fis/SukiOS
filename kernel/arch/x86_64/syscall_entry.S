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
    /* 此刻仍在用户栈上（RCX=用户RIP, R11=用户RFLAGS, IF=0）。
     * 先把用户 RSP 存入短命全局暂存 g_utmp_rsp —— 仅在本入口的关中断窗口
     * 内使用（IA32_FMASK 已清 IF，单核无并发），随后立即切到内核栈。
     * 注意：此处不能使用任何通用寄存器作暂存 —— rax=调用号、rcx=用户RIP、
     * r11=用户RFLAGS 都必须保持，其余寄存器属于用户须完整保存恢复。 */
    movq  %rsp, g_utmp_rsp(%rip)
    movq  g_syscall_kstack(%rip), %rsp

    /* 构造 iretq 返回帧（自底向上：SS,RSP,RFLAGS,CS,RIP） */
    pushq $0x23                          /* 用户 SS = USER_DS */
    pushq g_utmp_rsp(%rip)               /* 用户 RSP */
    /* RFLAGS：syscall 进入时 IA32_FMASK 强制清除了 IF，r11 中 IF=0。
     * 直接在 r11 上把 IF(位9)置 1 作为帧槽 —— 注意：不可用 rax 计算，
     * 否则会破坏 rax 中的系统调用号（num），导致内核以 RFLAGS 值当作
     * 调用号分发（典型表现：unknown syscall 582 = RFLAGS 0x246）。 */
    orq   $0x200, %r11
    pushq %r11                           /* 用户 RFLAGS（强制 IF=1，rax 不变） */
    pushq $0x1B                          /* 用户 CS = USER_CS */
    pushq %rcx                           /* 用户 RIP（syscall 时 CPU 存于 rcx） */

    /* 把用户 RIP/RSP 存入“当前任务”的返回暂存（g_scratch 指向当前任务的
     * scr_rip 字段）。必须是每任务的：若用全局变量，阻塞于 recv 的任务被
     * 其它任务的 syscall 覆盖该全局后，返回时 RSP 会被破坏（用户态缺页）。
     * 此刻帧已构建完毕，rcx/r11 可自由用作暂存。 */
    movq  g_scratch(%rip), %r11   /* r11 = &scr_rip（当前任务） */
    movq  %rcx, (%r11)            /* scr_rip = 用户 RIP */
    movq  g_utmp_rsp(%rip), %rcx
    movq  %rcx, 8(%r11)           /* scr_rsp = 用户 RSP */

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

    /* execve 可能在 syscall_dispatch 中改写了当前任务的 scr_rip/scr_rsp：
     * 用最新值覆盖 iretq 帧中的 RIP(帧低槽) 与 RSP(帧+24 槽)，使返回
     * 跳转到新程序入口而非原程序断点（否则 execve 会返回到 shell）。
     * 注意：必须用 rcx 作暂存，绝不能破坏 rax —— rax 此刻是系统调用
     * 返回值，iretq 后原样交还用户态（否则所有 syscall 返回值被破坏，
     * 表现为 mach_msg_send 等返回“垃圾”而非 0）。 */
    movq  g_scratch(%rip), %r11   /* r11 = &scr_rip（当前任务） */
    movq  (%r11), %rcx            /* rcx = scr_rip（execve 可能改写过） */
    movq  %rcx, 0(%rsp)           /* 帧 low 槽 = RIP */
    movq  8(%r11), %rcx           /* rcx = scr_rsp */
    movq  %rcx, 24(%rsp)          /* 帧 +24 槽 = RSP */

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

/* 短命暂存：仅在 syscall_entry 关中断窗口内保存用户 RSP（单核安全） */
.section .bss
.align 8
g_utmp_rsp: .skip 8

.section .note.GNU-stack, "", @progbits
