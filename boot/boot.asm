; =============================================================================
; SukiOS - Multiboot2 x86_64 引导程序（身份映射）
; 参考：OSDev Wiki - Multiboot2 规范、Setting Up Long Mode
;
; GRUB (Multiboot2) 以 ELF64 格式加载此文件，进入 32 位保护模式。
; 本代码完成 32 位保护模式 → 64 位长模式的切换，然后跳转到 kernel_main。
;
; 内存布局：
;   物理地址 = 虚拟地址（身份映射），内核加载到 ~1MB 处
;   页表映射前 128MB（使用 2MB 大页，只需 3 张页表）
;
; 编译：nasm -f elf64 boot.asm -o boot.o
; =============================================================================

; Multiboot2 常量
MB2_MAGIC       equ 0xE85250D6           ; Multiboot2 头部魔数
MB2_ARCH_I386   equ 0                    ; i386 架构（GRUB 兼容）
MB2_TAG_END     equ 0                    ; 结束标签类型
MB2_TAG_FB      equ 5                    ; 帧缓冲区请求标签类型
MB2_TAG_OPT     equ 1                    ; 可选标志位

; 页表物理地址（低于 1MB 的安全区域）
PML4_ADDR       equ 0x70000              ; PML4 表（4KB）
PDPTE_ADDR      equ 0x71000              ; PDPTE 表（4KB）
PD_ADDR         equ 0x72000              ; PD 表（4KB，使用 2MB 大页无需 PT）

; GDT 访问字节标志位
PRESENT         equ (1 << 7)             ; 段存在位
NOT_SYS         equ (1 << 4)             ; 非系统段
EXEC            equ (1 << 3)             ; 可执行
RW              equ (1 << 1)             ; 可读写

; GDT 标志字节位
GRAN_4K         equ (1 << 7)             ; 4KB 粒度
SZ_32           equ (1 << 6)             ; 32 位段
LONG_MODE       equ (1 << 5)             ; 长模式标志

; ==========================================================================
; Multiboot2 头部
; 必须位于文件前 32KB 内，8 字节对齐
; 参考：Multiboot2 规范 3.1.1 - 3.1.3
; ==========================================================================
section .multiboot align=8

multiboot_header:
    dd  MB2_MAGIC                                      ; 魔数
    dd  MB2_ARCH_I386                                  ; 架构
    dd  multiboot_header_end - multiboot_header         ; 头部长度
    dd  -(MB2_MAGIC + MB2_ARCH_I386 + (multiboot_header_end - multiboot_header))  ; 校验和

; 结束标签（必须，类型=0，大小=8）
end_tag:
    dw  MB2_TAG_END                  ; 类型 = 0
    dw  0                            ; 标志
    dd  end_tag_end - end_tag        ; 大小 = 8
end_tag_end:

multiboot_header_end:

; ==========================================================================
; 64 位 GDT（放在专用段中，引导代码可直接访问）
; 参考：OSDev "Setting Up Long Mode" - GDT 部分
; 参考：AMD64 APM Vol.2 第 4.8.1-4.8.2 章
; ==========================================================================
section .bootstrap_rodata align=16

gdt64:
.null: equ $ - gdt64
    dq 0                                          ; 空描述符（必须）

.code_seg: equ $ - gdt64
    dw 0xFFFF                                     ; 段限长低 16 位
    dw 0                                          ; 基址低 16 位
    db 0                                          ; 基址中 8 位
    db PRESENT | NOT_SYS | EXEC | RW              ; 访问字节：存在、代码段、可读
    db GRAN_4K | LONG_MODE | 0xF                  ; 标志 + 段限长高 4 位
    db 0                                          ; 基址高 8 位

.data_seg: equ $ - gdt64
    dw 0xFFFF                                     ; 段限长低 16 位
    dw 0                                          ; 基址低 16 位
    db 0                                          ; 基址中 8 位
    db PRESENT | NOT_SYS | RW                     ; 访问字节：存在、数据段、可写
    db GRAN_4K | SZ_32 | 0xF                      ; 标志 + 段限长高 4 位
    db 0                                          ; 基址高 8 位

.pointer:
    dw $ - gdt64 - 1                              ; GDT 大小 - 1
    dq gdt64                                      ; GDT 基址

; ==========================================================================
; 32 位引导代码
; GRUB 进入时的处理器状态（Multiboot2 规范 3.3）：
;   EAX = 0x36D76289（魔数），EBX = Multiboot2 信息结构物理地址
;   CS = 32 位代码段，DS/ES/FS/GS/SS = 32 位数据段
;   CR0.PG = 0（分页关闭），CR0.PE = 1（保护模式）
; ==========================================================================
section .bootstrap_text align=16
bits 32
global _start
extern kernel_main

