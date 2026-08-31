# Step51：大文件加载速度优化（DISK OOL 批量读取 + 内核块缓存）

## 一、性能瓶颈分析
在优化前，SukiOS 读一个文件的路径为：用户态 fs_server(FatFs) → `suki_disk_read_sectors` → DISK_PORT IPC → 内核 disk-srv（位于 `kernel/drivers/ata.c`）→ `blk_read` → 应答回传。

实测瓶颈有三层叠加：

1. **DISK 单次 IPC 批上限仅 7 扇区（3.5KB）**（`DISK_MAX_SECTORS=7`，受 `MACH_MSG_INLINE_MAX=3968B` 内联消息硬限制，应答为内联消息无法更大）。读 1MB 文件 = 约 293 次完整 IPC 往返（用户态↔内核 disk-srv↔磁盘），PIO 忙等叠加 IPC 开销极大。
2. **全局零块缓存**：每次 `disk_read` 无论扇区是否刚读过，都重新发 IPC 并落盘。FatFs 读大文件时反复读 FAT 表、目录项、以及文件数据本身，相同扇区被多次搬运。
3. **fs_server 单线程、无预读**（属应用层，本轮未改）。

## 二、优化方案
全部集中在内核态 disk-srv 与 IPC 层，不动用户态 FatFs（`drivers/FatFs/*`），保持 wire ABI 语义稳定。

### 2.1 内核 OOL 发送 API（突破 7 扇区内联上限）
文件：`include/ipc/port.h`、`kernel/ipc/port.c`

新增 `ipc_send_ool_kernel(dest, inline_msg, inline_size, ool_pages[], ool_page_count, ool_size)`：
- 发送方是内核任务（如 disk-srv），数据已位于内核直接映射区/已分配物理页，调用方直接提供**物理页号数组**，本函数仅 `pmm_incref` +1 引用并挂到 `kernel_msg_t->ool_pages[]`（零拷贝），inline 部分照常拷贝。
- 关键修复：**重新布局 inline 为 `[header][ool_desc 占位 16B][原 inline 数据(除 header)]`**，与接收方 `sys_mach_msg → ool_map_into_receiver` 的解析严格对称（`ool_map_into_receiver` 把 `mach_ool_desc_t` 置于 header 后、把 `m->data[sizeof(header)..]` 整段拷到用户缓冲的 `[header+ool_desc]` 处）。若发送方不预留 ool_desc 占位，接收方会把业务数据（如 `disk_read_resp_t.status`）误当成 ool_desc，导致 `status` 被读成 ool 地址低 32 位而非 0，表现为 `f_mount` 全盘失败。

### 2.2 DISK 批上限提升 7 → 64 扇区（OOL 回传）
文件：`include/ipc/disk_proto.h`、`kernel/drivers/ata.c`、`drivers/FatFs/diskio.c`

- `DISK_MAX_SECTORS` 由 7 提升到 **64（32KiB/批）**，读应答改为 **OOL 物理页传输**（`ipc_send_ool_kernel` 回传 `count` 个 8 扇区块对应的物理页，最多 8 页）。
- `drivers/FatFs/diskio.c` 的 `suki_disk_read_sectors` 改为用 `mach_msg_recv` 收 OOL，从 `ool_desc_t.address`（用户 VA）按 **`(lba%8)*512` 首块内偏移 + `count*512`** 截取所需字节（OOL 数据以 8 扇区整页对齐回传），读完后 `mach_msg_destroy(d->address)` 释放 OOL 窗口。
- **写路径保持内联 7 扇区**（`DISK_MAX_WRITE_SECTORS=7`），写场景非本次重点，避免 OOL 发送复杂度；`DISK_MAX_SECTORS` 命名保留给读，新增 `DISK_MAX_WRITE_SECTORS` 给写。
- 读 IPC 往返次数：1MB 文件由约 293 次降到约 32 次（≈9× 减少）。

### 2.3 内核块缓存（消除元数据/重复读落盘）
文件：`kernel/drivers/ata.c`（disk-srv 进程内）

在 disk-srv 读路径前插入 LBA→物理页哈希缓存：
- **缓存块单位 = 1 物理页（4KiB = 8 扇区）**，键 = `lba / 8`（8 扇区对齐）。
- 哈希表（直接取模）+ LRU 双向链表，容量 **256 项 = 1MiB**。
- 单核单线程 disk-srv，访问无需加锁。
- `cache_read_block(block_lba)`：命中则 `pmm_incref` 返回缓存页（交给 OOL 传输）；miss 则 `pmm_alloc_page` + `blk_read` 写入新页并插入 LRU。
- **写穿越（write-through）** `cache_write_through(lba, count, data)`：写成功后，对每个被覆盖的 8 扇区块——若缓存命中则把写入数据的对应扇区精确 `memcpy` 进缓存页（不触碰同块内其他扇区、不影响相邻块）；未命中则令该块失效。这避免了"写 A 块、读相邻 B 块命中旧缓存"的跨块污染（直接失效方案的缺陷：FatFs 更新目录项的写与 reopen 读的目录项常落在不同但相邻的 8 扇区块，简单失效会导致目录读到写前旧值，表现为写后读回失败）。
- 收益：FatFs 反复读 FAT 表/目录/同文件数据全部命中缓存，免 IPC + 落盘。

### 2.4 用户态 OOL 窗口释放 API
文件：`include/sukios/posix.h`、`kernel/ipc/port.c`、`kernel/syscall/syscall.c`、`user/lib/suki.h`

