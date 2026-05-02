#include "kernel/fat32_vfs.h"
#include "kernel/heap.h"
#include <stddef.h>

static fat32_fs_t *fat32_fs = NULL;

/* FAT32 文件节点操作实现 */
static int fat32_vfs_read(vfs_node_t *node, void *buf, uint32_t count, uint32_t offset)
{
    if (!node || !node->fs_data) return -1;
    fat32_file_t *file = (fat32_file_t *)node->fs_data;
    fat32_seek(file, offset);
    return fat32_read(file, buf, count);
}

static int fat32_vfs_write(vfs_node_t *node, const void *buf, uint32_t count, uint32_t offset)
{
    if (!node || !node->fs_data) return -1;
    fat32_file_t *file = (fat32_file_t *)node->fs_data;
    fat32_seek(file, offset);
    return fat32_write(file, buf, count);
}

static int fat32_vfs_close(vfs_node_t *node)
{
    if (!node || !node->fs_data) return -1;
    fat32_file_t *file = (fat32_file_t *)node->fs_data;
    fat32_close(file);
    kfree(file);
    node->fs_data = NULL;
    return 0;
}

static vfs_node_t *fat32_vfs_finddir_dir(vfs_node_t *dir, const char *name)
{
    if (!dir || !(dir->flags & VFS_FLAG_DIR)) return NULL;
    fat32_fs_t *fs = fat32_fs;
    if (!fs) return NULL;

    fat32_file_t tmp;
    if (fat32_open(fs, name, &tmp) != 0)
        return NULL;

    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) { fat32_close(&tmp); return NULL; }

    int i;
    for (i = 0; name[i] && i < 255; i++) node->name[i] = name[i];
    node->name[i] = '\0';
    node->flags = VFS_FLAG_FILE;
    node->size = tmp.size;
    node->inode = tmp.start_cluster;
    fat32_file_t *file = (fat32_file_t *)kmalloc(sizeof(fat32_file_t));
    if (!file) { kfree(node); fat32_close(&tmp); return NULL; }
    *file = tmp;
    node->fs_data = file;
    extern vfs_file_ops_t fat32_file_ops;
    node->ops = &fat32_file_ops;
    return node;
}

static int fat32_vfs_create(vfs_node_t *root, const char *name, uint32_t flags)
{
    (void)flags;
    fat32_fs_t *fs = fat32_fs;
    if (!fs) return -1;
    fat32_file_t file;
    if (fat32_create_file(fs, name, &file) != 0) return -1;
    fat32_close(&file);
    return 0;
}

static int fat32_vfs_delete(vfs_node_t *root, const char *name)
{
    fat32_fs_t *fs = fat32_fs;
    if (!fs) return -1;
    return fat32_delete(fs, name);
}

/* FAT32 文件操作表 */
vfs_file_ops_t fat32_file_ops = {
    .read = fat32_vfs_read,
    .write = fat32_vfs_write,
    .close = fat32_vfs_close,
    .finddir = fat32_vfs_finddir_dir,
    .create = fat32_vfs_create,
    .delete = fat32_vfs_delete,
};

/**
 * 生成 FAT32 根节点，并准备挂载到 VFS。
 */
vfs_node_t *fat32_mount_to_vfs(fat32_fs_t *fs)
{
    fat32_fs = fs;
    vfs_node_t *root = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!root) return NULL;

    root->name[0] = '/';
    root->name[1] = '\0';
    root->flags = VFS_FLAG_DIR | VFS_FLAG_MOUNTPOINT;
    root->size = 0;
    root->inode = 0;
    root->fs_data = NULL;

    static vfs_file_ops_t fat32_dir_ops = {
        .finddir = fat32_vfs_finddir_dir,
        .create = fat32_vfs_create,
        .delete = fat32_vfs_delete,
    };
    root->ops = &fat32_dir_ops;
    root->parent = NULL;
    root->children = NULL;
    root->next = NULL;
    root->mount_point = NULL;
    return root;
}