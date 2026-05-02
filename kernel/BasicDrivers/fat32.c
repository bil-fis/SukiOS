#include "kernel/fat32.h"
#include "kernel/ide.h"
#include "kernel/tty.h"
#include "kernel/heap.h"

/* ====== 磁盘物理读写 ====== */
static int disk_read(uint32_t lba, uint32_t count, void *buf) {
    for (uint32_t i = 0; i < count; ++i)
        if (!ide_read_sector(lba + i, (uint8_t*)buf + i * 512)) return -1;
    return 0;
}

static int disk_write(uint32_t lba, uint32_t count, const void *buf) {
    for (uint32_t i = 0; i < count; ++i) {
        if (!ide_write_sector(lba + i, (const uint8_t*)buf + i * 512)) {
            tty_print("[FAT32] Write error at LBA ");
            tty_print_hex64(lba + i);
            tty_print("\n");
            return -1;
        }
    }
    return 0;
}

/* ====== 短文件名转换 ====== */
static void to_short(const char *in, char out[11]) {
    int i = 0, j = 0;
    for (int k = 0; k < 11; ++k) out[k] = ' ';
    while (i < 8 && in[j] && in[j] != '.') {
        char c = in[j++];
        out[i++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    if (in[j] == '.') j++;
    i = 8;
    while (i < 11 && in[j]) {
        char c = in[j++];
        out[i++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
}

/* ====== FAT 表辅助函数 ====== */
#define FAT_EOF        0x0FFFFFF8

static inline uint32_t fat_next_cluster(fat32_fs_t *fs, uint32_t cluster) {
    if (cluster < 2 || cluster >= fs->total_clusters + 2) return FAT_EOF;
    return fs->fat[cluster] & 0x0FFFFFFF;
}

static inline uint32_t cluster_to_lba(fat32_fs_t *fs, uint32_t cluster) {
    return fs->first_data_sector + (cluster - 2) * fs->sectors_per_cluster;
}

static uint32_t find_free_cluster(fat32_fs_t *fs) {
    for (uint32_t i = 2; i < fs->total_clusters + 2; ++i)
        if ((fs->fat[i] & 0x0FFFFFFF) == 0) return i;
    return 0;
}

/* ====== 挂载/卸载 ====== */
int fat32_mount(fat32_fs_t *fs) {
    uint8_t boot[512];
    if (disk_read(0, 1, boot)) return -1;
    fat32_bpb_t *bpb = (fat32_bpb_t*)boot;

    if (bpb->signature != 0x28 && bpb->signature != 0x29) {
        tty_print("[FAT32] Bad signature\n");
        return -1;
    }
    if (bpb->bytes_per_sector != 512) {
        tty_print("[FAT32] Sector size != 512\n");
        return -1;
    }

    fs->bytes_per_sector    = 512;
    fs->sectors_per_cluster = bpb->sectors_per_cluster;
    fs->root_cluster        = bpb->root_cluster;
    fs->fat_start_sector    = bpb->reserved_sectors;

    uint32_t total_sectors = bpb->total_sectors_16 ? bpb->total_sectors_16 : bpb->total_sectors_32;
    uint32_t fat_size      = bpb->fat_size_32;
    fs->fat_size_sectors   = fat_size;
    uint32_t root_dir_sec  = 0;
    fs->first_data_sector  = bpb->reserved_sectors + bpb->num_fats * fat_size + root_dir_sec;
    fs->data_sectors       = total_sectors - fs->first_data_sector;
    fs->total_clusters     = fs->data_sectors / bpb->sectors_per_cluster;

    fs->fat = (uint32_t*)kmalloc(fat_size * 512);
    if (!fs->fat) return -1;
    if (disk_read(fs->fat_start_sector, fat_size, fs->fat)) {
        kfree(fs->fat);
        return -1;
    }

    tty_print("[FAT32] Mounted, ");
    tty_print_dec(fs->total_clusters);
    tty_print(" clusters.\n");
    return 0;
}

void fat32_unmount(fat32_fs_t *fs) {
    if (fs->fat) { kfree(fs->fat); fs->fat = NULL; }
}

/* ====== 列出根目录 ====== */
void fat32_list_root(fat32_fs_t *fs) {
    uint32_t cluster = fs->root_cluster;
    uint8_t *buf = (uint8_t*)kmalloc(fs->sectors_per_cluster * 512);
    if (!buf) return;

    while (cluster < FAT_EOF && cluster >= 2) {
        if (disk_read(cluster_to_lba(fs, cluster), fs->sectors_per_cluster, buf)) break;
        fat32_dir_entry_t *e = (fat32_dir_entry_t*)buf;
        int count = (fs->sectors_per_cluster * 512) / sizeof(fat32_dir_entry_t);
        for (int i = 0; i < count; ++i) {
            if ((unsigned char)e[i].name[0] == 0x00) break;
            if ((unsigned char)e[i].name[0] == 0xE5) continue;
            if (e[i].attr == 0x0F) continue;
            for (int j = 0; j < 11; ++j) {
                if (e[i].name[j] == ' ') break;
                tty_putchar(e[i].name[j]);
            }
            if (e[i].attr & 0x10) tty_print(" [DIR]\n");
            else { tty_print(" ("); tty_print_dec(e[i].file_size); tty_print(")\n"); }
        }
        cluster = fat_next_cluster(fs, cluster);
    }
    kfree(buf);
}

/* ====== 打开文件 ====== */
int fat32_open(fat32_fs_t *fs, const char *filename, fat32_file_t *file) {
    char target[11]; to_short(filename, target);

    uint32_t cluster = fs->root_cluster;
    uint8_t *buf = (uint8_t*)kmalloc(fs->sectors_per_cluster * 512);
    if (!buf) return -1;

    while (cluster < FAT_EOF && cluster >= 2) {
        if (disk_read(cluster_to_lba(fs, cluster), fs->sectors_per_cluster, buf)) break;
        fat32_dir_entry_t *e = (fat32_dir_entry_t*)buf;
        int count = (fs->sectors_per_cluster * 512) / sizeof(fat32_dir_entry_t);
        for (int i = 0; i < count; ++i) {
            if ((unsigned char)e[i].name[0] == 0x00) break;
            if ((unsigned char)e[i].name[0] == 0xE5) continue;
            if (e[i].attr == 0x0F) continue;
            int match = 1;
            for (int j = 0; j < 11; ++j) {
                char a = e[i].name[j];
                if (a >= 'a' && a <= 'z') a -= 32;
                if (a != target[j]) { match = 0; break; }
            }
            if (match) {
                file->fs = fs;
                file->start_cluster = e[i].first_cluster_low | ((uint32_t)e[i].first_cluster_high << 16);
                file->current_cluster = file->start_cluster;
                file->size = e[i].file_size;
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

/* ====== 读取文件 ====== */
int fat32_read(fat32_file_t *file, void *buf, uint32_t size) {
    uint8_t *dst = (uint8_t*)buf;
    uint32_t remain = size;
    fat32_fs_t *fs = file->fs;
    uint32_t cluster = file->current_cluster;
    uint32_t offset = file->offset;
    uint32_t clu_size = fs->sectors_per_cluster * 512;

    while (remain && cluster < FAT_EOF && cluster >= 2) {
        uint32_t lba = cluster_to_lba(fs, cluster);
        uint32_t sec_off = offset / 512;
        uint32_t byte_off = offset % 512;
        uint8_t temp[512];
        if (!ide_read_sector(lba + sec_off, temp)) return -1;
        uint32_t chunk = remain;
        if (chunk > 512 - byte_off) chunk = 512 - byte_off;
        for (uint32_t i = 0; i < chunk; ++i) dst[i] = temp[byte_off + i];
        dst    += chunk;
        remain -= chunk;
        offset += chunk;
        if (offset >= clu_size) { cluster = fat_next_cluster(fs, cluster); offset = 0; }
    }
    file->current_cluster = cluster;
    file->offset = offset;
    return (int)(size - remain);
}

/* ====== 写入文件 ====== */
int fat32_write(fat32_file_t *file, const void *buf, uint32_t size) {
    const uint8_t *src = (const uint8_t*)buf;
    uint32_t remain = size;
    fat32_fs_t *fs = file->fs;
    uint32_t cluster = file->current_cluster;
    uint32_t offset = file->offset;
    uint32_t clu_size = fs->sectors_per_cluster * 512;

    while (remain) {
        if (offset >= clu_size) {
            uint32_t next = fat_next_cluster(fs, cluster);
            if (next >= FAT_EOF) {
                next = find_free_cluster(fs);
                if (!next) return -1;
                fs->fat[cluster] = next;
                fs->fat[next] = FAT_EOF;
            }
            cluster = next;
            offset = 0;
        }
        uint32_t lba = cluster_to_lba(fs, cluster);
        uint32_t sec_off = offset / 512;
        uint32_t byte_off = offset % 512;
        uint8_t temp[512];
        if (!ide_read_sector(lba + sec_off, temp)) return -1;
        uint32_t chunk = remain;
        if (chunk > 512 - byte_off) chunk = 512 - byte_off;
        for (uint32_t i = 0; i < chunk; ++i) temp[byte_off + i] = src[i];
        if (!ide_write_sector(lba + sec_off, temp)) return -1;
        src    += chunk;
        remain -= chunk;
        offset += chunk;
    }
    file->current_cluster = cluster;
    file->offset = offset;
    if (file->offset > file->size) file->size = file->offset;
    return (int)size;
}

/* ====== 更新目录项大小 ====== */
static int fat32_update_dir_entry(fat32_file_t *file) {
    if (!file->fs) return -1;

    fat32_fs_t *fs = file->fs;
    uint32_t cluster = fs->root_cluster;
    uint8_t *buf = (uint8_t*)kmalloc(fs->sectors_per_cluster * 512);
    if (!buf) return -1;

    while (cluster < FAT_EOF && cluster >= 2) {
        if (disk_read(cluster_to_lba(fs, cluster), fs->sectors_per_cluster, buf)) break;
        fat32_dir_entry_t *e = (fat32_dir_entry_t*)buf;
        int count = (fs->sectors_per_cluster * 512) / sizeof(fat32_dir_entry_t);
        for (int i = 0; i < count; ++i) {
            if ((unsigned char)e[i].name[0] == 0x00) break;
            if ((unsigned char)e[i].name[0] == 0xE5) continue;
            if (e[i].attr == 0x0F) continue;
            uint32_t ent_cluster = e[i].first_cluster_low | ((uint32_t)e[i].first_cluster_high << 16);
            if (ent_cluster == file->start_cluster) {
                e[i].file_size = file->size;
                if (disk_write(cluster_to_lba(fs, cluster), fs->sectors_per_cluster, buf)) {
                    kfree(buf);
                    return -1;
                }
                kfree(buf);
                return 0;
            }
        }
        cluster = fat_next_cluster(fs, cluster);
    }
    kfree(buf);
    return -1;
}

/* ====== 关闭文件 ====== */
void fat32_close(fat32_file_t *file) {
    if (!file->fs) return;
    fat32_update_dir_entry(file);
    fat32_flush(file->fs);
}

/* ====== 强制写回FAT表 ====== */
void fat32_flush(fat32_fs_t *fs) {
    if (!fs->fat || !fs->fat_size_sectors) return;
    tty_print("[FAT32] Writing FAT...\n");
    disk_write(fs->fat_start_sector, fs->fat_size_sectors, fs->fat);
    tty_print("[FAT32] FAT flushed.\n");
}

/* ====== 创建文件 ====== */
int fat32_create_file(fat32_fs_t *fs, const char *filename, fat32_file_t *file) {
    char target[11]; to_short(filename, target);

    uint32_t cluster = fs->root_cluster;
    uint8_t *buf = (uint8_t*)kmalloc(fs->sectors_per_cluster * 512);
    if (!buf) return -1;
    int found = 0;
    uint32_t target_clu = 0, entry_idx = 0;

    while (cluster < FAT_EOF && cluster >= 2) {
        if (disk_read(cluster_to_lba(fs, cluster), fs->sectors_per_cluster, buf)) break;
        fat32_dir_entry_t *e = (fat32_dir_entry_t*)buf;
        int count = (fs->sectors_per_cluster * 512) / sizeof(fat32_dir_entry_t);
        for (int i = 0; i < count; ++i) {
            if ((unsigned char)e[i].name[0] == 0x00 || (unsigned char)e[i].name[0] == 0xE5) {
                found = 1; target_clu = cluster; entry_idx = i; break;
            }
        }
        if (found) break;
        uint32_t next = fat_next_cluster(fs, cluster);
        if (next >= FAT_EOF) break;
        cluster = next;
    }
    if (!found) { kfree(buf); return -1; }

    uint32_t new_clu = find_free_cluster(fs);
    if (!new_clu) { kfree(buf); return -1; }
    fs->fat[new_clu] = FAT_EOF;

    fat32_dir_entry_t *ent = &((fat32_dir_entry_t*)buf)[entry_idx];
    for (int i = 0; i < 11; ++i) ent->name[i] = target[i];
    ent->attr = 0x20;
    ent->first_cluster_low  = new_clu & 0xFFFF;
    ent->first_cluster_high = new_clu >> 16;
    ent->file_size = 0;
    disk_write(cluster_to_lba(fs, target_clu), fs->sectors_per_cluster, buf);
    kfree(buf);

    file->fs = fs;
    file->start_cluster = new_clu;
    file->current_cluster = new_clu;
    file->size = 0;
    file->offset = 0;
    fat32_flush(fs);
    return 0;
}

/* ====== 释放簇链 ====== */
static int fat32_free_cluster_chain(fat32_fs_t *fs, uint32_t start_cluster) {
    uint32_t cluster = start_cluster;
    while (cluster >= 2 && cluster < FAT_EOF) {
        uint32_t next = fs->fat[cluster] & 0x0FFFFFFF;
        fs->fat[cluster] = 0;          // 清零，表示空闲
        if (next >= FAT_EOF) break;
        cluster = next;
    }
    return 0;
}

/* ====== 删除文件 ====== */
int fat32_delete(fat32_fs_t *fs, const char *filename) {
    char target[11]; to_short(filename, target);

    // 1. 在根目录中找到对应目录项
    uint32_t dir_cluster = fs->root_cluster;
    uint8_t *buf = (uint8_t*)kmalloc(fs->sectors_per_cluster * 512);
    if (!buf) return -1;

    int found = 0;
    uint32_t file_first_cluster = 0;
    uint32_t entry_cluster = 0;
    int entry_index = 0;

    while (dir_cluster < FAT_EOF && dir_cluster >= 2) {
        if (disk_read(cluster_to_lba(fs, dir_cluster), fs->sectors_per_cluster, buf)) break;
        fat32_dir_entry_t *e = (fat32_dir_entry_t*)buf;
        int count = (fs->sectors_per_cluster * 512) / sizeof(fat32_dir_entry_t);
        for (int i = 0; i < count; ++i) {
            if ((unsigned char)e[i].name[0] == 0x00) break;
            if ((unsigned char)e[i].name[0] == 0xE5) continue;
            if (e[i].attr == 0x0F) continue;

            int match = 1;
            for (int j = 0; j < 11; ++j) {
                char a = e[i].name[j];
                if (a >= 'a' && a <= 'z') a -= 32;
                if (a != target[j]) { match = 0; break; }
            }
            if (match) {
                file_first_cluster = e[i].first_cluster_low | ((uint32_t)e[i].first_cluster_high << 16);
                e[i].name[0] = 0xE5;      // 标记已删除
                entry_cluster = dir_cluster;
                entry_index = i;
                found = 1;
                break;  // 跳出内层 for，仍需写回目录
            }
        }
        if (found) break;
        dir_cluster = fat_next_cluster(fs, dir_cluster);
    }

    if (!found) {
        kfree(buf);
        return -1;
    }

    // 2. 将修改后的目录簇写回磁盘
    if (disk_write(cluster_to_lba(fs, entry_cluster), fs->sectors_per_cluster, buf)) {
        kfree(buf);
        return -1;
    }
    kfree(buf);

    // 3. 释放文件占用的所有簇
    if (file_first_cluster != 0) {
        fat32_free_cluster_chain(fs, file_first_cluster);
    }

    // 4. 刷新 FAT 表
    fat32_flush(fs);

    return 0;
}