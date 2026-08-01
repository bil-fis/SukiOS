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
#include <kernel/ahci.h>     /* P0-7：disk-srv 统一分发（AHCI DMA 优先） */
#include <kernel/io.h>
#include <kernel/console.h>
#include <kernel/task.h>
#include <kernel/string.h>
#include <ipc/port.h>
#include <ipc/disk_proto.h>
#include <mm/kmalloc.h>

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

static bool     g_disk_present = false;
static uint64_t g_total_sectors = 0;      /* L2：改为 64 位，容纳 LBA48 容量 */
static bool     g_lba48 = false;          /* L2：磁盘是否支持 LBA48 寻址 */

/* 读备用状态寄存器 4 次 ≈ 400ns 通道稳定延迟 */
static void ata_delay400(void)
{
    for (int i = 0; i < 4; i++) {
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
    /* 关闭该通道中断（nIEN=1），纯轮询 */
    outb(ATA_DEV_CTRL, 0x02);

    /* 选择 Primary Master */
    outb(ATA_DRIVE, 0xA0);
    ata_delay400();

    /* 悬空总线检测：status=0xFF 说明无设备 */
    if (inb(ATA_STATUS) == 0xFF) {
        kprintf("[ata] no device (floating bus)\n");
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
        return false;
    }
    if (!ata_wait_not_busy()) {
        kprintf("[ata] IDENTIFY timeout (BSY)\n");
        return false;
    }
    /* ATA 设备：LBA_MID/HI 应为 0（ATAPI 为 0x14/0xEB） */
    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HI) != 0) {
        kprintf("[ata] device is not ATA (ATAPI?)\n");
        return false;
    }
    if (!ata_wait_drq()) {
        kprintf("[ata] IDENTIFY failed (no DRQ)\n");
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

bool ata_read_sectors(uint64_t lba, uint8_t count, void *buf)
{
    if (!g_disk_present || count == 0) {
        return false;
    }
    /* M10 修复：读路径补上越界读盘防护（此前仅写路径有）。lba+count 越过
     * 卷尾会令控制器读无效扇区/越界 DMA；无符号回绕一并防范。 */
    if ((uint64_t)lba + count > g_total_sectors || (uint64_t)lba + count < lba) {
        return false;
    }
    if (!ata_wait_not_busy()) {
        return false;
    }

    bool use48 = g_lba48 && ((uint64_t)lba + count) > 0x0FFFFFFFULL;
    ata_select_lba(lba, count, use48);
    outb(ATA_CMD, use48 ? 0x24 : CMD_READ_SECTORS);   /* READ SECTORS EXT */

    uint16_t *out = (uint16_t *)buf;
    for (uint8_t s = 0; s < count; s++) {
        if (!ata_wait_drq()) {
            return false;
        }
        for (int i = 0; i < 256; i++) {
            *out++ = inw(ATA_DATA);
        }
        ata_delay400();
    }
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
    if ((uint64_t)lba + count > g_total_sectors) {        /* 越界写保护 */
        return false;
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
        return true;
    }
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
            ipc_send_kernel(reply, resp, total);
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
                ok = blk_write(w->lba, (uint8_t)count, data);
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
