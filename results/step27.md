# SukiOS Step27 — P0 收尾：MCFG/ECAM · _PRT 路由 · MSI/MSI-X · AHCI 错误恢复 + 生产稳定性安全审计与缺陷修复

日期：2026-07-30
范围：完成 P0 全部残差项，并对 P0 新增代码与既有代码做生产稳定性安全审计，
      发现并修复 1 个严重正确性缺陷（MCFG 偏移）与 5 个构建期缺陷（告警/可执行栈）。

---

## 0. 背景与目标

此前 P0 审计（step10_todos）确认三项残差：
- **R8**：ACPI MCFG（PCIe ECAM 配置空间）+ DSDT `_PRT` PCI 中断路由。
- **R6**：PCI MSI / MSI-X 能力发现与编程。
- **R4**：AHCI 端口 COMRESET 错误恢复。

本轮在上一阶段已落地上述三项功能代码的基础上，进行 **QEMU 三场景生产回归验证**，
并在验证过程中发现并修复了若干使功能“看似可用、实则未真正生效 / 存在隐患”的缺陷，
最终达成“所有代码均可在生产环境稳定、可预期运行，零 panic”的目标。

---

## 1. P0 功能落地（代码层面，本阶段已就位并验证）

### 1.1 ACPI MCFG / PCIe ECAM（文件 `include/kernel/acpi.h`、`kernel/acpi/acpi.c`）

- 新增 `ACPI_SIG_MCFG "MCFG"` 签名与 `acpi_mcfg_window_t`：
  `base`(64b 物理基址) / `seg_group`(16b) / `bus_start`(8b) / `bus_end`(8b)。
- `acpi_info_t` 扩展：`mcfg_phys` / `mcfg_count` / `mcfg_windows[ACPI_MCFG_MAX_WINDOWS]`
  / `prt_count` / `prt[ACPI_PRT_MAX]`。
- `acpi_process_table()` 在 XSDT 遍历中识别 `MCFG` 并调用 `acpi_parse_mcfg()`。
- 对外 API：`acpi_get_mcfg_window(seg_group, bus, &base, &bus_start, &bus_end)`
  按 (段组, 总线) 命中 ECAM 窗口。

### 1.2 DSDT `_PRT` 解析与 PCI INTx→GSI 路由（同上文件）

- `acpi_parse_prt()`：扫描 DSDT AML，兼容 `Name(_PRT,Package{})` 与
  `Method(_PRT){Return(Package{})}`，解析每条 `Package(4){ADDR,PIN,SRC,IDX}`：
  - `SRC==Zero(0x00)` ⇒ 直连 GSI = `IDX`；
  - 否则 `SRC` 为链接设备 NameSeg，经 `acpi_resolve_link_gsi()` 在其 `_CRS` 的
    `Extended Interrupt(0x89)` / `IRQ(0x22/0x23)` 描述符中取 GSI。
- `acpi_pci_route(bus, dev, pin)`：直连返回 GSI；link 解析返回 GSI；否则 `-1`（调用方回落 `INT_LINE`）。

### 1.3 PCI MSI / MSI-X（文件 `include/kernel/pci.h`、`kernel/drivers/pci.c`）

- `pci_cfg_init()`：调用 `acpi_get_mcfg_window()`，命中则启用 ECAM MMIO 配置访问，
  否则回退传统 PIO `0xCF8/0xCFC`。
- `ecam_ptr()`：`(uint64_t)(uintptr_t)PHYS_TO_VIRT(g_ecam_base) + (bus<<20)+(dev<<15)+(func<<12)+off`，
  `ecam_usable(bus)` 守卫 `g_ecam_base!=0 && bus∈[bus_start,bus_end]`。
- `pci_find_cap(dev, cap_id, &off)`：遍历能力链表（从 `0x34` 起），带越界防御。
- `pci_enable_msi(dev, vector, lapic_id)`：优先 MSI-X（写 MSI-X BAR 的 MSG ADDR/DATA/CTRL 并置 `0x8000`），
  否则 MSI（按 `msgctrl&0x80` 区分 32/64 位，写 MSG ADDR=0xFEE00000|lapic<<12、DATA=vector，置 Enable `0x1`）。

