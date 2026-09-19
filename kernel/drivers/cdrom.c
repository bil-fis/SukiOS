/*
 * kernel/drivers/cdrom.c
 * -----------------------------------------------------------------------------
 * 内核态 ATAPI CD-ROM PIO 读取驱动（Secondary Master，0x170 通道）。
 *
 * 红线：这是唯一驻留 Ring0 的光盘原始读取通道，只响应 READ(10) 数据包；
 * ISO9660 文件系统语义完全由 kernel/fs/iso9660.c 负责。本文件不解析任何
 * 目录/卷结构，只提供「按 2048 字节逻辑块读盘」这一原子能力。
 *
 * 调用关系：kmain(boot_late_init 的 no-disk 分支) -> CdromInit()；
 *           iso9660.c -> CdromReadBlocks() -> ATAPI PACKET(READ 0x28)。
 *
 * 参考：OSDev《ATAPI》—— PACKET 命令相位（命令相位写 12 字节 CDB，再数据相位
 * 按 Interrupt Reason 寄存器的 IO 位决定方向）。全程轮询 + 超时，不依赖 IRQ，
 * 与现有 ATA PIO 驱动风格一致（无头/单核下确定性强）。
 */
#include <kernel/cdrom.h>
#include <kernel/io.h>
#include <kernel/spinlock.h>
#include <kernel/clock.h>
#include <kernel/console.h>
#include <kernel/string.h>

/* ---- IDE 通道寄存器：运行时由 CdromInit 探测决定基址（默认从通道 0x170）---- */
#define CD_BASE_DFLT 0x170
static uint16_t g_cd_io  = CD_BASE_DFLT;   /* 检测到的 CD-ROM I/O 基址 */
static uint8_t  g_cd_dev = 0;             /* 0=master, 1=slave */

#define CD_DATA   (g_cd_io + 0)   /* 数据 / PACKET 命令字节（16 位访问 CDB） */
#define CD_FEAT   (g_cd_io + 1)   /* Features（写 0：不用 DMA、不用 overlap） */
#define CD_IR     (g_cd_io + 2)   /* Interrupt Reason / Sector Count */
#define CD_LBA0   (g_cd_io + 3)
#define CD_LBA1   (g_cd_io + 4)
#define CD_LBA2   (g_cd_io + 5)
#define CD_DH     (g_cd_io + 6)   /* Device/Head：0xA0=master, 0xB0=slave */
#define CD_CMD    (g_cd_io + 7)   /* 命令 / 状态（同端口：写命令读状态） */
#define CD_ALT    (g_cd_io + 0x206)  /* Alternate Status / Device Control */

#define CD_ST_BSY  0x80
#define CD_ST_DRDY 0x40
#define CD_ST_DRQ  0x08
#define CD_ST_ERR  0x01

#define CD_CMD_PACKET   0xA0
#define CD_CMD_IDENTIFY 0xA1        /* IDENTIFY PACKET DEVICE */
#define SCSI_READ10     0x28

/* 单块读上限（防止一次请求过大造成堆分配/超时失控）；ISO9660 小文件足够。 */
#define CD_MAX_BLOCKS   64

static bool      g_cdrom_present = false;
static spinlock_t g_cd_lock = SPINLOCK_INIT("cdrom");

/* 通道稳定延迟（~420ns）：发命令/选盘后读 15 次 Alternate Status。 */
static void CdromDelay(void)
{
    for (int i = 0; i < 15; i++) {
        (void)inb(CD_ALT);
    }
}

/* 裸 TSC 读取（与 clock.c 同源，但此处不依赖 clock_init 状态，
 * 避免在关中断/锁内时钟服务未推进时造成死循环）。 */
static inline uint64_t CdromTsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}

/* 时间基准轮询：直接用裸 TSC 计时（独立于 clock_monotonic_ns）。
 * 关中断/持锁环境下也能正确推进；无 TSC 频率时退化成定数迭代兜底。 */
static bool CdromPoll(bool (*cond)(void), uint64_t timeout_ns)
{
    uint64_t hz = clock_tsc_freq_hz();
    if (hz == 0) {
        for (uint64_t i = 0; i < 4000000000ULL; i++) {
            if (cond()) {
                return true;
            }
            cpu_relax();
        }
        return false;
    }
    uint64_t t0 = CdromTsc();
    uint64_t limit = (timeout_ns / 1000000000ULL) * hz
                   + (timeout_ns % 1000000000ULL) * hz / 1000000000ULL + 1;
    for (;;) {
        if (cond()) {
            return true;
        }
        if ((CdromTsc() - t0) >= limit) {
            return false;
        }
        cpu_relax();
    }
}
static bool CdromCondNotBusy(void)   { return !(inb(CD_CMD) & CD_ST_BSY); }
static bool CdromCondDrq(void)
{
    uint8_t st = inb(CD_CMD);
    if (st & CD_ST_ERR) {
        return true;     /* 出错也结束轮询，由调用方判 ERR */
    }
    return !(st & CD_ST_BSY) && (st & CD_ST_DRQ);
}

/* 选择一个 ATAPI 设备（g_cd_dev：0=master,1=slave），关闭其中断（nIEN=1，纯轮询）。 */
static void CdromSelect(void)
{
    outb(CD_DH, 0xA0 | (g_cd_dev << 4));
    CdromDelay();
}

/* 发送一条 ATAPI PACKET 命令并传输数据。
 * cdb：12 字节命令描述符；buf/buf_bytes：数据缓冲（字节数须为偶数字节，
 *      因为以 16 位字为单位搬运）；is_read=true 表示设备->主机。
 * 返回 true 表示整条命令完成且未报错。 */
static bool CdromPacket(const uint8_t *cdb, uint8_t cdb_len,
                        void *buf, uint32_t buf_bytes, bool is_read)
{
    (void)cdb_len;
    if (buf_bytes & 1) {
        return false;    /* ISO9660 块为 2048 偶数字节；异常防护 */
    }

    uint32_t flags = spin_lock_irqsave(&g_cd_lock);

    CdromSelect();
    if (inb(CD_ALT) == 0xFF) {                 /* 悬空总线：无设备 */
        spin_unlock_irqrestore(&g_cd_lock, flags);
        return false;
    }
    if (!CdromPoll(CdromCondNotBusy, 1000000000ULL)) {
        spin_unlock_irqrestore(&g_cd_lock, flags);
        return false;
    }

    /* 进入 PACKET 命令相位 */
    outb(CD_FEAT, 0x00);          /* 不用 DMA / overlap */
    outb(CD_CMD, CD_CMD_PACKET);
    /* 等待 DRQ 置位（命令相位：设备准备好接收 CDB） */
    if (!CdromPoll(CdromCondDrq, 1000000000ULL)) {
        spin_unlock_irqrestore(&g_cd_lock, flags);
        return false;
    }
    if (inb(CD_CMD) & CD_ST_ERR) {
        spin_unlock_irqrestore(&g_cd_lock, flags);
        return false;
    }

    /* 写 12 字节 CDB（6 个 16 位字，小端字节序） */
    for (int i = 0; i < 6; i++) {
        uint16_t w = (uint16_t)((cdb[2 * i]) | (cdb[2 * i + 1] << 8));
        outw(CD_DATA, w);
    }

    /* 等待数据相位 DRQ（BSY 清、DRQ 置） */
    if (!CdromPoll(CdromCondDrq, 5000000000ULL)) {
        spin_unlock_irqrestore(&g_cd_lock, flags);
        return false;
    }
    if (inb(CD_CMD) & CD_ST_ERR) {
        spin_unlock_irqrestore(&g_cd_lock, flags);
        return false;
    }

    /* Interrupt Reason 寄存器 IO 位(bit1)指示方向：1=设备->主机(读)。 */
    uint8_t ir = inb(CD_IR);
    bool dev_to_host = (ir & 0x02) != 0;
    if (dev_to_host != is_read) {
        /* 方向与调用方预期不符：直接判失败（避免搬错方向数据） */
        spin_unlock_irqrestore(&g_cd_lock, flags);
        return false;
    }

    uint16_t *p = (uint16_t *)buf;
    uint32_t words = buf_bytes / 2;
    if (is_read) {
        for (uint32_t i = 0; i < words; i++) {
            p[i] = inw(CD_DATA);
        }
    } else {
        for (uint32_t i = 0; i < words; i++) {
            outw(CD_DATA, p[i]);
        }
    }

    /* 等待命令完成（BSY 清） */
    if (!CdromPoll(CdromCondNotBusy, 5000000000ULL)) {
        spin_unlock_irqrestore(&g_cd_lock, flags);
        return false;
    }
    bool ok = !(inb(CD_CMD) & CD_ST_ERR);

    spin_unlock_irqrestore(&g_cd_lock, flags);
    return ok;
}

