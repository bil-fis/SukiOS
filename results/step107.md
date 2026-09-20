# Step107 · 为 x86_64-sukios 重新编译 Rust std 并消除全部分发接线编译错误

## 1. 目标与背景

SukiOS 使用自定义 Rust target（`target_os = "sukios"`，spec 见 `rust/x86_64-sukios.json`：
`llvm-target = "x86_64-unknown-none"`、`has-thread-local = false`、`executables = true`）。
Rust 标准库（`std`）需从 `rust-src` 组件用 `-Zbuild-std=std,panic_abort` 交叉编译。

上一步遗留了「14 个分发接线问题」——即 std 源码中大量 `cfg_select!` 分支没有
`sukios` 分支，导致 std 无法为 sukios 编译。本步目标：确认原生 FFI 层在位 →
运行安装器（依赖层 + 分发补丁）→ 重编 std → 消除编译错误，使 std 真正可在
x86_64-sukios 下编译通过。

## 2. 原生 FFI 层（已确认在位）

- `rust/std/sys/sukios_ffi.rs`（38603 字节）：提供 SukiOS 原生系统调用封装，
  链接 `libsuki.a`。关键符号（全部 `pub`，供 std 各后端调用）：
  - `pub unsafe fn write(fd: c_int, buf: *const c_void, count: size_t) -> ssize_t`（→ SYS_DEBUG_WRITE，fd=1/2 即 stdout/stderr）
  - `pub unsafe fn exit(code: c_int) -> !`（→ SYS_TASK_EXIT）
  - `pub unsafe fn getrandom(buf, len, flags) -> ssize_t`（→ SYS_SUKI_RANDOM）
  - `pub unsafe fn futex(uaddr: *mut u32, op, val, timeout, uaddr2, val3) -> c_int`（→ SYS_SUKI_FUTEX_WAIT/WAKE）
  - `pub unsafe fn native_set_args(argc, argv, envp)`、`pub unsafe fn getpid() -> pid_t`
  - 常量：`FUTEX_WAIT=0`、`FUTEX_WAKE=1`、`STDIN_FILENO=0`、`STDOUT_FILENO=1`、
    `STDERR_FILENO=2`、`EBADF=9`、`EPERM=1`…`ECONNRESET` 等 errno。
  - 类型：`timespec`、`c_int`、`size_t`、`ssize_t`、`c_void`（`pub use core::ffi::c_void`）、
    `c_char`、`pid_t`、`gid_t`、`uid_t`、`iovec` 等。
- 注意：`sukios_ffi.rs` 仅依赖 `core::ffi`，**不依赖 libc**（与 std 去 libc 改造一致）。

## 3. 安装器与分发补丁机制

- `tools/rust-std-patch.sh`（依赖层，由 `make rust-std-patch` 调用）：在 `rust-src` 树里
  移除 `std/Cargo.toml` 的 `libc` 依赖（已用 `/tmp/rust-src-backup` 恢复为有效状态）。
- `tools/rust-std-install.sh`（分发层，由 `make rust-std-install` 调用）：
  1. **复制循环**：把 `rust/std/sys/**/*.rs` 安装进 `rust-src` 的 `std/src/sys/`
     （本步新增 `sync/futex/sukios.rs`、`random/sukios.rs`、`io/stdio/sukios.rs`）。
  2. **PATCHES 表**：向 std 各 `mod.rs` 的 `cfg_select!` 插入 `target_os = "sukios"` 分支，
     或在合适位置插入模块声明/豁免守卫。
     循环支持两种模式：3 元组 `(rel, anchor, insert)` 为 **insert**（锚点前插入，用于新增
     cfg 分支）；4 元组 `(rel, mode, anchor, insert)`（`mode="replace"`）为**直接替换**
     （用于 `lib.rs` 的 `extern crate libc` 与 `no_threads` 的 `compile_error` 守卫，避免重复插入）。
     幂等判定改为「插入内容 `insert in s` 即跳过」，比原先的全局
     `'target_os = "sukios" in s` 更精确（同一文件可有多个独立 sukios 补丁，如
     `thread_local/mod.rs`）。

