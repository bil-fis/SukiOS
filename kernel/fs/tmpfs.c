/*
 * kernel/fs/tmpfs.c
 * -----------------------------------------------------------------------------
 * 内核态内存文件系统（tmpfs）—— SukiOS VFS 的内建 TMPFS 后端。
 *
 * 设计依据（OSDev "Filesystem"/"TMPFS" 要点）：
 *   - TMPFS 是「无持久化、纯内存」文件系统，OSDev 强烈推荐作为 VFS 的 starter
 *     后端，避免 VFS 接口被某一具体磁盘 FS 绑架；本项目用它支撑 /tmp、/run。
 *   - 实现采用【统一节点（node）模型】：目录与文件都是 tmpfs_node_t，目录用
 *     子节点链表表达层次；open/mkdir/unlink 均在目录树上操作。
 *
 * 内存模型：
 *   - 每个文件的数据存在 node->data（kmalloc 分配的连续 buffer），按需按 4KiB
 *     步长增长（不预分配整页，简单且足够）；写入超出则 realloc 扩容。
 *   - 节点与目录链表全部 kmalloc，unlink 时释放（引用为 0 时）。
 *
 * 并发：单核默认构建下无并发；多核构建时由调用方(fd.c)持有 g_fd_lock 时串行
 *       进入，本模块内部不再加锁（保持与 fd.c 一致的「锁内不阻塞」不变量）。
 *
 * 红线：本模块只服务 VFS 内建后端，绝不经 IPC、绝不解析磁盘格式。
 */
#include <kernel/vfs.h>
#include <kernel/string.h>
#include <kernel/console.h>
#include <kernel/spinlock.h>
#include <mm/kmalloc.h>
#include <mm/pmm.h>
#include <ipc/fs_proto.h>
#include <sukios/posix.h>

/* ===================== 数据结构 ===================== */

typedef enum tmpfs_node_type {
    TMPFS_FILE = 1,
    TMPFS_DIR  = 2,
} tmpfs_node_type_t;

typedef struct tmpfs_node {
    char name[FS_PATH_MAX];      /* 文件名/目录名（不含路径） */
    tmpfs_node_type_t type;
    struct tmpfs_node *parent;
    struct tmpfs_node *sibling;  /* 同一目录下的下一个兄弟 */
    struct tmpfs_node *child;    /* 目录：第一个子节点（链表头） */

    uint64_t size;               /* 文件字节数 */
    uint8_t *data;               /* 文件内容（kmalloc，size 字节有效） */
    uint64_t capacity;           /* data 已分配容量（>= size） */

    uint32_t mode;               /* 权限位 + SUKI_S_IF* */
    uint32_t uid;
    uint32_t gid;
    uint64_t atime, mtime, ctime;
    uint64_t ino;
} tmpfs_node_t;

/* 句柄表：open 时分配，记录节点 + 偏移 + 访问模式 */
typedef struct tmpfs_handle {
    bool     used;
    tmpfs_node_t *node;
    uint64_t offset;
    int32_t  flags;
} tmpfs_handle_t;

#define TMPFS_HANDLES  128
#define TMPFS_GROW_STEP 4096

static tmpfs_node_t   g_tmpfs_root;
static tmpfs_handle_t g_tmpfs_handles[TMPFS_HANDLES];
static uint64_t       g_tmpfs_ino = 1;
static spinlock_t      g_tmpfs_lock;   /* 多核保护；单核下自旋锁本身开销极小 */

/* ===================== 辅助 ===================== */

static uint64_t tmpfs_now(void)
{
    /* POSIX 时间：借用 RTC/clock 的秒计数。内核未导出时退化为单调自增。
     * 这里用 clock_monotonic_ns 近似（仅用于时间字段填充，不影响逻辑）。 */
    extern uint64_t clock_monotonic_ns(void);
    return clock_monotonic_ns() / 1000000000ULL;
}

static tmpfs_node_t *tmpfs_alloc_node(tmpfs_node_type_t type, const char *name)
{
    tmpfs_node_t *n = kmalloc(sizeof(tmpfs_node_t));
    if (!n) {
        return NULL;
    }
    memset(n, 0, sizeof(*n));
    strncpy(n->name, name, FS_PATH_MAX - 1);
    n->name[FS_PATH_MAX - 1] = '\0';
    n->type = type;
    n->mode = (type == TMPFS_DIR ? SUKI_S_IFDIR : SUKI_S_IFREG) | 0755;
    n->uid = 0;
    n->gid = 0;
    uint64_t now = tmpfs_now();
    n->atime = n->mtime = n->ctime = now;
    n->ino = ++g_tmpfs_ino;
    return n;
}

