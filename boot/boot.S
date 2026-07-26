/*
 * boot/boot.S
 * -----------------------------------------------------------------------------
 * SukiOS 32 位引导入口 -> 64 位长模式 -> 高半区跳转。
 *
 * GRUB (Multiboot2) 移交控制时的状态：
 *   - 32 位保护模式，分页关闭，中断关闭，A20 已开启
 *   - EAX = Multiboot2 魔数 0x36D76289
 *   - EBX = Multiboot2 info 结构的物理地址
 *
 * 流程（手册第 3.2 节）：
 *   保存 magic/mbi -> 构建临时页表(恒等映射 + 高半区映射) -> 开启 PAE
 *   -> 置 EFER.LME -> 开启分页 -> 加载 64 位 GDT -> 远跳进入长模式
 *   -> 跳转至高半区 higher_half_entry -> 调用 C 语言 kmain。
 *
 * 内存布局红线：KERNEL_BASE = 0xFFFF800000000000
 *   PML4 索引(高半区) = 0xFFFF800000000000 >> 39 & 0x1FF = 256
 *   恒等映射占用 PML4[0]，高半区占用 PML4[256]，二者互不冲突。
 */

.set PAGE_PRESENT, 0x1
.set PAGE_WRITE,   0x2
.set PAGE_HUGE,    0x80          /* 2MB 大页 (PD 项的 PS 位) */
.set EFER_MSR,     0xC0000080
.set EFER_LME,     (1 << 8)

/* ========================================================================= */
/*  32 位引导代码（位于 .boot 段，链接在低物理地址，恒等可达）              */
/* ========================================================================= */
.section .boot, "ax", @progbits
.code32
.global _start
.type _start, @function
_start:
    cli
    cld

    /* 立即把 GRUB 传入的 magic / mbi 存到内存，防止后续被寄存器操作覆盖 */
    movl %eax, save_magic
    movl %ebx, save_mbi

    /* 临时 32 位引导栈 */
    movl $boot_stack_top, %esp

    /* --- 清零 p4 / p3_low / p3_high (共 3 页)，确保未用项为 0 --- */
    movl $p4_table, %edi
    xorl %eax, %eax
    movl $(4096 * 3 / 4), %ecx
    rep stosl

    /* --- 填充 p2_table：映射 0..4GB，使用 2048 个 2MB 大页 ---
     * 覆盖 4GB 以便访问位于 ~0xFD000000 的线性帧缓冲 (LFB)。
     * 后续 VMM 就绪后会以精确映射替换这套临时大页表。 */
    movl $p2_table, %edi
    xorl %ecx, %ecx
1:
    movl %ecx, %eax
    shll $21, %eax                          /* eax = index * 2MB (最大 2047*2MB<4GB) */
    orl  $(PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE), %eax
    movl %eax, (%edi)                       /* 项低 32 位 */
    movl $0, 4(%edi)                        /* 项高 32 位（物理地址 < 4GB） */
    addl $8, %edi
    incl %ecx
    cmpl $2048, %ecx
    jne  1b

    /* --- 链接页表项 --- */
    /* PML4[0] -> p3_low  (恒等映射) */
    movl $p3_low, %eax
    orl  $(PAGE_PRESENT | PAGE_WRITE), %eax
    movl %eax, p4_table + 0
    /* PML4[256] -> p3_high  (高半区 KERNEL_BASE) */
    movl $p3_high, %eax
    orl  $(PAGE_PRESENT | PAGE_WRITE), %eax
    movl %eax, p4_table + 256 * 8

    /* p3_low[0..3] / p3_high[0..3] -> p2_table 的 4 个连续 PD 页
     * 恒等映射与高半区映射复用同一组 PD，实现两地址空间指向同一物理内存。 */
    xorl %ecx, %ecx                        /* PD 序号 0..3 */
