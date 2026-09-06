/*
 * user/user_dyn.ld
 * 位置无关（PIE / 共享库）链接脚本，基址 0（ET_DYN，由内核分配随机基址）。
 * 用于：
 *   - 共享库 libtest.sl（gcc -shared -fPIC -T user_dyn.ld）
 * 与 user/user.ld 的区别仅在于首选加载基址（0x400000 → 0），其余段布局
 * （含 .dynamic / .dynsym / .dynstr / .rela.dyn / .rela.plt / .got / .got.plt）
 * 完全一致，确保内核 elf.c 能正确解析动态表与重定位表。
 */
OUTPUT_FORMAT("elf64-x86-64");
OUTPUT_ARCH(i386:x86-64);

ENTRY(_start);

SECTIONS {
    . = 0;

    .text : {
        *(.text.startup .text .text.*)
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
    .bss  : {
        *(COMMON)
        *(.bss .bss.*)
    }

    /* ---- 动态链接相关段（由 gcc -shared 生成，内核 elf.c 解析）---- */
    . = ALIGN(0x1000);
    .dynamic : { *(.dynamic) }
    .dynsym  : { *(.dynsym) }
    .dynstr  : { *(.dynstr) }
    .rela.dyn  : { *(.rela.dyn) }
    .rela.plt  : { *(.rela.plt) }

    . = ALIGN(0x1000);
    .got        : { *(.got) }
    .got.plt    : { *(.got.plt) }
}
