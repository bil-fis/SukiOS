# Step41：串口冗长诊断输出开关 + 系统文件后缀体系落地

## 1. 用户诉求

1. 串口调试输出（尤其 IPC 逐条追踪、disk-srv 每消息收发细节、console-srv
   IPC 回显）拖慢系统：串口是 PIO，完整 POSIX 层上线后 shell+fs_server+
   posixtest 并发时每秒数百条 IPC，逐条打印会刷爆串口、严重拖慢系统、淹没
   真正有用的输出。
   - `make run` 产出的内核**不带**这些冗长调试信息。
   - `make run-dbg` 产出的内核**带**全部冗长输出。
2. 按 `SukiOS 系统文件后缀参考.md` 更新所有后缀（内核 .ski、系统服务
   .ssvc、独立应用 .ska 等）。
3. 把该参考文件内容写入记忆（已执行，见末尾）。

## 2. 串口诊断开关实现（任务 A）

### 2.1 设计

沿用既有 `CONFIG_SMP` 的编译期注入机制：Makefile 生成 `build/config.h`，
在其中定义 `CONFIG_DEBUG_SERIAL`（0/1）。内核代码用受控宏 `dbg_printf`
区分"冗长诊断"与"关键事件"。

```c
/* include/kernel/console.h */
#if defined(CONFIG_DEBUG_SERIAL) && CONFIG_DEBUG_SERIAL
#define dbg_printf(...)  kprintf(__VA_ARGS__)
#else
#define dbg_printf(...)  ((void)0)
#endif
```

- `make run`：`DBG ?= 0` → `CONFIG_DEBUG_SERIAL=0` → `dbg_printf` 编译为空，
  不产生任何串口流量，系统全速运行。
- `make run-dbg`：递归子 make 传 `DBG=1` → `CONFIG_DEBUG_SERIAL=1` →
  `dbg_printf` 展开为 `kprintf`，输出全部冗长诊断。

**关键事件不进开关**（始终无条件 `kprintf`）：`[ipc] port table ready`、
`[ipc] port_allocate: table exhausted`、`[ipc] OOL recv window exhausted`、
`#PF`/`panic`、启动 FATAL、服务上线横幅。保证生产环境也能看到致命信息。

### 2.2 受控的冗长输出点（原无条件 kprintf → dbg_printf）

| 位置 | 原输出 | 受控后 |
|------|--------|--------|
| `kernel/ipc/port.c` 的 `ipc_trace` 宏 | 5 处 IPC 收发追踪 | `dbg_printf`，去掉局部 `IPC_TRACE` 开关 |
| `kernel/drivers/ata.c` | `[disk-srv] dbg:` 逐消息收发（READ/WRITE 两分支，共 7 处） | `dbg_printf` |
| `kernel/kmain.c:78` | `[console-srv] got N bytes via IPC: ...` | `dbg_printf` |

### 2.3 Makefile 改动

- `SMP ?= 0` 后新增：
  ```make
  DBG                  ?= 0
  CONFIG_DEBUG_SERIAL  := $(if $(filter 1,$(DBG)),1,0)
  ```
- `$(CONFIG_H)` 生成追加：`@printf '#define CONFIG_DEBUG_SERIAL %s\n' '$(CONFIG_DEBUG_SERIAL)' >> $@.tmp`
- 新增 `run-dbg` 目标（递归子 make 传 `DBG=1`，复用 `run-headless` 便于
  无图形环境验证）：
  ```make
  run-dbg:
  	$(MAKE) DBG=1 run-headless
  ```
- `.PHONY` 加入 `run-dbg`；`info` 目标打印 `DEBUG` 状态。

### 2.4 一个连带修复

`port.c` 原 `ipc_trace` 是空宏（`((void)0)`），参数中的未定义标识符
`wcpu`（line 391 附近）不会被求值，故原本能编译。改为 `dbg_printf`（真函数
宏，会求值参数）后暴露该 bug：`ipc_trace("[ipc] deliver dest=%u queued
(len=%u) woke_waiter=%s\n", ..., wcpu >= 0 ? "yes" : "no")` 引用了作用域内
不存在的 `wcpu`。已将 `wcpu` 字段删除（仅保留 dest/len 两个有效参数）。

## 3. 文件后缀体系落地（任务 B）

依据 `SukiOS 系统文件后缀参考.md`（已全文写入记忆），对实际产物做端到端
一致改名：

### 3.1 内核镜像 .ski（可见、引导级）
- `Makefile`：`KERNEL := $(BUILD)/kernel.ski`（原 `kernel.elf`）。
- `$(ISO)` 规则：`cp $(KERNEL) $(ISODIR)/boot/kernel.ski`。
- `grub/grub.cfg`：`multiboot2 /boot/kernel.ski`。
- `tools/layer_test.sh`、`tools/probe.py`、`tools/test_pvh.py`、`boot/pvh.S`
  注释中的 `kernel.elf` 全部改为 `kernel.ski`。
- 构建验证：`build/kernel.ski` 正常生成并引导。

### 3.2 系统服务 .ssvc（构建产物后缀）
- `USER_BLOBS` 改为 `$(patsubst %,$(BUILD)/user/%.ssvc.blob.o,$(USER_PROGS))`
  → `build/user/fs_server.ssvc.blob.o` 等（4 个服务）。
- blob 规则改为 `$(BUILD)/user/%.ssvc.blob.o: $(BUILD)/user/%.elf`，符号
  重命名用 `$(basename $*)` 剥离 `.ssvc`，生成 `user_fs_server_start` /
  `user_fs_server_end`（与 `kmain.c` 硬编码引用一致，**内核侧零改动**）。
- 说明：当前 SukiOS 服务为内嵌进内核（非 /system/services 磁盘加载），
  `.ssvc` 体现在构建产物文件名，符合官方后缀命名。

### 3.3 独立应用 .ska（磁盘可见、shell 可用）
- `disk` 目标：拷贝独立程序时文件名大写 + `.SKA`：
  `mcopy ... ::BIN/$$up.SKA`（原 `::BIN/$$up` 无后缀）。验证磁盘内：
  `HELLO.SKA`、`PLAYAUDIO.SKA`、`AUDIOTEST.SKA`。
- `user/shell.c` 的 `exec`：若 `sys_task_spawn(argv[0])` 失败且输入路径
  **不含 '.'**（如 `exec BIN/playaudio`），自动补 `.ska` 再尝试一次，使
  `exec BIN/playaudio` 与 `exec BIN/playaudio.ska` 都能装载（文件名大小写
  由 FatFs 不敏感处理）。
  - 初版误用 `u_strchr`（shell 链接集未定义该符号）→ 改为内联点号检测，
    消除 `undefined reference to u_strchr`。

## 4. 验证（QEMU 生产场景，零 panic）

### 4.1 make run（DBG=0，默认）
```
[ipc]           : 1   （仅无条件「port table ready」）
[disk-srv] dbg  : 0   （完全消除）
[console-srv]   : 0   （完全消除）
=== POSIX test summary: PASS=132 FAIL=0 ===
ALL POSIX TESTS PASSED
```
系统全速运行，串口流量极小，无 #PF、无 panic。

### 4.2 make run-dbg（DBG=1）
```
[ipc]           : 2089 行（IPC 逐条追踪恢复）
[disk-srv] dbg  : 677 行（disk-srv 收发细节恢复）
=== POSIX test summary: PASS=132 FAIL=0 ===
ALL POSIX TESTS PASSED
```
冗长诊断完整输出，且同样零 panic（证明开关仅影响日志、不影响逻辑）。

### 4.3 后缀落地验证
- `build/kernel.ski` 存在并成功引导。
- `build/user/*.ssvc.blob.o` 四个服务均生成。
- `mdir -i build/disk.img ::BIN` 显示 `HELLO.SKA`/`PLAYAUDIO.SKA`/`AUDIOTEST.SKA`。
- 系统服务全部上线：`[boot] all services spawned; system fully up`。
- 整体 132/132 PASS，零 panic。

## 5. 涉及文件

| 文件 | 改动 |
|------|------|
| `Makefile` | 加 `DBG`/`CONFIG_DEBUG_SERIAL`；config.h 注入；新增 `run-dbg`；`KERNEL`→`kernel.ski`；`USER_BLOBS`→`.ssvc.blob.o`；blob 规则 `.ssvc.blob.o` + `$(basename $*)`；disk 拷贝 `.SKA` |
| `include/kernel/console.h` | 新增 `dbg_printf` 受 `CONFIG_DEBUG_SERIAL` 控制的宏 |
| `kernel/ipc/port.c` | `ipc_trace` 改用 `dbg_printf`；删除未定义 `wcpu` 参数 |
| `kernel/drivers/ata.c` | READ/WRITE 两分支 `[disk-srv] dbg:` 改 `dbg_printf`（共 7 处） |
| `kernel/kmain.c` | console-srv IPC 回显改 `dbg_printf` |
| `grub/grub.cfg` | `multiboot2 /boot/kernel.ski` |
| `user/shell.c` | `exec` 自动补 `.ska` 后缀；内联点号检测替代 `u_strchr` |
| `tools/layer_test.sh`、`tools/probe.py`、`tools/test_pvh.py`、`boot/pvh.S` | `kernel.elf`→`kernel.ski` |
| `SukiOS 系统文件后缀参考.md` | 内容已写入记忆（ID 79299413） |

## 6. 提交

修复完成后执行 `git commit`（简短英文前缀 + 中文描述，不 `git push`）。
