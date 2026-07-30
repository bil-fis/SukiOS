# Step 28 — P1-2 FAT32 写路径完整化（完整且健壮的 FAT32 驱动）

> 目标：在 Ring3 用户态 `FS_SERVER` 中实现**完整、健壮、生产可用**的 FAT32 写功能，
> 覆盖创建/写/追加/删/改名/截断/子目录，并满足项目所有铁律（边界校验、错误闭环、
> 无死锁、内存安全、无 stub/TODO）。此前 FS_SERVER 仅支持读，磁盘写协议
> `DISK_MSG_WRITE` 在内核侧早已实现但从无调用者——本次补齐用户态落盘全链路。

---

## 1. 总体架构与数据流

```
Shell(Ring3) ──FS_MSG_*──> FS_SERVER(Ring3) ──DISK_MSG_READ/WRITE──> 内核 DISK_PORT
                                                           │
                                                           ▼
                                            disk_srv_task() → blk_read/blk_write
                                              (ATA PIO / AHCI DMA)
```

- FS_SERVER 完全驻留用户态，通过 Mach IPC 把**扇区读写请求**转发给内核 `DISK_PORT`，
  内核 `disk-srv` 任务（`kernel/drivers/ata.c` 的 `disk_srv_task()`）仅做边界校验后
  调用 `blk_read`/`blk_write`。内核不解析任何 FAT 结构，符合混合内核设计。
- 写路径全部在 `user/fs_server.c` 中实现，复用既有 FAT 读取基础设施（目录遍历、
  LFN 解析、`fat_next` 簇链跟随、扇区缓存）。

### 1.1 关键常量（来自 `include/ipc/fs_proto.h` 与 `disk_proto.h`）

| 常量 | 值 | 含义 |
|------|----|------|
| `SECTOR` | 512 | 扇区字节数（fs_server.c:24） |
| `FS_WRITE_MAX` | 3584 | 单条写请求内联数据上限（= `DISK_MAX_SECTORS(7)*512`，且 `< MACH_MSG_INLINE_MAX(3968)`） |
| `DISK_MAX_SECTORS` | 7 | 内核单次扇区读写上限 |
| `MACH_MSG_INLINE_MAX` | 3968 | 内联消息体上限 |
| `FS_DATA_MAX` | 3500 | 读应答内联数据上限 |
| `FAT_WALK_LIMIT` | 0x400000 | 簇链遍历步数硬上限（防环链死循环） |
| `FS_MSG_CREATE/MKDIR/WRITE/UNLINK/RENAME/TRUNCATE` | 5/6/7/8/9/10 | 新增写消息号 |

> 设计约束：单条写请求内联数据 ≤ 3584B，既满足 IPC 内联消息上限，又匹配磁盘单请求
> 扇区数上限；超过该长度的数据由调用方分多条 `FS_MSG_WRITE`（offset 递增）写入。

---

## 2. 文件改动清单

### 2.1 `include/ipc/fs_proto.h`（新增写协议）
- 新增写消息号 `FS_MSG_CREATE=5`、`FS_MSG_MKDIR=6`、`FS_MSG_WRITE=7`、
  `FS_MSG_UNLINK=8`、`FS_MSG_RENAME=9`、`FS_MSG_TRUNCATE=10`。
- `#define FS_WRITE_MAX 3584`（注释说明为何取此值）。
- 新增载荷结构：
  - `fs_write_req_t { uint32_t offset; uint32_t length; }` —— 其后紧跟 NUL 结尾文件名 + `length` 字节数据（无填充）。
  - `fs_trunc_req_t { uint32_t size; }` —— 其后紧跟 NUL 结尾文件名。
  - `fs_rename_req_t { char old_name[64]; char new_name[64]; }`。
- 应答状态沿用既有 `FS_OK=0 / FS_ERR_IO=1 / FS_ERR_NOENT=2 / FS_ERR_TOOBIG=3`，写类应答 `fr->length=0`（不带数据体）。

### 2.2 `user/fs_server.c`（核心：只读 → 读/写）
文件头注释改为 `FS_SERVER：Ring3 FAT32 文件系统服务（读/写，P1-2 写路径完整化）`。
所有写逻辑均带越界/环链/磁盘错误闭环，零 panic 路径。

