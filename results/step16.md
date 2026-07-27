# Step16：P0 地基第一阶段 —「离开古董机」(ACPI + APIC + 高精度时钟 + panic 诊断)

> 生成时间：2026-07-27
> 关联：`results/step10_todos.md` 路线图 P0 部分（P0-1/P0-2/P0-4/P0-9）。
> 目标：在不破坏现有可启动性的前提下，把内核从「8259 PIC + PIT + 无 ACPI 单核」推进到「ACPI 拓扑发现 + LAPIC/IOAPIC + TSC/LAPIC 高精度时钟 + panic 现场转储」，即路线图 M1 里程碑。
> 验证：`make iso disk && make run-headless`（QEMU i440FX / qemu64）冒烟通过——ACPI 表枚举正确、APIC 启用、100Hz 节拍由 LAPIC 定时器驱动、键盘经 IOAPIC、SMAP 仍 ENABLED、各 Ring3 服务在线、无 panic。

---

## 0. 本步范围与后续计划

P0 共 9 项，本步完成 **4 项（P0-1/P0-2/P0-4/P0-9）**，构成「离开古董机」地基。剩余：

| 项 | 状态 | 计划 |
|----|------|------|
| P0-1 ACPI 表解析 | ✅ 本步完成 | — |
| P0-2 LAPIC + IOAPIC + MSI | ✅ 本步完成（MSI 留待 P1 设备） | — |
| P0-3 SMP（AP 启动 + per-CPU + 锁体系） | ⏳ 待续 | 下一步 |
| P0-4 高精度时钟（TSC/HPET/LAPIC deadline） | ✅ 本步完成（LAPIC deadline 留待 SMP） | — |
| P0-5 按需分页 + COW + mmap | ⏳ 待续 | 第三步 |
| P0-6 UEFI 引导 | ⏳ 待续（与 BIOS 并存，风险高，靠后） | — |
| P0-7 AHCI 驱动 | ⏳ 待续 | 第四步 |
| P0-8 KASLR/KPTI/栈保护 | 🟡 栈保护已完成(step15 L5)；KASLR/KPTI 待续 | 第五步 |
| P0-9 panic 诊断 | ✅ 本步完成 | — |

---

## 1. P0-1：ACPI 表解析

**新增文件**：`kernel/acpi/acpi.c` + `include/kernel/acpi.h`

**设计**：
- **RSDP 定位**（规范 5.2.5）：先扫 EBDA（BDA 0x40:0x0E 段基址），再扫固定区 `0xE0000..0xFFFFF`，16 字节步长匹配 `"RSD PTR "` 签名并校验 8/20 字节校验和。直接内存扫描，不依赖 bootloader 是否提供 Multiboot2 ACPI 标签（GRUB/SeaBIOS 行为不一致）。
- **根表遍历**：ACPI 2.0+ 走 XSDT（64 位指针数组），ACPI 1.0（如 QEMU/SeaBIOS 默认）走 RSDT（32 位指针数组）。两者共用 `acpi_process_table()`，对每张表校验长度和校验和，记录 `MADT/FADT/HPET` 物理地址并打印签名。
- **MADT 解析**：枚举启用的本地 APIC（填 `g_acpi.madt_lapic_id[]` 与 `lapic_count`），定位第一个 I/O APIC 的 MMIO 基址与 GSI 基（`ioapic_phys`/`ioapic_gsi_base`）。同时取 MADT 给出的本地 APIC 寄存器基址填 `g_acpi.lapic_phys`（默认 0xFEE00000）。
- 全部经 `PHYS_TO_VIRT` 访问（0..4GB 已由引导页表覆盖），纯解析、不初始化设备。

**关键修复（本步踩坑）**：表签名仅 4 字节，原 `sig_eq` 用 `memcmp(...,8)` 比较 8 字节，导致 MADT/FADT/HPET 永远匹配失败（表现为「no IOAPIC in MADT」「HPET not present」）。改为 4 字节比较，RSDP 8 字节签名单独 `memcmp(...,8)`。修复后实测 MADT/HPET 正确枚举。

**QEMU 实测**：
```
[acpi] RSDT @ 0x...: 4 tables
  table[0]: 'FACP'  table[1]: 'APIC'  table[2]: 'HPET'  table[3]: 'WAET'
[acpi] MADT: 1 enabled LAPIC(s), IOAPIC @ 0xfec00000 (gsi_base=0)
[acpi] HPET table found @ 0x7ffe1bf8
```

---

## 2. P0-2：LAPIC + IOAPIC（取代 8259 PIC + PIT）

**新增文件**：`kernel/arch/x86_64/apic.c` + `include/kernel/apic.h`、`kernel/arch/x86_64/ioapic.c` + `include/kernel/ioapic.h`

### 2.1 LAPIC（本地 APIC）
- **使能**：`lapic_init()` 读 `MSR 0x1B`，置 `APIC_BASE_EN(bit11)`，写回；设 SVR（Spurious Vector Register）= `enable | 0xFF`（伪向量免 EOI）；屏蔽全部 LVT（LINT0/LINT1/ERROR/TIMER）清 BIOS 残留；写 EOI 清 pending。
- **EOI**：`lapic_eoi()` 写 LAPIC `EOI` 寄存器 0，对本地 LVT 与 IOAPIC 电平中断均有效。
- **MMIO 访问**：基址 `g_acpi.lapic_phys`（默认 0xFEE00000），经 `PHYS_TO_VIRT` 访问。注：真机要求 UC（PCD/PWT）属性，QEMU 下 WB 即可，后续 KPTI 重构时统一处理。
- **定时器**：`lapic_timer_calibrate(tsc_hz)` 用 TSC 作基准测出 LAPIC 计时频率（次/秒）；`lapic_timer_start(vec, hz)` 以周期模式启动，初始计数 = `counts_per_sec / hz`，向量 = `IRQ0(32)`。
- **特权指令隔离**：MSR 读写封装在 `__attribute__((noinline))` 函数并注释输入/输出/Clobber。

### 2.2 IOAPIC（I/O APIC）
- `ioapic_init()`：从 `g_acpi.ioapic_phys`（默认 0xFEC00000）取 MMIO 基址（ACPI MADT 已正确给出），读版本寄存器得 `max_redir`，**屏蔽全部 24 条重定向条目**防 BIOS 残留误触发。
- `ioapic_route(gsi, vector, level, active_low, dest)`：写重定向条目（低 32 含 vector/触发/极性/mask，高 32 含目标 LAPIC ID）。`ioapic_set_dest()` 设置默认目标（BSP）。键盘即 `ioapic_route(1, IRQ1, false, false, 0)`。

### 2.3 接线
- `kmain.c` 初始化顺序改为：`acpi_init() -> lapic_init() -> ioapic_init() -> ioapic_set_dest(bsp) -> pic_disable() -> clock_init()`，取代旧的 `pic_remap()/pit_init(100)`。
- `idt.c` `isr_dispatch()` 的 IRQ 分支改调 `lapic_eoi()`（替代 `pic_send_eoi`）。
- `keyboard.c` 改用 `ioapic_route()`（替代 `pic_clear_mask`）。
- `pic.c` 新增 `pic_disable()`：屏蔽 8259 双片数据端口，避免与 IOAPIC 双投递。

**QEMU 实测**：
```
[lapic] enabled: id=0 version=0x50014 base=0xfee00000
[ioapic] found @ 0xfec00000: id=0x0 version=0x170011 max_redir=23
[kbd] PS/2 keyboard ready (IOAPIC GSI1 -> IRQ1)
```

---

## 3. P0-4：高精度时钟

**新增文件**：`kernel/arch/x86_64/clock.c` + `include/kernel/clock.h`

**校准链路**（全程关中断、纯轮询，避免中断干扰）：
1. **TSC 频率**：`tsc_calibrate_via_pit()` 把 PIT 通道0 设为模式0(oneshot)、初值 0xFFFF，记录 `tsc0`；轮询 PIT 状态位 `OUT(bit7)` 直至计数到 0，记录 `tsc1`。`tsc_hz = (tsc1-tsc0) / (0x10000 / PIT_FREQ)`，并裁剪到 100MHz..10GHz 防失准。
2. **LAPIC 定时器频率**：`lapic_timer_calibrate(tsc_hz)` 启动一次性 LAPIC 定时器，用 TSC 测其下降固定窗口的真实时长，反推 `counts/sec`。
3. **系统节拍**：`lapic_timer_start(IRQ0, 100)` 以周期模式启动 100Hz 节拍，注册 `lapic_timer_handler -> sched_tick()`（与旧 `pit_irq_handler` 等价，复用 `sched_tick` 弱符号）。
4. **HPET**：从 ACPI HPET 表取基址记录（本步仅探测+日志，不作为节拍源；LAPIC 定时器已足够驱动调度，HPET 切换留待后续）。
- **单调时钟**：`clock_monotonic_ns()` = `rdtsc()*1e9 / tsc_hz`，供后续 `clock_gettime(CLOCK_MONOTONIC)` 接入。

**QEMU 实测**：
```
[clock] TSC freq = 2430596630 Hz (2430 MHz)
[lapic] timer calibrated: 1000269171 counts/sec
[lapic] timer started: 100 Hz (initial_count=10002691)
[clock] HPET detected @ 0xfed00000
[clock] high-precision clock ready (TSC + LAPIC timer 100Hz)
```

---

## 4. P0-9：panic 诊断（寄存器转储 + 栈回溯）

**新增文件**：`kernel/diagnostics.c` + `include/kernel/diagnostics.h`

