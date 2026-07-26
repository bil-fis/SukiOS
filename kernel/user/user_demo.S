/*
 * kernel/user/user_demo.S
 * -----------------------------------------------------------------------------
 * Ring3 演示程序（阶段六验证用）。
 *
 * 以位置无关代码 (RIP 相对寻址) 编入内核镜像，加载时由 task_create_user
 * 拷贝到用户地址空间 0x400000 处执行。它只使用 SukiOS syscall ABI：
 *   rax=调用号, rdi/rsi=参数, syscall 指令。
 *
 * 流程：debug_write(hello) -> yield -> debug_write(back) -> task_exit(42)。
 */

.section .rodata
.code64

.global user_demo_start
.global user_demo_end

user_demo_start:
    /* sys_debug_write(msg1, len1) */
    movq  $4, %rax
    leaq  msg1(%rip), %rdi
    movq  $(msg1_end - msg1), %rsi
    syscall

    /* sys_yield() */
    movq  $3, %rax
    syscall

    /* sys_debug_write(msg2, len2) */
    movq  $4, %rax
    leaq  msg2(%rip), %rdi
    movq  $(msg2_end - msg2), %rsi
    syscall

    /* 演示 copy_from_user 的指针校验：传内核地址，应返回 -1 而非崩溃 */
    movq  $4, %rax
    movabsq $0xFFFF800000100000, %rdi
    movq  $16, %rsi
    syscall

    /* sys_task_exit(42) */
    movq  $2, %rax
    movq  $42, %rdi
    syscall

1:  jmp 1b                      /* 不应到达 */

msg1: .ascii "  [ring3] hello from user mode! (sys_debug_write via copy_from_user)\n"
msg1_end:
msg2: .ascii "  [ring3] back after sys_yield, exiting with code 42\n"
msg2_end:

user_demo_end:

.section .note.GNU-stack, "", @progbits
