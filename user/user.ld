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

    /DISCARD/ : {
        *(.comment .note* .eh_frame*)
    }
}
