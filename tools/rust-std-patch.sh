#!/bin/bash
# =============================================================================
# tools/rust-std-patch.sh — 让 `cargo -Z build-std=std` 能在 SukiOS 上跑起来
# -----------------------------------------------------------------------------
# 背景：自定义目标 x86_64-sukios（target_os="sukios"）没有预编译 std，必须用
# `-Z build-std` 从 rust-src 现场编译。但 std 的依赖图里有若干 crate（unwind /
# panic_unwind / std_detect）对「非 Windows」目标无条件依赖 crates.io 的 `libc`
# crate，而 `libc` 本身不认识 target_os="sukios"（其 new/mod.rs 的 unix 分支会
# `pub use unistd::*` 失败）。
#
# 本脚本把这些依赖在 **sukios 目标下**排除掉（std 的 SukiOS 后端刻意不经 libc，
# 而是走 crate::sys::sukios_ffi），从而让 build-std 越过依赖层、进入 std 自身编译。
#
# 幂等：重复执行安全（已打过的补丁会跳过）。改动落在 rust-src 组件内，属于
# 「本仓库的 Rust 开发环境」的一部分（与 make make-rust-env 生成的目标规格同类）；
# rustup 升级工具链后需重新执行（make rust-std-patch）。
#
# 用法：tools/rust-std-patch.sh [--check]
#   --check : 只报告将要做/已做的改动，不写文件
# =============================================================================
set -e

CHECK=0
[ "$1" = "--check" ] && CHECK=1

export PATH="$HOME/.cargo/bin:$PATH"
if ! command -v rustc >/dev/null 2>&1; then
    echo "!! 未找到 rustc（先 make make-rust-env）" >&2
    exit 1
fi
SRC="$(rustc --print sysroot)/lib/rustlib/src/rust/library"
if [ ! -d "$SRC/std" ]; then
    echo "!! rust-src 组件缺失（rustup component add rust-src）" >&2
    exit 1
fi
echo "[std-patch] rust-src = $SRC"

CHECK=$CHECK SRC="$SRC" python3 - "$SRC" <<'PY'
import os, re, sys

src = sys.argv[1]
check = os.environ.get("CHECK") == "1"
changed = 0
skipped = 0
missing = 0

def apply(rel, old, new, note=""):
    """把 rel 中第一处 old 换成 new；已含 new 则跳过；找不到 old 则报告。"""
    global changed, skipped, missing
    p = os.path.join(src, rel)
    if not os.path.exists(p):
        print("  !! 文件不存在: %s" % rel); missing += 1; return
    s = open(p, encoding="utf-8").read()
    if new in s:
        skipped += 1
        print("  =  已是目标状态: %s %s" % (rel, note)); return
    if old not in s:
        print("  !! 未匹配到锚点: %s %s" % (rel, note)); missing += 1; return
    if not check:
        open(p, "w", encoding="utf-8").write(s.replace(old, new, 1))
    changed += 1
    print("  %s %s %s" % ("+ 打补丁:" if not check else "+ 待打补丁:", rel, note))

# ---------------------------------------------------------------- 1) libc 依赖排除
# unwind / panic_unwind / std_detect：sukios 下不依赖 libc（SukiOS 后端不用 libc）
apply("unwind/Cargo.toml",
      "[target.'cfg(not(all(windows, target_env = \"msvc\")))'.dependencies]",
      "[target.'cfg(all(not(all(windows, target_env = \"msvc\")), not(target_os = \"sukios\")))'.dependencies]",
      "(libc 依赖排除 sukios)")

apply("std_detect/Cargo.toml",
      "[target.'cfg(not(windows))'.dependencies]",
      "[target.'cfg(all(not(windows), not(target_os = \"sukios\")))'.dependencies]",
      "(libc 依赖排除 sukios)")

apply("panic_unwind/Cargo.toml",
      "[target.'cfg(not(all(windows, target_env = \"msvc\")))'.dependencies]",
      "[target.'cfg(all(not(all(windows, target_env = \"msvc\")), not(target_os = \"sukios\")))'.dependencies]",
      "(libc 依赖排除 sukios)")

# ------------------------------------------------------- 2) unwind：sukios 无 unwinder
apply("unwind/src/lib.rs",
      "// Force libc to be included even if unused. This is required by many platforms.\n#[cfg(not(all(windows, target_env = \"msvc\")))]\nextern crate libc as _;",
      "// Force libc to be included even if unused. This is required by many platforms.\n// SukiOS: 无 libc（走 crate::sys::sukios_ffi），故对 sukios 目标不引入。\n#[cfg(all(not(all(windows, target_env = \"msvc\")), not(target_os = \"sukios\")))]\nextern crate libc as _;",
      "(不引入 libc)")

apply("unwind/src/lib.rs",
      "    any(target_os = \"none\", target_os = \"espidf\", target_os = \"nuttx\") => {\n        // These \"unix\" family members do not have unwinder.\n    }",
      "    any(target_os = \"none\", target_os = \"espidf\", target_os = \"nuttx\",\n        target_os = \"sukios\") => {\n        // These \"unix\" family members do not have unwinder.（SukiOS：panic=abort，无需 unwinder）\n    }",
      "(走无 unwinder 分支)")

# ------------------------------------------------------- 3) std：移除 libc 依赖行
# 上游 std/Cargo.toml 在 [target.'cfg(unix)'.dependencies] 下声明 libc；
# SukiOS 后端不经 libc，直接删除该依赖行（cargo 对自定义 target_os 的 cfg 引擎
# 无法识别 sukios，故不能用 cfg 排除，只能删）。
p = os.path.join(src, "std/Cargo.toml")
s = open(p, encoding="utf-8").read()
if re.search(r"^libc = ", s, re.M):
    if not check:
        s2 = re.sub(r"^libc = .*\n", "", s, flags=re.M)
        s2 = s2.replace("[target.'cfg(unix)'.dependencies]",
                        "# SukiOS: 不依赖 libc，统一走 crate::sys::sukios_ffi\n"
                        "# （cargo 对自定义 target_os=\"sukios\" 的依赖 cfg 引擎无法识别，故直接移除依赖行）")
        open(p, "w", encoding="utf-8").write(s2)
    changed += 1
    print("  %s std/Cargo.toml (移除 libc 依赖行)" % ("+ 打补丁:" if not check else "+ 待打补丁:"))
else:
    skipped += 1
    print("  =  已是目标状态: std/Cargo.toml (无 libc 依赖行)")

print()
print("[std-patch] 结果：改动 %d 处，已就绪 %d 处，异常 %d 处" % (changed, skipped, missing))
sys.exit(1 if missing else 0)
PY
