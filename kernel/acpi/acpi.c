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
