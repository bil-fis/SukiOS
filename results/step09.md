# Step09 — 修复 HDA 播放"加速/抽帧/一顿一顿"：环缓冲流量控制自校正

## 现象
`exec bin/playaudio` 播放 MP3 时，音频听起来像被加速，中间有被"抽掉"的片段
（类似视频抽帧），整体一顿一顿。采样率日志正确（`hz=48000 ch=2`，`fmt=0x11`
= 48kHz/2ch/16bit），故不是速率配置错误，而是**播放环缓冲写入覆盖了尚未由硬件
播出的数据**，听感表现为错乱的加速与丢段。

## 根因：增量累计式流量控制的致命缺陷
旧 `kernel/drivers/hda.c` 用全局 `g_queued`（已写未播字节数）做增量累计：

```c
static void hda_update_consumed(void) {
    uint32_t lpib  = r32(sd_base() + SD_LPIB) % HDA_BUF_BYTES;
    uint64_t delta = (lpib + HDA_BUF_BYTES - g_lpib_last) % HDA_BUF_BYTES;
    g_lpib_last = lpib;
    g_queued = (delta >= g_queued) ? 0 : g_queued - delta;   // 增量扣减
}
// 写路径：
uint64_t free_bytes = HDA_BUF_BYTES - HDA_GUARD_BYTES - g_queued;  // 无符号减法
uint64_t n = len < free_bytes ? len : free_bytes;
```

QEMU 的 intel-hda 在**流复位 / 环回 / 内部音频定时器重置**时，LPIB（硬件读指针）
会瞬间**回跳**到低值。旧代码用 `g_lpib_last` 做模运算差值，把"回跳"误算成
"向前绕了一整圈的大幅消费" → `g_queued` 被扣到 0 → `free_bytes` 暴涨到 124KB →
一次性把 124KB 拷进环，写指针直接**追上并覆盖尚未播出的数据**（正是听到的
"被抽掉/重复"段落）。更糟的是 `free_bytes` 是无符号减法，`g_queued` 一旦漂移超过
上限会下溢成极大值，进一步加剧覆盖——形成正反馈循环。

保护间隙原来只有 1 页(4KB)，也无法覆盖 QEMU 音频定时器的 LPIB 突发前进步长。

## 修复：读写指针直接求差（绝对自校正）
核心思想：不再增量累计，每次直接由"写指针 − 读指针(LPIB)"求差得到真实排队量。
无论 LPIB 前跳还是回跳，算出的空闲空间只会变**保守**（写更少），绝不会超限覆盖。

`kernel/drivers/hda.c` 改动：
1. 删除 `g_queued`、`g_lpib_last` 两个会漂移/下溢的累计变量及其全部赋值。
2. 新增两个纯函数（宏 `HDA_GUARD_BYTES`/`HDA_PREFILL_BYTES` 移到函数之前，
   保证预处理器可见）：
   - `hda_queued_bytes()`：`!g_running` 时返回 `g_wr_ofs`（未运行=已写即未播）；
     运行后返回 `(g_wr_ofs + HDA_BUF_BYTES - lpib) % HDA_BUF_BYTES`，自校正。
   - `hda_free_space()`：若 `q + HDA_GUARD_BYTES >= HDA_BUF_BYTES` 返回 0，否则
     返回 `HDA_BUF_BYTES - HDA_GUARD_BYTES - q`。**恒在 [0, HDA_BUF_BYTES) 内，
     绝不会下溢成极大值**，因此 `n = min(len, free)` 永远不会越过保护间隙。
3. `hda_pcm_write()`：用 `hda_free_space()` 求 `n`；`free==0` 返回 0（调用方
   `sys_yield()` 重试）实现背压；写入后静音前哨只清零"空闲区"（写指针前方、
   即将被覆盖的区域），绝不触碰已排队未播放数据。
4. `hda_pcm_queued()`：直接返回 `hda_queued_bytes()`；未运行且排队>0 时启动 DMA。
5. 保护间隙 `HDA_GUARD_BYTES` 由 `PAGE_SIZE`(4KB) 提到 `2*PAGE_SIZE`(8KB)，覆盖
   QEMU 音频定时器 LPIB 突发前进步长，避免写指针在突发期间追上读指针。

## 关键常量/地址
- `HDA_BUF_BYTES` = 32×4KB = 128KB（44.1k/48k 16bit 立体声 ≈ 0.67s）。
- `HDA_GUARD_BYTES` = 8KB（新）。
- `HDA_PREFILL_BYTES` = 48KB（预填充水位，不变）。
- SD_LPIB 偏移 `0x04`（链路位置，环内字节偏移），读后 `% HDA_BUF_BYTES`。
- 流格式 `fmt=0x11`：bits[6:4]=001(16bit)，ch-1=1(2ch)，48k 基频=0x0000。

## 验证
`make iso disk` 后 `make run`（自动 KVM + PulseAudio）。
- `-audiodev none` 压力测试：LPIB 极快前进，自校正逻辑未触发任何覆盖/panic，
  播放顺利推进到上千帧，`progress frames=` 递增正常，最终 `done`。
- 采样率确认：`[hda] pcm open: 48000 Hz, 2 ch, 16 bit (fmt=0x11)`。
- 解码正确性：`decode#2: samples=1152 frame_bytes=576`，与 192kbps@48kHz 理论
  帧长精确一致（576B = 1152 样本×2ch×2B / 4，每帧 1152 样本）。

预期正常播放（带声）：串口依次出现
`hda output engine #N ready` → `first frame: hz=48000 ch=2 kbps=192` →
`pcm open` → `playing...` → `progress frames=64/128/...` → `done`，声音连贯无抽帧。

## 使用
```bash
make run                      # 自动 KVM + PulseAudio 出声
# shell 内：
exec bin/playaudio
```
若无声可换后端 `make run QEMU_AUDIODRV=alsa`。
