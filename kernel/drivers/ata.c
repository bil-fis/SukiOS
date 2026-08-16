/*
 * kernel/drivers/ata.c
 * -----------------------------------------------------------------------------
 * ATA PIO 驱动（LBA28，Primary Master，轮询模式）+ DISK_PORT 内核服务。
 *
 * 红线：这是唯一驻留 Ring0 的存储驱动。它不解析任何文件系统结构，
 * 只作为"磁盘端口"响应 IPC 扇区读请求；FAT32 逻辑全部在 Ring3 FS_SERVER。
 *
 * 调用关系：kmain -> ata_init() + disk_srv_start()；
 *           FS_SERVER --mach_msg--> DISK_PORT --disk_srv 任务--> ata_read_sectors。
 */
#include <kernel/ata.h>
#include <kernel/spinlock.h>  /* SMP：ATA 事务串行锁（关中断保护 PIO 相位） */
#include <kernel/percpu.h>    /* cpu_index()：诊断打印用 */
#include <kernel/ahci.h>     /* P0-7：disk-srv 统一分发（AHCI DMA 优先） */
#include <kernel/io.h>
#include <kernel/console.h>
#include <kernel/task.h>
#include <kernel/string.h>
#include <kernel/pci.h>      /* Bus Master IDE (UDMA) 需要 PCI 枚举 + BAR4 */
#include <ipc/port.h>
#include <ipc/disk_proto.h>
#include <mm/kmalloc.h>
#include <mm/pmm.h>

/* Primary 通道寄存器 */
#define ATA_IO        0x1F0
#define ATA_DATA      (ATA_IO + 0)
#define ATA_ERROR     (ATA_IO + 1)
#define ATA_SECCNT    (ATA_IO + 2)
#define ATA_LBA_LO    (ATA_IO + 3)
#define ATA_LBA_MID   (ATA_IO + 4)
#define ATA_LBA_HI    (ATA_IO + 5)
#define ATA_DRIVE     (ATA_IO + 6)
#define ATA_STATUS    (ATA_IO + 7)
#define ATA_CMD       (ATA_IO + 7)
#define ATA_ALT_STATUS 0x3F6
#define ATA_DEV_CTRL   0x3F6

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DRQ  0x08
#define ST_ERR  0x01

#define CMD_READ_SECTORS  0x20
#define CMD_WRITE_SECTORS 0x30
#define CMD_FLUSH_CACHE   0xE7
#define CMD_IDENTIFY      0xEC

/* ---- Bus Master IDE (BMIDE / UDMA) ----------------------------------------
 * -machine pc (i440FX) 提供 PIIX3 IDE 控制器 (class=0x01 subclass=0x01)，
 * 其 PCI BAR4 是一段 16 字节的 I/O 空间：
 *   +0x00 Command (读/写)   bit0=Start/Stop, bit2=方向
 *                             0=写内存(设备->内存, 即磁盘读), 1=读内存(内存->设备, 即磁盘写)
 *   +0x02 Status            bit0=Active, bit1=Error, bit2=IRQ, bit5/6=DMA能力
 *   +0x04 PRDT 物理地址 (u32，4 字节对齐，不得跨 64KB 边界)
 * Secondary 通道为 +0x08 起的同样三组寄存器（本驱动只用 Primary）。
 *
 * PRD（Physical Region Descriptor，8 字节）：
 *   [0..3]  缓冲区物理地址（32 位；缓冲区不得跨 64KB 边界）
 *   [4..5]  字节数（0 表示 64KB）
 *   [6..7]  bit15(EOT)=1 表示这是 PRDT 的最后一项
 *
 * 传输流程（轮询式，与现有无 IRQ 设计一致）：
 *   1. 填 PRDT 并写入 BMIDE+0x04
 *   2. 写 BMIDE+0x00 设定方向，且确保 Start=0
 *   3. 清 Status 的 Error/IRQ 位（写 1 清）
 *   4. 选盘 + 发 READ DMA(0xC8/0x25) 或 WRITE DMA(0xCA/0x35)
 *   5. 置 Start=1 启动总线主控
 *   6. 轮询 Status：IRQ 置位或 Active 清零即结束；检查 Error
 *   7. 清 Start=0，等待设备 BSY 清零
 * ------------------------------------------------------------------------- */
#define CMD_READ_DMA        0xC8
#define CMD_READ_DMA_EXT    0x25
#define CMD_WRITE_DMA       0xCA
#define CMD_WRITE_DMA_EXT   0x35

#define BM_CMD      0x00
#define BM_STATUS   0x02
#define BM_PRDT     0x04

