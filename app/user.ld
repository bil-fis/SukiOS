/* user.ld - SukiOS 用户程序链接脚本 */

OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    .text : {
        *(.text .text.*)
    }

    . = ALIGN(0x1000);
    .rodata : {
        *(.rodata .rodata.*)
    }

    . = ALIGN(0x1000);
    .data : {
        *(.data .data.*)
    }

    . = ALIGN(0x1000);
    .bss : {
        *(.bss .bss.*)
    }

    /* 丢弃不需要的段，避免地址冲突 */
    /DISCARD/ : {
        *(.note.*)
        *(.comment*)
        *(.eh_frame*)
    }
}