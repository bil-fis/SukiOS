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

/* 前向声明：PVH 引导信息解析（GRUB 不可用时 QEMU -kernel 走此路径） */
void pvh_parse(uint64_t hvm_phys, boot_info_t *out);

/*
 * bootinfo_prepare: 引导信息统一入口。
 *   magic == MULTIBOOT2_MAGIC -> GRUB multiboot2 路径
 *   否则                      -> 假定 PVH（QEMU -kernel 直接加载）
 * 返回 true 表示成功解析；false 表示无法识别的引导协议。
 */
bool bootinfo_prepare(uint64_t magic, uint64_t info_phys, boot_info_t *out)
{
    if (magic == MULTIBOOT2_MAGIC) {
        return multiboot2_parse(info_phys, out);
    }
    /* PVH：info_phys 为 hvm_start_info 物理地址，EAX 非 multiboot2 magic */
    pvh_parse(info_phys, out);
    return true;
}

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
        case MULTIBOOT_TAG_TYPE_EFI64:
            /* P0-6：存在 EFI64 系统表标签即 UEFI 启动（GRUB-EFI 注入）。
             * 系统表指针本身暂不使用（Boot Services 已被 GRUB ExitBootServices
             * 终结，Runtime Services 需虚拟地址重映射，P1 再接）。 */
            out->efi_boot = true;
            break;
        case MULTIBOOT_TAG_TYPE_ACPI_OLD:
        case MULTIBOOT_TAG_TYPE_ACPI_NEW: {
            /* P0-6：GRUB 把固件 RSDP 整体复制到 tag 数据区（头后 8 字节起）。
             * 记录其物理地址供 acpi_init 优先使用；tag 15（ACPI 2.0+ 36 字节，
             * 含 XSDT 指针）出现时覆盖 tag 14（1.0 副本，仅 RSDT）。 */
            uint64_t data_off = (uint64_t)(ptr - base) + 8;
            if (tag->type == MULTIBOOT_TAG_TYPE_ACPI_NEW ||
                out->rsdp_copy_phys == 0) {
                out->rsdp_copy_phys = mbi_phys + data_off;
            }
            break;
        }
        case MULTIBOOT_TAG_TYPE_MODULE: {
            /* 引导模块（configs/display.cfg）。仅采纳首个模块作为显示配置，
             * 后续模块（若有）暂忽略。mod_start/mod_end 为物理地址区间。 */
            if (out->cfg_phys == 0) {
                const struct mb2_tag_module *mod =
                    (const struct mb2_tag_module *)tag;
                out->cfg_phys = (uint64_t)mod->mod_start;
                out->cfg_size = mod->mod_end - mod->mod_start;
                if (out->cfg_size > 65536) out->cfg_size = 65536;
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
