# Step 55 — 终端退格失效修复 + 满屏滚动（替代整片刷新）

日期：2026-09-05

## 一、背景与问题

用户在图形窗口实测反馈两条现象：

1. **退格不好使**：在 shell 里按 Backspace 删除字符后，被删的字符仍残留在屏幕上，
   看起来像"没删掉"或"越删越乱"。
2. **shell 区域写满时应向上滚动，而不是刷新整个 shell 区域**：之前每当终端
   逻辑行号 `g_term_y >= g_term_rows` 时，显示服务会把整个窗口客户区用
   `fill_rect(... COL_WINBG)` 整体清空，导致历史文本丢失、整屏闪烁。

本次修复分两部分：内核外的显示服务（`user/display_server.c`）负责滚动像素；
用户态 shell（`user/shell.c`）负责把"退格"语义正确转成终端控制序列。

---

## 二、退格失效的根因

shell 的行编辑采用"整行重绘"策略：任何编辑动作后调用
`redraw_from_cursor()`，其旧实现为：

```c
u_print("\x1b[K");            /* 清到行尾 */
u_print(g_line + g_cur);      /* 打印光标后内容 */
int back = g_len - g_cur;     /* 把光标移回 */
if (back > 0) { ... ESC[<back>D ... }
```

问题在于：**它假设"终端当前光标列 == 逻辑光标 `g_cur` 对应的列"**，但二者并不同步。

具体偏差链（以输入 `abc` 后退格删末尾 `c` 为例）：

- 初始：屏幕打印 `abc`，**终端实际光标停在 `c` 之后**（列 = `起点 + 3`）。
  此时 shell 侧 `g_cur = 3, g_len = 3`。
- `edit_backspace()`：`g_cur--` → 2，`g_len--` → 2，`g_line` 变 `"ab"`。
- 旧 `redraw_from_cursor()`：
  - `ESC[K` 从**终端当前列（= 起点+3）**开始清行尾 → 列 0..2 的 `abc` 纹丝不动，
    只有列 3 之后被清。
  - 接着 `u_print("ab")` 把 `ab` 写到列 3、4。
  - 屏幕变成 `abcab`，被删的 `c` 残留 → 表现为"退格不好使"。

根因是：`redraw_from_cursor()` 不知道终端光标到底在什么列，便在**错误的起点**清行尾。

### 修复思路：在 shell 内维护"终端列镜像"

新增两个全局变量（shell 侧，与显示服务 `g_term_x` 的同步镜像）：

- `g_scr_x`：终端光标当前列（相对整行左缘），随每次输出同步推进。
- `g_prompt_col`：编辑行起点列（= `"SukiOS:"(7) + ">"(2) + cwd 长度`），`prompt()` 时初始化。

`redraw_from_cursor()` 改为自洽三步：

```c
int back_to_start = g_scr_x - g_prompt_col;  /* 终端光标距编辑行起点的偏移 */
if (back_to_start > 0) ed_left(back_to_start); /* 1) 先把光标移回编辑行起点 */
ed_raw("\x1b[K");                             /* 2) 从起点清到行尾 */
ed_raw(g_line);                               /* 3) 重印整行 */
int target = g_prompt_col + g_cur;            /* 4) 把光标移回 g_cur */
int delta = target - g_scr_x;                 /*    重印后 g_scr_x = 起点+g_len */
if (delta < 0)      ed_left(-delta);
else if (delta > 0) ed_right(delta);
```

为维持 `g_scr_x` 与显示服务 `g_term_x` 始终一致，新增 `ed_track()` 解析自身发出的
每个字符 / CSI 序列（支持 `\n \r \t \b` 以及 `ESC[<n>D` / `ESC[<n>C` 左/右移），
并统一通过 `ed_raw()`（先 `ed_track` 再 `u_print`）输出所有编辑相关文本。

### 配套改动（shell.c）

- `prompt()`：打印提示符后计算并写入 `g_prompt_col = 9 + strlen(g_cwd)`、
  `g_scr_x = g_prompt_col`（9 = "SukiOS:" 7 + "> " 2）。
- `edit_insert()`：去掉单独回显，统一走 `redraw_from_cursor()`（行尾/行中插入均正确）。
- `edit_backspace()` / `edit_delete()`：仅更新行缓冲后调用 `redraw_from_cursor()`。
- `edit_left/right/home/end()`：改用 `ed_left(n)/ed_right(n)`（经 `ed_raw` 同步镜像）。
- `line_replace()`：用镜像偏移 `g_scr_x - g_prompt_col` 计算"回退到行首"左移量，
  替代旧实现里用【旧】`g_cur` 计算左移量（旧算法在屏幕上光标已落在旧行重印位置后
  会错位，导致历史切换时上一句命令残留——见 step54 已修，本次用镜像彻底去依赖）。
- 补全 (`edit_tab_complete`) 列出候选后重印编辑行也改用 `ed_raw`，保证镜像同步。

> 设计取舍：编辑行假设在**单行内**（避免跨行环绕的绝对定位复杂度）。超出窗口宽度的
> 超长命令属于极端情况，本轮未做环绕支持（会退化为整行重绘的小偏差，不引发崩溃）。

---

## 三、满屏刷新 → 向上滚动（display_server.c）

### 旧实现（问题）

`term_putc()` 末尾：

```c
if (g_term_y >= g_term_rows) {
    g_term_y = g_term_rows - 1;
    fill_rect(CON_MARGIN, TITLE_H + CON_MARGIN,
              g_fb_width - CON_MARGIN * 2,
              g_fb_height - TITLE_H - CON_MARGIN * 2, COL_WINBG);  /* 整片清空 */
    g_term_y = 0; g_term_x = 0;
}
```

窗口框架（标题栏、边框）只在启动时绘制一次，这种方式既丢历史文本又让窗口
内容瞬间全白、再复位到顶部，体验差。

### 新实现：像素级上滚一行

新增 `term_scroll_up()`，直接对帧缓冲做逐行 `memcpy`（`xRGB32`，每像素 4 字节）：

```c
static void term_scroll_up(void)
{
    if (g_term_rows == 0) return;
    uint32_t left     = CON_MARGIN;
    uint32_t top      = TITLE_H + CON_MARGIN;          /* 文本客户区上沿 */
    uint32_t width_px = g_fb_width - CON_MARGIN * 2;
    uint32_t pitch_px = g_fb_pitch / 4;                /* 每行像素数 = pitch(字节)/4 */
    uint32_t row_px   = CHAR_H;
    uint32_t rows     = g_term_rows;

    if (g_cur_visible) cursor_restore_bg();            /* 滚动前先还原鼠标光标背景 */

    /* 逐行上移：第 y 行 <- 第 y+1 行（y = 0 .. rows-2）*/
    for (uint32_t y = 0; y + 1 < rows; y++) {
        uint32_t *dst = (uint32_t *)g_fb + (top + y * row_px) * pitch_px + left;
        uint32_t *src = (uint32_t *)g_fb + (top + (y + 1) * row_px) * pitch_px + left;
        memcpy(dst, src, (size_t)width_px * row_px * sizeof(uint32_t));
    }
    /* 清空末行（窗口背景色）*/
    uint32_t *dst = (uint32_t *)g_fb + (top + (rows - 1) * row_px) * pitch_px + left;
    for (uint32_t i = 0; i < width_px * row_px; i++) dst[i] = COL_WINBG;
}
```

`term_putc()` 满行分支改为：

```c
if (g_term_y >= g_term_rows) {
    term_scroll_up();                 /* 客户区上移一行，保留历史文本 */
    g_term_y = g_term_rows - 1;
    g_term_x = 0;
}
```

要点：

- **只滚文本客户区**：`top = TITLE_H + CON_MARGIN`，且 `rows * CHAR_H` 经
  `clear_screen_area` 同源公式保证客户区底恰好落在窗口内边（= `g_fb_height - CON_MARGIN`），
  因此标题栏与左右/底部 2px 窗口边框均不被滚动波及，窗口框保持完整。
- **逐行自顶向下拷贝，目标行恒在源行之上**：同一行内的 `memcpy` 源/目的不重叠，安全无撕裂。
- **鼠标光标**：滚动前若光标可见则先 `cursor_restore_bg()` 还原其背景并置
  `g_cur_visible = false`；下次鼠标移动事件会重新快照并绘制，避免箭头像素随内容上移
  造成残影。
- 滚动与 shell 的 `g_scr_x/g_cur` 镜像互不干扰：滚动只发生在命令输出阶段（经
  `u_print`，shell 不追踪），输出结束 `prompt()` 会重置镜像，编辑状态不受影响。

