/*
 * kernel/drivers/ahci.c
 * -----------------------------------------------------------------------------
 * AHCI SATA 驱动实现（P0-7：中断驱动 DMA）。见 include/kernel/ahci.h 头注释。
 *
 * 关键物理结构（AHCI 1.3.1 规范）：
 *   - 命令列表 (Command List)：32 个命令头 × 32B = 1KB，1KB 对齐（PxCLB）；
 *   - 接收 FIS 区 (Received FIS)：256B，256B 对齐（PxFB）；
 *   - 命令表 (Command Table)：CFIS(64B)+ACMD(16B)+保留(48B)+PRDT 表项，
 *     128B 对齐（命令头.CTBA）；
 *   - PRDT 表项：DBA(8B)+保留(4B)+DBC|I(4B)，本驱动单表项+4KB 弹跳页。
 * 布局：一页放 CLB(0)+FB(0x400)+CT(0x800)；另一页做 DMA 弹跳缓冲。
 *
 * 中断路径：盘完成 DMA -> D2H FIS -> PxIS.DHRS -> HBA IS.port -> PCI INTx
 *   -> PIIX3 -> IOAPIC(GSI=INT_LINE, 电平低有效) -> 向量 32+GSI
 *   -> ahci_irq_handler：读并 W1C 清 PxIS/IS，置 g_cmd_done。
 * 主流程 ahci_exec()：置 PxCI 后等待 g_cmd_done；同时轮询 PxCI 兜底
 * （中断丢失只降速不失败），TFES 置位判命令错误。
 *
 * 调用关系：kmain -> ahci_init()；ata.c::disk_srv_task -> ahci_read/write。
 */
#include <kernel/ahci.h>
#include <kernel/pci.h>
#include <kernel/console.h>
#include <kernel/string.h>
#include <kernel/interrupts.h>
#include <kernel/ioapic.h>
#include <mm/pmm.h>

/* ---- HBA 全局寄存器（ABAR 偏移） ---- */
#define HBA_CAP        0x00
#define HBA_GHC        0x04
#define HBA_IS         0x08
#define HBA_PI         0x0C
#define GHC_AE         (1u << 31)      /* AHCI Enable */
#define GHC_IE         (1u << 1)       /* 全局中断使能 */

/* ---- 端口寄存器（ABAR + 0x100 + port*0x80 偏移） ---- */
#define PX_CLB         0x00
#define PX_CLBU        0x04
#define PX_FB          0x08
#define PX_FBU         0x0C
#define PX_IS          0x10
#define PX_IE          0x14
#define PX_CMD         0x18
#define PX_TFD         0x20
#define PX_SIG         0x24
#define PX_SSTS        0x28
#define PX_SERR        0x30
#define PX_CI          0x38

#define PXCMD_ST       (1u << 0)       /* Start（命令处理开） */
#define PXCMD_FRE      (1u << 4)       /* FIS Receive Enable */
#define PXCMD_FR       (1u << 14)      /* FIS Receive Running */
#define PXCMD_CR       (1u << 15)      /* Command List Running */

#define PXIS_DHRS      (1u << 0)       /* D2H Register FIS（命令完成） */
#define PXIS_PSS       (1u << 1)
#define PXIS_DPS       (1u << 5)
#define PXIS_TFES      (1u << 30)      /* Task File Error */

#define TFD_BSY        0x80
#define TFD_DRQ        0x08
#define TFD_ERR        0x01

#define SATA_SIG_ATA   0x00000101u     /* SATA 硬盘签名 */
#define FIS_TYPE_H2D   0x27

#define AHCI_SECTOR    512
#define AHCI_MAX_SECT  8               /* 4KB 弹跳页 / 512 */

static volatile uint8_t *g_abar;       /* ABAR MMIO（内核直映虚地址） */
static uint32_t g_port = 32;           /* 选中端口号；32=无 */
static uint64_t g_total_sectors;
static bool     g_present;

