/* =============================================================================
 * SukiOS - EarlyKernel 入口
 *
 * EarlyKernel 负责：
 *   1. Multiboot2 引导信息解析
 *   2. 栈指针初始化（由 boot.asm 完成）
 *   3. GDT 初始化（含 TSS）
 *   4. IDT 初始化（CPU 异常处理）
 *   5. PIC 初始化
 *   6. 内存探测和物理内存管理器初始化
 *   7. 调用 init_kernel()
 *
 * 参考：OSDev Bare Bones, Creating a 64-bit kernel
 * ============================================================================= */

#include <stdint.h>
#include "multiboot2.h"
#include "kernel/tty.h"
#include "kernel/gdt.h"
#include "kernel/idt.h"
#include "kernel/pic.h"
#include "kernel/pmm.h"
#include "kernel/kernel.h"

/* ---- Multiboot2 标签解析 ---- */

static void parse_multiboot_tags(uint64_t mbi_addr)
{
    struct multiboot2_tag *tag;

    for (tag = (struct multiboot2_tag *)(mbi_addr + 8);
         tag->type != MULTIBOOT2_TAG_END;
         tag = (struct multiboot2_tag *)((uint8_t *)tag + ((tag->size + 7) & ~7)))
    {
        switch (tag->type) {
        case MULTIBOOT2_TAG_CMDLINE:
            tty_print("  [Cmdline] ");
            tty_print(((struct multiboot2_tag_string *)tag)->string);
            tty_putchar('\n');
            break;

        case MULTIBOOT2_TAG_BOOT_LOADER_NAME:
            tty_print("  [Bootloader] ");
            tty_print(((struct multiboot2_tag_string *)tag)->string);
            tty_putchar('\n');
            break;

        case MULTIBOOT2_TAG_BASIC_MEMINFO: {
            struct multiboot2_tag_basic_meminfo *mem =
                (struct multiboot2_tag_basic_meminfo *)tag;
            tty_print("  [Memory] lower: ");
            tty_print_dec(mem->mem_lower);
            tty_print(" KB, upper: ");
            tty_print_dec(mem->mem_upper);
            tty_print(" KB\n");
            break;
        }

        case MULTIBOOT2_TAG_FRAMEBUFFER: {
            struct multiboot2_tag_framebuffer *fb =
                (struct multiboot2_tag_framebuffer *)tag;
            tty_print("  [Framebuffer] ");
            tty_print_dec(fb->framebuffer_width);
            tty_print("x");
            tty_print_dec(fb->framebuffer_height);
            tty_print("x");
            tty_print_dec(fb->framebuffer_bpp);
            tty_print(" @ ");
            tty_print_hex64(fb->framebuffer_addr);
            tty_putchar('\n');
            break;
        }
        }
    }
}

/* ---- 物理内存管理器初始化 ---- */

static void init_pmm(uint64_t mbi_addr)
{
    struct multiboot2_tag *tag;

    for (tag = (struct multiboot2_tag *)(mbi_addr + 8);
         tag->type != MULTIBOOT2_TAG_END;
         tag = (struct multiboot2_tag *)((uint8_t *)tag + ((tag->size + 7) & ~7)))
    {
        if (tag->type == MULTIBOOT2_TAG_MMAP) {
            struct multiboot2_tag_mmap *mmap = (struct multiboot2_tag_mmap *)tag;
            uint32_t count = (mmap->size - sizeof(*mmap)) / mmap->entry_size;
            pmm_init(mmap->entries, count);
            return;
        }
    }

    tty_setcolor(VGA_RED, VGA_BLACK);
    tty_print("  WARNING: No memory map tag found!\n");
}

/* ---- 内核入口（由 boot.asm 调用） ---- */

void kernel_main(uint32_t magic, uint64_t mbi_addr)
{
    /* 1. 初始化 VGA 终端 */
    tty_init();

    tty_setcolor(VGA_GREEN, VGA_BLACK);
    tty_print("SukiOS EarlyKernel\n");
    tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);

    /* 2. 验证 Multiboot2 魔数 */
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        tty_setcolor(VGA_RED, VGA_BLACK);
        tty_print("ERROR: Invalid Multiboot2 magic!\n");
        goto halt;
    }
    tty_print("[OK] Multiboot2 verified\n");

    /* 3. 解析 Multiboot2 引导信息 */
    tty_print("[..] Parsing boot info...\n");
    parse_multiboot_tags(mbi_addr);
    tty_print("[OK] Boot info parsed\n");

    /* 4. 初始化 GDT（含 TSS） */
    tty_print("[..] Initializing GDT...\n");
    gdt_init();
    tty_print("[OK] GDT initialized\n");

    /* 5. 初始化 PIC */
    tty_print("[..] Remapping PIC...\n");
    pic_init();
    tty_print("[OK] PIC remapped\n");

    /* 6. 初始化 IDT（CPU 异常处理） */
    tty_print("[..] Initializing IDT...\n");
    idt_init();
    tty_print("[OK] IDT initialized\n");

    /* 7. 初始化物理内存管理器 */
    tty_print("[..] Initializing PMM...\n");
    init_pmm(mbi_addr);
    tty_print("[OK] PMM initialized\n");

    /* 8. 打印内存统计 */
    tty_print("\nMemory: ");
    tty_print_dec((uint32_t)(pmm_get_free_pages() * PAGE_SIZE / 1024));
    tty_print(" KB free, ");
    tty_print_dec((uint32_t)pmm_get_used_pages());
    tty_print(" pages used\n");

    /* 9. 进入 InitKernel */
    tty_print("\nEntering InitKernel...\n\n");
    init_kernel();

halt:
    for (;;) __asm__ volatile ("cli; hlt");
}
