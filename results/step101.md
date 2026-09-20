# step101 — USB 键鼠长时间无响应 / 长时间后才正常：根因定位与修复

用户反馈：

> 「还是不行，但是长时间等待后会正常。问题应该还是在 usb 驱动上，ps/2 就速度很快，很正常，
> 检查 usb 驱动是否长时间阻塞调度器。」

本轮定位到 **USB 驱动两处「长时间占用/阻塞」问题并修复**，USB 键鼠现在在服务/显示之前即完成枚举。

---

## 0. 结论速览

| 现象 | 根因 | 修复 |
|---|---|---|
| USB 键鼠长时间无反应，等很久才正常（PS/2 一直正常） | USB **枚举在轮询任务里异步执行**，其任务首次运行是否先于 display/input/shell，取决于 `task_create_kernel` 的 self-IPI 能否抢占 kmain（**非确定性**）。某些时序下该任务被推迟到开机自检全部跑完后才首次运行 → 枚举（含端口复位）发生在**几十秒后** | 在 `usb_init()` 里**同步完成首次枚举**，且位于驱动优先阶段（任何 Ring3 服务之前） |
| 整机卡顿、鼠标延迟（USB 时） | `uhci_poll()` 对「已连接但枚举失败」的根端口**每轮都重做 ~180ms 忙等复位**（SetReset 100ms + ClearReset 50ms + 稳定 30ms），实现为 `usb_udelay` 纯 `inb` 空转 | 增加**失败退避**：失败后仅每 ~512ms 重试一次；轮询间隔本身用 `msleep(8)` |

验证：USB 枚举在 3/3 次运行中稳定早于 `display-server`；shell 正常出提示符 `SukiOS:/>`、字体服务上线；**PASS=105 / FAIL=0 / 零 panic**。

---

## 1. 诊断过程（仅 bash + QEMU 自身机制）

对照用户日志：
- `[usb] host service started` 出现在**很早期**（驱动优先阶段），但 `[usb] device addr=1/2/3` 枚举行出现在**所有开机自检之后**；
- `[sched] load balance: cpu0 rq=16 user_switches=244` 切换数很低。

本地复现（`-device piix3-usb-uhci -device usb-kbd -device usb-hub -device usb-mouse`）：
- 多数运行枚举很早（USB 任务抢先于 kmain 的 self-IPI 生效）；
- 说明该"早/晚"取决于 **self-IPI 与 kmain 的竞态**，是非确定性的——正是用户"有时要等很久"的来源。

在 `usb_service_task` 入口加 `[usb-dbg] service task first run` 打印对比运行，确认「任务首次运行时刻」在两次运行间差异很大（早于 `SukiConsoleServer` 创建，或晚到自检之后）。

**代码审查另发现**：`kernel/drivers/usb/uhci.c::uhci_poll()` 对 `ccs && !g_root_enumed[p]` 的端口，只要枚举失败就**保持未枚举**，于是每个轮询轮次（8ms）都重新调用 `uhci_root_port_reset()`，而该函数内的复位时序全部是 **`usb_udelay` 忙等**：
```c
uw16(reg, UHCI_PORT_RESET);  usb_udelay(100000);   /* SetReset   100ms 忙等 */
...                          usb_udelay(50000);    /* ClearReset  50ms 忙等 */
...                          usb_udelay(30000);    /* 稳定 30ms 忙等 */
```
→ 若某端口枚举持续失败，`usb_service` 任务将 ~180ms 忙等 / 8ms 睡眠 ≈ **95% CPU 空耗**，系统性拖慢整机——即用户所说"USB 驱动长时间阻塞调度器"。

---

## 2. 修复

### 2.1 `kernel/drivers/usb/usb_core.c::usb_init()` —— 同步首次枚举（驱动先加载）
在 `uhci_init()` 成功、置 `g_hc_ready` 之后、**创建轮询任务之前**，直接对根端口/Hub 连做若干轮 `usb_poll()`：
```c
for (int i = 0; i < 6; i++) {
    usb_poll();      /* 第 1 轮枚举根端口(键盘/Hub)，其后各轮推进 Hub 下行(鼠标) */
}
task_t *t = task_create_kernel(usb_service_task, NULL, "SukiUsbHost");
```
- 该代码运行于**驱动优先阶段**（`kmain` 在 `posix_init()` 之后调用 `builtin_drivers_register()` → `probe_uhci` → `usb_init()`），即**任何 Ring3 服务之前**，此时仅 idle0 存在，枚举的一次性延时不影响任何服务/显示；
- 完成后 USB 键鼠立即可用；随后创建的轮询任务只做轻量 HID/Hub 周期轮询。

### 2.2 `kernel/drivers/usb/uhci.c` —— 根端口枚举失败退避
新增 `g_root_retry[8]`：
```c
if (ccs && !g_root_enumed[p]) {
    if (g_root_retry[p] > 0) { g_root_retry[p]--; }      /* 退避中：跳过本轮 */
    else if (uhci_root_port_reset(p, &ls)) {
        int idx = usb_core_root_connect(p, ls);
        if (idx >= 0) g_root_enumed[p] = true;
        else         g_root_retry[p] = 64;               /* ~512ms 后再试 */
    } else { g_root_retry[p] = 64; }
}
```
杜绝"每轮 ~180ms 忙等复位"的系统性空耗；断开重连时清零退避。

（轮询间隔 `msleep(8)` 已在早前引入，非忙等。）

---

## 3. 验证

**命令**：
```bash
make iso disk
qemu-system-x86_64 -machine pc -cpu Skylake-Client -smp 1 -m 2G -no-shutdown \
  -display none -serial file:/tmp/x.log -boot d -cdrom build/SukiOS.iso \
  -drive file=build/disk.img,format=raw,index=0,media=disk \
  -device piix3-usb-uhci -device usb-kbd -device usb-hub -device usb-mouse
```

**结果（连续多次）**：
```
[usb-hid] keyboard ready (addr=1 ...)      <-- 驱动优先阶段（早）
[usb-hub] ready: addr=2 ports=8
[usb-hid] mouse ready (addr=3 ...)
[driver] built-in drivers probed: disk=1 hda=0 e1000=1 usb=1
...
[boot] display-server ready (g_display_active=1, waited=2 yield rounds)   <-- 167 行
[shell] SukiOS shell online                                               <-- 175 行
[shell] font service spawned / [fontsrv] font service online
SukiOS:/>                                                                 <-- 提示符渲染
```
- USB 枚举稳定**早于** display-server（3/3 次，行号 114~121 vs 167）；
- `PASS=105 FAIL=0`，`panic/EXC/halted = 0`。

---

## 4. 关键文件/常量

| 项 | 位置 |
|---|---|
| 同步首次枚举 | `kernel/drivers/usb/usb_core.c::usb_init()`（`usb_poll()` × 6） |
| 轮询任务（轻量） | `usb_core.c::usb_service_task()`（`usb_poll(); msleep(8);`） |
| 端口复位忙等 | `uhci.c::uhci_root_port_reset()`（SetReset 100ms / ClearReset 50ms / 稳定 30ms） |
| 失败退避 | `uhci.c::uhci_poll()` + `g_root_retry[8]`（64 轮 ≈ 512ms） |
| 驱动优先阶段 | `kernel/driver/builtin_drivers.c` + `kernel/kmain.c`（step100 引入） |

## 5. 修改文件清单
- `kernel/drivers/usb/usb_core.c`：`usb_init()` 增加同步首次枚举；移除临时诊断
- `kernel/drivers/usb/uhci.c`：根端口枚举失败退避 `g_root_retry[]`

## 6. 已知/后续
- UHCI 端口复位的 100/50/30ms 仍为忙等（发生在驱动优先阶段的一次性枚举中，此时无其它任务，影响可忽略）；若后续需要热插拔时也零占用，可在轮询任务上下文改用内核 `msleep` 替代（需区分调用上下文）。
- 真实鼠标手感/窗口表现仍需图形界面实测（headless 无法注入真实鼠标移动）。
