/*
 * boot/pvh.S
 * ---------------------------------------------------------------------------
 * Xen PVH ELF Note。当 GRUB 不可用时，QEMU 的 `-kernel` 加载器对 64 位 ELF
 * 仅支持两条路径：Linux bzImage 或 Xen PVH。本内核两者皆非，故通过此 note
 * 声明自己为 PVH 内核，从而让 `qemu -kernel build/kernel.elf` 直接进入。
 *
 * QEMU PVH 加载器（hw/i386/pvh.c）扫描 ELF 的 SHT_NOTE 段，寻找：
 *     name = "Xen\0"   (4 字节)
 *     type = XEN_ELFNOTE_PHYS32_ENTRY (18)
 *     desc = 32 位物理入口地址
 * 加载完成后 CPU 状态：32 位保护模式、分页关闭、ring0；
 *     EBX = hvm_start_info 物理地址
 *     EAX = 未定义（QEMU 一般置 0，不会是 multiboot2 magic 0x36D76289）
 * boot.S 据此区分 GRUB(multiboot2) 与 PVH 两条引导路径。
 *
 * 注意：本 note 的 desc 必须是 *物理* 入口。_start 位于 .boot 段
 * （VMA==LMA==0x100000），故其符号值即物理 0x100000。
 */

.section .note.pvh, "a", @note
.align 4
.long 1f - 0f                /* namesz = "Xen\0" 长度 = 4 */
.long 3f - 2f                /* descsz = 4（单个 32 位入口） */
.long 18                     /* type = XEN_ELFNOTE_PHYS32_ENTRY */
0:
.asciz "Xen"                 /* name（含结尾 NUL，共 4 字节） */
1:
.align 4
2:
.long _start                 /* desc：32 位物理入口地址 (0x100000) */
3:

/* 补 .note.GNU-stack 声明为非可执行栈（安全红线：禁止可执行栈）。 */
.section .note.GNU-stack, "", @progbits
