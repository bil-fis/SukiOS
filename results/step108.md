# Step 108 — 移除 Rust 自检 + 落地 iSuki 原生控件库(libsui) + 显示服务重构(去桌面/纯黑/增量渲染)

> 日期：2026-09-20
> 关联前置：Step 60/Phase 0（SukiNative 双 API 预留）、Step 38（SMP 默认关闭）、Step 96–107（图形栈/窗口管理基线）
> 验证结果：**QEMU 无头生产场景零 panic**，libsui 端到端冒烟测试 `[suikitest] window rendered + flushed -> PASS`，POSIX 测试全过（PASS=105 FAIL=0），suikitest 进程退出码 0。

---

## 0. 背景与本次任务范围

用户给出三条明确指令（取代原「实现内核用户 ELF 加载器 + Rust stdtest」路线）：

1. **放弃 Rust**：把 Rust 相关代码从内核整体移除（RUSTHELLO/STDTEST 内嵌 blob 自检），保持内核可稳定启动（零 Rust 依赖）。
2. **实现 iSuki UI 控件库**：对照 `@/mnt/d/Projects/SukiOS/iSuki UI 界面库设计规范.md` 重新实现窗口、控件库等内容（即新建 `libsui`）。
3. **重构 display server**：删除其中「桌面相关」部分（桌面装饰条/菜单栏/任务栏等），只保留窗口管理 + 合成器；渲染方式改为**增量渲染**（哪里更新渲染哪里，没更新就不渲染）；**framebuffer 背景应为纯黑**。

本轮工作全部落在用户态（内核本身的行为基线不变），并修复了 libsui 端到端验证暴露出的一个真实 OOL 上限 bug。

---

## 1. 移除 Rust 自检（内核零 Rust 依赖）

### 1.1 改动点

- **`kernel/kmain.c`**
  - 删除 `RUSTHELLO` / `STDTEST` 两个 Rust 自检 spawn 块（原 `task_create_user(user_rusthello_start, ...)` / `task_create_user(user_stdtest_start, ...)`），替换为注释「Rust 已整体移除」；kmain 不再引用任何 `*rust*` / `*std*` 弱符号。
  - 调整自检 spawn 顺序（关键稳定性修复，见 §5.2）：`winhello` → `suikitest` → `dltest`（dltest 放最后，因其 `task_create_user` 装载会阻塞、若前置会卡死后续 spawn）。

- **`Makefile`**
  - 删除 `RUST_BLOB` / `STDT_BLOB` 变量定义。
  - 删除 `OBJS` 中的 `$(RUST_BLOB) $(STDT_BLOB)`。
  - 删除所有 `user_rusthello.*` / `user_stdtest.*` 的 objcopy blob 生成规则。
  - 删除 `rust-std-*` / `cargo` 相关目标与 `RUSTFLAGS` / `RUST_TARGET` 等残留。
  - `USER_PROGS` 增加 `suikitest`。

### 1.2 验证（零 Rust 依赖）

`make iso` 输出中不再出现任何 `RUST_*` / `.rs` / `cargo` 引用；最终链接命令仅含 C/汇编 `.o`。QEMU 启动日志：`[boot] SukiOS kernel entered`、各服务就绪、无 `Invalid Opcode`、无 panic。

---

## 2. iSuki 原生控件库 libsui（新建）

目录布局（全部新建）：

```
user/libsui/include/sui.h          # 公开头文件（控件库 API + 主题 + 画布原语）
user/libsui/src/sui_core.c         # 主题/画布/控件基类/布局/事件/窗口
user/libsui/src/sui_widgets.c     # 控件集实现
user/apps/suikitest.c              # libsui 端到端冒烟测试（内嵌自检）
```

构建规则（`Makefile`，已修复 `SUI_OBJS` patsubst bug）：

```make
SUI_SRCS := user/libsui/src/sui_core.c user/libsui/src/sui_widgets.c
SUI_OBJS := $(patsubst %.c,$(BUILD)/%.c.o,$(SUI_SRCS))
$(BUILD)/user/libsui/src/%.c.o: user/libsui/src/%.c
	$(USER_CC) $(USER_CFLAGS) -I user/libsui/include -c $< -o $@
$(BUILD)/user/suikitest.elf: $(BUILD)/user/suikitest.c.o $(SUI_OBJS) \
                              $(BUILD)/user/gui.c.o $(USER_LIB_OBJS) user/user.ld
	$(USER_CC) -nostdlib -static -no-pie ... -o $@ ...
```

> 修复点：原 `$(patsubst %,$(BUILD)/%.c.o,...)` 会生成错误的中间路径 `%.c.c.o`；改为 `$(patsubst %.c,$(BUILD)/%.c.o,...)`，正确产出 `build/user/libsui/src/sui_core.c.o` 等。

### 2.1 公开 API 与数据结构（`user/libsui/include/sui.h`）

- 前向声明：`typedef struct sui_canvas sui_canvas_t;`、`typedef struct sui_event sui_event_t;`、`typedef struct sui_widget_vtable sui_widget_vtable_t;`
- 基础工具（改为**公开**，供 `sui_widgets.c` 调用，避免 implicit declaration）：
  - `sui_strlen(const char*)` — 自实现长度（freestanding 无 libc）。
  - `sui__alloc(size_t)` / `sui__strcpy(char*,const char*)` — 静态池分配器封装。
- 控件基类 `sui_widget_t`：含 `type`、父/子链表、相对坐标 `x/y`、尺寸、`visible`、虚表指针 `vt`（`sui_widget_vtable_t`：`type_name/draw/measure/on_event/destroy`）。
- 主题（SUI 规范颜色/字体档）：`SUI_FONT_TITLE`/`SUI_FONT_SUBTITLE`、`SUI_WEIGHT_BOLD`/`SUI_WEIGHT_REGULAR`、强调色/背景色/边框色等。
- 画布原语：`sui_canvas_t`（包装 `suki_window_t* wk` + 离屏缓冲 `buffer/w/h`）。
- 窗口 API：`sui_init`、`sui_create_window(title,x,y,w,h,titlebar)`、`sui_window_set_event_port`、`sui_render_window`、`sui_window_present`、`sui_window_step`（事件泵）、`sui_window_get_running`。
- 布局：`sui_widget_set_pos`、`sui_widget_to_screen`（屏幕坐标换算，修复 misleading-indentation）、`sui_widget_from_screen`。
- 事件：`sui_event_t`、`sui_window_step` 经 `SukiPollEvent` 拉取 `WM_MSG_EVENT` 并分发到控件 `on_event`。

### 2.2 控件集（`user/libsui/src/sui_widgets.c`）

实现并接好虚表 `draw/on_event` 的控件：

| 控件 | 创建函数 | 关键交互 |
|------|----------|----------|
| Label 标签 | `sui_label_create` / `sui_label_set_font` | 仅绘制，无交互 |
| Button 按钮 | `sui_button_create(w,kind,size,text)` + `sui_button_on_click` | `SUI_BTN_PRIMARY/SECONDARY/DANGER`，点击回调 |
| Checkbox 复选框 | `sui_checkbox_create(w,text,checked)` | 点击切换勾选态 |
| Switch 开关 | `sui_switch_create(w,on)` + `sui_switch_on_change` | 点击切换，回调带 bool |
| Slider 滑块 | `sui_slider_create(w,min,max,val)` + `sui_slider_on_change`；`show_value` 字段 | 拖动改值，回调带 int |
| Progress 进度条 | `sui_progress_create(w,pct)` | 仅绘制 |
| Input 输入框 | `sui_input_create(w,placeholder)` | 键盘输入缓冲 |
| Card 卡片 | `sui_card_create(w,title,body)` | 承载结构化内容 |

> 修复点：`slider_on_event` 的 misleading-indentation 警告（拆行）；删除未用 `scaleb`；`sui__alloc` 由 `static` 改公开后 int-to-pointer-cast 警告消失。

### 2.3 设计要点（对照 iSuki 规范）

- 控件库**只绘制像素到自身窗口离屏缓冲**（经 `libsuki_gui` 的 `SukiSetPixel/SukiFillRect/SukiDrawText`），文本与图形内容全部由应用自绘，符合「显示服务绝不渲染任何字符」的架构约束。
- 提交经 `SukiFlush`（OOL 零拷贝）到 `WM_PORT`，由显示服务合成——libsui 自身不触碰帧缓冲硬件。
- 主题统一：标题栏/边框/控件强调色取自 SUI 规范调色板（Light 主题），与显示服务的窗口装饰（标题栏 20px、`COL_BAR_INACTIVE`/`COL_ACCENT`/`COL_CLOSEBG`）协调。

---

## 3. 显示服务重构（`user/display_server.c`）

### 3.1 删除桌面相关部分

- 删除 `TITLE_H` 桌面顶栏/装饰条概念中属于「桌面」的那部分（仅保留窗口级标题栏 `WIN_TITLE_H=20` 作为窗口装饰，非桌面元素）。
- `draw_desktop()` 只填纯黑（不再画顶栏/菜单栏/任务栏）：
  ```c
  #define COL_DESKTOP   0x00000000   /* 帧缓冲背景 = 纯黑（已删除桌面装饰条） */
  static void draw_desktop(void) {
      for (uint32_t y = 0; y < g_fb_height; y++)
          for (uint32_t x = 0; x < g_fb_width; x++)
              g_desk[y*g_stride + x] = COL_DESKTOP;
  }
  ```
- 保留的窗口装饰（属 WM 职责，非桌面）：标题栏条 `COL_BAR_INACTIVE`、强调色 `COL_ACCENT`、关闭按钮 `COL_CLOSEBG`/`COL_CLOSEFG`。

### 3.2 增量渲染（脏矩形机制）

核心数据结构与接口（文件顶部前向声明 + 实现）：

```c
#define MAX_DIRTY 8                       /* 最多 8 个脏矩形 */
static int g_dirty[8][4];                 /* [i] = {x,y,w,h} */
static int g_clx, g_cly, g_clw, g_clh;    /* 当前裁剪窗口（绘制相交用） */

static void mark_dirty(int x, int y, int w, int h);   /* 登记脏矩形 */
static void flush_dirty(void);                        /* 遍历脏矩形重绘+画光标 */
```

- `mark_dirty(x,y,w,h)`：把受影响的屏幕矩形加入 `g_dirty[]`（超过 8 个则退化为整屏 `composite()`）。
- `fill_clip(x,y,w,h,rgb)`：仅在裁剪窗口 `g_clx..g_clw` 内填色（窗内绘制用）。
- `wm_paint_window(w)`：仅重绘与当前脏矩形**相交**的窗口（Z-order 叠加边框/标题栏/关闭按钮）。
- `redraw_region(rx,ry,rw,rh)`：清黑（仅该区域）→ 重绘相交窗口 → 仅把脏区从离屏 `g_desk` 拷贝到真实帧缓冲 `g_fb`（省去整屏 3.6MB 拷贝）。
- `flush_dirty()`：遍历 `g_dirty[]`，逐区 `redraw_region` + 仅在该区画光标。
- `composite()`：全屏回退路径（初始整屏/极端情况）。

**所有消息循环 handler 均改为「`mark_dirty` + `flush_dirty`」增量路径**（不再整屏重绘）：
focus / destroy / set_pos / drag MOVE / drag release / close / click / mouse-up / WHEEL。
仅纯光标移动走 `cursor_move_only`（擦旧光标矩形 + 画新光标矩形，两个 ~12×18 小矩形）。

- `destroy` handler 在用数组移位覆盖槽位**之前**，先用 `ddx/ddy/ddw/ddh` 保存旧位置，供 `mark_dirty` 增量重绘原区域。
- 删除未使用函数 `fill_rect_fb` / `fb_px`（避免死代码）。

> 收益：窗口移动/调整/点击仅重绘受影响的小矩形；静止窗口零重绘开销；鼠标移动仅触碰 ~12×18 两小矩形。

---

## 4. libsui 端到端验证暴露的真实 bug：OOL 1MiB 上限

### 4.1 现象

首轮 suikitest 启动即 `[suikitest] FAIL: SukiCreateWindow returned NULL`，但显示服务随后仍创建了 `id=1`(winhello)、`id=2`（suikitest 重试前被误判为 race）。

### 4.2 根因（非 boot race，是确定性硬限制）

`user/lib/gui.c` 的 `SukiCreateWindow`：

```c
uint64_t need = (uint64_t)w * h * 4;
if (need == 0 || need > 256 * GUI_PAGE) return NULL;   /* OOL ≤256 页上限(1MiB) */
```

`GUI_PAGE = 4096`，故上限 = `256*4096 = 1,048,576` 字节（**1 MiB**）。这与内核 `include/ipc/port.h` 一致：

```c
#define MACH_MSG_OOL_MAX_PAGES 256   /* 单条 OOL 消息最多页数(2MiB 注释待勘误，实为 1MiB) */
```

> 勘误：`port.h` 注释写「256 页=2MiB」是错误的——256×4096 = 1,048,576 = 1MiB；`include/ipc/fs_proto.h` 第 66 行注释「256 页 = 1MiB」才是正确值。OOL 单条真实上限为 **1 MiB**（256 页）。

suikitest 原窗口 `660×440`：`660*440*4 = 1,161,600` 字节 > 1 MiB → **确定性** `return NULL`（与显示服务是否就绪无关）。

### 4.3 修复

将 suikitest 演示窗口缩到 ≤1MiB（`user/apps/suikitest.c`）：

```c
/* 内核 OOL 单条上限 MACH_MSG_OOL_MAX_PAGES(256 页 = 1MiB)，窗口离屏缓冲 w*h*4 ≤ 1MiB */
sui_window_t *w = sui_create_window("iSuki UI Demo", 180, 120, 600, 400, true);
```

`600*400*4 = 960,000` 字节 = 234 页，远低于 256 页上限；`SukiFlush` 的 OOL 提交 234 页也合法。控件布局（卡片在 x=300、各控件 y≤346）均在 600×400 内可见。

> 说明：本修复选择「缩小演示窗口」而非「提高内核 OOL 上限」——后者需同步放大 `port.h` 中 `ool_pages[256]`/`pages[256]` 数组及 `kernel_msg_t` 结构，属内核 IPC 协议变更，超出本次「libsui + 显示服务重构」范围。1MiB/窗口对当前演示与基础应用足够；如需更大窗口，后续单独立项提升 `MACH_MSG_OOL_MAX_PAGES`。

---

## 5. 启动顺序与稳定性

### 5.1 dltest 卡死修复（已确认根因）

`dltest` 内嵌 blob 的 `task_create_user` 装载未完成期间会阻塞。原顺序把 `dltest` 放在 `winhello`/`suikitest` **之前**，导致后两者永不 spawn。已把 `dltest` 重排到 `winhello`/`suikitest` **之后**，并加注释说明（kmain.c）。

### 5.2 验证日志（节选，`/tmp/sui.log`）

```
192:[boot-dbg] suikitest spawn ret=0xffffc00000ca0910
205:[suikitest] start (libsui UI library self-test)
212:[wm] window created id=1          ← winhello
217:[wm] window created id=2          ← suikitest (libsui)
218:[suikitest] window rendered + flushed -> PASS
383:[suikitest] lifecycle complete, exiting
386:[wm] destroy recv id=2
387:[wm] window destroyed
```

- `[suikitest] window rendered + flushed -> PASS` 出现 ⇒ libsui 端到端全链路打通（应用→WM_PORT→显示服务增量合成→帧缓冲）。
- suikitest 进程退出码 0，窗口销毁正常，无残留。
- 全程 `grep -niE "panic|triple|#gp|#pf|invalid opcode|deadlock|assert"` 仅命中 IDT 装载信息行（非故障）；POSIX 测试 `=== POSIX test summary: PASS=105 FAIL=0 ===`。

### 5.3 验证命令

```bash
make iso
timeout 25 qemu-system-x86_64 -machine pc -cpu qemu64 -m 512 \
  -serial file:/tmp/sui.log -display none -no-reboot -cdrom build/SukiOS.iso
grep -nE "suikitest|\[wm\]|FAIL|panic" /tmp/sui.log
```

---

## 6. 交付清单

| 文件 | 状态 | 说明 |
|------|------|------|
| `kernel/kmain.c` | 改 | 移除 Rust spawn；suikitest 加入；dltest 重排最后 |
| `Makefile` | 改 | 删除 Rust blob/规则；`USER_PROGS+=suikitest`；新增 libsui 构建（`SUI_OBJS` patsubst 修复） |
| `user/libsui/include/sui.h` | 新建 | iSuki 控件库公开头（API/主题/画布/控件基类/布局/事件/窗口） |
| `user/libsui/src/sui_core.c` | 新建 | 主题/画布/控件基类/布局/事件/窗口实现 |
| `user/libsui/src/sui_widgets.c` | 新建 | 控件集：button/checkbox/switch/slider/progress/card/input/label |
| `user/apps/suikitest.c` | 新建 | libsui 端到端冒烟测试（入口 `main`，窗口缩至 600×400） |
| `user/display_server.c` | 改 | 删桌面装饰；纯黑背景；增量渲染（脏矩形 `mark_dirty`/`flush_dirty`/`redraw_region`）+ 光标小矩形 |
| `user/lib/gui.c` | 引用 | `SukiCreateWindow` OOL 1MiB 上限（根因定位，未改逻辑） |
| `include/ipc/port.h` | 引用 | `MACH_MSG_OOL_MAX_PAGES=256`（即 1MiB，注释「2MiB」待勘误） |

---

## 7. 结论

- Rust 已从内核彻底移除，内核零 Rust 依赖、可稳定启动。
- iSuki 原生控件库 `libsui` 落地（窗口 + 8 类控件），经 `WM_PORT`/OOL 与显示服务对接，端到端冒烟通过。
- 显示服务重构完成：删桌面相关、纯黑背景、增量渲染（脏矩形 + 光标小矩形），静止窗口零重绘。
- 修复 suikitest 因 660×440 窗口超 1MiB OOL 上限导致的确定性 `SukiCreateWindow` 返回 NULL。
- QEMU 无头生产场景**零 panic**，libsui/winhello/POSIX 全部 PASS，suikitest 正常运行并干净退出。
