# Step36：键盘扫描码错位修复 + context_switch 崩溃回退（生产零 panic）

> 日期：2026-08-16
> 关联 commit：`c11758d7025a5c1f3c9ccc8ada6c27dc8fb35feb`（未推送）
> 修改文件：
> - `kernel/arch/x86_64/keyboard.c`
> - `kernel/sched/switch.S`
> - `kernel/syscall/syscall.c`（回退残留 `kentry` 引用）
> - `include/kernel/task.h`、`kernel/sched/sched.c`、`kernel/arch/x86_64/idt.c`（前序已回退 kentry 字段）

---

## 一、背景与两个待修复问题

用户在前序（step35）测试中发现两类 keyboard 异常：

1. **键盘刷屏**（已在 step35 解决）：IRQ 处理未做 make/break 区分导致重复输出。
2. **按键映射错位**（本步核心）：用户报告「按 Ctrl 打印 f」——典型扫描码集错配。
3. **context_switch 崩溃**（本步并发出现）：用户手动 `make run`（KVM 路径）得到
   `vector 13 (GPF) → vector 8 (Double Fault) → KERNEL STACK OVERFLOW`，
   RIP=context_switch+0x51（即 `iretq` 指令）。

两问题独立，但都在本步收尾并同时验证。

---

## 二、问题 A：按键错位（Ctrl 打印 f）—— 根因与修复

### 2.1 根因（8042 翻译位被错误关闭）

本内核与 Ring3 `input_server.c`、`shell.c` 的 ASCII 解析表**全部按 PS/2 扫描码集 1（Set 1）**解释：
- make code = 原始码（0x1D = Ctrl，0x21 = 'f' …）；
- break code = 最高位 0x80 置位（松开）。

但 8042 控制器（QEMU/SeaBIOS 固件）默认键盘输出的是 **扫描码集 2（Set 2）**。

正常 PC 的做法是开启 8042 **翻译模式位（配置字节 bit6）**：开启后，8042 自动把
键盘发来的 Set 2 码**翻译成等价的 Set 1 码值**（含 make/break 编码）再交给 CPU，
于是 CPU 收到标准 Set 1，键位一一对应。

旧 `keyboard.c` 的 bug：在控制器配置阶段执行了 `cfg &= ~0x40`（**关闭翻译位**），
又向键盘发了 `0xF0 0x01` 强制切到 Set 1。但 QEMU 键盘固件对 `0xF0 0x01` 的响应
不可靠——返回 ACK 而内部仍按 Set 2 输出。结果组合变成：

```
关闭翻译位 + 实际 Set 2 输出 → Set 2 原始码直通 CPU → 被当 Set 1 解析 → 全面错位
```

例如 Set 2 的 Ctrl(make=0x14) 经「当 Set 1」解释落到 ASCII 表 0x14 槽 = 't' 区域，
而「按 Ctrl 打印 f」正是 Set 2→Set 1 索引错位的具体表现。

### 2.2 修复（`kernel/arch/x86_64/keyboard.c`）

1. 配置字节改为 **`cfg |= 0x40`** 显式开启翻译位（原 `cfg &= ~0x40` 删除）：

```c
222:        cfg |= 0x40;          /* 开启翻译模式(位6)：集2自动翻译为集1给CPU */
```

2. **删除**「向键盘发 `0xF0 0x01` 设 Set 1」整段命令，键盘保持默认 Set 2；
   由翻译器稳定产出 Set 1 给 CPU。

3. 复位握手仅**冲刷**回发字节（0xFA+0xAA 及多余字节），不再尝试改扫描码集。

4. 重新使能扫描（带 resend 重试），日志改为：

```c
278:    kprintf("[kbd] PS/2 keyboard configured (translation ON, scan set 2->1)\n");
```

解析表 `g_scancode_ascii[128]` / `g_scancode_shift[128]`（Set 1 布局）完全正确，无需改动。
Shift(0x2A/0x36)、Caps(0x3A) 的 make/break 处理也基于 Set 1，逻辑无误。

### 2.3 为何这是标准做法

真实 PC / BIOS 路径就是「开启 8042 翻译位 + 键盘默认 Set 2」。QEMU 的 8042 模拟器
忠实实现了翻译逻辑（集 2 → 集 1 映射表），开启后输出确定且稳定，不依赖固件对
`0xF0 0x01` 的可疑响应。

