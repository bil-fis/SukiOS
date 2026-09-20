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
- **GUI 起点**：已实现 `display-server`（窗口管理 + 增量合成器，纯黑背景，窗口为 iSuki 新样式：38px 标题栏、圆角 12、左侧红/黄/绿三色交通灯）。客户端经 **iSuki 原生控件库 `libsui`** 自绘像素，经 OOL 零拷贝提交到 `WM_PORT` 合成。TTF 字形渲染由 **FreeType 字体服务**（`lib/freetype-2.14.3`，FTL 许可）承担，界面字体采用 `ResourceHanRoundedCN-Medium.ttf`（资源圆体，OFL-1.1）。

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
- 小消息内核 memcpy 转发；大消息 OOL 物理页重映射零拷贝（引用计数 +1）。单条 OOL 上限 **2048 页 = 8 MiB**（`include/ipc/port.h` 的 `MACH_MSG_OOL_MAX_PAGES`），窗口离屏缓冲上限同步放宽到 8 MiB。

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
- **display-server**：**窗口管理 + 纯合成器**。已删除桌面装饰条 / 菜单栏 / 任务栏，仅保留窗口管理与增量合成；帧缓冲背景纯黑。窗口为 iSuki 新样式（38px 标题栏、圆角 12、左侧三色交通灯、活动态强调蓝边框），客户区像素由客户端 `sys_mmap` 分配、经 OOL 零拷贝合成；采用脏矩形增量渲染（静止窗口零重绘），光标仅画小矩形。详见 `results/step108.md` / `step109.md`。
- **mouse-server**：Ring3 鼠标驱动（`.kdr` 形态，临时内嵌 spawn），经 `SYS_MOUSE_READ` 拉包、累计坐标、发光标事件到 display-server。
- **net-server**：网络服务（`user/net_server.c`）。作为 `NET_PORT` 客户端收发原始帧（交给 Ring0 e1000 驱动），并作为 `NS_PORT` 服务端用 **lwIP 2.2.1**（raw API，`NO_SYS=1`）实现 TCP/IP 协议栈与 DHCP；内核 150–166 socket syscall 经 IPC 转发到此。启动即自动获取 IP（QEMU user-net 网关 `10.0.2.2`），证明「e1000 → NET_PORT → lwIP」全链路打通。
- **fontsrv**：字体服务（`user/fontsrv.c`）。加载 `ResourceHanRoundedCN-Medium.ttf`，经 FreeType 光栅化字形位图，经 `FONT_PORT` IPC 提供给 display-server / `pchfnt` 等程序绘制到帧缓冲。
- **shell**：bash 风格交互式命令行。已实现：历史记录（**内存环形缓冲，最多 100 条，超出丢弃最旧，不落盘**；↑/↓ 滚动）、**Tab 文件名补全**（唯一匹配直接补全、目录补 `/`、多匹配补公共前缀并列出候选）、**行内光标编辑**（←/→ 移动光标、Home/End 跳行首行尾、Backspace 删前、Delete 删后、Enter 提交）、管道 `|` 与重定向 `>`、`$VAR`/`$?` 变量展开、内建命令（`help`/`echo`/`cat`/`ls`/`cd`/`pwd`/`mkdir`/`touch`/`rm`/`write`/`date`/`whoami`/`ps`/`ports`/`portclaim`/`exec`/`spawn`/`clear`/`reboot`）。方向键由 input_server 把 PS/2 扫描码（e0 前缀）编码为 ANSI 转义序列送达。
- **BMP 加载器**（`user/apps/bmploader.c`）：流式读取 BMP 并经内核 `SYS_DISPLAY_BLIT` 通道 blit 到帧缓冲。

**开机自检程序**（随内核启动自动运行，输出经串口落盘，验证各子系统；均随构建确定性运行）：
- **posixtest**：POSIX 系统调用层一致性测试，含 `pthread`/`clone`/`futex` 多线程用例。
- **nettest**：socket 冒烟测试，验证 `net_server` 通路（ARP/DHCP/TCP 端到端），并覆盖 libc 层 socket API（inet_*、getaddrinfo、poll）。
- **curl_test**：libcurl 移植端到端验证（easy 接口对宿主机 HTTP 服务发起真实 GET，校验 HTTP 200 与响应体字节）。
- **dltest**：动态链接端到端验证（DT_NEEDED 自动加载 + 运行期 `dlopen`/`dlsym`/`dlclose` + COPY 重定位 + 槽位回收）。
- **suikitest**：iSuki 原生控件库 `libsui` 端到端冒烟测试，创建窗口 + 控件、渲染并经 OOL 提交合成，成功打印 `[suikitest] window rendered + flushed -> PASS`（详见 §2.15）。

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

### 2.10 Rust 支持已整体移除（历史）
- SukiOS 现已为**纯 C 实现**：内核（C）+ 用户态服务 / 程序（C，freestanding）。原内核内嵌的 Rust 开机自检（`RUSTHELLO` / `STDTEST`）与 Rust blob 构建规则已在重构中整体移除——`Makefile` 不再引用 Rust 工具链，`kmain.c` 不再 spawn 任何 Rust 任务，`rust/` 目录为历史残留（可删除，不再参与构建）。
- 原计划经 Rust 移植 Servo 浏览器的路径已搁置；如未来重新引入 Rust，需重建 `make make-rust-env` 与目标规格 / blob 自检链路（参考历史 `results/step69.md` / `step70.md`）。当前纯 C 内核 / 服务 / 程序构建**不依赖任何 Rust 工具链**。

### 2.11 动态链接与共享库（`.sl`）
- **内核加载器 `ld.suki`**：ELF 加载（execve / spawn）时解析 `DT_NEEDED`，自动映射并基址重定位依赖的共享库；运行期 `dlopen` / `dlsym` / `dlclose` / `dlerror` 经系统调用 **126–129** 完全由内核完成符号解析与页表映射（用户指针一律 `copy_from_user` / `copy_to_user`）。
- **共享库形态**：`.sl`（ELF 共享对象），例：`libtest.sl`（`user/libs/libtest.c`），置于 `/LIB/`，由内核在加载期或运行期映射到进程地址空间。
- **libdl 封装**：用户态 `user/lib/dlfcn.c` 提供标准 `dlopen` / `dlsym` / `dlclose` / `dlerror` 接口（忽略 flags，当前仅立即绑定语义）。
- **验证**：`dltest`（`user/apps/dltest.c`）开机自检覆盖 ① 加载期 `DT_NEEDED` 依赖自动加载与 `R_X86_64_COPY` 拷贝重定位；② 同名库 `dlopen` 去重；③ 纯运行期 `dlopen` + `dlclose` 真正回收物理页；④ 槽位复用。

### 2.12 libcurl（HTTP 客户端库）
- **来源/配置**：`lib/curl`（git submodule，`8.22.0`，见 §5.1）。不运行 autotools/CMake，改为**手写 `user/lib/curl_config.h`**（经 `-DHAVE_CONFIG_H` 被 `curl_setup.h` 引入），按 SukiOS 用户态实有能力声明 `HAVE_*`。
- **libc 补齐**（libcurl 需要而原 libc 缺失的）：新增 `<sys/types.h>`/`<sys/stat.h>`/`<signal.h>` 标准头；新增 `FILE` 流层（`user/lib/fileio.c`：`fopen/fdopen/fread/fwrite/fgets/fprintf/...` 与 `stdin/stdout/stderr`）；新增 `strspn/strcspn/strpbrk`、`bsearch`、`fstat/stat/lstat`、`gethostname`；补网络 errno（`EWOULDBLOCK/EADDRINUSE/EISCONN/...`）与 `PF_*`。并按 POSIX/glibc 语义让 `<sys/socket.h>` 暴露 `fd_set`、`struct sigaction` 暴露 `sa_handler` 宏。
- **构建**：`build/libcurl.a`（`lib/*.c` + `curlx/*` + `vauth/*` + `vdns/*` + `vtls/*` + `vquic/vquic.c`），链接进 `curl_test`。
- **能力**：**HTTP / HTTPS（TLS）/ FTP / TFTP**；gzip/deflate 经 **zlib**（`lib/zlib` @ v1.3.1，`HAVE_LIBZ`）；TLS 后端为 **mbedTLS**（`lib/mbedtls` @ 3.6.7 LTS，`USE_MBEDTLS`）；仅 IPv4、同步 `getaddrinfo` 解析、无线程、无 c-ares；LDAP/SMTP/IMAP/POP3/... 经官方 `CURL_DISABLE_*` 关闭。
- **验证**：开机自检 `curl_test`（`user/apps/curl_test.c`）用 easy 接口对宿主机服务发起真实请求——**HTTP**（200 + 响应体）与 **HTTPS**（TLS 握手 + 200 + 响应体，自签证书）均通过，零 panic（详见 `results/step84.md`、`results/step85.md`）。

### 2.13 curl 命令行应用（`::BIN/CURL.SKA`）
- **同源**：直接编译上游 `lib/curl/src`（curl 命令行工具，42 个源文件），与 Linux 上的 `curl` 是同一份代码、同一套参数语义（`curl_getparam`/`tool_operate` 等），不是自研简化版。
- **构建**：`build/apps/curl.elf`（约 1.63 MiB），链接 `USER_LIB_OBJS + libcurl.a + libz.a + libmbedtls.a`；安装到 FAT32 磁盘 `::BIN/CURL.SKA`（与其他独立程序一致，`exec BIN/curl` 装载）。
- **内核配套**：原 `EXEC_ELF_MAX` 为 1 MiB，放不下 1.6 MiB 的 curl CLI，已上调至 **4 MiB**（exec 读盘本就是 `FS_MSG_READ_AT` 分块读，不受 OOL 上限——现为 2048 页 / 8 MiB——限制）。
- **libc 补齐**：为工具新增 `isatty`（返回 0，走非终端分支）、`ftruncate`、`freopen`、`rand/srand`；`fcntl` 改为标准可变参数；`<sys/stat.h>` 补 `st_atime/st_mtime/st_ctime` 成员别名。
- **验证**：开机自检 `curl_app_test`（`user/apps/curl_app_test.c`）以真实命令行参数 **execve 磁盘上的 `::BIN/CURL.SKA`**（与 shell 同路径），日志显示任务名变为 `CURL.SKA` 且 **exit code 0**（请求成功完成，响应体 `Hello from SukiOS host HTTP server!` 经用户 TTY 环形管道回显到 shell 终端窗口）。
- **构建坑（已修）**：`lib/curl/src` 内含独立工具 `curlinfo.c`（自带 `int main()`，仅打印功能清单），若一并编入 `curl.elf`，链接器会选用它的 `main` 而非 `tool_main.c` 的 `main`，导致运行 `curl` 时只打印功能列表、不做任何请求。已在 `Makefile` 的 `CURLTOOL_SRCS` 中 `$(filter-out .../curlinfo.c, ...)` 显式排除（详见 `results/step86.md`）。

### 2.14 内核内嵌压缩能力（miniz）
- **形态**：`kernel/abilities/miniz/`（miniz，zlib 风格）**编入 Ring0 内核**，对外提供内核 API（`include/kernel/abilities/kminiz.h`：`kminiz_compress/uncompress/compress_bound/adler32/crc32`），实现在 `kernel/abilities/miniz/kminiz.c`。
- **适配**：自定义 `assert/stdlib/string` 垫片适配 freestanding 环境；分配经垫片映射到内核堆（`kmalloc/kzalloc/krealloc/kfree`）。为此给内核堆新增了 `krealloc()`。
- **用途**：为后续**最小系统环境（minOSEnv / rootfs）**预留内核态解压能力（如 initramfs、`.spkg` 软件包）。开机自检 `kminiz_selftest()` 做压缩→解压往返校验（kernel.ski 启动即打印 `[kminiz] selftest OK`）。
- **说明**：用户态不再使用 miniz（改用真实 zlib，见 §2.12）。

### 2.15 iSuki 原生控件库 `libsui`
- **形态**：`user/libsui/`（接口 `include/sui.h` + 实现 `src/sui_core.c` / `src/sui_widgets.c`），C 编写、freestanding 用户态库；对照 `iSuki UI 界面库设计规范.md` 实现，作为 GUI 客户端的控件基座（非内核组件）。
- **窗口模型**：`sui_create_window()` 经 `SukiCreateWindow`（`user/lib/gui.c`）向 `WM_PORT` 请求创建窗口，客户端 `sys_mmap` 分配离屏缓冲，自绘像素后 `SukiFlush` 经 **OOL 零拷贝**提交合成；窗口装饰（标题栏 / 交通灯 / 圆角）由 `display-server` 合成时绘制（显示服务不渲染文字）。
- **控件集**：label / button / checkbox / switch / slider / progress / input / card 等，统一主题（iSuki 颜色 / 字体档）与画布原语（矩形 / 圆角）；控件自绘进离屏缓冲。
- **布局与事件**：简单布局（锚点 / 填充）+ 事件分发（`SUI_EVENT_*`）。
- **验证**：开机自检 `suikitest`（`user/apps/suikitest.c`）创建 660×440 窗口（1.16 MiB，落在 8 MiB OOL 上限内），绘制控件并经 OOL 提交合成，串口见 `[suikitest] window rendered + flushed -> PASS`、窗口 id 正常创建与销毁，零 panic（详见 `results/step108.md`）。

---

## 3. 尚未实现 / 早期

> 以下为当前明确**未实现 / 早期**的部分，列出以避免误用。

