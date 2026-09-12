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

**系统调用表（号位分区，详见 `include/sukios/posix.h` 与 `kernel/syscall/syscall.c`）：**

| 号段 | 类别 | 代表性调用（编号） |
|---|---|---|
| 0–19 | SukiOS / Mach 原生 | `sys_mach_msg`(0) `sys_task_spawn`(1) `sys_task_exit`(2) `sys_yield`(3) `SYS_DEBUG_WRITE`(4) `SYS_INPUT_READ`(5) `SYS_REBOOT`(6) `SYS_PORT_CLAIM`(7) `SYS_EXECVE`(8) `SYS_WAIT`(9) `SYS_AUDIO_*`(10–13) `SYS_MMAP_LEGACY`(14) `SYS_MUNMAP_LEGACY`(15) `SYS_SERIAL_READ`(16) `SYS_FORK`(17) `SYS_GETPID`(18) `SYS_GETPPID`(19) |
| 20–39 | 进程 / 调度 / 资源（POSIX） | `waitpid`(20) `kill`(21) `getuid/getgid`(22–27) `brk/sbrk`(28–29) `times`(30) `umask`(31) `gettid`(32) `exit_group`(33) `arch_prctl`(35) `futex`(36) `getrlimit/setrlimit`(37–38) `getrusage`(39) |
| 40–89 | 文件与目录 I/O（POSIX） | `open/close/read/write/lseek`(40–44) `stat/fstat/lstat`(45–47) `unlink/mkdir/rmdir`(48–50) `opendir/readdir/closedir`(51–53) `dup/dup2`(54–55) `fcntl`(56) `access`(57) `rename`(58) `truncate`(59–60) `chdir/getcwd`(61–62) `pipe`(63) `ioctl`(64) `link/symlink/readlink`(65–67) `chmod/fchmod`(68–69) `sync/fsync`(70–71) `utimes`(72) `getdents`(73) `statfs`(75–76) `pread/pwrite`(77–78) `readv/writev`(79–80) `select/poll`(81–82) `realpath`(83) `mknod`(84) `chown/fchown`(85–86) `telldir/seekdir`(87–88) `fpathconf`(89) |
| 90–99 | 内存映射（POSIX） | `mmap`(90) `munmap`(91) `mprotect`(92) `msync`(93) `madvise`(94) `mincore`(95) `mremap`(96) `SYS_PORT_ALLOC`(97) `SYS_PORT_FREE`(98) |
| 100–109 | 时间（POSIX） | `clock_gettime`(100) `clock_settime`(101) `clock_getres`(102) `gettimeofday`(103) `nanosleep`(104) `time`(105) `settimeofday`(106) `alarm`(107) `getitimer/setitimer`(108–109) |
| 110–129 | 系统信息 / 动态链接 / IPC | `uname`(110) `sysinfo`(111) `getrandom`(112) `sysconf`(113) `prctl`(114) `gethostname/sethostname`(115–116) `getpgrp/setpgrp`(117–118) `getsid`(119) `nice`(120) `getpriority/setpriority`(121–122) `SYS_OOL_UNMAP`(125) `SYS_DL_OPEN`(126) `SYS_DL_SYM`(127) `SYS_DL_CLOSE`(128) `SYS_DL_ERROR`(129) |
| 130–149 | **SukiNative 原生对象 API** | `OBJ_CREATE/DESTROY/DUPLICATE/QUERY`(130–133) `WAIT`(134) `EVENT_CREATE/SET/RESET`(135–137) `MUTEX_CREATE/LOCK/UNLOCK`(138–140) `SEM_CREATE/ACQUIRE/RELEASE`(141–143) **已实现**；`FILE_*`(144–147) `PROC_CREATE`(148) `MEM_ALLOC`(149) 预留（返回 `-ENOSYS`，Phase 2） |
| 150–199 | **网络 socket 子系统** | `socket`(150) `bind`(151) `connect`(152) `listen`(153) `accept`(154) `sendto/recvfrom`(155–156) `sendmsg/recvmsg`(157–158) `shutdown`(159) `setsockopt/getsockopt`(160–161) `getpeername/getsockname`(162–163) `socketpair`(164) `send/recv`(165–166) …（其余预留）。内核仅把调用翻译成 NS_PORT 的 IPC 消息，由 `net_server`（lwIP）执行，**不解析任何网络协议** |
| 200– | 内核扩展 | `SYS_FRAMEBUFFER_MAP`(200) `SYS_DISPLAY_READY`(201) `SYS_CONSOLE_READ`(202) `SYS_DISPLAY_BLIT`(203) `SYS_MOUSE_READ`(204) `SYS_CLONE`(205) `SYS_SIGACTION`(206) `SYS_SIGRETURN`(207) `SYS_SIGPROCMASK`(208) `SYS_TKILL`(209) `SYS_RAISE`(210) |

> 调用约定统一为 System V AMD64：`%rax`=号，`%rdi/%rsi/%rdx/%r10/%r8/%r9`=参数，返回值 `%rax`（失败为 `-errno`）。所有来自用户态的指针必须经 `copy_from_user` / `copy_to_user` 访问（项目强制规则）。POSIX 层（20–129）对外行为兼容 Linux/Unix，老程序（Servo 等只调 20–129）不受影响；SukiNative（130–149）是新服务可直接使用的原生接口。

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
- **线程与信号**：支持线程（`SYS_CLONE` 在共享地址空间造新任务跳入 trampoline，pthread 基座）+ 同步原语（`SYS_FUTEX` WAIT/WAKE，pthread 基础集）；基础信号框架（`SIGACTION` / `SIGRETURN` / `SIGPROCMASK` / `TKILL` / `RAISE`）已实现，`posixtest` 多线程用例验证通过。

### 2.4 IPC（Mach 风格）
- 全局端口表 `kernel_port_t`；`mach_msg_send` / `mach_msg_recv`；发送/接收权能力检查（`port_claim`）。
- 小消息内核 memcpy 转发；大消息 OOL 物理页重映射零拷贝（引用计数 +1）。

### 2.5 设备驱动（Ring0 裸驱动）
- **ATA / IDE PIO**（i440FX 传统 IDE）与 **AHCI**（SATA，DMA + 中断），probe 优先级 AHCI > ATA。
- **Intel HDA** 音频控制器（ICH6 兼容）输出编解码器。
- **键盘**：PS/2 键盘经 I/O APIC 投递（`input_server` 经 `SYS_INPUT_READ` 取扫描码）。
- **鼠标**：PS/2 鼠标经 IRQ12（IOAPIC GSI12）采集 3/4 字节包，内核侧解析后经 `SYS_MOUSE_READ` 派发；Ring3 `mouse_server` 拉包并经 `DISPLAY_PORT` 发光标事件。
- **网络（Intel 82540EM / e1000）**：Ring0 裸驱动（`kernel/drivers/e1000.c`），经 PCI class `0x02/subclass 0x00` 探测，DMA + 中断收发以太网帧，**只做帧搬运、不解析 IP/TCP/UDP**；原始帧经 `NET_PORT` IPC 交给用户态 `net_server`（lwIP 2.2.1）处理。启动自检会发 ARP 请求并收到 QEMU user 后端（网关 `10.0.2.2` / DNS `10.0.2.3`）应答，端到端验证 TX/RX 通路；`nettest` 开机自检进一步验证 socket 通路。
- **PCI**：ECAM（MCFG）探测，缺失时 PIO 回退；`_PRT` 路由、MSI 编程。
- **ACPI**：RSDP/XSDT/MADT/HPET/MCFG 解析；LAPIC / IOAPIC 取代 8259 PIC + PIT；TSC 校准。
- **RTC**：`kernel/time/rtc.c` 读取 CMOS 实时时钟。

### 2.6 用户态服务（Ring3 进程）
- **disk-srv**（内核线程）：响应 `DISK_PORT`，真实扇区读写。
- **fs-server**：用 **FatFs（ChaN R0.16）** 做 FAT32 解析；支持 create/write/append/read_at/read_file（OOL）/mkdir/rename/truncate/unlink/list，LFN 正确，整条路径经真实磁盘 IO 自检（self-test ALL PASS）。
- **input-server**：转发键盘输入。
- **display-server**：独占帧缓冲、栅格化终端文本（内嵌 8x8 字体）、绘制鼠标光标（像素快照法）；内核经 `SYS_DISPLAY_READY`/IPC 转发控制台输出避免覆盖桌面。界面字体经 `fontsrv`（FreeType）渲染。
- **mouse-server**：Ring3 鼠标驱动（`.kdr` 形态，临时内嵌 spawn），经 `SYS_MOUSE_READ` 拉包、累计坐标、发光标事件到 display-server。
- **net-server**：网络服务（`user/net_server.c`）。作为 `NET_PORT` 客户端收发原始帧（交给 Ring0 e1000 驱动），并作为 `NS_PORT` 服务端用 **lwIP 2.2.1**（raw API，`NO_SYS=1`）实现 TCP/IP 协议栈与 DHCP；内核 150–166 socket syscall 经 IPC 转发到此。启动即自动获取 IP（QEMU user-net 网关 `10.0.2.2`），证明「e1000 → NET_PORT → lwIP」全链路打通。
- **fontsrv**：字体服务（`user/fontsrv.c`）。加载 `ResourceHanRoundedCN-Medium.ttf`，经 FreeType 光栅化字形位图，经 `FONT_PORT` IPC 提供给 display-server / `pchfnt` 等程序绘制到帧缓冲。
- **shell**：bash 风格交互式命令行。已实现：历史记录（**内存环形缓冲，最多 100 条，超出丢弃最旧，不落盘**；↑/↓ 滚动）、**Tab 文件名补全**（唯一匹配直接补全、目录补 `/`、多匹配补公共前缀并列出候选）、**行内光标编辑**（←/→ 移动光标、Home/End 跳行首行尾、Backspace 删前、Delete 删后、Enter 提交）、管道 `|` 与重定向 `>`、`$VAR`/`$?` 变量展开、内建命令（`help`/`echo`/`cat`/`ls`/`cd`/`pwd`/`mkdir`/`touch`/`rm`/`write`/`date`/`whoami`/`ps`/`ports`/`portclaim`/`exec`/`spawn`/`clear`/`reboot`）。方向键由 input_server 把 PS/2 扫描码（e0 前缀）编码为 ANSI 转义序列送达。
- **BMP 加载器**（`user/apps/bmploader.c`）：流式读取 BMP 并经内核 `SYS_DISPLAY_BLIT` 通道 blit 到帧缓冲。

**开机自检程序**（随内核启动自动运行，输出经串口落盘，验证各子系统；其中 `RUSTHELLO` 经 `weak` 符号引用，对应 blob 缺失则跳过，其余恒随构建运行）：
- **posixtest**：POSIX 系统调用层一致性测试，含 `pthread`/`clone`/`futex` 多线程用例。
- **nettest**：socket 冒烟测试，验证 `net_server` 通路（ARP/DHCP/TCP 端到端）。
- **dltest**：动态链接端到端验证（DT_NEEDED 自动加载 + 运行期 `dlopen`/`dlsym`/`dlclose` + COPY 重定位 + 槽位回收）。
- **RUSTHELLO**：Rust 程序（详见 §2.10），打印 `hello from rust on SukiOS` 后退出。

**独立用户程序**（FAT32 磁盘 `::BIN/`）：`hello`、`playaudio`（minimp3 MP3 解码）、`audiotest`、`pchfnt`（TTF 字形渲染，经 `fontsrv`）。

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

### 2.11 动态链接与共享库（`.sl`）
- **内核加载器 `ld.suki`**：ELF 加载（execve / spawn）时解析 `DT_NEEDED`，自动映射并基址重定位依赖的共享库；运行期 `dlopen` / `dlsym` / `dlclose` / `dlerror` 经系统调用 **126–129** 完全由内核完成符号解析与页表映射（用户指针一律 `copy_from_user` / `copy_to_user`）。
- **共享库形态**：`.sl`（ELF 共享对象），例：`libtest.sl`（`user/libs/libtest.c`），置于 `/LIB/`，由内核在加载期或运行期映射到进程地址空间。
- **libdl 封装**：用户态 `user/lib/dlfcn.c` 提供标准 `dlopen` / `dlsym` / `dlclose` / `dlerror` 接口（忽略 flags，当前仅立即绑定语义）。
- **验证**：`dltest`（`user/apps/dltest.c`）开机自检覆盖 ① 加载期 `DT_NEEDED` 依赖自动加载与 `R_X86_64_COPY` 拷贝重定位；② 同名库 `dlopen` 去重；③ 纯运行期 `dlopen` + `dlclose` 真正回收物理页；④ 槽位复用。

---

## 3. 尚未实现 / 早期

> 以下为当前明确**未实现 / 早期**的部分，列出以避免误用。

- **网络子系统（早期但已打通）**：Ring0 **e1000 驱动 + 用户态 `net_server`（lwIP 2.2.1，raw API）** 已打通「e1000 → `NET_PORT` → lwIP」全链路，DHCP 自动获取 IP，内核 **150–166** socket syscall 经 `NS_PORT` 转发；`nettest` 开机自检验证通路。仍属早期：非阻塞 / 异步语义、连接状态精细管理、DNS 客户端应用、多网卡 / 多协议栈、virtio-net 等仍在完善；当前仅验证 QEMU `user` 后端（`10.0.2.2` 网关 / `10.0.2.3` DNS）。
- **真正的 GUI 应用框架**：已有 display-server 基础（终端栅格化 + 光标），但**无窗口系统 / 合成器 / 应用离屏 Buffer 合成管线**；TTF 字形渲染已接入：由 `fontsrv` 字体服务加载 FreeType 渲染字形位图，经 IPC 供 `pchfnt` 等程序绘制到帧缓冲（详见 `results/step53.md`）。
- **存储**：仅 FAT32 经 FatFs；无 ext2/3/4、exFAT、NTFS、ISO9660（除引导 ISO 外）。
- **多文件系统 / 多磁盘 / GPT**：仅识别首个磁盘首分区 FAT32。
- **完整 POSIX 语义（部分已实现）**：传统 `fork` / `clone` / 线程（`pthread` 基座）/ `futex` / **信号框架**（`SIGACTION` / `SIGRETURN` / `SIGPROCMASK` / `TKILL` / `RAISE`）已落地，并由 `posixtest` 多线程用例验证；仍缺：swap / huge page / NUMA，以及作业控制、会话 / 进程组精细语义、完整信号投递集等高级 POSIX 语义。
- **电源管理**：仅重启与 ACPI S5 关机；无睡眠 / 休眠 / 调频。
- **用户态隔离强化**：进程间靠端口能力粗粒度隔离，无完整能力 / 沙箱模型（无 seccomp 类机制）。
- **`.kdr` 内核模块加载器（未实现）**：**用户态共享库动态链接已实现**（`.sl` + 内核 `ld.suki` + syscall 126–129 + `dltest` 自检，见 §2.11）；但 **`.kdr` 内核模块 / 用户态驱动的动态装载机制尚未实现**，当前鼠标驱动以内嵌 spawn 形式运行，待加载器就绪后改为动态装载（内核侧无需改动）。
- **真实硬件适配广度**：主要在 QEMU 验证；未在现代物理机、不同 AHCI/网卡型号上系统测试。
- **多核调度策略**：开启 SMP 时为对称 RR；无 CFS / 优先级继承 / 负载均衡迁移。
- **HTTP/HTTPS 客户端（libcurl + mbedTLS，已备料未集成）**：libcurl `8.22.0` 与 mbedTLS `3.6.7`（LTS）已作为 **git submodule** 引入 `lib/`（见 §5.1），但**尚未接入构建**。其底座（BSD socket libc 包装、`getaddrinfo`/DNS、`select`/`poll`/`fcntl`、时间日历换算、miniz zlib 兼容层）已就绪（详见 `results/step82.md`）；后续按 `results/step81.md` 方案以手写 `curl_config.h` + `libcurl.a` 集成，并由 mbedTLS 提供 HTTPS/TLS。
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
# 0) 首次克隆后拉取第三方子模块（lwip / freetype / mbedtls / curl；缺失会构建失败）
git submodule update --init --recursive

# 1) 清理（会删除 build/ 下全部产物，含 disk.img；用 make clean，勿用 rm -rf）
make clean

# 2) 编译内核 + 全部用户态服务/程序，生成 build/SukiOS.iso
make iso

# 3) 生成 FAT32 磁盘镜像（含 README/HELLO/ROADMAP、::SYS、::BIN/*、MOONHALO.MP3）
make disk
```

> **第三方依赖以 git submodule 引入**：库类放在 `lib/`，程序 / 工具类放在 `compapps/`
> （详见 §5 与 §5.1）。克隆仓库时建议 `git clone --recursive`，或在已有克隆中执行
> `git submodule update --init --recursive` 拉齐 `lib/{lwip-2.2.1,freetype-2.14.3,mbedtls,curl}`。

> **默认即为单核**：`make iso` / `make run` 等目标默认 `CONFIG_SMP=0`（单核）。如需多核，用 `make iso SMP=1`（同时 QEMU 自动 `-smp 4`），但当前主线验证以单核为准。

常用变体：

```bash
make all                  # 仅编译内核 ELF
make run                  # 图形窗口运行（推荐人工交互测试 shell / 鼠标 / 音频）
make run-headless         # 无头运行，仅串口（自动化验证用）
make run-dbg              # 带内核串口详细诊断（DBG=1，verbose 日志）
make run-ahci             # 磁盘挂 AHCI（DMA+中断）验证
make run-ahci-headless    # AHCI + 无头
make run-uefi             # OVMF UEFI 启动验证
make run-uefi-headless    # UEFI + 无头
make run-q                # PVH 直启（qemu -kernel，不经 GRUB）
make run-q-debug          # PVH 直启 + 详细诊断
make debug                # 等价于 run-dbg（别名）
make info                 # 打印当前工具链/对象信息
make gcc                  # 从源码构建 x86_64-sukios 交叉工具链（Binutils+GCC，见 cross/）
make rust-libs            # 仅重建 libsuki.a 运行时归档
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
8. **第三方依赖**：`minimp3/`、`drivers/FatFs/`、`kernel/abilities/miniz/`、`resources/ResourceHanRoundedCN-Medium.ttf` 等为随仓库分发的第三方组件；`lib/{lwip-2.2.1,freetype-2.14.3,mbedtls,curl}` 为 **git submodule**（需 `--recursive` 拉取，见 §5.1）。版权见 `NOTICE`。

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
│   ├── syscall/           # 系统调用分发（syscall.c）+ POSIX 层（sys_posix.c）+ SukiNative（sys_suki.c）+ 信号（signal.c）
│   ├── acpi/              # ACPI 解析、LAPIC/IOAPIC、HPET、PCI
│   ├── fs/                # vfs.c / tmpfs.c / devfs.c / fd.c 路由层
│   ├── drivers/           # Ring0 裸驱动：ata.c、ahci.c、e1000.c、hda.c、pci.c（键盘/鼠标在 arch/x86_64/）
│   ├── time/              # rtc.c（CMOS 实时时钟）
│   ├── elf/               # ELF64 加载 + 动态链接（execve / DT_NEEDED / dlopen，ld.suki）
│   ├── abilities/         # 用户态能力库（miniz 压缩，依赖 libc，不入内核）
│   ├── kmain.c            # 内核 C 主入口（拉起 disk-srv 与全部 Ring3 服务 / 自检）
│   └── gdbstub.c          # 串口 GDB 远程调试 stub（注：项目调试以 QEMU 自带机制为主，GDB 交互非推荐路径）
├── user/                  # Ring3 服务与程序（C，freestanding）
│   ├── fs_server.c        # FAT32 服务（FatFs + DISK_PORT）
│   ├── input_server.c     # 键盘输入服务
│   ├── display_server.c   # 显示合成服务（帧缓冲独占 + 终端栅格化 + 光标）
│   ├── mouse_server.c     # 鼠标驱动（Ring3 .kdr 形态）
│   ├── net_server.c       # 网络服务（lwIP 2.2.1，NET_PORT/NS_PORT）
│   ├── fontsrv.c          # 字体服务（FreeType，FONT_PORT）
│   ├── shell.c            # 交互 shell
│   ├── apps/              # 独立程序：hello / playaudio / audiotest / bmploader / pchfnt / nettest / dltest
│   ├── libs/              # 共享库示例（libtest.sl 等，动态链接验证）
│   └── lib/               # 用户态 freestanding 桩：crt0 / suki.c / libc / pthread / dlfcn / suki.h
├── drivers/FatFs/         # ChaN FatFs R0.16
├── minimp3/               # minimp3 解码库（CC0，playaudio 用）
├── resources/             # ResourceHanRoundedCN-Medium.ttf（界面字体，OFL-1.1）
├── lib/                   # 第三方【库】——全部以 git submodule 引入（库文件放 lib/）
│   ├── lwip-2.2.1/        # lwIP TCP/IP 协议栈（BSD-3-Clause）@ tag STABLE-2_2_1_RELEASE，net_server 用
│   ├── freetype-2.14.3/   # FreeType 字体光栅化引擎（FTL）@ tag VER-2-14-3，fontsrv 用
│   ├── mbedtls/           # mbedTLS 3.6.7（LTS，Apache-2.0）@ tag mbedtls-3.6.7，TLS 后端（待接入）
│   └── curl/              # libcurl 8.22.0（curl 许可）@ tag curl-8_22_0，HTTP/HTTPS 客户端（待集成）
├── compapps/              # 第三方【程序/工具】——以 git submodule 引入（程序文件放 compapps/）
├── include/               # 内核 / 用户态公共头
├── grub/                  # grub.cfg（ISO 引导配置）
├── tools/                 # gen_relk.py（KASLR 重定位）等
├── results/               # 阶段性实现文档（stepNN.md）
├── osdev_wiki/            # OSDev 维基离线副本（实现参考）
├── rust/                  # Rust 工具链示例工程（make make-rust-env 生成规格/归档，源码入库）
├── SukiNative API 完整系统接口规范.md   # SukiNative 130–149 对象 API 规范
├── SukiOS 全栈技术参考手册.md           # 全栈技术参考
├── SukiOS 混合风格权限提升设计文档.md   # 权限/能力设计
├── build/                 # 构建产物（被 .gitignore 忽略）
├── Makefile
├── NOTICE                 # 第三方库版权与许可证
├── LICENSE                # GPL-3.0 全文
└── README.md              # 本文件
```

### 5.1 第三方依赖：git submodule 约定

SukiOS 引入外部第三方项目**一律使用 `git submodule`**（不再把上游源码直接拷贝入库），约定：

| 类别 | 位置 | 示例 |
|---|---|---|
| **库（library）** | `lib/` | `lib/lwip-2.2.1`、`lib/freetype-2.14.3`、`lib/mbedtls`、`lib/curl` |
| **程序 / 工具（program / tool）** | `compapps/` | 当前为空占位（见 `compapps/README.md`） |

当前已登记的子模块（`.gitmodules`，均为 `--depth 1` 浅克隆并按 tag 固定）：

| 子模块 | 上游 | 固定版本 | 用途 |
|---|---|---|---|
| `lib/lwip-2.2.1` | `lwip-tcpip/lwip` | `STABLE-2_2_1_RELEASE` | 用户态 `net_server` 的 TCP/IP 协议栈 |
| `lib/freetype-2.14.3` | `freetype/freetype` | `VER-2-14-3` | `fontsrv` 字形光栅化 |
| `lib/mbedtls` | `Mbed-TLS/mbedtls` | `mbedtls-3.6.7`（3.6 LTS） | HTTPS/TLS 后端（供 libcurl，待接入） |
| `lib/curl` | `curl/curl` | `curl-8_22_0` | HTTP/HTTPS 客户端（待集成） |

使用方式：

```bash
git clone --recursive <SukiOS-url>            # 初次克隆连子模块一起拉取
git submodule update --init --recursive       # 已有克隆补齐/同步子模块
git submodule status                          # 查看各子模块当前提交
```

> 说明：`lwip-2.2.1` / `freetype-2.14.3` 此前为 vendored（随仓库分发的源码副本），
> 现已切换为上述固定 tag 的子模块；两者内容与上游 tag 逐文件一致（构建产物尺寸不变），
> 项目侧的裁剪 / 配置全部通过外部头（`user/lib/lwipopts.h`、`user/lib/arch/cc.h`、
> `user/lib/freetype_shim.h`、`user/lib/ftmodule_min.h`）完成，**不修改上游源码**。
> `mbedtls` 与 `curl` 已作为子模块就绪但**尚未接入构建**（见 §3）。

---

## 6. 向仓库贡献

欢迎以 Issue、讨论、文档修订或 Pull Request 参与 SukiOS 开发。本节详述贡献流程、代码规范与本地验证要求，目的是让合并后的内核在单核 QEMU 生产场景下**零 panic**、架构行为**严格符合 OSDev 标准**。

### 6.1 开发环境准备

```bash
git clone --recursive https://github.com/bil-fis/SukiOS SukiOS && cd SukiOS
# 若未带 --recursive：git submodule update --init --recursive
make info                 # 打印 CC / 工具链 / 对象列表
which grub-mkrescue mformat mcopy mmd qemu-system-x86_64 python3
```

- 推荐启用 KVM（`/dev/kvm` 存在）获得接近真实的运行速度。
- 构建与运行**不需要** `source` 任何 env 脚本；构建环境由 `Makefile` 自身处理。
- 第三方依赖以 `git submodule` 引入（库放 `lib/`，程序放 `compapps/`）；拉齐子模块见 §5.1。
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

- `minimp3/`、`drivers/FatFs/`、`kernel/abilities/miniz/`、`resources/ResourceHanRoundedCN-Medium.ttf` 为随仓库分发的上游第三方组件；`lib/`、`compapps/` 下的 submodule（lwip / freetype / mbedtls / curl 等）为上游独立仓库，**一般不要在其内部做功能修改**（子模块改动不会随主仓库提交）。项目侧一律通过外部配置文件（如 `user/lib/lwipopts.h`、`user/lib/arch/cc.h`、`user/lib/freetype_shim.h`、`user/lib/ftmodule_min.h`）适配；如需上游修复优先走上游 PR，并在 `NOTICE` 记录偏差。
- 引入新的第三方库 / 程序请**使用 `git submodule`**：库放 `lib/`，程序 / 工具放 `compapps/`（见 §5.1）。
- 内核为 freestanding，用户态为自供桩；不要把 newlib 整体编入内核。

---

## 7. 致谢

- **OSDev 社区**（`wiki.osdev.org`）——分页、GDT/TSS、SMP、CR4、TLB、SWAPGS、syscall 等架构实现的标准参考。
- **ChaN** —— FatFs 通用 FAT 文件系统模块（R0.16）。
- **lieff** —— minimp3（CC0 公共领域），MP3 解码能力。
- **Rich Geldreich / RAD Game Tools / Valve** —— miniz（zlib 风格许可）。
- **Cyano Hao** —— Resource Han Rounded（资源圆体，OFL-1.1），界面字体。
- **FreeType Project** —— FreeType 字体光栅化引擎（FTL 许可），`lib/freetype-2.14.3/`（submodule），驱动 TTF 字形渲染。
- **lwIP（Adam Dunkels / Simon Goldschmidt 等）** —— lwIP 轻量级 TCP/IP 协议栈（BSD-3-Clause），`lib/lwip-2.2.1/`（submodule），驱动 `net_server` 网络能力。
- **Mbed TLS（Arm / TrustedFirmware）** —— 轻量级 TLS/加密库（Apache-2.0 或 GPL-2.0-or-later 双许可），`lib/mbedtls/`（submodule，3.6 LTS），规划为 SukiOS HTTPS/TLS 后端。
- **curl（Daniel Stenberg 等）** —— libcurl 传输库（curl 许可），`lib/curl/`（submodule），规划为 SukiOS 的 HTTP/HTTPS 客户端。
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
| lwIP（submodule） | `lib/lwip-2.2.1/` | BSD-3-Clause（Adam Dunkels / SICS 等） |
| FreeType（submodule） | `lib/freetype-2.14.3/` | FTL 或 GPLv2（双许可，FreeType Project） |
| mbedTLS（submodule） | `lib/mbedtls/` | Apache-2.0 或 GPL-2.0-or-later（双许可，Arm / TrustedFirmware） |
| libcurl（submodule） | `lib/curl/` | curl 许可（MIT/X 风格，Daniel Stenberg 等） |
| Rust 工具链（rustup / rustc / cargo） | 本地安装（不随仓库分发） | MIT OR Apache-2.0（Rust Project Developers） |

---

*本 README 随项目开发持续更新。如与代码实际状态不符，以代码与 `results/stepNN.md` 实现文档为准。*

&copy; 2026 SukiOS Developer Team. | built with :heart:
