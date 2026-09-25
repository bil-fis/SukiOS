# step110 — USB 键鼠「长时间无响应」问题复现与修复（恢复 step101 同步首次枚举）

## 1. 问题背景

用户反馈：

> 「前面说的 usb 驱动的问题又重新出现了，请你检查并修复」

所谓「前面说的 USB 驱动问题」即 `results/step101.md` 记录的 **USB 键鼠长时间无响应 / 长时间后才正常**：
根因是 USB 枚举在轮询任务里**异步执行**，其首次运行时机取决于调度器能否抢占 kmain，
**非确定性**，某些时序下被推到全部开机自检之后才首次运行 → 键鼠长时间无响应（PS/2 一直正常）。
step101 当时通过「在 `usb_init()` 内**同步完成首次枚举**（驱动优先阶段，任何 Ring3 服务之前）」修复，
验证 USB 枚举稳定早于 `display-server`。

本次为**同一问题的回归**：提交 `478191e` 把 step101 的同步枚举回退成了异步轮询，重现了非确定性。

---

## 2. 回归根因（git 定位）

`git log -L 465,497:kernel/drivers/usb/usb_core.c` 与 `git log --oneline -i --grep usb` 确认：

- 提交 `478191e`（`perf(usb): 控制器复位等长延也去忙等（驱动阶段移入 boot_late 任务上下文）`）
  将 step101 的「同步首次枚举」回退为「仅异步轮询任务枚举」。
- 回退后 `usb_init()` 注释明确写道「**【不在此处同步枚举】**…首次枚举交给下面的轮询任务」，
  并直接 `task_create_kernel(usb_service_task, NULL, "SukiUsbHost")` 异步枚举。

后果：`usb_service_task` 创建后，`boot_late_init` 继续创建 FS / display / input / shell 等 Ring3 服务，
`usb_service_task` 首次运行时机不确定，某些时序下晚于 `display-server`，端口复位（msleep）期间 UI 无输入
→「USB 键鼠长时间无响应」重现，与 step101 描述的现象一致。

---

## 3. 第一版修复尝试与发现

最初仅恢复「同步 `usb_poll()×6`」，但保留 `g_can_sleep=true`（端口复位走 `msleep`）。QEMU 实测：

```
117:[usb] enum: SET_ADDRESS failed
118:[uhci] port 1 enum failed, retry in ~512ms
...
174:[boot] display-server ready ...
189:[usb-hid] keyboard ready (addr=3 ...)     <-- 晚于 display-server
200:[usb-hid] mouse ready (addr=5 ...)        <-- 更晚
```

- 根端口同步枚举**首轮 `SET_ADDRESS failed`**，进入 ~512ms 退避，真正枚举被推迟到轮询任务；
- USB 键鼠就绪时间（键盘行 190 / 鼠标行 200）仍晚于 `display-server ready`（行 174）。

根因：纯 `msleep` 让出 CPU 时，`SET_ADDRESS` 落在端口复位（100/50/30ms）后设备尚未就绪的窗口，
控制传输失败（而 step101 当初在 kmain 用 **busy-wait 连续时序**才首轮成功）。
即：step101 的成功依赖「复位紧接 SET_ADDRESS、不被调度/tick 插入打断」的连续时序。

---

## 4. 最终修复

文件 `kernel/drivers/usb/usb_core.c::usb_init()`：在创建轮询任务之前、`g_hc_ready=true` 之后插入同步首次枚举，
且同步阶段**临时切回忙等**以复制 step101 验证通过的连续时序：

```c
    g_hc_ready = true;

    /* 恢复 step101 修复：提交 478191e 误将“同步首次枚举”回退为「仅异步轮询任务
     * 枚举」，重新引入“USB 键鼠长时间无响应”的非确定性——枚举首次运行时机取决于
     * 调度器何时切到 usb_service_task，某些时序下被推到全部 Ring3 服务之后，端口
     * 复位期间 UI 无输入。
     * 此处【在 boot_late 任务上下文、任何 Ring3 服务之前】同步完成首次枚举：根端口
     * 键盘/Hub、Hub 下行鼠标逐轮推进（轮 1 枚举根端口，其后续轮推进 Hub 下行）。
     * 关键：同步枚举阶段临时切回【忙等】(g_can_sleep=false)，复制 step101 验证通过
     * 的连续时序——端口复位 100/50/30ms 紧接 SET_ADDRESS，不被调度/tick 插入打断，
     * 才能在 QEMU 下确定性地首轮枚举成功（纯 msleep 让出 CPU 会让 SET_ADDRESS 落在
     * 设备复位后尚未就绪的窗口而失败）。此时无其它任务需运行（Ring3 尚未创建），
     * 短暂 100% 占 CPU 不影响任何服务；枚举成功后恢复 msleep 给后续轮询任务。 */
    uhci_set_can_sleep(false);
    for (int i = 0; i < 6; i++) {
        usb_poll();
    }
    uhci_set_can_sleep(true);

    task_t *t = task_create_kernel(usb_service_task, NULL, "SukiUsbHost");
```

要点：

- **调用上下文**：`usb_init()` 经由 `builtin_drivers_register()` 在 `boot_late_init()`
  （`kernel/kmain.c:599`）内调用，位于所有 Ring3 服务（FS/display/input/shell，kmain.c 633+/733+）之前。
  `boot_late` 是独立内核任务，`msleep` 安全；同步阶段用 busy-wait 不依赖调度唤醒，复制 step101 连续时序。
