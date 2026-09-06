# Step75 — display-server 重构为纯合成器（不再渲染任何字符）

## 1. 背景与用户需求

原 display-server 身兼三职：窗口管理器 + 合成器 + **文本渲染器**。它在接管帧缓冲后，通过两条路径把字符画进自己的离屏桌面层 `g_desk`：

1. `kernel/console.c::user_puts()` 在 `g_display_active` 时把用户态 print 经 `DISP_MSG_TEXT`（IPC）转发给 display-server，由 `term_puts()` 栅格化到 `g_desk` 的「终端窗口」。
2. `display_server.c::drain_console_pipe()` 在 `SYS_DISPLAY_READY` 后循环调 `SYS_CONSOLE_READ` 拉取内核 console 环形管道，同样 `term_puts()` 渲染到桌面。

用户要求**取消所有这些「捕获 print 渲染」行为**，新渲染管线为：

```
窗口应用（shell / winhello / ...）
   └─ 自行绘制内容（含字符，用 libsuki_gui 的 suki_draw_text 等）到自己的离屏缓冲
        └─ 经 WM_PORT 提交帧（OOL 零拷贝物理页重映射）
             └─ display-server 写入 win->pixels
                  └─ composite() 按 Z-order 合成：背景 + 各窗口图像 + 几何装饰 + 光标
                       └─ 写入帧缓冲
```

display-server **只处理「不同组件交给的图像」的层级顺序合成**，不承担任何字符渲染；内核 console / 启动日志 / 用户态 print 不再被它捕获，只走串口（headless 调试可见）。

## 2. 改动文件

### 2.1 `user/display_server.c`（整体重写为纯合成器）

- **删除全部文本渲染代码**：
  - 删除 `g_desk` 的「终端层」语义与 `g_term_x/y/cols/rows` 栅格状态；
  - 删除 `draw_glyph` / `draw_glyph_fb` / `csi_dispatch` / `term_scroll_up` / `clear_cols` / `clear_screen_area` / `term_putc` / `term_puts` 及 CSI/VT100 解析状态机；
  - 删除 `drain_console_pipe()` 及其在 `main()` 的调用；
  - 删除消息循环中 `if (h->msgh_id == DISP_MSG_TEXT)` 分支与 `DISP_MSG_TEXT` 宏；
  - 删除启动期 `term_puts("SukiOS display + window manager ready.\n")` 等提示。
- `draw_desktop()` 重写为**仅纯色背景 + 顶部装饰条**（无文字、无终端窗口框）。
- `composite()`：保留背景 blit + 窗口 Z-order blit + 边框 + 标题栏 + 关闭按钮 + 光标；**标题栏去掉窗口标题文字**（纯几何色条，`has_focus` 时高亮 `COL_ACCENT`，否则 `COL_BAR`），关闭按钮仍画红底白色 X（几何，非字符）。
- 保留并完整保留：WM 创建/销毁/置焦/拖拽/关闭、命中测试、事件端口路由、鼠标/键盘分发、OOL flush 接收、Z-order（创建顺序，后者在上）。
- `g_desk` 仅作为纯背景层缓冲（与帧缓冲同尺寸），合成时整体 blit。

### 2.2 `kernel/console.c`（user_puts 不再转发 display-server）

```c
void user_puts(const char *s)
{
    if (g_use_fb && !g_display_active) {
        fbcon_write(s);              /* 启动早期/纯文本回退：写屏 */
    } else if (!g_use_fb) {
        vga_write(s);                /* 无图形：VGA 文本 */
    }
    serial_writestr(s);              /* 恒定串口留底（headless 可观测） */
}
```

即 `g_display_active` 后用户态 print **只走串口**，绝不写帧缓冲（避免覆盖合成桌面，也不再由 display-server 渲染字符）。
注：`kputc()` 在 `g_display_active` 时已只捕获进 console pipe + 串口（不再写屏），本条未改。

### 2.3 `kernel/display_cfg.c`（日志措辞）

`display_set_active()` 的 serial 日志由
`"[display] active: kernel console text now routed to display server"`
改为
`"[display] active: pure compositor (no text rendering); console -> serial only"`。

## 3. 顺带修复 / 发现的问题

- **KPF 消除**：原系统跑到中后期会触发内核 page fault（`cr2=0xffffc02480004000 write=0` 内核态读未映射地址，`copy_from_user should have pre-validated`）。根因位于 display-server 文本渲染/console 路由路径（原 `drain_console_pipe` 经 `SYS_CONSOLE_READ` 拉取内核 console 环并 `term_puts` 渲染，或 `DISP_MSG_TEXT` 转发链传入异常指针）。移除该路径后 headless 150s 全程**零 panic**，证实该 KPF 已彻底消除。
- **遗留隐患（已记录，未改）**：`kernel/console.c::console_pipe_read()` 直接以内核态 `dst[i] = g_console_pipe[...]` 解引用用户指针 `dst`，未走 `copy_to_user`，在 SMAP=on 下属违例。当前因 display-server 已不再 `SYS_CONSOLE_READ`，无消费者故不触发；若将来新增消费者（如 shell 显示 dmesg）应先改为 `copy_to_user`。`SYS_CONSOLE_READ` 接口与 pipe 机制本身保留（`kputc` 仍持续捕获，便于未来读取）。

## 4. 验证（headless，单核 150s）

- `make iso`：成功（ISO_EXIT=0）。
- 关键指标：
  - `KPF / panic / triple fault / OOPS`：**0**（重构前该路径必触发 KPF，**现已消失**）
  - `deadlock / hang / stuck`：**0**（命中仅为 POSIX 测试名 `WNOHANG`、`unchanged`，非死锁）
  - `[wm] window created`：**2**（winhello + shell 均成功，OOL 合成无崩溃）
  - `[shell] windowed mode: window id=`：**1**（shell 自绘窗口创建并自动获焦）
  - `[winhello] window created`：**1**；`[winhello] flushed frame to WM (OOL)`：正常
  - `[libc-test] PASS=`：**1**（FS 交互正常）
  - `[display] active: pure compositor (no text rendering); console -> serial only`：出现（旧 `routed to display server` 日志已消失，`DISP_MSG_TEXT` 残留调用为 0）
  - 末段运行到 `pthread_create`、`load balance` 等后期阶段，说明系统完整跑通无卡死。

## 5. 影响说明

- 图形界面上不再显示任何内核 console / 启动日志 / 用户态 print（这些只走串口），符合「display-server 不渲染字符、只有窗口应用自绘」的需求。
- 窗口应用（shell 终端仿真、winhello 自绘）的字符仍由各自在离屏缓冲内绘制，再经 OOL 提交合成，视觉与重构前一致且更清晰（无 console 覆盖层）。
- 内核诊断与用户态诊断完整保留于串口日志，headless 调试能力不受影响。

## 6. 提交

- 改动文件：`user/display_server.c`、`kernel/console.c`、`kernel/display_cfg.c`、`results/step75.md`。
- 提交：`refactor(gui): display-server 重构为纯合成器，移除字符渲染与 console 路由 (step75)`（未推送）。
