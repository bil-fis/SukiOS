/* =============================================================================
 * SukiOS - InitKernel（int 0x80 版本）
 *
 * 包含完整的 Ring 3 切换准备：
 *   1. 用户代码/栈映射到低半区并设置 U/S=1
 *   2. 整条页表链修复 U/S 位及清 XD/PS
 *   3. 全局 TLB 刷新
 *   4. 切换到干净内核栈后进入用户态
 * ============================================================================= */

#include "kernel/tty.h"
#include "kernel/vmm.h"
#include "kernel/heap.h"
#include "kernel/apic.h"
#include "kernel/idt.h"
#include "kernel/keyboard.h"
#include "kernel/apic_timer.h"
#include "kernel/io.h"

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
    tty_setcolor(VGA_GREEN, VGA_BLACK);
    tty_print(" ____          _     _   ___   ____  \n");
    tty_print("/ ___|  _   _ | | __(_) / _ \\ / ___| \n");
    tty_print("\\___ \\ | | | || |/ /| || | | |\\___ \\ \n");
    tty_print(" ___) || |_| ||   < | || |_| | ___) |\n");
    tty_print("|____/  \\__,_||_|\\_\\_| \\___/ |____/ \n\n");

    tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
    tty_print("SukiOS v0.1.0 - x86_64\n\n");

    vmm_init();
    tty_print("\n");

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

    apic_init();

    tty_print("\n[..] Initializing LAPIC Timer...\n");
    irq_register_handler(0, apic_timer_irq_handler);
    apic_timer_init();

    tty_print("\n[..] Initializing PS/2 Keyboard...\n");
    irq_register_handler(1, keyboard_irq_handler);
    keyboard_init();
    apic_timer_set_callback(keyboard_timer_tick);

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

    /* ===== 切换到 Ring 3 ===== */
    tty_setcolor(VGA_CYAN, VGA_BLACK);
    tty_print("[..] Switching to Ring 3 (User Mode)...\n");
    tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);

    uint64_t entry_phys = kern_virt_to_phys((uint64_t)&userland_entry);
    uint64_t entry_offset = entry_phys & (VMM_PAGE_SIZE - 1);
    uint64_t code_page_phys = entry_phys & ~(VMM_PAGE_SIZE - 1);
    uint64_t stack_phys = kern_virt_to_phys((uint64_t)&user_stack_bottom);
    uint64_t stack_page_phys = stack_phys & ~(VMM_PAGE_SIZE - 1);

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

    /* 页表链强制修复（U/S=1，清 XD/PS） */
    {
        uint64_t cr3;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
        volatile uint64_t *pml4 = (volatile uint64_t *)(uintptr_t)cr3;

        uint64_t pml4e = pml4[0];
        if (!(pml4e & VMM_PRESENT)) { tty_print("ERROR: PML4[0] !P\n"); goto fallback; }
        pml4e &= ~(VMM_XD | VMM_PS);
        pml4e |= VMM_USER;
        pml4[0] = pml4e;

        volatile uint64_t *pdpte = (volatile uint64_t *)(uintptr_t)(pml4e & 0x000FFFFFFFFFF000ULL);
        uint64_t pdpte_e = pdpte[4];
        if (!(pdpte_e & VMM_PRESENT)) { tty_print("ERROR: PDPT[4] !P\n"); goto fallback; }
        pdpte_e &= ~(VMM_XD | VMM_PS);
        pdpte_e |= VMM_USER;
        pdpte[4] = pdpte_e;

        volatile uint64_t *pd = (volatile uint64_t *)(uintptr_t)(pdpte_e & 0x000FFFFFFFFFF000ULL);
        uint64_t pde = pd[0];
        if (!(pde & VMM_PRESENT)) { tty_print("ERROR: PD[0] !P\n"); goto fallback; }
        pde &= ~(VMM_XD | VMM_PS);
        pde |= VMM_USER;
        pd[0] = pde;

        volatile uint64_t *pt = (volatile uint64_t *)(uintptr_t)(pde & 0x000FFFFFFFFFF000ULL);
        uint64_t pte = pt[0];
        pte &= ~VMM_XD;
        pt[0] = pte;
    }

    /* 全局 TLB 刷新 */
    {
        uint64_t cr3;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");
    }

    uint64_t user_rip = USER_CODE_VADDR + entry_offset;
    uint64_t user_rsp = USER_STACK_VADDR + VMM_PAGE_SIZE;

    /* 切换到内核中断栈 */
    __asm__ volatile (
        "mov %[kstack], %%rsp"
        :
        : [kstack] "r"((uint64_t)&kernel_interrupt_stack_top)
        : "memory"
    );

    enter_ring3(user_rip, user_rsp);
    __builtin_unreachable();

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