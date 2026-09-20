# step106 —— Rust 标准库（std）移植：现状勘察、依赖层打通与 SukiNative 方案

> 本轮目标（用户）：按 Rust 官方文档用 `cargo build-std` 给 SukiOS 添加**完整 rust-std 支持**，
> 且 **使用 SukiNative，不要使用 POSIX**。
>
> 结论先说：这是一项**多阶段、体量很大**的移植（std 平台层 + 大量内核原生 syscall）。
> 本轮完成了完整的现状勘察、把 std 编译链路的**依赖层打通并入库固化**，并给出后续可执行的
> 分阶段方案。**尚未交付「可运行的 std」**——下文给出精确缺口与路线，避免后续返工。

---

## 1. 现状勘察（关键发现）

### 1.1 仓库侧：没有任何已入库的 std 支持

- `rust/` 下只有 `no_std` 示例工程（`src/main.rs` 自写 `_start` + 自定义 `panic_handler`），
  `rust/.cargo/config.toml` 为 `build-std = ["core", "alloc"]`。
- `README.md` §未完成项明确写着「Rust 标准库（std）未移植」，`results/step68/69/70.md` 只做到
  「no_std + 自定义 bare-metal 目标 + FFI 复用 libsuki.a」。
- 全仓检索 `sukios_ffi` / `target_os = "sukios"`：**零命中**（即下述 std 后端从未入库）。

### 1.2 工具链（rust-src 组件）里存在一套**未入库、且当前编不过**的 std 后端

`$(rustc --print sysroot)/lib/rustlib/src/rust/library/std/src/sys/` 下存在一批 `sukios` 后端：

```
sukios_ffi.rs(27K)  alloc/ args/ env/ fd/(23K) fs/(93K) path/ paths/(16K)
pipe/ random/ stdio/ sync/futex/ thread/(43K) time/  + process/sukios/
```

其设计取向与用户要求**相反**：

- `sukios_ffi.rs` 是一个 **libc 形状的 FFI 层（117 个函数）**，
  直接 `extern "C"` 调用 SukiOS 的 C 运行库 `libsuki.a`（`open/read/stat/opendir/fork/
  execve/waitpid/pthread_*/malloc/mmap/clock_gettime/getenv/signal/...`），即 **POSIX 路径**。
- 各 std 模块（含 `pal/unix`、`os/unix/*`、`sys/io/unix`、backtrace）被**全局 sed**：
  `libc::` → `crate::sys::sukios_ffi::`（共 **185 个文件**被改，含 aix/windows/wasi 等无关平台文件）。
- 目标规格 `rust/x86_64-sukios.json` 被手工改成 `"os": "sukios"` + `"target-family": "unix"`
  + `"has-thread-local": true`（而 `make make-rust-env` 生成的版本是 `"os": "none"`）。

### 1.3 SukiNative 内核侧现状（**已经相当完整**）

`SYS_SUKI_*`（130..149）已实现（`kernel/syscall/sys_suki.c`，Phase 1+2）：

| 能力 | 号 | 状态 |
|---|---|---|
| 对象/句柄：create/destroy/duplicate/query | 130-133 | ✅ |
| 多对象等待 `SUKI_WAIT_{ANY,ALL,NO_BLOCK}` | 134 | ✅ |
| 事件 135-137 / 互斥 138-140 / 信号量 141-143 | | ✅（含自锁检测、防丢唤醒） |
| FILE：open/read/write/close（句柄化，底层 `fd_*`） | 144-147 | ✅ |
| PROC：create（装载 ELF 建子进程 + 退出通知对象） | 148 | ✅ |
| MEM：匿名映射（按需零填，返回句柄） | 149 | ✅ |

外加原生块（0..19）：`mach_msg/task_spawn/task_exit/yield/debug_write/port_claim/
mmap_legacy/munmap_legacy/serial_read/fork/getpid/getppid/...`。

**但 std 需要的很多能力在原生面里还没有**：文件元数据（stat/chmod/access/isatty）、
目录（opendir/readdir/telldir/seekdir）、lseek/ftruncate/rename/unlink/mkdir/rmdir/chdir/getcwd、
dup/fcntl/pipe、进程等待并取退出码、线程创建（`SYS_CLONE` 有号但 std 需要 TLS/futex 配合）、
时钟（monotonic/realtime）、随机数、信号等。

### 1.4 那套 std 后端**当前编译不过**（本轮实测）

`cargo +nightly build -Zbuild-std=std,panic_abort --target x86_64-sukios.json`：

- **修复前**：卡在 `libc v0.2.189`（`unresolved import unistd`）——自定义 target_os 不被 libc 认识。
  依赖链定位（`cargo --unit-graph`）：`panic_unwind → libc`，`std_detect → libc`，`unwind → libc`。
- **本轮打通依赖层后**：越过 libc，**进入 std 自身编译**，报 **392 个错误**，分布：

| 错误码 | 数量 | 主要含义 |
|---|---|---|
| E0425 | 190 | 名字不存在（sukios_ffi 缺项 / sed 后 import 失配） |
| E0603 | 97 | 私有项（同上，模块可见性问题） |
| E0433/E0432 | 50 | 缺失模块（如 `os/unix` 引用的 `platform::fs`/`platform::raw`、backtrace 的 `addr2line`/`object`） |
| E0308/E0599/E0061/E0609/E0531… | 55 | 类型/方法/参数/图样等 |

热点文件：`sys/pipe/mod.rs`(20)、`sys/stdio/sukios.rs`(16)、`sys/mod.rs`(11)、`rt.rs`(9)、
`os/unix/net/addr.rs`(9)、`backtrace/.../gimli*`(7)、`sys/fd/sukios.rs`(6)、`io/stdio.rs`(6) …

**判断**：这不是「差一点」，而是**一个未完成、未验证、且方向与需求相反**的 WIP。
在它上面继续修，等于先花大力气把 POSIX 版修好，再推倒重做成 SukiNative —— 不划算。

---

## 2. 本轮已交付（已入库、已实测有效）

1. **依赖层打通**（`tools/rust-std-patch.sh`，幂等，可 `--check`）：
   - `unwind/Cargo.toml`、`panic_unwind/Cargo.toml`、`std_detect/Cargo.toml`：
     libc 依赖在 `target_os="sukios"` 下排除；
   - `unwind/src/lib.rs`：sukios 走「无 unwinder」分支（panic=abort 不需要）+ 不 `extern crate libc`；
   - `std/Cargo.toml`：移除 `libc` 依赖行。
   - 实测效果：从「卡在 libc」→「进入 std 编译（392 错误）」。
2. **构建接线**：`make rust-std-patch` / `make rust-std-build` / `make rust-std-clean`。
3. **验收夹具**：`rust/stdtest/`（std 冒烟测试：`println!`/`Vec`/`String`/`HashMap`/`args`/`Instant`）。
4. 保留现状勘察结论（本文档），避免后续按错误方向投入。

> 未做（有意）：**没有**把那 185 个被全局 sed 的 std 文件、也没把 `sukios_ffi.rs`（POSIX/libsuki 绑定）
> 复制进仓库——因为它与「使用 SukiNative」的要求冲突，入库即为负债。

---

## 3. 建议方案（SukiNative 路线）

### 3.1 目标规格调整（关键）

`rust/x86_64-sukios.json` 建议改为 **`"os": "sukios"`，不带 `target-family`**（即不声明 unix）：

- 这样 std 不会走 `pal::unix` / `os::unix::*` / 通用 unix 网络与 backtrace 代码，**不会引入 libc**，
  也不会踩 `platform::fs` 这类 unix 专用模块；
- std 的 `cfg_select!` 里 `target_os = "sukios"` 分支（细粒度 `sys/<模块>/sukios.rs`）仍然优先命中；
- 未覆盖的部分落到 `pal/unsupported`（std 对「新 OS 目标」的标准做法，见 rustc-dev-guide
  *Adding a new target* 与 `library/std/src/sys/pal/unsupported`）。

### 3.2 代码结构（全部入库，不 vendor 上游 82MB）

```
rust/
  x86_64-sukios.json          # 目标规格（make make-rust-env 生成，含绝对路径）
  std/                        # ← 我们的 std 平台层（入库）
    native.rs                 # SukiNative syscall 层（裸 syscall 指令，无 C 运行库）
    sys/                      # 各细粒度后端：alloc/args/env/fs/fd/path/paths/pipe/
                              #   process/random/stdio/sync/thread/time/…
  installer: tools/rust-std-install.sh   # 把 std/ 下的文件与 mod.rs 分发片段装进 rust-src
```

- **不再有 `sukios_ffi`（libc 形状）/ 不链接 `libsuki.a`**：`native.rs` 直接用 `syscall`
  指令调 SukiNative（130..149）与原生块（0..19），所有 I/O 都经**句柄**（`suki_handle_t`），
  错误码用 `suki_status_t`。