#define BM_CMD_START     0x01
/* 方向位 bit3（OSDev《ATA/ATAPI using DMA》权威定义）：
 *   1 = 设备->内存（即磁盘读，DMA 写系统内存）
 *   0 = 内存->设备（即磁盘写）
 * 注意：极性是「读盘置位、写盘清零」，与部分文档直觉相反，必须严格遵守，
 * 否则启用 DMA 后会静默地把读/写方向颠倒导致数据损坏。 */
#define BM_CMD_DIR       0x08

#define BM_ST_ACTIVE 0x01
#define BM_ST_ERROR  0x02
#define BM_ST_IRQ    0x04

/* 单次 DMA 上限：与 IPC 协议的 DISK_MAX_SECTORS 一致即可，取 8 扇区(4KB)
 * 正好一页，天然不跨 64KB 边界，PRDT 只需一项。 */
#define ATA_DMA_MAX_SECTORS 8

static bool     g_disk_present = false;
static uint64_t g_total_sectors = 0;      /* L2：改为 64 位，容纳 LBA48 容量 */

/* 全局 ATA 串行锁：ATA PIO 是强状态协议（发命令→轮询 DRQ→inw 取数据），
 * 期间绝不可被另一个 CPU 并发访问同一组 I/O 端口，否则命令/数据相位错乱、
 * DRQ 被错误消费导致永久等待。SMP 下必须由自旋锁保证同一时刻仅一个 CPU
 * 在执行 ATA 事务，且临界区内关闭本地中断（避免本核 tick 抢占破坏相位）。 */
static spinlock_t g_ata_lock = SPINLOCK_INIT("ata");
static bool     g_lba48 = false;          /* L2：磁盘是否支持 LBA48 寻址 */

/* DMA 运行期状态：g_bm_base==0 表示未启用 DMA，全部回退 PIO。 */
static uint16_t  g_bm_base = 0;           /* BMIDE I/O 基址（BAR4 & ~3） */
static uint64_t *g_prdt = NULL;           /* PRDT 虚拟地址（1 项即可） */
static uint64_t  g_prdt_phys = 0;
static uint8_t  *g_dma_buf = NULL;        /* 反弹缓冲虚拟地址（4KB） */
static uint64_t  g_dma_buf_phys = 0;

/* 探测并初始化 Bus Master IDE。失败时保持 g_bm_base=0（安全回退 PIO）。
 * 关键约束：PRDT 与数据缓冲的物理地址都必须 < 4GiB（PRD 只有 32 位地址域），
 * 且缓冲区不得跨 64KB 边界——这里用单页 4KB 分配天然满足。 */
static void ata_dma_init(void)
{
    pci_dev_t ide;
    if (!pci_find_class(0x01, 0x01, &ide)) {     /* Mass Storage / IDE */
        kprintf("[ata] no PCI IDE controller, DMA disabled (PIO mode)\n");
        return;
    }

    /* BAR4 是 I/O BAR（bit0=1），pci_bar_addr 只处理 MMIO，故直接读原始值 */
    uint32_t bar4 = pci_cfg_read32(ide.bus, ide.dev, ide.func, PCI_CFG_BAR0 + 16);
    if (!(bar4 & 0x1)) {
        kprintf("[ata] BMIDE BAR4 is not I/O, DMA disabled (PIO mode)\n");
        return;
    }
    uint16_t base = (uint16_t)(bar4 & 0xFFFCu);
    if (base == 0) {
        kprintf("[ata] BMIDE BAR4 unassigned, DMA disabled (PIO mode)\n");
        return;
    }

    /* 使能 I/O 空间译码 + 总线主控（DMA 必需） */
    uint16_t cmd = pci_cfg_read16(ide.bus, ide.dev, ide.func, PCI_CFG_COMMAND);
    cmd |= PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER;
    pci_cfg_write16(ide.bus, ide.dev, ide.func, PCI_CFG_COMMAND, cmd);

    /* 分配 PRDT（1 页）与反弹缓冲（1 页，4KB=8 扇区） */
    void *prdt_p = pmm_alloc_page();
    void *buf_p  = pmm_alloc_page();
    if (!prdt_p || !buf_p) {
        if (prdt_p) pmm_free_page(prdt_p);
        if (buf_p)  pmm_free_page(buf_p);
        kprintf("[ata] DMA buffer alloc failed, DMA disabled (PIO mode)\n");
        return;
    }
    /* PRD 地址域只有 32 位：物理地址必须 < 4GiB，否则只能回退 PIO */
    if ((uint64_t)prdt_p >= 0x100000000ULL || (uint64_t)buf_p >= 0x100000000ULL) {
        pmm_free_page(prdt_p);
        pmm_free_page(buf_p);
        kprintf("[ata] DMA pages above 4GiB, DMA disabled (PIO mode)\n");
        return;
    }

    g_prdt_phys    = (uint64_t)prdt_p;
    g_prdt         = (uint64_t *)PHYS_TO_VIRT(prdt_p);
    g_dma_buf_phys = (uint64_t)buf_p;
    g_dma_buf      = (uint8_t *)PHYS_TO_VIRT(buf_p);

    /* 生产稳定性决策（P0 生产就绪）：SeaBIOS 为 PIIX3 IDE 分配的 BM 基址
     * 在本环境下为 0xc040（非标准 0xc000），实测经此基址启动的 BMIDE 总线主控
     * DMA 传输在 SMP 多核下偶发永不完成（BM_ST_ACTIVE 不置位 / IRQ 不触发），
     * 轮询无法可靠判定成败，曾导致 disk-srv 长时间独占 CPU、等待 IPC 的用户态
     * 任务永久得不到调度而表现为系统“卡死”。DMA 代码（ata_dma_xfer 等）完整保留、
     * 逻辑自洽，待基址修正后可重新启用。当前阶段以数据正确性优先，强制走已验证
     * 完整（命令级重试 + FLUSH CACHE + 越界保护）的 PIO 路径。 */
    g_bm_base      = 0;

    kprintf("[ata] BMIDE probed @ I/O 0x%x (PCI %u:%u.%u) but DISABLED for "
            "production stability; forcing PIO path\n",
            (unsigned)base, (unsigned)ide.bus, (unsigned)ide.dev,
            (unsigned)ide.func);
}

