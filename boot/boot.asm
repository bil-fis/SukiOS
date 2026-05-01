; =============================================================================
; SukiOS - Multiboot2 x86_64 Higher-Half Bootstrap (4KB Pages)
; Reference: OSDev Wiki - Multiboot2, Setting Up Long Mode
;
; Memory layout:
;   Bootstrap:  VMA = LMA = KERNEL_PHYS (0x100000), 32-bit accessible
;   Kernel:     VMA = KERNEL_VIRT (0xFFFFFFFF80000000), 64-bit only
;   Page tables: 7 tables at 0x70000-0x76FFF (28KB)
;
;   Identity map:   0x00000000 - 0x003FFFFF (4MB)
;   Higher-half map: KERNEL_VIRT+ -> KERNEL_LMA+  (2MB)
; =============================================================================

; ===== Multiboot2 常量 =====
MB2_MAGIC       equ 0xE85250D6
MB2_ARCH        equ 0                   ; i386（兼容 x86_64）
MB2_TAG_END     equ 0

; ===== 地址常量 =====
KERNEL_PHYS     equ 0x100000            ; 物理加载地址 (1MB)
KERNEL_VIRT     equ 0xFFFFFFFF80000000  ; 虚拟基址（高半区）
BOOTSTRAP_MAX   equ 0x10000             ; Bootstrap 预留空间 (64KB)
KERNEL_LMA      equ KERNEL_PHYS + BOOTSTRAP_MAX  ; 0x110000

; ===== 页表地址（7 张表，0x70000-0x76FFF） =====
; Identity 链: PML4[0] -> PDPTE_A -> PD_A -> PT_A
; Higher-half 链: PML4[511] -> PDPTE_B[510] -> PD_B[0] -> PT_B
PML4_ADDR       equ 0x70000
PDPTE_A_ADDR    equ 0x71000
PD_A_ADDR       equ 0x72000
PT_A_ADDR       equ 0x73000
PDPTE_B_ADDR    equ 0x74000
PD_B_ADDR       equ 0x75000
PT_B_ADDR       equ 0x76000

; ===== GDT 标志 =====
PRESENT         equ (1 << 7)
NOT_SYS         equ (1 << 4)
EXEC            equ (1 << 3)
RW              equ (1 << 1)
GRAN_4K         equ (1 << 7)
SZ_32           equ (1 << 6)
LONG_MODE       equ (1 << 5)

; =============================================================================
; Multiboot2 Header（必须在 32KB 以内，8 字节对齐）
; =============================================================================
section .multiboot align=8

multiboot_header:
    dd  MB2_MAGIC
    dd  MB2_ARCH
    dd  multiboot_header_end - multiboot_header
    dd  -(MB2_MAGIC + MB2_ARCH + (multiboot_header_end - multiboot_header))

end_tag:
    dw  MB2_TAG_END
    dw  0
    dd  end_tag_end - end_tag
end_tag_end:

multiboot_header_end:

; =============================================================================
; Bootstrap GDT（物理地址，32/64 位模式下均可访问）
; 仅用于进入 64 位长模式，后续由 EarlyKernel/gdt.c 设置正式 GDT。
; =============================================================================
section .bootstrap_rodata align=16

gdt64:
.null: equ $ - gdt64
    dq 0

.code_seg: equ $ - gdt64              ; Selector 0x08
    dw 0xFFFF                         ; Limit low 16
    dw 0                              ; Base low 16
    db 0                              ; Base mid 8
    db PRESENT | NOT_SYS | EXEC | RW  ; Access: code, readable
    db GRAN_4K | LONG_MODE | 0xF      ; Flags + limit high 4
    db 0                              ; Base high 8

.data_seg: equ $ - gdt64              ; Selector 0x10
    dw 0xFFFF                         ; Limit low 16
    dw 0                              ; Base low 16
    db 0                              ; Base mid 8
    db PRESENT | NOT_SYS | RW         ; Access: data, writable
    db GRAN_4K | SZ_32 | 0xF          ; Flags + limit high 4
    db 0                              ; Base high 8

.pointer:
    dw $ - gdt64 - 1                  ; GDT size - 1
    dq gdt64                          ; GDT base（物理地址）

; =============================================================================
; Bootstrap 数据：kernel_main 和 stack_top 的虚拟地址
; 在身份映射区域，32/64 位均可访问。
; =============================================================================
section .bootstrap_data align=8

extern kernel_main

kernel_main_ptr:
    dq kernel_main                    ; kernel_main 虚拟地址
stack_top_ptr:
    dq stack_top                      ; BSS 栈顶虚拟地址

; =============================================================================
; 32 位 Bootstrap 代码
; GRUB 入口状态（Multiboot2 spec 3.3）：
;   EAX = 0x36D76289 (magic), EBX = MBI 物理地址
;   CS/DS/ES/FS/GS/SS = 32 位段, CR0.PG=0, CR0.PE=1
; =============================================================================
section .bootstrap_text align=16
bits 32
global _start

