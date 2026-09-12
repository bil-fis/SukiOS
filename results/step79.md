# Step 79 — 启动四阶段重构 + 无签名安全配置系统（registry）+ 服务改名 + 预留接入点

> 日期：2026-09-12
> 目标：按用户给定的启动序列（加载内核 → fbcon → 系统初始化/读配置/加载kdr → 显示服务/登录/桌面）
> 落地配置系统，移除 grub 参数传递，所有系统服务改名 `Suki*`（大驼峰+Suki前缀），并为你
> 的「混合风格权限提升」设计预留接入点（本期不实现）。

---

## 一、评估结论（驱动本次实现的要点）

对「SukiOS 安全配置存储系统（无签名版）」方案做了评估，落地时采纳并修正了以下要点：

- **内核/端口隔离 + IPC 仲裁** 是核心防护，方向正确，保留。
- **鉴权必须以安全令牌/UID/端口权为准**（项目已有 `security_token`/`integrity_level`/`suki_uid_t`），**不得用进程名**做判决（原方案 `strcmp(caller->name,...)` 不可信）——本期仅预留，未实现仲裁。
- **CRC32 仅作错误检测**，不能防离线篡改（离线有磁盘访问者可重算）；威胁模型如实定位。
- **generation 防回滚** 仅在在线（进程）场景有效；离线场景随重启归零，属移除签名的固有代价，不夸大。
- **路径统一为 Unix 风格 `/`**（原方案 `\System\...` 已改正，与 Python 工具一致）。
- **"二进制格式混淆"不是安全层**，只作整洁性，不计入安全收益。
- **hive 存储**采用单文件 + 扁平条目数组，避免原 parent/child/sibling 偏移的复杂度；后续若需抗断电可改双槽 A/B。

---

## 二、权限提升子系统：预留接入点（不实现）

依据 `SukiOS 混合风格权限提升设计文档.md`（UAC/授权/调试角色 `suki.debug=1`）。

- 在 `kernel/kmain.c` 保留调用 `SecurityReserve()`，位于 `security_init()` 之后。
- `SecurityReserve()` 当前仅打印一行预留日志：
  `[boot] security: 权限提升子系统(UAC/授权)预留，本期不实现（见设计文档）；仅保留接入点`
- 后续实现时在此接入：完整性级别检查、能力位图、授权票据、调试角色（`suki.debug=1`）等，不影响现有启动流程。

---

## 三、启动四阶段重构（kernel/kmain.c）

### 3.1 阶段划分
1. **加载内核**：`kmain` 早期（serial/GDT/IDT/PMM/驱动/SMP/sched/syscall/ipc/posix…）。
2. **启动 fbcon**：`fb_init(&g_boot)` + `draw_boot_logo()`（logo 用 fb 直接绘制，不经过 kprintf）。
3. **系统初始化 + 读配置 + 加载 kdr**：在 `boot_late_init` 内，磁盘/FS 起来后调用
   `Stage3LoadConfigAndKdr()` 读取 `/sys/configs/system.reg`，再按配置门控 `kdr_load_all()`；
   `BootPlayAnimation()` 预留（本期空）。
4. **用户环境**：拉起 `SukiInputServer`/`SukiDisplayServer` → 等待显示就绪 → `SukiMouseServer`
   → `SukiLogon()`（预留）→ 进入 `SukiDesktopManager()`（预留，目前以 `SukiShell` 作桌面替身）
   → 启动用户自检程序。

### 3.2 串口-only 日志门控（第三步不刷屏，除非 -v/--verbose）
- 新增全局 `bool g_boot_verbose`（定义于 `kernel/console.c`，声明于 `include/kernel/console.h`）。
- `kernel/console.c` 的 `kputc` 在「显示未激活」分支改为：
  ```c
  if (g_use_fb) {
      if (g_boot_verbose) fbcon_putc(c);   /* 仅 -v 时屏显 */
      /* 否则仅串口（不写 VGA，避免无谓寄存器访问） */
  } else {
      vga_putc(c);
  }
  ```
- `kmain` 在 `bootinfo_prepare` 后调用 `ParseBootCmdline()` 解析 GRUB 内核命令行。
- **命令行解析**：`kernel/arch/x86_64/multiboot2.c` 新增 `MULTIBOOT_TAG_TYPE_COMMAND_LINE (1)`
  解析，填入 `boot_info_t.cmdline[256]`（`include/kernel/multiboot2.h` 新增字段；`pvh.c` 因
  `memset(out,0,...)` 已安全）。`ParseBootCmdline()` 扫描 `-v` / `--verbose` 设置 `g_boot_verbose`。

---

## 四、SukiRegistry Hive（无签名）内核侧解析

