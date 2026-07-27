/*
 * kernel/acpi/acpi.c
 * -----------------------------------------------------------------------------
 * ACPI 表解析实现（见 include/kernel/acpi.h 的设计说明）。
 *
 * 调用关系：kmain() -> acpi_init()（须在 lapic_init/ioapic_init 之前，因为
 *           APIC 驱动要从 MADT 取 LAPIC/IOAPIC 基址与 GSI 映射）。
 *
 * 内存访问：ACPI 表位于低端物理内存（<4GB），经 PHYS_TO_VIRT 高半区恒等偏移
 * 读取（引导期页表已映射 0..4GB）。
 */
#include <kernel/acpi.h>
#include <kernel/console.h>
#include <kernel/io.h>
#include <string.h>

acpi_info_t g_acpi;

/* P0-6：Multiboot2 tag 14/15 提供的 RSDP 副本物理地址（0=未提供）。
 * UEFI 启动时 RSDP 位于 EFI 配置表指向的任意物理页，EBDA/0xE0000 传统
 * 扫描必然落空，此提示是 UEFI 路径的唯一可靠 RSDP 来源。 */
static uint64_t g_rsdp_hint_phys;

void acpi_set_rsdp_hint(uint64_t rsdp_phys)
{
    g_rsdp_hint_phys = rsdp_phys;
}

/* ---- RSDP / 表头结构 ---- */
struct acpi_rsdp {
    char     signature[8];     /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;         /* 0=ACPI1.0, 2=ACPI2.0+ */
    uint32_t rsdt_phys;
    uint32_t length;           /* ACPI 2.0+ 才有意义 */
    uint64_t xsdt_phys;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct acpi_sdt_hdr {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

/* MADT 头部（SDT 头 + 控制器基址 + 标志） */
struct acpi_madt {
    struct acpi_sdt_hdr hdr;
    uint32_t lapic_phys;       /* 本地 APIC 寄存器基址 */
    uint32_t flags;
    uint8_t  entries[0];
} __attribute__((packed));

struct acpi_madt_hdr {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

struct acpi_madt_lapic {
    struct acpi_madt_hdr h;
    uint8_t  acpi_cpu_id;
    uint8_t  lapic_id;
    uint32_t flags;
} __attribute__((packed));

struct acpi_madt_ioapic {
    struct acpi_madt_hdr h;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_phys;
    uint32_t gsi_base;
} __attribute__((packed));

/* 4 字节 ACPI 表签名比较（表头 signature 字段固定 4 字节：FACP/APIC/HPET…）。
 * 注意：RSDP 签名为 8 字节 "RSD PTR "，单独用 memcmp(...,8) 处理。 */
static bool sig_eq(const void *a, const char sig[4])
{
    return memcmp(a, sig, 4) == 0;
}

/* 简单 8 位校验和：所有字节相加必须为 0 */
static bool acpi_checksum_ok(const void *p, size_t len)
{
    const uint8_t *b = (const uint8_t *)p;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += b[i];
    }
    return sum == 0;
}

/* 在 [start,end) 范围内以 16 字节步长扫描 "RSD PTR " 签名 */
static const struct acpi_rsdp *acpi_scan_rsdp(uint64_t start, uint64_t end)
{
    for (uint64_t p = start; p + sizeof(struct acpi_rsdp) <= end; p += 16) {
        const struct acpi_rsdp *r = (const struct acpi_rsdp *)PHYS_TO_VIRT(p);
        if (memcmp(r->signature, ACPI_SIG_RSDP, 8) == 0 &&
            acpi_checksum_ok(r, (r->revision == 0) ? 20 : sizeof(struct acpi_rsdp))) {
            return r;
        }
    }
    return NULL;
}

/* 处理单张 ACPI 表：记录关键表（MADT/FADT/HPET）并打印签名。
 * 返回 false 表示该表头校验失败（调用方应跳过）。 */
static bool acpi_process_table(uint64_t tbl_phys, uint32_t idx)
{
    if (!tbl_phys) {
        return false;
    }
    const struct acpi_sdt_hdr *h =
        (const struct acpi_sdt_hdr *)PHYS_TO_VIRT(tbl_phys);
    uint32_t len = h->length;          /* 先读长度，避免越界读损坏表 */
    if (len < sizeof(struct acpi_sdt_hdr) || !acpi_checksum_ok(h, len)) {
        kprintf("[acpi]   table[%u] @ %p: BAD checksum/length, skipped\n",
                (unsigned)idx, (void *)tbl_phys);
        return false;
    }
    char sig8[9];
    memcpy(sig8, h->signature, 4);
    sig8[4] = '\0';

    if (sig_eq(h->signature, ACPI_SIG_MADT)) {
        g_acpi.madt_phys = tbl_phys;
    } else if (sig_eq(h->signature, ACPI_SIG_FADT)) {
        g_acpi.fadt_phys = tbl_phys;
    } else if (sig_eq(h->signature, ACPI_SIG_HPET)) {
        g_acpi.hpet_phys = tbl_phys;
    }

    if (idx < 24) {
        kprintf("[acpi]   table[%u]: '%s' @ %p len=%u\n",
                (unsigned)idx, sig8, (void *)tbl_phys, len);
    }
    return true;
}

/* 遍历 XSDT（64 位指针数组），对每个表头解析签名，定位关键表并提取 MADT 枚举 */
static void acpi_walk_xsdt(uint64_t xsdt_phys)
{
    const struct acpi_sdt_hdr *xsdt =
        (const struct acpi_sdt_hdr *)PHYS_TO_VIRT(xsdt_phys);
    if (!acpi_checksum_ok(xsdt, xsdt->length)) {
        kprintf("[acpi] XSDT checksum INVALID, ignoring\n");
        return;
    }
    uint64_t ent_count = (xsdt->length - sizeof(struct acpi_sdt_hdr)) / 8;
    const uint64_t *ents = (const uint64_t *)((const uint8_t *)xsdt +
                                              sizeof(struct acpi_sdt_hdr));
    kprintf("[acpi] XSDT @ %p: %u tables\n", (void *)xsdt_phys, (unsigned)ent_count);

    for (uint64_t i = 0; i < ent_count; i++) {
        acpi_process_table(ents[i], (uint32_t)i);
    }
}

/* 遍历 RSDT（32 位指针数组，ACPI 1.0 / 多数虚拟机的默认形态，如 SeaBIOS）。
 * 与 XSDT 逻辑相同，仅指针宽度为 32 位。 */
static void acpi_walk_rsdt(uint64_t rsdt_phys)
{
    const struct acpi_sdt_hdr *rsdt =
        (const struct acpi_sdt_hdr *)PHYS_TO_VIRT(rsdt_phys);
    if (!acpi_checksum_ok(rsdt, rsdt->length)) {
        kprintf("[acpi] RSDT checksum INVALID, ignoring\n");
        return;
    }
    uint64_t ent_count = (rsdt->length - sizeof(struct acpi_sdt_hdr)) / 4;
    const uint32_t *ents = (const uint32_t *)((const uint8_t *)rsdt +
                                              sizeof(struct acpi_sdt_hdr));
    kprintf("[acpi] RSDT @ %p: %u tables\n", (void *)rsdt_phys, (unsigned)ent_count);
    for (uint64_t i = 0; i < ent_count; i++) {
        acpi_process_table((uint64_t)ents[i], (uint32_t)i);
    }
}

/* 解析 MADT：枚举 LAPIC、定位 I/O APIC 基址与 GSI 基 */
static void acpi_parse_madt(void)
{
    if (!g_acpi.madt_phys) {
        return;
    }
    const struct acpi_madt *m =
        (const struct acpi_madt *)PHYS_TO_VIRT(g_acpi.madt_phys);
    if (!acpi_checksum_ok(m, m->hdr.length)) {
        kprintf("[acpi] MADT checksum INVALID, skipping parse\n");
        return;
    }
    /* MADT 给出的本地 APIC 基址（MSR 0x1B 也应一致，优先用 MSR 运行时值） */
    if (m->lapic_phys) {
        g_acpi.lapic_phys = (uint64_t)m->lapic_phys;
    }

    const uint8_t *p = m->entries;
    const uint8_t *end = (const uint8_t *)m + m->hdr.length;
    while (p + sizeof(struct acpi_madt_hdr) <= end) {
        const struct acpi_madt_hdr *h = (const struct acpi_madt_hdr *)p;
        if (h->length < 2) {
            break;          /* 防御畸形表 */
        }
        if (h->type == ACPI_MADT_LAPIC) {
            const struct acpi_madt_lapic *l = (const struct acpi_madt_lapic *)p;
            if (l->flags & ACPI_MADT_LAPIC_ENABLED) {
                if (g_acpi.lapic_count < 256) {
                    g_acpi.madt_lapic_id[g_acpi.lapic_count++] = l->lapic_id;
                }
            }
        } else if (h->type == ACPI_MADT_IOAPIC) {
            const struct acpi_madt_ioapic *io = (const struct acpi_madt_ioapic *)p;
            if (g_acpi.ioapic_phys == 0) {     /* 取第一个 I/O APIC */
                g_acpi.ioapic_phys  = (uint64_t)io->ioapic_phys;
                g_acpi.ioapic_gsi_base = io->gsi_base;
                g_acpi.ioapic_id    = io->ioapic_id;
            }
        }
        p += h->length;
    }
    kprintf("[acpi] MADT: %u enabled LAPIC(s), IOAPIC @ %p (gsi_base=%u)\n",
            (unsigned)g_acpi.lapic_count, (void *)g_acpi.ioapic_phys,
            (unsigned)g_acpi.ioapic_gsi_base);
}

bool acpi_init(void)
{
    memset(&g_acpi, 0, sizeof(g_acpi));
    g_acpi.lapic_phys = 0xFEE00000ULL;   /* 默认本地 APIC 基址（xAPIC） */

    const struct acpi_rsdp *rsdp = NULL;

    /* 0) P0-6：Multiboot2 RSDP 副本（UEFI 路径唯一可靠来源）。副本同样
     *    做签名+校验和验证——不信任引导器给的任何数据。 */
    if (g_rsdp_hint_phys) {
        const struct acpi_rsdp *r =
            (const struct acpi_rsdp *)PHYS_TO_VIRT(g_rsdp_hint_phys);
        if (memcmp(r->signature, ACPI_SIG_RSDP, 8) == 0 &&
            acpi_checksum_ok(r, (r->revision == 0) ? 20
                                                   : sizeof(struct acpi_rsdp))) {
            rsdp = r;
            kprintf("[acpi] RSDP from Multiboot2 tag (UEFI-safe path)\n");
        } else {
            kprintf("[acpi] MB2 RSDP copy invalid, falling back to scan\n");
        }
    }

    /* 1) EBDA：BDA 0x40:0x0E 存 EBDA 段基址（实模式段，*16 得线性地址） */
    if (!rsdp) {
        const uint16_t *bda_ebda =
            (const uint16_t *)PHYS_TO_VIRT(0x40E);
        uint16_t ebda_seg = *bda_ebda;
        uint64_t ebda_base = (uint64_t)ebda_seg << 4;
        if (ebda_base >= 0x400 && ebda_base < 0xA0000) {
            rsdp = acpi_scan_rsdp(ebda_base, ebda_base + 1024);
        }
    }
    /* 2) 固定范围 0xE0000..0xFFFFF */
    if (!rsdp) {
        rsdp = acpi_scan_rsdp(0x000E0000ULL, 0x00100000ULL);
    }

    if (!rsdp) {
        kprintf("[acpi] RSDP not found (legacy/BIOS-only machine?)\n");
        g_acpi.found = false;
        return false;
    }

    g_acpi.found = true;
    g_acpi.rsdp_revision = rsdp->revision;
    g_acpi.rsdt_phys = rsdp->rsdt_phys;
    g_acpi.xsdt_phys = rsdp->xsdt_phys;
    kprintf("[acpi] RSDP found @ %p (revision=%u)\n",
            (void *)rsdp, (unsigned)rsdp->revision);

    /* ACPI 2.0+ 优先走 XSDT；ACPI 1.0 仅 RSDT（如 SeaBIOS/QEMU 默认形态）。
     * 两种都解析，确保不同固件/虚拟机下都能拿到 MADT/FADT/HPET。 */
    if (rsdp->revision >= 2 && rsdp->xsdt_phys) {
        acpi_walk_xsdt(rsdp->xsdt_phys);
    } else if (rsdp->rsdt_phys) {
        acpi_walk_rsdt((uint64_t)rsdp->rsdt_phys);
    }

    acpi_parse_madt();
    kprintf("[acpi] HPET table %sfound @ %p\n",
            g_acpi.hpet_phys ? "" : "NOT ", (void *)g_acpi.hpet_phys);
    return true;
}

uint64_t acpi_find_table(const char sig[8])
{
    if (!g_acpi.found) {
        return 0;
    }
    if (sig_eq(sig, ACPI_SIG_MADT)) {
        return g_acpi.madt_phys;
    }
    if (sig_eq(sig, ACPI_SIG_FADT)) {
        return g_acpi.fadt_phys;
    }
    if (sig_eq(sig, ACPI_SIG_HPET)) {
        return g_acpi.hpet_phys;
    }
    return 0;
}
