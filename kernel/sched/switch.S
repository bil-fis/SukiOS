/*
 * kernel/sched/switch.S
 * -----------------------------------------------------------------------------
 * 内核态上下文切换与新任务跳板。
 *
 * context_switch(uint64_t *old_rsp_save /rdi/, uint64_t new_rsp /rsi/):
 *   保存被调用者保存寄存器到当前栈 -> 将当前 RSP 存入 *old_rsp_save
 *   -> 切换到 new_rsp -> 恢复被调用者保存寄存器 -> ret。
 * 被切走的任务"停泊"在本函数内；再次被调度时从此处继续返回。
 *
 * task_trampoline: 新任务首次被切入时的落地点。初始栈将 r12=entry、
 *   r13=arg 预置好，跳板据此调用 entry(arg)；若返回则调用 task_exit_current。
 */

.section .text
.code64

.global context_switch
.type context_switch, @function
context_switch:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq  %rsp, (%rdi)        /* *old_rsp_save = rsp */
    movq  %rsi, %rsp          /* rsp = new_rsp */
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbp
    popq  %rbx
    ret

.global task_trampoline
.type task_trampoline, @function
task_trampoline:
    /* 首次切入通常来自 IRQ0 中断上下文（IF=0）。必须重新开中断，
     * 否则新任务全程关中断运行，PIT 无法抢占，轮转退化为顺序执行。 */
    sti
    movq  %r13, %rdi         /* 参数 arg */
    callq *%r12              /* entry(arg) */
    movq  $0, %rdi           /* 入口正常返回 -> 退出码 0 */
    call  task_exit_current  /* 若入口返回则退出任务 */
1:  hlt
    jmp   1b

/* ---- FPU/SSE 状态保存与恢复（D2 项）---- */
/* fxsave64/fxrstor64 要求 16 字节对齐的操作数；task_t.fpu_state 已对齐。 */
.global fpu_fxsave
.type fpu_fxsave, @function
fpu_fxsave:
    fxsave64 (%rdi)
    ret

.global fpu_fxrstor
.type fpu_fxrstor, @function
fpu_fxrstor:
    fxrstor64 (%rdi)
    ret

.global fpu_fninit
.type fpu_fninit, @function
fpu_fninit:
    fninit
    ret

/* 启用 FPU：置 CR0.MP=1（监视协处理器，bit1）与 CR0.NE=1（numeric error，
 * bit5）确保 x87 异常以 #MF 而非外部 IRQ13 形式投递，且 fxsave/fxrstor 合法。
 * OSDev FPU 文档：CR0.NE 必须置位（现代 CPU 强制），否则异常行为未定义。 */
.global fpu_init
.type fpu_init, @function
fpu_init:
    fninit
    movq  %cr0, %rax
    orl   $(0x2 | 0x20), %eax   /* CR0.MP=bit1 | CR0.NE=bit5 */
    movq  %rax, %cr0
    ret

.section .note.GNU-stack, "", @progbits
