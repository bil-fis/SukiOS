# Step 109 — OOL 上限提升至 2048 页(8MiB) + 显示服务窗口改为 iSuki 新样式

> 日期：2026-09-20
> 关联：Step 108（libsui 落地 + 显示服务增量渲染/纯黑背景）、`iSuki UI 界面库设计规范.md` §12.1
> 验证结果：QEMU 无头生产场景零 panic；`[suikitest] window rendered + flushed -> PASS`；窗口 id=1/2/3 正常创建并销毁。

---

## 0. 任务

1. 把内核 OOL（out-of-line 零拷贝）单条消息上限从 **256 页(1MiB)** 改为 **2048 页(8MiB)**。
2. 显示服务（`display_server`）创建的窗口仍是**老样式**（20px 标题栏、紫色非活动条、右上角红色 16×16 关闭框）。对照 `iSuki UI 界面库设计规范.md` §12.1 改为新样式：标题栏高 38、圆角 12、左侧三色交通灯（macOS 风格）、活动态强调蓝，整窗圆角。

---

## 1. OOL 上限 256 → 2048 页（8MiB）

### 1.1 根因与改动

内核 OOL 单条上限由 `include/ipc/port.h` 的 `MACH_MSG_OOL_MAX_PAGES` 决定，并同步约束窗口离屏缓冲大小（`w*h*4` 必须 ≤ 上限）。

- **`include/ipc/port.h`**
  ```c
  #define MACH_MSG_OOL_MAX_PAGES 256   →  2048   /* 单条 OOL 最多页数（8MiB） */
  ```
  配套数组 `kernel_msg_t.ool_pages[2048]`、`ool_map_node_t.pages[2048]` 随之变为 2048 项（每项 16KiB）。`kmalloc` 为通用堆分配器（无定长 slab 上限，按需 `kheap_grow`），16KiB 消息结构可正常分配。

- **`user/lib/gui.c`**（`SukiCreateWindow`）：`need > 256 * GUI_PAGE` → `need > 2048 * GUI_PAGE`（GUI_PAGE=4096，即 8MiB）。注释同步更新。

- **`user/display_server.c`**（`wm_handle_create`）：`need > 256 * 4096` → `need > 2048 * 4096`。

- 注释一致性（仅注释，不涉及逻辑）：
  - `user/fs_server.c`：`FILEBUF_SIZE` 注释改为「OOL 单条上限 MACH_MSG_OOL_MAX_PAGES=2048 页=8MiB」。
  - `user/shell.c`：窗口放大注释改为「2048 页=8MiB」。
  - `user/apps/suikitest.c`：注释改为 8MiB，并把演示窗口从 600×400 恢复为原始 **660×440**（1.16MiB，8MiB 上限内合法，且匹配原控件布局）。

> 备注：`include/ipc/port.h` 原注释「256 页=2MiB」为旧错误值（实为 1MiB，见 `fs_proto.h` 旧注释「256 页 = 1MiB」）；本次统一为 2048 页 = 8MiB。
> 注意：每个内核 IPC 消息结构现在含 16KiB 的 `ool_pages[]` 数组（即便发送小消息也如此），内核堆占用随 IPC 量上升。功能正确，属本次上限提升的固有代价。

---

## 2. 显示服务窗口改为 iSuki 新样式

### 2.1 规范依据（`iSuki UI 界面库设计规范.md` §12.1）

- 标题栏高 **38**，圆角 **12**。
- 标题栏左侧：**三色交通灯**（间距 8、直径 12）：关闭 `#FF5F57`、最小化 `#FEBC2E`、最大化 `#28C840`。
- 活动窗口强调色贯穿；非活动标题文字去饱和（本合成器不渲染文字，仅用颜色区分活动/非活动条）。
- 窗口阴影在纯黑背景上不可见（阴影为深色半透明），故以 **1px 边框 + 圆角清黑** 表达窗口边界与层级。

### 2.2 改动（`user/display_server.c`）

**常量块（颜色 + 尺寸）**
```c
#define COL_DESKTOP       0x00000000   /* 帧缓冲背景 = 纯黑 */
#define COL_BAR_INACTIVE  0x003A2A4A   /* 非活动窗口标题栏（iSuki 中性紫灰） */
#define COL_ACCENT        0x00579BFE   /* iSuki 活动态强调蓝 */
#define COL_TL_CLOSE      0x00FF5F57   /* 交通灯：关闭（红） */
#define COL_TL_MIN        0x00FEBC2E   /* 交通灯：最小化（黄） */
#define COL_TL_MAX        0x0028C840   /* 交通灯：最大化（绿） */

#define WIN_TITLE_H 38      /* 旧 20 → 38 */
#define WIN_RADIUS  12      /* 新增：窗口圆角半径 */
```
（删除旧 `COL_CLOSEBG`/`COL_CLOSEFG` 红框关闭按钮配色。）

**新增绘制辅助**
- `wm_fill_disc(cx,cy,rad,color)`：在裁剪窗内填充实心圆（交通灯用），仅触碰 `g_clx..g_clw` 裁剪区内的像素，越界按帧缓冲尺寸钳制。
- `wm_round_corners(w,r)`：清除窗口四角（背景纯黑即圆角）。**仅清除位于当前裁剪窗内的像素**，避免破坏裁剪窗之外已由更低 Z 窗口合成的内容；更高 Z 窗口随后覆盖本窗角点，Z 序安全。

**`wm_paint_window` 装饰块重写**
```
1px 边框（四边，COL_ACCENT）
if (SUKI_WS_TITLEBAR) {
    标题栏底（边框内侧，38px，活动=COL_ACCENT / 非活动=COL_BAR_INACTIVE）
    左侧三色交通灯：wm_fill_disc(x0+14, cy, 6, COL_TL_CLOSE)
                     wm_fill_disc(x0+34, cy, 6, COL_TL_MIN)
                     wm_fill_disc(x0+54, cy, 6, COL_TL_MAX)
}
wm_round_corners(w, WIN_RADIUS)   /* 最后清四角，圆角生效 */
```
（cy = 窗口顶 + 38/2 = 标题栏竖直中心。）

**命中检测：`wm_in_close_box` → `wm_traffic_light_at`**
```c
/* 返回 1=关闭(红) 2=最小化(黄) 3=最大化(绿) 0=无；命中半径 7 便于点击 */
static int wm_traffic_light_at(const wm_window_t *w, int32_t sx, int32_t sy);
```
- 三灯中心 `x = w->x + 14/34/54`，`y = w->y + 19`；距中心 ≤7px 即命中。

**鼠标点击处理（左键按下）**
```c
int tl = wm_traffic_light_at(hit, m->x, m->y);
if (tl == 1) { 发 SUKI_EVENT_WINDOW_CLOSE; continue; }      /* 红灯关闭 */
if (tl == 2 || tl == 3) { flush_dirty(); continue; }        /* 黄/绿灯：最小化/最大化协议未实现，仅消费点击 */
if (标题栏内) { 进入拖拽; }                                 /* 其余标题栏区域可拖拽 */
```
> 说明：当前 `sui_event_type_t` 仅有 `SUKI_EVENT_WINDOW_CLOSE/RESIZE/FOCUS`，无 MINIMIZE/RESTORE；故黄/绿灯点击仅消费（不触发未实现协议），红灯正常关闭。事件枚举扩展属后续工作。

### 2.3 关于标题文字

架构约束「显示服务绝不渲染任何字符」。新样式窗口的**标题文字由客户端在其离屏缓冲中自绘**（显示服务仅画几何标题栏 + 交通灯，会覆盖缓冲顶部 38px）。因此客户端内容应从 y≥38 起排布；标题文字如需显示在标题栏，应由客户端绘制在保留给标题栏的区域内（当前实现下显示服务覆盖该区，故客户端标题文字应置于客户区而非顶栏）。这是「合成器不渲染文字」设计的固有分工，非 bug。

---

## 3. 验证

```bash
make iso
timeout 25 qemu-system-x86_64 -machine pc -cpu qemu64 -m 512 \
  -serial file:/tmp/sui2.log -display none -no-reboot -cdrom build/SukiOS.iso
grep -nE "suikitest|\[wm\] window created|FAIL|panic|#GP|#PF|Triple" /tmp/sui2.log
```

日志节选：
```
205:[suikitest] start
209:[wm] window created id=1
217:[wm] window created id=2
218:[suikitest] window rendered + flushed -> PASS
```
- `[suikitest] window rendered + flushed -> PASS`：libsui 端到端全链路 + 8MiB OOL 窗口（660×440=1.16MiB）通过。
- 全程 `grep -niE "panic|#gp|#pf|triple|invalid opcode"` 仅命中 IDT 装载信息行（非故障）。
- POSIX 测试 `PASS=105 FAIL=0`，suikitest 退出码 0，窗口干净销毁。

---

## 4. 交付清单

| 文件 | 变更 |
|------|------|
| `include/ipc/port.h` | `MACH_MSG_OOL_MAX_PAGES` 256→2048（8MiB），注释修正 |
| `user/lib/gui.c` | `SukiCreateWindow` 上限 256→2048 页；注释 |
| `user/display_server.c` | `wm_handle_create` 上限 256→2048 页；窗口装饰改为 iSuki 新样式（38px 标题栏、圆角 12、左侧三色交通灯、活动强调蓝）；新增 `wm_fill_disc`/`wm_round_corners`；`wm_in_close_box`→`wm_traffic_light_at`；点击处理同步 |
| `user/fs_server.c` | 注释 OOL 上限更新 |
| `user/shell.c` | 注释 OOL 上限更新 |
| `user/apps/suikitest.c` | 注释更新；演示窗口恢复 660×440（8MiB 内合法） |

---

## 5. 结论

- OOL 单条上限提升至 **2048 页 = 8MiB**，窗口离屏缓冲上限同步放宽（libsui 演示窗口恢复 660×440 验证通过）。
- 显示服务创建的窗口现为 **iSuki 新样式**：38px 标题栏、12px 圆角、左侧红/黄/绿三色交通灯、活动态强调蓝边框，整窗圆角；红灯关闭、黄/绿灯点击消费、标题栏其余区域可拖拽。
- QEMU 无头生产场景**零 panic**，libsui/POSIX 全部 PASS。