### 4.1 文件与布局
- `include/kernel/registry.h`：类型/结构/`system_config_t`/`sukreg_crc32`/`RegistryParseSystem` 声明。
- `kernel/registry/hive.c`：CRC32 + 解析实现（被 `find kernel -name '*.c'` 自动纳入构建）。
- **磁盘布局（与 `tools/registry_editor.py` 逐字节一致）**：
  ```
  [0..8)   magic  "SUKREG\0\0"
  [8..12)  version u32
  [12..16) flags u32
  [16..24) root_offset u64
  [24..32) entry_count u64
  [32..40) generation u64   (单调递增，在线防回滚)
  [40..48) timestamp u64
  [48..52) crc32 u32        (覆盖 [0..48) + body 的 CRC32)
  [52..64) reserved (补齐 64)
  [64..)   body：扁平条目数组，每项 = <type u32><flags u32><name_len u32><name><data_len u64><data>
  ```
- CRC32：多项式 `0xEDB88320`，初始 `0xFFFFFFFF`，与 Python 工具 `Crc32` 逐位一致。

### 4.2 内核消费字段（`system_config_t`）
- `/System/Display/Width|Height|Bpp` → 覆盖 `g_display.width/height`（注意 `display_config_t`
  无 `bpp` 字段，仅应用宽高）。
- `/System/Kernel/KdrEnabled`(bool) → 门控 `kdr_load_all()`。
- `/System/Boot/Verbose`(bool) → 覆盖 `g_boot_verbose`。

---

## 五、第三步内核读配置（不经 grub）

### 5.1 内核态同步读文件 `kern_fs_read_file`
- 新增于 `kernel/fs/fd.c`（声明于 `include/kernel/vfs.h`）：
  ```c
  int kern_fs_read_file(const char *path, uint8_t *buf, uint32_t cap, uint32_t *out_n);
  ```
- **实现要点**：直接走 `FS_PORT` 同步 RPC（`FS_MSG_OPEN`→`FS_MSG_READFD`→`FS_MSG_CLOSE`），
  **不绑定当前任务的 fd 表**（避免内核任务 fd 表未初始化导致的槽分配异常）。仅 DISK 后端，
  buffer 由调用方提供；失败返回负 errno，成功返回 0 且 `*out_n`=读取字节。
- 复用 fd.c 现有 `fs_rpc`/`rpc_req_buf`/`resp_hdr`/`resp_ret`/`fs_status_to_errno` 等基础设施。

### 5.2 `Stage3LoadConfigAndKdr()`（kernel/kmain.c）
- `kmalloc(65536)` → `kern_fs_read_file("/sys/configs/system.reg", ...)`。
- 失败/CRC 失败 → 打印警告并**回退默认值，绝不阻塞启动**（文件缺失、镜像未含配置均安全）。
- 成功 → `RegistryParseSystem()` 提取配置，应用显示参数、kdr 门控、verbose 覆盖。

### 5.3 kdr 按配置门控
- 原 `kdr_load_all()` 从 `kmain` 主流程移出，改在 `boot_late_init` 第三步、配置读取之后调用，
  受 `/System/Kernel/KdrEnabled` 门控（默认启用）。
- 在 `include/sukios/kdr.h` 补 `int kdr_load_all(void);` 声明（此前仅为隐式声明）。
- kdr 仍从 GRUB 引导模块加载（`.kdr`），后续「kdr 可选内核 hook 形式」为模块内部能力，本期未做。

---

## 六、启动动画 / 登录 / 桌面：预留（未实现）

- `BootPlayAnimation()`：第三步调用，当前仅打印 `stage3: boot animation reserved`。
- `SukiLogon()`：显示服务就绪、鼠标服务启动后调用，打印 `reserved; no password configured ->
  proceed to desktop`，随后调用 `SukiDesktopManager()`。
- `SukiDesktopManager()`：目前以 `SukiShell` 作为桌面占位替身启动；后续实装合成桌面/用户会话。
- 以上均为用户明确「暂定名称 / 暂时没有 / 预留位置」的显式延期，非静默 stub。

---

## 七、系统服务改名（Suki* 大驼峰 + Suki 前缀）

依据命名规范（记忆 45139565/服务名规范）：`kmain.c` 中所有服务 task 名改为：
`SukiFsServer`、`SukiNetServer`、`SukiInputServer`、`SukiDisplayServer`、`SukiMouseServer`、
`SukiShell`、`SukiPosixTest`、`SukiNetTest`、`SukiDlTest`、`SukiWinHello`、`SukiRustHello`、
`SukiConsoleServer`、`SukiBootLate`。同时固化到记忆：所有**新增代码 PascalCase**，且
**SukiNative API + 所有用户态暴露函数**回溯改为 PascalCase，其余代码可不动。

---

## 八、外部配置编辑工具（Python + tkinter）

- `tools/registry_editor.py`：SukiRegistry Hive 的**规范参考实现** + tkinter 外部编辑器 +
  `gen-defaults` 子命令。
  - `Crc32`/`RegistryHive` 与内核/C 侧格式完全一致（已回读校验）。
  - `gen-defaults [dest]`：把默认配置写入 `configs/default/{system,user,services}.reg`。
  - `edit <file>`：启动 tkinter GUI（树形浏览/编辑/保存）；tkinter **惰性导入**，无显示环境
    也能跑 `gen-defaults`。
