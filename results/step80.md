# Step 80 — SukiNative / 用户态 API 的 PascalCase 回溯改名

> 日期：2026-09-12
> 目标（承接 step79 末尾的待办 (1)）：按命名规范（记忆 45139565 / 67610012）把 **SukiNative 原生 API**
> 与**所有向用户态暴露的函数**回溯改名为 PascalCase（保留 `Suki` 前缀，形如 `SukiObjCreate`），
> 与系统服务名（`SukiDisplayServer` 等）风格统一。

---

## 一、改名范围

### 1.1 纳入改名（用户态暴露的 `suki_*` 函数，共 50 个）
- **SukiNative 对象/handle API**（`user/lib/suki.h`，声明 + `user/lib/suki_native.c` 实现，20 个）：
  `suki_event_create/set/reset`、`suki_mutex_create/lock/unlock`、`suki_sem_create/acquire/release`、
  `suki_wait`、`suki_obj_create/destroy/duplicate/query`、`suki_file_open/read/write/close`、
  `suki_proc_create`、`suki_mem_alloc`。
- **SukiNative socket API**（`include/sukios/net.h`，声明 + `user/lib/suki_native_net.c` 实现，15 个）：
  `suki_socket_create/bind/connect/listen/accept/send/recv/sendto/recvfrom/close/getsockname/
  getpeername/shutdown/setsockopt/getsockopt`。
- **GUI 客户端库 API**（`user/lib/suki_gui.h`，声明 + `user/lib/suki_gui.c` 实现，13 个）：
  `suki_create_window/destroy_window`、`suki_get_buffer`、`suki_set_pixel`、`suki_fill_rect`、
  `suki_draw_line`、`suki_draw_text`、`suki_flush`、`suki_set_event_port`、`suki_poll_event`、
  `suki_set_focus`、`suki_set_window_pos`、`suki_get_window_rect`。
- **用户态便捷封装**（`user/lib/suki.h` 内联，2 个）：`suki_poweroff`、`suki_reboot`。

### 1.2 明确不改名（避免破坏既有契约）
- **POSIX 兼容层**（记忆 46723010）：`sys_open`/`sys_read`/`open`/`read`/`write`/`fork`/`mmap` 等
  `sys_*` 与标准 POSIX 名——保持 Linux/Unix 生态兼容，不改。
- **内核内部实现符号**（非用户 API）：`include/kernel/suki_native.h` 的 `suki_obj_new`/`suki_obj_ref`/
  `suki_obj_unref`/`suki_handle_alloc`/`suki_handle_lookup`/`suki_handle_free`/`suki_handles_free_all`/
  `suki_obj_consume`/`suki_obj_ready`/`suki_obj_wake_all`、`task.h` 的 `suki_handles`/`suki_exit_notify` 等、
  `drivers/ata.c` 的 `suki_disk_read_sectors`/`suki_disk_write_sectors`——均为 Ring0 内部，非用户态接口。
- **类型/宏/常量**：`suki_handle_t`/`suki_status_t`/`suki_obj_type_t`/`suki_socket_t`/`suki_sockaddr_in_t`/
  `SUKI_*` 宏/`SYS_*` 号——命名规范仅约束函数/方法，类型与常量不动。
- **底层 syscall 原语**：`suki_syscall0..6`/`suki_syscallN`（ABI 胶水，内部使用）。
- **IPC 便捷封装**：`mach_msg_send`/`mach_msg_recv`/`mach_msg_tryrecv`/`mach_msg_destroy`（`mach_msg_*`
  前缀，非 `suki_`，属 Mach IPC 层，不改）。

---

## 二、映射表（节选，全部 50 个同构）

| 旧名 | 新名 | 旧名 | 新名 |
| :-- | :-- | :-- | :-- |
| `suki_obj_create` | `SukiObjCreate` | `suki_socket_create` | `SukiSocketCreate` |
| `suki_obj_destroy` | `SukiObjDestroy` | `suki_socket_sendto` | `SukiSocketSendTo` |
| `suki_obj_query` | `SukiObjQuery` | `suki_socket_getsockopt` | `SukiSocketGetSockOpt` |
| `suki_event_create` | `SukiEventCreate` | `suki_socket_shutdown` | `SukiSocketShutdown` |
| `suki_mutex_lock` | `SukiMutexLock` | `suki_create_window` | `SukiCreateWindow` |
| `suki_sem_acquire` | `SukiSemAcquire` | `suki_flush` | `SukiFlush` |
| `suki_wait` | `SukiWait` | `suki_poll_event` | `SukiPollEvent` |
| `suki_file_open` | `SukiFileOpen` | `suki_set_pixel` | `SukiSetPixel` |
| `suki_proc_create` | `SukiProcCreate` | `suki_draw_text` | `SukiDrawText` |
| `suki_mem_alloc` | `SukiMemAlloc` | `suki_get_window_rect` | `SukiGetWindowRect` |
| `suki_poweroff` | `SukiPoweroff` | `suki_reboot` | `SukiReboot` |

---

## 三、实现手法（全仓机械改名）

用 `sed -E` + `\b` 单词边界，对每个旧函数名做精确整词替换（保证 `suki_wait` 不会误伤内核
`suki_wait_set`/`suki_wait_nodes` 等内部符号，`suki_socket_send` 不会误伤 `suki_socket_sendto`）：
```bash
find kernel include user drivers -type f \( -name '*.h' -o -name '*.c' -o -name '*.S' \) \
  | xargs sed -i -E -e 's/\bsuki_obj_create\b/SukiObjCreate/g' ...  # 50 条 -e
```
替换作用于：函数声明（头文件）、函数定义（实现 `.c`）、全部调用点（`user/` 下服务与测试程序）、
以及根目录 API 文档（`SukiNative API 完整系统接口规范.md`、`SukiOS 全栈技术参考手册.md`、
`SukiOS 混合风格权限提升设计文档.md`、`SukiOS 系统文件后缀参考.md`）以保持文档一致。

替换后全局 grep 校验：50 个旧公共名在 `kernel/ include/ user/ drivers/` 中 **0 残留**
（内核内部 `suki_obj_new`/`suki_handle_*` 等仍按预期保留）。

---

## 四、验证（QEMU 无头，生产场景零 panic）

构建：`make clean && make iso disk` —— **零编译/链接错误**（证明声明、定义、调用点三处名字完全对齐）。

运行（无头，serial 落盘）：
```
qemu-system-x86_64 -machine pc -cpu qemu64 -smp 1 -m 2G -no-shutdown \
  -drive file=build/disk.img,format=raw,index=0,media=disk \
  -serial file:/tmp/suki3.ser -display none -boot d -cdrom build/SukiOS.iso
```
- **panic 检查**：`PANIC|triple fault|#GP|page fault|undefined` 命中 **0**。
- **GUI API 改名生效**：`SukiWinHello` 启动 GUI 自检 →
  `[winhello] window created id=1` / `[winhello] flushed frame to WM (OOL)` /
  `[winhello] poll loop done (non-blocking)` / `[winhello] PASS: window lifecycle complete`
  （证明 `SukiCreateWindow`/`SukiFlush`/`SukiPollEvent`/`SukiSetEventPort` 等改名后链路打通）。
- **Socket API 改名生效**：`SukiNetTest` →
  `[nettest] socket() ok fd=3` / `sendto(TFTP RRQ) nwritten=20` /
  `recvfrom() got 21 bytes from 10.0.2.2:69` / `POSIX UDP round-trip (with reply): PASS`
  （证明 `SukiSocketCreate`/`SukiSocketSendTo`/`SukiSocketRecvFrom` 改名后链路打通）。
- 其余服务（SukiDisplayServer/SukiInputServer/SukiMouseServer/SukiFsServer/SukiShell 等）与
  posixtest 自检照常，无回归。

---

## 五、改动文件清单

| 文件 | 改动 |
| :-- | :-- |
| `user/lib/suki.h` | 对象/handle API 声明 + `suki_poweroff`/`suki_reboot` 改名（含注释） |
| `user/lib/suki_native.c` | 对象/handle API 定义改名 |
| `include/sukios/net.h` | socket API 声明改名（含注释） |
| `user/lib/suki_native_net.c` | socket API 定义改名 |
| `user/lib/suki_gui.h` / `user/lib/suki_gui.c` | GUI API 声明/定义改名 |
| `user/`（display_server / input_server / mouse_server / shell / winhello / 各测试程序） | 调用点改名 |
| `SukiNative API 完整系统接口规范.md` 等根目录 `.md` | 文档同步改名 |

---

## 六、后续

- 内核内部 SukiNative 实现符号（`suki_obj_new`/`suki_handle_*` 等）与 POSIX 层、`mach_msg_*` 按规范
  维持现状，本次未动（属非用户态 API 的内部实现与底层 ABI）。
- 未做：权限提升子系统实装、启动动画实装、SukiLogon/SukiDesktopManager 实质化（均已在 step79 预留）。
