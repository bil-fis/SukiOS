/* =============================================================================
 * SukiOS 链接脚本 - x86_64 身份映射内核
 *
 * 所有段从物理地址 1MB 开始加载，使用身份映射（虚拟地址 = 物理地址）。
 * 高半区（higher-half）映射可后续添加。
 *
 * 段布局：
 *   .multiboot         — Multiboot2 头部（必须最先，8 字节对齐）
 *   .bootstrap         — 32 位引导代码 + GDT（分页启用前运行）
 *   .text              — 64 位内核代码
 *   .rodata            — 只读数据（字符串常量等）
 *   .data              — 已初始化数据
 *   .bss               — 未初始化数据（含内核栈）
 *
 * 注意：链接时必须使用 -z max-page-size=0x1000！
 *   GNU LD 对 elf64 的默认页大小为 0x200000（2MB），会导致 Multiboot2 头部
 *   无法位于文件前 32KB 内。参考：OSDev Multiboot Wiki。
 * ============================================================================= */

ENTRY(_start)

KERNEL_BASE = 0x100000;   /* 内核加载地址（1MB，GRUB 典型加载位置） */

SECTIONS
{
    . = KERNEL_BASE;

    /* 引导段：Multiboot2 头 + 32 位引导代码 + GDT */
    .multiboot ALIGN(4096) :
    {
        *(.multiboot)
    }

    .bootstrap ALIGN(4096) :
    {
        *(.bootstrap_text)
        *(.bootstrap_rodata)
    }

    /* 主内核段（64 位代码和数据） */
    .text ALIGN(4096) :
    {
        *(.text)
        *(.text.*)
    }

    .rodata ALIGN(4096) :
    {
        *(.rodata)
        *(.rodata.*)
    }

    .data ALIGN(4096) :
    {
        *(.data)
        *(.data.*)
    }

    .bss ALIGN(4096) :
    {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }

    /* 丢弃不需要的段 */
    /DISCARD/ :
    {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
    }
}