- 默认配置落盘：`configs/default/system.reg`(403B) / `user.reg`(192B) / `services.reg`(201B)。
- 构建集成：`Makefile` 的 `disk` 目标新增 `mmd -i $@ ::SYS/CONFIGS` 并 `mcopy` 上述三文件到
  `::/SYS/CONFIGS/`（已用 `mdir` 确认写入镜像）。

---

## 九、移除 grub 参数传递

- `grub/grub.cfg`：删除 `module2 /boot/display.cfg` 引导模块。
- `Makefile`：`$(ISO)` 依赖与配方中删除 `configs/display.cfg` 拷贝。
- `kernel/kmain.c`：删除 `display_cfg_parse(g_boot.cfg_phys, g_boot.cfg_size)` 调用及注释；
  显示配置改由第三步 registry 覆盖，内置默认值（`display_cfg.c` 的 1280x720）兜底。
  （`display_cfg_parse` 函数保留为可用解析器，未被删除。）

---

## 十、验证（QEMU 无头，生产场景回归零 panic）

构建：`make iso disk`（内核链接成功、合法 Multiboot2；配置已写入 `::SYS/CONFIGS`）。

运行（无头，serial 落盘）：
```
qemu-system-x86_64 -machine pc -cpu qemu64 -smp 1 -m 2G -no-shutdown \
  -drive file=build/disk.img,format=raw,index=0,media=disk \
  -serial file:/tmp/suki2.ser -display none -boot d -cdrom build/SukiOS.iso
```

serial 关键标记（全部出现，无 PANIC/#GP/triple fault）：
```
[boot] security: 权限提升子系统(UAC/授权)预留，本期不实现（见设计文档）；仅保留接入点
[boot] SukiFsServer spawned (pid=4), POSIX file syscalls enabled
[boot] config: loaded system.reg (generation=7)
[boot] config: display 1024x768          <-- registry 覆盖默认 1280x720 成功
[kdr] loading kernel drivers from boot modules...
[boot] stage3: boot animation reserved (not implemented yet)
[boot] SukiInputServer / SukiDisplayServer / SukiMouseServer spawned
[boot] display-server ready (g_display_active=1, ...)
[boot] SukiLogon: reserved; no password configured -> proceed to desktop
[boot] SukiDesktopManager: reserved; launching SukiShell as desktop surrogate
```
- posixtest / nettest 全部 `[PASS]`，DHCP BOUND，无 panic。
- 验证「第三步仅串口」：`g_boot_verbose` 默认 false，第三步日志不刷屏；`-v/--verbose` 分支已
  接通（fbcon_putc），headless 下无法观测屏幕，逻辑与既有 fbcon 路径一致，不引入新崩溃面。

---

## 十一、关键文件清单

| 文件 | 改动 |
| :-- | :-- |
| `kernel/kmain.c` | 四阶段重构、命令行解析、`SecurityReserve`/`BootPlayAnimation`/`SukiLogon`/`SukiDesktopManager`/`Stage3LoadConfigAndKdr`、`ParseBootCmdline`、服务改名、kdr 门控、移除 grub 解析 |
| `kernel/console.c` / `include/kernel/console.h` | `g_boot_verbose` + 第三步日志门控 |
| `include/kernel/multiboot2.h` / `kernel/arch/x86_64/multiboot2.c` | 内核命令行 tag1 解析 → `boot_info_t.cmdline` |
| `include/kernel/registry.h` / `kernel/registry/hive.c` | Hive 解析器（CRC32 + 按路径提取值） |
| `kernel/fs/fd.c` / `include/kernel/vfs.h` | `kern_fs_read_file`（走 FS_PORT 同步读） |
| `include/sukios/kdr.h` | 补 `kdr_load_all` 声明 |
| `tools/registry_editor.py` | Python tkinter 编辑器 + gen-defaults |
| `configs/default/{system,user,services}.reg` | 默认配置（生成） |
| `grub/grub.cfg` / `Makefile` | 移除 display.cfg 模块；disk 目标复制 `::SYS/CONFIGS/*` |
| `results/step79.md` | 本文档 |

---

## 十二、后续（未做，明确延期）

1. **权限提升子系统实装**：完整性级别/能力/授权/调试角色（`suki.debug=1`）——接入 `SecurityReserve()`。
2. **启动动画**实装（详见后续设计）。
3. **SukiLogon / SukiDesktopManager** 实质化：用户数据库/密码、登录页、桌面合成。
4. **kdr 内核 hook 形式**（可选）与配置驱动的磁盘 kdr 列表加载。
5. **SukiNative API + 用户态暴露函数**回溯改为 PascalCase（记忆 45139565）。
6. hive 抗断电：双槽 A/B + 加载时择优（FAT32 无原子 rename）。
