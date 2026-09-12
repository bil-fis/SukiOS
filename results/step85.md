# Step 85 — zlib 子模块化 + miniz 内核能力 + mbedTLS 接入 + curl 命令行应用

> 日期：2026-09-12
> 承接：`step84.md`（libcurl 首次集成，仅 HTTP）
> 本轮四项：
> **A** 以 git submodule 引入 zlib 并完成兼容（替换用户态 miniz 兼容层）
> **B** miniz 作为内核能力嵌入 Ring0（为后续 minOSEnv / rootfs 做准备）
> **C** 完成 mbedTLS 接入（libcurl 支持 HTTPS/TLS）
> **D** 创建 curl 命令行应用（`::BIN/CURL.SKA`）
> 结论：四项全部落地。**QEMU 回归零 panic**，内核 miniz 自检、真实 zlib、libcurl HTTP/HTTPS、POSIX 192、curl CLI 均通过。

---

## A. zlib 子模块化与兼容

### A.1 引入与构建
```
git submodule add --depth 1 https://github.com/madler/zlib.git lib/zlib   # 检出 tag v1.3.1
```
- 产物 `build/libz.a`（75 504 B，11 个对象）。
- 只编译**核心编解码源**（`adler32/compress/crc32/deflate/infback/inffast/inflate/inftrees/trees/uncompr/zutil`），
  **排除 `gz*` 文件流实现**（本仓用户态无文件流需求）。
- 编译参数：`-DHAVE_HIDDEN=0 -DNO_GZCOMPRESS -DNO_GZIP -I lib/zlib -I include -I user/lib/shims`。
  （未用 `Z_SOLO`——那样 `zcalloc/zcfree` 需要调用方自行提供；走默认路径由本 libc 的 `malloc/calloc/free` 提供。）

### A.2 切换与清理
- **删除** `user/lib/shims/zlib.h`（原基于 miniz 的 zlib 兼容 shim）。
- `USER_CFLAGS` 增加 `-I lib/zlib`；`CURL_CFLAGS` 增加 `-I lib/zlib`。
- shell `libc_selftest` 的压缩回环改用真实 zlib（`compress2`/`uncompress`/`Z_OK`/`uLongf`）。
- 用户态不再构建 `libminiz.a`；`shell.elf`/`curl_test.elf` 改为链接 `libz.a`。
- **注意**：删除 shim 后必须强制重编 libcurl（`make -B`），否则残留对象仍引用 `mz_*` 符号。

### A.3 运行时验证
```
[libc-test] zlib roundtrip OK (102 -> 89 bytes)
[curl] libcurl version: libcurl/8.22.0-DEV mbedTLS/3.6.7 zlib/1.3.1
```
（版本号由原先的 `zlib/11.3.2`(miniz) 变为 **`zlib/1.3.1`**，即真实 zlib。）

---

## B. miniz 作为内核能力（Ring0）

### B.1 形态
- 源码仍在 `kernel/abilities/miniz/`（该目录默认被 `C_SRCS` 的 `find ... grep -v '/abilities/'` 排除），
  本轮**显式**把 `miniz.c` 与新增的 `kminiz.c` 编入内核。
- 对外 API：`include/kernel/abilities/kminiz.h`
  ```
  kminiz_compress / kminiz_uncompress / kminiz_compress_bound / kminiz_adler32 / kminiz_crc32 / kminiz_selftest
  ```
- 实现：`kernel/abilities/miniz/kminiz.c`（包装 `mz_compress2` / `mz_uncompress`）。

### B.2 freestanding 适配
miniz.h 无条件包含 `<assert.h>/<stdlib.h>/<string.h>`，内核没有这些头，为此新增最小垫片目录
`kernel/abilities/miniz/shim/`（仅对 miniz 编译单元生效，`-I` 额外优先级）：
- `shim/assert.h` → `__kminiz_assert_fail()`（打印告警，不 panic）
- `shim/stdlib.h` → 声明 `malloc/calloc/realloc/free`
- `shim/string.h` → 转发 `<kernel/string.h>` + 补 `memchr`

