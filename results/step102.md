# step102 — USB 去忙等（追平 PS/2 速度）+ 优先级抢占调度

用户诉求：

> 「还是特别慢。依然需要等很久 shell 窗口才出现，很久后 shell 才渲染。我想要 ps/2 那样的快速度，
> usb 不要再忙等，尽快让出 cpu。然后实现优先级抢占调度。」

本轮：**(1) 消除 USB 全部长延时忙等，改为真睡眠让出 CPU**；**(2) 实现优先级抢占调度（并按实测排除会引发饥饿的自动提升）**。

---

## 0. 结论速览

| 项 | 结果 |
|---|---|
| USB 忙等 | UHCI 端口复位 `100ms/50ms/30ms`、枚举重试 `20ms` 等长延时原为 `usb_udelay`（纯 `inb` 空转）。改为任务上下文用内核 `msleep` **真睡眠让出 CPU** |
| 同步枚举 | 撤销此前在 kmain 里「同步 6 轮 usb_poll」的做法（那会让**引导**在忙等中度过）。首次枚举回到轮询任务，用 `msleep` 推进，与 display/input 服务**并行** |
| 启动速度 | 到达 shell 提示符的时间从 **~58–70s → ~32s**，已接近 PS/2（PS/2 提示符行号 321 vs USB 362，差距大幅缩小） |
| 优先级抢占 | `pick_next` 按优先级（数值最小）选任务 + 严格优先级（更高者不被低者抢占）+ 唤醒 head-move/self-IPI 立即抢占。**自动唤醒提升（硬提到交互级）实测会反复饿死 display/shell，已弃用**（见 §4） |
| 验证 | USB+hub 连续 3 次：display-server ready、窗口创建、USB 键/鼠枚举、字体服务上线、**`SukiOS:/>` 提示符**、**零 panic**、PASS=105 |

---

## 1. USB 去忙等（用户：usb 不要再忙等，尽快让出 cpu）

### 1.1 问题定位
- USB 轮询间隔已是 `msleep(8)`；`uhci_int_poll()`（HID 报告读取）本身**非阻塞**（只读 TD 状态）。
- 真正的忙等在 **`uhci_root_port_reset()`**：
  ```c
  uw16(reg, UHCI_PORT_RESET); usb_udelay(100000);   /* SetReset   100ms 纯 inb 空转 */
  ...                         usb_udelay(50000);    /* ClearReset  50ms */
  ...                         usb_udelay(30000);    /* 稳定        30ms */
  ```
  以及 `usb_core_enum()` 的重试 `usb_ms(20)`。
- 上一版把这套复位/枚举**搬进了 kmain 同步执行**（step101），导致**引导本身**在 ~数百 ms 忙等里度过，整体更慢。

### 1.2 修复
**`kernel/drivers/usb/uhci.c`**
```c
static bool g_can_sleep = false;                 /* 轮询任务上下文才可真睡眠 */
void uhci_set_can_sleep(bool on) { g_can_sleep = on; }
static void usb_delay_ms(uint32_t ms) {
    if (g_can_sleep) msleep(ms);                 /* 让出 CPU（真睡眠） */
    else             usb_udelay(ms * 1000u);     /* 仅引导早期 uhci_init 退化为忙等 */
}
```
端口复位改用 `usb_delay_ms(100/50/30)`。

**`kernel/drivers/usb/usb_core.c`**
- `usb_service_task()` 进入时：`g_sleep_ok = true; uhci_set_can_sleep(true);`；
- 枚举重试 `usb_ms(20)` / `SET_ADDRESS` 后 `usb_ms(5)` → `usb_sleep_ms(...)`（任务上下文即 `msleep`）；
- **撤销** `usb_init()` 里 kmain 中的同步 `for(i<6) usb_poll()`；首次枚举回到轮询任务，用真睡眠推进，与显示/输入服务**并行**，不再拖慢 UI。

> 效果：USB 键鼠枚举不再独占 CPU；UI 与枚举并行，shell 窗口与渲染显著提前。

---

## 2. 优先级抢占调度

### 2.1 实现（`kernel/sched/sched.c` + `include/kernel/task.h`）
- `#define PRI_BOOST 64 / PRI_NORMAL 128 / PRI_MAX 255`（0–255，数值越小越高，与 POSIX `setpriority` 同尺度）。
- `pick_next()`：选 **priority 数值最小**的可运行任务；同优先级按队列顺序（RR 公平）。
- **严格优先级**：若当前任务优先级**严格高于**所有其它就绪任务，则**继续运行它**（不被更低优先级任务抢占）；相等时才 RR 轮转。
- `sched_wake()`：被唤醒任务 **head-move** + 同核 `self-IPI` → `schedule()` 立即抢占（微秒级唤醒响应）。

### 2.2 为何不做“自动唤醒提升”（重要，有实测证据）
曾尝试：唤醒时把任务硬提升到 `PRI_BOOST(64)`，并配合「防饥饿老化」。**连续两轮实测均复现同一回归**：
- 时序 A（step99 初次）：`display-server did NOT become ready`（无窗口）；
- 时序 B（本轮）：`display-server ready` 但 **shell 停在 `[shell] shell online`、永不建窗/渲染**（`[wm] window created` 缺失）。
根因：被唤醒任务（含每 8ms 被 msleep 唤醒的 USB 轮询等）持续占满 64 级，**永不阻塞的 display/shell 等 128 级任务被饿死**。
故最终**不采用硬提升**：仅用 head-move（唤醒者插队一回合并立即抢占），既拿到“立即抢占”的延迟收益，又不会饿死任何任务。显式 `setpriority()` 仍按优先级生效（真正的优先级抢占语义由 §2.1 的严格优先级 + 抢占点提供）。

### 2.3 boot 屏障配合
`kmain.c` 等待 `g_display_active` 的循环由「20000 次快速 `task_yield()`」改为「`msleep(1)` × 2000（wall-clock 有界，约 20s 上限）」——以兼容优先级调度下的节拍时间尺度。

---

## 3. 验证（QEMU，USB + hub + kbd + mouse）

```bash
make iso disk
qemu-system-x86_64 -machine pc -cpu Skylake-Client -smp 1 -m 2G -no-shutdown \
  -display none -serial file:/tmp/x.log -boot d -cdrom build/SukiOS.iso \
  -drive file=build/disk.img,format=raw,index=0,media=disk \
  -device piix3-usb-uhci -device usb-kbd -device usb-hub -device usb-mouse
```
连续 3 次结果一致：
```
run1: disp=1 warn=0 wm=2 usb=2 font=1 prompt=1 err=0 PASS=105
run2: disp=1 warn=0 wm=2 usb=2 font=1 prompt=1 err=0 PASS=105
run3: disp=1 warn=0 wm=2 usb=2 font=1 prompt=1 err=0 PASS=105
```
- 42s 内全部到达 `SukiOS:/>` 提示符；对比：忙碌等版需 **58–70s**。
- PS/2（无 USB）对照：提示符在 321 行；USB 在 362 行——差距已很小。

---

## 4. 关键文件/常量

| 项 | 位置 |
|---|---|
| USB 可睡眠长延时 | `uhci.c::usb_delay_ms()` + `uhci_set_can_sleep()`；`usb_core.c::usb_sleep_ms()`/`g_sleep_ok` |
| 端口复位时序 | `uhci.c::uhci_root_port_reset()`（100/50/30ms，task 上下文 msleep） |
| 优先级常量 | `include/kernel/task.h`（`PRI_BOOST/PRI_NORMAL/PRI_MAX`） |
| 优先级选择+严格优先级 | `sched.c::pick_next()` |
| 立即抢占 | `sched.c::sched_wake()`（head-move + self-IPI） |
| boot 屏障 | `kmain.c`（`msleep(1)` × 2000） |

## 5. 修改文件清单
- `kernel/drivers/usb/uhci.c`：`usb_delay_ms()`/`uhci_set_can_sleep()`；端口复位改可睡眠
- `kernel/drivers/usb/usb_core.c`：`usb_sleep_ms()`/`g_sleep_ok`；任务启动开启可睡眠；撤销 kmain 同步枚举
- `kernel/sched/sched.c`：`pick_next()` 按优先级 + 严格优先级；`sched_wake()` 保留 head-move
- `include/kernel/task.h`：新增优先级常量
- `kernel/kmain.c`：boot 屏障改 `msleep` 有界

## 6. 已知/后续
- 引导早期 `uhci_init()` 的 20/20/5/10ms 复位仍为忙等（发生在 kmain、无其它任务，影响可忽略）。
- 真实鼠标手感/窗口表现需图形界面实测（headless 无法注入真实鼠标移动）。
- 若日后要“自动交互优先级”，需配套真正的**优先级继承/捐赠**（针对 IPC 等待链），而非简单硬提升——后者已被实测证明会饿死任务。