### 2.3 `user/shell.c`（新增交互写命令）
- 新增写请求缓冲 `g_wreq[]`（含 `FS_WRITE_MAX` 数据上限）。
- 新增辅助 `fs_request_write()`（组装 `FS_MSG_WRITE`：`fs_write_req_t`+文件名+数据，紧邻布局）、
  `fs_wait_status()`（阻塞等写应答并印 ok/错误）。
- 文件名缓冲 `name[64]`（原 64 已满足，旧 `fs_request` 复用）。
- 新增命令：`mkfile <FILE>` / `mkdir <DIR>` / `write <FILE> <TEXT>` / `rm <FILE>` /
  `rename <OLD> <NEW>` / `truncate <FILE> <SIZE>`；`help` 文本补全。

### 2.4 内核 `kernel/drivers/ata.c`（`disk_srv_task()`）
**无需改动**——已存在 `DISK_MSG_WRITE` 分支：边界校验（`lba+count` 不越界、簇号合法）后
调 `blk_write`；`blk_write` 同时支持 i440fx IDE PIO 与 q35 AHCI DMA 后端。

---

## 3. 写路径核心实现（数据结构 / 算法）

### 3.1 卷参数采集（`fat32_mount`，fs_server.c:91）
- 读 BPB（扇区 0）：`byts_per_sec`、`g_sec_per_clus`、`rsvd`、`nfats`、`fatsz32`、`g_root_clus`。
- **新增**：`g_nfats = nfats ? nfats : 1`（写回全部 FAT 副本）、`g_fat_size = fatsz32`。
- 计算 `g_total_clusters = (tot_sec - g_data_begin) / g_sec_per_clus`，用于卷内簇号范围校验与遍历步数上限。
- 防御：校验 `byts_per_sec==512`、`g_sec_per_clus!=0`、`fatsz32!=0`，否则 mount 失败。

### 3.2 扇区写入（`disk_write`，fs_server.c:632）
```c
static uint8_t g_wreq[sizeof(mach_msg_header_t) + sizeof(disk_write_req_t)
                      + DISK_MAX_SECTORS * SECTOR];
static bool disk_write(uint64_t lba, uint8_t count, const void *buf) {
    if (count == 0 || count > DISK_MAX_SECTORS) return false;   // 边界拒绝
    ... 组装 DISK_MSG_WRITE → mach_msg_send → recv FS_REPLY_PORT ...
    return rr->status == 0;                                       // 真正闭环（失败上报）
}
```
- 与 `disk_read` 对称；`lba` 为 `uint64_t` 防 4GB 内 32 位回绕。

### 3.3 FAT 表项写回（`fat_set`，fs_server.c:662）
- **越界保护**：`clus < 2 || g_total_clusters==0 || clus-2 >= g_total_clusters` → 拒绝写入。
- 值掩码 `val &= 0x0FFFFFFF`（清高 4 位保留位）。
- **写回全部 FAT 副本**：循环 `i ∈ [0, g_nfats)`，`lba = g_fat_begin + i*g_fat_size + sec_off`，
  先 `disk_read` 该扇区 → 改 4 字节 → `disk_write`。双/多 FAT 卷一致性保证。
- 写后立刻 `g_fat_cache_ok = false` 使读缓存失效，避免 `fat_next` 读到陈旧项导致链错乱。

### 3.4 簇分配（`fat_alloc_cluster`，fs_server.c:689）
- 从扫描起点 `g_fat_scan`（init=2）起环形扫描，`fat_next(c)==0` 判定空闲。
- 找到后立即 `fat_set(c, 0x0FFFFFFF)` 标记为已占用（**分配即占用**，避免并发/重入重复分配），
  更新 `g_fat_scan = c+1`（回绕到 2），加速下次连续分配。
- 无空闲簇 → 打印 `no free clusters` 返回 0，上层转 `FS_ERR_IO`。

### 3.5 簇链释放（`fat_free_chain`，fs_server.c:712）
- 沿链 `fat_set(c, 0)` 逐簇标记空闲；用 `g_total_clusters` 与 `guard` 双重边界防环链死循环。
- 遇到 EOC（`>=0x0FFFFFF8`）或非法簇即停止。

### 3.6 整簇清零（`write_zero_cluster`，fs_server.c:732）
- 新分配目录首簇 / 文件扩展簇前，用全局 `g_zero[128*512]`（全 0）把整簇每扇区写 0。
- 保证稀疏区/新目录读回为 0，且 `.`/`..` 之外不会残留旧数据（无信息泄漏）。

### 3.7 目录条目写（`dir_patch`/`write_entry_at`/`dir_entry_pos`，fs_server.c:744-785）
- `dir_patch(lba, off, src, len)`：读-改-写单扇区；`off+len > SECTOR` 拒绝（边界保护）。
- `dir_entry_pos(start_clus, idx, &clus, &byte)`：把全局目录项序号映射到 (簇, 簇内字节偏移)，
  跨簇链用 `fat_next` 推进，`hops < FAT_WALK_LIMIT` 防环。
- `dir_find_free_run(start_clus, count, &idx)`（fs_server.c:789）：在目录中找 `count` 个连续
  空闲项（0x00 或 0xE5）；若目录链结束仍不足，则 `fat_alloc_cluster` + `write_zero_cluster`
  + `fat_set` 扩展新簇后继续——**目录自动增长，无容量上限假设**。

### 3.8 名称 / 8.3 / LFN 生成（fs_server.c:847-996）
- `to_up`、`fix_char`：长名归一化，`fix_char` 把非法字符映射为 `_`（合规 FAT 字符集）。
- `make_shortname`：拆分 base/ext，大写化，截 8+3。
- `make_unique_83`（fs_server.c:923）：若 8.3 名与现有冲突，追加 `~N` 后缀（仿 Windows），
  经 `dir_has_83` 探测，最多 99 次尝试，避免覆盖已有文件。
- `lfn_checksum`（fs_server.c:939）：标准 FAT LFN 校验和算法
  `sum = (sum>>1)|(sum&1?0x80:0) + c`，供 LFN 条目 `e[13]=sum` 绑定到 8.3 条目。
- `build_83_entry` / `build_lfn_entry`：构造 32B 条目。LFN 条目标志 `e[0]=0x40|seq`（末条）
  或 `seq`，属性 `0x0F`，`build_lfn_entry` 按 13 字符/条填充 3 段（1/14/28 偏移），
  末字符后写 `0x0000`、其余填 `0xFFFF`。
- 日期字段统一用合法 FAT 日期 `0x21`（1980-01-01）避免解析器拒绝。

### 3.9 路径解析（`split_parent`，fs_server.c:1077）
- 按 `/` 拆分父目录与基名；父目录经既有 `resolve_path` 解析（支持多级子目录）。
- 返回 `parent_clus`，基名写入调用方缓冲；空基名返回失败。

### 3.10 文件/目录创建（`fs_create`，fs_server.c:1167）
- 幂等：已存在且类型一致 → `FS_OK`；类型冲突 → `FS_ERR_IO`。
- 目录：`fat_alloc_cluster` 首簇 → `init_dir_cluster`（写 `.`/`..`，`..` 指向父簇，根目录父簇记 0）。
- 文件：首簇留 0（首写时再分配）。
- 计算 `nlfn = (nchars+12)/13`，`count = nlfn+1`，
  `dir_find_free_run` 找连续空位 → 逆序写 LFN 条 → 写 8.3 条（属性 `0x10` 目录 / `0x20` 文件，size=0）。

### 3.11 文件写入（`fs_write`，fs_server.c:1214）
- `len==0` → `FS_OK`；`len > FS_WRITE_MAX` → 截断到上限（调用方应分条）。
- 不存在则先 `fs_create(path,false)`（自动建空文件）。
- 计算 `need = ceil((offset+len)/clus_bytes)`，`cur = chain_len(first_clus)`。
- 首簇为空时分配首簇并 `patch_dir_firstclus`；不足则循环分配新簇、`fat_set(last,nc)` 链接、
  `fat_set(nc, 0x0FFFFFFF)` 置 EOF。
- 按 `offset..offset+len` 逐扇区读-改-写（跨簇用 `cluster_at_index`），最后 `patch_dir_size` 更新文件大小。
- 所有 `disk_read`/`disk_write`/`fat_set` 失败均 `return FS_ERR_IO`（错误闭环，不 silently 成功）。

