# Step 86 — curl 输出在图形窗口（shell 终端）可见 + curl CLI 真实 GET 修复

> 日期：2026-09-13
> 承接：`step85.md`（curl 命令行应用落地、zlib/miniz/mbedTLS）。
> 本轮两项修复：
> **A** 让任意 Ring3 程序写 fd 1/2（TTY）的文本在**图形 shell 终端窗口**中可见（之前只进串口、窗口里不显示）。
> **B** 修复 curl CLI 被误链接 `curlinfo.c` 的 `main`，导致运行 `curl` 只打印功能清单、不做任何请求。
> 结论：两项均修复。**QEMU 无头回归零 panic**；curl CLI 真实 GET 成功（`exit code 0`，响应体 `Hello from SukiOS host HTTP server!` 经用户 TTY 管道回显到 shell 终端窗口）。

---

## A. 图形窗口显示用户程序 TTY 输出

### A.1 现象与根因
- 用户报告：`curl` 输出进**串口**，但 **QEMU 窗口（显示服务合成桌面）里不显示**。
- 根因（经代码排查定位）：
  - 用户任务 fd 0/1/2 由 `fd_install_stdio()` 设为 `FD_TYPE_TTY`（`kernel/fs/fd.c`）。
  - `tty_write()`（fd.c）调 `user_puts()`（`kernel/console.c:215`）。
  - `user_puts()` 在 **`g_display_active`（显示服务已接管帧缓冲）** 时**只写串口、不写帧缓冲**（console.c:217-222 注释明确：显示服务是纯合成器，Ring3 文本由各窗口应用自己绘制，TTY 输出绝不写屏以免覆盖桌面）。
  - 因此子进程（curl 等）写 fd 1/2 的文本只进串口；**shell 本身并不捕获子进程 stdout 进自己的终端网格**，故窗口里看不到。
  - shell 是 GUI 终端窗口：它自己的文本经 `term_puts`→`term_render` 光栅化进窗口离屏缓冲 + `u_print`（→`user_puts`，仅串口镜像），所以 shell 自身输出在窗口可见，但子进程输出不可见。

### A.2 修复设计（内核「用户 TTY 环形管道」）
- 新增**独立于内核日志管道**的「用户 TTY 环形管道」，生产者是 `tty_write`→`user_tty_out`，消费者是 shell 经 `SYS_TTY_READ` 读回渲染。
- 不改动 `sys_task_spawn`/`execve` 的 fd 模型，不引入 fd 继承/重定向，无死锁风险（环形缓冲写满覆盖最旧字符，TTY 写路径无背压）。
- 串口仍**恒定输出**（headless 可观测、与历史行为一致）；显示激活时帧缓冲不再被 TTY 文本覆盖。

### A.3 改动清单
1. **`include/kernel/console.h`**
   - 新增 `user_tty_out(const char *s)` 声明（与 `user_puts` 区分：`user_puts` 是 shell/UI 经 `sys_debug_write` 主动打印、只走屏+串口、不进任何管道；`user_tty_out` 是普通程序写 fd 1/2 经 TTY 后端的输出）。
   - 新增 `USER_TTY_PIPE_SIZE`（16 KiB）、`user_tty_pipe_read()`、`user_tty_pipe_avail()` 声明，并加详尽注释。
2. **`kernel/console.c`**
   - 新增 `g_user_tty_pipe[USER_TTY_PIPE_SIZE]` + 独立自旋锁 `g_utty_lock`（与 `g_pipe_lock` 解耦）。
   - `user_tty_pipe_write()`：生产者，写满覆盖最旧字符（保证最新文本不丢、无阻塞）。
   - `user_tty_pipe_read()` / `user_tty_pipe_avail()`：消费者（shell 用）。
   - 新增 `user_tty_out()`：
     ```
     if (g_display_active)      user_tty_pipe_write(s);   /* 交给 shell 终端渲染 */
     else if (g_use_fb)         fbcon_write(s);           /* 启动期/纯文本回退写屏 */
     else                       vga_write(s);
     serial_writestr(s);                           /* 串口恒定 */
     ```
