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
} boot_info_t;

/* 解析物理地址处的 Multiboot2 info，填充 out。返回 true 成功。 */
bool multiboot2_parse(uint64_t mbi_phys, boot_info_t *out);

#endif /* _SUKI_KERNEL_MULTIBOOT2_H */