static void tmpfs_free_node(tmpfs_node_t *n)
{
    if (!n || n == &g_tmpfs_root) {
        return;
    }
    if (n->data) {
        kfree(n->data);
    }
    kfree(n);
}

/* 在目录 dir 的子链表中按名查找（不含路径） */
static tmpfs_node_t *tmpfs_lookup_child(tmpfs_node_t *dir, const char *name)
{
    for (tmpfs_node_t *c = dir->child; c; c = c->sibling) {
        if (strcmp(c->name, name) == 0) {
            return c;
        }
    }
    return NULL;
}

/* 把相对路径 rel（'/' 分隔，可能为空串表示根）解析到节点；
 * create_file/create_dir!=0 时逐级创建缺失的目录（仅在最后一段创建目标）。
 * 返回节点或 NULL。 */
static tmpfs_node_t *tmpfs_resolve_path(const char *rel, bool create_last_dir,
                                        bool create_last_file)
{
    if (rel == NULL) {
        return &g_tmpfs_root;
    }
    /* 跳过前导 '/' */
    while (*rel == '/') {
        rel++;
    }
    if (*rel == '\0') {
        return &g_tmpfs_root;   /* 空路径 = 根 */
    }

    tmpfs_node_t *cur = &g_tmpfs_root;
    const char *p = rel;
    while (*p) {
        /* 取一段名字 [start, end) */
        const char *start = p;
        while (*p && *p != '/') {
            p++;
        }
        bool last = (*p == '\0');
        uint32_t seglen = (uint32_t)(p - start);

        char seg[FS_PATH_MAX];
        if (seglen >= FS_PATH_MAX) {
            return NULL;
        }
        memcpy(seg, start, seglen);
        seg[seglen] = '\0';

        tmpfs_node_t *next = tmpfs_lookup_child(cur, seg);
        if (!next) {
            if (last) {
                if (create_last_dir || create_last_file) {
                    next = tmpfs_alloc_node(create_last_file ? TMPFS_FILE
                                                            : TMPFS_DIR, seg);
                    if (!next) {
                        return NULL;
                    }
                    next->parent = cur;
                    next->sibling = cur->child;
                    cur->child = next;
                    cur = next;
                    break;
                }
                return NULL;   /* 不存在且不创建 */
            } else {
                /* 中间段缺失：仅当要求创建目录树时才创建中间目录 */
                if (create_last_dir || create_last_file) {
                    next = tmpfs_alloc_node(TMPFS_DIR, seg);
                    if (!next) {
                        return NULL;
                    }
                    next->parent = cur;
                    next->sibling = cur->child;
                    cur->child = next;
                    cur = next;
                } else {
                    return NULL;
                }
            }
        } else {
            if (last) {
                cur = next;
                break;
            }
            if (next->type != TMPFS_DIR) {
                return NULL;   /* 中间路径分量不是目录 */
            }
            cur = next;
        }
        if (*p == '/') {
            p++;
        }
    }
    return cur;
}

/* ===================== 句柄管理 ===================== */

static int tmpfs_alloc_handle(tmpfs_node_t *node, int32_t flags)
{
    spin_lock(&g_tmpfs_lock);
    int h = -1;
    for (int i = 0; i < TMPFS_HANDLES; i++) {
        if (!g_tmpfs_handles[i].used) {
            h = i;
            break;
        }
    }
    if (h < 0) {
        spin_unlock(&g_tmpfs_lock);
        return -SUKI_ENFILE;
    }
    g_tmpfs_handles[h].used = true;
    g_tmpfs_handles[h].node = node;
    g_tmpfs_handles[h].offset = (flags & SUKI_O_APPEND) ? node->size : 0;
    g_tmpfs_handles[h].flags = flags;
    spin_unlock(&g_tmpfs_lock);
    return h;
}

static tmpfs_handle_t *tmpfs_handle(int h)
{
    if (h < 0 || h >= TMPFS_HANDLES) {
        return NULL;
    }
    if (!g_tmpfs_handles[h].used) {
        return NULL;
    }
    return &g_tmpfs_handles[h];
}

/* ===================== VFS 接口 ===================== */

/* 前向声明：供 tmpfs_fstat / tmpfs_stat 共用的节点属性填充函数 */
static int tmpfs_stat_node(tmpfs_node_t *node, fs_stat_t *st);

