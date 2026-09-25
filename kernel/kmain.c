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
#include <sukios/kdr.h>
#include <kernel/console.h>
#include <kernel/ksym.h>    /* ksym_dump_count() */
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
#include <kernel/config.h>      /* CONFIG_SMP：构建形态（默认单核） */
#include <kernel/display_cfg.h> /* g_display：显示配置（内置默认值；运行时可被 registry 覆盖） */
#include <kernel/registry.h>   /* SukiRegistry Hive 解析（/sys/configs） */
#include <kernel/vfs.h>        /* kern_fs_read_file：内核态同步读文件 */
#include <kernel/cdrom.h>       /* CdromInit：ATAPI 光驱探测 */
#include <kernel/iso9660.h>     /* IsoMount/IsoIsMounted/IsoReadFile：内核 ISO9660 */
#include <kernel/usb.h>         /* usb_init：USB 主机栈（UHCI + Hub + HID 键鼠） */
#include <kernel/kbuild_info.h> /* KERNEL_ 与 CONFIG_DRIVER_ 系列宏的兜底默认 */
#include <kernel/posix.h>       /* posix_init()：完整 POSIX 系统调用层 */
#include <kernel/rtc.h>         /* rtc_time_init()：CLOCK_REALTIME 墙上时间基准 */
#include <kernel/smp.h>
#include <kernel/percpu.h>
#include <kernel/diagnostics.h>
#include <kernel/gdbstub.h>  /* P0-R3：常备串口 GDB stub（COM2） */
#include <kernel/security.h> /* P0-8：安全地基（UMIP/IST 守卫栈/Meltdown 检测） */
#include <kernel/keyboard.h>
#include <kernel/string.h>
#include <kernel/task.h>
#include <mm/vma.h> /* P0-5：vma_selftest */
#include <kernel/abilities/kminiz.h> /* 内核内嵌压缩能力（miniz）自检 */
#include <kernel/bootanim.h>    /* 启动动画：启动图标 + 圆角进度条（registry 驱动） */
#include <kernel/syscall.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/kmalloc.h>

#include <ipc/port.h>
#include <kernel/ata.h>
#include <kernel/ahci.h> /* P0-7：AHCI DMA 优先探测 */

/* 声明在 kernel/sched/sched.c：将 BSP 引导流切换到 idle0 独立内核栈 */
extern void sched_switch_to_idle0(void);

/* 内置驱动注册（kernel/driver/builtin_drivers.c）：把内建 PCI 驱动（ata/ahci/
 * hda/e1000/uhci）统一登记到设备/驱动管理器；drivers_disk_ready() 供 late-init
 * 判定磁盘子系统是否已就绪。 */
extern int  builtin_drivers_register(void);
extern bool drivers_disk_ready(void);

/* 前向声明：内核完全稳定后的引导收尾线程（定义于本文件末尾）。
 * 由 kmain 在 sched_switch_to_idle0() 之前经 task_create_kernel 拉起，
 * 运行于独立内核栈、被正常调度，负责加载 disk-srv 与全部 Ring3 服务。 */
static void boot_late_init(void *arg);
#include <kernel/pci.h> /* P0-1/P0-2：ECAM、_PRT 路由、MSI 编程 */
#include <kernel/hda.h>
#include <kernel/e1000.h>

/* L3：由 boot.S 在探测到 CPU 支持 SMAP 后置 1（见 syscall.c 的 copy_*_user
 * 围栏）。此处仅用于启动日志输出以验证 SMAP 是否真正生效。 */
extern uint8_t g_smap_enabled;

/* Ring3 用户程序 blob（user/ 下的 C 程序，Makefile 嵌入内核镜像） */
extern const uint8_t user_fs_server_start[], user_fs_server_end[];
extern const uint8_t user_input_server_start[], user_input_server_end[];
extern const uint8_t user_display_server_start[], user_display_server_end[];
extern const uint8_t user_shell_start[], user_shell_end[];
extern const uint8_t user_posixtest_start[], user_posixtest_end[];
extern const uint8_t user_mouse_server_start[], user_mouse_server_end[];
extern const uint8_t user_net_server_start[], user_net_server_end[];
extern const uint8_t user_nettest_start[], user_nettest_end[];
extern const uint8_t user_curl_test_start[], user_curl_test_end[];
extern const uint8_t user_curl_app_test_start[], user_curl_app_test_end[];
extern const uint8_t user_dltest_start[], user_dltest_end[];
extern const uint8_t user_winhello_start[], user_winhello_end[];
extern const uint8_t user_suikitest_start[], user_suikitest_end[];

/* 内核控制台服务：拥有 CONSOLE_PORT，接收文本消息并打印（阶段七演示） */
static void console_srv(void *arg)
{
    (void)arg;
    static uint8_t buf[512];
    port_set_owner(CONSOLE_PORT, sched_current());
    for (;;)
    {
        uint32_t n = 0;
        if (ipc_recv_kernel(CONSOLE_PORT, buf, sizeof(buf) - 1, &n, true) == MACH_MSG_SUCCESS && n > sizeof(mach_msg_header_t))
        {
            uint32_t len = n - (uint32_t)sizeof(mach_msg_header_t);
            char *text = (char *)buf + sizeof(mach_msg_header_t);
            text[len] = '\0';
            dbg_printf("  [console-srv] got %u bytes via IPC: %s", len, text);
        }
    }
}

boot_info_t g_boot;

/*
 * 早期屏幕铺垫：此刻 registry 尚未解析（stage3 才读 /sys/configs/system.sre），
 * 故这里只把屏幕清成黑色（= 启动动画的黑底），不做任何图标绘制。
 * 真正的启动图标与进度条由 BootPlayAnimation()（stage3，registry 解析后）依
 * System/Boot 的 ShowLogo / ShowProgress / BootLogoID / CustomLogo 绘制。
 */
static void draw_boot_logo(void)
{
    if (!fb_available())
    {
        return;
    }
    fb_clear(FB_BLACK);
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
    for (int i = 0; i < 64; i++)
    {
        if (arr[i] != 0)
        {
            zeroed = false;
            break;
        }
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

/* ===================== 启动阶段辅助（含预留接入点） ===================== */

/* 全局：registry 是否允许加载 kdr（可由 /System/Kernel/KdrEnabled 关闭） */
static bool g_kdr_enabled = true;

/* registry 解析结果（Stage3LoadConfigAndKdr 填充）：启动动画按其绘制，
 * 并作为「Kernel/Drivers/Services」等键的运行时查询基础。 */
static system_config_t g_syscfg;
static bool g_syscfg_ok = false;

/* 内核【实际探测】到的启动设备类型。注册表 System/Boot/BootDeviceType 内只是
 * 「标识」，真实取值以本探测为准：解析后经 RegistrySetOverrideString() 覆盖，
 * 任何（内核或 Ring3）按该路径读值的调用者拿到的都是这里的真实结果。 */
static const char *g_boot_dev_type = "none";

/* 解析 GRUB 内核命令行（-v/--verbose）。预留 suki.debug=1（后期权限子系统调试角色）。 */
static void ParseBootCmdline(void)
{
    const char *cl = g_boot.cmdline;
    if (!cl[0])
        return;
    for (size_t i = 0; cl[i]; ) {
        while (cl[i] == ' ' || cl[i] == '\t') i++;
        if (!cl[i]) break;
        size_t s = i;
        while (cl[i] && cl[i] != ' ' && cl[i] != '\t') i++;
        size_t len = i - s;
        if ((len == 2 && memcmp(cl + s, "-v", 2) == 0) ||
            (len == 9 && memcmp(cl + s, "--verbose", 9) == 0)) {
            g_boot_verbose = true;
        }
        /* 预留：suki.debug=1 -> 后期权限子系统调试角色启用点（本期未实现） */
    }
    kprintf("[boot] cmdline: '%s' (verbose=%s)\n", cl, g_boot_verbose ? "on" : "off");
}

/* 预留：混合风格权限提升子系统（UAC/授权/调试角色）。
 * 设计见《SukiOS 混合风格权限提升设计文档》，本期不实现，仅保留接入点。 */
static void SecurityReserve(void)
{
    kprintf("[boot] security: 权限提升子系统(UAC/授权)预留，本期不实现（见设计文档）；仅保留接入点\n");
}

/*
 * 启动动画（stage3，registry 解析后）。
 *   - ShowLogo      -> 是否绘制启动图标（CustomLogo 优先，其次 BootLogoID 内嵌图）；
 *   - ShowProgress  -> 是否绘制进度条（黑底白填充、两侧圆角）；
 *   - 之后由 boot_late_init 的各里程碑经 bootanim_progress() 推进进度。
 * 帧缓冲不可用（纯文本回退）时静默跳过，绝不阻塞启动。
 */
static void BootPlayAnimation(void)
{
    kprintf("[boot] stage3: boot animation (registry-driven)\n");
    bootanim_setup(g_syscfg_ok ? &g_syscfg : NULL);
    bootanim_progress(15);
}

/* 第三步：读取 registry 配置（/sys/configs/system.sre）并按配置门控 kdr。
 * 文件缺失/CRC 失败则回退默认，绝不阻塞启动。 */
/* 读取 registry 配置：优先内核 ISO9660（仅光盘启动、'/' 已挂 ISO 时），
 * 否则走 FS_PORT（FatFs/Ring3 FS_SERVER）。两者语义一致：成功填 buf/n，失败返回非 0。 */
static int Stage3ReadConfig(const char *path, uint8_t *buf, uint32_t cap, uint32_t *out_n)
{
    if (IsoIsMounted()) {
        return IsoReadFile(path, buf, cap, out_n);
    }
    return kern_fs_read_file(path, buf, cap, out_n);
}

static void Stage3LoadConfigAndKdr(void)
{
    uint8_t *buf = kmalloc(65536);
    if (!buf) {
        kprintf("[boot] config: kmalloc failed, use defaults\n");
        return;
    }
    uint32_t n = 0;
    int rc = Stage3ReadConfig("/sys/configs/system.sre", buf, 65536, &n);
    if (rc != 0 || n == 0) {
        kprintf("[boot] config: cannot read /sys/configs/system.sre (rc=%d), using defaults\n", rc);
        kfree(buf);
        return;
    }
    const sukreg_header_t *h = (const sukreg_header_t *)buf;
    system_config_t cfg;
    if (!RegistryParseSystem(buf, n, &cfg)) {
        /* 诊断：打印实际读到的字节与头部字段，便于定位「读到了什么」 */
        kprintf("[boot] config: hive parse/CRC failed, using defaults "
                "(n=%u magic=%02x%02x%02x%02x%02x%02x%02x%02x ver=%u "
                "body=%llu crc=%08x)\n",
                n, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6],
                buf[7], h->version, (unsigned long long)h->body_size,
                h->crc32);
        kfree(buf);
        return;
    }
    kprintf("[boot] config: loaded system.sre (generation=%llu)\n",
            (unsigned long long)h->generation);
    if (cfg.have_display) {
        g_display.width  = cfg.display_width;
        g_display.height = cfg.display_height;
        kprintf("[boot] config: display %ux%u\n",
                cfg.display_width, cfg.display_height);
    }
    if (cfg.have_verbose)
        g_boot_verbose = cfg.boot_verbose;
    if (cfg.have_kdr)
        g_kdr_enabled = cfg.kdr_enabled;

    /* 启动画面配置回声（ShowLogo/ShowProgress/BootLogoID/CustomLogo） */
    kprintf("[boot] config: boot logo=%s progress=%s id='%s' custom=%s\n",
            (cfg.have_show_logo ? (cfg.show_logo ? "on" : "off") : "default(on)"),
            (cfg.have_show_progress ? (cfg.show_progress ? "on" : "off") : "default(on)"),
            cfg.have_boot_logo_id ? cfg.boot_logo_id : "(default)",
            (cfg.have_custom_logo && cfg.custom_logo_enabled)
                ? (cfg.custom_logo_direct_path[0] ? cfg.custom_logo_direct_path
                                                  : cfg.custom_logo_path)
                : "off");

    /* 保留整份 hive 字节（内部拷贝）供运行时按路径查询：
     * SYS_REGISTRY_READ（Ring3 读注册表）与内核子系统（如 Drivers/Services 清单）
     * 都基于此缓存；System/Boot/BootDeviceType 另由 RegistrySetOverrideString()
     * 覆盖为内核实际探测结果。 */
    if (!RegistryCacheSystem(buf, n))
        kprintf("[boot] config: warn: registry cache alloc failed "
                "(registry path queries unavailable)\n");

    g_syscfg = cfg;
    g_syscfg_ok = true;

    kfree(buf);
}

/* 预留：SukiDesktopManager（sdm）桌面管理器。
 * 后期负责合成桌面/启动用户会话；本期以 SukiShell 作为桌面占位替身。 */
static void SukiDesktopManager(void)
{
    kprintf("[boot] SukiDesktopManager: reserved; launching SukiShell as desktop surrogate\n");
    task_create_user(user_shell_start,
                     (size_t)(user_shell_end - user_shell_start), "SukiShell");
}

/* 预留：SukiLogon 登录管理器（类 winlogon）。
 * 后期依据用户数据库/密码决定是否显示登录页；本期无用户库，直接进入桌面。 */
static void SukiLogon(void)
{
    kprintf("[boot] SukiLogon: reserved; no password configured -> proceed to desktop\n");
    SukiDesktopManager();
}

void kmain(uint64_t magic, uint64_t mbi_phys)
{
    serial_init();
    serial_writestr("\n[boot] SukiOS kernel entered (long mode, higher half).\n");

    /* P0-8：极早期重播种栈金丝雀（TSC 熵）。必须在开中断/启动 AP/创建
     * 任务之前——kmain 自身永不返回，是唯一「序言读旧值」的在飞栈帧。 */
    stack_canary_reseed();

    if (!bootinfo_prepare(magic, mbi_phys, &g_boot))
    {
        serial_writestr("[boot] FATAL: unsupported boot protocol (not Multiboot2/PVH).\n");
        for (;;)
        {
            __asm__ volatile("hlt");
        }
    }

    /* 解析 GRUB 内核命令行（-v/--verbose 等）。第三步系统初始化默认仅串口输出，
     * 仅当 -v/--verbose 时才镜像到屏幕。显示配置不再经 GRUB 模块传递，改由第三步
     * 从 registry（/sys/configs/system.sre）读取，并以内置默认值兜底。 */
    ParseBootCmdline();

    fb_init(&g_boot);
    fbcon_init();
    console_init();
    draw_boot_logo();

    /* 内核信息/版本（来自构建配置器生成的 build/config/build_config.h） */
    kprintf("[boot] %s %s '%s' — %s\n", KERNEL_NAME, KERNEL_VERSION,
            KERNEL_CODENAME, KERNEL_BUILD_INFO);

    /* Unicode/中文显示自测：含中文与 U+2713 勾号（3 字节 UTF-8） */
    kprintf("[console] UTF-8 test: 中文显示正常 ✓ 操作系统启动成功\n");
    kprintf("========================================\n");
    kprintf("      SukiOS  x86_64  Hybrid Kernel     \n");
    kprintf("========================================\n");
    kprintf("[boot] display: %s\n",
            fb_available() ? "framebuffer (graphics)" : "VGA text (fallback)");
    /* P0-6：报告固件形态。UEFI 下帧缓冲来自 GOP（GRUB 经 MB2 tag8 转交），
     * RSDP 来自 MB2 tag14/15 副本（acpi_set_rsdp_hint 通路）。 */
    kprintf("[boot] firmware: %s\n",
            g_boot.efi_boot ? "UEFI (OVMF/GOP path)" : "Legacy BIOS");
    kprintf("[boot] usable RAM: %u MiB\n",
            (unsigned)(g_boot.mem_total / (1024 * 1024)));
    /* L3：确认 SMAP（管理者态不可访问用户页）是否已随 boot.S 探测生效。
     * 若 CPU 不支持（如极旧虚拟机）则为 disabled，系统仍可运行但丧失该防线。 */
    kprintf("[boot] SMAP (user-memory protection): %s\n",
            g_smap_enabled ? "ENABLED" : "disabled (CPU lacks SMAP)");

    /* ---- 阶段三：中断/异常子系统（IDT 先就位，APIC 向量才能投递）---- */
    gdt_init();
    fpu_init(); /* 启用 FPU/SSE 状态保存（D2 项） */
    idt_init();

    /* ---- 阶段四：内存管理 ---- */
    pmm_init(&g_boot);
    vmm_init();
    kheap_init();
    mm_selftest();

    /* ---- 内核内嵌压缩能力（miniz）自检：为 minOSEnv/rootfs 预留 ----
     * 依赖内核堆（kheap_init）已就绪。 */
    kminiz_selftest();

    /* ---- P0-8：安全地基总装（vmm/kheap 就绪后、SMP 启动前）----
     * UMIP 使能 + NXE/SMEP/SMAP 复核 + BSP 守卫页 IST 栈 + Meltdown 检测 */
    security_init();

    /* 预留：混合风格权限提升子系统（UAC/授权）。本期不实现，仅保留接入点。 */
    SecurityReserve();

    /* ---- P0-R3：可诊断性设施 ----
     * gdbstub：COM2 常备 GDB 远程调试（断点/单步/读写内存寄存器/monitor klog）；
     * diag_selftest：故意触发一次 WARN_ON 验证「打印+回溯+继续」全链路。 */
    gdbstub_init();
    diag_selftest();

    /* ---- P0-1/P0-2/P0-4：ACPI 拓扑发现 + LAPIC/IOAPIC 取代 8259 PIC+PIT ---- */
    acpi_set_rsdp_hint(g_boot.rsdp_copy_phys); /* P0-6：UEFI 下唯一 RSDP 来源 */
    acpi_init();                               /* 解析 RSDP/XSDT/MADT/HPET/MCFG/_PRT */
    pci_cfg_init();                            /* P0-1：MCFG 存在则切 ECAM，否则 PIO 回退 */

    /* ---- 驱动/设备管理器 + .kdr 内核模块加载 ----
     * driver_manager_init / device_manager_init 建立驱动与设备注册表；
     * scan_pci 枚举真实 PCI 设备并注册为 device_t；
     * kdr_load_all 从引导模块加载 .kdr 内核驱动（其 kdr_init 注册 driver_t）；
     * 随后注册 2 个同型虚拟演示设备，验证「一套驱动驱动多个设备」。 */
    driver_manager_init();
    device_manager_init();
    ksym_dump_count();
    device_manager_scan_pci();
    /* kdr 加载移至第三步（boot_late_init 内 Stage3LoadConfigAndKdr 之后），
     * 由 registry 配置门控，默认启用。此处仅注册演示设备，待 kdr 加载后绑定。 */
    device_manager_add_demo(0, "demo-led-0");
    device_manager_add_demo(1, "demo-led-1");

    uint8_t bsp_lapic = lapic_init();          /* 启用本地 APIC */
    ioapic_init();                             /* 初始化 I/O APIC（屏蔽全部） */
    ioapic_set_dest(bsp_lapic);                /* 中断投递到 BSP */
    pic_disable();                             /* 屏蔽遗留 8259，防双投递 */
    clock_init();                              /* TSC 校准 + LAPIC 100Hz 节拍 + HPET 探测 */

    /* ---- P0-3：SMP —— BSP percpu 安装 + AP 启动（INIT-SIPI-SIPI）----
     * SMP 是编译期可选特性（include/kernel/config.h 的 CONFIG_SMP），默认
     * 关闭（单核构建）：此时 smp_init() 仅注册 IPI handler 并报告单核形态，
     * 不会唤醒任何 AP，系统全程运行在 BSP 上。开启方式：make SMP=1。 */
    kprintf("[boot] SMP build config: %s (max_cpus=%u)\n",
            CONFIG_SMP ? "ENABLED (multi-core)" : "disabled (single-core)",
            (unsigned)MAX_CPUS);
    percpu_install(0, bsp_lapic); /* BSP 的 GS_BASE -> percpu[0] */
    smp_init();                   /* 依 MADT 唤醒全部 AP */

    /* ---- 阶段五：调度器 ---- */
    sched_init();

    keyboard_init(); /* 经 I/O APIC GSI1 -> IRQ1 */
    /* P0 鼠标驱动（Ring3 .kdr 形态，内核态采集经 IRQ12 经 sys_mouse_read 派发） */
    extern bool mouse_init(void);
    mouse_init();    /* 经 I/O APIC GSI12 -> IRQ12 */
    interrupts_enable();

    /* ---- 阶段五·补：USB 主机栈改由「驱动优先阶段」统一初始化 ----
     * USB(UHCI) 现作为一个 driver_t 在下方 posix_init() 之后的
     * builtin_drivers_register() 中经设备/驱动管理器匹配、probe，与其它内置驱动
     * 一并【在任何 Ring3 服务之前】完成初始化，避免此前“USB 枚举与 display/input
     * 服务并发初始化互相拖慢、display-server 迟迟不就绪”的竞争窗口。 */

    /* ---- 阶段六：syscall + Ring3 ---- */
    syscall_init();

    /* POSIX 墙上时间基准（读一次 CMOS RTC）。依赖 clock_init 已完成 TSC 校准，
     * 且必须在任何用户任务跑起来之前——time()/clock_gettime 一开始就要有正确
     * 的墙上时间，而不是让应用先看到一段「退化时间」。 */
    rtc_time_init();

    /* ---- 阶段七：Mach IPC ---- */
    ipc_init();

    /* POSIX 系统调用层（fd 槽池等）。
     * 顺序要求：在 ipc_init() 之后（fd 层的 open/read/write 全部经 Mach IPC
     * 转发到 FS_SERVER，端口子系统须先就绪）、在任何用户任务创建之前
     * （fd_install_stdio 在每个 Ring3 任务创建时执行，需要槽池已初始化）。 */
    posix_init();

    /* 驱动优先阶段已移至 boot_late_init（内核收尾内核线程）的【最前面】执行：
     * 每个驱动 probe 会对其硬件做复位/等待（如 UHCI 控制器与端口复位 20~100ms），
     * 这些等待必须用内核 msleep 真睡眠让出 CPU（用户要求「复位也不要忙等」）。
     * 而 msleep 需要真实的“可睡眠任务上下文”，kmain 阶段 current_task 仍是 idle0
     * （把 idle0 置 BLOCKED 不安全），故统一放到 boot_late 任务里执行；位置仍在
     * 【任何 Ring3 服务之前】，满足“驱动最先加载”。 */
    task_create_kernel(console_srv, NULL, "SukiConsoleServer");

    /* HDA 音频（class 04/03）与 Intel 8254x/e1000 网卡（class 02/00）均已在上述
     * 驱动优先阶段经 device/driver manager 探测并初始化。net_srv_start()
     * （NET_PORT 内核收发服务）仍按原设计在 late-init 阶段启动。 */

    /* ---- 阶段八/九：磁盘（内核态特例）与 Ring3 服务的加载 ----
     * 关键设计（用户明确要求：「内核基本完全稳定后，再开始加载用户态」）：
     *   内核侧所有核心子系统（SMP/调度/IPC/磁盘探测/音频/全部 selftest）此时
     *   已全部静态完成。因此这部分「拉起 disk-srv 内核线程 + 用户态
     *   fs-server/input-server/shell」必须延后到一个独立的内核线程 boot_late_init
     *   中——它运行于独立内核栈、被正常调度，在内核彻底稳定后才执行，从而真正
     *   『先内核稳定、后用户态』，彻底消除此前『边初始化边跑用户态』的竞争窗口。
     *
     * 时序约束（务必遵循）：
     *   1) 先在 BSP 引导栈上 spawn boot_late_init（加入运行队列，向某核发 IPI）；
     *   2) 紧接 sched_switch_to_idle0() 把 BSP 引导流冻结为 idle0。
     *   这样 boot_late_init 永远不会在 BSP 引导栈仍活跃时被调度，避免与 kmain
     *   收尾重叠；它只会在 idle0 之外的核（或 idle0 被调度让出后）安全运行。 */

    /* 内核已稳定：先关中断再 spawn 引导收尾线程（磁盘 + Ring3 服务），最后冻结
     * BSP 引导栈。
     * 关键时序（防止 BSP 引导栈在冻结前被切走）：
     *   task_create_kernel 会向目标核发 IPI_RESCHED（若 boot-late 被 RR 绑到 BSP
     *   则为 self-IPI）。在中断**开启**窗口里 self-IPI 可能令 BSP 提前 schedule 到
     *   boot_late_init，使『加载用户态』发生在 BSP 引导栈仍活跃时——这正是要杜绝
     *   的竞态。故此处先 interrupts_disable()，让 self-IPI 暂存、不会切入；随后
     *   sched_switch_to_idle0() 在关中断下构造独立栈并 context_switch 到 idle0；
     *   idle0 的 bsp_idle 才 sti，此时 self-IPI 在独立栈上下文被响应、调度到
     *   boot_late_init——BSP 引导栈早已冻结，真正『内核稳定后再加载用户态』。 */
    interrupts_disable();
    task_create_kernel(boot_late_init, NULL, "SukiBootLate");

    /* task0 = idle0：将 BSP 引导流切换到 idle0 的独立内核栈，BSP 引导栈从此冻结。
     * 切换后 idle0 在独立栈上运行 bsp_idle（hlt + schedule 循环），避免 idle0 复
     * 用 BSP 引导栈导致切回时返回地址被覆盖而触发 #UD（vector 6 / system halted）。 */
    sched_switch_to_idle0();

    /* never returns */
    (void)0;
}

/*
 * registry 读取链路自检：验证「解析 -> 缓存 -> 按路径查询 -> 动态值覆盖」，
 * 即 SYS_REGISTRY_READ（Ring3 读注册表）所走的同一条通路。按类型安全解码
 * （hive 中的字符串值不带 NUL，必须先拷入临时缓冲再打印）。
 */
static void RegistrySelfTest(void)
{
    static const char *const paths[] = {
        "System/Boot/BootDeviceType",
        "System/Boot/ShowLogo",
        "System/Boot/ShowProgress",
        "System/Boot/BootLogoID",
        "System/Kernel/KdrEnabled",
        "System/Drivers/Intel/UHCI/Enabled",
        "System/Drivers/Intel/UHCI/Match",
        "System/Services/Shell/Autostart",
        "System/Display/Width",
        "System/Boot/NoSuchValue",
    };
    kprintf("[reg] self-test begin (parse->cache->query->override)\n");
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        uint32_t type = 0;
        const uint8_t *v = NULL;
        uint64_t len = 0;
        if (RegistryQuery(paths[i], &type, &v, &len) != 0) {
            kprintf("[reg]   %s -> (missing)\n", paths[i]);
            continue;
        }
        if (type == SUKREG_TYPE_STRING) {
            char tmp[96];
            size_t n = ((size_t)len < sizeof(tmp) - 1) ? (size_t)len
                                                       : sizeof(tmp) - 1;
            memcpy(tmp, v, n);
            tmp[n] = '\0';
            kprintf("[reg]   %s -> str \"%s\"\n", paths[i], tmp);
        } else if (type == SUKREG_TYPE_BOOL) {
            kprintf("[reg]   %s -> bool %s\n", paths[i],
                    (len > 0 && v[0]) ? "true" : "false");
        } else if (type == SUKREG_TYPE_UINT64 || type == SUKREG_TYPE_INT64) {
            uint64_t x = 0;
            for (uint64_t b = 0; b < 8 && b < len; b++)
                x |= (uint64_t)v[b] << (8 * b);
            kprintf("[reg]   %s -> int %llu\n", paths[i],
                    (unsigned long long)x);
        } else {
            kprintf("[reg]   %s -> type %u (%llu bytes)\n", paths[i],
                    (unsigned)type, (unsigned long long)len);
        }
    }
    kprintf("[reg] self-test end\n");
}