分配映射：`malloc/calloc/realloc/free` → `kmalloc/kzalloc/krealloc/kfree`。

### B.3 内核侧改动
- **新增 `krealloc()`**（`kernel/mm/kmalloc.c` + `include/mm/kmalloc.h`）：内核堆原只有
  `kmalloc/kzalloc/kfree`；利用块头的 `size` 字段实现（缩小时原地返回，扩大时新分配+拷贝+释放）。
- `kmain.c`：在 `kheap_init()`/`mm_selftest()` 之后调用 `kminiz_selftest()`。

### B.4 运行时验证
```
[kminiz] selftest OK (110 -> 103 bytes, zlib-style roundtrip)
```
（内核 2.9→3.05 MB；用户态改用真实 zlib，miniz 仅内核使用。）

---

## C. mbedTLS 接入（HTTPS）

### C.1 构建
- 子模块 `lib/mbedtls` @ `mbedtls-3.6.7`（3.6 LTS），编译 `library/*.c`（109 文件）→ `build/libmbedtls.a`（1 247 792 B）。
- 配置：`-DMBEDTLS_CONFIG_FILE='"suki_mbedtls_config.h"'` → `user/lib/suki_mbedtls_config.h`
  **先包含官方默认配置，再按 SukiOS 客观能力裁剪**：
  | 项 | 处理 | 原因 |
  |---|---|---|
  | `MBEDTLS_ENTROPY_HARDWARE_ALT` + `MBEDTLS_NO_PLATFORM_ENTROPY` | 启用 + 关闭平台熵 | 无 `/dev/urandom` |
  | `MBEDTLS_PLATFORM_MS_TIME_ALT` | 启用 | 交叉目标既不走 `__unix__` 也不走 Windows 分支，否则 `#error "No mbedtls_ms_time available"` |
  | `MBEDTLS_NET_C` / `MBEDTLS_TIMING_C` | 关闭 | TLS 走 libcurl 自定义 BIO；DTLS 计时不需要 |
  | `MBEDTLS_FS_IO` | 关闭 | 证书由 libcurl 以内存缓冲提供 |
  | `MBEDTLS_PSA_CRYPTO_STORAGE_C` / `PSA_ITS_FILE_C` | 关闭 | 无文件型 PSA 持久化后端；会话密钥驻留内存 |

### C.2 用户态垫片（`user/lib/mbedtls_glue.c`）
- `mbedtls_hardware_poll()`：**RDRAND** 优先（CPUID.01H:ECX[30] 检测，10 次重试），
  否则退化到 **splitmix64 软 PRNG**（种子 = TSC ⊕ time ⊕ 常量）。
- `mbedtls_ms_time()`：`clock_gettime(CLOCK_MONOTONIC)`，失败回退 `time(NULL)*1000`。

### C.3 libcurl 接入
- `curl_config.h`：`#define USE_MBEDTLS 1`（`curl_setup.h` 自动推出 `USE_SSL`）。
- **关键点**：curl 编译 mbedTLS 头必须与库用**同一份配置**（否则 `mbedtls_ssl_config` 等结构体布局不一致），
  故 `CURL_CFLAGS` 也要带 `-DMBEDTLS_CONFIG_FILE=... -I lib/mbedtls/include`。
- **踩坑**：改 `curl_config.h` 后 libcurl 对象**未自动重编**（`vtls/mbedtls.c.o` 仍是 712 B 空对象），
  表现为 `HTTPS: rc=1 Unsupported protocol`；`make -B` 强制重编后 mbedtls.c.o = 25 488 B，正常。

### C.4 验证（宿主机 `openssl s_server -WWW` 自签证书，curl 关校验）
```
[curl] libcurl version: libcurl/8.22.0-DEV mbedTLS/3.6.7 zlib/1.3.1
[curl] HTTP  status=200 body=72 bytes: Hello from SukiOS host HTTP server!
[curl] libcurl HTTP  GET: PASS
[curl] HTTPS status=200 body=72 bytes: Hello from SukiOS host HTTP server!
[curl] libcurl HTTPS GET: PASS
```

---

