/*
 * kernel/driver/builtin_drivers.c
 * -----------------------------------------------------------------------------
 * 内置（内核内建）PCI 驱动向「设备管理器 / 驱动管理器」的统一登记与绑定。
 *
 * 背景（本轮架构调整）：
 *   此前 ATA/AHCI、e1000、HDA、UHCI(USB) 等驱动是在 kmain / boot_late 里【直接】、
 *   分散地调用各自的 *_init()，绕过了 device_manager/driver_manager。这导致：
 *     1) 驱动初始化时机随调用点而定，USB 主机栈与显示/输入服务并发启动，存在
 *        “USB 枚举与 display-server 初始化互相拖慢”的竞争窗口（表现为某些时序下
 *        display-server 迟迟不就绪 → 只有窗口外框/无窗口）；
 *     2) 驱动本身不可被管理器统一枚举/匹配/绑定，不符合“所有驱动由设备管理器与
 *        驱动管理器管理”的架构要求。
 *
 * 本模块把每个内置驱动封装为一个 driver_t：
 *   - match()  按 PCI class/subclass 判定“本驱动能驱动哪些设备”；
 *   - probe()  真正执行硬件初始化（复用既有 *_init()，首次成功后幂等）；
 *   在【内核核心子系统（调度/IPC/POSIX）就绪之后、任何 Ring3 服务创建之前】由
 *   kmain 调用 builtin_drivers_register()，从而：
 *     - 所有驱动【先于】系统后续流程（console/网络/磁盘服务、显示、输入、shell）
 *       完成初始化；
 *     - 驱动注册即触发 driver_manager 的「双向匹配」，逐设备 probe，完全由
 *       设备/驱动管理器统一调度。
 *
 * 注意：match/probe 与 device_t/bus_type_t 均为 sukios/kdr.h 中与 .kdr 模块共享
 * 的 ABI（结构体布局一致），故内置驱动与可加载 .kdr 模块走同一套模型。
 */
#include <kernel/types.h>
#include <kernel/console.h>
#include <sukios/kdr.h>
#include <kernel/pci.h>

/* 各内置驱动初始化入口（定义于对应驱动文件，见 include/kernel/{usb,hda,e1000}.h
 * 与 kernel/drivers/{ata,ahci}.c）。统一返回 bool：true=探测并初始化成功。 */
extern bool ata_init(void);
extern bool ahci_init(void);
extern bool hda_init(void);
extern bool e1000_init(void);
#if CONFIG_DRIVER_USB
extern bool usb_init(void);
#endif

/* ---- 匹配助手：按 PCI class/subclass（class 0x01=存储、0x02=网络、0x04=多媒体、
 * 0x0C=串行总线，见 osdev PCI 类代码表）---- */
static bool match_pci_class(const device_t *d, uint8_t cls, uint8_t sub)
{
    return d && d->bus == BUS_PCI
        && d->class_code == cls && d->subclass == sub;
}

/* ---- 幂等状态：设备管理器对一个驱动可能匹配到多个同类设备而多次 probe，
 * 每类驱动只应真正初始化硬件一次（一套驱动服务同型号全部控制器）。 ---- */
static bool g_ata_tried   = false, g_ata_ok   = false;
static bool g_ahci_tried  = false, g_ahci_ok  = false;
static bool g_hda_tried   = false, g_hda_ok   = false;
static bool g_e1000_tried = false, g_e1000_ok = false;
static bool g_usb_tried   = false, g_usb_ok   = false;

/* 供 boot_late 判定“磁盘子系统是否已就绪”（替代原先直接看 ata_init() 返回值）。 */
static bool g_disk_ok = false;

/* ---- ATA（PIIX 传统 IDE，class 01/01）---- */
static bool match_ata(const device_t *d) { return match_pci_class(d, 0x01, 0x01); }
static int  probe_ata(device_t *d)
{
    (void)d;
    if (!g_ata_tried) { g_ata_tried = true; g_ata_ok = ata_init(); if (g_ata_ok) g_disk_ok = true; }
    return g_ata_ok ? 0 : -1;
}

/* ---- AHCI（SATA，class 01/06）---- */
static bool match_ahci(const device_t *d) { return match_pci_class(d, 0x01, 0x06); }
static int  probe_ahci(device_t *d)
{
    (void)d;
    if (!g_ahci_tried) { g_ahci_tried = true; g_ahci_ok = ahci_init(); if (g_ahci_ok) g_disk_ok = true; }
    return g_ahci_ok ? 0 : -1;
}

/* ---- Intel HDA 音频（class 04/03）---- */
static bool match_hda(const device_t *d) { return match_pci_class(d, 0x04, 0x03); }
static int  probe_hda(device_t *d)
{
    (void)d;
    if (!g_hda_tried) { g_hda_tried = true; g_hda_ok = hda_init(); }
    return g_hda_ok ? 0 : -1;
}

/* ---- Intel 8254x 网卡（class 02/00 = 以太网控制器）---- */
static bool match_e1000(const device_t *d) { return match_pci_class(d, 0x02, 0x00); }
static int  probe_e1000(device_t *d)
{
    (void)d;
    if (!g_e1000_tried) { g_e1000_tried = true; g_e1000_ok = e1000_init(); }
    return g_e1000_ok ? 0 : -1;
}

/* ---- UHCI 主机控制器（USB，class 0C/03）---- */
#if CONFIG_DRIVER_USB
static bool match_uhci(const device_t *d) { return match_pci_class(d, 0x0C, 0x03); }
static int  probe_uhci(device_t *d)
{
    (void)d;
    if (!g_usb_tried) { g_usb_tried = true; g_usb_ok = usb_init(); }
    return g_usb_ok ? 0 : -1;
}
#endif

/* 驱动单例（ABI 与 .kdr 模块共享） */
static driver_t drv_ata   = { .name = "ata-ide",  .match = match_ata,   .probe = probe_ata   };
static driver_t drv_ahci  = { .name = "ahci",     .match = match_ahci,  .probe = probe_ahci  };
static driver_t drv_hda   = { .name = "intel-hda",.match = match_hda,   .probe = probe_hda   };
static driver_t drv_e1000 = { .name = "e1000",    .match = match_e1000, .probe = probe_e1000 };
#if CONFIG_DRIVER_USB
static driver_t drv_uhci  = { .name = "uhci-usb", .match = match_uhci,  .probe = probe_uhci  };
#endif

/*
 * 注册全部内置驱动。每调用一次 driver_register() 即触发 driver_manager 的
 * 「双向匹配」：把该驱动绑定到所有已注册且未绑定的设备，并对其 probe() 一次。
 * 由于 kmain 在调用本函数前已完成 device_manager_scan_pci()（设备已入册）与
 * 调度/IPC/POSIX 初始化，故此刻 probe 可安全执行硬件初始化（含创建内核服务任务）。
 *
 * 返回：成功 probe 的驱动数量（0..5），供引导日志判定。
 */
int builtin_drivers_register(void)
{
    kprintf("[driver] registering built-in PCI drivers "
            "(ata/ahci/hda/e1000/uhci)...\n");
    driver_register(&drv_ata);
    driver_register(&drv_ahci);
    driver_register(&drv_hda);
    driver_register(&drv_e1000);
#if CONFIG_DRIVER_USB
    driver_register(&drv_uhci);
#endif

    int n = 0;
    if (g_disk_ok)   n++;
    if (g_hda_ok)    n++;
    if (g_e1000_ok)  n++;
    if (g_usb_ok)    n++;
    kprintf("[driver] built-in drivers probed: disk=%d hda=%d e1000=%d usb=%d\n",
            g_disk_ok ? 1 : 0, g_hda_ok ? 1 : 0, g_e1000_ok ? 1 : 0, g_usb_ok ? 1 : 0);
    return n;
}

bool drivers_disk_ready(void) { return g_disk_ok; }
bool drivers_usb_ready(void)  { return g_usb_ok; }
