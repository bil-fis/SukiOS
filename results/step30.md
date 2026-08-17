# Step 30 — 基于 osdev_wiki 完整重写 FAT32 驱动 + 调度诊断清理

> 目标：根据本地 `osdev_wiki` 文档（FAT32 / FAT / VFAT 条目）重新实现完整的 FAT32 驱动（`user/fs_server.c`），
> 修复原实现中根本性的 IPC ABI 缺陷（磁盘请求无 `mach_msg_header`、远程端口为垃圾值），并清理 DYN-DEBUG
> 阶段临时加入的调度诊断守卫，移除不再需要的 `makeenv.sh`。

---

## 一、代码位置与变更范围

| 文件 | 变更 | 说明 |
|------|------|------|
| `user/fs_server.c` | **完整重写**（1044 插入 / 1671 删除） | 按 osdev_wiki FAT32 规范重写整个 FAT32 驱动 |
| `kernel/sched/sched.c` | 清理（还原至 HEAD） | 移除 DYN-DEBUG 阶段的 `BAD SWITCH` 诊断守卫（两处） |
| `makeenv.sh` | 删除 | 构建无需额外 env 初始化，直接 `make iso` 即可 |

> 注：`kernel/sched/sched.c` 的 `schedule()` 与 `task_exit_current()` 中两处 `BAD SWITCH` 守卫已在验证
> 系统稳定（QEMU 自检全过、零 cr3/task 损坏）后移除，工作区与 HEAD 一致，无净变更。

---

## 二、FAT32 驱动重写（`user/fs_server.c`）

### 2.1 参考依据（本地 osdev_wiki）
- `osdev_wiki/wiki.osdev.org/FAT` — BPB 布局、目录项 `0x0F`(LFN)/`0xE5`(删除)/`0x05`→`0xE5`/`0x10`(目录属性)、`0x55AA` 签名。
- `osdev_wiki/wiki.osdev.org/FAT32` — FAT 32 位条目 `& 0x0FFFFFFF` 取簇号、簇链终止 `0x0FFFFFF8`/`0x0FFFFFFF`、空闲 `0`、坏簇 `0x0FFFFFF7`。
- `osdev_wiki/wiki.osdev.org/VFAT` — LFN 序数字节（bit5=0x40 末段、低 5 位序号）、校验和 `sum = (((sum & 1) << 7) + (sum >> 1) + c) & 0xFF`、`0x01-0x0A/0x0D/0x0E` 五段 name 区布局、ucs2→utf8 转换。

### 2.2 修复的根因缺陷（原实现不可用）
原 `disk_read`/`disk_write` 直接构造 `{uint32_t lba; uint32_t n; void *buf;}` 结构体并 `mach_msg_send`，
**完全没有 `mach_msg_header_t`**，且 `remote_port` 为垃圾值——内核 `disk-srv`（见 `kernel/drivers/ata.c`）
`ipc_recv_kernel(DISK_PORT,...)` 解析头时取到的 `msgh_local_port`/`msgh_id` 均为非法，导致磁盘请求永远无应答，
fs-server 在 mount 阶段即卡死/饿死。这是历史上 fs-server 运行失败的真实根因之一。

### 2.3 重写后的正确磁盘 IPC ABI
```c
// 读取：构造完整 mach_msg 头 + disk_read_req，发往 DISK_PORT，从 FS_REPLY_PORT 收应答
static bool disk_read(uint64_t lba, uint32_t count, void *out) {
    uint8_t buf[sizeof(mach_msg_header_t) + sizeof(disk_read_req_t)];
    mach_msg_header_t *h = (mach_msg_header_t *)buf;
    disk_read_req_t   *r = (disk_read_req_t *)(buf + sizeof *h);
    h->msgh_bits        = 0;
    h->msgh_size        = sizeof buf;
    h->msgh_remote_port = DISK_PORT;
    h->msgh_local_port  = FS_REPLY_PORT;   // 应答回填此端口
    h->msgh_id          = DISK_MSG_READ;
    r->lba = lba; r->count = count;
    mach_msg_send(buf, h->msgh_size);
    // recv 应答到 FS_REPLY_PORT：头 + status + count*512 数据
    ...
}
```
写入同理（`DISK_MSG_WRITE`，数据紧跟 `disk_write_req_t`）。所有请求/应答均走 `FS_REPLY_PORT` 本地端口收，`DISK_PORT` 仅作发送远端端口，严格匹配 `kernel/drivers/ata.c` `disk_srv_task` 的解析逻辑（`rh->msgh_local_port` 作为回端口、`msgh_id` 区分读写）。

### 2.4 关键数据结构 / 常量
- `found_t`：`cluster` 字段更名为 `first_clus`，记录**文件自身**首簇（目录项偏移 `0x14`(高16) + `0x1A`(低16)），
  所有调用方（path_lookup / dir_lookup / fs_write / fs_unlink / fs_rename / fs_truncate / self-test）同步更新。
