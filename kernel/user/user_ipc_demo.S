/*
 * kernel/user/user_ipc_demo.S
 * -----------------------------------------------------------------------------
 * Ring3 IPC 演示程序（阶段七验证用）。
 *
 * 1. 向 CONSOLE_PORT(5) 发送内联小消息（文本），由内核 console-srv 打印。
 * 2. OOL 零拷贝自环测试：把自身代码页 (0x400000, 4KB) 作为 OOL 描述符
 *    发到 DISPLAY_PORT(3)，再从该端口接收；内核应将同一物理页重映射到
 *    本进程 OOL 窗口 (0x600000000000)。比较两个虚拟地址的前 8 字节，
 *    一致即证明零拷贝（同一物理页两个映射）。
 *
 * 消息布局：header(24B) [+ ool_desc(16B)] [+ payload]
 * syscall ABI：rax=0(mach_msg), rdi=msg, rsi=option, rdx=send_size,
 *              r10=recv_limit, r8=port。
 */

.section .rodata
.code64

.global user_ipc_demo_start
.global user_ipc_demo_end

user_ipc_demo_start:
    subq  $192, %rsp                 /* 栈上消息缓冲 */

    /* ---- 1. 内联文本消息 -> CONSOLE_PORT(5) ---- */
    movl  $0, 0(%rsp)                /* msgh_bits = 0（无 OOL） */
    movl  $(24 + txt_end - txt), 4(%rsp)  /* msgh_size */
    movl  $5, 8(%rsp)                /* remote = CONSOLE_PORT */
    movl  $0, 12(%rsp)               /* local = 0 */
    movl  $1, 16(%rsp)               /* id = 1 (text) */
    movl  $0, 20(%rsp)
    leaq  txt(%rip), %rsi            /* 拷贝文本负载到 header 之后 */
    leaq  24(%rsp), %rdi
    movq  $(txt_end - txt), %rcx
    cld
    rep movsb

    movq  $0, %rax                   /* sys_mach_msg */
    movq  %rsp, %rdi
    movq  $1, %rsi                   /* MACH_SEND_MSG */
    movq  $(24 + txt_end - txt), %rdx
    movq  $0, %r10
    movq  $0, %r8
    syscall

    /* ---- 2. OOL 自环：发送自身代码页 -> DISPLAY_PORT(3) ---- */
    movl  $0x80000000, 0(%rsp)       /* msgh_bits = MACH_MSGH_BITS_OOL */
    movl  $40, 4(%rsp)               /* size = 24 + 16 */
    movl  $3, 8(%rsp)                /* remote = DISPLAY_PORT */
    movl  $0, 12(%rsp)
    movl  $2, 16(%rsp)               /* id = 2 (ool test) */
    movl  $0, 20(%rsp)
    movq  $0x400000, 24(%rsp)        /* ool.address = 本程序代码页 */
    movq  $4096, 32(%rsp)            /* ool.size */

    movq  $0, %rax
    movq  %rsp, %rdi
    movq  $1, %rsi                   /* SEND */
    movq  $40, %rdx
    movq  $0, %r10
    movq  $0, %r8
    syscall

    /* 从 DISPLAY_PORT 接收回来（内核会把物理页映射到 OOL 窗口并改写描述符） */
    movq  $0, %rax
    movq  %rsp, %rdi
    movq  $2, %rsi                   /* MACH_RECV_MSG */
    movq  $0, %rdx
    movq  $128, %r10                 /* recv_limit */
    movq  $3, %r8                    /* port = DISPLAY_PORT */
    syscall

    /* 校验：*(desc.address) == *(0x400000) ? */
    movq  24(%rsp), %rbx             /* rbx = 内核映射后的 OOL 窗口 VA */
    movq  $0x400000, %rdx
    movq  (%rdx), %rdx               /* 原代码页前 8 字节 */
    cmpq  %rdx, (%rbx)
    jne   1f

    movq  $4, %rax                   /* sys_debug_write(ok) */
    leaq  ok(%rip), %rdi
    movq  $(ok_end - ok), %rsi
    syscall
    jmp   2f
1:
    movq  $4, %rax
    leaq  bad(%rip), %rdi
    movq  $(bad_end - bad), %rsi
    syscall
2:
    movq  $2, %rax                   /* sys_task_exit(0) */
    xorq  %rdi, %rdi
    syscall
3:  jmp 3b

txt: .ascii "hello from Ring3 via mach_msg -> CONSOLE_PORT!\n"
txt_end:
ok:  .ascii "  [ring3-ipc] OOL zero-copy verified: same phys page, two mappings\n"
ok_end:
bad: .ascii "  [ring3-ipc] OOL FAILED: data mismatch\n"
bad_end:

user_ipc_demo_end:

.section .note.GNU-stack, "", @progbits
