# step112 — UI 优化：窗口四角黑框修复 + 鼠标移动脏区域增量重绘

## 0. 用户需求

> 「接下来对 ui 进行优化，现在创建的窗口四周有黑色的框（类似，就是在四角的位置）
> 请你检查并修复。然后，修改鼠标的渲染逻辑，鼠标移动的上次位置需要标记为脏，
> 增量重绘需要重绘掉所有脏区域。」

两条任务：
1. **窗口四角黑色框** —— 检查并修复。
2. **鼠标渲染逻辑** —— 鼠标移动的上次位置标记为脏，增量重绘重绘掉所有脏区域。

## 1. 根因定位（文件 `user/display_server.c`）

### 1.1 四角黑框：圆角实现“填黑”而非“挖洞”
原 `wm_round_corners()` 在实现圆角时，**直接把窗口四角、半径 `WIN_RADIUS`(12) 的圆外
像素写入背景黑 `0`**。这在“单个窗口浮在纯黑桌面”上视觉看似正常（圆角透出黑桌面），
但一旦存在**窗口重叠**——上层窗口（更高 Z）的圆角会把其下方**下层窗口**在角部的内容
**涂成黑色三角**，表现为“窗口四角的黑色框/块”。这正是用户看到的现象（多窗口叠加时，
某些窗口四角出现黑色区域）。

根因：圆角合成应采用“挖洞”语义（圆角处不写画布，让下层窗口/桌面自然透过），而非
强行填黑。原实现用填黑来“假装”圆角，破坏了下层内容。

### 1.2 鼠标渲染：旧光标位置未标记脏，且拖拽漏标
- 普通 `MOUSE_MSG_MOVE` 走 `cursor_move_only()`，它**直接 `memcpy` 从离屏层
  `g_desk` 恢复旧光标覆盖的整行**到显存（绕过了增量合成器），既不把旧光标矩形
  `mark_dirty`，也不参与窗口/圆角重绘——在窗口内容变化或圆角挖洞后，旧位置恢复可能与
  当前增量状态不一致，且无法正确恢复圆角处的下层内容。
- 拖拽分支（`g_drag` MOVE）只 `mark_dirty` 窗口的旧/新矩形，**漏标旧光标位置**，导致
  拖拽时旧光标残影留在显存（尤其当旧光标位于窗口矩形之外时）。
- 拖拽分支 `mark_dirty(ox, oy, w, w)` 的第四参（高度）误用 `w`（应为 `h`）——高度参数
  笔误会导致脏矩形高度不足、增量重绘区域偏小（已顺手修正为 `h`）。

## 2. 修复

### 2.1 圆角改为“挖洞”（`user/display_server.c`）
- 删除 `wm_round_corners()`（填黑实现），新增 `wm_corner_cut(const wm_window_t *w, int i, int j)`：
  仅当像素 `(i,j)` 落在某角顶点 `WIN_RADIUS` 的 `r×r` 邻域内且位于圆外时返回 1（需保留下层）。
- 新增全局 `g_paint_win`（当前正在合成的窗口）。
- `wm_paint_window()`：
  - 进入即 `g_paint_win = w;`。
  - **像素拷贝**：仅在窗口最上/最下 `WIN_RADIUS` 行（可能触及角邻域）逐像素跳过
    `wm_corner_cut` 命中的像素；中间行无角邻域，整块 `memcpy`，性能无损。
  - **装饰填充** `fill_clip()`：写入前检查 `wm_corner_cut(g_paint_win,i,j)`，圆角处不画
    边框/标题栏——边框、标题栏在圆角处自然断开，正确呈现圆角。
- 这样圆角处**不写画布**，下层窗口/桌面内容自然透过：
  - 单窗口浮于桌面 → 圆角透出黑桌面（与 iSuki 圆角设计一致）；
  - **重叠窗口** → 上层圆角透出下层窗口内容，**不再把下层涂黑**（修复黑框）；
  - `redraw_region()` 的“先清黑脏矩形、再按 Z 重绘相交窗口”逻辑不变，挖洞由窗口绘制
    阶段统一处理，无需在合成末尾二次填黑。

### 2.2 鼠标渲染改为脏区域增量重绘
- 删除 `fb_restore_row_from_desk()` 与 `cursor_move_only()`（被脏区域方案取代）。
- 普通 `MOUSE_MSG_MOVE` 分支：
  ```c
  mark_dirty(oldx, oldy, MOUSE_CURSOR_W, MOUSE_CURSOR_H);     /* 旧光标矩形 */
  mark_dirty(g_cur_x, g_cur_y, MOUSE_CURSOR_W, MOUSE_CURSOR_H); /* 新光标矩形 */
  flush_dirty();   /* 重绘所有脏区域（恢复下层+圆角），末尾统一画新光标 */
  ```
  旧光标位置由增量合成器重绘（正确恢复下层窗口内容并尊重圆角挖洞），新光标由
  `flush_dirty()` 末尾的 `draw_cursor_at()` 绘制。彻底消除“直接覆盖显存”带来的状态不一致。
