# SukiNative API v2.0 —— 完整系统接口规范

> **设计原则**：一切皆对象（Object），一切皆句柄（Handle），错误即返回值（Status）。
> **覆盖范围**：100% 替代 POSIX，同时保持与现有 SukiOS 内核及 Mach IPC 深度兼容。


## 一、核心类型与常量

```c
// include/sukios/types.h

/* 句柄 —— 所有内核资源的统一标识符 */
typedef uint64_t suki_handle_t;
#define SUKI_HANDLE_INVALID  ((suki_handle_t)-1)
#define SUKI_HANDLE_NULL     ((suki_handle_t)0)

/* 状态码 —— 所有 API 返回值（非负=成功，负=错误） */
typedef int64_t suki_status_t;

/* 核心状态码 */
#define SUKI_OK                    0
#define SUKI_ERR_GENERAL          -1
#define SUKI_ERR_NO_MEMORY        -2
#define SUKI_ERR_NOT_FOUND        -3
#define SUKI_ERR_ACCESS_DENIED    -4
#define SUKI_ERR_INVALID_HANDLE   -5
#define SUKI_ERR_INVALID_ARG      -6
#define SUKI_ERR_TIMEOUT          -7
#define SUKI_ERR_BUSY             -8
#define SUKI_ERR_EOF              -9
#define SUKI_ERR_ALREADY_EXISTS   -10
#define SUKI_ERR_IS_DIR           -11
#define SUKI_ERR_NOT_EMPTY        -12
#define SUKI_ERR_IN_USE           -13
#define SUKI_ERR_WOULD_BLOCK      -14
#define SUKI_ERR_INTERRUPTED      -15
#define SUKI_ERR_NOT_SUPPORTED    -16
#define SUKI_ERR_IS_SYSTEM        -17   /* 系统保留对象，不可销毁 */
#define SUKI_ERR_IS_DEBUG         -18   /* 调试角色未启用 */

/* 对象类型 */
typedef enum {
    SUKI_OBJ_TYPE_PROCESS,
    SUKI_OBJ_TYPE_THREAD,
    SUKI_OBJ_TYPE_FILE,
    SUKI_OBJ_TYPE_DIR,
    SUKI_OBJ_TYPE_PORT,          /* Mach 端口 */
    SUKI_OBJ_TYPE_EVENT,
    SUKI_OBJ_TYPE_MUTEX,
    SUKI_OBJ_TYPE_SEMAPHORE,
    SUKI_OBJ_TYPE_TIMER,
    SUKI_OBJ_TYPE_SOCKET,
    SUKI_OBJ_TYPE_PIPE,
    SUKI_OBJ_TYPE_SHARED_MEM,
    SUKI_OBJ_TYPE_DEVICE,
} suki_obj_type_t;

/* 访问模式 (用于文件/设备/端口) */
#define SUKI_ACCESS_READ          (1u << 0)
#define SUKI_ACCESS_WRITE         (1u << 1)
#define SUKI_ACCESS_EXEC          (1u << 2)
#define SUKI_ACCESS_SHARE_READ    (1u << 3)
#define SUKI_ACCESS_SHARE_WRITE   (1u << 4)
#define SUKI_ACCESS_CREATE        (1u << 5)
#define SUKI_ACCESS_TRUNCATE      (1u << 6)
#define SUKI_ACCESS_APPEND        (1u << 7)
#define SUKI_ACCESS_DIRECTORY     (1u << 8)

/* 等待标志 */
#define SUKI_WAIT_ANY             (0u)
#define SUKI_WAIT_ALL             (1u << 0)
#define SUKI_WAIT_ALERTABLE       (1u << 1)

/* 内存保护 (mmap) */
#define SUKI_MAP_READ             (1u << 0)
#define SUKI_MAP_WRITE            (1u << 1)
#define SUKI_MAP_EXEC             (1u << 2)
#define SUKI_MAP_SHARED           (1u << 3)
#define SUKI_MAP_PRIVATE          (1u << 4)
#define SUKI_MAP_ANON             (1u << 5)
#define SUKI_MAP_FIXED            (1u << 6)

/* 文件位置 */
#define SUKI_SEEK_SET             0
#define SUKI_SEEK_CUR             1
#define SUKI_SEEK_END             2

/* 进程退出 */
#define SUKI_EXIT_SUCCESS         0
#define SUKI_EXIT_FAILURE         1
```


## 二、生命周期管理

所有资源通过统一的创建/关闭接口管理：

```c
// include/sukios/lifecycle.h

/* 创建对象（通用工厂） */
suki_status_t suki_create(
    suki_obj_type_t   type,
    const void       *params,
    size_t            params_size,
    suki_handle_t    *out_handle
);

/* 销毁对象（通用析构） */
suki_status_t suki_destroy(suki_handle_t handle);

/* 复制句柄（用于进程间传递） */
suki_status_t suki_duplicate(
    suki_handle_t  src,
    suki_handle_t *out_dup
);

/* 查询对象信息 */
suki_status_t suki_query(
    suki_handle_t    handle,
    suki_obj_type_t *out_type,
    void            *out_info,
    size_t           info_size,
    size_t          *out_written
);
```


## 三、进程与线程

```c
// include/sukios/process.h

/* ---- 进程 ---- */
/* 创建进程（从可执行文件加载） */
suki_status_t suki_process_create(
    const char         *path,
    const char * const *argv,
    const char * const *env,
    uint32_t            flags,          /* 保留 */
    suki_handle_t      *out_proc
);

/* 替换当前进程映像（exec 风格） */
suki_status_t suki_process_exec(
    const char         *path,
    const char * const *argv,
    const char * const *env
);

/* 终止进程 */
suki_status_t suki_process_terminate(
    suki_handle_t proc,
    int           exit_code
);

/* 等待进程退出 */
suki_status_t suki_process_wait(
    suki_handle_t  proc,
    uint64_t       timeout_ms,
    int           *out_exit_code
);

/* 获取当前进程句柄 */
suki_handle_t suki_process_self(void);

/* 获取当前进程 ID */
uint64_t suki_process_get_id(suki_handle_t proc);

/* ---- 线程 ---- */
/* 创建线程（在当前进程中） */
suki_status_t suki_thread_create(
    void            (*entry)(void*),
    void             *arg,
    size_t            stack_size,
    suki_handle_t    *out_thread
);

/* 终止当前线程 */
suki_status_t suki_thread_exit(void *retval);

/* 等待线程退出 */
suki_status_t suki_thread_join(
    suki_handle_t thread,
    uint64_t      timeout_ms,
    void        **out_retval
);

/* 获取当前线程句柄 */
suki_handle_t suki_thread_self(void);

/* 主动让出 CPU */
suki_status_t suki_yield(void);
```


## 四、内存管理

```c
// include/sukios/memory.h

/* 分配虚拟内存（保留+提交） */
suki_status_t suki_mem_alloc(
    void         *hint,          /* 期望地址（可为 NULL） */
    size_t        size,
    uint32_t      protection,    /* SUKI_MAP_* */
    void        **out_addr
);

/* 释放虚拟内存 */
suki_status_t suki_mem_free(
    void   *addr,
    size_t  size
);

/* 修改内存保护属性 */
suki_status_t suki_mem_protect(
    void    *addr,
    size_t   size,
    uint32_t protection
);

/* 映射文件到内存（共享内存/文件映射） */
suki_status_t suki_mem_map(
    suki_handle_t  file,         /* 文件句柄，SUKI_HANDLE_NULL=匿名 */
    size_t         offset,
    size_t         size,
    uint32_t       protection,
    uint32_t       flags,        /* MAP_SHARED/MAP_PRIVATE/MAP_FIXED */
    void          *hint,
    void         **out_addr
);

/* 解除映射 */
suki_status_t suki_mem_unmap(
    void   *addr,
    size_t  size
);

/* 扩展堆（sbrk 风格） */
suki_status_t suki_mem_brk(
    void   *new_brk,
    void  **out_old_brk
);
```


## 五、文件与目录 I/O

```c
// include/sukios/file.h

/* 打开文件/设备 */
suki_status_t suki_file_open(
    const char    *path,
    uint32_t       access,       /* SUKI_ACCESS_* */
    uint32_t       mode,         /* 权限位（octal） */
    suki_handle_t *out_file
);

/* 关闭文件 */
suki_status_t suki_file_close(suki_handle_t file);

/* 读取文件 */
suki_status_t suki_file_read(
    suki_handle_t  file,
    void          *buffer,
    size_t         count,
    size_t        *out_read
);

/* 写入文件 */
suki_status_t suki_file_write(
    suki_handle_t  file,
    const void    *buffer,
    size_t         count,
    size_t        *out_written
);

/* 移动文件指针 */
suki_status_t suki_file_seek(
    suki_handle_t file,
    int64_t       offset,
    int           whence,        /* SUKI_SEEK_* */
    uint64_t     *out_pos
);

/* 获取文件大小 */
suki_status_t suki_file_size(
    suki_handle_t file,
    uint64_t     *out_size
);

/* 获取文件信息（元数据） */
suki_status_t suki_file_stat(
    suki_handle_t       file,
    struct suki_file_info *out_info
);

/* 截断文件 */
suki_status_t suki_file_truncate(
    suki_handle_t file,
    uint64_t      new_size
);

/* 同步到磁盘 */
suki_status_t suki_file_sync(suki_handle_t file);

/* ---- 目录 ---- */
/* 打开目录 */
suki_status_t suki_dir_open(
    const char    *path,
    suki_handle_t *out_dir
);

/* 读取目录项 */
suki_status_t suki_dir_read(
    suki_handle_t         dir,
    struct suki_dirent   *out_entry,
    uint64_t              index,       /* 从 0 开始 */
    uint64_t             *out_next     /* 下一个索引，-1 表示结束 */
);

/* 创建目录 */
suki_status_t suki_dir_create(
    const char *path,
    uint32_t    mode
);

/* 删除文件/空目录 */
suki_status_t suki_fs_remove(const char *path);

/* 重命名 */
suki_status_t suki_fs_rename(
    const char *old_path,
    const char *new_path
);

/* 改变当前工作目录 */
suki_status_t suki_fs_chdir(const char *path);

/* 获取当前工作目录 */
suki_status_t suki_fs_getcwd(
    char   *buffer,
    size_t  size
);
```


## 六、同步原语

```c
// include/sukios/sync.h

/* ---- 事件（Event，手动/自动复位）---- */
suki_status_t suki_event_create(
    bool           manual_reset,
    bool           initial_state,
    suki_handle_t *out_event
);

suki_status_t suki_event_set(suki_handle_t event);
suki_status_t suki_event_reset(suki_handle_t event);
suki_status_t suki_event_pulse(suki_handle_t event);  /* set + 唤醒 + reset */

/* ---- 互斥锁（Mutex，支持递归）---- */
suki_status_t suki_mutex_create(
    bool           recursive,
    suki_handle_t *out_mutex
);

suki_status_t suki_mutex_lock(suki_handle_t mutex);
suki_status_t suki_mutex_trylock(suki_handle_t mutex);
suki_status_t suki_mutex_unlock(suki_handle_t mutex);

/* ---- 信号量（Semaphore，计数）---- */
suki_status_t suki_sem_create(
    uint32_t       initial_count,
    uint32_t       max_count,
    suki_handle_t *out_sem
);

suki_status_t suki_sem_acquire(suki_handle_t sem, uint32_t count);
suki_status_t suki_sem_tryacquire(suki_handle_t sem);
suki_status_t suki_sem_release(suki_handle_t sem, uint32_t count);

/* ---- 条件变量（Condition Variable，需配合 Mutex）---- */
suki_status_t suki_cond_create(suki_handle_t *out_cond);
suki_status_t suki_cond_wait(
    suki_handle_t cond,
    suki_handle_t mutex,
    uint64_t      timeout_ms
);
suki_status_t suki_cond_signal(suki_handle_t cond);
suki_status_t suki_cond_broadcast(suki_handle_t cond);
```

**事件等待接口**（替代 `poll`/`select`/`epoll`）：

```c
// include/sukios/wait.h

/* 等待一个或多个对象变为"有信号"状态 */
suki_status_t suki_wait(
    const suki_handle_t *handles,
    size_t               count,
    uint32_t             flags,        /* SUKI_WAIT_ANY / SUKI_WAIT_ALL */
    uint64_t             timeout_ms,
    size_t              *out_index     /* 触发等待的对象索引（WAIT_ANY时） */
);

/* 等待单个对象（简化版） */
static inline suki_status_t suki_wait_one(
    suki_handle_t handle,
    uint64_t      timeout_ms
) {
    return suki_wait(&handle, 1, SUKI_WAIT_ANY, timeout_ms, NULL);
}
```


## 七、定时器与时间

```c
// include/sukios/time.h

/* ---- 定时器 ---- */
suki_status_t suki_timer_create(
    bool           periodic,
    uint64_t       initial_delay_ms,
    uint64_t       period_ms,
    suki_handle_t *out_timer
);

suki_status_t suki_timer_start(suki_handle_t timer);
suki_status_t suki_timer_stop(suki_handle_t timer);
suki_status_t suki_timer_arm(
    suki_handle_t timer,
    uint64_t      delay_ms,
    uint64_t      period_ms
);

/* ---- 时间查询 ---- */
/* 获取单调时间（纳秒，不受系统时间调整影响） */
suki_status_t suki_time_monotonic(uint64_t *out_ns);

/* 获取墙上时间（自 1970-01-01 UTC 起的纳秒） */
suki_status_t suki_time_realtime(uint64_t *out_ns);

/* 获取系统启动以来的时间（纳秒） */
suki_status_t suki_time_uptime(uint64_t *out_ns);

/* 休眠当前线程 */
suki_status_t suki_sleep(uint64_t ms);
```


## 八、网络（Socket）

```c
// include/sukios/net.h

/* ---- 套接字 ---- */
typedef enum {
    SUKI_AF_INET   = 2,
    SUKI_AF_INET6  = 10,
    SUKI_AF_UNIX   = 1,
} suki_addr_family_t;

typedef enum {
    SUKI_SOCK_STREAM   = 1,   /* TCP */
    SUKI_SOCK_DGRAM    = 2,   /* UDP */
    SUKI_SOCK_RAW      = 3,
} suki_sock_type_t;

typedef enum {
    SUKI_IPPROTO_TCP  = 6,
    SUKI_IPPROTO_UDP  = 17,
    SUKI_IPPROTO_RAW  = 255,
} suki_ip_proto_t;

/* 地址结构（简化版） */
typedef struct suki_sockaddr {
    uint16_t family;
    uint16_t port;
    uint32_t addr;          /* IPv4 地址 */
    uint8_t  pad[8];
} suki_sockaddr_t;

/* 创建套接字 */
suki_status_t suki_socket_create(
    suki_addr_family_t family,
    suki_sock_type_t   type,
    suki_ip_proto_t    protocol,
    suki_handle_t     *out_sock
);

/* 绑定地址 */
suki_status_t suki_socket_bind(
    suki_handle_t        sock,
    const suki_sockaddr_t *addr
);

/* 监听 */
suki_status_t suki_socket_listen(
    suki_handle_t sock,
    uint32_t      backlog
);

/* 接受连接 */
suki_status_t suki_socket_accept(
    suki_handle_t   sock,
    suki_sockaddr_t *out_addr,
    suki_handle_t  *out_client
);

/* 连接 */
suki_status_t suki_socket_connect(
    suki_handle_t        sock,
    const suki_sockaddr_t *addr,
    uint64_t              timeout_ms
);

/* 发送数据 */
suki_status_t suki_socket_send(
    suki_handle_t  sock,
    const void    *data,
    size_t         len,
    uint32_t       flags,
    size_t        *out_sent
);

/* 接收数据 */
suki_status_t suki_socket_recv(
    suki_handle_t  sock,
    void          *buffer,
    size_t         max_len,
    uint32_t       flags,
    size_t        *out_recv
);

/* 发送到（UDP） */
suki_status_t suki_socket_sendto(
    suki_handle_t        sock,
    const void          *data,
    size_t               len,
    const suki_sockaddr_t *addr,
    size_t              *out_sent
);

/* 从（UDP）接收 */
suki_status_t suki_socket_recvfrom(
    suki_handle_t   sock,
    void           *buffer,
    size_t          max_len,
    suki_sockaddr_t *out_addr,
    size_t         *out_recv
);

/* 关闭套接字 */
suki_status_t suki_socket_close(suki_handle_t sock);
```


## 九、管道与 IPC

```c
// include/sukios/pipe.h

/* ---- 管道（匿名，单向/双向） ---- */
suki_status_t suki_pipe_create(
    bool           bidirectional,
    suki_handle_t *out_read_end,
    suki_handle_t *out_write_end
);

suki_status_t suki_pipe_read(
    suki_handle_t pipe,
    void         *buffer,
    size_t        count,
    size_t       *out_read
);

suki_status_t suki_pipe_write(
    suki_handle_t pipe,
    const void   *buffer,
    size_t        count,
    size_t       *out_written
);

suki_status_t suki_pipe_close(suki_handle_t pipe);
```


## 十、Mach 端口（轻量级 IPC）

```c
// include/sukios/port.h

/* 端口是内核对象，因此也通过句柄操作 */
suki_status_t suki_port_create(suki_handle_t *out_port);
suki_status_t suki_port_destroy(suki_handle_t port);

/* 发送消息（阻塞/非阻塞） */
suki_status_t suki_port_send(
    suki_handle_t  port,
    const void    *msg,
    size_t         size,
    uint32_t       flags,        /* SUKI_MSG_NONBLOCK / SUKI_MSG_OOL */
    uint64_t       timeout_ms
);

/* 接收消息（阻塞/非阻塞） */
suki_status_t suki_port_recv(
    suki_handle_t  port,
    void          *buffer,
    size_t         max_size,
    uint32_t       flags,
    uint64_t       timeout_ms,
    size_t        *out_size
);

/* 为端口命名（服务发现） */
suki_status_t suki_port_name(
    suki_handle_t port,
    const char   *name
);

/* 按名称查找端口（返回句柄） */
suki_status_t suki_port_lookup(
    const char    *name,
    suki_handle_t *out_port
);
```


## 十一、设备与驱动

```c
// include/sukios/device.h

/* 打开设备（通过 /dev 路径或设备 ID） */
suki_status_t suki_device_open(
    const char    *path,
    uint32_t       access,
    suki_handle_t *out_dev
);

/* 设备控制（ioctl 风格） */
suki_status_t suki_device_control(
    suki_handle_t dev,
    uint32_t      request,
    void         *arg,
    size_t        arg_size
);

/* 读取设备数据 */
suki_status_t suki_device_read(
    suki_handle_t dev,
    void         *buffer,
    size_t        count,
    size_t       *out_read
);

/* 写入设备数据 */
suki_status_t suki_device_write(
    suki_handle_t  dev,
    const void    *buffer,
    size_t         count,
    size_t        *out_written
);
```


## 十二、环境与系统

```c
// include/sukios/system.h

/* ---- 环境变量 ---- */
suki_status_t suki_env_get(
    const char *name,
    char       *buffer,
    size_t      size,
    size_t     *out_len
);

suki_status_t suki_env_set(
    const char *name,
    const char *value,
    bool        overwrite
);

suki_status_t suki_env_unset(const char *name);

/* ---- 系统信息 ---- */
typedef struct suki_system_info {
    uint64_t    total_ram;
    uint64_t    free_ram;
    uint64_t    total_swap;
    uint64_t    free_swap;
    uint32_t    cpu_count;
    char        kernel_version[64];
    char        os_name[64];
    uint64_t    uptime_ns;
} suki_system_info_t;

suki_status_t suki_system_info(suki_system_info_t *out_info);

/* ---- 电源管理 ---- */
suki_status_t suki_system_reboot(void);
suki_status_t suki_system_poweroff(void);
```


## 十三、信号（事件通知，而非传统信号）

```c
// include/sukios/signal.h

/* SukiOS 的"信号"是事件对象，不中断系统调用 */
suki_status_t suki_signal_send(
    suki_handle_t target_process,
    uint32_t      signal_code,
    void         *data,
    size_t        data_size
);

/* 注册信号处理器（回调） */
suki_status_t suki_signal_handler(
    uint32_t signal_code,
    void    (*handler)(uint32_t code, void *data, size_t size)
);
```


## 十四、权限与安全

