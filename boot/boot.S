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

/* KASLR：高半区基址随机滑动的窗口与粒度（必须与 gen_relk.py / vmm.c 一致）。
 * slide_v = 槽位 * 1GB，槽位∈[1, KASLR_SLOTS]。KASLR_SLOTS 须 ≤ 508，使
 * PDP 新基址窗口(slide_idx..slide_idx+3) 不越过 PDP[256] 的 512 项上界。 */
.set KERNEL_BASE,  0xFFFF800000000000
.set KASLR_SLOTS,  256

/* ========================================================================= */
/*  32 位引导代码（位于 .boot 段，链接在低物理地址，恒等可达）              */
/* ========================================================================= */
.section .boot, "ax", @progbits
.code32
.global _start
.type _start, @function
.global g_smap_enabled           /* 由 C 端 syscall.c 定义；探测到 SMAP 后置 1 */
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

    /* --- 置 EFER.LME (Long Mode Enable) + 按需 EFER.NXE (P0-8) ---
     * NXE(bit 11) 使 PTE bit63(NX) 生效：用户栈/堆页不可执行才真正落地。
     * 必须先经 CPUID.80000001H:EDX[20] 探测 NX 支持——不支持的 CPU 上
     * 置 NXE 会 #GP。掩码存 esi（cpuid 会毁 eax/ebx/ecx/edx，不碰 esi）。 */
    movl $EFER_LME, %esi           /* esi = 待 OR 进 EFER 的位集合 */
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax         /* 最大扩展 leaf < 80000001H 则跳过 */
    jb   .Lno_nx
    movl $0x80000001, %eax
    cpuid
    testl $(1 << 20), %edx         /* NX 支持位 */
    jz   .Lno_nx
    orl  $(1 << 11), %esi          /* EFER.NXE */
.Lno_nx:
    movl $EFER_MSR, %ecx
    rdmsr
    orl  %esi, %eax
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

    /* L3 修复（SMAP 启用）：必须放在 64 位段——g_smap_enabled 位于高半区
     * .bss（地址 >4GB），32 位段无法用 32 位位移寻址。此处用 movabsq 装载
     * 其完整 64 位地址后再写。仅当 CPU 支持 SMAP(CPUID.07 EBX.bit19) 才置
     * CR4.SMAP 并令 g_smap_enabled=1，使 syscall.c 的 copy_*_user 在该硬件
     * 上执行 STAC/CLAC 围栏（不支持的 CPU 不置位，避免 STAC/CLAC 触发 #UD）。 */
    movl $7, %eax
    xorl %ecx, %ecx
    cpuid
    testl $(1 << 19), %ebx         /* SMAP 支持位 */
    jz   .Lno_smap
    movq %cr4, %rax
    orl  $(1 << 21), %eax          /* CR4.SMAP */
    movq %rax, %cr4
    movabsq $g_smap_enabled, %rax
    movb $1, (%rax)
.Lno_smap:

    /* ============================================================ */
    /*  P0-8 / P0-R2 内核代码段 KASLR：随机滑动高半区基址            */
    /* ============================================================ */
    /* 设计：slide_v = 随机槽位 * 1GB（槽位∈[1, KASLR_SLOTS]）。
     * 执行顺序（顺序错误会三重故障，见各步注释）：
     *  A) 计算 slide_v（%r12，跨 cpuid 保存）；
     *  B) 应用重定位：遍历 g_relk_offsets（此刻旧基址 p3_high[0..3] 仍
     *     映射，可经旧 VA 读写高半区），对每个【存储值】≥KERNEL_BASE 的
     *     8 字节绝对地址 += slide_v；
     *  C) 重写 PDP(p3_high)：清旧基址项(0..3)，置新基址项
     *     (slide_idx..slide_idx+3) = p2_table 的 4 个 PD 页（带 P|W 标志）。
     *     p2_table 映射 phys 0..4GB，1GB 对齐滑动保证内核 2MB 窗口仍由
     *     p2_table[0] 大页覆盖，无需 4KB 拆分；
     *  D) 写 g_kernel_slide / g_virt_base —— 必须在 C 之后：这两条 movabsq
     *     的立即数已在 B 中被修补为【新 VA】，只有新 PDP 项建立后其页表
     *     行走才能命中（新 VA 从未进 TLB，硬件必然重新行走页表）；
     *  E) 重载 CR3（清掉旧基址的陈旧 TLB 项）；
     *  F) 跳 higher_half_entry（其 movabsq 立即数已在 B 中修补为新 VA，
     *     绝不可再加 slide_v，否则变成 旧+2*slide -> 三重故障）。 */

    /* ---- 步骤 A：计算随机滑动量 ---- */
    call   kaslr_compute_slide
    movq   %rax, %r12              /* %r12 = slide_v（1GB 对齐，非零） */

    /* ---- 步骤 B：应用重定位（旧基址仍映射）---- */
    movabsq $g_relk_offsets, %r13  /* 本条自身也是 R_X86_64_64，但它在循环
                                    * 开始【之前】执行，取到的是旧 VA——正确，
                                    * 因为此刻只有旧基址映射可用 */
    movabsq $g_relk_count, %r14
    movabsq $KERNEL_BASE, %r15     /* 比较阈值（>32 位立即数不可直接 cmp） */
    movq   (%r14), %r14            /* count */
1:
    testq  %r14, %r14
    jz     2f
    movq   (%r13), %rbx            /* %rbx = 待修补位置 r_offset（可能位于
                                    * .boot 低地址或 .text 高半区旧 VA） */
    movq   (%rbx), %rax            /* %rax = 该位置【当前存储值】（链接期
                                    * 绝对地址）；对 movabsq $sym 即符号 VA */
    cmpq   %r15, %rax              /* 按“值”过滤：仅高半区绝对地址才滑动。
                                    * 物理地址/小常量/绝对符号(__kernel_phys_end
                                    * 等) < KERNEL_BASE 被跳过。
                                    * 注意：过滤“值”而非 r_offset —— .boot 段
                                    * 内 movabsq 的 r_offset 是低地址，但其存
                                    * 储值（高半区符号 VA）必须被滑动。 */
    jb     3f
    addq   %r12, %rax              /* 新值 = 旧值 + slide_v */
    movq   %rax, (%rbx)            /* 写回原位置（物理同体，切表后新 VA 可见） */
3:
    addq   $8, %r13
    decq   %r14
    jmp    1b
2:

    /* ---- 步骤 C：重写 PDP —— 清旧基址项，建新基址项 ----
     * 先清后建：slide_idx∈[1,3] 时新旧窗口重叠，后写的新项自然覆盖。
     * 每个新项必须：指向各自的 PD 页(p2_table + k*4096) 且带 P|W 标志
     * （裸物理地址无 PRESENT 位 => 页表行走即缺页 => 三重故障）。 */
    movabsq $p3_high, %r8
    xorq   %rcx, %rcx
4:  movq   $0, (%r8, %rcx, 8)      /* 清旧基址项 0..3 */
    incq   %rcx
    cmpq   $4, %rcx
    jne    4b
    movq   %r12, %rax
    shrq   $30, %rax               /* slide_idx = slide_v >> 30 ∈ [1,256] */
    movq   %rax, %rcx              /* %rcx = 目标 PDP 项索引 */
    movabsq $p2_table, %r9
    orq    $(PAGE_PRESENT | PAGE_WRITE), %r9   /* %r9 = PD 页物理地址|P|W */
    xorq   %rdx, %rdx              /* %rdx = k（第几个 1GB 窗口，0..3） */