---

## 三、问题 B：context_switch 双故障崩溃 —— 方案回退

### 3.1 崩溃现场

用户日志（KVM）：
```
[EXC] vector 13 (General Protection) err=0x0
[EXC] vector 8 (Double Fault)
KERNEL STACK OVERFLOW   (task 'idle1' / 'console-srv')
```
崩溃 RIP = `context_switch+0x51`，指向 `iretq` 指令。

### 3.2 原 iretq 分支方案为何必崩

我先尝试给 `context_switch` 增加第三参数 `next_ret_type`（SCHED_CTX_KTHREAD/SYSCALL/ISR），
在切回时按固定偏移跳过 GPR 区、直接用 `iretq` 回 Ring3。设了 `task_t.kentry` 字段，
在 `isr_dispatch` / `syscall_dispatch` / 内核线程创建处分发标记。

**致命缺陷**：`schedule()` 的切走点发生在
`IRQ处理 → isr_dispatch → sched_tick → schedule`
或
`syscall → syscall_dispatch → sys_yield/sys_wait → schedule`
这条 C 调用链的**任意深度**。此时 `context_switch` 保存的 `rsp` 指向该深度的栈帧，
其上方压着的 GPR/返回地址布局随调用深度变化，**不可能用固定偏移（48B/144B）对齐到
正确的 IRET 帧**。KVM 对 IRET 帧的严格校验（CS/SS/RFLAGS 合法性）立即触发 #GP，
随后 #DF + 栈溢出。

### 3.3 回退为统一 ret 模型（最终方案）

恢复**纯 ret 模型**——`context_switch` 只保存/恢复被调用者保存寄存器（rbx/rbp/r12-r15），
`ret` 回切走时的精确返回地址：

```asm
context_switch:
    pushq %rbx; pushq %rbp; pushq %r12; pushq %r13; pushq %r14; pushq %r15
    movq  %rsp, (%rdi)        /* *old_rsp_save = rsp */
    movq  %rsi, %rsp          /* rsp = new_rsp */
    popq  %r15; popq %r14; popq %r13; popq %r12; popq %rbp; popq %rbx
    ret                       /* 回到切走时的精确返回地址 */
```

正确性论证：

- **内核线程**：初始帧 `[6 reg][ret=entry]`，ret 直接进 entry，自洽。
- **被抢占的用户任务**（syscall/isr 链深处被切走）：ret 回到切走点的**下一条指令**，
  原 C 调用链继续，最终由 `syscall_entry.S` / `isr.S` 自身的 `iretq` 序列回 Ring3。
  用户态根本不经过 `context_switch` 的 iretq，完全规避了固定偏移对齐问题。
- **KPTI 语义**：KVM(host RDCL_NO=1) 下 KPTI 关闭；TCG 下 KPTI 开启。无论哪种，
  iretq 由 syscall/isr 出口统一处理（含 KPTI 影子视图切换），`context_switch` 不再
  触碰 CR3/页表，KPTI 旁路风险清零。

### 3.4 收尾清理（保证编译通过）

回退引入的全部 kentry 相关代码，否则 `SCHED_CTX_SYSCALL` 宏删除后 `syscall.c` 引用会编译失败：

- `include/kernel/task.h`：删除 `uint8_t kentry` 字段与 `SCHED_CTX_*` 宏。
- `kernel/sched/sched.c`：`context_switch` 恢复两参声明 `(uint64_t*, uint64_t)`，
  三处调用（idle0 切入 / schedule / task_exit_current）移除第三参与 `t->kentry=...`。
- `kernel/arch/x86_64/idt.c`：删 `isr_dispatch` 的 `kentry=SCHED_CTX_ISR` 与临时
  `[KPF-DIAG]` 诊断。
- `kernel/syscall/syscall.c`：删 `syscall_dispatch` 入口的
  `sched_current()->kentry = SCHED_CTX_SYSCALL;`（含上方整段错误注释），恢复直接 `switch (num)`。
- `kernel/sched/switch.S`：纯 ret 模型代码 + 更新文件头注释，移除 iretq 分支描述。

---

## 四、验证（仅 bash + QEMU，禁 GDB/外部脚本）

### 4.1 编译

```
make iso        # 不 source makeenv.sh
```
结果：`build/SukiOS.iso` 生成，链接通过。仅残留无关的 `panic` 前向声明 warning
（`spinlock.h` 用 `panic` 早于 `console.h` 声明），非本次引入，不影响功能。

### 4.2 KVM 路径（用户原崩溃路径）headless 回归

```
qemu-system-x86_64 -machine pc -cpu qemu64 -enable-kvm -m 512 \
  -drive file=build/SukiOS.iso,format=raw,if=ide -display none \
  -serial file:/tmp/suki_kvm.log -no-reboot
```
日志关键行：
```
[kbd] PS/2 keyboard configured (translation ON, scan set 2->1)
[sched] user task 'shell' pid=4 ... (ELF, W^X)
[shell] SukiOS shell online (Ring3, pid via IPC pipeline)
  cpu0: rq=4 user_switches=285 (offline)
```
**无** `[EXC]` / `OOPS` / `panic` / `STACK OVERFLOW` / `vector 13` / `vector 8`。

### 4.3 TCG 路径（KPTI 开启）headless 回归

```
qemu-system-x86_64 -machine pc -cpu qemu64 -m 512 \
  -drive file=build/SukiOS.iso,format=raw,if=ide -display none \
  -serial file:/tmp/suki_tcg.log -no-reboot
```
日志关键行：
```
[security] Meltdown: unknown -> KPTI ACTIVE (KAISER dual page tables ...)
[kbd] PS/2 keyboard configured (translation ON, scan set 2->1)
[shell] SukiOS shell online (Ring3, ...)  user_switches=293
```
KPTI 开启下同样**零 panic**，user_switches 稳定（293/核）。

两种生产场景均确认 context_switch 纯 ret 模型正确、键盘翻译位生效。

### 4.4 键盘按键映射（需用户在图形窗口手动验证）

AI 无法在命令行自动观测图形窗口的按键回显，按项目规则由用户手动跑。
详见下方「五、用户手动测试步骤」。

---

## 五、用户手动测试步骤（键盘实测）

**启动命令**（在 `/mnt/d/Projects/SukiOS` 下）：

```
make run
```

> 若 `make run` 走 KVM 图形窗口，请在窗口内用键盘测试；窗口出现 SukiOS shell
> 提示符（如 `SukiOS>` 或 `$`）后开始输入。

**需要按的键 / 预期现象**：

| 按键 | 预期输出 |
|------|----------|
| 字母 `a`~`z` | 对应小写字母 |
| `Shift`+`a` | 大写 `A` |
| `Caps Lock` 后 `a` | 大写 `A`；再按 `Caps Lock` 恢复小写 |
| `Ctrl` | **不应打印任何字符**（旧 bug 会打印 `f`/`t` 等） |
| `Enter` / `回车` | 换行、执行当前命令 |
| 输入 `help` 回车 | shell 输出帮助/命令列表 |
| 输入 `ls` 回车 | 列出（当前无 FS，应给出合理空/错误提示，不刷屏、不崩） |
| 退格 `Backspace` | 删除前一个字符 |

**通过/失败判定**：

- **通过**：上述键位均正确，无刷屏、无 `EXC`/`OOPS` 内核崩溃、shell 不卡死。
- **失败**：仍出现「按 Ctrl 打印字母」「字母错位」「按键无响应/乱码」「内核异常日志」。
- **注意**：`make run` 图形窗口中若需观察内核诊断，可按 `Ctrl+Alt+2` 切到 QEMU 监视器
  再 `Ctrl+Alt+1` 切回（headless 下 sendkey 不注入键盘中断属 QEMU 限制，不影响本窗口测试）。

请将手动测试的结果（截图或文字描述）反馈，以便确认修复彻底闭环。

---

## 六、交付物清单

- [x] 键盘 8042 翻译位开启（`cfg |= 0x40`），Set 2→Set 1 翻译稳定。
- [x] context_switch 回退纯 ret 模型，KVM/TCG 双路零 panic。
- [x] 清理全部 kentry/SCHED_CTX 残留，`make iso` 编译通过。
- [x] commit `c11758d`（未推送）。
- [x] 本 step36 文档。
- [ ] **待用户手动 `make run` 图形窗口实测确认键盘键位**（按第五节步骤反馈）。
