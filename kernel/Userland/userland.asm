; =============================================================================
; SukiOS - 用户态测试程序（Ring 3，使用 int 0x80 系统调用）
; =============================================================================

bits 64
default rel

section .text
global userland_entry

%define SYS_WRITE   1
%define SYS_READ    2
%define SYS_GETPID  3
%define SYS_EXIT    60

%macro SYSCALL_WRITE 1
    mov rax, SYS_WRITE
    mov rdi, %1
    int 0x80
%endmacro

%macro SYSCALL_READ 0
    mov rax, SYS_READ
    int 0x80
%endmacro

%macro SYSCALL_EXIT 1
    mov rax, SYS_EXIT
    mov rdi, %1
    int 0x80
%endmacro

userland_entry:
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

.read_loop:
    SYSCALL_READ           ; 返回字符在 RAX 中
    test al, al
    jz .read_loop          ; 无输入，继续等待

    cmp al, 27             ; ESC 键退出
    je .exit_program

    ; ---- 修正回显逻辑 ----
    movzx edi, al          ; 先将字符保存到 RDI（零扩展）
    mov rax, SYS_WRITE     ; 再设置系统调用号
    int 0x80               ; 调用 write
    jmp .read_loop

.exit_program:
    SYSCALL_WRITE '\n'
    SYSCALL_WRITE 'B'
    SYSCALL_WRITE 'y'
    SYSCALL_WRITE 'e'
    SYSCALL_WRITE '\n'
    SYSCALL_EXIT 0

section .note.GNU-stack noalloc noexec nowrite progbits