---

## 四、关键文件与符号

| 文件 | 改动 |
|------|------|
| `user/display_server.c` | 新增 `term_scroll_up()`（约 365 行后）；`term_putc()` 满行分支改调滚动 |
| `user/shell.c` | 新增 `g_scr_x`/`g_prompt_col` 镜像；`ed_track()`/`ed_raw()`/`ed_left()`/`ed_right()`；重写 `redraw_from_cursor()`；改写 `edit_insert/backspace/delete/left/right/home/end`、`line_replace()`、`prompt()`、补全尾部重绘 |

### 常量（来自 display_server.c 既有定义）

- `CHAR_W = 16`, `CHAR_H = 16`
- `CON_MARGIN = 24`（窗口外边距）
- `TITLE_H = 40`（标题栏高度）
- `COL_WINBG`：窗口背景填充色
- `g_fb`（`volatile uint32_t *`，xRGB32）、`g_fb_pitch`（字节/扫描行）、`g_fb_width/height`
- `g_term_x / g_term_y / g_term_cols / g_term_rows`：文本终端逻辑光标与尺寸
- `g_cur_x / g_cur_y / g_cur_visible / g_cur_bg`：鼠标光标快照（`cursor_restore_bg()` 复用）

---

## 五、验证方式

受项目铁律约束（禁用 GDB/外部调试脚本，仅用 bash + QEMU 自带机制），采用以下验证：

### 1. 编译

```
make iso
```

`build/kernel.ski` 与 ISO 均成功生成，`-Wall -Wextra` 无新增告警/错误。

### 2. 无头回归（零 panic）

```
timeout 25 qemu-system-x86_64 -machine pc -cpu qemu64 -smp 1 -m 2G \
  -no-shutdown -display none -serial file:/tmp/suki.serial.log \
  -boot d -cdrom build/SukiOS.iso -drive file=build/disk.img,format=raw,index=0,media=disk
```

`tail /tmp/suki.serial.log` 关键输出：

```
[shell] SukiOS shell online (Ring3, bash-like)
[fontsrv] framebuffer mapped 1280x720 pitch=5120
[fontsrv] font service online (FreeType, FONT_PORT=10)
[pchfnt] done: status=0 glyphs=15 cache_hits=3 bbox=314x129
[fs] self-test ALL PASS
[libc-test] PASS=8 FAIL=0  ALL OK
```

QEMU 进程被 `timeout` 正常终止（`EXIT=124`，非崩溃），**全程无任何 panic / #GP / 三重故障**，
显示服务、字体服务、shell、fs 自测、libc 自测全部正常。

### 3. 退格 / 滚动的肉眼验证（需用户在图形窗口手动确认）

由于 QEMU 在 headless 下无法注入键盘中断（项目已记录的 QEMU 限制），以下两项
**视觉正确性**须由用户在图形窗口手动验证：

**测试步骤（启动图形窗口）：**

```
make run
```

启动后图形窗口出现 SukiOS 控制台窗口。请逐项确认：

- **退格校验**：在 `SukiOS:/>` 提示符后依次输入 `abc`，确认显示 `abc`；
  再连续按 Backspace 三次，确认每个字符逐个消失、最终回到空提示符、**无残留字符**。
  再输入 `hello world`，用左方向键把光标移到中间（如 `hello| world`），
  按 Backspace 删除中间字符，确认光标右侧文本**整体左移补位**、且被删字符不残留。
- **滚动校验**：执行一串会产生大量输出的命令使终端写满（例如多次 `help`、
  或 `ls /`、`ls /FONTS` 等），确认当客户区写满时**顶部行被推出窗口、其余历史上移**，
  而非整片清空闪烁；滚动后窗口标题栏/边框仍完整、不出现黑块。
- **历史/补全回归**：按上下方向键切换历史命令，确认切换后整行正确替换、无旧命令残留；
  输入 `hel` 后按 Tab，确认列出 `help` 候选且编辑行恢复正确。

通过判据：上述操作无残留字符、无整片刷新、无图形界面卡死或内核 panic 即视为修复成功。

---

## 六、提交

按项目规则提交（不推送）：修复退格与满屏滚动两块，commit 信息见 `git log`。
