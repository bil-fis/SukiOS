# Step 56 — 修复 shell 残留字符（g_scr_x 失步）+ Tab 补全路径 + ls 列当前目录

日期：2026-09-05

## 一、问题现象与根因

用户实测图形窗口出现 `lsear` 等残留字符（输入被"吞"或残留）。根因分析：

shell 在行编辑期维护一个**终端光标的用户态镜像** `g_scr_x`（与显示服务 `g_term_x`
同步）。`redraw_from_cursor()` 依赖它来计算"回退到编辑行起点"所需的左移列数。
但**执行命令（尤其是有输出且输出未以换行结尾）后，显示侧光标停在任意列，而
`g_scr_x` 并未随之更新**——因为 `prompt()` 只在末尾把 `g_scr_x` 设成 `g_prompt_col`
（编辑行起点），却假定显示侧光标此刻也正好在该起点列。若上一条命令把光标留在了
行中间（如 `echo -n`、末尾无 `\n` 的 `cat`、`clear` 后的脏列等），则 `prompt()` 把
提示符打印在错误列，`g_scr_x` 与显示侧实际列失步，后续 `redraw_from_cursor()` 在
错误起点 `ESC[K` 清行尾，旧字符无法被完整清除 → 残留。

> 注：前一步（step55）的退格修复本身正确，但**镜像在所有命令输出路径上并未被维护**，
> 故命令输出造成的失步不在其覆盖范围内。本次从根上保证 prompt 起点复位。

---

## 二、修复 #1：prompt() 强制复位光标镜像（核心）

`user/shell.c` 的 `prompt()` 开头强制「回车+换行」，把显示侧光标拉回新行行首，并
同时清零镜像，使后续 `g_scr_x = g_prompt_col` 与显示侧真实列一致：

```c
static void prompt(void)
{
    /* 关键：强制 \r\n 让终端光标回到新行行首并重置镜像。否则若上一条命令
     * 输出未以换行结尾，显示侧光标停在任意列而 g_scr_x 仍按 g_prompt_col 计算，
     * 二者失步 → redraw_from_cursor() 清行尾起点算错 → 旧字符残留（如 "lsear"）。 */
    ed_raw("\r\n");
    g_scr_x = 0;
    u_print("SukiOS:");
    u_print(g_cwd);
    u_print("> ");
    g_prompt_col = 9 + (int)strlen(g_cwd);
    g_scr_x = g_prompt_col;
}
```

- `ed_raw("\r\n")`：`ed_track` 对 `\r`/`\n` 均把 `g_scr_x` 置 0，显示侧则回到新行第 0 列；
  `prompt()` 结尾再把 `g_scr_x` 设成 `g_prompt_col`，与"显示侧列 = 0 + 提示符长度 =
  `g_prompt_col`"严丝合缝。
- 代价：每次提示符前多一个空行（boot 首行、命令之间均会空一行）。这是为保证光标
  始终从新行第 0 列开始、彻底消除失步类残留的可接受折中（用户已确认）。
- 该复位对"命令输出是否换行"完全免疫：无论上条命令把光标留在哪，`\r\n` 都先归位，
  后续编辑重绘不再依赖命令输出阶段的镜像追踪。

---

## 三、修复 #2：Tab 补全相对路径拼接错误

`user/shell.c` 的 `edit_tab_complete()`：当 `frag` 含 `/` 且为相对路径时，原代码把
**整个 `frag`**（含基础名）拼到 `g_cwd` 之后，导致 `opendir` 拿到形如 `/SUB/F` 的文件
路径（而非目录 `/SUB`），枚举失败、补全无反应。

修复：只拼接**目录部分 `dir`**，而非整个 `frag`：

```c
if (dir[0] != '/') {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s%s%s", g_cwd,
             (g_cwd[0] && g_cwd[strlen(g_cwd)-1] != '/') ? "/" : "",
             dir);                     /* dir 已拆出目录部分，不含基础名 */
    strncpy(dir, tmp, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
}
```

- `dir` 在 `strrchr(frag,'/')` 处已拆为目录部分；绝对路径（`dir[0]=='/'`）不走此分支，
  保持原样。无斜杠的相对情形 `dir` 已被置为 `g_cwd`，同样跳过此分支。
