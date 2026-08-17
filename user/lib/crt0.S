/*
 * user/lib/crt0.S
 * -----------------------------------------------------------------------------
 * Ring3 用户程序入口（ELF 加载后从这里开始执行）。
 *
 * ELF 加载器已构造好符合 System V AMD64 ABI 的初始用户栈：
 *   [rsp]        = argc
 *   [rsp+8]      = argv[0] ...
 *   [rsp+8+argc*8] = NULL
 *   ...           envp[], auxv[]
 * 本入口读取 argc/argv/envp 并调用 main(int argc, char **argv, char **envp)，
 * main 返回值作为退出码经由 SYS_TASK_EXIT 退出。
 */
.section .text.start
.code64
.global _start
.extern main
_start:
    popq  %rdi                       /* argc */
    movq  %rsp, %rsi                /* argv = 弹出 argc 后的栈顶，指向 argv[0] */
    movq  %rdi, %rax
    leaq  8(%rsp,%rax,8), %rdx      /* envp = argv + (argc+1)*8 */
    andq  $-16, %rsp                /* ABI：16 字节栈对齐 */
    call  main
    movq  %rax, %rdi                /* 退出码 = main 返回值 */
    movq  $2, %rax                  /* SYS_TASK_EXIT */
    syscall
1:  jmp   1b

.section .note.GNU-stack, "", @progbits
