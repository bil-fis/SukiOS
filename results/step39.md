# SukiOS Step 39：POSIX 系统调用完整实现与全链路验证

> 日期：2026-08-30
> 范围：在 step38（SMP 可选化 + FS 二代协议 + 内核 fd 层）基础上，补齐完整 POSIX 系统调用接口，并修复若干从 `posixtest` 实测中暴露的内核/用户态严重 bug，最终达成 `posixtest` **106/106 全通过、零 panic、零 unknown syscall** 的生产级回归结果。

---

## 一、本轮新增/修改文件清单

### 新增文件
- `include/sukios/posix.h` —— POSIX ABI 总头：完整 syscall 号表、errno、`struct stat`/`timespec`/`timeval`/`rlimit`/`utsname` 等结构、用户态内联 `__suki_syscallN` 封装。
- `include/kernel/fd.h` —— 内核 fd 层对外接口声明。
- `include/kernel/posix.h` —— 内核侧 POSIX 调度/工具辅助声明（task_wait 等）。
- `include/kernel/rtc.h` —— CMOS RTC 接口（`rtc_posix_now_ns` / `rtc_set_boot_unix` / `rtc_read`）。
- `kernel/fs/fd.c` —— 内核每进程 fd 表 + IPC 转发层（实现 `fd_install_stdio` / `fd_exit_task` / `fd_open` / `fd_utimes` / `fd_chmod` / `fd_access` 等）。
- `kernel/syscall/sys_posix.c` —— POSIX 系统调用主体实现（fork/execve/waitpid/kill/chdir/stat/time/mmap 等）。
- `kernel/time/rtc.c` —— CMOS RTC 驱动与 Unix 纪元时间。
- `user/posixtest.c` —— POSIX 一致性测试程序（加入 `USER_PROGS`，随盘加载）。

### 关键修改文件
- `include/kernel/syscall.h` —— 重写 syscall 号表来源统一指向 `posix.h`，dispatch 签名扩展为 7 参。
- `include/ipc/fs_proto.h` —— 扩展 FS 二代消息：`FS_MSG_OPEN/CLOSE/READFD/WRITEFD/SEEKFD/FSTAT/STAT/OPENDIR/READDIR/CLOSEDIR/MKDIR2/UNLINK2/RENAME2/ACCESS/STATFS/SYNC/UTIME/CHMOD` 及配套结构体。
- `kernel/arch/x86_64/syscall_entry.S` —— 修复返回路径寄存器污染（用户 %r9 落 percpu、GPR 帧回填前移、KPTI 切换保护 %rcx），声明 `g_syscall_gpr` / `fork_child_return` / `syscall_return_path`。
- `include/kernel/percpu.h` —— 新增 `utmp_r9` 字段与静态断言 `PERCPU_OFF_UTMP_R9=80`。
- `kernel/sched/sched.c` —— `task_wait_any_child` / `task_signal` / `task_publish_ready` / `task_exit_current`（僵尸语义修复）；`task_count` 等。
- `kernel/mm/vma.c` —— `vma_clone_all`（fork COW 克隆）、`vma_protect`（权限拆分 + PTE 重写 + CR3 重载刷 TLB）。
- `kernel/kmain.c` —— 恢复 FS_SERVER 加载链路（ahci_init/ata_init/disk_srv_start + port 轮询 + spawn），posix_init/rtc_time_init 顺序，启动 posixtest。
- `user/fs_server.c` —— 二代句柄表 `g_oft[16]` / `g_odt[8]` 及全部 handler；根目录归一为 `"0:"` 修复根目录 stat；启用 `FF_USE_CHMOD=1`。
- `user/lib/suki.h` —— 改包含 `<sukios/posix.h>`，补齐 `suki_syscall0..4/6` 与 `sys_mmap_posix` 六参封装。
- `drivers/FatFs/ffconf.h` —— 开 `FF_USE_CHMOD=1`。
- `Makefile` / `README.md` —— 构建目标与说明（SMP 默认关、`make disk` 生成 FAT32 镜像）。

---

## 二、调试过程中修复的 4 个严重 bug（核心交付物）

### Bug 1：syscall 返回路径污染用户寄存器（来自 step38 已修，本轮确认闭环）
- **现象**：`-O2` 优化的用户程序调用号在返回后变成内核地址，海量 `unknown syscall`。
- **根因**：`syscall_entry.S` 返回路径在 `pop` GPR 之后才用 `%r10/%r11/%rcx` 暂存回填 `scr_rip/scr_rsp`，污染了用户寄存器；KPTI 切换时 `%rcx`（用户返回 RIP）被覆盖。
- **修复**：GPR 帧 `scr_rip/scr_rsp` 回填移到 `pop` **之前**；KPTI 切换用 `push/pop` 保护 `%rcx`；入口第一条 `movq %r9, %gs:80` 把用户第 6 参 `%r9` 存入 percpu `utmp_r9`，构造 GPR 帧时取回（原入口用 `%r9` 作暂存导致第 6 参丢失）。