_start:
    ; 使用页表区域之后的内存作为临时栈
    mov  esp, 0x74000
    push ebx                    ; 保存 Multiboot2 信息指针
    push eax                    ; 保存 Multiboot2 魔数

    ; -------------------------------------------------------
    ; 第 1 步：设置页表（身份映射前 128MB，使用 2MB 大页）
    ;
    ; 4 级页表结构（2MB 大页只需 3 张表）：
    ;   PML4[0]   → PDPTE
    ;   PDPTE[0]  → PD
    ;   PD[0..63] → 2MB 页面 × 64 = 128MB 身份映射
    ;
    ; 参考：OSDev "Setting Up Long Mode" - Setting up the Paging
    ; -------------------------------------------------------

    ; 清零 3 张页表（共 12KB = 3072 个双字）
    mov  edi, PML4_ADDR
    mov  ecx, (4096 * 3) / 4
    xor  eax, eax
    rep  stosd

    ; PML4[0] → PDPTE（存在 + 可读写）
    mov  dword [PML4_ADDR], PDPTE_ADDR + 0x03

    ; PDPTE[0] → PD（存在 + 可读写）
    mov  dword [PDPTE_ADDR], PD_ADDR + 0x03

    ; 填充 PD：64 个 2MB 大页 = 128MB 身份映射
    ; 每个 PD 条目格式：物理基址 | PS(1<<7) | RW(1<<1) | Present(1<<0)
    mov  edi, PD_ADDR
    mov  eax, 0x83                 ; 起始：地址=0, PS=1, RW=1, P=1
    mov  ecx, 64
.fill_pd:
    mov  [edi], eax
    add  eax, 0x200000             ; 下一页（2MB）
    add  edi, 8                    ; 下一项（每项 8 字节）
    dec  ecx
    jnz  .fill_pd

    ; -------------------------------------------------------
    ; 第 2 步：启用 PAE（CR4.PAE = 第 5 位）
    ; 长模式分页必须启用 PAE
    ; -------------------------------------------------------
    mov  eax, cr4
    or   eax, (1 << 5)
    mov  cr4, eax

    ; -------------------------------------------------------
    ; 第 3 步：将 PML4 物理地址加载到 CR3
    ; -------------------------------------------------------
    mov  eax, PML4_ADDR
    mov  cr3, eax

    ; -------------------------------------------------------
    ; 第 4 步：启用长模式（EFER.LME = 第 8 位）
    ; 参考：AMD64 APM Vol.2 15.5.1
    ; -------------------------------------------------------
    mov  ecx, 0xC0000080           ; IA32_EFER MSR
    rdmsr
    or   eax, (1 << 8)             ; 设置 LME 位
    wrmsr

    ; -------------------------------------------------------
    ; 第 5 步：启用分页（CR0.PG = 第 31 位）
    ; 此时 LME + PAE + PG 同时生效 → 进入长模式（兼容模式）
    ; -------------------------------------------------------
    mov  eax, cr0
    or   eax, (1 << 31)
    mov  cr0, eax

    ; -------------------------------------------------------
    ; 第 6 步：加载 64 位 GDT，远跳转到 64 位代码段
    ; -------------------------------------------------------
    lgdt [gdt64.pointer]

    ; 恢复 Multiboot2 参数
    pop  edi                       ; EDI = 魔数
    pop  esi                       ; ESI = 信息指针

    ; 远跳转到 64 位代码段 → 进入 64 位子模式
    jmp  gdt64.code_seg:long_mode_entry

; ==========================================================================
; 64 位入口点
; ==========================================================================
section .text
bits 64

long_mode_entry:
    ; 加载 64 位数据段寄存器
    mov  ax, gdt64.data_seg
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax

    ; 设置内核栈
    mov  rsp, stack_top

    ; 零扩展 EDI/ESI 到 RDI/RSI（System V AMD64 调用约定）
    mov  rdi, rdi
    mov  rsi, rsi

    ; 调用 C 语言内核入口
    call kernel_main

    ; 如果 kernel_main 返回，停机
.halt:
    cli
    hlt
    jmp  .halt

; ==========================================================================
; BSS 段：内核栈（16KB，16 字节对齐）
; ==========================================================================
section .bss align=16
stack_bottom:
    resb 16384                       ; 16 KB 栈空间
stack_top:
