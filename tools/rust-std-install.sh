#!/bin/bash
# =============================================================================
# tools/rust-std-install.sh — 把仓库里的 SukiOS std 后端装进 rust-src
# -----------------------------------------------------------------------------
# `cargo -Z build-std` 从 rust-src 组件现场编译 std，因此我们的后端文件必须落在
# rust-src 树内。本脚本按固定映射把 rust/std/sys/ 下的文件复制进去（幂等，
# 每次覆盖），并在文件头写入「来自仓库哪一路径」的标记，便于对照与清理。
#
# 映射规则： rust/std/sys/<name>.rs  ->  <rust-src>/std/src/sys/<name>.rs
#            rust/std/sys/<mod>/<name>.rs -> <rust-src>/std/src/sys/<mod>/<name>.rs
#
# 用法：
#   tools/rust-std-install.sh           # 安装（覆盖 rust-src 中对应文件）
#   tools/rust-std-install.sh --dry-run # 只列出将安装哪些文件
#   tools/rust-std-install.sh --diff    # 安装前显示与现版本的差异摘要
#
# 说明：rust-src 属「本仓库 Rust 开发环境」的一部分（与 make make-rust-env 生成的
# 目标规格同类）；rustup 更换工具链后需重新执行（make rust-std-install）。
# =============================================================================
set -e

MODE="${1:-install}"
export PATH="$HOME/.cargo/bin:$PATH"

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC_LOCAL="$REPO/rust/std"
if ! command -v rustc >/dev/null 2>&1; then
    echo "!! 未找到 rustc（先 make make-rust-env）" >&2
    exit 1
fi
SRC="$(rustc --print sysroot)/lib/rustlib/src/rust/library"
DST="$SRC/std/src"
[ -d "$DST/sys" ] || { echo "!! rust-src 未安装或不含 std（rustup component add rust-src）" >&2; exit 1; }

echo "[std-install] repo = $SRC_LOCAL"
echo "[std-install] rust-src = $DST"
[ -d "$SRC_LOCAL" ] || { echo "!! 缺少 $SRC_LOCAL" >&2; exit 1; }

installed=0
LIST="$(mktemp)"
find "$SRC_LOCAL" -type f -name '*.rs' | sort > "$LIST"
while IFS= read -r f; do
    rel="${f#"$SRC_LOCAL"/}"          # 如 sys/sukios_ffi.rs
    dst="$DST/$rel"
    case "$MODE" in
        --dry-run|dry-run)
            echo "  would install: $rel"; installed=$((installed+1)); continue ;;
        --diff|diff)
            if [ -f "$dst" ] && ! diff -q "$f" "$dst" >/dev/null 2>&1; then
                echo "  differs: $rel  ($(diff "$f" "$dst" | grep -c '^[<>]') changed lines)"
            fi
            ;;
    esac
    mkdir -p "$(dirname "$dst")"
    cp "$f" "$dst"
    installed=$((installed+1))
done < "$LIST"
rm -f "$LIST"

if [ "$MODE" = "--dry-run" ] || [ "$MODE" = "dry-run" ]; then
    echo "[std-install] 共 $installed 个文件（dry-run，未写入）"
    exit 0
fi
echo "[std-install] 已安装 $installed 个后端文件到 rust-src"

# ---------------------------------------------------------------------------
# 分发补丁：上游 std 的部分模块**没有** `target_os = "sukios"` 分支，需补上，
# 否则会报「none of the predicates in this cfg_select evaluated to true」或走到
# no_threads 兜底（触发 compile_error）。补丁把 sukios 分支插在某个**不会被
# sukios 命中的**既有分支之前（cfg_select 取首个命中项，位置只要不被抢先即可）。
# 幂等：文件中已存在 sukios 分支则跳过。
# ---------------------------------------------------------------------------
echo "[std-install] 打分发补丁（alloc / io::error / sync×4）..."
DST="$DST" python3 - <<'PY'
import os, sys, json

dst = os.environ["DST"]

# (相对路径, 插入点锚点, 插入内容)
PATCHES = [
    ("sys/alloc/mod.rs",
     '    target_os = "zkvm" => {\n',
     '    target_os = "sukios" => {\n'
     '        mod sukios;\n'
     '        use sukios as imp;\n'
     '    }\n'),
    ("sys/io/error/mod.rs",
     '    any(target_os = "vexos", target_family = "wasm", target_os = "zkvm", target_os = "trusty") => {\n',
     '    target_os = "sukios" => {\n'
     '        mod sukios;\n'
     '        pub use sukios::*;\n'
     '    }\n'),
    ("sys/sync/mutex/mod.rs", '    _ => {\n        mod no_threads;\n',
     '    target_os = "sukios" => {\n'
     '        mod futex;\n'
     '        pub use futex::Mutex;\n'
     '    }\n'),
    ("sys/sync/condvar/mod.rs", '    _ => {\n        mod no_threads;\n',
     '    target_os = "sukios" => {\n'
     '        mod futex;\n'
     '        pub use futex::Condvar;\n'
     '    }\n'),
    ("sys/sync/once/mod.rs", '    _ => {\n        mod no_threads;\n',
     '    target_os = "sukios" => {\n'
     '        mod futex;\n'
     '        pub use futex::{Once, OnceState};\n'
     '    }\n'),
    ("sys/sync/rwlock/mod.rs", '    _ => {\n        mod no_threads;\n',
     '    target_os = "sukios" => {\n'
     '        mod futex;\n'
     '        pub use futex::RwLock;\n'
     '    }\n'),

    # ---- 其余模块级补丁（P0 收尾） ----

    # std/src/lib.rs：无 libc（SukiOS 走 crate::sys::sukios_ffi，不链 libsuki.a）。
    # 原 `#[cfg(not(all(windows, target_env="msvc")))] extern crate libc;` 对 sukios
    # 仍为真 -> 拉进不存在的 libc crate。补 `not(target_os="sukios")` 门控。
    # 注意：DST 已是 std/src，故相对路径只用 "lib.rs"（不能写 "std/src/lib.rs"）。
    # lib.rs：replace 模式（直接替换原 extern crate libc，避免重复插入成两个）。
    ("lib.rs", "replace",
     '#[cfg(not(all(windows, target_env = "msvc")))]\nextern crate libc;\n',
     '#[cfg(all(not(all(windows, target_env = "msvc")), not(target_os = "sukios")))]\nextern crate libc;\n'),

    # sys/sync/futex/mod.rs：缺 sukios 分支（落到 `_ => {}` 空实现），导致
    # condvar/mutex/once/rwlock 的 futex.rs 找不到 Futex/futex_wait/...。
    # 用 WIP 随附的 sys/sync/futex/sukios.rs（直连 ffi::futex，原生 218/219）。
    ("sys/sync/futex/mod.rs", '    _ => {}\n',
     '    target_os = "sukios" => {\n'
     '        mod sukios;\n'
     '        pub use sukios::*;\n'
     '    }\n'),

    # sys/thread_local/mod.rs：P0 关 TLS（has-thread-local=false），需走 no_threads
    # 回退（TLS 数据存普通 static）。否则落到 `_ => os`（要求 ELF 原生 TLS），
    # 而 os.rs 的 cfg_select 不含 sukios -> 一堆「configured out」错误。
    ("sys/thread_local/mod.rs", '    _ => {\n        mod os;\n',
     '    target_os = "sukios" => {\n'
     '        mod no_threads;\n'
     '        pub use no_threads::{EagerStorage, LazyStorage, thread_local_inner};\n'
     '        pub(crate) use no_threads::{LocalPointer, local_pointer};\n'
     '    }\n'),

    # sys/mod.rs：声明 `mod sukios_ffi`，供 sys::sync::futex::sukios 经
    # `crate::sys::sukios_ffi` 引用（futex/时间/堆等原生 FFI）。仅 sukios 编译该模块。
    # 锚点 `mod pal;` 唯一，插入其后。
    ("sys/mod.rs", 'mod pal;\n',
     '// SukiOS: 原生 FFI 层（futex / 时间 / 堆等直连 SukiNative，无 libc）\n'
     '#[cfg(target_os = "sukios")]\n'
     'pub(crate) mod sukios_ffi;\n'),

    # sys/thread_local/no_threads.rs：其顶部 `compile_error!` 守卫
    # `#[cfg(target_has_threads)]` 对「有线程的 sukios」会误伤（我们刻意用
    # no_threads 回退，因为 has-thread-local=false 当前不支持原生 TLS）。
    # 把 sukios 从守卫中排除。
    ("sys/thread_local/no_threads.rs", "replace",
     '#[cfg(target_has_threads)]\ncompile_error!("Using no_threads implementation on a target with threads");\n',
     '#[cfg(all(target_has_threads, not(target_os = "sukios")))]\ncompile_error!("Using no_threads implementation on a target with threads");\n'),

    # sys/thread_local/mod.rs (guard 模块)：sukios 有线程但无原生 TLS，应像
    # wasm/uefi/zkvm/trusty/vexos 那样走空 `enable()` 分支，而非 key-based TLS
    #（后者要求 key::LazyKey/set，而 no_threads 后端不提供）。把 sukios 加入该
    # no-op 分支列表。
    ("sys/thread_local/mod.rs",
     '            target_os = "vexos",\n        ) => {\n            pub(crate) fn enable() {\n',
     '            target_os = "sukios",\n            target_os = "vexos",\n        ) => {\n            pub(crate) fn enable() {\n'),

    # sys/random/mod.rs：sukios 无原生随机分支，落到 `_ => {}` 导致 `sys::fill_bytes`
    # 缺失（std 的 random / HashMap 默认 RandomState 都依赖它）。新增 sukios 分支，
    # 经 SukiNative SYS_SUKI_RANDOM（getrandom 后端）填充。
    ("sys/random/mod.rs", '    _ => {}\n',
     '    target_os = "sukios" => {\n'
     '        mod sukios;\n'
     '        pub use sukios::fill_bytes;\n'
     '    }\n'),

    # sys/stdio/mod.rs（注意路径是 sys/stdio 不是 sys/io/stdio）：SukiOS 标准 I/O 后端
    # 已就绪（rust/std/sys/stdio/sukios.rs），写经 sukios_ffi::write(SYS_DEBUG_WRITE)。
    # 把 sukios 分支插到默认 unsupported 之前。
    ("sys/stdio/mod.rs",
     '    _ => {\n        mod unsupported;\n        pub use unsupported::*;\n',
     '    target_os = "sukios" => {\n'
     '        mod sukios;\n'
     '        pub use sukios::*;\n'
     '    }\n'),
]

ok = skip = bad = 0
for item in PATCHES:
    # 4 元组 (rel, mode, anchor, insert) 中 mode 为 "replace" 时直接替换锚点；
    # 缺省（3 元组）为 "insert"：在锚点前插入 insert（用于新增 cfg_select 分支）。
    if len(item) == 4:
        rel, mode, anchor, insert = item
    else:
        rel, anchor, insert = item
        mode = 'insert'
    p = os.path.join(dst, rel)
    if not os.path.exists(p):
        print("  !! 文件不存在: %s" % rel); bad += 1; continue
    s = open(p, encoding="utf-8").read()
    # 精确幂等：插入内容已存在则跳过（比全局 "target_os = sukios" 判断更稳，
    # 因为单个文件可能有多个彼此独立的 sukios 补丁，如 thread_local/mod.rs）。
    if insert in s:
        print("  =  已存在 sukios 分支: %s" % rel); skip += 1; continue
    if anchor not in s:
        print("  !! 锚点未匹配: %s" % rel); bad += 1; continue
    new = insert + anchor if mode == 'insert' else insert
    open(p, "w", encoding="utf-8").write(s.replace(anchor, new, 1))
    print("  + 已插入 sukios 分支: %s" % rel); ok += 1

print("[std-install] 分发补丁：新增 %d，已存在 %d，异常 %d" % (ok, skip, bad))
sys.exit(1 if bad else 0)
PY

echo "[std-install] 下一步： make rust-std-build"