- 新增 `SYS_OOL_UNMAP=125` 系统调用 + `ipc_ool_unmap_user(va)`（port.c）：从 `sched_current()->ool_maps` 找到 VA 节点，`vmm_unmap_page` 解映射每个页 + `pmm_decref` 递减物理页引用（与 `vmm_destroy_address_space` 在任务退出时的兜底回收对称），并把映射区间归还 `g_ool_free` 空闲链表供复用。
- `user/lib/suki.h` 新增 `mach_msg_destroy(ool_va)` 封装（调 `SYS_OOL_UNMAP`）。
- **必须**：`suki_disk_read_sectors` 每次收完 OOL 后调用 `mach_msg_destroy`，否则 OOL 接收窗口单调增长直至 `OOL_RECV_LIMIT` 耗尽、后续所有 OOL 接收失败（致命泄漏）。

## 三、关键数据结构与常量
| 项 | 值 | 说明 |
|----|----|----|
| `DISK_MAX_SECTORS` | 64 | 读批上限（OOL 回传，32KiB） |
| `DISK_MAX_WRITE_SECTORS` | 7 | 写批上限（内联，≤3968B） |
| `DISK_OOL_MAX_PAGES` | 8 | 64 扇区 / 8KB页 = 8 页 |
| `CACHE_BLOCK_SECTORS` | 8 | 缓存块 = 8 扇区 = 1 物理页 |
| `CACHE_ENTS` | 256 | 缓存容量 = 1MiB |
| `SYS_OOL_UNMAP` | 125 | 释放 OOL 接收窗口系统调用 |
| `MACH_MSG_INLINE_MAX` | 3968 | 内联消息上限（硬限制，是 7→64 提升必须走 OOL 的根因） |

## 四、调试过程记录（关键坑）
1. **`f_mount` 全盘失败**：`disk-srv` 用 `ipc_send_ool_kernel` 构造 OOL 消息时未预留 `ool_desc` 占位，接收方 `ool_map_into_receiver` 把 `disk_read_resp_t.status` 误当 `ool_desc` → fs_server 端读到的 `status` 为 ool 地址低 32 位（非零）→ 误判磁盘读失败。修复：发送方按 `[header][ool_desc 16B][inline]` 重排。
2. **shell `#PF`**：临时在 shell 启动加 `run_command("cat README.TXT")` 自检，但当时 fs-server 因坑1退出了，cat 路径踩内存。移除该自检探针后消失（该探针本是非功能验证用，不应留在生产路径）。
3. **fs self-test 写后读回 FAIL**：最初用 `cache_invalidate_range`（按写请求 lba/count 算出块区间失效），但 FatFs 写目录项与 reopen 读目录项常落在相邻但不同的 8 扇区块，简单失效导致读回命中写前旧缓存 → 文件"写后读不到"。改用 write-through 精确同步缓存页对应扇区，self-test 全部 PASS。
4. **`diskio.c` 状态偏移错位**：OOL 消息 inline 布局为 `[header][ool_desc_t][disk_read_resp_t]`，`suki_disk_read_sectors` 原先从 `sizeof(header)` 读 `status`（实际是 ool_desc 位置），改为 `sizeof(header)+sizeof(ool_desc_t)`。

## 五、验证（QEMU headless 回归，符合项目铁律：仅 bash + QEMU 自带机制）
```bash
make iso
qemu-system-x86_64 -machine pc -cpu qemu64 -smp 1 -m 2G -no-shutdown \
  -cdrom build/SukiOS.iso -boot d \
  -drive file=build/disk.img,format=raw,index=0,media=disk \
  -serial file:/tmp/suki_optD.log -display none -no-reboot
```
结果（serial 日志）：
- **零 panic / 零 fault / 零 #PF**；
- `[disk-srv] self-test: read=1 cache_hit=yes`（内核侧 cache+OOL 闭环自检通过）；
- `[fs] mounted FAT32 (FatFs)`、**`[fs] self-test ALL PASS`**（含 reopen/append/subdir/rename/trunc 写后读回全部通过，证明 OOL 读路径 + 块缓存 write-through 一致性正确）；
- `[boot] fs-server spawned (pid=4), POSIX file syscalls enabled`；
- shell 正常进入 `SukiOS>` 提示符；
- 临时诊断打印（`diskio]`/`disk-srv] READ|WRITE`/`dbg:`）已全部清除，日志干净。

## 六、性能收益预期
- 连续大文件（如 1MB）读取：IPC 往返次数 ~293 → ~32（≈9× 减少），PIO 忙等比例同比例下降。
- 随机/小读（FAT 表、目录项、配置、ELF 加载重复扇区）：绝大多数命中内核块缓存，免 IPC + 落盘，FatFs 实际磁盘 I/O 大幅下降。
- 写一致性：write-through 保证缓存与磁盘严格同步，无陈旧缓存读回问题。

## 七、提交
- 已 `git commit`（英文前缀 + 中文描述，符合项目规则，未 push）：
  - 新增 `ipc_send_ool_kernel` + `SYS_OOL_UNMAP`/`mach_msg_destroy` OOL 窗口释放；
  - DISK 读批上限 7→64 扇区（OOL 回传）+ diskio.c OOL 接收/释放；
  - 内核块缓存（8 扇区块/LRU/256 项）+ write-through 一致性；
  - 修复 OOL inline 布局错位与 diskio 状态偏移两处导致 f_mount 失败的 bug。
