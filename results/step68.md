# Step 68：让 Rust 运行在 SukiOS 上的评估与路线图

> 用户目标：后续要移植 Servo 开源浏览器，因此现在就需要为 SukiOS 做好 Rust 支持。
> 本步骤只做**评估**，不落地任何代码/构建改动（按用户明确要求）。

---

## 0. 结论速览

| 维度 | 结论 |
|------|------|
| 能否在 SukiOS 上跑 Rust？ | **可以**，且比"从零移植 std"要容易得多——SukiOS 已自带一套可用的 C 运行库与标准 x86_64 syscall ABI，Rust 可**链接现有 C 运行时**而非重写 std。 |
| 最短路径（no_std 应用） | 用 Rust 内置目标 `x86_64-unknown-none` + 交叉 `ld`/C 库，写 `#![no_std]` + `_start` + 自定义 `panic_handler`/`global_allocator`（包 C 的 `malloc`/`free`），即可让 Rust 应用/服务在 SukiOS 上打印、调用 syscall、使用堆。**工作量小，可快速验证。** |
| 完整 std（Servo 必需） | 需要为 SukiOS 写一个 `library/std/src/sys/sukios` 后端并用 `cargo build-std` 编译 std。工作量大（数月级），且 Servo 还依赖 SpiderMonkey/Skia/FreeType 等 C/C++ 库在 SukiOS 上的移植。**应分阶段，不要一步到位。** |
| 最关键的阻断点 | ① 本机无 Rust 工具链（前置）；② `user.ld` 丢弃 `.eh_frame`，破坏 unwind/C++ 异常（对 Servo 的 `panic=unwind` 是硬伤，需保留 eh_frame + 提供 libunwind/`rust_eh_personality`）；③ 缺 `epoll`/`signalfd`/`eventfd` 等 Servo 所需的事件/syscall。 |

---

## 1. SukiOS 当前对 Rust 已有的支撑（实测）

调查对象：`/mnt/d/Projects/SukiOS/{user/,Makefile,include/sukios/posix.h}`。

### 1.1 工具链与加载模型
- 用户态用专属交叉编译器 `x86_64-sukios-elf-gcc/ld/ar`（Makefile 自动探测，缺失则回退 `x86_64-elf-gcc`/`gcc`/`ld`）。见 `Makefile` 第 4–45 行。
- 应用是**固定装载于 `0x400000` 的扁平 ELF**（`user/user.ld`：`ENTRY(_start); . = 0x400000`），内核直接跳到基址执行；ELF 加载器 `elf_load` 也支持 `ET_DYN`。
- **关键**：加载基址固定 → Rust 用 `-C relocation-model=static -C position-independent=false` 即可，无需 GOT/PLT 重定位（仅 `R_X86_64_RELATIVE`，加载器已支持，见 step67 的 GLOB_DAT/COPY）。

### 1.2 syscall ABI（与 Rust 天然兼容）
- 标准 x86_64 `syscall` 指令：`%rax=调用号`，`%rdi/%rsi/%rdx/%r10/%r8/%r9` 为参数（`user/lib/suki.h` 内联汇编，System V AMD64）。Rust 用内联 `asm!` 或 FFI 调 `suki_syscallN` 都能直接发 syscall。
- 号表在 `include/sukios/posix.h`，覆盖极广：`open/read/write/lseek/stat/...`、`mmap/munmap/mprotect`、`fork/execve/clone/exit`、`futex`、`arch_prctl`、`kill/sigaction/sigreturn`、`clock_gettime/nanosleep`、`socket/bind/connect/...`（150–166）、`dlopen/dlsym/dlclose`（126–129）。**核心 POSIX 面已具备。**

### 1.3 已有 C 运行库（Rust 可直接复用，而非重写）
`user/lib/` 内含：
- `stdlib.c`：`malloc/free/calloc/realloc` 基于 **`SYS_BRK`**（sbrk），first-fit + 合并，**真正可用**（非 stub）；`exit/_exit/abort/getenv` 等。
- `pthread.c`：基于 `SYS_CLONE(CLONE_VM|SETTLS|CHILD_SETTID|CLEARTID)` + `SYS_FUTEX` + `arch_prctl(ARCH_SET_FS)` 的线程实现；`self` 指针置于 `%fs:0`，与 glibc 的 TLS 布局一致。即 **TLS/线程机制已就绪**。
- `unistd.c/stdio.c/string.c/time.c/dirent.c/dlfcn.c/signal.c/suki.c`：完整 POSIX 接口与 SukiNative 封装。

➡️ **这意味着 Rust 不需要从零移植 std**：可链接这套 C 库获得 syscall/堆/线程能力（类似 Rust 在 bare-metal 上链接 newlib/musl 的思路）。

### 1.4 常量齐备性
`include/sukios/posix.h` 实测存在：`SUKI_PROT_READ/WRITE`、`SUKI_MAP_PRIVATE/ANONYMOUS/FIXED`、`SUKI_ARCH_SET_FS=0x1002`、`SUKI_CLONE_SETTLS=0x0008`。与 `pthread.c` 用法吻合。

---

## 2. 两个层次的支持（必须分阶段）

```
层级 A：Rust no_std 裸应用/服务   ── 工作量小，可立即做，作为地基
   │  （core + alloc + 现有 C 库 FFI，panic=abort）
   ▼
层级 B：Rust std 完整移植          ── 工作量大，Servo 前提
   │  （library/std/src/sys/sukios 后端 + build-std）
   ▼
层级 C：Servo 浏览器               ── 极大，依赖 B + 一堆 C/C++ 库移植
        （SpiderMonkey / WebRender(Skia) / FreeType+HarfBuzz / SQLite …）
```

**强烈建议先只做 A**，把"SukiOS 上能跑 Rust、能调 syscall、能用堆/线程"跑通并要求 QEMU 生产回归零 panic，再谈 B/C。

---

## 3. 层级 A（no_std）落地方案（仅规划，未实现）

### 3.1 目标选择（关键简化）
用 Rust **内置目标** `x86_64-unknown-none`（tier-2 内置，无需自定义 JSON）即可，而不是立刻写自定义 target JSON：
- 它本身就是 freestanding、`no_std` 友好、`panic=abort` 可用。
- 通过 `.cargo/config.toml` 覆盖链接器与参数即可指向 SukiOS 工具链。

```toml
# .cargo/config.toml（规划草稿，未落地）
[target.x86_64-unknown-none]
linker = "x86_64-sukios-elf-gcc"
rustflags = [
  "-C", "link-arg=-Tuser/user.ld",
  "-C", "link-arg=-nostdlib",
  "-C", "link-arg=-lgcc",
  "-C", "link-arg=-lsuki",        # 由 user/lib 编出的静态归档
  "-C", "relocation-model=static",
  "-C", "position-independent=false",
]
[profile.dev]  panic = "abort"
[profile.release] panic = "abort"
```

### 3.2 入口与运行时（复用 C 的 crt0 语义）
- 选项一：Rust 自己写 `#[no_mangle] pub unsafe extern "C" fn _start() -> !`，按 `user/lib/crt0.S` 的栈布局（[rsp]=argc, [rsp+8]=argv, envp）取参，设好 FS base（若用 TLS），调 Rust 逻辑，最后 `suki_syscall1(SYS_TASK_EXIT, code)`。
- 选项二：链接 `crt0.o`，让 C 入口去调一个 `#[no_mangle] extern "C" fn main(...)`（需自己实现 Rust 的 `main` 跳板）。
- 必须提供：`#[panic_handler]`、`#[no_mangle] extern "C" fn rust_eh_personality`（abort 模式可简化）、可能的 `#[lang = "eh_personality"]` 占位。

### 3.3 全局分配器（包 C 的 malloc/free）
```rust
// 规划草稿
extern "C" { fn malloc(s: usize) -> *mut u8; fn free(p: *mut u8); }
struct SukiAlloc;
unsafe impl core::alloc::GlobalAlloc for SukiAlloc {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 { malloc(l.size()) }
    unsafe fn dealloc(&self, p: *mut u8, _: Layout) { free(p) }
}
#[global_allocator] static A: SukiAlloc = SukiAlloc;
```
这样 `alloc` 容器、`Vec`/`String` 直接可用，无需另写分配器。

### 3.4 调 syscall
- 最简：把 `suki_syscallN` 用 Rust `asm!`（`syscall` 指令，clobber `rcx,r11` 等）重写；或 extern "C" 直接调 `user/lib/suki.h` 的 C 函数（已内联汇编实现，最省事、与内核号表单一真相源一致）。

### 3.5 预期首个验证（QEMU 生产回归）
一个 Rust 程序：`_start` 里 `sys_debug_write("hello from rust\n")` → `SYS_TASK_EXIT(0)`。
构建：`cargo build --target x86_64-unknown-none` → 产物用我们的 `ld -T user/user.ld -nostdlib` 链接 `libsuki.a`/`libgcc` → 装进磁盘 → QEMU 跑，在 serial 日志看到输出、零 panic。

---

## 4. 层级 B（std 移植）需要的额外工作（仅规划）

Servo 强制依赖 std。std 移植 = 给 Rust 增加一个 OS 后端：

1. **自定义 target JSON**（`x86_64-sukios.json`）：`llvm-target` 仿 `x86_64-unknown-linux-gnu`，但 `os="sukios"`，`linker="x86_64-sukios-elf-gcc"`，`panic="unwind"`（Servo 用 unwind），`relocation-model="static"`。
2. **`library/std/src/sys/sukios/` 后端**：实现 `os`、`fs`、`net`、`thread`、`time`、`env`、`alloc` 等模块，把调用映射到 `posix.h` 的号表（open/read/write/mmap/clone/futex/arch_prctl/socket…）。这是 std 移植的主体工作量。
3. **TLS 初始化**：Rust 期望 `%fs` 基址指向 TCB（与现有 `pthread.c` 一致），需在 std 启动路径调用 `arch_prctl(ARCH_SET_FS)`。
4. **auxv**：Rust std 在 x86_64 上读 `auxv`（AT_PAGESZ 等）。当前 `crt0.S` 栈布局**只有 argc/argv/envp，未给 auxv**。需二选一：(a) 改加载器推送 auxv；(b) std 后端硬编码页大小等。属必须解决项。
5. **unwind**：`user.ld` 当前 `/DISCARD/ : { *(.eh_frame*) }` 会**破坏 Rust unwind 与 C++ 异常**。需：
   - 停止丢弃 `.eh_frame`/`.eh_frame_hdr` 并保证加载器映射；
   - 提供 `libunwind` 与 `rust_eh_personality`（可链接 LLVM libunwind，或用 Rust 自带 `unwind` 子系统）。
   - **这是 Servo 能否运行的最大单点风险之一。**
6. **red zone**：Rust/C 假定 Ring3 下 `%rsp` 下方 128 字节 red zone 不被破坏。需确认 SukiOS 信号递交路径（含嵌套、clone 子线程栈）不踩 red zone。
7. 用 `cargo build-std -Z build-std=std` 编译出 `std` 供 Servo 使用（需 nightly）。

---

## 5. 面向 Servo 的 syscall / 能力缺口（需在未来补）

`include/sukios/posix.h` 中**不存在**以下 Servo 常用项（grep 确认）：
- `epoll_create/ctl/wait`（Servo 事件循环核心；可临时用现成的 `poll(82)/select(81)` 模拟，但性能/语义有损）
- `signalfd`、`eventfd`、`timerfd`（libevent/ Servo 网络栈常用）
- `accept4`、`pipe2`、`dup3`、`preadv`/`pwritev`
- `sched_setaffinity`、`prctl` 细分子项、`set_robust_list`/`get_robust_list`（futex 鲁棒性）
- `memfd_create`、`io_uring` 等（可后放）

**已具备**（利好）：完整 socket 簇 150–166、`mmap/mprotect/madvise`、`clone/futex`、`clock_gettime/nanosleep/getrandom(112)`、`dlopen/dlsym`、FreeType 已移植。

> 注：Servo 还强依赖 SpiderMonkey（C++ JIT，需大量 syscall/线程/mmap/信号）、WebRender/Skia（图形——SukiOS 现有 `SYS_FRAMEBUFFER_MAP(200)`/`SYS_DISPLAY_BLIT(203)` 是帧缓冲级，无 GPU/OpenGL，WebRender 需软件光栅化或大幅改造）、HarfBuzz/FreeType（FreeType 已有）。**即便 Rust+std 跑通，Servo 完整可跑仍是数月乃至更久的工程**，建议先以"Rust 应用/服务能跑 + 关键 C 库能在 SukiOS 上被 Rust FFI 调用"为里程碑。

---

## 6. 前置条件与风险登记

| 项 | 状态 | 说明 |
|----|------|------|
| Rust 工具链（rustup/nightly/cargo） | ❌ 本机缺失 | 必须先 `rustup` 安装 nightly（含 `rust-src` 组件供后续 build-std）。 |
| 交叉 `x86_64-sukios-elf-gcc/ld/ar` | ✅ 已有 | Makefile 已支持，Rust 链接直接复用。 |
| C 运行库可编译为静态归档供 Rust 链接 | ⚠️ 需建归档 | 把 `user/lib` 编成 `libsuki.a`（或沿用现有 `.o` 列表）。 |
| `user.ld` 保留 `.eh_frame` | ❌ 当前丢弃 | 影响 unwind/C++ 异常；Servo 前必须修。 |
| auxv 推送 | ❌ 未提供 | std 启动需；或后端硬编码。 |
| 缺 epoll/signalfd/eventfd | ❌ 缺失 | Servo 前需补（至少 epoll 用 poll 模拟）。 |
| red zone 在信号路径安全 | ⚠️ 待验证 | 需查 `signal.c`/内核信号递交实现。 |
| 动态链接 Rust | ⚠️ 可静态 | 先用静态链接，绕开 ld.so 复杂度。 |

---

## 7. 建议的推进节奏（未执行）

1. **P1（地基，小）**：装 rustup nightly；把 `user/lib` 编成 `libsuki.a`；写最小 `#![no_std]` Rust 程序（调 `sys_debug_write` + 用堆）经 `x86_64-unknown-none` + 我们的 `ld` 跑通，QEMU 看输出、零 panic。
2. **P2（能力）**：在 Rust 侧通过 FFI 包装现有 C 库，实现 1–2 个真实 Rust 用户服务（如用 Rust 重写某个 server，验证 syscall/堆/pthread/futex 链路）。
3. **P3（std，大）**：仅当 P2 稳定后，再写 `sys/sukios` 后端 + 自定义 target + build-std；补 auxv、eh_frame、TLS、unwind。
4. **P4（Servo，极大）**：补 epoll/signalfd/eventfd 等 syscall；移植 SpiderMonkey/Skia/HarfBuzz（FreeType 已有）；以 headless/受限 Servo 为首个目标。

每一步都应在 QEMU 生产场景回归（零 panic、无堆损坏）后才算完成，再进入下一步。

---

## 8. 本步骤未做任何代码/构建改动
仅完成评估与路线图。下一步待用户确认后，可从 P1 的最小 Rust 程序起步（不触及 std/Servo）。