```c
// include/sukios/security.h

/* ---- 用户身份 ---- */
suki_status_t suki_user_get_uid(uint32_t *out_uid);
suki_status_t suki_user_get_euid(uint32_t *out_euid);
suki_status_t suki_user_get_gid(uint32_t *out_gid);

/* 切换身份（需授权） */
suki_status_t suki_user_set_uid(uint32_t uid);
suki_status_t suki_user_set_euid(uint32_t euid);
suki_status_t suki_user_set_gid(uint32_t gid);

/* ---- 授权请求（UAC 风格） ---- */
typedef enum {
    SUKI_AUTH_READ_FILE,
    SUKI_AUTH_WRITE_FILE,
    SUKI_AUTH_EXEC_FILE,
    SUKI_AUTH_INSTALL_PKG,
    SUKI_AUTH_MODIFY_SYSTEM,
    SUKI_AUTH_LOAD_DRIVER,
    SUKI_AUTH_SET_TIME,
    SUKI_AUTH_REBOOT,
    SUKI_AUTH_DEBUG,
} suki_auth_op_t;

suki_status_t suki_auth_request(
    suki_auth_op_t  op,
    const char     *resource,      /* 资源路径 */
    uint32_t        flags,
    uint64_t        timeout_ms
);

/* 检查是否已被授权（非阻塞） */
bool suki_auth_check(suki_auth_op_t op, const char *resource);

/* 撤销授权 */
suki_status_t suki_auth_revoke_all(void);
```


## 十五、POSIX → SukiNative 速查映射

| POSIX | SukiNative |
| :--- | :--- |
| `fork()` | `suki_process_create()` |
| `execve()` | `suki_process_exec()` |
| `exit()` | `suki_process_terminate(SUKI_HANDLE_NULL, code)` |
| `waitpid()` | `suki_process_wait()` |
| `getpid()` | `suki_process_id(suki_process_self())` |
| `open()` | `suki_file_open()` |
| `close()` | `suki_file_close()` |
| `read()` | `suki_file_read()` |
| `write()` | `suki_file_write()` |
| `lseek()` | `suki_file_seek()` |
| `stat()` | `suki_file_stat()` |
| `mkdir()` | `suki_dir_create()` |
| `rmdir()` | `suki_fs_remove()` |
| `opendir()` | `suki_dir_open()` |
| `readdir()` | `suki_dir_read()` |
| `mmap()` | `suki_mem_map()` |
| `munmap()` | `suki_mem_unmap()` |
| `sbrk()` | `suki_mem_brk()` |
| `pipe()` | `suki_pipe_create()` |
| `socket()` | `suki_socket_create()` |
| `bind()` | `suki_socket_bind()` |
| `connect()` | `suki_socket_connect()` |
| `accept()` | `suki_socket_accept()` |
| `send()`/`recv()` | `suki_socket_send()`/`suki_socket_recv()` |
| `poll()`/`select()` | `suki_wait()` |
| `pthread_create()` | `suki_thread_create()` |
| `pthread_join()` | `suki_thread_join()` |
| `pthread_mutex_*` | `suki_mutex_*` |
| `pthread_cond_*` | `suki_cond_*` |
| `sem_*` | `suki_sem_*` |
| `nanosleep()` | `suki_sleep()` |
| `clock_gettime()` | `suki_time_realtime()` |
| `getenv()`/`setenv()` | `suki_env_get()`/`suki_env_set()` |
| `errno` | **无**（状态码直接返回） |
| `signal()` | `suki_signal_handler()` |


## 十六、总结

| 特性 | POSIX | SukiNative |
| :--- | :--- | :--- |
| **错误处理** | 全局 `errno` | 直接返回状态码 |
| **资源标识** | `int fd`, `pid_t` | 统一 `suki_handle_t` |
| **对象模型** | 无统一模型 | 全部对象化 |
| **等待机制** | `poll`/`select` | 统一的 `suki_wait()`，支持任意对象 |
| **IPC** | `pipe`/`socket` | Mach 端口（原生） |
| **权限** | `uid/gid` | `suki_auth_request()` |
| **跨进程资源传递** | `SCM_RIGHTS` | `suki_duplicate()` |
| **内存管理** | `mmap`/`brk` 分离 | `suki_mem_alloc`/`suki_mem_map` 统一 |
| **学习曲线** | 40 年历史包袱 | 设计干净，仅 15 类 API |