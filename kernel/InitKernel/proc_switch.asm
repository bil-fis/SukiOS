; proc_switch.asm - Process Context Switch (x86_64)
; Reference: OSDev Brendan's Multi-tasking tutorial, software switching

bits 64
default rel

section .text
global switch_to_proc

; void switch_to_proc(pcb_t *next);
; rdi = next
switch_to_proc:
    ; 1. Load new stack pointer (points to register save area, r15 first)
    mov rsp, [rdi]          ; rsp = next->rsp

    ; 2. Restore general registers
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

    ; 3. Skip int_no and err_code
    add rsp, 16

    ; 4. Return to the interrupted execution (possibly ring 3)
    iretq