/* 物理结构页：页 A = CLB(0)+FB(0x400)+命令表(0x800)；页 B = DMA 弹跳缓冲 */
static uint64_t g_structs_phys, g_bounce_phys;
static uint8_t *g_structs, *g_bounce;

/* 中断完成标志与统计（volatile：IRQ 上下文写，任务上下文读） */
static volatile bool     g_cmd_done;
static volatile uint32_t g_irq_count;

/* ---- MMIO 访问（volatile 保序） ---- */
static inline uint32_t hba_rd(uint32_t off)
{
    return *(volatile uint32_t *)(g_abar + off);
}
static inline void hba_wr(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(g_abar + off) = v;
}
static inline uint32_t px_off(uint32_t reg)
{
    return 0x100 + g_port * 0x80 + reg;
}
static inline uint32_t px_rd(uint32_t reg)  { return hba_rd(px_off(reg)); }
static inline void px_wr(uint32_t reg, uint32_t v) { hba_wr(px_off(reg), v); }

/* TSC 粗延时/超时（clock.c 已校准 TSC，但此处只需量级正确的自旋等待） */
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* 等待寄存器位清零，超时 ~spin 次返回 false */
static bool wait_clear(uint32_t reg, uint32_t mask, uint64_t spins)
{
    for (uint64_t i = 0; i < spins; i++) {
        if (!(px_rd(reg) & mask)) {
            return true;
        }
        __asm__ volatile("pause");
    }
    return false;
}

/* ---- IRQ 处理器：W1C 清端口/全局中断状态，置完成标志 ---- */
static void ahci_irq_handler(registers_t *r)
{
    (void)r;
    uint32_t is = hba_rd(HBA_IS);
    if (is & (1u << g_port)) {
        uint32_t pis = px_rd(PX_IS);
        px_wr(PX_IS, pis);             /* W1C 端口状态 */
        hba_wr(HBA_IS, 1u << g_port);  /* W1C 全局状态 */
        g_irq_count++;
        g_cmd_done = true;             /* 错误也算“完成”，主流程查 TFES/TFD */
    }
}

/* ---- 停止/启动端口命令引擎（AHCI 10.1.2/10.3.1 规定顺序） ---- */
static bool port_stop(void)
{
    px_wr(PX_CMD, px_rd(PX_CMD) & ~PXCMD_ST);
    if (!wait_clear(PX_CMD, PXCMD_CR, 5000000)) {
        return false;
    }
    px_wr(PX_CMD, px_rd(PX_CMD) & ~PXCMD_FRE);
    return wait_clear(PX_CMD, PXCMD_FR, 5000000);
}
static void port_start(void)
{
    while (px_rd(PX_CMD) & PXCMD_CR) {
        __asm__ volatile("pause");
    }
    px_wr(PX_CMD, px_rd(PX_CMD) | PXCMD_FRE);
    px_wr(PX_CMD, px_rd(PX_CMD) | PXCMD_ST);
}

/* ---- 构造并执行一条命令（slot 0，单 PRDT，数据经弹跳页） ----
 * cmd    : ATA 命令码（0x25 DMA READ EXT / 0x35 DMA WRITE EXT / 0xEC IDENTIFY）
 * lba    : 起始扇区；count：扇区数（IDENTIFY 时忽略 LBA/count 填 0）
 * write  : 数据方向（true=主机->盘）
 * bytes  : 传输字节数（必须 ≤ 4096）
 * 返回 true=命令成功完成（无 TFES 且 TFD 无 ERR）。 */
