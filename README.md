<div align="center">
<img alt="SukiOS_Logo" src="images/EDBB1A1D-6657-433A-8707-B3212AA99754.png" width="100" height="100">
</div>

# SukiOS
![sukios](https://img.shields.io/badge/Suki-OS-00a1d6?labelColor=fb7299)
![sukios_version](https://img.shields.io/badge/version-0.0.1--beta-yellow)
![sukios_building](https://img.shields.io/badge/build-locally%20passing-brightgreen)
![sukios_usability](https://img.shields.io/badge/usable-single%20core%20stable-blue)

**x86_64 混合内核操作系统，面向真实使用。**

SukiOS 采用「混合内核（hybrid kernel）」架构：核心内核（Ring0）常驻调度器、内存管理、IPC 端口路由，以及**少量必要的裸硬件驱动**（ATA/AHCI 磁盘、Intel HDA 音频、PS/2 键鼠）；其余设备驱动（文件系统、显示合成、输入）以 **Ring3 用户态服务进程**的形态存在，彼此之间通过内核提供的 **Mach 风格消息传递（mach_msg）** 通信。

> **当前主线验证形态：单核（SMP 默认关闭）。** 单核下系统启动稳定、核心子系统自检通过、各用户态服务正常上线，可作为日常功能验证与使用的基础。多核（SMP）为编译期可选项（`make ... SMP=1`），默认不开启。

---

## 目录

1. [当前状态](#1-当前状态)
2. [已实现功能](#2-已实现功能)
3. [尚未实现 / 早期](#3-尚未实现--早期)
4. [如何编译与运行](#4-如何编译与运行)
   - 4.1 [环境依赖](#41-环境依赖)
   - 4.2 [构建命令](#42-构建命令)
   - 4.3 [运行（QEMU）](#43-运行qemu)
   - 4.4 [使用须知与限制](#44-使用须知与限制)
5. [项目结构速览](#5-项目结构速览)
6. [向仓库贡献](#6-向仓库贡献)
7. [致谢](#7-致谢)
8. [版权与许可证](#8-版权与许可证)

---

## 1. 当前状态

- **架构目标**：x86_64 物理机 / QEMU 虚拟机，64 位 long mode，4 级分页。
- **稳定基线**：单核（`CONFIG_SMP=0`，默认）已被确立为主验证形态。所有功能开发与回归均以单核为准，确保「生产场景零 panic」。
- **安全地基**：SMAP/SMEP、NX、UMIP、KPTI、栈金丝雀、用户指针 `copy_from_user`/`copy_to_user` 围栏、KASLR、IST 守卫栈均在启动早期装配。
- **GUI 起点**：已实现 display_server（帧缓冲独占 + 终端栅格化 + 鼠标光标绘制）。TTF 字形渲染由 **FreeType 字体服务**（`lib/freetype-2.14.3`，FTL 许可）承担，界面字体采用 `ResourceHanRoundedCN-Medium.ttf`（资源圆体，OFL-1.1）。

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
| 1 | `sys_task_create` / `sys_task_spawn` | 创建 / 派生用户态任务 |
| 2 | `sys_task_exit` | 任务退出 |
| 3 | `sys_yield` | 主动让出 CPU |
| 4 | `SYS_DEBUG_WRITE` | 调试输出（经 `copy_from_user` 校验） |
| 5 | `SYS_INPUT_READ` | 非阻塞取原始键盘扫描码 |
| 6 | `SYS_REBOOT` | 重启 / ACPI S5 关机 |
| 7 | `SYS_PORT_CLAIM` | 用户态认领端口接收权（IPC 能力） |
| 8 | `SYS_EXECVE` | 加载并执行 ELF 映像 |
| 9 | `SYS_CHDIR` | 改变进程工作目录 |
| 100~203 | POSIX 层 | `open/read/write/close/lseek/stat/mkdir/rename/unlink` 等完整 POSIX ABI（见 `kernel/syscall/sys_posix.c`） |
| 204 | `SYS_MOUSE_READ` | 非阻塞取内核 IRQ12 采集的鼠标事件（Ring3 鼠标驱动用） |

---

## 2. 已实现功能

> 以下均已在 QEMU（i440FX / qemu64，**单核**）实证，启动零 panic、核心子系统自检通过。

### 2.1 启动与底层体系
- **引导**：Multiboot2 头（GRUB 加载）+ Xen PVH 直启（`-kernel` 路径）双通道；`.boot` 段严格落在 1 MiB 且 Multiboot2 头位于首 32 KiB 内。
- **CPU 模式**：16 位实模式 → 32 位保护模式 → 64 位 long mode 完整切换。
- **GDT / TSS / IDT**：每 CPU 独立 TSS（Ring3→Ring0 用 `TSS.RSP0` 装载内核栈）；IDT 覆盖异常与中断向量，IST 守卫栈。
- **异常与诊断**：`#PF`/`#GP`/`#UD` 精确报告 RIP / 错误码 / CR3；`WARN_ON` 打印 + 调用链回溯。
- **SMP（可选）**：基于 ACPI MADT 发现多核，编译期 `SMP=1` 开启；默认单核，单核下 IPI_RESCHED 以 self-IPI 承担即时调度。

### 2.2 内存管理
- **PMM**：基于 boot 期内存探测的页帧分配器，含引用计数（OOL 共享页粘滞）。
- **VMM**：4 级分页、独立地址空间、按需分页（VMA 链表）、COW fork、`munmap`、`mprotect` 边界。
- **KPTI**：用户态 / 内核态分离页表（CR3 bit12 切换），内核段正确映射进影子视图。
- **KASLR**：内核代码段随机化（两阶段链接）；用户栈 ASLR。
- **内核堆**：`kmalloc` / `kzalloc` / `kfree`。

### 2.3 进程与调度
- **任务模型**：`task_t` 支持内核 / 用户任务、idle、zombie 回收。
- **调度器**：单一全局运行队列 + 每任务 `cpu` 占用标志（原子 CAS），杜绝双核同跑一任务竞态；RR 时间片抢占；`context_switch` 正确保存/恢复 callee-saved、切换 RSP/CR3、恢复 per-CPU GS。
- **syscall / 中断上下文**：`syscall_entry.S` 与 `isr.S` 均按 OSDev SWAPGS 标准配对，消除 GS 翻转态跨任务丢失。

### 2.4 IPC（Mach 风格）
- 全局端口表 `kernel_port_t`；`mach_msg_send` / `mach_msg_recv`；发送/接收权能力检查（`port_claim`）。
- 小消息内核 memcpy 转发；大消息 OOL 物理页重映射零拷贝（引用计数 +1）。

### 2.5 设备驱动（Ring0 裸驱动）
- **ATA / IDE PIO**（i440FX 传统 IDE）与 **AHCI**（SATA，DMA + 中断），probe 优先级 AHCI > ATA。
- **Intel HDA** 音频控制器（ICH6 兼容）输出编解码器。
- **键盘**：PS/2 键盘经 I/O APIC 投递（`input_server` 经 `SYS_INPUT_READ` 取扫描码）。
- **鼠标**：PS/2 鼠标经 IRQ12（IOAPIC GSI12）采集 3/4 字节包，内核侧解析后经 `SYS_MOUSE_READ` 派发；Ring3 `mouse_server` 拉包并经 `DISPLAY_PORT` 发光标事件。
- **PCI**：ECAM（MCFG）探测，缺失时 PIO 回退；`_PRT` 路由、MSI 编程。
- **ACPI**：RSDP/XSDT/MADT/HPET/MCFG 解析；LAPIC / IOAPIC 取代 8259 PIC + PIT；TSC 校准。
- **RTC**：`kernel/time/rtc.c` 读取 CMOS 实时时钟。

### 2.6 用户态服务（Ring3 进程）
- **disk-srv**（内核线程）：响应 `DISK_PORT`，真实扇区读写。
- **fs-server**：用 **FatFs（ChaN R0.16）** 做 FAT32 解析；支持 create/write/append/read_at/read_file（OOL）/mkdir/rename/truncate/unlink/list，LFN 正确，整条路径经真实磁盘 IO 自检（self-test ALL PASS）。
- **input-server**：转发键盘输入。
- **display-server**：**已实现**。独占帧缓冲、栅格化终端文本（内嵌 8x8 字体）、绘制鼠标光标（像素快照法）；内核经 `SYS_DISPLAY_READY`/IPC 转发控制台输出避免覆盖桌面。后续界面字体将采用 `ResourceHanRoundedCN-Medium.ttf`。
- **mouse-server**：Ring3 鼠标驱动（`.kdr` 形态，临时内嵌 spawn），经 `SYS_MOUSE_READ` 拉包、累计坐标、发光标事件到 display-server。
- **shell**：bash 风格交互式命令行。已实现：历史记录（**内存环形缓冲，最多 100 条，超出丢弃最旧，不落盘**；↑/↓ 滚动）、**Tab 文件名补全**（唯一匹配直接补全、目录补 `/`、多匹配补公共前缀并列出候选）、**行内光标编辑**（←/→ 移动光标、Home/End 跳行首行尾、Backspace 删前、Delete 删后、Enter 提交）、管道 `|` 与重定向 `>`、`$VAR`/`$?` 变量展开、内建命令（`help`/`echo`/`cat`/`ls`/`cd`/`pwd`/`mkdir`/`touch`/`rm`/`write`/`date`/`whoami`/`ps`/`ports`/`portclaim`/`exec`/`spawn`/`clear`/`reboot`）。方向键由 input_server 把 PS/2 扫描码（e0 前缀）编码为 ANSI 转义序列送达。
- **BMP 加载器**（`user/apps/bmploader.c`）：流式读取 BMP 并经内核 `SYS_DISPLAY_BLIT` 通道 blit 到帧缓冲。
- **独立用户程序**（FAT32 磁盘 `::BIN/`）：`hello`、`playaudio`（minimp3 MP3 解码）、`audiotest`。
- **posixtest**：POSIX 系统调用层自检程序。

### 2.7 VFS 路由层
- 内核 `vfs.c` 做最长前缀挂载解析：`/`→DISK（FAT32）、`/tmp`+`/run`→TMPFS、`/dev`→DEVFS。
- `tmpfs.c`：内存文件系统（写读自检通过）。
- `devfs.c`：设备文件系统，提供 `/dev/null`、`/dev/zero`、`/dev/urandom`、`/dev/console`（selftest 验证读写正确，修复了 devfs 句柄反算越界导致的 EBADF）。
- `fd.c`：统一文件描述符路由层，按 backend 分派到 DISK/TMPFS/DEVFS。

### 2.8 安全
- SMAP / SMEP / NX / UMIP 在 `security_init()` 装配；用户指针强制 `copy_from_user` / `copy_to_user`。
- 栈金丝雀（`-fstack-protector-strong` + 全局 guard）。
- KASLR + KPTI 已部署。

### 2.9 构建与启动
- ISO（BIOS+UEFI 双启动，GRUB）、FAT32 磁盘镜像（mtools）、QEMU 运行目标（`run` / `run-headless` / `run-ahci` / `run-uefi` / `run-q`）。
- 交叉工具链（`x86_64-elf-gcc`）自动探测，缺失回退主机 `gcc` + `-ffreestanding`。

### 2.10 Rust 开发工具链与运行时
- **一键环境**：`make make-rust-env`（别名 `rust-env`）自动探查/安装 rustup（nightly + `rust-src`）、构建 SukiOS 用户态运行时静态库 `libsuki.a`（来自 `user/lib/*.c`，提供 `malloc/free/pthread/syscall` 等），并生成 `rust/x86_64-sukios.json` 自定义目标规格与 `rust/.cargo/config.toml`（指向本仓库交叉链接器 `x86_64-sukios-elf-gcc` + `user/user.ld` + `libsuki.a`）。详见 `results/step69.md`。
- **编译示例**：`make rust-build`（等价 `cd rust && cargo build`）产出 `rust/target/x86_64-sukios/debug/sukios-hello` —— 一个合法的 SukiOS ELF（ET_EXEC，入口 `0x400000`）。
- **运行时验证（开机自检）**：该 Rust ELF 作为**内核内嵌 blob** 在 `boot_late_init` 由 `task_create_user()` 确定性 spawn（与 `posixtest` 同款路径，不依赖 shell）；其 `_start` 经标准 x86_64 `syscall` 指令（System V AMD64 ABI，`%rax`=号、`%rdi/%rsi/%rdx/%r10/%r8/%r9`=参数）调用 `SYS_DEBUG_WRITE(4)` 打印 `hello from rust on SukiOS`、再调 `SYS_TASK_EXIT(2,0)`，内核串口日志可见 `RUSTHELLO` 装载于 `0x400000`（W^X）、退出码 0、零 panic。详见 `results/step70.md`。
- **磁盘路径**：`make disk` 会（在 rust 二进制存在时）把它拷入 FAT32 磁盘 `::BIN/RUSTHELLO.SKA`，可在 shell 里 `exec BIN/rusthello` 经真实「从磁盘 exec」路径运行。
- 当前 Rust 支持为 **`#![no_std]` + 自定义 bare-metal 目标 + 经 FFI 复用 `libsuki.a`**；Rust `std` 库（Servo 前置）尚未移植（见 §3）。

---

## 3. 尚未实现 / 早期

> 以下为当前明确**未实现 / 早期**的部分，列出以避免误用。

- **网络栈**：完全未实现。无 virtio-net / Intel 网卡驱动，无 TCP/IP、UDP、socket。
- **真正的 GUI 应用框架**：已有 display-server 基础（终端栅格化 + 光标），但**无窗口系统 / 合成器 / 应用离屏 Buffer 合成管线**；TTF 字形渲染已接入：由 `fontsrv` 字体服务加载 FreeType 渲染字形位图，经 IPC 供 `pchfnt` 等程序绘制到帧缓冲（详见 `results/step53.md`）。
- **存储**：仅 FAT32 经 FatFs；无 ext2/3/4、exFAT、NTFS、ISO9660（除引导 ISO 外）。
- **多文件系统 / 多磁盘 / GPT**：仅识别首个磁盘首分区 FAT32。
- **完整 POSIX 语义**：无 fork（仅有 spawn/execve 模型）、无信号、无 pthread、无 swap、无 huge page、无 NUMA。
- **电源管理**：仅重启与 ACPI S5 关机；无睡眠 / 休眠 / 调频。
- **用户态隔离强化**：进程间靠端口能力粗粒度隔离，无完整能力 / 沙箱模型（无 seccomp 类机制）。
- **kdr 动态加载器**：`.kdr` 内核模块 / 用户态驱动的动态装载机制尚未实现；当前鼠标驱动以内嵌 spawn 形式运行，待加载器就绪后改为动态装载（内核侧无需改动）。
- **真实硬件适配广度**：主要在 QEMU 验证；未在现代物理机、不同 AHCI/网卡型号上系统测试。
- **多核调度策略**：开启 SMP 时为对称 RR；无 CFS / 优先级继承 / 负载均衡迁移。
- **Rust 标准库（`std`）未移植**：当前 Rust 支持为 `#![no_std]` + 自定义 bare-metal 目标（`rust/x86_64-sukios.json`）+ 经 FFI 复用 `libsuki.a`（提供 `malloc`/`pthread`/`syscall` 等底层能力）。`rust-src` 组件已随 `make make-rust-env` 安装，但 `library/std` 的 `os="sukios"` 后端（build-std 编译 std）尚未实现，故暂不能使用 `#[std]` 生态；Servo 等重型 Rust 应用移植需此能力，列为后续里程碑。

---

## 4. 如何编译与运行

### 4.1 环境依赖

| 依赖 | 用途 | 备注 |
|---|---|---|
| `gcc`（或 `x86_64-elf-gcc` 交叉编译器） | 编译内核 / 用户态 | 自动探测交叉链，缺失回退主机 gcc |
| `make` | 构建编排 | — |
| `grub-mkrescue` | 生成可引导 ISO | 需 `grub-pc` / `grub-efi` 模块 |
| `mtools`（`mformat`/`mcopy`/`mmd`） | 生成 FAT32 磁盘镜像 | `make disk` 依赖 |
| `qemu-system-x86_64` | 运行模拟器 | 建议 `/dev/kvm` 启用硬件加速 |
| `python3` | KASLR 重定位表生成（`tools/gen_relk.py`） | 构建期依赖 |
| Rust 工具链（`rustup`/`rustc`/`cargo`，nightly） | 构建 Rust 组件（`make make-rust-env` 自动安装） | 仅开发 Rust 程序时需要；纯 C 内核/服务构建不依赖 |
| （可选）`OVMF`（`/usr/share/OVMF/OVMF_CODE_4M.fd`） | UEFI 启动 | `make run-uefi` 需要 |

### 4.2 构建命令

```bash
# 1) 清理（会删除 build/ 下全部产物，含 disk.img；用 make clean，勿用 rm -rf）
make clean

# 2) 编译内核 + 全部用户态服务/程序，生成 build/SukiOS.iso
make iso

# 3) 生成 FAT32 磁盘镜像（含 README/HELLO/ROADMAP、::SYS、::BIN/*、MOONHALO.MP3）
make disk
```

> **默认即为单核**：`make iso` / `make run` 等目标默认 `CONFIG_SMP=0`（单核）。如需多核，用 `make iso SMP=1`（同时 QEMU 自动 `-smp 4`），但当前主线验证以单核为准。

常用变体：

```bash
make all              # 仅编译内核 ELF
make run              # 图形窗口运行（推荐人工交互测试 shell / 鼠标 / 音频）
make run-headless    # 无头运行，仅串口（自动化验证用）
make run-ahci        # 磁盘挂 AHCI（DMA+中断）验证
make run-uefi        # OVMF UEFI 启动验证
make run-q           # PVH 直启（qemu -kernel，不经 GRUB）
make info            # 打印当前工具链/对象信息
```

#### Rust 程序开发（可选，步骤见 `results/step69.md`）
```bash
make make-rust-env   # 一键安装 rustup(nightly)+rust-src、构建 libsuki.a、生成目标规格
make rust-build      # cd rust && cargo build -> rust/target/x86_64-sukios/debug/sukios-hello
make disk            # 把 rust 二进制拷入 ::BIN/RUSTHELLO.SKA（存在时）
make run-headless    # 开机自检自动 spawn RUSTHELLO，串口见 "hello from rust on SukiOS"
```
> 仅构建/运行纯 C 内核与用户态服务**不需要** Rust 工具链；Rust 为可选项，缺失时 `make iso`/`make disk` 忽略 Rust 部分并正常产出。

### 4.3 运行（QEMU）

```bash
# 图形窗口（推荐用于手动交互：shell 命令、鼠标光标、音频）
make run

# 无头（串口落盘，配合 cat -v / tail / grep 看日志做自动化判断）
make run-headless
```

启动成功后串口日志应出现类似：

```
[boot] SukiOS kernel entered (long mode, higher half).
[console] UTF-8 test: 中文显示正常 ✓ 操作系统启动成功
[mm] selftest ...
[mouse] PS/2 mouse ready (IOAPIC GSI12 -> IRQ12, std)
[fs] FS_SERVER starting
[fs] mounted FAT32 (FatFs)
[fs] self-test ALL PASS
[display] fb mapped 1280x720@32
SukiOS>
```

在 `make run` 图形窗口中：可看到桌面终端文本与白色鼠标箭头光标，移动鼠标光标跟随、左键按下光标尖端现红点。

### 4.4 使用须知与限制

1. **单核为稳定主线**：默认 `make` 即单核，启动零 panic、各服务正常。多核（`SMP=1`）为实验特性，未经生产场景充分回归。
2. **仅 QEMU 充分验证**：功能在 QEMU（i440FX / q35 / OVMF）实证；真实物理机 / 不同硬件可能遇未适配差异。
3. **硬件加速影响音频**：minimp3 解码在 KVM 下实时，纯 TCG 下约 1/8 实时，音频必然断续（仿真性能限制，非系统缺陷）。
4. **headless 下键盘不可注入**：QEMU headless 的 `sendkey` **不注入键盘中断**（QEMU 限制），故 shell 交互命令（`ls`/`cat`/`write`/`exec`）**无法在无头模式自动验证**，须 `make run` 图形窗口人工按键。
5. **headless 下鼠标不移动**：QEMU `-display none` 不向 PS/2 鼠标投递物理移动（monitor `mouse_move` 依赖图形后端），故真实鼠标移动的图形验证须在 `make run` 图形窗口人工操作。
6. **构建需完整链路**：`make iso` 须配合 `make disk` 才有 FAT32 磁盘；改源码后若异常，先 `make clean && make iso && make disk` 全量重建。
7. **KASLR 两阶段链接**：若 `tools/gen_relk.py` 或 Python 缺失，链接阶段会失败。
8. **第三方库**：`minimp3/`、`drivers/FatFs/`、`kernel/abilities/miniz/`、`lib/freetype-2.14.3/`、`resources/ResourceHanRoundedCN-Medium.ttf` 等均为第三方组件，版权见 `NOTICE`。

---

## 5. 项目结构速览

```
SukiOS/
├── boot/                  # 引导：boot.S、multiboot2 头、PVH 入口、linker.ld
├── kernel/
│   ├── arch/x86_64/       # isr.S、syscall_entry.S、switch.S、percpu.c、
│   │                      #   gdt.c、smp.c、security.c、pvh.c、keyboard.c、mouse.c
│   ├── mm/                # PMM / VMM / kmalloc / vma
│   ├── sched/             # 调度器（sched.c）、上下文切换（switch.S）
│   ├── ipc/               # Mach 端口与 mach_msg
│   ├── syscall/           # 系统调用分发（syscall.c）+ POSIX 层（sys_posix.c）
│   ├── acpi/              # ACPI 解析、LAPIC/IOAPIC、HPET、PCI
│   ├── fs/                # vfs.c / tmpfs.c / devfs.c / fd.c 路由层
│   ├── drivers/           # Ring0 裸驱动：ata.c、ahci.c、hda.c、keyboard.c
│   ├── time/              # rtc.c（CMOS 实时时钟）
│   ├── elf/               # ELF64 加载（execve 用）
│   ├── abilities/         # 用户态能力库（miniz 压缩，依赖 libc，不入内核）
│   ├── kmain.c            # 内核 C 主入口
│   └── gdbstub.c          # 串口 GDB 远程调试 stub
├── user/                  # Ring3 服务与程序（C，freestanding）
│   ├── fs_server.c        # FAT32 服务（FatFs + DISK_PORT）
│   ├── input_server.c     # 键盘输入服务
│   ├── display_server.c   # 显示合成服务（帧缓冲独占 + 终端栅格化 + 光标）
│   ├── mouse_server.c     # 鼠标驱动（Ring3 .kdr 形态）
│   ├── shell.c            # 交互 shell
│   ├── apps/              # 独立程序：hello / playaudio / audiotest / bmploader
│   └── lib/               # 用户态 freestanding 桩：crt0 / suki.c / libc / suki.h
├── drivers/FatFs/         # ChaN FatFs R0.16
├── minimp3/               # minimp3 解码库（CC0，playaudio 用）
├── resources/             # ResourceHanRoundedCN-Medium.ttf（界面字体，OFL-1.1）
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

欢迎以 Issue、讨论、文档修订或 Pull Request 参与 SukiOS 开发。本节详述贡献流程、代码规范与本地验证要求，目的是让合并后的内核在单核 QEMU 生产场景下**零 panic**、架构行为**严格符合 OSDev 标准**。

### 6.1 开发环境准备

```bash
git clone https://github.com/bil-fis/SukiOS SukiOS && cd SukiOS
make info                 # 打印 CC / 工具链 / 对象列表
which grub-mkrescue mformat mcopy mmd qemu-system-x86_64 python3
```

- 推荐启用 KVM（`/dev/kvm` 存在）获得接近真实的运行速度。
- 构建与运行**不需要** `source` 任何 env 脚本；构建环境由 `Makefile` 自身处理。
- 清理构建产物请用 `make clean`，勿用 `rm -rf build`。

### 6.2 分支与提交约定

- 主分支为 `master`。功能开发请在独立分支进行，经评审后合并；勿强推已发布历史。
- 提交信息采用**简短英文前缀 + 中文描述**，例如：
  - `sched: 重写全局运行队列消除双核同跑竞态`
  - `audit-fix: 修复 AP idle 切栈误 panic (H1 H2)`
  - `fatfs: fs-server 改用 FatFs R0.16 对接 DISK_PORT`
- 每条提交应**自洽且可工作**：能编译、能在单核 QEMU 跑通，不夹带半成品 / 留桩 / TODO。
- 提交后**默认不推送远程**，除非维护者明确要求；推送前确保已 `git pull --rebase`。

### 6.3 编码与架构规范（强制）

1. **文件路径精确**：涉及代码改动时标注完整绝对路径（如 `kernel/arch/x86_64/syscall/syscall.c`）。
2. **内存安全**：从用户态接收的指针在 syscall 处理中**严禁直接解引用**，必须 `copy_from_user` / `copy_to_user`。
3. **内联汇编隔离**：`wrmsr` / `lgdt` / `ltr` / `swapgs` 等特权指令内联汇编必须封装在独立 `__attribute__((noinline))` 函数并加详尽注释。
4. **以 OSDev 文档为准**：硬件 / 架构相关功能优先查本地 `osdev_wiki/` 离线副本及 `results/stepNN.md`，按其标准 / 最佳实现编写。
5. **禁止留桩 / 推迟**：所有路线图子项必须本轮做出真正可工作的实现并验证。
6. **结果文档**：完成一项工作后写入 `results/` 新 Markdown（从已有最大编号 +1），内容极详尽。
7. **禁止外部调试器**：调试内核严禁 GDB 交互 / gdbserver / 外部 Python 探针；仅用 bash + QEMU 自带机制（`-d int`、`-serial file:`、`-monitor`、`-display none`）。

### 6.4 本地验证（PR 前必须通过）

```bash
# 1) 全量重建
make clean && make iso && make disk

# 2) 单核无头冒烟（串口落盘）
timeout 45 qemu-system-x86_64 -machine pc -cpu qemu64 -smp 1 -m 2G \
  -no-shutdown -display none -serial file:/tmp/sukios.log \
  -boot d -cdrom build/SukiOS.iso \
  -drive file=build/disk.img,format=raw,index=0,media=disk

# 3) 判读（应无 panic，fs self-test ALL PASS，各服务就绪）
grep -iE 'panic|triple|#gp|#pf|DOUBLE FAULT' /tmp/sukios.log   # 期望：无输出
grep -E 'self-test ALL PASS|system fully up' /tmp/sukios.log
tail -8 /tmp/sukios.log
```

- 若改动涉及 SMP / 调度 / 页表 / GS，额外验证单核回退（默认即单核）及可选 `SMP=1` 多核。
- 涉及磁盘 / FS 的改动，确认 `fs-server self-test ALL PASS` 且 `make disk` 重建无 I/O error。
- 交互命令（`ls`/`cat`/`write`/`exec`）与鼠标移动无法在无头模式自动验证，须在 `make run` 图形窗口人工测试并在 PR 描述说明。

### 6.5 第三方组件贡献边界

- `minimp3/`、`drivers/FatFs/`、`kernel/abilities/miniz/`、`lib/freetype-2.14.3/`、`resources/ResourceHanRoundedCN-Medium.ttf` 为上游第三方组件，**一般不要在其内部做功能修改**；如需修复优先以上游 PR 方式进行，并在 `NOTICE` 记录偏差。
- 内核为 freestanding，用户态为自供桩；不要把 newlib 整体编入内核。

---

## 7. 致谢

- **OSDev 社区**（`wiki.osdev.org`）——分页、GDT/TSS、SMP、CR4、TLB、SWAPGS、syscall 等架构实现的标准参考。
- **ChaN** —— FatFs 通用 FAT 文件系统模块（R0.16）。
- **lieff** —— minimp3（CC0 公共领域），MP3 解码能力。
- **Rich Geldreich / RAD Game Tools / Valve** —— miniz（zlib 风格许可）。
- **Cyano Hao** —— Resource Han Rounded（资源圆体，OFL-1.1），界面字体。
- **FreeType Project** —— FreeType 字体光栅化引擎（FTL 许可），`lib/freetype-2.14.3/`，驱动 TTF 字形渲染。
- **newlib 贡献者**（Red Hat、UC Berkeley 等）—— freestanding 用户态实现参考。
- **GRUB / SeaBIOS / OVMF / QEMU** —— 可引导固件与验证环境。
- **CodeBuddy** —— 代码生成与调试支持。
- **DeepSeek** —— 项目结构、初期选型指导。
- **永雏塔菲（Ace Taffy）** —— 解决了等待 ai 执行时无聊的问题。

---

## 8. 版权与许可证

- **SukiOS 自有代码**以 **GNU General Public License Version 3（GPL-3.0）** 发布。完整许可证文本见 [`LICENSE`](./LICENSE)。
- 本项目聚合了若干第三方开源组件，各自保留原作者 / 权利人版权，依其自身许可证授权。GPL-3.0 与这些组件（BSD 系列、zlib、CC0、OFL-1.1 等）兼容。
- 第三方组件详尽版权与许可声明见 [`NOTICE`](./NOTICE)，分发源代码或二进制时**必须一并附带** `LICENSE` 与 `NOTICE`。

主要第三方组件一览（详见 `NOTICE`）：

| 组件 | 路径 | 许可证 |
|---|---|---|
| minimp3 | `minimp3/` | CC0 1.0 公共领域 |
| miniz | `kernel/abilities/miniz/` | zlib 风格 + 部分公共领域 |
| newlib（参考，不入库） | `lib/newlib-4.6.0.20260123/` | BSD 风格（Red Hat / UC Berkeley 等） |
| FatFs | `drivers/FatFs/` | 1-clause BSD 风格（ChaN） |
| Resource Han Rounded | `resources/ResourceHanRoundedCN-Medium.ttf` | SIL OFL-1.1（Cyano Hao） |
| Rust 工具链（rustup / rustc / cargo） | 本地安装（不随仓库分发） | MIT OR Apache-2.0（Rust Project Developers） |

---

*本 README 随项目开发持续更新。如与代码实际状态不符，以代码与 `results/stepNN.md` 实现文档为准。*

&copy; 2026 SukiOS Developer Team. | built with :heart:
