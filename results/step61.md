# Step 61：SukiNative Phase 1 — 对象/句柄子系统 + 事件/互斥/信号量 + SYS_SUKI_WAIT

> 延续 step60 的迁移路线。Phase 0 已完成号位预留与分发路由（130..149 指向 `sys_suki_dispatch`）。
> 本步实现 SukiNative 的核心内核基础设施，使「一切皆对象」的原生 API 真正可用，并新增
> 用户态 `libsuki` 封装与单任务功能测试。**POSIX 兼容层（20..129）保持不变**，老程序不受影响。
> 验证：QEMU 生产场景零 panic，`POSIX test summary: PASS=173 FAIL=0`，SukiNative 全部用例 `[PASS]`。

---

## 一、设计总览

| 层 | 内容 |
|---|---|
| 号位（posix.h） | `130..149` 已分配给 SukiNative 原生对象 API（step60） |
| 内核入口 | `syscall_dispatch` 在 `num∈[130,199]` 时路由到 `sys_suki_dispatch`（`kernel/syscall/syscall.c`） |
| 核心实现 | `kernel/syscall/sys_suki.c` + `include/kernel/suki_native.h` |
| 抽象 | `suki_object_t`（一切皆对象）+ 每任务句柄表 `suki_handle_entry_t[]` |
| 同步原语 | 事件 / 互斥锁 / 信号量，统一经 `SYS_SUKI_WAIT` 多对象等待 |
| 用户态 | `user/lib/suki_native.c`（libsuki 封装）+ `user/lib/suki.h` 声明 |
| 测试 | `user/posixtest.c::test_sukinative()` |

**关键约束**：从用户态接收的指针（handles 数组、out_index、info）一律经
`copy_from_user`/`copy_to_user` 访问；对象状态变更在全局 `g_suki_lock` 保护下进行。

---

## 二、数据结构（include/kernel/suki_native.h）

```c
typedef enum { SUKI_OT_NONE=0, SUKI_OT_EVENT, SUKI_OT_MUTEX, SUKI_OT_SEM,
               SUKI_OT_FILE, SUKI_OT_PROC, SUKI_OT_MEM } suki_obj_type_t;

typedef struct suki_waitnode {        /* 任务等待节点：每对象一个，串入该对象 waiter 链 */
    struct task *task;
    struct suki_waitnode *next;
} suki_waitnode_t;

typedef struct suki_object {
    suki_obj_type_t type;
    uint32_t        refcount;       /* 初值 1 = 由首个句柄持有；句柄释放即 unref */
    suki_waitnode_t *waiters;       /* 等待在本对象上的任务链表头 */
    union {
        struct { bool signaled; bool manual_reset; } event;
        struct { bool locked; struct task *owner; }  mutex;
        struct { int  count; int  max; }             sem;
    } u;
} suki_object_t;

typedef struct suki_handle_entry {
    suki_object_t *obj;
    uint32_t       rights;          /* 预留权限位（Phase 1 仅 0=全权） */
    bool           occupied;
} suki_handle_entry_t;
```

任务结构新增字段（`include/kernel/task.h`，惰性分配、不继承）：
```c
    suki_handle_entry_t *suki_handles;   /* NULL=尚未建表（首次 SukiNative 调用时 kzalloc） */
    uint32_t            suki_handle_cap;
    suki_waitnode_t     suki_wait_nodes[SUKI_MAX_WAIT];  /* 等待时每对象一个节点 */
    int                 suki_wait_n;
    suki_object_t      *suki_wait_set[SUKI_MAX_WAIT];
    bool                suki_wait_active;
```

ABI 公共类型（内核/用户共享、单一真相源）定义在 `include/sukios/posix.h` G' 段：
- `suki_obj_type_t` 枚举、`suki_handle_t`(uint32_t)、`suki_objinfo_t`(type/state/count/_pad)；
- 等待标志 `SUKI_WAIT_ANY=0x1` / `SUKI_WAIT_ALL=0x2` / `SUKI_WAIT_NO_BLOCK=0x4`。

常量：`SUKI_MAX_HANDLES=256`、`SUKI_MAX_WAIT=16`。

---

## 三、系统调用语义（130..149）