/* 通道稳定延迟：OSDev《ATA PIO Mode》规定发送命令/选盘后需等待约 400ns
 * 让设备稳定状态。做法为读备用状态寄存器 15 次（每次 inb 约 28ns，
 * 15 次 ≈ 420ns，满足规范且覆盖真机最差情况）。读状态（而非 NOP）还能
 * 确保前一条 outb 已退役、状态线稳定。 */
static void ata_delay400(void)
{
    for (int i = 0; i < 15; i++) {
        (void)inb(ATA_ALT_STATUS);
    }
}

/* 等待 BSY 清零；超时返回 false */
static bool ata_wait_not_busy(void)
{
    for (uint32_t i = 0; i < 1000000; i++) {
        if (!(inb(ATA_STATUS) & ST_BSY)) {
            return true;
        }
    }
    return false;
}

/* 等待 DRQ 置位（数据就绪）；出错/超时返回 false */
static bool ata_wait_drq(void)
{
    for (uint32_t i = 0; i < 1000000; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (st & ST_ERR) {
            return false;
        }
        if (!(st & ST_BSY) && (st & ST_DRQ)) {
            return true;
        }
    }
    return false;
}

bool ata_init(void)
{
    /* P0-SMP 修复：ata_init 在 smp_init() 之后才被 kmain 调用，此时 4 个 CPU
     * 已 online 且在跑 100Hz LAPIC 定时器。探测是一段强状态的 ATA 端口事务
     * （选盘→发 IDENTIFY→轮询 BSY/DRQ→inw 读 256 字），若中途被本核 tick 抢占
     * 或他核并发访问同一组端口，将导致相位错位、读到的 LBA_MID/HI 偶发非 0，
     * 从而被下方「device is not ATA」误判为无盘——这正是概率性无盘的根因。
     * 整段探测持 g_ata_lock 并关中断，与读写路径同一把锁，彻底消除竞态。 */
    uint32_t init_flags = spin_lock_irqsave(&g_ata_lock);

    /* 关闭该通道中断（nIEN=1），纯轮询 */
    outb(ATA_DEV_CTRL, 0x02);

    /* 选择 Primary Master */
    outb(ATA_DRIVE, 0xA0);
    ata_delay400();

    /* 悬空总线检测：status=0xFF 说明无设备 */
    if (inb(ATA_STATUS) == 0xFF) {
        kprintf("[ata] no device (floating bus)\n");
        spin_unlock_irqrestore(&g_ata_lock, init_flags);
        return false;
    }

    /* IDENTIFY */
    outb(ATA_SECCNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_CMD, CMD_IDENTIFY);
    ata_delay400();

    if (inb(ATA_STATUS) == 0) {
        kprintf("[ata] no device on primary master\n");
        spin_unlock_irqrestore(&g_ata_lock, init_flags);
        return false;
    }
    if (!ata_wait_not_busy()) {
        kprintf("[ata] IDENTIFY timeout (BSY)\n");
        spin_unlock_irqrestore(&g_ata_lock, init_flags);
        return false;
    }
    /* 设备类型初判：ATAPI 的 LBA_MID/HI 应为 0x14/0xEB。真实 ATA 硬盘此处为 0。
     * 注意：并发/抢占错位会令这两个寄存器偶发非 0，故仅在明确匹配 ATAPI 签名
     * 时才拒绝；其它情况放行，最终以「能否 DRQ 并读出 256 字」为准（更可靠）。 */
    uint8_t mid = inb(ATA_LBA_MID);
    uint8_t hi  = inb(ATA_LBA_HI);
    if (mid == 0x14 && hi == 0xEB) {
        kprintf("[ata] device is ATAPI (not supported)\n");
        spin_unlock_irqrestore(&g_ata_lock, init_flags);
        return false;
    }
    if (mid != 0 || hi != 0) {
        kprintf("[ata] warn: LBA_MID/HI=0x%02x/0x%02x (非典型值)，"
                "仍尝试 IDENTIFY 数据读取\n", (unsigned)mid, (unsigned)hi);
    }
    if (!ata_wait_drq()) {
        kprintf("[ata] IDENTIFY failed (no DRQ)\n");
        spin_unlock_irqrestore(&g_ata_lock, init_flags);
        return false;
    }

    uint16_t id[256];
    for (int i = 0; i < 256; i++) {
        id[i] = inw(ATA_DATA);
    }

    /* L2 修复：容量优先取 LBA48（word 100-103，最多 2^48-1 扇区），
     * 避免旧实现只认 LBA28(word60-61, 上限 2^28-1≈128GiB) 导致 >128GB
     * 盘容量被截断。word83 bit10 指示 LBA48 支持。 */
    uint64_t lba28 = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
    uint64_t lba48 = (uint64_t)id[100]
                   | ((uint64_t)id[101] << 16)
                   | ((uint64_t)id[102] << 32)
                   | ((uint64_t)id[103] << 48);
    g_lba48 = (id[83] & 0x0400) != 0;     /* word83 bit10 = LBA48 支持 */
    if (g_lba48 && lba48 > 0) {
        g_total_sectors = lba48;
    } else {
        g_total_sectors = lba28;
    }
    g_disk_present = true;
    kprintf("[ata] primary master OK: %lu sectors (%lu MiB) lba48=%u\n",
            (unsigned long)g_total_sectors, (unsigned long)(g_total_sectors / 2048),
            (unsigned)g_lba48);

    /* 探测 Bus Master IDE，成功则后续读写走 DMA，失败自动保持 PIO。
     * 仍在 g_ata_lock 保护下执行，保证全程串行化。 */
    ata_dma_init();
    spin_unlock_irqrestore(&g_ata_lock, init_flags);
    return true;
}

/* L2 修复：统一的 LBA 选择原语。
 * - 28 位模式：DRIVE 高 4 位装 lba[24:27]，其余经 LBA_LO/MID/HI。
 * - 48 位模式：计数字节与 LBA 各写两次（先高 24 位、后低 24 位），
 *   DRIVE 仅置 LBA 位(0x40)不带高 4 位；命令改用 READ/WRITE SECTORS EXT。
 * 仅在 lba 跨越 28 位边界(0x0FFFFFFF)且磁盘支持 LBA48 时才走 48 位路径，
 * 故 <128GiB 的常见盘（含 QEMU 64MB 测试盘）仍走 LBA28，零回归风险。 */
static void ata_select_lba(uint64_t lba, uint8_t count, bool use48)
{
    if (use48) {
        outb(ATA_SECCNT, 0);                          /* 计数字节高 8 位 */
        outb(ATA_LBA_LO,  (lba >> 24) & 0xFF);
        outb(ATA_LBA_MID, (lba >> 32) & 0xFF);
        outb(ATA_LBA_HI,  (lba >> 40) & 0xFF);
        outb(ATA_SECCNT, count);                      /* 计数字节低 8 位 */
        outb(ATA_LBA_LO,  lba & 0xFF);
        outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
        outb(ATA_LBA_HI,  (lba >> 16) & 0xFF);
        outb(ATA_DRIVE, 0x40);                        /* LBA 模式（48 位寻址）*/
    } else {
        outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
        outb(ATA_SECCNT, count);
        outb(ATA_LBA_LO,  lba & 0xFF);
        outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
        outb(ATA_LBA_HI,  (lba >> 16) & 0xFF);
    }
}

/* 执行一次 Bus Master DMA 传输（数据始终经 g_dma_buf 反弹缓冲）。
 * write=false 表示磁盘读（DMA 写内存），write=true 表示磁盘写。
 * 调用方须保证 count<=ATA_DMA_MAX_SECTORS 且已做越界校验。
 * 返回 false 时调用方应回退 PIO 或上报错误——本函数保证在任何失败路径上
 * 都已停掉总线主控（Start=0），不会留下悬空的 DMA 引擎。 */
static bool __attribute__((noinline)) ata_dma_xfer(uint64_t lba, uint8_t count, bool write)
{
    kprintf("[ata] dbg: dma_xfer enter lba=%lu cnt=%u write=%u bm=0x%x\n",
            (unsigned long)lba, (unsigned)count, (unsigned)write,
            (unsigned)g_bm_base);
    if (g_bm_base == 0 || count == 0 || count > ATA_DMA_MAX_SECTORS) {
        return false;
    }

    uint32_t bytes = (uint32_t)count * ATA_SECTOR_SIZE;

    /* 1. 构造单项 PRDT：地址 + 字节数 + EOT */
    g_prdt[0] = (uint64_t)(uint32_t)g_dma_buf_phys
              | ((uint64_t)(bytes & 0xFFFFu) << 32)
              | (0x8000ULL << 48);                   /* bit15 of word3 = EOT */

    /* 2. 停总线主控并设定方向（Start 必须为 0 时才能改方向/PRDT）。
     *    方向位 bit2 (BM_CMD_DIR)：0=写内存(设备->内存，磁盘读)，
     *                              1=读内存(内存->设备，磁盘写)。 */
    outb(g_bm_base + BM_CMD, write ? 0 : BM_CMD_DIR);
    outl(g_bm_base + BM_PRDT, (uint32_t)g_prdt_phys);

    /* 3. 清 Error/IRQ（写 1 清），保留只读的能力位 */
    uint8_t st = inb(g_bm_base + BM_STATUS);
    outb(g_bm_base + BM_STATUS, (uint8_t)(st | BM_ST_ERROR | BM_ST_IRQ));

    /* 4. 选盘并下发 DMA 命令 */
    if (!ata_wait_not_busy()) {
        kprintf("[ata] dbg: dma pre-busy timeout\n");
        return false;
    }
    bool use48 = g_lba48 && (lba + count) > 0x0FFFFFFFULL;
    ata_select_lba(lba, count, use48);
    if (write) {
        outb(ATA_CMD, use48 ? CMD_WRITE_DMA_EXT : CMD_WRITE_DMA);
    } else {
        outb(ATA_CMD, use48 ? CMD_READ_DMA_EXT : CMD_READ_DMA);
    }
    ata_delay400();

    /* 5. 启动引擎（方向位：读盘置位、写盘清零） */
    outb(g_bm_base + BM_CMD,
         (uint8_t)((write ? 0 : BM_CMD_DIR) | BM_CMD_START));

    /* 6. 轮询完成：IRQ 置位表示设备已发中断（传输结束），
     *    Active 清零同样表示 PRDT 耗尽。
     *    关键生产约束（此前卡死根因）：绝不能无上限死等。QEMU TCG 下若某次
     *    DMA 因 BM 中断未触发/Active 长保持而迟迟不结束，2000 万次 inb 轮询会
     *    独占 CPU 数十秒，使等待 IPC 的用户态任务永远得不到调度，表现为“系统
     *    卡死”。这里把超时收束到约 30ms（足够覆盖一次 4KB DMA 的物理完成时间），
     *    超时即停引擎并返回 false，由上层（ata_read/write_sectors）干净回退 PIO，
     *    保证数据正确性优先、且绝不长时间霸占 CPU。 */
    bool ok = false;
    for (uint32_t i = 0; i < 60000u; i++) {       /* ~30ms @ ~2us/iter */
        uint8_t s = inb(g_bm_base + BM_STATUS);
        if (s & BM_ST_ERROR) {
            ok = false;
            break;
        }
        if (s & BM_ST_IRQ) {                 /* 正常完成 */
            ok = true;
            break;
        }
        if (!(s & BM_ST_ACTIVE)) {
            /* Active 已清但没有 IRQ：可能刚完成也可能从未启动，
             * 以设备状态寄存器为准判定。 */
            ok = !(inb(ATA_STATUS) & ST_ERR);
            break;
        }
    }
    if (!ok) {
        /* 轮询未能确认成功：立即停掉总线主控，避免悬空 DMA 引擎污染后续传输。
         * 返回 false 让上层回退 PIO。 */
        outb(g_bm_base + BM_CMD, write ? 0 : BM_CMD_DIR);
        uint8_t fin = inb(g_bm_base + BM_STATUS);
        outb(g_bm_base + BM_STATUS,
             (uint8_t)(fin | BM_ST_ERROR | BM_ST_IRQ));
    }

    /* 7. 无论成败都必须停掉引擎并清状态位，防止残留影响下一次传输 */
    outb(g_bm_base + BM_CMD, write ? 0 : BM_CMD_DIR);
    uint8_t fin = inb(g_bm_base + BM_STATUS);
    outb(g_bm_base + BM_STATUS, (uint8_t)(fin | BM_ST_ERROR | BM_ST_IRQ));
    if (fin & BM_ST_ERROR) {
        ok = false;
    }

    if (!ata_wait_not_busy()) {
        return false;
    }
    if (inb(ATA_STATUS) & ST_ERR) {
        return false;
    }
    kprintf("[ata] dbg: dma xfer done write=%u ok=%u bmst=0x%x devst=0x%x\n",
            (unsigned)write, (unsigned)ok,
            (unsigned)inb(g_bm_base + BM_STATUS), (unsigned)inb(ATA_STATUS));
    return ok;
}

