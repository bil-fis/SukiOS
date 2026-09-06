# Step77 — 修复拖拽松手卡死与 shell 命令输出不显示

## 1. 用户反馈（图形界面手动验证后）

在 step76 修复「窗口过小 + 闪屏」后，用户在图形窗口中继续验证，反馈两个新问题：
1. **鼠标按住拖动，松手后无法放开** —— 拖拽一旦开始，松开鼠标也停不下来，窗口一直跟着光标走。
2. **键盘输入在 shell 内可以显示，但 shell 的命令输出无法显示** —— 打字回显可见，但执行 `ls`/`cat`/`echo`/`help` 等命令的结果不出现在窗口里（仅在 serial 控制台能看到）。

两个问题均属图形界面交互/渲染层面，需用户肉眼在图形窗口确认；本步骤完成代码修复并用 headless 验证了「功能性不崩溃、窗口与命令路径正常、自检不被污染」。

## 2. 根因分析

### 2.1 拖拽松手卡死
`user/display_server.c` 鼠标消息循环里的拖拽分支（约 408-430 行）：
```c
if (g_drag) {
    if (h->msgh_id == MOUSE_MSG_MOVE) { ... 跟随光标移动窗口 ...; composite(); }
    continue;   /* ← 关键：拖拽中收到任何消息（含 BUTTON 松开）都直接 continue */
}
```
拖拽态下对**所有**消息（MOUSE_MSG_MOVE / MOUSE_MSG_BUTTON / MOUSE_MSG_WHEEL）一律 `continue`，**BUTTON 松开事件从未被处理**，`g_drag` 永远不为 NULL，因而松手后拖拽永不结束（且后续每一次鼠标移动都继续把窗口拽走）。

### 2.2 shell 命令输出不显示
shell 的终端有两条并存的输出通道：
- **回显/提示符通道**：走 `term_puts()` → `term_emit_char()` 写入字符网格 `g_tgrid[TERM_ROWS][TERM_COLS]`，主循环每帧 `term_render()` 把网格光栅化到窗口离屏缓冲并 `suki_flush()`。键盘输入回显、提示符走此通道，故**输入可见**。
- **命令输出通道**：`fs_wait_and_print()`（line 382 `u_printn`）、`fs_wait_status()`（line 443-447 `u_print`）、`echo_print()`、`run_builtin_raw()` 里各 builtin（`help`/`ls`/`cat`/`cd`/`echo`/...，全文约 40 处 `u_print`）全部走 `u_print()` —— 而 `u_print()` 只写内核统一控制台（**串口镜像 + 启动时文本终端**），**不写 `g_tgrid`**。窗口创建后这些结果根本没进网格，故**窗口里看不到命令输出**（仅 serial 可见）。

换言之：step76 放大窗口后，窗口能开、输入能显，但「命令执行结果」仍只发往 serial，没接回窗口终端网格。

## 3. 改动文件

### 3.1 `user/display_server.c`（拖拽释放）
- 拖拽分支新增 BUTTON 处理：当 `h->msgh_id == MOUSE_MSG_BUTTON` 且 `(m->buttons & 1) == 0`（左键释放）时，将 `g_drag = NULL` 并 `composite()`；若该次松开命中某窗口，还补发一个 `SUKI_EVENT_MOUSE_UP` 事件给该窗口（保持与正常点击一致的事件语义）。
  ```c
  if (g_drag) {
      if (h->msgh_id == MOUSE_MSG_MOVE) {
          g_drag->x = m->x - g_drag_offx; g_drag->y = m->y - g_drag_offy; composite();
      } else if (h->msgh_id == MOUSE_MSG_BUTTON) {
          if (!(m->buttons & 1)) {            /* 左键释放 -> 结束拖拽 */
              g_drag = NULL;
              if (hit) { ... 下发 SUKI_EVENT_MOUSE_UP ... }
              composite();
          }
      }
      continue;
  }
  ```
- 把 `wm_window_t *hit = wm_hit_test(m->x, m->y);` 提到 `if (g_drag)` 之前（原定义在分支之后，导致新增代码引用 `hit` 时「未声明」）。现每次循环先算 `hit`，拖拽分支与非拖拽分支共用，逻辑等价，仅命中测试略提前。

### 3.2 `user/shell.c`（命令输出双写）
核心思路：新增**双写输出函数**，把原 `u_print`/`u_printn` 调用统一替换为「写终端网格（窗口显示）+ 串口镜像（headless 可观测）」，并加守卫避免在窗口创建前污染终端网格。

- 新增前向声明（line 78 前向声明区）：
  ```c
  static void shell_out(const char *s);
  static void shell_outn(const char *s, size_t n);
  ```
- 在 `term_puts()` 之后定义：
  ```c
  static void shell_out(const char *s) { term_puts(s); u_print(s); }
  static void shell_outn(const char *s, size_t n) {
      for (size_t i = 0; i < n; i++) term_emit_char(s[i]);
      g_term_dirty = true;
      u_printn(s, n);
  }
  ```
- `term_emit_char()` 开头加守卫：
  ```c
  if (!g_win) return;   /* 窗口未创建（启动/自检期）不写网格，避免污染终端缓冲 */
  ```
  作用：窗口创建前（如 `libc_selftest`、早期 `[shell]` 启动日志）调用 `shell_out` 时，`term_emit_char` 直接返回，**不写 `g_tgrid`**，仅 `u_print` 落 serial；窗口创建后（`g_win != NULL`）所有命令/交互输出同时进网格与 serial。这恰好复用原 `g_win==NULL` 时的 serial-only 降级路径。
- 全文 `u_print(` → `shell_out(`、`u_printn(` → `shell_outn(`（约 84 处，含 `fs_wait_and_print`/`fs_wait_status`/`echo_print`/各 builtin/提示符/帮助文本等）。至此 `ls`/`cat`/`echo`/`help`/`cd` 错误/`unknown command` 等一切用户可见输出均写入窗口终端网格。
- 附带修复：`clear` 命令原本 `u_print("\033[2J\033[H")` 只发 serial；改为 `shell_out` 后，`term_emit_char` 会解释该 VT100 转义序列执行清屏（终端仿真已实现 `ESC[2J`/`ESC[H`），`clear` 在窗口内真正生效。

## 4. 验证

### 4.1 构建
`make iso` → `ISO_EXIT=0`，无编译错误（修复了两会话期问题：拖拽分支 `hit` 未声明、shell_out 前向声明缺失）。

### 4.2 Headless 回归（单核 70s）
- `KPF / panic / triple fault / OOPS` = **0**。
- `window created id=` = **3**（winhello + shell 两窗口均被 WM 创建）。
- `suki_create_window` 失败迹象 = **0**。
- `[shell] windowed mode: window id=` = **1**（shell 成功进入窗口化）。
- `[libc-test] PASS=` = **1**（FS 交互正常，且 `term_emit_char` 守卫确保窗口创建前的自检输出不污染网格 —— 自检仍只落 serial）。
- serial 中 `SukiOS:`/`[shell]`/`[libc-test]` 输出正常出现（共 4 处匹配），证明 `shell_out` 的串口镜像与命令/交互输出路径未损坏。
- 末段运行到 DHCP BOUND / 后期任务退出阶段，系统完整跑通无卡死。

### 4.3 视觉验证（用户人工，headless 不可观测）
见末尾「手动验证步骤」：重点确认 (1) 拖拽窗口标题栏后**松开鼠标能正常停住**；(2) 在 shell 内输入 `ls`/`cat`/`echo hello`/`help` 等命令，**输出出现在窗口终端里**（不再只在 serial 可见）。

## 5. 影响与说明
- 拖拽修复后，鼠标交互完整：标题栏按下拖拽、松开结束，且松开时向目标窗口补发 MOUSE_UP，点击/拖拽事件语义一致。
- shell 输出现在走与回显相同的终端网格通道，窗口与 serial 同步可见；窗口创建前的启动/自检日志因 `term_emit_char` 守卫仍只落 serial，不污染交互终端（窗口打开后最先看到的是提示符与后续命令交互）。
- 对含二进制内容的 `cat`（如 `.bmp`）：`term_emit_char` 对 `<32` 非 `\n\r\b\t` 的控制字节会丢弃（与回显行为一致），纯文本与命令文本均正常。

## 6. 提交
- 改动文件：`user/display_server.c`、`user/shell.c`、`results/step77.md`。
- 提交：`fix(gui): 修复拖拽松手卡死并让 shell 命令输出双写到窗口终端网格 (step77)`（未推送）。

## 7. 手动验证步骤（请在图形窗口执行）
1. 启动图形 QEMU：
   ```
   make run
   ```
   （窗口参数见 step76；确保 `-display gtk` 或默认图形输出）
2. **拖拽验证**：在 Shell 或 WinHello 窗口标题栏按住左键拖动，移动到目标位置后**松开鼠标** —— 窗口应停在新位置，不再跟随光标（`g_drag` 已清零）。
3. **命令输出验证**：在 Shell 窗口内依次执行：
   - `ls` —— 应列出根目录文件，且**显示在窗口终端里**（非仅 serial）。
   - `cat <某文本文件>` —— 文件内容显示在窗口。
   - `echo hello world` —— 输出 `hello world` 显示在窗口。
   - `help` —— 帮助文本显示在窗口。
   - `clear` —— 窗口清屏（验证 VT100 转义在窗口内生效）。
4. 若仍发现某类输出不显示或拖拽异常，请把现象反馈，我据此继续细化（例如可把 `[shell]` 启动日志改回纯 serial 以避免窗口顶部混入启动信息）。