- 结果：`cd SUB/F` 补全时对 `/SUB` 做 `opendir`，正确枚举。

---

## 四、修复 #3：ls 应列出当前工作目录

`user/shell.c` 的 `ls` 原实现 `fs_request(FS_MSG_LIST, 0)`——把 `arg` 传 `0`（NULL），
FS 服务端 `handle_list` 又**硬编码列根目录 `"/"`**，因此无论 `cd` 到哪里，`ls` 永远列根。

修复：shell 侧传入当前目录 `g_cwd`（绝对路径）：

```c
fs_request(FS_MSG_LIST, g_cwd);
fs_wait_and_print(FS_MSG_LIST, false);
```

`user/fs_server.c` 侧配套：

1. `handle_list(uint32_t local_port, uint32_t id, const char *path)` 新增 `path` 形参，
   用 `normalize_path()` 归一化后 `f_opendir(&g_dir, pbuf)`（原硬编码 `"/"`）。
2. `normalize_path()`：绝对路径去掉前导 `/`，空串特判为卷根 `0:`（FatFs 要求），
   因此 `g_cwd="/"` → `0:` 列卷根；`g_cwd="/SUB"` → `SUB` 相对 FatFs 当前目录（根）→
   `/SUB`，行为正确。
3. 服务主循环 dispatch 处改为 `handle_list(local, id, (const char *)payload);`，
   把请求负载中的路径字符串传给处理函数。
4. `fs_request()` 的 `req.name[64]` 装载路径；`g_cwd` 通常很短，远小于 63 字符上限，
   无截断风险（深层目录极端情况属已知边界，与现有设计一致）。

---

## 五、关键文件与符号

| 文件 | 改动 |
|------|------|
| `user/shell.c` | `prompt()` 开头 `ed_raw("\r\n")` + `g_scr_x=0` 复位镜像；`edit_tab_complete()` 相对路径只拼目录部分；`ls` 传 `g_cwd` |
| `user/fs_server.c` | `handle_list()` 接受 `path` 参数并按其列目录；dispatch 传 `payload` 路径 |

相关既有符号：`g_scr_x`/`g_prompt_col`（shell 镜像）、`ed_raw()`/`ed_track()`（step55
新增）、`FS_MSG_LIST`、`normalize_path()`（fs_server，绝对/相对路径归一）。

---

## 六、验证方式

受项目铁律约束（禁用 GDB/外部调试脚本，仅用 bash + QEMU 自带机制）。

### 1. 编译
```
make iso
```
通过，`-Wall -Wextra` 无新增告警。

### 2. 无头回归（零 panic）
```
timeout 25 qemu-system-x86_64 -machine pc -cpu qemu64 -smp 1 -m 2G \
  -no-shutdown -display none -serial file:/tmp/suki2.serial.log \
  -boot d -cdrom build/SukiOS.iso -drive file=build/disk.img,format=raw,index=0,media=disk
```
`grep` 关键输出：
```
[fs] self-test ALL PASS
[shell] SukiOS shell online (Ring3, bash-like)
[fontsrv] font service online (FreeType, FONT_PORT=10)
```
`EXIT=124`（timeout 正常终止），**全程无任何 panic / #GP / 三重故障**。

### 3. 交互行为（需用户在图形窗口手动确认）
因 QEMU headless 无法注入键盘中断（项目已记录的 QEMU 限制），以下须由用户手动验证：

```
make run
```
- **残留修复**：依次输入 `echo -n hi`（不换行）后直接输入 `ls` 等命令，确认提示符
  出现在新行、无 `lsear` 类残留；任意命令输出后继续输入均不再出现旧字符残留。
- **Tab 补全**：`cd` 到含子目录的路径（如 `cd /FONTS` 后输入 `RESOUR`+Tab）应能补全；
  含 `/` 的相对片段（如 `cd FONTS/RE`+Tab）应基于目录部分枚举而非整串拼接。
- **ls 当前目录**：`cd /FONTS` 后 `ls` 应列出 `/FONTS` 下文件，而非总是根目录；
  `cd /` 后 `ls` 列出根。

通过判据：无残留字符、Tab 补全正确枚举当前目录、ls 跟随 `cd` 切换目录、且内核零 panic。

---

## 七、提交

按项目规则提交（不推送）：commit 信息见 `git log`。