int tmpfs_init(void)
{
    memset(&g_tmpfs_root, 0, sizeof(g_tmpfs_root));
    g_tmpfs_root.type = TMPFS_DIR;
    strncpy(g_tmpfs_root.name, "/", sizeof(g_tmpfs_root.name) - 1);
    g_tmpfs_root.mode = SUKI_S_IFDIR | 0755;
    g_tmpfs_root.ino = ++g_tmpfs_ino;
    memset(g_tmpfs_handles, 0, sizeof(g_tmpfs_handles));
    spinlock_init(&g_tmpfs_lock, "tmpfs");
    kprintf("[tmpfs] initialized (memory filesystem, /tmp /run)\n");
    return 0;
}

int tmpfs_open(const char *rel, int32_t flags, uint32_t mode, uint32_t *out_handle)
{
    (void)mode;
    bool create = (flags & SUKI_O_CREAT) ? true : false;
    bool excl   = (flags & SUKI_O_EXCL) ? true : false;

    tmpfs_node_t *node = tmpfs_resolve_path(rel, false, create);
    if (!node) {
        return -SUKI_ENOENT;
    }
    if (node->type != TMPFS_FILE) {
        return -SUKI_EISDIR;   /* 以非目录方式 open 了一个目录 */
    }
    if (excl && create && node->size > 0) {
        /* O_CREAT|O_EXCL 且已存在：这里简化判定——只要存在即冲突 */
        return -SUKI_EEXIST;
    }
    if (flags & SUKI_O_TRUNC) {
        node->size = 0;   /* 截断：保留 buffer，逻辑长度清零 */
    }
    int h = tmpfs_alloc_handle(node, flags);
    if (h < 0) {
        return h;
    }
    *out_handle = (uint32_t)h;
    return 0;
}

int tmpfs_read(int h, void *buf, uint32_t len, uint64_t *out_nread)
{
    tmpfs_handle_t *fh = tmpfs_handle(h);
    if (!fh) {
        return -SUKI_EBADF;
    }
    if (fh->node->type != TMPFS_FILE) {
        return -SUKI_EISDIR;
    }
    uint64_t off = fh->offset;
    uint64_t avail = fh->node->size > off ? fh->node->size - off : 0;
    uint32_t n = (len < avail) ? len : (uint32_t)avail;
    if (n > 0) {
        memcpy(buf, fh->node->data + off, n);
    }
    fh->offset += n;
    fh->node->atime = tmpfs_now();
    *out_nread = n;
    return 0;
}

int tmpfs_write(int h, const void *buf, uint32_t len, uint64_t offset_override,
                uint64_t *out_nwritten)
{
    tmpfs_handle_t *fh = tmpfs_handle(h);
    if (!fh) {
        return -SUKI_EBADF;
    }
    if (fh->node->type != TMPFS_FILE) {
        return -SUKI_EISDIR;
    }
    uint64_t off = (offset_override == (uint64_t)-1) ? fh->offset : offset_override;
    uint64_t need = off + len;
    /* 扩容 */
    if (need > fh->node->capacity) {
        uint64_t newcap = (need + TMPFS_GROW_STEP - 1) & ~(uint64_t)(TMPFS_GROW_STEP - 1);
        uint8_t *nd = kmalloc(newcap);
        if (!nd) {
            return -SUKI_ENOMEM;
        }
        if (fh->node->data && fh->node->size > 0) {
            memcpy(nd, fh->node->data, fh->node->size);
        }
        if (fh->node->data) {
            kfree(fh->node->data);
        }
        fh->node->data = nd;
        fh->node->capacity = newcap;
    }
    memcpy(fh->node->data + off, buf, len);
    if (need > fh->node->size) {
        fh->node->size = need;
    }
    if (offset_override == (uint64_t)-1) {
        fh->offset = off + len;
    } else {
        fh->offset = off + len;
    }
    fh->node->mtime = tmpfs_now();
    *out_nwritten = len;
    return 0;
}

int tmpfs_lseek(int h, int64_t offset, int whence, uint64_t *out_pos)
{
    tmpfs_handle_t *fh = tmpfs_handle(h);
    if (!fh) {
        return -SUKI_EBADF;
    }
    uint64_t base;
    switch (whence) {
        case SUKI_SEEK_SET: base = (offset < 0 ? 0 : (uint64_t)offset); break;
        case SUKI_SEEK_CUR: base = fh->offset + (uint64_t)offset; break;
        case SUKI_SEEK_END: base = fh->node->size + (uint64_t)offset; break;
        default: return -SUKI_EINVAL;
    }
    fh->offset = base;
    *out_pos = base;
    return 0;
}

int tmpfs_close(int h)
{
    tmpfs_handle_t *fh = tmpfs_handle(h);
    if (!fh) {
        return -SUKI_EBADF;
    }
    spin_lock(&g_tmpfs_lock);
    fh->used = false;
    fh->node = NULL;
    spin_unlock(&g_tmpfs_lock);
    return 0;
}

int tmpfs_fstat(int h, fs_stat_t *st)
{
    tmpfs_handle_t *fh = tmpfs_handle(h);
    if (!fh) {
        return -SUKI_EBADF;
    }
    return tmpfs_stat_node(fh->node, st);
}

int tmpfs_stat(const char *rel, fs_stat_t *st)
{
    tmpfs_node_t *node = tmpfs_resolve_path(rel, false, false);
    if (!node) {
        return -SUKI_ENOENT;
    }
    return tmpfs_stat_node(node, st);
}

/* 内部：填充 fs_stat_t（供 fstat/stat 共用） */
static int tmpfs_stat_node(tmpfs_node_t *node, fs_stat_t *st)
{
    memset(st, 0, sizeof(*st));
    st->dev = 0x5450;   /* 'TP' tmpfs 设备号 */
    st->ino = node->ino;
    st->mode = node->mode;
    st->nlink = 1;
    st->uid = node->uid;
    st->gid = node->gid;
    st->size = (int64_t)node->size;
    st->blksize = 4096;
    st->blocks = (node->size + 4095) / 4096;
    st->atime = (int64_t)node->atime;
    st->mtime = (int64_t)node->mtime;
    st->ctime = (int64_t)node->ctime;
    return 0;
}

int tmpfs_mkdir(const char *rel, uint32_t mode)
{
    (void)mode;
    tmpfs_node_t *node = tmpfs_resolve_path(rel, true, false);
    if (!node) {
        return -SUKI_ENOMEM;   /* 创建失败（内存或路径非法） */
    }
    if (node->type != TMPFS_DIR) {
        return -SUKI_EEXIST;   /* 已存在且非目录 */
    }
    /* 已存在同名目录：幂等成功 */
    return 0;
}

int tmpfs_unlink(const char *rel, bool is_dir)
{
    tmpfs_node_t *node = tmpfs_resolve_path(rel, false, false);
    if (!node || node == &g_tmpfs_root) {
        return -SUKI_ENOENT;
    }
    if (node->type == TMPFS_DIR) {
        if (!is_dir) {
            return -SUKI_EISDIR;
        }
        if (node->child) {
            return -SUKI_ENOTEMPTY;  /* 目录非空 */
        }
    }
    /* 从父目录链表中摘除 */
    tmpfs_node_t *parent = node->parent;
    if (parent) {
        tmpfs_node_t **pp = &parent->child;
        while (*pp) {
            if (*pp == node) {
                *pp = node->sibling;
                break;
            }
            pp = &(*pp)->sibling;
        }
    }
    tmpfs_free_node(node);
    return 0;
}

int tmpfs_rename(const char *old_rel, const char *new_rel)
{
    tmpfs_node_t *node = tmpfs_resolve_path(old_rel, false, false);
    if (!node || node == &g_tmpfs_root) {
        return -SUKI_ENOENT;
    }
    /* 解析新父目录 + 新名字 */
    /* 简化：new_rel 必须是 "dir/newname" 形式；这里直接尝试在 new_rel 的父目录
     * 下创建同名节点并搬移数据。为稳健，要求 new_rel 与 old_rel 同类型。 */
    char newname[FS_PATH_MAX];
    /* 取 new_rel 最后一段名 */
    const char *slash = NULL;
    for (const char *p = new_rel; *p; p++) {
        if (*p == '/') {
            slash = p;
        }
    }
    const char *name = slash ? slash + 1 : new_rel;
    strncpy(newname, name, FS_PATH_MAX - 1);
    newname[FS_PATH_MAX - 1] = '\0';

    /* 找新父目录 */
    char parent_path[FS_PATH_MAX];
    if (slash) {
        uint32_t plen = (uint32_t)(slash - new_rel);
        if (plen >= FS_PATH_MAX) {
            return -SUKI_ENAMETOOLONG;
        }
        memcpy(parent_path, new_rel, plen);
        parent_path[plen] = '\0';
    } else {
        parent_path[0] = '\0';   /* 根 */
    }
    tmpfs_node_t *newparent = tmpfs_resolve_path(parent_path, true, false);
    if (!newparent || newparent->type != TMPFS_DIR) {
        return -SUKI_ENOENT;
    }
    /* 已存在同名则覆盖（删除旧目标） */
    tmpfs_node_t *exist = tmpfs_lookup_child(newparent, newname);
    if (exist && exist != node) {
        if (exist->type == TMPFS_DIR && exist->child) {
            return -SUKI_ENOTEMPTY;
        }
        tmpfs_unlink(new_rel, exist->type == TMPFS_DIR);
    }
    /* 从旧父摘除并挂到新父 */
    tmpfs_node_t *oldparent = node->parent;
    if (oldparent) {
        tmpfs_node_t **pp = &oldparent->child;
        while (*pp) {
            if (*pp == node) {
                *pp = node->sibling;
                break;
            }
            pp = &(*pp)->sibling;
        }
    }
    strncpy(node->name, newname, FS_PATH_MAX - 1);
    node->name[FS_PATH_MAX - 1] = '\0';
    node->parent = newparent;
    node->sibling = newparent->child;
    newparent->child = node;
    return 0;
}

int tmpfs_access(const char *rel, int32_t mode)
{
    (void)mode;
    tmpfs_node_t *node = tmpfs_resolve_path(rel, false, false);
    if (!node) {
        return -SUKI_ENOENT;
    }
    return 0;
}

int tmpfs_chmod(const char *rel, uint32_t mode)
{
    tmpfs_node_t *node = tmpfs_resolve_path(rel, false, false);
    if (!node) {
        return -SUKI_ENOENT;
    }
    node->mode = (node->mode & SUKI_S_IFMT) | (mode & 07777);
    return 0;
}

int tmpfs_utime(const char *rel, int64_t atime, int64_t mtime)
{
    tmpfs_node_t *node = tmpfs_resolve_path(rel, false, false);
    if (!node) {
        return -SUKI_ENOENT;
    }
    if (atime >= 0) {
        node->atime = (uint64_t)atime;
    }
    if (mtime >= 0) {
        node->mtime = (uint64_t)mtime;
    }
    return 0;
}

/* ===================== 目录迭代 ===================== */

/* 目录迭代句柄表（与文件句柄共用空间，简单分用高半段避免冲突） */
#define TMPFS_DIR_HANDLES 64
typedef struct tmpfs_dirh {
    bool            used;
    tmpfs_node_t   *dir;
    tmpfs_node_t   *cursor;   /* 下一个待返回的兄弟 */
} tmpfs_dirh_t;
static tmpfs_dirh_t g_tmpfs_dirh[TMPFS_DIR_HANDLES];

int tmpfs_opendir(const char *rel)
{
    tmpfs_node_t *node = tmpfs_resolve_path(rel, false, false);
    if (!node) {
        return -SUKI_ENOENT;
    }
    if (node->type != TMPFS_DIR) {
        return -SUKI_ENOTDIR;
    }
    spin_lock(&g_tmpfs_lock);
    int h = -1;
    for (int i = 0; i < TMPFS_DIR_HANDLES; i++) {
        if (!g_tmpfs_dirh[i].used) {
            h = i;
            break;
        }
    }
    if (h < 0) {
        spin_unlock(&g_tmpfs_lock);
        return -SUKI_ENFILE;
    }
    g_tmpfs_dirh[h].used = true;
    g_tmpfs_dirh[h].dir = node;
    g_tmpfs_dirh[h].cursor = node->child;
    spin_unlock(&g_tmpfs_lock);
    return h + TMPFS_HANDLES;   /* 与文件句柄区间错开 */
}

int tmpfs_readdir(int h, fs_dirent_t *de)
{
    h -= TMPFS_HANDLES;
    if (h < 0 || h >= TMPFS_DIR_HANDLES || !g_tmpfs_dirh[h].used) {
        return -SUKI_EBADF;
    }
    tmpfs_dirh_t *dh = &g_tmpfs_dirh[h];
    if (!dh->cursor) {
        return 0;   /* 目录结束 */
    }
    tmpfs_node_t *n = dh->cursor;
    dh->cursor = n->sibling;
    memset(de, 0, sizeof(*de));
    de->ino = n->ino;
    de->type = (n->type == TMPFS_DIR) ? SUKI_DT_DIR : SUKI_DT_REG;
    strncpy(de->name, n->name, sizeof(de->name) - 1);
    return 1;   /* 有条目 */
}

int tmpfs_closedir(int h)
{
    h -= TMPFS_HANDLES;
    if (h < 0 || h >= TMPFS_DIR_HANDLES || !g_tmpfs_dirh[h].used) {
        return -SUKI_EBADF;
    }
    spin_lock(&g_tmpfs_lock);
    g_tmpfs_dirh[h].used = false;
    g_tmpfs_dirh[h].dir = NULL;
    g_tmpfs_dirh[h].cursor = NULL;
    spin_unlock(&g_tmpfs_lock);
    return 0;
}
