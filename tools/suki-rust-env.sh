#!/bin/sh
# =============================================================================
# SukiOS Rust 开发环境一键创建脚本
# -----------------------------------------------------------------------------
# 由 `make make-rust-env` 调用。职责：
#   1) 自动探查是否已安装 rust（rustup/rustc/cargo）；未安装则经 rustup 安装
#      nightly + rust-src（后续 Servo 移植需要 build-std 编译 std）。
#   2) 确保 SukiOS 用户态运行时静态库 build/libsuki.a 已构建（make rust-libs）。
#   3) 探测交叉链接器（与 Makefile 一致），生成 rust/x86_64-sukios.json 目标
#      规格（含本仓库绝对路径与 -T user/user.ld -L rust -lsuki 链接参数）。
#   4) 把 libsuki.a 拷贝到 rust/ 供 cargo 链接。
# 该环境“属于 SukiOS”：用我们的交叉链接器 + user/user.ld + libsuki.a。
# 仅生成产物，不改动内核/现有用户态程序。
# =============================================================================
set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export PATH="$HOME/.cargo/bin:$PATH"

# ---- 1) 探查 rust ----
if command -v rustup >/dev/null 2>&1 || command -v rustc >/dev/null 2>&1; then
  echo "[rust] 已检测到 Rust："
  command -v rustc  >/dev/null && rustc  --version
  command -v cargo >/dev/null && cargo --version
else
  echo "[rust] 未安装，经 rustup 安装 nightly + rust-src ..."
  if command -v curl >/dev/null 2>&1; then
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain nightly --profile minimal
  elif command -v wget >/dev/null 2>&1; then
    wget -qO- https://sh.rustup.rs | sh -s -- -y --default-toolchain nightly --profile minimal
  else
    echo "!! 错误：需要 curl 或 wget 才能安装 rustup" >&2
    exit 1
  fi
  export PATH="$HOME/.cargo/bin:$PATH"
fi

# ---- 2) 组件/目标（幂等） ----
rustup component add rust-src 2>/dev/null || true
rustup target  add x86_64-unknown-none 2>/dev/null || true

# ---- 3) 交叉链接器探测（与 Makefile 一致）；优先用绝对路径，使 cargo 无需
#        额外把 opt/bin 加入 PATH 也能找到链接器 ----
if [ -x "$REPO_ROOT/opt/bin/x86_64-sukios-elf-gcc" ]; then
  RUST_USER_CC="$REPO_ROOT/opt/bin/x86_64-sukios-elf-gcc"
elif command -v x86_64-sukios-elf-gcc >/dev/null 2>&1; then
  RUST_USER_CC=x86_64-sukios-elf-gcc
elif [ -x "$REPO_ROOT/opt/bin/x86_64-elf-gcc" ]; then
  RUST_USER_CC="$REPO_ROOT/opt/bin/x86_64-elf-gcc"
elif command -v x86_64-elf-gcc >/dev/null 2>&1; then
  RUST_USER_CC=x86_64-elf-gcc
else
  RUST_USER_CC=gcc
fi

# ---- 4) 确保 libsuki.a 已构建 ----
if [ ! -f "$REPO_ROOT/build/libsuki.a" ]; then
  echo "[rust] 构建 libsuki.a (make rust-libs) ..."
  ( cd "$REPO_ROOT" && make rust-libs )
fi

# ---- 5) 生成目标规格（含仓库绝对路径） ----
RD="$REPO_ROOT/rust"
mkdir -p "$RD/.cargo"
# 目标规格（x86_64-sukios）：
#   os = "sukios"           —— 自定义 OS；std 的 cfg_select 按 target_os="sukios"
#                              命中我们自己的 sys/<模块>/sukios.rs 后端。
#   **不声明 target-family** —— 避免 std 编译 unix 通用代码（pal::unix / os::unix /
#                              sys::io::unix / backtrace 符号化）而引入 crates.io 的
#                              libc（它不认识 sukios）。未覆盖的能力由 std 的
#                              unsupported 分支兜底。
#   main-needs-argc-argv    —— 让 rustc 生成 `main(argc, argv)`，与内核构造的
#                              System V 初始栈 + 我们自己的 _start 配合（无需 libc）。
#   has-thread-local = false —— P0 阶段：std 退化为 racy 的 thread_local 回退实现
#                              （不需要内核 ELF TLS 支持）；后续阶段再开。
cat > "$RD/x86_64-sukios.json" <<JSON
{
  "arch": "x86_64",
  "os": "sukios",
  "env": "",
  "vendor": "sukios",
  "main-needs-argc-argv": true,
  "has-thread-local": false,
  "linker-flavor": "gcc",
  "linker": "$RUST_USER_CC",
  "executables": true,
  "panic-strategy": "abort",
  "position-independent-executables": false,
  "relocation-model": "static",
  "disable-redzone": true,
  "code-model": "kernel",
  "cpu": "x86-64",
  "llvm-target": "x86_64-unknown-none",
  "data-layout": "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128",
  "max-atomic-width": 64,
  "target-c-int-width": 32,
  "target-endian": "little",
  "target-pointer-width": 64,
  "pre-link-args": { "gcc": ["-nostdlib", "-static", "-no-pie", "-T$REPO_ROOT/user/user.ld", "-L$RD", "-Wl,--build-id=none", "-Wl,--no-warn-rwx-segments"] },
  "late-link-args": { "gcc": ["-lsuki", "-lgcc"] }
}
JSON
echo "[rust] wrote $RD/x86_64-sukios.json (linker=$RUST_USER_CC)"

# ---- 6) 拷贝运行时归档 ----
cp "$REPO_ROOT/build/libsuki.a" "$RD/libsuki.a"
echo "[rust] copied libsuki.a -> $RD/libsuki.a"

# ---- 7) 后续步骤 ----
echo ""
echo "==> SukiOS Rust 环境就绪。"
echo "    构建示例： cd rust && cargo build"
echo "    产物    ： rust/target/x86_64-sukios/debug/sukios-hello"
echo "    （QEMU 运行验证见 results/step68.md 第二部分）"
