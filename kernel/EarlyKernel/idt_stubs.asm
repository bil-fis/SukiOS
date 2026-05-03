; =============================================================================
; SukiOS - x86_64 ISR 存根（中断服务例程）
; 修复内容：
;   - isr_common_stub 中开启中断 (sti)，返回前关闭 (cli)
;   - irq_common_stub 保存 g_current_proc->rsp
;   - 所有原有的功能保持不变
; =============================================================================

bits 64
default rel

section .text

extern exception_handler
extern irq_handler
extern spurious_irq_handler
extern g_current_proc

%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push qword 0
    push qword %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERRCODE 1
global isr%1
isr%1:
    push qword %1
    jmp isr_common_stub
%endmacro

; CPU 异常向量 0-31
ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_ERRCODE   21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_NOERRCODE 30
ISR_NOERRCODE 31

; ---- int 0x80 用户态系统调用入口 ----
global isr80
isr80:
    push qword 0          ; 无错误码
    push qword 0x80       ; 中断号 = 128
    jmp isr_common_stub

; ---- 通用 ISR 存根（异常 + int 0x80） ----
isr_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; ---- 保存当前 RSP 到 g_current_proc->rsp ----
    mov rax, [g_current_proc]
    test rax, rax
    jz .no_save
    mov [rax], rsp
.no_save:

    sti                         ; ★ 重新开启中断，允许键盘、定时器等 IRQ 到达

    mov rdi, rsp
    call exception_handler

    cli                         ; ★ 返回前关中断，保护后续的寄存器弹出操作

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16
    iretq

; ---- 专用栈定义 ----
section .bss align=16
global double_fault_stack_top, double_fault_stack_bottom
global nmi_stack_top, nmi_stack_bottom
global machine_check_stack_top, machine_check_stack_bottom
global kernel_interrupt_stack_top, kernel_interrupt_stack_bottom

double_fault_stack_bottom:    resb 16384
double_fault_stack_top:
nmi_stack_bottom:             resb 16384
nmi_stack_top:
machine_check_stack_bottom:   resb 16384
machine_check_stack_top:
kernel_interrupt_stack_bottom: resb 16384
kernel_interrupt_stack_top:

; ---- 伪中断 ISR 255 ----
section .text
global isr_spurious
isr_spurious:
    push qword 0
    push qword 255
    jmp spurious_common_stub

spurious_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rdi, rsp
    call spurious_irq_handler
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

; ---- IRQ 存根 ----
%macro IRQ 1
global irq%1
irq%1:
    push qword 0
    push qword %1
    jmp irq_common_stub
%endmacro

IRQ 32
IRQ 33
IRQ 34
IRQ 35
IRQ 36
IRQ 37
IRQ 38
IRQ 39
IRQ 40
IRQ 41
IRQ 42
IRQ 43
IRQ 44
IRQ 45
IRQ 46
IRQ 47

irq_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; ---- 保存当前 RSP 到 g_current_proc->rsp ----
    mov rax, [g_current_proc]
    test rax, rax
    jz .no_save
    mov [rax], rsp
.no_save:

    mov rdi, rsp
    call irq_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

; ---- syscall 存根（保留以备将来使用） ----
extern syscall_handler
%define GDT_SELECTOR_USER_CS 0x18
%define GDT_SELECTOR_USER_DS 0x20

section .text
global syscall_stub
global user_rsp_save
syscall_stub:
    mov [rel user_rsp_save], rsp
    lea rsp, [rel kernel_interrupt_stack_top]
    push rax
    push rdi
    push rsi
    push rdx
    push r10
    push r8
    push r9
    push rcx
    push r11
    mov rdi, rsp
    call syscall_handler
    pop r11
    pop rcx
    pop r9
    pop r8
    pop r10
    pop rdx
    pop rsi
    pop rdi
    add rsp, 8
    mov rsp, [rel user_rsp_save]
    sysretq

section .bss align=8
user_rsp_save: resq 1

; ---- Ring 3 切换存根 ----
section .text
global enter_ring3
enter_ring3:
    cli
    mov [rel user_rsp_save], rsi
    mov ax, (GDT_SELECTOR_USER_DS | 3)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push (GDT_SELECTOR_USER_DS | 3)
    push rsi
    pushfq
    or qword [rsp], 0x200
    push (GDT_SELECTOR_USER_CS | 3)
    push rdi
    iretq

section .data
extern g_current_proc