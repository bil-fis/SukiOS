# step113 — UI 圆角"黑边朝内"修复 + USB 枚举回归核查

## 0. 用户反馈（两条）
1. **圆角渲染反了，黑边朝内**：窗口四角出现朝向窗口内部的黑色边。
2. **USB 问题再次出现，请检查**：附启动日志显示 `usb keyboard ready` / `usb mouse ready`
   （枚举成功），但仍报告 usb 问题重现。

## 1. 圆角"黑边朝内"根因与修复

### 1.1 根因
本系统的图形架构是**应用自绘窗口**：每个窗口持有自己的像素缓冲 `w->pixels`，由应用
（libsui）绘制全部内容，包括**圆角**——`user/libsui/src/sui_core.c::sui_canvas_draw_background()`
与 `sui_canvas_fill_rounded_rect()` 会把窗口四角填成**窗口背景色**（圆角外观在应用层完成）。

显示服务 `user/display_server.c` 收到应用像素后，本应整块拷到帧缓冲。但：
- **旧实现** `wm_round_corners()` 把窗口四角圆外像素**强制填黑**；
- **step112 实现** `wm_corner_cut()` 把四角圆外像素**挖洞透出黑桌面**。

两者都**覆盖了应用已画好的圆角四角背景色**，导致窗口四角出现黑色（朝窗口内部侵蚀），
正是用户看到的"黑边朝内"——方向/语义反了：圆角本应由应用负责，display_server 不该
再动四角。

### 1.2 修复
display_server 改为**完全不做圆角挖洞/填黑，直接整块拷贝应用像素**（应用自绘圆角自然生效）：
- 删除全局 `g_paint_win` 与 `wm_corner_cut()` 前向声明；
- `fill_clip()` 装饰填充去掉圆角挖洞跳过（边框/标题栏整块填充）；
- 删除 `wm_corner_cut()` 定义；
- `wm_paint_window()` 像素拷贝恢复整块 `memcpy`（不再逐像素挖洞）；
- 更新相关注释（圆角由 libsui 在像素缓冲中绘制）。

> 注：display_server 仍叠加 1px 边框 + 标题栏 + 三色交通灯（iSuki 窗口装饰），这些绘制在
> 窗口最外边缘；应用圆角背景位于窗口四角客户区，二者不再冲突（无黑边）。边框在四角为
> 方角，属装饰细节，不影响"黑边朝内"的修复。

## 2. USB 枚举回归核查

### 2.1 枚举稳定性验证（QEMU headless + serial，仅 bash 自带机制）
- **含 Hub 拓扑**（kbd 根端口 + mouse 经 usb-hub 下行，最严格，需多轮枚举）：3 次运行，
  `enum failed = 0`，`keyboard ready` / `hub ready` / `mouse ready` 稳定早于 `display-server`。
- **无 Hub 直连拓扑**（kbd + mouse 直连根端口）：3 次运行，`enum failed = 0`，
  `device addr=1 keyboard ready` / `device addr=3 mouse ready` 稳定。
- 共 **6 次启动全 0 `enum failed`**，与用户所附日志一致（枚举成功）。

### 2.2 代码确认
`kernel/drivers/usb/usb_core.c::usb_init()` 中 step111 的同步首次枚举修复仍在：
`g_sleep_ok = false; uhci_set_can_sleep(false); for (12) usb_poll();` 其后恢复 msleep
轮询。**未引入回归**。

### 2.3 结论与待用户确认
- **USB 枚举本身无回归**（kbd 与 mouse 总线枚举确定性成功，6/6）。
- 用户报告"usb 问题再次出现"更可能指**鼠标光标不跟随移动**（归因为 usb，因鼠标是 USB
  设备）。该行为由 display_server 渲染，step112 已将其改为"旧/新光标矩形 `mark_dirty` +
  增量重绘 + 末尾 `draw_cursor_at` 画新光标"——经代码核查逻辑正确（旧位置重绘恢复下层窗口，
  新位置绘制光标）。请烧录**最新 ISO**（本步已 `make iso`）实测确认。
- 若"usb 问题"确为**枚举失败**（鼠标与键盘同时失灵），请附上失败时的串口日志——当前
  两种拓扑均无法复现失败，需据日志定位（如不同 QEMU 时序）。

## 3. 验证
- `make iso` 编译通过，`user/display_server.c` 无 lint 错误（圆角挖洞代码已全删，无悬空引用）。
- 启动日志无 panic / 崩溃；窗口创建正常（`[wm] window created`）。
- 圆角视觉（黑边是否消除）、鼠标移动（光标是否跟随）属图形/交互验证，headless 不可测，
  请在图形窗口内实测（见 step112 §4 命令）。

## 4. 关键文件
| 项 | 位置 |
|---|---|
| 圆角修复（整块拷贝应用像素） | `user/display_server.c::wm_paint_window()` / `fill_clip()` |
| 应用自绘圆角（窗口背景色） | `user/libsui/src/sui_core.c::sui_canvas_draw_background()` / `sui_canvas_fill_rounded_rect()` |
| USB 同步枚举（无回归） | `kernel/drivers/usb/usb_core.c::usb_init()`（step111） |

## 5. 修改文件清单
- `user/display_server.c`：移除圆角挖洞（`g_paint_win` / `wm_corner_cut` / `fill_clip` 跳过 /
  `wm_paint_window` 逐像素挖洞），恢复整块 `memcpy` 应用像素（圆角交还 libsui 自绘）。

## 6. 已知/后续
- 若希望 display_server 的 1px 边框也随应用圆角呈圆弧形（而非当前方角），可让 `fill_clip`
  在绘制边框时跳过应用圆角半径处的圆外像素；需对齐 libsui 的 `SUI_RADIUS_WINDOW` 与
  display_server 的 `WIN_RADIUS`（当前均为 12）。属视觉增强，非本次黑边修复必需。
- 若"usb 问题"实测仍为鼠标光标不动，需进一步排查 mouse_server→display_server 的事件链路
  或光标绘制（当前逻辑核查无误），请附具体现象/日志。
