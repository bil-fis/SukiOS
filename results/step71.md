# step71：README.md 全量核对与逐节更新

## 1. 背景与目标
用户要求重读 `README.md`、仔细阅读每一部分，并结合**当前系统实际状态**更新每一部分。
本次为纯文档审计与修订，未改动任何代码；目标是消除 README 与真实代码之间的脱节。

## 2. 核对方法
重读 `README.md` 全文，并逐项与以下真实代码/配置比对（以代码为权威来源）：
- `include/sukios/posix.h`（`SYS_*` 系统调用号表，唯一契约）、`include/kernel/syscall.h`、`kernel/syscall/syscall.c`
- `kernel/syscall/sys_suki.c`（SukiNative 130–149）、`kernel/syscall/sys_posix.c`（fork/clone/futex/信号）
- `kernel/net/socket.c`（150–199 网络转发）、`kernel/drivers/e1000.c`（e1000 驱动）
- `user/net_server.c`、`user/fontsrv.c`、`user/apps/{nettest,dltest,pchfnt}.c`、`user/lib/dlfcn.c`、`user/libs/libtest.c`
- `kernel/kmain.c`（服务/自检拉起顺序）、`Makefile`（构建/运行目标、lwIP 编译）

## 3. 发现的主要偏差（README 原内容与实际不符）

### 3.1 §1 系统调用表（最严重）
- 原表把 POSIX 写成「100~203」，实际 POSIX 层占 **20–129**；130–149 是 **SukiNative 原生对象 API**，150–199 是 **网络 socket 子系统**，200+ 才是内核扩展。原表分区完全错位。
- 原表漏列：SukiNative（130–149）、网络（150–199）、动态链接（126–129）、`SYS_WAIT`(9)、`SYS_AUDIO_*`(10–13)、`SYS_MMAP_LEGACY`(14/15)、`SYS_SERIAL_READ`(16)、`SYS_FORK`(17)、`SYS_CLONE`(205)、`SYS_SIGACTION`(206–210)、framebuffer/display/console（200–203）等。
- 已重写为按真实号段分组的表（0–19 / 20–39 / 40–89 / 90–99 / 100–109 / 110–129 / 130–149 / 150–199 / 200+）。

### 3.2 §2.5 设备驱动
- 漏列 **e1000 网络驱动**（`kernel/drivers/e1000.c`）：Ring0 只做帧收发（DMA+中断），经 `NET_PORT` IPC 交给 `net_server`；启动 ARP 自检经 QEMU user 后端（网关 10.0.2.2 / DNS 10.0.2.3）验证 TX/RX。已补。

### 3.3 §2.6 用户态服务
- 漏列 **net-server**（`user/net_server.c`，NS_PORT 服务端 + lwIP 2.2.1 raw API + DHCP）、**fontsrv**（`user/fontsrv.c`，FreeType 字形服务）。已补。
- 漏列开机自检程序 **nettest / dltest**（原仅 posixtest）；并补 **pchfnt** 到独立程序清单。
- display-server 字体来源已由 `fontsrv` 提供，去掉「后续将采用」措辞。

### 3.4 新增 §2.11 动态链接与共享库（`.sl`）
- 原 README **完全未提及**用户态动态链接（已真实落地）：内核 `ld.suki` 解析 `DT_NEEDED` + 运行期 `dlopen/dlsym/dlclose/dlerror`（syscall **126–129**）；`.sl` 共享库（如 `libtest.sl`）；`dltest` 开机自检覆盖 DT_NEEDED 自动加载、`R_X86_64_COPY` 重定位、运行期 dlopen、dlclose 回收、槽位复用。已新增专节。

### 3.5 §3 尚未实现（三处过时断言已更正）
- **网络**：原「完全未实现，无 Intel 网卡驱动/无 TCP/IP/无 socket」→ 实际 e1000 + net_server(lwIP) + 150–166 socket syscall 已打通，nettest 自检验证；改为「早期但已打通」，列出待完善项。
- **完整 POSIX 语义**：原「无 fork / 无信号 / 无 pthread」→ 实际 `SYS_FORK`/`SYS_CLONE`/线程(pthread 基座)/`SYS_FUTEX`/`SIGACTION`+`SIGRETURN`+`SIGPROCMASK`+`TKILL`+`RAISE` 均已实现，由 posixtest 多线程用例验证；改为「部分已实现」，列出仍缺项（swap/hugepage/NUMA、作业控制、会话/进程组精细语义、完整信号集）。
- **kdr 动态加载器**：原「动态链接未实现」→ 改为区分：用户态 `.sl` 动态链接**已实现**，仅 `.kdr` 内核模块装载器未做。

### 3.6 §4.2 构建命令
- 原变体列表缺 `run-dbg` / `run-ahci-headless` / `run-uefi-headless` / `run-q-debug` / `debug` / `gcc` / `rust-libs`。已补全。

### 3.7 §5 项目结构
- 补充：`kernel/net/`、`kernel/syscall/sys_suki.c`、`kernel/syscall/signal.c`、`user/net_server.c`、`user/fontsrv.c`、`user/apps/{nettest,dltest,pchfnt}.c`、`user/libs/`、`lib/freetype-2.14.3/`、`lib/lwip-2.2.1/`、`rust/`，以及三份设计文档（`SukiNative API 完整系统接口规范.md`、`SukiOS 全栈技术参考手册.md`、`SukiOS 混合风格权限提升设计文档.md`）；`gdbstub.c` 标注「项目调试以 QEMU 自带机制为主，GDB 交互非推荐路径」。

### 3.8 §7 致谢 / §8 第三方
- 新增 **lwIP（Adam Dunkels / Simon Goldschmidt 等，BSD-2-Clause，`lib/lwip-2.2.1/`）**。

### 3.9 §2.3 进程与调度
- 新增「线程与信号」条目：`SYS_CLONE` + `SYS_FUTEX`（pthread 基座）、基础信号框架已实现，与 §3 修正一致。

## 4. 关键事实沉淀（供后续文档/开发参考）
- **系统调用号段**：0–19 原生 / 20–39 进程·调度·资源 / 40–89 文件·目录 I/O / 90–99 mmap·port / 100–109 时间 / 110–129 系统信息·动态链接·IPC / 130–149 SukiNative（130–143 已实现，144–149 返回 -ENOSYS，Phase 2）/ 150–199 网络（150–166 已实现，转发 NS_PORT→net_server/lwIP）/ 200+ 内核扩展（framebuffer/display/clone/signal 等）。
- **网络架构**：Ring0 `e1000.c` 只搬帧；用户态 `net_server.c` 跑 lwIP 2.2.1 raw API（`NO_SYS=1`）+ DHCP；内核 `kernel/net/socket.c` 把 150–166 翻译成 NS_PORT IPC；nettest 开机自检验证通路。
- **动态链接**：内核 `ld.suki` 解析 `DT_NEEDED` + `dlopen/dlsym/dlclose/dlerror`（syscall 126–129）；`.sl` 共享库置于 `/LIB/`；`dltest` 自检。
- **线程/信号**：`SYS_CLONE` + `SYS_FUTEX`（pthread 基座）；`SIGACTION`/`SIGRETURN`/`SIGPROCMASK`/`TKILL`/`RAISE` 已实现。

## 5. 验证
- 通读修订后 `README.md`，确认 §1–§8 各节与真实代码一致；修正了§5 分支图中 `keyboard.c`/`mouse.c` 重复归类（`arch/x86_64/` 已列，drivers 行改为只列 ata/ahci/e1000/hda/pci）。
- 本次为纯文档更新，**未执行 QEMU 运行**（无功能变更，无需回归）。

## 6. 提交
- commit：`docs: README.md 逐节核对并更新以反映真实系统状态（step71）`（不推送）。
