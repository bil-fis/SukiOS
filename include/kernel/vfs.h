#ifndef SUKIOS_VFS_H
#define SUKIOS_VFS_H

#include <stdint.h>

#define VFS_FLAG_DIR        0x01
#define VFS_FLAG_FILE       0x02
#define VFS_FLAG_MOUNTPOINT 0x04

#define O_RDONLY 1
#define O_WRONLY 2
#define O_RDWR   3

/* 前置声明 */
struct vfs_node;

/* 文件操作函数指针表 */
typedef struct vfs_file_ops {
    int (*open)(struct vfs_node *node, uint32_t flags);
    int (*close)(struct vfs_node *node);
    int (*read)(struct vfs_node *node, void *buf, uint32_t count, uint32_t offset);
    int (*write)(struct vfs_node *node, const void *buf, uint32_t count, uint32_t offset);
    struct vfs_node *(*finddir)(struct vfs_node *node, const char *name);
    int (*mkdir)(struct vfs_node *node, const char *name);
    int (*create)(struct vfs_node *node, const char *name, uint32_t flags);
    int (*delete)(struct vfs_node *node, const char *name);
} vfs_file_ops_t;

/* VFS 节点 */
typedef struct vfs_node {
    char name[256];
    uint32_t flags;           // VFS_FLAG_*
    uint32_t permissions;
    uint32_t inode;           // 文件系统特定
    uint32_t size;
    void *fs_data;            // 驱动私有数据（如 fat32_file_t*）

    vfs_file_ops_t *ops;      // 指向操作表

    struct vfs_node *parent;
    struct vfs_node *children;
    struct vfs_node *next;    // 同级链表
    struct vfs_node *mount_point; // 如果是挂载点，指向另一个文件系统的根节点
} vfs_node_t;

/* 挂载点 */
typedef struct mount_point {
    char device[64];
    char path[256];           // 挂载路径，如 "/"
    uint32_t fs_type;         // FS_FAT32 等
    vfs_node_t *root;
    void *fs_data;
    struct mount_point *next;
} mount_point_t;

/* 文件描述符条目 */
typedef struct {
    vfs_node_t *node;
    uint32_t pos;
    uint32_t flags;
    int ref_count;
} file_handle_t;

#define MAX_FDS 256
extern file_handle_t fd_table[MAX_FDS];

/* VFS 初始化 */
void vfs_init(void);

/* 挂载、打开、读写等高层接口 */
int vfs_mount(const char *device, const char *path, uint32_t fs_type, void *fs_data);
vfs_node_t *vfs_open_node(const char *path);
int vfs_read_node(vfs_node_t *node, void *buf, uint32_t count, uint32_t offset);
int vfs_write_node(vfs_node_t *node, const void *buf, uint32_t count, uint32_t offset);
int vfs_close_node(vfs_node_t *node);
int vfs_create(const char *path, uint32_t flags);
int vfs_delete(const char *path);
vfs_node_t *vfs_finddir(vfs_node_t *dir, const char *name);

/* 文件描述符层（可选） */
int vfs_open(const char *path, uint32_t flags);
int vfs_read(int fd, void *buf, uint32_t count);
int vfs_write(int fd, const void *buf, uint32_t count);
int vfs_close(int fd);

/* 类型宏 */
#define FS_FAT32 1

#endif