- 拖拽分支 `MOUSE_MSG_MOVE`：在 `mark_dirty` 窗口旧/新矩形之外，补
  `mark_dirty(oldx, oldy, MOUSE_CURSOR_W, MOUSE_CURSOR_H)` 旧光标位置；并修正高度参数 `w→h`。
- `flush_dirty()` 逻辑不变（遍历脏矩形 `redraw_region` + 末尾 `draw_cursor_at`）。

## 3. 验证

### 3.1 编译 / 启动（QEMU headless + serial，仅 bash 自带机制）
```bash
make iso
qemu-system-x86_64 -machine pc -cpu Skylake-Client -smp 1 -m 2G -no-shutdown \
  -display none -serial file:/tmp/x5.log -boot d -cdrom build/SukiOS.iso \
  -device piix3-usb-uhci -device usb-kbd -device usb-mouse
```
- `make iso` 编译通过（`build/user/display_server.c.o` 无错；修复了一处 `y1→wy1` 未声明
  变量后编译干净）。
- 启动日志：`display-server ready`、`[wm] window created id=1/2`、`[winhello] window
  created id=3` 均正常；**全程无 panic / exception / triple fault / page fault**。
- 说明：headless 无法注入真实鼠标移动、也无法肉眼观察圆角像素，鼠标移动路径与四角
  视觉需图形界面人工实测（见 §4）。

### 3.2 逻辑正确性核对
- 圆角“挖洞”：`redraw_region()` 先清黑脏矩形（含窗口区外桌面），再按 Z 顺序
  `wm_paint_window()`；高 Z 窗口在圆角处不写画布，保留已绘制的低 Z 窗口/桌面 → 透出正确。
- 鼠标脏区域：`MOUSE_MSG_MOVE` 把旧/新光标矩形入脏表，`flush_dirty()` 重绘（旧位置恢复
  下层、新位置重绘窗口后由 `draw_cursor_at` 覆盖新光标）；`MAX_DIRTY=8` 足够（单次消息
  mark ≤3 个矩形后即刻 flush，不会累积溢出）。

## 4. 需人工图形实测（headless 不可测，请用户在 QEMU 窗口内观察）

启动命令（带图形窗口，便于肉眼观察）：
```bash
make iso disk
qemu-system-x86_64 -machine pc -cpu Skylake-Client -smp 1 -m 2G \
  -display gtk -boot d -cdrom build/SukiOS.iso \
  -device piix3-usb-uhci -device usb-kbd -device usb-mouse
```
（或 `make run` 若 Makefile 已封装；`-display gtk` 出窗口，用 `usb-mouse` 可直接移动光标。）

**测试要点**：
1. **四角黑框**：打开多个窗口并相互重叠/拖动，观察上层窗口的四角是否仍把下层窗口
   涂黑。预期：**上层圆角应透出下层窗口内容，不再有黑色三角/黑框**。
   - 若你指的是“单窗口本身的圆角黑三角”（iSuki 圆角设计透出黑桌面），且你希望改为
     **直角窗口**或调整圆角半径/融合方式，请告知，我再调整 `WIN_RADIUS` 或圆角策略。
2. **鼠标移动**：在窗口上移动鼠标，观察旧光标位置是否被完整重绘（无残影、无黑块），
   且鼠标经过窗口内容/圆角处时光标擦除正确。拖拽窗口标题栏移动时，旧光标位置亦无残影。
3. **零崩溃**：整套操作过程中系统不 panic、不卡死。

## 5. 关键文件/常量

| 项 | 位置 |
|---|---|
| 圆角挖洞判定 | `user/display_server.c::wm_corner_cut()`（替代 `wm_round_corners`） |
| 挖洞应用（像素/装饰） | `user/display_server.c::wm_paint_window()` + `fill_clip()` + 全局 `g_paint_win` |
| 圆角半径 | `user/display_server.c::WIN_RADIUS` (=12) |
| 鼠标移动脏区域 | `user/display_server.c` 消息循环 `MOUSE_MSG_MOVE`（普通 + 拖拽分支） |
| 增量合成器 | `user/display_server.c::mark_dirty()` / `redraw_region()` / `flush_dirty()` |

## 6. 修改文件清单
- `user/display_server.c`：圆角挖洞（`wm_corner_cut`/删除 `wm_round_corners`/`g_paint_win`/
  `fill_clip` 与 `wm_paint_window` 挖洞）；鼠标移动与拖拽改用脏区域增量重绘（删除
  `fb_restore_row_from_desk`/`cursor_move_only`）；修正拖拽 `mark_dirty` 高度笔误 `w→h`。

## 7. 已知/后续
- 圆角“挖洞”仅透出下层窗口/桌面（背景纯黑时圆角呈黑三角，属 iSuki 圆角外观）。若需
  圆角与窗口自身背景色融合（而非透出桌面），需应用提供窗口背景 alpha 或在挖洞处回填
  窗口背景色——属增强，非本次“黑框”修复范围。
- 鼠标光标位图仍为 12×18 单色掩码（`MOUSE_CURSOR_W/H`），脏矩形随之。若后续换更大/彩色
  光标，脏矩形尺寸同步更新即可。
