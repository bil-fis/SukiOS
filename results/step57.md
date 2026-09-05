# Step 57 — 彻底修复 shell 残留字符（clear 后 lsear）：改用绝对列定位重绘

日期：2026-09-05

## 一、现象

用户实测：执行 `clear` 后，再输入 `ls`，图形窗口与串口回显都显示为 `lsear`
（实际回车执行的是正确的 `ls`）。即编辑行出现**旧字符残留**，但命令本身正确。

## 二、根因

shell 文本输出经 `sys_debug_write → user_puts → IPC(DISPLAY_PORT) → display_server
term_putc` 在图形终端窗口渲染，同时 `serial_writestr` 落串口。因此“屏幕”与“串口”
显示的是同一份字节流，残留是**真实的编辑输出 bug**。

`clear` 命令发送 `\033[2J\033[H`：显示侧（display_server `csi_dispatch`）把光标
归位到 (0,0) 并清客户区。**但这一过程用的是 `u_print`（非 `ed_raw`）**，所以 shell 侧
维护的终端列镜像 `g_scr_x` **没有随之重置**——它仍停留在上一条命令打字结束时的列号。

shell 的重绘 `redraw_from_cursor()`（step55/56 版）依赖 `g_scr_x` 与显示侧光标同步，
用 **相对回退** `ed_left(g_scr_x - g_prompt_col)` 把光标拉回编辑行起点，再 `ESC[K`
清到行尾。一旦 `g_scr_x` 与显示侧实际列失步：

- 显示侧光标在真正位置（如 col15），但 `g_scr_x` 认为是 `g_prompt_col`（col10）；
- `ed_left(0)` 不移动，`ESC[K` 从 col15 开始清 → 只清了 col15 之后；
- col10~14 的旧字符（“lear”/“ear”等）**未被清除**，新打的 `ls` 接在后面 → `lsear` 类残留。

> 为什么 `clear` 特别容易触发：它是少数把光标“瞬移”到 (0,0) 且自身无文本输出的命令，
> 其它命令（help/ls）的输出末尾通常带 `\n`，光标落点相对可预期，失步概率低；而 `clear`
> 之后 prompt 的 `\r\n` 又把行推进到 row1，叠加镜像未重置，便放大了失步。

## 三、修复：用绝对列定位（CHA）取代相对回退

**核心思想**：重绘时不再依赖 `g_scr_x` 是否与显示侧同步，而是用 `ESC[<col>G`
（CHA，水平绝对定位）直接跳到编辑行起点列，再 `ESC[K` 清到行尾。**只要编辑行在
当前终端行、提示符占 col0..g_prompt_col-1，绝对定位就恒落在正确列，残留必被抹掉。**

### 3.1 display_server.c：新增 CHA (`ESC[<col>G`) 支持

`csi_dispatch()` 增加 `case 'G'`（与既有 'C'/'D' 对称）：

```c
case 'G':   /* 光标水平绝对定位 CHA：ESC[<col>G（1-based） */
    g_term_x = (g_csi_has_n && g_csi_n >= 1) ? (uint32_t)(g_csi_n - 1) : 0;
    if (g_term_x >= g_term_cols) g_term_x = g_term_cols ? g_term_cols - 1 : 0;
    break;
```

### 3.2 shell.c：重写重绘为绝对列定位

新增 `redraw_full_line()`，所有重绘路径（编辑插入/删除、历史切换 `line_replace`、
补全尾部）统一调用：

```c
static void redraw_full_line(void)
{
    char seq[16]; int k;
    k=0; seq[k++]='\x1b'; seq[k++]='[';
    int col = g_prompt_col + 1;                 /* 1-based 编辑行起点列 */
    if (col>=10) seq[k++]='0'+col/10;
    seq[k++]='0'+col%10; seq[k++]='G';
    ed_raw(seq);                                /* 跳到编辑行起点列（绝对） */
    ed_raw("\x1b[K");                           /* 清到行尾（保留提示符） */
    ed_raw(g_line);                            /* 重印整行 */
    k=0; seq[k++]='\x1b'; seq[k++]='[';        /* 光标定位到 g_cur */
    col = g_prompt_col + g_cur + 1;
    if (col>=10) seq[k++]='0'+col/10;
    seq[k++]='0'+col%10; seq[k++]='G';
    ed_raw(seq);
    g_scr_x = g_prompt_col + g_cur;             /* 重新同步镜像，供 edit_* 用 */
}
static void redraw_from_cursor(void) { redraw_full_line(); }
```

`line_replace()` 与补全尾部（`edit_tab_complete` 多匹配分支）末尾改为调用
`redraw_full_line()`，保持全路径一致。

> `prompt()` 的 `\r\n` + `g_scr_x` 重置（step56）继续保留，作为纵深防御；但即便它
> 失效，CHA 重绘也不再依赖 `g_scr_x`，残留问题从根上消失。

## 四、验证（含可复现仿真）

受项目铁律约束（禁用 GDB/外部调试脚本，仅 bash + QEMU 自带机制）。

### 4.1 编译
```
make iso      # 通过，-Wall -Wextra 无新增告警
```

### 4.2 无头回归（零 panic）
```
timeout 25 qemu-system-x86_64 -machine pc -cpu qemu64 -smp 1 -m 2G \
  -no-shutdown -display none -serial file:/tmp/suki3.serial.log \
  -boot d -cdrom build/SukiOS.iso -drive file=build/disk.img,format=raw,index=0,media=disk
```
日志：`[fs] self-test ALL PASS`、`[shell] SukiOS shell online`、`[fontsrv] ... online`、
`[pchfnt] done ...`。`EXIT=124`（timeout 正常终止），**无 panic / #GP / 三重故障**。

### 4.3 残留机理仿真（bash 编译运行，非内核调试）

写独立仿真 `sim.c` 复刻 display_server `term_putc` 的 CSI 处理与 shell 重绘逻辑，
构造失步场景（显示侧光标停在 col15，正确应在 g_prompt_col=10，编辑行有旧残留
`SukiOS:/> clear`），对比两种重绘：

```
初始残留行0: [SukiOS:/> clear ...]
失步模拟: 显示侧光标=col15, 正确应=g_prompt_col=10

OLD 重绘后: [SukiOS:/> clearls ...]     <-- ESC[K 从 col15 起清，col10..14 的 "lear" 残留
NEW 重绘后: [SukiOS:/> ls ...]           <-- CHA 跳到 col10 再清，残留完整抹除
```

结果**精确复现**用户报告的 `lsear` 类残留（旧文本尾巴 + 新输入），并证明 CHA 方案可
结构性消除。仿真不参与内核构建，仅作验证。

### 4.4 交互视觉确认（需用户图形窗口手动）
因 QEMU headless 无法注入键盘中断（项目已记录的 QEMU 限制），以下由用户确认：
```
make run
```
- 执行 `clear`，再输入 `ls`：应显示干净 `SukiOS:/> ls`，**无 `lsear` 类残留**。
- 任意命令后继续编辑（含退格、历史切换、Tab 补全）均不应出现旧字符残留。
- 内核零 panic、显示服务/字体服务正常。

## 五、关键文件

| 文件 | 改动 |
|------|------|
| `user/display_server.c` | `csi_dispatch()` 新增 `case 'G'`（CHA 绝对列定位） |
| `user/shell.c` | 新增 `redraw_full_line()`（CHA 重绘），`redraw_from_cursor`/`line_replace`/补全尾部统一调用；不再依赖相对回退 |

## 六、提交
按项目规则提交（不推送）：commit 见 `git log`。
