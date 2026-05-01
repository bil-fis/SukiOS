/* =============================================================================
 * SukiOS - InitKernel
 *
 * InitKernel 是内核初始化的第二阶段。
 * 在 EarlyKernel 完成所有底层初始化后调用。
 * ============================================================================= */

#include "kernel/tty.h"

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
    tty_print("SukiOS v0.1.0 - x86_64\n");
    tty_setcolor(VGA_GREEN, VGA_BLACK);
    tty_print("SukiOS booted successfully!\n");
}