2:
    movl %ecx, %eax
    shll $12, %eax                         /* eax = k * 4096 */
    addl $p2_table, %eax
    orl  $(PAGE_PRESENT | PAGE_WRITE), %eax
    movl %ecx, %edx
    shll $3, %edx                          /* edx = k * 8 (项偏移) */
    movl %eax, p3_low(%edx)
    movl %eax, p3_high(%edx)
    incl %ecx
    cmpl $4, %ecx
    jne  2b

    /* --- 加载 CR3 --- */
    movl $p4_table, %eax
    movl %eax, %cr3

    /* --- 开启 CR4: PAE(bit5) | OSFXSR(bit9)，SMEP/SMAP 需 CPUID 检测 ---
     * PAE    : 物理地址扩展（长模式必需）
     * OSFXSR : 允许 fxsave/fxrstor（FPU/SSE 状态保存，见 switch.S）
     * 注意：向 CR4 写入 CPU 不支持的位会立即 #GP（三重故障），
     * 因此 SMEP(bit20)/SMAP(bit21) 必须先用 CPUID.07H:EBX 探测。 */
    movl %cr4, %eax
    orl  $((1 << 5) | (1 << 9)), %eax
    movl %eax, %cr4

    /* --- 探测并按需启用 SMEP(EBX bit7) / SMAP(EBX bit20)（C1 项） --- */
    movl $0, %eax
    cpuid
    cmpl $7, %eax                  /* 最大 leaf < 7 则跳过 */
    jb   .Lno_smep
    movl $7, %eax
    xorl %ecx, %ecx
    cpuid
    testl $(1 << 7), %ebx          /* SMEP 支持位 */
    jz   .Lno_smep
    movl %cr4, %eax
    orl  $(1 << 20), %eax          /* CR4.SMEP */
    movl %eax, %cr4
.Lno_smep:

    /* --- 置 EFER.LME (Long Mode Enable) --- */
    movl $EFER_MSR, %ecx
    rdmsr
    orl  $EFER_LME, %eax
    wrmsr

    /* --- 开启 CR0.PG (bit 31) 与 CR0.PE (bit 0) --- */
    movl %cr0, %eax
    orl  $((1 << 31) | (1 << 0)), %eax
    movl %eax, %cr0

    /* --- 加载 64 位 GDT，远跳重载 CS 进入长模式 --- */
    lgdt gdt64_pointer
    ljmp $0x08, $long_mode_start

/* ========================================================================= */
/*  64 位过渡代码（仍在低地址恒等映射区执行）                                */
/* ========================================================================= */
.code64
long_mode_start:
    /* 重载数据段选择子为内核数据段 0x10 */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    /* 从内存取回 magic/mbi，放入 System V ABI 首两个参数寄存器
     * (movl 到 32 位寄存器会自动零扩展到 64 位) */
    movl save_magic, %edi          /* rdi = magic */
    movl save_mbi,   %esi          /* rsi = mbi 物理地址 */

    /* 绝对跳转到高半区入口 */
    movabs $higher_half_entry, %rax
    jmp    *%rax

/* ========================================================================= */
/*  高半区 64 位入口（位于 .text 段，虚拟地址在 KERNEL_BASE 之上）           */
/* ========================================================================= */
.section .text, "ax", @progbits
.code64
.global higher_half_entry
.type higher_half_entry, @function
higher_half_entry:
    /* 切换到高半区内核栈 */
    movabs $kernel_stack_top, %rsp
    xorq   %rbp, %rbp                /* 终止栈回溯链 */

    /* rdi/rsi 仍保存着 magic/mbi，直接调用 kmain(magic, mbi_phys) */
    call kmain

.hang:
    cli
    hlt
    jmp .hang

/* ========================================================================= */
/*  64 位 GDT（临时，仅用于进入长模式；正式 GDT 由 gdt.c 安装）             */
/* ========================================================================= */
.section .boot, "ax", @progbits
.align 16
gdt64:
    .quad 0x0000000000000000       /* 0x00: 空描述符 */
    .quad 0x00209A0000000000       /* 0x08: 内核代码段 (L=1, present, exec, read) */
    .quad 0x0000920000000000       /* 0x10: 内核数据段 (present, write) */
gdt64_pointer:
    .word gdt64_pointer - gdt64 - 1
    .quad gdt64

/* ========================================================================= */
/*  引导期使用的低地址数据：页表、引导栈、保存的 magic/mbi                    */
/*  放在 .boot.bss（PROGBITS 零填充），随内核加载到低物理地址且被清零。       */
/* ========================================================================= */
.section .boot.bss, "aw", @progbits
.align 4096
p4_table:   .skip 4096
p3_low:     .skip 4096
p3_high:    .skip 4096
p2_table:   .skip 4096 * 4        /* 4 个 PD，映射 0..4GB */

.align 16
boot_stack_bottom:
    .skip 4096
boot_stack_top:

save_magic: .skip 4
save_mbi:   .skip 4

/* ========================================================================= */
/*  高半区内核主栈（.bss，NOBITS，由引导加载器清零）                         */
/* ========================================================================= */
.section .bss, "aw", @nobits
.align 16
.global kernel_stack_bottom
kernel_stack_bottom:
    .skip 16384                     /* 16KB 内核栈 (手册 2.3) */
.global kernel_stack_top
kernel_stack_top:

/* 声明不需要可执行栈，消除链接器警告 */
.section .note.GNU-stack, "", @progbits
