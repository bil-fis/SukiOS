# SukiOS 完整功能总结 + Step11–27 演进摘要

日期：2026-07-30
范围：截至 step27 的操作系统完整功能清点，以及对 step11→step27 全部工作的阶段总结。
依据：本地 `results/stepNN.md` 历史文档、`include/` 头文件、`kernel/`、`user/` 源码、以及
      《SukiOS 全栈技术参考手册.md》（v1.0）。

---

# 第一部分：操作系统完整功能（截至 step27）

## 1. 系统定位与架构

- **架构**：x86_64（LP64），目标物理机（现阶段用 QEMU `i440fx`/`q35` 验证）。
- **内核模型**：混合内核（Mach 消息传递 + BSD 抽象）。Ring0 驻留调度器 / VMM / PMM /
  Mach IPC 端口路由 + ATA/AHCI 磁盘裸读写（响应 `DISK_PORT` IPC）；Ring3 用户态服务
  含 `FS_SERVER`(FAT32)、`INPUT_SERVER`、`SHELL` 等，所有驱动为用户态进程经 `mach_msg` 通信。
- **内存布局**：内核虚拟基址 `KERNEL_BASE=0xFFFF800000000000`；内核物理加载 `0x100000`(1MB)；
  用户态空间 `0x0 ~ 0x00007FFFFFFFFFFF`；4 级分页（PML4→PDP→PD→PT），4KB 页，
  引导页表映射低 4GB（含 MMIO，ECAM `0xB0000000` 可直映射）。
- **启动协议**：Multiboot2（GRUB），兼容 Legacy BIOS 与 UEFI(OVMF)；`_start` 物理入口 `0x100000`，
  `boot.S` 完成 CPUID→PAE→临时三级分页→跳高地址→销毁恒等映射，并探测 SMAP、布署栈金丝雀。

## 2. 启动与初始化顺序（`kernel/kmain.c`）

```
serial_init → multiboot2_parse → fb_init/fbcon_init/console_init → draw_boot_logo
→ gdt_init → fpu_init → idt_init → pmm_init → vmm_init → kheap_init
→ security_init → gdbstub_init → acpi_init(RSDP/XSDT/MADT/HPET/MCFG/_PRT)
→ pci_cfg_init(ECAM 或 PIO 回退) → lapic_init → ioapic_init
→ clock_init(TSC 校准 + LAPIC 100Hz + HPET 探测) → smp_init(唤醒 AP)
→ sched_init → keyboard_init → syscall_init → ipc_init → hda_init
→ ahci_init / ata_init → spawn fs-server(有盘时) → spawn input-server → spawn shell
→ idle 任务 hlt 等待中断（task0）
```

## 3. 内存管理（`kernel/mm/`）

- **PMM**（`pmm.c`）：位图管理器，每 bit = 4KB 页，仅管 `0x100000` 以上；引用计数（共享页零拷贝）；
  饱和粘滞 + 共享页守卫（`mm_selftest`/`pmm_selftest` 压测）。
- **VMM**（`vmm.c`）：4 级页表映射；`PHYS_TO_VIRT` 直映射；`vmm_map_page` 重复映射感知；
  COW fork、按需分页、`KPTI` 双页表（CR3 bit12 标记用户页表，防 Meltdown）。
- **kmalloc / kheap**（`kmalloc.c`）：内核堆，基址经 `KASLR-lite` 随机化。
- **VMA**（`vma.c`）：进程地址空间对象，`vma_populate` 缺页救援、`vma_find_free` 选址、
  `vma_unmap_range` 拆分；支撑 `sys_mmap`/`sys_munmap`、COW、栈自动增长。
- **kstack**（`kstack.c`）：每任务内核栈 + **守卫页**（越界即触发 #PF 诊断）。
- **栈金丝雀**（`stack_canary.c`）：内核态与用户态栈 canary，熵重播种。

## 4. CPU / 中断 / 异常（`kernel/arch/x86_64/`）

- **GDT / IDT**（`gdt.c`/`idt.c`/`isr.S`）：per-CPU GDT/TSS；IDT 装载异常(0‑31)+IRQ+IPI+
  **MSI 向量池(isr48..isr127)**。
- **FPU/SSE**（`fpu_init`）：开启 FPU/SSE 状态保存（FXSAVE/FXRSTOR）。
- **中断控制器**：LAPIC（100Hz 节拍 + IPI）+ I/O APIC（GSI 路由）；legacy 8259 PIC/PIT 保留。
- **高精度时钟**（`clock.c`）：TSC 校准 + LAPIC 定时器 100Hz + HPET 探测。
- **panic 诊断**（`diagnostics.c`）：寄存器转储 + 栈回溯；`WARN_ON`/`BUG_ON` 宏 + `diag_selftest`。
- **异常守卫**：`#DF=IST1`、`NMI=IST2` 守卫页 IST 栈，避免双重故障致命化。
- **klog**（`klog.c`）：早期日志环缓冲，可经 GDB 重放。

## 5. 对称多核（SMP，`kernel/arch/x86_64/smp.c`）

- 依 MADT 唤醒 AP（`INIT-SIPI-SIPI`），AP 跳板 `ap_boot.S`（物理 `0x8000`）。
- per-CPU 数据（`percpu.h`/`percpu.c`，`gs_base` 槽）；ticket 自旋锁（`spinlock.h`）。
- IPI：`RESCHED`（调度）+ `GDB_FREEZE`（调试冻结其它核）。
- **对称多核调度 + work-stealing 负载均衡**（`sched.c`）：`steal_task()` 从繁忙核偷任务、
  `pick_next()` 排除 idle、每核负载观测、`sched_balance_report()` 启动快照；全核安全位一致。

## 6. 进程与调度（`kernel/sched/`、`include/kernel/task.h`）

- `task_struct`：PID、状态、上下文（rsp/rip/cr3）、时间片、优先级(0‑255)、IPC reply 端口、cwd 簇号。
- 就绪队列 + 优先级 + 时间片；`sched_tick`（100Hz 节拍钩子，由 `sched.c` 强覆盖 `pit.c` 弱实现）。
- 上下文切换 `switch.S`；`sys_task_spawn`/`sys_execve`/`sys_wait`/`sys_task_exit`/`sys_yield`。

## 7. IPC（Mach 风格，`kernel/ipc/port.c`）

- 全局端口表 `kernel_port_t`；`mach_msg` 小消息(<4KB)内核 memcpy 转发，
  大消息（像素缓冲）经 **OOL 物理页重映射零拷贝**（传页框号，引用计数 +1，接收窗口上界校验）。
- 端口耗尽语义（0 号哨兵保留）、队列长度上限（背压）、接收侧防御性校验、所有权转移、
  `sys_port_claim` 认领 recv 权。

## 8. 系统调用（`kernel/syscall/syscall.c`，`syscall` 指令 / `IA32_LSTAR`）

约定 `%rax=调用号`，`%rdi/%rsi/%rdx/%r10/%r8/%r9=参数`（System V AMD64）。

| 号 | 名称 | 功能 |
|----|------|------|
| 0 | `SYS_MACH_MSG` | 发送/接收 IPC（阻塞/非阻塞） |
| 1 | `SYS_TASK_SPAWN` | 新建 Ring3 任务并装载 ELF |
| 2 | `SYS_TASK_EXIT` | 终止当前进程 |
| 3 | `SYS_YIELD` | 主动让出 CPU |
| 4 | `SYS_DEBUG_WRITE` | 调试输出(buf,len)，走 `copy_from_user`，长日志分块 |
| 5 | `SYS_INPUT_READ` | 取原始键盘扫描码（INPUT_SERVER 专用） |
| 6 | `SYS_REBOOT` | 8042 复位重启 |
| 7 | `SYS_PORT_CLAIM` | 用户态认领端口 recv 权 |
| 8 | `SYS_EXECVE` | 加载并执行 ELF（替换当前进程） |
| 9 | `SYS_WAIT` | 阻塞等待子任务退出并返回退出码 |
| 10 | `SYS_AUDIO_OPEN` | 打开 HDA 输出流(rate,channels,bits) |
| 11 | `SYS_AUDIO_WRITE` | 写 PCM(buf,len)，返回接受字节数 |
| 12 | `SYS_AUDIO_QUEUED` | 返回环中未播放字节数 |
| 13 | `SYS_AUDIO_STOP` | 停止并复位输出流 |
| 14 | `SYS_MMAP` | 匿名映射 mmap(len,prot)，返回基址 |
| 15 | `SYS_MUNMAP` | munmap(addr,len) |

> `copy_from_user`/`copy_to_user` 全程 `SMAP` 硬件围栏，严禁直接解引用用户指针。

## 9. 文件系统（`user/fs_server.c`，Ring3）

- FAT32 解析经 Mach IPC（`FS_SERVER` 端口 `0x1001`）：READ / LIST_DIR。
- 健壮性：簇链环检测、簇号数据区范围校验、LFN 序号伪造/越界校验、路径解析深度显式报错、
  `disk_read` 应答扇区数校验。
- `disk.img` 含 README / HELLO / ROADMAP / MOONHALO.MP3，启动自测列出验证。

## 10. 设备驱动（`kernel/drivers/`）

- **ATA PIO**（`ata.c`）：LBA48 容量 + 48 位 PIO 寻址；读路径越界防护。
- **AHCI SATA**（`ahci.c`）：中断驱动 DMA 取代 PIO；两页对齐的 DMA 命令/接收结构、ABAR 寄存器、
  H2D FIS；`ahci_exec` 中断 + 轮询双保险；**COMRESET 错误恢复**（`ahci_port_reset` + 重试闭环）；
  **MSI 中断路由**（失败回退 IOAPIC）；统一经 `blk_read`/`blk_write` 分发。
- **PCI**（`pci.c`）：`pci_cfg_init` 命中 MCFG 切 **ECAM MMIO**，否则 PIO `0xCF8/0xCFC` 回退；
  `ecam_ptr`/`ecam_usable` 守卫；`pci_find_cap` 能力链表越界防御；
  `pci_enable_msi`（MSI-X 优先 / MSI 64 位）编程；`acpi_pci_route` `_PRT` INTx→GSI 路由。
- **HDA 音频**（`hda.c`）：PCM 播放、环形缓冲；流所有权准入、connection list 上限 + 超时、
  BDL 分配失败回滚、verb 忙等让出 CPU。
- **键盘**（`keyboard.c`）：扫描码 → IRQ1 → I/O APIC → INPUT_SERVER 解析后广播。
- **帧缓冲 / 控制台**（`framebuffer.c`/`fbcon.c`/`console.c`/`font.h`/`vga_text.c`）：
  MB2/GOP 帧缓冲、`draw_boot_logo`、VGA 文本模式回退。

## 11. ACPI（`kernel/acpi/acpi.c`）

- RSDP 三级定位（EBDA / BIOS 区域 / MB2 tag14/15 副本）；XSDT 遍历。
- 解析：MADT（CPU/APIC/IOAPIC）、HPET、MCFG（ECAM 窗口）、FADT（`_S5` 软关机）、
  DSDT `_PRT`（INTx→GSI 路由）、DSDT `_S5` 最小化 AML 解析器。
- **电源**：`SYS_REBOOT`（8042 脉冲）+ `acpi_poweroff`（S5）经 `syscall.c`/Shell `poweroff`。

## 12. 用户态服务与应用（`user/`）

- `FS_SERVER`(0x1001)、`INPUT_SERVER`(0x1003)、`SHELL`。
- 应用：`apps/hello.c`（mmap/COW 端到端验证）、`apps/audiotest.c`、`apps/playaudio.c`。
- 用户态 `lib/`（`suki.c`/`suki.h`）：`u_utoa_s` 长度安全接口、syscall 包装（含 `suki_syscall5`
  寄存器 clobber 修复）。

## 13. 安全加固（`kernel/arch/x86_64/security.c`）

- **UMIP / NXE / SMEP / SMAP** 启动期终审；**KASLR-lite**（堆基址随机化）；
- **KPTI** 双页表（Meltdown 免疫检测 + CR3 bit12 标签）；
- 内核/用户态栈金丝雀 + 内核栈守卫页；**可执行栈修复**（blob 补 `.note.GNU-stack`，GNU_STACK=RW）；
- IST 守卫栈（#DF/NMI）；`copy_*_user` SMAP 围栏；特权指令内联汇编隔离为 `noinline`。

## 14. 调试与可诊断性

- 串口（COM1 日志）、klog 早期环缓冲、WARN_ON/BUG_ON、panic 寄存器转储 + 栈回溯。
- **GDB stub（`gdbstub.c`）**：RSP over COM2，支持寄存器打包/内存读写（绕过 SMAP、W^X），
  `IPI_GDB_FREEZE` 冻结多核；`.gdbinit` 接 `build/kernel.elf` @ `0xFFFF800000100000`。

---

# 第二部分：Step11–27 阶段总结

> 说明：step22 为「P0 生产就绪审计清单 / 残差项台账」，非功能实现步，单列于后。

## Step11 — 审计 H1：ELF 栈参数数组边界加固
- `elf_build_stack()` 三重防护（auxv 数组越界）；`include/kernel/elf.h` 新增共享常量；
  `syscall.c` 消除独立硬编码。QEMU 冒烟回归通过。

## Step12 — 生产安全审计批次二（H2/H3/H4/H6/D2）
- **H2** auxv 数组边界、**H3** 端口耗尽语义（`ipc_init` 保留 0 号哨兵 + 耗尽诊断）、
  **H4** OOL 接收窗口上界（`ool_map_into_receiver` 上界检查 + 错误码）、
  **H6** HDA 音频流所有权（`hda_pcm_open` 准入 + write/queued/stop 保护）、
  **D2** shell `exec` argv 数组清零 + 运行时实证根因定位。

## Step13 — 中风险审计批次（M1–M18）
- `kfree` 双重释放防护、pmm 引用计数溢出、vmm_map_page 重复映射感知、地址空间销毁与 OOL 共享页
  引用统一、端口队列长度上限(背压)、mach_msg 接收侧防御校验、任务数硬上限 + 内核栈溢出守卫、
  exec_copy_args 失败回滚、ELF 头整数回绕 / phnum 上限、ATA 读越界、HDA verb 忙等让出、
  HDA BDL 失败回滚、FAT32 簇链环检测、簇号范围校验、路径解析截断/深度报错、disk_read 扇区数校验、
  `u_utoa` 长度上界、`kprintf` 重入深度保护。

## Step14 — M2 回收路径审计收尾 + `u_utoa_s` 长度安全接口
- pmm 饱和粘滞 + 共享页守卫；全内核 8 处 `pmm_free_page` 调用点审计合法；新增 `pmm_selftest` OOL 压测；
  `u_utoa_s` 长度安全接口（22 处调用迁移到 `sizeof(缓冲)`），旧接口处置。

## Step15 — 低风险审计 L1–L8
- L1 `sys_debug_write` 长日志分块；L2 ATA LBA48 容量 + 48 位 PIO；L3 启用 SMAP + `copy_*_user` 围栏；
  L4 MB2 头 / LMA 构建期校验；L5 内核/用户态栈金丝雀；L6 HDA connection list 上限 + 超时；
  L7 LFN 序号伪造 / 越界校验；L8 `KSTACK_SIZE` 重命名 + 注释澄清。

## Step16 — P0 地基一阶段（ACPI / APIC / 高精度时钟 / panic 诊断）
- **P0-1** ACPI 表解析（RSDP/XSDT/MADT/HPET）、**P0-2** LAPIC+IOAPIC 取代 8259 PIC/PIT、
  **P0-4** 高精度时钟（TSC 校准 + LAPIC 100Hz + HPET）、**P0-9** panic 诊断（寄存器转储 + 栈回溯）。

## Step17 — P0-3 SMP 多核支持
- AP 启动 + per-CPU + ticket 自旋锁 + IPI；跳板 `ap_boot.S`(物理 `0x8000`)、INIT-SIPI-SIPI、
  per-CPU GDT/TSS、IPI 向量与处理、AP 生命周期；顺带修复 3 个存量 bug；QEMU `-smp 4` 验证。

## Step18 — P0-5 虚拟内存进阶（VMA + 按需分页 + COW + mmap/munmap）
- `vm_area_t`、缺页救援 `vma_populate`、COW fork/break、`munmap` 拆分、`mmap` 选址、
  `copy_*_user` 协同、栈自动增长；`vma_selftest` + Ring3 `hello.c` 端到端 + 回归。

## Step19 — P0-7 AHCI SATA 驱动（中断驱动 DMA 取代 ATA PIO）
- 两页对齐物理 DMA 结构、ABAR 寄存器、H2D FIS；`ahci_init` / `ahci_exec`（中断 + 轮询双保险）/
  FLUSH / 统一分发 `ata.c::blk_read|blk_write`；AHCI 路径 + IDE PIO 回归通过。

## Step20 — P0-8 内核安全地基
- 修复 **BSP `EFER.NXE` 从未开启**（致命缺陷）；新增 `security.c`：UMIP 使能、NXE/SMEP/SMAP 终审、
  守卫页 IST 栈(#DF=IST1,NMI=IST2)、Meltdown 免疫检测 → KPTI 结论、特权指令隔离；
  `kmalloc.c` 堆基址 KASLR-lite、`stack_canary.c` 熵重播种、gdt/idt/kmain 接线。

## Step21 — P0-6 UEFI 启动路径（OVMF + GRUB-EFI + GOP + MB2 RSDP 通路）
- 内核近乎零改动；`multiboot2.h/c`、RSDP 三级定位、`grub.cfg` GOP 帧缓冲关键修复、Makefile OVMF 目标；
  OVMF 与 SeaBIOS 双回归通过。

## Step22 — P0 生产就绪审计清单 / 残差项台账（非功能步）
- 汇总 R1(对称多核调度) / R3(可诊断性) / R5(栈守卫) / R6(MSI) / R7(syscall clobber) /
  R8(ACPI 关机/_PRT) 等清单与状态，作为后续 step23–27 的工作台账。

## Step23 — KPTI/SMP 三重故障根因定位与修复（CR3 bit12 语义冲突）
- 根因：PML4 表 8KB 对齐缺失导致 KPTI CR3 bit12 标签冲突引发三重故障；
- 修复：`p4_table` 改 8KB 对齐（根修复）、`isr.S`/`syscall_entry.S` 清位加 `g_kpti_enabled` 门控、
  `vmm_init` 防回归断言；确立 KPTI CR3 bit12 完整不变量；无盘/带盘全流程验证。

## Step24 — P0-R3 可诊断性 + R5 内核栈守卫页 + 回溯上界回绕修复
- **R3a** klog 早期日志环缓冲（无锁、单生产者、GDB 重放）；**R3b** `WARN_ON`/`BUG_ON` + `diag_selftest`；
  **R3c** 串口 GDB stub（RSP over COM2，安全内存读写，IPI 接入）；**R5** 内核栈守卫页(`kstack.c`)；
  `IPI_GDB_FREEZE`；修复 `kernel_addr()` 上界无符号回绕历史 bug。

## Step25 — P0-R1 对称多核调度 + 全核安全位统一
- **work-stealing 负载均衡**：`steal_task()`、`schedule()` 集成、`pick_next()` 排除 idle、
  每核负载观测、`sched_balance_report()` 启动快照；修复 **BSP `percpu[0].online` 漏置**；
  全核安全位(UMIP/NXE/SMEP/SMAP)一致性确认；`-smp 4` 验证通过。

## Step26 — P0-R8 ACPI S5 软关机 + R7 用户态 syscall clobber 修复
- **R8** FADT + DSDT `_S5` 完整解析（关键 FADT 偏移修正）+ 最小化 AML 解析器 + `acpi_poweroff()` +
  syscall/Shell `poweroff` 接线；**R7** 修复 `suki_syscall5` 寄存器 clobber 缺失（真实 bug，
  会导致调用约定破坏）；reboot 回归（"shell online" 出现 2 次）通过。

## Step27 — P0 收尾：MCFG/ECAM · _PRT 路由 · MSI/MSI-X · AHCI 错误恢复 + 生产稳定性安全审计
- 功能代码就位并验证：MCFG/ECAM 窗口、`_PRT` INTx→GSI 路由、PCI MSI/MSI-X、AHCI COMRESET 恢复、
  MSI 向量池(isr48..127)。
- **审计发现并修复 1 个严重正确性缺陷 + 5 个构建期缺陷**：
  - **A(严重)** MCFG 分配结构起始偏移误用 36，应为 **44**（含 8 字节保留）→ ECAM 此前从未真正启用
    （"ECAM enabled" 是假象，静默回退 PIO）；修复后 q35 显示 `ECAM enabled: base=0xb0000000 bus=0..255`。
  - **B** 未用参数 `bus`（`(void)bus`）；**C** `sched_tick` 隐式声明（`clock.h` 补原型）；
    **D** `acpi_poweroff` 隐式声明（`syscall.c` 补 include）；**E** `acpi_find_table` stringop-overread
    （形参改 `const char *sig`）；**F** 内核可执行栈（`blob` 补 `.note.GNU-stack`，GNU_STACK=RW）。
- 构建**零 warning/零 error**；AML 解析带长度守卫、AHCI 重试为有限 TSC 忙等（不持锁、不依赖调度器）、
  错误路径真正闭环。

---

# 第三部分：验证状态与成熟度

## 构建
- `make clean && make iso disk` 退出 0，**零 warning / 零 error**（step27 已消除历史 6 条告警）。

## QEMU 三场景生产回归（step27）
| 场景 | 命令要点 | 关键日志 | 结果 |
|------|----------|----------|------|
| i440fx 默认（IDE PIO） | `-machine pc` + IDE 盘 | no MCFG→PIO；ATA primary master OK 64MiB；FAT32 挂载 6 文件；shell online | 零 panic ✅ |
| i440fx + AHCI(MSI) | pc + `-device ahci` | MSI enabled 00:04.0→vec64；AHCI IRQ via MSI→vec64；wr-test PASS, irqs=8 | 零 panic ✅ |
| q35 + AHCI(ECAM+MSI) | q35 + `-device ahci` | MCFG 1 ECAM window；ECAM enabled base=0xb0000000 bus=0..255；MSI 00:03.0→vec64；wr-test PASS | 零 panic ✅ |

- 通用断言：MSI 向量池装载、`smp 4/4 CPUs online` + IPI selftest `3/3 APs acked`、
  FAT32 经 Mach IPC 正常、无 `panic`/`triple fault`/`#PF kernel`/`assert`/`fatal`。
- UEFI(OVMF) 路径 step21 已独立验证；KPTI 三重故障 step23 修复后全流程通过。

## 功能成熟度结论
- **P0 全部残差项已闭环**：R1 对称多核调度(+work-stealing)、R3 可诊断性(klog/WARN_ON/gdbstub)、
  R5 栈守卫页、R6 MSI/MSI-X、R7 syscall clobber、R8 ACPI S5 关机 + _PRT 路由，以及
  MCFG/ECAM、AHCI 错误恢复、生产稳定性审计（含 MCFG 偏移严重缺陷修复）。
- **生产稳定性铁律达标**：边界/越界全卫士、错误闭环、无死锁、外部输入不信任、特权操作合规、零 panic。
- **待真实固件/物理机补充验证**：含 `_PRT` 的真实 DSDT（OVMF/物理机）下 INTx→GSI 端到端；
  MSI-X 多向量（目前单向量 64）；ECAM 写后回读校验（可选加固）。

---

# 第四部分：改动文件清单（step11–27 汇总）

**内核核心**
- `kernel/kmain.c`：初始化顺序、EFI/RSDP 接线、服务启动。
- `kernel/elf/elf.c` + `include/kernel/elf.h`：栈参数/auxv 边界、共享常量。
- `kernel/ipc/port.c` + `include/ipc/port.h`：端口耗尽、OOL 上界、队列背压、接收校验。
- `kernel/mm/pmm.c` + `include/mm/pmm.h`：引用计数/饱和、共享页守卫、`pmm_selftest`。
- `kernel/mm/vmm.c` + `vma.c` + `kstack.c` + `kmalloc.c`：按需分页/COW/mmap、栈守卫页、KASLR-lite。
- `kernel/sched/sched.c` + `switch.S`：work-stealing、对称多核调度。
- `kernel/arch/x86_64/`：`smp.c`/`ap_boot.S`/`percpu`/`gdt.c`/`idt.c`/`isr.S`/`security.c`/
  `clock.c`/`fpu`/`boot.S`/`multiboot2.c`/`syscall_entry.S`：SMP、GDT/TSS/IST、安全位、KPTI、时钟。
- `kernel/acpi/acpi.c` + `include/kernel/acpi.h`：ACPI 全表、MCFG(偏移44修正)、_PRT、_S5。
- `kernel/drivers/`：`pci.c`(ECAM/MSI/_PRT)、`ahci.c`(DMA+恢复+MSI)、`ata.c`(LBA48)、
  `hda.c`(所有权/超时)、`keyboard.c`。
- `kernel/syscall/syscall.c` + `include/kernel/syscall.h`：16 系统调用、copy_*_user、隐式声明修复。
- `kernel/diagnostics.c` + `klog.c` + `gdbstub.c`：诊断宏、环缓冲、GDB stub。

**用户态**
- `user/fs_server.c`：FAT32 + 簇链/范围/LFN/路径 健壮性。
- `user/input_server.c`、`user/shell.c`（exec argv 清零 + poweroff）。
- `user/apps/`：`hello.c`(mmap/COW)、`audiotest.c`、`playaudio.c`。
- `user/lib/`：`suki.c`/`suki.h`（`u_utoa_s` 长度安全、syscall 包装 clobber 修复）。

**构建/配置**
- `Makefile`：blob 补 `.note.GNU-stack`（可执行栈修复）、OVMF 目标、QEMU 标志。
- `grub/grub.cfg`：GOP 帧缓冲修复。
- `include/kernel/`：`elf.h`/`task.h`/`pmm.h`/`vma.h`/`spinlock.h`/`percpu.h`/`security.h`/
  `gdt.h`/`smp.h`/`clock.h`/`acpi.h`/`pci.h`/`syscall.h` 等全面扩展。

---

*本文件为 step11→step27 的阶段总结，与 `results/stepNN.md` 各分文档互为补充；
更细的技术细节（数据结构、算法、关键地址/常量、调试命令）请查阅对应 step 文档。*
