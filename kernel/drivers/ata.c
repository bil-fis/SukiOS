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
#include <kernel/clock.h>     /* clock_monotonic_ns：DMA/PIO 超时的时间基准（问题5） */

/* 前向声明：供 ata_dma_init 在定义前调用，避免隐式声明/重复声明冲突 */
static bool ata_dma_xfer(uint64_t lba, uint8_t count, bool write);
static void ata_dma_self_test(void);

/* 无锁诊断输出（仅错误路径使用）：直接操作 COM1 端口，不获取 g_kp_lock /
 * g_serial_lock 等任何内核自旋锁。原因：ata 驱动全程持 g_ata_lock 并 cli，
 * 若在持锁期间调用 kprintf/serial_writestr 会顺带获取 console/serial 自旋锁，
 * 与其他正持有那些锁、反过来又想取 g_ata_lock 的任务形成交叉死锁（单核协作
 * 式调度下 ticket 自旋锁 owner 停滞 -> PANIC）。故持锁期诊断必须走无锁通道。 */
static void ata_err(const char *s)
{
    for (const char *p = s; *p; p++) {
        while (!(inb(0x3F8 + 5) & 0x20)) {
            cpu_relax();
        }
        outb(0x3F8, (uint8_t)*p);
    }
}

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

    /* BMIDE 启用策略（P0 生产就绪 + 本步按 OSDev 规范尝试真正启用）：
     * - 默认（含 CONFIG_SMP==1 与单核）均先设置 g_bm_base=base 并跑启动期
     *   DMA 自检（ata_dma_self_test）。自检通过则真正启用 DMA，失败回退 PIO。
     *   此前多核下 BM 基址 0xc040 偶发卡死，根因已定位为「持锁期调用
     *   kprintf 引入交叉自旋锁死锁」，现已用无锁 ata_err 消除，并补齐
     *   DMA 停用时 ACTIVE 清零等待，故多核下同样走自检打通（审计清单 #6）。
     * - 强制宏 ATA_FORCE_DMA：定义时无论单/多核都启用（供真实硬件验证用）。
     * 任一种情况 DMA 传输失败都会由 ata_read/write_sectors 干净回退 PIO，
     * 数据正确性始终优先。 */
    g_bm_base = 0;   /* 默认先关闭，自检通过后再打开 */
#if defined(ATA_FORCE_DMA)
    g_bm_base = base;
    kprintf("[ata] BMIDE @ I/O 0x%x (PCI %u:%u.%u) FORCE-ENABLED (ATA_FORCE_DMA)\n",
            (unsigned)base, (unsigned)ide.bus, (unsigned)ide.dev,
            (unsigned)ide.func);
    ata_dma_self_test();
#else
    g_bm_base = base;   /* 先打开，自检决定最终去留（单/多核一致） */
    kprintf("[ata] BMIDE @ I/O 0x%x (PCI %u:%u.%u) probed; running DMA self-test...\n",
            (unsigned)base, (unsigned)ide.bus, (unsigned)ide.dev,
            (unsigned)ide.func);
    ata_dma_self_test();
#endif
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

/* 时间基准轮询辅助：基于 clock_monotonic_ns()（源自 TSC/HPET，自启动单调）。
 * 解决「固定循环计数在不同 CPU 速度下超时偏差过大」的问题（审计清单 #5）。
 * 调用方须保证 clock_init() 已完成（kmain 中 ata_init 在其后执行，安全）。 */
static bool ata_poll_until_ns(uint64_t timeout_ns, bool (*cond)(void))
{
    uint64_t deadline = clock_monotonic_ns() + timeout_ns;
    for (;;) {
        if (cond()) {
            return true;
        }
        if (clock_monotonic_ns() >= deadline) {
            return false;
        }
        cpu_relax();
    }
}
static bool ata_cond_not_busy(void)   { return !(inb(ATA_STATUS) & ST_BSY); }
static bool ata_cond_drq(void)
{
    uint8_t st = inb(ATA_STATUS);
    if (st & ST_ERR) {
        return true;     /* 出错也结束轮询，由调用方判 ERR */
    }
    return !(st & ST_BSY) && (st & ST_DRQ);
}

/* 等待 BSY 清零；超时返回 false（超时 1 秒，覆盖最差真机情况） */
static bool ata_wait_not_busy(void)
{
    return ata_poll_until_ns(1000000000ULL, ata_cond_not_busy);
}

/* 等待 DRQ 置位（数据就绪）；出错/超时返回 false */
static bool ata_wait_drq(void)
{
    return ata_poll_until_ns(1000000000ULL, ata_cond_drq);
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
/* 单次 Bus Master DMA 传输（单 PRD，最多 ATA_DMA_MAX_SECTORS 扇区）。
 * 严格遵循 OSDev《ATA/ATAPI using DMA》命令序列：
 *   1) 编程 PRDT  -> 2) 写 PRDT 地址  -> 3) 设 R/W 方向(Start=0 时)
 *   4) 清 Error/IRQ  -> 5) 选盘 + 发 LBA  -> 6) 发 DMA 命令
 *   7) 置 Start 位  -> 8) 轮询 BM_STATUS 完成  -> 9) 清 Start + 清状态
 * 方向位 BM_CMD_DIR(bit3)：读盘(设备->内存)置 1，写盘(内存->设备)清 0。
 * 整个传输持 g_ata_lock + cli，且本函数内部严禁调用 dbg_printf/kprintf
 * （以免在持锁期间又去获取 console 自旋锁，引入交叉持锁导致 ticket 死锁）。
 * 返回 true 表示传输成功。轮询带 ~30ms 硬上限，超时即停引擎返回 false，
 * 由上层 ata_read/write_sectors 干净回退 PIO（数据正确性优先、绝不霸占 CPU）。 */
static bool __attribute__((noinline)) ata_dma_xfer(uint64_t lba, uint8_t count, bool write)
{
#if ATA_DEBUG
    static int g_dx = 0;
    bool dx = (g_dx++ < 4);
    if (dx) ata_err("[ata] D enter\n");
#else
    const bool dx = false;
#endif
    if (g_bm_base == 0 || count == 0 || count > ATA_DMA_MAX_SECTORS) {
        return false;
    }

    uint32_t bytes = (uint32_t)count * ATA_SECTOR_SIZE;

    /* 1. 构造单项 PRDT：物理缓冲地址 + 字节数(低16) + EOT(bit15 of word3)。
     *    OSDev：count 字段为 0 表示 64KiB；本路径 bytes<=4096 不会溢出。 */
    g_prdt[0] = (uint64_t)(uint32_t)g_dma_buf_phys
              | ((uint64_t)(bytes & 0xFFFFu) << 32)
              | (0x8000ULL << 48);                   /* bit15 of word3 = EOT */

    /* 2. 写 PRDT 物理地址（32 位；已确保 <4GiB） */
    outl(g_bm_base + BM_PRDT, (uint32_t)g_prdt_phys);

    /* 3. 设方向位（必须在 Start=0 时）：读盘置 BM_CMD_DIR，写盘清 0 */
    outb(g_bm_base + BM_CMD, write ? 0 : BM_CMD_DIR);

    /* 4. 清 Error/IRQ（写 1 清），保留只读能力位 */
    uint8_t st = inb(g_bm_base + BM_STATUS);
    outb(g_bm_base + BM_STATUS, (uint8_t)(st | BM_ST_ERROR | BM_ST_IRQ));

    /* 5. 选盘并下发 DMA 命令 */
    if (!ata_wait_not_busy()) {
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

    /* 6. 启动引擎：遵循 OSDev 规范分两步——先确保方向位已置(Start=0 时)，
     *    再单独置 Start=1。同时写方向+Start 在某些芯片组上可能令方向位未生效。
     *    （审计清单 #3） */
    outb(g_bm_base + BM_CMD, (uint8_t)(write ? 0 : BM_CMD_DIR));
    outb(g_bm_base + BM_CMD,
         (uint8_t)((write ? 0 : BM_CMD_DIR) | BM_CMD_START));

    /* 7. 轮询 BM_STATUS：IRQ 置位=正常完成；ERROR=失败；
     *    ACTIVE 清零且无 IRQ 时以设备状态寄存器为准判定。
     *    时间基准超时（50ms，覆盖最差真机），超时停引擎返回 false 回退 PIO。 */
    bool ok = false;
    {
        uint64_t deadline = clock_monotonic_ns() + 50000000ULL;   /* 50ms */
        for (;;) {
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
                ok = !(inb(ATA_STATUS) & ST_ERR);
                break;
            }
            if (clock_monotonic_ns() >= deadline) {
                ok = false;
                break;
            }
            cpu_relax();
        }
    }
    if (!ok) {
        outb(g_bm_base + BM_CMD, write ? 0 : BM_CMD_DIR);
        uint8_t fin = inb(g_bm_base + BM_STATUS);
        outb(g_bm_base + BM_STATUS,
             (uint8_t)(fin | BM_ST_ERROR | BM_ST_IRQ));
    }

    /* 8. 无论成败都必须停掉引擎并清状态位，防止残留影响下一次传输。
     *    【关键修复】仅清除 Start 位不够——必须等待硬件将 BM_ST_ACTIVE 清零，
     *    否则残留的 DMA 引擎会与下一次传输（尤其下次 DMA）冲突，导致数据错乱
     *    或死锁（审计清单 #1）。超时则强制停引擎。 */
    outb(g_bm_base + BM_CMD, write ? 0 : BM_CMD_DIR);
    {
        uint64_t deadline = clock_monotonic_ns() + 1000000ULL;   /* 1ms */
        for (uint32_t i = 0; i < 10000u; i++) {
            if (!(inb(g_bm_base + BM_STATUS) & BM_ST_ACTIVE)) {
                break;
            }
            if (clock_monotonic_ns() >= deadline) {
                break;   /* 超时：继续清状态，不无限等 */
            }
            cpu_relax();
        }
    }
    uint8_t fin = inb(g_bm_base + BM_STATUS);
    outb(g_bm_base + BM_STATUS, (uint8_t)(fin | BM_ST_ERROR | BM_ST_IRQ));
    if (fin & BM_ST_ERROR) {
        ok = false;
    }

    if (!ata_wait_not_busy()) {
        if (dx) ata_err("[ata] D abort(notbusy)\n");
        return false;
    }
    if (inb(ATA_STATUS) & ST_ERR) {
        if (dx) ata_err("[ata] D abort(stderr)\n");
        return false;
    }
    if (dx) ata_err("[ata] D exit ok\n");
    return ok;
}

/* 启动期 DMA 自检：在 BM 基址探测、缓冲分配完成后，做一次 1 扇区 LBA0 读取。
 * 成功则真正启用 DMA（g_bm_base=base），失败则回退 PIO（g_bm_base=0）。
 * 这样默认行为变为“自检通过才启用”，无需用户态干预即可实证本环境 BM 基址
 * 下的 DMA 是否可用；且自检失败自动降级，绝不引入生产风险。 */
static void __attribute__((noinline)) ata_dma_self_test(void)
{
    if (g_bm_base == 0) {
        return;     /* 无 BM 基址，无需自检 */
    }
    /* 复用 ata_dma_xfer 读 LBA0 到 g_dma_buf，仅以返回成功与否判定 DMA 链路可用。 */
    bool rc = ata_dma_xfer(0, 1, false);
    if (rc) {
        kprintf("[ata] BMIDE DMA self-test PASSED @ I/O 0x%x -> ENABLED\n",
                (unsigned)g_bm_base);
    } else {
        kprintf("[ata] BMIDE DMA self-test FAILED @ I/O 0x%x -> forcing PIO\n",
                (unsigned)g_bm_base);
        g_bm_base = 0;
    }
}


bool ata_read_sectors(uint64_t lba, uint8_t count, void *buf)
{
#if ATA_DEBUG
    static int g_diag = 0;
    bool diag = (g_diag++ < 6);   /* 只诊断前几次，避免刷屏 */
#else
    const bool diag = false;
#endif
    if (diag) ata_err("[ata] R enter\n");
    if (!g_disk_present || count == 0) {
        return false;
    }
    /* SMP 串行化：整个 ATA 事务持锁并关中断，防止本核 tick 抢占破坏 PIO 相位，
     * 或他核并发访问同一组 ATA I/O 端口导致命令/数据相位错乱。 */
    uint32_t ata_flags = spin_lock_irqsave(&g_ata_lock);
    if (diag) ata_err("[ata] R lock\n");
    /* M10 修复：读路径补上越界读盘防护（此前仅写路径有）。lba+count 越过
     * 卷尾会令控制器读无效扇区/越界 DMA；无符号回绕一并防范。 */
    if ((uint64_t)lba + count > g_total_sectors || (uint64_t)lba + count < lba) {
        ata_err("[ata] read_sectors OOB reject\n");
        spin_unlock_irqrestore(&g_ata_lock, ata_flags);
        return false;
    }
    /* 优先走 Bus Master DMA：按 ATA_DMA_MAX_SECTORS 分块，经反弹缓冲拷出。
     * 任一块 DMA 失败则整体回退 PIO 重做（保证数据正确性优先于速度）。 */
    if (g_bm_base != 0) {
        if (diag) ata_err("[ata] R dma-branch\n");
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
            spin_unlock_irqrestore(&g_ata_lock, ata_flags);
            return true;
        }
        /* 落到下面的 PIO 路径重试整个请求 */
    }

    if (diag) ata_err("[ata] R pio-branch\n");
    /* 【修复】PIO 读增加命令级重试（最多 3 次），抵御瞬时 DRQ/BSY 超时，
     * 增强健壮性（审计清单 #4）。每次重试前重新选盘+发命令。 */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!ata_wait_not_busy()) {
            ata_err("[ata] PIO read: not-busy timeout\n");
            continue;
        }
        bool use48 = g_lba48 && ((uint64_t)lba + count) > 0x0FFFFFFFULL;
        ata_select_lba(lba, count, use48);
        outb(ATA_CMD, use48 ? 0x24 : CMD_READ_SECTORS);   /* READ SECTORS EXT */

        bool ok = true;
        uint16_t *out = (uint16_t *)buf;
        for (uint8_t s = 0; s < count; s++) {
            if (!ata_wait_drq()) {
                ata_err("[ata] PIO read: drq timeout\n");
                ok = false;
                break;
            }
            for (int i = 0; i < 256; i++) {
                *out++ = inw(ATA_DATA);
            }
            ata_delay400();
        }
        if (ok) {
            spin_unlock_irqrestore(&g_ata_lock, ata_flags);
            return true;
        }
    }
    spin_unlock_irqrestore(&g_ata_lock, ata_flags);
    return false;
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
            /* 【关键修复】DMA 数据已成功落盘缓冲（传输完成），但 FLUSH 失败。
             * 此时重复 PIO 写入无益：FLUSH 失败的根本原因是设备状态异常，
             * PIO 重试不会改变它，反而可能因二次写入造成磨损/部分写。直接上报
             * 错误（审计清单 #2）。 */
            ata_err("[ata] DMA write done but FLUSH failed; report error\n");
            spin_unlock_irqrestore(&g_ata_lock, ata_flags);
            return false;
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
    dbg_printf("[ata] blk_read lba=%lu cnt=%u bm=0x%x present=%u ahci=%u\n",
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

/* ---- DISK 块缓存（内核态，消除大文件/元数据重复落盘） ----
 *
 * 为什么需要：FatFs 读一个大文件时要反复读 FAT 表、目录项、以及文件数据本身；
 * 原设计每次 disk_read 都发 IPC 落盘，重复读同一扇区（如 FAT 表）被多次搬运。
 * 本缓存在 disk-srv 进程内拦截所有读请求，命中则直接复用已读物理页（免 IPC 落盘
 * 与 PIO 等待），大幅提升大文件与随机小读性能。
 *
 * 设计（单核单线程 disk-srv，访问无需加锁）：
 *   - 缓存块单位 = 1 个物理页（4KiB = 8 扇区），键 = lba / 8（8 扇区对齐）。
 *   - 哈希表（直接取模）+ LRU 双向链表；容量 CACHE_ENTS，超限回收链表尾。
 *   - 每个表项持有物理页的 1 个引用计数（缓存持有）；OOL 回传时再 +1 交给传输。
 *   - 写请求不进缓存（保持简单，写场景非优化重点，且需回写一致性）。
 */
#define CACHE_BLOCK_SECTORS 8            /* 每缓存块 8 扇区 = 1 页 */
#define CACHE_ENTS          256          /* 256 * 4KiB = 1MiB 缓存 */
#define CACHE_HASH          CACHE_ENTS

typedef struct cache_ent {
    uint64_t  block_lba;        /* = 请求 lba 向下对齐到 8 扇区 */
    uint64_t  page_pa;          /* 物理页（缓存持有 1 引用） */
    bool      valid;
    struct cache_ent *lru_prev, *lru_next;      /* LRU 双向链表 */
    struct cache_ent *lru_next_in_bucket;       /* 哈希桶链表（开放寻址辅助） */
} cache_ent_t;

static cache_ent_t  g_cache_ents[CACHE_ENTS];
static cache_ent_t *g_cache_hash[CACHE_HASH];
static cache_ent_t *g_lru_head, *g_lru_tail;
static uint32_t     g_cache_used = 0;

static void cache_lru_remove(cache_ent_t *e)
{
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    else g_lru_head = e->lru_next;
    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    else g_lru_tail = e->lru_prev;
    e->lru_prev = e->lru_next = NULL;
}
static void cache_lru_push_front(cache_ent_t *e)
{
    e->lru_prev = NULL;
    e->lru_next = g_lru_head;
    if (g_lru_head) g_lru_head->lru_prev = e;
    else g_lru_tail = e;
    g_lru_head = e;
}
/* 把表项从哈希桶摘除 */
static void cache_hash_unlink(cache_ent_t *e)
{
    uint32_t h = (uint32_t)(e->block_lba / CACHE_BLOCK_SECTORS) % CACHE_HASH;
    cache_ent_t **pp = &g_cache_hash[h];
    while (*pp) {
        if (*pp == e) { *pp = e->lru_next_in_bucket; break; }
        pp = &(*pp)->lru_next_in_bucket;
    }
}
/* 把表项链入哈希桶 */
static void cache_hash_link(cache_ent_t *e)
{
    uint32_t h = (uint32_t)(e->block_lba / CACHE_BLOCK_SECTORS) % CACHE_HASH;
    e->lru_next_in_bucket = g_cache_hash[h];
    g_cache_hash[h] = e;
}

/* 查找缓存块；命中返回表项（已置于 LRU 头），否则 NULL */
static cache_ent_t *cache_lookup(uint64_t block_lba)
{
    uint32_t h = (uint32_t)(block_lba / CACHE_BLOCK_SECTORS) % CACHE_HASH;
    for (cache_ent_t *e = g_cache_hash[h]; e; e = e->lru_next_in_bucket) {
        if (e->valid && e->block_lba == block_lba) {
            cache_lru_remove(e);
            cache_lru_push_front(e);
            return e;
        }
    }
    return NULL;
}

/* 分配/复用一个缓存表项（可能回收 LRU 尾），写入新块内容，返回表项（已置 LRU 头
 * 且已在哈希中）。新块内容由调用方负责写入 page_pa 对应内核虚拟地址。 */
static cache_ent_t *cache_alloc_ent(uint64_t block_lba)
{
    cache_ent_t *e;
    if (g_cache_used < CACHE_ENTS) {
        e = &g_cache_ents[g_cache_used++];
        e->valid = false;
    } else {
        /* 回收 LRU 尾 */
        e = g_lru_tail;
        if (!e) return NULL;
        cache_lru_remove(e);
        cache_hash_unlink(e);
        pmm_decref((void *)e->page_pa);   /* 释放被回收块的引用 */
    }
    e->block_lba = block_lba;
    e->valid = true;
    cache_hash_link(e);
    cache_lru_push_front(e);
    return e;
}

/* 把一个 8 扇区块读入缓存（miss 时调用），返回其物理页。
 * 若已存在则直接返回缓存页（引用 +1 交给调用方用于 OOL）。 */
static uint64_t cache_read_block(uint64_t block_lba)
{
    cache_ent_t *e = cache_lookup(block_lba);
    if (e) {
        pmm_incref((void *)e->page_pa);   /* OOL 传输会 decref，缓存自身引用保留 */
        return e->page_pa;
    }
    e = cache_alloc_ent(block_lba);
    if (!e) return 0;
    void *pa = pmm_alloc_page();
    if (!pa) return 0;
    e->page_pa = (uint64_t)pa;
    pmm_incref((void *)pa);               /* 缓存自身持有 1 引用 */
    /* 物理读：把该 8 扇区块读入页内 */
    bool ok = blk_read(block_lba, CACHE_BLOCK_SECTORS,
                       (void *)PHYS_TO_VIRT(pa));
    if (!ok) {
        pmm_decref((void *)pa);
        e->valid = false;
        return 0;
    }
    pmm_incref((void *)pa);               /* OOL 传输引用（共 2：缓存+传输） */
    return (uint64_t)pa;
}

/* 把一个读请求拆成 8 扇区块，逐块经缓存取物理页，填入 out_pages[]（最多
 * DISK_OOL_MAX_PAGES），返回填充页数。返回 0 表示分配失败。 */
static uint32_t disk_srv_gather_pages(uint64_t lba, uint32_t count,
                                      uint64_t *out_pages)
{
    uint64_t first = lba / CACHE_BLOCK_SECTORS;
    uint64_t last  = (lba + count - 1) / CACHE_BLOCK_SECTORS;
    uint32_t idx = 0;
    for (uint64_t blk = first; blk <= last; blk++) {
        if (idx >= DISK_OOL_MAX_PAGES) break;
        uint64_t pa = cache_read_block(blk * CACHE_BLOCK_SECTORS);
        if (!pa) return 0;
        out_pages[idx++] = pa;
    }
    return idx;
}

/* 使一段 LBA 范围对应的缓存块失效（写回后调用，保证缓存一致性）。 */
/* 写穿越（write-through）：disk-srv 处理写请求成功后调用，保证块缓存与磁盘一致。
 * 对每个被写覆盖的 8 扇区块：
 *   - 若缓存命中该块：把写入数据中属于本块的部分精确 memcpy 进缓存页对应扇区
 *     偏移（不触碰同块内未被覆盖的扇区，也不影响相邻块），实现缓存与磁盘同步；
 *   - 若未命中：令该块失效，强制后续读重新落盘。
 * 采用 write-through 而非简单失效，可避免"写 A 块、读相邻 B 块命中旧缓存"的跨块
 * 污染（原 cache_invalidate_range 按写请求 lba/count 算出块区间，但读可能命中同
 * 块内另一扇区或相邻块，导致目录/FAT 元数据读到写前旧值，表现为文件写后读回失败）。 */
static void cache_write_through(uint64_t lba, uint32_t count, const uint8_t *data)
{
    uint64_t first = lba / CACHE_BLOCK_SECTORS;
    uint64_t last  = (lba + count - 1) / CACHE_BLOCK_SECTORS;
    for (uint64_t blk = first; blk <= last; blk++) {
        uint64_t block_lba = blk * CACHE_BLOCK_SECTORS;
        cache_ent_t *e = cache_lookup(block_lba);   /* 命中则移至 LRU 头 */
        /* 本块内属于 [lba, lba+count) 的扇区区间 */
        uint64_t seg_start = (block_lba < lba) ? lba : block_lba;
        uint64_t seg_end   = (block_lba + CACHE_BLOCK_SECTORS > lba + count)
                                 ? (lba + count) : (block_lba + CACHE_BLOCK_SECTORS);
        uint32_t off_sectors = (uint32_t)(seg_start - block_lba);   /* 块内偏移 */
        uint32_t n_sectors   = (uint32_t)(seg_end - seg_start);
        if (e && e->valid) {
            uint8_t *dst = (uint8_t *)PHYS_TO_VIRT(e->page_pa) + off_sectors * 512;
            const uint8_t *src = data + (seg_start - lba) * 512;
            for (uint32_t i = 0; i < n_sectors * 512; i++) dst[i] = src[i];
        } else {
            /* 未命中：失效该块（摘除并释放引用），下次读强制重读磁盘新值 */
            uint32_t h = (uint32_t)blk % CACHE_HASH;
            cache_ent_t **pp = &g_cache_hash[h];
            while (*pp) {
                cache_ent_t *x = *pp;
                if (x->valid && x->block_lba == blk) {
                    cache_lru_remove(x);
                    *pp = x->lru_next_in_bucket;
                    pmm_decref((void *)x->page_pa);
                    x->valid = false;
                    break;
                }
                pp = &x->lru_next_in_bucket;
            }
    }   /* end else */
    }   /* end for blk */
}       /* end cache_write_through */

/* ---- DISK_PORT 内核服务任务 ----
 * 消息循环：RECV DISK_PORT -> blk_read/blk_write -> SEND 应答到请求方端口。 */
static void disk_srv_task(void *arg)
{
    (void)arg;
    port_set_owner(DISK_PORT, sched_current());

    /* 启动自检：验证块缓存 + OOL 回传闭环（读 LBA0 两次，第二次应命中缓存，
     * 且两次返回同一物理页）。release 自检分配的 OOL 引用，避免泄漏。 */
    {
        uint64_t p1[DISK_OOL_MAX_PAGES], p2[DISK_OOL_MAX_PAGES];
        uint32_t n1 = disk_srv_gather_pages(0, 8, p1);
        uint32_t n2 = disk_srv_gather_pages(0, 8, p2);
        bool hit = (n1 == 1 && n2 == 1 && p1[0] == p2[0]);
        kprintf("[disk-srv] self-test: read=%u cache_hit=%s page=0x%lx\n",
                n1, hit ? "yes" : "no", (unsigned long)(n1 ? p1[0] : 0));
        for (uint32_t i = 0; i < n1; i++) pmm_decref((void *)p1[i]);
        for (uint32_t i = 0; i < n2; i++) pmm_decref((void *)p2[i]);
    }

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
        dbg_printf("[disk-srv] dbg: rx id=%u n=%u reply=%u\n",
                (unsigned)rh->msgh_id, (unsigned)n, (unsigned)reply);
        dbg_printf("[disk-srv] dbg: after-rx hdr_bits=%u remote=%u local=%u id=%u\n",
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
            if (count < 1) count = 1;
            if (count > DISK_MAX_SECTORS) {
                count = DISK_MAX_SECTORS;
            }

            mach_msg_header_t *h = (mach_msg_header_t *)resp;
            disk_read_resp_t *rr = (disk_read_resp_t *)(resp + sizeof(*h));

            /* 经块缓存逐 8 扇区块取物理页，填入 OOL 页数组。
             * OOL 回传以"整页(8 扇区)"对齐，接收方按请求在首块内的偏移截取。 */
            uint64_t ool_pages[DISK_OOL_MAX_PAGES];
            uint32_t npages = disk_srv_gather_pages(r->lba, count, ool_pages);
            bool ok = (npages > 0);

            rr->status = ok ? 0 : 1;

            uint32_t total = sizeof(*h) + sizeof(*rr);
            h->msgh_bits = 0;
            h->msgh_size = total;
            h->msgh_remote_port = reply;
            h->msgh_local_port = DISK_PORT;
            h->msgh_id = DISK_MSG_READ;
            h->msgh_reserved = 0;

            dbg_printf("[disk-srv] dbg: READ lba=%lu count=%u -> npages=%u ool\n",
                    (unsigned long)r->lba, (unsigned)count, (unsigned)npages);
            if (ok) {
                uint64_t ool_size = (uint64_t)npages * PAGE_SIZE;
                /* OOL 回传：inline 仅含状态头，数据走物理页（接收方负责 decref） */
                ipc_send_ool_kernel(reply, resp, total,
                                    ool_pages, npages, ool_size);
            } else {
                ipc_send_kernel(reply, resp, total);
            }
            dbg_printf("[disk-srv] dbg: reply(ool) sent to port=%u\n", (unsigned)reply);
        } else if (rh->msgh_id == DISK_MSG_WRITE &&
                   n >= sizeof(mach_msg_header_t) + sizeof(disk_write_req_t)) {
            disk_write_req_t *w =
                (disk_write_req_t *)(req + sizeof(mach_msg_header_t));
            uint32_t count = w->count;
            bool ok = false;
            /* 数据长度必须与 count 一致，防止越界读取请求缓冲；写上限保持 7 扇区（内联安全） */
            if (count >= 1 && count <= DISK_MAX_WRITE_SECTORS &&
                n >= sizeof(mach_msg_header_t) + sizeof(disk_write_req_t)
                     + count * ATA_SECTOR_SIZE) {
                const uint8_t *data = req + sizeof(mach_msg_header_t)
                                    + sizeof(disk_write_req_t);
                dbg_printf("[disk-srv] dbg: write lba=%u count=%u\n",
                        (unsigned)w->lba, (unsigned)count);
                ok = blk_write(w->lba, (uint8_t)count, data);
                dbg_printf("[disk-srv] dbg: write lba=%u count=%u ok=%u\n",
                        (unsigned)w->lba, (unsigned)count, (unsigned)ok);
                /* 写穿越：把写入同步进块缓存（命中则更新对应扇区，未命中则失效），
                 * 保证缓存与磁盘一致，避免写后读回得到陈旧数据。 */
                if (ok) cache_write_through(w->lba, count, data);
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