## 4. 修复的编译错误与根因

初始 7 个错误 → 经多轮修复归零。关键根因与处置：

| # | 错误 | 根因 | 处置 |
|---|------|------|------|
| 1 | `can't find crate for libc`（lib.rs） | 补丁用 `insert+anchor` 在锚点前插入，导致原 `extern crate libc` 保留成重复行，非门控版本仍拉 libc | lib.rs 改 `replace` 模式，仅保留 `not(target_os="sukios")` 门控版本 |
| 2 | `mod pal` 重定义（E0428，sys/mod.rs） | 同上，重复插入 `mod pal;` | sukios_ffi 声明的 insert 去掉前置 `mod pal;`，仅在锚点后追加 |
| 3 | `no_threads` 的 `compile_error`（E0428/守卫） | 同上，重复插入未门控 `compile_error`；且 sukios 有线程，`target_has_threads` 为真会误伤 | 改 `replace` 模式，守卫改为 `#[cfg(all(target_has_threads, not(target_os="sukios")))]` |
| 4 | `key::LazyKey`/`key::set` 未解析 | `sys/thread_local/mod.rs` 的 `guard` 模块对 sukios 走了 `key`-based TLS 分支，要求 `key::LazyKey/set`，而 no_threads 后端不提供 | 把 `target_os="sukios"` 加入 `guard` 的「空 `enable()`」分支（与 wasm/uefi/zkvm 同类），不再引用 `key` |
| 5/6 | `futex::Futex`/`Primitive` 未解析（E0432） | `futex/sukios.rs` 只提供 `futex_wait/wake/wake_all`，缺类型别名 | 在 `rust/std/sys/sync/futex/sukios.rs` 补 `pub type Primitive=u32; pub type Futex=AtomicU32; pub type SmallPrimitive=u32; pub type SmallFutex=AtomicU32;` |
| 7 | `c_void` 私有（E0603，alloc/sukios.rs、futex/sukios.rs） | `sukios_ffi.rs` 用 `use core::ffi::c_void;`（私有），外部模块 `crate::sys::sukios_ffi::c_void` 不可见 | 改 `pub use core::ffi::c_void;` |
| 8 | `sys::fill_bytes` 未找到（E0425，random） | `sys/random/mod.rs` 对 sukios 落到 `_ => {}`，无 `fill_bytes` | 新增 `rust/std/sys/random/sukios.rs`（经 `getrandom` 实现），并加 `target_os="sukios"` 分支 |
| 9 | `restricted_std` 未开启（E0658，stdtest） | sukios 非 std 已知平台，用户 crate 需显式开启 | `rust/stdtest/src/main.rs` 加 `#![feature(restricted_std)]` |
| 10 | stdtest 链接出空 ELF（无 `_start`/程序头） | Rust std 二进制默认无 crt0 入口，链接器 GC 掉全部代码 | `main.rs` 加 `#![no_main]` + 手写 `#[no_mangle] pub extern "C" fn _start()`（按 System V 初始栈读 argc/argv，调测试体，最后 `std::process::exit(0)`） |
| 11 | `crate::sys` 不可访问（E0433，stdtest） | `crate::sys` 是 std 内部模块，用户态不能访问 `native_set_args`/`exit` | 改用公开 API `std::process::exit`；`native_set_args` 由 std 内部在运行时初始化阶段完成 |
| 12 | stdio 无输出（运行时，非编译） | `sys/stdio/mod.rs` 无 sukios 分支，落到 `unsupported`（写操作被丢弃） | 新增 `rust/std/sys/io/stdio/sukios.rs`（完整后端，含 `stdin/stdout/stderr` 自由函数，写经 `sukios_ffi::write`→SYS_DEBUG_WRITE），加 `sys/stdio/mod.rs` 分支 |
| 13 | process 后端（运行时） | `process/sukios/` 是 unix 后端副本，依赖 fork/exec/wait/posix_spawn（SukiOS 无），路由会编译失败 | **保持** `process` 走 `unsupported`（P0 不 spawn 子进程，已链接；`std::process::exit` 可终止进程） |

> 注：`alloc/sukios.rs`、`sys/args/sukios.rs` 此前已存在于 `rust-src` 树（早期手动创建），
> 由 `alloc/mod.rs`、`args/mod.rs` 的 sukios 分支引用，编译通过；`args` 走默认实现返回空，
> 不致命。`process/sukios/`（unix 副本）本次**未**路由（避免引入 fork/exec 依赖）。

## 5. 最终验证结果

- **std 库编译**：`cargo +nightly build -Zbuild-std=std,panic_abort --target ../x86_64-sukios.json`
  → `Finished`，**0 编译错误**。
- **stdtest 链接**：产物 `rust/stdtest/target/x86_64-sukios/debug/stdtest`
  - 大小约 **5.6 MB**（真实代码，非空壳）；
  - `readelf -h`：`Type=EXEC`、`Entry point=0x40af10`、3 个 program header（LOAD 段）；
  - `_start` 符号已定义（`readelf -s` 可见），作为 ELF 入口。
- **install 幂等**：重跑 `tools/rust-std-install.sh` 全部补丁显示「已存在 sukios 分支」，无 `!!` 异常。

## 6. 已知边界（下一里程碑）

stdtest 二进制是**有效 ELF**，但要在 SukiOS 上真正运行并打印 `[stdtest] PASS`，
还需要内核侧支持：

1. **用户态 ELF 加载器**：当前内核 `kmain.c` 用 `task_create_user(user_shell_start, ...)`
   拉起的是**链接进内核的汇编用户 demo**（`kernel/user/user_demo.S` 等），没有从磁盘/嵌入
   加载任意 ELF 的机制。需新增 ELF 解析 + 段映射 + 用户栈建立。
2. **System V 初始栈**：内核跳到用户 `_start` 前需按 `[rsp]=argc, [rsp+8]=argv[], 之后 envp[]`
   布置初始栈（Rust `_start` 依赖此布局）。
3. **exit 干净终止**：`std::process::exit` → `sukios_ffi::exit` → `SYS_TASK_EXIT`，内核需实现
   该调用以回收用户任务（当前 `process` 后端走 `unsupported`，链接通过，运行时应能终止）。

这三项属「内核用户 ELF 执行」独立里程碑，不属本次 std 编译任务范围。

## 7. 复现命令

```bash
# 1) 依赖层（去 libc）—— 仅需一次（或 rust-src 变动后）
make rust-std-patch
# 2) 分发补丁（插入全部 sukios 分支 + 复制后端文件）
make rust-std-install
# 3) 编译 std + stdtest
cd rust/stdtest
cargo +nightly build -Zbuild-std=std,panic_abort --target ../x86_64-sukios.json
# 产物：
ls -l target/x86_64-sukios/debug/stdtest
readelf -h target/x86_64-sukios/debug/stdtest
```

## 8. 改动文件清单

- `tools/rust-std-install.sh`：PATCHES 表新增/修正 lib.rs(replace)、sys/mod.rs(sukios_ffi)、
  no_threads(replace 守卫)、thread_local/mod.rs(guard enable)、sys/random/mod.rs、
  sys/io/stdio/mod.rs；循环支持 `replace` 模式 + `insert in s` 精确幂等。
- `rust/std/sys/sukios_ffi.rs`：`use core::ffi::c_void;` → `pub use ...`（公开）。
- `rust/std/sys/sync/futex/sukios.rs`（新增）：Futex/Primitive/SmallFutex 类型别名 + futex 实现。
- `rust/std/sys/random/sukios.rs`（新增）：`fill_bytes` 经 `getrandom`。
- `rust/std/sys/io/stdio/sukios.rs`（新增）：完整 stdio 后端（写经 `sukios_ffi::write`）。
- `rust/stdtest/src/main.rs`：`#![feature(restricted_std)]` + `#![no_main]` + 手写 `_start`，
  冒烟测试覆盖 println!/Vec/String/HashMap/std::time/std::thread::current().name()。
