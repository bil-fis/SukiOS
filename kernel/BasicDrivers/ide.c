#include "kernel/ide.h"
#include "kernel/io.h"
#include "kernel/tty.h"

static uint8_t ide_drive_sel = 0;

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

static bool ide_probe_ata(uint8_t drive_sel)
{
    outb(IDE_DRIVE_SEL, drive_sel);
    for (int i = 0; i < 10; i++) inb(IDE_STATUS);

    outb(IDE_SECTOR_COUNT, 0);
    outb(IDE_LBA_LOW, 0);
    outb(IDE_LBA_MID, 0);
    outb(IDE_LBA_HIGH, 0);
    outb(IDE_COMMAND, IDE_CMD_IDENTIFY);

    if (inb(IDE_STATUS) == 0) return false;

    if (ide_wait_ready() != 0) return false;

    uint8_t mid  = inb(IDE_LBA_MID);
    uint8_t high = inb(IDE_LBA_HIGH);
    if (mid == 0x14 && high == 0xEB) return false;

    for (int i = 0; i < 256; i++) inw(IDE_DATA);
    return true;
}

void ide_init(void)
{
    tty_print("[..] Initializing IDE...\n");

    if (ide_probe_ata(0xE0)) {
        ide_drive_sel = 0xE0;
        tty_print("[OK] IDE ATA master detected.\n");
        return;
    }
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
    if (ide_drive_sel == 0) return false;

    ide_select_drive(lba);

    outb(IDE_SECTOR_COUNT, 1);
    outb(IDE_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(IDE_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(IDE_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(IDE_COMMAND, IDE_CMD_READ_SECTORS);

    if (ide_wait_drq() != 0) return false;

    for (int i = 0; i < IDE_SECTOR_SIZE / 2; i++) {
        uint16_t data = inw(IDE_DATA);
        buffer[i * 2]     = (uint8_t)(data & 0xFF);
        buffer[i * 2 + 1] = (uint8_t)(data >> 8);
    }
    return true;
}

bool ide_write_sector(uint32_t lba, const uint8_t *buffer)
{
    if (ide_drive_sel == 0) return false;

    ide_select_drive(lba);

    outb(IDE_SECTOR_COUNT, 1);
    outb(IDE_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(IDE_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(IDE_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(IDE_COMMAND, IDE_CMD_WRITE_SECTORS);

    if (ide_wait_drq() != 0) return false;

    for (int i = 0; i < IDE_SECTOR_SIZE / 2; i++) {
        uint16_t data = (buffer[i * 2 + 1] << 8) | buffer[i * 2];
        outw(IDE_DATA, data);
    }

    /* 等待写入完成并刷新缓存 */
    if (ide_wait_ready() != 0) return false;
    outb(IDE_COMMAND, IDE_CMD_CACHE_FLUSH);
    return (ide_wait_ready() == 0);
}