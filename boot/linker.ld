/* =============================================================================
 * SukiOS Linker Script - x86_64 Higher-Half Kernel
 *
 * Memory layout:
 *   Bootstrap: VMA = LMA = 0x100000 (32-bit accessible, max 64KB)
 *   Kernel:    VMA = 0xFFFFFFFF80000000, LMA = 0x110000
 *
 * Page table mapping (set up in boot.asm):
 *   Identity:    virtual 0x00000-0x3FFFFF -> physical 0x00000-0x3FFFFF
 *   Higher-half: virtual 0xFFFFFFFF80000000+ -> physical 0x110000+
 *
 * Must link with: -z max-page-size=0x1000
 * ============================================================================= */

ENTRY(_start)

KERNEL_PHYS    = 0x100000;
KERNEL_VIRT    = 0xFFFFFFFF80000000;
BOOTSTRAP_MAX  = 0x10000;    /* 64KB reserved for bootstrap sections */

SECTIONS
{
    /* ---- Bootstrap sections: VMA = LMA = physical address ---- */
    . = KERNEL_PHYS;

    .multiboot ALIGN(4096) :
    {
        *(.multiboot)
    }

    .bootstrap ALIGN(4096) :
    {
        *(.bootstrap_text)
        *(.bootstrap_rodata)
        *(.bootstrap_data)
    }

    /* 确保 bootstrap 不超过预留空间 */
    ASSERT(. <= KERNEL_PHYS + BOOTSTRAP_MAX,
           "Bootstrap code exceeds 64KB reserved space!")

    /* 填充到固定的 bootstrap 大小（页表映射依赖此地址） */
    . = KERNEL_PHYS + BOOTSTRAP_MAX;

    /* ---- 内核段：VMA = 高半区，LMA = bootstrap 之后的物理地址 ---- */

    /* 记录内核 LMA 的起始位置 */
    . = KERNEL_VIRT;

    /*
     * 使用固定公式计算 LMA：
     *   LMA = KERNEL_PHYS + BOOTSTRAP_MAX + (VMA - KERNEL_VIRT)
     *       = 0x110000 + 内核偏移
     * 这与 boot.asm 中的页表映射一致
     */
    .text ALIGN(4096) : AT(KERNEL_PHYS + BOOTSTRAP_MAX + ADDR(.text) - KERNEL_VIRT)
    {
        *(.text)
        *(.text.*)
    }

    .rodata ALIGN(4096) : AT(KERNEL_PHYS + BOOTSTRAP_MAX + ADDR(.rodata) - KERNEL_VIRT)
    {
        *(.rodata)
        *(.rodata.*)
    }

    .data ALIGN(4096) : AT(KERNEL_PHYS + BOOTSTRAP_MAX + ADDR(.data) - KERNEL_VIRT)
    {
        *(.data)
        *(.data.*)
    }

    .bss ALIGN(4096) : AT(KERNEL_PHYS + BOOTSTRAP_MAX + ADDR(.bss) - KERNEL_VIRT)
    {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }

    /* Discard unnecessary sections */
    /DISCARD/ :
    {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
    }
}
