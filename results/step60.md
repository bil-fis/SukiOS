# Step 60：SukiNative 迁移可行性评估 + Phase 0（号位预留与分发路由）

> 本步完成两件事：(1) 评估把 POSIX 作为 SukiNative 上层分发、向 Linux/Unix 生态兼容的可行性；
> (2) 落地 **Phase 0**——在 `posix.h` 预留 SukiNative 系统调用号位、新增 `sys_suki_dispatch`
> 分发路由、并清理上一阶段遗留的 `pending_kill` 死代码。所有改动**不改变任何现有 0–129
> 行为**，QEMU 生产场景回归零 panic。

---

## 一、可行性评估结论

把 POSIX（20–129）作为 SukiNative 上层、用于兼容 Linux/Unix 生态，**可行**，但工作量分三层、风险截然不同：

| 工作 | 风险 | 说明 |
|---|---|---|
| (a) 号位预留 + 分发路由（Phase 0） | **零** | 只动 `posix.h` 号定义 + `syscall.c` 加 `sys_suki_dispatch` 路由（未实现返回 `-ENOSYS`），不动任何现有 0–129 行为 |
| (b) SukiNative 对象/句柄子系统 + `SYS_SUKI_WAIT` + 事件/互斥/信号量 | **中** | 全新内核基础设施；需句柄表、对象注册表、把对象等待接入现有 `sched_wait/sched_wake` |
| (c) 把现有 POSIX 重写为 SukiNative 上层 | **高** | 会触碰已稳定（PASS=144、FS_SERVER 正常）的 FS/VFS 关键路径，极易引发 P0 生产就绪度虚高 |

**推荐架构（4 层分发）**：

```
syscall_dispatch:
  num <= 19     -> sys_mach_dispatch    # Mach 原生/端口
  num 20..129   -> posix_dispatch       # POSIX 兼容层（对外行为不变）
  num 130..199  -> sys_suki_dispatch    # SukiNative 对象 API（新）
  num >= 200    -> 内核扩展（下方显式 case）
```

**关键原则**：POSIX 层对外号位与行为保持不变；SukiNative 是**平行于 POSIX 的新原生 API**，
新服务可直接用，老程序（Servo 等只调 20–129）不受影响。

**现状核对**（基于真实代码，非设想）：
- 真实 `syscall_dispatch`（`kernel/syscall/syscall.c`）此前对 `130–199`/`200+` 全部 fall-through
  到 `posix_dispatch` 默认 `-ENOSYS`，**并不存在 `sys_ext_dispatch`**；所谓「网络预留 130–149」
  与「扩展 200–210」只是未用的空号。因此把它们重新分配给 SukiNative **零行为影响、零回归风险**。
- 内核**没有任何「句柄/对象表」抽象**（`handle_table|object_table|suki_handle_t|obj_type`
  等搜索零命中），SukiNative 的「一切皆句柄」需从零搭建——这是 (b) 层的核心工程量。
- `pending_kill` 边界检查当时已成死代码：`task_signal` 已改用 `pending` 位 + `sig_deliver_check`
  在返回路径处理默认终止，无任何写入方设置 `pending_kill`。本步一并清理。

---

## 二、Phase 0 具体改动

### 1. `include/sukios/posix.h`：号位重分配

将原「G. 网络（130..149）」的 15 个 socket 宏**重定位到 150..164**（新增「H. 网络」段），
腾出的 **130..149** 改为 **SukiNative 原生对象 API** 号段：

```c
/* ---- G. SukiNative 原生对象 API（130..149）---- */
#define SYS_SUKI_OBJ_CREATE     130
#define SYS_SUKI_OBJ_DESTROY    131
#define SYS_SUKI_OBJ_DUPLICATE  132
#define SYS_SUKI_OBJ_QUERY      133
#define SYS_SUKI_WAIT           134
#define SYS_SUKI_EVENT_CREATE   135
#define SYS_SUKI_EVENT_SET      136
#define SYS_SUKI_EVENT_RESET    137
#define SYS_SUKI_MUTEX_CREATE   138
#define SYS_SUKI_MUTEX_LOCK     139
#define SYS_SUKI_MUTEX_UNLOCK   140
#define SYS_SUKI_SEM_CREATE     141
#define SYS_SUKI_SEM_ACQUIRE    142
#define SYS_SUKI_SEM_RELEASE    143
#define SYS_SUKI_FILE_OPEN      144
#define SYS_SUKI_FILE_READ      145
#define SYS_SUKI_FILE_WRITE     146
#define SYS_SUKI_FILE_CLOSE     147
#define SYS_SUKI_PROC_CREATE    148
#define SYS_SUKI_MEM_ALLOC      149

/* ---- H. 网络（150..169）：socket 号位预留（协议栈未就绪，调用返回 -ENOSYS）---- */
#define SYS_SOCKET  150
#define SYS_BIND    151
...（151..164 依次对应原 130..144 的 15 个 socket 调用）
#define SYS_SOCKETPAIR  164
```

> 注：原 posix.h 在 socket 段之后还有一段「SukiNative 原生对象 API（130..199）」的注释块，
> 现已被真实定义取代，语义一致。

### 2. `include/kernel/syscall.h`：声明 `sys_suki_dispatch`

在 `syscall_dispatch` 声明之后新增（第 76 行起）：