bool CdromInit(void)
{
    g_cdrom_present = false;

    /* 候选 IDE 通道：主(0x1F0)/从(0x170)，各自 master/slave。
     * QEMU `-cdrom` 默认挂在从通道 master；但为兼容不同配置仍全扫描。
     * 注意：BIOS/GRUB 读过光盘后，「复位签名」(0x14/0xEB) 已被清掉，不能再据此
     * 判别；唯一可靠的判据是选盘后直接发 IDENTIFY PACKET DEVICE(0xA1)：
     *   - PACKET 设备会返回 256 字识别数据(DRQ 置位，无 ERR)；
     *   - 普通硬盘/无设备会 ERR(ABORT)，据此跳过。 */
    static const uint16_t s_bases[2] = { 0x1F0, 0x170 };
    for (int bi = 0; bi < 2; bi++) {
        for (int dev = 0; dev < 2; dev++) {
            g_cd_io  = s_bases[bi];
            g_cd_dev = (uint8_t)dev;

            uint32_t flags = spin_lock_irqsave(&g_cd_lock);
            outb(g_cd_io + 0x206, 0x02);            /* nIEN=1，纯轮询 */
            outb(g_cd_io + 6, 0xA0 | (dev << 4));   /* 选择设备 */
            CdromDelay();
            if (inb(g_cd_io + 0x206) == 0xFF) {     /* 悬空总线：通道无设备 */
                spin_unlock_irqrestore(&g_cd_lock, flags);
                continue;
            }
            if (!CdromPoll(CdromCondNotBusy, 1000000000ULL)) {
                spin_unlock_irqrestore(&g_cd_lock, flags);
                continue;
            }
            /* IDENTIFY PACKET DEVICE */
            outb(g_cd_io + 1, 0x00);
            outb(g_cd_io + 7, CD_CMD_IDENTIFY);
            /* 等待 DRQ(有识别数据) 或 ERR(被中止) */
            bool ready = CdromPoll(CdromCondDrq, 2000000000ULL);
            uint8_t st = inb(g_cd_io + 7);
            if (st & CD_ST_ERR) {
                (void)inb(g_cd_io + 1);             /* 读 Error 寄存器 */
                CdromPoll(CdromCondNotBusy, 1000000000ULL);
                spin_unlock_irqrestore(&g_cd_lock, flags);
                continue;                          /* 不是 PACKET 设备 */
            }
            if (!ready) {                          /* 既无 DRQ 也无 ERR：无响应 */
                spin_unlock_irqrestore(&g_cd_lock, flags);
                continue;
            }
            /* PACKET 设备：读完 256 字识别数据以清除 DRQ 相位 */
            for (int i = 0; i < 256; i++) {
                (void)inw(g_cd_io + 0);
            }
            (void)inb(g_cd_io + 7);
            CdromPoll(CdromCondNotBusy, 1000000000ULL);

            g_cdrom_present = true;
            spin_unlock_irqrestore(&g_cd_lock, flags);
            kprintf("[cdrom] ATAPI CD-ROM found at 0x%X dev=%d\n",
                    (unsigned)g_cd_io, dev);
            return true;
        }
    }
    kprintf("[cdrom] no ATAPI CD-ROM found on any IDE channel\n");
    return false;
}

bool CdromPresent(void)
{
    return g_cdrom_present;
}

/* 判断整段缓冲是否全零（用于识别 ATAPI 偶发零扇区，触发重试）。 */
static bool CdromBufAllZero(const void *buf, uint32_t bytes)
{
    const uint8_t *p = (const uint8_t *)buf;
    for (uint32_t i = 0; i < bytes; i++) {
        if (p[i] != 0) {
            return false;
        }
    }
    return true;
}

bool CdromReadBlocks(uint32_t lba, uint32_t count, void *buf)
{
    if (!g_cdrom_present || count == 0) {
        return false;
    }
    if (count > CD_MAX_BLOCKS) {
        /* 超大请求拆成多段，避免单次 DMA/堆分配过大 */
        uint8_t *p = (uint8_t *)buf;
        while (count > 0) {
            uint32_t chunk = (count > CD_MAX_BLOCKS) ? CD_MAX_BLOCKS : count;
            if (!CdromReadBlocks(lba, chunk, p)) {
                return false;
            }
            p += (uint64_t)chunk * 2048u;
            lba += chunk;
            count -= chunk;
        }
        return true;
    }

    uint8_t cdb[12];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_READ10;
    /* 32 位 LBA，大端 */
    cdb[2] = (uint8_t)((lba >> 24) & 0xFF);
    cdb[3] = (uint8_t)((lba >> 16) & 0xFF);
    cdb[4] = (uint8_t)((lba >> 8) & 0xFF);
    cdb[5] = (uint8_t)(lba & 0xFF);
    /* 传输长度（块数），大端 */
    cdb[7] = (uint8_t)((count >> 8) & 0xFF);
    cdb[8] = (uint8_t)(count & 0xFF);

    /* 防御性重试：ATAPI 在暖机/残留数据相位后偶发返回全零扇区（命令成功但
     * 数据为空）。目录/小文件绝不应整段为零，故全零即重试（最多 3 次）。
     * 真正失败（CdromPacket 返回 false）立即上报，不重试。 */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!CdromPacket(cdb, sizeof(cdb), buf, count * 2048u, true)) {
            return false;
        }
        if (!CdromBufAllZero(buf, count * 2048u)) {
            return true;            /* 读回有效数据 */
        }
    }
    /* 重试 3 次仍全零：属异常（设备/介质问题），打点后按读回零数据处理。 */
    kprintf("[cdrom] read lba=%u returned all-zero after retries\n", (unsigned)lba);
    return true;
}