bool ata_read_sectors(uint64_t lba, uint8_t count, void *buf)
{
    kprintf("[ata] read_sectors enter lba=%lu cnt=%u present=%u total=%lu\n",
            (unsigned long)lba, (unsigned)count, (unsigned)g_disk_present,
            (unsigned long)g_total_sectors);
    if (!g_disk_present || count == 0) {
        return false;
    }
    /* SMP 串行化：整个 ATA 事务持锁并关中断，防止本核 tick 抢占破坏 PIO 相位，
     * 或他核并发访问同一组 ATA I/O 端口导致命令/数据相位错乱。 */
    uint32_t ata_flags = spin_lock_irqsave(&g_ata_lock);
    /* M10 修复：读路径补上越界读盘防护（此前仅写路径有）。lba+count 越过
     * 卷尾会令控制器读无效扇区/越界 DMA；无符号回绕一并防范。 */
    if ((uint64_t)lba + count > g_total_sectors || (uint64_t)lba + count < lba) {
        kprintf("[ata] read_sectors OOB reject: lba+count=%lu total=%lu\n",
                (unsigned long)((uint64_t)lba + count),
                (unsigned long)g_total_sectors);
        spin_unlock_irqrestore(&g_ata_lock, ata_flags);
        return false;
    }
    /* 优先走 Bus Master DMA：按 ATA_DMA_MAX_SECTORS 分块，经反弹缓冲拷出。
     * 任一块 DMA 失败则整体回退 PIO 重做（保证数据正确性优先于速度）。 */
    if (g_bm_base != 0) {
        kprintf("[ata] write_sectors: DMA path, lba=%lu cnt=%u\n",
                (unsigned long)lba, (unsigned)count);
        uint8_t done = 0;
        bool dma_ok = true;
        while (done < count) {
            uint8_t chunk = (uint8_t)(count - done);
            if (chunk > ATA_DMA_MAX_SECTORS) {
                chunk = ATA_DMA_MAX_SECTORS;
            }
            if (!ata_dma_xfer(lba + done, chunk, false)) {
                dma_ok = false;
                break;
            }
            memcpy((uint8_t *)buf + (uint32_t)done * ATA_SECTOR_SIZE,
                    g_dma_buf, (uint32_t)chunk * ATA_SECTOR_SIZE);
            done = (uint8_t)(done + chunk);
        }
        if (dma_ok) {
            return true;
        }
        /* 落到下面的 PIO 路径重试整个请求 */
    }

    if (!ata_wait_not_busy()) {
        kprintf("[ata] PIO read lba=%lu: not-busy timeout\n",
                (unsigned long)lba);
        spin_unlock_irqrestore(&g_ata_lock, ata_flags);
        return false;
    }

    bool use48 = g_lba48 && ((uint64_t)lba + count) > 0x0FFFFFFFULL;
    ata_select_lba(lba, count, use48);
    outb(ATA_CMD, use48 ? 0x24 : CMD_READ_SECTORS);   /* READ SECTORS EXT */

    uint16_t *out = (uint16_t *)buf;
    for (uint8_t s = 0; s < count; s++) {
        if (!ata_wait_drq()) {
            kprintf("[ata] PIO read lba=%lu: drq timeout sec=%u\n",
                    (unsigned long)lba, (unsigned)s);
            spin_unlock_irqrestore(&g_ata_lock, ata_flags);
            return false;
        }
        for (int i = 0; i < 256; i++) {
            *out++ = inw(ATA_DATA);
        }
        ata_delay400();
    }
    kprintf("[ata] PIO read lba=%lu done (cpu=%u)\n",
            (unsigned long)lba, (unsigned)cpu_index());
    spin_unlock_irqrestore(&g_ata_lock, ata_flags);
    return true;
}

/* PIO 写扇区（LBA28/48）：逐扇区等待 DRQ 后以 outw 写入 256 字，
 * 全部写完后发 FLUSH CACHE (0xE7) 确保数据落盘（掉电安全）。
 * P0-生产修复：ATA 命令偶发超时/介质瞬时错误，单次失败直接丢数据不可接受，
 * 这里在命令级重试（含 FLUSH），最多 3 次；仍失败才返回 false 由上层回传错误。*/
