# Step 82 — 补全网络 API/头、时间内容，移植 miniz（mbedTLS 延后）

> 日期：2026-09-12
> 承接：`results/step81.md`（libcurl 移植评估）。
> 本轮范围（按用户指令「先完成不需要下载额外代码的部分」）：
> ① 补全 BSD socket 的 **libc 包装 + 标准网络头**；② 补全 **时间（日历换算）内容**；
> ③ **移植 miniz**（zlib 兼容压缩库，作为 libcurl 的压缩后端）；④ **mbedTLS 延后**（需联网下载源码）。
> 结论：三项已全部落地并在 QEMU 生产场景回归通过（零 panic），验证中还发现并修复了一个
> 既有 libc 缺陷（`vsnprintf` 不支持 `0` 填充/精度，导致 `strftime` 输出非标准）。

---

## 1. 交付物总览

| 子系统 | 新增/修改 | 关键文件 |
| :-- | :-- | :-- |
| 网络 libc 包装 | 新增 | `user/lib/net.c` |
| 标准网络头（shims） | 新增 | `user/lib/shims/{sys/socket.h,netinet/in.h,arpa/inet.h,netdb.h,poll.h,fcntl.h,sys/select.h,sys/time.h}` |
| 内核 socket 就绪查询 | 修改 | `kernel/net/socket.c`、`kernel/syscall/sys_posix.c`、`include/kernel/net/socket.h`、`include/sukios/net.h`、`user/net_server.c` |
| socket ABI 常量 | 修改 | `include/sukios/posix.h`（`SUKI_MSG_*`/`SUKI_SO_*`/`SUKI_SHUT_*`/`SUKI_IP_*`/`SUKI_TCP_NODELAY`） |
| 时间内容 | 修改 | `user/lib/time.c`、`user/lib/shims/time.h` |
| miniz 移植 | 新增构建 | `Makefile`（`MINIZ_LIB`）、`user/lib/shims/zlib.h`、`user/lib/shims/assert.h` |
| libc 格式化修复 | 修改 | `user/lib/stdio.c`（`vsnprintf` 标志位/精度）、`user/lib/shims/unistd.h`（自包含） |
| 端到端自检 | 修改 | `user/apps/nettest.c`（libc 网络层）、`user/shell.c`（时间 + miniz，`libc_selftest`） |

---

## 2. 网络 API 与标准头（P0）

### 2.1 `user/lib/net.c`（新增，BSD socket libc 包装）

所有函数把内核返回的**负 errno** 统一转换为 POSIX 语义（`errno = -r; return -1`），
采用的转换器为局部 `net_ret()`（负值区间 `[-4095, 0)` 视为 errno）：

* **socket 基础**：`socket/bind/connect/listen/accept/accept4/send/recv/sendto/recvfrom/
  sendmsg/recvmsg/setsockopt/getsockopt/getpeername/getsockname/shutdown`，
  逐一包装内核号位 `SYS_SOCKET(150) … SYS_RECV(166)`。
  - `accept/getpeername/getsockname/recvfrom` 正确处理 `socklen_t *` 回写（内核写本地副本，用户态拷回）。
  - `sendmsg/recvmsg` 实现单 `iovec` 路径（覆盖 libcurl 典型用法）。
  - `socketpair` 返回 `EOPNOTSUPP`（本系统无 UNIX domain pair）。
* **I/O 多路复用**：`select(81)/poll(82)`、`fcntl(56)` 包装。
* **地址转换**：`inet_pton/inet_ntop/inet_addr`（仅 IPv4；IPv6 返回失败，配合 `CURL_DISABLE_IPV6`）。
* **名称解析**：`getaddrinfo/freeaddrinfo/gai_strerror/gethostbyname/getnameinfo`。
  - `getaddrinfo` 支持：数字地址直通、`AI_PASSIVE`（0.0.0.0）、服务名/端口数字（内建表：
    http/https/ftp/ssh/telnet/smtp/dns/domain/tftp/ntp/pop3/imap/snmp/http-alt/https-alt）。
  - 非数字主机名走 **DNS-over-UDP 解析器**（`dns_resolve_a`）：向默认 DNS 服务器
    `10.0.2.3`（QEMU user-net 内建转发；可用环境变量 `SUKI_DNS_SERVER` 覆盖）发送 A 查询，
    自解析 DNS 报文（支持名字压缩指针），仅取 A 记录。查询超时以 `poll()+recv()` 重试 25 次。

