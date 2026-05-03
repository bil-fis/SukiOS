/* =============================================================================
 * SukiOS - InitKernel（集成进程调度器 + ELF 加载器）
 *
 * 完成内核高级初始化后，创建用户进程并启动抢占式调度。
 * 所有手动 Ring 3 切换已被完全替换。
 * ============================================================================= */

#include "kernel/tty.h"
#include "kernel/vmm.h"
#include "kernel/heap.h"
#include "kernel/apic.h"
#include "kernel/idt.h"
#include "kernel/keyboard.h"
#include "kernel/apic_timer.h"
#include "kernel/io.h"
#include "kernel/pci.h"
#include "kernel/ide.h"
#include "kernel/fat32.h"
#include "kernel/vfs.h"
#include "kernel/fat32_vfs.h"
#include "kernel/rtc.h"
#include "kernel/proc.h"         /* 新增：进程控制块与调度器 */

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

    /* ---- PCI 初始化 ---- */
    tty_print("\n[..] Initializing PCI...\n");
    pci_init();

    /* ---- IDE 驱动初始化 ---- */
    tty_print("\n[..] Initializing IDE...\n");
    ide_init();

    /* ---- 测试读取扇区 0 ---- */
    static uint8_t sector_buf[IDE_SECTOR_SIZE];
    tty_print("[..] Reading sector 0...\n");
    if (ide_read_sector(0, sector_buf)) {
        tty_print("[OK] Sector 0:\n");
        for (int i = 0; i < 64; i++) {
            tty_print_hex64((uint64_t)sector_buf[i]);
            tty_print(" ");
            if ((i + 1) % 16 == 0) tty_print("\n");
        }
        tty_print("\n");
    } else {
        tty_setcolor(VGA_RED, VGA_BLACK);
        tty_print("  IDE read failed!\n");
        tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
    }

    rtc_init();

    rtc_time_t now;
    rtc_read_time(&now);
    tty_print("[RTC] Current time: ");
    tty_print_dec(now.year);
    tty_print("-");
    tty_print_dec(now.month);
    tty_print("-");
    tty_print_dec(now.day);
    tty_print(" ");
    tty_print_dec(now.hour);
    tty_print(":");
    tty_print_dec(now.minute);
    tty_print(":");
    tty_print_dec(now.second);
    tty_print("\n");

    tty_print("[RTC] FAT time=0x");
    tty_print_hex64(rtc_to_fat_time(&now));
    tty_print(" FAT date=0x");
    tty_print_hex64(rtc_to_fat_date(&now));
    tty_print("\n");

    /* ---- 挂载 FAT32 并集成 VFS ---- */
    fat32_fs_t fs;
    if (fat32_mount(&fs) == 0) {
        tty_print("[OK] FAT32 mounted. Integrating with VFS...\n");

        vfs_node_t *root = fat32_mount_to_vfs(&fs);
        if (root) {
            vfs_mount("ata0", "/", FS_FAT32, root);
            tty_print("[VFS] FAT32 mounted at /.\n");

            int fd = vfs_open("/README.TXT", O_RDONLY);
            if (fd >= 0) {
                char buf[128];
                int n = vfs_read(fd, buf, sizeof(buf)-1);
                if (n > 0) {
                    buf[n] = '\0';
                    tty_print("--- VFS README.TXT ---\n");
                    tty_print(buf);
                    tty_print("\n--- end ---\n");
                }
                vfs_close(fd);
            } else {
                tty_print("[VFS] README.TXT not found via VFS.\n");
            }

            vfs_dir_t *dir = vfs_opendir("/");
            if (dir) {
                vfs_dir_entry_t ent;
                tty_print("[VFS] Root directory contents:\n");
                while (vfs_readdir(dir, &ent) > 0) {
                    tty_print("  ");
                    tty_print(ent.name);
                    if (ent.type == VFS_FLAG_DIR)
                        tty_print(" [DIR]\n");
                    else
                        tty_print("\n");
                }
                vfs_closedir(dir);
            }
        }
    } else {
        tty_print("[FAIL] FAT32 mount failed.\n");
    }

    /* ===== 初始化进程调度器 ===== */
    tty_print("\n[..] Initializing process scheduler...\n");
    scheduler_init();

    tty_print("\n[..] Initializing LAPIC Timer...\n");
    irq_register_handler(0, apic_timer_irq_handler);
    apic_timer_init();

    tty_print("\n[..] Initializing PS/2 Keyboard...\n");
    irq_register_handler(1, keyboard_irq_handler);
    keyboard_init();
    apic_timer_set_callback(keyboard_timer_tick);

    /* ---- 内核堆测试 ---- */
    tty_print("\n[..] Testing kernel heap...\n");
    if (heap_check() == 0) {
        tty_print("[OK] Initial heap integrity check passed\n");
    } else {
        tty_setcolor(VGA_RED, VGA_BLACK);
        tty_print("[FAIL] Initial heap check failed!\n");
        tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
    }

    void *a = kmalloc(128);
    void *b = kmalloc(200);
    if (a && b) {
        tty_print("[OK] kmalloc: a=0x");
        tty_print_hex64((uint64_t)(uintptr_t)a);
        tty_print(" b=0x");
        tty_print_hex64((uint64_t)(uintptr_t)b);
        tty_print("\n");

        kfree(a);
        tty_print("[OK] Freed a\n");

        void *c = kmalloc(100);
        tty_print("[OK] kmalloc c=0x");
        tty_print_hex64((uint64_t)(uintptr_t)c);
        tty_print(" (should reuse a's space)\n");

        heap_dump();

        kfree(b);
        kfree(c);
        tty_print("[OK] Freed b and c\n");
    } else {
        tty_setcolor(VGA_RED, VGA_BLACK);
        tty_print("  ERROR: kmalloc test failed!\n");
        tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
    }

    if (heap_check() == 0) {
        tty_print("[OK] Final heap integrity check passed\n");
    } else {
        tty_setcolor(VGA_RED, VGA_BLACK);
        tty_print("[FAIL] Final heap check failed!\n");
        tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
    }

    tty_setcolor(VGA_GREEN, VGA_BLACK);
    tty_print("\nSukiOS booted successfully!\n");
    tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
    tty_print("\nKeyboard ready.\n");

    // tty_print("\n[TEST] Polling keyboard (press any key)...\n");
    // for (int i = 0; i < 20000000; i++) {   // 等待几秒，确保用户有时间按键
    //     char c = keyboard_getchar_nb();
    //     if (c) {
    //         tty_print("[OK] Got: '");
    //         tty_putchar(c);
    //         tty_print("'\n");
    //         break;
    //     }
    //     // 短暂延迟，避免过度占用 CPU
    //     for (volatile int j = 0; j < 1000; j++);
    // }
    // tty_print("[TEST] Polling done.\n");


    /* ===== 从磁盘加载 ELF 可执行文件 ===== */
    tty_print("[..] Loading ELF program from disk...\n");

    int fd = vfs_open("/ISHELL", O_RDONLY);
    if (fd >= 0) {
        size_t file_size = vfs_get_size(fd);
        if (file_size > 0) {
            uint8_t *elf_buf = (uint8_t *)kmalloc(file_size);
            if (elf_buf) {
                if (vfs_read(fd, elf_buf, file_size) == (int)file_size) {
                    pcb_t *user_proc = proc_create_from_elf(elf_buf, file_size);
                    if (user_proc) {
                        tty_print("[OK] User process created (PID=");
                        tty_print_dec((uint32_t)user_proc->pid);
                        tty_print(")\n");
                    } else {
                        tty_print("[FAIL] Could not create process from ELF\n");
                    }
                }
                kfree(elf_buf);
            }
        }
        vfs_close(fd);
    } else {
        tty_print("[WARN] /USER.ELF not found! Starting scheduler without user process.\n");
    }

    keyboard_reenable_irq();

    /* ===== 启动调度器（永不返回） ===== */
    scheduler_start();

    /* 理论上不会执行到此 */
    for (;;) __asm__ volatile ("cli; hlt");
}