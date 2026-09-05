# Step 62 — SukiNative Phase 2：文件/进程/内存对象（号位 144–149）落地与验证

> 阶段目标：在保持 POSIX 兼容层（号位 20–129）完全不受影响的前提下，为 SukiNative
> 原生对象 API（号位 130–149）补齐 **Phase 2** 的三类对象子系统：
> - **FILE 对象**：`SYS_SUKI_FILE_OPEN / FILE_READ / FILE_WRITE / FILE_CLOSE`（144–147）
> - **PROC 对象**：`SYS_SUKI_PROC_CREATE`（148）
> - **MEM 对象**：`SYS_SUKI_MEM_ALLOC`（149）
>
> 全部桥接既有 VFS / elf_load / vma 子系统，复用 `fd_open/fd_read/fd_write/fd_close`、
> `task_create_user_args`、`vma_insert/vma_unmap_range`，**不触碰 POSIX 稳定路径**。
>
> 结果：**QEMU 生产场景回归零 panic；SukiNative Phase 2 全部 PASS；POSIX 一致性测试
> PASS=192 FAIL=0**（相较 Phase 1 结束时的 188/4，新增的 4 个失败项即 Phase 2 PROC
> 4 项，现已全部修复归零）。

---

## 1. 系统调用号位与分发路由

| 号位 | 名称 | 对象类型 | 说明 |
|------|------|----------|------|
| 144 | `SYS_SUKI_FILE_OPEN` | `SUKI_OT_FILE` | 打开文件，返回 FILE 句柄 |
| 145 | `SYS_SUKI_FILE_READ` | `SUKI_OT_FILE` | 从句柄读 |
| 146 | `SYS_SUKI_FILE_WRITE` | `SUKI_OT_FILE` | 向句柄写 |
| 147 | `SYS_SUKI_FILE_CLOSE` | `SUKI_OT_FILE` | 关闭并销毁 FILE 对象 |
| 148 | `SYS_SUKI_PROC_CREATE` | `SUKI_OT_PROC` | 创建子进程，返回 PROC 监控句柄 |
| 149 | `SYS_SUKI_MEM_ALLOC` | `SUKI_OT_MEM` | 分配用户态内存对象，返回 MEM 句柄 |

分发位置：`kernel/syscall/sys_suki.c` 的 dispatcher 末尾（原 537–543 区段），
六个 case 分别调用 `sys_suki_file_open/read/write/close`、`sys_suki_proc_create`、
`sys_suki_mem_alloc`。这些 handler 的实现集中在 `sys_suki.c` 文件末尾。

---

## 2. 数据结构改动

### 2.1 `suki_object_t` 类型联合扩展（`include/kernel/suki_native.h`）
在既有 `SUKI_OT_EVENT/MUTEX/SEM` 基础上新增三套字段：

```c
typedef struct suki_object {
    uint32_t        type;        /* SUKI_OT_FILE/PROC/MEM 之一 */
    uint32_t        refcount;
    struct suki_waiter *waiters; /* SYS_SUKI_WAIT 阻塞队列（Phase 1 已有） */
    struct suki_object *next_notify; /* PROC 退出通知链（挂入子任务 task_t.suki_exit_notify） */
    union {
        /* ... 既有 event/mutex/sem 字段 ... */
        struct { int fd; bool owned_by_suki; } file;   /* 复用 per-task fd，suki_owned 标记避免双重关闭 */
        struct { task_t *child; uint64_t pid; bool exited; int exit_code; } proc;
        struct { uint64_t uaddr; uint64_t size; bool owned_by_suki; } mem; /* 用户态地址+字节数 */
    } u;
} suki_object_t;
```

### 2.2 `task_t` 新增退出通知链头（`include/kernel/task.h`）
```c
struct suki_object *suki_exit_notify; /* 该任务作为子进程被 PROC 对象监控时挂入的链表头 */
```
仅在任务被 `SYS_SUKI_PROC_CREATE` 创建、且调用方持有 PROC 对象时挂链；普通任务该
字段为 `NULL`，`suki_proc_notify_exit` 直接跳过，零开销。

### 2.3 `fd_entry_t` 新增 `suki_owned` 标记（`include/kernel/fd.h`）
```c
bool suki_owned;  /* 由 SukiNative FILE_OPEN 打开的 fd 置 true */
```
`kernel/fs/fd.c::fd_exit_task` 释放 fd 表时，对 `suki_owned==true` 的槽**跳过**（不清
`fds`、不降引用、不释放），改由持有它的 SukiNative FILE 对象在引用归零（显式 CLOSE
或句柄表随任务退出回收）时统一调 `fd_close`，避免“任务退出路径”与“对象销毁路径”
对同一 fd 双重关闭。

