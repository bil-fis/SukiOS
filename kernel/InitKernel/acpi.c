/* =============================================================================
 * SukiOS - ACPI 表解析（RSDP 搜索 + MADT 解析）
 *
 * 参考：OSDev MADT, ACPI Specification 6.5
 * ============================================================================= */

#include "kernel/acpi.h"
#include "kernel/tty.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

/* ---- ISA 中断源覆盖表 ---- */
static struct acpi_madt_int_override isa_overrides[ACPI_MAX_ISA_OVERRIDES];
static int isa_override_count = 0;

void acpi_parse_isa_overrides(struct acpi_sdt_header *madt_hdr)
{
    isa_override_count = 0;
    memset(isa_overrides, 0, sizeof(isa_overrides));

    uint8_t *ptr = (uint8_t *)madt_hdr + sizeof(struct acpi_madt);
    uint8_t *end = (uint8_t *)madt_hdr + madt_hdr->length;

    while (ptr + sizeof(struct acpi_madt_entry) <= end) {
        struct acpi_madt_entry *entry = (struct acpi_madt_entry *)ptr;
        if (entry->length < sizeof(struct acpi_madt_entry))
            break;

        if (entry->type == 2 && isa_override_count < ACPI_MAX_ISA_OVERRIDES) {
            struct acpi_madt_int_override *ov =
                (struct acpi_madt_int_override *)entry;
            isa_overrides[isa_override_count++] = *ov;
        }

        ptr += entry->length;
    }
}

bool acpi_get_isa_override(uint8_t isa_irq, uint32_t *gsi, uint16_t *flags)
{
    for (int i = 0; i < isa_override_count; i++) {
        if (isa_overrides[i].irq == isa_irq) {
            *gsi = isa_overrides[i].gsi;
            *flags = isa_overrides[i].flags;
            return true;
        }
    }
    /* 默认：无 override，GSI = ISA IRQ */
    *gsi = isa_irq;
    *flags = 0;
    return false;
}

/* 在指定物理地址范围内搜索 RSDP */
static struct acpi_rsdp *search_rsdp(uintptr_t start, uintptr_t end)
{
    for (uintptr_t addr = start; addr < end; addr += 16) {
        struct acpi_rsdp *r = (struct acpi_rsdp *)addr;
        /* 检查签名 "RSD PTR " */
        if (r->signature[0] == 'R' && r->signature[1] == 'S' &&
            r->signature[2] == 'D' && r->signature[3] == ' ' &&
            r->signature[4] == 'P' && r->signature[5] == 'T' &&
            r->signature[6] == 'R' && r->signature[7] == ' ')
            return r;
    }
    return NULL;
}

/* 验证校验和 */
static bool verify_checksum(void *ptr, size_t len)
{
    uint8_t sum = 0;
    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0; i < len; i++)
        sum += p[i];
    return sum == 0;
}

struct acpi_rsdp *acpi_find_rsdp(void)
{
    struct acpi_rsdp *r;

    /* 1. 搜索 EBDA（Extended BIOS Data Area，前 1KB）
     * EBDA 段地址存储在 0x40E 处（16 位段地址） */
    uint16_t ebda_seg = *(volatile uint16_t *)0x40E;
    if (ebda_seg) {
        uintptr_t ebda_addr = (uintptr_t)ebda_seg << 4;
        r = search_rsdp(ebda_addr, ebda_addr + 1024);
        if (r) return r;
    }

    /* 2. 搜索 BIOS ROM 区域 0xE0000 - 0xFFFFF */
    r = search_rsdp(0xE0000, 0x100000);
    if (r) return r;

    return NULL;
}

struct acpi_sdt_header *acpi_find_table(struct acpi_rsdp *rsdp, const char *sig)
{
    if (rsdp->revision >= 2 && rsdp->xsdt_addr) {
        /* 使用 XSDT（64 位地址表） */
        struct acpi_sdt_header *xsdt = (struct acpi_sdt_header *)(uintptr_t)rsdp->xsdt_addr;
        if (!verify_checksum(xsdt, xsdt->length))
            return NULL;

        int count = (xsdt->length - sizeof(struct acpi_sdt_header)) / 8;
        uint64_t *entries = (uint64_t *)((uintptr_t)xsdt + sizeof(struct acpi_sdt_header));

        for (int i = 0; i < count; i++) {
            struct acpi_sdt_header *sdt =
                (struct acpi_sdt_header *)(uintptr_t)entries[i];
            if (sdt->signature[0] == sig[0] && sdt->signature[1] == sig[1] &&
                sdt->signature[2] == sig[2] && sdt->signature[3] == sig[3])
                return sdt;
        }
    } else if (rsdp->rsdt_addr) {
        /* 回退到 RSDT（32 位地址表） */
        struct acpi_sdt_header *rsdt = (struct acpi_sdt_header *)(uintptr_t)rsdp->rsdt_addr;
        if (!verify_checksum(rsdt, rsdt->length))
            return NULL;

        int count = (rsdt->length - sizeof(struct acpi_sdt_header)) / 4;
        uint32_t *entries = (uint32_t *)((uintptr_t)rsdt + sizeof(struct acpi_sdt_header));

        for (int i = 0; i < count; i++) {
            struct acpi_sdt_header *sdt =
                (struct acpi_sdt_header *)(uintptr_t)entries[i];
            if (sdt->signature[0] == sig[0] && sdt->signature[1] == sig[1] &&
                sdt->signature[2] == sig[2] && sdt->signature[3] == sig[3])
                return sdt;
        }
    }

    return NULL;
}

struct acpi_madt_info acpi_parse_madt(struct acpi_sdt_header *hdr)
{
    struct acpi_madt_info info;
    /* 清零整个结构体 */
    for (size_t i = 0; i < sizeof(info); i++)
        ((uint8_t *)&info)[i] = 0;

    struct acpi_madt *madt = (struct acpi_madt *)hdr;
    info.lapic_addr = madt->local_apic_addr;
    info.flags = madt->flags;

    /* 遍历 MADT 内的可变长条目 */
    uint8_t *ptr = (uint8_t *)hdr + sizeof(struct acpi_madt);
    uint8_t *end = (uint8_t *)hdr + hdr->length;

    while (ptr + sizeof(struct acpi_madt_entry) <= end) {
        struct acpi_madt_entry *entry = (struct acpi_madt_entry *)ptr;
        if (entry->length < sizeof(struct acpi_madt_entry))
            break;

        switch (entry->type) {
        case 1: { /* I/O APIC */
            struct acpi_madt_ioapic *io = (struct acpi_madt_ioapic *)entry;
            info.ioapic_found = true;
            info.ioapic_id = io->ioapic_id;
            info.ioapic_addr = io->ioapic_addr;
            info.ioapic_gsi_base = io->gsi_base;
            break;
        }
        case 5: { /* Local APIC Address Override（64 位地址） */
            struct acpi_madt_lapic_override *ov =
                (struct acpi_madt_lapic_override *)entry;
            info.lapic_addr = (uint32_t)ov->lapic_addr;
            break;
        }
        }

        ptr += entry->length;
    }

    return info;
}
