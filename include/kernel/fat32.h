#ifndef SUKIOS_FAT32_H
#define SUKIOS_FAT32_H

#include <stdint.h>

/* FAT32 BPB + 扩展 */
typedef struct {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;       // 0x0B
    uint8_t  sectors_per_cluster;    // 0x0D
    uint16_t reserved_sectors;       // 0x0E
    uint8_t  num_fats;               // 0x10
    uint16_t root_entries;           // 0x11
    uint16_t total_sectors_16;       // 0x13
    uint8_t  media;                  // 0x15
    uint16_t fat_size_16;            // 0x16
    uint16_t sectors_per_track;      // 0x18
    uint16_t heads;                  // 0x1A
    uint32_t hidden_sectors;         // 0x1C
    uint32_t total_sectors_32;       // 0x20
    /* FAT32 扩展 */
    uint32_t fat_size_32;            // 0x24
    uint16_t flags;                  // 0x28
    uint16_t version;                // 0x2A
    uint32_t root_cluster;           // 0x2C
    uint16_t fsinfo_sector;          // 0x30
    uint16_t backup_boot_sector;     // 0x32
    uint8_t  reserved[12];           // 0x34
    uint8_t  drive_number;           // 0x40
    uint8_t  nt_flags;               // 0x41
    uint8_t  signature;              // 0x42
    uint32_t serial;                 // 0x43
    char     label[11];              // 0x47
    char     fs_type[8];             // 0x52
} __attribute__((packed)) fat32_bpb_t;

/* 目录项 */
typedef struct {
    char     name[11];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  creation_tenths;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_access;
    uint16_t first_cluster_high;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_low;
    uint32_t file_size;
} __attribute__((packed)) fat32_dir_entry_t;

/* FAT32 文件系统信息 */
typedef struct fat32_fs {
    uint32_t first_data_sector;
    uint32_t data_sectors;
    uint32_t total_clusters;
    uint32_t fat_start_sector;
    uint32_t fat_size_sectors;   /* FAT表大小（扇区数） */
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_sector;
    uint32_t root_cluster;
    uint32_t *fat;               /* 缓存的FAT表 */
} fat32_fs_t;

/* 文件句柄 */
typedef struct {
    fat32_fs_t *fs;
    uint32_t    start_cluster;
    uint32_t    current_cluster;
    uint32_t    size;
    uint32_t    offset;
} fat32_file_t;

/* 初始化与挂载 */
int  fat32_mount(fat32_fs_t *fs);
void fat32_unmount(fat32_fs_t *fs);

/* 文件操作 */
int  fat32_open(fat32_fs_t *fs, const char *filename, fat32_file_t *file);
int  fat32_read(fat32_file_t *file, void *buf, uint32_t size);
int  fat32_write(fat32_file_t *file, const void *buf, uint32_t size);
void fat32_close(fat32_file_t *file);
int  fat32_create_file(fat32_fs_t *fs, const char *filename, fat32_file_t *file);

/* 目录操作 */
void fat32_list_root(fat32_fs_t *fs);
void fat32_flush(fat32_fs_t *fs); /* 强制写回FAT */

#endif