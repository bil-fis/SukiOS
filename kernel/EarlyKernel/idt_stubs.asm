; =============================================================================
; SukiOS - x86_64 ISR 存根（中断服务例程）
;
; 参考：OSDev IDT, Intel SDM Vol.3 6.14
;
; 每个 ISR 存根：
;   1. 如果 CPU 不自动压入错误码，则压入 0（保持栈布局一致）
;   2. 压入中断号
;   3. 保存所有通用寄存器（System V ABI callee-saved）
;   4. 调用 C 语言 exception_handler
;   5. 恢复寄存器并 iretq
;
; 注意：long mode 中必须使用 iretq（不是 iret）
; =============================================================================

bits 64
default rel

section .text

extern exception_handler

%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push qword 0              ; 压入占位错误码
    push qword %1             ; 压入中断号
    jmp isr_common_stub
%endmacro

%macro ISR_ERRCODE 1
global isr%1
isr%1:
    ; CPU 已自动压入错误码，只需压入中断号
    push qword %1
    jmp isr_common_stub
%endmacro

; ---- CPU 异常向量 0-31 ----
; 无错误码的异常
ISR_NOERRCODE 0     ; #DE 除零错误
ISR_NOERRCODE 1     ; #DB 调试异常
ISR_NOERRCODE 2     ; NMI 不可屏蔽中断
ISR_NOERRCODE 3     ; #BP 断点
ISR_NOERRCODE 4     ; #OF 溢出
ISR_NOERRCODE 5     ; #BR 边界检查
ISR_NOERRCODE 6     ; #UD 无效操作码
ISR_NOERRCODE 7     ; #NM 设备不可用
ISR_ERRCODE   8     ; #DF 双重错误（有错误码）
ISR_NOERRCODE 9     ; 协处理器段越界
ISR_ERRCODE  10     ; #TS 无效 TSS
ISR_ERRCODE  11     ; #NP 段不存在
ISR_ERRCODE  12     ; #SS 栈段错误
ISR_ERRCODE  13     ; #GP 一般保护错误
ISR_ERRCODE  14     ; #PF 页错误
ISR_NOERRCODE 15    ; 保留
ISR_NOERRCODE 16    ; #MF x87 FPU 错误
ISR_ERRCODE  17     ; #AC 对齐检查
ISR_NOERRCODE 18    ; #MC 机器检查
ISR_NOERRCODE 19    ; #XM SIMD 浮点异常
ISR_NOERRCODE 20    ; #VE 虚拟化异常
ISR_ERRCODE  21     ; #CP 控制保护
ISR_NOERRCODE 22    ; 保留
ISR_NOERRCODE 23    ; 保留
ISR_NOERRCODE 24    ; 保留
ISR_NOERRCODE 25    ; 保留
ISR_NOERRCODE 26    ; 保留
ISR_NOERRCODE 27    ; 保留
ISR_NOERRCODE 28    ; 保留
ISR_NOERRCODE 29    ; 保留
ISR_NOERRCODE 30    ; 保留
ISR_NOERRCODE 31    ; 保留

; ---- 通用 ISR 存根 ----
isr_common_stub:
    ; 保存所有通用寄存器（与 interrupt_frame 结构体顺序一致）
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

    ; RDI = 指向 interrupt_frame 的指针（System V ABI 第一个参数）
    mov rdi, rsp
    call exception_handler

    ; 恢复所有寄存器（逆序弹出）
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

    ; 弹出错误码和中断号（共 16 字节）
    add rsp, 16
    iretq

; ---- Double Fault 专用栈（16 KB） ----
section .bss align=16
global double_fault_stack_top
global double_fault_stack_bottom
double_fault_stack_bottom:
    resb 16384
double_fault_stack_top:
