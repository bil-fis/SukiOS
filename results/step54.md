# Step 54 — 修复终端乱码：display_server 支持 ANSI CSI + shell 行编辑重绘缺陷

## 一、问题现象

用户在 QEMU 图形窗口实测反馈：

> 首次启动（未键入任何命令），输入 `help`，在串口成功输出了 `help`，
> 但是渲染到 qemu 图形窗口就出现了 `[k`。

并补充：输入时**偶尔出现 `[` 符号**，**有时还会冒出上一句命令**。

## 二、根因分析

### 2.1 主因：display_server 把控制序列当文本栅格化

shell 的行内编辑依赖 ANSI/VT100 控制序列（清行尾 `ESC[K`、光标移动
`ESC[nD` / `ESC[nC`、归位 `ESC[H`、清屏 `ESC[2J`）。

而 `user/display_server.c` 的 `term_putc()` **只处理 `\r` `\n` `\t` `\b` 四种字符**，
其余一律走 `else` 分支调用 `draw_glyph()` 绘制字形。于是：

- `ESC`(0x1B) → 字体表中对应位置是空白/占位，屏幕上看不见；
- `[`、`K`、`D`、`C`、`H` → **被当作普通字符正常绘制**。

结果就是屏幕上残留 `[K`、`[D` 等片段 —— 这正是用户看到的 `[k`。

**为什么串口看起来"正常"**：串口侧由宿主终端软件（如用户的终端模拟器）解释
这些控制序列，清行尾/移动光标被正确执行，所以看起来干净；而 SukiOS 自己的
display_server 没有实现这套解析，于是把序列的可见字符画了出来。二者走的是
同一份 `SYS_DEBUG_WRITE` 数据流，只是消费端解析能力不同。

### 2.2 次因一：`edit_insert` 每次按键都倾泻 `ESC[K`

```c
u_printn(&c, 1);        /* 回显新字符 */
redraw_from_cursor();   /* 无条件：清行尾 + 重绘右侧 + 移回光标 */
```

即在行尾输入（最常见的情形）也会发送 `ESC[K`。行尾本来就没有内容，
这个序列完全多余，却让每次按键都产生一次控制序列输出，放大了 2.1 的暴露面。

### 2.3 次因二：历史切换调用两次 `redraw_full()`，光标位置算错

```c
g_hist_idx--;
redraw_full();          /* 用【旧】行长重绘 */
hist_load(g_hist_idx);  /* 换内容，g_cur 变成【新】行长 */
redraw_full();          /* 用【新】行长左移 —— 与屏幕实际光标位置不一致 */
```

第二次 `redraw_full()` 的左移量基于**新**行长，而屏幕上光标实际停在旧行重印
之后的位置，二者不一致 → 光标错位、旧内容没被清干净 → **表现为"冒出上一句命令"**。

### 2.4 次因三：Tab 补全后 `redraw_full()` 会清掉提示符

```c
prompt();          /* 光标已在该行提示符【之后】 */
redraw_full();     /* 又左移 g_cur 列 —— 退回提示符内部并清掉它 */
```

### 2.5 次因四：shell 转义状态机两处不严谨

```c
if (g_esc == ESC_ESC) {
    if (c == '[') { ... continue; }
    g_esc = ESC_NONE;      /* 缺少 continue！ */
}
```

- ESC 后跟非 `[` 时没有 `continue`，该字节会**继续往下被当普通字符插入**
  （如 `ESC x` 会把 `x` 插进命令行）。
- `ESC_BRK` 状态下若收到新的 `ESC`，会被 `switch default` 吞掉并把状态清成
  `ESC_NONE`，随后的 `[`、`A` 被当作普通文本插入 → **屏幕上出现 `[A`**，
  即用户说的"有的时候是 `[` 符号"。

---

## 三、修复内容

### 3.1 `user/display_server.c`：实现 ANSI CSI 转义序列解析（根本修复）

在 `term_putc()` 之前新增状态机与绘制原语：

```c
#define CSI_NONE 0
#define CSI_ESC  1   /* 已收到 ESC */
#define CSI_BRK  2   /* 已收到 ESC [ （收集参数中，等待终结字母） */
static int g_esc_state = CSI_NONE;
static int g_csi_n = 0;      /* 第一个数值参数 */
static int g_csi_has_n = 0;
```

