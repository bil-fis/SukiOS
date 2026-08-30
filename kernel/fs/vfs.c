/*
 * kernel/fs/vfs.c
 * -----------------------------------------------------------------------------
 * SukiOS 虚拟文件系统（VFS）核心：挂载表 + 路径解析(namei) + 后端分派。
 *
 * 设计依据见 include/kernel/vfs.h 顶部注释（OSDev VFS / Filesystem 条目）。
 * 本文件职责：
 *   1) 维护挂载表 vfs_mount_t g_mounts[]（内核全局，单核下无并发问题；
 *      多核下由 g_vfs_lock 保护，但 mount 只在启动期发生，后续只读，安全）。
 *   2) vfs_resolve()：对绝对路径做【最长前缀匹配】，返回命中后端与相对路径。
 *   3) vfs_init()：注册默认挂载点 '/'->DISK(FS_PORT)、'/tmp'&'/run'->TMPFS、
 *      '/dev'->DEVFS，并初始化 tmpfs/devfs 超级块。
 *   4) vfs_builtin_*()：内建后端（TMPFS/DEVFS）的统一操作入口，供 fd.c 在
 *      vfs_resolve 命中内建后端时调用。DISK 后端不在此处理——由 fd.c 直发
 *      FS_PORT（保持 FatFs 全链路兼容）。
 *
 * 红线：本文件不直接解析 FAT32（那是 Ring3 FS_SERVER 的职责）。内核 VFS 只做
 *       路由 + 内核态内存/设备 FS 这一小部分内建后端。
 */
#include <kernel/vfs.h>
#include <kernel/string.h>
#include <kernel/console.h>
#include <kernel/spinlock.h>
#include <kernel/percpu.h>
#include <mm/kmalloc.h>
#include <sukios/posix.h>      /* SUKI_E* 错误码 */
#include <ipc/fs_proto.h>

/* ========================================================================== */
/*  挂载表                                                                     */
/* ========================================================================== */

static vfs_mount_t g_mounts[VFS_MAX_MOUNTS];
static bool        g_vfs_initialized = false;
static spinlock_t  g_vfs_lock;        /* 仅保护 mount 表的写（启动期），读路径可无锁 */

/* 规范化前缀：必须以 '/' 开头，去除尾部多余的 '/'（除根 '/' 本身）。 */
static uint32_t vfs_norm_prefix(const char *in, char *out, uint32_t cap)
{
    if (in[0] != '/') {
        return 0;   /* 非法：非绝对路径 */
    }
    uint32_t i = 0, o = 0;
    /* 拷贝但压缩连续 '/' */
    bool prev_slash = false;
    for (i = 0; in[i] && o + 1 < cap; i++) {
        if (in[i] == '/') {
            if (prev_slash) {
                continue;   /* 跳过重复 '/' */
            }
            prev_slash = true;
        } else {
            prev_slash = false;
        }
        out[o++] = in[i];
    }
    /* 去掉尾部 '/'（根 '/' 保留） */
    if (o > 1 && out[o - 1] == '/') {
        o--;
    }
    out[o] = '\0';
    return o;
}

int vfs_mount(const char *prefix, vfs_backend_t backend, void *fs_ctx)
{
    char norm[VFS_MOUNT_PREFIX_MAX];
    uint32_t len = vfs_norm_prefix(prefix, norm, sizeof(norm));
    if (len == 0 || len >= VFS_MOUNT_PREFIX_MAX) {
        return -SUKI_EINVAL;     /* 前缀非法 */
    }
    spin_lock(&g_vfs_lock);
    int slot = -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spin_unlock(&g_vfs_lock);
        return -SUKI_ENOMEM;     /* 挂载表满 */
    }
    vfs_mount_t *m = &g_mounts[slot];
    memcpy(m->prefix, norm, len + 1);
    m->prefix_len = len;
    m->backend = backend;
    m->fs_ctx = fs_ctx;
    m->in_use = true;
    spin_unlock(&g_vfs_lock);
    return 0;
}

void vfs_resolve(const char *path, vfs_resolved_t *result)
{
    result->backend = VFS_BACKEND_NONE;
    result->mount = NULL;
    result->rel = path;

    if (path == NULL || path[0] != '/') {
        return;   /* 非绝对路径：无匹配 */
    }

    /* 最长前缀匹配：遍历挂载表，找 prefix_len 最大且 path 以 prefix 开头的项。
     * 匹配判定：path == prefix（精确），或 path[prefix_len] == '/'（前缀后跟分隔）。
     * 根 '/' 命中时 rel 指向原 path（含前导 '/'）。 */
    uint32_t best_len = 0;
    const vfs_mount_t *best = NULL;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        const vfs_mount_t *m = &g_mounts[i];
        if (!m->in_use) {
            continue;
        }
        uint32_t pl = m->prefix_len;
        bool match;
        if (pl == 1) {
            /* 根 '/'：所有绝对路径都算命中（rel = 原路径） */
            match = true;
        } else {
            match = (strncmp(path, m->prefix, pl) == 0) &&
                    (path[pl] == '/' || path[pl] == '\0');
        }
        if (match && pl >= best_len) {
            best_len = pl;
            best = m;
        }
    }
    if (best) {
        result->backend = best->backend;
        result->mount = best;
        if (best->prefix_len == 1) {
            result->rel = path;            /* 根挂载：相对路径 = 完整路径 */
        } else {
            /* 剥离前缀：跳过 prefix + 后面的 '/' */
            const char *r = path + best->prefix_len;
            if (*r == '/') {
                r++;
            }
            result->rel = r;               /* 可能为空串（访问挂载点本身） */
        }
    }
}

