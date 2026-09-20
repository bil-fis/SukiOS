# step100 — 驱动优先加载 + 统一由设备/驱动管理器管理；PCI/PCIe 能力完善

用户诉求：

> 「现在连窗口都不显示了。但是使用 PS/2 就正常。现在请你检查，如果可能，让驱动最先加载，
> 加载完毕后再进入系统后续流程的加载。所有驱动应该由设备管理器和驱动管理器管理。
> 如果可能，按照 osdev 实现 pci 和 pcie 驱动。」

本轮完成三件事：**(1) 驱动优先加载阶段**；**(2) 全部内置驱动统一交由 device/driver manager 管理**；
**(3) 依 osdev 完善 PCI/PCIe 能力探测**。q

---

## 0. 结论速览

| 事项 | 结果 |
|---|---|
| 复现「USB 无窗口」 | 非必现。曾观察到 `display-server did NOT become ready`（无窗口），确认为**时序竞争**而非崩溃 |
| 根因 | USB 主机栈与 display/input 服务**并发**初始化，单核下互相拖慢；某些时序下 display-server 迟迟不就绪 |
| 修复 | 新增【驱动优先阶段】：所有驱动在任何 Ring3 服务之前，由 device/driver manager 匹配 + probe 完成初始化 |
| 驱动统一管理 | AHCI/ATA/HDA/e1000/UHCI 均登记为 `driver_t`，经 `driver_register()`→双向匹配→`probe()` 绑定 |
| PCI/PCIe | 已有 ECAM(MCFG)+PIO 回退、能力链表、MSI/MSI-X；本轮新增 **PCIe 能力探测**（cap 0x10 + 设备/端口类型，osdev PCI Express） |
| 验证 | USB+hub 场景连续 3 次：`display-server ready` 稳定（0 → 3/3），窗口创建、USB 早枚举、零 panic、PASS=105 |

---

## 1. 复现与定位

**复现**：`-device piix3-usb-uhci -device usb-kbd -device usb-hub -device usb-mouse` 连跑 3 次，
观察到 1 次 `[boot] WARN: display-server did NOT become ready ... (UI may be degraded)` + 无窗口，
其余 2 次正常 → **非确定性时序问题**（用户所报"连窗口都不显示"）。

**排除误判**：一度出现"第二次运行日志为空"，实为前一个 QEMU 未退出、共用 cdrom 导致第二个未启动——
改为「等前一个 `timeout` 结束再跑」后样本有效。

**根因**：单核下，`usb_init()` 创建的 `SukiUsbHost` 轮询任务在 kmain 阶段启动即开始枚举（控制传输 +
端口复位含延时），与随后 `boot_late_init` 里并发创建的 `display_server/input_server` 竞争 CPU。
时序稍偏时 display-server 未在引导屏障（`kmain.c` 的 20000 轮 `task_yield` 等待 `g_display_active`）
内完成初始化 → 窗口层未就绪 → 只画外框/无窗口。PS/2 路径不创建 USB 主机任务，故"正常"。

---

## 2. 修复：驱动优先加载阶段（Driver-First Phase）

### 2.1 新增 `kernel/driver/builtin_drivers.c`
把内建 PCI 驱动封装为 `driver_t`（与 `.kdr` 模块共享同一 ABI），每个驱动给出：
- `match()`：按 PCI class/subclass 判定可驱动的设备（ATA 01/01、AHCI 01/06、HDA 04/03、e1000 02/00、UHCI 0C/03）；
- `probe()`：真正执行硬件初始化（复用既有 `ata_init/ahci_init/hda_init/e1000_init/usb_init`，首次成功后幂等，避免同类多设备重复初始化）；
- `builtin_drivers_register()`：依次 `driver_register()`，每次触发 driver_manager 的「双向匹配」→ `probe()`；
- `drivers_disk_ready()`：供 late-init 判定磁盘是否就绪（替代原直接看 `ata_init()` 返回值）。

USB 相关代码以 `#if CONFIG_DRIVER_USB` 门控，保证关闭该配置时不会引用 `usb_init` 造成缺符号。

### 2.2 `kernel/kmain.c` 时序调整
- 移除 kmain 中分散的 `usb_init()`（原在 syscall/ipc 之前）、`hda_init()`、`e1000_init()` 调用；
- 在 **`posix_init()` 之后、（创建任何 Ring3 服务之前）** 新增「驱动优先阶段」：
  ```c
  builtin_drivers_register();   /* 注册 ata/ahci/hda/e1000/uhci → 匹配 → probe */
  task_create_kernel(console_srv, ...);
  ```
  此刻内核核心（调度/IPC/POSIX）已就绪、`device_manager_scan_pci()` 已把 PCI 功能入册，故 probe 可安全初始化硬件（含创建内核服务任务），且**先于** console/net/disk/display/input/shell 等后续流程；
- `boot_late_init` 中删除 `ata_init()/ahci_init()`，改为 `bool disk_ok = drivers_disk_ready();` 后仅 `disk_srv_start()`。

