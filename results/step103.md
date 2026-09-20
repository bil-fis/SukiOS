# step103 — USB 复位也去忙等：驱动阶段移入 boot_late 任务，全部长延改真睡眠

用户诉求：

> 「复位也不要忙等。」

即：上一轮已消除 UHCI **端口复位/枚举**的忙等，但 **`uhci_init()`（控制器复位 HCRESET/GRESET 等）** 的 20/20/5/10ms 仍是 `usb_udelay`（纯 `inb` 空转）。本轮把它也改为真睡眠。

---

## 0. 结论速览

| 项 | 结果 |
|---|---|
| 为何之前不能直接改 | `uhci_init()` 在 **kmain** 里被驱动 probe 调用；kmain 阶段 `current_task` 仍是 **idle0**，在 kmain 调 `msleep` 会把 idle0 置 BLOCKED，**不安全** |
| 关键改动 | 把【驱动优先阶段】从 kmain 移到 **`boot_late_init` 内核任务的最前面**（其本身是可睡眠的真实任务），仍在**任何 Ring3 服务之前** |
| 效果 | `uhci_init()` 的控制器复位（20/20/5/10ms）、端口使能等待、控制传输 TD 等待全部走 `msleep` 真睡眠；仅保留 ≤2ms 的短自旋 |
| 验证 | USB+hub 连续 2 次：驱动阶段在 boot_late 起始、USB 键鼠枚举、窗口、提示符 `SukiOS:/>`、零 panic、PASS 全绿 |

---

## 1. 问题

`kernel/drivers/usb/uhci.c::uhci_init()` 中：
```c
uw16(UHCI_USBCMD, UHCI_CMD_HCRESET);  usb_udelay(20000);   /* 20ms 忙等 */
for (...) { ... usb_udelay(1000); }                        /* 每轮 1ms 忙等 */
uw16(UHCI_USBCMD, UHCI_CMD_GRESET);   usb_udelay(20000);   /* 20ms 忙等 */
uw16(UHCI_USBCMD, 0);                 usb_udelay(5000);    /* 5ms 忙等 */
... 使能后                            usb_udelay(10000);   /* 10ms 忙等 */
```
以及 `uhci_root_port_reset()` 的端口使能等待 `usb_udelay(1000)`。

这些都在 kmain（驱动优先阶段）里执行——而 kmain 阶段 `g_percpu[0].current_task == idle0`：
`msleep()` 会把 **idle0** 置 `BLOCKED` 并 `schedule()`，破坏 idle 兜底语义，**不安全**。所以之前它们只能忙等。

---

## 2. 修复

### 2.1 驱动优先阶段移入 boot_late 任务
**`kernel/kmain.c`**
- 删除 kmain 中的 `builtin_drivers_register()` 调用（保留 `console_srv` 创建）；
- 在 **`boot_late_init()` 的最前面**（`kprintf("[boot] late-init thread...")` 之后）调用 `builtin_drivers_register()`。

`boot_late_init` 是 `task_create_kernel` 创建的**真实内核任务**（独立内核栈、被正常调度），在其上下文调用 `msleep` 是安全的。位置仍在**所有 Ring3 服务之前** → 依旧满足“驱动最先加载”。

**`kernel/drivers/usb/usb_core.c::usb_init()`**：在 `uhci_init()` 之前置 `uhci_set_can_sleep(true)`，使控制器复位等长延走 `msleep`。

### 2.2 全部长延改真睡眠
**`kernel/drivers/usb/uhci.c`**（`usb_delay_ms()` 在 `g_can_sleep` 时用 `msleep`）：
- `uhci_init()`：`usb_udelay(20000/1000/20000/5000/10000)` → `usb_delay_ms(20/1/20/5/10)`；
- `uhci_root_port_reset()`：端口使能等待 `usb_udelay(1000)` → `usb_delay_ms(1)`；
- `uhci_control()`：TD 等待改为「先自旋 ~2ms（多数传输此时已完成），之后 `msleep(1)` 让出」。

**剩余 `usb_udelay`（均为 ≤2ms 短自旋，非长忙等）**：
```
104: usb_delay_ms 的 !g_can_sleep 回退分支（当前路径不会触发）
425: usb_udelay(200)   /* 控制传输前 ~2ms 短自旋 */
442: usb_udelay(2000)  /* 控制传输 NAK 重试间隔 2ms */
```

---

## 3. 验证（QEMU，USB + hub + kbd + mouse）

```
run1: disp=1 wm=2 usb=2 prompt=1 err=0 PASS=206
run2: disp=1 wm=2 usb=2 prompt=1 err=0 PASS=105
```
boot_late 起始即驱动阶段（顺序正确）：
```
[boot] late-init thread: kernel fully stable, loading services...
[driver] registering built-in PCI drivers (ata/ahci/hda/e1000/uhci)...
[driver] registered 'ata-ide' ; [device] '8086:7010' bound to 'ata-ide'
[driver] registered 'intel-hda'/'e1000'/'uhci-usb' ; ... bound ...
[driver] built-in drivers probed: disk=1 hda=0 e1000=1 usb=1
[disk-srv] serving DISK_PORT ...
[boot] display-server ready ...
[usb-hid] keyboard ready / mouse ready
SukiOS:/> ...
```
- 驱动在所有服务之前完成；USB 键鼠正常枚举；shell 出提示符；**零 panic/EXC**。

---

## 4. 关键文件

| 项 | 位置 |
|---|---|
| 驱动优先阶段改到任务上下文 | `kernel/kmain.c::boot_late_init()`（起始处 `builtin_drivers_register()`） |
| USB 可睡眠开关 | `uhci.c::uhci_set_can_sleep()/usb_delay_ms()`；`usb_core.c::usb_init()` 先置 true |
| 控制器复位去忙等 | `uhci.c::uhci_init()`（20/1/20/5/10ms → msleep） |
| 端口使能等待去忙等 | `uhci.c::uhci_root_port_reset()` |
| 控制传输 TD 等待 | `uhci.c::uhci_control()`（短自旋 + msleep） |

## 5. 修改文件清单
- `kernel/kmain.c`：驱动优先阶段移入 `boot_late_init` 起始
- `kernel/drivers/usb/usb_core.c`：`usb_init()` 先 `uhci_set_can_sleep(true)`
- `kernel/drivers/usb/uhci.c`：控制器复位/端口使能/控制传输等待改 `usb_delay_ms`(msleep)

## 6. 已知/后续
- `usb_delay_ms()` 的 `!g_can_sleep` 回退仍是微延迟忙等，但当前所有调用路径（boot_late 任务、轮询任务）都先置 `g_can_sleep=true`，该分支不会触发；保留仅为防御「万一在无调度上下文调用」。
- 真实鼠标手感/窗口表现需图形界面实测。
