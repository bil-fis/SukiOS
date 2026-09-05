/*
 * kernel/fs/fd.c
 * -----------------------------------------------------------------------------
 * 内核文件描述符（VFS）层：每进程 fd 表 + 到 Ring3 FS_SERVER 的 IPC 转发。
 *
 * 定位：本层不解析任何文件系统格式——FAT32 完全由用户态 FS_SERVER（FatFs）
 * 负责，符合 SukiOS「驱动与文件系统用户态化」的混合内核红线。内核侧只做：
 *   1) 每进程的 fd 表（类型、标志、后端句柄、路径副本）与全局槽池；
 *   2) 用户指针安全校验（一律经 copy_*_user，绝不直接解引用）；
 *   3) 把 POSIX 语义翻译成 FS_PORT 的 IPC 消息并同步等待应答；
 *   4) 特殊 fd 的本地实现：TTY（控制台 0/1/2）、PIPE（内核环形缓冲）。
 *
 * 三条不可违背的生产级不变量（见 include/kernel/fd.h 顶部）：
 *   A. 【锁内绝不阻塞】g_fd_lock 是自旋锁。任何 IPC 往返（会 schedule 让出）
 *      必须在解锁后执行。故槽的「摘除」与「释放」被拆成两阶段：
 *      fd_unbind_locked()（持锁、只摘链）/ fd_release_slot()（解锁后、可阻塞）。
 *   B. 【per-CPU 暂存缓冲】一次 RPC 等待应答时会让出 CPU；若暂存缓冲是全局
 *      静态的，同核另一任务的 fd_write 会覆写它。故所有大缓冲均为
 *      [MAX_CPUS] 数组，按 cpu_index() 取用。
 *   C. 【绝不信任外部长度】FS_SERVER 应答里的 length/value 一律先与真实收到
 *      的字节数比对，越界立即判 -EIO，不拷贝、不信任。
 *
 * 调用关系：kernel/syscall/sys_posix.c 的 sys_open/read/write/... -> 本文件。
 */
#include <kernel/fd.h>
#include <kernel/string.h>
#include <kernel/console.h>
#include <kernel/spinlock.h>
#include <kernel/task.h>       /* task_yield()：管道空/满时让出 CPU */
#include <kernel/percpu.h>
#include <kernel/syscall.h>    /* copy_from_user / copy_to_user（红线：用户指针
                                * 一律经这两个函数过墙，绝不直接解引用） */
#include <mm/kmalloc.h>
#include <ipc/port.h>
#include <ipc/fs_proto.h>
#include <kernel/vfs.h>       /* VFS 路由：DISK/tmpfs/devfs 多后端分派 */

/* ========================================================================== */
/*  全局 fd 槽池                                                               */
/* ========================================================================== */

static fd_entry_t g_fd_slots[FD_SLOT_MAX];
static spinlock_t g_fd_lock;

/* RPC 暂存缓冲（per-CPU，见不变量 B）。
 * 容量必须 >= 最大请求（写：头 + fs_write_fd_req_t + FS_WRITE_MAX）
 * 且 >= 最大应答（读：头 + fs_resp_t + fs_ret_t + FS_READ_MAX）。 */
#define FS_RPC_MAX   (sizeof(mach_msg_header_t) + sizeof(fs_write_fd_req_t) \
                      + FS_WRITE_MAX + 32)
static uint8_t g_rpc_req[MAX_CPUS][FS_RPC_MAX];
static uint8_t g_rpc_resp[MAX_CPUS][FS_RPC_MAX];

/* 本核的请求/应答缓冲 */
static inline uint8_t *rpc_req_buf(void)  { return g_rpc_req[cpu_index()]; }
static inline uint8_t *rpc_resp_buf(void) { return g_rpc_resp[cpu_index()]; }

/* TTY 写暂存（per-CPU） */
static uint8_t g_tty_buf[MAX_CPUS][512];

/* ========================================================================== */
/*  IPC 往返原语（严格闭环，绝不泄漏端口）                                     */
/* ========================================================================== */

/*
 * 向 FS_PORT 发一条请求并同步等待应答。
 * 返回 0 成功（resp 缓冲内为 fs_resp_t + 可选 fs_ret_t + 可选数据，
 * *resp_out = 实际收到的总字节数）；<0 为负 errno。
 * 注意：本函数会阻塞（等待应答时让出 CPU），严禁在持 g_fd_lock 时调用。
 */
static int fs_rpc(const void *req, uint32_t req_size, uint32_t *resp_out)
{
    task_t *self = sched_current();
    uint32_t rp = port_allocate(self);
    if (!rp) {
        return -SUKI_ENFILE;      /* 端口表耗尽（64 槽） */
    }

    /* 回填应答端口（请求头由调用方构造，此处统一修正路由字段） */
    mach_msg_header_t *h = (mach_msg_header_t *)req;
    h->msgh_local_port = rp;
    h->msgh_remote_port = FS_PORT;

    if (ipc_send_kernel(FS_PORT, req, req_size) != MACH_MSG_SUCCESS) {
        port_free(rp);
        return -SUKI_EIO;         /* FS 服务未挂载 / 队列满（背压） */
    }

    uint8_t *resp = rpc_resp_buf();
    uint32_t got = 0;
    if (ipc_recv_kernel(rp, resp, (uint32_t)FS_RPC_MAX, &got, true)
            != MACH_MSG_SUCCESS) {
        port_free(rp);
        return -SUKI_EIO;
    }
    port_free(rp);

    /* 服务下线通知：FS_SERVER 已死，内核排干队列后回送的占位应答 */
    mach_msg_header_t *rh = (mach_msg_header_t *)resp;
    if (rh->msgh_id == MSG_ID_SERVICE_DOWN) {
        return -SUKI_EIO;
    }
    if (got < (uint32_t)(sizeof(mach_msg_header_t) + sizeof(fs_resp_t))) {
        return -SUKI_EIO;         /* 畸形应答 */
    }
    if (resp_out) {
        *resp_out = got;
    }
    return 0;
}

/* 把 FS 协议状态码翻译为 POSIX errno */
static int fs_status_to_errno(uint32_t st)
{
    switch (st) {
    case FS_OK:          return 0;
    case FS_ERR_NOENT:   return -SUKI_ENOENT;
    case FS_ERR_EXIST:   return -SUKI_EEXIST;
    case FS_ERR_ACCES:   return -SUKI_EACCES;
    case FS_ERR_NOTDIR:  return -SUKI_ENOTDIR;
    case FS_ERR_ISDIR:   return -SUKI_EISDIR;
    case FS_ERR_NOSPC:   return -SUKI_ENOSPC;
    case FS_ERR_INVAL:   return -SUKI_EINVAL;
    case FS_ERR_BADF:    return -SUKI_EBADF;
    case FS_ERR_NFILE:   return -SUKI_EMFILE;
    case FS_ERR_TOOBIG:  return -SUKI_EFBIG;
    case FS_ERR_IO:
    default:             return -SUKI_EIO;
    }
}

/* 取应答中的 fs_resp_t / 紧跟其后的 fs_ret_t（调用方已确保长度足够） */
static inline fs_resp_t *resp_hdr(const void *resp)
{
    return (fs_resp_t *)((const uint8_t *)resp + sizeof(mach_msg_header_t));
}
static inline fs_ret_t *resp_ret(const void *resp)
{
    return (fs_ret_t *)((const uint8_t *)resp + sizeof(mach_msg_header_t)
                        + sizeof(fs_resp_t));
}

/* 应答固定开销（头 + fs_resp_t + fs_ret_t） */
#define RESP_RET_SIZE  (uint32_t)(sizeof(mach_msg_header_t) \
                                  + sizeof(fs_resp_t) + sizeof(fs_ret_t))

/* ========================================================================== */
/*  槽池管理（摘除与释放两阶段，遵守不变量 A）                                 */
/* ========================================================================== */

/* 持锁：分配一个空槽，返回槽号或 -1 */
static int slot_alloc_locked(void)
{
    for (int i = 0; i < FD_SLOT_MAX; i++) {
        if (g_fd_slots[i].refcount == 0) {
            memset(&g_fd_slots[i], 0, sizeof(g_fd_slots[i]));
            g_fd_slots[i].refcount = 1;
            g_fd_slots[i].backend = -1;
            g_fd_slots[i].type = FD_TYPE_NONE;
            return i;
        }
    }
    return -1;
}

/* 持锁：递减引用；返回 true 表示引用已归零，调用方须在【解锁后】调用
 * fd_release_slot() 完成真正的资源释放（后者可能阻塞）。 */
static bool slot_deref_locked(int slot)
{
    if (slot < 0 || slot >= FD_SLOT_MAX) {
        return false;
    }
    if (g_fd_slots[slot].refcount == 0) {
        return false;
    }
    return (--g_fd_slots[slot].refcount == 0);
}

/*
 * 解锁态：释放一个引用已归零的槽（关闭后端、释放管道与路径）。
 * 本函数可能阻塞（IPC），严禁在持 g_fd_lock 时调用。
 */
void fd_release_slot(int slot)
{
    if (slot < 0 || slot >= FD_SLOT_MAX) {
        return;
    }
    fd_entry_t *e = &g_fd_slots[slot];
    if (e->refcount != 0) {
        return;
    }
    if (e->type == FD_TYPE_FILE || e->type == FD_TYPE_DIR) {
        fd_close_backend(e);
    } else if (e->type == FD_TYPE_PIPE) {
        if (e->pipe) {
            /* 管道对象独立引用计数：两端都关闭后才释放 */
            if (--e->pipe->refs == 0) {
                kfree(e->pipe);
            }
            e->pipe = NULL;
        }
    }
    if (e->path) {
        kfree(e->path);
    }
    memset(e, 0, sizeof(*e));
    e->backend = -1;
    e->type = FD_TYPE_NONE;
}