### 2.4 用户态句柄信息扩展（`include/sukios/posix.h`）
```c
typedef struct suki_objinfo {
    uint32_t type;
    uint64_t base;   /* MEM 对象：用户态地址；PROC 对象：子进程 pid */
    uint64_t size;   /* MEM 对象：字节数 */
    /* ... */
} suki_objinfo_t;
```
`sys_suki_obj_query` 现已回填 `base/size`（MEM 返回 `u.uaddr/u.size`，PROC 返回
`pid`，FILE 返回 0）。

---

## 3. 各对象实现要点

### 3.1 FILE 对象（144–147）
- `sys_suki_file_open`：经 `copy_str_from_user` 取路径（与 POSIX `fetch_path` 一致，
  避免越界/反向判断），调 `fd_open(t,path,O_RDONLY,0)` 取得 per-task fd，封装为
  `SUKI_OT_FILE` 对象，`fd` 存入 `u.file.fd`，并置 `fd_entry_t.suki_owned=true`。
- `sys_suki_file_read/write`：经 `suki_handle_lookup` 取对象 → 校验类型 →
  `fd_read/fd_write`，返回字节数经 `copy_to_user` 写回 `out_n`。
- `sys_suki_file_close`：置 `u.file.fd=-1`（防对象销毁时重复关闭）后 `fd_close`，
  再 `suki_handle_free` 回收句柄。对象引用归零的 `suki_obj_free` 中若 `fd>=0` 也会
  兜底 `fd_close`。
- **W^X / 红线**：FILE 不映射执行权限，完全走既有 VFS 只读/只写路径。

### 3.2 MEM 对象（149）
- `sys_suki_mem_alloc`：先 `vma_find_free(t, VMA_MMAP_BASE, VMA_MMAP_TOP, aligned)`
  在用户态找一段空闲区间（与 `sys_mmap` 同一分配器），再 `vma_insert(...,
  VMA_TYPE_ANON, PTE_USER|PTE_WRITE|PTE_NX)` 按需零填充映射。
- 校验：成功时 `vma_find_free` 返回非零用户地址；失败（地址 0）返回 `-SUKI_ENOMEM`。
- 销毁：`suki_obj_free` 中对 `SUKI_OT_MEM` 调 `vma_unmap_range(t, uaddr, uaddr+size)`
  （仅当 owner 任务未退出时，避免解映射已释放地址空间）。
- **安全**：`PTE_NX|PTE_WRITE` 即 W^X，内存对象不可执行，杜绝由 SukiNative 分配的
  用户内存被当作代码执行。

### 3.3 PROC 对象（148）
- `sys_suki_proc_create`：
  1. `copy_str_from_user` 取路径；
  2. `fd_open` + **`fd_read_kern`**（见 §4.3）把 ELF 全文读进内核 `blob`（上限 8 MiB）；
  3. `task_create_user_args(blob, total, argc, argv, 0, NULL, path)` 创建子进程——内部
     `elf_load` 安全拷贝 argv（`argc=0/argv=NULL` 时为空进程）；
  4. 构造 `SUKI_OT_PROC` 对象，记录 `child/pid/exited=0/exit_code`；
  5. **持 `g_suki_lock`** 把对象挂入 `child->suki_exit_notify` 链（竞态兜底：若挂链前
     子进程已退出则立即置 `exited=true`）；
  6. `suki_handle_alloc` 返回句柄。
- 退出通知：`kernel/sched/sched.c::task_exit_current` 在释放 `g_sched_lock` 后、
  `cr3_switch` 前注入 `suki_proc_notify_exit(t)`；该函数遍历
  `dead->suki_exit_notify` 链，逐一对 PROC 对象置 `exited=true` 并 `suki_obj_wake_all`
  唤醒阻塞在 `SYS_SUKI_WAIT` 上的监控者。
- **锁序铁律（沿用 Phase 1）**：`g_suki_lock` 内**绝不**调用 `sched_wake`；
  `suki_proc_notify_exit` 持锁仅做“收集等待者 + 标记 exited”，释放锁后在调用方上下文
  `sched_wake`，彻底避免持锁唤醒导致的重入/死锁。

---

## 4. 调试中定位并修复的四个根因 bug（关键价值）

Phase 2 初版在 QEMU 生产场景全 FAIL，逐一用 serial 日志（`qemu -serial file:/tmp/suki.log`）
定位，共修复 4 个根因：

