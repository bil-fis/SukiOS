# Step 81 — 移植 libcurl 到 SukiOS：需要移植的内容评估

> 日期：2026-09-12
> 性质：**评估（不实现）**。目的：查清 SukiOS 现状能力，对照 libcurl 依赖，列出真正需要移植/补齐的内容、构建集成方式与验证路径，供后续分期实施。
> 结论速览：libcurl 源码本身可裁剪后基本原样编译；真正的移植量在 **SukiOS 侧适配**——①补齐 libc 网络/DNS/select/fcntl 包装与头文件；②提供 TLS 后端（推荐 mbedTLS）；③以手写 `curl_config.h` + 仿 FreeType 的静态库方式集成进 `Makefile`。

---

## 1. SukiOS 现状能力盘点（实测）

### 1.1 网络通路（已具备，且经生产验证）
- lwIP 2.2.1 跑在**用户态** `user/net_server.c`（`NO_SYS=1` raw API，e1000 帧经 `NET_PORT` Mach IPC 收发；`sys_now()` 由 `time.c` 驱动定时器）。源码编入见 `Makefile` 第 143-171、554-571 行。
- **POSIX socket 号位 150-166 已实现并路由到 net_server（非 ENOSYS）**：证据——`user/apps/nettest.c` 用 `suki_syscall5(SYS_SOCKET, SUKI_AF_INET, SUKI_SOCK_DGRAM,0)` 建 UDP socket，往返 QEMU 内置 TFTP（`10.0.2.2:69`）打印 `POSIX UDP round-trip (with reply): PASS`；上层 SukiNative `SukiSocketCreate/...`（`user/lib/suki_native.c` 第 212 行起）同样可用。
- `select`/`poll` 号位 81/82、`fcntl` 56、`ioctl` 64 已在 `include/sukios/posix.h` 预留（但**对 socket fd 的完整支持未验证**，见风险 R1/R3）。
- lwIP 自带 `dns.c`/`netdb.c`（`lwip_getaddrinfo`/`lwip_gethostbyname`），但**仅 net_server 私有**，应用态无法直接调用。

### 1.2 libc 已具备（对 libcurl 有用，`user/lib/`）
- 堆：`malloc/calloc/realloc/free`（`stdlib.c` 第 66/102/140/149 行）。
- 字符串：`mem*`/`str*`、`strdup`/`strndup`、`strerror`（`string.c` 第 72/81 行）。
- stdio：`printf/puts/snprintf/vsnprintf`（`stdio.c`）。
- 文件/进程：`open/read/write/close/fork/exec/getcwd`（`unistd.c` 第 81 行 `getcwd`）。
- 时间：`time/gettimeofday/clock_gettime`（`time.c`）——但 `user/lib/shims/time.h` 第 7 行**明确未实现** `struct tm/localtime/strftime` 等日历换算。
- 目录：`opendir/readdir`（`dirent.c`）。
- 信号：`sigaction/signal/kill/raise`（`signal.c`）。
- 线程：`pthread.c/.h`（单核下有限）；动态加载：`dlfcn.c`（dlopen/dlsym/dlclose）。
- 其它：`getopt/getopt_long`、`mmap/munmap`、`errno`、`clone/futex/arch_prctl`（见 `libc.h` 第 233-273 行）。

### 1.3 工具链与 vendored 库范式
- 用户态编译器：`x86_64-sukios-elf-gcc`（`Makefile` 第 29/36 行，`USER_CC`），**freestanding**，`-nostdlib -static -no-pie`（第 270/279 等）。
- 第三方库范式（直接可照搬）：
  - **lwIP**：源文件编为 `$(BUILD)/lwip/%.c.o`，特殊 include 路径，链入 `net_server.elf`（`Makefile` 第 143-171、554-571 行）。
  - **FreeType**：编为 `$(BUILD)/libfreetype.a`，并用 `user/lib/freetype_shim.h` 经 `FT_CONFIG_STANDARD_LIBRARY_H` 屏蔽宿主依赖（`Makefile` 第 284-314 行）。

---

## 2. libcurl 依赖清单（按其源码实际引用）

按 libcurl 源码特征（C 可裁剪、feature-detection 宏驱动），依赖分四类：

| 类别 | 需要的内容 | SukiOS 现状 |
| :-- | :-- | :-- |
| 标准 C | `stdint/inttypes/stdarg`（有）；完整 `stdio/string/stdlib`（基本有） | 基本满足 |
| 网络 API | `socket/bind/connect/listen/accept/send/recv/sendto/recvfrom/setsockopt/getsockopt/getpeername/getsockname/shutdown`、`select/poll`、`fcntl(O_NONBLOCK)`、`getaddrinfo/gethostbyname`、`inet_pton/inet_ntop`、`htons/ntohl` | **几乎全缺**（仅内核号位 150-166 可用，无 libc 包装） |
| 网络头 | `<sys/socket.h>` `<netinet/in.h>` `<arpa/inet.h>` `<netdb.h>` `<sys/select.h>` `<poll.h>` `<fcntl.h>` `<sys/time.h>` | **全部缺失**（仅有内核 `kernel/net/socket.h`，非用户态接口） |
| 时间 | `gettimeofday`（有）；`gmtime_r/strftime`（无，cookie/日期解析用） | 部分缺失 |
| TLS | OpenSSL / mbedTLS / GnuTLS / BearSSL 之一（**必备，否则无 HTTPS**） | **完全缺失** |
| 可选 | zlib（压缩）、c-ares（异步 DNS）、`getpwuid/getuid/gethostname`（可关闭） | 缺失但可关 |

---

## 3. 缺口评估（按工作量分级）

### P0 必须 —— 补齐 libc 网络/基础 API（工作量：中）
1. **BSD socket libc 包装**：新建 `user/lib/net.c`，对每个函数 `suki_syscallN(SYS_*, ...)`（号位取自 `posix.h` 第 729-746 行），实现 `socket/bind/connect/listen/accept/send/sendto/recv/recvfrom/setsockopt/getsockopt/getpeername/getsockname/shutdown/socketpair`，并统一用 `libc.h` 的 `libc_ret()` 做 `-errno→errno/-1` 语义。
2. **网络头文件**（建议放 `user/lib/shims/`，与现有 `shims/*.h` 一致）：
   - `<sys/socket.h>`：`struct sockaddr`、`socklen_t`、`AF_INET/AF_INET6`、`SOCK_STREAM/SOCK_DGRAM`、`MSG_*`、`struct msghdr/cmsghdr`；值映射到 `posix.h` 的 `SUKI_AF_*`/`SUKI_SOCK_*`/`SUKI_MSG_*`。
   - `<netinet/in.h>`：`struct sockaddr_in`、`struct in_addr`、`htons/ntohs/htonl/ntohl`（**当前完全缺失**，手写内联）、`INADDR_*`。
   - `<arpa/inet.h>`：`inet_pton`/`inet_ntop`（实现，可基于 `lwip/inet_chksum` 思路或纯手写）。
   - `<netdb.h>`：`struct addrinfo/hostent`、`getaddrinfo/gethostbyname/freeaddrinfo/gai_strerror` 声明（实现见下 4）。
3. **select/poll 包装**：`syscall 81/82` 包装 + `<sys/select.h>`(`fd_set`/`FD_*`)、`<poll.h>`(`struct pollfd`)；**需先确认内核侧对 socket fd 的 select/poll 支持**（风险 R1）。
4. **fcntl 包装**：`syscall 56`，至少 `F_SETFL/F_GETFL` + `O_NONBLOCK`（libcurl 非阻塞 I/O 主路径必需）。
5. **DNS 解析器（P0 最易被低估的关键项）**：
   - **方案A（推荐）**：扩展 `NET_PORT` 协议，新增 `NET_MSG_RESOLVE(name)→ip`，由 `net_server.c` 调 lwIP 已有 `dns_gethostbyname`（`lib/lwip-2.2.1/src/core/dns.c`，需把异步回调改同步：net_server 主循环轮询 dns 结果后应答 IPC）；`user/lib` 实现 `getaddrinfo/gethostbyname/freeaddrinfo/gai_strerror` 发该 IPC。**零新依赖**，复用现有 DNS 基础设施。
   - 方案B：移植 c-ares（独立 UDP 解析库）。更重，不优先。
   - 只要提供 POSIX `getaddrinfo`，libcurl 默认解析路径即通，**无需改 libcurl 源码**。
6. **时间**：补最小 `gmtime_r`（及其日历换算），或关闭 libcurl 相关特性（cookie/date 解析）。

### P1 必须 —— TLS 后端（工作量：大，最大块）
- **推荐 mbedTLS**（Apache-2.0，最易嵌入，libcurl 一等支持）。OpenSSL 过重、BearSSL API 偏门。
- 移植内容：
  - 编译 `library/*.c` 为 `libmbedtls.a`/`libmbedcrypto.a`/`libmbedx509.a`（仿 FreeType 模式 + `mbedtls_config.h` 关不需要功能）。
  - 平台适配层（`mbedtls/platform.h` 对应）：
    - **熵源**：`mbedtls_entropy_func` → x86_64 `RDRAND`（CPUID 0x01 ECX bit30，需确认 QEMU 实现；否则用 RTC+cycle 软 PRNG 播种）——子任务 R4。
    - **timing**：`mbedtls_timing_hardclock`/`get_timer` → `gettimeofday`。
    - 网络层不需（HTTPS 走 libcurl 自己的 socket）。
  - libcurl 以 `-DUSE_MBEDTLS` 链接三库。
- 若首期不需要 HTTPS：libcurl 配 `--without-ssl`（`CURL_DISABLE_SSL`）先跑通明文 HTTP，TLS 作为后续分期（不影响 P0 评估）。

### P2 可选 —— 压缩/异步 DNS/IPv6
- **zlib**：HTTP 压缩；可关（`CURL_DISABLE_DEFLATE`/`CURL_DISABLE_GZIP`）或移植 `miniz`（内核 `kernel/abilities/miniz/miniz.c` 搬用户态）。
- **c-ares**：默认用同步 `getaddrinfo`，不需要。
- **IPv6**：建议首期 `CURL_DISABLE_IPV6=1`，避免 `AF_INET6` plumbing。

---

## 4. libcurl 自身构建集成方案

- **vendor**：仓库放 `lib/curl-X.Y.Z`（源码，与 `lib/lwip-2.2.1`、`lib/freetype-2.14.3` 同级）。
- **不跑 `./configure`**：freestanding 交叉无法运行宿主 configure。改为：
  - 手写 `curl_config.h`（`HAVE_*`/`SIZEOF_*`/缺省开关），屏蔽 host 探测。
  - 仿 FreeType 提供 `curl_shim.h`（经 `CURL_CONFIG_STANDARD_LIBRARY_H` 或等价宏指向自定义配置），屏蔽 `<unistd.h>` 之外的宿主特性。
  - `Makefile` 新增 `CURL_OBJS`（编译 `lib/*.c` + `src/*.c` 所需源），产出 `$(BUILD)/libcurl.a`。
  - 关键开关（经 `curl_config.h` 表达，等价于 `./configure` 选项）：
    - HTTP 开；HTTPS 由 TLS 决定；FTP 可开（依赖 socket）；LDAP/GOPHER/DICT/TELNET/... 全关；
    - `CURL_DISABLE_IPV6=1`；`CURL_DISABLE_THREADED_RESOLVER=1`（用同步 `getaddrinfo`，避免依赖线程）；
    - `HAVE_GETTIMEOFDAY=1`、`HAVE_SIGACTION=1`、`HAVE_STRCASECMP`/等来自 `string.c`；
    - 关闭 `CURL_DISABLE_CRYPTO_AUTH` 视 TLS 而定。
- **链接**：应用 `-lcurl` + `-lmbedtls -lmbedx509 -lmbedcrypto` + 用户库（`USER_LIB_OBJS`）。

---

## 5. 验证方案（QEMU 生产场景，零 panic；遵循记忆 30993985/56133661）

- **测试程序**：`curl_test` 应用，调用 `curl_easy_perform` 抓取 URL。
- **网络通道**：QEMU `-net user` 下 guest 出访宿主机 `10.0.2.2`。宿主起 `python3 -m http.server 8000`（或复用已验的 TFTP），SukiOS 内 `curl http://10.0.2.2:8000/foo.txt`。
- **逐层验证**：
  1. **DNS**：先用 IP 直连绕过，再开 `NET_MSG_RESOLVE` 域名解析（R5 之前先 IP）。
  2. **TCP+HTTP**：明文抓取成功 = socket/connect/send/recv/select/fcntl 链路打通。
  3. **HTTPS**：启用 mbedTLS 后抓 `https://...`（自签或真实可达），验证 TLS 握手。
- **判定**：`-serial file:/tmp/x.log` 落盘，`cat -v`/`grep` 确认无 `PANIC`/`#GP`/`page fault`，且 curl 返回内容字节与宿主文件一致（可让 curl 写文件经 `fs_server` 读回，或 serial 打印前若干字节比对）。headless 下不人工看网页，以字节比对为通过依据（记忆 71761331 的交互限制在此不触发，因验证走网络回环+文件读回，无需人工看屏）。

---

## 6. 风险与待确认（R）

- **R1**：内核 `select`/`poll`（syscall 81/82）是否对 socket fd 完整支持（`posix.h` 注释曾称 ENOSYS）。nettest 仅验 UDP 收发，未验 socket 上的 select/poll。须先补/验证。
- **R2**：TCP 流式 `connect/send/recv` 在「内核→net_server→lwIP」代理路径上的完整语义（阻塞/超时/错误码映射）仅在 UDP 验过；HTTP 必经 TCP，须先做 TCP 冒烟。
- **R3**：非阻塞 `fcntl(O_NONBLOCK)`+`select` 组合是 libcurl 主路径；内核 socket 实现须支持非阻塞与 `EWOULDBLOCK` 映射。
- **R4**：mbedTLS 熵源在 QEMU 的可用性（RDRAND 是否实现）；否则软 PRNG 播种。
- **R5**：libcurl 大量 feature-detection 宏，逐一审 `curl_config.h` 时可能暴露更多缺失 libc 函数（`getpwuid`、`gethostname`、`strerror_r`、`getaddrinfo` 细节等），按 `HAVE_*` 关闭或补桩。
- **R6**：单核（`SMP` 默认关，记忆 62710060）+ 同步 DNS，libcurl 主线程阻塞等网络，符合 net_server 阻塞-IPC 模型，无死锁风险（与 nettest 一致）。

---

## 7. 工作量与分期建议

- **第1期（P0 网络 libc + DNS，跑通 HTTP）**：中。先做 TCP 冒烟（R2）+ select/poll/fcntl（R1/R3）+ DNS（NET_MSG_RESOLVE）。
- **第2期（P1 mbedTLS，跑通 HTTPS）**：大。熵源+timing+编译三库+`curl_config` `USE_MBEDTLS`。
- **第3期（P2 压缩/精细化）**：小。

---

## 8. 结论

libcurl 本体源码基本可裁剪后原样编译（C 可移植、feature 宏驱动）；**真正的移植工作量 100% 在 SukiOS 侧适配**：
1. 补齐 libc 的 BSD socket/select/poll/fcntl 包装与 `<sys/socket.h>`/`<netinet/in.h>`/`<arpa/inet.h>`/`<netdb.h>` 头；
2. 新增 DNS 解析通道（扩展 `NET_PORT` + 复用 lwIP `dns.c`，或移植 c-ares）；
3. 提供 TLS 后端（mbedTLS 推荐）；
4. 以手写 `curl_config.h` + 仿 FreeType 的静态库构建方式集成进 `Makefile`。

建议按上面三期推进，每期均以 QEMU 生产场景零 panic 回归（含 TCP 冒烟、DNS、HTTP、HTTPS 逐级验证）。
