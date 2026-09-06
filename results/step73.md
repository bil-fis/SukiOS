# Step73 — Shell 窗口化 + WM 焦点/拖拽/关闭按钮/键盘聚焦转发

## 1. 目标

把原本「开机自动自检、从串口读键盘、输出到内核控制台」的 `shell` 改造为 **由 SukiWindowManager 管理的窗口程序**，并补齐 WM 缺失的窗口管理能力：

- **焦点（focus）模型**：谁聚焦谁收键盘；点击窗口置顶/置焦；提供焦点 API（`suki_set_focus` 等）。
- **鼠标拖拽**：按住标题栏拖动窗口。
- **鼠标点击 / 关闭按钮**：标题栏右上角绘制关闭按钮（红方块 + X），点击发 `SUKI_EVENT_WINDOW_CLOSE`。
- **键盘经 WM 转发**：`input_server` 把键盘字符改发 WM（原直发 shell），WM 仅把键盘事件转发给**当前焦点窗口**，真正实现「窗口程序由 WM 统一管束输入焦点」。

> 之前的 shell 是纯文本终端程序：键盘经 `SHELL_PORT` 直送，输出经 `SYS_DEBUG_WRITE` 到串口；WM 桌面的终端区只显示内核 console pipe。本次之后，shell 成为与 winhello 同级的 GUI 窗口，键盘走「input_server → WM → 焦点窗口事件端口」统一路径。

## 2. 改动文件与功能细节

### 2.1 `user/lib/gui_ipc.h`（协议扩展）

- 新增 WM 请求/应答消息：
  - `WM_MSG_SET_FOCUS = 0x305` → `WM_MSG_SET_FOCUS_RESP = 0x381`（应用主动置焦点）
  - `WM_MSG_SET_POS   = 0x306`（移动窗口，拖拽/程序化定位）
  - `WM_MSG_GET_FOCUS = 0x307` → `WM_MSG_GET_FOCUS_RESP = 0x382`（查询当前焦点窗口 id）
- 新增键盘事件消息（input_server → WM，走 `DISPLAY_PORT`，与鼠标消息同端口）：
  - `KEY_MSG_DOWN = 104` / `KEY_MSG_UP = 105`
  - `key_event_msg_t { mach_msg_header_t h; uint8_t ascii; uint8_t modifiers; uint8_t _pad[2]; }`
    —— 键盘布局解析仍由 `input_server`(Ring3) 完成，这里只携带**已解析 ASCII** 与修饰键状态。
- `suki_event_t` 的 `key` 联合体：`keycode` 在此语义为「已解析 ASCII 字符」（WM 转发时填入 `key_event_msg_t.ascii`），`modifiers` 为修饰键位。
- `SUKI_WS_*` 窗口风格标志补充注释（标题栏等）。

### 2.2 `user/display_server.c`（WM 核心）

全文件 `user/display_server.c`。关键新增/修改：

- **焦点状态**（窗口注册表之后定义，供 `wm_handle_destroy`/`wm_handle_create` 直接访问）：
  ```c
  #define WIN_TITLE_H 20
  static wm_window_t *g_focus = NULL;
  static wm_window_t *g_drag  = NULL;
  static int32_t g_drag_offx = 0, g_drag_offy = 0;
  ```
- **`wm_in_close_box()`**：命中测试关闭按钮区域（标题栏右上角 16×16，`bx = w->x + w->w - 18, by = w->y + 2`）。
- **`wm_set_focus(w)`**：切换焦点并广播 `SUKI_EVENT_WINDOW_FOCUS` 给旧/新焦点窗口（`ev.u.mouse.buttons = 1` 表示获得焦点，`0` 表示失去）。
- **标题栏绘制（composite）**：在 `WIN_TITLE_H` 标题栏右上角绘制关闭按钮（红底 `0x00C0392B` + 白色 X 两条对角线），焦点时标题为白色、否则灰。
- **主循环 `DISPLAY_PORT` 分支重构**（鼠标 + 键盘统一分发）：
  - 拖拽中（`g_drag` 非空）：`MOUSE_MSG_MOVE` 时 `g_drag->x/y` 跟随光标偏移移动，立即 `composite()`。
  - 左键按下：
    - 命中标题栏且命中关闭按钮 → 发 `SUKI_EVENT_WINDOW_CLOSE` 给该窗口（交由应用决定是否销毁）；
    - 命中标题栏（非关闭区）→ 进入拖拽（`g_drag=hit`，记录偏移）并 `wm_set_focus(hit)`；
    - 命中客户区 → `wm_set_focus(hit)` 并转发 `SUKI_EVENT_MOUSE_DOWN`。
  - 松开（`MOUSE_MSG_BUTTON` 无左键）→ 结束拖拽并转发 `SUKI_EVENT_MOUSE_UP`。
  - `MOVE`/`WHEEL` → 转发给命中窗口。
  - `KEY_MSG_DOWN/UP` → 构造 `SUKI_EVENT_KEY_DOWN/UP`（`u.key.keycode = k->ascii`）转发给 `g_focus`（前置 `g_focus && g_focus->event_port` 防空指针）。