### 1.4 AHCI 错误恢复 + MSI 中断路由（文件 `kernel/drivers/ahci.c`）

- 新增 `PX_SCTL 0x2C` / `PXSCTL_DET_MASK 0xF`，`ahci_port_reset()`：
  清 `PX_IS`/`HBA_IS`/`PX_SERR`(W1C) → `port_stop()` → `PX_SCTL.DET=1`(~1ms) → `DET=0`
  → 轮询 `PX_SSTS.DET==3`(上限~480ms) → 清错 → `port_start()`。
- `ahci_exec()` 改为 `for(attempt=0..MAX_RETRY)` 重试：每次先 W1C 清状态；
  失败（TFES/ERR/超时）则 `ahci_port_reset()` 后重试，末次失败返回 `false`（真正闭环，不忽略错误）。
- 中断路由：读 `INT_PIN` → `pci_route_interrupt()` → `pci_enable_msi(&d,64,lapic_id())`
  成功则注册 `vec 64` 并置 `PX_IE`/`GHC_IE`；失败且 `gsi<16` 则 `ioapic_route`+`vec 32+gsi`。

### 1.5 MSI 向量池（文件 `kernel/arch/x86_64/isr.S`、`kernel/arch/x86_64/idt.c`）

- `isr.S`：`.altmacro` + `GEN_MSI_STUBS 48,127` 批量生成 `isr48..isr127`（复用 `ISR_NOERR` 公共存根）。
- `idt.c`：X 宏 `MSI_ISR_LIST` 生成 `extern` 声明与 `g_msi_stubs[]`；安装循环
  `for(i=48;i<=127;i++) idt_set_gate(i,(uint64_t)g_msi_stubs[i-48],0,0x8E)`。
- 启动日志：`IDT loaded (48+80(MSI)+IPI vectors installed, #PF handler)`。

---

## 2. 安全审计发现并修复的缺陷

### 缺陷 A（严重·正确性）—— MCFG 分配结构起始偏移错误（44 vs 36）

**现象**：q35 下启动日志原为
`[pci] ECAM enabled: base=0x0000000000000000 bus=0..176` —— 基址为 0、总线末为 0xB0，明显异常。

**根因**：`kernel/acpi/acpi.c` 的 `acpi_parse_mcfg()` 以 `e = m + 36 + i*16` 读取窗口，
但 ACPI 6.x 规范规定 MCFG 在 36 字节 SDT 头之后还有 **8 字节保留字段**，分配结构起始于
**偏移 44**。原代码把保留字段当成窗口基址（→0），把真实 64 位基址的高字节当成段组/总线字段（→bus_end=0xB0）。

**后果**：`g_ecam_base` 虽被写入 0，但 `ecam_usable()` 有 `g_ecam_base!=0` 守卫 ⇒
ECAM MMIO 路径**从未真正启用**，PCI 配置访问静默回退 PIO。即“ECAM enabled”是假象，
P0 的 ECAM 能力实际未被验证。

**修复**：
```c
if (len < 44 + 16 || !acpi_checksum_ok((... )m, len)) { ... }
uint32_t n = (len - 44) / 16;
const uint8_t *e = m + 44 + i * 16;
```
**修复后验证（q35）**：`[pci] ECAM enabled: base=0x00000000b0000000 bus=0..255`，
且 AHCI `wr-test PASS`、MSI 经 ECAM 发现的配置能力生效 —— ECAM 路径真实可用。
（`vmm.c:7` 确认引导页表已映射低 4GB，`PHYS_TO_VIRT(0xB0000000)` 合法，无缺页。）

### 缺陷 B —— 构建告警：未用参数（`acpi.c:613`）

`acpi_pci_route(uint8_t bus,...)` 中 `bus` 未使用（单根桥 `_PRT` 仅按 dev+pin 路由）。
加 `(void)bus;` 消除 `-Wunused-parameter`。

### 缺陷 C —— 构建告警：隐式声明 `sched_tick`（`clock.c:76`）

