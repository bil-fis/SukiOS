# Step 84 — 移植 libcurl 到 SukiOS（HTTP 客户端库可用，零 panic 验证）

> 日期：2026-09-12
> 承接：`results/step81.md`（移植评估）、`step82.md`（网络 libc/时间/miniz）、`step83.md`（submodule 引入 mbedTLS/libcurl）。
> 目标：把 `lib/curl`（submodule，8.22.0）真正**编译为 `build/libcurl.a` 并跑通真实 HTTP 抓取**。
> 结论：完成。libcurl 已集成，开机自检 `curl_test` 对宿主机 HTTP 服务发起真实 GET，得 **HTTP 200 + 正确响应体**，两次 QEMU 回归零 panic。**HTTPS/TLS 后端（mbedTLS）为下一步**（本轮未接入）。

---

## 1. 总体思路

libcurl 无法在宿主运行 autotools/CMake 探测（本仓为 freestanding 交叉环境）。因此：

1. **不跑 configure**：手写 `user/lib/curl_config.h`，以 `-DHAVE_CONFIG_H` 让 `curl_setup.h` 经 `#include "curl_config.h"` 引入（`-I user/lib` 命中）。
2. **补齐 libc 缺口**：按编译/链接报错逐个补齐标准头与函数（见 §3）。
3. **构建** `build/libcurl.a`（`-r` 归档），链接进开机自检 `curl_test`。

---

## 2. curl_config.h 关键决策

- **类型尺寸**（x86_64 LP64）：`SIZEOF_INT=4 / LONG=8 / LONG_LONG=8 / VOIDP=8 / OFF_T=8 / CURL_OFF_T=8 / CURL_SOCKET_T=4 / SIZE_T=8 / TIME_T=8`。
- **仅 IPv4**：不定义 `ENABLE_IPV6`/`USE_IPV6`。
- **同步 resolver**：`HAVE_GETADDRINFO`/`HAVE_FREEADDRINFO`；不启用 c-ares / threaded resolver / `HAVE_GETHOSTBYNAME_R`。
- **无线程**：不定义 `HAVE_THREADS_POSIX`。
- **IPv4 + 无 unix socket**：不定义 `USE_UNIX_SOCKETS`。
- **关闭不需要的协议/功能**（官方 `CURL_DISABLE_*` 开关）：`DICT/FILE/GOPHER/IMAP/LDAP/LDAPS/MQTT/POP3/RTSP/SMTP/TELNET/WEBSOCKETS/DOH/ALTSVC/HSTS/AWS/HTTPSIG/MIME/FORM_API/NETRC/PROGRESS_METER/GETOPTIONS/SHA512_256/IPFS/SHUFFLE_DNS/BINDLOCAL/KERBEROS_AUTH/NEGOTIATE_AUTH`。
  保留：**HTTP（含 HTTPS 预留）/ FTP / TFTP**、proxy、cookies。
- **zlib**：`HAVE_LIBZ 1`，由 **miniz** 的 zlib 兼容层提供（见 `user/lib/shims/zlib.h`；`curl_version()` 报 `zlib/11.3.2`）。
- **TLS**：本轮不定义任何 `USE_*`（无后端）；`USE_MBEDTLS` 预留注释，待 mbedTLS 接入时启用。
- **关键坑（务必注意）**：libcurl 对 `HAVE_*` 一律用 `#ifdef` 判定，**「不具备」的能力必须完全不定义**，绝不能写 `#define HAVE_X 0`（那仍算已定义，曾导致 `struct sockaddr_storage` 未定义却被使用而报 incomplete type）。

---

## 3. 为 libcurl 补齐的 libc 能力

| 类别 | 内容 | 文件 |
| :-- | :-- | :-- |
| 标准头（新增） | `<sys/types.h>`、`<sys/stat.h>`、`<signal.h>` | `user/lib/shims/{sys/types.h, sys/stat.h, signal.h}` |
| `FILE` 流层（新增） | `FILE` 类型、`stdin/stdout/stderr`、`fopen/fdopen/fclose/fread/fwrite/fgets/fgetc/fputc/fputs/fflush/ferror/feof/clearerr/fseek/ftell/rewind/fprintf/vfprintf/fileno`（fd 薄封装，无缓冲） | `user/lib/fileio.c`（+ `user/lib/shims/stdio.h`） |
| 字符串 | `strspn` / `strcspn` / `strpbrk` | `user/lib/string.c`（+ `shims/string.h`） |
| 查找 | `bsearch` | `user/lib/stdlib.c`（+ `shims/stdlib.h`） |
| 文件元数据 | `stat` / `fstat` / `lstat`（包装 SYS_STAT/FSTAT/LSTAT；布局与内核 `suki_stat_t` 一致） | `user/lib/unistd.c` |
| 主机名 | `gethostname`（SYS_GETHOSTNAME，失败回退 "sukios"） | `user/lib/unistd.c` |
| errno | 补 `EWOULDBLOCK/EADDRINUSE/EADDRNOTAVAIL/EAFNOSUPPORT/EISCONN/ECONNABORTED/ECONNRESET/EDESTADDRREQ/EHOSTDOWN/ENETDOWN/ENETRESET/ENETUNREACH/ENOBUFS/ENOTCONN/ENOPROTOOPT/EPFNOSUPPORT/EPROTOTYPE/EPROTO/ESHUTDOWN/ESOCKTNOSUPPORT/EOVERFLOW/EBADMSG/EBADR/ENODATA/ENOMSG/ENOSTR/ETIME/EIDRM/EMULTIHOP/ENOTBLK/ENOLINK` | `user/lib/shims/errno.h` |
| socket 常量 | `PF_UNSPEC/PF_UNIX/PF_LOCAL/PF_INET/PF_INET6`；`<sys/socket.h>` 现提供 `fd_set`/`FD_*`（POSIX 语义，对齐 glibc） | `user/lib/shims/sys/socket.h` |
| 信号 | 按 glibc 做法补 `sa_handler`/`sa_sigaction` 兼容宏（供第三方直接写 `act.sa_handler`） | `user/lib/libc.h`、`user/lib/shims/signal.h` |

**顺带修复的既有 libc 缺陷**：
- `tolower()` 实现错误（`c - 'a' + 'A'`，应为 `c - 'A' + 'a'`）→ 已修（`user/lib/string.c`）。
- `libc.h` 中 `typedef struct stat stat;` 与函数 `stat()` 冲突（"different kind of symbol"）→ 删除该 typedef（无使用点）。
- `freetype_shim.h` 自定义的私有 `FILE` 与新增标准 `FILE` 冲突 → 改为复用标准 `FILE`（`#include <stdio.h>`）。

---

## 4. 构建集成（Makefile）

```make
CURL_DIR    := lib/curl
CURL_SRCS   := lib/*.c + lib/curlx/*.c + lib/vauth/*.c + lib/vdns/*.c + lib/vtls/*.c + lib/vquic/vquic.c
CURL_LIB    := $(BUILD)/libcurl.a                       # 775 080 B（-r 归档，183 个对象）
CURL_CFLAGS := -ffreestanding -nostdlib -std=gnu11 -O2 ... -DBUILDING_LIBCURL -DHAVE_CONFIG_H -DCURL_STATICLIB
               -I lib/curl/include -I lib/curl/lib -I user/lib -I user/lib/shims -I include
```

- `user/lib/fileio.c.o` 加入 `USER_LIB_OBJS`。
- 新增内嵌服务 `curl_test`：`USER_PROGS` 增 `curl_test`；专用 `.c.o`/`.elf` 规则链接 `USER_LIB_OBJS + libcurl.a + libminiz.a`；`kernel/kmain.c` 在 `nettest` 之后 `task_create_user(user_curl_test_start, ...)` 拉起。
- 产物：`build/user/curl_test.elf` = 598 552 B；`build/kernel.ski` ≈ 1.96 MB；ISO 13.8 MB。

来源：`lib/curl` 为 submodule（tag `curl-8_22_0`），构建时保持其目录结构（`lib/easy.c → build/curl/lib/easy.c.o`）。

---

## 5. 迭代排错记录（编译→链接→运行）

编译期（`make -k build/libcurl.a`，最终 0 error）依次解决：
1. `sigpipe.h: signal.h: No such file` → 新增 `<signal.h>` shim（含 `struct sigaction`、`SIGPIPE`、`sigaction()`）。
2. `EWOULDBLOCK/EISCONN/EAFNOSUPPORT/EADDRNOTAVAIL/EADDRINUSE undeclared` → 补 `errno.h` 网络错误码。
3. `sockaddr.h: field 'sa_stor' has incomplete type` → 根因是我此前写了 `#define HAVE_STRUCT_SOCKADDR_STORAGE 0`；**`#ifdef` 仍算已定义**，改为完全不定义。
4. `PF_INET undeclared` → 补 `PF_*`。
5. `strpbrk/strcspn/strspn/bsearch/fdopen implicit` → 补实现与声明。
6. `curl.h 中 fd_set unknown` → 依 POSIX/glibc 让 `<sys/socket.h>` 暴露 `fd_set`。
7. `'stat' redeclared as different kind of symbol` → 删除 `libc.h` 里冲突的 `typedef struct stat stat;`。
8. `fontsrv.o: conflicting types for 'FILE'` → `freetype_shim.h` 改用标准 `FILE`。

链接期（`build/user/curl_test.elf`）：
9. `undefined reference to Curl_conn_may_http3` → 该符号定义在 `lib/vquic/vquic.c`（我在首轮按目录裁剪时排除了 vquic），显式纳入该文件。
10. `undefined reference to fstat`（经 `curlx_fstat`）→ 在 libc 实现 `stat/fstat/lstat`。

运行期：
11. **DHCP 时序竞态**：`nettest`/`curl_test` 在 DHCP BOUND 前耗尽重试（各为 50 / 60 次纯 `yield` 自旋，微秒级耗尽，而 DHCP 需数十毫秒），且两者互相抢 CPU 加剧。修复：两者改为**每 20ms `nanosleep` 一次、共约 8s 预算**（既给 DHCP 墙钟时间、又不与 `net_server` 争抢），并降低日志频率。修复后两轮回归均稳定通过。

---

## 6. 端到端验证（QEMU 生产场景）

### 6.1 方法
```bash
printf 'Hello from SukiOS host HTTP server!\nlibcurl on SukiOS works. 0123456789\n' > /tmp/httproot/hello.txt
(cd /tmp/httproot && python3 -m http.server 8000 &)      # 宿主机 HTTP 对端（QEMU user-net: 10.0.2.2）
make iso disk
timeout 170 make run-headless QEMU_SERIAL="-serial file:/tmp/suki_curl.log"
grep -inE "panic|#GP|#PF" /tmp/suki_curl.log             # 期望无
grep -nE "\[curl\]|DHCP BOUND|nettest\]|POSIX test summary|libc-test\]" /tmp/suki_curl.log
```

### 6.2 结果（两轮均一致，零 panic）
```
[net] DHCP BOUND: ip=10.0.2.15 gw=10.0.2.2 mask=255.255.255.0
[curl] === libcurl easy GET ===
[curl] URL: http://10.0.2.2:8000/hello.txt
[curl] libcurl version: libcurl/8.22.0-DEV zlib/11.3.2
[curl] HTTP status=200 body=72 bytes
[curl] body: Hello from SukiOS host HTTP server!
[curl] libcurl HTTP GET: PASS
[nettest] POSIX UDP round-trip (with reply): PASS
[nettest] libc-net API: PASS=4 FAIL=0  ALL OK
[libc-test] PASS=11 FAIL=0  ALL OK
=== POSIX test summary: PASS=192 FAIL=0 ===
```
即：**libcurl 经「easy API → 内核 socket(150-166) → net_server(lwIP) → 真实 TCP → 宿主机 HTTP」完成了一次真实 GET**，状态码与响应体字节均正确。

---

## 7. 文件清单

**新增**
```
user/lib/curl_config.h          user/lib/fileio.c
user/lib/shims/sys/types.h      user/lib/shims/sys/stat.h      user/lib/shims/signal.h
user/apps/curl_test.c
```
**修改**
```
Makefile                 user/lib/libc.h       user/lib/stdio.c      user/lib/string.c
user/lib/stdlib.c        user/lib/unistd.c     user/lib/signal.c     user/lib/freetype_shim.h
user/lib/shims/stdio.h   user/lib/shims/unistd.h   user/lib/shims/errno.h
user/lib/shims/string.h  user/lib/shims/stdlib.h   user/lib/shims/sys/socket.h
user/lib/shims/zlib.h    user/apps/nettest.c   kernel/kmain.c        README.md
```

**关键常量/产物**
```
libcurl 版本: libcurl/8.22.0-DEV    zlib(miniz) 兼容层版本: 11.3.2
build/libcurl.a = 775 080 B（183 对象）    build/user/curl_test.elf = 598 552 B
CURL_CFLAGS 关键宏: -DBUILDING_LIBCURL -DHAVE_CONFIG_H -DCURL_STATICLIB
```

---

## 8. 未完成 / 下一步（明确说明）

- **HTTPS / TLS 后端（mbedTLS）**：本轮**未接入**，故 `https://` 暂不支持（`curl_config.h` 已预留 `USE_MBEDTLS`）。
  落地方案：把 `lib/mbedtls`（submodule，3.6.7 LTS）的 `library/*.c` 编译为静态库，`mbedtls_config.h` 裁剪；
  提供用户态平台适配——熵源（x86_64 `RDRAND`，CPUID 0x01 ECX bit30；否则 RTC/cycle 软 PRNG 播种）、
  timing（`gettimeofday`）；链接时以 `-DUSE_MBEDTLS` 让 libcurl 走 `lib/vtls/mbedtls.c`。届时 `curl_test` 增加 HTTPS 用例。
