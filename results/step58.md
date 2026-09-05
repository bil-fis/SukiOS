# SukiOS 步骤 58：完整 POSIX 多线程基座（pthread / clone / futex / TLS）

> 日期：2026-09-05
> 目标：在既有 fork/exec/wait/poll/select/pipe/完整文件 I/O 的 POSIX 层之上，补齐
> **真正的多线程**能力，使 `pthread_create`/`pthread_join`/`pthread_exit`/互斥锁/条件变量
> 端到端可用，从而支撑后续把 Linux 多线程程序移植到 SukiOS。
> 本步骤同时修复了 `fork`→`waitpid` 僵尸回收被误判退出（连带 2 项 posixtest 失败）。

---

## 1. 验证结果（QEMU 无头回归，生产场景零 panic）

```
--- pthread (clone/futex/TLS) ---
  [PASS] pthread_create
  [PASS] pthread_join
  [PASS] pthread_shared_counter     (4 线程 + 主线程各累加 50000，互斥锁保护，结果精确 = 250000)
  [PASS] pthread_self_main
  [PASS] pthread_cond_ops
=== POSIX test summary: PASS=137 FAIL=0 ===
```

- 启动无 `[KERNEL OOPS]`、`[panic]`、`#GP`、`triple fault`；`[posix] clone: parent pid=9 -> tid=12..15` 正常打印。
- 回归命令（与项目规则一致，仅用 bash + QEMU 自带机制）：
  ```bash
  make run-headless QEMU_SERIAL="-serial file:/tmp/qemu.serial.log" RUN_TIMEOUT=90
  grep -aE "pthread|clone|OOPS|panic|#GP|PASS=" /tmp/qemu.serial.log
  ```

---

## 2. 设计要点

### 2.1 线程模型
- 每线程 = 一个独立内核 `task_t`；`SYS_CLONE` 带 `SUKI_CLONE_VM` 时**共享父地址空间**
  （同一 `cr3` 与 `vma_list` 链表），否则按 fork 复制地址空间。
- 线程退出不经由 `waitpid` 僵尸语义（否则会永久滞留）：内核在 `task_exit_current`
  中以 `is_group_leader`（即 `tgid==id`）判断——线程 `tgid==leader.tgid` 故非组长，
  直接进 `g_dead_list` 立即回收；进程 leader 才保留为 zombie 等父 `waitpid`。

### 2.2 TLS（每线程 FSBASE）
- 内核用 **GS**（`swapgs`）做 per-CPU，**FS 段专供用户态 TLS**；内核栈保护用全局
  `__stack_chk_guard`（`-mstack-protector-guard=global`），不依赖 `%fs`，故将 FS 交还
  用户态 TLS 是安全的。
- `task_t` 新增 `fs_base` 字段；每次上下文切换（`schedule()` 与 `task_exit_current`
  切到 next 前）统一调用 `sched_set_fs_base(next->fs_base)` 刷新 FS base。
- 用户态 `arch_prctl(ARCH_SET_FS, tcb)` 设置本线程 TLS 基址（TCB 地址）。

### 2.3 clone 内核实现（`kernel/syscall/sys_posix.c::sys_clone`）
- 复用 `sys_fork` 的内核栈帧构造（取自当前 CPU `g_syscall_gpr[cpu]` 保存的 12 个用户 GPR），
  仅把子线程 RIP 覆盖为 `trampoline`、RSP 覆盖为 `child_stack`，FS base 设为 `tls`。
- 子线程入口 `fork_child_return`（置 `rax=0` 后 `iretq`）直接进入用户态 trampoline。
- `SUKI_CLONE_CHILD_SETTID`：把子 tid 写入用户 `*ptid`（=TCB.tid）；
  `SUKI_CLONE_CHILD_CLEARTID`：登记 `clear_child_tid`（内核清理时清零，供后续增强 join 语义）。
- 线程共享父 fd 槽表（`fd_fork_clone`），各自独立内核栈 / 用户栈 / TLS。

### 2.4 futex（真正的等待/唤醒，非 stub）
- 文件：`kernel/sched/futex.c` + `include/kernel/futex.h`。
- 哈希表：`futex_hash(cr3, uaddr)` 按「地址空间 + 用户字地址」分桶（线程共享 cr3，故
  同进程内所有线程落同一桶）。
- `futex_wait`：比较 `*uaddr==val`（经 `copy_from_user`）后才睡眠；否则立即返回
  `-EAGAIN`（避免丢失唤醒）。睡眠时 `cur->state=BLOCKED; cur->in_rq=false` 后调用
  `schedule()`；唤醒后自行从桶摘除并 `kfree` waiter 节点。
- `futex_wake`：**先持锁收集待唤醒任务，释放 `g_futex_lock` 后再 `sched_wake`**，以免与
  `task_exit_current`（持 `g_sched_lock` 后调用 `futex_wake` 的潜在路径）形成 `g_futex_lock → g_sched_lock`
  与 `g_sched_lock → g_futex_lock` 的反向锁序死锁。
- 退出清零 `clear_child_tid` 时**只写 0、不调用 `futex_wake`**（join 走用户态 `join_futex`），
  避免在持 `g_sched_lock` 时递归自死锁。

### 2.5 地址空间回收（线程共享 AS）
- 新增 `address_space_shared(t)`：遍历 `g_all_tasks`，若有**存活且非僵尸**的任务与 `t` 共享
  `cr3` 则返回 true。
- `task_exit_current` 仅在 `!address_space_shared(t)` 时销毁 `cr3/vma`（由最后退出的线程负责释放，
  避免误伤仍在运行兄弟线程的映射）。僵尸（zombie）共享者不计入，避免地址空间泄漏。

### 2.6 用户态 pthread 库（`user/lib/pthread.c` + `pthread.h`）
- `pthread_create`：`mmap` 独立 2MB 用户栈；TCB 置于栈顶，子线程 RSP 自 TCB 向低地址增长绝不踩踏
  TCB；`clone(SUKI_CLONE_VM|FILES|THREAD|SETTLS|CHILD_SETTID|CHILD_CLEARTID, tcb, thread_trampoline, tcb, &tcb->tid, &tcb->clear_tid)`。
- `thread_trampoline`：读 `%fs:0` 取得 TCB，调用 `tcb->start(tcb->arg)`，结果存入 `tcb->retval` 后
  `pthread_exit`。
- `pthread_join`：在目标 `join_futex` 上 `futex_wait` 直至 `join_futex==tid`，再 `munmap` 线程栈。
- `pthread_exit`：置 `join_futex=tid` 并 `futex_wake`，再 `SYS_TASK_EXIT`。
- `pthread_mutex_*`：CAS 自旋 + `futex_wait/wake`（标准 futex 互斥锁）。
- `pthread_cond_*`：seq 计数器 + futex（signal/broadcast 自增 seq 并唤醒）。
- `pthread_once`：CAS 访问即置位，确保仅执行一次。
- 主线程首次调用任意 pthread 函数时 `arch_prctl(ARCH_SET_FS, main_tcb)` 建立 TLS。

---

## 3. 关键文件 / 常量 / 地址

| 文件 | 内容 |
|---|---|
| `kernel/syscall/sys_posix.c` | `sys_clone`、真实 `sys_futex`/`sys_arch_prctl`/`sys_set_tid_address`、`sys_getpid` 返回 `tgid`、fork 子进程 `tgid=id`（修复 waitpid） |
| `kernel/sched/futex.c` | `futex_wait` / `futex_wake` 实现 |
| `kernel/sched/sched.c` | `set_fs_base`/`sched_set_fs_base`、地址空间共享判定、`task_exit_current` 线程回收与 AS 释放、FS base 切换 |
| `include/kernel/task.h` | `task_t` 新增 `tid/tgid/fs_base/clear_child_tid/owns_as` |
| `include/sukios/posix.h` | `SYS_CLONE=205`、`SUKI_ARCH_SET_FS/GET_FS`、`SUKI_CLONE_*` 系列常量 |
| `include/kernel/futex.h` | `futex_wait`/`futex_wake` 声明 |
| `user/lib/pthread.c` / `pthread.h` | 用户态 pthread 实现 |
| `user/lib/syscalls.c` / `libc.h` | `clone/futex/arch_prctl/gettid/set_tid_address/mmap/munmap` 包装 |
| `user/posixtest.c` | `test_pthread`（含共享计数器精确性、join、cond 操作） |
| `kernel/kmain.c` | 开机自动运行 `posixtest`（含 pthread 用例） |

- FS base MSR：`IA32_FS_BASE = 0xC0000100`（`wrmsr` 语义 `ecx=索引, edx:eax=值`）。
- 内核 CS/DS：`0x08`/`0x10`；用户 CS/DS：`0x1B`/`0x23`；`USER_RFLAGS=0x202`。

---

## 4. 修复的关联缺陷

- **fork→waitpid 僵尸被误回收**：`task_exit_current` 的 `has_parent` 误判 fork 子进程为线程。
  根因：`sys_fork` 经 `memcpy(child, parent)` 继承了 `tgid`（=父 `tgid`），导致
  `is_group_leader=(tgid==id)` 为假。修复：fork 子进程显式 `child->tgid = child->id`
  （fork 创建的是独立进程组组长，POSIX 语义正确）。修复后 `[FAIL] waitpid reaps child` /
  `[FAIL] libc waitpid reaps child` 两项 137→0 失败。

---

## 5. 后续 POSIX 路线图（本步骤未覆盖，列为下一里程碑）

- **信号处理器（signal/sigaction/raise/kill→用户态 handler）**：当前仅 `task_signal`（kill）可
  终止任务，无用户态 handler 投递。需在 `syscall_entry.S::syscall_return_path` 与 `isr.S::iretq`
  返回路径注入「待处理信号 → 构造用户态 ucontext 帧 → 跳 handler → sigreturn 还原」。
- **文件映射 mmap（file-backed）**：当前 `sys_mmap` 仅支持 `MAP_ANONYMOUS`，需接 VFS 把文件页
  按需/整段映射进用户地址空间。
- **socket（网络栈）**：需新增网络协议栈（无现成 net 子系统），属大型子系统。
- **dlopen（动态链接器）**：需用户态 ELF 动态加载 + 符号解析，当前系统为静态链接。