- std 的 `fs/thread/net/...` 逻辑保留上游结构，但把底层实现换成 `native::*`。

### 3.3 需要新增的**内核原生 syscall**（本方案的主要工作量）

建议在 `include/sukios/posix.h` 新开 **SukiNative Phase 3（214..249）**，在
`kernel/syscall/sys_suki.c` 实现（全部是既有内核内部的薄封装，如 `fd_open/fd_read/
vma_*/task_*`，不含 POSIX 语义层）：

| 组 | 号位建议 | 说明 |
|---|---|---|
| 文件元数据 | 214 STAT, 215 FSTAT, 216 CHMOD, 217 ACCESS, 218 ISATTY | `fd_stat()` 等 |
| 定位/截断 | 219 SEEK, 220 TRUNCATE | `fd_lseek`/`fd_ftruncate` |
| 目录 | 221 DIR_OPEN, 222 DIR_READ, 223 DIR_CLOSE | 复用 `fd_opendir/fd_readdir` |
| 目录/命名空间 | 224 MKDIR, 225 RMDIR, 226 UNLINK, 227 RENAME, 228 CHDIR, 229 GETCWD | |
| 复制/管道 | 230 DUP, 231 PIPE | |
| 进程 | 232 PROC_WAIT（等子进程并取退出码，基于现有 PROC 对象+`SUKI_WAIT` 扩展） | |
| 线程/同步 | 233 THREAD_CREATE（共享 AS 的 `SYS_CLONE` 语义封装）, 234 FUTEX_WAIT, 235 FUTEX_WAKE, 236 TLS_SET/GET | 对应 `arch_prctl` |
| 时间 | 237 TIME_MONOTONIC, 238 TIME_REALTIME, 239 TIME_SLEEP | |
| 随机 | 240 RANDOM | `/dev/urandom` 或 TSC 熵 |
| 字符/信号 | 241 UNAME, 242 SIGNAL_*（可先最小实现以让 std 的 unix 路径退化） | |

### 3.4 分阶段里程碑（每阶段都要 QEMU 实跑验收）

| 阶段 | 内容 | 验收 |
|---|---|---|
| **P0** | 目标规格去 unix + 最小 `native.rs`（exit/debug_write/mem） + `sys/{alloc,stdio,args,env,exit}` + `_start`(裸 asm) | `stdtest` 能编过、能 `println!`/`Vec`/`HashMap`，串口看到输出 |
| **P1** | `sys/{time,thread_local,thread,sync}` + 内核 THREAD_CREATE/FUTEX/TLS/TIME | `std::thread::spawn` + `Mutex`/`Condvar` 用例通过 |
| **P2** | `sys/fs`（+ 内核 STAT/DIR/SEEK/…） | `std::fs` 读写/建删目录/列目录通过 |
| **P3** | `sys/process`（+ PROC_WAIT/SPAWN）+ `sys/pipe` | `Command::new(...)` 跑磁盘程序并取退出码 |
| **P4** | `sys/net`（+ `std::net` 绑定到 SukiNative socket/NS_PORT） | TCP/UDP 回环用例 |
| **P5** | panic/backtrace 最小化（trace-only）、`std::time` 精度、随机源质量 | `panic!` 输出文件行号（可退化） |

---

## 4. 本轮验证记录

```bash
# 依赖层补丁（幂等）
make rust-std-patch          # 或 tools/rust-std-patch.sh --check

# 期望：不再报 libc，进入 std 编译
make rust-std-build          # cargo +nightly build -Zbuild-std=std,panic_abort --target x86_64-sukios.json
```

- 修复前：`error[E0432]: unresolved import unistd (libc-0.2.189/src/new/mod.rs:255)`
- 修复后：`error: could not compile std (lib) due to 392 previous errors`（即有进展，缺口量化见 §1.4）

## 4b. 方案 A 已启动（本轮实测进展）

按用户决定采用方案 A。本轮已落地：

1. **目标规格改造**（`tools/suki-rust-env.sh` 生成器同步改）：
   - 去掉 `"target-family": "unix"`（避免 std 编译 unix 通用代码而引入 libc）；
   - 新增 `"main-needs-argc-argv": true`（rustc 生成 `main(argc, argv)`，配合内核构造的
     System V 初始栈 + 我们自己的 `_start`，无需 libc）；
   - `"has-thread-local": false`（P0：std 走 racy 回退实现，暂不需要内核 ELF TLS）。
2. **内核 SukiNative Phase 3 开场**（`include/sukios/posix.h` 214..217 +
   `kernel/syscall/sys_suki.c` 实现 + `syscall.c` 路由 214..249）：
   `TIME_MONOTONIC`(214, `clock_monotonic_ns`)、`TIME_REALTIME`(215, `rtc_posix_now_ns`)、
   `SLEEP_NS`(216, 循环 `task_yield` 不忙等)、`RANDOM`(217, TSC+单调播种 xorshift64*，
   逐块 `copy_to_user`)。**内核编译通过**（`make all` → Linked）。
3. 实测：把新目标规格套到「sysroot 里那套 WIP 后端」上，报 **403 个错误**，且错误集中在
   `sys/*/sukios.rs`（POSIX 形状：`sukios_ffi` 的 libc 形 API、`timespec::default` 缺失、
   `random` 借用错误…）与 `os/unix/*`。

**结论**：WIP 后端无法沿用到 native 架构——它的模块是按「libc 形 FFI」写的。
下一步即 step106 §3.2 的核心工作：**用我们自己的 `sys/<模块>/sukios.rs` 逐个替换**
（`native.rs` 裸 syscall 层 + 模块实现），并以编译器为准迭代。P0 需替换/新增的模块清单：

| 模块 | P0 需要的内容 |
|---|---|
| `sys/native.rs`(新增) | syscall 封装 + 小型堆（MEM_ALLOC/OBJ_DESTROY）+ 错误码/常量 |
| `alloc` | malloc/calloc/realloc/free/posix_memalign 走 native 堆 |
| `stdio`+`fd` | `FileDesc`：fd0 读(serial)、fd1/2 写(SYS_DEBUG_WRITE)、其余经 FILE_* 句柄 |
| `args` / `env` | 从初始栈 argv/envp 取（`_start` 已解包）；env 表本地维护 |
| `time` | `Instant`/`SystemTime` ← native 214/215/216 |
| `random` | `fill_bytes`/`hashmap_random_keys` ← native 217 |
| `thread`(最小) | 当前线程名/`sleep`/`yield_now`/`available_parallelism`；spawn 先返回 Unsupported |
| `thread_local`/`sync` | 走 std 的 racy/unsupported 回退（P0 不开 TLS/futex） |
| `path`/`paths` | unix 风格分隔符；程序名/当前目录最小实现 |
| `fs`(最小 API 完整) | `File/OpenOptions/FileAttr/Dir` 骨架 + 经 native FILE_* 的读写；元数据类暂 ENOSYS |
| `process`/`pipe`/`net`/`io` | 指向 std 的 `unsupported` 或最小实现（编译期契约完整、运行期 ENOSYS） |
| `_start`(应用侧) | 裸 asm：读初始栈 argc/argv → `call main` → `SYS_TASK_EXIT` |

## 5. 关键文件索引

| 类别 | 位置 |
|---|---|
| 依赖层补丁 | `tools/rust-std-patch.sh`、`Makefile`（`rust-std-patch`/`rust-std-build`/`rust-std-clean`） |
| 目标规格 | `rust/x86_64-sukios.json`（由 `tools/suki-rust-env.sh` 生成；**待改：去 target-family**） |
| std 冒烟测试 | `rust/stdtest/{Cargo.toml,src/main.rs}` |
| SukiNative 内核实现 | `kernel/syscall/sys_suki.c`、`include/sukios/posix.h`（130..149）、`include/kernel/suki_native.h` |
| 既有 no_std 通路 | `rust/src/main.rs`、`build/libsuki.a`（`make rust-libs`）、`user/user.ld` |

## 6. 遗留与风险

- **工作量**：P0–P5 是 std 平台层的完整实现（数千行 Rust + ~25 个内核原生 syscall），
  非单轮可完成；建议按里程碑推进，每个里程碑单独 QEMU 验收。
- **TLS**：`std` 依赖线程局部存储；内核需按 ABI 建立 TLS 块并在任务切换时维护 FS 基址
  （现有 `SUKI_ARCH_SET_FS` 通路 + 需补 TLS 镜像/gap 处理）。
- **panic=abort**：不做 unwinding，`panic!` 只能「打印 + 退出」；需要 backtrace 行号则后续再评估。
- **系统调用号位**：新增 SukiNative Phase 3 需同步 `include/sukios/posix.h`、`SYSCALL_MAX`、
  用户态 Rust 侧常量（单一真相源）。
