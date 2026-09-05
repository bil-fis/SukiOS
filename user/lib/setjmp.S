/*
 * user/lib/setjmp.S
 * -----------------------------------------------------------------------------
 * x86_64 SysV 的 setjmp/longjmp 实现（供 FreeType 等使用）。
 *
 * jmp_buf 布局（与 setjmp.h 一致）：8 个 uint64_t
 *   [0]=RBX [1]=RBP [2]=R12 [3]=R13 [4]=R14 [5]=R15 [6]=RSP [7]=RIP(返回地址)
 *
 * 约定：
 *   setjmp  : %rdi = jmp_buf* ；成功后返回 0
 *   longjmp : %rdi = jmp_buf* ，%rsi = 返回码（val）
 *   注意 longjmp 必须返回一个"非零值"，若 val==0 则强制返回 1。
 *
 * 使用 noinline 纯汇编，Clobber 包含全部被修改的寄存器。
 */
    .intel_syntax noprefix
    .text

    .globl setjmp
    .type  setjmp, @function
setjmp:
    mov [rdi+0*8], rbx
    mov [rdi+1*8], rbp
    mov [rdi+2*8], r12
    mov [rdi+3*8], r13
    mov [rdi+4*8], r14
    mov [rdi+5*8], r15
    mov [rdi+6*8], rsp
    /* 返回地址 = 调用 setjmp 的下一条指令（当前 rsp 处的值，但调用约定里
       返回地址在 [rsp]，而 setjmp 自身未压栈；用 rip 寄存器读取当前指令后的值 */
    lea rax, [rip+setjmp_ret]
    mov [rdi+7*8], rax
    xor eax, eax          /* 返回 0 */
setjmp_ret:
    ret

    .globl longjmp
    .type  longjmp, @function
longjmp:
    mov rbx, [rdi+0*8]
    mov rbp, [rdi+1*8]
    mov r12, [rdi+2*8]
    mov r13, [rdi+3*8]
    mov r14, [rdi+4*8]
    mov r15, [rdi+5*8]
    mov rsp, [rdi+6*8]
    mov rax, [rdi+7*8]    /* 恢复返回地址 */
    /* val 处理：若 rsi==0 则置 1 */
    test esi, esi
    jnz  .Lval_ok
    mov esi, 1
.Lval_ok:
    mov eax, esi          /* longjmp 返回 val（非0） */
    /* 跳回 setjmp 的返回点：把恢复出的 RIP 压栈，ret */
    push rax              /* 占位（ret 用 rax 值经 rsp 不行，下面直接 jmp） */
    /* 实际用 jmp 到恢复地址更稳（longjmp 不返回原栈帧，rsp 已恢复） */
    add rsp, 8            /* 撤销上面的 push */
    jmp rax
