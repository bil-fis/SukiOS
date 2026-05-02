/* =============================================================================
 * SukiOS - InitKernel (最终修复版)
 *
 * 修复项：
 *   1. 用户代码/栈映射到低半区虚拟地址。
 *   2. 完整页表链（PML4→PDPTE→PD→PT）强制设置 U/S=1，清除 XD/PS。
 *   3. 全局 TLB 刷新，确保硬件加载正确权限。
 *   4. 切换前使用安全内核栈，避免栈污染。
 * ============================================================================= */

#include "kernel/tty.h"
#include "kernel/vmm.h"
#include "kernel/heap.h"
#include "kernel/apic.h"
#include "kernel/idt.h"
#include "kernel/keyboard.h"
#include "kernel/apic_timer.h"

#define KERNEL_VIRT_BASE    0xFFFFFFFF80000000ULL
#define KERNEL_LMA          0x110000ULL

#define USER_CODE_VADDR     0x100000000ULL
#define USER_STACK_VADDR    0x100001000ULL

extern uint64_t userland_entry;
extern uint8_t  user_stack_bottom;
extern uint8_t  user_stack_top;
extern void     enter_ring3(uint64_t rip, uint64_t rsp);
extern uint8_t  kernel_interrupt_stack_top;

static inline uint64_t kern_virt_to_phys(uint64_t vaddr)
{
    return vaddr - KERNEL_VIRT_BASE + KERNEL_LMA;
}

void init_kernel(void)
{
    /* ========== 启动画面 ========== */
    tty_setcolor(VGA_GREEN, VGA_BLACK);
    tty_print(" ____          _     _   ___   ____  \n");
    tty_print("/ ___|  _   _ | | __(_) / _ \\ / ___| \n");
    tty_print("\\___ \\ | | | || |/ /| || | | |\\___ \\ \n");
    tty_print(" ___) || |_| ||   < | || |_| | ___) |\n");
    tty_print("|____/  \\__,_||_|\\_\\_| \\___/ |____/ \n\n");

    tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
    tty_print("SukiOS v0.1.0 - x86_64\n\n");

    /* ========== 1. 虚拟内存管理器 ========== */
    vmm_init();
    tty_print("\n");

    /* ========== 2. 内核堆 ========== */
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

    /* ========== 3. APIC 中断控制器 ========== */
    apic_init();

    /* ========== 4. LAPIC 定时器 + 键盘 ========== */
    tty_print("\n[..] Initializing LAPIC Timer...\n");
    irq_register_handler(0, apic_timer_irq_handler);
    apic_timer_init();

    tty_print("\n[..] Initializing PS/2 Keyboard...\n");
    irq_register_handler(1, keyboard_irq_handler);
    keyboard_init();
    apic_timer_set_callback(keyboard_timer_tick);

    /* ========== 5. 内核堆分配测试 ========== */
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
    tty_print("\nKeyboard ready.\n");

    /* ========== 6. 切换到 Ring 3 ========== */
    tty_setcolor(VGA_CYAN, VGA_BLACK);
    tty_print("[..] Switching to Ring 3 (User Mode)...\n");
    tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);

    /* ----- 6.1 计算物理地址与偏移 ----- */
    uint64_t entry_phys = kern_virt_to_phys((uint64_t)&userland_entry);
    uint64_t entry_offset = entry_phys & (VMM_PAGE_SIZE - 1);
    uint64_t code_page_phys = entry_phys & ~(VMM_PAGE_SIZE - 1);
    uint64_t stack_phys = kern_virt_to_phys((uint64_t)&user_stack_bottom);
    uint64_t stack_page_phys = stack_phys & ~(VMM_PAGE_SIZE - 1);

    /* ----- 6.2 映射用户代码页和栈页 ----- */
    if (vmm_map_page(code_page_phys, USER_CODE_VADDR,
                     VMM_PRESENT | VMM_USER) != 0) {
        tty_setcolor(VGA_RED, VGA_BLACK);
        tty_print("ERROR: failed to map user code page\n");
        tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
        goto fallback;
    }
    if (vmm_map_page(stack_page_phys, USER_STACK_VADDR,
                     VMM_PRESENT | VMM_USER | VMM_WRITE) != 0) {
        tty_setcolor(VGA_RED, VGA_BLACK);
        tty_print("ERROR: failed to map user stack page\n");
        tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
        goto fallback;
    }

    /* ----- 6.3 页表链强制修复（U/S=1, 清 XD/PS） ----- */
    tty_print("  Page table chain fix (U/S=1, clr XD/PS):\n");
    {
        uint64_t cr3;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
        volatile uint64_t *pml4 = (volatile uint64_t *)(uintptr_t)cr3;

        // PML4[0] (Index for 0x00000000..)
        uint64_t pml4e = pml4[0];
        tty_print("  PML4[0] raw = 0x"); tty_print_hex64(pml4e);
        if (!(pml4e & VMM_PRESENT)) { tty_print("  ERROR: !P\n"); goto fallback; }
        pml4e &= ~(VMM_XD | VMM_PS);        // 清除执行禁止和大页标志
        pml4e |= VMM_USER;                  // 设置用户可访问
        pml4[0] = pml4e;
        tty_print(" -> fixed = 0x"); tty_print_hex64(pml4[0]); tty_print("\n");

        volatile uint64_t *pdpte = (volatile uint64_t *)(uintptr_t)(pml4e & 0x000FFFFFFFFFF000ULL);

        // PDPTE[4] (Index for 0x100000000..)
        uint64_t pdpte_e = pdpte[4];
        tty_print("  PDPTE[4] raw = 0x"); tty_print_hex64(pdpte_e);
        if (!(pdpte_e & VMM_PRESENT)) { tty_print("  ERROR: !P\n"); goto fallback; }
        pdpte_e &= ~(VMM_XD | VMM_PS);
        pdpte_e |= VMM_USER;
        pdpte[4] = pdpte_e;
        tty_print(" -> fixed = 0x"); tty_print_hex64(pdpte[4]); tty_print("\n");

        volatile uint64_t *pd = (volatile uint64_t *)(uintptr_t)(pdpte_e & 0x000FFFFFFFFFF000ULL);

        // PD[0] (Index for 0x100000000.. within the PDPT)
        uint64_t pde = pd[0];
        tty_print("  PD[0] raw = 0x"); tty_print_hex64(pde);
        if (!(pde & VMM_PRESENT)) { tty_print("  ERROR: !P\n"); goto fallback; }
        pde &= ~(VMM_XD | VMM_PS);
        pde |= VMM_USER;
        pd[0] = pde;
        tty_print(" -> fixed = 0x"); tty_print_hex64(pd[0]); tty_print("\n");

        volatile uint64_t *pt = (volatile uint64_t *)(uintptr_t)(pde & 0x000FFFFFFFFFF000ULL);

        // PT[0] (User code page, already mapped with U/S=1, just ensure XD=0)
        uint64_t pte = pt[0];
        tty_print("  PT[0] raw = 0x"); tty_print_hex64(pte);
        pte &= ~VMM_XD;
        pt[0] = pte;
        tty_print(" -> final = 0x"); tty_print_hex64(pt[0]); tty_print("\n");
    }

    /* ----- 6.4 全局 TLB 刷新 ----- */
    {
        uint64_t cr3;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");
    }

    /* ----- 6.5 计算用户态 RIP 和 RSP ----- */
    uint64_t user_rip = USER_CODE_VADDR + entry_offset;
    uint64_t user_rsp = USER_STACK_VADDR + VMM_PAGE_SIZE;

    tty_print("  user_entry phys=0x"); tty_print_hex64(entry_phys);
    tty_print("  offset=0x"); tty_print_hex64(entry_offset);
    tty_print("  RIP=0x"); tty_print_hex64(user_rip);
    tty_print("\n  user_stack phys=0x"); tty_print_hex64(stack_phys);
    tty_print("  RSP=0x"); tty_print_hex64(user_rsp);
    tty_print("\n");

    /* ----- 6.6 切换到内核中断栈（干净栈，避免污染 iretq 帧） ----- */
    __asm__ volatile (
        "mov %[kstack], %%rsp"
        :
        : [kstack] "r"((uint64_t)&kernel_interrupt_stack_top)
        : "memory"
    );

    /* ----- 6.7 进入 Ring 3 ----- */
    enter_ring3(user_rip, user_rsp);
    __builtin_unreachable();

    /* ----- 后备模式：Ring 3 切换失败时 ----- */
fallback:
    tty_setcolor(VGA_RED, VGA_BLACK);
    tty_print("ERROR: Ring 3 switch failed! Falling back to kernel mode.\n");
    tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);

    tty_print("\nKernel fallback mode. Type something:\n> ");
    for (;;) {
        char c = keyboard_getchar_nb();
        if (c) {
            tty_putchar(c);
        }
        __asm__ volatile ("sti; hlt");
    }
}