**关键 ABI 约定**：SukiOS socket 层在 syscall 边界统一使用**主机字节序**（`sin_port` 主机序、
`sin_addr.s_addr` 为 4 字节八位组 `a.b.c.d`），故 `htons/ntohs/htonl/ntohl` 为**恒等宏**
（见 `user/lib/shims/netinet/in.h`）。实测 nettest 以 `sin_port=69`（主机序）成功往返。

### 2.2 标准头（`user/lib/shims/`）

| 头 | 内容 |
| :-- | :-- |
| `<sys/socket.h>` | `struct sockaddr`、`socklen_t`、`AF_*`、`SOCK_*`、`MSG_*`、`SOL_SOCKET`、`IPPROTO_*`、`SO_*`、`IP_*`、`TCP_NODELAY`、`SHUT_*`、`struct iovec/msghdr`、全部 socket 函数原型；`ssize_t` 经守卫 `_SUKI_SHIM_SSIZE_T_DEFINED` 提供（自包含） |
| `<netinet/in.h>` | `struct in_addr`（`uint8_t s_addr[4]`）、`struct sockaddr_in`、`htons/ntohs/htonl/ntohl`（恒等）、`INADDR_*`、`INET_ADDRSTRLEN` |
| `<arpa/inet.h>` | `inet_pton/inet_ntop/inet_addr` |
| `<netdb.h>` | `struct hostent/addrinfo`、`AI_*`、`EAI_*`、`getaddrinfo` 族 |
| `<poll.h>` | `struct pollfd`（int fd + short events + short revents，布局与内核 `suki_pollfd_t` 一致）、`POLL*` |
| `<sys/select.h>` | `fd_set = suki_fd_set_t`（1024 位）、`FD_ZERO/SET/CLR/ISSET`、`select` |
| `<sys/time.h>` | `struct timeval` |
| `<fcntl.h>` | `F_SETFL/F_GETFL`、`O_NONBLOCK` 等 |

常量全部映射自 `<sukios/posix.h>` 的 `SUKI_*`（单一真相源，内核/用户值一致）。

### 2.3 内核侧：socket 的 `select/poll` 就绪查询（修复 R1）

step81 列出的风险 **R1**（内核 `select/poll` 对 socket fd 可能「假就绪」）本轮修复：

* 新增 Mach 消息 **`SOCK_MSG_POLL = 16`**（`include/sukios/net.h`）：
  `req.flags`=关注事件，`resp.result`=就绪掩码（`SUKI_POLL*`）。
* `user/net_server.c::sock_handle` 新增 `SOCK_MSG_POLL` 分支，基于 **lwIP 真实状态**计算掩码：
  - TCP：`rx_head||rx_eof`→`POLLIN`；`S_LISTENING && accept_pcb`→`POLLIN`；
    `tcp_sndbuf>0`→`POLLOUT`；`!pcb && S_CONNECTED`→`POLLERR`。
  - UDP：有数据→`POLLIN`；恒 `POLLOUT`（无发送流控）。
  - 未知句柄→`POLLNVAL`。
* `kernel/net/socket.c::net_poll(fd, want)`：经 `SOCK_MSG_POLL` 向 net_server 查询并把
  `resp.result` 返回内核（非 socket / 失败返回 0）。
* `kernel/syscall/sys_posix.c` 的 `sys_poll`/`sys_select` 在遍历 fd 时新增
  `FD_TYPE_SOCKET` 分支：调用 `net_poll()` 用真实 lwIP 状态填 `revents`，杜绝「普通文件恒就绪」式假阳性。

---

## 3. 时间内容补全

`user/lib/time.c` 在原有 `time/gettimeofday/clock_gettime/nanosleep/sleep/usleep` 基础上补齐
**完整日历换算**（纯 UTC，本系统无时区数据）：

* 采用 Howard Hinnant 的 `civil_from_days` / `days_from_civil`（无循环、无查表、整数精确）。
* 提供：`gmtime_r`、`gmtime`、`localtime_r`、`localtime`、`mktime`（含字段归一化）、
  `timegm`、`asctime`、`ctime`、`strftime`。
* `strftime` 支持的转换：`%Y %y %m %d %H %M %S %j %w %a %A %b %B %F %T %c %Z %z %%`。
* 注：`difftime` 返回 double，本仓用户态工具链禁用 SSE（`-mgeneral-regs-only`）无法经 XMM
  返回浮点，故**不提供** `difftime`（调用方以 `(t1-t0)` 相减替代）。
* `user/lib/shims/time.h` 补齐 `struct tm` 与上述函数声明。

---

## 4. miniz 移植（zlib 兼容压缩库）

* **源码**：复用仓库内 `kernel/abilities/miniz/miniz.c`（freestanding 友好，1.1 万行单文件）。
* **构建**（`Makefile`）：
  - `MINIZ_DIR := kernel/abilities/miniz`，产物 `$(BUILD)/libminiz.a`。
  - `MINIZ_CFLAGS`：`-ffreestanding -nostdlib -std=gnu11 -O2 -fcommon`
    `-DMINIZ_USE_ZLIB_COMPAT -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DNDEBUG`，
    经 `-include user/lib/shims/{string.h,stdlib.h,assert.h}` 提供宿主依赖（malloc/free）。
  - 链接为可重定位归档：`ld -r -Wl,--build-id=none -Wl,--allow-multiple-definition`。
* **兼容头** `user/lib/shims/zlib.h`：`#include <kernel/abilities/miniz/miniz.h>`，
  并**强制定义与库一致的 reduced-feature 宏**（`MINIZ_NO_STDIO/TIME/ARCHIVE_APIS`），
  以匹配本 libc 无 `FILE*` 抽象的客观约束——头文件声明的 API 子集与 `libminiz.a` 严格一致。
* **消费者**：`shell.elf` 链接 `libminiz.a`，由 shell 的 `libc_selftest` 做压缩/解压往返自检
  （`mz_compress2` → `mz_uncompress`）。libcurl 到来时可直接 `-lminiz` 并以 `HAVE_ZLIB` 接入。

---

## 5. 验证中发现并修复的 libc 缺陷（重要）

首轮 QEMU 验证中，时间自检报错：
```
[libc-test] FAIL gmtime_r/strftime (2021- 1- 1T 0: 0: 0)
```
根因：`user/lib/stdio.c::vsnprintf` 把 `%02d` 的前导 `0` 当作**宽度数字**解析（`width=2`），
因而始终以**空格**填充；且**不支持精度**（`%.3s`）。这使 `strftime` 的 `%m/%d/%H/%M/%S/%j`
（标准要求零填充）以及 `asctime` 的 `%.3s` 全部输出非标准——libcurl 的 cookie/日期解析会受影响。

修复 `vsnprintf`（`user/lib/stdio.c`），完整支持：
* **标志位**：`-`（左对齐）、`0`（数值域零填充）。
* **宽度**：数字或 `*`（负宽度等价左对齐）。
* **精度**：`.` 后数字或 `*`（`%.Ns` 截断字符串；`%.Nd` 最少位数补前导零；`%.0d(0)`→空）。
* **长度**：`l`/`ll`/`h`。
* 取绝对值改为 `-(v+1)+1` 形式，**消除 `INT64_MIN` 取负的 UB**。
* 数值零填充时符号位正确位于填充零之前（`-0003`）。

另修 `user/lib/shims/unistd.h`：补 `#include <errno.h>` 并给 `ssize_t` 加
`_SUKI_SHIM_SSIZE_T_DEFINED` 守卫，使 `<unistd.h>` 与 `<sys/socket.h>` 可任意包含顺序而自洽。

---

## 6. 构建集成（`Makefile`）

* `USER_LIB_OBJS` 加入 `$(BUILD)/user/lib/net.c.o`（网络包装随所有用户程序/服务链入）。
* miniz：新增 `MINIZ_SRCS/MINIZ_OBJS/MINIZ_LIB` 规则（见 §4）。
* `$(BUILD)/user/shell.elf` 依赖并链接 `$(MINIZ_LIB)`（供 shell 自检）。
* 内嵌自检 `nettest` 走既有 `USER_PROGS`→`.ssvc.blob.o`→内核，**无需改 kmain**；
  其磁盘版 `::BIN/NETTEST.SKA` 亦仍 < 64KiB（43 984 B，满足 execve OOL(16 页) 上限）。

---

## 7. QEMU 生产场景验证

### 7.1 命令（遵循记忆 30993985/56133661：仅 bash + QEMU 自身机制）
```bash
cd /mnt/d/Projects/SukiOS && make iso disk
timeout 160 make run-headless QEMU_SERIAL="-serial file:/tmp/suki3.log"
grep -inE "panic|#GP|#PF|page fault" /tmp/suki3.log
grep -nE "libc-test\]|libc-net API|nettest\]|POSIX test summary" /tmp/suki3.log
```
（`make iso disk` 全程无 error；`-serial file:` 落盘后由 `cat/grep` 判读，未使用任何外部调试器/脚本。）

### 7.2 结果（零 panic，全绿）
```
[fs] self-test ALL PASS
[boot] display-server ready (g_display_active=1, waited=0 yield rounds); mounting dependent services...
[libc-test] miniz roundtrip OK (115 -> 101 bytes)
[libc-test] PASS=11 FAIL=0  ALL OK                       ← libc 自检（含时间 + miniz）
[nettest] POSIX UDP round-trip (with reply): PASS        ← 内核→net_server→lwIP 真实 UDP 往返
[nettest] libc poll()+recvfrom() got 21 bytes
[nettest] libc-net API: PASS=4 FAIL=0  ALL OK            ← inet_* / getaddrinfo / socket 包装 / poll
[nettest] all done.
=== POSIX test summary: PASS=192 FAIL=0 ===
```
* `[libc-test]` 覆盖：getenv/setenv、getopt/getopt_long、opendir/readdir、strerror、
  expand_vars、**gmtime_r/strftime/mktime 往返**（2021-01-01T00:00:00Z ↔ 1609459200，周五/第0天）、
  gettimeofday/clock_gettime、**miniz 压缩解压回环**（115→101 字节且逐字节一致）。
* `libc-net API PASS=4`：① inet_pton/ntop/addr；② getaddrinfo(ip,port) + 服务名(http→80)；
  ③ libc `socket()/sendto()/poll()/recvfrom()/close()` 对 QEMU TFTP(10.0.2.2:69) 的真实往返。
* 无 `PANIC`/`#GP`/`#PF`；唯一 `WARN` 是 `kernel/diagnostics.c` 的**故意**触发项（自检设计）。
* 产物尺寸：`kernel.ski` 1 347 880 B；`build/user/shell.elf` 121 912 B（含 miniz）；
  `build/apps/nettest.elf` 43 984 B。

---

## 8. 未完成 / 后续（明确说明，非 stub）

* **mbedTLS（P1）**：本轮**未做**，属用户指令「先完成不需要下载额外代码的部分」之外——需
  先下载 mbedTLS 源码。落地路径已在 step81 §3 给出：编译三库 + `mbedtls_config.h` 裁剪 +
  熵源（x86_64 `RDRAND` 或 RTC/cycle 软 PRNG）+ timing 适配 + libcurl `-DUSE_MBEDTLS`。
* **libcurl 本体集成**：待 mbedTLS 就绪后，以手写 `curl_config.h` + `libcurl.a` 方式集成，
  复用本轮已备好的 socket/DNS/select/fcntl/时间/zlib 全部底座。
* 风险 R2/R3（TCP 流式语义、`fcntl(O_NONBLOCK)`+`select` 组合）本轮内核已具备 `net_poll`
  真实状态查询；完整 TCP 冒烟与 libcurl 联调留待 libcurl 集成阶段（本轮 UDP 往返与 poll 已验）。

---

## 9. 文件清单

**新增**
```
user/lib/net.c
user/lib/shims/sys/socket.h  user/lib/shims/sys/select.h  user/lib/shims/sys/time.h
user/lib/shims/netinet/in.h  user/lib/shims/arpa/inet.h   user/lib/shims/netdb.h
user/lib/shims/poll.h        user/lib/shims/fcntl.h       user/lib/shims/zlib.h
user/lib/shims/assert.h
```
**修改**
```
Makefile                       include/kernel/net/socket.h   include/sukios/net.h
include/sukios/posix.h         kernel/net/socket.c           kernel/syscall/sys_posix.c
user/net_server.c              user/lib/time.c               user/lib/shims/time.h
user/lib/stdio.c               user/lib/shims/unistd.h
user/apps/nettest.c            user/shell.c
```
**关键常量/接口**
```
SYS_SOCKET..SYS_RECV = 150..166     SYS_SELECT = 81   SYS_POLL = 82   SYS_FCNTL = 56
SOCK_MSG_POLL = 16                  net_poll(fd, want) -> uint32_t（kernel/net/socket.c）
DNS 默认服务器 10.0.2.3（SUKI_DNS_SERVER 可覆盖）    DNS 端口 53
miniz 产物 $(BUILD)/libminiz.a      压缩级别 MZ_DEFAULT_LEVEL = 6   MZ_OK = 0
```