| num | 名称 | 语义 |
|---|---|---|
| 130 | `SYS_SUKI_OBJ_CREATE`   | 通用工厂：type=a1，a2/a3 类型相关（事件:init,manual；信号量:initial,max），返回句柄 |
| 131 | `SYS_SUKI_OBJ_DESTROY`  | 释放句柄（unref，引用归零销毁对象） |
| 132 | `SYS_SUKI_OBJ_DUPLICATE`| 复制句柄（同一对象 +1 引用） |
| 133 | `SYS_SUKI_OBJ_QUERY`    | 复制 `suki_objinfo_t` 到用户（type/state/count） |
| 134 | `SYS_SUKI_WAIT`         | 多对象等待：handles=a1,count=a2,flags=a3,out_index=a5（a4 timeout 暂未实现） |
| 135 | `SYS_SUKI_EVENT_CREATE` | init_signaled=a1, manual_reset=a2 |
| 136 | `SYS_SUKI_EVENT_SET`    | 置 signaled，唤醒所有等待者 |
| 137 | `SYS_SUKI_EVENT_RESET`  | 清 signaled |
| 138 | `SYS_SUKI_MUTEX_CREATE` | 初始未锁定 |
| 139 | `SYS_SUKI_MUTEX_LOCK`   | 空闲即获取（owner=cur）；否则阻塞（自锁检测返回 -EDEADLK） |
| 140 | `SYS_SUKI_MUTEX_UNLOCK` | 解锁并唤醒 |
| 141 | `SYS_SUKI_SEM_CREATE`   | initial=a1, max=a2 |
| 142 | `SYS_SUKI_SEM_ACQUIRE`  | count>0 即减一返回；否则阻塞 |
| 143 | `SYS_SUKI_SEM_RELEASE`  | count++（封顶 max）并唤醒 |
| 144..149 | FILE/PROC/MEM | **Phase 2（按需）**，本阶段返回 -ENOSYS |

`SYS_SUKI_WAIT` 就绪判定（统一抽象）：
- 事件：signaled 即就绪；auto-reset 命中后自动清信号，manual-reset 不自动清；
- 互斥：未锁定即就绪；命中即加锁（owner=cur）；
- 信号量：count>0 即就绪；命中即 count--。

返回值：成功返回 0（或句柄号为正），失败返回负 `SUKI_E*`（EINVAL/EBADF/ENOSYS/EAGAIN/EMFILE/EFAULT/EDEADLK…）。

---

## 四、多对象等待算法（SYS_SUKI_WAIT 核心）

`suki_wait_objects()`（`sys_suki.c`）采用「持锁检查→入 waiter 链→释放锁→schedule」的
标准睡眠/唤醒协议（对照 `futex.c` 的 `futex_wait`），避免丢失唤醒：

```
持 g_suki_lock:
  loop:
    遍历 objs：找第一个就绪对象（WAIT_ANY）或检查全部就绪（WAIT_ALL）
    if 找到:
        消费就绪态（auto-reset 清信号 / mutex 加锁 / sem 减一）
        释放锁; *out_index=命中下标; return 0
    if NO_BLOCK: 释放锁; return -EAGAIN
    // 阻塞：把本任务挂入每个对象的 waiter 链（每对象一个节点）
    for i: cur->suki_wait_nodes[i] = {task=cur, next=objs[i]->waiters}; objs[i]->waiters = &node[i]
          cur->suki_wait_set[i] = objs[i]
    cur->state=BLOCKED; cur->in_rq=false; 释放锁
    schedule()
    // 被 suki_obj_wake_all 经 sched_wake 唤醒后从此返回
    持 g_suki_lock:
        从每个 suki_wait_set[i] 的 waiter 链摘除本任务节点
        cur->suki_wait_n=0; 回到 loop 重新评估（应对虚假唤醒/竞争）
```

`obj` 状态变更侧（SET/UNLOCK/RELEASE）：先持 `g_suki_lock` 改状态，**释放锁后**再调
`suki_obj_wake_all()`；`suki_obj_wake_all` 持锁收集等待任务、释放锁后逐个 `sched_wake()`。

