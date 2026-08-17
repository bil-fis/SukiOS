# Step 08：MP3 连贯播放修复（磁盘协议截断 bug + FS 预读缓存 + KVM 加速）

## 背景

Step 07 打通了 HDA 播放通路，但播放严重断续：每次 `[pa] fs_read_at #N ...` 串口输出后只出一小段声音，随后停顿。本阶段定位并修复了三个叠加问题，最终实现实时连贯播放。

## 问题一（致命）：DISK_MAX_SECTORS=64 违反内联 IPC 上限，磁盘数据被静默截断

### 现象
`playaudio` 输入缓冲的首字节 dump 为：
```
49 45 4E 44 AE 42 60 82 FF FB B4 00 00 00 00 00 ...（其后全零）
```
仅前 50 字节正确（`IEND` PNG 尾 + `FF FB` MP3 同步字），其余为 0，minimp3 解不出任何帧（`samples=0, frame_bytes=31500`）。

### 根因链
1. Step 07 曾把 `include/ipc/disk_proto.h` 的 `DISK_MAX_SECTORS` 从 7 改为 64，意图减少 IPC 往返。
2. 但磁盘应答是**内联 IPC 消息**，受 `MACH_MSG_INLINE_MAX = 3968` 字节硬限制（`include/ipc/port.h`）。7×512=3584 + 头(32) + status(8) = 3624B 恰好放得下；64×512=32768B 远超上限。
3. 内核 `ipc_send_kernel()`（`kernel/ipc/port.c`）对超限消息直接返回 `MACH_SEND_TOO_LARGE` 拒发；而 `fs_server` 的 `disk_read()` 客户端按 `count*512` 从 `g_diskbuf`（BSS，全零）拷贝——收到的旧短应答只覆盖前部，其余全是零。
4. 首扇区偏移计算：1986510 % 512 = 462，512−462 = 50 —— 与"仅前 50 字节正确"精确吻合，证明只有单扇区旧路径数据存活。

### 修复
```
include/ipc/disk_proto.h
```
- `DISK_MAX_SECTORS` 恢复为 **7**，并加注释说明这是内联 IPC 协议硬约束，严禁调大；需要更大吞吐必须在客户端分批。

## 问题二：fs_server 顺序读缓存永远失效 + 无数据缓存

### 根因
Step 07 版 `read_file_at()`（`user/fs_server.c`）每次按整个 run（最多 64 簇）推进 `g_ra_idx`，但单次请求只交付 `FS_DATA_MAX=3500B`（约 7 簇）。下次请求的 `want_idx < g_ra_idx`，顺序读缓存条件 `g_ra_idx <= want_idx` 不成立 → **每次请求都从簇链头重新遍历数千簇**（几十次 FAT 扇区 PIO，每请求数百 ms 停顿）。且每次请求都重新读盘，无数据复用。

### 修复（`user/fs_server.c`）
1. **簇指针按实际消费推进**：`adv = (within + n) / clus_bytes`，只前进消费过的完整簇数，`within` 保留簇内残余偏移。顺序读缓存恒命中。
2. **32KB 预读缓存**：
   - `RA_SECS=64`（32KB）预读窗口：`g_blk_lba/g_blk_secs/g_blk_buf[RA_SECS*SECTOR]`。
   - 新增 `disk_read_batched(lba, secs, dst)`：按 `DISK_MAX_SECTORS=7` 分批发磁盘 IPC，合法填满 32KB 缓存。
   - 命中判定：所需扇区区间 `[need_first, need_last) ⊆ [g_blk_lba, g_blk_lba+g_blk_secs)` 时零磁盘 IPC 直接 memcpy。
   - 效果：3500B/请求 × 9 次 ≈ 一个 32KB 窗口，磁盘停顿从每请求 1 次降为每 9 请求 1 次。
3. 连续簇探测上限从 `DISK_MAX_SECTORS/spc` 改为 `RA_SECS/spc`（`fat_next` 已有 FAT 扇区缓存，探测无额外磁盘读）。

## 问题三：TCG 软件仿真解码仅 ~1/8 实时

### 根因
minimp3 浮点解码在 QEMU TCG 下每条 SSE 指令都软件模拟，bench 实测约 4~5 帧/秒（实时需 41.7 帧/秒）。这是仿真固有开销，非代码问题。

### 修复（`Makefile`）
宿主机 `/dev/kvm` 存在（WSL2 嵌套虚拟化已启用、当前用户可写），加自动检测：
```makefile
QEMU_ACCEL ?= $(shell test -w /dev/kvm && echo kvm || echo tcg)
ifeq ($(QEMU_ACCEL),kvm)
QEMU_FLAGS := -machine pc,accel=kvm -cpu host -m 2G -no-shutdown
else
QEMU_FLAGS := -machine pc -cpu qemu64 -m 2G -no-shutdown
endif
```
- KVM 下 Ring3 浮点全部原生执行，解码远超实时。
- 可 `make run QEMU_ACCEL=tcg` 强制回退软件仿真。

## 附带清理

- `user/apps/playaudio.c`：`fs_read_at` 的逐次串口打印（原 80 次）降为仅前 3 次——每次打印占用串口与时间片，本身就是播放期间的停顿源之一。

## 验证

命令（无声后端验证通路；实际听声用 `make run`，默认 pa 后端）：
```bash
make iso disk
qemu-system-x86_64 -machine pc,accel=kvm -cpu host -m 2G -no-shutdown \
  -serial file:/tmp/sukios.log -display none \
  -audiodev none,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0 \
  -boot d -cdrom build/SukiOS.iso \
  -drive file=build/disk.img,format=raw,index=0,media=disk
# shell 内执行：exec bin/playaudio
```

结果（约 28s 墙钟）：
```
[playaudio] first frame: hz=48000 ch=2 kbps=192
[playaudio] playing...
[playaudio] decode#2: samples=1152 frame_bytes=576 filled=30916   ← 576B = 192kbps@48kHz 理论帧长，数据完整
[playaudio] progress frames=64
...
[playaudio] progress frames=1408                                  ← 1408×1152/48000 = 33.8s 音频 / 28s 墙钟
```
- **数据完整性**：`frame_bytes=576` 与 192kbps@48kHz 理论值（144×192000/48000=576）精确一致；每帧稳定 1152 样本。
- **连贯性**：33.8s 音频 / 28s 墙钟 ≥ 实时（受 HDA 环背压节流，实际播放由 DMA 时钟定速）。

## 用户使用方式

```bash
make run                      # 自动启用 KVM（有 /dev/kvm 时），PulseAudio 出声
make run QEMU_AUDIODRV=alsa   # 换 ALSA 后端
make run QEMU_ACCEL=tcg       # 强制软件仿真（播放会慢，仅调试用）
```
进入 shell 后：
```
exec bin/playaudio                      # 播放 MOONHALO.MP3
exec bin/playaudio MOONHALO.MP3 bench   # 只解码不出声（解码吞吐基准）
```

## 涉及文件

| 文件 | 改动 |
|---|---|
| `include/ipc/disk_proto.h` | `DISK_MAX_SECTORS` 64→7（协议硬约束回滚+注释） |
| `user/fs_server.c` | `read_file_at` 重写：按消费推进簇指针；`RA_SECS=64` 预读缓存；`disk_read_batched` 分批 IPC |
| `user/apps/playaudio.c` | fs_read_at 调试打印 80 次→3 次 |
| `Makefile` | `QEMU_ACCEL` 自动检测 /dev/kvm，KVM 下 `-machine pc,accel=kvm -cpu host` |

## 经验教训

1. **协议常量不可孤立调整**：`DISK_MAX_SECTORS` 与 `MACH_MSG_INLINE_MAX` 存在隐式耦合，调大前必须核对整条消息路径的容量约束。截断是静默的（客户端读到全零而非报错），极难察觉。
2. **缓存失效要对称验证**：写缓存推进逻辑时，必须用"下一次请求的命中条件"反向检验推进量。
3. **仿真环境性能结论要标注加速器**：TCG 与 KVM 的浮点性能差一个数量级以上，性能类 bug 先确认 `/dev/kvm`。
