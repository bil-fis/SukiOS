/*
 * include/kernel/vfs.h
 * -----------------------------------------------------------------------------
 * SukiOS 虚拟文件系统（VFS）层——内核侧挂载路由 + 内核内建文件系统后端。
 *
 * 设计依据（OSDev "Filesystem" / "Virtual File System" 条目要点）：
 *   - VFS 不解析任何具体文件系统格式，只负责【路径解析(namei) + 挂载点路由 +
 *     vnode/inode 语义抽象】；具体格式由后端实现。
 *   - 多后端经【挂载树(mount tree)】组织：根 '/' 挂一个后端，其它挂载点挂其它
 *     后端。新文件系统以 mount(前缀, 后端) 注册即可被 VFS 路由。
 *   - OSDev 强烈建议以 TMPFS/RAMFS 作为 "starter filesystem" 避免 VFS 接口被
 *     某一具体 FS 绑架；本项目以 tmpfs 作为内核内建内存 FS 后端，devfs 作为设备
 *     文件系统后端。
 *
 * SukiOS 混合内核适配：
 *   - DISK 后端 = FS_PORT 的 Ring3 FS_SERVER(FatFs)。VFS 把剥离挂载前缀后的
 *     相对路径原样转发给 FS_SERVER（保持已工作的 FAT32 全链路 100% 兼容）。
 *   - TMPFS / DEVFS 后端 = 内核态实现（不经 IPC、零拷贝、无端口占用），用于
 *     /tmp、/run、/dev 等。这是真正可工作的内核 FS，证明 VFS 多后端路由有效。
 *
 * 路由算法：vfs_resolve(path) 在所有已挂载前缀里取【最长前缀匹配】，
 * 返回 {backend, rel_path（剥离前缀后的相对路径）}。
 *   /foo/bar  -> 匹配 '/'  -> DISK, rel='/foo/bar'
 *   /tmp/x    -> 匹配 '/tmp' -> TMPFS, rel='x'
 *   /dev/null -> 匹配 '/dev' -> DEVFS, rel='null'
 */
#ifndef _SUKI_KERNEL_VFS_H
#define _SUKI_KERNEL_VFS_H

#include <stdint.h>
#include <stdbool.h>
#include <ipc/fs_proto.h>   /* FS_PATH_MAX / fs_stat_t / fs_dirent_t / FS_* 错误码 */

/* VFS 后端类型 */
typedef enum vfs_backend {
    VFS_BACKEND_NONE = 0,   /* 未挂载 / 路径无匹配 */
    VFS_BACKEND_DISK = 1,   /* FS_PORT 的 FatFs 服务（Ring3 FS_SERVER） */
    VFS_BACKEND_TMPFS = 2,  /* 内核态内存文件系统（/tmp、/run） */
    VFS_BACKEND_DEVFS = 3,  /* 内核态字符设备文件系统（/dev） */
} vfs_backend_t;

#define VFS_MAX_MOUNTS   16
#define VFS_MOUNT_PREFIX_MAX  FS_PATH_MAX

/* 单个挂载项 */
typedef struct vfs_mount {
    bool         in_use;
    char         prefix[VFS_MOUNT_PREFIX_MAX];  /* 必须以 '/' 开头，NUL 结尾 */
    uint32_t     prefix_len;                    /* strlen(prefix) */
    vfs_backend_t backend;
    void        *fs_ctx;                        /* 后端私有上下文（tmpfs 根目录等） */
} vfs_mount_t;

/* vfs_resolve 结果 */
typedef struct vfs_resolved {
    vfs_backend_t backend;
    const vfs_mount_t *mount;   /* 命中的挂载项（可能为 NULL=NONE） */
    const char   *rel;          /* 相对路径（指向入参 path 内部，剥离前缀后） */
} vfs_resolved_t;

/* ===================== VFS 核心 API ===================== */

/* 注册一个挂载点：prefix 必须以 '/' 开头且为规范绝对路径前缀。
 * 返回 0 成功，<0 失败（表满 / 前缀非法）。 */
