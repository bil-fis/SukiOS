# Step74 — 移除窗口化 shell 的串口输出镜像

## 1. 目标

`step73` 把 `shell` 改造为窗口程序时，为便于 headless 观测，在 `term_puts` 中把终端输出**同时镜像 `u_print` 到串口**。本次按需求移除该镜像：shell 正常终端输出仅渲染进窗口，不再写串口。

## 2. 改动

`user/shell.c` 的 `term_puts()`：

```c
/* 改前 */
static void term_puts(const char *s)
{
    const char *orig = s;
    for (; s && *s; s++) term_emit_char(*s);
    g_term_dirty = true;
    if (orig) u_print(orig);   /* 镜像到 serial（headless 可观测） */
}

/* 改后 */
static void term_puts(const char *s)
{
    for (; s && *s; s++) term_emit_char(*s);
    g_term_dirty = true;
}
```

仅删除 `orig` 临时变量与末尾 `u_print(orig)` 镜像调用。`ed_raw()`/`handle_key()`/提示符/命令回显等仍经 `term_puts` → 窗口网格 → `term_render`，行为不变。

> 启动诊断（`u_print("[shell] windowed mode: window id=...")`）与 `libc_selftest` 内直接的 `u_print`（如 `[libc-test] PASS=`）不经过 `term_puts`，仍走串口——这些属于诊断日志而非 shell 终端回显，保留以便系统运维观测。

## 3. 验证

- `make iso` 成功（ISO_EXIT=0）。
- headless（`-display none -serial stdio`，单核 150s）：
  - 全系统零 panic / triple fault：✅ 0
  - `[shell] windowed mode: window id=`：✅ 1（窗口创建成功、自动获焦）
  - `[libc-test] PASS=`：✅ 1（证明 FS 交互 `opendir("/")` 正常，未因移除镜像而受影响）
  - `[wm] window created`：✅ 2（winhello + shell 均成功，OOL 合成无崩溃）
  - 末 4 行为正常服务启动（pchfnt 等），无卡死/崩溃迹象

## 4. 影响说明

- headless 下：shell 终端内容（提示符、命令回显、命令结果）不再经串口可见；但系统级诊断（`windowed mode`、各服务日志、`libc-test`）仍可见，足以判断系统健康。
- 图形窗口下：用户在 **Shell** 窗口内正常看到终端输出（窗口渲染），与移除前视觉一致。

## 5. 提交

- 改动文件：`user/shell.c`、`results/step74.md`。
- 提交：`refactor(gui): 移除窗口化 shell 的串口输出镜像 (step74)`（未推送）。