/* 关闭后端（向 FS_SERVER 发 CLOSE / CLOSEDIR）。会阻塞，严禁持锁调用。 */
void fd_close_backend(fd_entry_t *e)
{
    if (!e || e->backend < 0) {
        return;
    }
    /* 内建后端（tmpfs/devfs）：直接在本地关闭，不经 FS_PORT */
    if (e->vfs_backend == FD_BACKEND_TMPFS) {
        if (e->type == FD_TYPE_DIR) {
            vfs_builtin_closedir(e->backend, VFS_BACKEND_TMPFS);
        } else {
            tmpfs_close(e->backend);
        }
        e->backend = -1;
        return;
    }
    if (e->vfs_backend == FD_BACKEND_DEVFS) {
        /* devfs 设备无状态，无需关闭（句柄即表索引，下次 open 复用） */
        e->backend = -1;
        return;
    }
    /* DISK 后端：转发 FS_PORT */
    uint8_t *req = rpc_req_buf();
    memset(req, 0, sizeof(mach_msg_header_t) + sizeof(fs_fd_req_t));
    mach_msg_header_t *h = (mach_msg_header_t *)req;
    h->msgh_bits = 0;
    h->msgh_size = (uint32_t)(sizeof(mach_msg_header_t) + sizeof(fs_fd_req_t));
    h->msgh_id = (e->type == FD_TYPE_DIR) ? FS_MSG_CLOSEDIR : FS_MSG_CLOSE;
    fs_fd_req_t *fr = (fs_fd_req_t *)(req + sizeof(mach_msg_header_t));
    fr->fd = (uint32_t)e->backend;
    /* 失败可忽略：服务已死 / 端口耗尽时不该让退出路径失败 */
    (void)fs_rpc(req, h->msgh_size, NULL);
    e->backend = -1;
}

/* ========================================================================== */
/*  初始化与任务生命周期                                                       */
/* ========================================================================== */

void fd_init(void)
{
    spinlock_init(&g_fd_lock, "fd");
    memset(g_fd_slots, 0, sizeof(g_fd_slots));
    for (int i = 0; i < FD_SLOT_MAX; i++) {
        g_fd_slots[i].backend = -1;
        g_fd_slots[i].type = FD_TYPE_NONE;
    }
}

int fd_install_stdio(struct task *t)
{
    if (!t) {
        return -SUKI_EINVAL;
    }
    /* 根因修复（P0-R7）：task_t 由 kzalloc 清零，fds[] 初始为 0 而非 -1。
     * fd 层约定 -1 = 空位（见 task.h 注释），残留 0 会被 fd_bind_locked 误判
     * 为「已占用且指向 slot 0」，导致所有 open 找不到空位返回 -EMFILE（POSIX
     * 的 24）。故在此把整张 fd 表显式清零为 -1，确保空位判定正确。 */
    for (int i = 0; i < SUKI_FD_MAX; i++) {
        t->fds[i] = -1;
    }

    uint64_t f = spin_lock_irqsave(&g_fd_lock);
    int s0 = slot_alloc_locked();
    int s1 = slot_alloc_locked();
    int s2 = slot_alloc_locked();
    if (s0 < 0 || s1 < 0 || s2 < 0) {
        if (s0 >= 0) { g_fd_slots[s0].refcount = 0; g_fd_slots[s0].type = FD_TYPE_NONE; }
        if (s1 >= 0) { g_fd_slots[s1].refcount = 0; g_fd_slots[s1].type = FD_TYPE_NONE; }
        if (s2 >= 0) { g_fd_slots[s2].refcount = 0; g_fd_slots[s2].type = FD_TYPE_NONE; }
        spin_unlock_irqrestore(&g_fd_lock, f);
        return -SUKI_EMFILE;
    }
    g_fd_slots[s0].type = FD_TYPE_TTY;
    g_fd_slots[s0].flags = SUKI_O_RDONLY;
    g_fd_slots[s0].backend = -1;
    g_fd_slots[s1].type = FD_TYPE_TTY;
    g_fd_slots[s1].flags = SUKI_O_WRONLY;
    g_fd_slots[s1].backend = -1;
    g_fd_slots[s2].type = FD_TYPE_TTY;
    g_fd_slots[s2].flags = SUKI_O_WRONLY;
    g_fd_slots[s2].backend = -1;

    t->fds[0] = s0;
    t->fds[1] = s1;
    t->fds[2] = s2;
    spin_unlock_irqrestore(&g_fd_lock, f);
    return 0;
}

void fd_exit_task(struct task *t)
{
    if (!t) {
        return;
    }
    /* 阶段 1：持锁摘链并递减引用；收集需要真正释放的槽 */
    int pending[SUKI_FD_MAX];
    int npending = 0;
    uint64_t f = spin_lock_irqsave(&g_fd_lock);
    for (int i = 0; i < SUKI_FD_MAX; i++) {
        int s = t->fds[i];
        if (s >= 0 && s < FD_SLOT_MAX) {
            /* SukiNative FILE 对象独占的 fd：跳过（不清 fds、不降引用、不释放）。
             * 由持有它的 SukiNative FILE 对象在引用归零时统一 fd_close 释放，
             * 避免与对象销毁路径对同一 fd 双重关闭。 */
            if (g_fd_slots[s].suki_owned) {
                continue;
            }
            t->fds[i] = -1;
            if (slot_deref_locked(s) && npending < SUKI_FD_MAX) {
                pending[npending++] = s;
            }
        }
    }
    spin_unlock_irqrestore(&g_fd_lock, f);

    /* 阶段 2：解锁后释放（可能阻塞） */
    for (int i = 0; i < npending; i++) {
        fd_release_slot(pending[i]);
    }
}

int fd_fork_clone(struct task *child, const struct task *parent)
{
    if (!child || !parent) {
        return -SUKI_EINVAL;
    }
    uint64_t f = spin_lock_irqsave(&g_fd_lock);
    for (int i = 0; i < SUKI_FD_MAX; i++) {
        child->fds[i] = -1;
    }
    for (int i = 0; i < SUKI_FD_MAX; i++) {
        int s = parent->fds[i];
        if (s >= 0 && s < FD_SLOT_MAX && g_fd_slots[s].refcount > 0) {
            g_fd_slots[s].refcount++;
            child->fds[i] = s;
        }
    }
    /* 管道对象引用计数随槽引用一起增加（两端各自 +1） */
    spin_unlock_irqrestore(&g_fd_lock, f);
    return 0;
}

fd_entry_t *fd_get(struct task *t, int fd)
{
    if (!t || fd < 0 || fd >= SUKI_FD_MAX) {
        return NULL;
    }
    int s = t->fds[fd];
    if (s < 0 || s >= FD_SLOT_MAX) {
        return NULL;
    }
    if (g_fd_slots[s].refcount == 0 || g_fd_slots[s].type == FD_TYPE_NONE) {
        return NULL;
    }
    return &g_fd_slots[s];
}

/* 持锁：绑定新槽到最小可用 fd 号。返回 fd 号或负 errno。 */
static int fd_bind_locked(struct task *t, int type, int32_t flags)
{
    int slot = slot_alloc_locked();
    if (slot < 0) {
        return -SUKI_EMFILE;
    }
    int fdnum = -1;
    for (int i = 0; i < SUKI_FD_MAX; i++) {
        if (t->fds[i] < 0) {
            fdnum = i;
            break;
        }
    }
    if (fdnum < 0) {
        g_fd_slots[slot].refcount = 0;
        g_fd_slots[slot].type = FD_TYPE_NONE;
        return -SUKI_EMFILE;
    }
    g_fd_slots[slot].type = (uint32_t)type;
    g_fd_slots[slot].flags = flags;
    g_fd_slots[slot].backend = -1;
    g_fd_slots[slot].offset = 0;
    g_fd_slots[slot].path = NULL;
    g_fd_slots[slot].pipe = NULL;
    t->fds[fdnum] = slot;
    return fdnum;
}

/* 持锁：摘除 fd；返回 true 表示槽引用归零、调用方须在解锁后 fd_release_slot */
static bool fd_unbind_locked(struct task *t, int fd, int *slot_out)
{
    if (fd < 0 || fd >= SUKI_FD_MAX || t->fds[fd] < 0) {
        return false;
    }
    int s = t->fds[fd];
    t->fds[fd] = -1;
    bool zero = slot_deref_locked(s);
    if (slot_out) {
        *slot_out = s;
    }
    return zero;
}

/* ========================================================================== */
/*  TTY 后端（fd 0/1/2）                                                       */
/* ========================================================================== */

static int tty_write(const void *buf, size_t n)
{
    if (n == 0) {
        return 0;
    }
    char *tmp = (char *)g_tty_buf[cpu_index()];
    const char *p = (const char *)buf;
    size_t done = 0;
    while (done < n) {
        size_t chunk = n - done;
        if (chunk > 511) {
            chunk = 511;
        }
        memcpy(tmp, p + done, chunk);
        tmp[chunk] = '\0';
        user_puts(tmp);
        done += chunk;
    }
    return (int)n;
}

static int tty_read(void *buf, size_t n)
{
    /* 控制台输入由 INPUT_SERVER（Ring3）从键盘/串口采集后经 IPC 投递给消费者；
     * 内核 TTY 读路径在此仅返回 0（EOF 语义），保持接口闭环、不阻塞。
     * 交互式行编辑（回显/退格/历史）属用户态职责，不进内核。 */
    (void)buf;
    (void)n;
    return 0;
}

/* ========================================================================== */
/*  管道后端                                                                   */
/* ========================================================================== */

/*
 * 管道读：无数据且写端仍打开 -> 让出 CPU 后重试（最多 PIPE_SPIN_MAX 轮，
 * 每轮一次 schedule，避免永久自旋占满 CPU；超轮次返回 -EAGAIN）。
 * 写端已关闭（peer 槽失效）且缓冲空 -> 返回 0（EOF），符合 POSIX。
 */
#define PIPE_SPIN_MAX  200

static suki_ssize_t pipe_read(fd_entry_t *e, void *ubuf, size_t count)
{
    if (!e->pipe) {
        return -SUKI_EBADF;
    }
    fd_pipe_t *pp = e->pipe;
    int peer = e->backend;
    for (uint32_t spin = 0; spin < PIPE_SPIN_MAX; spin++) {
        if (pp->count > 0) {
            break;
        }
        /* 写端是否还活着：peer 槽存在且引用 > 0 */
        bool writer_alive = (peer >= 0 && peer < FD_SLOT_MAX
                             && g_fd_slots[peer].refcount > 0);
        if (!writer_alive) {
            return 0;               /* EOF */
        }
        task_yield();
    }
    if (pp->count == 0) {
        return (count == 0) ? 0 : -SUKI_EAGAIN;
    }

    size_t n = (count < pp->count) ? count : pp->count;
    if (n > FD_PIPE_BYTES) {
        n = FD_PIPE_BYTES;
    }
    /* 环形缓冲可能绕回：先拷到 per-CPU 暂存再一次性 copy_to_user。
     * （copy_to_user 可能触发按需补页，不能在其间持锁或分两次拷跨界。） */
    uint8_t *tmp = g_rpc_req[cpu_index()];   /* 复用 per-CPU 暂存（不与 RPC 并发） */
    for (size_t i = 0; i < n; i++) {
        tmp[i] = pp->data[(pp->tail + i) % FD_PIPE_BYTES];
    }
    if (copy_to_user(ubuf, tmp, n) != n) {
        return -SUKI_EFAULT;
    }
    pp->tail = (pp->tail + (uint32_t)n) % FD_PIPE_BYTES;
    pp->count -= (uint32_t)n;
    return (suki_ssize_t)n;
}

static suki_ssize_t pipe_write(fd_entry_t *e, const void *ubuf, size_t count)
{
    if (!e->pipe) {
        return -SUKI_EBADF;
    }
    fd_pipe_t *pp = e->pipe;
    int peer = e->backend;
    /* 读端已关闭 -> SIGPIPE 语义简化为 -EPIPE（本阶段无信号投递） */
    bool reader_alive = (peer >= 0 && peer < FD_SLOT_MAX
                         && g_fd_slots[peer].refcount > 0);
    if (!reader_alive) {
        return -SUKI_EPIPE;
    }

    uint8_t *tmp = g_rpc_req[cpu_index()];
    size_t total = 0;
    while (total < count) {
        if (pp->count >= FD_PIPE_BYTES) {
            /* 缓冲满：让出一次；仍满则返回已写（短写，POSIX 允许） */
            task_yield();
            if (pp->count >= FD_PIPE_BYTES) {
                break;
            }
        }
        size_t space = FD_PIPE_BYTES - pp->count;
        size_t chunk = count - total;
        if (chunk > space) {
            chunk = space;
        }
        if (chunk > FS_RPC_MAX) {
            chunk = FS_RPC_MAX;
        }
        if (copy_from_user(tmp, (const uint8_t *)ubuf + total, chunk)
                != chunk) {
            return total ? (suki_ssize_t)total : -SUKI_EFAULT;
        }
        for (size_t i = 0; i < chunk; i++) {
            pp->data[(pp->head + i) % FD_PIPE_BYTES] = tmp[i];
        }
        pp->head = (pp->head + (uint32_t)chunk) % FD_PIPE_BYTES;
        pp->count += (uint32_t)chunk;
        total += chunk;
    }
    return (suki_ssize_t)total;
}

/* ========================================================================== */
/*  POSIX 文件操作实现                                                         */
/* ========================================================================== */

/*
 * OPEN_DBG —— fd_open 失败路径的定点诊断，默认【关闭】。
 * 排查「FS 已 mount 但 open 仍失败」这类跨进程（内核 VFS -> Ring3 FS_SERVER）
 * 问题时打开：它会打印失败发生在哪一步（IPC 往返 / FS 状态码 / 应答长度 /
 * 后端句柄值），把「黑盒 -errno」定位到具体环节。
 */
#define FD_OPEN_DBG 0

#if FD_OPEN_DBG
static uint32_t g_open_dbg_n;
#define fd_open_dbg(stage, path, rc, got, v1, v2)                        \
    do {                                                                 \
        if (g_open_dbg_n < 6) {                                          \
            g_open_dbg_n++;                                              \
            kprintf("[fd:dbg] %s path=%s rc=%ld got=%lu v1=%lld v2=%lld\n", \
                    (stage), (path), (long)(rc), (unsigned long)(got),   \
                    (long long)(v1), (long long)(v2));                   \
        }                                                                \
    } while (0)
#else
#define fd_open_dbg(...)  ((void)0)
#endif

int fd_open(struct task *t, const char *path, int flags, uint32_t mode)
{
    if (!t || !path || path[0] == '\0') {
        return -SUKI_EINVAL;
    }
    size_t plen = strlen(path);
    if (plen >= FS_PATH_MAX) {
        return -SUKI_ENAMETOOLONG;
    }
    /* 访问模式必须明确（O_RDONLY=0 亦合法） */
    if ((flags & ~(SUKI_O_ACCMODE | SUKI_O_CREAT | SUKI_O_EXCL | SUKI_O_TRUNC
                   | SUKI_O_APPEND | SUKI_O_NONBLOCK | SUKI_O_DIRECTORY
                   | SUKI_O_CLOEXEC)) != 0) {
        return -SUKI_EINVAL;
    }

    /* ---- VFS 路由：命中内建后端（tmpfs/devfs）时直接走内核实现 ---- */
    if (vfs_ready()) {
        vfs_resolved_t vr;
        vfs_resolve(path, &vr);
        if (vr.backend == VFS_BACKEND_TMPFS || vr.backend == VFS_BACKEND_DEVFS) {
            uint32_t vh = 0;
            int brc = vfs_builtin_open(vr.rel, (int32_t)flags, mode, &vh,
                                       vr.backend);
            if (brc < 0) {
                return brc;   /* 负 errno */
            }
            int type = (flags & SUKI_O_DIRECTORY) ? FD_TYPE_DIR : FD_TYPE_FILE;
            uint64_t f = spin_lock_irqsave(&g_fd_lock);
            int fdnum = fd_bind_locked(t, type, (int32_t)flags);
            if (fdnum >= 0) {
                fd_entry_t *e = &g_fd_slots[t->fds[fdnum]];
                e->backend = (int)vh;                 /* VFS 内部句柄 */
                e->vfs_backend = (vr.backend == VFS_BACKEND_TMPFS)
                                 ? FD_BACKEND_TMPFS : FD_BACKEND_DEVFS;
                e->path = (char *)kmalloc(plen + 1);
                if (e->path) {
                    memcpy(e->path, path, plen + 1);
                }
            }
            spin_unlock_irqrestore(&g_fd_lock, f);
            if (fdnum < 0) {
                /* 本地槽耗尽：关闭内建句柄 */
                if (vr.backend == VFS_BACKEND_TMPFS) {
                    tmpfs_close((int)vh);
                }
                return fdnum;
            }
            return fdnum;
        }
        /* DISK 后端（含 '/' 根挂载）：落到下方原 FS_PORT 路径，路径用原 path */
    }

    uint8_t *req = rpc_req_buf();
    uint32_t reqsz = (uint32_t)(sizeof(mach_msg_header_t)
                                + sizeof(fs_open_req_t) + plen + 1);
    if (reqsz > FS_RPC_MAX) {
        return -SUKI_EINVAL;
    }
    memset(req, 0, reqsz);
    mach_msg_header_t *h = (mach_msg_header_t *)req;
    h->msgh_bits = 0;
    h->msgh_size = reqsz;
    h->msgh_id = FS_MSG_OPEN;
    h->msgh_reserved = 0;
    h->msgh_local_port = 0;
    h->msgh_remote_port = FS_PORT;
    fs_open_req_t *or = (fs_open_req_t *)(req + sizeof(mach_msg_header_t));
    or->flags = (int32_t)flags;
    or->mode = mode;
    memcpy(req + sizeof(mach_msg_header_t) + sizeof(fs_open_req_t),
           path, plen + 1);

    uint32_t got = 0;
    int rc = fs_rpc(req, reqsz, &got);
    if (rc < 0) {
        fd_open_dbg("rpc_fail", path, rc, got, 0, 0);
        return rc;
    }
    fs_resp_t *fr = resp_hdr(rpc_resp_buf());
    if (fr->status != FS_OK) {
        rc = fs_status_to_errno(fr->status);
        fd_open_dbg("fs_status", path, rc, got, (int64_t)fr->status, 0);
        return rc;
    }
    if (got < RESP_RET_SIZE) {
        fd_open_dbg("short_resp", path, -SUKI_EIO, got, 0, 0);
        return -SUKI_EIO;
    }
    int64_t backend = resp_ret(rpc_resp_buf())->value;
    if (backend < 0) {
        rc = fs_status_to_errno((uint32_t)(-backend));
        fd_open_dbg("backend_neg", path, rc, got, backend, 0);
        return rc;
    }

    int type = (flags & SUKI_O_DIRECTORY) ? FD_TYPE_DIR : FD_TYPE_FILE;
    uint64_t f = spin_lock_irqsave(&g_fd_lock);
    int fdnum = fd_bind_locked(t, type, (int32_t)flags);
    if (fdnum >= 0) {
        fd_entry_t *e = &g_fd_slots[t->fds[fdnum]];
        e->backend = (int)backend;
        e->vfs_backend = FD_BACKEND_DISK;   /* 走 FS_PORT 的 FatFs 服务 */
        e->path = (char *)kmalloc(plen + 1);
        if (e->path) {
            memcpy(e->path, path, plen + 1);
        }
    }
    spin_unlock_irqrestore(&g_fd_lock, f);

    if (fdnum < 0) {
        /* 本地槽耗尽：通知 FS_SERVER 关掉刚开的句柄，避免服务端泄漏 */
        fd_entry_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.type = (uint32_t)type;
        tmp.backend = (int)backend;
        fd_close_backend(&tmp);
        return fdnum;
    }
    return fdnum;
}

int fd_close(struct task *t, int fd)
{
    int slot = -1;
    uint64_t f = spin_lock_irqsave(&g_fd_lock);
    if (fd < 0 || fd >= SUKI_FD_MAX || t->fds[fd] < 0) {
        spin_unlock_irqrestore(&g_fd_lock, f);
        return -SUKI_EBADF;
    }
    bool zero = fd_unbind_locked(t, fd, &slot);
    spin_unlock_irqrestore(&g_fd_lock, f);

    if (zero) {
        fd_release_slot(slot);     /* 解锁后：可能阻塞 */
    }
    return 0;
}

