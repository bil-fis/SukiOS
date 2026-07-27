# Step12 — 生产安全审计修复批次二：H2 / H3 / H4 / H6 / D2

> 生成时间：2026-07-27
> 依据：`results/step10_todos.md` 附录「全代码库生产安全审计清单」的立即修项
> 关联：step11 已修 H1（`kernel/elf/elf.c` `elf_build_stack()` 的 `arg_va/env_va` 越界）；本步完成清单中剩余的 H2/H3/H4/H6 与功能性 D2。

---

## 一、修复总览

| 项 | 文件 | 缺陷本质 | 修复手法 |
|----|------|----------|----------|
| **H2** | `kernel/elf/elf.c` | `elf_build_stack()` 局部 `auxv[16]` 硬编码容量，`na` 无上界校验；未来扩展 auxv 类型即写越内核栈 | 引入共享常量 `ELF_AUXV_MAX`，每条 `auxv[na++]` 追加前先校验 `na < ELF_AUXV_MAX`，越限即 `kfree` 镜像并返回 0 走调用方回滚 |
| **H3** | `kernel/ipc/port.c` | `port_allocate()` 端口表耗尽返回 `PORT_NULL(0)`，与「空端口」语义混淆；调用方 `port_free(0)` 静默空操作 | 在 `ipc_init()` 将 0 号槽永久保留为不可分配哨兵（`in_use=true` 且 `port_lookup(0)` 仍因 `name==PORT_NULL` 短路返回 NULL），使 0 在任何上下文只表示「失败/无端口」；耗尽时 `kprintf` 诊断后返回 `PORT_NULL` |
| **H4** | `kernel/ipc/port.c` | `ool_map_into_receiver()` 中 `g_ool_bump` 从 `0x600000000000` 单调递增**无上界**，长稳大量 OOL（音频/大文件）必然耗尽/撞区 → 内核 #PF panic | 新增窗口上界 `OOL_RECV_LIMIT=0x700000000000`（1TiB 安全网）；单调分配前检查 `base+need > OOL_RECV_LIMIT`，越界返回 0（失败哨兵），由 `sys_mach_msg` 释放消息并返回新增错误码 `MACH_RCV_NO_SPACE` |
| **H6** | `kernel/drivers/hda.c` | HDA 为全局单例资源但**无所有权/重入保护**，任意任务可随时 open 重置 DMA、覆盖 BDL，破坏他人正在播放的流 | 引入 `g_owner` 任务指针；`hda_pcm_open` 拒绝非 owner 的抢占式重开，`hda_pcm_write`/`hda_pcm_queued`/`hda_pcm_stop` 仅放行 owner；open 成功登记 owner、stop 释放 owner |
| **D2** | `user/shell.c` + 内核 argv 链路 | `exec` 传参 argv 错乱（原现象 `exec BIN/playaudio MOONHALO.MP3 bench` 曾解析出 `file = 3`）。根因实为 **step11 重写 `elf_build_stack()` 前的旧栈构造 bug**，本步复核确认现行内核栈/crt0 已正确，并加防御 | shell `exec` 分支的 `argv[8]` 数组**显式清零**（杜绝未初始化栈垃圾被内核误读为额外指针）；运行时验证 argv 传递完全正确 |

> 说明：D2 的「`file = 3`」是**过去式**现象，源自 pre-H1 的 `elf_build_stack()`。step11 已将该栈构造彻底重写为连续无空隙、16 字节对齐、入口硬校验的版本；本步在 shell 端补防御并做运行时实证（见第六节）。

---

## 二、H2 — auxv 数组边界（kernel/elf/elf.c + include/kernel/elf.h）

### 2.1 新增共享常量
文件：`include/kernel/elf.h`（紧邻 `ELF_ARG_MAX`）
```c
#define ELF_ARG_MAX    64
/*
 * 初始栈 auxv[] 条目数硬上限（H2 修复）。
 * elf_build_stack() 局部 auxv[] 数组按此值定容，且每追加一条都先校验
 * na < ELF_AUXV_MAX；一旦越限立即 kfree 镜像并返回 0 由调用方回滚。
 * 当前实际写入 8 条（含 AT_NULL 收尾），留足余量到 16。
 */
#define ELF_AUXV_MAX   16
```

