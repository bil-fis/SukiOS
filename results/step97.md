# step97 —— 输入源优先级：USB 键鼠优先，PS/2 自动回退

## 0. 需求
- 确保 USB 鼠标 / 键盘可用，并**优先使用 USB**；
- USB 缺失或异常时**自动回退 PS/2**；
- 确保 **QEMU 启动命令也优先使用 USB**（默认即挂 USB 键鼠）。

## 1. 现状与冲突
USB HID 与 PS/2 最终都汇入**同一内核输入缓冲**：
- PS/2：IRQ1/IRQ12 → `kbd_irq_handler`（keyboard.c）→ `kbd_feed_byte` / `mouse_feed_byte`；
- USB：`usb_hid` 把 HID 报告转译为 PS/2 事件后，同样调用 `kbd_feed_byte` / `mouse_feed_byte`。

两者同时在场若无仲裁，会**双份输入**（同一按键被 PS/2 与 USB 各注入一次）。

## 2. 方案：输入源优先级仲裁
新增 `include/kernel/input_pref.h` + `kernel/input/input_pref.c`：
```c
void input_pref_set_usb_kbd(bool);      void input_pref_set_usb_mouse(bool);
bool input_pref_usb_kbd_active(void);   bool input_pref_usb_mouse_active(void);
```
（两个 `volatile bool`，单字原子，无锁。）

**在 PS/2 分发点丢弃字节**（`kernel/arch/x86_64/keyboard.c` 的 `kbd_irq_handler`）：
```c
if (st & STS_AUX) {                 /* 鼠标 */
    if (!input_pref_usb_mouse_active()) mouse_feed_byte(sc);
} else {                            /* 键盘 */
    if (!input_pref_usb_kbd_active())   kbd_feed_byte(sc);
}
```
- USB 就绪 → 丢弃对应 PS/2 字节（USB 独占，无双份输入）；
- USB 失效 → 标志清零 → 字节恢复放行（自动回退）。

## 3. 优先级状态由 USB 核心维护（`usb_core.c`）
每次 `usb_poll()` 末尾按**设备表**重算：
```c
bool kbd=false, mouse=false;
for each dev: if (state==ENUMERATED && int_slot!=INVALID) {
    if (kind==HID_KBD) kbd=true; else if (kind==HID_MOUSE) mouse=true;
}
input_pref_set_usb_kbd(kbd); input_pref_set_usb_mouse(mouse);
```
同时把 HID 轮询改为返回错误码：`usb_hid_poll()` 返回 `-1`（端点致命错误）时，
`usb_core` 调 `usb_core_free(i)` 释放设备 → 设备表更新 → 下次重算即回退 PS/2。
（`include/kernel/usb_hid.h` 同步改签名。）

覆盖的“异常”情形：枚举失败、中断端点 STALL/CRC/DBE/Babble、设备拔出、`CONFIG_DRIVER_USB=0`。

## 4. QEMU 默认优先 USB（`Makefile`）
```make
QEMU_INPUT  ?= -usb -device usb-kbd -device usb-mouse
QEMU_FLAGS  := ... -no-shutdown $(QEMU_INPUT)
```
并入 `QEMU_FLAGS`，故 `make run` / `run-headless` / `run-single-iso` / `run-uefi` 等**默认即挂 USB 键鼠**。
如需纯 PS/2：`make run QEMU_INPUT=`。

## 5. 验证（QEMU，串口落盘 + monitor 注入）
| 场景 | 命令要点 | 结果 |
|---|---|---|
| A 挂 USB 键鼠 | `-usb -device usb-kbd -device usb-mouse` + `sendkey a/b` | USB HID 处理 2 次按键；PS/2 未收到（QEMU 有 USB 键盘时 `sendkey` 只投递给 USB 键盘 → 天然无双份输入）；`[usb-hid] keyboard/mouse ready` |
| B 无 USB | 不带 `-usb` + `sendkey a/b` | PS/2 放行 4 字节（a/b 各 make+break）；`[usb] no host controller, USB stack disabled` → 正确回退 |
| C 仅 USB 鼠标 | `-usb -device usb-mouse` + `sendkey a` | `[usb-hid] mouse ready`；PS/2 键盘放行 2 字节（按设备独立，鼠标不影响键盘） |
| 回归 | 带/不带 USB 各一次 | **零 panic**；`make -n run` 已含 `usb-kbd/usb-mouse` |

（验证用临时日志 `[ps2-dbg]`/`[usb-hid-dbg]` 已在提交前移除。）

## 6. 涉及文件
| 文件 | 改动 |
|---|---|
| `include/kernel/input_pref.h` | 新增：仲裁接口 |
| `kernel/input/input_pref.c` | 新增：两个标志的实现 |
| `kernel/arch/x86_64/keyboard.c` | PS/2 分发按仲裁丢弃/放行 |
| `kernel/drivers/usb/usb_core.c` | 末尾重算优先级；HID 错误释放设备 |
| `kernel/drivers/usb/usb_hid.c` | `usb_hid_poll` 返回 int（-1=致命错误） |
| `include/kernel/usb_hid.h` | 签名同步 |
| `Makefile` | `QEMU_INPUT`（USB 键鼠）并入 `QEMU_FLAGS` |

## 7. 提交
`feat(input): USB 键鼠优先、PS/2 自动回退；QEMU 默认启动挂 USB 键鼠`

## 8. 说明
- QEMU 在同时存在 PS/2 与 USB 键盘时，`sendkey` 仅投递给 USB 键盘，故“PS/2 丢弃”分支
  在 QEMU 中难以直接触发；该分支在真机（PS/2 与 USB 同时产生字节）下生效。已通过
  “按设备独立”用例（仅 USB 鼠标时 PS/2 键盘仍放行）间接验证仲裁逻辑。
- 该机制与既有的“USB↔PS/2 同构注入”正交：注入缓冲不变，仅在 PS/2 入口按优先级取舍。
