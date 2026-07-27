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
#include <kernel/acpi.h>
#include <kernel/apic.h>
#include <kernel/ioapic.h>
#include <kernel/clock.h>
#include <kernel/smp.h>
#include <kernel/percpu.h>
#include <kernel/diagnostics.h>
#include <kernel/keyboard.h>
#include <kernel/string.h>
#include <kernel/task.h>
#include <mm/vma.h>           /* P0-5：vma_selftest */
#include <kernel/syscall.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/kmalloc.h>

#include <ipc/port.h>
#include <kernel/ata.h>
#include <kernel/hda.h>

/* L3：由 boot.S 在探测到 CPU 支持 SMAP 后置 1（见 syscall.c 的 copy_*_user
 * 围栏）。此处仅用于启动日志输出以验证 SMAP 是否真正生效。 */
extern uint8_t g_smap_enabled;

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

    /* M2 审计：引用计数压测（OOL 共享/饱和粘滞/共享页守卫） */
    pmm_selftest();

    /* P0-5：VMA 链表 + 按需分页 + COW fork/断开 + munmap 全链路自检 */
    vma_selftest();

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
    /* L3：确认 SMAP（管理者态不可访问用户页）是否已随 boot.S 探测生效。
     * 若 CPU 不支持（如极旧虚拟机）则为 disabled，系统仍可运行但丧失该防线。 */
    kprintf("[boot] SMAP (user-memory protection): %s\n",
            g_smap_enabled ? "ENABLED" : "disabled (CPU lacks SMAP)");

    /* ---- 阶段三：中断/异常子系统（IDT 先就位，APIC 向量才能投递）---- */
    gdt_init();
    fpu_init();                     /* 启用 FPU/SSE 状态保存（D2 项） */
    idt_init();

    /* ---- 阶段四：内存管理 ---- */
    pmm_init(&g_boot);
    vmm_init();
    kheap_init();
    mm_selftest();

    /* ---- P0-1/P0-2/P0-4：ACPI 拓扑发现 + LAPIC/IOAPIC 取代 8259 PIC+PIT ---- */
    acpi_init();                                   /* 解析 RSDP/XSDT/MADT/HPET */
    uint8_t bsp_lapic = lapic_init();              /* 启用本地 APIC */
    ioapic_init();                                 /* 初始化 I/O APIC（屏蔽全部） */
    ioapic_set_dest(bsp_lapic);                    /* 中断投递到 BSP */
    pic_disable();                                 /* 屏蔽遗留 8259，防双投递 */
    clock_init();                                  /* TSC 校准 + LAPIC 100Hz 节拍 + HPET 探测 */

    /* ---- P0-3：SMP —— BSP percpu 安装 + AP 启动（INIT-SIPI-SIPI）---- */
    percpu_install(0, bsp_lapic);                  /* BSP 的 GS_BASE -> percpu[0] */
    smp_init();                                    /* 依 MADT 唤醒全部 AP */

    /* ---- 阶段五：调度器 ---- */
    sched_init();

    keyboard_init();              /* 经 I/O APIC GSI1 -> IRQ1 */
    interrupts_enable();

    /* ---- 阶段六：syscall + Ring3 ---- */
    syscall_init();

    /* ---- 阶段七：Mach IPC ---- */
    ipc_init();
    task_create_kernel(console_srv, NULL, "console-srv");

    /* ---- 阶段八·补：Intel HDA 音频（内核态特例，类 ATA） ---- */
    hda_init();

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
