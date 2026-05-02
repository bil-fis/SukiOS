; =============================================================================
; SukiOS - 用户态测试程序（Ring 3）
;
; 此代码运行在 Ring 3（用户态），通过 syscall 与内核通信。
; 参考：OSDev SYSCALL, AMD64 APM Vol.2 12.4.1
;
; syscall 约定（Linux x86_64 ABI）：
;   RAX = 系统调用编号
;   RDI, RSI, RDX, R10, R8, R9 = 参数
;   返回值在 RAX
;
; 可用系统调用：
;   1 = write(char c)    输出一个字符到终端
;   2 = read()           读取一个键盘字符
;   3 = getpid()         获取进程 ID
;   60 = exit(code)      退出程序
; =============================================================================

bits 64
default rel

section .text

; 外部符号（无 — Ring 3 切换由 idt_stubs.asm 的 enter_ring3 完成）

global userland_entry

; ---- 用户态入口（由 enter_ring3 通过 iretq 跳转至此） ----
%define SYS_WRITE   1
%define SYS_READ    2
%define SYS_GETPID  3
%define SYS_EXIT    60

; ---- 辅助宏 ----

; syscall 写入一个字符: syscall_write(char)
%macro SYSCALL_WRITE 1
    mov rax, SYS_WRITE
    mov rdi, %1
    syscall
%endmacro

; syscall 读取一个字符: 返回值在 RAX
%macro SYSCALL_READ 0
    mov rax, SYS_READ
    syscall
%endmacro

; syscall 退出: syscall_exit(code)
%macro SYSCALL_EXIT 1
    mov rax, SYS_EXIT
    mov rdi, %1
    syscall
%endmacro

; ---- 用户态入口 ----
userland_entry:
    ; 打印欢迎信息
    SYSCALL_WRITE 'U'
    SYSCALL_WRITE 's'
    SYSCALL_WRITE 'e'
    SYSCALL_WRITE 'r'
    SYSCALL_WRITE ' '
    SYSCALL_WRITE 'M'
    SYSCALL_WRITE 'o'
    SYSCALL_WRITE 'd'
    SYSCALL_WRITE 'e'
    SYSCALL_WRITE ' '
    SYSCALL_WRITE '('
    SYSCALL_WRITE 'R'
    SYSCALL_WRITE 'i'
    SYSCALL_WRITE 'n'
    SYSCALL_WRITE 'g'
    SYSCALL_WRITE ' '
    SYSCALL_WRITE '3'
    SYSCALL_WRITE ')'
    SYSCALL_WRITE ':'
    SYSCALL_WRITE ' '
    SYSCALL_WRITE 'O'
    SYSCALL_WRITE 'K'
    SYSCALL_WRITE '\n'

    ; 进入键盘回显循环
.read_loop:
    SYSCALL_READ          ; 读取键盘字符，返回值在 RAX
    test al, al           ; 如果 RAX=0，没有字符
    jz .read_loop

    ; 检查是否是 ESC (ASCII 27)
    cmp al, 27
    je .exit_program

    ; 回显字符
    mov rax, SYS_WRITE
    movzx edi, al          ; 零扩展 al → edi（自动清零 rdi 高32位）
    syscall
    jmp .read_loop

.exit_program:
    SYSCALL_WRITE '\n'
    SYSCALL_WRITE 'B'
    SYSCALL_WRITE 'y'
    SYSCALL_WRITE 'e'
    SYSCALL_WRITE '\n'
    SYSCALL_EXIT 0

; 消除链接器警告
section .note.GNU-stack noalloc noexec nowrite progbits
