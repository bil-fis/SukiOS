/*
 * kernel/arch/x86_64/syscall_entry.S
 * -----------------------------------------------------------------------------
 * syscall 指令入口 与 Ring0->Ring3 进入函数（P0-R1 多核安全版）。
 *
 * syscall 进入时的 CPU 状态：
 *   RCX = 用户 RIP，R11 = 用户 RFLAGS，RSP = 仍是用户栈！
 *   IF 已被 IA32_FMASK 清零（进入即关中断）。
 * 本入口：经 %gs:0 取本 CPU 索引 -> 索引 per-CPU 数组切到当前任务内核栈 ->
 *          构造 iretq 帧 -> 调 C 分发器 -> iretq 返回 Ring3。
 *
 * 多核安全要点：g_syscall_kstack / g_scratch / g_utmp_rsp 均为 [MAX_CPUS]
 * 数组，用本核索引访问，杜绝多核并发 syscall 互踩。数组基址用 movabsq
 * 取 64 位绝对地址（高半区内核符号无法用 32 位绝对重定位），再以 %r11=cpu
 * 作变址。CPU 索引暂存于 %r11（caller-saved；入口处仅 RFLAGS 在 r11，
 * 改用固定帧 RFLAGS=0x202 释放该寄存器）。
 *
 * 调用关系：Ring3 syscall -> syscall_entry -> syscall_dispatch(C) -> iretq。
 */

.section .text
.code64

.extern syscall_dispatch
.extern g_syscall_kstack
.extern g_scratch
.extern g_utmp_rsp

.global syscall_entry
.type syscall_entry, @function
syscall_entry:
    /* 取本 CPU 索引（%gs:0 = percpu.cpu_index）到 %r11 */
    movl  %gs:0, %r11d

    /* 存用户 RSP 到本核 per-CPU 槽（切栈前，此时 rsp 仍是用户栈）。
     * 数组基址用 movabsq 取 64 位绝对地址，避免 32 位重定位截断。
     * 寄存器纪律（安全修复）：入口全程用 %r9 作暂存指针——%r9 不承载任何
     * syscall 参数（本内核 ABI 五参：rdi/rsi/rdx/r10/r8）。此前版本用 %r10
     * 作暂存，把用户第 4 参（如 mach_msg_recv 的 recv_limit）在保存前就
     * 覆写为内核指针，接收缓冲区长度校验被静默旁路（真实越界写风险）。 */
    movabsq $g_utmp_rsp, %r9
    movq  %rsp, (%r9,%r11,8)
    /* 切到本核当前任务的内核栈 */
    movabsq $g_syscall_kstack, %r9
    movq  (%r9,%r11,8), %rsp

    /* P0-R2 KPTI：syscall 自 Ring3 进入时 CR3=影子视图（bit12=1），上面对
     * per-CPU 数组的读写与下面的压栈均落在「影子已映射」的内核映像/任务
     * 内核栈内；现在切到内核视图，之后才能访问完整内核数据。
     * （%rax=调用号须保留，借内核栈 push/pop 一对保存。）
     * 必须先查 g_kpti_enabled（与 isr.S 入口、本文件出口一致）：KPTI 未启用
     * 时任务 PML4 为单页分配，物理地址 bit12 可合法为 1（CR3 bit12 属 PML4
     * 基址），无条件清位会把 CR3 改成垃圾页表导致三重故障。 */
    pushq %rax
    movabsq $g_kpti_enabled, %rax
    cmpb  $0, (%rax)
    je    11f                    /* KPTI 未启用：CR3 原样使用 */
    movq  %cr3, %rax
    testq $0x1000, %rax
    jz    11f
    andq  $-4097, %rax           /* 清 bit12 -> 本地址空间内核视图 */
    movq  %rax, %cr3
11: popq  %rax

    /* 构造 iretq 返回帧（自底向上压栈：SS,RSP,RFLAGS,CS,RIP；rsp 最终指向 RIP 槽）。
     * RFLAGS 固定 0x202（IF=1）；FMASK 已清 TF/DF，固定值足够并释放 r11。 */
    pushq $0x23                          /* 用户 SS = USER_DS */
    movabsq $g_utmp_rsp, %r9
    pushq (%r9,%r11,8)                   /* 用户 RSP（本核槽） */
    pushq $0x202                         /* 用户 RFLAGS: IF=1 */
    pushq $0x1B                          /* 用户 CS = USER_CS */
    pushq %rcx                           /* 用户 RIP（syscall 时 CPU 存于 rcx） */

    /* 把用户 RIP/RSP 存入本核当前任务的返回暂存（g_scratch[cpu]->scr_rip）。
     * 每任务 + 每核，杜绝多核并发 syscall 互覆盖返回帧。
     * 寄存器纪律：%r11 全程保持 CPU 索引不被覆盖（此前版本在此处把 r11
     * 覆盖为指针后又拿它当索引，产生野地址 #GP）；%r9 作暂存指针（不承载
     * 参数）；%rcx 的用户 RIP 已压入帧 RIP 槽，可安全改作暂存。 */
    movabsq $g_scratch, %r9
    movq  (%r9,%r11,8), %r9      /* r9 = &scr_rip（本核当前任务） */
    movq  %rcx, (%r9)            /* scr_rip = 用户 RIP */
    movabsq $g_utmp_rsp, %rcx
    movq  (%rcx,%r11,8), %rcx    /* rcx = 用户 RSP（本核槽） */
    movq  %rcx, 8(%r9)           /* scr_rsp = 用户 RSP */

    /* 保存可能被子 C 调用破坏的用户寄存器（%r9 此刻为暂存指针残值，属
     * caller-saved 临时值，按 ABI 无需为用户恢复，pop 时还原为无关值；
     * %r10 保有真实的用户第 4 参数） */
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

    /* execve 可能改写当前任务的 scr_rip/scr_rsp：用最新值覆盖 iretq 帧
     * （0(%rsp)=RIP 槽, 24(%rsp)=RSP 槽）。重新取本核索引（r11 已被 C 破坏）。 */
    movl  %gs:0, %r11d
    movabsq $g_scratch, %r10
    movq  (%r10,%r11,8), %r11   /* r11 = &scr_rip（本核当前任务） */
    movq  (%r11), %rcx          /* rcx = scr_rip（execve 可能改写过） */
    movq  %rcx, 0(%rsp)         /* 帧 RIP 槽 = scr_rip */
    movq  8(%r11), %rcx         /* rcx = scr_rsp */
    movq  %rcx, 24(%rsp)        /* 帧 RSP 槽 = scr_rsp */

    /* P0-R2 KPTI：返回 Ring3 前切回影子视图（bit12=1）。
     * 不变量：此刻 CR3 = 当前任务地址空间的内核视图（普通 syscall 未动
     * CR3；execve 已 vmm_switch(new_as)；schedule 恢复亦写 task->cr3）——
     * 成对分配保证 |0x1000 即其影子。切换后仅剩 iretq 一条指令：取指自
     * 内核映像（影子已映射），帧读自任务内核栈（影子已映射），不会缺页。
     * NMI 若命中该微窗口：isr.S 保存-还原 saved_cr3，退出时如实还原影子，
     * 不产生「内核视图带回 Ring3」旁路。%rcx 此刻空闲（RIP 已在帧内）。 */
    movabsq $g_kpti_enabled, %rcx
    cmpb  $0, (%rcx)
    je    12f
    movq  %cr3, %rcx
    orq   $0x1000, %rcx
    movq  %rcx, %cr3
12:
    /* 栈顶现在正是 iretq 帧；iretq 恢复用户 RIP/CS/RFLAGS/RSP/SS */
    iretq

/*
 * enter_user_mode(rdi=用户RIP, rsi=用户RSP)
 * 首次从 Ring0 进入 Ring3：构造 iretq 帧并跳转。永不返回。
 * RFLAGS = 0x202（IF=1，保证用户任务可被定时器抢占）。
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
    /* P0-R2 KPTI：首次进入 Ring3 前切到影子视图。调用方（user_task_thunk）
     * 经 schedule/vmm_switch 已置 CR3=本任务内核视图（成对偶页），|0x1000
     * 即影子。切换后的取指（xorq 序列/iretq）在内核映像内、帧读在任务内核
     * 栈上，均已映射进影子。KPTI 未启用时跳过（CR3 保持单页 PML4 不动）。 */
    movabsq $g_kpti_enabled, %rax
    cmpb  $0, (%rax)
    je    21f
    movq  %cr3, %rax
    orq   $0x1000, %rax
    movq  %rax, %cr3
21:
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

.section .note.GNU-stack, "", @progbits
