# step95 —— USB 主机栈：UHCI 主机控制器 + USB Hub + HID 键鼠（即插即用）

## 0. 目标与依据
按本地 `osdev_wiki/`（`USB`、`USB_Hubs`、`USB_Human_Input_Devices`、`UHCI`）实现：
- **UHCI（USB 1.1）主机控制器**：PCI 探测、寄存器、帧列表、QH/TD、控制/中断传输、端口复位使能。
- **USB 核心**：端点 0 控制管道枚举、设备表、类分派、即插即用（热插拔）。
- **Hub 类驱动**：读描述符、端口供电、端口状态轮询、下游设备复位枚举。
- **HID boot 键盘/鼠标**：报告解析并转译注入既有输入栈。

## 1. 文件与结构
| 文件 | 作用 |
|---|---|
| `include/kernel/uhci.h` | UHCI 寄存器/位定义 + HC 驱动 API |
| `include/kernel/usb.h` | 描述符结构、标准请求、设备表、核心 API |
| `include/kernel/usb_hid.h` / `usb_hub.h` | 类驱动入口 |
| `kernel/drivers/usb/uhci.c` | UHCI 主机控制器（DMA/帧列表/TD/端口） |
| `kernel/drivers/usb/usb_core.c` | 枚举 + 设备表 + 类分派 + `SukiUsbHost` 服务任务 |
| `kernel/drivers/usb/usb_hid.c` | HID boot 键鼠 |
| `kernel/drivers/usb/usb_hub.c` | Hub 类 |

内核 `C_SRCS := $(shell find kernel -name '*.c')`（Makefile:141）**自动收集** `kernel/drivers/usb/*.c`，无需改 Makefile。

## 2. UHCI 关键实现（`uhci.c`）
### 2.1 初始化
- PCI 探测：class `0x0C` / subclass `0x03` / prog-if **精确 `0x00`**（`(pi&0x0F)==0` 会误配 EHCI/xHCI，低 4 位同为 0）。
- 禁用 BIOS legacy：向 PCI 配置 `0xC0` 写 `0x2000`；使能 I/O+BusMaster。
- **I/O 基址取 BAR**：Intel PIIX3 UHCI 的 I/O BAR 在 **BAR4（offset 0x20）**（`0xC041`→基址 `0xC040`），不可假定 BAR0。故扫描 BAR0..BAR5 取首个 bit0=1 者。
- 复位：HCRESET（bit1，自清）+ GRESET（bit2）；分配 4KB 对齐帧列表（1024 项，初值 `0x1` 终止）写 `FRBASEADD`；`SOFMOD=0x40`；`USBCMD = RS|CF|MAXP`（`0x00C1`）。
- 端口探测：`PORTSCn = 0x10+2*(n-1)`，bit7 恒 1 且值≠`0xFFFF` 为有效端口。

### 2.2 传输结构
- **TD（32B）**：`next/status/token/buffer` + 16B 系统区。
  - `status`（CS）：bit23 Active、bit24 IOC、bit26 LS、bit27-28 CERR、bit29 SPD、bit22 Stalled、bit19 NAK、bit10-0 ActualLen。
  - `token`：bit0-7 PID（IN `0x69`/OUT `0xE1`/SETUP `0x2D`）、bit8-14 dev、bit15-18 ep、bit19 toggle、bit21-31 MaxLen=(n-1)。
  - `next`：bit0 终止、bit2 Depth、bit1 类型。
- **控制传输**：`SETUP(toggle0) → DATA(toggle1,0,1...) → STATUS(反向, toggle1, 0B)`，TD 间以 Depth 链；带重试（NAK 重发，STALL/CRC/DBE/Babble 失败），超时 `500×200us`。
- **中断 IN**：单 TD，完成后读 ActualLen、翻转 toggle 重新激活（UHCI 不自动翻转 toggle）。

### 2.3 关键修复：帧表用「直接 TD 调度链」而非 QH
- 现象（QEMU 8.2.2）：控制器 RS 已置位、FRNUM 递增、`FRBASEADD`/帧项/QH 物理地址回读全部正确，但 TD 的 Active 位**从不被清除**（TD 从未执行）。
- 实验：把帧表项**直接指向控制 TD 链**（`frame entry -> TD`，USB1.1 允许）后，控制传输立即成功（`td0=0x18000007` 实际传输 8B）。
- 结论：QEMU UHCI 对 QH 路径的处理与本实现约定不一致；改为**不使用 QH**，帧表项指向 TD 链：
  - `uhci_rebuild_chain()`：把各活动中断槽的 TD 用 `next|DEPTH` 串成链，帧表 1024 项全指向链首（无槽位则 `TERM`）。
  - 控制传输：临时把 1024 帧项指向控制 TD 链，执行后 `uhci_rebuild_chain()` 恢复中断链。
- 中断多槽链：控制器会「找第一个 Active 的 TD」，非活动 TD 被跳过，故多 HID 设备可共链。

### 2.4 端口复位使能时序（OSDev）
`SetReset → 100ms → ClearReset → 50ms → SetPED → 等待 PED（至多 100ms）→ 稳定 30ms`；随后读 `LSDA(bit8)` 判低速。

