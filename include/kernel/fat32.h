#ifndef SUKIOS_FAT32_H
#define SUKIOS_FAT32_H

#include <stdint.h>

/* FAT32 BPB + 扩展 */
typedef struct {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t  media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;

    uint32_t fat_size_32;
    uint16_t flags;
    uint16_t version;
    uint32_t root_cluster;
    uint16_t fsinfo_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  nt_flags;
    uint8_t  signature;
    uint32_t serial;
    char     label[11];
    char     fs_type[8];
} __attribute__((packed)) fat32_bpb_t;

/* 8.3 短目录项 */
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

/* 挂载信息 */
typedef struct {
    uint32_t first_data_sector;
    uint32_t data_sectors;
    uint32_t total_clusters;
    uint32_t fat_start_sector;
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_sector;
    uint32_t root_cluster;
    uint32_t *fat;
} fat32_fs_t;

/* 文件句柄 */
typedef struct {
    fat32_fs_t *fs;
    uint32_t start_cluster;
    uint32_t current_cluster;
    uint32_t size;
    uint32_t offset;
} fat32_file_t;

int  fat32_mount(fat32_fs_t *fs);
void fat32_unmount(fat32_fs_t *fs);
int  fat32_open(fat32_fs_t *fs, const char *filename, fat32_file_t *file);
int  fat32_read(fat32_file_t *file, void *buf, uint32_t size);
void fat32_close(fat32_file_t *file);
void fat32_list_root(fat32_fs_t *fs);

#endif