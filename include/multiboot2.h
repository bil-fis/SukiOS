/* =============================================================================
 * SukiOS - Multiboot2 信息结构定义
 * 参考：Multiboot2 规范 2.0 (https://www.gnu.org/software/grub/manual/multiboot2/)
 * ============================================================================= */

#ifndef SUKIOS_MULTIBOOT2_H
#define SUKIOS_MULTIBOOT2_H

#include <stdint.h>

/* ---- Multiboot2 魔数 ---- */
#define MULTIBOOT2_HEADER_MAGIC    0xE85250D6   /* 头部魔数 */
#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36D76289  /* 引导加载器魔数（EAX） */

/* ---- 架构类型 ---- */
#define MULTIBOOT2_ARCH_I386      0             /* i386（兼容 x86_64） */

/* ---- 头部标签类型 ---- */
#define MULTIBOOT2_HEADER_TAG_END              0   /* 结束标签 */
#define MULTIBOOT2_HEADER_TAG_REQUEST          1   /* 信息请求 */
#define MULTIBOOT2_HEADER_TAG_ADDRESS          2   /* 地址 */
#define MULTIBOOT2_HEADER_TAG_ENTRY_ADDRESS    3   /* 入口地址 */
#define MULTIBOOT2_HEADER_TAG_CONSOLE_FLAGS    4   /* 控制台标志 */
#define MULTIBOOT2_HEADER_TAG_FRAMEBUFFER      5   /* 帧缓冲区请求 */
#define MULTIBOOT2_HEADER_TAG_MODULE_ALIGN     6   /* 模块对齐 */

/* ---- 信息结构标签类型 ---- */
#define MULTIBOOT2_TAG_END              0   /* 结束标签 */
#define MULTIBOOT2_TAG_CMDLINE          1   /* 命令行 */
#define MULTIBOOT2_TAG_BOOT_LOADER_NAME 2   /* 引导加载器名称 */
#define MULTIBOOT2_TAG_MODULE           3   /* 启动模块 */
#define MULTIBOOT2_TAG_BASIC_MEMINFO    4   /* 基本内存信息 */
#define MULTIBOOT2_TAG_BOOTDEV          5   /* BIOS 启动设备 */
#define MULTIBOOT2_TAG_MMAP             6   /* 内存映射 */
#define MULTIBOOT2_TAG_VBE              7   /* VBE 信息 */
#define MULTIBOOT2_TAG_FRAMEBUFFER      8   /* 帧缓冲区信息 */
#define MULTIBOOT2_TAG_ELF_SECTIONS     9   /* ELF 段信息 */
#define MULTIBOOT2_TAG_APM              10  /* APM 信息 */
#define MULTIBOOT2_TAG_EFI32            11  /* EFI 32 位系统表 */
#define MULTIBOOT2_TAG_EFI64            12  /* EFI 64 位系统表 */

/* ---- 内存类型（用于内存映射标签）---- */
#define MULTIBOOT_MEMORY_AVAILABLE      1   /* 可用 RAM */
#define MULTIBOOT_MEMORY_RESERVED       2   /* 保留区域 */
#define MULTIBOOT_MEMORY_ACPI           3   /* ACPI 可回收 */
#define MULTIBOOT_MEMORY_NVS            4   /* ACPI NVS（需保留） */
#define MULTIBOOT_MEMORY_BADRAM         5   /* 损坏的 RAM */

/* ---- 基本标签结构（信息结构中所有标签的头部）---- */
struct multiboot2_tag {
    uint32_t type;                     /* 标签类型 */
    uint32_t size;                     /* 标签大小（含头部） */
};

/* ---- 命令行 / 引导加载器名称标签（type=1,2）---- */
struct multiboot2_tag_string {
    uint32_t type;
    uint32_t size;
    char     string[];                 /* 以 '\0' 结尾的字符串 */
};

/* ---- 基本内存信息标签（type=4）---- */
struct multiboot2_tag_basic_meminfo {
    uint32_t type;
    uint32_t size;
    uint32_t mem_lower;               /* 低端内存大小（KB），从 0 开始 */
    uint32_t mem_upper;               /* 高端内存大小（KB），从 1MB 开始 */
};

/* ---- 内存映射条目 ---- */
struct multiboot2_mmap_entry {
    uint64_t addr;                    /* 基地址 */
    uint64_t len;                     /* 长度 */
    uint32_t type;                    /* 内存类型 */
    uint32_t zero;                    /* 保留，置零 */
};

/* ---- 内存映射标签（type=6）---- */
struct multiboot2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;              /* 单条目大小 */
    uint32_t entry_version;           /* 条目版本（当前为 0） */
    struct multiboot2_mmap_entry entries[];
};

/* ---- 帧缓冲区信息标签（type=8）公共部分 ---- */
struct multiboot2_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;        /* 帧缓冲区物理地址 */
    uint32_t framebuffer_pitch;       /* 每行字节数 */
    uint32_t framebuffer_width;       /* 宽度（像素） */
    uint32_t framebuffer_height;      /* 高度（像素） */
    uint8_t  framebuffer_bpp;         /* 每像素位数 */
    uint8_t  framebuffer_type;        /* 类型：0=索引色，1=RGB，2=EGA文本 */
    uint16_t reserved;                /* 保留 */
    /* 后续为颜色信息（根据 framebuffer_type 不同而不同） */
};

#define MULTIBOOT_FRAMEBUFFER_TYPE_INDEXED  0
#define MULTIBOOT_FRAMEBUFFER_TYPE_RGB      1
#define MULTIBOOT_FRAMEBUFFER_TYPE_EGA_TEXT 2

#endif /* SUKIOS_MULTIBOOT2_H */
