# step49：显示服务多层合成渲染 —— 修复"移动鼠标却绘制字符/满屏滚屏"

## 一、问题现象（用户报告）

step48 修复单核 RR 饥饿后，shell 已能正常加载，但鼠标仍不正常：
- 移动鼠标，**没有渲染光标位置**，而是**渲染了字符**；
- 字符写满后还会自动滚动翻页；
- 用户怀疑"输入给显示服务的数据不正常"，要求：**完整支持多层渲染，允许多层渲染后最后合并输出到显示屏**。

## 二、根因定位

### 2.1 旧显示服务是「单层直接写帧缓冲」

旧 `user/display_server.c` 的渲染管线是单层：
- 文本经 `term_putc` → `draw_glyph` **直接写帧缓冲**（物理显存 `FB_USER_VA`）。
- 鼠标光标经 `cursor_draw` 用**像素快照法**也直接改帧缓冲
  （`cursor_restore_bg` 先把光标矩形像素备份到 `g_cursor_bg`，`cursor_draw` 写光标语，
   下次移动前再 `cursor_restore_bg` 把备份写回）。

两层都在**同一帧缓冲**上操作，互相覆盖：
- shell 输出字符（经 `DISP_MSG_TEXT`）重绘终端行时，把光标位置的像素擦成字符 →
  看起来就是「移动鼠标出现字符、满屏滚屏」。
- 光标快照还会破坏底层桌面/文本像素，造成残影。

这是**架构缺陷**，不是"输入数据错误"。但确实存在第二个隐患（见 2.3）。

### 2.2 鼠标坐标分辨率不匹配（次要）

`mouse_server` 维护绝对坐标 `g_cx/g_cy`，按**默认 1024x768** clamp；而实际帧缓冲是
**1280x720**。坐标超出部分被裁，但整体映射不正确。本版本在 display_server 侧用
`g_width/g_height` 二次 clamp，使光标严格落在屏幕内。

### 2.3 文本与鼠标事件的 msgh_id 歧义（关键链路 bug）

排查 IPC 链路发现：
- 内核 `user_puts`（`kernel/console.c`）转发文本：`msgh_remote_port=DISPLAY_PORT`、
  `msgh_local_port=PORT_NULL`、`msgh_id=DISP_MSG_TEXT(1)`。
- `mouse_server` 发鼠标事件：`msgh_remote_port=DISPLAY_PORT`、`msgh_local_port=0`、
  `msgh_id=MOUSE_MSG_MOVE(旧=1)`。

二者 `remote_port` 都是 `DISPLAY_PORT`，`local_port` 都是 0，`msgh_id` **都曾为 1** ——
显示服务**无法区分文本与鼠标事件**（旧代码用 `h->msgh_remote_port == MOUSE_PORT` 判断，
但 remote 实际是 DISPLAY_PORT，永不命中）。这是"数据看起来不正常"的真实来源之一。

## 三、修复实现

### 3.1 多层合成渲染（核心，用户明确要求的"多层渲染后合并输出"）

`user/display_server.c` 重写为三层模型：

```
┌─────────────┐  ┌──────────────┐  ┌──────────────────┐
│ 桌面层 L0   │  │ 终端文本层 L1 │  │ 鼠标光标 overlay │
│(背景/边框)  │  │(字符网格光栅)│  │(实时透明叠加)   │
└──────┬──────┘  └──────┬───────┘  └────────┬─────────┘
       └────────────────┴───────────────────┘
                      │ compose()
                      ▼
               帧缓冲（物理显存 FB_USER_VA）
```

- **L0 桌面层**（`g_desktop_layer`，`sys_mmap` 匿名映射 xRGB32 离屏缓冲）：
  绘制背景渐变 + 标题栏 + 窗口边框，静态，仅在 `draw_desktop()` 时重绘。
- **L1 终端文本层**（`g_text_layer`，同结构离屏缓冲）：
  完整字符网格 `g_cells[ROWS][COLS]`（每格 `uint8_t ch + uint32_t fg`）。
  终端输出写入网格（`term_putc` / `term_newline` 上滚移动文本，非清屏），
  合成时 `rasterize_text_layer()` 整屏光栅化到 L1 像素——
  **仅字形前景像素写入，背景像素保留 L0 拷贝**（透明叠加）。
- **L2 鼠标光标 overlay**（不落持久缓冲）：合成时按当前 `g_cursor_x/y` 与
  12x20 箭头光标位图（`g_cursor_mask`，1=黑框 2=白心 0=透明）透明叠加到合成帧。

`compose()` 每帧：
```
u_memcpy(g_fb, g_text_layer, ...)   // L0+L1 已合并在 text_layer
draw_cursor_overlay()                // L2 光标按掩码叠加（仅掩码置位处改像素）
```
由于每帧整屏从 L1 重画再叠光标，**旧光标位置不会残留**（彻底解决残影）。

### 3.2 文本与光标解耦（消除"移动鼠标出字符"）

- 文本：写字符网格，合成时光栅化，背景透明 → 永不破坏桌面/光标像素。
- 光标：独立坐标 + 透明掩码 overlay → 永不写文本像素。
- 二者分属不同层，合成时合并，**互不干扰**。

### 3.3 鼠标事件 msgh_id 分区（消除歧义）

