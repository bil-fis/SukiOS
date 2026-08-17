/*
 * boot/multiboot2_header.S
 * -----------------------------------------------------------------------------
 * SukiOS Multiboot2 头部。GRUB 会在内核 ELF 文件前 32KB 内搜索该魔数以识别内核。
 *
 * 手册第 3.1 节要求：请求 1024x768x32 帧缓冲。
 * 说明：EFI64 / ADDRESS 标签在阶段一暂不加入（ADDRESS 标签与标准 ELF 加载
 *       方式互斥；EFI64 会在后续 UEFI 适配阶段补充），当前 BIOS 启动无需它们。
 *
 * 必须 8 字节对齐，位于文件最前部（由 linker.ld 的 KEEP(*(.multiboot)) 保证）。
 */

.set MB2_MAGIC,       0xE85250D6      /* Multiboot2 魔数 */
.set MB2_ARCH,        0              /* 0 = i386 (protected mode) */
.set MB2_HDR_LEN,     (mb2_header_end - mb2_header_start)
.set MB2_CHECKSUM,    -(MB2_MAGIC + MB2_ARCH + MB2_HDR_LEN)

.section .multiboot, "a", @progbits
.align 8
mb2_header_start:
    .long MB2_MAGIC
    .long MB2_ARCH
    .long MB2_HDR_LEN
    .long MB2_CHECKSUM

    /* --- Framebuffer 请求标签 (type = 5) --- */
    .align 8
fb_tag_start:
    .word 5                 /* type: framebuffer */
    .word 0                 /* flags: 0 = 必须满足 */
    .long fb_tag_end - fb_tag_start
    .long 1024              /* 请求宽度 */
    .long 768               /* 请求高度 */
    .long 32                /* 请求色深 (bpp) */
fb_tag_end:

    /* --- 结束标签 (type = 0) --- */
    .align 8
    .word 0                 /* type: end */
    .word 0                 /* flags */
    .long 8                 /* size */
mb2_header_end:

.section .note.GNU-stack, "", @progbits