- **网络子系统（早期但已打通）**：Ring0 **e1000 驱动 + 用户态 `net_server`（lwIP 2.2.1，raw API）** 已打通「e1000 → `NET_PORT` → lwIP」全链路，DHCP 自动获取 IP，内核 **150–166** socket syscall 经 `NS_PORT` 转发；`nettest` 开机自检验证通路。仍属早期：非阻塞 / 异步语义、连接状态精细管理、DNS 客户端应用、多网卡 / 多协议栈、virtio-net 等仍在完善；当前仅验证 QEMU `user` 后端（`10.0.2.2` 网关 / `10.0.2.3` DNS）。
- **桌面环境与 WM 高级特性（早期）**：窗口系统、增量合成器、iSuki 控件库 `libsui` 已落地（见 §2.6 / §2.15），但尚缺完整桌面环境（无任务栏 / 开始菜单 / 多工作区 / 窗口动画 / 阴影）、system tray、全局快捷键管理等。TTF 字形渲染已接入：由 `fontsrv` 字体服务加载 FreeType 渲染字形位图，经 IPC 供 `pchfnt` 等程序绘制到帧缓冲（详见 `results/step53.md`）。
- **存储**：仅 FAT32 经 FatFs；无 ext2/3/4、exFAT、NTFS、ISO9660（除引导 ISO 外）。
- **多文件系统 / 多磁盘 / GPT**：仅识别首个磁盘首分区 FAT32。
- **完整 POSIX 语义（部分已实现）**：传统 `fork` / `clone` / 线程（`pthread` 基座）/ `futex` / **信号框架**（`SIGACTION` / `SIGRETURN` / `SIGPROCMASK` / `TKILL` / `RAISE`）已落地，并由 `posixtest` 多线程用例验证；仍缺：swap / huge page / NUMA，以及作业控制、会话 / 进程组精细语义、完整信号投递集等高级 POSIX 语义。
- **电源管理**：仅重启与 ACPI S5 关机；无睡眠 / 休眠 / 调频。
- **用户态隔离强化**：进程间靠端口能力粗粒度隔离，无完整能力 / 沙箱模型（无 seccomp 类机制）。
- **`.kdr` 内核模块加载器（未实现）**：**用户态共享库动态链接已实现**（`.sl` + 内核 `ld.suki` + syscall 126–129 + `dltest` 自检，见 §2.11）；但 **`.kdr` 内核模块 / 用户态驱动的动态装载机制尚未实现**，当前鼠标驱动以内嵌 spawn 形式运行，待加载器就绪后改为动态装载（内核侧无需改动）。
- **真实硬件适配广度**：主要在 QEMU 验证；未在现代物理机、不同 AHCI/网卡型号上系统测试。
- **多核调度策略**：开启 SMP 时为对称 RR；无 CFS / 优先级继承 / 负载均衡迁移。
- **curl CLI 输出在图形窗口中可见（已实现）**：早期 `curl` 等命令行程序的 stdout/stderr 写内核 `TTY` 后端，而 `user_puts` 在显示服务接管帧缓冲后**只写串口、不写帧缓冲**，故窗口里看不到。已在内核新增「用户 TTY 环形管道」（`kernel/console.c` 的 `g_user_tty_pipe` + `SYS_TTY_READ=204`），`tty_write` 在显示激活时把输出捕获进该管道；shell（`user/shell.c`）在事件循环与 `exec` 等待后 `drain_tty_pipe()` 取回并渲染进自己的终端窗口。串口仍恒定输出（headless 可观测）。因此 `exec BIN/curl <url>` 的响应体现在图形 shell 窗口中可见。`curlinfo.c` 误编入导致 CLI 只打印功能列表的坑已修（见 §2.13）。


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

| （可选）`OVMF`（`/usr/share/OVMF/OVMF_CODE_4M.fd`） | UEFI 启动 | `make run-uefi` 需要 |

### 4.2 构建命令