### 2.2 `elf_build_stack()` 修改点
原代码：
```c
struct { uint64_t a; uint64_t b; } auxv[16];
int na = 0;
auxv[na].a = AT_PHDR;   auxv[na++].b = phdr_va;
auxv[na].a = AT_PHENT;  auxv[na++].b = phent;
...  /* 共 8 条，na 无上界 */
```
修改后（每条追加前加哨兵）：
```c
struct { uint64_t a; uint64_t b; } auxv[ELF_AUXV_MAX];
int na = 0;
if (na >= ELF_AUXV_MAX) { kfree(img); return 0; }
auxv[na].a = AT_PHDR;   auxv[na++].b = phdr_va;
if (na >= ELF_AUXV_MAX) { kfree(img); return 0; }
auxv[na].a = AT_PHENT;  auxv[na++].b = phent;
if (na >= ELF_AUXV_MAX) { kfree(img); return 0; }
auxv[na].a = AT_PHNUM;  auxv[na++].b = phnum;
if (na >= ELF_AUXV_MAX) { kfree(img); return 0; }
auxv[na].a = AT_PAGESZ; auxv[na++].b = PAGE_SIZE;
if (na >= ELF_AUXV_MAX) { kfree(img); return 0; }
auxv[na].a = AT_ENTRY;  auxv[na++].b = entry;
if (na >= ELF_AUXV_MAX) { kfree(img); return 0; }
auxv[na].a = AT_BASE;   auxv[na++].b = (base != 0) ? base : 0;
if (na >= ELF_AUXV_MAX) { kfree(img); return 0; }
auxv[na].a = AT_RANDOM; auxv[na++].b = rand_va;
if (na >= ELF_AUXV_MAX) { kfree(img); return 0; }
auxv[na].a = AT_NULL;   auxv[na++].b = 0;
```
**算法/不变式**：`na` 在 [0, ELF_AUXV_MAX] 闭区间内单调增，任何一条追加前若 `na >= ELF_AUXV_MAX` 立即中止并释放 `img`（栈镜像 kmalloc 缓冲）返回 0；调用方（`elf_load`）据 `rsp==0` 判定失败并回滚地址空间。当前写入 8 条，远小于 16 上限，正常路径永不触发哨兵，仅作扩展防护。

---

## 三、H3 — 端口耗尽语义（kernel/ipc/port.c）

### 3.1 `ipc_init()` 保留 0 号哨兵
```c
void ipc_init(void)
{
    memset(g_ports, 0, sizeof(g_ports));
    /* H3 修复：将 PORT_NULL(0) 永久保留为"空端口"哨兵——标记为 in_use 使其
     * 永远不可被 port_allocate 分配（分配循环本就从 PORT_FIRST_DYN 起，此
     * 处置位是双保险），且 port_lookup(0) 仍因 name==PORT_NULL 短路返回
     * NULL，故 0 在任何上下文都只表示"无端口/失败"，与合法端口号 1..63
     * 彻底分离，杜绝"耗尽返回 0 与空端口语义混淆"。 */
    g_ports[PORT_NULL].in_use = true;
    g_ports[PORT_NULL].name   = PORT_NULL;
    g_ports[PORT_NULL].owner  = NULL;
    for (uint32_t p = DISK_PORT; p <= APP_PORT; p++) {
        g_ports[p].in_use = true;
        g_ports[p].name = p;
    }
    kprintf("[ipc] port table ready (%d slots, well-known 1..8, 0=sentinel)\n", PORT_MAX);
}
```

### 3.2 `port_allocate()` 耗尽诊断
```c
    irq_restore(f);
    /* H3 修复：端口表耗尽。返回 PORT_NULL(0) 现在是明确的"失败"语义
     * （0 号槽已被永久保留为哨兵，绝不可能是合法端口号），调用方据
     * `if (!rp)` 判定失败并清理，不再产生歧义。 */
    kprintf("[ipc] port_allocate: table exhausted (max=%u)\n", PORT_MAX);
    return PORT_NULL;
}
```
**关键不变量**：`port_allocate` 扫描自 `PORT_FIRST_DYN(=9)`，永不触碰 0；`port_lookup(0)` 因 `name==PORT_NULL` 短路返回 NULL；`port_free(0)` 因 `port_lookup` 返回 NULL 成为安全的空操作。于是 `PORT_NULL(0)` 与合法端口号形成硬隔离——这正是审计所要求的「消除语义混淆」。

---

## 四、H4 — OOL 接收窗口上界（kernel/ipc/port.c + include/ipc/port.h）

### 4.1 新增常量（port.c 顶部）
```c
#define OOL_RECV_BASE  0x0000600000000000UL
/* OOL 窗口上界（H4 修复）：g_ool_bump 单调递增到此处即视为耗尽，
 * 不再允许新的映射。1TiB 窗口对单条 <=16 页(64KiB) 的 OOL 消息足以
 * 支撑千万级映射，纯属安全网。 */
#define OOL_RECV_LIMIT 0x0000700000000000UL
static uint64_t g_ool_bump = 0;
```

