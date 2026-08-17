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
static void acpi_parse_fadt(uint64_t fadt_phys);   /* 前向声明（定义见下文） */
static void acpi_parse_mcfg(void);                  /* 前向声明（P0-1 MCFG） */
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
        acpi_parse_fadt(tbl_phys);   /* P0-R8：解析 PM 寄存器用于 S5 关机 */
    } else if (sig_eq(h->signature, ACPI_SIG_HPET)) {
        g_acpi.hpet_phys = tbl_phys;
    } else if (sig_eq(h->signature, ACPI_SIG_MCFG)) {
        g_acpi.mcfg_phys = tbl_phys;
        acpi_parse_mcfg();   /* P0-1：记录 PCIe ECAM 窗口供 PCI 配置访问 */
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

/* 读 DSDT AML 包内一个整数表达式（仅支持常见常量形态）：
 *   ByteConst 0x0A、WordConst 0x0B（取低 8 位）、Zero 0x00、One 0x01。
 * 返回该值，并把 *pos 推进到表达式之后；遇到其它（如 NameString 引用）
 * 则跳过 1 字节回落 0（不解析复杂 AML 表达式——_S5 包内一般为常量）。 */
static uint8_t acpi_aml_pkg_int(const uint8_t *p, uint32_t end, uint32_t *pos)
{
    if (*pos >= end) {
        return 0;
    }
    uint8_t op = p[*pos];
    if (op == 0x0A) {            /* ByteConst */
        uint8_t v = (*pos + 1 < end) ? p[*pos + 1] : 0;
        *pos += 2;
        return v;
    } else if (op == 0x0B) {     /* WordConst（取低 8 位） */
        uint8_t v = (*pos + 1 < end) ? p[*pos + 1] : 0;
        *pos += 3;
        return v;
    } else if (op == 0x00) {     /* Zero */
        *pos += 1;
        return 0;
    } else if (op == 0x01) {     /* One */
        *pos += 1;
        return 1;
    }
    *pos += 1;                   /* 未知：跳过 1 字节，回落 0 */
    return 0;
}

/* P0-R8：从 DSDT 抽取 S5 的 SLP_TYP。ACPI 规范中 FADT 本身不含 SLP_TYP，
 * 必须从 ACPI 命名空间的 _S5 对象得到。支持两种常见形态：
 *   Name (_S5, Package (N) {a, b, ...})       -> NameOp 0x08 + NameSeg "_S5" + PackageOp 0x12
 *   Method (_S5, 0) { Return (Package (N){a,b,...}) } -> MethodOp 0x14 + NameSeg + 包内 0x12
 * 取包内前两个整数（PM1a/PM1b 的 SLP_TYP）的低 3 位。找不到时回落平台通用
 * 值 5（QEMU/SeaBIOS 与多数 x86 机器 S5=5）。 */
static void acpi_parse_s5(uint64_t dsdt_phys)
{
    const struct acpi_sdt_hdr *h =
        (const struct acpi_sdt_hdr *)PHYS_TO_VIRT(dsdt_phys);
    if (!acpi_checksum_ok(h, h->length)) {
        kprintf("[acpi] S5: DSDT checksum invalid, default SLP_TYP=5\n");
        g_acpi.slp_typ_a = 5;
        g_acpi.slp_typ_b = 5;
        return;
    }
    const uint8_t *aml = (const uint8_t *)h + sizeof(struct acpi_sdt_hdr);
    uint32_t len = h->length - sizeof(struct acpi_sdt_hdr);

    for (uint32_t i = 0; i + 3 < len; i++) {
        /* 定位 NameSeg "_S5"（4 字节：0x5F 0x53 0x35 0x5F，末字节为填充 '_'） */
        if (aml[i] != 0x5F || aml[i + 1] != 0x53 || aml[i + 2] != 0x35) {
            continue;
        }
        uint8_t lead = (i >= 1) ? aml[i - 1] : 0;
        uint32_t body = 0;
        if (lead == 0x08) {                 /* Name (_S5, ...) */
            body = i + 4;                   /* 跳过 4 字节 NameSeg */
        } else if (lead == 0x14) {          /* Method (_S5, ...) */
            uint32_t mp = i - 1;            /* MethodOp 位置 */
            uint8_t mlead = aml[mp + 1];    /* 方法 pkglen 引导字节 */
            uint32_t mbody;
            if (mlead & 0x80) {
                uint8_t nn = mlead & 0x0F;
                mbody = mp + 2 + nn;        /* pkglen 占 1+nn 字节 */
            } else {
                mbody = mp + 2;
            }
            uint32_t mend = mp + 1 + (mlead & 0x80 ?
                                      (1 + (mlead & 0x0F)) : mlead);
            /* 在方法体内找 Return 紧跟的 PackageOp 0x12 */
            body = 0;
            for (uint32_t j = mbody; j + 1 < mend && j + 1 < len; j++) {
                if (aml[j] == 0x12) {
                    body = j;
                    break;
                }
            }
            if (body == 0) {
                continue;
            }
        } else {
            continue;                       /* 命中 '_S5' 但非对象定义 */
        }

        if (body + 1 >= len || aml[body] != 0x12) {
            continue;                       /* 期望 PackageOp */
        }
        /* PackageOp: 12 <pkglen> <numelements> <elems...> */
        uint8_t pl = aml[body + 1];
        uint32_t pstart = body + 2;
        if (pl & 0x80) {
            uint8_t nn = pl & 0x0F;
            pstart = body + 2 + nn;        /* pkglen 占 1+nn 字节 */
        }
        if (pstart + 1 >= len) {
            continue;
        }
        uint32_t e = pstart + 1;            /* 跳过 numelements 字节 */
        uint8_t a = acpi_aml_pkg_int(aml, len, &e);
        uint8_t b = acpi_aml_pkg_int(aml, len, &e);
        g_acpi.slp_typ_a = a & 0x7;
        g_acpi.slp_typ_b = b & 0x7;
        kprintf("[acpi] S5 from DSDT _S5: PM1a SLP_TYP=%u PM1b SLP_TYP=%u\n",
                (unsigned)a, (unsigned)b);
        return;
    }
    kprintf("[acpi] S5: _S5 not found, default SLP_TYP=5 (QEMU-compatible)\n");
    g_acpi.slp_typ_a = 5;
    g_acpi.slp_typ_b = 5;
}

/* P0-R8：解析 FADT 电源管理寄存器 + DSDT _S5。
 *
 * FADT 字段偏移（ACPI 6.x Table 5-33，1.0 起即固定，实测与 QEMU/SeaBIOS 一致）：
 *   0x08  Revision              (1)
 *   0x28  DSDT                  (4)  32 位 DSDT 物理地址
 *   0x38  PM1a_EVT_BLK          (4)
 *   0x3C  PM1b_EVT_BLK          (4)
 *   0x40  PM1a_CNT_BLK          (4)  ← S5 写入目标
 *   0x44  PM1b_CNT_BLK          (4)
 *   0x48  PM2_CNT_BLK           (4)
 *   0x4C  PM_TMR_BLK            (4)
 *   0x8C  X_DSDT                (8)  ACPI 2.0+
 *   0xAC  X_PM1a_CNT_BLK        (12) GAS：id/width/off/access(4B) + address(8B @0xB0)
 *   0xB8  X_PM1b_CNT_BLK        (12) GAS：address @0xBC
 * 历史 bug：早期实现误用 0x48/0x4C 取 PM1a/PM1b（那其实是 PM2_CNT/PM_TMR），
 * 后又误写成 0x3B/0x3F（错位 1 字节），导致把 0x604 读成 0x00060400。
 *
 * SLP_TYP 不在 FADT 内，必须由 DSDT 的 _S5 对象给出（见 acpi_parse_s5）。 */
static void acpi_parse_fadt(uint64_t fadt_phys)
{
    const uint8_t *f = (const uint8_t *)PHYS_TO_VIRT(fadt_phys);
    uint32_t flen = *(const uint32_t *)(f + 0x04);
    uint8_t rev = f[0x08];

    uint32_t pm1a = *(const uint32_t *)(f + 0x40);
    uint32_t pm1b = *(const uint32_t *)(f + 0x44);

    /* ACPI 2.0+：32 位字段为 0 时回落 X_ GAS（仅接受 SystemIO 空间 id==1） */
    if (rev >= 2 && flen >= 0xB8 + 12) {
        if (pm1a == 0 && f[0xAC] == 1) {
            pm1a = (uint32_t)(*(const uint64_t *)(f + 0xB0) & 0xFFFFFFFFULL);
        }
        if (pm1b == 0 && f[0xB8] == 1) {
            pm1b = (uint32_t)(*(const uint64_t *)(f + 0xBC) & 0xFFFFFFFFULL);
        }
    }
    g_acpi.pm1a_cnt_blk = pm1a;
    g_acpi.pm1b_cnt_blk = pm1b;

    uint64_t dsdt = (uint64_t)(*(const uint32_t *)(f + 0x28));
    if (dsdt == 0 && rev >= 2 && flen >= 0x8C + 8) {
        dsdt = *(const uint64_t *)(f + 0x8C);       /* X_DSDT */
    }
    g_acpi.dsdt_phys = dsdt;

    if (dsdt) {
        acpi_parse_s5(dsdt);
    } else {
        /* 无 DSDT：回落 ACPI 通用软关机类型（多数 x86 平台 S5=5） */
        g_acpi.slp_typ_a = 5;
        g_acpi.slp_typ_b = 5;
        kprintf("[acpi] S5: no DSDT, default SLP_TYP=5\n");
    }
    kprintf("[acpi] FADT rev=%u PM1a_CNT=0x%x PM1b_CNT=0x%x DSDT=0x%x S5 A=%u B=%u\n",
            (unsigned)rev, (unsigned)pm1a, (unsigned)pm1b,
            (unsigned)dsdt,
            (unsigned)g_acpi.slp_typ_a, (unsigned)g_acpi.slp_typ_b);
}

/* P0-1：解析 MCFG，记录所有 PCIe ECAM 配置空间窗口。
 * MCFG 表头（36 字节）后跟每组 16 字节窗口：
 *   base[8] | seg_group[2] | bus_start[1] | bus_end[1] | reserved[4]。
 * 传统 PCI（i440FX）不提供 MCFG，此时 mcfg_count==0，PCI 走 PIO 0xCF8。 */
static void acpi_parse_mcfg(void)
{
    if (!g_acpi.mcfg_phys) {
        return;
    }
    const uint8_t *m = (const uint8_t *)PHYS_TO_VIRT(g_acpi.mcfg_phys);
    uint32_t len = *(const uint32_t *)(m + 0x04);
    /* MCFG：36 字节 SDT 头 + 8 字节保留字段后，才是分配结构（每项 16 字节）。
     * 条目起始偏移为 44，而非 36（ACPI 6.x 规范 §PCI Express Memory
     * Mapped Configuration Space Base Address Allocation Structure）。 */
    if (len < 44 + 16 || !acpi_checksum_ok((const struct acpi_sdt_hdr *)m, len)) {
        kprintf("[acpi] MCFG: BAD length/checksum, skipped\n");
        return;
    }
    uint32_t n = (len - 44) / 16;
    if (n > ACPI_MCFG_MAX_WINDOWS) {
        n = ACPI_MCFG_MAX_WINDOWS;
    }
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = m + 44 + i * 16;
        uint64_t base = 0;
        uint16_t seg = 0;
        memcpy(&base, e, 8);              /* 用 memcpy 避免未对齐 64 位读取 */
        memcpy(&seg, e + 8, 2);
        g_acpi.mcfg_windows[i].base      = base;
        g_acpi.mcfg_windows[i].seg_group = seg;
        g_acpi.mcfg_windows[i].bus_start = e[10];
        g_acpi.mcfg_windows[i].bus_end   = e[11];
    }
    g_acpi.mcfg_count = n;
    kprintf("[acpi] MCFG: %u ECAM window(s):\n", (unsigned)n);
    for (uint32_t i = 0; i < n; i++) {
        kprintf("    seg=%u bus=%u..%u base=%p\n",
                (unsigned)g_acpi.mcfg_windows[i].seg_group,
                (unsigned)g_acpi.mcfg_windows[i].bus_start,
                (unsigned)g_acpi.mcfg_windows[i].bus_end,
                (void *)(uintptr_t)g_acpi.mcfg_windows[i].base);
    }
}