- `diag_dump_registers(r)`：完整打印 GPR（rax..r15）、RIP/CS/RFLAGS/RSP/SS、ERR/INT、CR0/CR2/CR3（CR2/CR3 现从 msr 读出，旧实现缺 CR3）。
- `diag_dump_trace(rbp, rip)`：基于 x86_64 调用约定 `[rbp]=上一层rbp, [rbp+8]=返回地址`，沿 RBP 链向上最多 32 层打印返回地址（每层做内核地址范围校验，越界/非对齐即停，防回溯读坏栈引发三重故障）。返回地址可由 `objdump -d build/kernel.elf` 离线映射函数（符号化后续接）。
- `kernel_oops(msg, r)`：异常/内核缺页统一入口，打印消息+寄存器+回溯后 `cli;hlt`。
- `diag_dump_self()`：供 `panic()` 在缺寄存器帧时打印当前执行流回溯。
- **接线**：`idt.c` 的未处理 CPU 异常与内核态 `#PF` 改调 `kernel_oops()`（替代零散打印+`panic`）；`console.c` 的 `panic()` 末尾调 `diag_dump_self()`。

---

## 5. 构建与验证

### 5.1 命令
```
make iso disk          # 自动收集 kernel/ 下所有 .c（新文件无需改 Makefile）
make                   # 仅链接内核 + L4 校验
make run-headless      # QEMU 无头冒烟（i440FX / qemu64）
```

### 5.2 QEMU 关键验证点（实测全过）
| 项 | 期望 | 实测 |
|----|------|------|
| ACPI | RSDT 遍历出 FACP/APIC/HPET/WAET，MADT 枚举 LAPIC+IOAPIC | ✅ |
| LAPIC | id=0, SVR 使能, 基址 0xFEE00000 | ✅ |
| IOAPIC | 基址 0xFEC00000, max_redir=23, 键盘 GSI1→IRQ1 | ✅ |
| 时钟 | TSC 校准 ~2.4GHz, LAPIC 100Hz, HPET @0xFED00000 | ✅ |
| 节拍 | 调度由 LAPIC 定时器驱动（shell/fs 在线即证） | ✅ |
| SMAP | 仍 ENABLED（L3 未受影响） | ✅ |
| 服务 | fs-server/shell/input-server 在线, FAT32 挂载 | ✅ |
| 无 panic | 启动无 KPF/KERNEL OOPS | ✅ |

### 5.3 GDB 调试（按项目规则）
```bash
qemu-system-x86_64 -s -S -machine pc -cpu qemu64 -m 2G -cdrom build/SukiOS.iso
gdb -ex "target remote :1234" \
    -ex "break lapic_init" -ex "continue" \
    -ex "info registers"            # 验证 MSR 0x1B / CR8 / LAPIC 映射
```
验证栈回溯符号化：
```bash
objdump -d build/kernel.elf | grep -A2 "<kernel_oops>:"   # 将回溯地址映射到函数
```

---

## 6. 文件改动清单

| 文件 | 改动 |
|------|------|
| `include/kernel/acpi.h`（新） | ACPI 解析接口与 `acpi_info_t` 结果结构 |
| `kernel/acpi/acpi.c`（新） | RSDP 扫描 + XSDT/RSDT 遍历 + MADT/FADT/HPET 提取 |
| `include/kernel/apic.h`（新） | LAPIC 寄存器/标志与接口 |
| `kernel/arch/x86_64/apic.c`（新） | LAPIC 使能/EOI/定时器校准与启动（MSR 隔离） |
| `include/kernel/ioapic.h`（新） | IOAPIC 接口 |
| `kernel/arch/x86_64/ioapic.c`（新） | IOAPIC 初始化与 GSI 路由 |
| `include/kernel/clock.h`（新） | 时钟接口 |
| `kernel/arch/x86_64/clock.c`（新） | TSC/PIT 校准 + LAPIC 100Hz 节拍 + HPET 探测 |
| `include/kernel/diagnostics.h`（新） | 诊断接口 |
| `kernel/diagnostics.c`（新） | 寄存器转储 + RBP 栈回溯 + kernel_oops |
| `kernel/arch/x86_64/idt.c` | IRQ 分支改 `lapic_eoi()`；异常/内核#PF 改 `kernel_oops()` |
| `kernel/arch/x86_64/keyboard.c` | `pic_clear_mask` → `ioapic_route(GSI1→IRQ1)` |
| `kernel/arch/x86_64/pic.c` + `include/kernel/pic.h` | 新增 `pic_disable()` 屏蔽 8259 |
| `kernel/kmain.c` | 初始化顺序：`acpi→lapic→ioapic→pic_disable→clock` |
| `kernel/console.c` | `panic()` 末尾调 `diag_dump_self()` |

> 注：`kernel/arch/x86_64/pit.c` 仍编译但不再作为节拍源（仅其 PIT 端口在 clock.c 校准时短暂使用）。`kernel/stack_canary.c` 等 step15 内容不变。
