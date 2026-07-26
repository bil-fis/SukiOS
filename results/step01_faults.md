# SukiOS 代码审查报告 — 简化项与生产环境风险清单 (step01_faults)

> 审查时间：2026-07-26
> 审查范围：`kernel/`（全部 .c + .S：vmm / pmm / kmalloc / ata / idt / sched / switch / syscall / port / boot）、`user/`（fs_server.c / shell.c / lib/suki.c）、`include/`（port.h / vmm.h / syscall.h / types.h）、`Makefile`、引导 `boot/boot.S`
> 结论：**当前系统是教学/原型级微内核实现，绝对不能用于生产环境。** 它能在 QEMU 2G 单核下演示“内核态 + Ring3 服务 + Mach 式 IPC + FAT32 只读”的完整链路，但存在多处可被用户态直接触发的致命崩溃、能力校验缺失与系统性资源泄漏。
> 下文按严重程度分级：🔴 致命（可被用户态利用或直接宕机）、🟠 严重（资源泄漏/生命周期错误/并发正确性）、🟡 功能简化（能力缺失，非单点 bug 但不可用于生产）。

---

## 总览（严重程度统计）

| 类别 | 条目 | 级别 |
|------|------|------|
| A. 安全/隔离 | A1 缺页无处理→坏指针宕机 | 🔴 |
| A. 安全/隔离 | A2 IPC 端口无能力校验 | 🔴 |
| A. 安全/隔离 | A3 OOL 引用计数/映射泄漏 | 🟠（但可被触发） |
| A. 安全/隔离 | A4 单等待者丢失唤醒 | 🟠 |
| B. 资源泄漏 | B1 进程退出不释放资源 | 🟠 |
| B. 资源泄漏 | B2 任务创建中途不回滚 | 🟠 |
| B. 资源泄漏 | B3 内核堆不收缩/无保护 | 🟠 |
| B. 资源泄漏 | B4 内核仅映射 0..4GB | 🔴（>4GB 硬件） |
| C. 隔离/硬化 | C1 用户页 W+X、无 SMEP/SMAP | 🔴 |
| C. 隔离/硬化 | C2 无 KPTI（Meltdown 侧信道） | 🟡→🔴 |
| C. 隔离/硬化 | C3 无 ASLR/KASLR | 🟡 |
| D. 功能简化 | D1 单核无 SMP | 🟡 |
| D. 功能简化 | D2 无 FPU/SSE 保存 | 🔴（用户用浮点即崩） |
| D. 功能简化 | D3 无缺页/COW/按需分页 | 🟡 |
| D. 功能简化 | D4 ATA PIO 仅读 LBA28 | 🟡 |
| D. 功能简化 | D5 FAT32 只读 8.3 | 🟡 |
| D. 功能简化 | D6 无动态进程创建 | 🟡 |
| D. 功能简化 | D7 无权限/网络/定时器 syscall | 🟡 |
| D. 功能简化 | D8 无 OOM 回收 | 🟡 |

---

## Part A — 安全/隔离类缺陷

### A1. 缺页异常无处理器 → 任意用户态“坏指针”即可让整个内核宕机 🔴

**技术细节**
- 中断分发器 `isr_dispatch()`（`kernel/arch/x86_64/idt.c:104-128`）：对 `vec < 32` 的 CPU 异常，仅当 `g_handlers[vec]` 非 NULL 时才调用已注册处理器；否则直接 `panic("Unhandled CPU exception %u")`。
- 缺页异常向量号 14。全局检索 `register_interrupt_handler` 的调用点：**没有任何代码注册 14 号异常**（`idt.c:80` 仅把外部 `isr0..isr47` 安装为 IDT 门，未注册 C 级 #PF 处理器）。`idt.c:122-126` 仅在 panic 前顺手打印 `CR2`，却不能恢复。
- `copy_from_user` / `copy_to_user`（`kernel/syscall/syscall.c:34-52`）的“红线”实现：先 `user_range_ok(uptr, n)` 做*范围*校验（`[0, USER_SPACE_TOP=0x00007FFFFFFFFFFF]`，`include/kernel/syscall.h:30`），通过就直接 `memcpy`。其自身注释明确写道（syscall.c:40）：“若页未映射将触发缺页并 panic —— 后续可升级为 EFAULT 恢复路径”。
- 系统调用进入时由 `IA32_FMASK=0x700`（`syscall_init.c:74`）清 `IF|TF|DF`，整个 syscall 处理期间中断关闭，缺页一旦发生，在关中断上下文里 `#PF` → panic，无机会投递 EFAULT。

**根因**：内核访问用户指针依赖“用户地址空间已映射该页”这一未被保证的前提；既没有 #PF 处理器，也没有 `get_user/put_user` 式的异常表（`.fixup`/`__get_user`）保护。

**可被触发的场景**：用户程序只要用 `[0, 0x7FFFFFFFFFFF]` 区间内一个*当前未映射*的地址（例如指向未分配区、BSS 之外的空洞、或故意越界）作为 `sys_debug_write(buf, len)` 的 `buf`，内核 `memcpy` 触碰该 VA 触发 #PF → panic → **整机死机**。这等价于“任意一个普通用户程序一条 `syscall` 即可让操作系统崩溃”（DoS，且无法恢复）。

**生产风险**：🔴 致命。任何用户态程序一个错指针即可宕机，完全不满足“用户/内核故障隔离”这一操作系统最低要求。

**修复建议**
1. 注册 `#PF` 处理器：区分“内核访问用户指针（来自 syscall）”与“真正的非法访问”。
2. 引入异常表机制（`__get_user`/`__put_user` 配合 `.section .fixup`），`copy_from_user` 改为逐字节安全访问，缺页返回 0（EFAULT）而非 panic。
3. 在 `copy_from_user` 拷贝前用 `vmm_translate(current->cr3, va)`（已有 `vmm_translate`，vmm.c:115）逐页校验存在性，缺页即返回 EFAULT。
4. 长期应实现按需分页 + COW（见 D3）。

---

### A2. IPC 端口无能力（权能）校验：任意任务可发往/接收任何端口（含内核端口） 🔴

**技术细节**
- 消息收发主入口 `sys_mach_msg()`（`kernel/ipc/port.c:237-316`）：
  - **发送路径**（port.c:240-277）：取 `dest = h->msgh_remote_port`，仅调用 `port_lookup(dest)`（port.c:56-62），后者只检查 `name < PORT_MAX && g_ports[name].in_use`。**不检查当前任务是否具有向该端口发送的权利**，也不检查端口类型（内核端口亦可被任意用户发往）。
  - **接收路径**（port.c:279-314）：`port_lookup((uint32_t)port)` 同样**完全不检查 `owner`**（注：内核旁路的 `ipc_recv_kernel` 是内核线程，另当别论）。
- `kernel_port_t` 结构体带 `owner` 字段（`include/ipc/port.h:87`），`disk_srv` 启动时调用 `port_set_owner(DISK_PORT, sched_current())`（`ata.c:171`）设置了 owner，但**发送/接收路径从未读取或校验 `owner`**——该字段形同虚设。
- 知名端口全部是全局固定号（`port.h:60-68`：`DISK_PORT=1`、`FS_PORT=2`、`SHELL_PORT=6`、`FS_REPLY_PORT=7` 等），任何任务都知道端口号。

**影响**
- 任意用户任务可 `MACH_SEND_MSG` 向 `FS_PORT`/`DISPLAY_PORT`/`INPUT_PORT` 注入伪造消息，扰乱正常服务；
- 任意任务可 `MACH_RECV_MSG` 从 `SHELL_PORT`/`FS_REPLY_PORT` 等“偷走”别的任务的应答（消息被其消费），造成服务死锁、信息泄露；
- 微内核的**基本隔离原则（端口权能）完全缺失**——这是 Mach/微内核架构的安全基石，缺此即失去微内核意义。

**生产风险**：🔴 致命（违反微内核隔离，可越权注入/窃听 IPC）。

**修复建议**：每个端口维护 `send_right`/`recv_right` 权能集；`sys_mach_msg` 发送前校验当前任务是否持有目标端口的发送权，接收前校验是否持有该端口的接收权（或 `owner` 为本任务）；引入端口权能传递（在消息中携带权能，接收方获得副本）。

---

### A3. OOL 物理页引用计数泄漏 + 接收方映射永不回收 🟠（但可被用户态反复触发）

**技术细节**
- 发送方 `ool_capture()`（port.c:192-214）对每个页 `pmm_incref(pa)`（pmm.c:124-130，引用计数 +1，零拷贝共享）。
- **发送失败路径泄漏**：`sys_mach_msg` 发送末尾 `uint64_t rc = deliver(dest, m); if (rc != MACH_MSG_SUCCESS) { return rc; }`（port.c:273-276）。`deliver()` 在 `invalid dest` 时内部已 `kfree(m)`（port.c:127-130）并返回，但**OOL 页的 `pmm_incref` 从未 `pmm_decref`** → 物理页引用计数永久虚高，无法回收。
- **接收路径泄漏**：`ool_map_into_receiver()`（port.c:217-230）把页 `vmm_map_page` 进接收方 `cr3`（带 `PTE_USER|PTE_NX`），但接收末尾仅 `kfree(m)`（port.c:313），**未对 `m->ool_pages` 做 `pmm_decref`，也从不 `vmm_unmap_page` 回收接收方地址空间里的映射**。
- `g_ool_bump` 单调递增（port.c:29，port.c:221-222），OOL 接收窗口从 `OOL_RECV_BASE=0x600000000000` 起只增不减，无空闲链表。

**影响**
- 每次收发 OOL：发送方仍保留原映射（语义上应是“移动”却退化成“共享”），接收方泄漏物理页引用与一段虚拟地址区间；
- 持续使用 OOL 后，PMM 引用计数虚高致物理页实际不可回收，接收方 OOL 窗口被耗尽 → 后续 OOL 必然失败甚至触发越界映射；
- 发送失败（dest 非法）也会被利用做确定性引用计数泄漏。

**生产风险**：🟠 严重（资源泄漏 / 生命周期错误，DoS 向量）。

**修复建议**：接收方使用完毕后 `pmm_decref` + `vmm_unmap_page`；OOL 采用 move 语义（发送方解除或递减自身权）；维护 OOL 窗口空闲链表而非单调 bump。

---

### A4. 单等待者模型 → 多等待者丢失唤醒（lost wakeup） 🟠

**技术细节**
- `kernel_port_t.waiter` 是**单个指针**（`include/ipc/port.h:86`）。
- `enqueue()`（port.c:103-106）只唤醒 `p->waiter` 并置 NULL。
- `sys_mach_msg` 接收（port.c:292）与 `ipc_recv_kernel`（port.c:181）都以**覆盖式**写入 `p->waiter = sched_current()`。若两个任务同时等待同一端口，第二个 `waiter` 直接覆盖第一个，且 `enqueue` 只唤醒链表头的那一个 → 被覆盖的等待者**永久 `WAITING` 饥饿**。

**影响**：多客户端并发等待某服务端口时，部分任务永不唤醒。

**生产风险**：🟠 严重（并发正确性问题）。

**修复建议**：等待者改为 FIFO 队列，入队/出队与唤醒基于此队列。

---

## Part B — 资源/内存泄漏与生命周期错误

### B1. 进程退出不释放任何资源（task_exit_current 从简保留） 🟠

**技术细节**
- `task_exit_current()`（`kernel/sched/sched.c:261-285`）：置 `alive=false`、摘除链表、直接 `context_switch` 到 next。**不** `kfree` task 结构、**不**释放内核栈、**不**释放页表（PML4/PD/PT）、**不**回收用户代码/BSS/栈物理页。注释（sched.c:272）明确：“不能立即 kfree 当前内核栈（正在其上运行），交由后续回收，此处从简保留”。全程关中断，无法在自身栈上释放自身。
- 前置缺陷：VMM 层**根本没有**地址空间销毁函数——`vmm.c` 仅有 `vmm_unmap_page`（vmm.c:99-113，单页解除），没有 `vmm_destroy_address_space`（递归释放 PML4/PD/PT 及所映射物理页）。因此即便想释放，也缺少基础设施。

**影响**：每个用户任务退出即泄漏其全部内存（代码页 + BSS + 栈 + 内核栈 + task_t + 各级页表）。长生命周期服务若重启/崩溃，内存单调泄漏直至 PMM 耗尽 → 整机不可用。

**生产风险**：🟠 严重。

**修复建议**：采用“僵尸态 + 回收者(reaper)”或引用计数；切换到别的任务栈后再 `kfree` 自身栈与 task_t；实现 `vmm_destroy_address_space` 递归撤销页表（先 `invlpg`/`tlb flush` 再还页给 PMM）。

---

### B2. 用户任务创建中途 PMM 失败不回滚 🟠

**技术细节**
- `task_create_user()`（sched.c:119-187）：依次循环分配代码页（129-145）、BSS（149-158）、用户栈（161-170），任一 `pmm_alloc_page()` 返回 NULL 即 `return NULL`。但此前已分配的若干物理页、以及已建好的新地址空间 `as`（sched.c:122 `vmm_create_address_space()`）**均不释放**。调用方（kmain 启动阶段）对 NULL 不处理 → `as` 与其已分配物理页泄漏。

**影响**：内存压力下创建任务导致确定性泄漏。

---

### B3. 内核堆 kmalloc 不收缩、无保护 🟠

**技术细节**
- `kmalloc.c`：首次适配 + 相邻合并（`split_block`/`kfree`）；`kheap_grow`（32-51）只增 `g_heap_end`，`kfree` 仅置 `free` 并合并，**从不把空闲物理页归还 PMM**。单核下 `kmalloc/kfree` 未加锁（无 cli/spinlock）。
- `kfree`（141-167）不做越界/重复释放/魔数校验，野指针 `kfree` 会静默破坏空闲链表。

**影响**：长期运行堆只增不减；多核或中断上下文调用会产生数据竞争；错误释放难以定位。

**修复建议**：引入边界 canary、空闲块着色、`shrink` 路径；多核需加锁或 per-CPU 堆。

---

### B4. 内核高半区仅映射 0..4GB 物理内存（>4GB 真机不可运行） 🔴

**技术细节**
- 引导页表（`boot/boot.S:51-65`）用 4 个 PD 的 2MB 大页把 **0..4GB** 同时做恒等映射与高半区映射（`PML4[256] -> p3_high -> p2_table`，覆盖 4GB 以便访问 LFB）。
- `vmm_init()`（vmm.c:167-172）只是 `read_cr3()` 保存内核 PML4，**没有**用精细 4KB 页表替换这套临时 2MB 大页，也没有扩展覆盖 >4GB。
- `pmm_init()`（pmm.c:35-95）依据 Multiboot2 mmap 把**全部**物理页计入位图（`g_total_pages = highest/PAGE_SIZE`，pmm.c:41），`pmm_alloc_page()`（pmm.c:97-111）从 `g_meta_end_phys` 起扫描全位图。若机器 >4GB，高地址页会被分配，经 `PHYS_TO_VIRT(p) = p + KERNEL_BASE`（types.h:20）落入**未映射的高半区** → 访问即 #PF → panic。
- `boot/boot.S` 注释与 `framebuffer.c:7` “引导页表已映射 0..4GB” 均自证仅映射低 4GB。

**影响**：在内存 >4GB 的真实硬件/服务器上，内核随时可能分配到高地址物理页并崩溃；QEMU 2G 下侥幸可跑，生产服务器不可。

**生产风险**：🔴 致命（硬件兼容性）。

**修复建议**：VMM 就绪后建立覆盖全部物理内存的内核页表（必要时动态扩展 PML4 项），并以 4KB 精细映射替换临时大页。

---

## Part C — 隔离/硬化缺失

### C1. 用户代码页 W+X，且无 SMEP/SMAP 🔴

**技术细节**
- `task_create_user()` 把代码页映射为 `PTE_PRESENT | PTE_WRITE | PTE_USER`（sched.c:143-144），即**用户页可写且可执行**；BSS/栈带 `PTE_NX`（157、169）但代码页没有 NX。
- `syscall_init()` 已置 `EFER.NXE=1`（syscall_init.c:65）启用 NX 位，但代码页并未设 NX。
- 内核态未启用 `CR4.SMEP(bit20)` / `CR4.SMAP(bit21)`（全程未见相关设置）。

**影响**：用户态任意可写内存即可当代码执行（flat binary 下 .data/.bss 与代码同段），数据注入→代码执行门槛极低；内核态无 SMAP 时误访问用户内存不会陷阱（与 A1 缺页缺失叠加 → 错误指针直接 panic 而非 EFAULT），无 SMEP 时若内核跳转用户地址会直接执行。

**修复建议**：代码页设 `PTE_NX` + 仅可执行；引入 ELF 装载按段拆分 R/W/X（W^X）；启用 `CR4.SMEP|SMAP`。

---

### C2. 内核半区全程映射进每个用户地址空间（无 KPTI） 🟡→🔴

**技术细节**
- `vmm_create_address_space()`（vmm.c:147-153）把 `PML4[256..511]`（内核高半区）原样复制进每个用户地址空间。这些项通过 supervisor 位（无 `PTE_USER`）禁止用户态直接访问，隔离“方向上正确”，但内核映射始终存在。

**影响**：在受 Spectre/Meltdown 影响的硬件上，用户态可通过侧信道读内核内存；微内核应做到最小暴露面。

**修复建议**：内核/用户页表分离（KPTI），syscall/中断切换 CR3。

---

### C3. 无地址空间随机化 🟡

固定加载基址 `USER_CODE_BASE=0x400000`（sched.c:27）、OOL 窗口 `0x600000000000`（port.c:28）、内核 `KERNEL_BASE=0xFFFF800000000000`（boot.S:16）。ROP/跳板利用地址完全可预测。

---

## Part D — 功能简化（能力缺失，不可用于生产但非单点 bug）

- **D1 单核无 SMP**：调度器/IPC/端口表全部假设单 CPU，`irq_save/restore` 用 `cli/sti`（port.c:32-43）仅在单核有效；无 AP 启动、无 LAPIC 定时器、仅 legacy PIT+PIC（`idt.c:130` 用 `IRQ_BASE`，PIT 在 `sched_tick`）。
- **D2 无 FPU/SSE/AVX 状态保存** 🔴：`context_switch()`（switch.S:20-35）仅保存 `rbx/rbp/r12-r15`，不 `fxsave/fxrstor`；用户任务一旦使用浮点/SSE 即触发 `#NM`（设备不可用），而 14 号之外该向量若未处理 → panic。即“用户程序用浮点即宕机”。
- **D3 无缺页/COW/按需分页**：所有用户页在创建时一次性分配，无法缺页（与 A1 互为表里）。
- **D4 ATA PIO 仅读**：`ata.c` 仅 `ata_read_sectors`（LBA28，`ata.c:141-145` 高 4 位 `0x0F`，上限 128GiB），无写、无 DMA、无 AHCI、仅 Primary Master、PIO 轮询（超时靠 `1000000` 次忙等）。
- **D5 FAT32 只读 8.3**：`fs_server.c` 仅解析 FAT32 根目录（`walk_root` 遍历 `clus>=2 && clus<0x0FFFFFF8`），仅 8.3 短名（忽略 LFN，`fs_server.c:147`）、无子目录遍历、无写入、无长文件名、无多分区。
- **D6 无动态进程创建**：`SYS_TASK_CREATE` 恒返回 -1（syscall.c:67-71）；系统仅含静态创建的 `fs_server`/`disk-srv`/`shell`（及可能的 `input_server`），用户无法启动新程序。
- **D7 无权限/账户/网络/用户态定时器**：无 uid/gid、无文件系统权限位、无网络栈、无 `nanosleep`/定时 syscall（仅靠 PIT 抢占）、无信号/异常投递给用户态（`#PF`/`#GP` 直接 panic 整机，`idt.c:127`）。
- **D8 无 OOM 回收**：`pmm_alloc_page` 返回 NULL 时多数调用方直接 `return NULL` 或 panic，无优雅回收（OOL 超限、任务创建失败等均无兜底）。

---

## Part E — 按文件的简化点/风险清单（速查表）

| 文件 | 行号 | 简化/风险 | 级别 |
|------|------|-----------|------|
| `kernel/arch/x86_64/idt.c` | 104-128 | #PF(14) 未注册处理器，任意坏指针 → panic 整机 | 🔴 |
| `kernel/syscall/syscall.c` | 34-52 | `copy_from_user` 仅范围校验，无逐页/异常处理，缺页 panic | 🔴 |
| `kernel/ipc/port.c` | 56-62, 280 | `port_lookup` 不查 owner；收发均无能力校验 | 🔴 |
| `kernel/ipc/port.c` | 273-276, 217-230, 313 | OOL 引用计数/映射泄漏、发送失败泄漏、`g_ool_bump` 单调 | 🟠 |
| `kernel/ipc/port.c` | 103-106, 181, 292 | 单 `waiter` 指针 → 多等待者丢失唤醒 | 🟠 |
| `kernel/sched/sched.c` | 261-285 | 进程退出不释放 task/栈/页表/用户页 | 🟠 |
| `kernel/sched/sched.c` | 119-187 | 创建中途 PMM 失败不回滚 `as` 与已分配页 | 🟠 |
| `kernel/mm/kmalloc.c` | 32-51,141-167 | 堆不收缩、无锁、无越界/重复释放保护 | 🟠 |
| `boot/boot.S` | 51-65 | 内核仅映射 0..4GB，>4GB 机器崩溃 | 🔴 |
| `kernel/sched/sched.c` | 143-144 | 用户代码页 W+X，无 NX | 🔴 |
| `kernel/arch/x86_64/switch.S` | 20-35 | 不保存/恢复 FPU/SSE/AVX，用户用浮点即 #NM→panic | 🔴 |
| `kernel/mm/vmm.c` | 147-153 | 无 `vmm_destroy_address_space`，地址空间不可拆 | 🟠 |
| `kernel/syscall/syscall.c` | 67-71 | `SYS_TASK_CREATE` 恒 -1，无动态进程 | 🟡 |
| `kernel/drivers/ata.c` | 132-159 | LBA28 仅读 PIO，无写/DMA/AHCI | 🟡 |
| `user/fs_server.c` | 133-159 | FAT32 只读、8.3、无子目录/LFN/写 | 🟡 |
| `kernel/mm/pmm.c` | 97-111 | 无 OOM 处理，NULL 直接传播 | 🟡 |

---

## Part F — 生产就绪性总体评级与优先修复顺序

**总体评级：不可用于生产（教学/原型级，单核 QEMU 演示用）。**

优先修复顺序（按“用户态可触发 → 系统级崩溃 → 资源泄漏 → 功能补齐”）：

1. **P0（致命，用户态可触发宕机）**：A1 注册 #PF + `copy_from_user` 异常表（EFAULT 而非 panic）；A2 加端口权能校验；B4 扩展内核映射覆盖 >4GB；C1 用户代码页 NX + 启用 SMEP/SMAP；D2 保存 FPU 状态。
2. **P1（严重，泄漏/并发）**：A3 OOL 引用计数与映射回收；A4 等待者队列化；B1 `task_exit` 资源回收 + `vmm_destroy_address_space`；B2 创建回滚；B3 堆保护/收缩。
3. **P2（硬化/功能）**：C2 KPTI；C3 ASLR/KASLR；D1 SMP；D3 缺页/COW；D4/D5 存储与文件系统能力；D6 动态进程；D7 权限/网络/定时 syscall；D8 OOM 回收。

> 说明：以上所有判定均基于 `results/` 审查时的代码（`results/step01.md` 记录的九个阶段为“能跑通演示链路”的验证，不等于生产安全）。后续每完成一项加固，应在 `results/stepNN.md` 增量记录修复点与复测方式。
