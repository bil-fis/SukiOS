# Step 29 — FAT32 驱动生产环境加固（基于 83e560e）

> 目标：在当前 `83e560e`（P1-2 FAT32 写路径完整化）基础上，全面审查并修复 FAT32 驱动（用户态 `user/fs_server.c`）与内核 ATA 裸读写（`kernel/drivers/ata.c`）的潜在生产隐患，确保 FAT32 读写/删除/重命名/截断/大文件流式读稳定可预期运行，杜绝 panic/数据损坏/读尾块丢失。

---

## 一、代码位置与架构

- FAT32 驱动实现：**`user/fs_server.c`**（Ring3 用户态服务，经 `mach_msg` 响应 `FS_PORT` 请求；磁盘裸读写经 IPC 给内核 `DISK_PORT`，由 `kernel/drivers/ata.c` 的 `disk-srv` 任务执行 PIO/PIO+FLUSH）。
- 内核磁盘服务：**`kernel/drivers/ata.c`**（`blk_read`/`blk_write` → `ata_read_sectors`/`ata_write_sectors`，含 LBA28/48 选择、越界保护、`ata_select_lba` 统一 LBA 选择原语）。
- 磁盘镜像布局：由 `Makefile` 用 `mformat -i build/disk.img -F` **整盘格式化 FAT32（无 MBR 分区表）**，故 `fat32_mount` 读 LBA0 当 BPB 是正确的（无需解析 MBR）。
- 关键常量：`SECTOR=512`、`DISK_MAX_SECTORS=7`（内联消息上限 3968B）、`FAT_WALK_LIMIT=100000`（环链兜底）、`LFN_CAP=256`、`RA_SECS=64`（32KB 预读窗口）。

---

## 二、发现并修复的问题（按严重度）

### A. 【严重】`fat_next` 坏簇/EOF 未终止 → 簇链越界读飞
**文件**：`user/fs_server.c`（`fat_next`，原 149 行）
**原行为**：读 FAT 项后直接 `return v & 0x0FFFFFFF`，调用方以 `clus < 0x0FFFFFF8` 续链。坏簇 `0x0FFFFFF7` 满足 `< 0x0FFFFFF8` 被当有效簇继续 walk → `clus_to_lba(0x0FFFFFF7)` 算出巨大 LBA → 内核 `ata_read_sectors` 越界保护拒绝 → 读失败。
**修复**：
- 入口加 `if (clus < 2 || clus >= g_max_cluster) return 0;` 拦截非法簇号（`g_max_cluster = 2 + g_total_clusters`）。
- `if (v >= 0x0FFFFFF7) return 0x0FFFFFFF;`（EOF 哨兵）。坏簇(0x0FFFFFF7)与 EOF(>=0x0FFFFFF8)统一归一成 EOF 哨兵，调用方 `clus<0x0FFFFFF8` 正确停止；空闲簇(v==0)保持返回 0，不影响 `fat_alloc_cluster` 的“==0 判空闲”语义。

### B. 【严重】`read_file_at` 预读越卷边界 → 大文件尾块读不到
**文件**：`user/fs_server.c`（`read_file_at`，原 622 行 `disk_read_batched(base_lba, run_secs, ...)`）
**原行为**：预读窗口 `run_secs` 按连续簇链探测，不裁剪到卷末。当文件尾部接近卷边界，`base_lba+run_secs-1 > g_max_lba` → 内核越界保护拒绝 → `disk_read_batched` 返回 false → `read_file_at` 提前 `return done`（尾块丢失）。这正是 6.8MB MP3 流式播放接近卷尾时可能丢尾块的根因。
**修复**：在预读前裁剪 `run_secs` 不超过卷末扇区 LBA（`g_max_lba`）；若整段越界则 `break` 停止读取。向下取整到整簇，保证 `run_secs` 与 `run` 一致。
- 新增全局 `g_max_lba`（卷末扇区 LBA），在 `fat32_mount` 中由 `tot_sec-1` 设置。

### C. 【中等】`fat32_mount` 校验不足 → 损坏/非 FAT32 镜像被错误挂载
**文件**：`user/fs_server.c`（`fat32_mount`）
**修复**：增加
- BPB 引导签名 `0x55AA` 校验；
- FAT32 判定：`byts_per_sec==512`、`g_sec_per_clus>0`、`fatsz32>0`、`fatsz16==0`、`root_ent==0`、`g_root_clus>=2`；
- `g_total_clusters` 合理性（非 0 且 `< 0x0FFFFFF7`），否则挂载失败并打印原因。
- 同时记录 `g_max_cluster` 与 `g_max_lba`。

### D. 【中等】`dir_lookup` 损坏 LFN 误匹配 → 改名/误删隐患
**文件**：`user/fs_server.c`（`dir_lookup`，约 1098 行）
**原行为**：累积 LFN 名直接用于匹配，未校验 LFN 校验和与 8.3 短名一致。损坏 LFN（删除残留导致误拼接）可能返回错误长名并被匹配。
**修复**：当 `nlfn>0` 时，用 `lfn_checksum(e, &sum)` 计算短名校验和，与 `lfn_slots[0].off+13`（最高序号 LFN 项的校验和字节）比较；不匹配则回退 `fmt_83(e, name)` 用 8.3 短名，杜绝误匹配。
（注：`fs_unlink` 已正确清除前导 LFN 项，`read_dir` 的 LFN 连续性与 orphan 处理已有 L7 修复兜底，本项再加一道校验和防线。）

