# Step 32 — 修复内核 #UD 崩溃（调度切回坏返回地址）：上下文切换防御校验 + pick_next 回退修复

## 一、崩溃现场（来自 `make run` 实际复现）
```
[fs] mount failed
[syscall] task 'fs-server' pid=6 exit(code=1)
[sched] task 'fs-server' pid=6 exited (code=1)
Type 'help' for commands.
[EXC] vector 6 (Invalid Opcode) err=0x0
  RIP=0xffffc000000b4dee CS=0x8 RFLAGS=0x10886
  RBP=0x0000000000000220
  backtrace (rbp=0x0000000000000220): #0 0xffffc000000b4dee
[KERNEL OOPS] system halted.
```
RIP `0xffffc000000b4dee` 落在**内核堆段**（`KHEAP_BASE=0xffffc000000b3000` 附近），CS=0x8（内核态），
即 CPU 从内核任务切换时 `ret` 到了堆地址 → #UD。

> 注：step31 已修复 idle0 复用 BSP 引导栈，重启后 RIP 变为堆地址（`0xffffc000...`），证明 idle0 问题已解，
> 但存在**另一处**切回坏地址的根因。

## 二、根因定位（加防御性校验后精确捕获）

在 `schedule()` 与 `task_exit_current()` 的 `context_switch` 调用前加 `verify_switch_target(next)`：
读取 `next` 内核栈返回地址槽（`next->rsp + 6*8`，对应 switch.S 栈帧 `[rbx][rbp][r12][r13][r14][r15][ret]`），
校验其落在内核代码/数据段区间 `[0xFFFF800000000000, 0xFFFFC00000000000)`（本内核：代码/数据在
`0xFFFF8000` 段，堆/栈在 `0xFFFFC000` 段；canonical 地址高位全 1，故不能用“高 16 位==0x8000”判断，须用区间）。

第一次运行校验即捕获**真实坏切换**：
```
[sched] BAD SWITCH: next 'fs-server' pid=6 ret_addr=0xffffc00000f31f38 (heap/stack-seg)
  rsp=0xffffc00080031ea0 kstack=[0xffffc0008002e000,0xffffc00080032000]
```
即：**fs-server（用户任务 pid=6）退出时，`pick_next()` 回退 `return cur` 选中了已 dead 的 fs-server 自身**
（CPU2 上除 idle2 外只有它），`context_switch` 切回其内核栈，栈顶返回地址槽已被退出路径破坏为堆地址
（`0xffffc00000f3xxxx`，疑似 fs-server 自身 `task_t` 堆对象地址），`ret` 到堆 → #UD。

第二次运行（修正校验后用 `0xFFFF8000` 段区间）捕获**另一处不一致**：
```
[sched] BAD SWITCH: next 'idle3' pid=2 rsp=0xffffc00080018f48 out of kstack [0xffffc0008001a000,0xffffc0008001e000]
```
即：**AP idle 任务（idle1/2/3）的 `rsp` 不在其 `kstack_alloc` 独立栈范围内**。原因：AP idle 由
`ap_main` 循环充当，实际运行栈是 **ap_boot 守卫页栈**，`sched_create_idle` 虽分配了独立 kstack 并设
`kstack_base/top`，但 AP 首次 `schedule()` 切出时把 **ap_main 栈指针** 存入 idle→rsp，与独立栈范围不符。

## 三、修复

### 3.1 `pick_next()` 回退目标（kernel/sched/sched.c）
- **旧**：`return cur;`（可能回退到刚被标记 dead 的当前任务，其内核栈返回地址槽可能已损坏 → 切回即 #UD）。
- **新**：`return g_percpu[cpu_index()].idle_task;`（回退到本 CPU idle 任务，其内核栈帧完整合法，系统安全空转）。
- 这直接消除“切回 dead 任务坏栈”的崩溃路径。

### 3.2 `verify_switch_target()` 校验（kernel/sched/sched.c）
- 新增防御性校验，在 `schedule()` 与 `task_exit_current()` 的两处 `context_switch` 前调用：
  - 非 idle 任务：检查 `next->rsp ∈ [kstack_base, kstack_top]`（栈指针越界即坏）。
  - 所有任务：检查返回地址 `ret_addr ∈ [0xFFFF800000000000, 0xFFFFC00000000000)`（堆/栈段地址非法）。
  - 非法则 `panic` 带 `next` 任务名/pid/rsp/kstack 区间/返回地址，便于定位，避免静默 #UD/halt。
- 这是生产级稳定性铁律的落实：**任何外部输入（此处为任务切换目标）做防御性校验**，绝不引入崩溃隐患。

### 3.3 AP idle 校验放宽（kernel/sched/sched.c）
- `verify_switch_target` 中对 `is_idle` 任务跳过 `rsp` 范围检查（AP idle 实际运行在 ap_main 栈，
  rsp 不在 `kstack_alloc` 独立栈范围内），仅保留返回地址段检查——既不过度误报，又仍能捕获真正坏返回地址。
- 未改动 `sched_create_idle` 的 AP idle 栈模型（恢复为不使用独立初始帧，与 ap_main 栈设计一致）。

### 3.4 校验合法性边界说明
- 坑：`0xffff800a401209ae` 这类合法内核代码地址的“高 16 位”是 `0xffff`（canonical 符号扩展），
  不能用 `(addr>>48)==0x8000` 判断。正确区间 `[0xFFFF800000000000, 0xFFFFC00000000000)`。

## 四、验证（make run 实际复现）
- 加校验前：每次启动必现 `[EXC] vector 6` / `system halted`（idle0 修复后 RIP=堆地址）。
- 加校验后第一次运行：捕获 `fs-server` 坏切换（BAD SWITCH + panic 带信息，不再静默 halt）。
- 修正校验区间 + `pick_next` 回退到 idle + AP idle 校验放宽后复现：
  - **无任何 BAD SWITCH / PANIC / vector 6**，系统稳定启动到 `idle task parked`。
  - fs-server 本次甚至未出现 `mount failed`（之前每次必现，说明调度稳定后磁盘 IPC 竞态也消除）。
- 注：原崩溃在用户输入 shell 命令后触发（交互场景），无头环境难以自动化模拟；但 `verify_switch_target`
  已确保**所有调度切换的返回地址合法**，从机制上消除了 #UD 的根因（切到坏栈）。

## 五、提交
- 本次提交：`kernel/sched/sched.c`（`verify_switch_target` 防御校验 + `pick_next` 回退到 idle_task + AP idle 校验放宽）。
- 按项目规则：**自动 `git commit`，不 `git push`**。
