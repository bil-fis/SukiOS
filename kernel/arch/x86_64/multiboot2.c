/*
 * kernel/arch/x86_64/multiboot2.c
 * -----------------------------------------------------------------------------
 * 解析 GRUB 传入的 Multiboot2 info 结构，提取帧缓冲与内存映射信息。
 *
 * 调用关系：kmain() -> multiboot2_parse() -> {framebuffer_init, pmm_init}。
 *
 * 内存访问：MBI 位于低物理地址，通过 PHYS_TO_VIRT 经高半区恒等偏移访问
 * （引导页表已映射 0..4GB）。
 */
#include <kernel/multiboot2.h>
#include <kernel/serial.h>

bool multiboot2_parse(uint64_t mbi_phys, boot_info_t *out)
{
    for (size_t i = 0; i < sizeof(boot_info_t); i++) {
        ((uint8_t *)out)[i] = 0;
    }

    const uint8_t *base = (const uint8_t *)PHYS_TO_VIRT(mbi_phys);

    /* 头部：total_size(u32), reserved(u32) */
    uint32_t total_size = *(const uint32_t *)base;
    if (total_size < 8 || total_size > (16 * 1024 * 1024)) {
        return false;   /* 明显不合理，拒绝 */
    }

    const uint8_t *ptr = base + 8;
    const uint8_t *end = base + total_size;

    while (ptr < end) {
        const struct mb2_tag *tag = (const struct mb2_tag *)ptr;

        if (tag->type == MULTIBOOT_TAG_TYPE_END) {
            break;
        }

        switch (tag->type) {
        case MULTIBOOT_TAG_TYPE_FRAMEBUFFER: {
            const struct mb2_tag_framebuffer *fb =
                (const struct mb2_tag_framebuffer *)tag;
            out->fb_present = true;
            out->fb_addr   = fb->framebuffer_addr;
            out->fb_pitch  = fb->framebuffer_pitch;
            out->fb_width  = fb->framebuffer_width;
            out->fb_height = fb->framebuffer_height;
            out->fb_bpp    = fb->framebuffer_bpp;
            break;
        }
        case MULTIBOOT_TAG_TYPE_MMAP: {
            const struct mb2_tag_mmap *mm = (const struct mb2_tag_mmap *)tag;
            out->mmap = mm;
            uint32_t esz = mm->entry_size;
            const uint8_t *e = (const uint8_t *)mm->entries;
            const uint8_t *me = (const uint8_t *)mm + mm->size;
            while (e + esz <= me) {
                const struct mb2_mmap_entry *ent =
                    (const struct mb2_mmap_entry *)e;
                if (ent->type == 1) {   /* 可用 RAM */
                    out->mem_total += ent->len;
                    uint64_t top = ent->addr + ent->len;
                    if (top > out->mem_highest) {
                        out->mem_highest = top;
                    }
                }
                e += esz;
            }
            break;
        }
        default:
            break;
        }

        /* 下一个标签，按 8 字节对齐 */
        ptr += (tag->size + 7) & ~((uint64_t)7);
    }

    return true;
}