## D. curl 命令行应用（`::BIN/CURL.SKA`）

### D.1 组成
- 直接编译上游 `lib/curl/src`（**42 个源文件**，与 Linux curl 同源：`tool_getparam/tool_operate/tool_main/tool_help/...`），
  不是自研简化版。
- `build/apps/curl.elf` = 1 705 448 B（链接 `USER_LIB_OBJS + libcurl.a + libz.a + libmbedtls.a`）。
- 安装到磁盘 `::BIN/CURL.SKA`（与其他独立程序一致；shell 中 `exec BIN/curl` 装载）。

### D.2 内核配套改动
- `EXEC_ELF_MAX`：`1 MiB` → **`4 MiB`**（`kernel/syscall/syscall.c`）。
  原因：curl CLI 1.63 MiB 超原上限；exec 读盘本就是 `FS_MSG_READ_AT` 分块读，不受 OOL 16 页限制。

### D.3 为工具补齐的 libc
| 新增/修正 | 说明 |
|---|---|
| `isatty()` | 返回 0（非 POSIX tty），使第三方程序走非终端分支（不画进度条/不报终端告警） |
| `ftruncate()` | `SYS_FTRUNCATE` 包装 |
| `freopen()` | `fileio.c`：抽 `mode_to_flags()` 复用，重定向已有流 |
| `rand()/srand()` | C 标准 LCG（mbedTLS `rsa.c` 也用到） |
| `fcntl()` | 改为**标准可变参数** `int fcntl(int,int,...)`（`F_GETFL` 无第三参，原 3 固定参原型导致工具报 "too few arguments"） |
| `<sys/stat.h>` | 补 `st_atime/st_mtime/st_ctime` 成员别名（`struct stat` 实为纳秒拆分字段） |

### D.4 验证
开机自检 `curl_app_test`（`user/apps/curl_app_test.c`）以真实参数 **execve 磁盘上的 `::BIN/CURL.SKA`**
（与 shell 装载同一条路径），日志：
```
[curl-app] exec /BIN/CURL.SKA: curl -sS http://10.0.2.2:8000/hello.txt
[syscall] task 'CURL.SKA' pid=14 exit(code=0)
[sched]   task 'CURL.SKA' pid=14 exited (code=0)
```
即：**CLI 被内核从磁盘装载并成功跑完，退出码 0（请求完成）**。
> 已知限制（非 curl 缺陷）：其 stdout 指向控制台设备，无头模式不进串口，
> 故回归日志中看不到响应体文本；在图形 shell 中 `exec BIN/curl` 可正常看到输出。已记入 README §3。

---

## E. 回归总览（QEMU 无头，零 panic）
```
[kminiz] selftest OK (110 -> 103 bytes, zlib-style roundtrip)   # B
[fs] self-test ALL PASS
[libc-test] zlib roundtrip OK (102 -> 89 bytes)                 # A
[libc-test] PASS=11 FAIL=0  ALL OK
[curl] libcurl HTTP  GET: PASS                                  # C
[curl] libcurl HTTPS GET: PASS                                  # C
=== POSIX test summary: PASS=192 FAIL=0 ===
[syscall] task 'CURL.SKA' pid=14 exit(code=0)                   # D
PANIC: 无
```

## F. 主要文件清单
**新增**
```
user/lib/suki_mbedtls_config.h   user/lib/mbedtls_glue.c        user/apps/curl_app_test.c
include/kernel/abilities/kminiz.h               kernel/abilities/miniz/kminiz.c
kernel/abilities/miniz/shim/{assert.h,stdlib.h,string.h}
```
**修改**
```
Makefile            kernel/kmain.c       kernel/syscall/syscall.c
kernel/mm/kmalloc.c include/mm/kmalloc.h
user/lib/{fileio.c,net.c,unistd.c,stdlib.c,shell.c}
user/lib/shims/{stdio.h,stdlib.h,unistd.h,fcntl.h,sys/stat.h}
user/apps/curl_test.c   user/lib/curl_config.h   README.md  NOTICE
```
**子模块新增**：`lib/zlib` @ `v1.3.1`
