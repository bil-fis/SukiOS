# Step50：回退显示服务到 74a4d25 视觉基线 + 清除键盘/鼠标 dbg 刷屏

## 一、问题背景（用户反馈）
用户报告：在 step49（feeb82f）引入的"三层合成显示服务"导致：
1. **整个界面完全乱码**（桌面/终端排版错乱，文字铺满全屏无窗口边框/客户区偏移）。
2. **鼠标指针变形**（光标掩码设计成实心大白方块，不再是正常箭头）。
3. 用户明确指出：`git show 74a4d25`（sched-fix 提交）时的画面是他**想要的视觉效果**。
4. 移动鼠标会触发键盘诊断刷屏：
   ```
   [kbd-diag] irq=256 aux=256 to_mouse=256
   [mouse-diag] pktdone=256 (dx=-57 dy=-25 btn=0)
   ```
   这类 `irq=256/512/768…` 递增计数打印是调试残留，严重污染串口且说明 keyboard.c / mouse.c 里还残留诊断日志。

### 关键结论
- 74a4d25 本身**未改** `display_server.c`，它当时就是"单层快照法"版本（桌面 + 标题栏 + 终端窗口 + 像素快照法光标），视觉效果正确。
- step49 改成三层合成 + 错误光标掩码 → 乱码 + 变形。
- 用户要的是 74a4d25 的**视觉**，但 74a4d25 版的 `display_server.c` 主循环把鼠标事件 `msgh_id` 当 1（与文本同 id），这正是当初 step48 要修的"移动鼠标出现字符"根因。所以本次不能简单 `git checkout` 旧文件，必须做：**恢复视觉正确版 + 把鼠标事件 id 对齐到 101/102/103（与 mouse_server 一致）+ 加坐标 clamp 防御越界**。

## 二、修改清单

### 1. `user/display_server.c`（重写为 74a4d25 视觉基线 + 修复）
文件路径：`/mnt/d/Projects/SukiOS/user/display_server.c`

#### 恢复的内容（来自 git 74a4d25 的 display_server.c）
- **桌面合成布局**：
  - 全屏铺 `COL_DESKTOP`(0x00101926) 桌面底色；
  - 顶部 `TITLE_H=40` 标题栏 `COL_BAR`(0x003A2A4A)，画 "SukiOS" 标题（紫色 `COL_TITLE`）；
  - 客户区窗口：边框 `COL_WINBORDER`(0x00606080) + 内部 `COL_WINBG`(0x0014141C)；
  - 文本客户区左上角偏移 `CON_MARGIN=24`，文本从 `TITLE_H+CON_MARGIN` 起排版。
- **8x8 内置点阵字体**（`disp_font8x8[128][8]`），C99 designated initializer，未指明的控制字符自动为零（空字形，绝不会出现乱码方块）。
- **字形 2x 放大**：`draw_glyph` 把每个 8x8 像素块放大成 16x16（`CON_SCALE=2`），`CHAR_W=CHAR_H=16`。
- **像素快照法光标**：`g_cur_bg[12*18]` 保存光标覆盖区背景，`cursor_restore_bg()` 先恢复旧位、`cursor_draw()` 再画新位；光标为 **12x18 空心箭头位图**(`g_cursor_mask`)，白色(0x00FFFFFF)前景、透明背景取桌面/窗口像素。

#### 本轮对 74a4d25 版本的修正（关键，避免"移动鼠标出字符"复发）
- **鼠标事件 msgh_id 分区**：
  ```c
  #define DISP_MSG_TEXT   1
  #define MOUSE_MSG_MOVE   101
  #define MOUSE_MSG_BUTTON 102
  #define MOUSE_MSG_WHEEL  103
  ```
  与 `user/mouse_server.c` 中的定义**逐字一致**（mouse_server 当前发 101/102/103）。主循环据此分流：
  ```c
  if (h->msgh_id == DISP_MSG_TEXT)      term_puts(text);
  else if (h->msgh_id == MOUSE_MSG_MOVE ||
           h->msgh_id == MOUSE_MSG_BUTTON ||
           h->msgh_id == MOUSE_MSG_WHEEL) {
      mouse_event_msg_t *m = (mouse_event_msg_t *)msgbuf;
      cursor_draw(m->x, m->y);
      if (m->buttons & 1) put_px(m->x,m->y, 0x00FF3030); /* 左键按下红点反馈 */
  }
  ```
  这样鼠标坐标不再被 `term_puts` 当字符串打印（旧版 1 与文本混淆的 bug 彻底消除）。
- **坐标 clamp 防御越界**：`mouse_server` 基于 1024x768 基准累计绝对坐标，需 clamp 到实际 1280x720：
  ```c
  static int32_t clamp_cursor_x(int32_t x){ if(x<0)x=0;
    if(x+MOUSE_CURSOR_W > g_fb_width) x=g_fb_width-MOUSE_CURSOR_W;
    if(x<0)x=0; return x; }
  static int32_t clamp_cursor_y(int32_t y){ /* 同理对 height */ }
  ```
  `cursor_draw()` 入口先 clamp 再绘制，防止越界写帧缓冲触发 #PF 或画面错位。
- **fb_map_result_t 严格对齐内核**：保留 `uint32_t _pad0` 填充，使 `fb_user_va` 落在 offset 8（uint64_t 对齐），与 `include/kernel/framebuffer.h` 布局逐字节一致（错位会读错 width/height/pitch）。
- **调用修正**：`sys_port_claim(DISPLAY_PORT)` + `suki_syscall2(SYS_FRAMEBUFFER_MAP, &res, 0)` + `suki_syscall1(SYS_DISPLAY_READY, 0)`，统一走 `user/lib/suki.h`（删除了本地端口重定义）。

### 2. `kernel/arch/x86_64/keyboard.c`（清除 dbg 残留）
文件路径：`/mnt/d/Projects/SukiOS/kernel/arch/x86_64/keyboard.c`
- 删除诊断计数器 `g_kirq_cnt / g_kirq_aux / g_kirq_to_mouse` 及其声明。
- 删除 `kbd_irq_handler` 末尾的：
  ```c
  if ((g_kirq_cnt & 0xFF) == 0)
      kprintf("[kbd-diag] irq=%u aux=%u to_mouse=%u\n", ...);
  ```
  现在 handler 仅保留核心逻辑：
  ```c
  uint8_t st = inb(KBD_STATUS);
  if (!(st & STS_OUT_FULL)) return;
  uint8_t sc = inb(KBD_DATA);
  if (st & STS_AUX) mouse_feed_byte(sc);   /* 鼠标字节 */
  else kbd_feed_byte(sc);                   /* 键盘字节 */
  ```
  注：键盘/鼠标统一 IRQ1 分发（STS_AUX 分流）逻辑保留，这是 step48 修好的稳定行为，本次只删诊断打印，不改变路由。

### 3. `kernel/arch/x86_64/mouse.c`（清除 dbg 残留）
文件路径：`/mnt/d/Projects/SukiOS/kernel/arch/x86_64/mouse.c`
- 删除诊断计数器 `g_mirq_pktdone` 及声明。
- 删除 `mouse_feed_byte` 末尾的：
  ```c
  g_mirq_pktdone++;
  if ((g_mirq_pktdone & 0xFF) == 0)
      kprintf("[mouse-diag] pktdone=%u (dx=%d dy=%d btn=%u)\n", ...);
  ```
  仅保留 `mse_parse()` + `g_pkt_idx = 0;` 核心逻辑。

## 三、数据结构与关键常量
| 项 | 值 | 说明 |
|----|----|----|
| `DISP_MSG_TEXT` | 1 | 文本消息 id（内核 console.c user_puts 转发须一致） |
| `MOUSE_MSG_MOVE/BUTTON/WHEEL` | 101/102/103 | 鼠标事件 id，**与 mouse_server.c 逐字一致** |
| `MOUSE_CURSOR_W/H` | 12 / 18 | 光标位图尺寸（像素快照缓冲 `g_cur_bg[12*18]`） |
| `CON_SCALE` | 2 | 8x8 字形放大倍数 → 16x16 |
| `CON_MARGIN` | 24 | 窗口客户区边距 |
| `TITLE_H` | 40 | 标题栏高度 |
| `COL_DESKTOP/BAR/WINBG/WINBORDER/FG/TITLE` | 0x00RRGGBB | xRGB32 配色 |
| 帧缓冲 | 1280x720@32bpp | BGA 经 display.cfg(video_mode=on w=1280 h=720)设定 |

## 四、算法流程
1. `main()` → `sys_port_claim(DISPLAY_PORT)` 认领端口。
2. `SYS_FRAMEBUFFER_MAP` 取用户态线性映射（`fb_user_va`）。
3. `draw_desktop()` 画桌面/标题栏/窗口边框 + 终端客户区网格初始化（`g_term_cols/rows` 由客户区尺寸 / `CHAR_W` 算得）。
4. `term_puts("SukiOS display server ready.\n")` + `term_puts("Kernel boot log:\n")`。
5. `SYS_DISPLAY_READY` → 内核关掉 fbcon 直写屏、把诊断捕获进内核环形管道。
6. `drain_console_pipe()` 经 `SYS_CONSOLE_READ` 把内核启动日志刷进桌面终端窗口（类似 dmesg 回放）。
7. 消息循环：`mach_msg_recv` 阻塞在 `DISPLAY_PORT`，按 `msgh_id` 分流（文本→term_puts，鼠标→cursor_draw + 左键红点反馈），每次 `sys_yield()`。
8. 光标移动 = `cursor_restore_bg()`（恢复旧位背景）+ `cursor_draw()`（快照新位背景并画白色箭头）。文本重绘不会影响光标层，鼠标移动也不会擦成字符。

## 五、验证（QEMU headless 回归，符合项目铁律：仅 bash + QEMU 自带机制）
命令：
```bash
make iso
qemu-system-x86_64 -machine pc -cpu qemu64 -m 512 -smp 1 \
  -drive file=build/SukiOS.iso,format=raw,if=ide,media=cdrom -boot d \
  -serial file:/tmp/suki_disp.log -display none -no-reboot &
sleep 12; kill $QPID
cat -v /tmp/suki_disp.log | grep -iE "display|panic|fault|mouse|kbd|shell|SukiOS>"
```
结果：
- **零 panic / 零 fault**：日志无 `#GP/#PF/panic`；
- **display active**：`[display] active: kernel console text now routed to display server` + `g_display_active=1, waited=1 yield rounds`（启动屏障立刻放行，说明单核 RR 调度稳定，无饥饿）；
- **dbg 刷屏已清除**：日志中**不再出现** `[kbd-diag]` / `[mouse-diag]`，串口干净；
- **服务正常**：display-server(pid4) / mouse-server(pid5) / shell(pid6) 依次 spawn，`SukiOS>` 提示符出现，鼠标自测事件 `dx=12 dy=-7 buttons=1` 经 Ring3 mouse-server 正确上报。

## 六、待用户手动确认（图形窗口）
headless 只能验证启动完整性与零 panic，像素级渲染需用户在 **QEMU 图形窗口**手动确认：
```bash
make run   # 或 qemu-system-x86_64 ... -vga std -display gtk  不带 -display none
```
预期现象：
1. 桌面为深蓝底 + 顶部紫色标题栏 "SukiOS" + 居中终端窗口，内核启动日志显示在终端内（不再是满屏乱码）；
2. 移动鼠标 → 白色箭头光标在桌面/窗口上平滑移动，**不会**擦出字符、**不会**变形（恢复 74a4d25 的箭头形状）；
3. 左键按下时光标尖端出现红点反馈；
4. 串口/屏幕上**不再**出现 `[kbd-diag]` / `[mouse-diag]` 刷屏。

## 七、提交
- 已 `git commit`（英文前缀 + 中文描述，符合项目规则，未 push）：
  - 回退显示服务于 74a4d25 视觉基线（单层快照法 + 12x18 箭头光标）；
  - 鼠标事件 msgh_id 对齐 101/102/103 + 坐标 clamp；
  - 清除 keyboard.c / mouse.c 的 `[kbd-diag]` / `[mouse-diag]` 调试残留打印。
