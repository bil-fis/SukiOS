/* =============================================================================
 * SukiOS - InitKernel
 *
 * InitKernel 是内核初始化的第二阶段。
 * 在 EarlyKernel 完成所有底层初始化后调用。
 *
 * 初始化顺序：
 *   1. 虚拟内存管理器（VMM）- 建立正式页表，高半区映射
 *   2. 内核堆（Heap）- 基于 VMM 的动态内存分配
 *   3. APIC 中断控制器 - 使用 VMM 映射 MMIO
 * ============================================================================= */

#include "kernel/tty.h"
#include "kernel/vmm.h"
#include "kernel/heap.h"
#include "kernel/apic.h"

void init_kernel(void)
{
    tty_setcolor(VGA_GREEN, VGA_BLACK);
    tty_print("  _____ _____ __  __  __  __ \n");
    tty_print(" |  __ \\_   _|  \\/  | |  \\/  |\n");
    tty_print(" | |__) || | | .  . | | .  . |\n");
    tty_print(" |  ___/ | | | |\\/| | | |\\/| |\n");
    tty_print(" | |    _| |_| |  | | | |  | |\n");
    tty_print(" |_|   |_____|_|  |_|_|_|  |_|\n");
    tty_print("\n");

    tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
    tty_print("SukiOS v0.1.0 - x86_64\n\n");

    /* ---- 1. 初始化虚拟内存管理器 ---- */
    vmm_init();
    tty_print("\n");

    /* ---- 2. 初始化内核堆 ---- */
    tty_print("[..] Initializing kernel heap...\n");
    heap_init(VMM_HEAP_BASE, VMM_HEAP_SIZE);
    {
        size_t heap_total, heap_used, heap_free;
        heap_get_stats(&heap_total, &heap_used, &heap_free);
        tty_print("[OK] Kernel heap at ");
        tty_print_hex64(VMM_HEAP_BASE);
        tty_print(" (");
        tty_print_dec((uint32_t)(heap_total / 1024));
        tty_print(" KB mapped)\n");
    }
    tty_print("\n");

    /* ---- 3. 初始化 APIC 中断控制器（使用 VMM 映射 MMIO） ---- */
    apic_init();

    /* ---- 4. 验证堆分配 ---- */
    tty_print("\n[..] Testing kernel heap...\n");
    {
        void *p1 = kmalloc(128);
        void *p2 = kmalloc(256);
        void *p3 = kmalloc(64);

        if (p1 && p2 && p3) {
            tty_print("[OK] kmalloc: p1=");
            tty_print_hex64((uint64_t)(uintptr_t)p1);
            tty_print(" p2=");
            tty_print_hex64((uint64_t)(uintptr_t)p2);
            tty_print(" p3=");
            tty_print_hex64((uint64_t)(uintptr_t)p3);
            tty_print("\n");

            kfree(p1);
            kfree(p3);

            void *p4 = kmalloc(32);
            tty_print("[OK] kfree + kmalloc: p4=");
            tty_print_hex64((uint64_t)(uintptr_t)p4);
            tty_print("\n");

            size_t heap_total, heap_used, heap_free;
            heap_get_stats(&heap_total, &heap_used, &heap_free);
            tty_print("[OK] Heap stats: ");
            tty_print_dec((uint32_t)(heap_used / 1024));
            tty_print(" KB used, ");
            tty_print_dec((uint32_t)(heap_free / 1024));
            tty_print(" KB free\n");
        } else {
            tty_setcolor(VGA_RED, VGA_BLACK);
            tty_print("  ERROR: kmalloc test failed!\n");
            tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
        }
    }

    tty_setcolor(VGA_GREEN, VGA_BLACK);
    tty_print("\nSukiOS booted successfully!\n");
    tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
}