### E. 【增强】写路径后预读缓存失效 → 读后写再读命中陈旧数据
**文件**：`user/fs_server.c`（`disk_write`）
**修复**：`disk_write` 成功返回前 `g_blk_lba = 0`，使顺序读预读缓存失效。避免同一会话内“读→写→再读同一区间”命中写前陈旧数据。FAT 读缓存由 `fat_set` 单独失效，此处只针对数据区预读缓存。

### F. 【内核增强】`ata.c` 写重试 + 64 位 LBA 支持
**文件**：`kernel/drivers/ata.c`（`ata_read_sectors`/`ata_write_sectors`/`blk_read`/`blk_write` + `include/kernel/ata.h`）
**修复**：
- 函数参数 `lba` 由 `uint32_t` 改为 `uint64_t`（`ata_select_lba` 已支持 LBA48 64 位，此前 `blk_read`/`blk_write` 强转 `uint32_t` 会丢失 >4GB 盘的 LBA 高 32 位）。
- `ata_write_sectors` 增加命令级重试：ATA 命令/FLUSH 偶发失败最多重试 3 次，仍失败才返回 false，由上层 `disk_write` 应答 `status!=0` 让 FS 回传 `FS_ERR_IO`（错误闭环，不静默丢数据）。
- 写路径越界保护改为 `(uint64_t)lba + count > g_total_sectors`（与原读路径一致）。

### G. 【一致化】`fat_alloc_cluster` 上限
`fat_alloc_cluster` 扫描范围 `[2, 2+g_total_clusters)`，返回值天然合法；调用方（如 `dir_find_free_run`）的 `0x0FFFFFF7` 比较由散落常量统一到 `g_max_cluster` 语义（已在 `fat_next`/`fat_set` 中采用 `g_max_cluster`）。

---

## 三、验证（QEMU KVM 直接运行，串口落盘，无 GDB）

构建：`make iso`（成功，relk 2490 项重定位）。

启动命令（headless，音频后端 none 避免无桌面环境失败）：
```
qemu-system-x86_64 -machine pc,accel=kvm -cpu host -smp 4 -m 2G -no-shutdown \
  -display none -serial file:fs_boot3.log -audiodev none,id=snd0 \
  -device intel-hda -device hda-duplex,audiodev=snd0 -boot d \
  -cdrom build/SukiOS.iso -drive file=build/disk.img,format=raw,index=0,media=disk
```

FS_SERVER 启动自检输出（全部 **PASS**）：
```
[fs] FAT32 mounted: spc=1 fat@32 data@512 root_clus=2 clusters=...
[fs] root directory (self-test):
[fs]   README.TXT  94 bytes
[fs]   MOONHALO.MP3  6885398 bytes
[fs] write-path self-test begin
[fs]   write+readback PASS
[fs]   append PASS
[fs]   mkdir+subdir PASS
[fs]   rename PASS
[fs]   unlink PASS
[fs]   rmdir PASS
[fs]   truncate PASS
[fs] write-path self-test: ALL PASS
[fs]   bigfile whole-read PASS          <-- 新增：6.8MB 整读 + 尾块 100B 校验
[fs] entering service loop on FS_PORT
```

新增自检（`fs_server.c` self-test 末尾）：分块整读 `MOONHALO.MP3`（6885398 字节，每块 `FS_DATA_MAX=3500`，约 1968 次 `read_file_at`），累计字节 == size 且尾块 `read_file_at(size-100)` 返回 100 → 验证修复 B（越界裁剪）使接近卷尾的大文件尾块不丢。

无 panic / oops / EXC / triple fault。4/4 CPU online，FS_SERVER 进入服务循环后系统稳定运行。

---

## 四、修复清单与防御性设计总结

| 项 | 问题 | 修复 | 防御措施 |
|----|------|------|----------|
| A | 坏簇/EOF 续链越界 | 归一 EOF 哨兵 + `g_max_cluster` 校验 | `fat_next` 拦截非法簇号 |
| B | 预读越卷边界丢尾块 | 裁剪 `run_secs` 至 `g_max_lba` | 新增 `g_max_lba` 全局 |
| C | 损坏镜像被挂载 | BPB/卷参数校验 | 签名+类型+簇数合理性 |
| D | 损坏 LFN 误匹配 | 校验和与 8.3 比对 | 不匹配回退短名 |
| E | 写后读命中陈旧缓存 | `disk_write` 后 `g_blk_lba=0` | 读缓存失效 |
| F | 写失败丢数据 / >4GB 截断 | 写重试 3 次 + 64 位 LBA | 错误闭环 + 大盘支持 |

所有修复均为真实完整实现，无 stub/TODO，错误处理闭环（失败上报 `FS_ERR_IO`/`false`，不静默忽略）。

---

## 五、提交信息

```
fat32-fix: P0 生产加固 FAT32 驱动与 ATA 裸写 (A-F)

- fat_next: 坏簇/EOF 归一 EOF 哨兵, 非法簇号拦截(g_max_cluster)
- read_file_at: 预读不超过卷末 LBA, 修复大文件尾块越界丢数据
- fat32_mount: BPB 签名/类型/簇数校验, 记录 g_max_lba/g_max_cluster
- dir_lookup: LFN 校验和与 8.3 短名比对, 损坏 LFN 回退短名
- disk_write: 成功后使顺序读预读缓存失效
- ata.c: 写命令级重试(3次)+64位LBA(去 uint32_t 截断)
- fs_server self-test: 新增 6.8MB 大文件整读+尾块读校验
```
