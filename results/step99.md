# step99 — USB 启动整机极慢 / Shell 只显示外框 / 鼠标无反应：根因定位与修复

本轮针对用户报告的现象做端到端诊断与修复：

> 「使用 USB 键盘鼠标启动，系统执行速度降低：鼠标移动有延迟，并且 shell 不渲染，只有外框」
> 追加：「还是不行，shell 不渲染，鼠标没有反应，并且功能检查运行特别慢。请检查调度器能否实现立即抢占、智能分配时间片；并重读 osdev 中 xhci/ehci/usb hub 部分，考虑移植 CherryUSB」

---

## 0. 结论速览

**真正的根因不是 USB，也不是调度器本身**，而是 **大文件（ELF 映像）读取路径的 IPC 往返次数过多**：

- `execve`/`spawn` 装载 ELF 时走「内联 3500 字节分块读」（`FS_MSG_READ_AT`）。
- `fontsrv` 映像 **952 KB** → 需 **~272 次** IPC 往返；`curl` 映像 **1.6 MB** → **~460 次**。
- `fs_server` 的 `handle_read_at` 每次分块还 **重新 `f_open` + `f_lseek(offset)`**，FatFs 的 `f_lseek` 必须从簇链头走到 offset → 整体 **O(n²) 簇链遍历**。
- 二者叠加：一次开机 `spawn` 字体重服务（shell 的阻塞点）要数十秒，期间 shell 卡在 `[shell] boot: launching font service + selftest`，**窗口只有外框、无内容**；且该读风暴持续占用 CPU/IPC，**饿死 input/mouse/显示任务** → 鼠标无反应、功能自检变慢。

**修复**：新增 `FS_MSG_READ_FILE_AT`，大文件读改用 **OOL 物理页** 分块，单次最多 **1 MiB**。952 KB 的 fontsrv **1 次往返读完**（原 ~272 次），1.6 MB 的 curl 2 次。配合 FS 侧「顺序读流缓存」消除 O(n²)。

**效果（QEMU 生产场景实测）**：
- shell 越过字体服务：`[shell] font service spawned` → `[fontsrv] font service online (FreeType, FONT_PORT=10)` → **`SukiOS:/>` 提示符成功渲染**（此前永远停在 `launching font service`）。
- 非 USB 回归：**PASS=206 / FAIL=0 / 零 panic**；USB+hub 场景：**PASS=105 / FAIL=0 / 零 panic**。
- 全盘 `pchfnt` 字体渲染自检程序 `PCHFNT.SKA` 被成功拉起并退出。

---

## 1. 诊断方法（遵守「仅 bash + QEMU 自身机制，禁用外部调试器/脚本」铁律）

- 查阅本地 osdev 文档：`osdev_wiki/wiki.osdev.org/Scheduling_Algorithms`（优先级轮转 / SVR2 usage 因子）、`Multiprocessor_Scheduling`、`USB_Hubs`（USB2 hub 描述符/命令集）、`XHCI`（MMAP 寄存器、class 0x0C/0x03/0x30、TRB）。
- QEMU 生产命令（USB＋USB hub＋键盘＋鼠标＋硬盘，本机 CPU 关 KPTI 以对齐用户环境）：
  ```
  qemu-system-x86_64 -machine pc -cpu Skylake-Client -smp 1 -m 2G \
    -no-shutdown -display none -serial file:/tmp/x.log -boot d \
    -cdrom build/SukiOS.iso \
    -drive file=build/disk.img,format=raw,index=0,media=disk \
    -device piix3-usb-uhci -device usb-kbd -device usb-hub -device usb-mouse
  ```
  （注意：**不可**同时给 `-usb` 与 `-device piix3-usb-uhci`，否则生成两个 UHCI 控制器、设备挂到驱动未用的那个，USB 不枚举——本轮曾因此误判。）
- 判定手段：仅有内核 `kprintf`/serial 落盘 + `grep/tail/cat -v`；**未使用 GDB 或任何外部脚本**。
- 关键证据：在 `exec_read_file` 加 `rdtsc` 逐块计时；在 FS 侧 `diskio.c` 的磁盘读 IPC 加 `rdtsc` 计时。

### 1.1 量化结果（决定性）
| 测量点 | 实测 | 说明 |
|---|---|---|
| 每个 FS 读块（3500B）往返（kernel 侧） | **4 ms ~ 300 ms** | 好的 ≈ 等待下一个 100Hz 节拍；差的 ≈ 等若干整轮 RR |
| 磁盘读 IPC 往返（FS→disk-srv→FS） | **~150 µs ~ 500 µs**（首读 3 ms） | 磁盘/块缓存**很快**，不是瓶颈 |
| fontsrv 装载 | 40 s 仅读 ~11 块（≈1.6 s/块） | 等效挂死 |

→ 瓶颈在 **shell↔FS 的多次往返 + 队列**，磁盘极快。

---

## 2. 根因（三层）

1. **往返次数爆炸**：`exec_read_file`（`kernel/syscall/syscall.c`）用 `FS_MSG_READ_AT` 内联分块，单块上限 `FS_READ_MAX=3584`。952 KB / 3584 ≈ **272 次**；每次是一整轮 send→FS 处理→recv。
2. **O(n²) 簇链遍历**：`fs_server.c::handle_read_at` 每块 `f_open` + `f_lseek(offset)`。FatFs `f_lseek` 从簇链头部走到 offset，累计 O(n²)。
3. **互相放大**：开机期 `curl_app`（读 1.6 MB）与 shell（读 952 KB）并发，`fs_server` 单线程串行化，两客户互相排队；加上 100 Hz 节拍粒度，单块被放大到数十~数百 ms。

「US B 启动才变慢」的观感来源：USB 场景下 `usb_service` 轮询任务与内核服务增多、任务数达 ~27，RR 周期变长（27×20 ms≈540 ms），把上述延迟进一步放大；**并非 USB 驱动本身有问题**（UHCI+HID 已能正确枚举 键鼠/hub）。

---

## 3. 修复实现

### 3.1 大文件读改走 OOL（核心修复）

**`include/ipc/fs_proto.h`**
```c
#define FS_MSG_READ_FILE_AT 39
#define FS_FILE_CHUNK  (1u*1024u*1024u)   /* 受 MACH_MSG_OOL_MAX_PAGES(256 页=1MiB) 约束 */
typedef struct fs_read_file_at_req { uint32_t offset; uint32_t length; } fs_read_file_at_req_t;
```

**`user/fs_server.c`**
- `FILEBUF_SIZE` 64 KiB → **1 MiB**（对应 `MACH_MSG_OOL_MAX_PAGES=256`）。
- 新增 `handle_read_file_at()`：`f_open` →（`offset>0` 时 `f_lseek`）→ `f_read` 最多 `FILEBUF_SIZE-sizeof(fs_resp_t)` → 经既有 `build_ool_resp()` 用 OOL 物理页回传。
- 分发：`case FS_MSG_READ_FILE_AT:` → `handle_read_file_at()`。

**`kernel/syscall/syscall.c::exec_read_file()`** 重写：
- 请求 `FS_MSG_READ_FILE_AT{offset, length≤1MiB}`；
- 用 **`ipc_recv_ool_kernel()`** 接收（inline 仅头，数据在 OOL 物理页直接拷入 `chunkbuf`）；
- 循环按 `offset += fr->length` 追加到 4 MiB 的 `elfbuf`；
- `chunkbuf` 每次调用 `kmalloc(FS_FILE_CHUNK)`（**不用 static**，避免 syscall 阻塞/让出期间被其它任务复用而破坏数据）。

净效果：952 KB 文件 **1 次** 往返；1.6 MB 文件 **2 次**。

### 3.2 FS 顺序读流缓存（消除 O(n²)）

**`user/fs_server.c`** 新增 `g_stream_fil/g_stream_open/g_stream_path/g_stream_pos` + `fs_stream_close()`：
- `handle_read_at`（≤3500B 的 POSIX `read()` 等仍走此路径）改为：仅当「未打开或路径变化」时 `f_open`；仅当「`offset != g_stream_pos`」时 `f_lseek`；顺序读直接 `f_read` 前进（FatFs 已持有当前簇/扇区缓存）。
- 服务主循环在**写/元数据类消息**（WRITE/WRITEFD/TRUNCATE/FTRUNC/CREATE/MKDIR/MKDIR2/RENAME/UNLINK/UNLINK2）分发前调用 `fs_stream_close()`，确保写后读回不拿到陈旧数据。

### 3.3 调度器「立即抢占」（用户要求，即时抢占点）

