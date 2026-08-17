# SukiOS 阶段汇总报告 — Step 05 ~ Step 10

> 汇总时间：2026-07-27
> 覆盖范围：ELF 加载器与 execve（Step05），spawn/wait 进程模型（Step06），Intel HDA 音频驱动与 MP3 播放全链路（Step07~Step10）。
> 环境：Linux(WSL2) 开发，QEMU 验证（i440FX `-machine pc`；有 `/dev/kvm` 时自动 KVM + `-cpu host`，否则 TCG + `qemu64`）。
> 前四阶段（引导→Shell→加固→Unicode 控制台）见 `results/step1-4_summary.md`。

---

# 零、本阶段总览（一句话速览）

**Step05~06** 让 SukiOS 拥有了真正的进程模型：ELF64 装载（W^X、System V ABI 初始栈）、execve、类 Unix spawn+wait，Shell 可反复运行外部程序并回到提示符。
**Step07~10** 打通并根治了完整的音频通路：PCI 枚举 → Intel HDA 驱动（CORB/RIRB、BDL DMA、流控自校正）→ Ring3 minimp3 解码 → 实时连贯正确播放 MP3。

新增子系统与文件：

| 子系统 | 文件 |
|---|---|
| ELF 加载器 | `kernel/elf/elf.c`、`include/kernel/elf.h` |
| PCI 枚举 | `kernel/drivers/pci.c`、`include/kernel/pci.h` |
| Intel HDA 驱动 | `kernel/drivers/hda.c`、`include/kernel/hda.h` |
| 用户 app 框架 | `user/apps/`（hello / playaudio / audiotest）、`user/lib/shims/` |
| MP3 解码 | `minimp3/`（第三方，MINIMP3_ONLY_MP3 + NO_SIMD） |

系统调用表扩展至：`0=SYS_MACH_MSG, 1=SYS_TASK_SPAWN, 2=SYS_TASK_EXIT, 3=SYS_YIELD, 4=SYS_DEBUG_WRITE, 5=SYS_INPUT_READ, 6=SYS_REBOOT, 7=SYS_PORT_CLAIM, 8=SYS_EXECVE, 9=SYS_WAIT`，另有 SYS_AUDIO_OPEN/WRITE/QUEUED/STOP 音频组。

---

# 一、Step 05 — ELF 加载器 / execve 全链路（三个致命 syscall 入口 Bug）

## 1.1 成果

Shell 键入 `exec BIN/HELLO.ELF a b`：内核经 FS IPC 从 FAT32 读出 ELF64 → `elf_load` 建新地址空间（W^X）→ 替换进程映像 → 新程序以 argc=3、argv 正确、16 字节对齐初始栈从 `_start` 运行。

## 1.2 修复的四个 Bug（syscall 入口寄存器纪律）

1. **RFLAGS 计算破坏 rax（调用号）**：`syscall_entry.S` 构 iretq 帧时借用 rax 计算 RFLAGS → `unknown syscall 582`（0x246 = 用户 RFLAGS）。改为直接 `orq $0x200, %r11; pushq %r11`。
2. **execve 帧改写破坏 rax（返回值）**：返回路径用 rax 作暂存覆盖了 syscall 返回值。改用 rcx/r11（iretq 前本就是自由 scratch）。
3. **全局 scratch 并发覆盖 → 用户态 #PF**：`g_user_rip/rsp_scratch` 是单例全局，任务 A 睡在 `mach_msg_recv` 时被任务 B 的 syscall 覆盖。改为**每任务** `task_t.scr_rip/scr_rsp` + 全局指针 `g_scratch`（三处上下文切换点同步）。铁律：**任何"每进程状态"绝不允许放单例全局**。
4. **elf_build_stack argc/argv 空洞**：中途 16 字节对齐在 argc 与 argv[] 之间插入 8B 空洞 → argv 野指针。改为预算指针块总大小、预对齐 `sp_final`、自底向上零空隙连续写（System V AMD64 ABI 3.4.1）。

## 1.3 关键约定

`syscall` 指令：rcx=用户RIP、r11=用户RFLAGS、rax=调用号/返回值；AT&T 的 `movq sym(%rip),%reg` 取变量值而非解引用，字段访问必须两步。

---

# 二、Step 06 — spawn + wait 进程模型（exec 后回不到 Shell 的根治）

## 2.1 根因

`sys_execve` 语义是"替换当前进程映像"，Shell 用它等于自杀；子程序 exit 后不存在可返回的 Shell 任务。缺 fork/spawn 原语。

## 2.2 方案

新增 `SYS_TASK_SPAWN=1`（新建独立 Ring3 子任务装载 ELF，返回子 PID）与 `SYS_WAIT=9`（阻塞等子任务退出取退出码）。Shell 的 `exec` 命令改为 spawn + wait。

## 2.3 实现要点

- `task_t` 新增 `parent_id`（PID 而非指针，防悬空）、`waiters/wait_link`（等待者链）、`exit_code/zombie/wait_result`、`all_next`（全局任务表 `g_all_tasks`）。
- `task_exit_current(code)`：先把退出码**拷入每个等待者自身的 `wait_result`** 再释放子结构 → 父读自身字段，无 use-after-free。
- `task_reap()`：首次 wait 命中 zombie 立即回收；二次 wait 同 PID 返回 -1（不死锁）；未被 wait 的僵尸由 `reap_dead()` 兜底（不泄漏）。
- `path/argv/envp` 一律 `copy_from_user`；`schedule()` 关中断下调用。

验证：连续 `exec`、不存在文件的优雅失败、`ls`/`cat` 交替，全程无 #PF/panic，每次都回到 `SukiOS>` 提示符。

---

# 三、Step 07 — HDA 初始化打通 + minimp3 samples=0 + FS 流式读取优化

## 3.1 HDA verb 超时（QEMU intel-hda 三个建模怪癖）

对照 QEMU `hw/audio/intel-hda.c` 源码定位：

1. **rirb_cnt 闸门**：QEMU 把偏移 0x5A 建模为 RINTCNT（而非规范的 RIRBRP），`rirb_count == rirb_cnt(默认0)` 时 CORB 引擎**永不运行**。修复：初始化写 `REG_RINTCNT=0x00FF`。
2. **CORB 槽 off-by-one**：QEMU 从 `(corb_rp+1)` 槽读 verb，首条必须写槽 1 而非槽 0。
3. **RIRB 槽 off-by-one**（对称）：响应写在 `(rirb_wp+1)` 槽。

修复后：`codec cad=0 vendor/device=1af40022`，AFG/DAC/PIN 拓扑枚举成功，输出引擎就绪（128 KiB BDL 环，轮询无中断）。**注意真机 0x5A 是 RIRBRP，移植时需按规范改回。**

## 3.2 minimp3 samples=0

MP3 文件头含 1986510 B 的 ID3v2 标签，minimp3 核心 API 不自动跳过。修复：读前 10 字节，按 syncsafe 7-bit 编码算标签长，直接跳 `file_off`。

## 3.3 FS 流式读取性能

`fat_next()` 逐簇 PIO（3877 簇 = 3877 次读同一批 FAT 扇区）→ 加 1 扇区 FAT 缓存 + 连续簇段合并读取，walk PIO 从 ~3877 降到 ~30。

---

# 四、Step 08 — 磁盘协议截断 + FS 预读缓存 + KVM 加速

1. **DISK_MAX_SECTORS=64 违反内联 IPC 上限（致命）**：磁盘应答是内联消息，受 `MACH_MSG_INLINE_MAX=3968` 硬限制；64×512 远超 → `ipc_send_kernel` 拒发 → 客户端从全零 BSS 拷数据，**静默截断**（仅首扇区残余 50 字节正确，与 1986510%512=462 精确吻合）。回滚为 7 并注释"协议硬约束，严禁调大"。
2. **顺序读缓存永远失效**：簇指针按整 run 推进但单次只交付 3500B → 下次请求条件不成立，每次从簇链头重新遍历。改为按实际消费推进 + 32KB 预读窗口（`disk_read_batched` 按 7 扇区分批填充）。
3. **TCG 浮点太慢（~1/8 实时）**：Makefile 自动探测 `/dev/kvm`，KVM 下 `-machine pc,accel=kvm -cpu host`，解码远超实时。

教训：**协议常量不可孤立调整**（DISK_MAX_SECTORS 与 MACH_MSG_INLINE_MAX 隐式耦合，截断是静默的）。

---

# 五、Step 09 — HDA 环缓冲流量控制自校正

旧驱动用增量累计（`g_queued` / `g_lpib_last` 差分扣减）做流控；QEMU 的 LPIB 在流复位/环回/定时器重置时会**回跳**，差分把回跳误算成"绕整圈的大消费" → free 暴涨 → 写指针追上读指针覆盖未播数据；且 free 为无符号减法，可下溢成极大值（正反馈）。

修复：改为**读写指针直接求差**的绝对自校正——`hda_queued_bytes() = (g_wr_ofs + BUF - lpib) % BUF`，`hda_free_space()` 恒在 `[0, BUF)` 不可能下溢；保护间隙 4KB→8KB。任何 LPIB 异常只会让写入变保守，绝不越界覆盖。

---

# 六、Step 10 — 根治"加速 + 前后颠倒乱序"：file_off 双重推进

## 6.1 取证方法（本阶段方法论最大产出）

分层隔离 + CRC 逐位取证：

