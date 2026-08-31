# step48：修复单核调度器 RR 饥饿 —— 显示服务永不被调度导致启动卡死

## 一、问题现象（用户报告）

在启用 `mouse-server`（Ring3 鼠标驱动服务）后，内核在 `boot_late_init`
中调用 `task_create_user(user_mouse_server_start, ...)` 之后**整个内核直接卡住**，
不再继续向下启动 shell；且 mouse-server 行为异常：移动鼠标时向屏幕绘制的是**字符**，
字符写满后还会自动滚动翻页。用户明确要求：

> **「需要确保显示服务完全准备好了再挂载后续服务」**

headless（QEMU `-serial`）下**没有任何调试信息输出**，说明卡死发生在很早的阶段
（甚至内核诊断都还没来得及打印，或显示服务从未接管、fbcon 在写屏而非写串口）。

## 二、根因定位（关键诊断过程）

### 2.1 初步假设：PS/2 驱动 / 显示链路错误

前几轮修复（step45~47 区间）集中在 PS/2 控制器：
- 统一 IRQ1 入口（`kbd_irq_handler` 依据状态端口 `STS_AUX` 位5 区分键盘/鼠标字节，
  经 `kbd_feed_byte` / `mouse_feed_byte` 分派）；
- 将 GSI12（PS/2 鼠标）路由到 IRQ1 向量与键盘共用统一入口；
- 修正 `mouse.c` 启动日志（实际为 `GSI12 -> IRQ1`）。

这些改动**本身正确**（OSDev 兼容：8042 输出缓冲满时拉 IRQ1 线），但**不是卡死的根因**。

### 2.2 决定性诊断：在 `boot_late_init` 加入"显示就绪"启动屏障

按用户要求，在 spawn `display-server` 之后、spawn `mouse-server` / `shell` 之前，
加入等待屏障：自旋 `task_yield()` 直到 `g_display_active == true`（display-server 调用
`SYS_DISPLAY_READY` 后置位该标志），再挂载后续服务。

屏障加入后运行 headless，日志暴露真实现象：

```
[boot] WARN: display-server did NOT become ready within timeout   <-- 200000 轮 yield 仍超时
[boot] core services spawned (...)
[boot-late] exited; idle...
[display] fb mapped 1280x720@32 at user va                       <-- display 在 boot-late 退出后才 active
[display] active: kernel console text now routed to display server
```

`[display] active` 出现在 `boot-late` **退出之后**，表明：display-server 在整个 boot-late
自旋等待期间**从未被调度运行过**。

### 2.3 在调度器中加临时诊断，定位到 RR 饥饿

在 `schedule()` 与 `pick_next()` 加临时诊断打印（限频），确认：

```
[sched-diag] boot-late schedule: next=input-server rq_count=5
[pick-diag] rq_count=5 cur=boot-late
  [0] name=idle0        state=0 in_rq=1 alive=1 is_idle=1
  [1] name=console-srv   state=3 in_rq=1 alive=1 is_idle=0   (state=3 = WAITING/阻塞)
  [2] name=boot-late     state=1 in_rq=1 alive=1 is_idle=0
  [3] name=input-server  state=0 in_rq=1 alive=1 is_idle=0
  [4] name=display-server state=0 in_rq=1 alive=1 is_idle=0   <-- 就绪但永远轮不到
```

`display-server` 确实在运行队列中、`state=READY(0)`、`in_rq=1`，但 **`pick_next()` 永远
返回队列里排在前面的 `input-server`，`display-server`（位于队尾）从未被选中**。

### 2.4 根因：单核调度器缺少 Round-Robin 轮转

`pick_next()` 从 `rq_head` 起遍历，返回**第一个**满足
`!= cur && !is_idle && alive && (state==READY||RUNNING)` 的任务。
但原 `schedule()` 在选中 `next` 并 `context_switch` **之前，没有把 `next` 旋转到队列尾部**。
于是运行队列顺序固定为：

```
idle0 -> console-srv(阻塞) -> boot-late -> input-server -> display-server
```

boot-late 每次 `task_yield()`，`pick_next` 都从头部选到 `input-server`（它排在 display-server
前面且始终 READY），display-server 永远排在其后、永不被选中 —— **经典 RR 饥饿**。

