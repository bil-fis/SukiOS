# SukiOS 构建结果报告 — Step 01

> 完成时间：2026-07-26
> 范围：阶段一 ~ 阶段九，最小可运行混合内核系统全部完成，均经 QEMU 实机验证。

---

## 一、总体成果

SukiOS 是一个 x86_64 混合内核（Mach 风格微内核 + 必要内核态驱动）操作系统。
本次交付完成了从 GRUB 引导到 Ring3 用户态 Shell 的完整启动链路，所有核心子系统
均在 QEMU 中实测通过。

一键运行：

```bash
make run            # 图形窗口运行（含 FAT32 磁盘），直接拉起可启动 SukiOS
make run-headless   # 串口无头运行（自动化验证用）
make debug          # qemu -s -S 等待 GDB 连接
```

---

## 二、分阶段验证结果

| 阶段 | 需求 | 验证证据 |
|---|---|---|
| 一 | GRUB Multiboot2 引导（BIOS/UEFI ISO） | `grub-file --is-x86-multiboot2` 通过，`grub-mkrescue` 生成双固件 ISO |
| 二 | 高地址内核 `0xFFFF800000000000` | `kmain @ 0xffff8000001xxxxx`，用户态低地址 `0x400000` |
| 三 | 帧缓冲图形终端 + 开机 Logo | 1024×768×32 @ `0xFD000000`，像素级校验字形/Logo；无 FB 时回退 VGA 文本 `0xB8000` |
| 四 | PIT + PS/2 中断 | 100Hz 节拍驱动抢占；键盘 IRQ1 采集原始扫描码 |
| 五 | 抢占式轮转调度 | 4 worker 任务完美交错（`4→3→2→1→4...`），修复新任务关中断不可抢占的 bug |
| 六 | syscall ABI | `LSTAR` 入口、`0x08/0x10/0x1B/0x23` 选择子红线、`copy_from_user` 拦截内核指针攻击 |
| 七 | Mach IPC | 小消息双拷贝 + OOL 零拷贝实测（同一物理页双映射校验一致） |
| 八 | 混合内核红线 | 仅 ATA PIO 驻留内核（DISK_PORT）；FAT32、键盘解析、Shell 全部 Ring3 |
| 九 | Ring3 Shell 四命令 | `ls`（根目录 3 文件 + 1 目录）、`cat HELLO.TXT`、`help`、`reboot`（8042 复位，日志确认二次启动） |

---

## 三、交互链路（全 IPC）

```
键盘 IRQ(内核, 仅采集) → sys_input_read → INPUT_SERVER(Ring3 解析)
  → mach_msg → SHELL_PORT → shell(Ring3) → mach_msg → FS_PORT
  → FS_SERVER(Ring3 FAT32) → mach_msg → DISK_PORT → 内核 ATA → 原路返回
```

---

## 四、本次变更（Makefile）

- `run` / `run-headless` / `debug` 目标统一加入 `-boot d`，强制从光驱（ISO）引导。
  原因：FAT32 磁盘位于 `index=0`（第一硬盘）且无引导扇区，`-machine pc` 默认会
  优先尝试硬盘启动而导致无法引导。加入 `-boot d` 后 `make run` 可直接拉起一个
  完整可启动的 SukiOS 模拟环境。

---

## 五、已知简化项（后续迭代方向）

1. **sysret 未用**：手册选择子顺序（CS=0x1B 在 DS=0x23 前）与 `sysretq` 选择子算术
   不兼容，返回用户态改用 `iretq`（完全符合选择子红线，性能略低）。
2. 用户程序为平坦二进制（RWX 混排），后续引入 ELF 装载器实现 W^X。
3. `copy_from_user` 缺页时 panic 而非 EFAULT 恢复；OOL 窗口为全局 bump 分配。
4. DISPLAY_SERVER 合成器、磁盘写入、VGA 组合键热切换为下一里程碑。