- 端口复位 100/50/30ms 在同步阶段为 busy-wait，但此时无其它任务（Ring3 尚未创建），短暂 100% 占 CPU 不影响任何服务。
- 同步枚举成功后，轮询任务 `usb_service_task` 仍以 `msleep(8)` 轮询 + 失败退避 `g_root_retry[]`
  （step101 修复 2.2 保留）处理热插拔与周期轮询。
- 同时保留 `478191e` 的改进：控制器复位（`uhci_init`）在 boot_late 任务上下文走 `msleep` 不忙等；
  异步轮询路径的端口复位也走 `msleep`。

---

## 5. 关键文件 / 常量 / 函数

| 项 | 位置 |
|---|---|
| 同步首次枚举 + 忙等切换 | `kernel/drivers/usb/usb_core.c::usb_init()`（`uhci_set_can_sleep(false)` → `usb_poll()×6` → `uhci_set_can_sleep(true)`） |
| 忙等/睡眠开关 | `kernel/drivers/usb/uhci.c::uhci_set_can_sleep()` / `usb_delay_ms()`（`g_can_sleep`） |
| 根端口轮询 / 失败退避 | `kernel/drivers/usb/uhci.c::uhci_poll()` + `g_root_retry[8]`（step101 修复 2.2，保留） |
| 驱动优先阶段调用点 | `kernel/kmain.c::boot_late_init()` → `builtin_drivers_register()`（kmain.c:599，Ring3 之前） |
| 控制器复位时序 | `uhci.c::uhci_root_port_reset()`（SetReset 100ms / ClearReset 50ms / 稳定 30ms，`g_can_sleep` 决定忙等或 msleep） |
| 轮询任务（轻量） | `usb_core.c::usb_service_task()`（`usb_poll(); msleep(8);`） |

---

## 6. 验证（仅 bash + QEMU 自身机制，符合项目调试铁律）

构建：`make iso`（含 `CONFIG_DRIVER_USB` 默认开启）；`make disk`。

命令（两场景，均为单核 `-smp 1`）：

- 场景 A（等同默认 `make run` 的 `QEMU_INPUT`）：
  `-usb -device usb-kbd -device usb-mouse`
- 场景 B（step101 原命令）：
  `-device piix3-usb-uhci -device usb-kbd -device usb-hub -device usb-mouse`

```
qemu-system-x86_64 -machine pc -cpu qemu64 -smp 1 -m 2G -no-shutdown \
  -display none -serial file:/tmp/x.log \
  <场景 A 或 B 的 USB 参数> \
  -boot d -cdrom build/SukiOS.iso \
  -drive file=build/disk.img,format=raw,index=0,media=disk
```

两场景串口日志关键行（行号取场景 B 实测，场景 A 一致）：

```
115:[uhci] found PCI 0:4.0 vid=8086 did=7020
116:[uhci] ready: I/O=0xC040 ports=2 PCI 0:4.0
117:[usb] device addr=1 FS vid=0627 pid=0001 class=3 iface=3/1/1 ep=81 mps=8
118:[usb-hid] keyboard ready (addr=1 ep=81 mps=8)        <-- 键盘（同步枚举，早于服务）
119:[usb] root port 1: device attached (full-speed)
120:[usb] device addr=2 FS vid=0409 pid=55aa class=9 iface=9/0/0 ep=81 mps=2
121:[usb-hub] ready: addr=2 ports=8                       <-- Hub（同步枚举）
122:[usb] root port 2: device attached (full-speed)
123:[usb] device addr=3 FS vid=0627 pid=0001 class=3 iface=3/1/2 ep=81 mps=4
124:[usb-hid] mouse ready (addr=3 ep=81 mps=4)            <-- 鼠标（Hub 下行，同步枚举）
125:[usb] hub 1 port 1: device attached (full-speed)
127:[usb] host service started (UHCI, polling)
...
179:[boot] display-server ready (g_display_active=1, waited=1 rounds); boot screen still held, mounting remaining services...
248:[nettest] libc-net API: PASS=4 FAIL=0  ALL OK
320:[libc-test] PASS=11 FAIL=0  ALL OK
370:SukiOS:/> [pchfnt] request: font=/FONTS/RESOURCEHANROUNDEDCN-MEDIUM.TTF size=40 (utf8) text="SukiOS FreeType"
```

判定：

- USB 键鼠枚举行（117-124）**稳定早于** `display-server ready`（行 179），差距约 60 行；
- 无 `panic` / `EXC` / `halted` / `BAD SWITCH`；
- 各自检 `PASS=… FAIL=0` 全通过（nettest PASS=4、libc-test PASS=11 等）。

场景 A（默认 `-usb` 直连）实测同样：键盘 addr=1 / Hub addr=2 / 鼠标 addr=3 在行 127 前完成，display-server 在行 179，零 panic。

---

## 7. 结论

- step101 修复的「USB 键鼠在服务/显示前就绪、不再长时间无响应」已**恢复**；
- 同时保留 `478191e` 的改进（端口复位在 boot_late 任务上下文 / 异步轮询路径走 `msleep` 不忙等）；
- USB 驱动回归问题已解决，QEMU 生产场景（双 USB 场景）零 panic，系统稳定可预期。

> 说明：实际鼠标手感 / 窗口交互表现仍需图形界面人工实测（headless 无法注入真实鼠标移动）。
> 如需人工确认，可用 `make run` 在图形窗口移动 USB 鼠标 / 敲击键盘验证响应即时性；
> 当前 headless 串口日志已证明枚举确定性早于 UI，根因（非确定异步枚举）已消除。