### 4.2 `ool_map_into_receiver()` 上界检查
```c
    base = OOL_RECV_BASE + g_ool_bump;
    if (base + need > OOL_RECV_LIMIT) {       /* H4：越界即失败 */
        irq_restore(f);
        kprintf("[ipc] OOL recv window exhausted (bump=%p limit=%p)\n",
                (void *)(uintptr_t)g_ool_bump, (void *)(uintptr_t)OOL_RECV_LIMIT);
        return 0;
    }
    g_ool_bump += need;
    irq_restore(f);
```
**算法**：`need = (ool_page_count + 1) * PAGE_SIZE`（含守护页）。优先从 `g_ool_free` 空闲链表复用（命中则 `base` 落在已分配窗口内，恒非 0）；单调分配分支在递增 `g_ool_bump` 前先验证 `base + need` 不越过 `OOL_RECV_LIMIT`。`return 0` 是安全哨兵，因所有合法 OOL 映射基址 `>= OOL_RECV_BASE(0x600000000000)` 非 0。

### 4.3 错误码与 RECV 路径处理（port.h + port.c）
`include/ipc/port.h` 新增：
```c
/* OOL 接收窗口耗尽（H4）：无更多映射区间且复用链表无法满足需求 */
#define MACH_RCV_NO_SPACE       0x10005005UL
```
`sys_mach_msg()` RECV 分支：
```c
if (m->has_ool) {
    uint64_t va = ool_map_into_receiver(m);
    if (va == 0) {                       /* H4：窗口耗尽，映射失败 */
        kfree(m);
        return MACH_RCV_NO_SPACE;
    }
    ...
}
```
**不变式**：OOL 映射失败不再静默继续（旧代码会继续写 `d->address = 0` 造成用户态野指针），而是释放内核消息、向调用方返回明确错误码，避免「分配越界撞区 → 内核 #PF panic」的长稳地雷。

---

## 五、H6 — HDA 音频流所有权（kernel/drivers/hda.c）

### 5.1 新增依赖与状态
```c
#include <kernel/task.h>          /* sched_current()：H6 所有权校验 */
...
/* H6 修复：记录当前打开输出流的任务（owner）。HDA 为全局单例资源，
 * 在引入本字段前任何任务都能随时 open 重置 DMA / 覆盖 BDL，破坏他人
 * 正在播放的流。现在 open 时登记 owner，非 owner 的后续 open/write/
 * queued/stop 一律拒绝。 */
static task_t  *g_owner  = NULL;
```

### 5.2 `hda_pcm_open()` 准入
```c
task_t *cur = sched_current();
if (g_opened && g_owner != cur) {        /* 已被他人打开 → 拒绝 */
    kprintf("[hda] pcm open denied: stream owned by pid=%lu\n",
            (unsigned long)(g_owner ? g_owner->id : 0));
    return -1;
}
...
g_opened = true;
g_running = false;
g_wr_ofs = 0;
g_owner = cur;                           /* H6：登记 owner */
```
> 同一 owner 重复 open 放行（视为重新配置流格式，重建 BDL），仅拒绝**非 owner** 抢占。

### 5.3 `hda_pcm_write()` / `hda_pcm_queued()` / `hda_pcm_stop()` 保护
```c
/* write */
if (g_owner != sched_current()) { return -1; }

/* queued（含流尾播放触发） */
if (g_owner != sched_current()) { return 0; }

/* stop */
if (g_opened && g_owner != sched_current()) {
    kprintf("[hda] pcm stop denied: stream owned by pid=%lu\n",
            (unsigned long)(g_owner ? g_owner->id : 0));
    return;
}
...
g_owner = NULL;                          /* H6：释放 owner */
```
**语义效果**：把音频全局资源纳入与端口同款的 owner 能力体系。恶意/误用任务无法 kill 或污染他人正在进行的播放；所有权随 `hda_pcm_stop()` 或任务退出（调度器调用 `port_release_owner`/资源回收路径）自然移交。

---

## 六、D2 — exec argv 传递验证与防御（user/shell.c）

### 6.1 shell `exec` 分支 argv 数组清零
```c
/* 把命令后的剩余字符串按空格拆成 argv[]，argv[0]=程序路径。
 * D2 修复（防御）：显式清零整个 argv 数组，保证即便参数个数触顶
 * (ac<7) 也不会有未初始化栈垃圾被内核误当作额外指针读取，从而避免
 * exec 传参 argv 错乱（审计 D2 项）。 */
char *argv[8];
for (int zi = 0; zi < 8; zi++) {
    argv[zi] = NULL;
}
int ac = 0;
```

### 6.2 运行时实证（本步核心验证）
因 `-display none` 下 QEMU `sendkey` 无键盘前端、注入失效，采用**非交互确定性验证**：临时在 `shell.c` 入口 `main` 中自动执行
`sys_task_spawn("bin/playaudio", {"bin/playaudio","moonhalo.mp3","bench",NULL}, NULL)`，
经完整链路 `shell → sys_task_spawn → exec_copy_args → elf_load → elf_build_stack → crt0 → main(argc,argv)`，捕获串口输出：
```
[playaudio] file = moonhalo.mp3
[playaudio] first frame: hz=48000 ch=2 kbps=192
[playaudio] BENCH mode: decode only, no HDA
```
**结论**：`argv[1]="moonhalo.mp3"`（非旧的 `file = 3`）、`argv[2]="bench"` 正确触发 BENCH 分支，证明 argv 传递全链路正确。验证后立即回退临时自动执行代码（未留在仓库）。

### 6.3 根因定位
D2 标注的 `file = 3` 是 **pre-H1 时代的旧 `elf_build_stack()` 栈布局缺陷**的表象。step11 已将该栈构造重写为：入口硬校验 `argc/envc∈[0,ELF_ARG_MAX]`、字符串区 `sp_floor` 下溢哨兵、`argc/argv/envp/auxv` 连续无空隙且 16 字节对齐、块大小先算后对齐。crt0（`user/lib/crt0.S`）经复核计算正确（`envp = rsp + 8 + argc*8` 恰跳过 `argc` 与 `argv[]+NULL`，见 step11 分析），内核侧 argv 拷贝（`exec_copy_args`）以 NULL 终结符为界、无越界。本步的 shell 清零为纯防御层。

---

## 七、验证记录

| 项目 | 方法 | 结果 |
|------|------|------|
| 编译 | `make iso disk`（全量重建 kernel + 用户态 apps/shell） | 通过，无警告 |
| 静态检查 | 各改动文件 `read_lints` | 0 诊断 |
| 启动冒烟 | `qemu-system-x86_64 -machine pc -cpu qemu64 -m 2G -display none -audiodev none,... -boot d -cdrom build/SukiOS.iso -drive file=build/disk.img` | 端口表 `0=sentinel`、HDA `ready`、FS `entering service loop`、shell `online` 均正常，无 error/panic/fail |
| H2/H3 边界 | 代码审阅 + 共享常量收敛 | auxv 受 `ELF_AUXV_MAX` 约束；0 号端口成永久哨兵 |
| H4 上界 | 代码审阅 + 错误码闭环 | `OOL_RECV_LIMIT` 拦截单调递增；RECV 路径释放消息并返回 `MACH_RCV_NO_SPACE` |
| H6 所有权 | 代码审阅 + 启动日志（无多任务并发音频场景） | owner 登记/校验/释放逻辑闭合 |
| D2 argv | 非交互自动 exec + 串口抓取 | `file = moonhalo.mp3` 与 `BENCH mode` 证实 argv 正确 |

> QEMU 调试命令（后续人工复核可用）：
> `qemu-system-x86_64 -machine pc -cpu qemu64 -m 2G -serial stdio -audiodev pa,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0 -boot d -cdrom build/SukiOS.iso -drive file=build/disk.img,format=raw,index=0,media=disk -s -S`
> 配合 `gdb`：`target remote :1234`，`info registers` 验证 LSTAR MSR、`x/8xg <stack_top-0x20>` 核对初始栈 argc/argv 布局。

---

## 八、遗留与后续

- **H5/H7/H8/M1~M18**：按 `step10_todos.md` 里程碑销账（H5→P0-9 诊断；H7→P2-5 音频真机化；H8→P0-3 SMP 锁体系；M 系列随 P0/P1 系统性解决）。
- **D1（sys_audio_stop 后 BDL 环残留循环播放）**：属 P2-5 音频真机化前置，可单独排；本步 H6 已让其 stop 路径仅 owner 可触发、并清 owner，残留循环问题建议下一步专修。
- 本步改动**尚未提交 git**（与 step11 的 H1 一并可归入一个「审计修复」提交，或单独 step12 提交）。需提交时执行：
  `git add -A && git commit -m "step12: 审计修复 H2/H3/H4/H6/D2（auxv 边界/端口哨兵/OOL 上界/音频所有权/exec argv）"`