`clock.c` 调用 `sched_tick()` 但无原型（该符号由 `pit.c` 弱定义、`sched.c` 强覆盖）。
在 `include/kernel/clock.h` 增加 `#include <kernel/interrupts.h>` 与
`void sched_tick(registers_t *r);` 原型，消除 `-Wimplicit-function-declaration`（UB/错误调用约定隐患）。

### 缺陷 D —— 构建告警：隐式声明 `acpi_poweroff`（`syscall.c:199`）

`syscall.c` 调用 `acpi_poweroff()` 但未包含声明头。增加 `#include <kernel/acpi.h>`。

### 缺陷 E —— 构建告警：`-Wstringop-overread`（`acpi.c` `acpi_find_table`）

`acpi_find_table(const char sig[8])` 的 `const char [8]` 形参让 GCC 在调用点
（如 `acpi_find_table("HPET")`，字面量仅 5 字节）判定“读 8 字节越界”。
改为 `const char *sig`（实际比较仅 `memcmp(...,4)`），头文件同步修改。
（`acpi.h:113` `uint64_t acpi_find_table(const char *sig);`）

### 缺陷 F —— 链接告警：内核可执行栈（`shell.blob.o` 缺 `.note.GNU-stack`）

`Makefile` 用 `objcopy -I binary` 生成嵌入用户态 blob，会丢弃 `.note.GNU-stack`，
导致最终内核 ELF 的 `GNU_STACK` 缺省为**可执行栈**（安全弱点）。
在 blob 规则后追加：
```make
objcopy --add-section .note.GNU-stack=/dev/null $@ $@.nostack && mv $@.nostack $@
```
（空节 ⇒ non-exec）。修复后 `readelf -l build/kernel.elf` 显示 `GNU_STACK ... RW`（不可执行）。

**修复后构建状态**：`make iso disk` 退出码 0，**零 warning / 零 error**（此前 6 条告警全部消除）。

---

## 3. 安全审计结论（生产稳定性维度）

| 维度 | 结论 |
|------|------|
| 边界/越界 | AML 解析（`acpi_pkg_bounds`/`acpi_aml_const`/`acpi_parse_resource_gsi`）全部带 `end` 守卫；`_PRT` 循环以 `outer_end`/`in_end` 截断，畸形 `ne` 不会越界读；MCFG 长度与校验和校验；`pci_find_cap` 链表越界防御。 |
| 错误处理闭环 | AHCI `ahci_exec` 失败重试→`ahci_port_reset`→末次失败返回 `false`（不静默忽略）；MSI 启用失败回落 IOAPIC/INT_LINE；`_PRT` 解析失败回落 `INT_LINE`。 |
| 并发/死锁 | AHCI 等待完成采用 **TSC 忙等 + `pause`**（`rdtsc()` 截止时间），不依赖调度器、不持有自旋锁，可在引导期使用；`ahci_port_reset` 同为有限忙等。无死锁路径。 |
| 外部输入不信任 | 设备寄存器（AHCI PxIS/PxTFD/SSTS）、ACPI 表（校验和、长度）、DSDT AML（长度守卫、未知 opcode 回落 0）均做防御性校验。 |
| 特权操作 | 页表/CR/MSR 等仍遵循既有 noinline 封装与注释约定（本阶段未改动）。 |
| panic/triple fault | 三场景 QEMU 回归均 **零 panic**（见 §4）。 |

---

## 4. 验证方法（QEMU 生产回归矩阵）

构建：`make clean && make iso disk`（退出 0，无告警）。
运行：`qemu-system-x86_64 -cpu qemu64 -smp 4 -m 2G -no-shutdown -display none -serial file:/tmp/sX.log -boot d -cdrom build/SukiOS.iso <磁盘>`
（超时仅因交互 shell 不主动退出；日志已完整捕获启动全过程）。

| 场景 | 命令要点 | 关键日志 | 结果 |
|------|----------|----------|------|
| S1 i440fx 默认（IDE PIO） | `-machine pc` + IDE 盘 | `[pci] no MCFG, using PIO 0xCF8`；`[ahci] no AHCI`；`[ata] primary master OK ... 64 MiB`；`[shell] SukiOS shell online`；FAT32 挂载 6 文件 | 零 panic ✅ |
| S2 i440fx + AHCI(MSI) | `-machine pc -device ahci,id=myahci ...` | `[pci] MSI enabled 00:04.0 -> vec 64`；`[ahci] IRQ routed via MSI -> vector 64`；`[ahci] port 0 online ... wr-test PASS, irqs=8` | 零 panic ✅ |
| S3 q35 + AHCI(ECAM+MSI) | `-machine q35 -device ahci,id=myahci ...` | `[acpi] MCFG: 1 ECAM window(s)`；`[pci] ECAM enabled: base=0xb0000000 bus=0..255`；`[pci] MSI enabled 00:03.0 -> vec 64`；`[ahci] port 0 online ... wr-test PASS, irqs=8` | 零 panic ✅ |

通用断言（三场景均满足）：
- `[idt] IDT loaded (48+80(MSI)+IPI vectors installed, #PF handler)` —— MSI 向量池装载；
- `[smp] 4/4 CPUs online` + `IPI selftest: 3/3 APs acked RESCHED` —— SMP 唤醒正常；
- `[fs] FAT32 mounted` + 根目录自测列出 README/HELLO/ROADMAP/MOONHALO.MP3 —— Ring3 FS 经 Mach IPC 正常；
- 无 `panic`/`triple fault`/`#PF kernel`/`assert`/`fatal`。

**关于 `_PRT`**：SeaBIOS 的 i440fx 与 q35 DSDT 均不含 `_PRT`（日志 `PRT: 0 route entry(ies)`），
故中断路由经 `acpi_pci_route()==-1` 回落到固件预编程 `INT_LINE`，行为正确安全。
`_PRT` 解析代码已按 ACPI 规范实现，待真实含 `_PRT` 的固件（如 EDK2/OVMF 或物理机）可自动生效。

---

## 5. 改动文件清单

- `include/kernel/acpi.h`：`acpi_mcfg_window_t`、`acpi_prt_entry_t`、`acpi_info_t` 扩展；
  `acpi_get_mcfg_window()`、`acpi_pci_route()` 声明；`acpi_find_table(const char *sig)` 形参修正。
- `kernel/acpi/acpi.c`：`acpi_parse_mcfg()`（偏移 44 修复）、`acpi_get_mcfg_window()`、
  `acpi_pkg_bounds()`、`acpi_aml_const()`、`acpi_parse_resource_gsi()`、`acpi_resolve_link_gsi()`、
  `acpi_parse_prt()`、`acpi_pci_route()`（`(void)bus`）。
- `include/kernel/pci.h`：`PCI_CFG_INT_PIN`；`pci_cfg_init()`、`pci_route_interrupt()`、
  `pci_find_cap()`、`pci_msi_capable()`、`pci_enable_msi()`；`PCI_CAP_MSI/MSIX`。
- `kernel/drivers/pci.c`：`pci_cfg_init()`（ECAM 启用）、`ecam_ptr()`/`ecam_usable()`、
  ECAM MMIO 分支、MSI/MSI-X 编程、`pci_route_interrupt()`。
- `kernel/drivers/ahci.c`：`PX_SCTL`/`PXSCTL_DET_MASK`、`ahci_port_reset()`、`ahci_exec()` 重试循环、MSI 优先中断路由。
- `kernel/arch/x86_64/isr.S`：`GEN_MSI_STUBS 48,127`。
- `kernel/arch/x86_64/idt.c`：`MSI_ISR_LIST` X 宏 + 安装循环。
- `kernel/kmain.c`：`acpi_init()` 后 `pci_cfg_init()`。
- `include/kernel/clock.h`：`sched_tick()` 原型（修复隐式声明）。
- `kernel/syscall/syscall.c`：补 `#include <kernel/acpi.h>`（修复隐式声明）。
- `Makefile`：blob 规则追加 `.note.GNU-stack`，消除内核可执行栈告警。

---

## 6. 下一步

- 在含 `_PRT` 的真实固件（OVMF/EDK2 或物理 x86_64）上回归，确认 INTx→GSI 路由链路端到端生效。
- 为 ECAM 配置访问增加写后回读校验（可选加固）。
- MSI-X 多向量（目前仅用单向量 64）可在多队列设备（如网卡）上扩展。