## 3. USB 核心（`usb_core.c`）
- 枚举：地址 0 读 8B 设备描述符（重试 12×20ms，容忍热插拔早期）→ `SET_ADDRESS`（分配唯一地址，5ms 恢复）→ 全设备描述符 → 配置描述符（9B 头 + 全量 ≤480）→ 解析首个接口类/子类/协议与首个中断 IN 端点 → `SET_CONFIGURATION` → 类分派。
- 设备表 `usb_device_t g_dev[16]`，含 address/low_speed/max_packet0/kind/父 hub/端口/中断槽/`int_mps`/`hub_ports` 等。
- 类分派：HID(3)→`usb_hid_start`；Hub(9)→`usb_hub_start`；其它→枚举但无驱动。
- 即插即用：`uhci_poll()` 根端口变化回调 `usb_core_root_connect/disconnect`；`usb_hub_poll()` 回调 `usb_core_hub_connect/disconnect`；断开递归回收下游。
- 服务任务 `SukiUsbHost`（`task_create_kernel`）：循环 `usb_poll(); usb_ms(8);`，在**进程上下文**执行（可安全使用复位/枚举延时），避免在中断里阻塞。

## 4. Hub 类（`usb_hub.c`）
- `GET_DESCRIPTOR(HUB,0x29)` 取 `bNbrPorts`（越界回退 4）。
- 每端口 `SET_FEATURE(PORT_POWER=8)` 供电，等待 100ms。
- 轮询（约每 50ms）：`GET_STATUS(Class/Other)` 取 `wPortStatus/wPortChange`；`C_PORT_CONNECTION` 则清该位并判连接：连接→`SET_FEATURE(PORT_RESET=4)` 保持 50ms→等 `PORT_RESET` 清除→清 `C_PORT_RESET`→读 `LOW_SPEED(bit9)`→`usb_core_hub_connect`；断开→`usb_core_hub_disconnect`。

## 5. HID 类（`usb_hid.c`）
- `usb_hid_start`：`SET_PROTOCOL(boot,0)`、`SET_IDLE(0)`（Class/Interface 请求），注册中断 IN（`uhci_int_add`）。
- 键盘（8B：修饰键+保留+6 键码）：按修饰键位与键码集合**差异比较**，HID usage→PS/2 set-1 扫描码（扩展键加 `0xE0`），make/break 经 `kbd_feed_byte` 注入既有键盘环形缓冲。
- 鼠标（按键+dx+dy，可选 wheel）：合成 PS/2 3/4 字节包（`0x08|buttons, dx, dy[, wheel]`）经 `mouse_feed_byte` 注入；包长与当前 PS/2 模式（`mouse_packet_len()` 3/4）一致。
- 复用既有输入栈：USB 键鼠与 PS/2 对 `INPUT_SERVER`/console 完全同构。

## 6. 配套改动
- `include/kernel/keyboard.h`：导出 `kbd_feed_byte`。
- `include/kernel/mouse.h` + `kernel/arch/x86_64/mouse.c`：导出 `mouse_feed_byte` 与新增 `mouse_packet_len`。
- `kernel/kmain.c`：调度器就绪后调用 `usb_init()`（由 `CONFIG_DRIVER_USB` 门控）。

## 7. 验证（QEMU，bash + serial 落盘，未用 GDB）
启动命令（示例）：
```
qemu-system-x86_64 -machine pc -cpu qemu64 -smp 1 -m 2G -no-shutdown \
  -display none -serial file:/tmp/a.log -boot d -cdrom build/SukiOS-single.iso \
  -usb -device usb-kbd -device usb-mouse
```
结果：
- 根端口键盘 `[uhci] ready I/O=0xC040 ports=2`、`device addr=1 vid=0627 pid=0001 class=3 iface=3/1/1`、`[usb-hid] keyboard ready`。
- 根端口 Hub `addr=2 vid=0409 pid=55aa class=9`、`[usb-hub] ready ports=8`；**Hub 下游鼠标** `addr=3 ... iface=3/1/2`、`[usb-hid] mouse ready`。
- 键盘事件（monitor `sendkey`）：`a→set1=0x1e`、`b→0x30`、`c→0x2e`、`Enter→0x1c`。
- 鼠标事件（monitor `mouse_move 20 10`）：`dx=20 dy=10`。
- **热插拔**：启动后 monitor `device_add usb-kbd` → 内核自动检测端口连接、复位枚举、`keyboard ready`。
- 带 USB 与不带 USB 两场景**零 panic**（唯一 `#PF` 命中为 IDT 描述行）。

## 8. 提交
`5f91dfd feat(usb): UHCI 主机控制器 + USB Hub + HID 键鼠驱动（即插即用）`

## 9. 已知限制
- QEMU UHCI 帧表使用「直接 TD 链」而非 QH（真机同样合法）；如需 QH 语义可后续补充。
- 低速设备经 USB2 Hub 需拆分事务（SSPLIT/CSPLIT）未实现；本例 Hub 下游为全速设备，工作正常。