bool vfs_ready(void)
{
    return g_vfs_initialized;
}

void vfs_init(void)
{
    g_vfs_initialized = false;

    /* 1) 初始化内核内建后端超级块 */
    if (tmpfs_init() != 0) {
        kprintf("[vfs] WARNING: tmpfs_init failed\n");
    }
    if (devfs_init() != 0) {
        kprintf("[vfs] WARNING: devfs_init failed\n");
    }

    /* 2) 注册默认挂载点 */
    if (vfs_mount("/", VFS_BACKEND_DISK, NULL) != 0) {
        kprintf("[vfs] FATAL: cannot mount '/' as DISK\n");
    }
    if (vfs_mount("/tmp", VFS_BACKEND_TMPFS, NULL) != 0) {
        kprintf("[vfs] WARNING: mount /tmp tmpfs failed\n");
    }
    if (vfs_mount("/run", VFS_BACKEND_TMPFS, NULL) != 0) {
        kprintf("[vfs] WARNING: mount /run tmpfs failed\n");
    }
    if (vfs_mount("/dev", VFS_BACKEND_DEVFS, NULL) != 0) {
        kprintf("[vfs] WARNING: mount /dev devfs failed\n");
    }

    g_vfs_initialized = true;
    kprintf("[vfs] initialized: /->DISK(FS_PORT) /tmp,/run->TMPFS /dev->DEVFS\n");
}

/* ========================================================================== */
/*  内建后端统一入口（供 fd.c 调用）                                            */
/*  这些函数假定 path 已是相对挂载点的路径（由 fd.c 经 vfs_resolve 剥离前缀后   */
/*  传入）。对根挂载触发的 DISK 后端，fd.c 不走这里。                           */
/* ========================================================================== */

int vfs_builtin_open(const char *rel, int32_t flags, uint32_t mode,
                     uint32_t *out_handle, vfs_backend_t backend)
{
    if (backend == VFS_BACKEND_TMPFS) {
        return tmpfs_open(rel, flags, mode, out_handle);
    }
    if (backend == VFS_BACKEND_DEVFS) {
        return devfs_open(rel, flags, mode, out_handle);
    }
    return -SUKI_EINVAL;
}

int vfs_builtin_stat(const char *rel, fs_stat_t *st, vfs_backend_t backend)
{
    if (backend == VFS_BACKEND_TMPFS) {
        return tmpfs_stat(rel, st);
    }
    if (backend == VFS_BACKEND_DEVFS) {
        return devfs_stat(rel, st);
    }
    return -SUKI_EINVAL;
}

int vfs_builtin_mkdir(const char *rel, uint32_t mode, vfs_backend_t backend)
{
    if (backend == VFS_BACKEND_TMPFS) {
        return tmpfs_mkdir(rel, mode);
    }
    /* devfs 不支持创建目录 */
    return -SUKI_EPERM;
}

int vfs_builtin_unlink(const char *rel, bool is_dir, vfs_backend_t backend)
{
    if (backend == VFS_BACKEND_TMPFS) {
        return tmpfs_unlink(rel, is_dir);
    }
    /* devfs 节点只读，不可删除 */
    return -SUKI_EPERM;
}

int vfs_builtin_rename(const char *old_rel, const char *new_rel,
                       vfs_backend_t backend)
{
    if (backend == VFS_BACKEND_TMPFS) {
        return tmpfs_rename(old_rel, new_rel);
    }
    return -SUKI_EPERM;
}

int vfs_builtin_access(const char *rel, int32_t mode, vfs_backend_t backend)
{
    if (backend == VFS_BACKEND_TMPFS) {
        return tmpfs_access(rel, mode);
    }
    if (backend == VFS_BACKEND_DEVFS) {
        return devfs_access(rel, mode);
    }
    return -SUKI_EINVAL;
}

int vfs_builtin_chmod(const char *rel, uint32_t mode, vfs_backend_t backend)
{
    if (backend == VFS_BACKEND_TMPFS) {
        return tmpfs_chmod(rel, mode);
    }
    return -SUKI_EPERM;   /* devfs 权限固定 */
}

int vfs_builtin_utime(const char *rel, int64_t atime, int64_t mtime,
                      vfs_backend_t backend)
{
    if (backend == VFS_BACKEND_TMPFS) {
        return tmpfs_utime(rel, atime, mtime);
    }
    return -SUKI_EPERM;
}

int vfs_builtin_opendir(const char *rel, vfs_backend_t backend)
{
    if (backend == VFS_BACKEND_TMPFS) {
        return tmpfs_opendir(rel);
    }
    if (backend == VFS_BACKEND_DEVFS) {
        return devfs_opendir(rel);
    }
    return -SUKI_EINVAL;
}

int vfs_builtin_readdir(int dd, fs_dirent_t *de, vfs_backend_t backend)
{
    if (backend == VFS_BACKEND_TMPFS) {
        return tmpfs_readdir(dd, de);
    }
    if (backend == VFS_BACKEND_DEVFS) {
        return devfs_readdir(dd, de);
    }
    return -SUKI_EINVAL;
}

/* ========================================================================== */
/*  VFS 启动自测：验证多后端路由 + 内建 FS 真正可工作                          */
/*  在 vfs_init() 之后由 boot-late 调用。纯内核态、不经 IPC，失败仅打印不挂死。*/
/* ========================================================================== */
void vfs_selftest(void)
{
    kprintf("[vfs] selftest begin\n");

    /* 1) tmpfs 写 + 读回环 */
    {
        uint32_t h = 0;
        int rc = tmpfs_open("selftest.txt", SUKI_O_CREAT | SUKI_O_RDWR, 0644, &h);
        if (rc < 0) {
            kprintf("[vfs] selftest FAIL: tmpfs open (rc=%d)\n", rc);
        } else {
            const char *msg = "SukiOS tmpfs works";
            uint64_t nw = 0;
            rc = tmpfs_write(h, msg, (uint32_t)strlen(msg), (uint64_t)-1, &nw);
            char rbuf[64];
            uint64_t nr = 0;
            tmpfs_lseek(h, 0, SUKI_SEEK_SET, &(uint64_t){0});
            rc = tmpfs_read(h, rbuf, (uint32_t)strlen(msg), &nr);
            rbuf[nr] = '\0';
            if (nr == strlen(msg) && memcmp(rbuf, msg, nr) == 0) {
                kprintf("[vfs] selftest PASS: tmpfs write/read ('%s')\n", rbuf);
            } else {
                kprintf("[vfs] selftest FAIL: tmpfs rw mismatch (got %llu bytes)\n",
                        (unsigned long long)nr);
            }
            tmpfs_close(h);
            tmpfs_unlink("selftest.txt", false);
        }
    }

    /* 2) devfs /dev/zero 读零 */
    {
        uint32_t h = 0;
        int rc = devfs_open("zero", SUKI_O_RDONLY, 0, &h);
        if (rc < 0) {
            kprintf("[vfs] selftest FAIL: /dev/zero open (rc=%d)\n", rc);
        } else {
            char z[16];
            uint64_t nr = 0;
            rc = devfs_read(h, z, sizeof(z), 0, &nr);
            bool allzero = (nr == sizeof(z));
            for (uint32_t i = 0; i < nr; i++) {
                if (z[i] != 0) {
                    allzero = false;
                    break;
                }
            }
            kprintf("[vfs] selftest %s: /dev/zero read rc=%d %llu zero bytes\n",
                    (rc == 0 && allzero) ? "PASS" : "FAIL", rc,
                    (unsigned long long)nr);
        }
    }

    /* 3) devfs /dev/null 写丢弃 */
    {
        uint32_t h = 0;
        int rc = devfs_open("null", SUKI_O_WRONLY, 0, &h);
        if (rc < 0) {
            kprintf("[vfs] selftest FAIL: /dev/null open (rc=%d)\n", rc);
        } else {
            uint64_t nw = 0;
            rc = devfs_write(h, "discardme", 9, 0, &nw);
            kprintf("[vfs] selftest %s: /dev/null write %llu bytes\n",
                    (rc == 0 && nw == 9) ? "PASS" : "FAIL",
                    (unsigned long long)nw);
        }
    }

    /* 4) devfs /dev 目录列出 */
    {
        int dd = devfs_opendir("");
        if (dd < 0) {
            kprintf("[vfs] selftest FAIL: /dev opendir (rc=%d)\n", dd);
        } else {
            kprintf("[vfs] selftest: /dev entries: ");
            fs_dirent_t de;
            int n = 0;
            while (devfs_readdir(dd, &de) == 1) {
                kprintf("%s ", de.name);
                n++;
            }
            kprintf("(%d total)\n", n);
            devfs_closedir(dd);
        }
    }

    kprintf("[vfs] selftest end\n");
}

int vfs_builtin_closedir(int dd, vfs_backend_t backend)
{
    if (backend == VFS_BACKEND_TMPFS) {
        return tmpfs_closedir(dd);
    }
    if (backend == VFS_BACKEND_DEVFS) {
        return devfs_closedir(dd);
    }
    return -SUKI_EINVAL;
}
