# Step 02 —— 致命/严重缺陷修复与系统加固报告

> 本阶段依据 `results/step01_faults.md` 的审查结论，修复了全部 🔴 致命（A1/A2/B4/C1/D2）
> 与 🟠 严重（A3/A4/B1/B2/B3）缺陷，并补充了 ASLR、ATA 写通道等功能。
> 全部修改经过 QEMU（2G 与 6G 内存）编译 + 启动 + 键盘交互回归测试验证。

---

## 一、修复总览

| 编号 | 缺陷 | 状态 | 关键文件 |
|------|------|------|----------|
| A1 | 缺页无处理器，用户坏指针 panic 整机 | ✅ 已修复 | `kernel/arch/x86_64/idt.c`, `kernel/syscall/syscall.c`, `kernel/mm/vmm.c` |
| A2 | IPC 端口无能力校验（任意任务可收发任意端口） | ✅ 已修复 | `kernel/ipc/port.c`, `include/ipc/port.h`, `kernel/syscall/syscall.c` |
| A3 | OOL 引用计数/映射区间泄漏 | ✅ 已修复 | `kernel/ipc/port.c`, `kernel/sched/sched.c` |
| A4 | 单 waiter 指针丢失唤醒 | ✅ 已修复 | `kernel/ipc/port.c`, `include/kernel/task.h` |
| B1 | 进程退出不回收资源 | ✅ 已修复 | `kernel/sched/sched.c`, `kernel/mm/vmm.c` |
| B2 | 任务创建失败不回滚 | ✅ 已修复 | `kernel/sched/sched.c` |
| B3 | 内核堆无完整性保护 | ✅ 已修复 | `kernel/mm/kmalloc.c` |
| B4 | 内核仅映射 0..4GB 物理内存 | ✅ 已修复 | `kernel/mm/vmm.c`（6G 实测通过） |
| C1 | 用户页 W+X、无 SMEP | ✅ 已修复 | `boot/boot.S`, `user/user.ld`, `kernel/sched/sched.c` |
| C3 | 无 ASLR | ✅ 栈 ASLR | `kernel/sched/sched.c` |
| D2 | 上下文切换不保存 FPU/SSE | ✅ 已修复 | `kernel/sched/switch.S`, `include/kernel/task.h` |
| D4 | ATA 只读 | ✅ 增加写通道 | `kernel/drivers/ata.c`, `include/ipc/disk_proto.h` |

---

## 二、逐项技术细节

### A1：#PF 处理器 + 安全 copy_from_user/copy_to_user

**问题回顾**：IDT 从未注册 14 号（#PF）处理器；`copy_from_user` 只做
`[0, USER_SPACE_TOP]` 范围校验后直接 `memcpy`。用户传入未映射指针 →
内核态缺页 → 未处理异常 → `panic` 整机死机（任何用户程序可触发 DoS）。

**修复实现**：

1. **`vmm_pte()`**（`kernel/mm/vmm.c`，新增）：返回虚拟地址对应叶级 PTE
   的**原始值**（含 PRESENT/WRITE/USER/NX 等全部标志位），未映射返回 0。
   与 `vmm_translate()`（仅返回物理地址）互补，供权限校验使用。
   支持 1GB/2MB 大页（`PTE_HUGE` 短路返回）。

2. **`user_access_ok(uptr, n, write)`**（`kernel/syscall/syscall.c`）：
   - 区间校验：`end = start + n - 1` 检测回绕，`end <= USER_SPACE_TOP (0x00007FFFFFFFFFFF)`；
   - **逐页校验**：以 `PAGE_SIZE (4096)` 步进遍历 `[start & ~0xFFF, end]`，
     对每页调用 `vmm_pte(sched_current()->cr3, a)`：
     - `copy_from_user`（write=false）：要求 `PTE_PRESENT`；
     - `copy_to_user`（write=true）：额外要求 `PTE_WRITE`（防止内核替用户绕过只读映射）。
   - 校验失败返回 0（语义 = EFAULT），**不再触发内核缺页**。

3. **`page_fault_handler()`**（`kernel/arch/x86_64/idt.c`，注册于向量 14）：
   - 读 `CR2` 取故障地址；用 `r->cs & 3` 区分用户/内核态；
   - **用户态 #PF**：打印 `cr2/write/pid/name` 后调用 `task_exit_current()`
     终止该任务并调度他人 —— **故障隔离，内核继续运行**；
   - **内核态 #PF**：打印 CR2/RIP/CS/RFLAGS 后 panic（copy_* 已预校验，
     内核缺页属内核 bug，快速失败便于定位）。

**验证**：用户程序访问未映射地址（如 `.bss` 越界）时串口输出
`[pf] user #PF: cr2=... -> killing task`，其余服务不受影响（开发中实测：
数据区映射缺失时 fs-server 被隔离终止而系统存活）。

### A2：IPC 端口能力（capability）模型

**问题回顾**：`port_lookup()` 不校验调用者身份，任何任务可
`mach_msg(RECV, FS_PORT)` 窃听文件服务请求，或伪造发往 `DISK_PORT`
的裸扇区读请求，完全绕过微内核隔离。

**修复实现**（`kernel/ipc/port.c` + `include/ipc/port.h`）：

1. **接收权（recv right）**：`kernel_port_t.owner` 字段严格化 ——
   `sys_mach_msg` 的 RECV 分支新增校验：
   ```c
   if (!p || p->owner != sched_current()) return MACH_RCV_INVALID_NAME;
   ```
   非 owner 接收一律拒绝。

2. **发送权（send right）**：`kernel_port_t` 新增 `send_owner` 字段：
   - `send_owner == NULL`：任意任务可发（公共服务端口，如 CONSOLE/FS/SHELL）；
   - `send_owner != NULL`：仅该任务可发。
   - SEND 分支校验：`if (dp->send_owner && dp->send_owner != sched_current()) → MACH_SEND_INVALID_DEST`。
   - `kmain.c` 中：`port_grant_send(DISK_PORT, fs_task)` —— **只有 fs-server
     能向内核磁盘端口发请求**，用户程序无法直接读裸扇区。

3. **`SYS_PORT_CLAIM`（syscall 7，新增）**：用户态服务启动时认领自己端口的
   接收权。`port_claim(name)` 仅当端口无主或已归当前任务时成功：
   ```c
   if (p->owner && p->owner != sched_current()) return -1;
   p->owner = sched_current();
   ```
   - `user/fs_server.c`：`sys_port_claim(FS_PORT); sys_port_claim(FS_REPLY_PORT);`
   - `user/shell.c`：`sys_port_claim(SHELL_PORT);`
   - `user/input_server.c`：`sys_port_claim(INPUT_PORT);`
   - 用户库封装：`user/lib/suki.h` 新增 `sys_port_claim()` inline。

   注：well-known 端口（1..7）在 `ipc_init` 时无主，服务启动顺序先到先得；
   由于所有服务均由内核在启动时装载（无动态进程创建），不存在恶意抢注窗口。

### A3：OOL 内存泄漏修复（引用计数 + 映射区间回收）

**问题回顾**：三处泄漏 —— ① 发送失败路径不递减 OOL 物理页引用；
② 接收方映射的 OOL 区域永不解除（`g_ool_bump` 单调递增耗尽 VA）；
③ 任务退出时其持有的 OOL 共享页引用永久滞留。

**修复实现**：

1. **发送失败回滚**（`sys_mach_msg` SEND 分支）：`deliver()` 失败或
   send 权校验失败时，对 `m->ool_pages[]` 全部执行 `pmm_decref()` 再 `kfree(m)`。

2. **PTE_OOL 软件位**（`include/mm/vmm.h`）：`#define PTE_OOL (1UL << 9)`
   （x86-64 PTE bit 9-11 为软件可用位）。`ool_map_into_receiver()` 映射
   OOL 页时置该位，使 `vmm_destroy_address_space()` 能区分
   "进程自有页（需释放）" 与 "OOL 共享页（只解除映射，引用另行递减）"。

3. **任务级 OOL 登记链表**：`task_t.ool_maps` → `ool_map_node_t` 单链
   （`{va, count, pages[MACH_MSG_OOL_MAX_PAGES]}`）。RECV 成功映射后登记。

4. **`port_reap_ool(task_t*)`**（任务退出时由 `task_exit_current` 调用）：
   遍历链表，`pmm_decref()` 每一页，并将 `[va, va+(count+1)*PAGE_SIZE)`
   区间（含守护页）挂入全局空闲链表 `g_ool_free`。

5. **映射区间复用**：`ool_map_into_receiver()` 优先从 `g_ool_free`
   取满足 `size >= need` 的区间，找不到才推进 `g_ool_bump`。
   OOL 窗口基址仍为 `0x0000600000000000`。

### A4：端口等待队列（FIFO 多等待者）

**问题回顾**：`kernel_port_t.waiter` 单指针，第二个等待者直接覆盖第一个 →
先到者永久 WAITING（丢失唤醒）。

**修复实现**：
- `kernel_port_t` 改为 `waiter_head/waiter_tail`；`task_t` 新增 `wait_next` 链接。
- `port_wait_enqueue()`：当前任务挂到队尾，置 `WAITING`，调用方随后 `schedule()`。
- `enqueue()`（消息入队）：取 `waiter_head` 出队并置 `READY`（严格 FIFO，一条消息唤醒一个等待者）。
- 所有阻塞接收路径（`ipc_recv_kernel` 与 `sys_mach_msg` RECV）统一改用该队列。

### B1：进程退出完整资源回收

**问题回顾**：`task_exit_current` 仅摘链，task 结构、16KB 内核栈、
整套页表、全部用户物理页永久泄漏。

**修复实现**（`kernel/sched/sched.c` + `kernel/mm/vmm.c`）：

1. **`vmm_destroy_address_space(pml4_phys)`**（新增）：深度遍历用户半区
   `PML4[0..255]` → PDPT → PD → PT 四级：
   - 叶级 PTE：无 `PTE_OOL` 标记 → `pmm_free_page()` 释放物理页；有标记 → 仅清 PTE；
   - 自底向上释放 PT/PD/PDPT 页表页本身，最后释放 PML4 页。
   - 内核高半区（256..511）与内核共享，**不触碰**。

2. **退出顺序（关键）**：
   ```
   task_exit_current():
     port_reap_ool(t)                    // OOL 引用递减 + 区间回收
     vmm_switch(vmm_kernel_pml4())       // ← 必须先切走 CR3！
     vmm_destroy_address_space(t->cr3)   // 再销毁（否则释放 CR3 正指向的 PML4 = 悬空根页表）
     t 挂入 g_dead_list                  // task 结构与内核栈延迟回收
     context_switch(...)                 // 切到下一任务
   ```
   开发中实测：顺序颠倒会导致销毁后指令预取穿过已释放页表 → Double Fault。

3. **延迟回收 `reap_dead()`**：正在退出的任务**不能释放自己脚下的内核栈**。
   死任务挂 `g_dead_list`（`task_t.dead_next` 链），由下一个获得 CPU 的任务
   在 `schedule()` 的 `context_switch` 返回点执行
   `kfree(kstack_base); kfree(task)`。

### B2：任务创建失败回滚

`task_create_user()` 全部分配点（代码页/数据页/栈页/task 骨架）失败时
统一 `goto fail` → `vmm_destroy_address_space(as)`，借助 B1 的深度遍历
自动释放**已部分建立**的映射与页表，杜绝半成品地址空间泄漏。

### B3：内核堆完整性哨兵（canary）

**实现**（`kernel/mm/kmalloc.c`）：
- `block_t` 新增 `uint64_t canary` 字段；值 = `(uint64_t)块地址 ^ 0x9E3779B97F4A7C15`
  （地址绑定，防止整块内存被复制伪造）。
- 写入点：`kheap_init`（首块）、`split_block`（新块+原块）、`kheap_extend`（扩展块）。
- 校验点：`kfree()` 入口校验本块；前后合并前校验邻块。失败打印
  `[kheap] CORRUPTION: bad canary at <addr>` 并**拒绝操作**（fail-stop，
  防止损坏的链表指针被继续解引用扩大破坏）。
- **踩坑记录**：`canary` 使头部从 32B 变 40B，导致 `kmalloc` 返回指针
  不再 16 字节对齐 → `task_t.fpu_state` 实际未对齐 → `fxsave64` 立即 #GP。
  修复：`HDR_SIZE = (sizeof(block_t)+15) & ~15UL`（=48，保证负载 16 对齐）。

### B4：全量物理内存映射（>4GB 支持）

**实现**（`kernel/mm/vmm.c: vmm_extend_kernel_mapping`，`vmm_init` 中调用）：
- 引导期 `boot.S` 只映射 0..4GB（4 个 PD × 512 × 2MB 大页）。
- 扩展逻辑：对 `[4GB, min(total_ram, 512GB))` 的每个 1GB 块：
  - 同时处理 `PML4[0]`（恒等）与 `PML4[256]`（高半区）两个半区；
  - PDPT 项缺失则分配新 PD 页；以 2MB 大页（`PTE_HUGE`）填满
    `PRESENT|WRITE` 映射。
- 上限 512GB = 单个 PML3 覆盖范围；`pmm_alloc_page` 失败时保守截断。
- **验证**：QEMU `-m 6G` 启动，日志
  `[pmm] total=7168 MiB, pages=1835008`（QEMU 将 >3.5G 部分重定位到 4GB 上方）、
  `[vmm] extended kernel mapping up to 7168 MiB of RAM`，
  全部服务正常上线（此前版本触碰 >4GB 页帧即三重故障）。

### C1：W^X 用户内存 + SMEP

**1. 链接脚本拆分**（`user/user.ld`）：
```
.text/.rodata @ 0x400000 起 → RX 区
. = ALIGN(0x100000);  _etext = .;   ← 1MiB 硬边界
.data/.bss @ 0x500000 起 → RW 区
```

**2. 装载器按边界拆权限**（`kernel/sched/sched.c: task_create_user`，
`USER_DATA_SPLIT = 0x100000`）：
- `[0, min(size,1MiB))`：`PTE_PRESENT|PTE_USER` —— **可执行、不可写**；
- `[1MiB, 1MiB+data+256KiB)`：`PTE_PRESENT|PTE_WRITE|PTE_USER|PTE_NX`
  —— **可写、不可执行**（含 .data 拷贝 + .bss/堆零页 256KiB）；
- 用户栈：同样 `RW+NX`。
- **踩坑记录**：`objcopy -O binary` 在 `.data` 为空时不输出尾部空隙，
  bin 只含 text/rodata（约 3.5KB）；数据区必须**无条件**按
  `USER_CODE_BASE + USER_DATA_SPLIT` 映射零页，否则 `.bss`（链接于
  0x500000）无映射，服务一碰静态缓冲即缺页。

**3. SMEP**（`boot/boot.S`）：
- `CR4.OSFXSR(bit9)` 无条件开启（fxsave 前置条件）；
- **SMEP(bit20) 必须 CPUID 探测后启用**：`CPUID.07H:EBX.SMEP[bit7]`。
  踩坑记录：无条件 `or $1<<20, %cr4` 在 QEMU 默认 `qemu64` 模型
  （无 SMEP）上直接 #GP → 三重故障 → 串口无任何输出。现探测流程：
  ```asm
  cpuid(0) → 最大 leaf ≥ 7 ？ → cpuid(7,0) → test EBX bit7 → 置 CR4.SMEP
  ```
  支持 SMEP 的真机/`-cpu host` 自动获得内核不可执行用户页保护；
  老 CPU 优雅降级（NX + W^X 仍有效）。

### C3：用户栈 ASLR

- `aslr_random()`：`rdtsc` 取 TSC，SplitMix64 风格混淆
  （`x ^= x>>33; x *= 0xFF51AFD7ED558CCD; x ^= x>>33`）。
- 栈顶 = `USER_STACK_TOP - (rand & 0xFF) * 4096`，即随机下移 0..255 页
  （最大 ~1MiB），每个进程独立。
- 局限（如实声明）：平坦二进制按绝对地址链接，**代码基址无法随机化**；
  完整 ASLR 需 ELF PIE 装载器（遗留项，见 §四）。

### D2：FPU/SSE 上下文保存

- `task_t` 新增 `uint8_t fpu_state[512] __attribute__((aligned(16)))` +
  `bool fpu_valid`（fxsave64 区域标准 512B，16B 对齐硬性要求）。
- `switch.S` 新增原语：`fpu_fxsave`（fxsave64）、`fpu_fxrstor`（fxrstor64）、
  `fpu_fninit`、`fpu_init`（fninit + 置 `CR0.MP`）。
- `schedule()` 与 `task_exit_current()` 切换序列：
  `fxsave(prev)` → `next->fpu_valid ? fxrstor(next) : fninit()`（首次运行
  给全新 FPU 状态，避免继承别人的寄存器/控制字）。
- `kmain` 在 `gdt_init` 后调用 `fpu_init()`。
- 效果：用户/内核任务可安全使用 float/double/SSE，上下文不互相污染。

### D4：ATA 写通道

- **`ata_write_sectors(lba, count, buf)`**（`kernel/drivers/ata.c`）：
  LBA28 `WRITE SECTORS (0x30)`，逐扇区等 DRQ 后 `outw` 写 256 字；
  完成后发 `FLUSH CACHE (0xE7)` 并等 BSY 清零 —— **确保落盘（掉电安全）**；
  含 `lba+count > total_sectors` 越界写保护。
- **IPC 协议扩展**（`include/ipc/disk_proto.h`）：`DISK_MSG_WRITE = 2`，
  `disk_write_req_t {lba, count}` 后随 `count*512` 数据，应答
  `disk_write_resp_t {status}`。
- disk-srv 消息循环新增 WRITE 分支：严格校验
  `n >= header + req + count*512` 防止越界读请求缓冲；请求缓冲扩容至
  `header + write_req + 7*512`。
