# step98：USB 启动性能退化与 Shell 仅外框修复

## 1. 问题现象（用户报告）

使用 **USB 键盘/鼠标** 启动 SukiOS 时：

1. 系统执行速度明显降低；
2. 鼠标移动有延迟；
3. Shell 不渲染，**只有外框**（窗口边框可见，但内部文字/内容不出现）。

目标：定位并修复上述退化，恢复 USB 键鼠可用、Shell 完整渲染、系统流畅。

## 2. 根因分析

经 QEMU 带硬盘 + USB 键鼠复现（关键：必须挂 `-drive ... media=disk`，否则 FS_SERVER 不启动会误判为假象），确认问题由**两类独立缺陷叠加**造成，且都属“启动期资源被不合理占用/调度不及时”，与 USB 设备本身是否工作无直接因果：

### 2.1 根因 A：FS↔disk-srv 紧耦合 IPC 唤醒不及时（导致 Shell 只外框）

- Shell 启动后要 `sys_task_spawn("/BIN/FONTSRV.SKA")` 等，需经 **FS_SERVER → DISK_PORT（内核 disk-srv）** 的 IPC 往返读取大量小文件。
- 唤醒协议：`enqueue` → `sched_wake` → 发 `RESCHED` IPI。原来同核分支用 `lapic_send_ipi(本核 apic_id, ...)` 显式投 self，在 QEMU/部分 LAPIC 实现下**不投递**（上一轮已修：改用 `lapic_send_ipi_self` shorthand self，验证 smp-dbg 计数从 0→1）。
- 即便 IPI 触发了 `schedule()`，原 RR 调度按运行队列顺序选任务：刚被唤醒的 disk-srv/FS_SERVER 若不在队首，要等其它就绪任务轮转完（每个时间片 ~10ms），导致相邻磁盘读 **gap 中位数仍达 ~20ms**（=10ms tick 整数倍），数千次单扇区 IPC 往返被放大到数十毫秒，等效挂死 → Shell 永远等不到内容 flush → “只外框”。

### 2.2 根因 B：USB 主机轮询任务 busy-wait 占用 80% CPU（导致整机变慢、鼠标延迟）

- USB 为**中断关闭的轮询模式**（`UHCI_USBINTR=0`）。内核任务 `SukiUsbHost` 每轮执行 `usb_poll()` 后用 `usb_ms(8)` 延时。
- `usb_ms(8)` 实为 `usb_us(8000)`：**纯 `inb(0x80)` 空转忙等 8ms**（见 `kernel/drivers/usb/usb_core.c`）。
- 该任务是普通 RR 任务，时间片 10ms：每轮 8ms 纯自旋 + 2ms 有效工作，单核下**白占约 80% CPU**，导致 FS_SERVER/disk-srv/shell/display 等拿不到 CPU → 整机卡顿、鼠标事件注入后迟迟得不到处理（延迟感）。

### 2.3 附带发现的 QEMU 测试命令陷阱（非内核缺陷）

- 自验命令若同时写 `-device piix3-usb-uhci -usb -device usb-kbd -device usb-mouse`，QEMU 把键鼠挂到默认 UHCI（00:1.2），而显式添加的空控制器被驱动先扫描初始化 → `uhci_init` 找到的是**没有设备的空控制器**，PORTSC 显示 CCS=0，键鼠不枚举。
- 正确自验命令：**只用 `-usb -device usb-kbd -device usb-mouse`**（默认 pc 已含 UHCI 于 00:1.2，键鼠即挂其上）。这仅影响 QEMU 测试；真实硬件单 UHCI 不受影响（用户已确认键鼠可用、仅延迟）。

## 3. 修复内容

### 3.1 唤醒抢占加速：`rq_move_head_cpu` 接入 `sched_wake`（根因 A 彻底解决）

- 新增 `kernel/sched/sched.c` 静态函数 `rq_move_head_cpu(task_t *t, uint32_t cpu)`：将任务从运行队列摘除并**重新插入队首**（已在队列则先摘链）。
- 在 `sched_wake()` 同核/异核唤醒分支统一调用：被唤醒任务一律提到队首，使 `lapic_send_ipi_self` 触发的 `schedule()` **立即选中它**，把紧耦合 IPC 回合延迟从数十毫秒降到微秒级。
- 不改动 `task_publish` 同核分支（仍走不投递的显式 self-IPI）：因 kmain 引导期处于 **BSP 引导栈（非可调度任务上下文）**，若 self-IPI 真正投递会在 boot 期把 kmain 中断劫持、打断服务创建；该分支“不投递”恰是安全的，且服务创建完成后由 100Hz tick / idle0 hlt→schedule 兜底调度，无需改动。

### 3.2 内核 `msleep` + 消除 USB 忙等（根因 B 彻底解决）

- `include/kernel/task.h`：`task_t` 新增字段 `uint64_t wake_jiffies;`；新增声明 `void msleep(uint32_t ms);`。
- `kernel/sched/sched.c`：
  - 新增静态全局 `static uint64_t g_jiffies = 0;`（100Hz 节拍，BSP 每 tick +1）。
  - 新增 `msleep(uint32_t ms)`：计算截止节拍 → 置 `BLOCKED` + 登记 `wake_jiffies` → `schedule()` 让出；睡眠期间 CPU 交还其它任务。
  - `sched_tick()`（仅 cpu0）推进 `g_jiffies`，并扫描 `g_all_tasks` 中 `wake_jiffies` 到期且 `BLOCKED` 的任务，**持锁仅收集指针、释放锁后再 `sched_wake`**（避免自旋锁非递归死锁）。
  - `sched_wake()` 唤醒时**清零 `wake_jiffies`**，防止被 msleep 再次唤醒。
- `kernel/drivers/usb/usb_core.c`：`usb_service_task` 的周期 `usb_ms(8)` 改为 `msleep(8)`（真正睡眠，不再占满时间片）。枚举期的 `usb_ms(20/5)` 属一次性、保留。

### 3.3 `uhci_poll` 对“已连接未枚举”根端口主动枚举（附带健壮性）

- `kernel/drivers/usb/uhci.c`：新增 `static bool g_root_enumed[8];`。
- 重写 `uhci_poll()`：除响应 `CSC`（连接变化）位外，对 **CCS 置位但尚未枚举** 的根端口主动 `uhci_root_port_reset` + `usb_core_root_connect`；断开（CCS 清）且已枚举则 `usb_core_root_disconnect`。修复“设备在内核轮询启动前就已连接（CSC 已被清）则永不被枚举”的缺陷，并支持可靠热插拔。

### 3.4 清理全部临时诊断打印

移除上一轮排障遗留的刷屏诊断（均为临时探测，非功能代码）：
- `kernel/arch/x86_64/smp.c`：`[smp-dbg]`（ipi_resched_handler 计数）。
- `kernel/drivers/ata.c`：`[disk-dbg]` READ gap/dt（及专用 `t0/s_last/t1` 计时变量）。
- `kernel/syscall/syscall.c`：`[spawn-dbg]`。
- `user/fs_server.c`：`[fs-dbg]` read_file 流程。
- `user/display_server.c`：`[display] FLUSH` 像素统计（保留合法状态打印如 `[display] active`）。
- `kernel/drivers/usb/uhci.c`：本次排障新增的 `[uhci-dbg]` PORTSC dump。

## 4. 验证

### 4.1 构建
```
make iso          # 构建内核镜像 + ISO（含服务 blob）
make disk         # 磁盘镜像（已存在则 skip）
```

### 4.2 运行（正确 QEMU 命令：仅 `-usb`，勿再加 `-device piix3-usb-uhci`）
```
qemu-system-x86_64 -machine pc -cpu qemu64 -smp 1 -m 2G -no-shutdown \
  -display none -serial file:/tmp/ur.log -boot d -cdrom build/SukiOS.iso \
  -drive file=build/disk.img,format=raw,index=0,media=disk \
  -usb -device usb-kbd -device usb-mouse
```
等待 ~15s 后 `cat -v /tmp/ur.log` 判读。

### 4.3 关键日志证据（修复后）
- USB 完整枚举：
  ```
  [usb] host service started (UHCI, polling)
  [usb-hid] keyboard ready (addr=1 ep=81 mps=8)
  [usb] root port 1: device attached (full-speed)
  [usb] root port 2: device attached (full-speed)
  [usb-hid] mouse ready (addr=3 ep=81 mps=4)
  [usb] hub 1 port 1: device attached (full-speed)   # 鼠标经内置 hub
  ```
- 启动/渲染/健康：
  ```
  [shell] SukiOS shell online (Ring3, bash-like)
  [sched] load balance: 1 CPUs
    cpu0: rq=27 user_switches=4551        # 调度频繁、系统活跃、无 panic
  ```
- 修复前对照：磁盘读相邻 gap 中位数 ~20ms（10ms tick 整数倍）；修复后 gap 降至 **数百微秒**（如 300–600µs，pio 本身 ~70µs，瓶颈消除）。带诊断的验证轮曾打印 `[display] FLUSH ... nonbg=86400`，证明窗口内容（非仅外框）已合成。
- 残余诊断检查：`[smp-dbg]/[disk-dbg]/[spawn-dbg]/[fs-dbg]/[uhci-dbg]/[display] FLUSH` 全部为空。
- 全程无 `panic` / `system halted` / 三重故障。

## 5. 涉及文件与关键常量

| 文件 | 改动 |
|------|------|
| `kernel/sched/sched.c` | 新增 `g_jiffies`、`rq_move_head_cpu()`、`msleep()`；`sched_wake` 接入 boost 并清零 `wake_jiffies`；`sched_tick` 推进 jiffies 并唤醒到期睡眠者；移除 `[smp-dbg]` |
| `include/kernel/task.h` | `task_t.wake_jiffies`；声明 `msleep()` |
| `kernel/drivers/usb/usb_core.c` | `usb_service_task` 用 `msleep(8)` 替代 `usb_ms(8)` |
| `kernel/drivers/usb/uhci.c` | `uhci_poll` 主动枚举已连接未枚举端口 + `g_root_enumed[]`；移除 `[uhci-dbg]` |
| `kernel/drivers/ata.c` | 移除 `[disk-dbg]` 及计时变量 |
| `kernel/syscall/syscall.c` | 移除 `[spawn-dbg]` |
| `user/fs_server.c` | 移除 `[fs-dbg]` |
| `user/display_server.c` | 移除 `[display] FLUSH` 刷屏统计 |

关键常量：`UHCI_PORT_CCS=1<<0`、`UHCI_PORT_CSC=1<<1`、`UHCI_PORT_RESET=1<<9`（`include/kernel/uhci.h`）；`IPI_RESCHED` 向量；100Hz tick（`TIME_SLICE_TICKS` 对应 ~10ms）；`msleep` 粒度 `(ms+9)/10` 节拍。

## 6. 约束遵循

- 未改动双系统调用 API（POSIX 20–129 与 SukiNative 130–149 平行共存）；
- 未使用 GDB/外部调试脚本，全部经 bash + QEMU 串口日志（`cat -v`/`grep`）判读；
- 未引入 stub/TODO，所有改动均端到端可用并验证；
- 构建缓存清理沿用 `make clean`（本次未触发，仅增量重编）。