/* 查询段组/总线对应的 ECAM 窗口（见 acpi.h 说明）。 */
bool acpi_get_mcfg_window(uint16_t seg_group, uint8_t bus,
                          uint64_t *base_out, uint8_t *bus_start_out,
                          uint8_t *bus_end_out)
{
    for (uint32_t i = 0; i < g_acpi.mcfg_count; i++) {
        if (g_acpi.mcfg_windows[i].seg_group == seg_group &&
            bus >= g_acpi.mcfg_windows[i].bus_start &&
            bus <= g_acpi.mcfg_windows[i].bus_end) {
            if (base_out)    *base_out    = g_acpi.mcfg_windows[i].base;
            if (bus_start_out) *bus_start_out = g_acpi.mcfg_windows[i].bus_start;
            if (bus_end_out) *bus_end_out  = g_acpi.mcfg_windows[i].bus_end;
            return true;
        }
    }
    return false;
}

/* ── _PRT 解析辅助：ACPI PkgLength 解码（ACPI 6.x 表 17-3）──────────────
 * 返回包内容长度，并通过 *cstart 给出内容起始偏移（pkg = PackageOp 位置）。 */
static uint32_t acpi_pkg_bounds(const uint8_t *aml, uint32_t pkg,
                                 uint32_t *cstart)
{
    uint8_t lead = aml[pkg + 1];
    if (!(lead & 0x80)) {
        *cstart = pkg + 2;
        return lead;
    }
    uint8_t nn = (lead >> 6) & 0x03;     /* 后续字节数（0..3） */
    uint32_t L = (uint32_t)(lead & 0x0F);
    for (uint8_t j = 0; j < nn; j++) {
        L |= ((uint32_t)aml[pkg + 2 + j] << ((j + 1) * 8));
    }
    *cstart = pkg + 2 + nn;
    return L;
}

