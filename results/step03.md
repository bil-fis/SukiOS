# Step03：修复 `reboot` 使 QEMU 进入 paused 状态

## 现象
在 shell 中执行 `reboot` 命令后，QEMU 不是真正重启机器，而是进入 **paused（暂停）** 状态，需手动 `cont` 或退出。

## 根因分析
`reboot` 命令路径：
- 用户态 `user/shell.c` 执行 `suki_syscall5(SYS_REBOOT, ...)` → 内核 `kernel/syscall/syscall.c::sys_reboot()`
- `sys_reboot()` 通过 8042 键盘控制器发复位脉冲：`outb(0x64, 0xFE)`（Pulse Output Port B / CPU RESET），随后 `hlt` 等待复位。

8042 0xFE 脉冲是 x86 标准复位手段，QEMU 的 `pc` 机器（i440FX/PIIX3）支持该脉冲并应触发机器重置。问题不在内核，而在 **QEMU 启动参数**：

`Makefile` 中原 `QEMU_FLAGS` 为：
```
QEMU_FLAGS := -machine pc -cpu qemu64 -m 2G -no-reboot -no-shutdown
```
- `-no-reboot`：收到复位信号时不执行重启，而当作“关机”处理
- `-no-shutdown`：关机时不退出 QEMU，而是停止（paused）虚拟机

两者组合后，内核发出的 8042 复位脉冲被 QEMU 捕获 → 因 `-no-reboot` 不重启 → 因 `-no-shutdown` 不退出 → 虚拟机停在 paused 状态。这正是观察到的现象。

## 修复
`Makefile` 第 63 行移除 `-no-reboot`：
```diff
-QEMU_FLAGS  := -machine pc -cpu qemu64 -m 2G -no-reboot -no-shutdown
+# 注意：-no-reboot 会让 QEMU 把 8042 复位脉冲当作"关机"处理，配合
+#       -no-shutdown 会进入 paused 状态而非真正重启。故移除 -no-reboot，
+#       使内核的 sys_reboot()（8042 0xFE 脉冲）可触发真正的机器重启。
+QEMU_FLAGS  := -machine pc -cpu qemu64 -m 2G -no-shutdown
```
`run` / `run-headless` / `debug` 三个 target 均引用 `QEMU_FLAGS`，故一处修改即生效。保留 `-no-shutdown` 不影响重启（仅作用于本系统未实现的“关机/退出”路径），且可避免意外退出。

## 验证
无头启动，串口写入文件，键入 `reboot<ret>`，随后再键入 `quit<ret>`：
- 第 51 行 `rebooting...`
- 第 54 行立即出现第二次 `[boot] SukiOS kernel entered (long mode, higher half).`
- 第 94 行 `[shell] SukiOS shell online` 重新上线（确认全新启动完成）
- 重启后 `quit` 命令照常执行，QEMU 正常退出

结论：`reboot` 现已能真正重启机器，QEMU 不再 paused。内核 `sys_reboot()` 实现本身无需改动。

## 残留说明
- 系统尚未实现 ACPI 关机（`poweroff`/S5）命令；`-no-shutdown` 保留以便在未来实现关机路径时，QEMU 不会立即退出，便于调试。
- 三重启（triple fault）时因已移除 `-no-reboot`，QEMU 会直接重启而非暂停，调试三重故障时可临时加回 `-no-reboot -no-shutdown` 或用 `-d int` 日志定位。
