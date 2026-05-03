#include "kernel/vfs.h"
#include "kernel/heap.h"
#include "kernel/tty.h"
#include <stddef.h>

/* 全局挂载点链表 */
static mount_point_t *mount_list = NULL;

/* 全局文件描述符表 */
file_handle_t fd_table[MAX_FDS];

/* ========== 初始化 ========== */
void vfs_init(void)
{
    tty_print("[VFS] Init.\n");
    for (int i = 0; i < MAX_FDS; i++) {
        fd_table[i].node = NULL;
        fd_table[i].pos = 0;
        fd_table[i].flags = 0;
        fd_table[i].ref_count = 0;
    }
}

/* ========== 挂载 ========== */
int vfs_mount(const char *device, const char *path, uint32_t fs_type, void *fs_data)
{
    mount_point_t *mp = (mount_point_t *)kmalloc(sizeof(mount_point_t));
    if (!mp) return -1;

    int i;
    for (i = 0; device[i] && i < 63; i++) mp->device[i] = device[i];
    mp->device[i] = '\0';
    for (i = 0; path[i] && i < 255; i++) mp->path[i] = path[i];
    mp->path[i] = '\0';

    mp->fs_type = fs_type;
    mp->fs_data = fs_data;
    mp->root = (vfs_node_t *)fs_data;   // 调用者传入的根节点
    mp->next = mount_list;
    mount_list = mp;

    tty_print("[VFS] Mounted '");
    tty_print(mp->device);
    tty_print("' at ");
    tty_print(mp->path);
    tty_print("\n");
    return 0;
}

/* ========== 路径解析（支持多级目录，但暂未处理挂载点跳转） ========== */
static vfs_node_t *resolve_path(const char *path)
{
    if (!path || path[0] != '/') return NULL;

    /* 找到根挂载点 */
    mount_point_t *mp = mount_list;
    while (mp) {
        if (mp->path[0] == '/' && mp->path[1] == '\0')
            break;
        mp = mp->next;
    }
    if (!mp || !mp->root) return NULL;

    vfs_node_t *current = mp->root;
    const char *p = path + 1;       // 跳过 '/'

    if (*p == '\0') return current; // 根目录

    char token[256];
    while (*p) {
        int i = 0;
        while (*p && *p != '/' && i < 255)
            token[i++] = *p++;
        token[i] = '\0';
        if (*p == '/') p++;

        if (!(current->flags & VFS_FLAG_DIR) || !current->ops || !current->ops->finddir)
            return NULL;

        current = current->ops->finddir(current, token);
        if (!current) return NULL;
    }
    return current;
}

/* ========== 节点操作 ========== */
vfs_node_t *vfs_open_node(const char *path) {
    return resolve_path(path);
}

int vfs_read_node(vfs_node_t *node, void *buf, uint32_t count, uint32_t offset) {
    if (!node || !(node->flags & VFS_FLAG_FILE) || !node->ops || !node->ops->read)
        return -1;
    return node->ops->read(node, buf, count, offset);
}

int vfs_write_node(vfs_node_t *node, const void *buf, uint32_t count, uint32_t offset) {
    if (!node || !(node->flags & VFS_FLAG_FILE) || !node->ops || !node->ops->write)
        return -1;
    return node->ops->write(node, buf, count, offset);
}

int vfs_close_node(vfs_node_t *node) {
    if (!node || !node->ops || !node->ops->close) return -1;
    return node->ops->close(node);
}

vfs_node_t *vfs_finddir(vfs_node_t *dir, const char *name) {
    if (!dir || !(dir->flags & VFS_FLAG_DIR) || !dir->ops || !dir->ops->finddir)
        return NULL;
    return dir->ops->finddir(dir, name);
}

int vfs_stat_node(vfs_node_t *node, vfs_stat_t *stat) {
    if (!node || !stat) return -1;
    stat->size = node->size;
    stat->flags = node->flags;
    stat->permissions = node->permissions;
    return 0;
}

int vfs_create(const char *path, uint32_t flags) {
    if (!path || path[0] != '/') return -1;
    const char *filename = path + 1;
    mount_point_t *mp = mount_list;
    while (mp) {
        if (mp->path[0] == '/' && mp->path[1] == '\0') break;
        mp = mp->next;
    }
    if (!mp || !mp->root) return -1;
    if (!mp->root->ops || !mp->root->ops->create) return -1;
    return mp->root->ops->create(mp->root, filename, flags);
}

int vfs_delete(const char *path) {
    if (!path || path[0] != '/') return -1;
    const char *filename = path + 1;
    mount_point_t *mp = mount_list;
    while (mp) {
        if (mp->path[0] == '/' && mp->path[1] == '\0') break;
        mp = mp->next;
    }
    if (!mp || !mp->root) return -1;
    if (!mp->root->ops || !mp->root->ops->delete) return -1;
    return mp->root->ops->delete(mp->root, filename);
}

/* ========== 目录迭代 ========== */
vfs_dir_t *vfs_opendir(const char *path) {
    vfs_node_t *node = resolve_path(path);
    if (!node || !(node->flags & VFS_FLAG_DIR)) return NULL;

    vfs_dir_t *dir = (vfs_dir_t *)kmalloc(sizeof(vfs_dir_t));
    if (!dir) return NULL;
    dir->node = node;
    dir->internal = NULL;   // 文件系统驱动在第一次 readdir 时初始化

    return dir;
}

int vfs_readdir(vfs_dir_t *dir, vfs_dir_entry_t *entry) {
    if (!dir || !dir->node || !dir->node->ops || !dir->node->ops->readdir)
        return -1;
    return dir->node->ops->readdir(dir->node, dir, entry);
}

void vfs_closedir(vfs_dir_t *dir) {
    if (!dir) return;
    // 如果 internal 需要释放，可由文件系统提供 closedir 回调，但这里简单 kfree
    if (dir->internal) {
        kfree(dir->internal);
        dir->internal = NULL;
    }
    kfree(dir);
}

/* ========== 文件描述符层 ========== */
int vfs_open(const char *path, uint32_t flags) {
    vfs_node_t *node = vfs_open_node(path);
    if (!node || !(node->flags & VFS_FLAG_FILE)) return -1;

    int fd = -1;
    for (int i = 0; i < MAX_FDS; i++) {
        if (fd_table[i].node == NULL) {
            fd = i;
            break;
        }
    }
    if (fd == -1) return -1;

    fd_table[fd].node = node;
    fd_table[fd].flags = flags;
    fd_table[fd].pos = 0;
    fd_table[fd].ref_count = 1;
    return fd;
}

int vfs_read(int fd, void *buf, uint32_t count) {
    if (fd < 0 || fd >= MAX_FDS || !fd_table[fd].node) return -1;
    int ret = vfs_read_node(fd_table[fd].node, buf, count, fd_table[fd].pos);
    if (ret > 0) fd_table[fd].pos += ret;
    return ret;
}

int vfs_write(int fd, const void *buf, uint32_t count) {
    if (fd < 0 || fd >= MAX_FDS || !fd_table[fd].node) return -1;
    int ret = vfs_write_node(fd_table[fd].node, buf, count, fd_table[fd].pos);
    if (ret > 0) fd_table[fd].pos += ret;
    return ret;
}

int vfs_close(int fd) {
    if (fd < 0 || fd >= MAX_FDS || !fd_table[fd].node) return -1;
    vfs_close_node(fd_table[fd].node);
    fd_table[fd].node = NULL;
    fd_table[fd].ref_count = 0;
    return 0;
}

size_t vfs_get_size(int fd)
{
    /* 检查文件描述符有效性 */
    if (fd < 0 || fd >= MAX_FDS || !fd_table[fd].node)
        return 0;

    /* 直接返回节点的 size 字段 */
    return fd_table[fd].node->size;
}