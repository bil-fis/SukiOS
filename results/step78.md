# Step78 — 修复 shell 命令执行后输入区残留/重复上次字符

## 1. 用户反馈（图形窗口手动验证）
在上一步（step77：命令输出已能显示在窗口）的基础上，用户在图形窗口继续验证，反馈：
> 输入并执行完毕一条命令后，清除输入缓冲区，来等待下一次全新输入（防止输入区显示上次重复字符）

具体现象：敲完一条命令（如 `ls`）回车执行后，下一次开始输入新命令时，**编辑区会把上一条命令的残留字符也显示出来**（例如上条是 `ls`，新输入 `p` 却显示成 `ps`），看起来像"上次字符重复"。

## 2. 根因分析
shell 的输入/编辑显示统一走 term 网格路径：`ed_raw()` 就是 `term_puts()`（`user/shell.c:87`），把字节送入 `term_emit_char()` 写入字符网格 `g_tgrid`，再由 `term_render()` 光栅化到窗口。`redraw_full_line()` 在每次编辑后用 `ed_raw(g_line)`（即 `term_puts(g_line)`）把整行重印到网格——而 `term_puts` 以**遇到 `\0` 才停止**。

问题在于**编辑缓冲区 `g_line` 从来没有被可靠地以 `\0` 终止**：
- `edit_insert()`（约 147 行）：`g_line[g_cur]=c; g_len++; g_cur++;` —— 只更新长度，**不写 `g_line[g_len]='\0'`**。
- `edit_backspace()` / `edit_delete()`：删字符后 `g_len--`，**也不写 `g_line[g_len]='\0'`**。
- 命令执行后（约 1168 行）：`g_len = 0; g_cur = 0; g_line[0] = 0;` —— 只把首字节清零，**`g_line[1..]` 仍残留上一条命令的字节**（例如 `ls` 执行后 `g_line = "\0s\0..."`）。

于是下一次输入：
1. 用户输入 `p`，`edit_insert` 在 `g_cur=0` 处写入 `g_line[0]='p'`，`g_len=1`，但 `g_line[1]` 仍是旧命令残留的 `'s'`。
2. `redraw_full_line()` → `ed_raw(g_line)` → `term_puts("\0s\0..")` 起始虽是 `p`，但 `term_puts` 遇 `\0` 才停，`g_line[1]='s'` 不是 `\0`，于是把残留的 `s` 也一起印到了编辑行 → 显示 `ps`。

这就是"输入区显示上次重复字符"的成因——缓冲区未清 0 + 缺 `\0` 终止符，导致重绘时把历史残留字节一并输出。

## 3. 改动文件

### 3.1 `user/shell.c`（编辑缓冲区 `\0` 终止）
在三个修改 `g_line` 的编辑函数末尾补 `g_line[g_len] = '\0'` 截断，使缓冲区在任何编辑动作后都保持正确的 C 串终止，下一次输入从干净状态开始（等价于"清除输入缓冲区"）：

- `edit_insert()`：`g_len++; g_cur++;` 之后加 `g_line[g_len] = '\0';`
  ```c
  g_line[g_cur] = c;
  g_len++; g_cur++;
  g_line[g_len] = '\0';   /* 维持 C 串终止，避免旧命令残留字节被 redraw_full_line 印出 */
  redraw_from_cursor();
  ```
- `edit_backspace()`：`g_len--;` 之后加 `g_line[g_len] = '\0';`
  ```c
  g_len--;
  g_line[g_len] = '\0';   /* 截断，避免删后旧字符残留被重绘 */
  redraw_from_cursor();
  ```
- `edit_delete()`：`g_len--;` 之后加 `g_line[g_len] = '\0';`
  ```c
  g_len--;
  g_line[g_len] = '\0';   /* 截断，避免删后旧字符残留被重绘 */
  redraw_from_cursor();
  ```

`line_replace()`（历史上下浏览）本身已正确维护 `g_line[i] = '\0'`（约 204/208 行），Tab 补全经由 `edit_insert()` 间接获得截断，无需额外改动。命令执行后的 `g_len=0; g_cur=0; g_line[0]=0;` 与 `main` 初始化处的同等重置保留不变——配合上述 `\0` 维护，下一行输入必然从空缓冲区开始。

### 3.2 影响范围
- 仅影响行编辑的内部缓冲区终止性，不改变终端光标追踪/重绘逻辑（cursor tracking 由 `term_emit_char` 经 `ed_raw`= `term_puts` 统一维护，本就正确）。
- 退格、删除、中间插入、历史切换、Tab 补全后的重绘结果都不再串入历史残留字节。

## 4. 验证

### 4.1 构建
`make iso` → `ISO_EXIT=0`，无编译/链接错误（shell_out 前向声明与定义已在 step77 就位）。

### 4.2 Headless 回归（单核 70s）
- `KPF / panic / triple fault / OOPS` = **0**。
- `window created id=` = **3**（winhello + shell 两窗口均被 WM 创建）。
- `suki_create_window` 失败迹象 = **0**。
- `[shell] windowed mode: window id=` = **1**（shell 成功进入窗口化）。
- `[libc-test] PASS=` = **1**（FS 交互正常；`term_emit_char` 守卫确保窗口创建前自检不污染网格）。
- 系统跑通到 DHCP BOUND / 后期任务退出阶段，无卡死。

### 4.3 交互验证（用户人工；headless 无法注入键盘）
键盘字符注入受 QEMU headless 限制（sendkey 不触发键盘中断），无法自动化验证编辑区渲染，须用户在图形窗口人工确认。见末尾「手动验证步骤」。

## 5. 影响与说明
- 本次修复与 step77（命令输出双写、clear 清屏生效）正交：step77 解决"命令结果可见"，本步解决"下一次输入干净"。
- 修复后，编辑行重绘只经由 `term_emit_char` 终端仿真路径（含 CHA/EL 绝对列定位），缓冲区始终 `\0` 终止，残留字节不再被 `term_puts` 输出。
- 未触碰拖拽修复（step77）与窗口化逻辑，回归安全。

## 6. 提交
- 改动文件：`user/shell.c`、`results/step78.md`。
- 提交：`fix(shell): 编辑缓冲区缺 \0 终止导致命令执行后输入区残留上次字符 (step78)`（未推送）。

## 7. 手动验证步骤（请在图形窗口执行）
1. 启动图形 QEMU：
   ```
   make run
   ```
2. 在 Shell 窗口输入并执行一条较长命令，例如：
   ```
   cat <某个存在的文本文件>
   ```
   观察其输出正常显示在窗口（step77 已修复）。
3. **关键验证**：回车执行后，马上输入一条**更短**的新命令，例如只敲 `pwd` 或 `ls`：
   - 期望：编辑区只显示你正在输入的新字符（`pwd` 显示 `pwd`），**不应**出现上一条命令的残留尾巴（如 `cat...pwd` 之类）。
   - 反例（修复前）：上条是 `cat /FONTS/...`，新输入 `p` 却显示成 `p...`（残留旧字符）。
4. 额外验证编辑操作：输入 `hello`，用退格删到只剩 `he`，再输入 `y` → 应显示 `hey` 且无残留；按上箭头调出 `cat ...` 历史，按 `Home`/`End`、左右方向键移动光标再编辑，重绘后无残留乱码。
5. 若仍有残留/重复字符现象，请把复现步骤（上条命令内容、新输入内容、显示内容）反馈，我据此继续定位。
