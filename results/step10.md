# Step 10 — 根治 MP3 播放"加速 + 前后颠倒乱序"：file_off 双重推进 Bug（附完整取证过程）

> 日期：2026-07-27
> 状态：根因 100% 锁定（主机逐位复现，CRC 铁证），修复已落地 `user/apps/playaudio.c`
> 验证环境：Linux(WSL2) + QEMU（KVM，`-machine pc,accel=kvm -cpu host`），`-device intel-hda -device hda-duplex`

---

## 1. 问题现象（用户报告）

Step09 修复环缓冲流量控制后，用户反馈问题依旧：

> "还是存在这个问题，速度加快，然后听到的内容像是音频数据前后颠倒乱序播放了那样"

源文件确认（ffmpeg）：`moonhalo.mp3`，48000 Hz，stereo，fltp，192 kb/s，时长 **00:03:24.05**（204 秒）。

## 2. 排查路径（分层隔离取证）

本次采用"逐层隔离 + CRC 取证"方法，按数据通路从下游到上游逐层排除：

### 2.1 第一层：HDA 驱动/DMA 链路 —— 排除 ✅

- **手段**：新建独立测试程序 `user/apps/audiotest.c`（440Hz 正弦，运行时自建 1024 点正弦表避免 libm 依赖，48000/2/16，6 秒），配合 QEMU WAV 后端（`-audiodev wav,id=snd0,path=/tmp/cap.wav`）录制实际送达"声卡"的 PCM。
- **分析**：Goertzel 频谱检测捕获 WAV，主频 422 Hz（理论 421.875 Hz，整数相位累加截断所致，完全吻合），谐波 < -40 dB。
- **结论**：HDA 驱动 → BDL DMA → QEMU 声卡链路**纯净无失真、无乱序**，PCM 逐字节忠实播出。Step09 的流量控制修复有效。

### 2.2 第二层：FS 读取路径 —— 排除 ✅

- **手段**：在 `fs_read_at()` 拷贝出口累积原始输入字节的 CRC32（IEEE 0xEDB88320），周期打印 `rawCRC`。
- **对照**：主机端 C 程序按相同偏移读取同一文件计算 CRC。
- **QEMU 实测**：`frames=1024 rawCRC=1497523496 fileoff=3327430` —— 与主机按**相同的（含 bug 的）读取序列**模拟的结果逐位一致。
- **结论**：FS_SERVER（FAT32 + 预读缓存 + DISK IPC）交付的每一个字节都正确。

### 2.3 第三层：minimp3 解码输出 —— 发现异常 🔴

在 Ring3 累积解码 PCM 的 CRC32，与主机端同一 minimp3（同宏 `MINIMP3_ONLY_MP3 + MINIMP3_NO_SIMD`）、同 ID3 跳过量（1986510）的参考解码比对：

| 检查点 | QEMU 实机（sukios6.log） | 主机参考解码 | 一致？ |
|---|---|---|---|
| frames=1024 PCM CRC | 995342451 | 3012670116 | ❌ |
| frames=2048 PCM CRC | 2386867850 | 983452316 | ❌ |

**Ring3 解出的 PCM 与正确结果不符** —— 但 FS 字节正确、FPU/SSE 上下文正常（正弦测试用同样的用户态浮点路径无异常）。唯一剩下的解释：**送入解码器的字节流序列本身错了**。

## 3. 根因：`file_off` 游标双重推进（读取跳字节）

`user/apps/playaudio.c` 的流式读取采用 32 KiB 滑动窗口，存在两处对 `file_off` 的推进：

**填充阶段**（正确）—— 游标推到"缓冲区尾部对应的文件位置"：

```c
while (in_filled + FS_DATA_MAX <= INBUF_SIZE && !eof) {
    uint32_t n = fs_read_at(fname, file_off, FS_DATA_MAX, g_inbuf + in_filled);
    in_filled += n;
    file_off  += n;          /* 游标 = 缓冲尾部的文件偏移 */
}
```

**消费阶段**（Bug）—— 消费缓冲区**内部**已在内存中的字节时，**再次**推进游标：

```c
/* 旧代码（错误） */
if (consumed >= in_filled) {
    file_off += consumed;            /* 部分场景多加了 in_filled */
    in_filled = 0;
} else {
    memmove(g_inbuf, g_inbuf + consumed, in_filled - consumed);
    in_filled -= consumed;
    file_off  += consumed;           /* ← 致命：重复推进！ */
}
```