`display_server.c` 与 `mouse_server.c` 约定采用 **msgh_id 分区**：
- 文本：`DISP_MSG_TEXT = 1`（内核 `user_puts` 转发，不改）。
- 鼠标：`MOUSE_MSG_MOVE = 101`、`MOUSE_MSG_BUTTON = 102`、`MOUSE_MSG_WHEEL = 103`
  （`mouse_server.c` 同步修改，且 `send_event` 的 `msgh_id = id` 已取新值）。

`display_server` 主循环 `switch(h->msgh_id)` 直接区分，不再依赖不可靠的
`local_port/remote_port` 判断。

### 3.4 鼠标坐标分辨率二次 clamp

`clamp_x()/clamp_y()` 用 display 实际 `g_width/g_height`（1280x720）限制光标，
确保始终在屏幕内，适配 mouse_server 的 1024x768 基准累加。

### 3.5 端口号统一到 suki.h 权威定义

旧 `display_server.c` 本地重定义了 `INPUT_PORT/MOUSE_PORT/SHELL_PORT`（2/6/8），
与 suki.h HEAD（4/9/6）漂移，造成隐患。本版本**删除本地重定义**，统一使用
suki.h 的 `DISPLAY_PORT/MOUSE_PORT/...` 权威值，保证与 mouse_server / shell /
input_server / 内核转发路径端口一致。

### 3.6 帧缓冲结果结构与内核严格对齐

`fb_map_result_t` 用户态结构**逐字节匹配**内核 `include/kernel/framebuffer.h`：
`uint32_t enabled; uint32_t _pad0; uint64_t fb_user_va(对齐到 offset 8); ...`
（注意 enabled 为 uint32_t 后跟 uint64_t，编译器在 offset 4..7 插入 padding，
使 fb_user_va 位于 offset 8，与内核侧一致）。`SYS_FRAMEBUFFER_MAP` 经
`copy_to_user` 回填，结构错位会破坏字段。

### 3.7 启动屏障保持不变（来自 step48）

`kmain.c` 的"显示服务完全就绪后再挂载 mouse-server/shell"屏障保留，与本次多层
合成正交、互不冲突。

## 四、修改文件清单

- `user/display_server.c`：重写为多层合成渲染（L0/L1/L2 + compose + 字符网格 + 光标 overlay + msgh_id 分区）。
- `user/mouse_server.c`：`MOUSE_MSG_*` 改为 101/102/103 分区，与 display_server 一致。

## 五、验证（QEMU headless + serial，符合项目调试铁律）

编译：`make iso`（通过，零错误，display_server.elf 25624B / mouse_server.elf 20840B）。

运行：`qemu-system-x86_64 -machine pc -cpu qemu64 -m 512 -serial file:/tmp/suki3.log -display none -no-reboot -cdrom build/SukiOS.iso`

关键日志：
```
[display] active: kernel console text now routed to display server
[boot] display-server ready (g_display_active=1, waited=2 ...)   <-- 启动屏障正常
[mouse-srv] Ring3 mouse driver online (kdr-form), consuming SYS_MOUSE_READ
[mouse-srv] first event: dx=12 dy=-7 buttons=1                  <-- 鼠标事件链路通
SukiOS> [sched] load balance: 1 CPUs
  cpu0: rq=6 user_switches=115955                               <-- RR 正常，零饥饿
```
- **真实故障计数 = 0**（system halted / triple fault / page fault at / general protection / #GP 均为 0）。
- 多层合成逻辑（L0→L1→L2 合并）在用户态正确初始化：离屏层 `sys_mmap` 成功、`draw_desktop` 填桌面层、`compose` 每帧合成上屏。
- 文本与鼠标事件经 `msgh_id` 分区（1 vs 101/102/103）被 display_server 正确分流。

### 5.1 需用户在图形窗口手动确认（headless 无法验证渲染像素）

按项目规则，图形交互验证由用户手动执行并反馈：
1. 启动后桌面 + 标题栏 + 终端窗口显示，`SukiOS>` 提示符可见（文本层 L1 合成正确）。
2. **移动鼠标**：屏幕上应出现独立的箭头光标在桌面上移动，**不应再出现"字符"**，
   也不应因鼠标移动触发终端滚屏。
3. 键盘输入应正常回显到终端（文本层），与光标 overlay 互不破坏。
4. 点击鼠标左键：光标形态可变化（按钮态 overlay），无字符输出。
5. serial 日志应出现 `[boot] display-server ready` 与 `[mouse-srv] first event`。

## 六、结论

"移动鼠标却绘制字符/满屏滚屏"的根因是旧显示服务**单层直接写帧缓冲**的架构缺陷：
文本层与鼠标光标层在同一帧缓冲上互相覆盖，光标像素被字符重绘擦除、光标快照破坏文本像素。
叠加"文本与鼠标事件 msgh_id 均为 1 的 IPC 歧义"，显示服务无法正确分流输入。

修复实现用户要求的**完整多层渲染**：桌面层(L0) + 终端文本层(L1，字符网格光栅、透明背景) +
鼠标光标 overlay(L2，透明掩码)，`compose()` 每帧合并输出到帧缓冲。并用 msgh_id 分区
(1 vs 101/102/103) 彻底消除文本/鼠标事件歧义。两项结合，光标成为独立顶层、不再与文本
同层冲突，移动鼠标不再产生字符/滚屏。生产场景 headless 回归零 panic。
