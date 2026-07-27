# Step 06 — exec 后无法回到 Shell 的根治：实现 spawn + wait 进程模型（类 Unix fork/exec/wait）

## 0. 问题现象（用户报告）

在 Shell 中输入 `exec BIN/HELLO.ELF a b`，子程序正常打印 `argc/argv` 后退出，
但 **系统没有回到 `SukiOS>` 提示符**，Shell 如同“消失”。这与「生产环境可安全运行」
的目标相悖——任何命令执行都会让交互式 Shell 永久丢失。

## 1. 根因分析（Root Cause）

`kernel/syscall/syscall.c` 中的 `sys_execve`（调用号 `SYS_EXECVE=8`）的语义是
**「加载 ELF 并替换当前进程映像」**：它直接对 `sched_current()`（即正在执行 `exec`
的 Shell 任务）执行：

```c
task_t *t = sched_current();
... vmm_destroy_address_space(old_cr3);
t->cr3 = new_as;
t->user_rip = res.entry;        /* 把 Shell 的代码/数据/栈整体换成 HELLO.ELF */
t->user_stack_top = res.stack_top;
```

当 `HELLO.ELF` 调用 `sys_exit` → `task_exit_current()` 时，退出的是 **这个已经被
替换成 HELLO.ELF 的任务本身**。任务一旦 `exit`，调度器把它从就绪环摘除并回收，
**根本不存在一个独立的 Shell 任务可以“返回”**。Unix 中 Shell 之所能回到提示符，
是因为它先 `fork()` 出子进程再 `exec()`，父（Shell）自身始终存活、只是 `wait()` 阻塞。
本系统此前 `SYS_TASK_CREATE`（创建用户任务）只是个返回 `-1` 的空桩，**没有任何
“新建独立进程”的手段**，因此 Shell 只能用 `execve` 自杀式替换自己。

> 关键结论：`exec` 命令的语义缺陷 + 内核缺少 spawn/wait 原语 = Shell 被替换后消失。

## 2. 修复方案（类 Unix fork+exec+wait）

新增两个系统调用，使 Shell 在**不替换自身**的前提下运行外部程序，并在子进程结束后
继续接管控制台：

| 调用号 | 宏 | 作用 |
|--------|------|------|
| 1 | `SYS_TASK_SPAWN` | 新建一个**独立** Ring3 子任务并装载 ELF（不替换当前进程），返回子 PID |
| 9 | `SYS_WAIT` | 阻塞等待指定 PID 的子任务退出，返回其退出码（非本任务子进程返回 -1） |

Shell 的 `exec` 命令改为：`spawn 子任务 → sys_wait(子PID) → 打印退出码 → 回到提示符`。
原 `SYS_EXECVE`（替换自身）保留为合法原语（例如单用户救援场景仍可用）。

## 3. 数据结构变更（`include/kernel/task.h`）

在 `task_t` 中新增进程关系与退出同步字段：

```c
uint64_t parent_id;     /* 父任务 PID（用 PID 而非指针，避免父退出后悬空指针） */
struct task *waiters;   /* 阻塞在本任务“退出”上的等待者链表头 */
struct task *wait_link; /* 等待者链表指针（链入父/子的 waiters） */
uint64_t exit_code;     /* 退出码（task_exit_current 写入，供父 sys_wait 读） */
bool     zombie;        /* 已退出、尚未被父 sys_wait 回收 */
uint64_t wait_result;   /* 等待者被唤醒后读取的退出码（子退出时写入） */
struct task *all_next;  /* 全局任务链表 g_all_tasks，供按 PID 查找 */
```

函数声明新增：
- `task_t *task_create_user_args(const void*blob,size_t size,int argc,
   const char*const argv[],int envc,const char*const envp[],const char*name);`
- `task_t *task_lookup(uint64_t pid);`
- `void task_reap(task_t *t);`
- `task_exit_current` 签名由 `void` 改为 `void(uint64_t code)`。

## 4. 算法与实现要点

### 4.1 全局任务表 `g_all_tasks`（`kernel/sched/sched.c`）
- 新增静态 `task_t *g_all_tasks = NULL;`
- `task_create_kernel()` 在原有 `irq_save` 临界区内把新任务链入 `g_all_tasks`；
  原 `kzalloc` 已把新字段清零（parent_id=0、waiters=NULL、zombie=false…）。
- `task_lookup(pid)` 沿 `all_next` 线性扫描，可命中**尚未回收的 zombie**（供 wait 读取退出码）。
- `reap_dead()`（每次 `schedule()` 末尾执行）在释放已退出任务前，先从 `g_all_tasks`
  摘除该节点，避免后续 `task_lookup` 返回悬空指针。

### 4.2 `task_create_user_args()`（取代旧的扁平二进制装载）
- 复用 `elf_load()`，但把 `argc/argv/envp` 传入，由 `elf.c::elf_build_stack()` 构造
  符合 System V AMD64 ABI 的连续 `argc/argv/envp/auxv` 初始用户栈（step05 已修的空洞 Bug）。
- 复用 `task_create_kernel(user_task_thunk,...)` 骨架，整段在 `irq_save` 下补写
  `is_user/cr3/user_rip/user_stack_top` 与 `rsp` 中的 r13 参数槽（规避 PIT 抢占竞态）。
- 原 `task_create_user()` 退化为 `task_create_user_args(...,0,NULL,0,NULL,...)` 薄封装，
  启动期装载（fs-server/input-server/shell）行为不变。

### 4.3 `exec_read_file()` 重构（`kernel/syscall/syscall.c`）
- 把 `sys_execve` 内联的「内核作 IPC 客户端向 FS_PORT 发 `FS_MSG_READ_FILE`、经 OOL
  收文件内容」逻辑抽成可复用助手，返回值指向 `fs_resp_t` 之后的 ELF 字节区间。
  `sys_execve` 与 `sys_task_spawn` 共用，避免重复代码。

### 4.4 `sys_task_spawn()`（新建子进程）
```
1. exec_copy_args() 用 copy_from_user 逐字节拷贝 path/argv[]/envp[]（红线：不直解用户指针）
2. exec_read_file() 经 FS IPC 读取 ELF 字节
3. task_create_user_args(elf, len, argc, argv, envc, envp, name) 建独立地址空间
4. child->parent_id = sched_current()->id;     /* 记录父子关系 */
5. 返回 child->id；失败路径统一 kfree argv/envp/elfbuf 并返回 -1
```
子任务进入就绪环，**Shell 自身完全不受影响**。

### 4.5 `sys_wait()`（阻塞等待子进程）
```
uint64_t f = irq_save();
child = task_lookup(pid);
if (!child || child->parent_id != cur->id) { irq_restore(f); return -1; }  /* 非子进程 */
if (child->zombie) { rc = child->exit_code; task_reap(child); irq_restore(f); return rc; }
cur->state = BLOCKED;
cur->wait_link = child->waiters; child->waiters = cur;   /* 登记为等待者 */
schedule();                 /* 关中断下切换（与 port.c 一致） */
irq_restore(f);
return cur->wait_result;    /* 退出码由子进程退出时写入本任务的 wait_result */
```
- **关键**：`schedule()` 必须在中断关闭下调用（单核；避免切换途中被抢占重入）。
- 唤醒后读的是**自身字段** `wait_result`，绝不访问可能已被回收的子任务结构（无悬空读）。

### 4.6 `task_exit_current(code)`（退出并唤醒等待者）
```
interrupts_disable();
/* 唤醒所有等待者：写入退出码并置 READY（task 结构此时仍有效，尚未释放） */
for (w = t->waiters; w; w = wn) { w->wait_result = code; w->wait_link=NULL; w->state=READY; }
t->exit_code = code; t->zombie = true;
port_reap_ool(t); 释放地址空间; ... 入 g_dead_list; 切到下一任务
```
- 子进程退出码先**拷贝进每个等待者自己的 `wait_result`**，再随 `reap_dead` 释放子结构，
  父任务读 `wait_result` 永不发生 use-after-free。

### 4.7 `task_reap()`（防二次 wait 死锁 + 内存泄漏）
- 首次 `sys_wait` 命中 zombie 时立即 `task_reap(child)`：从 `g_dead_list` 与 `g_all_tasks`
  双摘并释放 task 结构与内核栈。
- 效果：二次 `sys_wait` 同 PID 时 `task_lookup` 失败 → 返回 -1（**不再永久阻塞**）；
  僵尸也不会长期占用内存。
- 未被 wait 的僵尸由 `reap_dead()` 在后续 `schedule()` 中照常回收（无泄漏）。

## 5. 关键地址 / 常量
- `USER_STACK_TOP = 0x00007FFFFFFFF000UL`（用户栈顶）
- `EXEC_ELF_MAX = 16*4096`（OOL 单条 16 页上限，与 FS OOL 一致）
- `SYS_TASK_SPAWN=1`、`SYS_WAIT=9`、`SYSCALL_MAX=10`
- `USER_CS=0x1B / USER_DS=0x23`（与 `syscall_entry.S` iretq 帧一致）
- `FS_MSG_READ_FILE = 3`（`include/ipc/fs_proto.h`，内核读文件专用 OOL 消息）

## 6. 修改文件清单
| 文件 | 改动 |
|------|------|
| `include/kernel/task.h` | 新增 7 个任务关系字段；`task_create_user_args`/`task_lookup`/`task_reap` 声明；`task_exit_current(uint64_t)` |
| `kernel/sched/sched.c` | `g_all_tasks` 全局表；`task_create_user_args` + `task_create_user` 封装；`task_lookup`；`task_reap`；`reap_dead` 从全局表摘除；`task_exit_current` 唤醒等待者并写退出码 |
| `kernel/syscall/syscall.c` | `irq_save/irq_restore` 本地临界区原语；`exec_copy_args`/`exec_read_file` 助手；`sys_task_spawn`（替换旧空桩 `sys_task_create`）；`sys_wait`；`syscall_dispatch` 接入；`sys_task_exit` 传退出码 |
| `include/kernel/syscall.h` | `SYS_TASK_CREATE→SYS_TASK_SPAWN=1`；新增 `SYS_WAIT=9`、`SYSCALL_MAX=10` |
| `user/lib/suki.h` | 同步宏；新增 `sys_task_spawn()` / `sys_wait()` 用户态封装 |
| `user/shell.c` | `exec` 命令改为 `spawn + wait`，子进程退出后打印 `child pid=N exited (code=C)` 并回到提示符 |

## 7. 验证（QEMU，实机等价：i440FX + qemu64 + 2G，串口日志）

构建：`make iso` 通过，仅 `user/hello.blob.o` 一条与本次改动无关的 `.note.GNU-stack` 提示。

通过 QEMU monitor (`unix:/tmp/qmon.sock`) 模拟键盘输入，覆盖正常/异常/重复路径：

```
SukiOS> exec BIN/HELLO.ELF a b
[syscall] spawn: parent pid=5 -> child 'HELLO.ELF' pid=6
=== EXECVE OK: HELLO.ELF loaded as ELF64 and now running ===
argc = 3
  argv[0] = BIN/HELLO.ELF
  argv[1] = a
  argv[2] = b
[syscall] task 'HELLO.ELF' pid=6 exit(code=0)
  [shell] child pid=6 exited (code=0)      <-- 回到 Shell！
SukiOS> exec BIN/NOPE.ELF                   <-- 不存在的文件：优雅失败
exec failed: file not found or invalid ELF
SukiOS>                                     <-- Shell 仍存活，未卡死
SukiOS> exec BIN/HELLO.ELF x y z
[syscall] spawn: parent pid=5 -> child 'HELLO.ELF' pid=7
argc = 4
  argv[1] = x  argv[2] = y  argv[3] = z
[syscall] task 'HELLO.ELF' pid=7 exit(code=0)
  [shell] child pid=7 exited (code=0)
SukiOS> ls                                 <-- 连续命令验证 Shell 持续可用
README.TXT  HELLO.TXT  ROADMAP.TXT  SYS/  BIN/
SukiOS> cat README.TXT                    <-- FS 管线正常
...
SukiOS>
```

**判定**：
- ✅ `exec` 执行后**稳定回到 Shell 提示符**（核心 Bug 已根治）
- ✅ 重复 `exec`、与 `ls`/`cat` 交替执行无崩溃
- ✅ 错误路径（`exec` 不存在文件）不再卡死，Shell 继续可用
- ✅ 全程无 `#PF / panic / unknown syscall / triple fault`
- ✅ 子任务参数（argc/argv）正确传递，W^X 装载、ASLR 栈、auxv 不受影响

### 7.1 调试 / 复验命令
```bash
# 构建
make iso
# 启动（串口输出到文件，monitor 走 unix socket 便于脚本注入按键）
qemu-system-x86_64 -machine pc -cpu qemu64 -m 2G -no-shutdown -display none \
  -serial file:build/serial.log \
  -monitor unix:/tmp/qmon.sock,server,nowait \
  -boot d -cdrom build/SukiOS.iso \
  -drive file=build/disk.img,format=raw,index=0,media=disk
# GDB 验证（另开终端）：
#   qemu-system-x86_64 ... -s -S
#   gdb> target remote :1234
#   gdb> info registers        # 确认 syscall 返回后 RIP 落在用户态 0x00004xxx（HELLO 入口/Shell 续跑）
#   gdb> x/2i $rip             # 确认是 HELLO.ELF 或 Shell 的合法用户指令
```

## 8. 生产安全性加固小结
1. **进程模型正确**：Shell 永不被替换，外部程序以独立子任务运行，退出后父存活。
2. **无悬空指针 / use-after-free**：`parent_id` 用 PID 比较而非指针；子退出码先拷贝进
   等待者自身字段再释放子结构；`reap_dead` 与 `task_lookup` 通过 `g_all_tasks` 同步。
3. **无死锁**：二次 `wait` 立即回收 zombie，查不到即返回 -1；非子进程 PID 直接 -1。
4. **内存红线保持**：`path/argv/envp` 一律 `copy_from_user`；`schedule()` 在关中断下调用。
5. **僵尸不泄漏**：未 wait 的僵尸由 `reap_dead` 在后续调度回收；已 wait 的由 `task_reap` 立即回收。
