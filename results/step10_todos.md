# SukiOS 对标现代 OS 内核功能路线图（step10_todos）

> 生成时间：2026-07-27
> 目标：在**保持混合内核架构**（Ring0 驻留调度器/VMM/PMM/Mach IPC + 必要驱动；Ring3 用户态服务经 mach_msg 通信）的前提下，实现对标 Windows NT / Linux / macOS(XNU) / iOS 内核的完整功能集。
> 运行目标：**x86_64 物理机 + 虚拟机（QEMU/KVM、VMware、VirtualBox、Hyper-V）均可稳定运行**。
> 范围：本文只覆盖内核（及内核紧耦合的用户态系统服务），不含桌面/应用层。
> 优先级定义：P0=地基（阻塞后续一切）；P1=类 Unix 内核核心（达到"完整功能内核"及格线）；P2=对标现代 OS 的差异化能力；P3=高级/远期。

---

## 一、现状基线（截至 Step10）

已有：Multiboot2 BIOS 引导、高半区内核、4 级分页(4KB)、单核 100Hz PIT 轮转调度、syscall ABI（10+ 调用）、Mach IPC（内联双拷贝 + OOL 零拷贝 + 端口权能）、#PF 用户态故障隔离、W^X/SMEP/栈 ASLR/堆 canary、>4GB 映射、FPU/SSE 上下文、ATA PIO 读写、FAT32 只读(Ring3)、ELF64 装载 + execve + spawn/wait、PCI 枚举、Intel HDA 播放、帧缓冲控制台(UTF-8/中文)、PS/2 键盘。

核心短板：**单核、PIC+PIT 古董中断/时钟体系、无 ACPI、无按需分页、BIOS-only 引导、PIO 磁盘、无网络、无 USB、无 VFS、FS 只读**——这些决定了它目前只能跑在 QEMU i440FX 特定配置上，距离物理机/主流虚拟机通用还有明确差距。

---

## 二、P0 — 地基层（阻塞一切后续功能，必须最先完成）

| # | 功能 | 对标 | 现状 | 要点 |
|---|------|------|------|------|
| P0-1 | **ACPI 表解析**（RSDP/XSDT/MADT/FADT/MCFG/HPET） | 四家全有 | 无 | 物理机拓扑发现的唯一入口；MADT→CPU/APIC 枚举，MCFG→PCIe ECAM，FADT→电源/复位。不做 ACPI 就无法离开 QEMU i440FX |
| P0-2 | **LAPIC + IOAPIC + MSI/MSI-X**，废弃 8259 PIC | 四家全有 | 8259+PIT | LAPIC 定时器取代 PIT；IOAPIC 路由 GSI；MSI 是 AHCI/NVMe/网卡的前提。含中断亲和与 EOI 广播抑制 |
| P0-3 | **SMP：AP 启动 + per-CPU 体系 + 锁体系** | 四家全有 | 单核，全靠 cli/sti | INIT-SIPI-SIPI 唤醒 AP；per-CPU GDT/TSS/idle/runqueue（GS_BASE）；自旋锁/读写锁/顺序锁全面替换 cli/sti 临界区（审计清单里所有"单核假设"项都在此销账）；IPI（重调度/TLB shootdown） |
| P0-4 | **高精度时钟体系**：TSC(invariant) 校准 + HPET + LAPIC deadline timer + 单调时钟 | 四家全有 | 仅 PIT 100Hz | clock_gettime 语义基础；tickless 预留；RTC 读墙钟 |
| P0-5 | **按需分页 + COW + mmap 骨架**（VMA/地址空间对象化） | 四家全有 | 全部立即分配 | task 地址空间改为 VMA 链/树；#PF 缺页按 VMA 补页；fork 走 COW；文件映射预留接口。这是 fork、动态链接、大进程的前提 |
| P0-6 | **UEFI 引导路径**（GRUB UEFI 或自带 loader + GOP 帧缓冲） | 四家全有 | 仅 BIOS ISO | 2020 后物理机默认 UEFI；GOP 取帧缓冲替代 VBE 假设；Secure Boot 先不做但布局兼容 |
| P0-7 | **AHCI 驱动**（中断驱动 DMA，替代 ATA PIO） | 四家全有 | ATA PIO 轮询 | 真机 SATA 默认 AHCI 模式；保持混合架构：内核只做裸块读写响应 DISK_PORT，FS 仍在 Ring3 |
| P0-8 | **内核安全地基补全**：SMAP(stac/clac)、KASLR（内核基址随机化）、KPTI、栈保护(-fstack-protector + IST 守卫页) | Win: VBS/KASLR；Linux: 同名；XNU: KASLR/PPL | 仅 SMEP/W^X/栈ASLR | 与 P0-3 一起做（per-CPU 后才有干净的 KPTI trampoline） |
| P0-9 | **内核诊断地基**：panic 现场转储（寄存器/栈回溯/符号化）、串口 gdbstub 常备、WARN/BUG 宏、boot log 环缓冲 | Win: BSOD+minidump；Linux: oops/kdump | panic 仅打印后 hlt | 生产化前提：任何崩溃可事后分析 |

---

## 三、P1 — 完整功能内核及格线（类 Unix 核心 + Mach 混合特色）

| # | 功能 | 对标 | 现状 | 要点 |
|---|------|------|------|------|
| P1-1 | **VFS 层**（vnode/mount/dcache/fd 表） | Linux VFS / XNU vnode / NT IO Manager | 无（syscall 直连 FS_PORT） | 内核 vnode 抽象 + 路径解析缓存；FS 后端仍是 Ring3 服务（保持混合架构），VFS 作为路由/缓存层；open/read/write/close/lseek/stat/readdir 标准 fd 语义 |
| P1-2 | **FAT32 写路径 + 长文件名完整化** | — | 只读 8.3+部分LFN | 簇分配/FAT 镜像同步/目录项更新/fsync；这是自举开发环境的前提 |
| P1-3 | **原生日志文件系统**（或先移植 ext2 + 日志） | NTFS/ext4/APFS | 无 | 掉电一致性（journal 或 CoW）；APFS 式快照可后置 P3 |
| P1-4 | **页缓存 + 统一缓冲管理** | 四家全有 | FS 服务内 32KB 私有预读 | 与 VFS/mmap 打通：read=页缓存拷贝，mmap=同一页映射 |
| P1-5 | **完整进程模型**：fork(COW)、进程组/会话、控制终端、凭据(uid/gid) | POSIX | 仅 spawn/wait | fork 依赖 P0-5；凭据为权限体系地基 |
| P1-6 | **信号**（POSIX signals：产生/屏蔽/用户态 handler/sigreturn） | POSIX / NT APC | 无 | 用户态回调帧构造 + 与 mach 异常端口双轨（XNU 式：Mach exception → BSD signal 转换） |
| P1-7 | **同步原语 syscall**：futex 式等待队列、用户态互斥/条件变量支持 | futex / NT KeWait / XNU ulock | 仅 yield | pthread 类库的前提 |
| P1-8 | **IPC 完备化（Mach 语义补全）**：端口权转移(move/copy send)、port set、死亡通知、消息队列上限/背压、大 OOL 分片 | XNU Mach | 简化权能+固定知名端口 | 消灭"知名端口硬编码"，服务发现走 bootstrap 端口；审计项（队列无上限、OOL bump 无上界）在此销账 |
| P1-9 | **动态链接**：PIE + ld.so（用户态动态链接器）+ vDSO | 四家全有 | 静态平坦 ELF | 代码段 ASLR 依赖 PIE；vDSO 提供 gettimeofday 免陷入 |
| P1-10 | **定时/事件 syscall**：nanosleep、timer_create、poll/epoll(kqueue 式) 事件多路复用 | epoll/kqueue/IOCP | 无 | 网络与 GUI 事件循环的前提；建议直接做 kqueue 语义（贴 XNU） |
| P1-11 | **NVMe 驱动** | 四家全有 | 无 | 现代物理机主流启动盘；MSI-X + 多队列（SMP 后收益） |
| P1-12 | **USB xHCI + HID（键盘/鼠标）+ MSC(U盘)** | 四家全有 | 仅 PS/2 | 物理机键鼠几乎全 USB；驱动主体放 Ring3（保持混合架构），内核只留 xHCI 环 DMA 门 |
| P1-13 | **网络栈**：virtio-net + e1000(e) 驱动 → 以太网/ARP/IPv4/ICMP/UDP/TCP → socket syscall | 四家全有 | 无 | 驱动内核态（DMA 环），协议栈可 Ring3 网络服务（贴微内核）或内核态（贴性能），建议协议栈 Ring3 + 共享环 buffer |
| P1-14 | **virtio 全家桶**（virtio-blk/net/rng/console/gpu 基础） | 各家 guest 支持 | 无 | "能跑在各主流虚拟机"的最短路径；VMware(vmxnet3/pvscsi)、Hyper-V(VMBus) 后置 P2 |
| P1-15 | **OOM 与内存回收**：页回收水位、匿名页交换(swap) 可后置，先做缓存收缩 + OOM killer | 四家全有 | 分配失败即失败 | 与页缓存(P1-4)联动 |
| P1-16 | **Ring3 服务健壮化**：服务崩溃自动重启（launchd 式监管）、端口移交 | XNU launchd / NT SCM / systemd | 服务死=功能死 | 混合内核的"故障隔离红利"必须靠它兑现 |

---

## 四、P2 — 对标现代 OS 的差异化能力

| # | 功能 | 对标 | 要点 |
|---|------|------|------|
| P2-1 | **电源管理**：ACPI S5 关机/S3 睡眠、P-state(cpufreq)/C-state、笔记本电池/热管理 | 四家全有 | 目前连 poweroff 都没有；QEMU 下先做 S5 |
| P2-2 | **驱动框架对象化**（IOKit 式设备树/匹配/电源域 + 设备接口稳定 ABI） | IOKit / NT PnP+WDM / Linux device model | 现驱动全是散函数；热插拔(USB/NVMe)依赖它 |
| P2-3 | **安全框架**：代码签名验证(ELF 签名)、能力/entitlement、沙箱(seatbelt/LSM 式钩子)、任务间 IPC 授权 | iOS codesign+sandbox / NT token / Linux LSM | 混合内核卖点：签名校验放内核，策略引擎放 Ring3 |
| P2-4 | **显示通路**：virtio-gpu / GOP 多头 + Ring3 Display Server 合成器（OOL 零拷贝像素）+ 组合键 VT 切换 | 各家窗口服务器 | 既有 OOL IPC 设计即为此准备；真 GPU 驱动(P3) |
| P2-5 | **音频框架化**：HDA 真机 codec 兼容（0x5A=RIRBRP、widget 全枚举、耳机/功放路径）、混音服务(Ring3)、多流 | CoreAudio / WASAPI / ALSA | 现驱动只认 QEMU 建模；step10 遗留(stop 残留/44.1k)在此销账 |
| P2-6 | **crash dump 完整链**：内核 minidump 落盘、用户态 core dump、符号服务 | 四家全有 | 依赖 FS 写(P1-2) |
| P2-7 | **hypervisor guest 集成**：VMware Tools 级（vmxnet3/pvscsi/时间同步）、Hyper-V VMBus/enlightenments、KVM pvclock | 各家 guest 增强 | "运行在所有主流虚拟机"的收尾 |
| P2-8 | **审计与可观测性**：syscall 审计流、内核 tracepoint/计数器（dtrace/eBPF 的极简前身）、per-task 统计(top 数据源) | dtrace/ETW/eBPF | |
| P2-9 | **文件系统高级**：权限位/ACL、符号链接/硬链接、配额、fsck 工具链 | 四家全有 | 依赖 P1-3 |
| P2-10 | **内核模块/驱动动态加载**（kext 式，带签名校验） | kext / .sys / .ko | 混合内核可选：优先"新驱动=Ring3 服务"，仅性能关键才入内核 |
| P2-11 | **多架构预留**：ARM64 移植层（MMU/GIC/PSCI 抽象隔离） | 四家全有(iOS/mac 即 ARM) | 只做 HAL 分层，不急于实现 |

---

## 五、P3 — 高级 / 远期

| # | 功能 | 对标 |
|---|------|------|
| P3-1 | 实时调度类（SCHED_FIFO/RR、优先级继承）+ 低延迟音频路径 | 四家全有 |
| P3-2 | NUMA 感知内存/调度 | Win/Linux/XNU |
| P3-3 | cgroup/Jobs 式资源控制 + 命名空间/容器 | Linux cgroups / NT Job / iOS jetsam |
| P3-4 | 异步 I/O 环（io_uring / IOCP 式提交-完成环） | io_uring / IOCP |
| P3-5 | 透明大页/内存压缩（XNU compressor 式） | 四家全有 |
| P3-6 | 自研 hypervisor 能力（Hypervisor.framework / KVM 式 vCPU API） | KVM / Hyper-V / HVF |
| P3-7 | 快照/克隆文件系统（APFS 式 CoW）+ 全盘加密（FileVault/BitLocker 式） | APFS/NTFS/ext4+LUKS |
| P3-8 | Secure Boot 链 + 内核完整性保护（PatchGuard/KTRR 式） | 四家全有 |
| P3-9 | 图形栈深水区：真 GPU 驱动(i915/amdgpu 级) 或 GPU 直通 | 各家 |
| P3-10 | 无线/蓝牙栈 | 各家 |

---

## 六、建议实施顺序（里程碑切片）

1. **M1「离开古董机」**：P0-1 ACPI → P0-2 APIC/MSI → P0-4 时钟 →（此时仍单核，但中断/时钟现代化）
2. **M2「多核」**：P0-3 SMP + 锁体系 + P0-8 安全地基 + P0-9 诊断（三者强耦合，一起做）
3. **M3「现代内存」**：P0-5 VMA/按需分页/COW/mmap
4. **M4「真机可启动」**：P0-6 UEFI + P0-7 AHCI + P1-11 NVMe + P1-12 USB HID → **第一次物理机点亮**
5. **M5「完整 Unix 语义」**：P1-1 VFS → P1-2 FAT32 写 → P1-5/6/7 进程/信号/futex → P1-9 动态链接 → P1-10 事件
6. **M6「联网」**：P1-13 网络栈 + P1-14 virtio
7. **M7 起**：P2 逐项（电源→驱动框架→安全→显示→音频真机化→虚拟机集成）
8. P3 按需。

每个里程碑完成时按项目规则写 `results/stepNN.md`。

---
---

# 附录：全代码库生产安全审计清单（不能稳定安全运行于生产环境的代码点）

> 审计时间：2026-07-27（Step10 后全量代码）。范围：`kernel/`、`include/`、`user/`、`boot/`、`Makefile`。
> 总体结论：用户指针红线（copy_from_user）落实较好，但存在若干**边界差一、资源池无上界、单核假设、超时/错误路径缺失**类硬伤。修复建议随 P0/P1 里程碑销账（对应项已标注）。

## A. 高风险（可致内核崩溃 / 内存越界 / 越权 / 长稳必炸）

| # | 位置 | 问题 | 销账里程碑 |
|---|------|------|-----------|
| H1 | `kernel/elf/elf.c` `elf_build_stack()` | 局部数组 `arg_va[64]/env_va[64]` 与 `EXEC_ARG_MAX=64` 边界差一：argc==64 时 `for(i<=argc)` 写 `arg_va[64]` 越界 8 字节，破坏相邻栈变量 | 立即修 |
| H2 | `kernel/elf/elf.c` `elf_build_stack()` | `auxv[16]` 容量硬编码且 `na` 无上界校验，未来扩展 auxv 即越界写栈 | 立即修 |
| H3 | `kernel/ipc/port.c` `port_allocate()` | 端口耗尽返回 `PORT_NULL(0)`，与合法"空端口"语义混淆；调用方 `port_free(0)` 静默空操作 → 临时端口泄漏、execve 静默失败 | 立即修 |
| H4 | `kernel/ipc/port.c` `ool_map_into_receiver()` | `g_ool_bump` 自 `0x600000000000` 单调递增**无上界检查**；空闲链复用条件苛刻。长稳大量 OOL（音频/大文件）必然耗尽/撞区 → 内核 #PF panic | 立即修（上界+区间管理）；彻底解决在 P1-8 |
| H5 | `kernel/arch/x86_64/idt.c` `page_fault_handler()` | 内核态 #PF 直接 panic→`cli;hlt` 整机挂死，无现场转储/无恢复路径；`kprintf` 非重入安全，嵌套异常输出撕裂 | P0-9 |
| H6 | `kernel/drivers/hda.c` `hda_pcm_open/write` | HDA 为全局单例资源但**无所有权/重入保护**：任何任务可随时 open 重置 DMA、覆盖 BDL，破坏他人正在播放的流（对比端口有 owner 机制，音频完全没有） | 立即修（记录 owner task） |
| H7 | `kernel/drivers/hda.c` `hda_verb_raw/hda_param` | 超时返回 `0xFFFFFFFF` 与合法响应值混淆，`hda_param` 直接把超时当参数用（如 AMP caps）→ 真机上超时被静默当有效能力位 | P2-5 |
| H8 | `kernel/syscall/syscall.c` `sys_audio_write()` 等 | 静态内核缓冲依赖"syscall 串行"这一**单核+关中断隐含假设**，无锁注释/断言；SMP 化后即数据竞争（同类单核假设遍布 sched/port/pmm/vmm 全局变量） | P0-3 |

## B. 中风险（边界/溢出/泄漏/健壮性缺陷）

| # | 位置 | 问题 |
|---|------|------|
| M1 | `kernel/mm/kmalloc.c` `kfree()` | 无双重释放防护，重复 kfree 破坏合并逻辑（canary 只防覆写不防 double-free） |
| M2 | `kernel/mm/pmm.c` `pmm_incref()` | 引用计数无溢出保护；`pmm_alloc_page` 位图+refcount 非原子（SMP 竞态，P0-3 销账） |
| M3 | `kernel/mm/vmm.c` `vmm_map_page()` | 已存在 PTE 被静默覆盖，重复映射/权限降级无感知 |
| M4 | `kernel/mm/vmm.c` `vmm_destroy_address_space()` | 仅凭 PTE_OOL 位区分共享页，未校验共享计数，异常路径可误释放他任务页 |
| M5 | `kernel/ipc/port.c` `enqueue()` | 消息队列**无长度上限**，恶意任务狂发可耗尽内核堆（无背压/丢弃策略）（P1-8） |
| M6 | `kernel/ipc/port.c` `sys_mach_msg()` | 接收侧未充分校验 msgh_bits/OOL 描述符伪造的 size 字段组合 |
| M7 | `kernel/sched/sched.c` | 任务数无硬上限（PID/物理页耗尽无保护）；内核栈 16KB 固定且**无守卫页**，深调用链/中断嵌套溢出即静默破坏相邻堆块 |
| M8 | `kernel/syscall/syscall.c` `exec_copy_args()` | kmalloc 中途失败的部分释放路径未经受测试证明（建议统一 goto-fail 审查） |
| M9 | `kernel/elf/elf.c` `elf_validate/elf_load` | `e_phnum` 无上限校验，`e_phoff + phnum*phentsize` 可回绕越界读；`vaddr+memsz` 相加无回绕检查，恶意 ELF 可请求映射到内核区 |
| M10 | `kernel/drivers/ata.c` `ata_read_sectors()` | 读路径未校验 `lba+count > total_sectors`（写路径有）→ 越界读盘；等待超时后无复位/重试，坏盘静默失败 |
| M11 | `kernel/drivers/hda.c` `hda_verb_raw()` | 100000 次 io_wait 忙等（无调度让出），codec 无响应时长期占死 CPU |
| M12 | `kernel/drivers/hda.c` `hda_init()` | BDL/CORB/RIRB 页分配中途失败不回滚，已分配页泄漏 |
| M13 | `user/fs_server.c` `fat_next()` | FAT 簇链**无环检测**，损坏文件系统的环形链 → read_dir/read_file_at 无限循环（FS 服务假死） |
| M14 | `user/fs_server.c` `read_dir()` | 簇号仅判 `>=2 && <0x0FFFFFF8`，未校验落在数据区有效范围，越界簇号误读任意扇区 |
| M15 | `user/fs_server.c` `resolve_path()` | 路径组件 `comp[8][13]` 固定上限，超长路径静默截断（应显式报错） |
| M16 | `user/fs_server.c` `disk_read()` | 应答 status 失败分支仍可能按 count 拷贝，客户端缓冲读到未初始化数据 |
| M17 | `user/lib/suki.c` `u_utoa()` | 输出 buf 由调用方提供且无长度参数，接口天然不安全（现调用点均够大，属接口债务） |
| M18 | `kernel/console.c` + IRQ 路径 | `kprintf` 无锁非重入（单核 IF=0 下侥幸安全），IRQ handler 内打印属隐患（P0-3/P0-9） |

## C. 低风险（可观测性 / 接口债务 / 构建硬化）

| # | 位置 | 问题 |
|---|------|------|
| L1 | `kernel/syscall/syscall.c` `sys_debug_write()` | 单条截断 256B 且无声丢弃，长日志丢失（仅影响可观测性） |
| L2 | `kernel/drivers/ata.c` `ata_init()` | 仅取 IDENTIFY word 60/61（LBA28），>128GB 盘容量截断（P0-7 AHCI 换代后自然销账） |
| L3 | `boot/boot.S` | 引导页表硬编码 4 个 PD 映射 0..4GB（vmm_init 已补 >4GB，但引导早期触碰高地址仍炸）；SMAP 未启用（P0-8） |
| L4 | `boot/linker.ld` | LMA 0x100000 + MB2 头须在文件前 32KB 的强布局假设，缺构建期断言 |
| L5 | `Makefile` | 全程 `-fno-stack-protector`，内核/用户态均无栈金丝雀（P0-8）；用户程序无 PIE（P1-9） |
| L6 | `kernel/drivers/hda.c` `hda_codec_setup()` | connection list 条目数未校验上限，异常 codec 可致越界读响应窗口 |
| L7 | `user/fs_server.c` `lfn_pull()` | LFN 序号可被伪造导致名字截断处无 NUL（调用方缓冲够大，暂无害） |
| L8 | `kernel/sched/sched.c` | `KSTACK_SIZE` 命名/注释与用户栈常量易混淆（纯可读性） |

## D. Step10 已知功能性遗留（非安全类）

| # | 问题 | 归属 |
|---|------|------|
| D1 | `sys_audio_stop` 后 BDL 环残留数据疑似继续循环播放（6s 正弦录出 21.94s WAV） | P2-5 前置，可先修（stop 路径静音清环 + 确认 DMA 停止） |
| D2 | exec 传参 argv 错乱（`exec BIN/playaudio MOONHALO.MP3 bench` 曾解析出 `file = 3`），shell→spawn→elf_build_stack 链路待专项排查（注意与 H1 边界 bug 可能相关） | 立即排查 |
| D3 | QEMU audiodev 默认 fixed-settings 强制 44100Hz（QEMU 内部重采样，不影响音准，仅 WAV 录制头异常）；真机无此问题 | 记录即可，或 run 目标加 `out.fixed-settings=off` |

> **销账原则**：标注"立即修"的 H1~H4/H6/D2 应在进入 P0 里程碑前用一个独立 step 完成（预计 step11）；其余随对应里程碑（P0-3 锁体系、P0-9 诊断、P1-8 IPC 完备化、P2-5 音频真机化）系统性解决，避免在旧架构上打散补丁。
