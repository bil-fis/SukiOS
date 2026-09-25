# Step 115 — SukiIsukiDemo 改名为 iSukiDemo 并改写为非内核内嵌的用户态程序

## 1. 目标与背景

用户两项要求：
1. **记录项目策略记忆**：系统的自有功能务必优先使用 **SukiNative** 原生 API，不要使用 POSIX；POSIX 仅作为「兼容已有 Unix/Linux 生态」与「快速移植第三方程序」之用，后续移植其他程序也尽量用 SukiNative。
2. **把 `SukiIsukiDemo` 改名为 `iSukiDemo`，并改写为用户态程序、不再内嵌内核**。

第 1 项已写入记忆（ID `84981044`，标题「SukiNative 优先策略」）。本步骤完成第 2 项：将 `isukidemo` 从「内核 blob 内嵌 + 开机自动 spawn」改为「独立磁盘用户程序 + 由 shell 从 `/BIN/ISUKIDEMO.SKA` 开机拉起」，并在程序内将任务名改为 `iSukiDemo`。

---

## 2. 改动前的形态（问题）

`isukidemo` 此前是**用户态 ELF**，但采取与 `fs_server`/`shell` 等相同的「内嵌内核」模式：
- `Makefile` 把它列入 `USER_PROGS`（第 170 行），由通用规则 `$(BUILD)/user/%.ssvc.blob.o: $(BUILD)/user/%.elf`（Makefile:822）生成 blob `user_isukidemo_start/end[]`，**链入内核镜像**；
- `kernel/kmain.c` 在 `boot_late_init` 用 `task_create_user(user_isukidemo_start, size, "SukiIsukiDemo")` **开机自动 spawn**。

「内嵌内核」即：其二进制作为字节流 blob 编译进 `kernel.elf`，开机由内核直接装载。**用户希望它作为独立用户程序，从磁盘加载，而非塞进内核镜像。**

---

## 3. 重构内容

### 3.1 移除内核内嵌（Makefile + kmain.c）

- **`Makefile:170`**：`USER_PROGS` 列表删除 `isukidemo`。后果：不再生成 `user_isukidemo.ssvc.blob.o`，内核镜像不再包含其二进制。
- **`kernel/kmain.c`**：
  - 删除外部声明 `extern const uint8_t user_isukidemo_start[], user_isukidemo_end[];`（原第 91 行）。
  - 删除开机自动 spawn 块（原第 864–868 行）：
    ```c
    /* iSukiUI 概念稿完整复刻演示：spawn isukidemo（...）。 */
    task_t *sp6 = task_create_user(user_isukidemo_start,
                    (size_t)(user_isukidemo_end - user_isukidemo_start),
                    "SukiIsukiDemo");
    kprintf("[boot-dbg] isukidemo spawn ret=%p\n", (void *)sp6);
    ```

### 3.2 保留为独立用户程序（Makefile 构建/拷贝）

`isukidemo.elf` 本就由**专用显式规则**构建（不依赖 `USER_PROGS`），故移出 `USER_PROGS` 后仍可独立编译：
- `Makefile:346–353`：`$(BUILD)/user/isukidemo.c.o` / `$(BUILD)/user/isukidemo.elf` 显式规则（链接 `$(SUI_OBJS) $(BUILD)/user/gui.c.o $(USER_LIB_OBJS)`，走 libsui 静态链入）。

但 `iso-single`/`disk` 目标的依赖列表原本不含 `isukidemo.elf`（它此前靠 blob 链被间接构建）。移出后必须显式补回依赖，否则磁盘拷贝会找不到文件：
- **`Makefile:1008`**（iso-single 依赖）追加 `$(BUILD)/user/isukidemo.elf`。
- **`Makefile:1056`**（disk 依赖）追加 `$(BUILD)/user/isukidemo.elf`。
- 原磁盘拷贝指令保留：`mcopy -i $@ $(BUILD)/user/isukidemo.elf ::BIN/ISUKIDEMO.SKA`（Makefile:1110）与 ISO 拷贝（Makefile:1043）。

### 3.3 改为从磁盘开机拉起（user/shell.c）

与 `fontsrv`/`pchfnt` 完全一致的方式——由 shell 在 boot 阶段用 `sys_task_spawn` 从磁盘路径拉起，而非内核 blob：
```c
/* iSukiUI 概念稿复刻演示 iSukiDemo：从磁盘 /BIN/ISUKIDEMO.SKA 拉起
 * （不再内嵌内核；headless 下窗口合成 + 自检测经串口落盘，可验证） */
{
    char *iargv[] = { (char*)"/BIN/ISUKIDEMO.SKA", NULL };
    int ipid = sys_task_spawn((char*)"/BIN/ISUKIDEMO.SKA", iargv, NULL);
    if (ipid < 0) shell_out("[shell] warn: iSukiDemo spawn failed\n");
    else          shell_out("[shell] iSukiDemo launched\n");
}
```
放置于 shell 的 boot 拉起段（fontsrv/pchfnt 之后），与字体服务同批启动。

### 3.4 改名为 iSukiDemo（user/apps/isukidemo.c）

- **程序内自命名**：`main` 入口用 `prctl(PR_SET_NAME, "iSukiDemo")` 将任务名从初始的磁盘名 `ISUKIDEMO.SKA` 改为 `iSukiDemo`。实现：
  ```c
  {
      const char *nm = "iSukiDemo";
      suki_syscall2(114 /* SYS_PRCTL */, 15 /* PR_SET_NAME */, (uint64_t)(const void *)nm);
  }
  ```
  - `suki_syscall2` 来自 `user/lib/suki.h`（isukidemo 已 `include "../lib/suki.h"`）。
  - `SYS_PRCTL=114`、`PR_SET_NAME=15` 取自 `include/sukios/posix.h`（与 `posixtest.c:757` 同一机制，内核 `sys_posix.c:695` 实现，从用户指针安全拷名）。
  - **说明**：改名是唯一的 POSIX 调用，属装饰性自命名；程序核心功能（建窗/渲染/控件）仍全部走 SukiNative（`SukiCreateWindow` / `sui_*` / `SukiFlush` 等），符合「SukiNative 优先」策略。当前 SukiNative API 无任务改名接口，故沿用既有的 prctl。
- **文件头注释更新**：字体后端说明改为「中文经 fontsrv 离屏渲染（libsui 委托 SukiFontServer，不再自带 FreeType）」；运行说明改为「开机由 shell 从 `/BIN/ISUKIDEMO.SKA` 拉起（任务名 iSukiDemo）；也可 `exec iSukiDemo` 手动交互」。

---

## 4. 关键技术细节

- **`USER_PROGS` / `USER_BLOBS` 机制**（`Makefile:170,237`）：`USER_PROGS` 经 `$(patsubst %,$(BUILD)/user/%.ssvc.blob.o,$(USER_PROGS))` 生成内核可见的 blob，是「内嵌内核」的唯一来源；移出即解除内嵌。
- **`sys_task_spawn(path, argv, NULL)`**（shell 调用）：从磁盘路径装载 ELF，内核按路径派生初始任务名（`ISUKIDEMO.SKA`）。`prctl` 在 `main` 中随后改名。
- **`run-headless` 启动形态**（`Makefile:1167`）：`-boot d -cdrom $(ISO)`（最小 ISO，仅内核+grub，无运行文件）**+ 挂接 `$(QEMU_DISK)=disk.img`**。内核启动后把 `disk.img` 作为根 `/`，故 `/BIN/*.SKA` 可达——fontsrv/pchfnt 已验证可从此盘拉起，isukidemo 同理。
- **磁盘文件名大小写**：盘内文件 `ISUKIDEMO.SKA`（FAT 大小写不敏感），shell 用 `/BIN/ISUKIDEMO.SKA` 拉起；用户在图形 shell 也可 `exec iSukiDemo`（FAT 不敏感解析到同一文件）。

---

## 5. 验证（headless QEMU 生产回归）

命令：
```
make iso disk            # 确认内核/shell/isukidemo 编译 + 磁盘镜像含 ::BIN/ISUKIDEMO.SKA
make run-headless QEMU_SERIAL="-serial file:/tmp/sui6.log" RUN_TIMEOUT=60
```
日志关键结论（`/tmp/sui6.log`）：
```
[syscall] spawn: parent pid=11 -> child 'ISUKIDEMO.SKA' pid=22      # shell 从磁盘拉起
[shell] iSukiDemo launched
[isukidemo] start (iSukiUI concept replica)                         # 程序运行（prctl 改名前）
[isukidemo] window rendered + flushed (all pages + dialog self-checked) -> PASS
[isukidemo] controls: button/input/textarea/.../toast OK
[isukidemo] lifecycle complete, exiting
[syscall] task 'iSukiDemo' pid=22 exit(code=0)                      # 改名生效：退出名 iSukiDemo
[sched] task 'iSukiDemo' pid=22 exited (code=0)
```
- `iSukiDemo` 已**不再内嵌内核**：由 shell 经 `sys_task_spawn("/BIN/ISUKIDEMO.SKA")` 从磁盘拉起（pid=22，父为 shell pid=11），初始名 `ISUKIDEMO.SKA`，`prctl` 改名后退出名为 `iSukiDemo`。
- 端到端通过：`window rendered + flushed -> PASS`、`controls: ... OK`、`exit(code=0)`。
- `winhello` / `suikitest` / `pchfnt` / `fontsrv` 均正常；**零 panic**。
- 仅有的 `#PF` 为既有 `SukiPosixTest`（pid=12，`cr2=0xfffffffffffffffe`，即 `-2` 指针，`rip=0x402e94`）——属 POSIX 兼容自测里某未实现 syscall 返回 `-ENOSYS` 被解引用的既有用例，不依赖 libsui/字体后端、不在本步骤改动列表，**与本次重构无关**。

---

## 6. 改动文件清单

- `Makefile` — `USER_PROGS` 移除 `isukidemo`；`iso-single`/`disk` 目标补 `$(BUILD)/user/isukidemo.elf` 依赖；更新两处 isukidemo 注释。
- `kernel/kmain.c` — 删除 `user_isukidemo` 外部声明与开机自动 spawn 块。
- `user/shell.c` — boot 阶段从磁盘 `/BIN/ISUKIDEMO.SKA` 拉起 `iSukiDemo`。
- `user/apps/isukidemo.c` — 文件头注释更新；`main` 入口 `prctl(PR_SET_NAME,"iSukiDemo")` 自命名。

---

## 7. 结论

- `SukiIsukiDemo` 已改名为 **`iSukiDemo`**，并改写为**独立磁盘用户程序**：不再编译进内核镜像（移出 `USER_PROGS` blob），改由 shell 从 `/BIN/ISUKIDEMO.SKA` 开机拉起，核心功能仍走 SukiNative。
- headless 回归确认：iSukiDemo 从磁盘拉起、改名生效、渲染/控件自检 PASS、`exit(0)`、零 panic。
- 项目策略记忆「SukiNative 优先」已建立（ID `84981044`）：自有功能优先 SukiNative，POSIX 仅作兼容/快速移植。
