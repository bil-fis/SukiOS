/*
 * boot/linker.ld
 * -----------------------------------------------------------------------------
 * SukiOS 高半区内核链接脚本。
 *   物理加载地址 (LMA) = 0x100000 (1MB)
 *   虚拟运行地址 (VMA) = 0xFFFF800000000000 + 物理偏移 (KERNEL_BASE)
 *
 * .boot / .boot.bss 段例外：VMA == LMA（低地址），因为 32 位引导代码在分页
 * 开启前必须以物理地址运行，且页表须位于恒等可达区域。
 *
 * 手册 8.3：预留救援堆栈见 boot.S 的 boot_stack。
 */

ENTRY(_start)

KERNEL_VMA = 0xFFFF800000000000;
LOAD_ADDR  = 0x100000;

SECTIONS
{
    . = LOAD_ADDR;
    __phys_start = .;

    /* ---- 低地址引导段（恒等映射，VMA == LMA） ---- */
    .boot ALIGN(8) :
    {
        KEEP(*(.multiboot))
        *(.boot)
    }
    .boot.bss ALIGN(4096) :
    {
        *(.boot.bss)
    }

    /* ---- Xen PVH ELF Note（独立 SHT_NOTE 段，供 QEMU -kernel 识别）----
     * QEMU 的 PVH 加载器遍历 SHT_NOTE 段寻找 name="Xen" type=18 的 note，
     * 故本段必须保持独立（不可并入 .boot/.text 等 PROGBITS 段），否则
     * QEMU 无法识别 PVH 入口。该段不分配（不进入任何 PT_LOAD）。 */
    .note.pvh ALIGN(4) :
    {
        KEEP(*(.note.pvh))
    }

    /* ---- 切换到高半区：VMA 加上 KERNEL_BASE，LMA 由 AT() 保持连续 ---- */
    . += KERNEL_VMA;

    .text ALIGN(4096) : AT(ADDR(.text) - KERNEL_VMA)
    {
        __text_start = .;
        *(.text .text.*)
        __text_end = .;
    }

    .rodata ALIGN(4096) : AT(ADDR(.rodata) - KERNEL_VMA)
    {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    }

    .data ALIGN(4096) : AT(ADDR(.data) - KERNEL_VMA)
    {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    }

    .bss ALIGN(4096) : AT(ADDR(.bss) - KERNEL_VMA)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }

    . = ALIGN(4096);
    __kernel_end = .;                 /* 高半区虚拟结束地址 */
    __kernel_phys_end = . - KERNEL_VMA;

    /* KASLR 重定位表（build/relk.c 生成）：放 .bss 之后，避免改变 .text/
     * .rodata/.data/.bss 的虚拟地址，从而保证 gen_relk.py 由预链接 ELF
     * 抽取的重定位偏移与最终链接一致。本段位于高半区，boot 期经旧基址映射
     * 读取，重载 CR3 后旧基址失效，但运行期不再访问，无需映射。 */
    .relk ALIGN(8) : AT(ADDR(.relk) - KERNEL_VMA)
    {
        __relk_start = .;
        *(.relk)
        __relk_end = .;
    }

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.eh_frame_hdr)
        /* 注意：(.note.pvh) 已在 .boot 段 KEEP，不会被此处丢弃 */
        *(.note.gnu*)
        *(.comment)
    }
}
