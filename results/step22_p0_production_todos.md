# P0 生产就绪对账 + 残留任务清单（step22）

> 生成时间：2026-07-27
> 目的：step11–step21 已把 P0-1~P0-9「功能」逐项跑通，但「功能跑通」≠「可在生产/物理机稳定运行」。
> 本文在 step10_todos 审计清单（H1–H8 / M1–M18 / L1–L8 / D1–D3）基础上重新核对**当前代码实际状态**，
> 对 9 个 P0 项重新评估「生产就绪度」，并给出为达到「安全稳定运行」仍需完成的残留任务（P0-R1…R8）。
> 审计方法：子代理全代码静态核对 + 亲自复核 `idt.c`/`boot.S`/`smp.c`/`security.c`。

---

## 一、审计项销账总表（相对 step10_todos）

| 项 | 原标注 | 当前状态 | 证据 |
|---|---|---|---|
| H1 | 立即修 | **FIXED** | `kernel/elf/elf.c:141,156` 统一 `ELF_ARG_MAX` + 边界拒入 |
| H2 | 立即修 | **FIXED** | `elf.c:202-219` `ELF_AUXV_MAX=16` + `na<MAX` 校验 |
| H3 | 立即修 | **FIXED** | `port.c:62-64` `PORT_NULL(0)` 置哨兵，耗尽即失败 |
| H4 | 立即修 | **FIXED** | `port.c:33,357-363` `OOL_RECV_LIMIT` + 空闲链表 |
| H5 | P0-9 | **PARTIAL** | `idt.c:114-118` 改走 `kernel_oops()` 转储，但无 gdbstub/环缓冲 |
| H6 | 立即修 | **FIXED** | `hda.c:127,633,715,790` 引入 `g_owner` 所有权校验 |
| H7 | P2-5 | **OPEN** | `hda.c:254,318` 超时 `0xFFFFFFFF` 与合法 GET_PARAM 值无专属哨兵 |
| H8 | P0-3 | **OPEN** | `syscall.c:577` `static kbuf[8192]` 依赖 syscall 串行，多核调度启用即竞争 |
| M1 | — | **FIXED** | `kmalloc.c:195-199` 双重释放防护 |
| M2 | P0-3 | **FIXED** | `pmm.c:38,154-303` 引用计数饱和 + 原子化 |
| M3 | — | **FIXED** | `vmm.c:95-107` 已映射先释放再覆盖 |
| M4 | — | **FIXED** | `vmm.c:261-269` 统一 `pmm_decref` |
| M5 | P1-8 | **FIXED** | `port.c:243-247` `PORT_QUEUE_MAX` 背压 |
| M6 | P1-8 | **FIXED** | `port.c:525-544` recv 侧 OOL size 重验 |
| M7 | — | **PARTIAL** | `sched.c:31,338-341` 任务上限 + 栈哨兵，**无未映射守卫页** |
| M8 | — | **FIXED** | `syscall.c:321-331` `fail:` 部分释放 |
| M9 | — | **FIXED** | `elf.c:85-97` `ELF_PHDR_MAX` + 回绕校验 |
| M10 | — | **PARTIAL** | `ata.c:185` 读越界已校验，**超时无控制器复位重试** |
| M11 | P2-5 | **FIXED** | `hda.c:281-283` 每 8192 次 `task_yield()` |
| M12 | P2-5 | **FIXED** | `hda.c:603-613` 中途失败回滚 |
| M13 | — | **PARTIAL** | `fs_server.c:27,348` 调用点 `FAT_WALK_LIMIT` 兜底，`fat_next` 自身无环检测 |
| M14 | — | **FIXED** | `fs_server.c:351` 数据区范围校验 |
| M15 | — | **FIXED** | `fs_server.c:404-434` 组件长度 + 深度限制 |
| M16 | — | **FIXED** | `fs_server.c:36-66` status 失败即不拷贝 |
| M17 | — | **FIXED** | `suki.c:115-142` `u_utoa_s(buf,size)` |
| M18 | P0-3/P0-9 | **FIXED** | `console.c:24,91-95` `g_kp_lock` 跨 CPU 串行 + 递归放弃 |
| L1 | — | **FIXED** | `syscall.c:164-182` 分块循环 |
| L2 | P0-7 | **FIXED** | `ata.c:133-143` 优先 LBA48 |
| L3 | P0-8 | **FIXED(仅 BSP)** | `boot.S:107-184` BSP SMAP/SMEP/NXE 真使能；**AP 路径遗漏** |
| L4 | — | **FIXED** | `Makefile:195-199` `readelf` 构建期断言 |
| L5 | P0-8 | **PARTIAL** | 内核/用户 `-fstack-protector-strong` 已上；**用户程序仍 `-fno-pie`（代码段固定）** |
| L6 | P2-5 | **FIXED** | `hda.c:391-399` conn list 钳制 64 |
| L7 | — | **FIXED** | `fs_server.c:253-323` 整段清零 + 序号连续性校验 |
| L8 | — | **FIXED** | `sched.c:27-44` 重命名 `KERNEL_STACK_BYTES` |
| D1 | P2-5 | **OPEN** | `hda.c:783-803` `hda_pcm_stop` 未清空 BDL 数据页 |
| D2 | 立即排查 | **FIXED** | 与 H1 一并修复 |
| D3 | — | **OPEN** | `Makefile:117-118` 未钉死 QEMU 采样率（仅 QEMU 可观测性） |

**结论**：原清单高/中风险绝大多数已销账（H1–H6、M1–M9、M11–M12、M14–M18、L1–L4、L6–L8、D2 = FIXED）；
残留 OPEN = **H7、H8、D1、D3 + AP 路径 SMAP/SMEP/UMIP 遗漏**；其余为 PARTIAL（已缓解但留生产缺口）。

---

## 二、9 个 P0 项「生产就绪度」重新评估

| P0 项 | 功能完成度 | 生产就绪度 | 关键缺口（不能安全稳定运行的根因） |
|---|---|---|---|
| **P0-1 ACPI** | 完成 | **中** | RSDP 三级定位 OK；但缺 FADT reset/S5 关机、多 IOAPIC/PCI _PRT 路由健壮性；物理机拓扑可能不全 |
| **P0-2 LAPIC/IOAPIC** | 基本完成 | **中高** | IOAPIC 路由 + LAPIC 定时器 OK；**MSI/MSI-X 未实现**（注释标「P0-2 收尾」），阻塞 NVMe/网卡等物理机驱动 |
| **P0-3 SMP** | 部分完成 | **低** | AP 仅 `for(;;)hlt` idle，**不对称调度**：AP 不跑 Ring3、不参与调度域；`port.c`/`sched.c` 仍 `cli/sti` 临界区（多核启用即失效）；**AP 未置 SMAP/SMEP/UMIP** |
| **P0-4 时钟** | 完成 | **中** | TSC/HPET/LAPIC deadline 已建；缺生产所需的「时钟源降级/校验」与 tickless 收敛验证 |
| **P0-5 VMA/按需分页/COW** | 完成 | **中高** | 用户态 VMA/COW/mmap 可用、execve 清空间；**内核栈无未映射守卫页（M7）**，溢出先踩 kmalloc 堆 |
| **P0-6 UEFI** | 完成 | **高** | BIOS/UEFI 双路径均点亮；Secure Boot 明确延后（可接受） |
| **P0-7 AHCI** | 完成 | **中** | 中断 DMA 读写 OK；**错误/介质健壮性弱**：TFD ERR / 坏块无恢复重试、无控制器复位（M10 仅覆盖 PIO） |
| **P0-8 安全地基** | 部分完成 | **低** | SMAP/SMEP/NXE/UMIP/IST 守卫页 + 堆滑移 KASLR 已上（BSP）；**AP 安全位遗漏**、**代码段无 KASLR（todo 写"内核基址随机化"未做）**、**KPTI 仅检测未部署** |
| **P0-9 诊断** | 部分完成 | **中低** | `kernel_oops` 现场转储 OK；**gdbstub / WARN-BUG 宏 / boot log 环缓冲 三项全无**，崩溃后难事后分析 |

---

## 三、残留任务清单（达到「生产安全稳定运行」必须完成）

> 排序依据：生产危害 × 当前 P0 宣称完成度（宣称完但实际上不行的，优先于明确留待 P1 的）。

### P0-R1 【关键】对称 SMP 调度 + 全核安全位统一（补全 P0-3 + L3/AP 缺陷 + H8）
- **对应原项**：P0-3（锁体系/对称调度）、L3（AP SMAP/SMEP）、H8（sys_audio_write 竞争）
- **现状**：AP 空转；BSP 单核调度；`port.c`/`sched.c` 用 `cli/sti` 而非 spinlock；AP 未置 CR4 安全位。
- **具体动作**：
  1. 调度器支持 per-CPU runqueue + 负载均衡，AP 真正运行 Ring3 任务（`ap_main` 改为 `sched_idle()` 接管）。
  2. `ap_main` 中按 BSP 同样探测并写入 `CR4.SMAP/SMEP/UMIP` 与 `EFER.NXE`（建议抽 `cpu_enable_security_features()` 共用）。
  3. 把 `port.c`/`sched.c` 的 `cli/sti` 临界区替换为 `spin_lock_irqsave`（多核下 `cli` 不隔离其他核）。
  4. 验证跨核映射变更时 `IPI_TLB_FLUSH` 真正广播（handler 已注册，需触发路径）。
- **验收**：`run-headless` 下 4 核均跑用户任务、负载可均衡；AP 上 `CR4.SMAP/SMEP/UMIP` 实测置位；`sys_audio_write` 多并发无竞争。
- **关联审计**：H8、L3(AP)、B(partial)。

### P0-R2 【高】安全地基真补全：代码段 KASLR + KPTI 部署（补全 P0-8）
- **对应原项**：P0-8（KASLR「内核基址随机化」、KPTI）
- **现状**：仅堆基址滑移；代码段固定 `KERNEL_BASE`；KPTI 仅做 Meltdown 免疫检测未部署双页表。
- **具体动作**：
  1. **代码段 KASLR**：内核编译为 `-fPIC` + 重定位表，加载时随机基址 + 应用重定位（BIOS/UEFI 均需）；或退而求其次「有限滑块」（固定偏移随机化）。
  2. **KPTI**：对 `RDCL_NO=0` 的 CPU 真正部署用户/内核双页表 + 系统调用 trampoline（PGD 切换）；`RDCL_NO=1` 仍跳过。
  3. AP 安全位（见 R1）一并覆盖。
- **验收**：每次启动内核代码段基址不同；Meltdown 易感 CPU 上 `metric` 显示 KPTI active，用户态无法经推断读内核内存。
- **关联审计**：L5(PIE 缺口)、P0-8 设计取舍。

### P0-R3 【高】诊断地基补全：gdbstub + WARN/BUG + boot log 环缓冲（补全 P0-9）
- **对应原项**：P0-9（串口 gdbstub 常备 / WARN-BUG 宏 / boot log 环缓冲）
- **现状**：仅 `kernel_oops` 转储；三项全无。
- **具体动作**：
  1. 常备串口 GDB stub（拦截 `#DB`/`#BP`，实现 GDB 远程协议 `?/g/G/m/M/c/s` 最小集），`make debug` 可 `target remote`。
  2. `WARN(cond,fmt)` / `BUG_ON(cond)` 宏，统一走 `kernel_oops` 转储 + 可配置 `panic_on_bug`。
  3. 早期 boot log 环缓冲（覆盖 `serial_init` 之前的 `kprintf`），崩溃/OOPS 时随现场一并 dump。
- **验收**：QEMU `-s -S` + GDB 能断点/单步内核；`BUG_ON(1)` 触发标准 OOPS；重启后环缓冲可回看早期日志。
- **关联审计**：H5(partial)、M18。

### P0-R4 【中】AHCI 错误恢复 + 介质健壮性（补全 P0-7 + M10）
- **对应原项**：P0-7（生产稳健）、M10（超时复位重试）
- **现状**：读写成功路径可靠；错误路径（TFD ERR / 坏块 / 超时）无恢复、无控制器复位。
- **具体动作**：
  1. `ahci_exec` 检测 PxTFD.ERR / CI 超时 → 端口复位（`PxCMD` FRST）、命令槽回收、最多 N 次重试。
  2. 读路径（含 AHCI）补齐 `lba+count>total` 与回绕校验（M10 仅 PIO 已做）。
  3. 写返回失败时向上层（disk server）回报错误而非静默成功。
- **验收**：注入坏块/超时后驱动自恢复或明确报错；不静默丢数据。
- **关联审计**：M10、P0-7 稳健性。

### P0-R5 【中】内核栈守卫页（补全 P0-5 / M7）
- **对应原项**：P0-5（地址空间稳健）、M7
- **现状**：每任务内核栈 16KB + 栈底哨兵，但无未映射守卫页；溢出先踩相邻 kmalloc 堆块。
- **具体动作**：为每个内核栈（含 AP 栈、IRQ 栈）在其下方映射 1 页**不可访问守卫页**，栈溢出立即 #PF（走已就绪的 `#DF` IST 守卫栈转储）。
- **验收**：内核栈溢出触发可诊断 #PF/NMI 现场，而非静默堆损坏。
- **关联审计**：M7、D。

### P0-R6 【中】MSI/MSI-X 支持（补全 P0-2）
- **对应原项**：P0-2（MSI/MSI-X）
- **现状**：仅 IOAPIC INTx；MSI-X 未实现（注释标「P0-2 收尾」）。
- **具体动作**：实现 `msi_alloc()`/`msix_setup()`（写 PCI 能力结构 + 消息地址 `0xFEE00000` + 向量）；AHCI/NVMe/网卡驱动切换到 MSI-X（多队列）。
- **验收**：AHCI 走 MSI-X 向量收中断；为 P1-11 NVMe / P1-13 网卡铺路。
- **关联审计**：P0-2 收尾。

### P0-R7 【中】Ring3 服务健壮性（HDA 真机化前置 + fs_server 环检测）
- **对应原项**：P0-9（故障隔离兑现）、H7、D1、M13
- **现状**：混合内核「服务崩溃=功能死」，但 HDA/fs_server 仍有生产缺陷。
- **具体动作**：
  1. **H7**：HDA verb 超时引入专属错误哨兵（如 `~0ULL` 与合法 `0xFFFFFFFF` 区分），`hda_param` 判错不再误用。
  2. **D1**：`hda_pcm_stop` 显式清零 BDL 数据页 + 确认 DMA 停止（防残留循环播放）。
  3. **M13**：`fat_next` 原生环检测（visited 集合 / 步数硬上限），损坏 FS 不再无限循环。
  4. （远期 P1-16）服务崩溃自动重启，兑现故障隔离红利。
- **验收**：HDA 在边界 codec 上参数解析正确；stop 后无残留播放；损坏 FAT 镜像 FS 服务不死循环。
- **关联审计**：H7、D1、M13。

### P0-R8 【中】ACPI 电源与拓扑健壮性（补全 P0-1）
- **对应原项**：P0-1（FADT 电源/复位）、生产关机可靠性
- **现状**：RSDP/XSDT/MADT 解析 OK；缺 FADT reset 寄存器、S5 关机、多 IOAPIC / PCI _PRT 中断路由。
- **具体动作**：
  1. 实现 `acpi_reset()`（FADT RESET_REG）与 `acpi_poweroff()`（FADT S5 + PM1a/b）。
  2. 解析多 IOAPIC 条目 + PCI _PRT，建立 GSI→IOAPIC 路由表（为 MSI-X 之外的传统设备兜底）。
- **验收**：`sys_reboot` 走 ACPI reset；物理机可 S5 关机；多 IOAPIC 设备中断正确路由。
- **关联审计**：P0-1 生产缺口、P2-1 前置。

---

## 四、优先级建议（里程碑切片）

1. **M-P0a「真多核 + 安全一致性」**：P0-R1（对称调度 + 全核安全位）→ 解锁「多核生产」与 H8。
2. **M-P0b「可诊断 + 可防」**：P0-R3（gdbstub/WARN/环缓冲）+ P0-R5（内核栈守卫页）→ 崩溃可事后分析。
3. **M-P0c「安全地基收口」**：P0-R2（代码段 KASLR + KPTI）→ 达到现代内核安全基线。
4. **M-P0d「物理机驱动就绪」**：P0-R6（MSI-X）→ P0-R4（AHCI 错误恢复）→ P0-R8（ACPI 电源/路由）。
5. **M-P0e「服务健壮性」**：P0-R7（HDA/fs_server 真机化）。

> 完成以上即满足「可在生产/物理机安全稳定运行」的 P0 基线；P1 起进入完整 Unix 语义与网络/外设。

---

## 五、与 step10_todos 的差异说明

- step10_todos 把 P0-3 / P0-8 / P0-9 的「锁体系 / KASLR / KPTI / gdbstub」标注为「与某里程碑一起做」即视为可销账。
  本轮代码核对发现：**这些子项实际只做了一部分**（对称调度未做、AP 安全位遗漏、代码段 KASLR 与 KPTI 未部署、gdbstub 缺失）。
  因此把它们从「已完成」挪回「残留任务」，重新编号为 P0-R1…R8，避免「宣称完成但生产不达标」的风险。
- 原 H/M/L/D 清单中绝大多数（约 35 项）确已 FIXED，确认 step11–step21 的质量；本文只新增/重开真正影响生产稳定的缺口。