```bash
# 0) 首次克隆后拉取第三方子模块（lwip / freetype / mbedtls / curl 等，缺失会构建失败）
git submodule update --init --recursive
#    注意：WPE WebKit 体积 GB 级，建议按需单独浅拉取（见 §5.1）：
#    git submodule update --init --depth 1 lib/wpewebkit-2.54

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
```



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
│   ├── display_server.c   # 显示合成服务（窗口管理 + 增量合成器，纯黑背景，iSuki 新样式窗口）
│   ├── mouse_server.c     # 鼠标驱动（Ring3 .kdr 形态）
│   ├── net_server.c       # 网络服务（lwIP 2.2.1，NET_PORT/NS_PORT）
│   ├── fontsrv.c          # 字体服务（FreeType，FONT_PORT）
│   ├── shell.c            # 交互 shell
│   ├── apps/              # 独立程序：hello / playaudio / audiotest / bmploader / pchfnt / nettest / dltest / suikitest
│   ├── libsui/            # iSuki 原生控件库（include/sui.h + src/sui_core.c + src/sui_widgets.c）
│   ├── libs/              # 共享库示例（libtest.sl 等，动态链接验证）
│   └── lib/               # 用户态 freestanding 桩：crt0 / suki.c / libc / pthread / dlfcn / suki.h
├── drivers/FatFs/         # ChaN FatFs R0.16
├── minimp3/               # minimp3 解码库（CC0，playaudio 用）
├── resources/             # ResourceHanRoundedCN-Medium.ttf（界面字体，OFL-1.1）
├── lib/                   # 第三方【库】——全部以 git submodule 引入（库文件放 lib/）
│   ├── lwip-2.2.1/        # lwIP TCP/IP 协议栈（BSD-3-Clause）@ tag STABLE-2_2_1_RELEASE，net_server 用
│   ├── freetype-2.14.3/   # FreeType 字体光栅化引擎（FTL）@ tag VER-2-14-3，fontsrv 用
│   ├── mbedtls/           # mbedTLS 3.6.7（LTS，Apache-2.0）@ tag mbedtls-3.6.7，TLS 后端（已接入）
│   ├── curl/              # libcurl 8.22.0（curl 许可）@ tag curl-8_22_0，HTTP/HTTPS/FTP/TFTP（已集成，见 §2.12）
│   ├── zlib/              # zlib 1.3.1（zlib 许可）@ tag v1.3.1，用户态压缩（libcurl 用）
│   ├── lua-5.4.9/         # Lua 5.4.9（MIT）@ tag v5.4.9，脚本语言运行时（submodule）
│   └── wpewebkit-2.54/    # WPE WebKit（WebKit 官方工程）@ branch webkitglib/2.54（submodule，GB 级体积）
├── compapps/              # 第三方【程序/工具】——以 git submodule 引入（程序文件放 compapps/）
├── include/               # 内核 / 用户态公共头
├── grub/                  # grub.cfg（ISO 引导配置）
├── tools/                 # gen_relk.py（KASLR 重定位）等
├── results/               # 阶段性实现文档（stepNN.md）
├── osdev_wiki/            # OSDev 维基离线副本（实现参考）
├── rust/                  # 历史示例工程（已弃用：内核已移除 Rust 依赖，见 §2.10；可删除）
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
| `lib/mbedtls` | `Mbed-TLS/mbedtls` | `mbedtls-3.6.7`（3.6 LTS） | HTTPS/TLS 后端（**已接入** libcurl，见 §2.12） |
| `lib/curl` | `curl/curl` | `curl-8_22_0` | HTTP/HTTPS 客户端库 libcurl（**已集成**，见 §2.12） |
| `lib/zlib` | `madler/zlib` | `v1.3.1` | 用户态压缩库（libcurl 的 gzip/deflate Content-Encoding） |
| `lib/lua-5.4.9` | `lua/lua` | `v5.4.9` | Lua 5.4 脚本语言运行时（MIT） |
| `lib/wpewebkit-2.54` | `WebKit/WebKit` | `webkitglib/2.54` | WPE WebKit 浏览器引擎（WebKit 官方工程；**体积 GB 级，按需浅拉取**） |

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
> `mbedtls` 与 `curl` 已作为子模块就绪并**已接入构建**（见 §2.12 / §2.13）：libcurl 经手写 `curl_config.h` 集成，HTTPS 由 mbedTLS 提供后端。

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
- **Mbed TLS（Arm / TrustedFirmware）** —— 轻量级 TLS/加密库（Apache-2.0 或 GPL-2.0-or-later 双许可），`lib/mbedtls/`（submodule，3.6 LTS），已作为 SukiOS HTTPS/TLS 后端接入 libcurl（见 §2.12）。
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
| Lua（submodule） | `lib/lua-5.4.9/` | MIT（Lua.org / PUC-Rio） |
| WPE WebKit（submodule） | `lib/wpewebkit-2.54/` | LGPL-2.1-or-later / BSD（WebKit 项目与贡献者） |


---

*本 README 随项目开发持续更新。如与代码实际状态不符，以代码与 `results/stepNN.md` 实现文档为准。*

&copy; 2026 SukiOS Developer Team. | built with :heart:
