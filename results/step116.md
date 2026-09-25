# Step 116 — iSukiDemo 事件循环改为常驻（关闭窗口才退出）

## 1. 目标与背景

用户反馈（基于 step115 的 headless 回归日志）：

```
[isukidemo] lifecycle complete, exiting
[syscall] task 'iSukiDemo' pid=22 exit(code=0)
[wm] destroy recv id=4
[wm] window destroyed
```

`isukidemo`（已改名为 iSukiDemo，见 step115）完成自检后会**自动退出**并销毁窗口。用户要求：**不要退出，调整生命周期**——即让它像真正的 GUI 应用一样常驻运行，直到用户主动关闭窗口才退出。

---

## 2. 根因

`isukidemo.c` 的 `main` 末尾事件循环带**有界计数器**：

```c
/* 旧代码（step115 形态） */
sui_event_t ev;
int limit = 6000;
while (w->running && limit-- > 0) {
    if (!sui_window_step(w, &ev))
        sys_yield();
}
u_print("[isukidemo] lifecycle complete, exiting\n");
```

- `sui_window_step(w, &ev)`（libsui）在**无事件**时返回 `false`，于是 `sys_yield()` 让出 CPU。
- headless（无键盘/鼠标输入）下始终无事件，循环纯 `sys_yield` 空转，约 6000 步（几秒）即耗尽 `limit` 退出 → 表现为"自检完就退出、窗口被销毁"。
- 即便是图形环境，不交互时同理会走到上限退出。

---

## 3. 改动内容

**文件：`user/apps/isukidemo.c`**（仅此一处）

把有界循环改为**无限常驻**，退出完全交由"用户关闭窗口"事件驱动：

```c
/* 事件循环：常驻运行，直至用户关闭窗口。
 * libsui 在收到 SUI_EVENT_WINDOW_CLOSE 时会把 w->running 置 false
 * （见 sui_core.c:771），故此处不再设 limit——此前 6000 步即自动退出，
 * 现改为真正交互式常驻：图形环境持续运行，关闭窗口（标题栏 × / ESC）才退出。
 * headless（无输入）下本循环持续 yield 不退出；自检信号已于上方 PASS 行输出，
 * 回归仅据此判断，无需本程序主动 exit。 */
sui_event_t ev;
while (w->running) {
    if (!sui_window_step(w, &ev))
        sys_yield();
}

u_print("[isukidemo] window closed, exiting\n");
SukiDestroyWindow(w->wk);
sys_exit(0);
return 0;
```

退出机制（无需在 demo 内手动处理 `ev`）：
- `sui_window_step`（`user/libsui/src/sui_core.c:766`）拉取一个事件；若类型为 `SUI_EVENT_WINDOW_CLOSE`，**直接把 `win->running` 置 `false`**（sui_core.c:771）并返回 `true`。
- `SUI_EVENT_WINDOW_CLOSE` 由显示服务器在用户操作关闭时经**窗口事件端口**下发（main 已 `sui_window_set_event_port(w)` 注册端口，见 isukidemo.c:442）。
- 触发关闭的入口：标题栏关闭按钮、ESC（具体由显示/输入服务与 libsui 按键映射决定）。
- `w->running` 初始值由 `sui_create_window` 置 `true`（step115 已验证循环可进入），故 `while (w->running)` 在收到关闭事件前永不退出。

控件回调（`on_nav` / `on_theme` / 各 `on_*`）已由 libsui 在 `sui_window_step` 内部经 `sui_dispatch_event` 自动分发，不依赖本循环额外处理。

---

## 4. 关键技术细节

- **事件循环 API**：`bool sui_window_step(sui_window_t*, sui_event_t*)`（`user/libsui/include/sui.h:425`）——单帧拉事件 + 命中/焦点分发 + 渲染 + present；无事件返回 `false`。
- **关闭事件常量**：`SUI_EVENT_WINDOW_CLOSE`（`sui.h:375`）。
- **running 字段**：`bool running`（`sui.h:406`），由 `sui_create_window` 初始化为 `true`，`sui_window_step` 在关闭事件时置 `false`。
- **退出路径**：循环退出后 `SukiDestroyWindow(w->wk)` 通知显示服务器销毁窗口，`sys_exit(0)` 结束任务（先前的窗口销毁日志 `[wm] destroy recv` 由此而来）。

---

## 5. 验证

### 5.1 headless QEMU 回归（自动）

命令：
```
make iso disk
make run-headless QEMU_SERIAL="-serial file:/tmp/sui7.log" RUN_TIMEOUT=40
```
日志结论（`/tmp/sui7.log`）：
```
[shell] iSukiDemo launched
[isukidemo] start (iSukiUI concept replica)
[isukidemo] window rendered + flushed (all pages + dialog self-checked) -> PASS
[isukidemo] controls: button/input/.../toast OK
```
- isukidemo 自检 **PASS**、controls OK；其后**无任何** `lifecycle complete` / `window closed` 退出信息 → 证实**不再自动退出**，已进入常驻循环（被 `timeout` 杀，退出码 124 属预期，非 panic）。
- 用户态 `#PF` 仅 1 处：`pid=12 'SukiPosixTest' rip=0x402e94 cr2=0xfffffffffffffffe`（即 `-2` 指针），为 POSIX 兼容自测里某未实现 syscall 返回 `-ENOSYS` 被解引用的**既有用例**，与本次改动无关；**零新增 panic**。
- `winhello` / `suikitest` 仍正常自检退出，系统整体稳定。

### 5.2 关闭窗口退出（图形环境，待手动验证）

headless 无输入无法触发 `SUI_EVENT_WINDOW_CLOSE`，故"用户关闭窗口 → 干净退出"需在图形会话中肉眼确认（见第 6 节 notify 步骤）。代码路径已静态确认：`sui_window_step` 置 `running=false` → 循环退出 → 打印 `window closed, exiting` → `SukiDestroyWindow` → `sys_exit(0)`。

---

## 6. 改动文件清单

- `user/apps/isukidemo.c` — 事件循环由有界（limit=6000）改为无限常驻 `while (w->running)`；退出信息文案改为 `window closed, exiting`（旧 `lifecycle complete, exiting` 已删除）。

---

## 7. 结论

iSukiDemo 已调整为**常驻事件循环**：完成自检后保持窗口运行、持续可交互，仅在用户主动关闭窗口（标题栏 × / ESC）时干净退出。headless 回归确认不再自动退出、零 panic。关闭退出路径代码已就绪，待图形环境手动验证。