### 3.12 删除（`fs_unlink`，fs_server.c:1273）
- 目录：非空（除 `.`/`..` 外有任何条目）→ `FS_ERR_IO` 拒绝删除（防数据丢失）。
- `fat_free_chain(first_clus)` 释放簇链 → 8.3 条首字节写 `0xE5` → 其前导 LFN 条逐个写 `0xE5`。

### 3.13 重命名（`fs_rename`，fs_server.c:1293）
- 解析新旧父目录与基名（可跨目录/重命名目录）。
- `make_unique_83(new_parent, new_base, s83)` 取唯一 8.3 名；写新 LFN+8.3 条（保留原 `first_clus` 与 `size`）。
- 把旧 8.3 条及前导 LFN 条置 `0xE5`（逻辑移动，簇链不复制，O(1) 完成，**无数据重写**）。

### 3.14 截断（`fs_truncate`，fs_server.c:1335）
- `need = ceil(new_size/clus_bytes)`。
- 扩展（`need>cur`）：分配并链接新簇（同 `fs_write`），稀疏区已 `write_zero_cluster` 清零。
- 收缩（`need<cur`）：
  - `new_size==0`：释放整条链并 `patch_dir_firstclus(0)`。
  - 否则：`cluster_at_index(first, need-1)` 处 `fat_set(c, 0x0FFFFFFF)` 截断，对剩余尾链 `fat_free_chain`。
- 最后 `patch_dir_size(new_size)`。

### 3.15 辅助
- `chain_len`/`cluster_at_index`（fs_server.c:1105）：带 `g_total_clusters`+`guard` 边界，防环链。
- `fs_cmp`/`fs_read_all`：自检读回比较用。

---

## 4. 服务循环与启动自检

### 4.1 `serve()`（fs_server.c:1481）
新增 6 个写消息分支，均在 `mach_msg_recv(FS_PORT)` 后分发；写类应答 `fr->length=0`，
文件名缓冲末尾强制 NUL 终止防越界读。
- `FS_MSG_WRITE` 解析：从 `fs_write_req_t` 后取文件名，计算 `avail` 数据上界，
  `len = min(wr->length, avail, FS_WRITE_MAX)`，指针数据取自 `name+namelen+1`（**用户态内
  部缓冲，非外部指针解引用**，符合内存安全红线；FS_SERVER 自身是数据拥有者）。

### 4.2 启动写路径自检（`fs_selftest`，fs_server.c:1399）
`main()` 在 `mount` + 列根目录后调用 `fs_selftest()`，覆盖：

| 用例 | 动作 | 校验 |
|------|------|------|
| write+readback | `fs_write("WRITETST.TXT", msg, len, 0)` 再读回 | 字节级 `fs_cmp` 一致 |
| append | 在 offset=len 处追加 `"APPEND"` | 读回 = msg+APPEND |
| mkdir+subdir | `fs_create("TESTDIR", true)` + `fs_write("TESTDIR/INNER.TXT", "inner ok\n", 8, 0)` | 子目录下文件读回一致 |
| rename | `fs_rename("WRITETST.TXT", "WT2.TXT")` | 新名读回内容一致，旧名不可见 |
| unlink | `fs_unlink("WT2.TXT")` + `fs_unlink("TESTDIR/INNER.TXT")` | `resolve_path` 返回 `found=false` |
| rmdir | `fs_unlink("TESTDIR")` | 目录消失 |
| truncate | `fs_write("TRUNC.TXT","0123456789",10,0)` 后 `fs_truncate("TRUNC.TXT",4)` | 读回 = `"0123"` |

全部 PASS 时打印 `[fs] write-path self-test: ALL PASS`（任一 FAIL 打印对应 FAIL 行并整体 FAIL）。
自检产生的临时文件在用例内清理（`fs_unlink`），不影响根目录持久状态。

---

## 5. Shell 交互写命令（`user/shell.c`）

- `mkfile <FILE>` → `FS_MSG_CREATE`；`mkdir <DIR>` → `FS_MSG_MKDIR`
- `write <FILE> <TEXT>` → `fs_request_write()`：组装 `FS_MSG_WRITE`（`offset=0`），数据紧邻文件名，
  `len = min(strlen, FS_WRITE_MAX)`；`fs_wait_status` 印 ok/I/O error。
- `rm <FILE>` → `FS_MSG_UNLINK`；`rename <OLD> <NEW>` → `FS_MSG_RENAME`（直接填 `fs_rename_req_t` 内联）；
  `truncate <FILE> <SIZE>` → `FS_MSG_TRUNCATE`（`size` 由 ASCII 数字解析，越界长度截断到 `FS_WRITE_MAX`）。
- 所有文件名经 `upcase` 转 8.3 大写，与 FS_SERVER 匹配；应答经 `SHELL_PORT` 异步回收，
  等待期间丢弃键盘字符（`MSG_ID_KEYCHAR` 跳过），不串扰。

---

## 6. 构建与验证

### 6.1 构建
```
make clean && make iso disk
```
退出码 0，**零 warning / 零 error**（修复了 `make_unique_83` 中未用变量 `bi` 告警）。

### 6.2 QEMU 运行验证（两磁盘后端）
- i440fx（传统 IDE PIO）：`-machine pc -cpu qemu64`
- q35（AHCI DMA）：`-machine q35 -cpu qemu64`

两种后端均捕获到 FS_SERVER 启动自检完整日志：
```
[fs] FS_SERVER starting (Ring3 FAT32, read/write)
[fs] FAT32 mounted: spc=... fat@... data@... root_clus=...
[fs] root directory (self-test):
[fs]   README.TXT  ...
[fs] write-path self-test begin
[fs]   write+readback PASS
[fs]   append PASS
[fs]   mkdir+subdir PASS
[fs]   rename PASS
[fs]   unlink PASS
[fs]   rmdir PASS
[fs]   truncate PASS
[fs] write-path self-test: ALL PASS
[fs] entering service loop on FS_PORT
```
- 无任何 `panic` / `triple fault`；交互 shell 下 `timeout` 退出码 124/143 为交互进程不主动退出所致，非故障（日志已完整捕获）。

### 6.3 关键安全/稳定性验证点
- **边界**：`disk_read`/`disk_write` 拒绝 `count==0 || >DISK_MAX_SECTORS`；`fat_set` 拒绝越界簇；
  `dir_patch` 拒绝 `off+len>SECTOR`。
- **环链防护**：所有 `fat_next` 遍历均受 `g_total_clusters` 与 `FAT_WALK_LIMIT` 双重限制。
- **错误闭环**：所有磁盘/簇操作失败均上报 `FS_ERR_IO`，无静默成功、无未处理错误返回。
- **一致性**：FAT 写回全部副本 + 读缓存失效；分配即占用；新簇整簇清零。
- **无 stub/TODO**：所有 7 项写功能均真实可工作并已自检通过。

---

## 7. 调用关系小结

```
main() → sys_port_claim(FS_PORT/FS_REPLY_PORT) → fat32_mount()
      → walk_root(selftest_cb) → fs_selftest()  → serve()
serve() ──FS_MSG_WRITE──> fs_write() ─> split_parent/dir_lookup/fs_create/
                           fat_alloc_cluster/fat_set/write_zero_cluster/
                           dir_entry_pos/cluster_at_index/patch_dir_size
        ──FS_MSG_CREATE/MKDIR──> fs_create()
        ──FS_MSG_UNLINK──> fs_unlink() ─> dir_is_empty/fat_free_chain/dir_patch
        ──FS_MSG_RENAME──> fs_rename() ─> make_unique_83/build_lfn_entry/dir_patch
        ──FS_MSG_TRUNCATE──> fs_truncate() ─> fat_alloc_cluster/fat_free_chain/patch_dir_size
        （以上均经 disk_write() → DISK_MSG_WRITE → 内核 disk_srv_task → blk_write）
```

---

## 8. 结论

P1-2 FAT32 写路径已**完整、健壮、生产可用**地实现于 Ring3 FS_SERVER：创建/写/追加/删/改名/
截断/子目录七项功能全部真实可工作，并经双磁盘后端（IDE PIO + AHCI DMA）QEMU 生产场景回归
`ALL PASS`，零 panic、零告警、零 stub。磁盘写协议此前已在内核侧就绪，本轮补齐用户态落盘全链路，
使操作系统具备完整的用户态文件系统读写能力。