1. **HDA 链路**：新工具 `user/apps/audiotest.c`（440Hz 正弦）+ QEMU WAV 后端录制 → Goertzel 频谱干净（谐波<-40dB）→ 排除驱动。
2. **FS 路径**：Ring3 累积原始输入字节 rawCRC ↔ 主机同偏移 CRC → 逐位一致 → 排除 FS。
3. **解码输出**：Ring3 PCM CRC ≠ 主机参考解码 CRC → 锁定"送入解码器的字节序列错了"。
4. **主机逐位复现**：`/tmp/host_sim.c` 用同一 minimp3/分块/窗口，buggy 模式的 PCM CRC 与 rawCRC 均与 QEMU 日志**逐位一致** → 铁证。

## 6.2 根因

`playaudio.c` 滑动窗口的 `file_off` 游标**双重推进**：填充时已推到缓冲尾部，消费缓冲内部字节时又 `file_off += consumed` → 每帧跳过 576B 文件数据 → 流以 2.24× 推进（204s 歌 91s 播完），帧被拦腰截断后重同步 → "加速 + 前后颠倒乱序"。

## 6.3 修复与回归基准

- 游标不变式：`file_off` == 缓冲尾部文件偏移；消费缓冲内部字节游标不动；`consumed >= in_filled` 时只加差额 `consumed - in_filled`。
- 长期回归基准：`exec bin/playaudio MOONHALO.MP3 bench` 期望 **CRC=699861505**（8505 帧 / 9797760 样本/声道 / 204s）。
- 遗留：QEMU audiodev 默认强制 44100（fixed-settings，不影响音准）、`sys_audio_stop` 残留循环、exec 传参 argv 错乱 —— 已录入 `step10_todos.md`。

---

# 七、六阶段累计能力边界

**新增已实现**（相对 Step04）：
ELF64 装载器（W^X、ABI 初始栈、auxv）、execve、spawn+wait 进程模型（全局任务表/zombie/reap）、每任务 syscall scratch、PCI 总线枚举、Intel HDA 驱动（CORB/RIRB verb、codec 拓扑枚举、BDL DMA、自校正流控、背压）、音频 syscall 组、Ring3 minimp3 实时 MP3 解码播放、FS 预读缓存与连续簇合并、KVM 自动加速、CRC 取证回归基准。

**尚未实现 / 遗留**（详见 `step10_todos.md`）：
SMP、KPTI/SMAP、代码段 ASLR（需 PIE）、FAT32 写路径、信号/定时器 syscall、网络栈、Display Server/GUI、audio stop 残留、exec argv bug、真机 HDA 兼容（0x5A 语义）等。

---

# 八、复现与调试命令速查

```bash
make iso disk                 # 构建（自动收集 kernel/*.c；app 见 APP_PROGS）
make run                      # KVM(若有 /dev/kvm) + PulseAudio 出声
make run QEMU_AUDIODRV=alsa   # ALSA 后端
make run QEMU_ACCEL=tcg       # 强制 TCG（浮点慢 ~10x，仅调试）

# WAV 取证（录实际送声卡的 PCM）
qemu-system-x86_64 -machine pc,accel=kvm -cpu host -m 2G -no-shutdown \
  -serial file:/tmp/sukios.log -display none \
  -audiodev wav,id=snd0,path=/tmp/cap.wav \
  -device intel-hda -device hda-duplex,audiodev=snd0 \
  -boot d -cdrom build/SukiOS.iso \
  -drive file=build/disk.img,format=raw,index=0,media=disk

# shell 内回归序列
exec BIN/HELLO.ELF a b                  # spawn+wait+argv
exec bin/playaudio                      # 全曲 204s 连贯播放
exec bin/playaudio MOONHALO.MP3 bench   # 解码 CRC 回归：期望 699861505
exec bin/audiotest                      # HDA 链路：干净 440Hz 正弦
```

---

# 九、六阶段经验教训（Top 6）

1. **syscall 入口寄存器纪律**：rax/rcx/r11 每条指令前后都要明确"谁持有什么"；每进程状态绝不放单例全局。
2. **协议常量成对审查**：调整任何容量宏前必须核对整条消息路径的上限（内联 IPC 3968B 教训）。
3. **滑动窗口游标要有书面不变式**：填充/消费分离百行之外，双重推进靠肉眼看不出来。
4. **CRC 逐层取证 > 猜测**：两级 CRC（原始字节 + 解码输出）一次把故障面从四个子系统收窄到 20 行。
5. **主机逐位复现是修复信心的来源**：I/O 模式（分块/窗口/游标）完整复刻后 CRC 全对，才算"完全理解了实机行为"。
6. **仿真怪癖要与规范区分标注**：QEMU intel-hda 的 0x5A/槽位 off-by-one 已注释"真机需按规范处理"，避免未来移植踩坑。