- 发送权限：`DISK_PORT.send_owner = fs-server`（A2），只有文件服务可写盘。

### 并发竞态修复（开发中发现的新缺陷）

**用户任务创建竞态**（比 step01 清单更隐蔽的真实 bug，QEMU `-d int` 定位）：
`task_create_kernel()` 把新任务插入就绪环后**无条件 `sti`**；而
`task_create_user()` 在其返回后才补写 `cr3/user_rip/user_stack_top` 和
r13 参数槽。若 PIT 在此窗口抢占，调度器会以 `arg=NULL, cr3=内核页表`
运行 `user_task_thunk` → 从 `NULL->user_rip`（物理页 0 = BIOS IVT）读出
垃圾入口 `0xf000efd2f000e82e` → iretq 进 Ring3 → 三重故障。

修复：
- `sched.c` 引入 `irq_save()/irq_restore()`（保存 RFLAGS.IF 的嵌套安全
  临界区），替换 `task_create_kernel` 中无条件 `interrupts_enable()`；
- `task_create_user` 的"创建骨架 + 补写字段"整段包在 `irq_save/restore`
  中 —— 调度器看到新任务时其字段必然完整。

---

## 三、验证记录

### 构建
```
make iso disk   # x86_64-elf GCC 交叉工具链，0 error
```

### 回归测试 1：标准启动（QEMU 2G / qemu64 / 无显示 / 串口日志）
```
qemu-system-x86_64 -m 2G -cpu qemu64 -display none -serial file:... \
    -cdrom SukiOS.iso -drive file=disk.img,format=raw
```
关键输出（全部通过）：
```
[idt] IDT loaded (48 vectors installed, #PF handler registered)
[vmm] extended kernel mapping up to 2047 MiB of RAM
[sched] user task 'fs-server'  pid=3 ... (W^X)
[sched] user task 'input-server' pid=4 ... (W^X)
[sched] user task 'shell' pid=5 ... (W^X)
[fs] FAT32 mounted: spc=1 fat@32 data@2050 root_clus=2
[shell] SukiOS shell online
```

### 回归测试 2：键盘交互（QEMU monitor sendkey 注入）
完整链路 `IRQ1 → 内核扫描码队列 → input-server(Ring3) → SHELL_PORT →
shell(Ring3) → FS_PORT → fs-server(Ring3) → DISK_PORT(能力校验) →
ata → 逐级应答`：
```
SukiOS> ls
README.TXT    94 bytes
HELLO.TXT     20 bytes
ROADMAP.TXT   80 bytes
SYS           <DIR>
SukiOS> cat hello.txt
hello from FAT32 :)
```

### 回归测试 3：大内存（QEMU 6G，验证 B4）
```
[boot] usable RAM: 6143 MiB
[pmm] total=7168 MiB, pages=1835008
[vmm] extended kernel mapping up to 7168 MiB of RAM
... 全部服务正常上线，shell 可用
```

---

## 四、剩余限制（如实声明）

以下项目**未在本阶段完成**，生产部署前仍需评估：

1. **单核（无 SMP）**：调度器/IPC/堆的临界区依赖 `cli/sti`，仅单核正确。
   多核需 per-CPU runqueue、自旋锁、LAPIC/IOAPIC、AP 启动协议。
2. **FAT32 写路径**：内核 ATA 写通道与 `DISK_MSG_WRITE` 协议已就绪，
   但 fs-server 尚未实现簇分配/目录项更新/FAT 表镜像同步，文件系统
   仍为只读；短文件名 8.3 限制仍在。
3. **代码段 ASLR / KPTI**：平坦二进制无法随机化代码基址（需 ELF PIE
   装载器）；未实现内核页表隔离（Meltdown 缓解）。
4. **SMAP 未启用**：`copy_*_user` 走显式页表校验路径，风险可控；启用
   SMAP 需在 copy 原语中包裹 `stac/clac`。
5. **无动态进程创建**（spawn/exec）、无网络栈、无用户态定时器 syscall。
6. **`qemu64` 模型无 SMEP**：CPUID 探测自动降级；`-cpu host` 或真机
   （Ivy Bridge+）自动启用。

**结论**：step01 列出的全部致命（A 类 + B4/C1/D2）与严重（B 类）缺陷
已修复并通过回归；系统在故障隔离、权能校验、资源回收、内存保护
（NX/W^X/SMEP/canary）方面达到可长期运行的健壮性。但受单核、FS 只读、
无动态进程等架构性限制，当前定位为**加固后的教学/嵌入式只读设备级系统**；
通用生产环境仍需完成 §四 所列项目。