_start:
    ; 临时栈（页表上方，身份映射范围内）
    mov  esp, 0x78000
    push ebx                          ; 保存 MBI 地址
    push eax                          ; 保存 magic

    ; ---------------------------------------------------------------
    ; 步骤 1: 初始化页表（清零 7 张表，28KB）
    ; ---------------------------------------------------------------
    mov  edi, PML4_ADDR
    mov  ecx, 7 * 4096 / 4           ; 7168 dwords
    xor  eax, eax
    rep  stosd

    ; ---------------------------------------------------------------
    ; 步骤 2: Identity mapping - virtual 0-4MB -> physical 0-4MB
    ; 链: PML4[0] -> PDPTE_A -> PD_A -> PT_A (1024 x 4KB pages)
    ; ---------------------------------------------------------------
    mov  dword [PML4_ADDR],          PDPTE_A_ADDR | 0x03
    mov  dword [PDPTE_A_ADDR],       PD_A_ADDR | 0x03
    mov  dword [PD_A_ADDR],          PT_A_ADDR | 0x03

    ; 填充 PT_A: 映射 0x00000 - 0x3FFFFF
    mov  edi, PT_A_ADDR
    mov  eax, 0x03                    ; 物理地址 0x0, Present + RW
    mov  ecx, 1024
.fill_pt_a:
    mov  [edi], eax
    add  eax, 0x1000
    add  edi, 8
    dec  ecx
    jnz  .fill_pt_a

    ; ---------------------------------------------------------------
    ; 步骤 3: Higher-half mapping - KERNEL_VIRT -> KERNEL_LMA
    ; 链: PML4[511] -> PDPTE_B[510] -> PD_B[0] -> PT_B (512 x 4KB = 2MB)
    ;
    ; 虚拟地址 0xFFFFFFFF80000000 页表索引分解（Python 验证）：
    ;   PML4[511]  (bits 47:39) = 511
    ;   PDPTE[510] (bits 38:30) = 510
    ;   PD[0]      (bits 29:21) = 0
    ;   PT[0]      (bits 20:12) = 0
    ; ---------------------------------------------------------------
    mov  dword [PML4_ADDR + 511*8],      PDPTE_B_ADDR | 0x03
    mov  dword [PDPTE_B_ADDR + 510*8],   PD_B_ADDR | 0x03
    mov  dword [PD_B_ADDR],              PT_B_ADDR | 0x03

    ; 填充 PT_B: 映射 KERNEL_LMA 到 KERNEL_LMA+2MB
    mov  edi, PT_B_ADDR
    mov  eax, KERNEL_LMA | 0x03
    mov  ecx, 512
.fill_pt_b:
    mov  [edi], eax
    add  eax, 0x1000
    add  edi, 8
    dec  ecx
    jnz  .fill_pt_b

    ; ---------------------------------------------------------------
    ; 步骤 4: 启用 PAE（CR4.PAE = bit 5）
    ; ---------------------------------------------------------------
    mov  eax, cr4
    or   eax, (1 << 5)
    mov  cr4, eax

    ; ---------------------------------------------------------------
    ; 步骤 5: 加载 PML4 到 CR3
    ; ---------------------------------------------------------------
    mov  eax, PML4_ADDR
    mov  cr3, eax

    ; ---------------------------------------------------------------
    ; 步骤 6: 启用 Long Mode（EFER.LME = bit 8）
    ; ---------------------------------------------------------------
    mov  ecx, 0xC0000080             ; IA32_EFER MSR
    rdmsr
    or   eax, (1 << 8)
    wrmsr

    ; ---------------------------------------------------------------
    ; 步骤 7: 启用分页（CR0.PG = bit 31）
    ; LME + PAE + PG -> 进入长模式（兼容子模式）
    ; ---------------------------------------------------------------
    mov  eax, cr0
    or   eax, (1 << 31)
    mov  cr0, eax

    ; ---------------------------------------------------------------
    ; 步骤 8: 加载 64 位 GDT，恢复 Multiboot2 参数，far jump
    ; ---------------------------------------------------------------
    lgdt [gdt64.pointer]

    pop  edi                          ; EDI = magic（零扩展到 RDI）
    pop  esi                          ; ESI = MBI 地址（零扩展到 RSI）

    ; Far jump 到 64 位代码段 -> 进入 64 位子模式
    jmp  gdt64.code_seg:long_mode_entry

; =============================================================================
; 64 位入口（仍在 bootstrap 段，运行在物理地址）
; =============================================================================
bits 64

long_mode_entry:
    ; 加载 64 位数据段寄存器
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax

    ; 零扩展 Multiboot2 参数到 64 位（System V AMD64 ABI）
    mov  edi, edi                    ; RDI = magic
    mov  esi, esi                    ; RSI = MBI 地址

    ; 加载内核栈（虚拟地址，来自 bootstrap_data）
    mov  rsp, [stack_top_ptr]

    ; 间接调用 kernel_main（通过虚拟地址）
    mov  rax, [kernel_main_ptr]
    call rax

    ; 如果 kernel_main 返回，停机
.halt:
    cli
    hlt
    jmp  .halt

; =============================================================================
; BSS: 内核栈（虚拟地址，在高半区）
; =============================================================================
section .bss align=16
stack_bottom:
    resb 16384                        ; 16 KB 栈
stack_top:
