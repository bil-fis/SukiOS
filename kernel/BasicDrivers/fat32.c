#include "kernel/fat32.h"
#include "kernel/ide.h"
#include "kernel/tty.h"
#include "kernel/heap.h"
#include <stddef.h>

/* 从磁盘读取多个扇区 */
static int disk_read(uint32_t lba, uint32_t count, void *buf)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (!ide_read_sector(lba + i, (uint8_t *)buf + i * 512))
            return -1;
    }
    return 0;
}

/* 将普通文件名转换为 8.3 短文件名（11 字节，空格填充） */
static void to_short_filename(const char *input, char output[11])
{
    for (int i = 0; i < 11; ++i) output[i] = ' ';
    int i = 0, j = 0;
    // 处理文件名部分（最多 8 个字符）
    while (i < 8 && input[j] != '\0' && input[j] != '.') {
        output[i++] = (input[j] >= 'a' && input[j] <= 'z') ? input[j] - 32 : input[j];
        j++;
    }
    // 跳过点号
    if (input[j] == '.') j++;
    // 处理扩展名部分（最多 3 个字符）
    i = 8;
    while (i < 11 && input[j] != '\0') {
        output[i++] = (input[j] >= 'a' && input[j] <= 'z') ? input[j] - 32 : input[j];
        j++;
    }
}

/* FAT 链下一个簇号 */
static inline uint32_t fat_next_cluster(fat32_fs_t *fs, uint32_t cluster)
{
    if (cluster < 2 || cluster >= fs->total_clusters + 2) return 0x0FFFFFF7;
    return fs->fat[cluster] & 0x0FFFFFFF;
}

/* 簇号 -> LBA */
static inline uint32_t cluster_to_lba(fat32_fs_t *fs, uint32_t cluster)
{
    return fs->first_data_sector + (cluster - 2) * fs->sectors_per_cluster;
}

/* 挂载 FAT32 */
int fat32_mount(fat32_fs_t *fs)
{
    uint8_t boot[512];
    if (disk_read(0, 1, boot) != 0) return -1;
    fat32_bpb_t *bpb = (fat32_bpb_t *)boot;

    if (bpb->signature != 0x28 && bpb->signature != 0x29) {
        tty_print("[FAT32] Invalid boot signature\n");
        return -1;
    }
    if (bpb->bytes_per_sector != 512) {
        tty_print("[FAT32] Unsupported sector size\n");
        return -1;
    }

    fs->bytes_per_sector = 512;
    fs->sectors_per_cluster = bpb->sectors_per_cluster;
    fs->root_cluster = bpb->root_cluster;

    uint32_t total_sectors = bpb->total_sectors_16 ? bpb->total_sectors_16 : bpb->total_sectors_32;
    uint32_t fat_size_sectors = bpb->fat_size_32;
    uint32_t fat_count = bpb->num_fats;
    uint32_t root_dir_sectors = 0;   // FAT32 不使用根目录扇区，直接使用 cluster
    fs->fat_start_sector = bpb->reserved_sectors;
    fs->first_data_sector = bpb->reserved_sectors + fat_count * fat_size_sectors + root_dir_sectors;
    fs->data_sectors = total_sectors - fs->first_data_sector;
    fs->total_clusters = fs->data_sectors / bpb->sectors_per_cluster;

    // 加载 FAT 表
    uint32_t fat_bytes = fat_size_sectors * 512;
    fs->fat = (uint32_t *)kmalloc(fat_bytes);
    if (!fs->fat) return -1;
    if (disk_read(fs->fat_start_sector, fat_size_sectors, fs->fat) != 0) {
        kfree(fs->fat);
        return -1;
    }

    tty_print("[FAT32] Mounted, ");
    tty_print_dec(fs->total_clusters);
    tty_print(" clusters.\n");
    return 0;
}

void fat32_unmount(fat32_fs_t *fs)
{
    if (fs->fat) kfree(fs->fat);
    fs->fat = NULL;
}

void fat32_list_root(fat32_fs_t *fs)
{
    uint32_t cluster = fs->root_cluster;
    uint8_t *buf = (uint8_t *)kmalloc(512 * fs->sectors_per_cluster);
    if (!buf) return;

    while (cluster < 0x0FFFFFF8 && cluster >= 2) {
        disk_read(cluster_to_lba(fs, cluster), fs->sectors_per_cluster, buf);
        fat32_dir_entry_t *entry = (fat32_dir_entry_t *)buf;
        int count = (512 * fs->sectors_per_cluster) / sizeof(fat32_dir_entry_t);
        for (int i = 0; i < count; ++i) {
            if ((unsigned char)entry[i].name[0] == 0x00) break;
            if ((unsigned char)entry[i].name[0] == 0xE5) continue;
            if (entry[i].attr == 0x0F) continue;   // 跳过长文件名项
            // 打印 8.3 文件名
            for (int j = 0; j < 11; ++j) {
                char c = entry[i].name[j];
                if (c == ' ') break;
                tty_putchar(c);
            }
            if (entry[i].attr & 0x10) {
                tty_print(" [DIR]\n");
            } else {
                tty_print(" (");
                tty_print_dec(entry[i].file_size);
                tty_print(" bytes)\n");
            }
        }
        cluster = fat_next_cluster(fs, cluster);
    }
    kfree(buf);
}

int fat32_open(fat32_fs_t *fs, const char *filename, fat32_file_t *file)
{
    char short_name[11];
    to_short_filename(filename, short_name);

    uint32_t cluster = fs->root_cluster;
    uint8_t *buf = (uint8_t *)kmalloc(512 * fs->sectors_per_cluster);
    if (!buf) return -1;

    while (cluster < 0x0FFFFFF8 && cluster >= 2) {
        disk_read(cluster_to_lba(fs, cluster), fs->sectors_per_cluster, buf);
        fat32_dir_entry_t *entry = (fat32_dir_entry_t *)buf;
        int count = (512 * fs->sectors_per_cluster) / sizeof(fat32_dir_entry_t);
        for (int i = 0; i < count; ++i) {
            if ((unsigned char)entry[i].name[0] == 0x00) break;
            if ((unsigned char)entry[i].name[0] == 0xE5) continue;
            if (entry[i].attr == 0x0F) continue;
            // 比较 11 字节短名称（空格填充，大小写不敏感）
            int match = 1;
            for (int j = 0; j < 11; ++j) {
                char a = entry[i].name[j];
                char b = short_name[j];
                if (a >= 'a' && a <= 'z') a -= 32;
                if (a != b) { match = 0; break; }
            }
            if (match) {
                file->fs = fs;
                file->start_cluster = entry[i].first_cluster_low |
                                     ((uint32_t)entry[i].first_cluster_high << 16);
                file->current_cluster = file->start_cluster;
                file->size = entry[i].file_size;
                file->offset = 0;
                kfree(buf);
                return 0;
            }
        }
        cluster = fat_next_cluster(fs, cluster);
    }
    kfree(buf);
    return -1;
}

int fat32_read(fat32_file_t *file, void *buf, uint32_t size)
{
    uint8_t *dst = (uint8_t *)buf;
    uint32_t remaining = size;
    fat32_fs_t *fs = file->fs;
    uint32_t cluster = file->current_cluster;
    uint32_t offset = file->offset;
    uint32_t cluster_size = fs->sectors_per_cluster * 512;

    while (remaining > 0 && cluster < 0x0FFFFFF8 && cluster >= 2) {
        uint32_t lba = cluster_to_lba(fs, cluster);
        uint32_t sector_in_cluster = offset / 512;
        uint32_t byte_in_sector = offset % 512;
        uint32_t sector_lba = lba + sector_in_cluster;
        uint8_t temp[512];
        if (!ide_read_sector(sector_lba, temp)) return -1;

        uint32_t chunk = remaining;
        if (chunk > 512 - byte_in_sector) chunk = 512 - byte_in_sector;
        for (uint32_t i = 0; i < chunk; ++i) dst[i] = temp[byte_in_sector + i];
        dst += chunk;
        remaining -= chunk;
        offset += chunk;
        if (offset >= cluster_size) {
            cluster = fat_next_cluster(fs, cluster);
            offset = 0;
        }
    }
    file->current_cluster = cluster;
    file->offset = offset;
    return (int)(size - remaining);
}

void fat32_close(fat32_file_t *file)
{
    (void)file;
}