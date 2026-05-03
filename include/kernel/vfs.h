#ifndef SUKIOS_VFS_H
#define SUKIOS_VFS_H

#include <stdint.h>
#include <stddef.h>

/* ========== 标志常量 ========== */
#define VFS_FLAG_DIR        0x01
#define VFS_FLAG_FILE       0x02
#define VFS_FLAG_MOUNTPOINT 0x04

/* 文件打开标志 */
#define O_RDONLY 1
#define O_WRONLY 2
#define O_RDWR   3

/* ========== 结构体前置声明 ========== */
struct vfs_node;
struct vfs_dir;

/* ========== 目录条目（readdir 返回） ========== */
typedef struct vfs_dir_entry {
    char name[256];
    uint32_t inode;
    uint32_t type;          // VFS_FLAG_FILE 或 VFS_FLAG_DIR
} vfs_dir_entry_t;

/* ========== 目录迭代器 ========== */
typedef struct vfs_dir {
    struct vfs_node *node;  // 指向目录节点
    void *internal;         // 文件系统特定的迭代状态（如 FAT32 当前簇/偏移）
} vfs_dir_t;

/* ========== 文件统计信息 ========== */
typedef struct vfs_stat {
    uint32_t size;
    uint32_t flags;         // VFS_FLAG_FILE / VFS_FLAG_DIR
    uint32_t permissions;
} vfs_stat_t;

/* ========== 文件操作接口表 ========== */
typedef struct vfs_file_ops {
    int (*open)(struct vfs_node *node, uint32_t flags);
    int (*close)(struct vfs_node *node);
    int (*read)(struct vfs_node *node, void *buf, uint32_t count, uint32_t offset);
    int (*write)(struct vfs_node *node, const void *buf, uint32_t count, uint32_t offset);
    struct vfs_node *(*finddir)(struct vfs_node *node, const char *name);
    int (*mkdir)(struct vfs_node *node, const char *name);
    int (*create)(struct vfs_node *node, const char *name, uint32_t flags);
    int (*delete)(struct vfs_node *node, const char *name);
    int (*readdir)(struct vfs_node *node, struct vfs_dir *dir, vfs_dir_entry_t *entry);
    int (*ioctl)(struct vfs_node *node, int request, void *arg);
} vfs_file_ops_t;

/* ========== VFS 节点 ========== */
typedef struct vfs_node {
    char name[256];
    uint32_t flags;
    uint32_t permissions;
    uint32_t inode;
    uint32_t size;
    void *fs_data;                  // 驱动私有数据（如 fat32_file_t*）

    vfs_file_ops_t *ops;

    struct vfs_node *parent;
    struct vfs_node *children;      // 子节点链表（主要供 readdir 动态构建）
    struct vfs_node *next;          // 同级兄弟
    struct vfs_node *mount_point;   // 若为挂载点，可指向另一文件系统的根
} vfs_node_t;

/* ========== 挂载点描述 ========== */
typedef struct mount_point {
    char device[64];
    char path[256];
    uint32_t fs_type;               // 例如 FS_FAT32
    vfs_node_t *root;               // 目标文件系统的根节点
    void *fs_data;
    struct mount_point *next;
} mount_point_t;

/* ========== 文件描述符条目 ========== */
typedef struct {
    vfs_node_t *node;
    uint32_t pos;
    uint32_t flags;
    int ref_count;
} file_handle_t;

#define MAX_FDS 256
extern file_handle_t fd_table[MAX_FDS];

/* ========== 初始化 ========== */
void vfs_init(void);

/* ========== 文件系统挂载 ========== */
int vfs_mount(const char *device, const char *path, uint32_t fs_type, void *fs_data);

/* ========== 节点操作 ========== */
vfs_node_t *vfs_open_node(const char *path);
int vfs_read_node(vfs_node_t *node, void *buf, uint32_t count, uint32_t offset);
int vfs_write_node(vfs_node_t *node, const void *buf, uint32_t count, uint32_t offset);
int vfs_close_node(vfs_node_t *node);
int vfs_create(const char *path, uint32_t flags);
int vfs_delete(const char *path);
vfs_node_t *vfs_finddir(vfs_node_t *dir, const char *name);
int vfs_stat_node(vfs_node_t *node, vfs_stat_t *stat);

/* ========== 目录迭代 ========== */
vfs_dir_t *vfs_opendir(const char *path);
int vfs_readdir(vfs_dir_t *dir, vfs_dir_entry_t *entry);
void vfs_closedir(vfs_dir_t *dir);

/* ========== 文件描述符层 ========== */
int vfs_open(const char *path, uint32_t flags);
int vfs_read(int fd, void *buf, uint32_t count);
int vfs_write(int fd, const void *buf, uint32_t count);
int vfs_close(int fd);

/* ========== 文件系统类型宏 ========== */
#define FS_FAT32 1

size_t vfs_get_size(int fd);

#endif