<div align="center">
<img alt="SukiOS_Logo" src="images/EDBB1A1D-6657-433A-8707-B3212AA99754.png" width="100" height="100">
</div>

# SukiOS
![sukios](https://img.shields.io/badge/Suki-OS-00a1d6?labelColor=fb7299)
![sukios_version](https://img.shields.io/badge/version-0.0.1--beta-yellow
)
![sukios_building](https://img.shields.io/badge/build-locally%20passing-brightgreen
)
![sukios_usablility](https://img.shields.io/badge/usable-obviously%20not-red)

**x86_64 混合内核操作系统**。

SukiOS 采用「混合内核（hybrid kernel）」架构：核心内核（Ring0）常驻调度器、内存管理、IPC 端口路由，以及**少量必要的裸硬件驱动**（ATA/AHCI 磁盘、Intel HDA 音频）；其余设备驱动（文件系统、显示合成、输入）以 **Ring3 用户态服务进程**的形态存在，彼此之间通过内核提供的 **Mach 风格消息传递（mach_msg）** 通信。

---

## 目录

1. [项目介绍](#1-项目介绍)
2. [项目已经实现的](#2-项目已经实现的)
3. [项目还没实现的](#3-项目还没实现的)
4. [如何编译与运行](#4-如何编译与运行)
   - 4.1 [环境依赖](#41-环境依赖)
   - 4.2 [构建命令](#42-构建命令)
   - 4.3 [运行（QEMU）](#43-运行qemu)
   - 4.4 [关于「可能无法正常运行」的重要说明](#44-关于可能无法正常运行的重要说明)
5. [项目结构速览](#5-项目结构速览)
6. [向仓库贡献](#6-向仓库贡献)
7. [致谢](#7-致谢)
8. [版权与许可证](#8-版权与许可证)

---

## 1. 项目介绍

SukiOS 是一个从引导扇区开始、完全自研的 64 位操作系统内核，运行目标为 **x86_64 物理机 / QEMU 虚拟机**。其设计灵感来自 XNU（macOS 内核）的「混合内核」思想，但代码完全独立实现。

**核心设计原则：**

- **混合内核 + Mach IPC**：内核只做最必要的事（调度、内存、进程、IPC、极少裸驱动），把文件系统、显示、输入等「驱动」上移为用户态服务进程，通过内核全局端口表（`kernel_port_t`）做消息路由。小消息（< 4 KiB）由内核 memcpy 转发；大消息（如像素缓冲）通过 **OOL（out-of-line）物理页重映射零拷贝**（传递页框号 + 引用计数 +1）。
- **对称多处理（SMP）**：基于 ACPI MADT 发现多核，经 INIT-SIPI-SIPI 唤醒 AP，所有核对称参与调度（每核 LAPIC 100 Hz 节拍，Ring3 抢占式 RR）。
- **安全地基优先**：SMAP/SMEP、NX、UMIP、KPTI（Meltdown 缓解）、栈金丝雀、用户指针 `copy_from_user` / `copy_to_user` 围栏、KASLR（代码段随机化）、IST 守卫栈，均在启动早期装配。
- **可诊断性**：常备串口 GDB stub（COM2）、内核日志环形缓冲、WARN_ON 回溯，便于在真实硬件 / 虚拟机的受限环境下定位问题。

**硬件 / 架构约定：**

| 项 | 值 |
|---|---|
| 架构 | x86_64（long mode，4 级分页，4 KiB 页） |
| 内核虚拟基址 `KERNEL_BASE` | `0xFFFF800000000000` |
| 内核物理加载地址 | `0x100000`（1 MiB） |
| 用户态地址空间 | `0x0` ~ `0x00007FFFFFFFFFFF` |
| 段选择子 | 内核 `CS=0x08 / DS=0x10`；用户 `CS=0x1B / DS=0x23` |
| 系统调用 | `syscall` 指令（IA32_LSTAR MSR） |
| 调用约定 | `%rax`=调用号；`%rdi/%rsi/%rdx/%r10/%r8/%r9`=参数（System V AMD64） |
| 启动协议 | Multiboot2（GRUB）/ PVH（qemu `-kernel`） |
| 固件 | Legacy BIOS（SeaBIOS）与 UEFI（OVMF）双支持 |

**系统调用表（已实现）：**

| 号 | 名称 | 说明 |
|---|---|---|
| 0 | `sys_mach_msg` | Mach 风格 IPC 收发 |
| 1 | `sys_task_create` / `sys_task_spawn` | 创建 / 派生用户态任务（类 fork+exec） |
| 2 | `sys_task_exit` | 任务退出 |
| 3 | `sys_yield` | 主动让出 CPU |
| 4 | `SYS_DEBUG_WRITE` | 调试输出（经 `copy_from_user` 校验） |
| 5 | `SYS_INPUT_READ` | 非阻塞取原始键盘扫描码 |
| 6 | `SYS_REBOOT` | 重启 / ACPI S5 关机 |
| 7 | `SYS_PORT_CLAIM` | 用户态认领端口接收权（IPC 能力） |
| 8 | `SYS_EXECVE` | 加载并执行 ELF 映像（替换自身 / 新建子任务） |

---

## 2. 项目已经实现的

> 以下均已在 QEMU（i440FX / qemu64，SMP 4 核，KVM 或 TCG）实证，启动零 panic、核心子系统自检通过。

### 2.1 启动与底层体系
- **引导**：Multiboot2 头（GRUB 加载）+ Xen PVH 直启（`-kernel` 路径）双通道；`.boot` 段严格落在 1 MiB 且 Multiboot2 头位于首 32 KiB 内。
- **CPU 模式**：从 16 位实模式 → 32 位保护模式 → 64 位 long mode 的完整切换。
- **GDT / TSS / IDT**：每 CPU 独立 TSS（Ring3→Ring0 用 `TSS.RSP0` 装载内核栈）；IDT 覆盖异常与中断向量，IST 守卫栈防栈溢出。
- **SMP**：ACPI MADT 解析、`percpu` 每 CPU 槽（经 `KernelGSBase` MSR + `swapgs` 模型访问）、INIT-SIPI-SIPI 唤醒 AP、对称调度。
- **异常与诊断**：`#PF`/`#GP`/`#UD` 等精确报告 RIP / 错误码 / CR3；`WARN_ON` 打印 + 调用链回溯 + 继续；串口 GDB stub 常备。

### 2.2 内存管理
- **PMM**（物理内存管理）：基于 boot 期内存探测（multiboot2 / ACPI）的页帧分配器，含引用计数（OOL 共享页粘滞）、压力自检。
- **VMM**（虚拟内存）：4 级分页、独立地址空间创建、按需分页（VMA 链表）、COW fork / 断开、`munmap`、`mprotect` 边界。
- **KPTI**：为缓解 Meltdown，用户态 / 内核态使用分离页表（CR3 bit12 切换），内核 `.text/.data/.bss`（含 per-CPU TSS、percpu）正确映射进每个地址空间的影子视图。
- **KASLR**：内核代码段加载基址随机化（两阶段链接：预链接抽重定位表 → 重链），用户栈亦做 ASLR。
- **内核堆**：`kmalloc` / `kzalloc` / `kfree` 简易分配器。

### 2.3 进程与调度
- **任务模型**：`task_t` 支持内核任务与用户任务、idle 任务、zombie 回收。
- **调度器**：**单一全局运行队列 + 每任务 `cpu` 占用标志（原子 CAS 风格）**，从构造上杜绝「双核同跑一任务」竞态；RR 时间片抢占；阻塞唤醒经 IPI 广播；`context_switch` 正确保存 / 恢复 callee-saved 寄存器、切换 RSP、切换 CR3（刷 TLB）、恢复 per-CPU GS。
- **syscall / 中断上下文切换**：`syscall_entry.S` 与 `isr.S` 均按 OSDev SWAPGS 标准做 `swapgs` 配对，且 `context_switch` 末尾确定性恢复内核态 GS，消除 GS 翻转态跨任务丢失导致的 page fault。

### 2.4 IPC（Mach 风格）
- 全局端口表 `kernel_port_t`；`mach_msg_send` / `mach_msg_recv`；发送权 / 接收权（owner）能力检查（`port_claim`）。
- 小消息内核 memcpy 转发；大消息 OOL 物理页重映射零拷贝（引用计数 +1）。

### 2.5 设备驱动（Ring0 裸驱动）
- **ATA / IDE PIO**（i440FX 传统 IDE）与 **AHCI**（SATA，DMA + 中断）磁盘驱动，probe 优先级 AHCI > ATA。
- **Intel HDA** 音频控制器（PCI class 0x04/0x03 探测，ICH6 兼容），输出编解码器。
- **键盘**：PS/2 键盘经 I/O APIC 投递（`input_server` 经 `SYS_INPUT_READ` 取扫描码）。
- **PCI**：ECAM（MCFG）探测，缺失时 PIO 回退；`_PRT` 路由、MSI 编程。
- **ACPI**：RSDP/XSDT/MADT/HPET/MCFG 解析；LAPIC / IOAPIC 取代 8259 PIC + PIT；TSC 校准。

### 2.6 用户态服务（Ring3 进程）
- **disk-srv**（内核线程）：响应 `DISK_PORT`，做真实扇区读写。
- **fs-server**：用 **FatFs（ChaN R0.16）** 做 FAT32 解析；`diskio.c` 经 `DISK_PORT` IPC 转交 disk-srv 做磁盘 IO。支持 create / write / append / read_at / read_file（OOL）/ mkdir / rename / truncate / unlink / list，长文件名（LFN）正确，整条路径经真实磁盘 IO 自检（self-test ALL PASS）。
- **input-server**：转发键盘输入。
- **shell**：交互式命令行，转发 `FS_MSG_*` 给 fs-server（支持 `ls` / `cat` / `write` / `mkdir` / `exec` 等）。
- **独立用户程序**（放入 FAT32 磁盘 `::BIN/`）：`hello`、`playaudio`（minimp3 MP3 解码，依赖 SSE）、`audiotest`，由 shell `exec` 从磁盘装载。

### 2.7 安全
- SMAP / SMEP / NX / UMIP 在 `security_init()` 装配；用户指针强制 `copy_from_user` / `copy_to_user`（SMAP 围栏 + 内核缺页 panic / 用户缺页杀任务）。
- 栈金丝雀（`-fstack-protector-strong` + 全局 guard）。
- KASLR + KPTI 已部署。

### 2.8 构建与启动多样性
- ISO（BIOS+UEFI 双启动，GRUB）、FAT32 磁盘镜像（mtools 生成）、QEMU 运行目标（`run` / `run-headless` / `run-ahci` / `run-uefi` / `run-q` 等）。
- 交叉工具链（`x86_64-elf-gcc`）自动探测，缺失时回退主机 `gcc` + `-ffreestanding`。

---

## 3. 项目还没实现的

> 以下为当前明确**未实现 / 处于早期或缺失**的部分，列出以避免误用。

- **显示合成服务（Display Server）**：架构上规划了「应用离屏 Buffer → OOL IPC → Display Server → 合成 → 写帧缓冲」的 GUI 路径，但 `display_server` 进程**尚未实现**，当前仅内核自带简单帧缓冲控制台（`fbcon`）。无窗口系统 / 合成器。
- **网络栈**：**完全未实现**。无 virtio-net / Intel 网卡驱动，无 TCP/IP、UDP、socket 抽象。
- **真正的 GUI / 图形应用**：仅有帧缓冲文本控制台，无图形桌面、无应用框架。
- **存储**：仅 FAT32（只读属性到位、写路径已实现）经 FatFs；**无 ext2/3/4、exFAT、NTFS、ISO9660 等其它文件系统**（FatFs 本身理论上可扩展，但当前仅挂卷 0 且未接其它 fs 类型）。
- **完整 POSIX 兼容层**：用户态为 freestanding（无 libc，仅 `user/lib/suki.c` 提供极简桩），无 fork/exec 的 Unix 语义、无信号、无 pthread、无文件描述符表（一切经 Mach 端口）。
- **虚拟内存高级特性**：无 swap（交换到磁盘）、无 huge page、无 NUMA 感知。
- **电源管理**：仅 `SYS_REBOOT` 重启与 ACPI S5 关机；无睡眠 / 休眠 / CPU 频率调节。
- **用户态隔离强化**：进程间目前靠端口能力做粗粒度隔离，无完整的能力 / 沙箱模型，无用户态页表隔离之外的权限边界（如 seccomp 类机制）。
- **调试体验**：GDB stub 存在但依赖串口（COM2）；无内核符号自动加载脚本、无崩溃自动 dump 分析。
- **真实硬件适配广度**：仅在 QEMU（i440FX / q35 / OVMF）验证；未在现代物理机、不同 AHCI/网卡型号上测试。
- **多核调度策略**：目前为对称 RR；无 CFS / 优先级继承 / 负载均衡迁移（任务创建后可跑任意 AP，但无主动负载均衡）。
- **存储热插拔 / 分区表**：仅识别首个磁盘首分区 FAT32；无 GPT 多分区、无多磁盘管理。

> 注：上述「未实现」是本项目**当前阶段**的真实状态，会随开发推进变化。路线图中的 GUI、网络等属于「远期目标」，并非已具备功能。

---

## 4. 如何编译与运行

### 4.1 环境依赖

| 依赖 | 用途 | 备注 |
|---|---|---|
| `gcc`（或 `x86_64-elf-gcc` 交叉编译器） | 编译内核 / 用户态 | 自动探测交叉链，缺失回退主机 gcc |
| `make` | 构建编排 | — |
| `grub-mkrescue` | 生成可引导 ISO | 需 `grub-pc` / `grub-efi` 模块 |
| `mtools`（`mformat`/`mcopy`/`mmd`） | 生成 FAT32 磁盘镜像 | `make disk` 依赖 |
| `qemu-system-x86_64` | 运行模拟器 | 建议安装以 `/dev/kvm` 启用硬件加速 |
| `python3` | KASLR 重定位表生成（`tools/gen_relk.py`） | 构建期依赖 |
| （可选）`OVMF`（`/usr/share/OVMF/OVMF_CODE_4M.fd`） | UEFI 启动 | `make run-uefi` 需要 |
| （可选）`x86_64-elf-binutils` | 交叉链接 | 配合交叉 gcc |

### 4.2 构建命令

```bash
# 1) 清理（会删除 build/ 下全部产物，含 disk.img，需重跑 make disk）
make clean

# 2) 编译内核 + 全部用户态服务/程序，生成 build/SukiOS.iso
make iso

# 3) 生成 FAT32 磁盘镜像（含 README/HELLO/ROADMAP、::SYS、::BIN/*、MOONHALO.MP3）
make disk
```

常用变体：

```bash
make all              # 仅编译内核 ELF
make run              # 图形窗口运行（需 X / GTK，可肉眼观察输出）
make run-headless    # 无头运行，仅串口（自动化验证用）
make run-ahci        # 磁盘挂 AHCI（DMA+中断）验证
make run-uefi         # OVMF UEFI 启动验证
make run-q           # PVH 直启（qemu -kernel，不经 GRUB）
make info            # 打印当前工具链/对象信息
```

> 默认 `-smp 4`（4 核）、KVM 自动检测（无 `/dev/kvm` 时回退 TCG）；可用 `make run QEMU_SMP=1` 单核回退、`make run QEMU_ACCEL=tcg` 强制软件仿真。

### 4.3 运行（QEMU）

```bash
# 图形窗口（推荐用于手动交互测试 shell / 音频）
make run

# 无头（串口落盘，后台/前台均可，配合 cat -v / tail / grep 看日志）
make run-headless
```

启动成功后串口日志应出现类似：

```
[boot] SukiOS kernel entered (long mode, higher half).
[console] UTF-8 test: 中文显示正常 ✓ 操作系统启动成功
[mm] selftest ...
[fs] FS_SERVER starting
[fs] mounted FAT32 (FatFs)
[fs] self-test ALL PASS
SukiOS>
```

### 4.4 关于「可能无法正常运行」的重要说明

本项目**当前仍处于活跃开发阶段**，以下情况可能导致你拉取后无法按预期运行，请知悉：

1. **仅 QEMU 验证过**：所有功能均在 QEMU（i440FX / q35 / OVMF）下实证；**未在真实物理机 / 不同硬件型号上测试**。在实体机上可能遭遇未适配的 ACPI / 设备差异而启动失败。
2. **硬件加速影响体验**：音频解码（minimp3）在 KVM 下可实时，纯 TCG 软件仿真下仅约 1/8 实时，音频必然断续（非系统缺陷，是仿真性能限制）。
3. **headless 下键盘不可注入**：QEMU headless 模式的 `sendkey` **不注入键盘中断**（QEMU 限制，非本系统缺陷），因此 shell 的交互命令（`ls`/`cat`/`write`/`exec`）**无法在无头模式自动验证**，必须在 `make run` 图形窗口中由人工按键测试。
4. **构建产物需完整链路**：`make iso` 必须配合 `make disk` 才有 FAT32 磁盘；缺磁盘则 fs-server 无盘可挂。修改源码后若行为异常，先 `make clean && make iso && make disk` 全量重建（避免增量链接残留旧对象）。
5. **KASLR 两阶段链接**：内核采用预链接 + 重定位表重链，若 `tools/gen_relk.py` 或 Python 环境缺失，链接阶段会失败。
6. **稳定性边界**：虽已追求「生产零 panic」，但本项目为教学 / 研究性质，SMP 竞态、设备差异、边界输入仍可能触发未预见的崩溃；如遇崩溃，串口日志与 GDB stub 是主要诊断手段。
7. **第三方库位置**：`minimp3/`、`drivers/FatFs/`、`kernel/abilities/miniz/`、`lib/newlib-4.6.0.20260123/` 均为第三方组件，版权见 `NOTICE`；其中 newlib 仅作为参考 / 部分符号来源，并未整体编入内核（内核为 freestanding，用户态桩自供）。

**简而言之**：本项目能跑、功能在 QEMU 下可演示，但「开箱即用于生产 / 真实硬件」尚不成立。把它当作一个**可运行的研究型教学内核**来使用。

---

## 5. 项目结构速览

```
SukiOS/
├── boot/                  # 引导：boot.S、multiboot2 头、PVH 入口、linker.ld
├── kernel/
│   ├── arch/x86_64/       # 架构相关：isr.S、syscall_entry.S、switch.S、
│   │                      #   percpu.c、gdt.c、smp.c、security.c、pvh.c …
│   ├── mm/                # PMM / VMM / kmalloc / vma（按需分页 COW）
│   ├── sched/             # 调度器（sched.c）、上下文切换（switch.S）
│   ├── ipc/               # Mach 端口与 mach_msg
│   ├── syscall/           # 系统调用分发与各 sys_* 实现
│   ├── acpi/              # ACPI 解析、LAPIC/IOAPIC、HPET、PCI
│   ├── drivers/           # Ring0 裸驱动：ata.c、ahci.c、hda.c、keyboard.c
│   ├── elf/               # ELF64 加载（execve 用）
│   ├── user/              # 内核侧用户任务装载 / crt0 支持
│   ├── abilities/         # 用户态能力库（miniz 压缩，依赖 libc，不入内核）
│   ├── kmain.c            # 内核 C 主入口（初始化顺序编排）
│   ├── console.c / klog.c / diagnostics.c / stack_canary.c
│   └── gdbstub.c          # 串口 GDB 远程调试 stub
├── user/                  # Ring3 服务与程序（C，freestanding）
│   ├── fs_server.c        # FAT32 服务（FatFs + DISK_PORT）
│   ├── input_server.c     # 键盘输入服务
│   ├── shell.c            # 交互 shell
│   ├── apps/              # 独立程序：hello / playaudio / audiotest
│   └── lib/               # 用户态 freestanding 桩：crt0 / suki.c / stack_canary
├── drivers/FatFs/         # ChaN FatFs R0.16（ff.c/ff.h/ffunicode.c/diskio.c/ffconf.h）
├── lib/newlib-4.6.0.20260123/  # newlib 参考（不编入内核）
├── minimp3/               # minimp3 解码库（CC0，playaudio 用）
├── include/               # 内核 / 用户态公共头
├── grub/                  # grub.cfg（ISO 引导配置）
├── tools/                 # gen_relk.py（KASLR 重定位）等
├── results/               # 阶段性实现文档（stepNN.md）
├── osdev_wiki/            # OSDev 维基离线副本（实现参考）
├── build/                 # 构建产物（被 .gitignore 忽略）
├── Makefile
├── NOTICE                 # 第三方库版权与许可证
├── LICENSE                # GPL-3.0 全文
└── README.md              # 本文件
```

---

## 6. 向仓库贡献

欢迎以 Issue、讨论、文档修订或 Pull Request 的形式参与 SukiOS 开发。本节详述贡献流程、代码规范与本地验证要求，目的是让合并后的内核在 QEMU 生产场景下**零 panic**、架构行为**严格符合 OSDev 标准**。

### 6.1 开发环境准备

```bash
# 克隆（若从远程）
git clone https://github.com/bil-fis/SukiOS SukiOS && cd SukiOS

# 确认工具链与依赖就绪
make info                 # 打印 CC / 工具链 / 对象列表
which grub-mkrescue mformat mcopy mmd qemu-system-x86_64 python3
```

- 推荐启用 KVM（`/dev/kvm` 存在）以获得接近真实的运行速度，尤其是音频解码相关验证。
- 构建与运行**不需要** `source` 任何 env 脚本；构建环境由 `Makefile` 自身处理。

### 6.2 分支与提交约定

- 主分支为 `master`。请勿直接向 `master` 强推（`--force`）已发布历史；功能开发请在独立分支进行，经评审后合并。
- 提交信息采用**简短英文前缀 + 中文描述**，例如：
  - `sched: 重写全局运行队列消除双核同跑竞态`
  - `audit-fix: 修复 AP idle 切栈误 panic (H1 H2)`
  - `fatfs: fs-server 改用 FatFs R0.16 对接 DISK_PORT`
  - 若一次提交修复多个审计项，可在括号内标注编号（如 `(H1 H2 H3)`）。
- 提交范围应**自洽且可工作**：每条提交都应是能编译、能在 QEMU 跑通的完整改动，不夹带半成品 / 留桩（stub）/ TODO 占位。
- 提交后**默认不推送远程**，除非维护者明确要求；推送前确保已 `git pull --rebase` 避免无意义 merge。

### 6.3 编码与架构规范（强制）

这些规则来自项目长期积累的开发铁律，违反会导致评审被拒：

1. **文件路径精确**：涉及代码改动时，在讨论 / 文档中标注完整绝对路径（如 `kernel/arch/x86_64/syscall/syscall_init.c`）。
2. **内存安全**：所有从用户态接收的指针在 syscall 处理中**严禁直接解引用**，必须用 `copy_from_user(dest, user_src, n)` 拷到内核缓冲区（内核缺页 panic、用户缺页杀任务）。
3. **内联汇编隔离**：`wrmsr` / `lgdt` / `ltr` / `swapgs` 等特权指令的内联汇编必须封装在独立 `__attribute__((noinline))` 函数，并加详尽注释说明输入 / 输出约束与 Clobber List。
4. **以 OSDev 文档为准**：实现或修复任何硬件 / 架构相关功能（分页、GDT/IDT、SMP、CR4、TLB、SWAPGS、syscall、KPTI 等）时，**优先查询本地 `osdev_wiki/` 离线副本**及 `results/stepNN.md` 历史文档，按其描述的标准 / 最佳实现编写，而非凭记忆硬编码。凡文档建议开启的特性（如 SWAPGS 模型、FSGSBASE、SMEP/SMAP）应按文档采用标准做法。
5. **禁止留桩 / 推迟**：所有路线图子项必须在本轮做出真正可工作的实现并验证，不得打 TODO、不得标注「留待以后」。
6. **结果文档**：完成一项工作或阶段后，写入 `results/` 下新的 Markdown 文件（从已有最大编号 +1，不覆盖旧文件），内容须极详尽（功能 / 数据结构 / 算法 / 关键地址常量 / 文件路径 / 验证方式）。
7. **禁止外部调试器**：调试内核 / 系统**严禁使用 GDB 交互、gdbserver、外部 Python 探针脚本等**。唯一允许的诊断手段是 bash 命令行 + QEMU 自带机制（`-d int` 捕获异常、`-serial file:` 落盘日志、`-monitor`、`-display none` 看启动日志等）。

### 6.4 本地验证（PR 前必须通过）

每次贡献在提交前，必须完成以下回归（生产零 panic 是硬指标）：

```bash
# 1) 全量重建（避免增量链接残留旧对象）
make clean
make iso
make disk

# 2) 无头冒烟（串口落盘，自动化判断）
timeout 45 qemu-system-x86_64 -machine pc -cpu qemu64 -smp 4 -m 2G \
  -no-shutdown -display none -serial file:/tmp/sukios.log \
  -boot d -cdrom build/SukiOS.iso \
  -drive file=build/disk.img,format=raw,index=0,media=disk \
  -audiodev none,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0

# 3) 判读（应无 OOPS/KPF/panic，fs self-test ALL PASS，shell 就绪）
grep -iE 'OOPS|KPF|panic|HALT' /tmp/sukios.log      # 期望：无输出
grep -E 'self-test ALL PASS|system fully up' /tmp/sukios.log
tail -8 /tmp/sukios.log
```

- 若改动涉及 SMP / 调度 / 页表 / GS，务必用 `-smp 4` 多核回归，并额外验证单核回退（`make run-headless QEMU_SMP=1`）。
- 涉及磁盘 / FS 的改动，确认 `fs-server self-test ALL PASS` 且 `make disk` 重建后无 I/O error。
- **交互命令（`ls`/`cat`/`write`/`exec`）无法在无头模式自动验证**（QEMU headless 下 `sendkey` 不注入键盘中断，属 QEMU 限制）。这类功能需在 `make run` 图形窗口中人工按键验证，并在 PR 描述里说明手动测试结果。
- 若引入新的潜在竞态 / 设备差异，应真实运行足够时长（建议 ≥ 30s）观察是否稳定，而非仅看启动瞬间。

### 6.5 Issue 与 PR 内容建议

- **Bug Report**：附复现命令（QEMU 参数）、串口日志片段（`cat -v` 后的关键行）、预期行为 vs 实际行为、涉及的 commit / 分支。
- **功能 PR**：在描述中说明（a）实现的功能与数据结构；（b）对应的 OSDev 文档依据；（c）本地验证结果（无头日志 grep 结论 + 必要时人工测试结论）；（d）是否更新了 `results/stepNN.md` 与必要文档。
- 涉及第三方库升级（minimp3 / miniz / FatFs / newlib 参考）时，**必须同步更新 `NOTICE`** 中的版本与版权信息，并保持其原有许可证条款。

### 6.6 第三方组件贡献边界

- `minimp3/`、`drivers/FatFs/`、`kernel/abilities/miniz/`、`lib/newlib-4.6.0.20260123/` 为上游第三方组件，**一般不要在其内部做功能修改**；如需修复，优先以补丁 / 上游 PR 方式进行，并在 `NOTICE` 记录偏差。
- 内核为 freestanding，用户态为自供桩；不要试图把 newlib 整体编入内核（链接会缺 libc 符号）。

---

## 7. 致谢

- **OSDev 社区**（`wiki.osdev.org`，离线副本见 `osdev_wiki/`）——本项目的分页、GDT/TSS、SMP、CR4、TLB、SWAPGS、syscall 等架构实现均以 OSDev 文档为标准参考。
- **ChaN** —— FatFs 通用 FAT 文件系统模块（R0.16），使 Ring3 fs-server 能正确解析 FAT32。
- **lieff** —— minimp3（CC0 公共领域），提供 MP3 解码能力。
- **Rich Geldreich / RAD Game Tools / Valve** —— miniz（zlib 风格许可），压缩能力库。
- **newlib 贡献者**（Red Hat、UC Berkeley 等）—— 作为 freestanding 用户态实现的参考与符号来源。
- **GRUB / SeaBIOS / OVMF / QEMU** 项目 —— 提供可引导固件与验证环境。
- 所有为操作系统底层技术布道、撰写教程与开源代码的前辈与社区成员。  
- **CodeBuddy Hy3 Model** —— 提供代码生成与调试支持。
- **DeepSeek V3** —— 提供项目结构、初期选型方面的指导。
- **Xiaomi MiMo V2 Pro** —— 承担SukiOS早期版本的全部代码编写工作（现已弃用，并从代码提交历史中删除。未参与当前版本编写工作。感谢Xiaomi MiMo对项目的支持）。
- **永雏塔菲（Ace Taffy）** —— 解决了等待ai执行时无聊的问题。

---

## 7. 版权与许可证

- **SukiOS 自有代码**以 **GNU General Public License Version 3（GPL-3.0）** 发布。完整许可证文本见仓库根目录的 [`LICENSE`](./LICENSE) 文件。
- 本项目聚合了若干第三方开源组件，它们各自保留原作者 / 权利人的版权，并依其自身许可证授权。GPL-3.0 与这些组件的许可证（BSD 系列、zlib、CC0 公共领域等）兼容。
- 第三方组件的详尽版权与许可声明见仓库根目录的 [`NOTICE`](./NOTICE) 文件，分发源代码或二进制时**必须一并附带** `LICENSE` 与 `NOTICE`。

主要第三方组件一览（详见 `NOTICE`）：

| 组件 | 路径 | 许可证 |
|---|---|---|
| minimp3 | `minimp3/` | CC0 1.0 公共领域 |
| miniz | `kernel/abilities/miniz/` | zlib 风格 + 部分公共领域 |
| newlib | `lib/newlib-4.6.0.20260123/` | BSD 风格（Red Hat / UC Berkeley 等） |
| FatFs | `drivers/FatFs/` | 1-clause BSD 风格（ChaN） |

---

*本 README 随项目开发持续更新。如与代码实际状态不符，以代码与 `results/stepNN.md` 实现文档为准。*


---
&copy; 2026 SukiOS Developer Team. | built with :heart: