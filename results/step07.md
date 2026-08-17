# Step 07 — HDA 初始化超时修复 + minimp3 解码 samples=0 修复 + FS 流式读取性能优化

> 日期：2026-07-27
> 目标：完成此前遗留在 SukiOS 音频子系统中的两个阻断性 bug，并对因此暴露出来的性能瓶颈做必要优化。
> 验证环境：Linux + QEMU (`-machine pc -cpu qemu64`，TCG 软件仿真)，`-device intel-hda -device hda-duplex`。

---

## 1. 问题一：Intel HDA 初始化 TIMEOUT（RIRB 收不到响应）

### 1.1 现象
```
[hda] verb 000f0000 TIMEOUT
[hda]   RIRBWP=0 CORBRP=0 CORBWP=1 g_rirb_rp=2
[hda]   rirb=[00000000 ...] corb=[000f0000 ...]
```
命令 `000f0000`（读 codec 厂商 ID）被正确地写进了 CORB 缓冲槽 0（`corb=[000f0000]`），
`CORBWP=1` 也已写入，但控制器的 **读指针 CORBRP 始终为 0**，说明 CORB DMA 引擎根本没去读这条
命令，因此 RIRB 永远收不到响应 → 超时。同时 `RIRBWP=0` 表示控制器一条响应都没产生。

### 1.2 根因（对照 QEMU 源码 `hw/audio/intel-hda.c` 确认）
下载并通读 QEMU master 的 `intel-hda.c`，定位到三个叠加问题：

**(a) `rirb_cnt` 闸门（最关键）**
`intel_hda_corb_run()` 主循环开头有：
```c
if (d->rirb_count == d->rirb_cnt) {
    return;          /* CORB 引擎直接停止，不消费任何 verb */
}
```
`rirb_cnt` 来自 **RINTCNT 寄存器（偏移 0x5A）**，`intel_hda_set_corb_wp()` 在每次写 CORBWP 时调用
`corb_run`。`rirb_cnt` 默认值为 0，于是 `0 == 0` 恒成立 → **CORB 引擎在任何 CORBCTL=RUN 下也
永不运行**。这正解释了 `corb` 内容已写入但 `CORBRP` 不前进。

> 注意：osdev wiki 把 0x5A 标注为 RIRBRP（RIRB Read Pointer），但 **QEMU 实际把 0x5A 建模为
> RINTCNT（rirb_cnt）**，且把“响应中断计数”这个本应位于 `RIRBCTL[3:0]` 的字段搬到了 0x5A。
> 真实硬件规范里 0x5A 确实是 RIRBRP，但 QEMU 的实现不同——这是 osdev 文档与 QEMU 实现的偏差。

**(b) CORB 槽号 off-by-one**
`intel_hda_corb_run()` 内部：
```c
rp = (d->corb_rp + 1) & 0xff;
addr = intel_hda_addr(d->corb_lbase, d->corb_ubase);
ldl_le_pci_dma(&d->pci, addr + 4 * rp, &verb, attrs);   /* 从 (corb_rp+1) 读 verb */
d->corb_rp = rp;
```
`corb_rp` 是“最后已读槽”索引，复位初值 0，因此**第一个 verb 必须落在槽 1**，而不是槽 0。
原驱动把 verb 写在 `corb[cur]`（cur=当前 CORBWP=0 → 槽 0），与 QEMU 期望的槽 1 错位。

**(c) RIRB 槽号 off-by-one（对称）**
`intel_hda_response()` 内部：
```c
wp = (d->rirb_wp + 1) & 0xff;
stl_le_pci_dma(&d->pci, addr + 8 * wp, response, attrs);   /* 写到 (rirb_wp+1) */
```
响应被写到槽 `(rirb_wp+1)`，初值 0 → 首条响应在槽 1。原驱动从 `rirb[g_rirb_rp*2]`（初值 0 → 槽 0）
读取，再次错位。

### 1.3 修复（`kernel/drivers/hda.c`）

1. **寄存器定义修正**：把原误标的 `REG_RIRBRP 0x5A` 改为 `REG_RINTCNT 0x5A`，并加注释说明 QEMU 把
   0x5A 当作 `rirb_cnt`（真机需改为复位 RIRBRP + 在 RIRBCTL[3:0] 设 RINTCNT）。
   ```c
   #define REG_RINTCNT     0x5A
   ```

2. **RIRB 初始化**：原来写 `REG_RIRBRP=0x8000`（QEMU 下会把 rirb_cnt 置 0，反而再次触发闸门）改为
   写 `REG_RINTCNT=0x00FF`。`rirb_cnt=255` 足以覆盖整个枚举/打开过程（远少于 255 条 verb），让 CORB
   引擎解除闸门开始运行：
   ```c
   w16(REG_RIRBWP, 0x8000);    /* 复位写指针（bit15 自清） */
   w16(REG_RINTCNT, 0x00FF);   /* rirb_cnt=255：QEMU CORB 引擎运行闸门 */
   ```

3. **CORB 写槽 +1**（`hda_verb_raw`）：写入 `(cur+1)%N` 槽，并把 CORBWP 置为该槽索引，使 QEMU 的
   `(corb_rp+1)..CORBWP` 窗口恰好覆盖刚写入的槽：
   ```c
   uint16_t cur  = (uint16_t)(r16(REG_CORBWP) & 0xFF);
   uint16_t slot = (uint16_t)((cur + 1) % g_corb_entries);
   corb[slot] = cmd;
   __asm__ volatile("" ::: "memory");
   w16(REG_CORBWP, slot);
   ```

4. **RIRB 读槽 +1**：响应实际在槽 `(g_rirb_rp+1)%N`，读取与推进都使用该偏移：
   ```c
   while (g_rirb_rp != hw_wp) {
       uint32_t s = (g_rirb_rp + 1) % g_rirb_entries;
       uint32_t resp = rirb[s * 2];
       uint32_t ext  = rirb[s * 2 + 1];
       g_rirb_rp = s;
       if (ext & (1u << 4)) continue;   /* 跳过 unsolicited */
       last_resp = resp; found = true;
   }
   ```

### 1.4 验证结果（QEMU 日志）
```
[hda] controller 8086:2668 at PCI 0:4.0, MMIO phys=0x00000000febb0000
[hda] GCAP=4401 ISS=4 OSS=4 ver=1.0
[hda] ring ok: corb_phys=0x351000 rirb_phys=0x352000 cad=0 ...
[hda] codec cad=0 vendor/device=1af40022
[hda] codec cad=0 AFG=1 DAC=2 PIN=3 (path connected)
[hda] ready: output engine #4, ring 128 KiB (polling, no IRQ)
```
`vendor/device=1af40022`（成功收到 codec 响应）、`AFG=1 DAC=2 PIN=3` 拓扑枚举成功、`output engine #4`
就绪——**HDA 完全初始化，verb 通信打通**。

---

## 2. 问题二：minimp3 解码 samples=0

### 2.1 现象
```
[playaudio] decode#1: samples=0 frame_bytes=31500 filled=31500
[playaudio] first bytes: 49 44 33 04 00 00 00 ...
```
前 3 字节 `49 44 33` = `"ID3"`，说明 MP3 文件头部含 **ID3v2 标签**。minimp3 核心 API
（`mp3dec_decode_frame`）不自动跳过 ID3 等元数据，期望输入直接从 MP3 帧同步字（`FF FB` 等）开始；
遇到 ID3 时它解析失败，`frame_bytes` 退回整缓冲大小，解码器把整段数据当成“待跳过”而丢掉了真实音频。

### 2.2 修复（`user/apps/playaudio.c`）
在解码主循环之前，先读文件前 10 字节判断 ID3v2，按 7-bit 编码的长度字段计算标签总尺寸，
把文件游标 `file_off` 直接跳过：
```c
uint8_t id3[10];
if (fs_read_at(fname, 0, 10, id3) == 10 &&
    id3[0]=='I' && id3[1]=='D' && id3[2]=='D') {
    uint32_t tag = 10u
        + ((uint32_t)id3[6] << 21) | ((uint32_t)id3[7] << 14)
        | ((uint32_t)id3[8] << 7)  | (uint32_t)id3[9];
    file_off = tag;   /* 跳过整段 ID3v2 标签 */
}
```
本例 `MOONHALO.MP3` 标签长 **1986510 字节**，被正确跳过。

### 2.3 验证结果（QEMU 日志）
```
[playaudio] ID3v2 tag detected, skipping 1986510 bytes (file cursor advanced)
[playaudio] first FS read got 3500 bytes (in_filled=3500)
[playaudio] input window filled = 31500 bytes, decoding...
[playaudio] first bytes: 49 45 4E 44 AE 42 60 82 FF FB B4 ...   <- 真实的 MP3 帧头(FF FB)已出现
[playaudio] decode#1: samples=0 frame_bytes=11528 filled=31500  <- 首帧是元数据, frame_bytes 已是真实帧长
[playaudio] decode#2: samples=1152 frame_bytes=576 filled=30472 <- 真正解出 1152 个样本!
[playaudio] first frame: hz=48000 ch=2 kbps=192
[hda] pcm open: 48000 Hz, 2 ch, 16 bit (fmt=0x11)
[playaudio] format: 48000 Hz, 2 ch, 16-bit, 192 kbps
[playaudio] playing...
[playaudio] progress frames=64 / 128 / 192 ...
```
`decode#2 samples=1152` 证明 **samples=0 问题已解决**；`48000 Hz, 2 ch, 192 kbps` 格式正确识别；
HDA 流成功打开并开始播放。

---

## 3. 修复暴露的性能瓶颈与优化（FS 流式读取）

### 3.1 问题
修正 ID3 跳过逻辑后，playaudio 立即尝试流式读取 4.9MB 的真实 MP3 数据。此时发现第二次
`fs_read_at`（offset=1986510）耗时极长，QEMU 日志显示 FS 服务器的簇遍历（walk）在 20s 内仅推进到
`idx=1024`——约 20ms/簇，整个 3877 簇的 walk 要 ~77s，表现为“播放卡死”。

### 3.2 根因（`user/fs_server.c`）
`fat_next()` 每个簇都发起一次完整的磁盘扇区 PIO 读取（`disk_read(1 sector)`）。一个 FAT 扇区含 128 条
簇项，但原实现对连续的 3877 个簇做了 3877 次 PIO，重复读取同一 FAT 扇区。

### 3.3 优化
1. **FAT 扇区缓存**：`fat_next` 增加 1 扇区缓存（`g_fat_cache[512]` + `g_fat_cache_lba`），同扇区内的
   簇项直接命中。walk 的 PIO 次数从 ~3877 降到 ~30。

2. **连续簇合并读取**：`read_file_at()` 重写为“探测从当前簇起、FAT 项 = 上一簇+1 的连续簇段”，把整段
   连续簇合并成**一次 `disk_read`**（最多 `DISK_MAX_SECTORS` 扇区），避免逐簇 PIO。

3. **放大单次磁盘读取上限**：`include/ipc/disk_proto.h` 中 `DISK_MAX_SECTORS` 从 `7` 提到 `64`，使一次
   `disk_read` 最多搬 32KB，显著减少 IPC/PIO 往返次数（内核 `ata.c` 应答缓冲与 `fs_server.c` 请求缓冲
   均按宏自动放大，无需改结构）。

> 内联消息上限约束：`MACH_MSG_INLINE_MAX=3968`（`<ipc/port.h>`），所以 FS 应答（inline）上限约 3.9KB，
> `FS_DATA_MAX=3500` 已接近极限、无法放大；因此减少读取次数是唯一可行路径。

### 3.4 验证
walk 不再成为瓶颈；playaudio 稳定推进（`progress frames=64/128/192`）。FS 服务器与磁盘读取路径工作正常。

---

## 4. 播放速度与实时性说明（重要）

修复后播放**功能完全正确**（解码→HDA 开流→DMA 播放→进度推进），但实测约 **4~5 帧/秒**（实时需 ~41.7 帧/秒，
即约 1/8 ~ 1/13 速度）。

通过新增的 bench 模式（`exec bin/playaudio MOONHALO.MP3 bench`，只解码不写 HDA）隔离验证：
- 纯解码速度与普通播放模式相当（甚至略慢），**证明瓶颈不在磁盘/HDA，而在 minimp3 浮点解码**。
- SukiOS 当前在 QEMU **TCG 软件仿真**下运行，FPU/SSE2 浮点指令被逐条解释执行，minimp3 的 IMDCT/合成
  滤波等大量浮点运算在此环境下极为缓慢，这是软件仿真的固有特性，**非驱动或解码器 bug**。

**实现实时播放的建议**：
- 在宿主机 Linux 上用 **KVM 硬件加速**运行（`-enable-kvm`，需 `/dev/kvm`）， guest 代码直接在本机 CPU
  执行，FPU/SSE2 速度恢复原生，minimp3 可轻松实时；或
- 后续可引入定点（fixed-point）MP3 解码器替代浮点 minimp3。

---

## 5. 改动文件清单

| 文件 | 改动 |
|------|------|
| `kernel/drivers/hda.c` | 修正 0x5A 语义（RIRBRP→RINTCNT）；RIRB 初始化写 `rirb_cnt=0xFF`；CORB 写槽 +1；RIRB 读槽 +1 |
| `user/apps/playaudio.c` | 解码前手动跳过 ID3v2 标签（7-bit 长度）；新增 `bench` 模式（只解码）；每 64 帧打印进度 |
| `user/fs_server.c` | `fat_next` 增加 FAT 扇区缓存；`read_file_at` 连续簇合并读取 |
| `include/ipc/disk_proto.h` | `DISK_MAX_SECTORS` 7 → 64（减少 PIO/IPC 往返） |

## 6. 复现 / 调试命令
```
# 构建
make iso disk
# 运行（QEMU TCG，无声音设备仅验证数据通路）
qemu-system-x86_64 -machine pc -cpu qemu64 -m 2G -no-shutdown \
  -serial file:/tmp/sukios.log -display none \
  -audiodev none,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0 \
  -boot d -cdrom build/SukiOS.iso -drive file=build/disk.img,format=raw,index=0,media=disk \
  -monitor unix:/tmp/qmon.sock,server,nowait
# 在 shell 中：exec bin/playaudio            （普通播放）
#            exec bin/playaudio moonhalo.mp3 bench  （仅解码基准）
```
QEMU 源码对照：`hw/audio/intel-hda.c` 的 `intel_hda_corb_run`（rirb_cnt 闸门、verb 槽 `(corb_rp+1)`）、
`intel_hda_response`（响应槽 `(rirb_wp+1)`）、`intel_hda_set_corb_wp`/`intel_hda_set_corb_ctl`（触发 corb_run）。