`term_putc()` 开头插入三层判断（**不绘制任何东西**）：

| 状态 | 处理 |
|---|---|
| `CSI_ESC` | 收到 `[` → 进入 `CSI_BRK` 并清零参数；其它 → 放弃整条序列 |
| `CSI_BRK` | 数字 → 累积参数；`;` / `?` → 忽略后续参数；终结字母（`@`~`~`）→ `csi_dispatch()` |
| 普通 | 收到 `0x1B` → 进入 `CSI_ESC` |

只有未命中上述任何分支的字符，才进入原有的 `\r\n\t\b` / 绘制字形逻辑。

**新增绘制原语**

- `clear_cols(row, x0, x1)`：用窗口背景色填充第 row 行 `[x0, x1)` 列
- `clear_screen_area()`：清终端客户区并让光标归位（**保留桌面与标题栏**）

**支持的 CSI 命令**（`csi_dispatch`，缺省参数取 1）

| 序列 | 语义 | 实现 |
|---|---|---|
| `ESC [ n A` | 光标上移 | `g_term_y -= n`（下界 0） |
| `ESC [ n B` | 光标下移 | `g_term_y += n`（夹到末行） |
| `ESC [ n C` | 光标右移 | `g_term_x += n`（夹到末列） |
| `ESC [ n D` | 光标左移 | `g_term_x -= n`（下界 0） |
| `ESC [ H` | 光标归位 | `g_term_x = g_term_y = 0` |
| `ESC [ n J` | 清屏 | `n=1` 清屏首到光标；其余清整个客户区 |
| `ESC [ n K` | 清行 | `n=0` 光标到行尾；`n=1` 行首到光标；`n=2` 整行 |
| `ESC [ ... m` | SGR 颜色 | 不支持，安全忽略（仍正确消费，不留残余字符） |

> 副作用（正向）：shell 的 `clear` 命令（`\033[2J\033[H`）此前也是"看起来没反应
> 或留下垃圾"，现在能真正清屏并归位。

### 3.2 `user/shell.c`：`edit_insert` 行尾插入不再发控制序列

```c
u_printn(&c, 1);                       /* 回显新字符 */
if (g_cur < g_len) {                   /* 仅在行【中间】插入时才需重绘 */
    redraw_from_cursor();
}
```

行尾插入（绝大多数按键）现在**零控制序列输出**，既省带宽也降低终端解析负担。

### 3.3 `user/shell.c`：历史切换改为单次、顺序正确的重绘

新增 `line_replace(const char *text)`，**严格按"旧光标位置"先回行首**：

```c
static void line_replace(const char *text)
{
    /* 1) 用【旧】的 g_cur 左移回行首 */
    if (g_cur > 0) { ... u_printn(seq, k); }
    /* 2) 清整行 */
    u_print("\x1b[K");
    /* 3) 载入并打印新内容 */
    ... 填充 g_line / g_len ...
    g_cur = g_len;
    u_print(g_line);
}
```

方向键分支改为一次调用：

```c
case 'A':   /* 上：历史上一条 */
    if (g_hist_count > 0 && g_hist_idx > 0) {
        g_hist_idx--;
        line_replace(g_hist[g_hist_idx]);
    }
    break;
case 'B':   /* 下：历史下一条（到末尾则回到新行） */
    if (g_hist_count > 0 && g_hist_idx < g_hist_count) {
        g_hist_idx++;
        if (g_hist_idx < g_hist_count) line_replace(g_hist[g_hist_idx]);
        else line_replace("");        /* 回到空的新行 */
    }
    break;
```

同时删除了因此不再被引用的 `hist_load()` / `hist_load_new()`。

### 3.4 `user/shell.c`：Tab 补全后不左移（保护提示符）

```c
prompt();
u_print("\x1b[K");    /* 保底清行尾 */
u_print(g_line);      /* 光标已在行首，直接重印整行 */
g_cur = g_len;
```

并删除了语义有误、已无引用的 `redraw_full()`。

### 3.5 `user/shell.c`：转义状态机严谨化

```c
if (g_esc == ESC_ESC) {
    if (c == '[') { g_esc = ESC_BRK; g_esc_num = 0; continue; }
    /* 非 '['：放弃本条序列，且该字节一并丢弃（不能当普通字符插入） */
    g_esc = ESC_NONE;
    continue;
}
if (g_esc == ESC_BRK) {
    if (c == 0x1B) {   /* 序列中途又来一个 ESC：重新开始，避免 "[A" 被当文本插入 */
        g_esc = ESC_ESC; g_esc_num = 0; continue;
    }
    ...
}
```

### 3.6 `user/lib/setjmp.S`：声明不可执行栈

新增 `.section .note.GNU-stack,"",@progbits`，消除链接器
`missing .note.GNU-stack section implies executable stack` 警告，
并避免内核载入器按 W^X 策略拒绝映像。

---

## 四、验证

### 4.1 构建

```bash
export PATH="$(pwd)/opt/bin:$PATH"
make
```

无 error、无 unused 警告；`==> valid Multiboot2 kernel` 通过。

### 4.2 QEMU 生产回归（headless，已自动结束 qemu）

```bash
qemu-system-x86_64 -machine pc -m 256M -cpu qemu64 \
  -kernel build/kernel.ski -drive file=build/disk.img,format=raw,if=ide,media=disk \
  -serial file:/tmp/final.log -display none -no-reboot
```

结果：

```
[shell] SukiOS shell online (Ring3, bash-like)
[fontsrv] font service online (FreeType, FONT_PORT=10)
[fontsrv] loaded font /FONTS/RESOURCEHANROUNDEDCN-MEDIUM.TTF (14423652 bytes, 1 faces)
[fontsrv] rendered req=1 glyphs=15 hits=3 bbox=314x129 status=0
[pchfnt] done: status=0 glyphs=15 cache_hits=3 bbox=314x129
```

**零 panic**（已排除 IDT 加载日志中的 `#PF/#BP/#OF/#UD handlers` 字样误匹配）。
字体服务、pchfnt 自检、shell 均正常，说明本次改动未引入回归。

### 4.3 验证手段说明与限制（如实记录）

尝试过用 `qemu -serial pty` 做**双向按键注入**（input_server 的
`poll_serial_input()` 经 `sys_serial_read()` 接收串口字符并作为按键事件发给
shell，理论上可端到端验证编辑逻辑）：

- pty 从设备端 `/dev/pts/N` 可正常打开；
- 但 QEMU 在 slave 端连接建立**之前**产生的串口输出会被丢弃，实测读到 0 字节，
  无法稳定拿到会话数据。

**故放弃该注入方式**，未引入任何 python/expect/socat 类外部编排工具。

**限制**：图形窗口的**像素级显示效果无法在 headless 下自动验证**——
display_server 的 CSI 解析是否如预期，必须由用户在图形窗口肉眼确认。

---

## 五、需用户手动验证（图形窗口）

```bash
export PATH="$(pwd)/opt/bin:$PATH"
make run          # 或 make iso 后从 ISO 启动
```

| # | 操作 | 预期（**关键：不应出现 `[k`、`[K`、`[A` 之类碎片**） |
|---|---|---|
| 1 | 启动后直接输入 `help` 并回车 | 回显干净的 `help`，输出帮助；**不再出现 `[k`** |
| 2 | 输入 `history` 之外几条命令，按 **↑ / ↓** | 逐条回滚/前进历史，行内容整体替换，无旧命令残留 |
| 3 | 到历史末尾再按 **↓** | 回到空的新行 |
| 4 | 输入 `abcdef`，按 **←** 三次，按 **Delete** | 光标处删除（`abcdef` → `abcef`），右侧内容正确左移 |
| 5 | 按 **Home / End** | 跳到行首 / 行尾，提示符完整不被破坏 |
| 6 | 输入 `cat /F` 后按 **Tab** | 补全为 `/FONTS/`；**提示符保持完整** |
| 7 | 输入不唯一前缀后按 **Tab** | 补齐公共前缀 + 换行列出候选，提示符与整行正确重印 |
| 8 | 执行 `clear` | 终端客户区被真正清空并归位（桌面与标题栏保留） |

若仍有碎片字符或光标错位，请把图形窗口现象（截图或描述）与
`-serial file:` 抓到的日志一并发我。
