/*
 * kernel/kmain.c
 * -----------------------------------------------------------------------------
 * SukiOS 内核 C 语言主入口。
 *
 * 初始化顺序（阶段一 ~ 三）：
 *   serial -> multiboot2_parse -> framebuffer/console
 *   -> gdt(+tss) -> idt -> pic_remap -> pit -> keyboard -> sti
 *   -> 早期内核回显循环（验证 PIT 节拍与键盘中断）
 */
#include <kernel/types.h>
#include <kernel/serial.h>
#include <kernel/console.h>
#include <kernel/multiboot2.h>
#include <kernel/framebuffer.h>
#include <kernel/gdt.h>
#include <kernel/interrupts.h>
#include <kernel/pic.h>
#include <kernel/pit.h>
#include <kernel/keyboard.h>
#include <kernel/string.h>
#include <kernel/task.h>
#include <kernel/syscall.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/kmalloc.h>

#include <ipc/port.h>
#include <kernel/ata.h>

/* Ring3 用户程序 blob（user/ 下的 C 程序，Makefile 嵌入内核镜像） */
extern const uint8_t user_fs_server_start[],    user_fs_server_end[];
extern const uint8_t user_input_server_start[], user_input_server_end[];
extern const uint8_t user_shell_start[],        user_shell_end[];

/* 内核控制台服务：拥有 CONSOLE_PORT，接收文本消息并打印（阶段七演示） */
static void console_srv(void *arg)
{
    (void)arg;
    static uint8_t buf[512];
    port_set_owner(CONSOLE_PORT, sched_current());
    for (;;) {
        uint32_t n = 0;
        if (ipc_recv_kernel(CONSOLE_PORT, buf, sizeof(buf) - 1, &n, true)
                == MACH_MSG_SUCCESS && n > sizeof(mach_msg_header_t)) {
            uint32_t len = n - (uint32_t)sizeof(mach_msg_header_t);
            char *text = (char *)buf + sizeof(mach_msg_header_t);
            text[len] = '\0';
            kprintf("  [console-srv] got %u bytes via IPC: %s", len, text);
        }
    }
}

static boot_info_t g_boot;

static void draw_boot_logo(void)
{
    if (!fb_available()) {
        return;
    }
    uint32_t W = fb_width();
    fb_clear(FB_BG);
    uint32_t s = 96;
    uint32_t x = (W - s) / 2;
    uint32_t y = 40;
    fb_fill_rect(x, y, s, s, FB_PINK);
    fb_fill_rect(x + 16, y + 16, s - 32, s - 32, FB_BG);
    fb_fill_rect(x + 32, y + 32, s - 64, s - 64, FB_CYAN);
}

/* 阶段四内存子系统自检：PMM 分配/释放、kmalloc 读写、独立地址空间创建 */
static void mm_selftest(void)
{
    kprintf("[mm] selftest: free pages before = %lu\n",
            (unsigned long)pmm_free_pages());

    void *p1 = pmm_alloc_page();
    void *p2 = pmm_alloc_page();
    kprintf("[mm] pmm_alloc -> %p, %p\n", p1, p2);
    pmm_free_page(p1);
    pmm_free_page(p2);

    char *buf = (char *)kmalloc(128);
    strcpy(buf, "kmalloc works: SukiOS heap OK");
    kprintf("[mm] kmalloc(128)=%p -> \"%s\"\n", buf, buf);

    int *arr = (int *)kzalloc(64 * sizeof(int));
    bool zeroed = true;
    for (int i = 0; i < 64; i++) {
        if (arr[i] != 0) { zeroed = false; break; }
    }
    arr[63] = 0xABCD;
    kprintf("[mm] kzalloc zeroed=%s, arr[63]=0x%x\n",
            zeroed ? "yes" : "no", (unsigned)arr[63]);
    kfree(buf);
    kfree(arr);

    uint64_t as = vmm_create_address_space();
    kprintf("[mm] new address space PML4 @ phys %p\n", (void *)as);

    kprintf("[mm] selftest: free pages after = %lu\n",
            (unsigned long)pmm_free_pages());
}

void kmain(uint64_t magic, uint64_t mbi_phys)
{
    serial_init();
    serial_writestr("\n[boot] SukiOS kernel entered (long mode, higher half).\n");

    if (magic != MULTIBOOT2_MAGIC) {
        serial_writestr("[boot] FATAL: not Multiboot2.\n");
        for (;;) { __asm__ volatile("hlt"); }
    }
    if (!multiboot2_parse(mbi_phys, &g_boot)) {
        serial_writestr("[boot] FATAL: bad Multiboot2 info.\n");
        for (;;) { __asm__ volatile("hlt"); }
    }

    fb_init(&g_boot);
    fbcon_init();
    console_init();
    draw_boot_logo();

    /* Unicode/中文显示自测：含中文与 U+2713 勾号（3 字节 UTF-8） */
    kprintf("[console] UTF-8 test: 中文显示正常 ✓ 操作系统启动成功\n");

    kprintf("========================================\n");
    kprintf("      SukiOS  x86_64  Hybrid Kernel     \n");
    kprintf("========================================\n");
    kprintf("[boot] display: %s\n",
            fb_available() ? "framebuffer (graphics)" : "VGA text (fallback)");
    kprintf("[boot] usable RAM: %u MiB\n",
            (unsigned)(g_boot.mem_total / (1024 * 1024)));

    /* ---- 阶段三：中断子系统 ---- */
    gdt_init();
    fpu_init();                     /* 启用 FPU/SSE 状态保存（D2 项） */
    idt_init();
    pic_remap();

    /* ---- 阶段四：内存管理 ---- */
    pmm_init(&g_boot);
    vmm_init();
    kheap_init();
    mm_selftest();

    /* ---- 阶段五：调度器 ---- */
    sched_init();

    keyboard_init();
    pit_init(100);            /* 100 Hz 系统节拍，驱动抢占 */
    interrupts_enable();

    /* ---- 阶段六：syscall + Ring3 ---- */
    syscall_init();

    /* ---- 阶段七：Mach IPC ---- */
    ipc_init();
    task_create_kernel(console_srv, NULL, "console-srv");

    /* ---- 阶段八：磁盘（内核态特例）与 Ring3 FAT32 服务 ---- */
    bool disk_ok = ata_init();
    if (disk_ok) {
        disk_srv_start();
        task_t *fs_task = task_create_user(user_fs_server_start,
                         (size_t)(user_fs_server_end - user_fs_server_start),
                         "fs-server");
        /* A2 项：仅 FS_SERVER 被授权向内核 DISK_PORT 发送磁盘请求 */
        if (fs_task) {
            port_grant_send(DISK_PORT, fs_task);
        }
    } else {
        kprintf("[boot] no disk: FS_SERVER not started\n");
    }

    /* ---- 阶段九：Ring3 输入服务 + Shell ---- */
    task_create_user(user_input_server_start,
                     (size_t)(user_input_server_end - user_input_server_start),
                     "input-server");
    task_create_user(user_shell_start,
                     (size_t)(user_shell_end - user_shell_start), "shell");

    kprintf("[boot] all services spawned; idle task parked.\n\n");

    /* task0 = idle：仅剩 hlt 等待中断（一切交互走 Ring3 服务管线） */
    for (;;) {
        __asm__ volatile("hlt");
    }
}