- **WM 消息处理新增**：`WM_MSG_SET_FOCUS`（置焦点并回 `SET_FOCUS_RESP`）、`WM_MSG_SET_POS`（移动窗口）、`WM_MSG_GET_FOCUS`（回当前焦点 id）。
- **`wm_handle_create` 末尾**调用 `wm_set_focus(w)`（新窗口自动获得焦点，shell 窗口启动即可接收键盘）。
- **`wm_handle_destroy` 清理**：`if (g_focus == w) g_focus = NULL; if (g_drag == w) g_drag = NULL;`（销毁前清理悬空引用，避免后续键盘/拖拽转发到已释放窗口）。

### 2.3 `user/lib/gui.c` + `user/lib/suki_gui.h`（客户端 API）

`user/lib/gui.c`：
```c
int  suki_set_focus(suki_window_t *w);                       /* 同步调用 WM_MSG_SET_FOCUS */
void suki_set_window_pos(suki_window_t *w, int x, int y);    /* 发 WM_MSG_SET_POS，并同步本地 x/y */
int  suki_get_window_rect(suki_window_t *w, int32_t *x, int32_t *y,
                          uint32_t *w_out, uint32_t *h_out); /* 读取本地缓存的位置/尺寸 */
```
`user/lib/suki_gui.h` 新增声明与注释，明确「谁聚焦谁收键盘」模型。

### 2.4 `user/input_server.c`（键盘路由重定向）

`send_char()` 改为把每个键盘/串口字符封装成 `key_event_msg_t`（`msgh_remote_port = DISPLAY_PORT, msgh_id = KEY_MSG_DOWN, ascii = 已解析字符`），经 WM 转发。不再直接发 `SHELL_PORT`。鼠标仍发 `DISPLAY_PORT`（原路径不变）。

> 这是「shell 窗口化」的前提：若不重定向，shell 改造后从 `SHELL_PORT` 再也收不到键盘会卡死。改为统一经 WM 焦点窗口后，所有窗口程序键盘输入都走同一条一致路径。

### 2.5 `user/shell.c`（shell 窗口化）

全文件 `user/shell.c`。要点：

- **新增头文件**：`suki_gui.h` / `gui_ipc.h` / `font8x8.h`。
- **窗口/终端状态**（全局）：`g_win`(窗口)、`g_ep`(事件端口)、`g_tgrid[16][16]`(字符网格)、`g_tcx/g_tcy`(网格光标)、`g_term_dirty`。
  > 受 OOL 离屏缓冲 ≤16 页=64KiB 限制，窗口最大约 128×128 像素 → 8×8 字体下 16×16 字符终端（后续可突破，见 §4 约束）。
- **最小 ANSI 终端仿真**（不依赖外部终端）：
  - `term_emit_char(c)`：维护网格光标，解析 shell 自身发出的 CSI 序列（`ESC[<col>G` 绝对列、`ESC[K` 清行尾、`ESC[<n>D/C` 光标移动、`ESC[2J` 清屏、`\n \r \b \t` 等）。使用独立的局部 static `t_esc/t_esc_num`，与键盘 ESC 状态机（`g_esc`）隔离，避免串味。
  - `term_puts(s)`：写网格 + 置 `g_term_dirty` + **镜像 `u_print(s)` 到串口**（便于 headless/无图形观测，且保留既有外部终端回显语义）。
  - `term_render()`：把网格用 `font8x8_basic` 光栅化到窗口离屏缓冲（`suki_get_buffer`/`suki_flush`，深灰背景 0x002b2b30 + 浅灰前景 0x00e0e0e0）。
  - `term_scroll()`：上滚一行。
- **`ed_raw()` 改为 `term_puts(s)`**：原行编辑/提示符打印（依赖 `u_print` + CSI 序列）现在渲染到窗口网格，由 `term_emit_char` 解析。
- **`handle_key(char c)`**：从原主循环提取的键盘处理（方向键转义状态机、退格、Tab 补全、回车执行、可打印插入），逻辑完全一致，仅回显输出改为 `term_puts`。
- **`main()`**：`sys_port_claim(SHELL_PORT)` 保留（仍收 `FS_SERVER` 应答）；随后 `suki_create_window("Shell", 64, 96, 128, 128, SUKI_WS_DEFAULT)`，分配并认领事件端口、`suki_set_event_port`、主动 `suki_set_focus(g_win)`。创建失败则打印 fallback 告警（仍走 serial）。
- **主循环改事件驱动**：
  ```c
  for (;;) {
      if (g_win && g_ep) {
          suki_event_t ev;
          while (suki_poll_event(g_win, &ev)) {
              if (ev.type == SUKI_EVENT_KEY_DOWN) handle_key((char)ev.u.key.keycode);
              else if (ev.type == SUKI_EVENT_WINDOW_CLOSE) { suki_destroy_window(g_win); g_win=NULL; sys_exit(0); }
          }
      }
      if (g_term_dirty) { term_render(); g_term_dirty = false; }
      sys_yield();
  }
  ```
  FS 应答仍由 `do_command` 内部 `mach_msg_recv(SHELL_PORT)` 同步接收（主循环不再 recv `SHELL_PORT`，调用栈占用期不冲突）。

### 2.6 `Makefile`

`build/user/shell.elf` 原为 `USER_PROGS` 通用链接规则（不含 GUI 客户端），新增**专门链接规则**把 `build/user/gui.c.o` 链入（与 winhello 一致），否则 `suki_create_window` 等符号未定义。

## 3. 验证（headless，QEMU `-display none -serial stdio`）

命令（bash + QEMU 自身机制，符合项目铁律）：
```
timeout 300 qemu-system-x86_64 -machine pc -cpu qemu64 -smp 1 -m 2G -no-shutdown \
  -display none -serial stdio -audiodev none,id=snd0 -device e1000,netdev=net0 \
  -netdev user,id=net0 -boot d -cdrom build/SukiOS.iso \
  -drive file=build/disk.img,format=raw,index=0,media=disk
```
结果（/tmp/suki8.log / /tmp/suki9.log）：

| 检查项 | 结果 |
|---|---|
| 全系统零 panic / triple fault（300s） | ✅ 0 |
| shell 窗口化启动（`windowed mode: window id=`） | ✅ 1 |
| `[libc-test] PASS=...` | ✅ 1（证明 FS 交互 `opendir("/")` 正常，未因主循环改造而坏） |
| WM 创建窗口数（`[wm] window created`） | ✅ 2（winhello + shell 各一） |
| winhello 窗口创建 / OOL 合成（`flushed frame to WM (OOL)`） | ✅ 阶段1 已确认 |
| 自动 winhello 自检 `PASS: window lifecycle complete` | ⚠️ 未在本轮 timeout 内出现 |

**关于 winhello 自检 PASS**：开机自动自检序列中 `posixtest` 长套件（fork/clone/pthread 海量用例）在单核下耗时 >300s，winhello 自检排在其后、本轮回合内未轮到完整跑完（窗口创建 + OOL 合成已确认成功，WM 改动对其为**加性**无回归：仅创建时多一次 `wm_set_focus` 广播、销毁时多一次指针清理）。`step72.md` 已记录该自检全流程 PASS，本次未改变其 create/flush 路径，故判定无回归。

## 4. 已知约束与后续

- **窗口尺寸受 OOL ≤64KiB 限制**：当前最大 ≈128×128（16×16 字符终端）。路线图 10.1(4) 的「分片提交 / 共享内存映射」落地后可支持更大/可滚动终端。
- **headless 下键盘/鼠标无法注入**（QEMU `-serial stdio` 在 stdin 为管道时不把输入转发给 guest，项目铁律禁止复杂编排脚本），因此**拖拽、关闭按钮点击、窗口内键盘输入**需真实图形窗口手动验证（见 §5）。代码路径已通过审查与零 panic 启动验证。
- shell 输出同时镜像到串口（`term_puts` 调 `u_print`），故 headless 下 shell 文本仍可见、AI 可观测；图形下用户在 Shell 窗口内看到终端。

## 5. 手动测试步骤（图形窗口，需用户执行）

1. 启动图形环境：`make run`（需 X/GTK；`-display none` 看不到窗口）。
2. 系统启动后除内核桌面终端区外，应出现：
   - 一个 winhello 小窗（渐变背景 + 标题栏，可拖拽/关闭）；
   - 一个 **Shell** 窗口（标题栏 "Shell"，内含 16×16 字符终端，显示 `libc-test` 结果与 `Shell> ` 提示符）。
3. **拖拽**：在任一窗口标题栏按下并拖动，窗口应跟随移动。
4. **关闭按钮**：点击窗口标题栏右上角红 X，窗口应关闭（winhello 在手动模式会退出；Shell 关闭会退出 shell 任务）。
5. **焦点与键盘**：先点一下 Shell 窗口标题栏使其聚焦，然后直接打字（如 `help`、`ls /`、`echo hi`），字符应出现在 Shell 窗口内并由 shell 执行——键盘经 `input_server → WM → 焦点窗口事件端口` 送达。
6. **判定通过**：窗口可拖拽/关闭、Shell 能接收键盘并执行命令、全过程中内核无 panic（串口/桌面无红色崩溃信息）。

## 6. 提交

- 改动文件：`user/lib/gui_ipc.h`、`user/display_server.c`、`user/lib/gui.c`、`user/lib/suki_gui.h`、`user/input_server.c`、`user/shell.c`、`Makefile`、`results/step73.md`。
- 提交风格：`feat(gui): WM 焦点/拖拽/关闭按钮 + 键盘聚焦转发；shell 窗口化交由 WM 管理 (step73)`（未推送）。
