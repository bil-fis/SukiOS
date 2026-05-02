#ifndef SUKIOS_FAT32_VFS_H
#define SUKIOS_FAT32_VFS_H

#include "kernel/vfs.h"
#include "kernel/fat32.h"

vfs_node_t *fat32_mount_to_vfs(fat32_fs_t *fs);

#endif