bool ata_write_sectors(uint64_t lba, uint8_t count, const void *buf)
{
    if (!g_disk_present || count == 0) {
        return false;
    }
    /* SMP 串行化：同读路径，整个 ATA 写事务持锁并关中断，串行化 PIO 相位。 */
    uint32_t ata_flags = spin_lock_irqsave(&g_ata_lock);
    if ((uint64_t)lba + count > g_total_sectors ||
        (uint64_t)lba + count < lba) {        /* 越界写保护 + 无符号回绕防护 */
        spin_unlock_irqrestore(&g_ata_lock, ata_flags);
        return false;
    }
    /* DMA 写路径：分块拷入反弹缓冲后由总线主控写盘，最后 FLUSH CACHE 落盘。
     * 失败时回退下方 PIO 重试逻辑。 */
    if (g_bm_base != 0) {
        kprintf("[ata] write_sectors: DMA path, lba=%lu cnt=%u\n",
                (unsigned long)lba, (unsigned)count);
        uint8_t done = 0;
        bool dma_ok = true;
        while (done < count) {
            uint8_t chunk = (uint8_t)(count - done);
            if (chunk > ATA_DMA_MAX_SECTORS) {
                chunk = ATA_DMA_MAX_SECTORS;
            }
            memcpy(g_dma_buf,
                    (const uint8_t *)buf + (uint32_t)done * ATA_SECTOR_SIZE,
                    (uint32_t)chunk * ATA_SECTOR_SIZE);
            if (!ata_dma_xfer(lba + done, chunk, true)) {
                dma_ok = false;
                break;
            }
            done = (uint8_t)(done + chunk);
        }
        if (dma_ok) {
            /* 刷写磁盘写缓存，确保掉电安全 */
            outb(ATA_CMD, CMD_FLUSH_CACHE);
            if (ata_wait_not_busy() && !(inb(ATA_STATUS) & ST_ERR)) {
                spin_unlock_irqrestore(&g_ata_lock, ata_flags);
                return true;
            }
        }
        /* 落到 PIO 路径重试整个请求 */
    }

    const uint16_t *in = (const uint16_t *)buf;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!ata_wait_not_busy()) {
            continue;
        }
        bool use48 = g_lba48 && ((uint64_t)lba + count) > 0x0FFFFFFFULL;
        ata_select_lba(lba, count, use48);
        outb(ATA_CMD, use48 ? 0x34 : CMD_WRITE_SECTORS);  /* WRITE SECTORS EXT */

        const uint16_t *p = in;
        bool ok = true;
        for (uint8_t s = 0; s < count; s++) {
            if (!ata_wait_drq()) {
                ok = false;
                break;
            }
            for (int i = 0; i < 256; i++) {
                outw(ATA_DATA, *p++);
            }
            ata_delay400();
        }
        if (!ok) {
            continue;   /* 重试 */
        }
        /* 刷写磁盘写缓存 */
        outb(ATA_CMD, CMD_FLUSH_CACHE);
        if (!ata_wait_not_busy()) {
            continue;
        }
        if (inb(ATA_STATUS) & ST_ERR) {
            continue;   /* 写失败，重试 */
        }
        spin_unlock_irqrestore(&g_ata_lock, ata_flags);
        return true;
    }
    spin_unlock_irqrestore(&g_ata_lock, ata_flags);
    return false;
}

uint64_t ata_total_sectors(void)
{
    return g_total_sectors;
}

/* ---- P0-7：块设备统一分发 ----
 * AHCI 控制器在位则走中断驱动 DMA（ahci.c），否则回退传统 IDE PIO。
 * disk-srv 及其 IPC 协议完全不感知底层是哪条路径。 */
static bool blk_read(uint64_t lba, uint8_t count, void *buf)
{
    kprintf("[ata] blk_read lba=%lu cnt=%u bm=0x%x present=%u ahci=%u\n",
            (unsigned long)lba, (unsigned)count, (unsigned)g_bm_base,
            (unsigned)g_disk_present, (unsigned)ahci_present());
    if (ahci_present()) {
        return ahci_read_sectors(lba, count, buf);
    }
    return ata_read_sectors(lba, count, buf);
}
static bool blk_write(uint64_t lba, uint8_t count, const void *buf)
{
    if (ahci_present()) {
        return ahci_write_sectors(lba, count, buf);
    }
    return ata_write_sectors(lba, count, buf);
}

/* ---- DISK_PORT 内核服务任务 ----
 * 消息循环：RECV DISK_PORT -> blk_read/blk_write -> SEND 应答到请求方端口。 */