### 4.1 路径拷贝反向判断 + 越界读取（FILE_OPEN / PROC_CREATE）
原代码：
```c
if (copy_from_user(path_buf, (const void*)a1_path, 512) != 0) return -EFAULT;
```
`copy_from_user` 语义为“成功返回 n（=512），失败返回 0”，`!=0` 判断**完全反向**——
成功时被误判 `EFAULT` 提前返回，正是 `file_open` 诊断行从不打印的原因；且固定读 512
字节会越界。改为 `copy_str_from_user(path_buf, a1_path, 512)`，返回 `<0` 才失败，且
按 NUL 终止安全拷贝。

### 4.2 句柄经 `copy_to_user(out_handle)` 返回但用户态封装覆盖为 0（FILE/PROC/MEM）
初版 handler 用 `copy_to_user(out_handle, &h, 8)` 把句柄写入用户 `*out` 并返回 0；而
用户态封装 `suki_mem_alloc` 等是 `int64_t h = __suki_syscallN(...); *out=(suki_handle_t)h;`——
成功时内核 `rax=0`，封装把 `*out` **覆盖成 0**，句柄丢失 → `mh=0` → `mp=0` → 写内存
触发 #PF 杀任务。与 **Phase 1 已有约定**（句柄经 `%rax` 返回，`*out=h` 恰好正确）对齐：
三个 handler 统一改为 `return (uint64_t)h;`，移除 `copy_to_user(out_handle,...)`，用户态
封装无需改动。

### 4.3 引用计数提前释放（FILE/PROC/MEM）
初版在 `suki_handle_alloc(o,...)` **之后无条件** `suki_obj_unref(o)`，把 `suki_obj_new`
建立的唯一初始引用（ref 1→0）直接释放，对象被提前 free，句柄表指向野对象；后续
`file_read/query/close` 拿到垃圾 `type/fd` → #PF（任务 code=139）。与 Phase 1 的
`OBJ_CREATE/EVENT_CREATE/...` 对齐：`suki_handle_alloc` **不增引用**，成功路径不再
unref，仅分配失败时 `suki_obj_unref(o)`。

### 4.4 `fd_read` 经 `copy_to_user` 写内核 blob 失败（PROC_CREATE 的根因）
`kernel/fs/fd.c::fd_read` 对所有 backend 都用 `copy_to_user(ubuf, data, n)` 把数据写回
【用户虚拟地址】（syscall 语义要求）。`proc_create` 初版把 ELF 读进【内核 blob 指针】，
`copy_to_user` 的内核地址校验失败返回 `-EFAULT` → 读取循环 `n<=0` 立即 break →
`total=0` → blob 为空 → `elf_load` 报 `elf_load FAILED` → PROC 4 项全 FAIL。

**修复**：新增内核态读原语 `fd_read_kern(t, fd, kbuf, count)`（`include/kernel/fd.h`
声明 + `kernel/fs/fd.c` 实现），镜像 `fd_read` 的 DISK/TMPFS/DEVFS 分支，但用
`memcpy(kbuf, data, n)` 直接写入内核缓冲，绕开用户指针校验；TTY/PIPE/DIR 返回
`-EINVAL`（非合法装载源）。`proc_create` 改用 `fd_read_kern` 后，ELF 正确载入内核，
`task_create_user_args` → `elf_load` 成功创建子进程。

> 该原语也作为内核自身“读文件进内核空间”的通用基础设施，未来可被 exec/loader 等复用。

---

## 5. 用户态封装与测试（`user/lib/suki.h` / `user/lib/suki_native.c` / `user/posixtest.c`）

- `suki.h` 新增 `suki_file_open/read/write/close`、`suki_proc_create`、`suki_mem_alloc`
  声明；`suki_native.c` 用 `__suki_syscall4` 等封装（号位 144–149）。
- `posixtest.c::test_sukinative` 在 `wait_fs_ready()` 之后运行（FILE/PROC 依赖 FS 就绪），
  新增 Phase 2 用例：
  - **FILE**：`suki_file_open("/README.TXT",RDONLY)` → `suki_file_read` → `suki_obj_query`
    → `suki_file_close`；再 `suki_file_open(creat)` → `suki_file_write` → `reopen` →
    `read(back)` → `file content match` → 关闭。
  - **MEM**：`suki_mem_alloc(4096)` → `suki_obj_query(mem)` 校验 `base/size` → 写入
    `0xAB`/`0xCD` 并读回校验 → `suki_obj_destroy(mh)`。
  - **PROC**：`suki_proc_create("/BIN/HELLO.SKA",0,NULL,&ph)` → `suki_obj_query(proc)`
    → `suki_wait` **阻塞**等待子进程退出（HELLO.SKA 调 `sys_exit`）→ 由
    `suki_proc_notify_exit` 唤醒 → `suki_obj_destroy(ph)`。

---

## 6. 验证方式与结果

