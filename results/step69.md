# Step 69：SukiOS Rust 开发工具链（第一部分 —— 环境搭建）

> 承接 results/step68.md 的评估。用户目标：为后续移植 Servo 准备好 Rust 支持。
> 本步骤只做**第一部分：搭建“属于 SukiOS”的 Rust 开发工具链**，通过 `make make-rust-env`
> 给仓库其余用户一键创建 Rust 环境，并**自动探查是否已安装 rust**。
> 第二部分（把 Rust 程序真正装入磁盘、在 QEMU 下跑起来）见后续步骤。

---

## 1. 交付内容

### 1.1 Makefile 新增目标（`Makefile` 第 382 行附近）
- `make-rust-env`：一键创建 Rust 开发环境（核心目标）。
- `rust-env`：别名（= make-rust-env）。
- `rust-libs`：仅重建 SukiOS 用户态运行时静态库 `libsuki.a`。
- 三者已加入 `.PHONY`。

关键变量与规则：
```make
# Rust 运行时归档：复用 USER_LIB_OBJS，剔除 crt0（Rust 自带 _start）。
RUST_LIB_OBJS := $(filter-out $(BUILD)/user/lib/crt0.S.o,$(USER_LIB_OBJS))
RUST_LIB      := $(BUILD)/libsuki.a
RUST_DIR      := $(CURDIR)/rust
# 归档工具：优先交叉 ar，回退宿主 ar。
# 交叉链接器探测（与 USER_CC 同逻辑）。
$(RUST_LIB_OBJS): | $(CONFIG_H)          # 确保用户态配置头先就位
$(RUST_LIB): $(RUST_LIB_OBJS) | $(CONFIG_H)
	$(RUST_AR) rcs $@ $(RUST_LIB_OBJS)
make-rust-env: $(RUST_LIB)
	@sh $(CURDIR)/tools/suki-rust-env.sh
```
`libsuki.a` 把 `user/lib` 的 `suki.c/string.c/stdlib.c/stdio.c/unistd.c/time.c/dirent.c/
syscalls.c/pthread.c/signal.c/stack_canary.c/setjmp.S/suki_native.c/dlfcn.c/errno.c`
编成静态归档（剔除 `crt0.S.o`，因为 Rust 自己提供 `_start`），供 Rust 链接获得
`malloc/free/pthread/syscall` 等能力，而**无需从零移植 std**。

### 1.2 `tools/suki-rust-env.sh`（实际干活的逻辑）
职责（每个动作都幂等）：
1. **自动探查 rust**：`command -v rustup || command -v rustc`。若已安装则打印版本并跳过安装；
   若未安装，经 `curl https://sh.rustup.rs | sh -s -- -y --default-toolchain nightly --profile minimal`
   安装（fallback 到 `wget`；两者皆无则报错退出）。
2. `rustup component add rust-src` + `rustup target add x86_64-unknown-none`（幂等，`|| true` 保护）。
3. 探测交叉链接器：优先 `opt/bin/x86_64-sukios-elf-gcc`（**用绝对路径**），回退
   `x86_64-elf-gcc`，再回退 `gcc`——与 `Makefile` 的 `USER_CC` 探测逻辑一致。
4. 确保 `build/libsuki.a` 已构建（`make rust-libs`），并拷贝到 `rust/libsuki.a`。
5. **生成 `rust/x86_64-sukios.json` 目标规格**：`os="none"`、`vendor="sukios"`、链接器为绝对路径、
   `pre-link-args` 含 `-T<repo>/user/user.ld -L<repo>/rust -Wl,--build-id=none -Wl,--no-warn-rwx-segments`、
   `late-link-args` 含 `-lsuki -lgcc`；所有路径用 `$(CURDIR)` 展开为绝对路径，保证换机器可用。
6. 打印后续步骤（`cd rust && cargo build` 等）。

### 1.3 提交入库的 `rust/` 工程（模板/示例）
- `rust/rust-toolchain.toml`：`channel = "nightly"` + `components = ["rust-src"]`
  （nightly 是后续 Servo 用 `build-std` 编 std 的前置）。
- `rust/Cargo.toml`：`edition = "2021"`，`[profile.dev]/[profile.release]` 均 `panic = "abort"`。
- `rust/.cargo/config.toml`（见下，含 unstable 开关）。
- `rust/src/main.rs`：最小 `no_std` + `no_main` 示例，自写 `_start` 经 `core::arch::asm!`
  发 `syscall` 调 `SYS_DEBUG_WRITE`(4) 打印、再 `SYS_TASK_EXIT`(2) 退出；同时作为
  “Rust 在 SukiOS 运行”的模板（第二部分将装入磁盘验证）。

### 1.4 `.gitignore`
新增忽略生成物：`/rust/x86_64-sukios.json`、`/rust/libsuki.a`、`/rust/target/`。
（其余 `rust/` 源文件入库。）

---

## 2. 关键设计点

### 2.1 为什么用自定义 JSON 目标规格而非内置 `x86_64-unknown-none`
- 让目标“属于 SukiOS”：`vendor="sukios"`、用我们的交叉链接器、`-T user/user.ld`、
  链接 `libsuki.a`，整套链接行为由本仓控制。
- JSON 目标规格经 `--target <path>` 传入，可用任意 `os`/`env`（绕过内置目标名校验），
  为后续 `os="sukios"` 正式移植 std 预留扩展位。

### 2.2 `rust/.cargo/config.toml` 的三个要点
```toml
[build]
target = "x86_64-sukios.json"

[unstable]
json-target-spec = true      # cargo 使用 .json 目标规格需此不稳定特性（nightly）
build-std = ["core", "alloc"] # 自定义目标无预编译 core，必须从 rust-src 源码编
```
- `json-target-spec`：cargo 对 `.json` 规格的限制（nightly 专属，已固定 nightly 工具链）。
- `build-std`：自定义目标没有预编译的 `core`，首次 `cargo build` 会从 `rust-src` 现场编译
  `core`/`alloc`/`compiler_builtins`（约 14s，缓存后很快）。

### 2.3 链接模型与内核加载器匹配
- `x86_64-sukios.json` 设 `panic-strategy="abort"`、`relocation-model="static"`、
  `position-independent-executables=false`、`disable-redzone=true`（与 `user/lib` 的
  `-mno-red-zone` 一致）、`code-model="kernel"`。
- 产出的 ELF 为 **ET_EXEC**，基址 `0x400000`（`user.ld` 的 `ENTRY(_start)` + `. = 0x400000`），
  与 SukiOS ELF 加载器期望的固定装载形态完全一致 → 可直接被内核装载执行。

---

## 3. 搭建与验证过程（踩坑与修复）

执行 `make make-rust-env` 与 `cargo build` 时逐条解决了以下真实问题（均已固化进脚本/配置，
其余用户无需再踩）：

| # | 现象 | 原因 | 修复 |
|---|------|------|------|
| 1 | `cargo: .json target specs require -Zjson-target-spec` | cargo 对 .json 规格需不稳定特性 | `.cargo/config.toml` 加 `[unstable] json-target-spec = true` |
| 2 | `unknown field 'environment'` | 新版 rustc 该字段已改为 `env` | 脚本 JSON 用 `"env": ""` |
| 3 | `target-c-int-width: invalid type: string "32", expected u16` | 该字段需整数 | 改为 `32`（整数） |
| 4 | `target-pointer-width: ... expected u16` | 同上 | 改为 `64`（整数） |
| 5 | `unknown field 'needs-plt'` | 该字段在当前 rustc 已被移除 | 删除该字段 |
| 6 | `target feature sse2 is required by the ABI but gets disabled` | 早期 `x86_64-unknown-none` 的 `features` 禁用了 SSE2，与新 rustc ABI 要求冲突 | 删除 `features` 字段，恢复架构默认（SSE2 开启；内核 switch.S 已 fxsave/fxrstor 保存 XMM，安全） |
| 7 | `data-layout ... differs from LLVM target's ...` | 缺 `-i128:128` 段 | data-layout 补成 `...-i64:64-i128:128-f80:128-...` |
| 8 | `can't find crate for core` / `target may not be installed` | 自定义目标无预编译 core | 启用 `build-std = ["core","alloc"]`（需 `rust-src` 组件） |
| 9 | `linker x86_64-sukios-elf-gcc not found` | cargo 调用链接器时未带 `opt/bin` 的 PATH | 脚本把链接器写成**绝对路径** `opt/bin/x86_64-sukios-elf-gcc` |
| 10 | `unrecognized command-line option '--build-id=none' / '--no-warn-rwx-segments'` | 这两条是 ld 选项，不能由 gcc 驱动直接识别 | 改为 `-Wl,--build-id=none` / `-Wl,--no-warn-rwx-segments` 转发给 ld（与现有 C 构建一致） |

最终工具版本：`rustc 1.100.0-nightly (f248f4038 2026-09-05)`。

---

## 4. 验证结果（生产可工作，非 stub）

1. `make make-rust-env`：自动检测到本机未装 rust → 经 rustup 安装 nightly + rust-src →
   构建 `build/libsuki.a`（13 个用户态对象）→ 生成 `rust/x86_64-sukios.json` 与
   `rust/libsuki.a` → 打印就绪信息。**幂等**：再次运行仅重新生成，不重复安装。
2. `cd rust && cargo build`：现场编译 `compiler_builtins` / `core` / `alloc` 后，成功链接出
   `target/x86_64-sukios/debug/sukios-hello`。
3. `readelf` 结构校验：
   - `Type: EXEC (Executable file)` —— SukiOS 加载器期望的 ET_EXEC。
   - `Entry point address: 0x400000`。
   - 符号 `_start` 位于 `0x400000`（FUNC, 206 字节）。
   - `LOAD` 段 `0x400000` R E，段大小/文件偏移自洽。
   → 该 ELF 具备被 SukiOS 内核 ELF 加载器直接装载执行的形态。

> 注：本步骤只验证“工具链能产出正确的 SukiOS 形态 ELF”。把该二进制真正装入磁盘、
> 在 QEMU 下启动并由内核执行、串口打印 `hello from rust on SukiOS` 的**运行态验证**
> 属于第二部分（Rust 程序在 SukiOS 上真正跑起来），不在本步骤范围内。

---

## 5. 给其余用户的使用方式

```sh
# 首次（或换机器）创建 Rust 环境：自动探查/安装 rust + 构建 libsuki.a + 生成目标规格
make make-rust-env

# 进入示例工程构建（首次会从 rust-src 编 core，约十几秒）
cd rust
cargo build
# 产物： rust/target/x86_64-sukios/debug/sukios-hello  （SukiOS 可执行 ELF）

# 仅重建运行时归档（当 user/lib 改动后）
make rust-libs
```

---

## 6. 后续步骤（第二部分预告）
- 把 `sukios-hello` 装入 FAT32 磁盘镜像（`::BIN/`），由 shell `exec` 或开机自启动；
- 在 `main.rs` 扩展：经 FFI 调用 `libsuki.a` 的 `malloc`/pthread/syscall，验证堆、线程、FD；
- QEMU 启动，确认串口输出 `hello from rust on SukiOS`、零 panic，完成运行态回归。
- 长远：自定义 `os="sukios"` 目标 + `library/std/src/sys/sukios` 后端（`build-std` 编 std），
  为 Servo 铺路（见 step68 评估）。