/* 读一个 AML 整型常量并返回其值，并把 *pos 推进到表达式之后。
 * 支持 Byte/Word/DWord/QWord 前缀、Zero/One/Ones；未知形态跳过 1 字节回落 0。 */
static uint64_t acpi_aml_const(const uint8_t *aml, uint32_t end, uint32_t *pos)
{
    if (*pos >= end) {
        return 0;
    }
    uint8_t op = aml[*pos];
    switch (op) {
    case 0x00: *pos += 1; return 0;                       /* Zero */
    case 0x01: *pos += 1; return 1;                       /* One */
    case 0xFF: *pos += 1; return 0xFFFFFFFFULL;           /* Ones */
    case 0x0A:                                            /* ByteConst */
        if (*pos + 2 > end) { *pos = end; return 0; }
        { uint64_t v = aml[*pos + 1]; *pos += 2; return v; }
    case 0x0B:                                            /* WordConst */
        if (*pos + 3 > end) { *pos = end; return 0; }
        { uint64_t v = (uint64_t)aml[*pos + 1] | ((uint64_t)aml[*pos + 2] << 8);
          *pos += 3; return v; }
    case 0x0C:                                            /* DWordConst */
        if (*pos + 5 > end) { *pos = end; return 0; }
        { uint64_t v = (uint64_t)aml[*pos + 1] | ((uint64_t)aml[*pos + 2] << 8) |
                       ((uint64_t)aml[*pos + 3] << 16) | ((uint64_t)aml[*pos + 4] << 24);
          *pos += 5; return v; }
    case 0x0E:                                            /* QWordConst */
        if (*pos + 9 > end) { *pos = end; return 0; }
        { uint64_t v = 0; for (uint8_t b = 0; b < 8; b++) {
              v |= (uint64_t)aml[*pos + 1 + b] << (8 * b); }
          *pos += 9; return v; }
    default:
        *pos += 1; return 0;
    }
}

/* 在 DSDT 中定位链接设备 NameSeg 的 _CRS，若是常量 ResourceTemplate 则
 * 解析其中的 Extended Interrupt(0x89) 或 IRQ(0x22) 描述符取 GSI。
 * 计算型 _CRS（需方法求值）无法解析，返回 -1 由调用方回落 INT_LINE。 */
static int acpi_parse_resource_gsi(const uint8_t *aml, uint32_t len, uint32_t *pos)
{
    uint32_t p = *pos;
    while (p < len) {
        uint8_t t = aml[p];
        if (t == 0x89) {                       /* Extended Interrupt 描述符 */
            if (p + 2 >= len) break;
            uint8_t cnt = aml[p + 3];          /* 中断表长度 */
            if (p + 4 + 4 > len) break;
            uint32_t gsi = (uint32_t)aml[p + 4] | ((uint32_t)aml[p + 5] << 8) |
                           ((uint32_t)aml[p + 6] << 16) | ((uint32_t)aml[p + 7] << 24);
            (void)cnt;
            *pos = p + 4 + 4;
            return (int)gsi;
        } else if (t == 0x22) {                /* 小 IRQ 描述符：位图 */
            if (p + 4 > len) break;
            uint16_t mask = (uint16_t)aml[p + 2] | ((uint16_t)aml[p + 3] << 8);
            for (uint8_t b = 0; b < 16; b++) {
                if (mask & (1u << b)) { *pos = p + 4; return (int)b; }
            }
            break;
        } else if (t == 0x23) {                /* 大 IRQ 描述符：变长位图 */
            if (p + 1 >= len) break;
            uint8_t blen = aml[p + 1];
            uint32_t bit = 0;
            for (uint8_t bb = 0; bb < blen && p + 2 + bb < len; bb++) {
                uint8_t byte = aml[p + 2 + bb];
                for (uint8_t bitin = 0; bitin < 8; bitin++) {
                    if (byte & (1u << bitin)) { *pos = p + 2 + bb; return (int)bit; }
                    bit++;
                }
            }
            break;
        }
        p++;
    }
    return -1;
}