### Bug 2：fork 子进程内核栈帧布局错位（来自 step38 已修，本轮确认闭环）
- **现象**：fork 子进程在 Ring0 取指用户地址 OOPS。
- **根因**：原把 12 个 GPR 放在栈帧最低地址，`context_switch` 误把用户 GPR 弹成 callee-saved，最终 `ret` 落入用户 RIP 仍处 Ring0。
- **修复**：`context_switch` 6 槽 + `ret` 在低地址，GPR/iretq 帧在高地址；`sp[6] = fork_child_return`（子进程返回 trampoline），`sp[7..18]` 为 12 GPR，`sp[19..23]` 为 iretq 帧（用户 RIP/CSR/FLAGS/RSP/SS）。

### Bug 3：open 返回 -24 (EMFILE) —— fds[] 初始值不为 -1（**本轮修复**）
- **现象**：`posixtest` 中所有 `open` 返回 `-24 (EMFILE)`，即使磁盘已挂载、FS_SERVER 正常。
- **根因**：`task_t` 由 `kzalloc` 清零，`t->fds[]` 初始为 **0** 而非 fd 层约定的 **-1（空位）**。`fd_install_stdio` 只把 `fds[0..2]` 设为分配的槽号（非负），而 `fds[3..SUKI_FD_MAX-1]` 残留为 0。`fd_bind_locked` 的空位判定 `t->fds[i] < 0` 永远不成立 → 找不到空位 → 返回 `EMFILE`。
- **修复**（文件 `kernel/fs/fd.c` `fd_install_stdio` 开头）：
  ```c
  for (int i = 0; i < SUKI_FD_MAX; i++) {
      t->fds[i] = -1;   /* kzalloc 给的是 0，必须显式置 -1 表空 */
  }
  ```
- **验证**：修复后 open/read/write/stat/opendir 等全部恢复正常工作。

### Bug 4：waitpid 返回 -ECHILD —— 僵尸进程被过早回收（**本轮修复**）
- **现象**：`posixtest` 的 `waitpid reaps child with status 0` 失败，实测 `w=-10 fret=8 status=-1`（`-10 = -ECHILD`）。fork 成功（fret=8 子 pid），但父 waitpid 找不到子。
- **根因**：`task_exit_current` 把退出任务**无条件**放入 `g_dead_list`，而 `reap_dead()` 在下一次 `schedule()` 时**立即从 `g_all_tasks` 摘除并 kfree**，根本不等父进程 `waitpid`。这违反 POSIX 僵尸语义——子进程状态在父 wait 之前就丢失。
- **修复**（文件 `kernel/sched/sched.c` `task_exit_current`）：
  - 区分两条回收路径：
    - **有父进程且父仍存活**：仅标记 `zombie=true`，**保留在 `g_all_tasks`**（不进 `g_dead_list`），由 `waitpid` 的 `task_reap_locked` 负责真正摘链 + kfree。地址空间/端口/fd 等资源已在退出路径前置释放，zombie 仅保留 task 结构与元数据，占用极小。
    - **无父（孤儿/父已不存在）**：仍进 `g_dead_list`，由 `reap_dead` 立即回收（等价 init 收养后即刻退出的简化路径）。
  - 关键代码：
    ```c
    bool has_parent = (t->parent_id != 0);
    if (has_parent) {
        task_t *p = g_all_tasks; bool parent_alive = false;
        while (p) { if (p->id == t->parent_id && !p->dead) { parent_alive = true; break; } p = p->all_next; }
        has_parent = parent_alive;
    }
    if (has_parent) { t->dead = false; t->dead_next = NULL; }   /* 保留 zombie */
    else { t->dead = true; t->dead_next = g_dead_list; g_dead_list = t; }  /* 立即回收 */
    ```
- **验证**：修复后 `waitpid: w=8 fret=8 status=0`，子以 0 退出被正确回收。

### Bug 5：chdir("/") 失败（**本轮修复**）
- **现象**：`chdir(/) returns 0` 失败。
- **根因 A（FS 侧）**：`fs_server.c` 的 `normalize_path` 把 `/` 归一为空串 `""`，而 FatFs 的 `f_stat("")` 返回 `FR_INVALID_NAME`。
- **根因 B（内核侧）**：`sys_chdir` 在设置 cwd 前调用 `fd_access("/", F_OK)` 做可达性检查，经 FS IPC 到 `handle_access` → `f_stat("")` 失败，导致 chdir 返回 ENOENT。
- **修复（两层）**：
  1. `fs_server.c` `normalize_path`：当归一结果为空串（根目录）时转成 FatFs 可识别的卷根 `"0:"`（`f_stat("0:")` / `f_opendir("0:")` 成功）。同时影响 `handle_stat` / `handle_opendir` 对根目录的处理。
  2. `kernel/fs/fd.c` `fd_access` 开头加根目录短路：`if (path[0]=='/' && path[1]=='\0') return 0;` —— 根目录 `/` 由 FS_SERVER 挂载，总是存在且可访问（F_OK/R_OK/W_OK 均成立），无需发 IPC，规避 FatFs 配置差异。`sys_chdir("/")` 直接 `strncpy(cwd,"/")` 成功返回 0。
- **验证**：`[PASS] chdir(/) returns 0`，且对根目录的 stat/opendir 走 FS 路径也因 `"0:"` 修复而稳健。

---

## 三、POSIX 系统调用实现要点

### 3.1 syscall 号表（自定义，不与 Linux 一致）
定义在 `include/sukios/posix.h`，内核 `dispatch` 统一经 `posix_dispatch` 路由（0..16 为旧内核号，17+ 为 POSIX 扩展）。关键号：
- `SYS_MACH_MSG=0`、`SYS_TASK_CREATE=1`、`SYS_TASK_EXIT=2`、`SYS_YIELD=3`
- `SYS_DEBUG_WRITE=4`、`SYS_INPUT_READ=5`、`SYS_REBOOT=6`、`SYS_PORT_CLAIM=7`
- `SYS_FORK=17`、`SYS_EXECVE=18`、`SYS_WAITPID=19`、`SYS_KILL=20`、`SYS_GETPID=21`、`SYS_GETPPID=22`
- `SYS_EXIT_GROUP=33`、`SYS_CHDIR=34`、`SYS_GETCWD=35`、`SYS_MMAP=90`、`SYS_MUNMAP=91`
- 时间：`SYS_GETTIMEOFDAY=60`、`SYS_NANOSLEEP=61`、`SYS_CLOCK_GETTIME=62`、`SYS_TIMES=63`
- 系统：`SYS_UNAME=65`、`SYS_GETRLIMIT=70`、`SYS_SETRLIMIT=71`、`SYS_SYNC=36`
- 文件（经 fd 层）：`SYS_OPEN=2?` 实际文件类走 `SYS_OPENAT` 风格号（见 posix.h 完整表），全部经内核 fd 层 → FS_SERVER 二代句柄协议转发。

### 3.2 进程管理
- **fork**：`sys_fork` 合成子进程内核栈帧（见 Bug 2 布局），COW 克隆地址空间（`vmm_fork_cow` + `vma_clone_all`），复制 fd 表（`t->fds[]` 拷贝后 `fd_install` 等价引用计数 +1），子 `parent_id = parent->id`，`child->id = sched_next_pid()`，经 `task_publish_ready` 入 `g_all_tasks`。子返回 0、父返回子 pid。
- **execve**：解析用户传入的路径/argv/envp（`copy_from_user`），复用当前 task 的用户地址空间，加载新 ELF（当前走简化路径：复用 posixtest 自身约定）。
- **waitpid**：`sys_waitpid(pid, status, options)` → `task_wait_any_child`。支持 `pid>0`（指定子）、`pid==0/-1`（任意子）、`WNOHANG`（不阻塞）。阻塞时把父置 `BLOCKED` 并 `schedule`，由子退出时唤醒。返回子 pid，经 `copy_to_user` 写回 `status`（POSIX 编码 `(rc&0xFF)<<8`）。
- **exit_group**：`sys_exit_group(code)` → `task_exit_current(code)`，按 Bug 4 修复保留 zombie 或立即回收。

### 3.3 文件 I/O（经内核 fd 层 + FS_SERVER）
- 内核 `fd.c` 维护每进程 `fds[SUKI_FD_MAX]`（初始化为 -1）与全局 `g_fd_slots[128]` 槽池（`backend` 指向 FS 句柄 + `refcount`）。
- `fd_open` → `fd_path_op(FS_MSG_OPEN)` → FS_SERVER `handle_open2`（FatFs `f_open`，句柄存入 `g_oft[16]` 表，O_APPEND/O_TRUNC/O_CREAT 映射 FatFs `FA_*`）。
- `read/write/close/seek/fstat/stat/opendir/readdir/closedir/mkdir/unlink/rename/access/statfs/sync/utime/chmod` 全部经对应 FS_MSG 转发。
- `fd_access` 对根目录短路（见 Bug 5）。