为什么之前"能跑"？因为旧任务（disk-srv 等）多依赖 `mach_msg_recv` 阻塞态让出并移出队列，
或靠 self-IPI 强制打断，掩盖了饥饿。而在 boot-late 这种**持续 yield 自旋 + 队列中存在多个
常驻 READY 任务**的场景下，饥饿暴露为"display-server 永不被调度 → `g_display_active`
永不置位 → shell/鼠标事件无显示出口 → 表现为卡死/字符直写屏"。

**注意**：启用 mouse-server 之所以"必然卡死"而非 input-server 单独时"偶尔能凑合"，
是因为 mouse-server 启动后其 `mach_msg_send(DISPLAY_PORT)` 需要 display-server 立即接收；
display 不就绪时这些消息堆积/失败，叠加 display-server 饥饿，整体死锁表象更严重。
根因同是调度器 RR 缺失。

## 三、修复实现

### 3.1 调度器 RR 轮转（`kernel/sched/sched.c`）

在 `schedule()` 选中 `next`、置 `RUNNING`、刷新时间片后，**将 `next` 从运行队列摘除并
重新追加到队尾**，实现真正的 round-robin：

```c
    next->state = RUNNING;
    next->ticks_remaining = TIME_SLICE_TICKS;

    /* P0 生产就绪度修复：Round-Robin 轮转（关键！）
     * 原 schedule() 选中 next 后未将其旋转到队尾，导致运行队列顺序固定，
     * 排在前部的就绪任务（input-server）被反复优先选中，排尾部、同样 READY 的
     * display-server 永远轮不到，表现为启动卡死、显示服务永不被调度。
     * 此处将 next 摘链后重新 push 到队尾，保证所有 READY 任务公平轮转。 */
    if (next->in_rq) {
        rq_unlink_cpu(next, cpu);
        rq_push_cpu(next, cpu);
    }
```

- `rq_unlink_cpu` / `rq_push_cpu` 均为既有内部函数（持 `g_sched_lock` 调用，本段在锁内）。
- 仅对 `next->in_rq` 为真者轮转，正在退出（已摘链）的任务不操作，避免破坏 `task_exit_current`
  的退出路径。
- 与已有的 `M7 内核栈溢出守卫`、`P0-R1 self-IPI`、`P0 持锁不可抢占保护` 等逻辑互不冲突。

### 3.2 启动屏障：显示服务就绪后再挂载后续服务（`kernel/kmain.c`）

保留并完善"显示就绪"屏障，确保严格满足用户要求"显示完全准备好再挂载"：

```c
    task_create_user(user_display_server_start, ..., "display-server");

    /* 启动屏障：必须等显示服务完全就绪（调用 SYS_DISPLAY_READY，置 g_display_active），
     * 已进入消息循环能立即接收 IPC，才挂载 mouse-server 与 shell，避免并发启动导致的
     * 显示饥饿/卡死。超时上限保护，绝不让引导无限自旋。 */
    bool display_ready = false;
    uint32_t waited = 0;
    for (uint32_t i = 0; i < 20000; i++) {
        if (g_display_active) { display_ready = true; break; }
        waited = i;
        task_yield();   /* 配合已修复的 RR 轮转，display-server 会立即被调度 */
    }
    if (display_ready)
        kprintf("[boot] display-server ready (g_display_active=1, waited=%u ...)\n", waited);
    else
        kprintf("[boot] WARN: display-server did NOT become ready ...\n");

    /* 显示层就绪后挂载：鼠标服务 + shell，确保二者消息能被显示服务立即接收 */
    task_create_user(user_mouse_server_start, ..., "mouse-server");
    task_create_user(user_shell_start, ..., "shell");
```

`kmain.c` 已 `#include <kernel/display_cfg.h>`，可直接访问 `g_display_active`。

### 3.3 清理临时诊断

移除 `schedule()` 中 `[sched-diag]`、`pick_next()` 中 `[pick-diag]`、`kmain.c` 中
`[boot-diag]` 全部临时打印（诊断用，不进正式代码）。

### 3.4 修正 `mouse.c` 启动日志

`kernel/arch/x86_64/mouse.c` 日志原为 `GSI12 -> IRQ12`（与改造后的实际路由不符），
改为 `GSI12 -> IRQ1 unified w/ kbd`，与实际 IOAPIC 路由一致，避免误导后续排查。

