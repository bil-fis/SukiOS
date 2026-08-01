# Step 31 — 修复内核 #UD 崩溃（idle0 复用 BSP 引导栈导致切回时执行非法指令）

## 一、崩溃现场
```
[sched] task 'fs-server' pid=6 exited (code=1)
SukiOS>
[EXC] vector 6 (Invalid Opcode) err=0x0
  RIP=0xffffc00000eb3db6 CS=0x8 RFLAGS=0x10803
  RSP=0xffffc0008002ced8 RAX=0xffff801a00120626 RBX=0x00007ffffffe50e8
  RBP=0x0000000000000220 ... ERR=0x0 INT=0x6
  CR3=0x0000000000102000
  backtrace (rbp=0x0000000000000220):
    #0  0xffffc00000eb3db6
[KERNEL OOPS] system halted.
```

## 二、根因分析

### 2.1 关键地址语义
- `KSTACK_AREA_BASE = 0xFFFFC00080000000`（`include/mm/kstack.h`）：内核栈区基址。
- 崩溃 `RIP=0xffffc00000eb3db6` 落在 `0xFFFFC000` 段，且低 32 位 `00EB3DB6` 与 BSP 引导栈区
  （`0xFFFFC00000800000` 附近，注释见 `include/mm/kstack.h:36`）吻合 —— 它是一个 **BSP 引导栈地址**，
  被当成了指令地址。
- `kernel_addr()`（`kernel/diagnostics.c:30`）上界为 `0xFFFFC00000000000`，而
  `0xFFFFC00000eb3db6 >= 0xFFFFC00000000000`，故 backtrace 判定其“非内核地址”，只打印 `#0` 即止。

### 2.2 真正根因：idle0 复用 BSP 引导栈
旧 `sched_init()`（`kernel/sched/sched.c`）把 BSP 的引导执行流封装为 `idle0`，但：
```c
t0->kstack_base = 0;                          /* 标记“无独立栈” */
t0->kstack_top  = (uint64_t)&kernel_stack_top;/* 复用 BSP 引导栈 */
/* 未设置 t0->rsp */
```
即 **idle0 没有独立内核栈，直接复用 BSP 引导栈，且初始 `rsp` 未初始化**。

`context_switch(old_rsp_save, new_rsp)`（`kernel/sched/switch.S`）的语义：
- 在旧任务栈上 `movq %rsp,(%rdi)` 把**当前 BSP 栈指针**存入 `old_rsp_save`（即 `&idle0->rsp`）；
- `ret` 从 `new_rsp`（新任务栈）恢复返回地址。

时序：
1. BSP 在 kmain 引导栈上运行；`idle0` 设为 `current_task`，但 `idle0->rsp` 未初始化。
2. 第一次 `schedule()` 从引导栈切走 `idle0` 时，`idle0->rsp` 被写入**当前 BSP 引导栈指针**
   （如 `0xFFFFC00000eb3xxx`）。
3. kmain 后续继续在**同一 BSP 引导栈**上执行（创建用户服务、`for(;;)hlt` 空闲循环等），
   调用栈向低地址增长，**覆盖了 `idle0` 停泊点的返回地址**。
4. 用户任务（fs-server）退出后 CPU 切回 `idle0`，`context_switch` 从 `idle0->rsp`（BSP 栈）
   弹出已被覆盖的 `0xFFFFC00000eb3db6`，CPU 把它当指令解码 → **#UD (vector 6)** → system halted。

`fs-server exited (code=1)` 是独立事件：用户态自检/某操作触发异常被内核 `task_exit_current(1)`
杀掉；随后内核调度切回 idle0 时撞上这条被覆盖的坏返回地址。两件事共同暴露了 idle 栈设计缺陷。

## 三、修复（对齐 AP 的 `ap_main` idle 循环，彻底解耦 BSP 引导栈）

### 3.1 `kernel/sched/sched.c`
1. 新增 `bsp_idle(void)`：`interrupts_enable(); for(;;){ in_idle=1; hlt; in_idle=0; schedule(); }`
   —— 与 AP 的 `ap_main` idle 循环语义一致。
2. 改写 `sched_init()`：
   - `idle0` 改用 `kstack_alloc()` 分配**独立内核栈**（带守卫页），`kstack_base/top` 指向它；
   - 构造初始栈帧（与 `task_alloc_kernel` 同布局）：`ret→bsp_idle; r15=0; r14=0; r12=bsp_idle; r13=0; rbp=0; rbx=0`，
     令首次 `context_switch` 落到 `bsp_idle`；
   - 设置 `g_syscall_kstack[0]` 与 `g_scratch[0]`（原先仅在 AP 路径设置，BSP 漏设）。
3. 新增 `sched_switch_to_idle0(void)`：
   - `interrupts_disable()`（防切换间隙 100Hz tick 把 BSP 栈写回 `idle0->rsp`）；
   - **强制重置 `idle0->rsp` 为独立栈帧**（抵消初始化阶段 tick 对其 rsp 的潜在覆盖）；
   - `uint64_t krsp; context_switch(&krsp, idle0->rsp);` —— 注意第一个参数是局部 `krsp` 而非
     `&idle0->rsp`，故 `idle0->rsp` 不被改写，始终指向独立栈；一切换走后 BSP 引导流冻结在 `krsp`，
     `idle0` 独立栈上跑 `bsp_idle`。函数一去不返。

### 3.2 `kernel/kmain.c`
- 删除末尾的 `for(;;){ hlt; }`（它驻留在 BSP 引导栈，正是被覆盖的源头），
  改为调用 `sched_switch_to_idle0()` 将 BSP 执行流切到 `idle0` 独立栈。
- 顶部加 `extern void sched_switch_to_idle0(void);`。

### 3.3 为何 AP 不受影响
AP 的 idle 由 `ap_main` 自身循环担任（`kernel/arch/x86_64/smp.c:114`），
AP 的 `sched_create_idle` 产生的 idle 任务 `rsp` 虽未初始化，但 AP **从不通过 `context_switch` 切回该 idle 任务**
（当前流即 ap_main 的 hlt 循环），故无此缺陷。BSP 是唯一“引导流即 idle 流”且需被调度器切回的路径。

## 四、验证
- `make iso` 编译干净（无 warning/error）。
- 静态确认：`idle0` 现在拥有独立 `kstack_base`（非 0），`rsp` 指向独立栈帧，`context_switch` 切回时
  从独立栈弹出 `bsp_idle` 地址（`0xffff8000...` 合法内核代码），不再引用 BSP 引导栈返回地址。
- 防御加固建议（已符合既有铁律）：`kernel_addr()` 上界 `0xFFFFC00000000000` 恰好使本类坏地址被
  backtrace 判为非法而提前停栈；后续若复现类似切飞，可在 `context_switch` 的 `ret` 前对恢复地址做
  `kernel_addr()`/`text 段范围`校验并 panic 带 `next` 信息，避免静默 #UD。

## 五、提交
- 本次提交：`kernel/sched/sched.c`（idle0 独立栈 + bsp_idle + sched_switch_to_idle0）、
  `kernel/kmain.c`（切到 idle0 独立栈）。
- 按项目规则：**自动 `git commit`，不 `git push`**。