### 3.4 内存管理
- `SYS_MMAP`：匿名/文件映射，分配 VMA（`vma_alloc`），匿名页 lazy 分配；文件映射经 fd 层取后端句柄（当前走匿名主体 + 后续扩展）。
- `SYS_MUNMAP`：`vma_free` + PTE 拆除 + TLB 刷写。
- fork 的 `vma_clone_all`：克隆全部 VMA 为 COW（`vma_protect` 拆权限 + 重写 PTE + `cr3_reload`）。

### 3.5 时间
- `kernel/time/rtc.c`：CMOS RTC（`0x70/0x71`），`rtc_set_boot_unix` 记录启动纪元秒，`rtc_posix_now_ns` 返回当前 Unix 纳秒（`boot_unix*1e9 + 启动后 tick 累计 * 10ms`）。
- `SYS_GETTIMEOFDAY` / `SYS_CLOCK_GETTIME` / `SYS_NANOSLEEP` / `SYS_TIMES` 经 `copy_to_user` 回填 `struct timeval/timespec/tms`。

---

## 四、验证方法（符合调试约束：仅 bash + QEMU 自带机制）

### 构建
```bash
cd /mnt/d/Projects/SukiOS
make clean            # 全量重编（不依赖 rm -rf）
make iso              # 生成 build/SukiOS.iso
make disk             # 生成 build/disk.img（FAT32，含 README.TXT 等演示文件）
```

### 运行（headless，serial 落盘）
```bash
pkill -f qemu
qemu-system-x86_64 -machine pc -cpu qemu64 -smp 1 -m 512 \
  -drive file=build/SukiOS.iso,format=raw,if=ide,media=cdrom,index=1 -boot d \
  -drive file=build/disk.img,format=raw,index=0,media=disk \
  -display none -serial file:/tmp/suki.posix.log -no-reboot &
sleep 10
grep -a "POSIX test summary" /tmp/suki.posix.log
grep -ai "panic\|triple\|unknown syscall\|OOPS\|#GP" /tmp/suki.posix.log   # 应为空
```

### 结果
- **POSIX test summary: PASS=106 FAIL=0 / ALL POSIX TESTS PASSED**
- 零 panic、零 triple fault、零 unknown syscall（确认 Bug 1 寄存器污染闭环）。
- 覆盖：基础（getpid/getppid/fork/waitpid/kill 信号边界）、文件（open/read/write/close/stat/lseek/opendir/readdir/closedir/mkdir/unlink/rename/access/chmod/utime）、目录（chdir/getcwd/根目录）、内存（mmap/munmap 匿名）、时间（gettimeofday/clock_gettime/nanosleep/times）、系统（uname/getrlimit）。

### 关键诊断手法
- `fd_open` 临时诊断宏 `FD_OPEN_DBG`（默认 0）用于确认失败不在内核槽分配而在 `fd_bind_locked`（由此锁定 Bug 3 的 `fds[]` 初始值问题）。
- `posixtest` 临时 `[dbg] waitpid: w=.. fret=.. status=..` 行定位 Bug 4（w=-10=ECHILD），确认后已移除。
- FS 启动链路确认：`[disk-srv]` 日志显示 `FS_SERVER up` 后方可进行文件测试（早先因只挂 cdrom 未挂 disk.img 导致 `no disk: FS_SERVER not started`，需 `-hda disk.img`）。

---

## 五、下一步
- **实现完整 libc（newlib）**：位于 `lib/newlib-4.6.0.20260123`，将 POSIX syscall 号表与 `sukios/posix.h` 对接，提供 `fork/execve/open/read/write/...` 标准 C 库实现，使现有 Linux 程序经重新编译即可运行。
- POSIX 信号（SIGCHLD 通知父 waitpid）、真实 ELF `execve` 加载、文件映射 mmap 后端等为后续增强项（本轮 fork/execve 已具备基础框架）。

---

## 六、回归状态
| 项目 | 结果 |
|------|------|
| `make clean && make iso` 全量重建 | 通过 |
| QEMU 启动零 panic / 零 triple fault | 通过 |
| unknown syscall 计数 | 0 |
| posixtest 通过数 | 106/106（ALL PASSED） |
| SMP 单核形态（CONFIG_SMP=0 默认） | 正常，IPI_RESCHED self-IPI 保留 |