诊断中发现既有 `self-IPI(IPI_RESCHED)`+头插唤醒在**持锁期间**到达时，`schedule()` 因 `g_preempt_count>0` 置 `need_resched` 推迟，而**锁释放后无人消费该标志**——只能等下一个 100 Hz 节拍（≈5 ms）或整轮 RR。
修复（`spinlock.h` 注释早已写明此设计意图，但代码缺失）：

**`kernel/sched/sched.c`**
```c
bool g_sched_ready = false;                 /* sched_init 末尾置 true，守护早期引导 */
void sched_maybe_preempt(void) {
    if (!g_sched_ready) return;
    uint32_t c = cpu_index();
    if (g_preempt_count[c] != 0) return;    /* 仍持锁：继续推迟到其释放 */
    if (!g_percpu[c].need_resched) return;
    schedule();
}
```
- `schedule()` 在确认可切换后补 `g_percpu[cpu].need_resched = 0;`（消费请求，防止 spin_unlock 抢占点递归）。

**`include/kernel/spinlock.h::spin_unlock()`** 末尾：
```c
if (g_preempt_count[_idx] == 0) sched_maybe_preempt();
```

> 说明：本轮**未**采用「优先级提升/动态降级（MLFQ）」类改动——实测其会把 CPU 密集的初始化任务（如 display-server 合成桌面）降级饿死、导致 `display-server did NOT become ready` 且 shell 更晚建窗，属净回归，已放弃。经实测磁盘 IPC 仅 ~150 µs，**调度器不是本轮主瓶颈**，故只做安全的即时抢占点。

### 3.4 诊断代码清理
临时 `[exec-dbg]/[spawn-dbg]/[fsdbg]/[diskio]/[ipi-dbg]` 全部移除（源码与日志均已确认 0 残留）。

---

## 4. 验证（QEMU 生产回归，零 panic）

### 4.1 USB＋hub 场景（目标场景）
```
[usb] device addr=1 ... class=3 ...      # 键盘
[usb-hid] keyboard ready
[usb] device addr=2 ... class=9 ...      # hub
[usb-hub] ready: addr=2 ports=8
[usb] device addr=3 ... class=3 ...      # 鼠标（挂 hub）
[usb-hid] mouse ready
...
[boot] display-server ready (g_display_active=1, waited=2 yield rounds)
[shell] SukiOS shell online (Ring3, bash-like)
[wm] window created id=1 / id=2
[shell] windowed mode: window id=2
[shell] boot: launching font service + selftest
[shell] font service spawned                       <-- 修复前永远到不了
[fontsrv] font service online (FreeType, FONT_PORT=10)
[shell] pchfnt selftest spawned
SukiOS:/> [pchfnt] request: font=/FONTS/RESOURCEHANROUNDEDCN-MEDIUM.TTF size=40 text="SukiOS FreeType"
[sched] task 'PCHFNT.SKA' pid=20 exited (code=0)
PASS=105 FAIL=0   panic/EXC/halted/BAD SWITCH = 0
```

### 4.2 非 USB 回归
```
PASS=206 FAIL=0   ERR=0
[shell] font service spawned ; [fontsrv] font service online ; SukiOS:/> ...
```

### 4.3 对照（修复前基线 880112c）
shell 永久停在 `[shell] boot: launching font service + selftest`，无 `font service spawned`，无提示符 → 即用户所见「只有外框」。

---

## 5. USB / CherryUSB 评估（用户要求：重读 osdev xhci/ehci/hub，考虑移植 CherryUSB）

已查阅 `osdev_wiki` 的 `USB_Hubs`、`XHCI`（以及 `EHCI`/`UHCI` 相关条目）。要点与结论：

