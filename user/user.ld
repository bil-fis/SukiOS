/*
 * user/user.ld
 * -----------------------------------------------------------------------------
 * Ring3 用户程序链接脚本：平坦二进制，装载于用户空间 0x400000。
 * .text.start (_start) 必须位于最前端 —— 内核直接跳转到装载基址执行。
 */
ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    .text : {
        *(.text.start)
        *(.text .text.*)
    }

    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)
    }

    /* 代码/数据 W^X 边界：链接脚本把 .text/.rodata 放在 [0, 1MiB)，
     * .data/.bss 放在 [1MiB, ...)。内核据此分别映射为 RX 与 RW+NX（C1 项）。 */
    . = ALIGN(0x100000);
    _etext = .;

    .data : ALIGN(16) {
        *(.data .data.*)
    }

    .bss : ALIGN(16) {
        *(.bss .bss.*)
        *(COMMON)
    }

    /* 动态链接程序（dltest 等）所需的动态段/全局偏移表。它们随 DT_NEEDED
     * 由内核 elf_link_dynamic 在加载期重定位填充，必须落在 RW 数据区（W^X：
     * 内核按段 PF_W 映射为 RW+NX）；.dynamic 通常只读，.got 必须可写。 */
    .dynamic : ALIGN(16) {
        *(.dynamic)
    }

    .got : ALIGN(16) {
        *(.got .got.plt)
    }

    .data.rel.ro : ALIGN(16) {
        *(.data.rel.ro .data.rel.ro.*)
    }

    /DISCARD/ : {
        *(.comment .note* .eh_frame*)
    }
}
