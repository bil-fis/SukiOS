; =============================================================================
; SukiOS - Multiboot2 x86_64 Higher-Half Bootstrap (4KB Pages)
; Reference: OSDev Wiki - Multiboot2, Setting Up Long Mode, GCC Cross-Compiler
;
; Memory layout:
;   Bootstrap:  VMA = LMA = KERNEL_PHYS (~0x100000), 32-bit accessible
;   Kernel:     VMA = KERNEL_VIRT (0xFFFFFFFF80000000), 64-bit only
;   Page tables: 7 tables at 0x70000-0x76FFF (28KB)
;
;   Identity map:   0x00000000 - 0x003FFFFF (4MB, 4KB pages)
;   Higher-half map: KERNEL_VIRT - KERNEL_VIRT+2MB → KERNEL_LMA - KERNEL_LMA+2MB
;
; Compile: nasm -f elf64 boot.asm -o boot.o
; =============================================================================

; ===== Multiboot2 Constants =====
MB2_MAGIC       equ 0xE85250D6
MB2_ARCH        equ 0                   ; i386 (GRUB compatible)
MB2_TAG_END     equ 0

; ===== Address Constants =====
KERNEL_PHYS     equ 0x100000            ; Physical load address (1MB)
KERNEL_VIRT     equ 0xFFFFFFFF80000000  ; Virtual base (higher-half)
BOOTSTRAP_MAX   equ 0x10000             ; 64KB reserved for bootstrap
KERNEL_LMA      equ KERNEL_PHYS + BOOTSTRAP_MAX  ; 0x110000

; ===== Page Table Addresses (7 tables, 0x70000-0x76FFF) =====
; Identity mapping chain: PML4[0] -> PDPTE_A -> PD_A -> PT_A
; Higher-half mapping chain: PML4[511] -> PDPTE_B -> PD_B -> PT_B
PML4_ADDR       equ 0x70000
PDPTE_A_ADDR    equ 0x71000
PD_A_ADDR       equ 0x72000
PT_A_ADDR       equ 0x73000
PDPTE_B_ADDR    equ 0x74000
PD_B_ADDR       equ 0x75000
PT_B_ADDR       equ 0x76000

; ===== GDT Flags =====
PRESENT         equ (1 << 7)
NOT_SYS         equ (1 << 4)
EXEC            equ (1 << 3)
RW              equ (1 << 1)
GRAN_4K         equ (1 << 7)
SZ_32           equ (1 << 6)
LONG_MODE       equ (1 << 5)

; =============================================================================
; Multiboot2 Header (must be within first 32KB, 8-byte aligned)
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
; 64-bit GDT (in bootstrap, physical address, accessible in both modes)
; Reference: AMD64 APM Vol.2 4.8.1-4.8.2, OSDev "Setting Up Long Mode"
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
    dq gdt64                          ; GDT base (physical address)

; =============================================================================
; Bootstrap Data: Virtual address pointers for kernel_main and stack_top
; These are at physical addresses (identity mapped), accessible from both modes.
; The linker fills in the 64-bit virtual addresses (R_X86_64_64 relocations).
; =============================================================================
section .bootstrap_data align=8

extern kernel_main

kernel_main_ptr:
    dq kernel_main                    ; Virtual address of kernel_main
stack_top_ptr:
    dq stack_top                      ; Virtual address of BSS stack top

; =============================================================================
; 32-bit Bootstrap Code
; GRUB entry state (Multiboot2 spec 3.3):
;   EAX = 0x36D76289 (magic), EBX = MBI physical address
;   CS/DS/ES/FS/GS/SS = 32-bit segments, CR0.PG=0, CR0.PE=1
; =============================================================================
section .bootstrap_text align=16
bits 32
global _start

_start:
    ; === 调试：写 '1' 到 VGA ===
    mov  word [0xB8000], 0x0F31

    ; Temporary stack (above page tables, within identity-mapped range)
    mov  esp, 0x78000
    push ebx                          ; Save MBI address
    push eax                          ; Save magic

    ; === 调试：写 '2' ===
    mov  word [0xB8002], 0x0F32

    ; ---------------------------------------------------------------
    ; Step 1: Initialize page tables (clear all 7 tables, 28KB)
    ; ---------------------------------------------------------------
    mov  edi, PML4_ADDR
    mov  ecx, 7 * 4096 / 4           ; 7168 dwords
    xor  eax, eax
    rep  stosd

    ; ---------------------------------------------------------------
    ; Step 2: Identity mapping - virtual 0-4MB -> physical 0-4MB
    ; Chain: PML4[0] -> PDPTE_A -> PD_A -> PT_A (1024 x 4KB pages)
    ; Reference: OSDev "Setting Up Long Mode" - Setting up the Paging
    ; ---------------------------------------------------------------
    mov  dword [PML4_ADDR],          PDPTE_A_ADDR | 0x03
    mov  dword [PDPTE_A_ADDR],       PD_A_ADDR | 0x03
    mov  dword [PD_A_ADDR],          PT_A_ADDR | 0x03

    ; === 调试：写 '3' ===
    mov  word [0xB8004], 0x0F33

    ; Fill PT_A: 1024 entries mapping 0x00000 - 0x3FFFFF
    mov  edi, PT_A_ADDR
    mov  eax, 0x03                    ; Physical 0x0, Present + RW
    mov  ecx, 1024
.fill_pt_a:
    mov  [edi], eax
    add  eax, 0x1000
    add  edi, 8
    dec  ecx
    jnz  .fill_pt_a

    ; ---------------------------------------------------------------
    ; Step 3: Higher-half mapping - KERNEL_VIRT -> KERNEL_LMA
    ; Chain: PML4[511] -> PDPTE_B[510] -> PD_B[0] -> PT_B (512 x 4KB = 2MB)
    ;
    ; 虚拟地址 0xFFFFFFFF80000000 的页表索引分解（经 Python 验证）：
    ;   PML4[511]  (bits 47:39) = 511
    ;   PDPTE[510] (bits 38:30) = 510    ← 关键！
    ;   PD[0]      (bits 29:21) = 0
    ;   PT[0]      (bits 20:12) = 0
    ;
    ; Virtual:   0xFFFFFFFF80000000 + i*4096
    ; Physical:  KERNEL_LMA + i*4096  (= 0x110000 + i*4096)
    ; ---------------------------------------------------------------
    mov  dword [PML4_ADDR + 511*8],      PDPTE_B_ADDR | 0x03
    mov  dword [PDPTE_B_ADDR + 510*8],   PD_B_ADDR | 0x03
    mov  dword [PD_B_ADDR],              PT_B_ADDR | 0x03

    ; === 调试：写 '4' ===
    mov  word [0xB8006], 0x0F34

    ; Fill PT_B: 512 entries mapping KERNEL_LMA to KERNEL_LMA+2MB
    mov  edi, PT_B_ADDR
    mov  eax, KERNEL_LMA | 0x03      ; Physical 0x110000, Present + RW
    mov  ecx, 512
.fill_pt_b:
    mov  [edi], eax
    add  eax, 0x1000
    add  edi, 8
    dec  ecx
    jnz  .fill_pt_b

    ; ---------------------------------------------------------------
    ; Step 4: Enable PAE (CR4.PAE = bit 5)
    ; Long mode requires PAE paging
    ; ---------------------------------------------------------------
    mov  eax, cr4
    or   eax, (1 << 5)
    mov  cr4, eax

    ; ---------------------------------------------------------------
    ; Step 5: Load PML4 into CR3
    ; ---------------------------------------------------------------
    mov  eax, PML4_ADDR
    mov  cr3, eax

    ; === 调试：写 '5' ===
    mov  word [0xB8008], 0x0F35

    ; ---------------------------------------------------------------
    ; Step 6: Enable Long Mode (EFER.LME = bit 8)
    ; Reference: AMD64 APM Vol.2 15.5.1
    ; ---------------------------------------------------------------
    mov  ecx, 0xC0000080             ; IA32_EFER MSR
    rdmsr
    or   eax, (1 << 8)
    wrmsr

    ; ---------------------------------------------------------------
    ; Step 7: Enable Paging (CR0.PG = bit 31)
    ; LME + PAE + PG -> Enter long mode (compatibility sub-mode)
    ; ---------------------------------------------------------------
    mov  eax, cr0
    or   eax, (1 << 31)
    mov  cr0, eax

    ; === 调试：写 '7' ===
    mov  word [0xB800C], 0x0F37

    ; ---------------------------------------------------------------
    ; Step 8: Load 64-bit GDT, restore Multiboot2 args, far jump
    ; ---------------------------------------------------------------
    lgdt [gdt64.pointer]

    ; === 调试：写 '8' ===
    mov  word [0xB800E], 0x0F38

    pop  edi                          ; EDI = magic (zero-extend to RDI later)
    pop  esi                          ; ESI = MBI address (zero-extend to RSI later)

    ; Far jump to 64-bit code segment -> enter 64-bit sub-mode
    ; long_mode_entry is in bootstrap (physical address, 32-bit reachable)
    jmp  gdt64.code_seg:long_mode_entry

; =============================================================================
; 64-bit Entry Point (still in bootstrap section, running at physical address)
; =============================================================================
bits 64

long_mode_entry:
    ; === 覆盖式调试：每次都写 0xB8000 ===
    mov  word [0xB8000], 0x0F39          ; '9' - 进入 64 位模式

    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax
    mov  word [0xB8000], 0x0F46          ; 'F' - 段寄存器加载完成

    ; 加载 kernel_main 虚拟地址
    mov  rax, [kernel_main_ptr]
    mov  word [0xB8000], 0x0F4A          ; 'J' - kernel_main 地址已加载

    ; 直接测试高半区映射：从 kernel_main 虚拟地址读取第一个字节
    mov  bl, byte [rax]
    mov  word [0xB8000], 0x0F4D          ; 'M' - 高半区代码页可读

    ; 加载栈并准备参数
    mov  rsp, [stack_top_ptr]
    mov  word [0xB8000], 0x0F48          ; 'H' - 栈已加载

    mov  edi, edi
    mov  esi, esi

    mov  word [0xB8000], 0x0F4B          ; 'K' - 即将调用 kernel_main

    call rax

    ; If kernel_main returns, halt
.halt:
    cli
    hlt
    jmp  .halt

; =============================================================================
; BSS: Kernel Stack (at virtual address, in higher-half)
; =============================================================================
section .bss align=16
stack_bottom:
    resb 16384                        ; 16 KB stack
stack_top:
