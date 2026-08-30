# Step 46 — 内核 VFS 路由层 + 内建 tmpfs/devfs 后端（含 /dev/zero 句柄修复）

## 一、背景与目标

在 Step 45 中我们修复了 `ata.c` 的 DMA 相关问题并确认磁盘 DMA 已启用
（`[ata] BMIDE DMA self-test PASSED -> ENABLED`）。用户确认 DMA 已启用后，本步开始
构建内核 **VFS（Virtual File System）路由层**，目标是把"一切皆文件"的 POSIX 接口统一，
并内置两个内核态文件系统后端：

- **tmpfs**：内核内存文件系统，挂载于 `/tmp`、`/run`（不占磁盘、零拷贝、启动即用）。
- **devfs**：字符设备文件系统，挂载于 `/dev`，暴露标准设备
  `/dev/null`、`/dev/zero`、`/dev/urandom`、`/dev/console`，不经 IPC，零端口占用。

DISK 后端（`/`）仍走原 FS_PORT 的 FatFs 服务（100% 兼容既有磁盘镜像逻辑），
本步**不破坏**任何既有磁盘路径，只是加了一层挂载点路由。

设计依据参考 OSDev 维基「Device File」「Virtual File System」「tmpfs」条目的
"一切皆文件 / 挂载点最长前缀匹配 / 字符设备 ioctl 回调" 标准做法。

## 二、模块与文件清单

### 2.1 新建文件

| 文件 | 作用 |
|------|------|
| `include/kernel/vfs.h` | VFS 公共头：后端枚举、挂载表、解析结构、API 声明 |
| `kernel/fs/vfs.c` | 挂载表 + `vfs_resolve` 最长前缀匹配 + 后端分派 + 启动自测 |
| `kernel/fs/tmpfs.c` | 内核内存文件系统后端（全功能） |
| `kernel/fs/devfs.c` | 字符设备文件系统后端（null/zero/urandom/console） |

### 2.2 改造文件

| 文件 | 改动 |
|------|------|
| `include/kernel/fd.h` | `fd_entry_t` 增加 `uint8_t vfs_backend` 字段（后端路由标记） |
| `kernel/fs/fd.c` | 所有 fd 入口（open/read/write/lseek/fstat/opendir/readdir/close/rename/chmod/utimes/stat/path_op）增加 VFS 路由分支 |
| `kernel/kmain.c` | FS_SERVER 授权后调用 `vfs_init()` 与 `vfs_selftest()` |

## 三、VFS 路由层设计（vfs.c / vfs.h）

### 3.1 后端枚举

```c
typedef enum {
    VFS_BACKEND_DISK   = 0,  /* 经 FS_PORT 的 FatFs 磁盘服务（既有路径） */
    VFS_BACKEND_TMPFS  = 1,  /* 内核内存 FS，挂载 /tmp /run */
    VFS_BACKEND_DEVFS  = 2   /* 字符设备 FS，挂载 /dev */
} vfs_backend_t;
```

对应 fd 层标记枚举（fd.h）：`FD_BACKEND_DISK / FD_BACKEND_TMPFS / FD_BACKEND_DEVFS`。

### 3.2 挂载表（编译期静态，运行期只读）

```c
static const vfs_mount_t g_vfs_mounts[] = {
    { "/tmp",  VFS_BACKEND_TMPFS, NULL },   /* 内存盘 */
    { "/run",  VFS_BACKEND_TMPFS, NULL },
    { "/dev",  VFS_BACKEND_DEVFS, NULL },
    { "/",     VFS_BACKEND_DISK,  NULL },    /* 兜底：磁盘 FatFs */
};
#define VFS_MOUNTS  (sizeof(g_vfs_mounts)/sizeof(g_vfs_mounts[0]))
```

注意顺序：`/tmp`/`/dev` 这种**前缀更长的**写在前面，`/` 写在最后作兜底。
`vfs_resolve` 按最长前缀匹配，避免 `/tmp/xxx` 被 `/` 错误路由到磁盘。

### 3.3 解析函数（最长前缀匹配）

```c
int vfs_resolve(const char *path, vfs_resolved_t *out)
{
    size_t best = 0;
    size_t best_len = 0;
    for (size_t i = 0; i < VFS_MOUNTS; i++) {
        const char *mp = g_vfs_mounts[i].mountpoint;
        size_t mlen = strlen(mp);
        if (strncmp(path, mp, mlen) == 0) {
            /* 精确命中（mp=="/" 或 path==mp）或其后紧跟 '/' */
            if (mlen == 1 || path[mlen] == '/' || path[mlen] == '\0') {
                if (mlen > best_len) { best = i; best_len = mlen; }
            }
        }
    }
    out->backend = g_vfs_mounts[best].backend;
    /* 计算相对路径：跳过挂载点前缀 */
    const char *mp = g_vfs_mounts[best].mountpoint;
    size_t mlen = strlen(mp);
    if (mlen == 1) { out->rel = path; }            /* "/" 根，rel 即全路径 */
    else { out->rel = path + mlen; }               /* 跳过 "/tmp" 等 */
    return 0;
}
```

例如 `vfs_resolve("/dev/zero", &vr)` → `backend=DEVFS, rel="zero"`；
`vfs_resolve("/tmp/a/b.txt")` → `backend=TMPFS, rel="/a/b.txt"`；
`vfs_resolve("/home/user/x")` → `backend=DISK, rel="/home/user/x"`。

### 3.4 内建后端分派

`vfs_builtin_open/read/write/close/stat/lseek/opendir/readdir/closedir/
unlink/rename/chmod/utime` 全部按 `vfs_backend_t` 分派到 tmpfs / devfs，
DISK 后端不在此分派（由 fd.c 走 FS_PORT 缓存路径）。

### 3.5 初始化与自测

```c
void vfs_init(void)
{
    tmpfs_init();
    devfs_init();
    kprintf("[vfs] initialized: /->DISK(FS_PORT) /tmp,/run->TMPFS /dev->DEVFS\n");
}
```

`vfs_selftest()` 覆盖 4 项：tmpfs 写读回环、/dev/zero 读零、/dev/null 写丢弃、
/dev 目录列举。

## 四、tmpfs 后端（tmpfs.c）

### 4.1 数据结构

```c
typedef struct tmpfs_node {
    bool          used;
    char          name[FS_PATH_MAX];
    uint8_t      *data;        /* 内存页缓冲（kmalloc，按需增长） */
    uint64_t      capacity;    /* 已分配容量 */
    uint64_t      size;        /* 当前文件大小 */
    uint32_t      mode;
    uint64_t      ino;
    bool          is_dir;
    /* 目录子项：扁平链表，仅保存子节点名 + 指向 node 索引 */
} tmpfs_node_t;

#define TMPFS_NODES 256
static tmpfs_node_t g_tmpfs_nodes[TMPFS_NODES];
static int          g_tmpfs_handles[TMPFS_MAX_HANDLES]; /* 句柄->node 索引 */
```

### 4.2 已实现的完整语义

- `tmpfs_open`（O_CREAT / O_RDWR / O_TRUNC / O_APPEND）
- `tmpfs_read` / `tmpfs_write`（自动 `kmalloc` 扩容，按 offset=-1 表追加/当前位）
- `tmpfs_lseek`（SEEK_SET/SEEK_CUR/SEEK_END）
- `tmpfs_fstat` / `tmpfs_stat`
- `tmpfs_mkdir` / `tmpfs_opendir` / `tmpfs_readdir` / `tmpfs_closedir`
- `tmpfs_unlink` / `tmpfs_rename` / `tmpfs_access` / `tmpfs_chmod` / `tmpfs_utime`

所有节点操作在单核下无需加锁（注册在 `vfs_init` 阶段完成、运行期结构稳定）；
句柄表用 `spinlock_init` 保护的 `g_tmpfs_lock`（name="tmpfs"），符合"锁内不阻塞"不变量。

## 五、devfs 后端（devfs.c）—— 关键修复点

### 5.1 内置设备（字符设备回调模型）

| 设备 | read | write |
|------|------|-------|
| `/dev/null` | 返回 0（EOF） | 丢弃，返回 len |
| `/dev/zero` | `memset(buf,0,len)` 返回 len | 丢弃，返回 len |
| `/dev/urandom` | 基于 TSC 的简易 PRNG（非密码学） | 丢弃 |
| `/dev/console` | 返回 0（EOF） | 逐字符 `kputc` 输出到内核控制台 |

### 5.2 ⚠️ 关键 Bug 与修复：devfs 句柄语义错误

**问题现象**：`vfs_selftest` 中 `/dev/zero read rc=-9 0 zero bytes`（`-9 = EBADF`）。

**根因分析**：`devfs_open` 原实现把句柄计算成**字节偏移 + 1**：

```c
/* 错误实现 */
*out_handle = (uint32_t)((uintptr_t)n - (uintptr_t)g_devfs_nodes + 1);
```

`g_devfs_nodes` 是 `devfs_node_t[64]` 数组，`n` 是第 1 个节点（idx=1）时，
字节偏移 = `sizeof(devfs_node_t) * 1`（约 88 字节），handle = 89。
但 `devfs_handle_node` 把 handle 当作**数组下标**反算：

```c
static devfs_node_t *devfs_handle_node(uint32_t h) {
    uintptr_t idx = (uintptr_t)h - 1;     /* 89-1 = 88 */
    if (idx >= DEVFS_NODES) return NULL;  /* 88 >= 64 -> 返回 NULL */
    ...
}
```

→ 反算 idx=88 越界，返回 `NULL` → `devfs_read` 返回 `-SUKI_EBADF (-9)`。

**为什么 /dev/null 之前能 PASS**：null 是第 0 个节点，字节偏移=0，handle=1，
反算 idx=0 恰好正确，掩盖了 bug；只有 idx>=1 的节点（zero/urandom/console）会失败。

**修复**（改为返回**数组下标 + 1**）：

```c
/* 正确实现 */
uintptr_t idx = (uintptr_t)(n - g_devfs_nodes);
*out_handle = (uint32_t)(idx + 1);
```

这样 `devfs_handle_node(h)` 用 `h-1` 反算即得到正确的数组下标，fd.c 的
`e->backend=(int)vh` 链路与 selftest 直调 `devfs_read` 完全一致。

## 六、fd.c 路由改造要点

在所有 fd 入口开头加 `vfs_resolve(path, &vr)`，按 `vr.backend` 分派：

- **DISK**：保持原 FS_PORT 缓存逻辑（`fd_open` 中 `e->vfs_backend=FD_BACKEND_DISK`）。
- **TMPFS / DEVFS**：调 `vfs_builtin_open`，把返回句柄存入 `e->backend`，
  并标记 `e->vfs_backend`。后续 `fd_read/fd_write/fd_lseek/fd_fstat/fd_readdir/
  fd_close_backend` 按 `e->vfs_backend` 分派到 `vfs_builtin_*`。

`fd.h` 新增字段：

```c
typedef struct fd_entry {
    ...
    int      type;          /* FD_TYPE_FILE / FD_TYPE_DIR */
    uint8_t  vfs_backend;   /* FD_BACKEND_DISK/TMPFS/DEVFS 路由标记 */
    int      backend;       /* DISK: FS_PORT fd；TMPFS: tmpfs 句柄；DEVFS: idx+1 */
    ...
} fd_entry_t;
```

`devfs` 关闭时无状态（`e->backend=-1` 即可），tmpfs 关闭调 `tmpfs_close`。

## 七、验证结果（QEMU 生产场景回归）

### 7.1 构建命令

```bash
cd /mnt/d/Projects/SukiOS && make iso
```

### 7.2 运行命令

```bash
timeout 45 qemu-system-x86_64 -machine pc -cpu qemu64 -smp 1 -m 2G \
  -no-shutdown -display none -serial file:/tmp/sukios_full.log \
  -boot d -cdrom build/SukiOS.iso \
  -drive file=build/disk.img,format=raw,index=0,media=disk
```

### 7.3 启动日志关键行（节选）

```
[tmpfs] initialized (memory filesystem, /tmp /run)
[devfs] initialized (/dev/null /dev/zero /dev/urandom /dev/console)
[vfs] initialized: /->DISK(FS_PORT) /tmp,/run->TMPFS /dev->DEVFS
[vfs] selftest begin
[vfs] selftest PASS: tmpfs write/read ('SukiOS tmpfs works')
[vfs] selftest PASS: /dev/zero read rc=0 16 zero bytes
[vfs] selftest PASS: /dev/null write 9 bytes
[vfs] selftest: /dev entries: null zero urandom console (4 total)
[vfs] selftest end
...
[disk-srv] serving DISK_PORT (kernel-resident, IPC only)
[fs] FS_SERVER starting
[fs] mounted FAT32 (FatFs)
[fs] self-test ALL PASS
[input] INPUT_SERVER online (Ring3 scancode parser)
[display] active: kernel console text now routed to display server
[shell] SukiOS shell online (Ring3, pid via IPC pipeline)
```

### 7.4 验证结论

- VFS 4 项自测 **全部 PASS**（修复前 /dev/zero 为 FAIL rc=-9）。
- 磁盘 FAT32 挂载 + FS self-test ALL PASS（DISK 后端兼容未破坏）。
- 全链路无 panic / triple fault / #GP / #PF / double fault。
- 各 Ring3 服务（fs/input/display/shell）正常上线。

## 八、后续可扩展方向（非本步范围，留作路线图）

- 更多 devfs 设备：`/dev/random`（熵池）、`/dev/klog`、`/dev/fb0`（帧缓冲）。
- tmpfs 与 swap 联动（内存紧张时换出）。
- VFS 层加权限/路径规范化（`.`/`..` 解析、符号链接）。

---

本步交付的 VFS 路由层已真正可工作并零 panic 通过生产回归。