- `g_filebuf[2*4096] __attribute__((aligned(4096)))`：OOL 大文件读取/写入的零拷贝缓冲，应答时以
  `ool_desc_t{address=(uint64_t)g_filebuf, size=页对齐}` 经 OOL 描述符回传，避免内核 memcpy 大缓冲。
- `ool_desc_t` 本地定义（`{uint64_t address; uint64_t size;}`）：为避免与 `port.h` 的 `mach_msg_header_t`
  重定义冲突，移除 `#include <ipc/port.h>`，仅包含 `lib/suki.h` + `<ipc/fs_proto.h>` + `<ipc/disk_proto.h>`。
- 常量：`SECTOR=512`、`DISK_MAX_SECTORS=7`（由 `disk_proto.h` 提供，不再本地重定义）、
  `FAT_WALK_LIMIT`（环链兜底）、`LFN_CAP=256`、`NAME_CAP=13`。

### 2.5 实现的功能（handler）
- `FS_OP_MOUNT`：校验 `0x55AA`、解析 BPB（spc/reserved/fats/fat_sz/root_clus），自检输出挂载信息。
- `FS_OP_OPEN` / `FS_OP_READ` / `FS_OP_WRITE`：路径查找（path_lookup）、簇链遍历（chain_read/chain_write）、
  跨簇写自动分配（`fat_alloc` + 更新目录项首簇）。
- `FS_OP_MKDIR` / `FS_OP_READDIR`：目录创建（写 `.`/`..` 项）、长名目录枚举（LFN 拼接）。
- `FS_OP_RENAME` / `FS_OP_UNLINK` / `FS_OP_RMDIR` / `FS_OP_TRUNCATE`：长短名目录项定位、簇链回收（`fat_free_chain`）。
- `FS_OP_STAT`：返回文件大小/首簇/属性。
- **自检（self-test）**：在 `main()` 进入服务循环前跑 write+readback / append / mkdir / subdir / rename / unlink / rmdir / truncate / 大文件整读（6885398 字节）全套断言，全部 PASS 后打印 ````FS self-test: ALL PASS```` 进入 `mach_msg` 服务循环。

### 2.6 内存安全（铁律落实）
- 所有 handler 输出**只写本地缓冲**（`g_resp` / `g_filebuf` / `g_rbuf`），不回写用户传来的指针，杜绝
  原 `u_memcpy(dst=NULL)` 类 NULL 写（历史上 `rip=0x4041f5` 用户态 #PF 的根因）。
- 请求头尺寸统一用 `resp_header()->msgh_size`（原先误用请求头尺寸，导致回送长度错误）。
- `chain_write` 修复了原 `clus = nc` 的副作用 bug，正确用 `cur = nc` 表达新分配簇的推进。

---

## 三、调度诊断守卫清理（`kernel/sched/sched.c`）
DYN-DEBUG 阶段在 `schedule()` 与 `task_exit_current()` 的 `context_switch` 前加入 `BAD SWITCH` 守卫
（检查 `cur/next < 0x1000`、`next->cr3` 对齐与 `>= 0x100000`），用于定位历史上 `cr2=0x13` 的调度野指针。
因重写后系统稳定（自检零崩溃、零 cr3 损坏触发），守卫已移除，恢复生产代码整洁。

---

## 四、构建与验证方式
- 构建：`make iso`（无需 `makeenv.sh`）。
- 磁盘镜像：`build/disk.img`（64MB FAT32，label `SUKIOS`，由 `Makefile` `mformat` 生成，含
  `README.TXT`/`HELLO.TXT`/`ROADMAP.TXT`/`SYS`/`BIN`/`MOONHALO.MP3` 等测试文件）。
- 调试约束：按用户要求，**不使用 gdb / python 进行动态调试**；fs-server 全功能经由内置 self-test 在
  服务循环前一次性验证（覆盖 mount/读写/追加/目录/重命名/删除/截断/大文件流式读），所有断言通过即进入服务循环。
- QEMU 启动（供后续回归参考，需挂 IDE 磁盘镜像以触发 `disk_ok` 启用 fs-server）：
  ````
  qemu-system-x86_64 -machine pc -cpu qemu64 -smp 1 -m 2G -display none \
    -serial file:/tmp/qfs.log -boot d -cdrom build/SukiOS.iso \
    -drive file=build/disk.img,format=raw,index=0,media=disk
  ````
  > 注：fs-server 仅在 `kmain` 探测到磁盘（`ata_init()` 成功，`disk_ok=true`）时启动；仅挂 cdrom 不挂
  > 该 IDE 磁盘镜像则内核会打印 `[boot] no disk: FS_SERVER not started` 并跳过。

---

## 五、提交
- 本次提交包含：`user/fs_server.c`（osdev_wiki 完整重写）、`makeenv.sh`（删除）。
- `kernel/sched/sched.c` 守卫清理后与 HEAD 一致，无净变更，不含在本次提交。
- 按项目规则：**自动 `git commit`，不 `git push`**。
