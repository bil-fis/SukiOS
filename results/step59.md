# Step 59 — 信号子系统回归修复：FS_SERVER #GP 崩溃根因与修复

## 1. 背景与问题

在信号（signal/sigaction）功能实现后，系统从「上次稳定提交 `335a939`（零 panic）」
回归为：**FS_SERVER 启动即 #GP 崩溃**，QEMU 日志显示内核态 `iretq` 处
`#GP err=0x200`（非法段选择子 0x200 出现在 iret 帧的 CS/SS 槽）。

按项目铁律「默认新写代码全部错误、对照上次稳定提交定位」，本次对全部新增/修改代码
与 `335a939` 逐项比对，定位出 **3 个致命错误 + 1 个机制冲突**，全部修复后系统回到
「生产场景零 panic」，且信号功能端到端可用。

验证结论（QEMU 无头 60s，串口落盘）：
```
[libc-test] PASS=8 FAIL=0  ALL OK
[POSIX test summary: PASS=144 FAIL=0]
--- signal (sigaction/kill/raise/sigprocmask) ---
  [PASS] signal+raise handler invoked
  [PASS] signal signo matches
  [PASS] sigaction+kill handler invoked
  [PASS] kill signo matches
  [PASS] blocked signal not delivered
  [PASS] signal delivered after unblock
  [PASS] SIG_IGN ignored
```

## 2. 根因分析（对照稳定提交）

### Bug A（致命，导致 #GP）：汇编返回路径多弹 8 字节

`kernel/arch/x86_64/syscall_entry.S` 的 `syscall_return_path` 中，我原本插入了：

```asm
    mov   %rax, %rdi
    mov   %rsp, %rsi
    call  sig_deliver_check
    addq  $8, %rsp          /* 自以为清除 call 的栈参数 */
    movq  %gs:80, %rax
    movq  %rax, (%rsp)
    popq  %r9
```

**错误**：`call`/`ret` 已经平衡了栈（call 压入返回地址、ret 弹出），这里再 `addq $8,%rsp`
等于把栈指针又下移 8 字节。后果：后续 `popq %r9 … popq %rbx` 全部**错位 8 字节**，
iret 帧被从错位地址读取，CS/SS 槽变成垃圾 → `iretq #GP err=0x200`。

通过二分验证：单独回退 `syscall_entry.S` 到 `335a939`，FS_SERVER 立即停止崩溃，
确认崩溃 100% 由该汇编块引入。

### Bug B（致命，handler 永远跑不起来）：投递只改 f[FR_IP] 未改 g_scratch

`syscall_return_path`（同一文件 172–174 行）在末尾会用
`g_scratch[cpu]->scr_rip/scr_rsp` **覆盖** iret 帧的 RIP/RSP。因此
`sig_deliver_check` 即便改了 `f[FR_IP/FR_SP]`，也会被覆盖回原 syscall 返回地址，
用户态 handler 永远不会被进入。

### Bug C（正确性问题）：FR_RDX 常量算错

`kernel/syscall/signal.c` 原 `#define FR_RDX (2*8)`，但按汇编 push 顺序
（`r9,r8,r10,rdx,rsi,rdi,…`），rdx 是第 4 个 GPR 槽，应为 `3*8`。`2*8` 指向的是
`r10` 槽，导致投递时 `&ucontext` 被写进 r10 而非 rdx（对 `sa_sigaction` 三参 handler
语义错误）。

### 附加隐患：rax 返回值被覆盖

原汇编块 `movq %gs:80,%rax` 把系统调用返回值 rax 覆盖成用户 r9，会导致所有 syscall
返回错误值（虽非 #GP 直接原因，但属破坏返回契约）。

### 机制冲突：kill 走旧 pending_kill 会误杀任务

`kernel/sched/sched.c::task_signal`（sys_kill/sys_tkill 入口）原本只置
`pending_kill/pending_signo`，由 `syscall_dispatch` 入口检查后 `task_exit_current`
直接终止目标——即旧实现**没有 handler 投递**。问题在于：
- `kill(getpid(), sig)` 的「发给自己」分支会 `task_exit_current(128+signo)` **立即杀掉
  自身任务**，使 posixtest 的 T2 `sigaction+kill handler invoked` 根本无法验证 handler；
- 旧 `pending_kill` 路径与新的 `pending`（bitmask）框架并存，语义割裂。

## 3. 修复方案

核心决策：**把信号投递从汇编返回路径移出，改在 C 的 `posix_dispatch()` 返回前做**。
这样天然规避 rax 返回值破坏与栈对齐/平衡问题，且覆盖全部 POSIX 系统调用号。

### 3.1 回退汇编危险块（Bug A / rax 覆盖）

`kernel/arch/x86_64/syscall_entry.S` 的 `syscall_return_path` 恢复为稳定提交原貌：

```asm
    movq  8(%r11), %rcx         /* rcx = scr_rsp */
    movq  %rcx, 120(%rsp)       /* 帧 RSP 槽 = scr_rsp */

    /* 信号投递改在 posix_dispatch 返回前（C 路径）做，避免破坏 rax 与栈平衡 */
    popq  %r9
```

### 3.2 sig_deliver_check 重写（Bug B / Bug C / 去调试）

文件 `kernel/syscall/signal.c`：

- 去掉空函数 `#if 0` 包体与 `kprintf` 调试输出；
- 在投递真实 handler 时，**同步更新 `g_scratch[cpu]`**（关键修复）：

```c
    /* 关键：syscall_return_path 会用 g_scratch 覆盖 iret 帧 RIP/RSP */
    uint64_t *scr = g_scratch[cpu_index()];   /* scr[0]=scr_rip, scr[1]=scr_rsp */
    scr[0] = (uint64_t)hdl;                   /* 覆盖后 iret→handler */
    scr[1] = new_rsp;                         /* 覆盖后 iret→新用户栈 */
```
- 修正 GPR 帧常量表，补全全部槽并修正 `FR_RDX`：

```c
#define FR_R9    (0 * 8)
#define FR_R8    (1 * 8)
#define FR_R10   (2 * 8)
#define FR_RDX   (3 * 8)   /* 修正：rdx 位于第 4 个 GPR 槽 */
#define FR_RSI   (4 * 8)
#define FR_RDI   (5 * 8)
#define FR_R15   (6 * 8)
#define FR_R14   (7 * 8)
#define FR_R13   (8 * 8)
#define FR_R12   (9 * 8)
#define FR_RBP   (10 * 8)
#define FR_RBX   (11 * 8)
#define FR_IP    (12 * 8)
#define FR_CS    (13 * 8)
#define FR_FL    (14 * 8)
#define FR_SP    (15 * 8)
#define FR_SS    (16 * 8)
```
- 增加 `in_signal` 守卫：已在 handler 中时不重入投递，待 `sigreturn` 返回后再检查。

### 3.3 在 posix_dispatch 返回前挂接投递

`kernel/syscall/sys_posix.c`：

```c
    /* 信号投递：在返回用户态前检查 pending 信号并注入 handler。
     * 直接改写 g_syscall_gpr[cpu] 帧与 g_scratch[cpu]（scr_rip/scr_rsp），
     * syscall_return_path 末尾的 scr 覆盖逻辑据此写 iret 帧，handler 得以进入。 */
    *ret = (int64_t)sig_deliver_check((uint64_t)r,
                                      (uint64_t)g_syscall_gpr[cpu_index()]);
    return true;
```
并在文件顶部信号 externs 处补声明：
```c
extern uint64_t sig_deliver_check(uint64_t rax, uint64_t frame);
```

### 3.4 统一 kill/tkill 到新信号框架（机制冲突）

`include/kernel/task.h` 声明 `task_signal_send`；`kernel/sched/sched.c::task_signal`
改为调用 `task_signal_send(t, sig)`（置 `pending` 位，由 `sig_deliver_check` 在
syscall 返回边界按 handler/默认终止/忽略处理），删除旧的 `pending_kill` 置位与
「发给自己立即 task_exit_current」逻辑。自此 `kill(getpid(), sig)` 也能正确投递给
已安装的 handler。

> 注：`syscall_dispatch` 内的 `pending_kill` 检查（task.h 中 `pending_kill/pending_signo`
> 字段仍存在）已变为死代码（不再有写入方），本轮保留不删以免扩大 diff 风险，不影响行为。

## 4. 关键数据结构与常量

### 4.1 内核帧布局（g_syscall_gpr[cpu] 指向 GPR 槽底 = rsp）

```
偏移     内容
0x00     r9   (FR_R9)
0x08     r8   (FR_R8)
0x10     r10  (FR_R10)
0x18     rdx  (FR_RDX)        <-- 修正点
0x20     rsi  (FR_RSI)
0x28     rdi  (FR_RDI)
0x30     r15  (FR_R15)
0x38     r14  (FR_R14)
0x40     r13  (FR_R13)
0x48     r12  (FR_R12)
0x50     rbp  (FR_RBP)
0x58     rbx  (FR_RBX)
0x60     RIP  (FR_IP)         iret 帧
0x68     CS   (FR_CS)   = 0x1B (SIG_USER_CS)
0x70     RFLAGS(FR_FL)  = 0x202 (SIG_USER_RFL, IF=1)
0x78     RSP  (FR_SP)         iret 帧
0x80     SS   (FR_SS)   = 0x23 (SIG_USER_DS)
```
此布局严格对应 `syscall_entry.S` 的 push 顺序。

