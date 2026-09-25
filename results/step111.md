# step111 — USB 键鼠「时而好时坏、鼠标坏时键盘也坏」：根因定位与修复

## 0. 现象（用户反馈）

> 「现在 usb 鼠标的问题就是时而好时坏：鼠标进入 qemu 后无论怎么样都无法移动系统内的
> 光标，但是有的时候（很少）是可以正常移动的，并且在鼠标无法移动的情况下键盘也无法使用。」

关键特征：
- **鼠标无法移动时，键盘也完全无法使用** —— 二者同时失灵，是**总线级**故障（不是单个 HID 端点问题）；
- **偶发**（「时而好时坏」「很少能正常移动」）—— 非确定性，典型的启动竞态。

## 1. 根因定位

### 1.1 直接诱因：上一步「还原上次改动」把修复删了
本步之前用户要求「还原上次改动」，该操作 `git revert be08eac` 撤销了 **step101 的
「同步首次枚举」修复**，USB 回到**异步枚举**模型。step101 原文明确指出异步枚举的缺陷：

> 枚举在轮询任务里异步执行，其任务首次运行是否先于 display/input/shell，取决于
> self-IPI 能否抢占 kmain（**非确定性**）。某些时序下该任务被推迟到开机自检全部跑完后
> 才首次运行 → 枚举（含端口复位）发生在几十秒后；或更糟，首次枚举因设备未稳定而失败，
> 之后即便重试也偶发不成功。

一旦整个 UHCI 总线枚举失败，`usb_poll()` 末尾的仲裁会把
`input_pref_set_usb_kbd(false)` / `input_pref_set_usb_mouse(false)` 置位 → 输入栈回退到
**PS/2**。而本系统在 QEMU 下只有 USB 输入设备（无 PS/2 硬件），于是**键鼠同时失灵** ——
完美解释「鼠标不动时键盘也用不了」的对称故障。

### 1.2 二次根因：引导早期 `msleep` 不可靠（为什么单纯「异步→同步」还不够）
重新应用时若让同步枚举走 `msleep` 让出 CPU（step102 引入的「不忙等」语义），
实测首次枚举仍 `SET_ADDRESS failed`、两端口 `enum failed, retry in ~512ms`，
要靠异步轮询任务的退避重试才最终成功（日志行 115–118 失败，187/193 才 ready）。
说明**引导早期（boot_late 任务上下文）`msleep` 因定时器/调度器尚未完全就绪而不精确**，
导致 `uhci_root_port_reset()` 的端口复位 100/50/30ms 与枚举重试 20ms 稳定延时**形同虚设**，
设备尚未从复位恢复 → SET_ADDRESS 被 NAK/超时。

而 step101 当时同步枚举走的是 **busy-wait**（`g_can_sleep=false`，纯 `inb` 延时，不依赖
时钟），所以能确定性等待设备稳定、首次即成功。step102 文档自身也保留：
> 「引导早期 `uhci_init()` 的 20/20/5/10ms 复位仍为忙等（发生在 kmain、无其它任务，影响可忽略）。」

## 2. 修复

文件：`kernel/drivers/usb/usb_core.c::usb_init()`

在 `uhci_init()` 成功、置 `g_hc_ready=true` 之后、**创建轮询任务之前**，重新加入同步
首次枚举，但做关键改进 —— **同步阶段临时 busy-wait（一次性、无竞争、确定性），枚举完成
后恢复 msleep 轮询（不忙等，满足 step102 要求）**：

```c
    g_hc_ready = true;

    /* 同步首次枚举：在驱动优先阶段（任何 Ring3 服务之前）确定性地完成首次枚举，
     * 彻底消除异步轮询任务「首次运行时机不确定」带来的偶发『整个 UHCI 总线枚举
     * 失败』——表现为键鼠同时失灵（鼠标不动时键盘也不可用）：二者挂在同一控制器，
     * 枚举失败后整体回退 PS/2，而系统仅有 USB 输入设备（step101 根因）。
     * 同步阶段临时关闭「可睡眠」走忙等（启动期一次性、无其它任务竞争，影响可忽略；
     * step102 同样保留引导期 uhci_init 的忙等），以完全复现 step101 确定性成功的
     * 时序——引导早期 msleep 因定时器尚未就绪可能不精确，会导致端口复位/稳定延时
     * 失效、SET_ADDRESS 偶发失败。枚举完成后由下面的轮询任务恢复 msleep 真睡眠
     * （不忙等，step102 要求）。 */
    g_sleep_ok = false;
    uhci_set_can_sleep(false);
    for (int i = 0; i < 12; i++) {
        usb_poll();
    }
    g_sleep_ok = true;
    uhci_set_can_sleep(true);

    task_t *t = task_create_kernel(usb_service_task, NULL, "SukiUsbHost");
```

要点：
- **同步枚举确定性完成**：`usb_init()` 在驱动优先阶段（任何 Ring3 服务之前）串行跑
  12 轮 `usb_poll()`，覆盖根端口与 Hub 下行端口（鼠标经 `usb-hub` 的下行端口需在 Hub
  枚举后由 `usb_hub_poll` 推进，多轮循环保证覆盖）。
- **同步阶段 busy-wait**：`g_sleep_ok=false` + `uhci_set_can_sleep(false)` 使端口复位
  100/50/30ms、枚举重试 20ms 全部走 `usb_udelay`（纯 `inb` 延时），**不依赖尚未就绪的
  定时器**，设备必然稳定后再发 SET_ADDRESS → 首次即成功（实测 0 次 `enum failed`）。
- **轮询任务仍不忙等**：同步枚举结束后恢复 `g_sleep_ok=true`/`uhci_set_can_sleep(true)`，
  后续 `usb_service_task` 的 `msleep(8)` 周期轮询 + HID 中断读取维持 step102 的「不忙等」
  语义，不与显示/输入/磁盘服务争抢 CPU。
- 原有的 `g_root_retry[8]` 失败退避逻辑（step101 §2.2）保留，作为热插拔/偶发失败的兜底。

## 3. 验证（QEMU，仅 bash + QEMU 自带机制）

镜像：`make iso`（最新 `build/kernel.ski` 已链接，`SukiOS.iso` 已生成）。

拓扑 A（最严格，含 Hub，鼠标在 Hub 下行，需多轮枚举）：
```bash
qemu-system-x86_64 -machine pc -cpu Skylake-Client -smp 1 -m 2G -no-shutdown \
  -display none -serial file:/tmp/xN.log -boot d -cdrom build/SukiOS.iso \
  -device piix3-usb-uhci -device usb-kbd -device usb-hub -device usb-mouse
```
拓扑 B（根端口直连，无 Hub）：将上面 `-device usb-hub -device usb-mouse` 换为
`-device usb-kbd -device usb-mouse`。

**连续 4 次（A×3 + B×1）结果一致**：
```
[usb]     device addr=1 FS vid=0627 pid=0001 class=3 ...   <-- keyboard
[usb-hid] keyboard ready (addr=1 ep=81 mps=8)
[usb]     device addr=2 FS vid=0409 pid=55aa class=9 ...   <-- hub
[usb]     device addr=3 FS vid=0627 pid=0001 class=3 ...   <-- mouse (Hub 下行)
[usb-hid] mouse ready (addr=3 ep=81 mps=4)
[usb]     host service started (UHCI, polling)             <-- 同步枚举已结束
[boot]    display-server ready (...)                       <-- 晚于 USB 枚举
```
- **`enum failed` 次数 = 0**（4/4 次）；无 `SET_ADDRESS failed`；
- USB 枚举稳定**早于** `display-server ready`（115–122 行 vs 171 行）；
- 4 次日志均**无 panic / 无 triple fault / 无 EXC halted**；
- 对比此前 msleep 版本的失败表现（115–118 行 `SET_ADDRESS failed` + `enum failed`，
  依赖异步重试才在 187/193 行勉强 ready），busy-wait 同步枚举彻底消除非确定性。

## 4. 关键文件/常量

| 项 | 位置 |
|---|---|
| 同步首次枚举（busy-wait 时序） | `kernel/drivers/usb/usb_core.c::usb_init()`（`g_sleep_ok=false; uhci_set_can_sleep(false); for(12) usb_poll();` 再恢复） |
| 端口复位时序（100/50/30ms） | `kernel/drivers/usb/uhci.c::uhci_root_port_reset()`（`usb_delay_ms` → busy-wait） |
| 失败退避兜底 | `kernel/drivers/usb/uhci.c::uhci_poll()` + `g_root_retry[8]`（64 轮 ≈ 512ms） |
| 轻量轮询（不忙等） | `kernel/drivers/usb/usb_core.c::usb_service_task()`（`usb_poll(); msleep(8);`） |
| 输入源优先级仲裁 | `kernel/drivers/usb/usb_core.c::usb_poll()` 末尾 `input_pref_set_usb_kbd/mouse()` |

## 5. 与历史步骤的关系

- **step101**：首次引入「同步首次枚举」修复「USB 键鼠长时间无响应/等很久才正常」。
  本步重新应用该修复（因上一步 `git revert be08eac` 误删）。
- **step102**：引入「USB 全部长延时改 msleep 真睡眠让出 CPU（不忙等）」+ 优先级抢占调度。
  本步**保留**其「轮询任务不忙等」语义，但将**启动期同步枚举**改回 busy-wait，
  以规避引导早期 `msleep` 不精确导致的首枚举失败（step102 自身亦保留启动期 `uhci_init`
  忙等，与此一致）。
- **step110**：上一步（be08eac）创建的文档已被 `git revert` 删除，本步以 step111 续号，
  不与其同名冲突。

## 6. 修改文件清单
- `kernel/drivers/usb/usb_core.c`：`usb_init()` 重新加入同步首次枚举，同步阶段临时
  `g_sleep_ok=false`/`uhci_set_can_sleep(false)`，结束后恢复（轮询任务不忙等）。

## 7. 已知/后续
- 真实鼠标手感/窗口表现仍需图形界面人工实测（headless 无法注入真实鼠标移动）。
  若人工测试仍发现移动方向/灵敏度问题，请按 step 文档中的 `USB_MOUSE_INVERT_Y`
  （`kernel/drivers/usb/usb_hid.c`）与报告长度逻辑微调，与本修复（枚举确定性）无关。
- 若日后要严格「零忙等」启动，需先确保 boot_late 阶段定时器/调度器已就绪再改用
  `msleep`；当前采用 step102 认可的「启动期一次性 busy-wait，影响可忽略」方案。
