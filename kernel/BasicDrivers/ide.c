#include "kernel/ide.h"
#include "kernel/io.h"
#include "kernel/tty.h"

static uint8_t ide_drive_sel = 0;   // 0xE0 或 0xF0

static int ide_wait_ready(void)
{
    uint8_t status;
    do {
        status = inb(IDE_STATUS);
    } while (status & IDE_SR_BSY);
    return (status & IDE_SR_DRDY) ? 0 : -1;
}

static int ide_wait_drq(void)
{
    uint8_t status;
    // 等待 DRQ 或 ERR
    do {
        status = inb(IDE_STATUS);
    } while (!(status & (IDE_SR_DRQ | IDE_SR_ERR)));
    if (status & IDE_SR_ERR) return -1;
    return 0;
}

static void ide_select_drive(uint32_t lba)
{
    outb(IDE_DRIVE_SEL, ide_drive_sel | ((lba >> 24) & 0x0F));
    for (int i = 0; i < 10; i++) inb(IDE_STATUS);
}

/**
 * 探测指定驱动器是否为 ATA 硬盘（排除 ATAPI 光盘）。
 * 返回 true 表示找到了可用的 ATA 硬盘。
 */
static bool ide_probe_ata(uint8_t drive_sel)
{
    outb(IDE_DRIVE_SEL, drive_sel);
    for (int i = 0; i < 10; i++) inb(IDE_STATUS);

    outb(IDE_SECTOR_COUNT, 0);
    outb(IDE_LBA_LOW, 0);
    outb(IDE_LBA_MID, 0);
    outb(IDE_LBA_HIGH, 0);
    outb(IDE_COMMAND, IDE_CMD_IDENTIFY);

    // 若状态为 0，设备不存在
    if (inb(IDE_STATUS) == 0)
        return false;

    // 等待命令完成
    if (ide_wait_ready() != 0)
        return false;

    // ATAPI 设备在执行 IDENTIFY 后会在 LBA Mid/High 留下 0x14/0xEB 签名
    uint8_t mid  = inb(IDE_LBA_MID);
    uint8_t high = inb(IDE_LBA_HIGH);
    if (mid == 0x14 && high == 0xEB) {
        // 是 ATAPI 设备（如 CD‑ROM），跳过
        return false;
    }

    // 确实是 ATA 硬盘，将 IDENTIFY 数据读走（不关心内容）
    for (int i = 0; i < 256; i++)
        inw(IDE_DATA);

    return true;
}

void ide_init(void)
{
    tty_print("[..] Initializing IDE...\n");

    // 优先尝试主盘
    if (ide_probe_ata(0xE0)) {
        ide_drive_sel = 0xE0;
        tty_print("[OK] IDE ATA master detected.\n");
        return;
    }

    // 主盘不是 ATA（可能是 ATAPI 光盘或不存在），再试从盘
    if (ide_probe_ata(0xF0)) {
        ide_drive_sel = 0xF0;
        tty_print("[OK] IDE ATA slave detected.\n");
        return;
    }

    tty_setcolor(VGA_RED, VGA_BLACK);
    tty_print("  No IDE ATA device found.\n");
    tty_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
}

bool ide_read_sector(uint32_t lba, uint8_t *buffer)
{
    if (ide_drive_sel == 0)
        return false;

    ide_select_drive(lba);

    outb(IDE_SECTOR_COUNT, 1);
    outb(IDE_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(IDE_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(IDE_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(IDE_COMMAND, IDE_CMD_READ_SECTORS);

    if (ide_wait_drq() != 0)
        return false;

    for (int i = 0; i < IDE_SECTOR_SIZE / 2; i++) {
        uint16_t data = inw(IDE_DATA);
        buffer[i * 2]     = (uint8_t)(data & 0xFF);
        buffer[i * 2 + 1] = (uint8_t)(data >> 8);
    }
    return true;
}

bool ide_write_sector(uint32_t lba, const uint8_t *buffer)
{
    (void)lba;
    (void)buffer;
    tty_print("[IDE] Write not implemented.\n");
    return false;
}