### 4.2 g_scratch[cpu] 布局（scr_rip/scr_rsp 指针）

```c
uint64_t *g_scratch[MAX_CPUS];   /* 指向当前任务 scr_rip(scr[0]) / scr_rsp(scr[1]) */
```
`syscall_return_path` 末尾：`f[FR_IP]=scr[0], f[FR_SP]=scr[1]`。

### 4.3 信号帧（用户栈，向下 16 字节对齐生长）

```
new_rsp+0x00  restorer  (8B, = __sighandler_trampoline)   ← 返回地址槽
new_rsp+0x08  struct suki_siginfo  (48B)
new_rsp+0x38  struct suki_ucontext (sizeof, FPU 512B 等)
```
handler 经 `ret` 跳到 restorer，此时 `rsp = new_rsp+8`，蹦床：
```asm
lea 48(%rsp), %rdi     /* rdi = &ucontext = new_rsp+8+48 = new_rsp+0x38 */
mov $SYS_SIGRETURN, %rax
syscall
```
与内核写入位置 `new_rsp+8+sizeof(struct suki_siginfo)` 精确对应。

### 4.4 用户态信号 API（已实现并验证）

`user/lib/signal.c`：
- `sigaction()` 始终置 `sa_restorer = __sighandler_trampoline` + `SA_RESTORER`；
- `signal()` = sigaction + SA_RESTORER 简化版；
- `raise()/kill()/sigprocmask()/sigreturn()` 走 `SYS_RAISE/SYS_KILL/SYS_SIGPROCMASK/SYS_SIGRETURN`；
- `__sighandler_trampoline`（naked）取 `&ucontext` 后 `SYS_SIGRETURN` 还原上下文。

## 5. 验证方式（仅用 bash + QEMU 自带机制，符合项目约束）

```bash
cd /mnt/d/Projects/SukiOS
make iso                                   # 编译内核并产出 build/SukiOS.iso
make run-headless QEMU_SERIAL="-serial file:/tmp/qemu.serial.log" RUN_TIMEOUT=60
grep -iE "signal|kill|raise|SIG_IGN" /tmp/qemu.serial.log   # 7 项信号断言 PASS
grep -iE "KERNEL OOPS|panic|triple fault|#GP|#PF" /tmp/qemu.serial.log   # 零命中
grep -iE "POSIX test summary" /tmp/qemu.serial.log           # PASS=144 FAIL=0
```

- 信号 7 子项全 PASS：`signal+raise`/`kill` handler invoked、signo matches、
  blocked not delivered、delivered after unblock、SIG_IGN ignored；
- FS_SERVER 正常 `starting`，无 OOPS/GP/PF/triple fault；
- 回归零 panic：POSIX 测试 144 项全过，libc-test 8 项全过。

## 6. 关于 newlib 移植的评估

用户提示「如难以修复可借鉴/移植 newlib」。经排查，本次崩溃**纯属内核返回路径 bug**
（Bug A 栈错位 + Bug B 未更新 g_scratch + Bug C 常量错误），与用户态 C 库无关。
用户态信号 API 已由 `user/lib/signal.c` 以标准 Linux 风格（sa_restorer + trampoline +
SYS_SIGRETURN）完整提供，POSIX 语义验证通过，**无需引入 newlib**。

结论：newlib 移植**非本次必需**；可作为未来「用户态 C 库增强（printf/floating point/
完整 POSIX）」的可选项记录，但本次信号功能用自研 libc 已端到端可用。若后续需要
完整 libc 符号/数学库/stdio 时再评估 newlib 移植（zipres 下已有 newlib 源码包备用）。

## 7. 改动文件清单

- `kernel/arch/x86_64/syscall_entry.S` — 回退危险 asm 信号调用块（修复 Bug A/rax）
- `kernel/syscall/signal.c` — 重写 sig_deliver_check（修复 Bug B g_scratch、Bug C FR_RDX、去调试）
- `kernel/syscall/sys_posix.c` — posix_dispatch 返回前挂 sig_deliver_check
- `kernel/sched/sched.c` — task_signal 统一到 task_signal_send 新框架（修复 kill 误杀）
- `include/kernel/task.h` — 声明 task_signal_send，更新注释
- （前序已实现、本轮一并纳入验证）`kernel/syscall/signal.c`、`user/lib/signal.c`、
  `include/sukios/posix.h`、`user/lib/libc.h`、`user/posixtest.c` 信号相关实现