suki_ssize_t fd_read(struct task *t, int fd, void *ubuf, size_t count)
{
    if (count == 0) {
        return 0;
    }
    fd_entry_t *e = fd_get(t, fd);
    if (!e) {
        return -SUKI_EBADF;
    }
    if ((e->flags & SUKI_O_ACCMODE) == SUKI_O_WRONLY) {
        return -SUKI_EBADF;
    }
    if (e->type == FD_TYPE_TTY) {
        return tty_read(ubuf, count);
    }
    if (e->type == FD_TYPE_PIPE) {
        return pipe_read(e, ubuf, count);
    }
    if (e->type == FD_TYPE_DIR) {
        return -SUKI_EISDIR;
    }
    if (e->vfs_backend == FD_BACKEND_TMPFS) {
        /* 内建 tmpfs 文件读：直接调 tmpfs_read（offset 用 fd 偏移） */
        uint8_t *kbuf = kmalloc(count ? count : 1);
        if (!kbuf) {
            return -SUKI_ENOMEM;
        }
        uint64_t nread = 0;
        int rc = tmpfs_read(e->backend, kbuf, (uint32_t)count, &nread);
        if (rc < 0) {
            kfree(kbuf);
            return rc;
        }
        /* 更新文件偏移（tmpfs_read 已推进 fh->offset，但 fd 层也需记录以便 lseek
         * 语义一致；这里以 tmpfs 内部偏移为准，fd->offset 仅对 DISK 用）。 */
        e->offset += nread;
        if (nread > 0) {
            rc = copy_to_user(ubuf, kbuf, (size_t)nread);
            if (rc < 0) {
                kfree(kbuf);
                return rc;
            }
        }
        kfree(kbuf);
        return (suki_ssize_t)nread;
    }
    if (e->vfs_backend == FD_BACKEND_DEVFS) {
        /* 内建 devfs 字符设备读：offset 对字符设备通常忽略（传 e->offset） */
        uint8_t *kbuf = kmalloc(count ? count : 1);
        if (!kbuf) {
            return -SUKI_ENOMEM;
        }
        uint64_t nread = 0;
        int rc = devfs_read((uint32_t)e->backend, kbuf, (uint32_t)count, e->offset, &nread);
        if (rc < 0) {
            kfree(kbuf);
            return rc;
        }
        e->offset += nread;
        if (nread > 0) {
            rc = copy_to_user(ubuf, kbuf, (size_t)nread);
            if (rc < 0) {
                kfree(kbuf);
                return rc;
            }
        }
        kfree(kbuf);
        return (suki_ssize_t)nread;
    }
    if (e->backend < 0) {
        return -SUKI_EBADF;
    }

    uint32_t want = (count > FS_READ_MAX) ? FS_READ_MAX : (uint32_t)count;
    uint8_t *req = rpc_req_buf();
    uint32_t reqsz = (uint32_t)(sizeof(mach_msg_header_t)
                                + sizeof(fs_read_req_t));
    memset(req, 0, reqsz);
    mach_msg_header_t *h = (mach_msg_header_t *)req;
    h->msgh_bits = 0;
    h->msgh_size = reqsz;
    h->msgh_id = FS_MSG_READFD;
    fs_read_req_t *rr = (fs_read_req_t *)(req + sizeof(mach_msg_header_t));
    rr->fd = (uint32_t)e->backend;
    rr->length = want;

    uint32_t got = 0;
    int rc = fs_rpc(req, reqsz, &got);
    if (rc < 0) {
        return rc;
    }
    uint8_t *resp = rpc_resp_buf();
    fs_resp_t *fr = resp_hdr(resp);
    if (fr->status != FS_OK) {
        return fs_status_to_errno(fr->status);
    }
    if (got < RESP_RET_SIZE) {
        return -SUKI_EIO;
    }
    int64_t nread = resp_ret(resp)->value;
    if (nread < 0) {
        return fs_status_to_errno((uint32_t)(-nread));
    }
    if (nread == 0) {
        return 0;                       /* EOF */
    }
    /* 不变量 C：声称的数据量必须在真实收到的消息长度之内 */
    if ((uint32_t)nread > FS_READ_MAX
        || got < RESP_RET_SIZE + (uint32_t)nread) {
        return -SUKI_EIO;
    }
    const uint8_t *data = resp + RESP_RET_SIZE;
    if (copy_to_user(ubuf, data, (size_t)nread) != (size_t)nread) {
        return -SUKI_EFAULT;
    }
    return (suki_ssize_t)nread;
}

/*
 * fd_read_kern —— 内核态读变体。
 *
 * 背景：普通 fd_read 把 FS/后端数据经 copy_to_user() 写回【用户虚拟地址】，
 * 因为 syscall 语义要求数据最终落到调用进程的用户缓冲。但内核自身需要把
 * 文件内容（典型如 SukiNative PROC_CREATE 要装载的 ELF 映像）读进【内核
 * 虚拟地址】再交给 elf_load()，此时若仍用 fd_read，copy_to_user 的内核地址
 * 校验会失败、返回 -EFAULT，导致读到 0 字节、blob 为空、elf_load 失败。
 *
 * 本函数对所有「常规文件」后端（DISK=FS_SERVER / TMPFS / DEVFS）直接把数据
 * memcpy 进内核缓冲 kbuf，不经过用户指针校验。TTY/PIPE/DIR 不是合法的
 * 可执行装载源，且需要用户虚拟地址做 copy_to_user，故直接返回 -EINVAL。
 *
 * 注意：DISK 后端的文件偏移由 FS_SERVER 侧按后端 fd 维护，内核 fd 层不碰
 * e->offset（与 fd_read 的 DISK 分支保持一致），故此处也不更新。
 */
suki_ssize_t fd_read_kern(struct task *t, int fd, void *kbuf, size_t count)
{
    if (count == 0) {
        return 0;
    }
    fd_entry_t *e = fd_get(t, fd);
    if (!e) {
        return -SUKI_EBADF;
    }
    if ((e->flags & SUKI_O_ACCMODE) == SUKI_O_WRONLY) {
        return -SUKI_EBADF;
    }
    if (e->type == FD_TYPE_TTY || e->type == FD_TYPE_PIPE) {
        return -SUKI_EINVAL;            /* 需用户虚拟地址做 copy_to_user，非装载源 */
    }
    if (e->type == FD_TYPE_DIR) {
        return -SUKI_EISDIR;
    }

    if (e->vfs_backend == FD_BACKEND_TMPFS) {
        /* 内建 tmpfs 文件读：直接读进内核缓冲（offset 用 fd 偏移） */
        uint64_t nread = 0;
        int rc = tmpfs_read(e->backend, (uint8_t *)kbuf, (uint32_t)count, &nread);
        if (rc < 0) {
            return rc;
        }
        e->offset += nread;
        return (suki_ssize_t)nread;
    }
    if (e->vfs_backend == FD_BACKEND_DEVFS) {
        /* 内建 devfs 字符设备读：直接读进内核缓冲 */
        uint64_t nread = 0;
        int rc = devfs_read((uint32_t)e->backend, (uint8_t *)kbuf,
                            (uint32_t)count, e->offset, &nread);
        if (rc < 0) {
            return rc;
        }
        e->offset += nread;
        return (suki_ssize_t)nread;
    }
    if (e->backend < 0) {
        return -SUKI_EBADF;
    }

    uint32_t want = (count > FS_READ_MAX) ? FS_READ_MAX : (uint32_t)count;
    uint8_t *req = rpc_req_buf();
    uint32_t reqsz = (uint32_t)(sizeof(mach_msg_header_t)
                                + sizeof(fs_read_req_t));
    memset(req, 0, reqsz);
    mach_msg_header_t *h = (mach_msg_header_t *)req;
    h->msgh_bits = 0;
    h->msgh_size = reqsz;
    h->msgh_id = FS_MSG_READFD;
    fs_read_req_t *rr = (fs_read_req_t *)(req + sizeof(mach_msg_header_t));
    rr->fd = (uint32_t)e->backend;
    rr->length = want;

    uint32_t got = 0;
    int rc = fs_rpc(req, reqsz, &got);
    if (rc < 0) {
        return rc;
    }
    uint8_t *resp = rpc_resp_buf();
    fs_resp_t *fr = resp_hdr(resp);
    if (fr->status != FS_OK) {
        return fs_status_to_errno(fr->status);
    }
    if (got < RESP_RET_SIZE) {
        return -SUKI_EIO;
    }
    int64_t nread = resp_ret(resp)->value;
    if (nread < 0) {
        return fs_status_to_errno((uint32_t)(-nread));
    }
    if (nread == 0) {
        return 0;                       /* EOF */
    }
    if ((uint32_t)nread > FS_READ_MAX
        || got < RESP_RET_SIZE + (uint32_t)nread) {
        return -SUKI_EIO;
    }
    const uint8_t *data = resp + RESP_RET_SIZE;
    memcpy(kbuf, data, (size_t)nread);  /* 内核->内核，不经 copy_to_user */
    return (suki_ssize_t)nread;
}

suki_ssize_t fd_write(struct task *t, int fd, const void *ubuf, size_t count)
{
    if (count == 0) {
        return 0;
    }
    fd_entry_t *e = fd_get(t, fd);
    if (!e) {
        return -SUKI_EBADF;
    }
    if ((e->flags & SUKI_O_ACCMODE) == SUKI_O_RDONLY) {
        return -SUKI_EBADF;
    }

    /* TTY：分批拷入内核再输出（红线：用户指针绝不直接交给输出路径） */
    if (e->type == FD_TYPE_TTY) {
        uint8_t *kbuf = g_tty_buf[cpu_index()];
        size_t done = 0;
        while (done < count) {
            size_t chunk = count - done;
            if (chunk > 512) {
                chunk = 512;
            }
            if (copy_from_user(kbuf, (const uint8_t *)ubuf + done, chunk)
                    != chunk) {
                return done ? (suki_ssize_t)done : -SUKI_EFAULT;
            }
            int w = tty_write(kbuf, chunk);
            if (w <= 0) {
                return done ? (suki_ssize_t)done : -SUKI_EIO;
            }
            done += (size_t)w;
        }
        return (suki_ssize_t)done;
    }
    if (e->type == FD_TYPE_PIPE) {
        return pipe_write(e, ubuf, count);
    }
    if (e->type == FD_TYPE_DIR) {
        return -SUKI_EBADF;
    }
    if (e->vfs_backend == FD_BACKEND_TMPFS) {
        /* 内建 tmpfs 文件写：copy_from_user -> tmpfs_write（offset=-1 表当前位） */
        uint8_t *kbuf = kmalloc(FS_WRITE_MAX);
        if (!kbuf) {
            return -SUKI_ENOMEM;
        }
        size_t total = 0;
        while (total < count) {
            size_t chunk = count - total;
            if (chunk > FS_WRITE_MAX) {
                chunk = FS_WRITE_MAX;
            }
            if (copy_from_user(kbuf, (const uint8_t *)ubuf + total, chunk) != chunk) {
                kfree(kbuf);
                return total ? (suki_ssize_t)total : -SUKI_EFAULT;
            }
            uint64_t nw = 0;
            int rc = tmpfs_write(e->backend, kbuf, (uint32_t)chunk,
                                 (uint64_t)-1, &nw);
            if (rc < 0) {
                kfree(kbuf);
                return total ? (suki_ssize_t)total : rc;
            }
            total += (size_t)nw;
        }
        kfree(kbuf);
        return (suki_ssize_t)total;
    }
    if (e->vfs_backend == FD_BACKEND_DEVFS) {
        /* 内建 devfs 字符设备写：直接转发到设备回调（e.g. /dev/console 输出） */
        uint8_t *kbuf = kmalloc(FS_WRITE_MAX);
        if (!kbuf) {
            return -SUKI_ENOMEM;
        }
        size_t total = 0;
        while (total < count) {
            size_t chunk = count - total;
            if (chunk > FS_WRITE_MAX) {
                chunk = FS_WRITE_MAX;
            }
            if (copy_from_user(kbuf, (const uint8_t *)ubuf + total, chunk) != chunk) {
                kfree(kbuf);
                return total ? (suki_ssize_t)total : -SUKI_EFAULT;
            }
            uint64_t nw = 0;
            int rc = devfs_write((uint32_t)e->backend, kbuf, (uint32_t)chunk,
                                 e->offset, &nw);
            if (rc < 0) {
                kfree(kbuf);
                return total ? (suki_ssize_t)total : rc;
            }
            total += (size_t)nw;
            e->offset += nw;
        }
        kfree(kbuf);
        return (suki_ssize_t)total;
    }
    if (e->backend < 0) {
        return -SUKI_EBADF;
    }

    /* 文件：单次最多 FS_WRITE_MAX，超出则循环分多次写 */
    uint8_t *wbuf = rpc_req_buf() + sizeof(mach_msg_header_t)
                    + sizeof(fs_write_fd_req_t);
    size_t total = 0;
    while (total < count) {
        size_t chunk = count - total;
        if (chunk > FS_WRITE_MAX) {
            chunk = FS_WRITE_MAX;
        }
        if (copy_from_user(wbuf, (const uint8_t *)ubuf + total, chunk)
                != chunk) {
            return total ? (suki_ssize_t)total : -SUKI_EFAULT;
        }

        uint8_t *req = rpc_req_buf();
        uint32_t reqsz = (uint32_t)(sizeof(mach_msg_header_t)
                                    + sizeof(fs_write_fd_req_t) + chunk);
        memset(req, 0, sizeof(mach_msg_header_t) + sizeof(fs_write_fd_req_t));
        mach_msg_header_t *h = (mach_msg_header_t *)req;
        h->msgh_bits = 0;
        h->msgh_size = reqsz;
        h->msgh_id = FS_MSG_WRITEFD;
        fs_write_fd_req_t *wr =
            (fs_write_fd_req_t *)(req + sizeof(mach_msg_header_t));
        wr->fd = (uint32_t)e->backend;
        wr->length = (uint32_t)chunk;
        wr->offset = -1LL;      /* 从当前文件位置写（POSIX write 语义） */

        uint32_t got = 0;
        int rc = fs_rpc(req, reqsz, &got);
        if (rc < 0) {
            return total ? (suki_ssize_t)total : rc;
        }
        uint8_t *resp = rpc_resp_buf();
        fs_resp_t *fr = resp_hdr(resp);
        if (fr->status != FS_OK) {
            int er = fs_status_to_errno(fr->status);
            return total ? (suki_ssize_t)total : er;
        }
        if (got < RESP_RET_SIZE) {
            return total ? (suki_ssize_t)total : -SUKI_EIO;
        }
        int64_t nw = resp_ret(resp)->value;
        if (nw <= 0) {
            /* 未推进：避免死循环，返回已写部分或 ENOSPC */
            return total ? (suki_ssize_t)total : -SUKI_ENOSPC;
        }
        total += (size_t)nw;
        if ((size_t)nw < chunk) {
            break;                      /* 短写（磁盘满）：按 POSIX 返回已写 */
        }
    }
    return (suki_ssize_t)total;
}

suki_off_t fd_lseek(struct task *t, int fd, suki_off_t off, int whence)
{
    fd_entry_t *e = fd_get(t, fd);
    if (!e) {
        return -SUKI_EBADF;
    }
    if (e->type == FD_TYPE_TTY || e->type == FD_TYPE_PIPE) {
        return -SUKI_ESPIPE;
    }
    if (e->vfs_backend == FD_BACKEND_TMPFS) {
        uint64_t pos = 0;
        int rc = tmpfs_lseek(e->backend, off, whence, &pos);
        if (rc < 0) {
            return (suki_off_t)rc;
        }
        e->offset = pos;   /* 同步 fd 层偏移（DISK 路径由服务端维护，fd 不依赖） */
        return (suki_off_t)pos;
    }
    if (e->vfs_backend == FD_BACKEND_DEVFS) {
        return -SUKI_ESPIPE;   /* 字符设备不支持 seek */
    }
    if (e->backend < 0) {
        return -SUKI_EBADF;
    }
    if (whence != SUKI_SEEK_SET && whence != SUKI_SEEK_CUR
        && whence != SUKI_SEEK_END) {
        return -SUKI_EINVAL;
    }

    uint8_t *req = rpc_req_buf();
    uint32_t reqsz = (uint32_t)(sizeof(mach_msg_header_t)
                                + sizeof(fs_seek_req_t));
    memset(req, 0, reqsz);
    mach_msg_header_t *h = (mach_msg_header_t *)req;
    h->msgh_bits = 0;
    h->msgh_size = reqsz;
    h->msgh_id = FS_MSG_SEEKFD;
    fs_seek_req_t *sr = (fs_seek_req_t *)(req + sizeof(mach_msg_header_t));
    sr->fd = (uint32_t)e->backend;
    sr->whence = (int32_t)whence;
    sr->offset = off;

    uint32_t got = 0;
    int rc = fs_rpc(req, reqsz, &got);
    if (rc < 0) {
        return rc;
    }
    uint8_t *resp = rpc_resp_buf();
    fs_resp_t *fr = resp_hdr(resp);
    if (fr->status != FS_OK) {
        return fs_status_to_errno(fr->status);
    }
    if (got < RESP_RET_SIZE) {
        return -SUKI_EIO;
    }
    int64_t pos = resp_ret(resp)->value;
    if (pos < 0) {
        return fs_status_to_errno((uint32_t)(-pos));
    }
    return (suki_off_t)pos;
}

/* 把内核态 fs_stat_t 拷贝到用户态 suki_stat_t（字段语义一致） */
static void fd_stat_copy(suki_stat_t *out, const fs_stat_t *fs_st)
{
    memset(out, 0, sizeof(*out));
    out->st_dev = fs_st->dev;
    out->st_ino = fs_st->ino;
    out->st_mode = fs_st->mode;
    out->st_nlink = fs_st->nlink;
    out->st_uid = fs_st->uid;
    out->st_gid = fs_st->gid;
    out->st_rdev = fs_st->rdev;
    out->st_size = fs_st->size;
    out->st_blksize = fs_st->blksize;
    out->st_blocks = fs_st->blocks;
    out->st_atim_sec = fs_st->atime;
    out->st_atim_nsec = 0;
    out->st_mtim_sec = fs_st->mtime;
    out->st_mtim_nsec = 0;
    out->st_ctim_sec = fs_st->ctime;
    out->st_ctim_nsec = 0;
}

/* stat / fstat 共用：by_fd=1 走句柄，=0 走路径 */
static int fd_stat_common(int by_fd, int backend, const char *path,
                          suki_stat_t *out)
{
    uint8_t *req = rpc_req_buf();
    uint32_t reqsz;
    memset(req, 0, FS_RPC_MAX);
    mach_msg_header_t *h = (mach_msg_header_t *)req;
    h->msgh_bits = 0;
    h->msgh_id = by_fd ? FS_MSG_FSTAT : FS_MSG_STAT;

    if (by_fd) {
        reqsz = (uint32_t)(sizeof(mach_msg_header_t) + sizeof(fs_fd_req_t));
        fs_fd_req_t *fr2 = (fs_fd_req_t *)(req + sizeof(mach_msg_header_t));
        fr2->fd = (uint32_t)backend;
    } else {
        size_t plen = strlen(path);
        if (plen >= FS_PATH_MAX) {
            return -SUKI_ENAMETOOLONG;
        }
        reqsz = (uint32_t)(sizeof(mach_msg_header_t) + plen + 1);
        memcpy(req + sizeof(mach_msg_header_t), path, plen + 1);
    }
    h->msgh_size = reqsz;

    uint32_t got = 0;
    int rc = fs_rpc(req, reqsz, &got);
    if (rc < 0) {
        return rc;
    }
    uint8_t *resp = rpc_resp_buf();
    fs_resp_t *fr = resp_hdr(resp);
    if (fr->status != FS_OK) {
        return fs_status_to_errno(fr->status);
    }
    uint32_t need = RESP_RET_SIZE + (uint32_t)sizeof(fs_stat_t);
    if (got < need) {
        return -SUKI_EIO;
    }
    if (resp_ret(resp)->value < 0) {
        return fs_status_to_errno((uint32_t)(-resp_ret(resp)->value));
    }
    const fs_stat_t *fs_st = (const fs_stat_t *)(resp + RESP_RET_SIZE);
    fd_stat_copy(out, fs_st);
    return 0;
}

int fd_fstat(struct task *t, int fd, suki_stat_t *out)
{
    fd_entry_t *e = fd_get(t, fd);
    if (!e) {
        return -SUKI_EBADF;
    }
    if (e->type == FD_TYPE_TTY) {
        memset(out, 0, sizeof(*out));
        out->st_mode = SUKI_S_IFCHR | 0644;
        out->st_blksize = 4096;
        out->st_nlink = 1;
        return 0;
    }
    if (e->type == FD_TYPE_PIPE) {
        memset(out, 0, sizeof(*out));
        out->st_mode = SUKI_S_IFIFO | 0644;
        out->st_size = e->pipe ? (int64_t)e->pipe->count : 0;
        out->st_blksize = FD_PIPE_BYTES;
        out->st_nlink = 1;
        return 0;
    }
    if (e->vfs_backend == FD_BACKEND_TMPFS) {
        fs_stat_t st;
        int rc = tmpfs_fstat(e->backend, &st);
        if (rc < 0) {
            return rc;
        }
        fd_stat_copy(out, &st);
        return 0;
    }
    if (e->vfs_backend == FD_BACKEND_DEVFS) {
        fs_stat_t st;
        int rc = devfs_stat(e->path ? e->path : "/dev", &st);
        /* devfs_stat/devfs_find 可识别完整路径 /dev/xxx 或相对名 */
        if (rc < 0) {
            return rc;
        }
        fd_stat_copy(out, &st);
        return 0;
    }
    if (e->backend < 0) {
        return -SUKI_EBADF;
    }
    return fd_stat_common(1, e->backend, NULL, out);
}

int fd_stat(const char *path, suki_stat_t *out)
{
    if (!path || path[0] == '\0') {
        return -SUKI_EINVAL;
    }
    /* VFS 路由：内建后端走 vfs_builtin_stat */
    if (vfs_ready()) {
        vfs_resolved_t vr;
        vfs_resolve(path, &vr);
        if (vr.backend == VFS_BACKEND_TMPFS || vr.backend == VFS_BACKEND_DEVFS) {
            fs_stat_t st;
            int rc = vfs_builtin_stat(vr.rel, &st, vr.backend);
            if (rc < 0) {
                return rc;
            }
            fd_stat_copy(out, &st);
            return 0;
        }
    }
    return fd_stat_common(0, 0, path, out);
}

int fd_opendir(struct task *t, const char *path)
{
    if (!t || !path || path[0] == '\0') {
        return -SUKI_EINVAL;
    }
    size_t plen = strlen(path);
    if (plen >= FS_PATH_MAX) {
        return -SUKI_ENAMETOOLONG;
    }

    /* VFS 路由：内建后端（tmpfs/devfs 目录）走 vfs_builtin_opendir */
    if (vfs_ready()) {
        vfs_resolved_t vr;
        vfs_resolve(path, &vr);
        if (vr.backend == VFS_BACKEND_TMPFS || vr.backend == VFS_BACKEND_DEVFS) {
            int dd = vfs_builtin_opendir(vr.rel, vr.backend);
            if (dd < 0) {
                return dd;
            }
            uint64_t f = spin_lock_irqsave(&g_fd_lock);
            int fdnum = fd_bind_locked(t, FD_TYPE_DIR,
                                       SUKI_O_RDONLY | SUKI_O_DIRECTORY);
            if (fdnum >= 0) {
                fd_entry_t *e = &g_fd_slots[t->fds[fdnum]];
                e->backend = dd;     /* 内建目录句柄 */
                e->vfs_backend = (vr.backend == VFS_BACKEND_TMPFS)
                                 ? FD_BACKEND_TMPFS : FD_BACKEND_DEVFS;
                e->path = (char *)kmalloc(plen + 1);
                if (e->path) {
                    memcpy(e->path, path, plen + 1);
                }
            }
            spin_unlock_irqrestore(&g_fd_lock, f);
            if (fdnum < 0) {
                vfs_builtin_closedir(dd, vr.backend);   /* 槽满，关内建句柄 */
                return fdnum;
            }
            return fdnum;
        }
    }

    uint8_t *req = rpc_req_buf();
    uint32_t reqsz = (uint32_t)(sizeof(mach_msg_header_t) + plen + 1);
    memset(req, 0, reqsz);
    mach_msg_header_t *h = (mach_msg_header_t *)req;
    h->msgh_bits = 0;
    h->msgh_size = reqsz;
    h->msgh_id = FS_MSG_OPENDIR;
    memcpy(req + sizeof(mach_msg_header_t), path, plen + 1);

    uint32_t got = 0;
    int rc = fs_rpc(req, reqsz, &got);
    if (rc < 0) {
        return rc;
    }
    uint8_t *resp = rpc_resp_buf();
    fs_resp_t *fr = resp_hdr(resp);
    if (fr->status != FS_OK) {
        return fs_status_to_errno(fr->status);
    }
    if (got < RESP_RET_SIZE) {
        return -SUKI_EIO;
    }
    int64_t dd = resp_ret(resp)->value;
    if (dd < 0) {
        return fs_status_to_errno((uint32_t)(-dd));
    }

    uint64_t f = spin_lock_irqsave(&g_fd_lock);
    int fdnum = fd_bind_locked(t, FD_TYPE_DIR,
                               SUKI_O_RDONLY | SUKI_O_DIRECTORY);
    if (fdnum >= 0) {
        fd_entry_t *e = &g_fd_slots[t->fds[fdnum]];
        e->backend = (int)dd;
        e->vfs_backend = FD_BACKEND_DISK;
        e->path = (char *)kmalloc(plen + 1);
        if (e->path) {
            memcpy(e->path, path, plen + 1);
        }
    }
    spin_unlock_irqrestore(&g_fd_lock, f);

    if (fdnum < 0) {
        fd_entry_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.type = FD_TYPE_DIR;
        tmp.backend = (int)dd;
        fd_close_backend(&tmp);
        return fdnum;
    }
    return fdnum;
}

int fd_readdir(struct task *t, int fd, suki_dirent_t *out)
{
    fd_entry_t *e = fd_get(t, fd);
    if (!e) {
        return -SUKI_EBADF;
    }
    if (e->type != FD_TYPE_DIR || e->backend < 0) {
        return -SUKI_EBADF;
    }

    /* 内建后端目录读：直接调 vfs_builtin_readdir */
    if (e->vfs_backend == FD_BACKEND_TMPFS || e->vfs_backend == FD_BACKEND_DEVFS) {
        fs_dirent_t de;
        int rc = vfs_builtin_readdir(e->backend, &de, e->vfs_backend);
        if (rc < 0) {
            return rc;
        }
        /* rc==1 有条目，rc==0 目录结束 */
        memset(out, 0, sizeof(*out));
        out->d_ino = de.ino;
        out->d_type = de.type;
        strncpy(out->d_name, de.name, sizeof(out->d_name) - 1);
        return rc;
    }

    uint8_t *req = rpc_req_buf();
    uint32_t reqsz = (uint32_t)(sizeof(mach_msg_header_t)
                                + sizeof(fs_fd_req_t));
    memset(req, 0, reqsz);
    mach_msg_header_t *h = (mach_msg_header_t *)req;
    h->msgh_bits = 0;
    h->msgh_size = reqsz;
    h->msgh_id = FS_MSG_READDIR;
    fs_fd_req_t *dr = (fs_fd_req_t *)(req + sizeof(mach_msg_header_t));
    dr->fd = (uint32_t)e->backend;

    uint32_t got = 0;
    int rc = fs_rpc(req, reqsz, &got);
    if (rc < 0) {
        return rc;
    }
    uint8_t *resp = rpc_resp_buf();
    fs_resp_t *fr = resp_hdr(resp);
    if (fr->status != FS_OK) {
        return fs_status_to_errno(fr->status);
    }
    if (got < RESP_RET_SIZE) {
        return -SUKI_EIO;
    }
    int64_t have = resp_ret(resp)->value;
    if (have < 0) {
        return fs_status_to_errno((uint32_t)(-have));
    }
    if (have == 0) {
        return 0;                       /* 目录结束 */
    }
    if (got < RESP_RET_SIZE + (uint32_t)sizeof(fs_dirent_t)) {
        return -SUKI_EIO;
    }
    const fs_dirent_t *de = (const fs_dirent_t *)(resp + RESP_RET_SIZE);

    memset(out, 0, sizeof(*out));
    out->d_ino = de->ino;
    out->d_type = de->type;
    out->d_reclen = (uint16_t)sizeof(suki_dirent_t);
    out->d_off = (int64_t)(++e->offset);   /* 内核侧游标计数 */
    {
        size_t i = 0;
        for (; i < sizeof(out->d_name) - 1 && de->name[i]; i++) {
            out->d_name[i] = de->name[i];
        }
        out->d_name[i] = '\0';              /* 强制终结，绝不信任外部数据 */
    }
    return 1;
}

