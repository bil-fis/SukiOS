/*
 * kernel/fs/devfs.c
 * -----------------------------------------------------------------------------
 * 内核态设备文件系统（devfs）—— SukiOS VFS 的内建 DEVFS 后端，挂载于 /dev。
 *
 * 设计依据（OSDev "Device File" / 类 Unix devfs 惯例）：
 *   - 设备以文件形式暴露（字符设备），open/read/write/ioctl 经 VFS 路由到内核
 *     驱动回调；不占 FS_PORT，不经 IPC，零拷贝、无端口占用。
 *   - 预建标准字符设备：/dev/null、/dev/zero、/dev/urandom、/dev/console。
 *     用户态程序可用统一 POSIX 接口访问设备（符合"一切皆文件"）。
 *
 * 节点模型：固定大小节点数组（DEVFS_NODES 上限），注册时静态分配，运行期只读
 * 结构（名称/回调不可变），故单核/多核均无需加锁。read/write 回调内可能自旋等
 * 硬件，但绝不持 fd 锁——符合 fd.c「锁内不阻塞」不变量（devfs 操作由 fd.c 在
 * 解锁后调用）。
 *
 * 红线：本模块只服务 VFS 内建后端，绝不经 IPC。
 */
#include <kernel/vfs.h>
#include <kernel/string.h>
#include <kernel/console.h>
#include <mm/kmalloc.h>
#include <ipc/fs_proto.h>
#include <sukios/posix.h>

#define DEVFS_NODES  64

typedef struct devfs_node {
    bool          used;
    char          name[FS_PATH_MAX];
    devfs_io_fn   read_fn;
    devfs_io_fn   write_fn;
    void         *ctx;
    uint32_t      mode;
    uint64_t      size;        /* 对字符设备通常 0（无限流）或固定 */
} devfs_node_t;

static devfs_node_t g_devfs_nodes[DEVFS_NODES];
static int g_devfs_dir_cursor = 0;   /* readdir 迭代游标（opendir 重置） */

/* ===================== 内置设备回调 ===================== */

/* /dev/null：读返回 0（EOF），写丢弃 */
static int dev_null_write(void *ctx, char *buf, uint32_t len, uint64_t off)
{
    (void)ctx; (void)buf; (void)off;
    return (int)len;   /* 全部"成功丢弃" */
}
static int dev_null_read(void *ctx, char *buf, uint32_t len, uint64_t off)
{
    (void)ctx; (void)buf; (void)off;
    return 0;   /* EOF */
}

/* /dev/zero：读返回全 0，写丢弃 */
static int dev_zero_read(void *ctx, char *buf, uint32_t len, uint64_t off)
{
    (void)ctx; (void)off;
    memset(buf, 0, len);
    return (int)len;
}
static int dev_zero_write(void *ctx, char *buf, uint32_t len, uint64_t off)
{
    (void)ctx; (void)buf; (void)off;
    return (int)len;
}

/* /dev/urandom：读返回伪随机字节（基于 TSC 的简易 PRNG，足够非密码学用途） */
static uint64_t g_devfs_rng_state = 0x9e3779b97f4a7c15ULL;
static int dev_urandom_read(void *ctx, char *buf, uint32_t len, uint64_t off)
{
    (void)ctx; (void)off;
    uint64_t s = g_devfs_rng_state ^ (uint64_t)(uintptr_t)buf;
    for (uint32_t i = 0; i < len; i++) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        buf[i] = (char)(s >> 33);
    }
    g_devfs_rng_state = s;
    return (int)len;
}
static int dev_urandom_write(void *ctx, char *buf, uint32_t len, uint64_t off)
{
    (void)ctx; (void)buf; (void)off;
    return (int)len;   /* 丢弃种子写入 */
}

/* /dev/console：写 -> 内核控制台输出；读 -> 当前未缓冲，返回 0（EOF） */
static int dev_console_write(void *ctx, char *buf, uint32_t len, uint64_t off)
{
    (void)ctx; (void)off;
    /* 逐字符经内核控制台输出（kputc 已实现，内核态可用） */
    extern void kputc(char c);
    for (uint32_t i = 0; i < len; i++) {
        kputc(buf[i]);
    }
    return (int)len;
}
static int dev_console_read(void *ctx, char *buf, uint32_t len, uint64_t off)
{
    (void)ctx; (void)buf; (void)off;
    return 0;   /* 控制台输入走键盘服务，这里不回显 */
}

/* ===================== 节点管理 ===================== */

static devfs_node_t *devfs_find(const char *rel)
{
    /* rel 可以是相对 /dev 的路径（如 "null"、"console"）或完整路径
     * （如 "/dev/null"、"dev/console"）；统一剥到节点名。 */
    const char *name = rel;
    if (strncmp(name, "/dev/", 5) == 0) {
        name += 5;                       /* 跳过 "/dev/" */
    } else {
        while (*name == '/') {
            name++;                      /* 跳过前导 '/' */
        }
    }
    /* 扁平化：遇到后续 '/' 截断（本项目 devfs 无子目录） */
    char buf[FS_PATH_MAX];
    uint32_t i = 0;
    while (name[i] && name[i] != '/' && i < FS_PATH_MAX - 1) {
        buf[i] = name[i];
        i++;
    }
    buf[i] = '\0';
    for (int j = 0; j < DEVFS_NODES; j++) {
        if (g_devfs_nodes[j].used && strcmp(g_devfs_nodes[j].name, buf) == 0) {
            return &g_devfs_nodes[j];
        }
    }
    return NULL;
}

int devfs_register(const char *name, devfs_io_fn read_fn, devfs_io_fn write_fn,
                   void *ctx, uint32_t mode, uint64_t size)
{
    if (!name || !name[0]) {
        return -SUKI_EINVAL;
    }
    /* 拒绝重复注册 */
    if (devfs_find(name)) {
        return -SUKI_EEXIST;
    }
    for (int i = 0; i < DEVFS_NODES; i++) {
        if (!g_devfs_nodes[i].used) {
            devfs_node_t *n = &g_devfs_nodes[i];
            memset(n, 0, sizeof(*n));
            strncpy(n->name, name, FS_PATH_MAX - 1);
            n->name[FS_PATH_MAX - 1] = '\0';
            n->read_fn = read_fn;
            n->write_fn = write_fn;
            n->ctx = ctx;
            n->mode = mode;
            n->size = size;
            n->used = true;
            return 0;
        }
    }
    return -SUKI_ENOMEM;
}

int devfs_init(void)
{
    memset(g_devfs_nodes, 0, sizeof(g_devfs_nodes));
    int rc = 0;
    rc |= devfs_register("null",    dev_null_read,    dev_null_write,    NULL, 0666, 0);
    rc |= devfs_register("zero",    dev_zero_read,    dev_zero_write,    NULL, 0666, 0);
    rc |= devfs_register("urandom", dev_urandom_read, dev_urandom_write, NULL, 0444, 0);
    rc |= devfs_register("console", dev_console_read, dev_console_write, NULL, 0600, 0);
    if (rc != 0) {
        kprintf("[devfs] WARNING: some standard devices failed to register (rc=%d)\n", rc);
        return -1;
    }
    kprintf("[devfs] initialized (/dev/null /dev/zero /dev/urandom /dev/console)\n");
    return 0;
}

/* ===================== VFS 接口 ===================== */

int devfs_open(const char *rel, int32_t flags, uint32_t mode, uint32_t *out_handle)
{
    (void)flags; (void)mode;
    devfs_node_t *n = devfs_find(rel);
    if (!n) {
        return -SUKI_ENOENT;
    }
    /* 句柄 = 节点在数组中的下标 + 1（非 0），由 devfs_handle_node() 反算。
     * 注意必须用"下标"而非"字节偏移"：节点体含变长内容对齐，用字节偏移反算
     * 会得到远超 DEVFS_NODES 的越界下标，导致 EBADF（selftest 曾因此失败）。 */
    uintptr_t idx = (uintptr_t)(n - g_devfs_nodes);
    *out_handle = (uint32_t)(idx + 1);
    return 0;
}

static devfs_node_t *devfs_handle_node(uint32_t h)
{
    uintptr_t idx = (uintptr_t)h - 1;
    if (idx >= DEVFS_NODES) {
        return NULL;
    }
    if (!g_devfs_nodes[idx].used) {
        return NULL;
    }
    return &g_devfs_nodes[idx];
}

int devfs_read(uint32_t h, void *buf, uint32_t len, uint64_t offset, uint64_t *out_nread)
{
    devfs_node_t *n = devfs_handle_node(h);
    if (!n || !n->read_fn) {
        return -SUKI_EBADF;
    }
    int r = n->read_fn(n->ctx, buf, len, offset);
    if (r < 0) {
        return r;
    }
    *out_nread = (uint64_t)r;
    return 0;
}

int devfs_write(uint32_t h, const void *buf, uint32_t len, uint64_t offset,
                uint64_t *out_nwritten)
{
    devfs_node_t *n = devfs_handle_node(h);
    if (!n || !n->write_fn) {
        return -SUKI_EBADF;
    }
    int r = n->write_fn(n->ctx, (char *)buf, len, offset);
    if (r < 0) {
        return r;
    }
    *out_nwritten = (uint64_t)r;
    return 0;
}

int devfs_stat(const char *rel, fs_stat_t *st)
{
    devfs_node_t *n = devfs_find(rel);
    if (!n) {
        return -SUKI_ENOENT;
    }
    memset(st, 0, sizeof(*st));
    st->dev = 0x4456;   /* 'DV' devfs 设备号 */
    st->ino = (uint64_t)(n - g_devfs_nodes) + 1;
    st->mode = SUKI_S_IFCHR | n->mode;
    st->nlink = 1;
    st->size = (int64_t)n->size;
    st->blksize = 4096;
    return 0;
}

int devfs_access(const char *rel, int32_t mode)
{
    (void)mode;
    return devfs_find(rel) ? 0 : -SUKI_ENOENT;
}

int devfs_opendir(const char *rel)
{
    /* /dev 根目录可迭代；子路径无目录概念 */
    const char *p = rel;
    while (*p == '/') {
        p++;
    }
    if (*p != '\0') {
        return -SUKI_ENOENT;
    }
    g_devfs_dir_cursor = 0;   /* opendir 重置迭代游标 */
    return 1;   /* 单一目录句柄（值恒定） */
}

int devfs_readdir(int dd, fs_dirent_t *de)
{
    (void)dd;
    /* 全局迭代游标（devfs 只读、单核安全；多核下 opendir 串行重置可接受）。 */
    while (g_devfs_dir_cursor < DEVFS_NODES) {
        devfs_node_t *n = &g_devfs_nodes[g_devfs_dir_cursor++];
        if (n->used) {
            memset(de, 0, sizeof(*de));
            de->ino = (uint64_t)(n - g_devfs_nodes) + 1;
            de->type = SUKI_DT_CHR;
            strncpy(de->name, n->name, sizeof(de->name) - 1);
            return 1;
        }
    }
    return 0;   /* 目录结束 */
}

int devfs_closedir(int dd)
{
    (void)dd;
    return 0;
}