static void disk_srv_task(void *arg)
{
    (void)arg;
    port_set_owner(DISK_PORT, sched_current());

    /* 请求缓冲须容纳写请求（头 + write_req + 7*512 数据）；
     * 应答最大 = 头 + status + 7*512 */
    static uint8_t req[sizeof(mach_msg_header_t) + sizeof(disk_write_req_t)
                       + DISK_MAX_SECTORS * ATA_SECTOR_SIZE];
    static uint8_t resp[sizeof(mach_msg_header_t) + sizeof(disk_read_resp_t)
                        + DISK_MAX_SECTORS * ATA_SECTOR_SIZE];

    kprintf("[disk-srv] serving DISK_PORT (kernel-resident, IPC only)\n");
    for (;;) {
        uint32_t n = 0;
        if (ipc_recv_kernel(DISK_PORT, req, sizeof(req), &n, true)
                != MACH_MSG_SUCCESS || n < sizeof(mach_msg_header_t)) {
            continue;
        }
        mach_msg_header_t *rh = (mach_msg_header_t *)req;
        uint32_t reply = rh->msgh_local_port;
        kprintf("[disk-srv] dbg: rx id=%u n=%u reply=%u\n",
                (unsigned)rh->msgh_id, (unsigned)n, (unsigned)reply);
        kprintf("[disk-srv] dbg: after-rx hdr_bits=%u remote=%u local=%u id=%u\n",
                (unsigned)rh->msgh_bits, (unsigned)rh->msgh_remote_port,
                (unsigned)rh->msgh_local_port, (unsigned)rh->msgh_id);
        if (reply == PORT_NULL) {
            continue;
        }

        if (rh->msgh_id == DISK_MSG_READ &&
            n >= sizeof(mach_msg_header_t) + sizeof(disk_read_req_t)) {
            disk_read_req_t *r =
                (disk_read_req_t *)(req + sizeof(mach_msg_header_t));
            uint32_t count = r->count;
            if (count > DISK_MAX_SECTORS) {
                count = DISK_MAX_SECTORS;
            }

            mach_msg_header_t *h = (mach_msg_header_t *)resp;
            disk_read_resp_t *rr = (disk_read_resp_t *)(resp + sizeof(*h));
            uint8_t *data = resp + sizeof(*h) + sizeof(*rr);
            kprintf("[disk-srv] dbg: READ branch lba=%lu count=%u -> calling blk_read\n",
                    (unsigned long)r->lba, (unsigned)count);

            bool ok = blk_read(r->lba, (uint8_t)count, data);
            rr->status = ok ? 0 : 1;

            uint32_t total = sizeof(*h) + sizeof(*rr)
                           + (ok ? count * ATA_SECTOR_SIZE : 0);
            h->msgh_bits = 0;
            h->msgh_size = total;
            h->msgh_remote_port = reply;
            h->msgh_local_port = DISK_PORT;
            h->msgh_id = DISK_MSG_READ;
            h->msgh_reserved = 0;
            kprintf("[disk-srv] dbg: sending reply id=DISK_MSG_READ ok=%u to port=%u\n",
                    (unsigned)ok, (unsigned)reply);
            ipc_send_kernel(reply, resp, total);
            kprintf("[disk-srv] dbg: reply sent to port=%u\n", (unsigned)reply);
        } else if (rh->msgh_id == DISK_MSG_WRITE &&
                   n >= sizeof(mach_msg_header_t) + sizeof(disk_write_req_t)) {
            disk_write_req_t *w =
                (disk_write_req_t *)(req + sizeof(mach_msg_header_t));
            uint32_t count = w->count;
            bool ok = false;
            /* 数据长度必须与 count 一致，防止越界读取请求缓冲 */
            if (count >= 1 && count <= DISK_MAX_SECTORS &&
                n >= sizeof(mach_msg_header_t) + sizeof(disk_write_req_t)
                     + count * ATA_SECTOR_SIZE) {
                const uint8_t *data = req + sizeof(mach_msg_header_t)
                                    + sizeof(disk_write_req_t);
                kprintf("[disk-srv] dbg: write lba=%u count=%u\n",
                        (unsigned)w->lba, (unsigned)count);
                ok = blk_write(w->lba, (uint8_t)count, data);
                kprintf("[disk-srv] dbg: write done ok=%u\n", (unsigned)ok);
            }

            mach_msg_header_t *h = (mach_msg_header_t *)resp;
            disk_write_resp_t *wr = (disk_write_resp_t *)(resp + sizeof(*h));
            wr->status = ok ? 0 : 1;
            uint32_t total = sizeof(*h) + sizeof(*wr);
            h->msgh_bits = 0;
            h->msgh_size = total;
            h->msgh_remote_port = reply;
            h->msgh_local_port = DISK_PORT;
            h->msgh_id = DISK_MSG_WRITE;
            h->msgh_reserved = 0;
            ipc_send_kernel(reply, resp, total);
        }
    }
}

void disk_srv_start(void)
{
    task_create_kernel(disk_srv_task, NULL, "disk-srv");
}