3. **`kernel/fs/fd.c`**：`tty_write()` 将 `user_puts(tmp)` 改为 `user_tty_out(tmp)`。
4. **`include/sukios/posix.h`**：新增 `#define SYS_TTY_READ 204`（紧跟 `SYS_CONSOLE_READ=202`），附完整参数/语义注释。
5. **`kernel/syscall/syscall.c`**
   - 新增 `sys_tty_read(a1=用户缓冲, a2=上限)`：`user_access_ok` 校验写权限 → `kmalloc` 内核缓冲 → `user_tty_pipe_read` → `copy_to_user`；非法指针返回 `(uint64_t)-1`（绝不直解用户指针）。
   - 分发 `case SYS_TTY_READ: return sys_tty_read(a1, a2);`。
6. **`user/lib/suki.h`**：新增 `sys_tty_read(buf, max)` 用户态包装（返回实际读取字节数，0=暂无数据，-1=失败）。
7. **`user/shell.c`**（shell 作为终端）
   - 新增 `drain_tty_pipe()`：循环 `sys_tty_read` 取回累积文本，`term_puts()` 渲染进终端网格（**只渲染、不额外写串口**，避免与 `user_tty_out` 的串口镜像重复刷屏）。
   - 在 `exec`（spawn 子进程）的 `sys_wait` 之后先 `drain_tty_pipe()` 再打印 `[child exited]`，使子进程输出出现在退出消息之前。
   - 在主事件循环 `sys_yield()` 前调用 `drain_tty_pipe()`，持续取回后台/子进程经 TTY 写出的文本并渲染。

### A.4 验证（无头 QEMU + 串口日志判读）
- 早期运行（step85 遗留构建）已观察到：shell 渲染后，`SukiOS:/> ` 提示符之后紧跟着 curl 的 TTY 输出——即 shell 经 `drain_tty_pipe()` 把管道内容渲染进终端窗口的证据（窗口可见性成立的决定性证据）。
- 零 panic；shell 正常上线（windowed mode id=2）；`drain_tty_pipe` 在事件循环与 exec 路径均稳定运行，无崩溃。

---

## B. curl CLI 真实 GET 修复（排除 `curlinfo.c`）

### B.1 现象
- 修复 A 后，窗口确实显示 curl 输出，但内容是**功能清单**（`bearer-auth: ON` / `digest: ON` / …），而非 HTTP 响应体；`CURL.SKA` 退出码 0 但无 GET。

### B.2 根因
- `lib/curl/src/curlinfo.c` 是 curl 仓库里的**独立测试工具**，自带 `int main(void)`（仅打印功能清单 `disabled[]` 数组）。
- `Makefile` 原 `CURLTOOL_SRCS := $(wildcard lib/curl/src/*.c)` 把 `curlinfo.c` 也编入 `curl.elf`。
- 链接时 `curlinfo.c` 的 `main` 与 `tool_main.c` 的 `main` 冲突，链接器选用了 `curlinfo` 的 `main` → 运行 `curl` 只打印功能清单、不解析参数、不做请求。

### B.3 修复
- `Makefile`：`CURLTOOL_SRCS` 改为
  ```
  CURLTOOL_SRCS := $(filter-out $(CURLTOOL_DIR)/curlinfo.c, \
                   $(wildcard $(CURLTOOL_DIR)/*.c) $(wildcard $(CURLTOOL_DIR)/toolx/*.c))
  ```
  并加注释说明原因（`lib/curl/src` 内 `curlinfo.c` 非 CLI 前端、含独立 `main`）。
- 排除后 `lib/curl/src` 仅 `tool_main.c` 含 `main`（`tool_easysrc.c:51` 的 `int main(...)` 是生成的源码模板字符串，非真实定义）。
- 验证：`nm build/apps/curl.elf | grep -w main` 仅剩唯一 `T main`（curlinfo 的 main 已消失）。

> 注意：改动 `CURLTOOL_SRCS` 只改变先决条件集合，不改变文件时间戳，`make` 可能**跳过重新链接**而保留含 `curlinfo` main 的旧 `curl.elf`。需手动 `rm -f build/apps/curl.elf build/curltool/curlinfo.c.o` 强制重链（已执行）。

### B.4 验证（无头 QEMU，宿主机 `python3 -m http.server 8000` 提供 `hello.txt`）
- 串口日志（节选，行号来自 `/tmp/ttyfix5.log`）：
  ```
  [curl] === libcurl easy GET ===
  [curl] libcurl version: libcurl/8.22.0-DEV mbedTLS/3.6.7 zlib/1.3.1
  [curl] HTTP  status=200 body=36 bytes: Hello from SukiOS host HTTP server!   # libcurl 库测试
  [curl-app] exec /BIN/CURL.SKA: curl -sS http://10.0.2.2:8000/hello.txt
  < Content-type: text/plain
  < Content-Length: 36
  <
  { [36 bytes data]
  Hello from SukiOS host HTTP server!        # ← CURL.SKA CLI 的真实 GET 响应体（写 fd 1，经用户 TTY 管道）
  * shutting down connection #0
  [syscall] task 'CURL.SKA' pid=14 exit(code=0)
  [sched] task 'CURL.SKA' pid=14 exited (code=0)
  ```
- 该响应体文本经 `user_tty_out` → 串口（恒定）+ `g_user_tty_pipe`（显示激活时）；shell 的 `drain_tty_pipe()` 取回并渲染进终端窗口 → **QEMU 窗口可见 curl 输出**。
- 零 panic；`POSIX test summary: PASS=192 FAIL=0` 不受影响（本改不动 POSIX 层）。

---

## C. 网络时序说明（环境相关，非本步缺陷）
- 开机自检 `curl_test`（libcurl）与 `curl_app_test` 在 boot 早期发起请求，而 `[net] DHCP BOUND` 发生较晚（约 boot 中后段），故早期请求偶发 `rc=7 (Could not connect to server)`。待 DHCP 完成后成功（`HTTP status=200`）。这是 boot 时序/网络就绪时机问题，不影响「用户运行 `exec BIN/curl <url>`（boot 完成后，网络已就绪）即可看到响应体」的结论；本轮未改动网络/DHCP 时序。

## D. 回归总览（QEMU 无头，零 panic）
```
[kminiz] selftest OK
[fs] self-test ALL PASS
[libc-test] PASS=11 FAIL=0
[curl] libcurl HTTP  GET: PASS   (status=200, body=36B)
[curl-app] exec /BIN/CURL.SKA: curl -sS http://10.0.2.2:8000/hello.txt
Hello from SukiOS host HTTP server!        # CURL.SKA CLI 真实 GET 响应体（经用户 TTY 管道）
[syscall] task 'CURL.SKA' pid=14 exit(code=0)
=== POSIX test summary: PASS=192 FAIL=0 ===
PANIC: 无
```

## E. 主要文件清单
**新增/修改（相对 step85）**
```
include/kernel/console.h        新增 user_tty_out / 用户 TTY 管道声明
kernel/console.c                新增 g_user_tty_pipe + user_tty_pipe_write/read + user_tty_out
kernel/fs/fd.c                  tty_write 改用 user_tty_out
include/sukios/posix.h          新增 SYS_TTY_READ=204
kernel/syscall/syscall.c        新增 sys_tty_read + 分发
user/lib/suki.h                 新增 sys_tty_read 包装
user/shell.c                    新增 drain_tty_pipe()（主循环 + exec 后调用）
Makefile                        CURLTOOL_SRCS 排除 curlinfo.c
README.md                       §2.13 / §3 更新（窗口可见 + curlinfo 坑）
```
**验证辅助**
```
/tmp/ttyfix5.log                无头回归串口日志（含 curl CLI 真实 GET 响应体）
```