```c
uint64_t sys_suki_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6);
```

### 3. `kernel/syscall/sys_suki.c`（新建）：SukiNative 分发入口

- 由 `C_SRCS := $(shell find kernel -name '*.c')`（`Makefile:123`）自动纳入构建，无需改 Makefile。
- 当前 Phase 0 仅做**路由骨架**：所有 `SYS_SUKI_*` 号统一返回 `-SUKI_ENOSYS`（=38，等价于
  POSIX 的 `-ENOSYS`；内核无 `ENOSYS` 宏，用户态 `errno.h` 里 `ENOSYS` 即定义为 `SUKI_ENOSYS`）。
- 文件头注释明确：Phase 1 起在此实现 `suki_object_t`、每进程句柄表、`SYS_SUKI_WAIT`
  多对象等待、事件/互斥/信号量等；从用户态接收的指针必须用 `copy_from_user/copy_to_user` 访问。

### 4. `kernel/syscall/syscall.c`：删除死代码 + 新增路由

- 删除 `syscall_dispatch` 开头已失效的信号边界块：
  ```c
  /* 信号边界：有待处理信号且默认动作为终止 -> 自我终止（128+signo） */
  if (cur->pending_kill) {
      int signo = cur->pending_signo;
      cur->pending_kill = false;
      cur->pending_signo = 0;
      task_exit_current((uint64_t)(128 + signo));
  }
  ```
  改为在调用具体 handler 前插入 SukiNative 路由（覆盖 130..199，包含已重定位的 socket 号）：
  ```c
  if (num >= 130 && num <= 199) {
      return sys_suki_dispatch(num, a1, a2, a3, a4, a5, a6);
  }
  ```
- 同步更新函数顶部「信号边界」文档注释：信号投递统一在 `posix_dispatch` 返回路径经
  `sig_deliver_check()` 处理，本函数不再处理 `pending_kill`。

### 5. `include/kernel/task.h`：移除废弃字段

删除 `task_t` 中的 `bool pending_kill;` 与 `int pending_signo;` 及其注释（信号已改用
`pending` 位 + `sig_deliver_check`）。

### 6. `kernel/syscall/sys_posix.c`：删除两处初始化

删除 `sys_fork`/`sys_clone` 路径里对 `child->pending_kill = false;` / `child->pending_signo = 0;`
的两处初始化（`replace_all`，共 2 处）。

---

## 三、关键技术细节

- **号段语义**：`0–19` Mach 原生、`20–129` POSIX（不变）、`130–149` SukiNative 对象 API、
  `150–164` socket 预留、`200+` 内核扩展（FRAMEBUFFER_MAP 等）。
- **返回值约定**：Phase 0 的 SukiNative 未实现号返回 `-SUKI_ENOSYS`（38），与 POSIX 的
  `-ENOSYS` 数值一致，用户态 `ck(..., sk == -SUKI_ENOSYS)` 判定通过。
- **行尾注意**：`syscall.c`/`task.h` 为 CRLF，其余为 LF；`replace_in_file` 已自动归一化，编辑成功。
- **`ENOSYS` 宏缺失**：内核源码中没有 `ENOSYS` 宏定义（仅用户态 `user/lib/shims/errno.h`
  有 `#define ENOSYS SUKI_ENOSYS`），故内核侧一律用 `SUKI_ENOSYS`。此点已记入代码注释。

---

## 四、验证（仅 QEMU 自带机制）

构建：`make iso` —— 链接 `build/kernel.ski` 成功，`sys_suki.c.o` 正常编译进内核。

运行：`make run-headless QEMU_SERIAL="-serial file:/tmp/qemu.serial.log" RUN_TIMEOUT=60`，
随后 `cat/tail/grep` 串口日志：

- **零 panic / 零 #GP / 零 triple fault**（健康 grep 仅命中 IDT 加载信息中的 `#PF` 子串，非异常）。
- `libc-test PASS=8 FAIL=0  ALL OK`
- `POSIX test summary: PASS=144 FAIL=0`
- `[PASS] socket returns -ENOSYS (stack not yet)` —— 验证 **socket 号重定位到 150 后**
  仍经 `sys_suki_dispatch` 返回 `-SUKI_ENOSYS`，断言通过；证明号位迁移与路由零回归。

---

## 五、下一步（Phase 1 候选，未在本步实施）

1. 在 `sys_suki.c` 实现对象/句柄表：`suki_object_t`（类型枚举 + 引用计数 + ops 表 + 私有状态）、
   每进程句柄表 `suki_handle_t` → 对象指针 + 权限位（可桥接现有 `kernel_port_t` 端口表思想）。
2. 实现 `SYS_SUKI_EVENT_*` / `SYS_SUKI_MUTEX_*` / `SYS_SUKI_SEM_*` 与核心 `SYS_SUKI_WAIT`
   （多对象等待需把对象等待集合接入 `sched_wait/sched_wake`）。
3. 用户态 `user/lib/libsuki.c` 封装 `__suki_syscallN`。
4. **保持 POSIX（20–129）独立稳定**，不强行重写为 SukiNative 上层，避免触碰已稳定的 FS 路径。
   仅在后续单独评估、确认无回归风险时，才考虑让 POSIX fd 映射到 SukiNative handle。