/* 单路径操作（unlink/rmdir/mkdir/access）：统一走路径式消息 */
static int fd_path_op(uint32_t msg_id, const char *path, int32_t mode)
{
    if (!path || path[0] == '\0') {
        return -SUKI_EINVAL;
    }
    size_t plen = strlen(path);
    if (plen >= FS_PATH_MAX) {
        return -SUKI_ENAMETOOLONG;
    }

    /* VFS 路由：内建后端（tmpfs/devfs）走 vfs_builtin_* */
    if (vfs_ready()) {
        vfs_resolved_t vr;
        vfs_resolve(path, &vr);
        if (vr.backend == VFS_BACKEND_TMPFS || vr.backend == VFS_BACKEND_DEVFS) {
            if (msg_id == FS_MSG_UNLINK2) {
                return vfs_builtin_unlink(vr.rel, mode ? true : false, vr.backend);
            }
            if (msg_id == FS_MSG_MKDIR2) {
                return vfs_builtin_mkdir(vr.rel, (uint32_t)mode, vr.backend);
            }
            if (msg_id == FS_MSG_ACCESS) {
                return vfs_builtin_access(vr.rel, mode, vr.backend);
            }
            /* 其它 msg_id 在内建后端不支持，回落到下面 DISK 路径会失败；
             * 但本项目 FD 层只有这三类走 fd_path_op 且内建支持，故不会到此。 */
        }
    }
    uint8_t *req = rpc_req_buf();
    uint32_t reqsz = (uint32_t)(sizeof(mach_msg_header_t)
                                + sizeof(fs_path_mode_req_t) + plen + 1);
    memset(req, 0, reqsz);
    mach_msg_header_t *h = (mach_msg_header_t *)req;
    h->msgh_bits = 0;
    h->msgh_id = msg_id;
    h->msgh_size = reqsz;
    fs_path_mode_req_t *pm =
        (fs_path_mode_req_t *)(req + sizeof(mach_msg_header_t));
    pm->mode = mode;
    memcpy(req + sizeof(mach_msg_header_t) + sizeof(fs_path_mode_req_t),
           path, plen + 1);

    uint32_t got = 0;
    int rc = fs_rpc(req, reqsz, &got);
    if (rc < 0) {
        return rc;
    }
    fs_resp_t *fr = resp_hdr(rpc_resp_buf());
    if (fr->status != FS_OK) {
        return fs_status_to_errno(fr->status);
    }
    if (got >= RESP_RET_SIZE && resp_ret(rpc_resp_buf())->value < 0) {
        return fs_status_to_errno((uint32_t)(-resp_ret(rpc_resp_buf())->value));
    }
    return 0;
}

int fd_unlink(const char *path) { return fd_path_op(FS_MSG_UNLINK2, path, 0); }
int fd_rmdir(const char *path)  { return fd_path_op(FS_MSG_UNLINK2, path, 1); }
int fd_mkdir(const char *path, uint32_t mode)
{
    return fd_path_op(FS_MSG_MKDIR2, path, (int32_t)mode);
}
int fd_access(const char *path, int mode)
{
    /* 根目录特判（P0-R7）："/" 总是存在且可访问（由 FS_SERVER 挂载的卷根）。
     * 无需发 IPC 给 FS_SERVER 做 f_stat（FatFs 对根目录的 stat 在不同配置下
     * 行为不稳定）。F_OK/R_OK/W_OK 对根目录均成立。 */
    if (path && path[0] == '/' && path[1] == '\0') {
        (void)mode;
        return 0;
    }
    return fd_path_op(FS_MSG_ACCESS, path, (int32_t)mode);
}

int fd_rename(const char *oldp, const char *newp)
{
    if (!oldp || !newp || oldp[0] == '\0' || newp[0] == '\0') {
        return -SUKI_EINVAL;
    }
    /* VFS 路由：两路径须同属一个内建后端才可重命名（暂不支持跨后端） */
    if (vfs_ready()) {
        vfs_resolved_t vo, vn;
        vfs_resolve(oldp, &vo);
        vfs_resolve(newp, &vn);
        if ((vo.backend == VFS_BACKEND_TMPFS || vo.backend == VFS_BACKEND_DEVFS)
            && vo.backend == vn.backend) {
            return vfs_builtin_rename(vo.rel, vn.rel, vo.backend);
        }
    }
    size_t ol = strlen(oldp), nl = strlen(newp);
    if (ol >= FS_PATH_MAX || nl >= FS_PATH_MAX) {
        return -SUKI_ENAMETOOLONG;
    }
    uint8_t *req = rpc_req_buf();
    uint32_t reqsz = (uint32_t)(sizeof(mach_msg_header_t) + ol + 1 + nl + 1);
    if (reqsz > FS_RPC_MAX) {
        return -SUKI_EINVAL;
    }
    memset(req, 0, reqsz);
    mach_msg_header_t *h = (mach_msg_header_t *)req;
    h->msgh_bits = 0;
    h->msgh_id = FS_MSG_RENAME2;
    h->msgh_size = reqsz;
    memcpy(req + sizeof(mach_msg_header_t), oldp, ol + 1);
    memcpy(req + sizeof(mach_msg_header_t) + ol + 1, newp, nl + 1);

    uint32_t got = 0;
    int rc = fs_rpc(req, reqsz, &got);
    if (rc < 0) {
        return rc;
    }
    fs_resp_t *fr = resp_hdr(rpc_resp_buf());
    if (fr->status != FS_OK) {
        return fs_status_to_errno(fr->status);
    }
    if (got >= RESP_RET_SIZE && resp_ret(rpc_resp_buf())->value < 0) {
        return fs_status_to_errno((uint32_t)(-resp_ret(rpc_resp_buf())->value));
    }
    return 0;
}

int fd_truncate(const char *path, suki_off_t len)
{
    if (!path || path[0] == '\0' || len < 0) {
        return -SUKI_EINVAL;
    }
    size_t plen = strlen(path);
    if (plen >= FS_PATH_MAX) {
        return -SUKI_ENAMETOOLONG;
    }
    uint8_t *req = rpc_req_buf();
    uint32_t reqsz = (uint32_t)(sizeof(mach_msg_header_t)
                                + sizeof(fs_trunc_req_t) + plen + 1);
    memset(req, 0, reqsz);
    mach_msg_header_t *h = (mach_msg_header_t *)req;
    h->msgh_bits = 0;
    h->msgh_id = FS_MSG_TRUNCATE;
    h->msgh_size = reqsz;
    fs_trunc_req_t *tr = (fs_trunc_req_t *)(req + sizeof(mach_msg_header_t));
    tr->size = (uint32_t)((uint64_t)len > 0xFFFFFFFFULL
                          ? 0xFFFFFFFFULL : (uint64_t)len);
    memcpy(req + sizeof(mach_msg_header_t) + sizeof(fs_trunc_req_t),
           path, plen + 1);

    uint32_t got = 0;
    int rc = fs_rpc(req, reqsz, &got);
    if (rc < 0) {
        return rc;
    }
    fs_resp_t *fr = resp_hdr(rpc_resp_buf());
    if (fr->status != FS_OK) {
        return fs_status_to_errno(fr->status);
    }
    if (got >= RESP_RET_SIZE && resp_ret(rpc_resp_buf())->value < 0) {
        return fs_status_to_errno((uint32_t)(-resp_ret(rpc_resp_buf())->value));
    }
    return 0;
}

int fd_ftruncate(struct task *t, int fd, suki_off_t len)
{
    fd_entry_t *e = fd_get(t, fd);
    if (!e) {
        return -SUKI_EBADF;
    }
    if (e->type != FD_TYPE_FILE || e->backend < 0 || len < 0) {
        return -SUKI_EINVAL;
    }
    uint8_t *req = rpc_req_buf();
    uint32_t reqsz = (uint32_t)(sizeof(mach_msg_header_t)
                                + sizeof(fs_ftrunc_req_t));
    memset(req, 0, reqsz);
    mach_msg_header_t *h = (mach_msg_header_t *)req;
    h->msgh_bits = 0;
    h->msgh_id = FS_MSG_FTRUNC;
    h->msgh_size = reqsz;
    fs_ftrunc_req_t *fr2 = (fs_ftrunc_req_t *)(req + sizeof(mach_msg_header_t));
    fr2->fd = (uint32_t)e->backend;
    fr2->length = len;

    uint32_t got = 0;
    int rc = fs_rpc(req, reqsz, &got);
    if (rc < 0) {
        return rc;
    }
    fs_resp_t *fr = resp_hdr(rpc_resp_buf());
    if (fr->status != FS_OK) {
        return fs_status_to_errno(fr->status);
    }
    if (got >= RESP_RET_SIZE && resp_ret(rpc_resp_buf())->value < 0) {
        return fs_status_to_errno((uint32_t)(-resp_ret(rpc_resp_buf())->value));
    }
    return 0;
}

int fd_dup(struct task *t, int oldfd)
{
    uint64_t f = spin_lock_irqsave(&g_fd_lock);
    if (oldfd < 0 || oldfd >= SUKI_FD_MAX || t->fds[oldfd] < 0) {
        spin_unlock_irqrestore(&g_fd_lock, f);
        return -SUKI_EBADF;
    }
    int s = t->fds[oldfd];
    int fdnum = -1;
    for (int i = 0; i < SUKI_FD_MAX; i++) {
        if (t->fds[i] < 0) {
            fdnum = i;
            break;
        }
    }
    if (fdnum < 0) {
        spin_unlock_irqrestore(&g_fd_lock, f);
        return -SUKI_EMFILE;
    }
    g_fd_slots[s].refcount++;
    t->fds[fdnum] = s;
    spin_unlock_irqrestore(&g_fd_lock, f);
    return fdnum;
}

int fd_dup2(struct task *t, int oldfd, int newfd)
{
    if (oldfd < 0 || oldfd >= SUKI_FD_MAX) {
        return -SUKI_EBADF;
    }
    if (newfd < 0 || newfd >= SUKI_FD_MAX) {
        return -SUKI_EBADF;
    }
    if (oldfd == newfd) {
        return (t->fds[oldfd] >= 0) ? newfd : -SUKI_EBADF;
    }

    int slot = -1;
    bool zero = false;
    uint64_t f = spin_lock_irqsave(&g_fd_lock);
    int s = t->fds[oldfd];
    if (s < 0) {
        spin_unlock_irqrestore(&g_fd_lock, f);
        return -SUKI_EBADF;
    }
    if (t->fds[newfd] >= 0) {
        zero = fd_unbind_locked(t, newfd, &slot);
    }
    g_fd_slots[s].refcount++;
    t->fds[newfd] = s;
    spin_unlock_irqrestore(&g_fd_lock, f);

    if (zero) {
        fd_release_slot(slot);     /* 解锁后释放被覆盖的旧槽 */
    }
    return newfd;
}