int vfs_mount(const char *prefix, vfs_backend_t backend, void *fs_ctx);

/* 解析路径：最长前缀匹配。
 * 若命中，result->backend != NONE，result->rel 指向剥离前缀后的相对路径
 *（根挂载 '/' 命中时 rel 为原路径本身，含前导 '/'）。
 * 注意：path 必须是以 '/' 开头的绝对路径。 */
void vfs_resolve(const char *path, vfs_resolved_t *result);

/* 初始化默认挂载表：'/' -> DISK(FS_PORT)，'/tmp'、'/run' -> TMPFS，'/dev' -> DEVFS。
 * 应在 boot-late 中 FS_SERVER 就位后调用。 */
void vfs_init(void);

/* 是否已初始化（供 fd.c 判断走 VFS 内部路由还是旧的纯 FS_PORT 透传）。 */
bool vfs_ready(void);

/* 启动自测：验证多后端路由 + 内建 tmpfs/devfs 真正可工作（由 boot-late 调用）。 */
void vfs_selftest(void);

/* ===================== VFS 统一文件操作入口（供 fd.c 调用） =====================
 * 这些函数对调用方透明：它们内部按 vfs_resolve 结果分派到对应后端。
 * 所有路径参数必须是绝对路径（以 '/' 开头）。返回值沿用 POSIX 语义：
 *   成功：>=0（open 返回 fd 或句柄、stat 填充 stat、readdir 返回 1/0 等）；
 *   失败：负 errno（SUKI_E*，见 kernel/errno.h）。
 * 对于 DISK 后端，这些函数把相对路径透传给 FS_PORT（保持 FatFs 兼容）。
 *
 * 注：read/write/seek 等"经已打开句柄"的操作不经过 VFS（句柄已绑定后端），
 *     仍由 fd.c 直发 FS_PORT；仅"按路径"的操作走 VFS 路由。 */

/* open：flags/mode 用 sukios/posix.h 的 SUKI_O_ 与 SUKI_S_ 语义。
 * 成功返回 >=0 的 FS 句柄（DISK 后端为 FS_SERVER 句柄；内建后端为 VFS 内部句柄）。 */
int vfs_open(const char *path, int32_t flags, uint32_t mode,
             char *rel_out, uint32_t rel_out_cap);

/* stat：成功返回 0 并填充 *st */
int vfs_stat(const char *path, fs_stat_t *st);

/* mkdir：成功返回 0 */
int vfs_mkdir(const char *path, uint32_t mode);

/* unlink：is_dir=1 走 rmdir 语义。成功返回 0 */
int vfs_unlink(const char *path, bool is_dir);

/* rename：成功返回 0 */
int vfs_rename(const char *oldp, const char *newp);

/* access：成功返回 0 */
int vfs_access(const char *path, int32_t mode);

/* chmod：成功返回 0 */
int vfs_chmod(const char *path, uint32_t mode);

/* utime：成功返回 0 */
int vfs_utime(const char *path, int64_t atime, int64_t mtime);

/* opendir：成功返回 >=0 目录句柄 */
int vfs_opendir(const char *path);

/* readdir：成功返回 1（有条目，填充 *de）或 0（目录结束） */
int vfs_readdir(int dd, fs_dirent_t *de);

/* closedir：成功返回 0 */
int vfs_closedir(int dd);

/* 内核态同步读整个文件（仅 DISK 后端，供启动期读 registry 等小文件）。
 * 成功返回 0，*out_n 为读取字节；失败返回负 errno。 */
int kern_fs_read_file(const char *path, uint8_t *buf, uint32_t cap, uint32_t *out_n);

/* ===================== 内建后端统一入口（供 fd.c 调用） =====================
 * 这些函数假定入参路径已是相对挂载点的路径（fd.c 经 vfs_resolve 剥离前缀后传入）。
 * DISK 后端不在此处理——由 fd.c 直发 FS_PORT（保持 FatFs 兼容）。 */
int vfs_builtin_open(const char *rel, int32_t flags, uint32_t mode,
                     uint32_t *out_handle, vfs_backend_t backend);
int vfs_builtin_stat(const char *rel, fs_stat_t *st, vfs_backend_t backend);
int vfs_builtin_mkdir(const char *rel, uint32_t mode, vfs_backend_t backend);
int vfs_builtin_unlink(const char *rel, bool is_dir, vfs_backend_t backend);
int vfs_builtin_rename(const char *old_rel, const char *new_rel,
                       vfs_backend_t backend);
int vfs_builtin_access(const char *rel, int32_t mode, vfs_backend_t backend);
int vfs_builtin_chmod(const char *rel, uint32_t mode, vfs_backend_t backend);
int vfs_builtin_utime(const char *rel, int64_t atime, int64_t mtime,
                      vfs_backend_t backend);
int vfs_builtin_opendir(const char *rel, vfs_backend_t backend);
int vfs_builtin_readdir(int dd, fs_dirent_t *de, vfs_backend_t backend);
int vfs_builtin_closedir(int dd, vfs_backend_t backend);

/* ===================== 内核内建后端初始化 ===================== */
int tmpfs_init(void);   /* 初始化 tmpfs 超级块/根目录，返回 0 成功 */
int devfs_init(void);   /* 初始化 devfs 根目录并注册标准字符设备 */

/* --- tmpfs 内部操作（供 VFS 核心与 fd.c 调用，不经 IPC） --- */
int tmpfs_open(const char *rel, int32_t flags, uint32_t mode, uint32_t *out_handle);
int tmpfs_read(int h, void *buf, uint32_t len, uint64_t *out_nread);
int tmpfs_write(int h, const void *buf, uint32_t len, uint64_t offset_override,
                uint64_t *out_nwritten);
int tmpfs_lseek(int h, int64_t offset, int whence, uint64_t *out_pos);
int tmpfs_close(int h);
int tmpfs_fstat(int h, fs_stat_t *st);
int tmpfs_stat(const char *rel, fs_stat_t *st);
int tmpfs_mkdir(const char *rel, uint32_t mode);
int tmpfs_unlink(const char *rel, bool is_dir);
int tmpfs_rename(const char *old_rel, const char *new_rel);
int tmpfs_access(const char *rel, int32_t mode);
int tmpfs_chmod(const char *rel, uint32_t mode);
int tmpfs_utime(const char *rel, int64_t atime, int64_t mtime);
int tmpfs_opendir(const char *rel);
int tmpfs_readdir(int dd, fs_dirent_t *de);
int tmpfs_closedir(int dd);

/* --- devfs 内部操作（供 VFS 核心与 fd.c 调用，不经 IPC） --- */
int devfs_open(const char *rel, int32_t flags, uint32_t mode, uint32_t *out_handle);
int devfs_read(uint32_t h, void *buf, uint32_t len, uint64_t offset, uint64_t *out_nread);
int devfs_write(uint32_t h, const void *buf, uint32_t len, uint64_t offset,
                uint64_t *out_nwritten);
int devfs_stat(const char *rel, fs_stat_t *st);
int devfs_access(const char *rel, int32_t mode);
int devfs_opendir(const char *rel);
int devfs_readdir(int dd, fs_dirent_t *de);
int devfs_closedir(int dd);

/* devfs 设备写回调类型：把 buf[len] 写入设备；返回写入字节数或负 errno。
 * read 回调同理（从设备读 len 字节到 buf）。 */
typedef int (*devfs_io_fn)(void *dev_ctx, char *buf, uint32_t len, uint64_t offset);

/* 向 devfs 注册一个字符设备节点。
 * name 为相对 /dev 的路径（如 "null"、"sda"、"tty0"）。
 * 返回 0 成功。read/write 任一可为 NULL（只读/只写）。 */
int devfs_register(const char *name, devfs_io_fn read_fn, devfs_io_fn write_fn,
                   void *ctx, uint32_t mode, uint64_t size);

#endif /* _SUKI_KERNEL_VFS_H */