### 6.1 构建
```
make iso      # 生成 build/kernel.ski + build/SukiOS.iso（含 /BIN/HELLO.SKA）
```
编译零错误（仅有既有的、与本次无关的警告）。

### 6.2 运行（遵循项目调试铁律：仅 bash + QEMU 自带机制，禁 GDB/外部脚本）
```
timeout 60 qemu-system-x86_64 -machine pc -cpu qemu64 -smp 1 -m 2G \
  -no-shutdown -display none -serial file:/tmp/suki.log \
  -boot d -cdrom build/SukiOS.iso build/disk.img
```
运行后用 `grep -aE` 从 `/tmp/suki.log` 读取 SukiNative 段与总结。

### 6.3 实测结果（节选自 serial 日志）
```
--- sukinative file object ---
  [PASS] suki_file_open(/README.TXT, RDONLY)
  [PASS] suki_file_read
  [PASS] suki_obj_query(file)
  [PASS] suki_file_close(rf)
  [PASS] suki_file_open(creat)
  [PASS] suki_file_write
  [PASS] suki_file_open(reopen)
  [PASS] suki_file_read(back)
  [PASS] file content match
  [PASS] suki_file_close(wf)
  [PASS] suki_file_close(rf2)
--- sukinative mem object ---
  [PASS] suki_mem_alloc(4096)
  [PASS] suki_obj_query(mem)
  [PASS] mem write/read
  [PASS] suki_obj_destroy(mh)
--- sukinative proc object ---
[sched] user task '/BIN/HELLO.SKA' pid=16 ... (ELF, W^X)
=== app HELLO loaded as ELF64 and now running ===
hello from a freshly spawned ELF process; exiting now.
[syscall] task '/BIN/HELLO.SKA' pid=16 exit(code=0)
suki_proc_create(/BIN/HELLO.SKA)
  [PASS] suki_obj_query(proc)
  [PASS] wait proc exit => 0
  [PASS] suki_obj_destroy(ph)
[sukinative] done

=== POSIX test summary: PASS=192 FAIL=0 ===
ALL POSIX TESTS PASSED
```
- **Phase 2（FILE/MEM/PROC）全 PASS**，子进程 HELLO.SKA 经 `elf_load` 正常启动并退出，
  `SYS_SUKI_WAIT` 正确阻塞后被 `suki_proc_notify_exit` 唤醒。
- **POSIX 兼容性零回归**：192 项全 PASS（含 Phase 1 的 event/mutex/sem/wait 与全部
  POSIX 一致性用例），确认双 API 平行共存、老程序（Servo 等只调 20–129）不受影响。
- **零 panic / 零三重故障 / 零 #GP/#PF**，符合生产就绪度要求。

---

## 7. 文件清单（本次改动）

| 文件 | 改动 |
|------|------|
| `include/kernel/suki_native.h` | `suki_object_t` 扩展 FILE/PROC/MEM 联合字段 + `next_notify` |
| `include/kernel/task.h` | `task_t` 增加 `suki_exit_notify` 链头 |
| `include/sukios/posix.h` | `suki_objinfo_t` 增加 `base/size` |
| `include/kernel/fd.h` | 声明 `fd_read_kern`；`fd_entry_t` 增加 `suki_owned` |
| `kernel/fs/fd.c` | 新增 `fd_read_kern`（内核缓冲读）；`fd_exit_task` 跳过 `suki_owned` 槽 |
| `kernel/syscall/sys_suki.c` | 六个 Phase 2 handler + `suki_proc_notify_exit` + `suki_obj_free`/`suki_proc_detach`；修复 §4 四个根因 |
| `kernel/sched/sched.c` | `task_exit_current` 释放 `g_sched_lock` 后注入 `suki_proc_notify_exit(t)` |
| `user/lib/suki.h` | 新增 FILE/PROC/MEM 封装声明 |
| `user/lib/suki_native.c` | 实现 FILE/PROC/MEM 封装（号位 144–149） |
| `user/posixtest.c` | `test_sukinative` 增加 Phase 2 用例 |

---

## 8. 结论与后续

Phase 2 已交付真正可工作的实现并验证通过：SukiNative 现具备“文件/进程/内存”三类原生
对象，配套句柄管理、引用计数、阻塞等待（wait）、退出通知与查询。POSIX 层稳定不变，
双 API 平行架构（POSIX 20–129 兼容 + SukiNative 130–149 原生）持续成立。

后续可选增强（非本次必须、不阻塞）：为 MEM 对象增加 `MEM_FREE`/resize、为 FILE 对象
增加 `seek`/追加模式、为 PROC 对象增加 `proc_wait_pid` 直接按 pid 等待等，均可在不
触碰 POSIX 路径的前提下增量扩展。
