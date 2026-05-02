#ifndef SUKIOS_IDE_H
#define SUKIOS_IDE_H

#include <stdint.h>
#include <stdbool.h>

/* 端口和宏保持不变 */
#define IDE_DATA          0x1F0
#define IDE_ERROR         0x1F1
#define IDE_FEATURES      0x1F1
#define IDE_SECTOR_COUNT  0x1F2
#define IDE_LBA_LOW       0x1F3
#define IDE_LBA_MID       0x1F4
#define IDE_LBA_HIGH      0x1F5
#define IDE_DRIVE_SEL     0x1F6
#define IDE_COMMAND       0x1F7
#define IDE_STATUS        0x1F7
#define IDE_ALT_STATUS    0x3F6

#define IDE_SR_BSY    0x80
#define IDE_SR_DRDY   0x40
#define IDE_SR_DF     0x20
#define IDE_SR_DRQ    0x08
#define IDE_SR_ERR    0x01

#define IDE_CMD_READ_SECTORS  0x20
#define IDE_CMD_WRITE_SECTORS 0x30
#define IDE_CMD_IDENTIFY      0xEC
#define IDE_CMD_CACHE_FLUSH   0xE7

#define IDE_SECTOR_SIZE  512

void ide_init(void);
bool ide_read_sector(uint32_t lba, uint8_t *buffer);
bool ide_write_sector(uint32_t lba, const uint8_t *buffer);

static inline bool ide_read_sectors(uint32_t lba, uint8_t count, uint8_t *buffer)
{
    for (uint8_t i = 0; i < count; ++i)
        if (!ide_read_sector(lba + i, buffer + i * IDE_SECTOR_SIZE))
            return false;
    return true;
}

static inline bool ide_write_sectors(uint32_t lba, uint8_t count, const uint8_t *buffer)
{
    for (uint8_t i = 0; i < count; ++i)
        if (!ide_write_sector(lba + i, buffer + i * IDE_SECTOR_SIZE))
            return false;
    return true;
}

#endif