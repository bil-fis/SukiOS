/*
 * kernel/arch/x86_64/ap_boot.S
 * -----------------------------------------------------------------------------
 * AP（应用处理器）启动跳板（P0-3 SMP）。
 *
 * 流程：BSP 把 [ap_tramp_start, ap_tramp_end) 整块拷贝到物理 0x8000，
 * 填好尾部邮箱（CR3/栈顶/入口/CPU索引），随后对目标 LAPIC 发
 * INIT-SIPI-SIPI（SIPI 向量 0x08 => CS:IP = 0800:0000 = 物理 0x8000）。
 *
 * AP 上电进入实模式 -> 加载跳板内 GDT -> 保护模式(32) -> 开 PAE、装 CR3
 * （内核 PML4：低区恒等映射 + 高半区，AP 可先在低地址跑再跳高半区）
 * -> EFER.LME|NXE -> 开分页进长模式(64) -> 从邮箱取栈与入口 -> jmp ap_main。
 *
 * 位置无关性：本代码链接在内核高半区 VMA，但运行在 0x8000。
 *   - 16/32 位段：全部用 (label - ap_tramp_start + 0x8000) 常量绝对寻址；
 *   - 64 位段：用 %rip 相对寻址（代码与邮箱同块拷贝，相对偏移不变）。
 */

.set AP_PHYS, 0x8000

/* 计算跳板内标号在 0x8000 处的物理地址 */
#define APADDR(sym) (sym - ap_tramp_start + AP_PHYS)

.section .text
.global ap_tramp_start
.global ap_tramp_end
.global ap_mb_cr3
.global ap_mb_stack
.global ap_mb_entry
.global ap_mb_idx

/* ========================================================================= */
/*  16 位实模式入口（SIPI 落地点）                                            */
/* ========================================================================= */
.code16
ap_tramp_start:
    cli
    cld
    xorw    %ax, %ax
    movw    %ax, %ds                       /* DS=0：后续绝对寻址基于物理地址 */

    lgdtl   APADDR(ap_gdt_ptr)             /* 加载跳板内临时 GDT */

    movl    %cr0, %eax
    orl     $1, %eax                       /* CR0.PE */
    movl    %eax, %cr0

    ljmpl   $0x18, $APADDR(ap_pm32)        /* 0x18 = 32 位代码段 */

/* ========================================================================= */
/*  32 位保护模式：开 PAE -> CR3 -> EFER.LME|NXE -> 开分页                    */
/* ========================================================================= */
.code32
ap_pm32:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss

    /* CR4: PAE(bit5) | OSFXSR(bit9)（与 BSP boot.S 一致） */
    movl    %cr4, %eax
    orl     $((1 << 5) | (1 << 9)), %eax
    movl    %eax, %cr4

    /* CR3 = 内核 PML4（邮箱低 4 字节足够：PML4 物理地址 < 4GB） */
    movl    APADDR(ap_mb_cr3), %eax
    movl    %eax, %cr3

    /* EFER: LME(bit8) | NXE(bit11)。NXE 必须与 BSP 一致——内核映射中
     * 用户页带 PTE_NX(bit63)，NXE=0 时该位是保留位，翻译即缺页。 */
    movl    $0xC0000080, %ecx
    rdmsr
    orl     $((1 << 8) | (1 << 11)), %eax
    wrmsr

    /* CR0: PG(bit31) | PE(bit0) | MP(bit1，fxsave 合法性与 BSP fpu_init 一致) */
    movl    %cr0, %eax
    orl     $0x80000003, %eax
    movl    %eax, %cr0

    ljmp    $0x08, $APADDR(ap_lm64)        /* 0x08 = 64 位代码段 */

/* ========================================================================= */
/*  64 位长模式：取邮箱栈/入口，跳入内核 C 代码（高半区）                     */
/* ========================================================================= */
.code64
ap_lm64:
    movq    ap_mb_stack(%rip), %rsp        /* 先切到本 AP 独立内核栈（64 位地址） */
    andq    $-16, %rsp
    subq    $8, %rsp                       /* 模拟 call 后的栈相位（SysV ABI） */
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    %ax, %fs
    movw    %ax, %gs                       /* GS_BASE 稍后由 percpu_install 设置 */
    xorq    %rbp, %rbp                     /* 终止栈回溯链 */

    movq    ap_mb_idx(%rip), %rdi          /* 参数：CPU 逻辑索引 */
    movq    ap_mb_entry(%rip), %rax        /* 入口：ap_main（高半区地址） */
    jmp     *%rax

/* ========================================================================= */
/*  跳板内临时 GDT（AP 进入高半区后由 gdt_init_ap 换成正式 per-CPU GDT）      */
/* ========================================================================= */
.align 16
ap_gdt:
    .quad 0x0000000000000000               /* 0x00 空 */
    .quad 0x00209A0000000000               /* 0x08 64 位代码段 (L=1) */
    .quad 0x00CF92000000FFFF               /* 0x10 数据段（平坦 4GB：32 位段
                                            *  会检查 limit，limit=0 的描述符
                                            *  在 ap_pm32 一读邮箱即 #GP 三重
                                            *  故障；64 位模式忽略 limit 不冲突） */
    .quad 0x00CF9A000000FFFF               /* 0x18 32 位代码段 (D=1, 4G) */
ap_gdt_ptr:
    .word ap_gdt_ptr - ap_gdt - 1
    .long APADDR(ap_gdt)                   /* 32 位基址（lgdtl m16&32） */

/* ========================================================================= */
/*  邮箱（BSP 在 SIPI 前填写；偏移由 C 侧用符号差计算）                       */
/* ========================================================================= */
.align 8
ap_mb_cr3:   .quad 0                       /* 内核 PML4 物理地址 */
ap_mb_stack: .quad 0                       /* AP 栈顶（内核虚拟地址） */
ap_mb_entry: .quad 0                       /* ap_main 地址 */
ap_mb_idx:   .quad 0                       /* CPU 逻辑索引 */
ap_tramp_end:

.section .note.GNU-stack, "", @progbits
