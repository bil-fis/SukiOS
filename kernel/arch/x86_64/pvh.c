/*
 * kernel/arch/x86_64/pvh.c
 * ---------------------------------------------------------------------------
 * Xen PVH 引导信息解析。当 GRUB 不可用时，QEMU `-kernel` 以 PVH 协议加载本
 * 内核：进入 _start 时 EAX 不为 MULTIBOOT2_MAGIC，EBX 指向物理的
 * struct hvm_start_info。本文件将其翻译成内核统一的 boot_info_t，使
 * 后续初始化（pmm/vmm/acpi）与 GRUB(multiboot2) 路径完全一致。
 *
 * 关键事实：hvm_memmap_table_entry 与 Multiboot2 的 mb2_mmap_entry 内存布局
 * 完全同构（addr:8, size/len:8, type:4, reserved/zero:4），故 memmap 可直接
 * 当作 mb2 mmap 使用（entry_size=24），pmm_init 的遍历逻辑无需改动。
 *
 * 物理内存访问：本函数运行在 long mode 高半区，boot.S 已建立低 1MB 起的
 * 恒等映射，故 PHYS_TO_VIRT() 可读 hvm_start_info 及其引用的 memmap。
 */
#include <kernel/types.h>
#include <kernel/string.h>
#include <kernel/console.h>
#include <kernel/multiboot2.h>
#include <kernel/acpi.h>

#define HVM_START_MAGIC_VALUE  0x336EC578u

struct hvm_start_info {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t nr_modules;
    uint64_t modlist_paddr;
    uint64_t cmdline_paddr;
    uint64_t rsdp_paddr;
    uint64_t memmap_paddr;
    uint32_t memmap_entries;
    uint32_t reserved;
};

struct hvm_memmap_entry {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
    uint32_t reserved;
};

/* M2 审计：memmap 包装缓冲须在函数外静态分配（C 栈帧高半区可写，但 memmap
 * 指针要长期存活供 pmm_init 使用，不能放局部）。最坏 256 条 e820 项。 */
#define PVH_MMAP_MAX_ENTRIES  256
#define PVH_MMAP_BUF_BYTES    (sizeof(struct mb2_tag_mmap) + \
                               PVH_MMAP_MAX_ENTRIES * sizeof(struct mb2_mmap_entry))
static uint8_t g_pvh_mmap_buf[PVH_MMAP_BUF_BYTES] __attribute__((aligned(8)));

/*
 * pvh_parse: 解析 PVH hvm_start_info 至 boot_info_t。
 * 返回 void；失败时（magic 错误）保守填充 mem_highest=128MB 回退。
 */
void pvh_parse(uint64_t hvm_phys, boot_info_t *out)
{
    memset(out, 0, sizeof(*out));
    const struct hvm_start_info *h =
        (const struct hvm_start_info *)PHYS_TO_VIRT(hvm_phys);

    if (h->magic != HVM_START_MAGIC_VALUE) {
        kprintf("[pvh] FATAL: bad hvm_start_info magic 0x%08x (expect 0x%08x)\n",
                h->magic, HVM_START_MAGIC_VALUE);
        out->mem_highest = 0x8000000;   /* 128MB 回退，避免 pmm 越界 */
        return;
    }

    kprintf("[pvh] hvm_start_info: version=%u flags=%u modules=%u "
            "rsdp_paddr=0x%llx memmap_entries=%u\n",
            h->version, h->flags, h->nr_modules,
            (unsigned long long)h->rsdp_paddr,
            h->memmap_entries);

    /* ---- 内存映射：hvm memmap -> mb2 mmap 包装 ---- */
    const struct hvm_memmap_entry *src =
        (const struct hvm_memmap_entry *)PHYS_TO_VIRT(h->memmap_paddr);
    uint32_t n = h->memmap_entries;
    if (n > PVH_MMAP_MAX_ENTRIES) {
        kprintf("[pvh] memmap entries %u exceed cap %d, truncating\n",
                n, PVH_MMAP_MAX_ENTRIES);
        n = PVH_MMAP_MAX_ENTRIES;
    }

    struct mb2_tag_mmap *mm = (struct mb2_tag_mmap *)g_pvh_mmap_buf;
    mm->type = 6;                       /* MB2_TAG_MMAP */
    mm->entry_size = sizeof(struct mb2_mmap_entry);
    mm->entry_version = 1;

    uint8_t *dst = (uint8_t *)mm->entries;
    uint64_t highest = 0;
    for (uint32_t i = 0; i < n; i++) {
        /* 同构复制（addr/len/type/zero 布局一致） */
        memcpy(dst, &src[i], sizeof(struct mb2_mmap_entry));
        /* 最高物理地址仅由可用 RAM(type==1) 决定；QEMU PVH 会把 64-bit
         * PCI MMIO 窗口等保留区也报成极大地址（可达 1TiB），若据此计算
         * PMM 总页数会导致 refcount 数组溢出物理 RAM 边界，触发 QEMU 崩溃。 */
        if (src[i].type == 1) {
            uint64_t end = src[i].addr + src[i].size;
            if (end > highest) highest = end;
        }
        dst += sizeof(struct mb2_mmap_entry);
    }
    mm->size = (uint32_t)(8 + (uint64_t)n * sizeof(struct mb2_mmap_entry));

    out->mmap        = mm;
    out->mem_total   = highest;
    out->mem_highest = highest;

    kprintf("[pvh] memmap: %u entries, highest phys = 0x%llx (%llu MiB)\n",
            n, (unsigned long long)highest,
            (unsigned long long)(highest / (1024 * 1024)));

    /* ---- RSDP：PVH 直接给出物理地址（gPEB 无固件注入时回退 EBDA 扫描） ---- */
    if (h->rsdp_paddr) {
        acpi_set_rsdp_hint(h->rsdp_paddr);
        out->rsdp_copy_phys = h->rsdp_paddr;
        kprintf("[pvh] rsdp hint set from hvm_start_info (0x%llx)\n",
                (unsigned long long)h->rsdp_paddr);
    } else {
        out->rsdp_copy_phys = 0;
        kprintf("[pvh] no rsdp_paddr from loader; acpi_init will scan EBDA\n");
    }

    out->efi_boot  = false;     /* PVH 始终为 BIOS/legacy 启动 */
    out->fb_present = false;    /* PVH 不提供 framebuffer，回退 VGA 文本 */
}
