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

; ---- Double Fault 专用栈（IST1, 16 KB） ----
; Double Fault 可能在内核栈已损坏时触发，必须使用独立栈
; 参考：Intel SDM Vol.3 6.15, OSDev Double Fault
section .bss align=16
global double_fault_stack_top
global double_fault_stack_bottom
double_fault_stack_bottom:
    resb 16384
double_fault_stack_top:

; ---- NMI 专用栈（IST2, 16 KB） ----
; NMI 可在任何时刻（包括中断处理过程中）中断内核，必须使用独立栈
; 参考：Intel SDM Vol.3 6.7, OSDev NMI
global nmi_stack_top
global nmi_stack_bottom
nmi_stack_bottom:
    resb 16384
nmi_stack_top:

; ---- Machine Check 专用栈（IST3, 16 KB） ----
; Machine Check 是不可屏蔽的 abort，可随时发生，必须使用独立栈
; 参考：Intel SDM Vol.3 15, OSDev Machine Check
global machine_check_stack_top
global machine_check_stack_bottom
machine_check_stack_bottom:
    resb 16384
machine_check_stack_top:

; ---- 内核中断栈（TSS.RSP0, ring3→ring0 中断时使用, 16 KB） ----
; 当 CPU 从 ring3 接收中断时，自动切换到此栈（TSS.RSP0）
; 参考：Intel SDM Vol.3 6.12.1, OSDev Task State Segment
global kernel_interrupt_stack_top
global kernel_interrupt_stack_bottom
kernel_interrupt_stack_bottom:
    resb 16384
kernel_interrupt_stack_top:

; ---- 伪中断处理存根（ISR 255） ----
; APIC 启用后会产生伪中断，需要发送 EOI 并返回
; 参考：OSDev APIC - Spurious Interrupt Vector Register
section .text

extern spurious_irq_handler

global isr_spurious
isr_spurious:
    push qword 0                  ; 占位错误码
    push qword 255                ; 中断号 = 0xFF
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

; =============================================================================
; IRQ 存根（中断请求）
;
; IRQ 0-15 对应向量 0x20-0x2F（偏移 IRQ_BASE）
; 与 CPU 异常存根共享相同的寄存器保存/恢复逻辑。
;
; 每个 IRQ 存根：
;   1. 压入 0（IRQ 无错误码）
;   2. 压入中断号
;   3. 跳转到 irq_common_stub
;
; 注意：IRQ 存根不自动发送 LAPIC EOI，由 C 处理函数负责。
; =============================================================================

extern irq_handler

; 用于生成 IRQ 存根的宏（与 ISR_NOERRCODE 结构相同）
%macro IRQ 1
global irq%1
irq%1:
    push qword 0              ; 占位错误码
    push qword %1             ; 中断号
    jmp irq_common_stub
%endmacro

; IRQ 存根 0-15
IRQ 32    ; IRQ0: 定时器 (LAPIC Timer)
IRQ 33    ; IRQ1: PS/2 键盘
IRQ 34    ; IRQ2: 级联（APIC 模式下未使用）
IRQ 35    ; IRQ3: 串口 2/4
IRQ 36    ; IRQ4: 串口 1/3
IRQ 37    ; IRQ5: 并口 2 / 声卡
IRQ 38    ; IRQ6: 软盘控制器
IRQ 39    ; IRQ7: 并口 1
IRQ 40    ; IRQ8: RTC (实时时钟)
IRQ 41    ; IRQ9: ACPI / SCI
IRQ 42    ; IRQ10: 未使用
IRQ 43    ; IRQ11: 未使用
IRQ 44    ; IRQ12: PS/2 鼠标
IRQ 45    ; IRQ13: FPU / 协处理器
IRQ 46    ; IRQ14: 主 ATA (IDE)
IRQ 47    ; IRQ15: 从 ATA (IDE)

; IRQ 通用处理存根
irq_common_stub:
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

    ; RDI = 指向 interrupt_frame 的指针
    mov rdi, rsp
    call irq_handler

    ; 恢复所有寄存器
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

; 消除 "missing .note.GNU-stack" 链接器警告
section .note.GNU-stack noalloc noexec nowrite progbits
