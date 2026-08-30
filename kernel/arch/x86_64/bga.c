/*
 * kernel/arch/x86_64/bga.c
 * -----------------------------------------------------------------------------
 * Bochs VBE / Virtio 标准图形适配器（BGA）驱动，用于把帧缓冲设置到任意分辨率。
 *
 * 依据：osdev_wiki/wiki.osdev.org/Bochs_VBE_extensions
 *
 * 关键事实（来自 OSDev 文档）：
 *   - QEMU `-vga std`（即 `-device VGA`，PCI 设备 0x1234:0x1111）实现的就是
 *     Bochs Graphics Adaptor (BGA)。QEMU 默认出厂分辨率是 1024x768，且 GRUB/
 *     Multiboot2 无法把模式设到 1024x768 之外（这是 GRUB 限制，不是硬件限制）。
 *   - 但 BGA 本身**支持任意 X*Y（X 必须是 8 的倍数）分辨率 + 32bpp 线性帧缓冲**，
 *     只需通过 IO 端口编程，无需 real-mode trampoline。
 *   - 控制寄存器：
 *       INDEX 端口 0x01CE（写索引）/ 0x01CF（读+写数据，32 位）
 *       VBE_DISPI_INDEX_ID     0   -> 写入 0xB0C5 探测/使能 BGA
 *       VBE_DISPI_INDEX_XRES   1
 *       VBE_DISPI_INDEX_YRES   2
 *       VBE_DISPI_INDEX_BPP    3   -> 32 表示 32bpp
 *       VBE_DISPI_INDEX_ENABLE 4   -> bit0 使能，bit1 使能 LFB（线性帧缓冲）
 *       VBE_DISPI_INDEX_VIRT_W 5
 *       VBE_DISPI_INDEX_VIRT_H 6
 *   - 线性帧缓冲物理地址来自 PCI BAR0（设备 0x1234:0x1111）。
 *
 * 本驱动在 fb_init() 内被调用，把屏幕设到 display.cfg 声明的 1280x720@32bpp。
 * 若设置失败（设备不存在/不支持），fb_init 回退到 Multiboot2 给的分辨率。
 */
#include <kernel/types.h>
#include <kernel/io.h>
#include <kernel/console.h>
#include <kernel/pci.h>
#include <kernel/framebuffer.h>

/* ---- BGA 寄存器（见文件头注释）---- */
#define BGA_INDEX_PORT  0x01CE
#define BGA_DATA_PORT   0x01CF

#define VBE_DISPI_INDEX_ID     0
#define VBE_DISPI_INDEX_XRES   1
#define VBE_DISPI_INDEX_YRES   2
#define VBE_DISPI_INDEX_BPP    3
#define VBE_DISPI_INDEX_ENABLE 4
#define VBE_DISPI_INDEX_VIRT_W 5
#define VBE_DISPI_INDEX_VIRT_H 6

#define VBE_DISPI_ID_MAGIC     0xB0C5   /* 写入即确认支持并锁定版本 */
#define VBE_DISPI_BPP_32       32
#define VBE_DISPI_ENABLED       0x01
#define VBE_DISPI_LFB_ENABLED  0x40

/* BGA PCI 身份（QEMU `-vga std` / `-device VGA`） */
#define BGA_PCI_VENDOR  0x1234
#define BGA_PCI_DEVICE  0x1111

/* 写入 BGA 索引/数据寄存器。封装为独立 noinline 函数便于注释 clobber。 */
static __attribute__((noinline)) void bga_write(uint16_t index, uint16_t value)
{
    /* 两条 outw 指令：先写索引端口，再写数据端口。无内存副作用，
     * clobber 仅 "memory" 以阻止编译器乱序（硬件寄存器是易失的）。 */
    outw(BGA_INDEX_PORT, index);
    outw(BGA_DATA_PORT, value);
}

static __attribute__((noinline)) uint16_t bga_read(uint16_t index)
{
    outw(BGA_INDEX_PORT, index);
    return inw(BGA_DATA_PORT);
}

/*
 * bga_set_mode —— 把 BGA 设到指定分辨率/色深（32bpp LFB）。
 * 返回 true 表示设置成功（读回寄存器已确认）。
 *
 * 步骤（OSDev 规范）：
 *   1) 写 ID 0xB0C5 使能 BGA 扩展；
 *   2) 写 XRES/YRES/BPP；
 *   3) 写虚拟分辨率（与显示分辨率一致，避免线性地址错位）；
 *   4) 写 ENABLE = ENABLED | LFB_ENABLED；
 *   5) 读回 XRES/YRES 验证是否与请求一致。
 *
 * 注意：BGA 要求 X 为 8 的倍数；1280 满足。
 */
static bool bga_set_mode(uint32_t width, uint32_t height)
{
    /* 1) 使能 BGA 扩展（锁定版本） */
    bga_write(VBE_DISPI_INDEX_ID, VBE_DISPI_ID_MAGIC);
    if (bga_read(VBE_DISPI_INDEX_ID) != VBE_DISPI_ID_MAGIC) {
        kprintf("[bga] probe failed: device did not accept magic ID 0xB0C5\n");
        return false;
    }

    /* 2) 分辨率 + 色深 */
    bga_write(VBE_DISPI_INDEX_XRES, (uint16_t)width);
    bga_write(VBE_DISPI_INDEX_YRES, (uint16_t)height);
    bga_write(VBE_DISPI_INDEX_BPP,  VBE_DISPI_BPP_32);

    /* 3) 虚拟分辨率（与显示分辨率一致，OSDev 推荐做法） */
    bga_write(VBE_DISPI_INDEX_VIRT_W, (uint16_t)width);
    bga_write(VBE_DISPI_INDEX_VIRT_H, (uint16_t)height);

    /* 4) 使能（线性帧缓冲模式） */
    bga_write(VBE_DISPI_INDEX_ENABLE,
              (uint16_t)(VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED));

    /* 5) 读回验证 */
    uint16_t rx = bga_read(VBE_DISPI_INDEX_XRES);
    uint16_t ry = bga_read(VBE_DISPI_INDEX_YRES);
    uint8_t  rb = (uint8_t)bga_read(VBE_DISPI_INDEX_BPP);
    bool ok = (rx == (uint16_t)width) && (ry == (uint16_t)height)
              && (rb == VBE_DISPI_BPP_32);
    kprintf("[bga] set %ux%u@32: readback %ux%u@%u %s\n",
            (unsigned)width, (unsigned)height,
            (unsigned)rx, (unsigned)ry, (unsigned)rb,
            ok ? "OK" : "MISMATCH");
    return ok;
}

/*
 * bga_locate_and_set —— 找到 BGA 设备，读 LFB 物理地址，并设置到目标分辨率。
 * 成功时把 *out_phys 设为 BAR0（LFB 物理基址），返回 true；失败返回 false。
 *
 * 兼容性说明：本驱动**显式**扫描所有 (bus,dev,func)，不再依赖
 * pci_find_class（BGA 的 class=0x03/0x00 即可，但按厂商设备号找最精确、
 * 且避免与 virtio-gpu 等设备混淆）。
 */
bool bga_locate_and_set(uint32_t width, uint32_t height, uint64_t *out_phys)
{
    if (out_phys) *out_phys = 0;

    /* 扫描全部 PCI 总线/设备/功能（QEMU 下 BGA 在 bus0） */
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint16_t vid = pci_cfg_read16((uint8_t)bus, dev, 0,
                                          PCI_CFG_VENDOR_ID);
            if (vid == 0xFFFF) {
                continue;                       /* 该设备槽为空 */
            }
            uint8_t nfunc = (pci_cfg_read8((uint8_t)bus, dev, 0,
                                           PCI_CFG_HEADER_TYPE) & 0x80)
                                ? 8 : 1;
            for (uint8_t fn = 0; fn < nfunc; fn++) {
                uint16_t v = pci_cfg_read16((uint8_t)bus, dev, fn,
                                            PCI_CFG_VENDOR_ID);
                uint16_t d = pci_cfg_read16((uint8_t)bus, dev, fn,
                                            PCI_CFG_DEVICE_ID);
                if (v == BGA_PCI_VENDOR && d == BGA_PCI_DEVICE) {
                    kprintf("[bga] found Bochs VGA at %02x:%02x.%u\n",
                            (unsigned)bus, (unsigned)dev, (unsigned)fn);

                    pci_dev_t dev_loc = {
                        .bus = (uint8_t)bus,
                        .dev = dev,
                        .func = fn,
                        .vendor_id = v,
                        .device_id = d,
                    };

                    /* 使能 MMIO 空间（BAR0 是 32 位 MMIO LFB） */
                    pci_enable_device(&dev_loc);

                    uint64_t bar0 = pci_bar_addr(&dev_loc, 0);
                    if (bar0 == 0) {
                        kprintf("[bga] BAR0 invalid, cannot map LFB\n");
                        return false;
                    }
                    if (out_phys) *out_phys = bar0;

                    /* 编程分辨率；成功与否均返回（失败由 fb_init 回退） */
                    bool ok = bga_set_mode(width, height);
                    return ok;
                }
            }
        }
    }

    kprintf("[bga] Bochs VGA (0x1234:0x1111) not found on PCI bus\n");
    return false;
}