int fd_pipe(struct task *t, int fds[2])
{
    if (!t || !fds) {
        return -SUKI_EINVAL;
    }
    fd_pipe_t *pp = (fd_pipe_t *)kmalloc(sizeof(fd_pipe_t));
    if (!pp) {
        return -SUKI_ENOMEM;
    }
    memset(pp, 0, sizeof(*pp));
    pp->refs = 2;               /* 读端 + 写端各一份 */

    uint64_t f = spin_lock_irqsave(&g_fd_lock);
    int rs = slot_alloc_locked();
    int ws = slot_alloc_locked();
    if (rs < 0 || ws < 0) {
        if (rs >= 0) { g_fd_slots[rs].refcount = 0; g_fd_slots[rs].type = FD_TYPE_NONE; }
        if (ws >= 0) { g_fd_slots[ws].refcount = 0; g_fd_slots[ws].type = FD_TYPE_NONE; }
        spin_unlock_irqrestore(&g_fd_lock, f);
        kfree(pp);
        return -SUKI_EMFILE;
    }
    g_fd_slots[rs].type = FD_TYPE_PIPE;
    g_fd_slots[rs].flags = SUKI_O_RDONLY;
    g_fd_slots[rs].pipe = pp;
    g_fd_slots[rs].backend = ws;      /* 读端记住写端槽号（探测 EOF） */

    g_fd_slots[ws].type = FD_TYPE_PIPE;
    g_fd_slots[ws].flags = SUKI_O_WRONLY;
    g_fd_slots[ws].pipe = pp;
    g_fd_slots[ws].backend = rs;      /* 写端记住读端槽号（探测 EPIPE） */

    int rfd = -1, wfd = -1;
    for (int i = 0; i < SUKI_FD_MAX; i++) {
        if (t->fds[i] < 0) {
            if (rfd < 0) {
                rfd = i;
            } else {
                wfd = i;
                break;
            }
        }
    }
    if (rfd < 0 || wfd < 0) {
        g_fd_slots[rs].refcount = 0;
        g_fd_slots[rs].type = FD_TYPE_NONE;
        g_fd_slots[ws].refcount = 0;
        g_fd_slots[ws].type = FD_TYPE_NONE;
        spin_unlock_irqrestore(&g_fd_lock, f);
        kfree(pp);
        return -SUKI_EMFILE;
    }
    t->fds[rfd] = rs;
    t->fds[wfd] = ws;
    spin_unlock_irqrestore(&g_fd_lock, f);

    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}

int fd_fcntl(struct task *t, int fd, int cmd, int64_t arg)
{
    fd_entry_t *e = fd_get(t, fd);
    if (!e) {
        return -SUKI_EBADF;
    }
    switch (cmd) {
    case SUKI_F_DUPFD: {
        if (arg < 0 || arg >= SUKI_FD_MAX) {
            return -SUKI_EINVAL;
        }
        uint64_t f = spin_lock_irqsave(&g_fd_lock);
        int s = t->fds[fd];
        int fdnum = -1;
        for (int i = (int)arg; i < SUKI_FD_MAX; i++) {
            if (t->fds[i] < 0) {
                fdnum = i;
                break;
            }
        }
        if (fdnum < 0) {
            spin_unlock_irqrestore(&g_fd_lock, f);
            return -SUKI_EMFILE;
        }
        g_fd_slots[s].refcount++;
        t->fds[fdnum] = s;
        spin_unlock_irqrestore(&g_fd_lock, f);
        return fdnum;
    }
    case SUKI_F_GETFD:
        return (e->flags & SUKI_O_CLOEXEC) ? SUKI_FD_CLOEXEC : 0;
    case SUKI_F_SETFD:
        if (arg & SUKI_FD_CLOEXEC) {
            e->flags |= SUKI_O_CLOEXEC;
        } else {
            e->flags &= ~SUKI_O_CLOEXEC;
        }
        return 0;
    case SUKI_F_GETFL:
        return e->flags;
    case SUKI_F_SETFL:
        /* 仅允许改 O_APPEND / O_NONBLOCK；访问模式不可事后变更 */
        e->flags = (int32_t)((e->flags & ~(SUKI_O_APPEND | SUKI_O_NONBLOCK))
                             | (int32_t)(arg & (SUKI_O_APPEND
                                                | SUKI_O_NONBLOCK)));
        return 0;
    default:
        return -SUKI_EINVAL;
    }
}

int fd_isatty(struct task *t, int fd)
{
    fd_entry_t *e = fd_get(t, fd);
    if (!e) {
        return -SUKI_EBADF;
    }
    if (e->type != FD_TYPE_TTY) {
        return -SUKI_ENOTTY;
    }
    return 1;
}

int fd_statfs(suki_statfs_t *out)
{
    uint8_t *req = rpc_req_buf();
    uint32_t reqsz = (uint32_t)sizeof(mach_msg_header_t);
    memset(req, 0, reqsz);
    mach_msg_header_t *h = (mach_msg_header_t *)req;
    h->msgh_bits = 0;
    h->msgh_size = reqsz;
    h->msgh_id = FS_MSG_STATFS;

    uint32_t got = 0;
    int rc = fs_rpc(req, reqsz, &got);
    if (rc < 0) {
        return rc;
    }
    uint8_t *resp = rpc_resp_buf();
    fs_resp_t *fr = resp_hdr(resp);
    if (fr->status != FS_OK) {
        return fs_status_to_errno(fr->status);
    }
    uint32_t need = RESP_RET_SIZE + (uint32_t)sizeof(fs_statfs_t);
    if (got < need) {
        return -SUKI_EIO;
    }
    const fs_statfs_t *sf = (const fs_statfs_t *)(resp + RESP_RET_SIZE);
    memset(out, 0, sizeof(*out));
    out->f_type = sf->type;
    out->f_bsize = sf->bsize;
    out->f_blocks = sf->blocks;
    out->f_bfree = sf->bfree;
    out->f_files = sf->files;
    out->f_ffree = sf->ffree;
    out->f_namelen = sf->namelen;
    return 0;
}

/*
 * chmod / utimes：经 FS_SERVER 的 f_chmod / f_utime 真正改写卷上的
 * FAT 属性字节与时间戳（不是占位实现）。
 *   fd_chmod  : 路径式，mode 用 POSIX 权限位（FS 侧只承载「只读」位）
 *   fd_utimes : 路径式，atime/mtime 为 POSIX 秒，<0 表示不修改该字段
 */
int fd_chmod(const char *path, uint32_t mode)
{
    if (!path || path[0] == '\0') {
        return -SUKI_EINVAL;
    }
    size_t plen = strlen(path);
    if (plen >= FS_PATH_MAX) {
        return -SUKI_ENAMETOOLONG;
    }
    if (vfs_ready()) {
        vfs_resolved_t vr;
        vfs_resolve(path, &vr);
        if (vr.backend == VFS_BACKEND_TMPFS || vr.backend == VFS_BACKEND_DEVFS) {
            return vfs_builtin_chmod(vr.rel, mode, vr.backend);
        }
    }
    uint8_t *req = rpc_req_buf();
    uint32_t reqsz = (uint32_t)(sizeof(mach_msg_header_t)
                                + sizeof(fs_chmod_req_t) + plen + 1);
    memset(req, 0, reqsz);
    mach_msg_header_t *h = (mach_msg_header_t *)req;
    h->msgh_bits = 0;
    h->msgh_id = FS_MSG_CHMOD;
    h->msgh_size = reqsz;
    fs_chmod_req_t *cm = (fs_chmod_req_t *)(req + sizeof(mach_msg_header_t));
    cm->mode = mode;
    cm->reserved = 0;
    memcpy(req + sizeof(mach_msg_header_t) + sizeof(fs_chmod_req_t),
           path, plen + 1);

    uint32_t got = 0;
    int rc = fs_rpc(req, reqsz, &got);
    if (rc < 0) {
        return rc;
    }
    fs_resp_t *fr = resp_hdr(rpc_resp_buf());
    if (fr->status != FS_OK) {
        return fs_status_to_errno(fr->status);
    }
    if (got >= RESP_RET_SIZE && resp_ret(rpc_resp_buf())->value < 0) {
        return fs_status_to_errno((uint32_t)(-resp_ret(rpc_resp_buf())->value));
    }
    return 0;
}

int fd_utimes(const char *path, int64_t atime, int64_t mtime)
{
    if (!path || path[0] == '\0') {
        return -SUKI_EINVAL;
    }
    size_t plen = strlen(path);
    if (plen >= FS_PATH_MAX) {
        return -SUKI_ENAMETOOLONG;
    }
    if (vfs_ready()) {
        vfs_resolved_t vr;
        vfs_resolve(path, &vr);
        if (vr.backend == VFS_BACKEND_TMPFS || vr.backend == VFS_BACKEND_DEVFS) {
            return vfs_builtin_utime(vr.rel, atime, mtime, vr.backend);
        }
    }
    uint8_t *req = rpc_req_buf();
    uint32_t reqsz = (uint32_t)(sizeof(mach_msg_header_t)
                                + sizeof(fs_utime_req_t) + plen + 1);
    memset(req, 0, reqsz);
    mach_msg_header_t *h = (mach_msg_header_t *)req;
    h->msgh_bits = 0;
    h->msgh_id = FS_MSG_UTIME;
    h->msgh_size = reqsz;
    fs_utime_req_t *ut = (fs_utime_req_t *)(req + sizeof(mach_msg_header_t));
    ut->atime = atime;
    ut->mtime = mtime;
    memcpy(req + sizeof(mach_msg_header_t) + sizeof(fs_utime_req_t),
           path, plen + 1);

    uint32_t got = 0;
    int rc = fs_rpc(req, reqsz, &got);
    if (rc < 0) {
        return rc;
    }
    fs_resp_t *fr = resp_hdr(rpc_resp_buf());
    if (fr->status != FS_OK) {
        return fs_status_to_errno(fr->status);
    }
    if (got >= RESP_RET_SIZE && resp_ret(rpc_resp_buf())->value < 0) {
        return fs_status_to_errno((uint32_t)(-resp_ret(rpc_resp_buf())->value));
    }
    return 0;
}

int fd_sync(void)
{
    uint8_t *req = rpc_req_buf();
    uint32_t reqsz = (uint32_t)sizeof(mach_msg_header_t);
    memset(req, 0, reqsz);
    mach_msg_header_t *h = (mach_msg_header_t *)req;
    h->msgh_bits = 0;
    h->msgh_size = reqsz;
    h->msgh_id = FS_MSG_SYNC;
    uint32_t got = 0;
    int rc = fs_rpc(req, reqsz, &got);
    if (rc < 0) {
        return rc;
    }
    return fs_status_to_errno(resp_hdr(rpc_resp_buf())->status);
}