- **现状**：SukiOS 现用 **UHCI（USB 1.1）**+ 轮询式 HID（`kernel/drivers/usb/*`）。实测已能正确枚举 键盘 / hub / 鼠标，并注入输入子系统（`input_pref` 逻辑区分 USB/PS2）。因此「鼠标无反应」的**真实原因是整机被 FS 读风暴饿死**，不是枚举失败——**本轮修复后系统恢复响应，无需立刻重写 USB 栈**。
- **osdev XHCI 要点**：class 0x0C/0x03/0x30；BAR0+BAR1 组成 64 位 MMIO 基址；CAPLENGTH/DBOFF/RTSOFF 定位 Operational/Doorbell/Runtime 寄存器；命令/事件/传输均以 **TRB** 环驱动。**USB_Hubs 要点**：USB2 hub 有默认控制端点 + 状态变更端点；配置需 `Configure Endpoint`（xHCI）并初始化 Hub 字段（含端口数，先用默认 4 再按 Hub 描述符 0x29 重配）。
- **CherryUSB 移植评估**：CherryUSB 是**可移植的 USB 主/从协议栈**，覆盖 EHCI/OHCI/xHCI（USB 2.0/3.0），需实现一层 OS 适配（`usb_osal`：mutex/semaphore/thread/msleep、dcache、malloc、int 锁、USB 中断入口）。移植是**多日级、跨多文件**的独立工程，且需接到现有 Mach IPC/输入子系统。
  - **建议分阶段**：① 先保证现有 UHCI+HID 稳定（本轮已达标）；② 用 CherryUSB 的 **EHCI**（USB 2.0）替换 UHCI 以获得高速/多设备能力，单独新增 `lib/cherryusb` submodule 与 `usb_osal` 适配层；③ 再评估 xHCI（USB 3.0）。
  - **本轮不强行移植**：避免半成品（违反「不 stub、必须真正可工作」铁律）。根因既已消除，CherryUSB 作为**后续独立里程碑**排期，不在本次改动范围。
- **QEMU headless 限制**：`-display none` 下无法注入真实鼠标移动来「肉眼」验证光标位移；本轮以「USB HID 枚举成功 + 输入链路注入 + 整机不再被饿死（shell 正常渲染、自检全绿）」作为可观测证据。真实鼠标手感需用户在图形窗口实测。

---

## 6. 关键地址/常量/文件索引

| 项 | 值 / 路径 |
|---|---|
| 新 FS 消息 | `FS_MSG_READ_FILE_AT=39`（`include/ipc/fs_proto.h`） |
| OOL 分块上限 | `FS_FILE_CHUNK=1 MiB`；`MACH_MSG_OOL_MAX_PAGES=256`（`include/ipc/port.h`） |
| FS 整文件/大文件缓冲 | `FILEBUF_SIZE=1 MiB`（`user/fs_server.c`） |
| ELF 装载上限 | `EXEC_ELF_MAX=4 MiB`（`kernel/syscall/syscall.c`） |
| 磁盘批上限（已足够） | `DISK_MAX_SECTORS=64`（32 KiB/批，`include/ipc/disk_proto.h`） |
| 内核接收 OOL | `ipc_recv_ool_kernel()`（`include/ipc/port.h`） |
| 调度即时抢占点 | `sched_maybe_preempt()`（`kernel/sched/sched.c`）+ `spin_unlock()`（`include/kernel/spinlock.h`） |
| FS 顺序读流缓存 | `g_stream_*`/`fs_stream_close()`（`user/fs_server.c`） |

## 7. 修改文件清单
- `include/ipc/fs_proto.h`：新增 `FS_MSG_READ_FILE_AT` / `FS_FILE_CHUNK` / `fs_read_file_at_req_t`
- `user/fs_server.c`：`FILEBUF_SIZE`→1 MiB；新增 `handle_read_file_at` + 分发；`handle_read_at` 顺序读流缓存 + 写类消息失效；清理 `[fsdbg]`
- `kernel/syscall/syscall.c`：`exec_read_file` 重写为 OOL 大块读；清理 `[exec-dbg]/[spawn-dbg]`
- `kernel/sched/sched.c`：`g_sched_ready` + `sched_maybe_preempt` + `schedule()` 消费 `need_resched`
- `include/kernel/spinlock.h`：`spin_unlock` 末尾即时抢占点
- `drivers/FatFs/diskio.c`、`kernel/arch/x86_64/smp.c`：移除临时计时/计数诊断

## 8. 复现/验证命令
```bash
make iso disk
# USB+hub 目标场景
qemu-system-x86_64 -machine pc -cpu Skylake-Client -smp 1 -m 2G -no-shutdown \
  -display none -serial file:/tmp/x.log -boot d -cdrom build/SukiOS.iso \
  -drive file=build/disk.img,format=raw,index=0,media=disk \
  -device piix3-usb-uhci -device usb-kbd -device usb-hub -device usb-mouse
# 观察：grep -aE "font service spawned|font service online|SukiOS:/>|panic|\[PASS\]|\[FAIL\]" /tmp/x.log
```