static int acpi_resolve_link_gsi(const uint8_t link[4])
{
    if (!g_acpi.dsdt_phys) {
        return -1;
    }
    const struct acpi_sdt_hdr *h =
        (const struct acpi_sdt_hdr *)PHYS_TO_VIRT(g_acpi.dsdt_phys);
    if (!acpi_checksum_ok(h, h->length)) {
        return -1;
    }
    const uint8_t *aml = (const uint8_t *)h + sizeof(*h);
    uint32_t len = h->length - sizeof(*h);
    for (uint32_t i = 0; i + 4 + 4 < len; i++) {
        if (memcmp(&aml[i], link, 4) != 0) {
            continue;
        }
        /* 在 NameSeg 之后有限范围内找 "_CRS" */
        for (uint32_t j = i + 4; j + 4 < len && j < i + 0x60; j++) {
            if (aml[j] == 0x5F && aml[j + 1] == 'C' &&
                aml[j + 2] == 'R' && aml[j + 3] == 'S') {
                uint32_t d = j + 4;
                if (d >= len) break;
                if (aml[d] == 0x08) {          /* Name(_CRS, <data>) */
                    uint32_t k = d + 1;
                    int g = acpi_parse_resource_gsi(aml, len, &k);
                    if (g >= 0) return g;
                } else if (aml[d] == 0x14) {   /* Method(_CRS,...) 计算型：不求值 */
                    return -1;
                }
            }
        }
    }
    return -1;
}

/* P0-1/R8：解析 DSDT 中的 _PRT（PCI 根桥 INTx# -> GSI 路由表）。
 * 典型形态：Name(_PRT, Package(){ Package(4){ADDR,PIN,SRC,IDX}, ... })。
 * SRC 为 0（Zero）表示直连 GSI；否则为链接设备 NameSeg（link 模式下通过
 * _CRS 解析 GSI，失败则回落固件预编程的 INT_LINE）。 */
static void acpi_parse_prt(void)
{
    if (!g_acpi.dsdt_phys) {
        return;
    }
    const struct acpi_sdt_hdr *h =
        (const struct acpi_sdt_hdr *)PHYS_TO_VIRT(g_acpi.dsdt_phys);
    if (!acpi_checksum_ok(h, h->length)) {
        kprintf("[acpi] PRT: DSDT bad checksum, skip\n");
        return;
    }
    const uint8_t *aml = (const uint8_t *)h + sizeof(*h);
    uint32_t len = h->length - sizeof(*h);
    uint32_t count = 0;

    for (uint32_t i = 0; i + 4 < len && count < ACPI_PRT_MAX; i++) {
        if (aml[i] != 0x5F || aml[i + 1] != 'P' ||
            aml[i + 2] != 'R' || aml[i + 3] != 'T') {
            continue;                           /* 非 "_PRT" */
        }
        uint32_t p = i + 4;                     /* NameSeg 之后 */
        /* 兼容 Name(_PRT, Package{}) 与 Method(_PRT){Return(Package{})} 两种
         * 形态：在有限窗口内向前找首个 PackageOp(0x12) 并解析为 _PRT 包。 */
        bool prt_found = false;
        for (uint32_t ws = 0; ws <= 0x20 && p + ws < len; ws++) {
            if (aml[p + ws] == 0x12) {
                p = p + ws;
                prt_found = true;
                break;
            }
        }
        if (!prt_found) {
            continue;
        }
        uint32_t outer_start = 0, outer_len = 0;
        outer_len = acpi_pkg_bounds(aml, p, &outer_start);
        uint32_t outer_end = outer_start + outer_len;
        if (outer_end > len) outer_end = len;
        uint32_t k = outer_start;
        uint32_t ne = (k < outer_end) ? aml[k] : 0;   /* 包元素个数 */
        k++;
        for (uint32_t e = 0; e < ne && count < ACPI_PRT_MAX; e++) {
            if (k + 1 >= outer_end) break;
            if (aml[k] != 0x12) { k++; continue; }     /* 期望内层 Package */
            uint32_t in_start = 0, in_len = 0;
            in_len = acpi_pkg_bounds(aml, k, &in_start);
            uint32_t in_end = in_start + in_len;
            if (in_end > outer_end) in_end = outer_end;
            uint32_t f = in_start;
            uint64_t addr = acpi_aml_const(aml, in_end, &f);
            uint64_t pin  = acpi_aml_const(aml, in_end, &f);
            uint8_t link[4] = {0, 0, 0, 0};
            uint32_t gsi_idx = 0;
            if (f < in_end && aml[f] == 0x00) {       /* Zero => 直连 GSI */
                f++;
                gsi_idx = (uint32_t)acpi_aml_const(aml, in_end, &f);
            } else {                                   /* 链接设备 NameSeg */
                if (f + 4 <= in_end) { memcpy(link, &aml[f], 4); f += 4; }
                gsi_idx = (uint32_t)acpi_aml_const(aml, in_end, &f);
            }
            if (count < ACPI_PRT_MAX) {
                g_acpi.prt[count].addr     = (uint32_t)addr;
                g_acpi.prt[count].pin      = (uint8_t)pin;
                memcpy(g_acpi.prt[count].link, link, 4);
                g_acpi.prt[count].gsi_index = gsi_idx;
                count++;
            }
            k = in_end;
        }
        break;                                   /* 仅取第一个 _PRT */
    }
    g_acpi.prt_count = count;
    kprintf("[acpi] PRT: %u route entry(ies)\n", (unsigned)count);
}