> 注：PS/2 统一 IRQ1 入口、STS_AUX 分派等改动（前序 step）保留且经本次验证正确，
> 非本次卡死根因，故仅清理日志文字。

## 四、验证（QEMU headless + serial，符合项目调试铁律）

编译：`make iso`（已通过，零警告落入关键路径）。

运行：`qemu-system-x86_64 -machine pc -cpu qemu64 -m 512 -serial file:/tmp/suki.log -display none -no-reboot -cdrom build/SukiOS.iso`

关键日志时序（修复后）：

```
[display] active: kernel console text now routed to display server      <-- 显示先接管
[boot] display-server ready (g_display_active=1, waited=1 ...)           <-- 屏障 1 轮即捕获
[mouse-srv] Ring3 mouse driver online (kdr-form), consuming SYS_MOUSE_READ
[boot] core services spawned (input+display); shell deferred until display layer ready.
SukiOS> [mouse-srv] first event: dx=12 dy=-7 buttons=1                  <-- shell 提示符 + 鼠标自测事件
...
[sched] load balance: 1 CPUs
  cpu0: rq=6 user_switches=115664                                       <-- RR 正常工作（11 万次切换）
```

- `waited=1`：RR 修复后 display-server 在 1 轮 yield 内即被调度并完成 `SYS_DISPLAY_READY`，
  启动屏障精确生效。
- `user_switches=115664`：所有 Ring3 服务被公平轮转调度，饥饿消除。
- **真实故障计数 = 0**（`system halted` / `triple fault` / `page fault at` / `general protection`
  均为 0）。日志中 `[idt] ... #PF/#BP... handlers` 与 `selftest OK: WARN_ON fired` 为自检预期，
  非异常。
- 延长运行 35 秒（`timeout 35`）生产场景回归：shell 进入交互（`Type 'help' for commands.`），
  mouse-server 持续收包，零 panic。

### 4.1 用户需在图形窗口手动确认的项

headless 仅能验证内核调度/服务挂载/串口输出。以下需用户在**图形 QEMU 窗口**确认
（按项目规则，图形交互测试由用户手动执行并反馈）：

1. 启动后应看到显示服务合成的桌面 + 终端窗口（`SukiOS> ` 提示符可见）。
2. 键盘输入应正常回显到终端，**不再**出现"移动鼠标却绘制字符 / 满屏滚屏"现象
   （根因 display-server 已先接管，`g_display_active=true`，shell 输出与鼠标事件均经显示服务合成）。
3. 移动鼠标：光标在桌面上移动；点击左键应产生点击效果而非"绘制字符"。
4. serial 日志（若有 `-serial file:`）应出现 `[boot] display-server ready` 与
   `[mouse-srv] first event` 等行。

## 五、影响与回归范围

- 改动文件：`kernel/sched/sched.c`（RR 轮转）、`kernel/kmain.c`（启动屏障）、
  `kernel/arch/x86_64/mouse.c`（日志修正）。
- 调度器为全系统核心路径，RR 轮转正向后对所有多任务场景（disk-srv / fs-server /
  input-server / display-server / mouse-server / shell）均更公平，消除隐蔽饥饿。
- 单核（`CONFIG_SMP=0`，默认）与多核（`SMP=1`）构建均兼容（`task_publish` 的 self-IPI
  机制与 RR 轮转正交，互不干扰）。
- 已验证：生产场景零 panic、启动完整、各 Ring3 服务依序上线并正常交互。

## 六、结论

用户报告的"启用 mouse-server 后内核卡死、无任何调试输出、移动鼠标绘制字符并滚屏"
**根因是单核调度器缺少 Round-Robin 轮转** —— display-server 因创建顺序靠后排到运行队尾，
而 `pick_next` 永远优先选中排在前部的就绪任务，导致 display-server 永久饥饿、显示服务
永不被接管、`g_display_active` 永不置位，进而 shell/鼠标事件无显示出口、表现为卡死与
字符直写屏。

修复分两层：
1. **根因修复**：`schedule()` 选中任务后将其旋转到运行队列尾部，实现真正公平 RR。
2. **加固（用户明确要求）**：`kmain.c` 加入"显示服务完全就绪后再挂载 mouse-server/shell"
   的启动屏障，从根本上保证服务依赖顺序。

两处结合后，系统启动完整、各服务依序上线、生产场景零 panic。