### 3.1 错误机理推演

游标不变式应为：**`file_off` == 下一次 `fs_read_at` 应读取的文件偏移 == 缓冲区尾部对应的文件位置**。

违反后每解一帧（576 B @192kbps/48kHz）游标多走 576 B → 下次补读**跳过 576 B 的真实文件数据**。宏观效果：

1. **加速**：数据流推进速度 ≈ (3500 + 576) / 3500 … 实测综合 **2.24×**（91 s 播完 204 s 的歌）；
2. **前后颠倒乱序感**：跳过的 576 B 使后续 MP3 帧被拦腰截断 → minimp3 丢弃残帧、向后搜索下一个同步字（`FF FB`）重新对齐 → 解出的相邻两帧在时间轴上并不相邻，且 bit reservoir（跨帧主数据回指）失效造成频谱残渣，听感即"内容前后颠倒、乱序拼接"。

### 3.2 铁证：主机端逐位复现

编写 `/tmp/host_sim.c`：主机上用**同一 minimp3、同一 3500 B 分块尺寸、同一 32 KiB 窗口**，可切换"buggy（复现双重推进）/ fixed（正确游标）"两种模式：

```text
=== BUGGY（复现 SukiOS 旧逻辑）===
bench CRC=0          rawCRC=558089543   fileoff=2018594 frames=0
bench CRC=995342451  rawCRC=1497523496  fileoff=3327430 frames=1024   ← 与 QEMU 日志逐位一致
bench CRC=2386867850 rawCRC=4155228618  fileoff=4657126 frames=2048   ← 与 QEMU 日志逐位一致
done frames=3825 samples/ch=4406400 (~91s @48k)                        ← 204s 的歌只剩 91s = 2.24× 加速

=== FIXED（修正游标）===
done frames=8505 samples/ch=9797760 (~204s @48k) CRC=699861505         ← 时长精确 = ffmpeg 的 3:24
```

三重吻合：
1. buggy 模式 frames=1024/2048 的 **PCM CRC 与 rawCRC 均与 QEMU 实机日志完全一致** —— 证明实机行为被逐位复现，且 FS 交付字节正确；
2. buggy 模式总时长 91 s，加速比 204/91 = **2.24×** —— 与听感"加速"吻合；
3. fixed 模式总时长 **204 s、全曲 PCM CRC=699861505 与主机参考解码一致** —— 证明修正后解码输出恢复正确。

## 4. 修复

```c
/* user/apps/playaudio.c —— 消费阶段（新） */
uint32_t consumed = (uint32_t)info.frame_bytes;
if (consumed == 0) {
    consumed = 1;                        /* 无同步头：跳 1 字节重对齐 */
}
if (consumed >= in_filled) {
    file_off += consumed - in_filled;    /* 只跳过"缓冲之外"的部分（如 1.9MB ID3 标签） */
    in_filled = 0;
} else {
    memmove(g_inbuf, g_inbuf + consumed, in_filled - consumed);
    in_filled -= consumed;
    /* file_off 不动：消费的是已在内存中的数据 */
}
```

要点：
- 消费缓冲**内部**字节：`file_off` 严禁推进（数据早已读入内存）；
- 消费量**超出**缓冲（minimp3 要求跳过大段元数据，如 ~1.9 MB ID3v2 标签）：只把"超出缓冲的差额" `consumed - in_filled` 加到游标上（旧代码此处加了完整 `consumed`，也多跳了 `in_filled` 字节）；
- 代码内新增"游标不变式"注释块，防止回归。

### 4.1 附带清理（同文件）

- `bench` 还原为正常参数判定：`bool bench = (argc > 2 && u_strcmp(argv[2], "bench") == 0);`（此前为绕过 exec 传参 bug 临时硬编码 `true`）；
- 删除全部临时取证代码：`g_raw_crc` 累积与打印、`g_first_pcm` 首 16 采样打印、首 32 字节 hex dump、`decode#N` 逐帧调试打印；
- 保留 `g_crc`（bench 模式 PCM CRC32），注释标注全曲期望值 **699861505**，作为长期回归基准。

## 5. 关键常量 / 数据

| 项 | 值 |
|---|---|
| 源文件 | moonhalo.mp3，48000 Hz / stereo / 192 kbps / 204 s |
| ID3v2 标签长（syncsafe） | 1986510 B（跳过后首同步字 `FF FB`） |
| MP3 帧长 | 576 B（144×192000/48000），每帧 1152 样本/声道 |
| 输入窗口 / FS 单次读 | INBUF_SIZE=32768 B / FS_DATA_MAX=3500 B |
| 全曲正确 PCM CRC32（IEEE 0xEDB88320） | **699861505**（8505 帧，9797760 样本/声道） |
| buggy 加速比 | 2.24×（4406400/9797760 样本，91 s/204 s） |

## 6. 修改文件清单

| 文件 | 改动 |
|---|---|
| `user/apps/playaudio.c` | 修正 file_off 游标不变式（核心修复）；bench 还原 argv 判定；清除全部临时取证代码；保留 CRC 回归基准 |
| `user/apps/audiotest.c` | 新增：440Hz 正弦 HDA 链路隔离测试程序（保留作回归工具） |
| `Makefile` | `APP_PROGS` 加入 `audiotest`；`APP_CFLAGS` 含 `-msse -msse2` |

## 7. 验证方式

```bash
make iso disk
# 正常听声验证（KVM + PulseAudio）：
make run
#   shell 内：exec bin/playaudio        → 应全曲 204s 连贯播放，无加速无乱序
# WAV 取证验证（录下实际送声卡的 PCM）：
qemu-system-x86_64 -machine pc,accel=kvm -cpu host -m 2G -no-shutdown \
  -serial file:/tmp/sukios.log -display none \
  -audiodev wav,id=snd0,path=/tmp/cap.wav \
  -device intel-hda -device hda-duplex,audiodev=snd0 \
  -boot d -cdrom build/SukiOS.iso \
  -drive file=build/disk.img,format=raw,index=0,media=disk
# 解码正确性回归（无声，纯 CRC）：
#   shell 内：exec bin/playaudio MOONHALO.MP3 bench
#   期望串口输出：BENCH CRC32 of decoded PCM = 699861505
# HDA 链路回归：
#   shell 内：exec bin/audiotest       → WAV 应为干净 440Hz 正弦（谐波 < -40dB）
```

## 8. 已知遗留问题（未在本步修复，已录入 step10_todos.md）

1. **QEMU audiodev 强制 44100 Hz**：QEMU 音频后端默认 `fixed-settings=on`，AUD voice 恒为 44100，无论 SD_FMT 写 0x11(48k) 还是 0x811(96k)。QEMU 内部会做重采样故**不影响音准**，仅 WAV 录制头恒为 44100；真机无此问题。可用 `-audiodev pa,id=snd0,out.fixed-settings=off` 关闭。
2. **`sys_audio_stop` 后残留循环**：audiotest 实测 6 s 正弦录出 21.94 s WAV，停流后 BDL 环内残留数据疑似继续循环（RUN=0 时机/wav 后端缓冲特性），需进一步在驱动 stop 路径做静音清环 + 等待 DMA 停止确认。
3. **exec 传参 argv 错乱**：`exec BIN/playaudio MOONHALO.MP3 bench` 曾解析出 `file = 3`，shell→spawn→elf_build_stack 传参链路存在 bug，需专项排查。

## 9. 经验教训

1. **滑动窗口读取的游标必须有书面不变式**："`file_off` == 缓冲尾部的文件偏移"这一约定必须写进注释；填充与消费两处对同一游标的操作分离在 80 行之外，靠肉眼极难发现双重推进。
2. **CRC 逐层取证是定位数据通路 bug 的最强手段**：本 bug 前两轮（step09 流控、采样率怀疑）都在"猜"，而 rawCRC/PCM-CRC 两级校验一次性把故障面从"驱动+FS+解码器+FPU"收窄到 20 行游标逻辑。
3. **主机端可复现模拟 = 铁证**：把嵌入式环境的 I/O 模式（分块尺寸、窗口大小、游标逻辑）在主机逐位复刻，CRC 全对即证明"实机行为已被完全理解"，修复才有把握。
4. **听感描述可量化**："加速"= 样本总数比 2.24×；"前后颠倒"= 帧截断后重同步造成的时间轴跳变。先量化再修，避免误修无关层。
