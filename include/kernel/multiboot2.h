/*
 * include/kernel/multiboot2.h
 * -----------------------------------------------------------------------------
 * Multiboot2 info 结构解析（子集）。解析帧缓冲标签与内存映射标签。
 * 规范：https://www.gnu.org/software/grub/manual/multiboot2/
 */
#ifndef _SUKI_KERNEL_MULTIBOOT2_H
#define _SUKI_KERNEL_MULTIBOOT2_H

#include <kernel/types.h>

#define MULTIBOOT2_MAGIC              0x36D76289

#define MULTIBOOT_TAG_TYPE_END        0
#define MULTIBOOT_TAG_TYPE_MMAP       6
#define MULTIBOOT_TAG_TYPE_FRAMEBUFFER 8
#define MULTIBOOT_TAG_TYPE_MODULE     3    /* 引导模块（如 /boot/display.cfg） */
#define MULTIBOOT_TAG_TYPE_COMMAND_LINE 1  /* 内核命令行（GRUB 启动参数） */
/* P0-6：UEFI 启动路径新增标签 */
#define MULTIBOOT_TAG_TYPE_EFI64      12   /* EFI 64 位系统表指针（UEFI 启动标识） */
#define MULTIBOOT_TAG_TYPE_ACPI_OLD   14   /* ACPI 1.0 RSDP 副本（20 字节） */
#define MULTIBOOT_TAG_TYPE_ACPI_NEW   15   /* ACPI 2.0+ RSDP 副本（36 字节） */

/* 通用标签头 */
struct mb2_tag {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

/* type = 8: 帧缓冲信息 */
struct mb2_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;   /* 每行字节数 */
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;     /* 每像素位数 */
    uint8_t  framebuffer_type;    /* 1 = 直接 RGB */
    uint16_t reserved;
} __attribute__((packed));

/* type = 6: 内存映射 */
struct mb2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;                /* 1 = 可用 RAM */
    uint32_t zero;
} __attribute__((packed));

struct mb2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct mb2_mmap_entry entries[0];
} __attribute__((packed));

/* type = 3: 引导模块（GRUB module2 加载的配置/资源文件）。
 * mod_start/mod_end 为物理地址区间 [start, end)，其后紧跟以 NUL 结尾的模块名。
 * 首个模块约定为 display.cfg（显示服务配置）。 */
struct mb2_tag_module {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;           /* 模块数据物理地址 */
    uint32_t mod_end;             /* 模块数据结束（不含） */
    char     cmdline[0];          /* 模块名（含 NUL，可变长） */
} __attribute__((packed));

/* 解析结果：供 kmain 及后续 PMM/framebuffer 使用 */
typedef struct boot_info {
    /* 帧缓冲 */
    bool     fb_present;
    uint64_t fb_addr;
    uint32_t fb_pitch;
    uint32_t fb_width;
    uint32_t fb_height;
    uint8_t  fb_bpp;
    /* 内存 */
    uint64_t mem_total;           /* 可用 RAM 总字节数 */
    uint64_t mem_highest;         /* 最高可用物理地址 */
    /* 内存映射标签指针（虚拟地址），供 PMM 遍历 */
    const struct mb2_tag_mmap *mmap;
    /* ---- P0-6：UEFI 启动路径 ----
     * GRUB 会把固件 RSDP 复制进 MBI（tag 14/15）。UEFI 机器上 RSDP 位于
     * EFI 配置表指向的任意物理页，不在 EBDA/0xE0000 传统扫描区——acpi_init
     * 必须优先使用这里的副本地址。 */
    bool     efi_boot;            /* MBI 含 EFI64 系统表标签 => UEFI 启动 */
    uint64_t rsdp_copy_phys;      /* MBI 内 RSDP 副本的物理地址（0=无） */
    /* ---- 引导配置模块 ----
     * GRUB 经 module2 加载的配置文件（约定首个模块为 configs/display.cfg）。
     * 内核在 fb_init 前解析之，决定显示模式与分辨率。物理地址区间
     * [cfg_phys, cfg_phys + cfg_size)，内容为以 NUL 结尾的纯文本。 */
    uint64_t cfg_phys;            /* 配置模块物理地址（0=无） */
    uint32_t cfg_size;            /* 配置模块字节数（含 NUL，0=无） */

    /* 内核命令行（GRUB 启动参数，如 "-v --verbose"）。由 tag 1 填充；PVH 下为空。 */
    char     cmdline[256];

    /* 引导模块表：GRUB module2 加载的全部模块（.kdr 内核驱动 + 配置文件）。
     * kdr 加载器遍历本表，凡 is_kdr 者加载为内核模块。 */
#define BOOT_MOD_MAX 16
    struct boot_module {
        uint64_t phys;           /* 模块数据物理地址 */
        uint32_t size;           /* 模块字节数 */
        char     name[64];        /* 模块名（来自 cmdline，截断至 63 字符） */
        bool     is_kdr;         /* 是否为 .kdr 内核驱动模块 */
    } mods[BOOT_MOD_MAX];
    int nmods;
} boot_info_t;

/* 全局引导信息（由 kmain.c 持有，kdr 加载器共享访问） */
extern boot_info_t g_boot;

/* 解析物理地址处的 Multiboot2 info，填充 out。返回 true 成功。 */
bool multiboot2_parse(uint64_t mbi_phys, boot_info_t *out);

/* 统一引导信息入口：magic==MULTIBOOT2_MAGIC 走 multiboot2，否则走 PVH
 * （QEMU -kernel 直接加载，GRUB 不可用时）。返回 true 成功。 */
bool bootinfo_prepare(uint64_t magic, uint64_t info_phys, boot_info_t *out);

#endif /* _SUKI_KERNEL_MULTIBOOT2_H */
