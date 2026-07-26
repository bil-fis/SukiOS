/*
 * user/lib/crt0.S
 * -----------------------------------------------------------------------------
 * Ring3 用户程序入口：位于二进制最前端 (.text.start)，内核把程序装载到
 * 0x400000 后从这里开始执行。调用 main()，返回值作为退出码 sys_task_exit。
 */
.section .text.start
.code64
.global _start
.extern main
_start:
    andq  $-16, %rsp          /* 对齐 System V ABI 要求的 16 字节栈 */
    call  main
    movq  %rax, %rdi          /* 退出码 = main 返回值 */
    movq  $2, %rax            /* SYS_TASK_EXIT */
    syscall
1:  jmp 1b

.section .note.GNU-stack, "", @progbits