### 2.3 效果（QEMU 实测日志）
```
[device] registered #3 '8086:7010' cls=01/01
[device] registered #6 '8086:100e' cls=02/00
[device] registered #7 '8086:7020' cls=0c/03
...
[driver] registering built-in PCI drivers (ata/ahci/hda/e1000/uhci)...
[driver] registered 'ata-ide'
[device] '8086:7010' (id #3) bound to driver 'ata-ide'  [nbound=1]
[driver] registered 'ahci'
[driver] registered 'intel-hda'
[driver] registered 'e1000'
[device] '8086:100e' (id #6) bound to driver 'e1000'  [nbound=1]
[driver] registered 'uhci-usb'
[device] '8086:7020' (id #7) bound to driver 'uhci-usb'  [nbound=1]
[driver] built-in drivers probed: disk=1 hda=0 e1000=1 usb=1
[usb-hid] keyboard ready ... / [usb-hub] ready ... / [usb-hid] mouse ready ...   <-- 早于 display
[boot] display-server ready (g_display_active=1, waited=2 yield rounds)
```
USB 枚举（`usb-hid` 行）现在**远早于** display-server，竞争窗口消除。

---

## 3. PCI/PCIe（按 osdev 完善）

**已有（本轮确认，见 `kernel/drivers/pci.c`）**：
- `pci_cfg_init()`：解析 ACPI **MCFG**，有则切换到 **ECAM**（内存映射配置空间，覆盖 PCIe 扩展配置 0..4095 字节），否则回退传统 PIO `0xCF8/0xCFC`；
- 能力链表遍历 `pci_find_cap()`、**MSI/MSI-X** 能力发现与编程（`pci_enable_msi`）、BAR 解析、INTx→GSI 路由（_PRT）。

**本轮新增（osdev "PCI Express"）**：
- `PCI_CAP_PCIE = 0x10`；
- `pci_is_pcie(dev)`：设备能力链表是否含 0x10；
- `pci_pcie_type(dev)`：读 PCIe Capabilities Register(cap+0x02)[7:4] 的设备/端口类型（1=Endpoint, 4=Root Port, …）；
- `device_mgr.c::pci_register_one()` 扫描时对 PCIe 设备打印 `^ PCIe device bb:dd.f type=N`。
  （`-machine pc`/i440FX 无 PCIe，故本轮日志无该行——符合预期；具备 MCFG 的 q35/PCIe 平台会命中。）

---

## 4. 验证

**USB（UHCI + hub + keyboard + mouse）连续 3 次**：
```
run1: disp_ready=1 warn=0 wm=2 usbenum=2 err=0 PASS=105
run2: disp_ready=1 warn=0 wm=2 usbenum=2 err=0 PASS=105
run3: disp_ready=1 warn=0 wm=2 usbenum=2 err=0 PASS=105
```
- `display-server ready` **3/3 稳定**（修复前偶发 0），`did NOT become ready` 警告 **0 次**；
- 均创建 WM 窗口；含 `[shell] font service spawned` / `[fontsrv] font service online` / `SukiOS:/>` 提示符；
- **零 panic / EXC / halted**；`PASS=105 FAIL=0`。

**命令**：
```bash
make iso disk
qemu-system-x86_64 -machine pc -cpu Skylake-Client -smp 1 -m 2G -no-shutdown \
  -display none -serial file:/tmp/x.log -boot d -cdrom build/SukiOS.iso \
  -drive file=build/disk.img,format=raw,index=0,media=disk \
  -device piix3-usb-uhci -device usb-kbd -device usb-hub -device usb-mouse
grep -aE "\[driver\]|bound to driver|display-server ready|did NOT become ready|SukiOS:/>|panic" /tmp/x.log
```

---

## 5. 关键文件/常量

| 项 | 位置 |
|---|---|
| 驱动优先阶段入口 | `kernel/driver/builtin_drivers.c::builtin_drivers_register()` / `drivers_disk_ready()` |
| 驱动框架 ABI | `include/sukios/kdr.h`（`driver_t` / `device_t`） |
| 驱动管理器 | `kernel/driver/driver_mgr.c`（`driver_register` 触发双向匹配） |
| 设备管理器 | `kernel/driver/device_mgr.c`（`device_register` / `device_manager_scan_pci`） |
| PCI/PCIe 访问 | `kernel/drivers/pci.c` + `include/kernel/pci.h`（ECAM/PIO、能力链表、MSI/X、**PCIe cap 0x10**） |
| 引导顺序调整 | `kernel/kmain.c`（`posix_init()` 后 `builtin_drivers_register()`；boot_late 用 `drivers_disk_ready()`） |

## 6. 修改文件清单
- 新增 `kernel/driver/builtin_drivers.c`
- `kernel/kmain.c`：移除分散 usb/hda/e1000 初始化与 boot_late 的 ata/ahci；新增驱动优先阶段
- `include/kernel/pci.h` + `kernel/drivers/pci.c`：新增 PCIe 能力（`PCI_CAP_PCIE`、`pci_is_pcie`、`pci_pcie_type`）
- `kernel/driver/device_mgr.c`：扫描时标注 PCIe 设备/端口类型

## 7. 已知/后续
- 「USB 启动整体仍偶有较慢」的根因（大文件读 IPC 往返）已在 step99 修复；本轮进一步消除驱动与服务并发竞争，display 稳定性显著提升。
- 真实鼠标手感/图形窗口表现需在**图形界面**实测（headless `-display none` 无法注入真实鼠标移动）。
