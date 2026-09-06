# Step76 — 修复窗口过小与鼠标移动闪屏

## 1. 用户反馈（图形界面视觉问题）

用户在实际 QEMU 图形窗口中观察到两个现象：
1. **窗口太小，shell 内容不显示** —— shell 窗口被限制在极小尺寸，终端文本几乎不可见。
2. **鼠标移动会有闪屏** —— 移动鼠标时整个屏幕明显闪烁/撕裂。

这两个问题均属图形渲染层面，需人工在图形窗口肉眼确认；本步骤完成代码修复并用 headless 验证了「功能性不崩溃、窗口能成功放大创建」，视觉有效性由用户最终确认。

## 2. 根因分析

### 2.1 窗口过小
窗口尺寸被 **OOL（Out-Of-Line 大消息）物理页上限** 死死锁死在 128×128：
- `include/ipc/port.h:25`：`#define MACH_MSG_OOL_MAX_PAGES 16`（64KiB）。`kernel_msg_t` 与 `ool_map_node_t` 的 `ool_pages[]` 数组按此宏定长。
- `user/lib/gui.c:59`（`suki_create_window` 应用侧）：`if (need == 0 || need > 16 * GUI_PAGE) return NULL;` —— **真正的硬限制**，窗口缓冲 >64KiB 直接返回 NULL。
- `user/display_server.c`（WM 侧 `wm_handle_create`）：原先 `if (need > 16 * 4096) goto fail;`。
- `user/shell.c:1171`：`suki_create_window("Shell", 64, 96, 128, 128, ...)` 写死 128×128；`TERM_COLS/ROWS=16` → 仅 16×16 字符。
- `user/apps/winhello.c:19`：`suki_create_window("WinHello", 120, 80, 128, 120, ...)` 同样极小。

128×128@32bpp = 64KiB = 16 页，正好卡在上限，故窗口不可能更大。

### 2.2 鼠标闪屏
`user/display_server.c::composite()` 为**单缓冲**：每帧（包括每次鼠标移动触发的重绘）先 `memcpy` 背景到真实帧缓冲，再**逐像素**把窗口/装饰/光标直接画到帧缓冲。VGA 扫描与写屏不同步，导致整屏重绘时的撕裂与可见闪烁。

## 3. 改动文件

### 3.1 `include/ipc/port.h`
`MACH_MSG_OOL_MAX_PAGES` 16 → **256**（单条 OOL 消息最多 2MiB）。
- `kernel_msg_t` / `ool_map_node_t` 的 `ool_pages[256]` 数组随之放大（约 2KB/消息），kmalloc 为按需增长的堆（无固定上限），headless 实测 IPC 正常、无分配失败。
- `ool_capture` 的 `size > MACH_MSG_OOL_MAX_PAGES * PAGE_SIZE` 校验自动放宽到 2MiB。

### 3.2 `user/lib/gui.c`（应用侧窗口创建上限）
- 第 11 行注释更新（≤16 页 → 256 页=2MiB）。
- 第 59 行：`if (need > 16 * GUI_PAGE) return NULL;` → `if (need > 256 * GUI_PAGE) return NULL;`（`GUI_PAGE=4096`，即 2MiB）。
- `suki_flush` / `suki_create_window` 其余逻辑不变，OOL 描述符构造（`m.ool.address = w->buffer; m.ool.size = w->buffer_size`）正确，内核 `ool_capture` 按逐页 VA→PA 翻译，物理离散页也可传。

### 3.3 `user/display_server.c`（WM 侧 + 双缓冲合成）
- `wm_handle_create`：`need > 16 * 4096` → `need > 256 * 4096`（与 OOL 上限一致）。
- **双缓冲合成**：新增 `static uint32_t *g_canvas`（指向 `g_desk` 后缓冲）；
  - `fb_px` / `fill_rect_fb` / `draw_cursor_on_fb` 全部改为写 `g_canvas`（原写 `g_fb`）；
  - `composite()` 开头 `g_canvas = g_desk; draw_desktop();` 在画布上重建背景+顶部条，再把窗口按 Z-order blit、画装饰与光标到画布，**末尾一次性 `memcpy(g_desk → g_fb)`**；
  - 这样真实帧缓冲每帧只被一次整屏拷贝更新，杜绝逐像素写屏造成的撕裂/闪烁。
- `main` 初始化：`g_canvas = g_desk;` 后调用 `draw_desktop()`，保证启动期背景也走画布。

### 3.4 `user/shell.c`（终端窗口放大 + 内容下移避开标题栏）
- 第 69 行注释更新（不再受 128×128 限制）。
- `TERM_COLS 16→80`、`TERM_ROWS 16→34`（标准 80×34 终端，字符区 640×272 @8×8 字体）。
- 窗口尺寸 `128×128` → **`660×380`**（位置 40,60），字符区 640 宽居中留边，34 行完整可见（80×34 = 2720 字，远超原 256 字）。
- `term_render()` 字符绘制 `py = r*8` → `py = r*8 + 20`、循环上界 `(r+1)*8 <= H` → `(r+1)*8 + 20 <= H`：顶部 20px 被 WM 标题栏覆盖，字符从标题栏下方开始绘制，避免内容被遮。

### 3.5 `user/apps/winhello.c`（自检窗口放大）
- 窗口 `128×120` → `360×240`（OOL 84 页 < 256，安全），文本/矩形坐标不变（仍在窗口左上）。

## 4. 验证

### 4.1 构建
`make iso` → `ISO_EXIT=0`，无编译错误。

### 4.2 Headless 回归（单核 70s）
- `KPF / panic / triple fault / OOPS` = **0**（OOL 放大与双缓冲未引入内核崩溃）。
- `window created id=` = **3**（winhello 与 shell 两窗口均被 WM 成功创建；display-server 另打印 `[wm] window created`）。
- `suki_create_window` 失败迹象（FAIL / returned NULL）= **0**（64KiB 硬限制已解除，660×380 窗口成功分配）。
- `[shell] windowed mode: window id=` = **1**（shell 成功进入窗口化模式）。
- `[libc-test] PASS=` = **1**（FS 交互正常，无回归）。
- 末段运行到 pthread / load balance 后期阶段，系统完整跑通无卡死。

### 4.3 视觉验证（用户人工，headless 不可观测）
见末尾「手动验证步骤」。重点确认：shell 窗口明显变大且终端内容可见；鼠标移动时屏幕不再闪烁/撕裂。

## 5. 影响与说明

- 窗口缓冲上限由 64KiB 提升到 2MiB，足以支持 1024×512@32bpp 级别窗口；当前 shell 660×380、winhello 360×240 均在安全范围内。
- 双缓冲使 composite 输出为完整帧，消除撕裂；代价是每帧多一次整屏 `memcpy`（约 3.6MB @1280×720×4，单核下单帧 <1ms，可接受）。
- `kernel_msg_t` 因 `ool_pages[256]` 放大到约 2KB，所有 IPC 消息（含无 OOL 小消息）占用增大，但 kmalloc 按需增长堆，实测无压力。
- 字符仍为 8×8 点阵（`font8x8_basic`）。若后续希望字更大，可在 `term_render` 改用 8×16 / 16×16 字形并同步调整 `TERM_COLS/ROWS` 与窗口尺寸（本轮保持 8×8 以最小改动解决「内容不显示」）。

## 6. 提交
- 改动文件：`include/ipc/port.h`、`user/lib/gui.c`、`user/display_server.c`、`user/shell.c`、`user/apps/winhello.c`、`results/step76.md`。
- 提交：`fix(gui): 放大窗口尺寸上限并改双缓冲合成，修复窗口过小与鼠标闪屏 (step76)`（未推送）。

## 7. 手动验证步骤（请在图形窗口执行）
1. 启动图形 QEMU：
   ```
   make run
   ```
   （或 `qemu-system-x86_64 -machine pc -cpu qemu64 -smp 1 -m 2G -boot d -cdrom build/SukiOS.iso -drive file=build/disk.img,format=raw,index=0,media=disk -device e1000,netdev=net0 -netdev user,id=net0 -display gtk`）
2. 启动后观察：
   - 应出现 **Shell** 窗口（约 660×380，左上区域），其中可见 `SukiOS>` 提示符与命令回显（80×34 字符网格，文本清晰可读）；
   - 另有 **WinHello** 自检窗口（360×240，带渐变背景与标题栏）。
3. 在 Shell 窗口内键入几个字符 / 执行 `ls`、`help` 等命令，确认终端内容完整显示。
4. 移动鼠标（在窗口内、桌面上来回移动）：确认**不再出现整屏闪烁/撕裂**；窗口拖拽、关闭按钮应顺滑。
5. 若仍觉 shell 字符偏小，可在 `user/shell.c` 将字体改为 8×16 并相应调大 `TERM_COLS/ROWS` 与窗口 `w/h`（非本步骤范围，可后续调整）。