**锁序（防死锁关键）**：绝不在持 `g_suki_lock` 时调用 `sched_wake`（否则与
`suki_handles_free_all` 的 `g_sched_lock→g_suki_lock` 形成反向锁序）。当前唯一跨锁序是
任务退出路径 `reap`：持 `g_sched_lock` → 调 `suki_handles_free_all` → 其内 `suki_obj_unref`
持 `g_suki_lock`（每对象单独取放，非嵌套），无反向路径，故无死锁。

---

## 五、用户态 libsuki（user/lib/suki_native.c + suki.h）

`__suki_syscallN`（`posix.h` 内 `static inline`）封装 `syscall` 指令。提供：
`suki_event_create/set/reset`、`suki_mutex_create/lock/unlock`、`suki_sem_create/acquire/release`、
`suki_wait`、`suki_obj_destroy/duplicate/query`、`suki_obj_create`。
状态码 `suki_status_t=int64_t`（0 成功，<0 为 `SUKI_E*`）。`suki.h` 已加入这些声明；
Makefile 的 `USER_LIB_OBJS` 新增 `user/lib/suki_native.c.o` 纳入编译。

---

## 六、改动文件清单

- `include/sukios/posix.h`：G' 段新增 SukiNative ABI 公共类型（枚举/句柄/标志/查询结构）。
- `include/kernel/suki_native.h`（**新建**）：对象/句柄/等待节点定义与生命周期原型。
- `include/kernel/task.h`：include suki_native.h；task_t 增加句柄表与等待态字段。
- `kernel/syscall/sys_suki.c`（**重写**）：完整实现 130..149 分发与各 handler。
- `kernel/syscall/syscall.c`：删除残留 `pending_kill` 死代码时的未用 `cur`（上步引入）；
  130..199 路由已在 step60 加入。
- `kernel/sched/sched.c`：`reap_dead` 与 `task_reap_locked` 回收任务前调用
  `suki_handles_free_all(t)` 释放句柄表并 unref 对象（防泄漏）。
- `user/lib/suki.h` / `user/lib/suki_native.c`（**新建**）：libsuki 封装。
- `Makefile`：`USER_LIB_OBJS` 加入 `suki_native.c.o`。
- `user/posixtest.c`：新增 `test_sukinative()` 并自 `main` 调用。

---

## 七、验证（仅 QEMU 自带机制）

构建：`make iso` —— `build/kernel.ski` 链接成功，`sys_suki.c.o` / `suki_native.c.o` 正常编译。

运行：`make run-headless QEMU_SERIAL="-serial file:/tmp/qemu.serial.log" RUN_TIMEOUT=60`，
随后 `cat/tail/grep` 串口日志：

- **零 panic / 零 #GP / 零 triple fault**（健康 grep 仅命中 IDT 加载信息中的 `#PF` 子串，非异常）。
- **SukiNative 全部 `[PASS]`**：
  - 事件 auto-reset：未触发 wait 非阻塞→EAGAIN；set 后 wait→index 0；消费后再 wait→EAGAIN；
  - 事件 manual-reset：触发后多次 wait 命中、reset 后停止；
  - 多对象 ANY：仅触发第二个→返回 index 1；
  - 互斥：空闲 lock→0、unlock→0；
  - 信号量：create(2,5)/acquire/release 后 query count==2；
  - 句柄 duplicate + 多次 destroy（引用计数）正常。
- `libc-test PASS=8`、`POSIX test summary: PASS=173 FAIL=0`（较 step59 的 144 新增约 29 项
  为 SukiNative 用例，全部通过）。

---

## 八、下一步（Phase 2，未在本步实施）

1. `SYS_SUKI_FILE_OPEN/READ/WRITE/CLOSE`、`SYS_SUKI_PROC_CREATE`、`SYS_SUKI_MEM_ALLOC`：
   文件/进程/内存对象（耦合 FS/VFS，需单独评估，避免触碰已稳定路径）。
2. 跨任务阻塞测试：让任务 A 等待某事件，任务 B（或定时器）set 后验证 A 被唤醒并消费，
   以端到端验证 `sched_wake` 多核/单核唤醒路径（本步仅单任务功能验证）。
3. 超时支持：`SYS_SUKI_WAIT` 的 a4 timeout_ms（当前忽略，阻塞至被唤醒）。