5:  movq   %r9, (%r8, %rcx, 8)     /* p3_high[slide_idx+k] = p2_table+k*4K|P|W */
    addq   $4096, %r9              /* 下一个 PD 页 */
    incq   %rcx
    incq   %rdx
    cmpq   $4, %rdx                /* 4 个 1GB 窗口覆盖 phys 0..4GB */
    jne    5b

    /* ---- 步骤 D：写 g_kernel_slide / g_virt_base（经新 VA，映射已建立）---- */
    movabsq $g_kernel_slide, %rax  /* 立即数已被 B 修补为新 VA */
    movq   %r12, (%rax)
    movabsq $g_virt_base, %rax     /* 同上；g_virt_base = KERNEL_BASE+slide */
    movq   %r15, %rbx
    addq   %r12, %rbx
    movq   %rbx, (%rax)

    /* ---- 步骤 E：重载 CR3（冲刷旧基址的陈旧 TLB 项）---- */
    movabsq $p4_table, %rax
    movq   %rax, %cr3

    /* ---- 步骤 F：跳高半区入口（movabsq 立即数已是新 VA，直接跳）---- */
    movabsq $higher_half_entry, %rax
    jmp    *%rax

/* kaslr_compute_slide：返回随机 slide_v（%rax），1GB 对齐、槽位∈[1,SLOTS]。
 * 优先 RDRAND（CPUID.7 EBX[18] 探测），否则回退 TSC 折叠。
 * 调用约定：无参数，返回 %rax；不破坏除 %rax 外的调用者寄存器。 */
kaslr_compute_slide:
    /* 探测 RDRAND（CPUID.07 EBX bit18）*/
    movl   $7, %eax
    xorl   %ecx, %ecx
    cpuid
    testl  $(1 << 18), %ebx
    jz     .kaslr_tsc              /* 不支持 RDRAND -> TSC */
    rdrand %rax
    jc     .kaslr_slot
.kaslr_tsc:
    rdtsc                           /* edx:eax = TSC */
    shlq   $32, %rdx
    orq    %rdx, %rax
.kaslr_slot:
    /* 槽位 = (rax % KASLR_SLOTS)，再 +1 保证非零（非零才有随机化意义）。
     * divq 被除数在 rdx:rax（128 位）；此处随机值仅 64 位，故 rdx 清 0。 */
    movl   $KASLR_SLOTS, %ecx
    xorq   %rdx, %rdx              /* 高 64 位 = 0 */
    divq   %rcx                    /* rax = 商, rdx = 余数(槽位) */
    movq   %rdx, %rax
    incq   %rax                    /* 槽位∈[1, KASLR_SLOTS] */
    movq   $0x40000000, %rcx       /* 1GB */
    mulq   %rcx                    /* rax = 槽位 * 1GB = slide_v */
    ret

/* ========================================================================= */
/*  早期调试：COM1 直写单字符（KASLR 排错用，稳定后删除）                      */
/*  COM1 基址 0x3F8，LSR 在 0x3FD(bit5=THR 空)。本例程自身位于 .boot(低地址、   */
/*  恒等映射，重载 CR3 前后均可达)。调用前字符置于 %al；仅破坏 %rax/%rdx。    */
/* ========================================================================= */
.section .boot, "ax", @progbits
.code64

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
/* P0-R2 KPTI 硬性约束：p4_table 必须 8KB 对齐（物理地址 bit12=0）。
 * KPTI 用 CR3 bit12 区分「内核视图(0)/影子视图(1)」；osdev CPU_Registers_
 * x86-64 载明 CR3 bit12..63 属 PML4 物理基址，bit12=1 是完全合法的页对齐
 * 地址——若 p4_table 恰好落在奇数 4KB 页（如 0x101000），中断入口会把它
 * 误判为影子视图并清 bit12，CR3 变成指向内核映像代码字节的垃圾"页表"，
 * 下一次取指立即 #PF(RSVD) -> 递归 #PF -> #DF -> 三重故障（实测于 AP 首个
 * 中断）。8KB 对齐从根上保证内核 PML4 bit12=0。 */
.align 8192
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