static bool ahci_exec(uint8_t cmd, uint64_t lba, uint16_t count,
                      bool write, uint32_t bytes)
{
    /* 命令头（命令列表槽 0）：
     *   DW0: CFL=5(20B/4) | W<<6 | PRDTL<<16
     *   DW1: PRDBC（HBA 回写实际传输字节，置 0）
     *   DW2/3: CTBA 命令表物理基址 */
    volatile uint32_t *hdr = (volatile uint32_t *)g_structs;
    uint64_t ct_phys = g_structs_phys + 0x800;
    uint32_t prdtl = bytes ? 1u : 0u;    /* FLUSH 等无数据命令 PRDTL=0 */
    hdr[0] = 5u | (write ? (1u << 6) : 0) | (prdtl << 16);
    hdr[1] = 0;
    hdr[2] = (uint32_t)ct_phys;
    hdr[3] = (uint32_t)(ct_phys >> 32);

    /* 命令表：CFIS = H2D Register FIS（20B），PRDT[0] 在偏移 0x80 */
    uint8_t *ct = g_structs + 0x800;
    memset(ct, 0, 0x100);
    ct[0] = FIS_TYPE_H2D;
    ct[1] = 1u << 7;                   /* C=1：命令 FIS */
    ct[2] = cmd;
    ct[4] = (uint8_t)(lba);            /* LBA48 低 24 位 */
    ct[5] = (uint8_t)(lba >> 8);
    ct[6] = (uint8_t)(lba >> 16);
    ct[7] = 0x40;                      /* device：LBA 模式 */
    ct[8] = (uint8_t)(lba >> 24);      /* LBA48 高 24 位 */
    ct[9] = (uint8_t)(lba >> 32);
    ct[10] = (uint8_t)(lba >> 40);
    ct[12] = (uint8_t)(count);         /* 扇区计数 */
    ct[13] = (uint8_t)(count >> 8);

    /* PRDT[0]：DBA=弹跳页，DBC=bytes-1（0 基），I=1（传输完成中断）。
     * 无数据命令（FLUSH）PRDTL=0，跳过。 */
    if (prdtl) {
        volatile uint32_t *prdt = (volatile uint32_t *)(ct + 0x80);
        prdt[0] = (uint32_t)g_bounce_phys;
        prdt[1] = (uint32_t)(g_bounce_phys >> 32);
        prdt[2] = 0;
        prdt[3] = (bytes - 1) | (1u << 31);
    }

    /* 等待端口空闲（BSY|DRQ 清零）后发命令 */
    for (uint64_t i = 0; i < 5000000; i++) {
        if (!(px_rd(PX_TFD) & (TFD_BSY | TFD_DRQ))) {
            break;
        }
        __asm__ volatile("pause");
    }
    g_cmd_done = false;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    px_wr(PX_CI, 1u);                  /* 发槽 0 */

    /* 等待完成：优先中断标志；轮询 PxCI 兜底（约 2 秒 TSC 超时） */
    uint64_t deadline = rdtsc() + 4000000000ULL;
    for (;;) {
        if (g_cmd_done || !(px_rd(PX_CI) & 1u)) {
            break;
        }
        if (px_rd(PX_IS) & PXIS_TFES) {
            break;
        }
        if (rdtsc() > deadline) {
            kprintf("[ahci] command 0x%x timeout (CI=0x%x TFD=0x%x)\n",
                    cmd, px_rd(PX_CI), px_rd(PX_TFD));
            return false;
        }
        __asm__ volatile("pause");
    }
    /* 兜底路径可能未经 IRQ 清状态：此处再 W1C 一次（幂等） */
    uint32_t pis = px_rd(PX_IS);
    if (pis) {
        px_wr(PX_IS, pis);
        hba_wr(HBA_IS, 1u << g_port);
    }
    if ((pis & PXIS_TFES) || (px_rd(PX_TFD) & TFD_ERR)) {
        kprintf("[ahci] command 0x%x error (PxIS=0x%x TFD=0x%x)\n",
                cmd, pis, px_rd(PX_TFD));
        return false;
    }
    return true;
}

bool ahci_init(void)
{
    pci_dev_t d;
    if (!pci_find_class(0x01, 0x06, &d)) {       /* SATA AHCI 控制器 */
        kprintf("[ahci] no AHCI controller on PCI bus\n");
        return false;
    }
    pci_enable_device(&d);                        /* MMIO + 总线主控(DMA) */
    uint64_t abar_phys = pci_bar_addr(&d, 5);     /* ABAR = BAR5 */
    if (!abar_phys) {
        kprintf("[ahci] BAR5 unavailable\n");
        return false;
    }
    g_abar = (volatile uint8_t *)PHYS_TO_VIRT(abar_phys);

    hba_wr(HBA_GHC, hba_rd(HBA_GHC) | GHC_AE);    /* AHCI 使能 */

    /* 选第一个「物理在位(DET=3) 且签名=SATA 盘」的实现端口 */
    uint32_t pi = hba_rd(HBA_PI);
    g_port = 32;
    for (uint32_t p = 0; p < 32; p++) {
        if (!(pi & (1u << p))) {
            continue;
        }
        uint32_t ssts = hba_rd(0x100 + p * 0x80 + PX_SSTS);
        uint32_t sig  = hba_rd(0x100 + p * 0x80 + PX_SIG);
        if ((ssts & 0x0F) == 3 && sig == SATA_SIG_ATA) {
            g_port = p;
            break;
        }
    }
    if (g_port == 32) {
        kprintf("[ahci] controller %04x:%04x present but no SATA disk\n",
                d.vendor_id, d.device_id);
        return false;
    }

    /* 分配物理结构页 + DMA 弹跳页（pmm 返回已清零页；<4GB 由 pmm 布局保证，
     * 且 CLB 1KB/FB 256B/CT 128B 对齐由页对齐自然满足） */
    g_structs_phys = (uint64_t)pmm_alloc_page();
    g_bounce_phys  = (uint64_t)pmm_alloc_page();
    if (!g_structs_phys || !g_bounce_phys) {
        kprintf("[ahci] out of pages\n");
        return false;
    }
    g_structs = (uint8_t *)PHYS_TO_VIRT(g_structs_phys);
    g_bounce  = (uint8_t *)PHYS_TO_VIRT(g_bounce_phys);

    /* 规定顺序：停引擎 -> 设 CLB/FB -> 清 SERR/IS -> 开 FRE+ST */
    if (!port_stop()) {
        kprintf("[ahci] port %u stop timeout\n", g_port);
        return false;
    }
    px_wr(PX_CLB,  (uint32_t)g_structs_phys);
    px_wr(PX_CLBU, (uint32_t)(g_structs_phys >> 32));
    px_wr(PX_FB,   (uint32_t)(g_structs_phys + 0x400));
    px_wr(PX_FBU,  (uint32_t)((g_structs_phys + 0x400) >> 32));
    px_wr(PX_SERR, 0xFFFFFFFFu);
    px_wr(PX_IS,   0xFFFFFFFFu);
    port_start();

    /* 中断路由：SeaBIOS 已把 GSI 写入 PCI INT_LINE；i440FX/PIIX3 下 PCI
     * INTx 为电平低有效。向量取 32+GSI（IDT 前 48 stub 已就位）。 */
    uint8_t gsi = pci_cfg_read8(d.bus, d.dev, d.func, PCI_CFG_INT_LINE);
    if (gsi > 0 && gsi < 16) {
        register_interrupt_handler(32 + gsi, ahci_irq_handler);
        ioapic_route(gsi, 32 + gsi, true /*level*/, true /*active_low*/, 0);
        px_wr(PX_IE, PXIS_DHRS | PXIS_PSS | PXIS_DPS | PXIS_TFES);
        hba_wr(HBA_GHC, hba_rd(HBA_GHC) | GHC_IE);
        kprintf("[ahci] IRQ routed: GSI %u -> vector %u (level, active-low)\n",
                gsi, 32 + gsi);
    } else {
        kprintf("[ahci] no valid INT_LINE, polling mode only\n");
    }

    /* IDENTIFY DEVICE：取容量（LBA48 word100-103 优先，回退 word60-61） */
    if (!ahci_exec(0xEC, 0, 0, false, 512)) {
        kprintf("[ahci] IDENTIFY failed\n");
        return false;
    }
    uint16_t *id = (uint16_t *)g_bounce;
    uint64_t lba28 = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
    uint64_t lba48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16)
                   | ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    g_total_sectors = ((id[83] & 0x0400) && lba48) ? lba48 : lba28;

    /* 自检：读 LBA0 验证 MBR/引导扇区 0x55AA 签名（证明 DMA 通路正确） */
    if (!ahci_read_sectors(0, 1, g_structs + 0xC00)) {   /* 借结构页尾暂存 */
        kprintf("[ahci] LBA0 selftest read failed\n");
        return false;
    }
    uint8_t *mbr = g_structs + 0xC00;
    bool sig_ok = (mbr[510] == 0x55 && mbr[511] == 0xAA);

    /* 写路径自检：末扇区「读原值 -> 写图案 -> 读回校验 -> 恢复原值」。
     * 全程可恢复（图案只在校验窗口存在），验证 WRITE DMA EXT + FLUSH。 */
    bool wr_ok = false;
    if (g_total_sectors > 0) {
        uint64_t last = g_total_sectors - 1;
        uint8_t *orig = g_structs + 0xC00;         /* 结构页尾暂存（512B） */
        uint8_t  patt[512];
        if (ahci_read_sectors(last, 1, orig)) {
            for (int i = 0; i < 512; i++) {
                patt[i] = (uint8_t)(i ^ 0x5A);
            }
            uint8_t back[512];
            wr_ok = ahci_write_sectors(last, 1, patt)
                 && ahci_read_sectors(last, 1, back)
                 && memcmp(patt, back, 512) == 0
                 && ahci_write_sectors(last, 1, orig);   /* 恢复 */
        }
    }

    g_present = true;
    kprintf("[ahci] port %u online: %lu sectors (%lu MiB), LBA0 sig %s, "
            "wr-test %s, irqs=%u\n",
            g_port, (unsigned long)g_total_sectors,
            (unsigned long)(g_total_sectors / 2048),
            sig_ok ? "OK(55AA)" : "absent",
            wr_ok ? "PASS" : "FAIL", g_irq_count);
    return true;
}

bool ahci_present(void)
{
    return g_present;
}

uint64_t ahci_total_sectors(void)
{
    return g_total_sectors;
}

bool ahci_read_sectors(uint64_t lba, uint32_t count, void *buf)
{
    if ((!g_present && !g_total_sectors) || count == 0 ||
        count > AHCI_MAX_SECT) {
        return false;
    }
    if (g_total_sectors && (lba + count > g_total_sectors ||
                            lba + count < lba)) {
        return false;                              /* 越界/回绕防护 */
    }
    uint32_t bytes = count * AHCI_SECTOR;
    if (!ahci_exec(0x25, lba, (uint16_t)count, false, bytes)) {
        return false;                              /* READ DMA EXT */
    }
    memcpy(buf, g_bounce, bytes);
    return true;
}

bool ahci_write_sectors(uint64_t lba, uint32_t count, const void *buf)
{
    if ((!g_present && !g_total_sectors) || count == 0 ||
        count > AHCI_MAX_SECT) {
        return false;
    }
    if (g_total_sectors && (lba + count > g_total_sectors ||
                            lba + count < lba)) {
        return false;
    }
    uint32_t bytes = count * AHCI_SECTOR;
    memcpy(g_bounce, buf, bytes);
    if (!ahci_exec(0x35, lba, (uint16_t)count, true, bytes)) {
        return false;                              /* WRITE DMA EXT */
    }
    /* FLUSH CACHE EXT：确保落盘（掉电安全，与 ata.c 语义一致） */
    return ahci_exec(0xEA, 0, 0, false, 0);
}