/* P0-1/R8：依 _PRT 把 PCI 设备 (bus,dev,pin) 的 INTx#（0=A..3=D）解析为 GSI。
 * 命中直连条目返回 GSI；命中 link 条目且能解析 _CRS 返回 GSI；否则返回 -1
 * （调用方应回落到固件预编程的 INT_LINE 配置寄存器）。 */
int acpi_pci_route(uint8_t bus, uint8_t dev, uint8_t pin)
{
    (void)bus;   /* 单根桥 _PRT 仅按 (dev,pin) 路由，不随 bus 区分 */
    for (uint32_t i = 0; i < g_acpi.prt_count; i++) {
        uint32_t addr = g_acpi.prt[i].addr;
        uint8_t pdev  = (addr >> 16) & 0xFF;
        if (pdev != dev) {
            continue;
        }
        if (g_acpi.prt[i].pin != pin) {
            continue;
        }
        bool is_direct = (g_acpi.prt[i].link[0] == 0 &&
                          g_acpi.prt[i].link[1] == 0 &&
                          g_acpi.prt[i].link[2] == 0 &&
                          g_acpi.prt[i].link[3] == 0);
        if (is_direct) {
            return (int)g_acpi.prt[i].gsi_index;
        }
        int g = acpi_resolve_link_gsi(g_acpi.prt[i].link);
        if (g >= 0) {
            return g;
        }
        return -1;                              /* link 不可解析 => 回落 INT_LINE */
    }
    return -1;
}

/* P0-R8：ACPI S5 软关机。
 *
 * 写 PM1a_CNT（若存在则同时写 PM1b_CNT）：SLP_TYP<<10 | SLP_EN(1<<13)。
 * QEMU 侧对应 hw/acpi/core.c:acpi_pm1_cnt_write —— 检测 SLP_EN 后取
 * (val>>10)&7 与 s4_val/0 比较，命中软关机则 qemu_system_shutdown_request()。
 * QEMU PIIX4 的 pm1_cnt MemoryRegionOps 限定 min/max_access_size = 2，
 * 因此必须用 16 位 outw；32 位 outl 不会命中该寄存器。
 *
 * 写入后保持 vCPU 忙等而非 HLT：关机请求由 QEMU 主循环 BH 处理，立即 HLT
 * 会让 VM 停在 halted 态、进程不退出（本项目实测过该现象）。
 * 若平台未提供 PM1 控制块，则退化为永久停机（不破坏内核现场）。 */
void acpi_poweroff(void)
{
    uint16_t val_a = (uint16_t)(((uint16_t)g_acpi.slp_typ_a << 10) | (1u << 13));
    uint16_t val_b = (uint16_t)(((uint16_t)g_acpi.slp_typ_b << 10) | (1u << 13));
    kprintf("[acpi] poweroff: S5 -> PM1a=0x%x val=0x%x PM1b=0x%x val=0x%x\n",
            (unsigned)g_acpi.pm1a_cnt_blk, (unsigned)val_a,
            (unsigned)g_acpi.pm1b_cnt_blk, (unsigned)val_b);

    if (g_acpi.pm1a_cnt_blk) {
        outw((uint16_t)g_acpi.pm1a_cnt_blk, val_a);
    }
    if (g_acpi.pm1b_cnt_blk) {
        outw((uint16_t)g_acpi.pm1b_cnt_blk, val_b);
    }

    /* 给宿主/固件时间完成断电；保持 vCPU 可运行（见上方注释） */
    for (volatile uint64_t spin = 0; spin < 200000000ULL; spin++) {
        __asm__ volatile("pause");
    }
    kprintf("[acpi] poweroff: platform did not power off, halting\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
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
    acpi_parse_prt();        /* P0-1/R8：解析 _PRT（依赖 FADT 已设 dsdt_phys） */
    kprintf("[acpi] HPET table %sfound @ %p\n",
            g_acpi.hpet_phys ? "" : "NOT ", (void *)g_acpi.hpet_phys);

    return true;
}

uint64_t acpi_find_table(const char *sig)
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
    if (sig_eq(sig, ACPI_SIG_MCFG)) {
        return g_acpi.mcfg_phys;
    }
    return 0;
}