/*
 * boot_late_init —— 内核完全稳定后、统一加载磁盘与 Ring3 服务的引导收尾线程。
 *
 * 运行时机：由 kmain 在 sched_switch_to_idle0() 之前经 task_create_kernel 拉起、
 * 加入运行队列；kmain 随后立即冻结为 idle0，本线程在独立内核栈被调度时，
 * 此时：
 *   - SMP 所有 AP 已 online 且各自 LAPIC 100Hz 节拍已跑（对称调度已生效）；
 *   - 调度器/IDT/IPC 端口表已静态完成，BSP 引导栈已冻结为 idle0；
 *   - 本线程运行在独立内核栈、被正常调度，绝不会与任何『内核收尾』动作重叠。
 * 因此此时加载用户态是安全的，彻底消除了『边初始化边跑用户态』的竞争窗口。
 *
 * 内部顺序（仍保留「生产者先就绪」同步）：
 *   1) 探测 AHCI/ATA，启动 disk-srv 内核线程；
 *   2) 自旋等 disk-srv 真正阻塞在 DISK_PORT（port_has_waiter）后再 spawn 依赖它的
 *      fs-server 并授权其向 DISK_PORT 发请求——避免消费者早于生产者就绪丢失唤醒；
 *   3) 依次 spawn input-server、shell。
 */
static void boot_late_init(void *arg)
{
    (void)arg;

    kprintf("[boot] late-init thread: kernel fully stable, loading services...\n");

    /* ============================================================
     * 驱动优先阶段（在任何 Ring3 服务之前）——由 device/driver manager 统一注册
     * 并 probe 内置驱动（ata/ahci/hda/e1000/uhci）。
     *
     * 关键：驱动探测/复位（如 UHCI 控制器与端口复位 20~100ms）一律用内核 msleep
     * 真睡眠让出 CPU（不再忙等）。msleep 需要真实可睡眠的任务上下文，本线程
     * （boot_late 内核任务）满足；kmain 阶段 current_task 是 idle0，不满足，故放这里。
     * ========================================================== */
    builtin_drivers_register();

    /* 进入用户态服务前关闭「内核诊断镜像到帧缓冲」：此后 [ipc]/[sched] 等
     * 运行期日志只走串口，帧缓冲专供 Ring3 shell/UI，避免按键时被刷屏。
     * 用户态输出经 sys_debug_write -> user_puts() 独立路径，不受影响。 */
    console_set_fb_diag(false);

    /* ============================================================
     * 磁盘（内核态特例）与 Ring3 FAT32 服务。
     *
     * 为什么要恢复这一段：完整 POSIX 文件系统调用（open/read/write/stat/
     * opendir/readdir…）全部经内核 VFS 层（kernel/fs/fd.c）以 Mach IPC 转发
     * 到用户态 FS_SERVER。不加载它，所有文件类 syscall 都会返回 -EIO，
     * POSIX 层形同虚设。
     *
     * 时序（「生产者先就绪」同步，缺此会丢唤醒）：
     *   1) 先探测 AHCI（中断驱动 DMA），失败再回退 ATA PIO；
     *   2) 启动 disk-srv 内核线程（DISK_PORT 的服务端）；
     *   3) 自旋等 disk-srv 真正阻塞在 DISK_PORT 上（port_has_waiter）后再
     *      spawn FS_SERVER 并授权其向 DISK_PORT 发请求——否则消费者先发、
     *      生产者尚未进入等待，该条请求会永久无人应答（历史故障）。
     * ========================================================== */
    /* 磁盘驱动（AHCI/ATA）已在「驱动优先阶段」由 device/driver manager 探测
     * 初始化；此处仅查询其结果并启动 DISK_PORT 服务端（disk-srv）。 */
    bool disk_ok = drivers_disk_ready();
    if (disk_ok)
    {
        disk_srv_start();
        /* 等 disk-srv 真正阻塞在 DISK_PORT（上限约 2 秒，绝不无限自旋：
         * 磁盘线程若因初始化失败未能进入等待，继续等待只会挂死引导）。 */
        for (uint32_t i = 0; i < 2000 && !port_has_waiter(DISK_PORT); i++)
        {
            task_yield();
        }
        task_t *fs_task = task_create_user(
            user_fs_server_start,
            (size_t)(user_fs_server_end - user_fs_server_start),
            "SukiFsServer");
        /* A2 项：仅 FS_SERVER 被授权向内核 DISK_PORT 发送磁盘请求 */
        if (fs_task)
        {
            port_grant_send(DISK_PORT, fs_task);
            kprintf("[boot] SukiFsServer spawned (pid=%lu), POSIX file syscalls "
                    "enabled\n",
                    (unsigned long)fs_task->id);
        }
        /* 初始化内核 VFS：注册挂载点（/->DISK, /tmp+/run->TMPFS, /dev->DEVFS）
         * 并初始化 tmpfs/devfs 后端。必须在 FS_SERVER 授权后调用，使 DISK 后端
         * 可用。 */
        extern void vfs_init(void);
        vfs_init();
        extern void vfs_selftest(void);
        vfs_selftest();

        /* 第三步：读取 registry 配置（/sys/configs/system.sre）并按配置门控 kdr。
         * 文件缺失/CRC 失败则回退默认，绝不阻塞启动。 */
        Stage3LoadConfigAndKdr();

        /* 记录【内核实际探测】到的启动设备：硬盘存在 -> '/' 由 FAT32（DISK 后端）
         * 服务。注册表 System/Boot/BootDeviceType 内只是标识，真实结果以探测为准，
         * 故此处覆盖该路径的读值（任何按路径读它的内核/Ring3 调用者都得到本结果）。 */
        g_boot_dev_type = "disk";
        RegistrySetOverrideString("System/Boot/BootDeviceType", g_boot_dev_type);
        kprintf("[boot] BootDeviceType: detected '%s' (kernel probe), / = FAT32 disk\n",
                g_boot_dev_type);

        if (g_kdr_enabled) {
            kdr_load_all();
        } else {
            kprintf("[boot] kdr loading disabled by registry config\n");
        }
        BootPlayAnimation();   /* 启动动画：图标 + 圆角进度条（registry 驱动） */
        bootanim_progress(30);
    }
    else
    {
        kprintf("[boot] no disk detected: FS_SERVER not started.\n");
        /* 仅光盘启动（脱离硬盘）：探测 ATAPI 光驱并挂载内核 ISO9660，
         * 使 '/' 由内核直接服务（含全部系统文件），Ring3 服务改从光盘读取。 */
        bool iso_ok = false;
        if (CdromInit() && IsoMount()) {
            iso_ok = true;
            kprintf("[boot] booting from ISO9660 on CD-ROM (read-only).\n");
        } else {
            kprintf("[boot] no CD-ROM/ISO either: POSIX file syscalls will "
                    "return -EIO.\n");
        }
        extern void vfs_init(void);
        vfs_init();
        extern void vfs_selftest(void);
        vfs_selftest();

        /* 第三步：从 ISO 读取 registry 配置并按配置门控 kdr（无盘也需读配置） */
        Stage3LoadConfigAndKdr();

        /* 实际探测结果：光盘挂载成功 -> '/' 由内核 ISO9660 服务。 */
        g_boot_dev_type = iso_ok ? "cdrom" : "none";
        RegistrySetOverrideString("System/Boot/BootDeviceType", g_boot_dev_type);
        kprintf("[boot] BootDeviceType: detected '%s' (kernel probe), / = %s\n",
                g_boot_dev_type, iso_ok ? "ISO9660 CD-ROM" : "(no filesystem)");

        if (g_kdr_enabled) {
            kdr_load_all();
        } else {
            kprintf("[boot] kdr loading disabled by registry config\n");
        }
        BootPlayAnimation();   /* 启动动画：图标 + 圆角进度条（registry 驱动） */
        bootanim_progress(30);
    }

    /* ---- registry 读取链路自检 ----
     * 校验「解析 -> 缓存 -> 按路径查询 -> 动态值覆盖」全链路（SYS_REGISTRY_READ
     * 走的正是这条路），并确证 System/Boot/BootDeviceType 返回的是内核探测结果。 */
    RegistrySelfTest();

    /* ============================================================
     * 显示服务最先拉起：映射帧缓冲 -> 声明 ready -> 阻塞等待「开机动画交接」。
     *
     * 语义（用户要求：开机动画要一直显示到「驱动 + 服务」全部初始化完毕）：
     *   1) 显示服务此刻只做「映射帧缓冲 + 画离屏背景层」，**不提交显存**，
     *      故不会覆盖内核正在显示的开机动画；随后调用 SYS_DISPLAY_READY，
     *      内核置 g_display_active=true、内核诊断改走环形管道（不再涂抹屏幕）。
     *   2) 紧接着它阻塞在 SYS_BOOT_SPLASH_WAIT 上；内核仍持有屏幕显示开机动画，
     *      并在其后陆续拉起网络/输入/鼠标等服务。
     *   3) 待全部服务就绪且 bootanim_finish() 收尾后，内核 bootanim_handoff()
     *      放行 —— 显示服务首次提交桌面帧并进入消息循环，屏幕交给桌面，
     *      随后 SukiLogon 启动 shell，进入用户登录/桌面流程。
     *
     * 启动屏障：必须等显示服务声明 ready（g_display_active 置位）再挂载后续
     * 服务。历史故障是 display-server 与 mouse-server/shell 并发 spawn，单核下
     * mouse-server 的紧凑轮询循环抢占 CPU，导致 display-server 迟迟到不了
     * SYS_DISPLAY_READY、谁都不写屏而表现为「内核卡死」。用 msleep 让出（而非
     * 空转 yield）在有界时间内等待，绝不无限自旋挂死引导。
     * ========================================================== */
    task_create_user(user_display_server_start,
                     (size_t)(user_display_server_end - user_display_server_start),
                     "SukiDisplayServer");
    bootanim_progress(40);   /* 里程碑：显示服务已拉起（此时尚未接管屏幕） */

    bool display_ready = false;
    uint32_t waited = 0;
    for (uint32_t i = 0; i < 2000; i++)
    {
        if (g_display_active)
        {
            display_ready = true;
            break;
        }
        waited = i;
        msleep(1);   /* wall-clock 有界（约 10ms/轮，上限约 20s） */
    }
    if (display_ready)
        kprintf("[boot] display-server ready (g_display_active=1, waited=%u rounds); "
                "boot screen still held, mounting remaining services...\n", waited);
    else
        kprintf("[boot] WARN: display-server did NOT become ready within "
                "timeout; continuing anyway (UI may be degraded)\n");

    /* ---- 网络：启动 NET_PORT 内核服务（e1000 原始帧收发）----
     * 放在磁盘/FS_SERVER 之后：网卡自检会发送 ARP 并轮询等待应答（最多 2 秒，
     * 全程 task_yield 让出），不应阻塞早期引导的关键路径。
     * 网络服务（lwIP）后续经 NET_PORT 收发帧，并需 port_grant_send 授权。 */
#if 1
    net_srv_start();
    bootanim_progress(45);   /* 里程碑：网卡/DISK 服务就绪 */

    /* Ring3 网络服务（lwIP 协议栈）：作为 NET_PORT 的【客户端】收发帧。
     * 须授权其向 NET_PORT 发送（net_srv_task 已在 e1000_init 把 NET_PORT 设为
     * 服务端）。net_server 自身会 sys_port_claim(NET_REPLY_PORT) 收应答。 */
    {
        task_t *net_task = task_create_user(user_net_server_start,
                (size_t)(user_net_server_end - user_net_server_start),
                "SukiNetServer");
        if (net_task) {
            port_grant_send(NET_PORT, net_task);
            kprintf("[boot] net-server spawned (pid=%lu), lwIP stack coming up\n",
                    (unsigned long)net_task->id);
        }
    }
    bootanim_progress(60);   /* 里程碑：Ring3 网络服务已拉起 */
#endif

    /* ---- Ring3 输入服务 + 显示服务（Shell 暂不启动）----
     * 用户明确要求：「显示服务启动时，shell 不应该启动」。
     * 因此本阶段只拉起 input-server 与 display-server；display-server
     * 完成桌面合成、进入消息循环前会调用 SYS_DISPLAY_READY，内核随即置
     * g_display_active=true、关闭 fbcon 对真实屏幕的写（改把内核诊断捕获进
     * 内核环形管道，供后续用户态 shell 经 SYS_CONSOLE_READ 读回，类似 dmesg）。
     * shell 的启动将作为后续独立里程碑，在显示层就绪后再接入。 */
    task_create_user(user_input_server_start,
                     (size_t)(user_input_server_end - user_input_server_start),
                     "SukiInputServer");
    bootanim_progress(70);   /* 里程碑：输入服务已拉起 */

    /* Ring3 鼠标驱动（.kdr 形态，待 kdr 加载器就绪后改由加载器动态装载）。
     * 经 SYS_MOUSE_READ 拉取内核 IRQ12 采集的鼠标包，把光标事件经 DISPLAY_PORT
     * 发给显示服务渲染。显示服务此刻已声明 ready（帧缓冲已映射），事件会先进入
     * 其端口队列，交接后立即被处理。 */
    task_create_user(user_mouse_server_start,
                     (size_t)(user_mouse_server_end - user_mouse_server_start),
                     "SukiMouseServer");
    bootanim_progress(78);   /* 里程碑：鼠标服务已拉起 */

    /* ============================================================
     * 开机动画收尾 + 交接（用户要求：全部初始化完成后才离开开机动画界面）。
     *   1) bootanim_finish()：保证启动画面至少可见 BOOTANIM_MIN_MS，并把进度
     *      平滑补到 100%。此刻驱动初始化、FS/网络/输入/鼠标/显示服务均已就绪；
     *   2) bootanim_handoff()：放行显示服务 —— 它从 SYS_BOOT_SPLASH_WAIT 返回，
     *      提交首帧桌面并进入消息循环，屏幕正式交给桌面。
     * ========================================================== */
    bootanim_finish();
    bootanim_handoff();

    /* 启动 SukiLogon（预留：登录管理器）。当前无用户/密码库，直接进入桌面
     * （SukiDesktopManager 以 SukiShell 作为桌面占位替身）。 */
    SukiLogon();

    /* ---- POSIX 一致性测试（Ring3，开机自检）----
     * 在 fs-server 之后启动：posixtest 内部会轮询等待 FS 就绪（wait_fs_ready），
     * 再跑文件类用例，故与 fs-server 的 mount 时序无关。
     * 它的输出是「完整 POSIX 系统调用层」的验收依据，用
     * `make run-headless QEMU_SERIAL="-serial file:/tmp/x.log"` 收集。 */
    /* POSIX 一致性测试（含本次新增的 pthread/clone/futex 多线程用例），开机自检。
     * 输出经串口落盘，用于 QEMU 无头回归判定（验证多线程零 panic + 计数精确）。 */
    task_t *sp1 = task_create_user(user_posixtest_start,
                     (size_t)(user_posixtest_end - user_posixtest_start),
                     "SukiPosixTest");
    kprintf("[boot-dbg] posixtest spawn ret=%p\n", (void *)sp1);

    /* 网络子系统端到端验证：spawn nettest（UDP -> QEMU TFTP 10.0.2.2:69 往返 +
     * SukiNative 原生 socket 冒烟）。与 posixtest 同为开机自检，输出经串口落盘。 */
    task_t *sp2 = task_create_user(user_nettest_start,
                     (size_t)(user_nettest_end - user_nettest_start),
                     "SukiNetTest");
    kprintf("[boot-dbg] nettest spawn ret=%p\n", (void *)sp2);

    /* libcurl 移植端到端验证：spawn curl_test（libcurl easy GET/HTTPS GET）。
     * 输出经串口落盘，作为「libcurl 真实抓取」的验收依据。 */
    task_t *sp2c = task_create_user(user_curl_test_start,
                     (size_t)(user_curl_test_end - user_curl_test_start),
                     "SukiCurlTest");
    kprintf("[boot-dbg] curl_test spawn ret=%p\n", (void *)sp2c);

    /* curl 命令行应用验证：spawn 一个极小包装器，它 execve 磁盘上的
     * ::BIN/CURL.SKA（与 shell 装载同一条路径）并传入真实命令行参数。 */
    task_t *sp2d = task_create_user(user_curl_app_test_start,
                     (size_t)(user_curl_app_test_end - user_curl_app_test_start),
                     "SukiCurlAppTest");
    kprintf("[boot-dbg] curl_app_test spawn ret=%p\n", (void *)sp2d);

    /* 窗口系统端到端自检：spawn winhello（libsuki_gui 创建窗口 + OOL 零拷贝合成）。
     * 验证「应用 -> WM_PORT -> 显示服务合成 -> 帧缓冲」全链路，输出经串口落盘。 */
    task_t *sp4 = task_create_user(user_winhello_start,
                     (size_t)(user_winhello_end - user_winhello_start),
                     "SukiWinHello");
    kprintf("[boot-dbg] winhello spawn ret=%p\n", (void *)sp4);

    /* libsui 控件库端到端自检：spawn suikitest（libsui 创建窗口 + 控件 + OOL 合成）。
     * 验证「应用 -> WM_PORT -> 显示服务增量合成 -> 帧缓冲」全链路，输出经串口落盘。 */
    task_t *sp5 = task_create_user(user_suikitest_start,
                    (size_t)(user_suikitest_end - user_suikitest_start),
                    "SukiSuiTest");
    kprintf("[boot-dbg] suikitest spawn ret=%p\n", (void *)sp5);

    /* 动态链接验证：spawn dltest（运行期 dlopen("/LIB/libtest.sl") + dlsym）。
     * 验证内核 elf.c 的 ET_DYN 模块加载/重定位/符号解析（dlopen 路径）。
     * 注：dltest 内嵌 blob 体积较大，load_elf 偶发长耗时；故置于 winhello/suikitest
     * 之后，确保 GUI 自检先完成、不阻塞窗口链路验证。 */
    task_t *sp3 = task_create_user(user_dltest_start,
                     (size_t)(user_dltest_end - user_dltest_start),
                     "SukiDlTest");
    kprintf("[boot-dbg] dltest spawn ret=%p\n", (void *)sp3);

    /* 注：Rust 工具链开机自检（原 RUSTHELLO / STDTEST 内嵌 blob spawn）已按
     * 用户决策整体移除——不再向内核注入任何 Rust ELF；内核启动与 GUI/WM 自检
     * 由 winhello 等 C 程序承担，保持零 Rust 依赖、可稳定启动。 */

    kprintf("[boot] core services spawned (input+display); shell deferred "
            "until display layer ready.\n\n");

    /* 本线程使命完成，退出（zombie 由调度器回收）。 */
    task_exit_current(0);
}
