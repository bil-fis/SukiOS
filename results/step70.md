# Step 70：Rust 程序在 SukiOS 上真实运行（第二部分 —— QEMU 验证）

> 承接 results/step69.md（第一部分：搭建 SukiOS Rust 开发工具链 `make make-rust-env`）。
> 本步骤目标：把 Rust 程序真正**装入磁盘 / 内嵌内核**，在 QEMU 下的 SukiOS 中**启动并由内核执行**，
> 经串口确认打印 `hello from rust on SukiOS`，完成运行态回归（零 panic）。

---

## 1. 设计决策与演进

### 1.1 把 Rust ELF 放进磁盘 `::BIN/RUSTHELLO.SKA`
- 在 `Makefile` 的 `disk` 目标里守卫式地把 `rust/target/x86_64-sukios/debug/sukios-hello`
  拷入 `::BIN/RUSTHELLO.SKA`；缺失则跳过（不影响其余程序）。`$(DISK)` 现依赖 `$(RUST_BIN)`，
  保证 rust 二进制更新后磁盘会重建并重新拷入。
- 这样用户可在 shell 里 `exec BIN/rusthello` 经真实「从磁盘 exec」路径运行（Servo 将来同款）。

### 1.2 自动运行的两种尝试
**(A) 在 shell 启动序列里 spawn + sys_wait —— 弃用（非确定）**
- 初版在 `user/shell.c` 启动序列（fontsrv/pchfnt 自检之后）插入：
  `sys_task_spawn("/BIN/RUSTHELLO.SKA",...)` + `sys_wait`。
- **问题**：shell 在 spawn 前会 `sys_wait(fpid)` 等待 `FONTSRV`（常驻服务）退出；
  该等待的完成依赖显示服务就绪与单核 RR 调度负载，时序不确定。压测场景下
  `fontsrv` 迟迟不退 → shell 卡在 `sys_wait` 前、rust 块永远不执行 → 自动验证时灵时不灵。
- 结论：shell 启动路径不适合做**确定性**自动验证，故**回退 shell.c**（恢复原始）。

**(B) 内嵌 blob 开机自检（最终方案，确定性）**
- 改在 `kernel/kmain.c` 的 `boot_late_init` 里，与 `posixtest`/`nettest`/`dltest` **完全相同的机制**
  把 Rust ELF 作为**内嵌 blob** 经 `task_create_user()` 直接 spawn。
- 用 **weak 符号**守卫：`_binary_rusthello_start/_end`；未构建 rust 二进制时符号解析为 NULL，
  自检跳过、不影响启动。已构建则确定性地在开机时 spawn。
- 优点：不依赖 shell、不读磁盘 FS（避开 `boot_late_init` 期间调度器尚未切换上下文时
  `exec_read_file` 的阻塞式 `mach_msg` 死锁）、与既有自检一致、可重复。

---

## 2. 改动清单

### 2.1 `Makefile`
- `RUST_BIN := $(wildcard rust/target/x86_64-sukios/debug/sukios-hello)`
  （用字面前缀路径，因为所在位置早于 `RUST_DIR` 的定义）。
- 条件定义 `RUST_BLOB`（非空仅当 `RUST_BIN` 存在），并加入 `OBJS` 链入内核。
- 新增 blob 构建规则（`ifneq ($(RUST_BIN),)` 守卫）：
  `cp $(RUST_BIN) build/user/rusthello.bin` →
  `objcopy -I binary -O elf64-x86-64 -B i386:x86-64 --redefine-sym ... build/user/rusthello.blob.o`
  → 补 `.note.GNU-stack`（不可执行栈）。
- `disk` 目标守卫式拷入 `::BIN/RUSTHELLO.SKA`，且 `$(DISK)` 依赖 `$(RUST_BIN)`。
- 新增 `rust-build` 便捷目标：`export PATH=$$HOME/.cargo/bin:$$PATH; cd $(RUST_DIR) && cargo build`。

### 2.2 `kernel/kmain.c`
`boot_late_init` 末尾（dltest 之后）加入 Rust 开机自检块：
```c
{
    extern const uint8_t _binary_rusthello_start[] __attribute__((weak));
    extern const uint8_t _binary_rusthello_end[]   __attribute__((weak));
    const uint8_t *rb = _binary_rusthello_start;
    const uint8_t *re = _binary_rusthello_end;
    if (rb && re && re > rb) {
        task_t *rt = task_create_user(rb, (size_t)(re - rb), "RUSTHELLO");
        if (rt) kprintf("[rust-boot] spawned RUSTHELLO pid=%lu\n", (unsigned long)rt->id);
        else    kprintf("[rust-boot] warn: RUSTHELLO spawn failed\n");
    } else {
        kprintf("[rust-boot] (skipped: rust ELF blob not built)\n");
    }
}
```

### 2.3 `user/shell.c`
- 已**回退**到原始启动流程（移除第一版脆弱的 rust 自检块）。
- 用户仍可用 `exec BIN/rusthello` 经磁盘路径运行（真实场景）。

---

## 3. 踩坑与修复（固化进 Makefile/代码）

| # | 现象 | 原因 | 修复 |
|---|------|------|------|
| 1 | `RUST_BIN` 始终为空、blob 没链入 | `RUST_BIN` 定义早于一处 `RUST_DIR` 定义，`$(RUST_DIR)` 为空 | 改用字面前缀路径 `rust/target/...` |
| 2 | `make: I: No such file` / Error 127 | `$(OBJCOPY)` 在本 Makefile 未定义 | 改用字面 `objcopy`（与既有 `.ssvc.blob.o` 规则一致） |
| 3 | blob 符号名不符、weak 恒为 NULL、自检 skip | `objcopy -I binary` 把**输入路径**编入符号：`build/user/rusthello.bin` → `_binary_build_user_rusthello_bin_start` | `--redefine-sym _binary_build_user_rusthello_bin_start=_binary_rusthello_start`（按真实路径化符号改名） |
| 4 | 仅改 disk 配方里的拷入命令、`disk.img` 不重建 | `$(DISK)` 先决条件未含 rust 二进制，且 `build/disk.img` 已是最新 | 让 `$(DISK)` 依赖 `$(RUST_BIN)`（存在时）；本步首跑前手动 `rm -f build/disk.img` 强制重建 |
| 5 | shell 自动运行时灵时不灵 | shell 的 `sys_wait(fontsrv)` 在 fontsrv 未退出前阻塞，rust 块到不了 | 弃用 shell 路径，改内核内嵌 blob 开机自检（确定性） |
| 6 | 反复 `make run-headless` 后 serial 日志不生成 / 内容被旧进程覆盖 | 残留 qemu 进程仍占用 `/dev/kvm` 与 ISO，新 qemu 静默起不来；旧 qemu 持有旧日志 fd 继续写 | 每次重跑前 `pkill -9 -f qemu-system-x86_64; sleep` 彻底清场（纯环境/进程管理问题，非代码缺陷） |

---

## 4. 验证（生产可工作，非 stub）

构建与运行：
```sh
make make-rust-env        # 安装 rustup + 生成 x86_64-sukios.json + libsuki.a（见 step69）
cd rust && cargo build    # 产出 rust/target/x86_64-sukios/debug/sukios-hello
make run-headless QEMU_SERIAL="-serial file:rb.log" RUN_TIMEOUT=90
```

`rb.log` 关键行（节选，pid 因启动顺序可能变化）：
```
[sched] user task 'RUSTHELLO' pid=14 cpu=0 cr3=0x... entry=0x0000000000400000 (ELF, W^X)
[rust-boot] spawned RUSTHELLO pid=14
hello from rust on SukiOS
[syscall] task 'RUSTHELLO' pid=14 exit(code=0)
[sched] task 'RUSTHELLO' pid=14 exited (code=0)
```

含义：
- 内核 ELF 加载器把 Rust ELF 装载到 **0x400000**（`user.ld` 固定基址），并强制 **W^X**；
- 开机自检经 `task_create_user` 确定性 spawn（与 posixtest 同款路径）；
- Rust 的 `_start` 经标准 x86_64 `syscall` 指令调 `SYS_DEBUG_WRITE(4)` 打印
  `hello from rust on SukiOS`（该输出经 `user_puts→serial_writestr` 一定落串口，`console.c:228/239`，
  故无图形 headless 下也可见）；
- 随后 `SYS_TASK_EXIT(2)` 退出，**退出码 0**，内核干净回收。
- 同一启动中 POSIX 一致性测试仍 `PASS=192 FAIL=0`，**零 panic**。

> 说明：`hello from rust on SukiOS` 前偶见 `  [PASS] ` 串扰，那是 `posixtest` 打印
> 测试前缀（不带换行）时被单核 RR 抢占、Rust 插空打印所致——属两进程并发运行的
> 正常现象，Rust 实际打印内容不含该前缀（见干净一次的 `rb.log:167`）。

---

## 5. 交付状态

- **第一部分**（step69）：`make make-rust-env` 自动探查/安装 rust、构建 `libsuki.a`、生成
  `x86_64-sukios.json` 目标规格；`cargo build` 产出合法 SukiOS ELF。
- **第二部分**（本步）：Rust 程序经内核内嵌 blob 开机自检在 QEMU 下的 SukiOS **真实运行**，
  串口确认 `hello from rust on SukiOS`、退出码 0、零 panic；磁盘里亦留有 `::BIN/RUSTHELLO.SKA`
  供用户经 shell `exec` 运行。

两阶段共同构成「SukiOS Rust 支持」可用基线，为后续 Servo 移植（自定义 `os="sukios"` 目标 +
`library/std` 后端 build-std）扫清了工具链与运行时装载/ syscall 通路。

---

## 6. 后续（展望，非本轮必做）
- 扩展 `rust/src/main.rs` 经 FFI 调 `libsuki.a` 的 `malloc`/pthread/syscall，验证堆、线程、FD。
- 把 `RUSTHELLO` 由「内嵌 blob 自检」升级为「磁盘加载 + 用户态手动 exec」的端到端示例文档。
- 正式 `os="sukios"` 目标规格 + `rust-src` 编译 `library/std` 的 `sys/sukios` 后端（Servo 前置）。
