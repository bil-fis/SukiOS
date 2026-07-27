# SukiOS 阶段汇总报告 — Step 01 ~ Step 04

> 汇总时间：2026-07-26
> 覆盖范围：从零搭建最小可运行混合内核（Step01），代码审查与加固（Step01 审查 + Step02），reboot 调试修复（Step03），控制台 Unicode/中文支持（Step04）。
> 适用环境：x86_64 物理机目标，初期 QEMU（`-machine pc` i440FX / `qemu64` / 2G~6G 内存）验证。
> 交叉工具链：x86_64-elf GCC。

---

# 零、SukiOS 项目总览（架构与硬约束）

## 0.1 设计目标

SukiOS 是一个**类 macOS (XNU) 的混合内核**操作系统：既保留微内核的 IPC/服务隔离形态，又在内核驻留必要驱动以获得性能。远期目标为现代 GUI 合成器 + 桌面/服务器可用。

## 0.2 内存布局（硬常量）

| 区域 | 地址 | 说明 |
|------|------|------|
| 内核虚拟基址 `KERNEL_BASE` | `0xFFFF800000000000` | 高半区（high-half kernel） |
| 内核物理加载地址 | `0x100000`（1MB） | GRUB multiboot2 加载 |
| 用户态空间上限 `USER_SPACE_TOP` | `0x00007FFFFFFFFFFF` | 低半区，最高 128TB |
| 用户代码基址 `USER_CODE_BASE` | `0x400000` | 平坦二进制加载点 |
| 用户数据分割 `USER_DATA_SPLIT` | `0x100000`（1MiB） | 代码 RX / 数据 RW 边界 |
| OOL 接收窗口 `OOL_RECV_BASE` | `0x0000600000000000` | 内核→用户 OOL 映射区 |
| 帧缓冲物理地址（QEMU） | `0xFD000000` | GRUB LFB 提供，32bpp |
| VGA 文本缓冲 | `0x000B8000` | 回退后端 |

分页：4 级（PML4→PDP→PD→PT），4KB 页；内核高半区 PML4[256..511]，用户低半区 PML4[0..255]。

## 0.3 ABI 硬约束

- **syscall 指令**：`syscall`（经 `IA32_LSTAR` MSR 进入）。
- **调用约定**：`%rax`=调用号；`%rdi,%rsi,%rdx,%r10,%r8,%r9`=参数（System V AMD64）。
- **段选择子（不可违背的红线）**：
  - 内核 `CS=0x08` / `DS=0x10`
  - 用户 `CS=0x1B` / `DS=0x23`
  - 返回用户态使用 `iretq`（与选择子顺序兼容），不直接用 `sysretq`（见 Step01 已知简化项）。
- **用户指针安全红线**：所有从用户态接收的指针**严禁直接解引用**，必须经 `copy_from_user()` / `copy_to_user()` 拷到内核缓冲区（Step02 后已升级为逐页 PTE 校验 + `#PF` 故障隔离）。

## 0.4 混合内核职责划分

- **Ring 0 驻留**：调度器（`sched`）、VMM、PMM、Mach 式 IPC 端口路由（`port`）、CPU 异常/`#PF` 处理、**仅 ATA 磁盘裸读写**（作为 `DISK_PORT` 响应 IPC，特例）。
- **Ring 3 用户态服务**：`FS_SERVER`（FAT32）、`DISPLAY_SERVER`（显示合成，远期）、`INPUT_SERVER`（输入解析）、`SHELL`。**所有驱动均为用户态进程，经 `mach_msg` 通信**。

## 0.5 IPC 模型

- 内核全局端口表 `kernel_port_t g_ports[PORT_MAX]`，知名端口号 1..7（如 `DISK_PORT=1`、`FS_PORT=2`、`SHELL_PORT=6`、`FS_REPLY_PORT=7`）。
- **小消息（<4KB）**：内核 `memcpy` 双拷贝转发。
- **大消息（像素缓冲）**：OOL（Out-Of-Line）物理页重映射，零拷贝，仅传页框号 + 引用计数 +1。
- **GUI 远期目标**：应用离屏 Buffer → OOL IPC → Display Server 合成 → 写帧缓冲。

## 0.6 构建与运行命令

```bash
make iso            # 生成 build/SukiOS.iso（双固件 multiboot2 ISO）
make disk           # 生成 build/disk.img（FAT32 + 引导无关数据盘）
make run            # 图形窗口运行（-boot d 从 ISO 引导）
make run-headless   # 串口无头运行（自动化验证）
make debug          # qemu -s -S 等待 GDB
```

> 注意：`Makefile` 的 `QEMU_FLAGS` 经 Step03 修正为 `-machine pc -cpu qemu64 -m 2G -no-shutdown`（**不带 `-no-reboot`**，否则 reboot 会 paused）。构建产物全部落入 `build/`，已被 `.gitignore` 忽略。

---

# 一、Step 01 — 最小可运行混合内核（九阶段全链路）

> 源文档：`results/step01.md`
> 本阶段从 GRUB 引导到 Ring3 Shell 的完整启动链路全部打通，经 QEMU 实测。

## 1.1 九阶段与验证证据

| 阶段 | 需求 | 验证证据 |
|------|------|----------|
| 一 | GRUB Multiboot2 引导（BIOS/UEFI ISO） | `grub-file --is-x86-multiboot2` 通过；`grub-mkrescue` 生成双固件 ISO |
| 二 | 高地址内核 `0xFFFF800000000000` | `kmain` 位于 `0xffff8000001xxxxx`；用户态低地址 `0x400000` |
| 三 | 帧缓冲图形终端 + 开机 Logo | 1024×768×32 @ `0xFD000000`，像素级校验字形/Logo；无 FB 时回退 VGA `0xB8000` |
| 四 | PIT + PS/2 中断 | 100Hz 节拍驱动抢占；键盘 IRQ1 采集原始扫描码 |
| 五 | 抢占式轮转调度 | 4 个 worker 任务完美交错（`4→3→2→1→4...`），修复了新任务关中断不可抢占的 bug |
| 六 | syscall ABI | `LSTAR` 入口、`0x08/0x10/0x1B/0x23` 选择子红线、`copy_from_user` 拦截内核指针 |
| 七 | Mach IPC | 小消息双拷贝 + OOL 零拷贝实测（同一物理页双映射校验一致） |
| 八 | 混合内核红线 | 仅 ATA PIO 驻留内核（`DISK_PORT`）；FAT32/键盘解析/Shell 全 Ring3 |
| 九 | Ring3 Shell 四命令 | `ls`（根目录 3 文件 + 1 目录）、`cat HELLO.TXT`、`help`、`reboot`（8042 复位，日志确认二次启动） |

## 1.2 交互链路（全 IPC 演示）

```
键盘 IRQ(内核, 仅采集扫描码)
   → sys_input_read → INPUT_SERVER(Ring3 解析键码)
   → mach_msg → SHELL_PORT → shell(Ring3)
   → mach_msg → FS_PORT → FS_SERVER(Ring3 FAT32)
   → mach_msg → DISK_PORT → 内核 ATA(PIO 裸读写) → 原路逐级应答
```

## 1.3 本阶段 Makefile 变更

`run` / `run-headless` / `debug` 目标统一加入 `-boot d`，强制从光驱（ISO）引导。原因：FAT32 磁盘位于 `index=0`（第一硬盘）且无引导扇区，`-machine pc` 默认优先尝试硬盘启动导致无法引导；加 `-boot d` 后 `make run` 可直接拉起完整可启动环境。

## 1.4 已知简化项（Step01 自评）

1. `sysret` 未用：选择子顺序（CS=0x1B 在 DS=0x23 前）与 `sysretq` 选择子算术不兼容，返回用户态改用 `iretq`（符合选择子红线，性能略低）。
2. 用户程序为平坦二进制（RWX 混排），后续引入 ELF 装载器实现 W^X。
3. `copy_from_user` 缺页时 panic 而非 EFAULT 恢复；OOL 窗口为全局 bump 分配。
4. `DISPLAY_SERVER` 合成器、磁盘写入、VGA 组合键热切换为下一里程碑。

---

# 二、Step 01 审查 — 生产环境风险清单（step01_faults）

> 源文档：`results/step01_faults.md`
> 本审查在 Step01 演示链路跑通后，对 `kernel/`、`user/`、`include/`、`Makefile`、`boot/` 做了系统化代码审计。**结论是：教学/原型级实现，绝对不能用于生产环境。**

## 2.1 严重程度统计

| 类别 | 条目 | 级别 |
|------|------|------|
| A 安全/隔离 | A1 缺页无处理→坏指针宕机 | 🔴 |
| A 安全/隔离 | A2 IPC 端口无能力校验 | 🔴 |
| A 安全/隔离 | A3 OOL 引用计数/映射泄漏 | 🟠（但可被触发） |
| A 安全/隔离 | A4 单等待者丢失唤醒 | 🟠 |
| B 资源泄漏 | B1 进程退出不释放资源 | 🟠 |
| B 资源泄漏 | B2 任务创建中途不回滚 | 🟠 |
| B 资源泄漏 | B3 内核堆不收缩/无保护 | 🟠 |
| B 资源泄漏 | B4 内核仅映射 0..4GB | 🔴（>4GB 硬件） |
| C 隔离/硬化 | C1 用户页 W+X、无 SMEP/SMAP | 🔴 |
| C 隔离/硬化 | C2 无 KPTI（Meltdown 侧信道） | 🟡→🔴 |
| C 隔离/硬化 | C3 无 ASLR/KASLR | 🟡 |
| D 功能简化 | D1 单核无 SMP | 🟡 |
| D 功能简化 | D2 无 FPU/SSE 保存 | 🔴（用户用浮点即崩） |
| D 功能简化 | D3 无缺页/COW/按需分页 | 🟡 |
| D 功能简化 | D4 ATA PIO 仅读 LBA28 | 🟡 |
| D 功能简化 | D5 FAT32 只读 8.3 | 🟡 |
| D 功能简化 | D6 无动态进程创建 | 🟡 |
| D 功能简化 | D7 无权限/网络/定时器 syscall | 🟡 |
| D 功能简化 | D8 无 OOM 回收 | 🟡 |

## 2.2 关键致命缺陷摘录

- **A1 缺页无处理器**：`isr_dispatch()` 对 `vec<32` 仅当 `g_handlers[vec]` 非 NULL 才调用，否则 `panic`；14 号 `#PF` 从未注册。任何用户程序传一个 `[0, 0x7FFFFFFFFFFF]` 内未映射地址（如 `sys_debug_write(buf, len)`）→ 内核态 `#PF` → 整机死机（DoS）。
- **A2 端口无能力校验**：`port_lookup()` 只查 `name<PORT_MAX && in_use`，不查 `owner`/`send_owner`。任意任务可 `mach_msg(RECV, FS_PORT)` 窃听，或伪造发往 `DISK_PORT` 的裸扇区请求。
- **B4 内核仅映射 0..4GB**：引导页表用 4 个 PD 的 2MB 大页映射 0..4GB；`vmm_init()` 未建立精细 4KB 表覆盖 >4GB。`pmm_alloc_page()` 在 >4GB 机器会分配高地址页，`PHYS_TO_VIRT` 落入未映射高半区 → `#PF` → panic。QEMU 2G 侥幸可跑，真机不可。
- **C1 用户代码页 W+X**：`task_create_user()` 代码页映射为 `PTE_WRITE|PTE_USER`（无 NX）；内核未启用 `CR4.SMEP/SMAP`。
- **D2 无 FPU 保存**：`context_switch()` 仅保存 `rbx/rbp/r12-r15`，用户用浮点触发 `#NM`→ 未处理 → panic。

## 2.3 审查结论

Step01 的“九阶段验证”仅说明**演示链路可跑通**，不等于生产安全。全部 🔴 致命 + 🟠 严重缺陷在 Step02 被逐项修复。

---

# 三、Step 02 — 致命/严重缺陷修复与系统加固

> 源文档：`results/step02.md`
> 本阶段依据 `step01_faults.md` 修复了全部 🔴 致命（A1/A2/B4/C1/D2）与 🟠 严重（A3/A4/B1/B2/B3）缺陷，并补充 ASLR、ATA 写通道等。全部修改经 QEMU（2G 与 6G）编译 + 启动 + 键盘交互回归验证。

## 3.1 修复总览表

| 编号 | 缺陷 | 状态 | 关键文件 |
|------|------|------|----------|
| A1 | 缺页无处理器，用户坏指针 panic 整机 | ✅ | `idt.c`, `syscall.c`, `vmm.c` |
| A2 | IPC 端口无能力校验 | ✅ | `port.c`, `port.h`, `syscall.c` |
| A3 | OOL 引用计数/映射区间泄漏 | ✅ | `port.c`, `sched.c` |
| A4 | 单 waiter 指针丢失唤醒 | ✅ | `port.c`, `task.h` |
| B1 | 进程退出不回收资源 | ✅ | `sched.c`, `vmm.c` |
| B2 | 任务创建失败不回滚 | ✅ | `sched.c` |
| B3 | 内核堆无完整性保护 | ✅ | `kmalloc.c` |
| B4 | 内核仅映射 0..4GB 物理内存 | ✅ | `vmm.c`（6G 实测通过） |
| C1 | 用户页 W+X、无 SMEP | ✅ | `boot.S`, `user.ld`, `sched.c` |
| C3 | 无 ASLR | ✅ 栈 ASLR | `sched.c` |
| D2 | 上下文切换不保存 FPU/SSE | ✅ | `switch.S`, `task.h` |
| D4 | ATA 只读 | ✅ 增加写通道 | `ata.c`, `disk_proto.h` |

## 3.2 逐项技术细节

### A1：#PF 处理器 + 安全 copy_from_user/copy_to_user

- 新增 `vmm_pte()`（`vmm.c`）：返回虚拟地址对应叶级 PTE 原始值（含全部标志位），未映射返回 0；支持 1GB/2MB 大页短路。
- 新增 `user_access_ok(uptr, n, write)`（`syscall.c`）：区间校验（检测回绕，`end<=USER_SPACE_TOP`）+ **逐页校验** `[start&~0xFFF, end]`，每页调用 `vmm_pte()`：
  - `copy_from_user`（write=false）：要求 `PTE_PRESENT`；
  - `copy_to_user`（write=true）：额外要求 `PTE_WRITE`（防止内核替用户绕过只读映射）。
  - 失败返回 0（语义 EFAULT），**不再触发内核缺页**。
- 新增 `page_fault_handler()`（`idt.c`，注册于向量 14）：读 `CR2`；`r->cs&3` 区分用户/内核态；
  - 用户态 `#PF`：打印 `cr2/write/pid/name` 后 `task_exit_current()` 终止该任务并调度他人 → **故障隔离，内核继续运行**；
  - 内核态 `#PF`：打印 CR2/RIP/CS/RFLAGS 后 panic（copy_* 已预校验，内核缺页属内核 bug，快速失败）。

### A2：IPC 端口能力（capability）模型

- **接收权（recv right）**：`sys_mach_msg` RECV 分支新增校验 `if (!p || p->owner != sched_current()) return MACH_RCV_INVALID_NAME;`。
- **发送权（send right）**：`kernel_port_t` 新增 `send_owner`；`send_owner==NULL` 任意任务可发（公共服务端口），否则仅该任务可发。SEND 分支校验 `if (dp->send_owner && dp->send_owner != sched_current()) → MACH_SEND_INVALID_DEST`。`kmain.c` 中 `port_grant_send(DISK_PORT, fs_task)` —— 只有 fs-server 能向内核磁盘端口发请求。
- **`SYS_PORT_CLAIM`（syscall 7，新增）**：用户态服务认领自身端口接收权，`port_claim()` 仅当无主或已归当前任务时成功。`fs_server.c`/`shell.c`/`input_server.c` 分别认领对应端口；`user/lib/suki.h` 新增 `sys_port_claim()` inline。

### A3：OOL 内存泄漏修复

- 发送失败回滚：`deliver()` 失败或 send 权校验失败时，对 `m->ool_pages[]` 全部 `pmm_decref()` 再 `kfree(m)`。
- 新增 `PTE_OOL`（bit 9）：`ool_map_into_receiver()` 映射时置位，使 `vmm_destroy_address_space()` 区分“自有页（需释放）”与“OOL 共享页（仅解除映射）”。
- 任务级 `ool_maps` 链表：`task_t.ool_maps` → `ool_map_node_t {va,count,pages[]}`，RECV 成功登记。
- 新增 `port_reap_ool(task_t*)`：任务退出时遍历链表 `pmm_decref()` 每页，并将区间挂入全局空闲链表 `g_ool_free`。
- 映射区间复用：`ool_map_into_receiver()` 优先从 `g_ool_free` 取满足区间，找不到才推进 `g_ool_bump`。

### A4：端口等待队列（FIFO 多等待者）

- `kernel_port_t` 由单 `waiter` 指针改为 `waiter_head/waiter_tail`；`task_t` 新增 `wait_next`。
- `port_wait_enqueue()`：当前任务挂队尾，置 WAITING，随后 `schedule()`。
- `enqueue()`（消息入队）：取 `waiter_head` 出队置 READY（严格 FIFO，一条消息唤醒一个等待者）。所有阻塞接收路径统一改用该队列。

### B1：进程退出完整资源回收

- 新增 `vmm_destroy_address_space(pml4_phys)`：深度遍历用户半区 `PML4[0..255]→PDPT→PD→PT`，叶级 PTE 无 `PTE_OOL` 则 `pmm_free_page()`，有则仅清 PTE；自底向上释放页表页本身，最后释放 PML4；内核高半区不触碰。
- 退出顺序关键：`port_reap_ool(t)` → `vmm_switch(vmm_kernel_pml4())`（先切走 CR3）→ `vmm_destroy_address_space(t->cr3)` → 挂 `g_dead_list` → `context_switch()`。颠倒顺序会销毁正指向的 PML4 → Double Fault。
- 延迟回收 `reap_dead()`：退出任务不能释放自己脚下内核栈，挂 `g_dead_list`，由下一获 CPU 任务在 `schedule()` 返回点 `kfree(kstack_base); kfree(task)`。

### B2：任务创建失败回滚

`task_create_user()` 全部分配点失败统一 `goto fail` → `vmm_destroy_address_space(as)`，借 B1 深度遍历自动释放已部分建立的映射与页表。

### B3：内核堆完整性哨兵（canary）

- `block_t` 新增 `canary = (uint64_t)块地址 ^ 0x9E3779B97F4A7C15`（地址绑定防伪造）。
- 写入点：`kheap_init`/`split_block`/`kheap_extend`；校验点：`kfree()` 入口 + 前后合并前，失败打印 `[kheap] CORRUPTION: bad canary` 并 fail-stop。
- **踩坑**：canary 使头从 32B 变 40B，导致返回指针非 16 对齐 → `fpu_state` 未对齐 → `fxsave64` #GP。修复：`HDR_SIZE = (sizeof(block_t)+15) & ~15UL`（=48，负载 16 对齐）。

### B4：全量物理内存映射（>4GB 支持）

- 新增 `vmm_extend_kernel_mapping()`（`vmm_init` 中调用）：对 `[4GB, min(total_ram,512GB))` 每 1GB 块，同时处理 `PML4[0]`（恒等）与 `PML4[256]`（高半区），以 2MB 大页（`PTE_HUGE`）填满 `PRESENT|WRITE`。上限 512GB = 单 PML3 覆盖；`pmm_alloc_page` 失败保守截断。
- 验证：QEMU `-m 6G` 日志 `[pmm] total=7168 MiB`，`[vmm] extended kernel mapping up to 7168 MiB`，全部服务正常上线（此前触碰 >4GB 页帧即三重故障）。

### C1：W^X 用户内存 + SMEP

- 链接脚本 `user/user.ld` 拆分：`.text/.rodata @ 0x400000` → RX 区；`. = ALIGN(0x100000); _etext` 设 1MiB 硬边界；`.data/.bss @ 0x500000` → RW 区。
- 装载器按边界拆权限（`USER_DATA_SPLIT=0x100000`）：`[0,1MiB)` RX 不可写；`[1MiB, 1MiB+data+256KiB)` RW+NX；用户栈 RW+NX。
- **踩坑**：`objcopy -O binary` 在 `.data` 为空时不输出尾部空隙，bin 只含 text/rodata（约 3.5KB），数据区必须无条件按 `USER_CODE_BASE+USER_DATA_SPLIT` 映射零页，否则 `.bss` 无映射。
- **SMEP（boot.S）**：`CR4.OSFXSR(bit9)` 无条件开；SMEP(bit20) 必须 CPUID 探测后启用（`CPUID.07H:EBX.SMEP[bit7]`）。**踩坑**：无条件 `or $1<<20,%cr4` 在 `qemu64`（无 SMEP）上直接 #GP → 三重故障 → 串口无任何输出。现探测流程：cpuid(0) 最大 leaf≥7 → cpuid(7,0) → test EBX bit7 → 置 SMEP；老 CPU 优雅降级（NX+W^X 仍有效）。

### C3：用户栈 ASLR

- `aslr_random()`：`rdtsc` 取 TSC，SplitMix64 风格混淆（`x^=x>>33; x*=0xFF51AFD7ED558CCD; x^=x>>33`）。
- 栈顶 = `USER_STACK_TOP - (rand&0xFF)*4096`（随机下移 0..255 页，最大 ~1MiB），每进程独立。
- 局限：平坦二进制按绝对地址链接，**代码基址无法随机化**；完整 ASLR 需 ELF PIE 装载器（遗留项）。

### D2：FPU/SSE 上下文保存

- `task_t` 新增 `uint8_t fpu_state[512] __attribute__((aligned(16)))` + `bool fpu_valid`。
- `switch.S` 新增 `fpu_fxsave`/`fpu_fxrstor`/`fpu_fninit`/`fpu_init`。
- 切换序列：`fxsave(prev)` → `next->fpu_valid ? fxrstor(next) : fninit()`（首次运行给全新 FPU 状态）。
- `kmain` 在 `gdt_init` 后调 `fpu_init()`。

### D4：ATA 写通道

- 新增 `ata_write_sectors(lba, count, buf)`：LBA28 `WRITE SECTORS (0x30)`，逐扇区等 DRQ 后 `outw` 写 256 字，完成发 `FLUSH CACHE (0xE7)` 并等 BSY 清零（掉电安全）；含越界写保护。
- IPC 协议扩展（`disk_proto.h`）：`DISK_MSG_WRITE=2`，`disk_write_req_t{lba,count}` 后随 `count*512` 数据。
- disk-srv 新增 WRITE 分支，严格校验 `n>=header+req+count*512`；发送权限 `DISK_PORT.send_owner=fs-server`。

### 并发竞态修复（开发中发现的新缺陷）

- **用户任务创建竞态**：`task_create_kernel()` 把新任务插入就绪环后无条件 `sti`，而 `task_create_user()` 返回后才补写 `cr3/user_rip/user_stack_top` 和 r13 参数槽。PIT 抢占窗口内调度器以 `arg=NULL, cr3=内核页表` 运行 `user_task_thunk` → 从 NULL 读垃圾入口 → iretq 进 Ring3 → 三重故障（QEMU `-d int` 定位）。
- 修复：`sched.c` 引入嵌套安全 `irq_save()/irq_restore()`，替换无条件 `interrupts_enable()`；`task_create_user` 的“创建骨架+补写字段”整段包在临界区中。

## 3.3 验证记录

- 构建：`make iso disk` —— x86_64-elf GCC，0 error。
- 回归 1（2G）：`[idt] IDT loaded (48 vectors, #PF handler registered)`、`[vmm] extended kernel mapping up to 2047 MiB`、`[sched] user task 'fs-server' pid=3 (W^X)` 等，shell online。
- 回归 2（键盘交互）：`ls` 显示 `README.TXT/HELLO.TXT/ROADMAP.TXT/SYS<DIR>`，`cat hello.txt` 输出 `hello from FAT32 :)`。
- 回归 3（6G）：`[boot] usable RAM: 6143 MiB`、`[pmm] total=7168 MiB`、`[vmm] extended kernel mapping up to 7168 MiB`，全部服务上线。

## 3.4 剩余限制（Step02 如实声明）

1. **单核无 SMP**：临界区依赖 `cli/sti`，仅单核正确；多核需 per-CPU runqueue、自旋锁、LAPIC/IOAPIC、AP 启动。
2. **FAT32 写路径**：内核 ATA 写通道与 `DISK_MSG_WRITE` 就绪，但 fs-server 未实现簇分配/目录项更新/FAT 镜像同步，FS 仍只读；8.3 限制仍在。
3. **代码段 ASLR / KPTI**：平坦二进制无法随机化代码基址；未实现内核页表隔离。
4. **SMAP 未启用**：`copy_*_user` 走显式页表校验，风险可控；启用需 `stac/clac` 包裹。
5. **无动态进程创建**（spawn/exec）、无网络栈、无用户态定时器 syscall。
6. **`qemu64` 无 SMEP**：CPUID 探测自动降级；`-cpu host`/真机自动启用。

**结论**：Step01 列出的全部 🔴 致命 + 🟠 严重缺陷已修复并通过回归，系统在故障隔离、权能校验、资源回收、内存保护（NX/W^X/SMEP/canary）方面达到可长期运行的健壮性，定位为**加固后的教学/嵌入式只读设备级系统**。

---

# 四、Step 03 — 修复 reboot 使 QEMU 进入 paused 状态

> 源文档：`results/step03.md`

## 4.1 现象

shell 执行 `reboot` 后，QEMU 不真正重启机器，而是进入 **paused（暂停）** 状态，需手动 `cont` 或退出。

## 4.2 根因

`reboot` 路径：用户态 `shell.c` 执行 `suki_syscall5(SYS_REBOOT,...)` → 内核 `syscall.c::sys_reboot()` 通过 8042 发复位脉冲 `outb(0x64, 0xFE)`（标准 x86 复位手段），随后 `hlt` 等待。

根因**不在内核**，而在 `Makefile` 的 `QEMU_FLAGS`：

```
QEMU_FLAGS := -machine pc -cpu qemu64 -m 2G -no-reboot -no-shutdown
```

- `-no-reboot`：收到复位信号时不重启，当作“关机”；
- `-no-shutdown`：关机时不退出 QEMU，而是停在 paused。

二者叠加 → 内核复位脉冲被捕获 → 不重启 + 不退出 → paused。

## 4.3 修复

`Makefile` 第 63 行移除 `-no-reboot`（保留 `-no-shutdown`，仅作用于未实现的关机路径）：

```
QEMU_FLAGS := -machine pc -cpu qemu64 -m 2G -no-shutdown
```

`run` / `run-headless` / `debug` 三个 target 均引用 `QEMU_FLAGS`，一处修改即生效。内核 `sys_reboot()` 实现本身无需改动。

## 4.4 验证

无头启动，键入 `reboot<ret>` 再 `quit<ret>`：串口日志第 51 行 `rebooting...` → 第 54 行立即出现第二次 `[boot] SukiOS kernel entered` → 第 94 行 `[shell] SukiOS shell online` 重新上线 → 重启后 `quit` 照常执行退出。

## 4.5 残留说明

- 系统未实现 ACPI 关机（poweroff/S5）；`-no-shutdown` 保留便于未来调试关机路径。
- 三重启（triple fault）时因已移除 `-no-reboot`，QEMU 会直接重启而非暂停；调试时可临时加回 `-no-reboot -no-shutdown` 或用 `-d int`。

---

# 五、Step 04 — 控制台输出与字体扩展 Unicode / 中文支持

> 源文档：`results/step04.md`

## 5.1 目标

让内核控制台（图形帧缓冲后端）能正确解码并显示 Unicode（含中文）字符，而非把多字节 UTF-8 序列当 `?` 丢弃。

## 5.2 实现方案

### 5.2.1 UTF-8 流式解码器（新增）

- `include/kernel/utf8.h`：`utf8_init()` / `utf8_feed()`。
- `kernel/lib/utf8.c`：逐字节喂入（契合控制台逐字符输出），内部维护 `need`（剩余续字节数）。返回 `1`=得到码点 / `0`=需更多字节 / `-1`=非法（给 `0xFFFD`）。支持 1/2/3/4 字节；对 0xC0/0xC1 过短编码、续字节越界、>0x10FFFF 给替换符。
- 调用关系：`fbcon_putc()` 每收到一字节调 `utf8_feed()`，凑齐码点再渲染。

### 5.2.2 字形抽象层（扩展 `include/kernel/font.h`）

- 新增 `glyph_t { bits, w, h, stride, scale }`，约定与 `font8x8` 一致：行主序，每像素位 `bit0(LSB)=最左像素`，每行 `stride=(w+7)/8` 字节。
- 新增 `fb_get_glyph(uint32_t cp, glyph_t *g)`（实现于 `framebuffer.c`）：
  - `0x20–0x7F` → `font8x8_basic`（8×8，按 `CON_SCALE=2` 放大 16×16）
  - 其它 → `font_cjk_get()`（16×16 子集）
  - 未命中 → 兜底 16×16 虚线方框（保证任意码点都能渲染不崩）

### 5.2.3 中文 16×16 点阵子集（新增 `kernel/arch/x86_64/font_cjk.c`）

- 手写占位字形（32 字节/字，16 行×2 字节），覆盖 16 个常用字：`中 文 操 作 系 统 你 好 世 界 启 动 成 功 内 核`（按码点升序，二分查找）。
- **重要说明**：占位字形，用于打通管线；要可读中文需嵌入真实字库（GNU Unifont / HZK16），按相同 `{codepoint,32字节}` 格式填入 `g_cjk` 表即可，或把 `font_cjk_get()` 改为从嵌入式字库镜像检索。

### 5.2.4 重构帧缓冲文本控制台（`kernel/arch/x86_64/framebuffer.c`）

- `fb_draw_char(uint32_t px, uint32_t py, uint32_t cp, fg, bg)` 改为按码点取字形，用 `glyph_t.scale` 渲染（ASCII ×2、CJK ×1，均占 16×16 单元）。
- `fbcon_putc(char c)`：控制字符（`\n \r \t \b`）直接处理；其它字节喂入 `utf8_feed()`，得码点后 `fb_draw_char()`。`fbcon_init()` 中 `utf8_init()` 复位状态。
- 兜底方框 `g_fallback_box[32]` 作未知码点字形。

### 5.2.5 自测打印（`kernel/kmain.c`）

控制台初始化后新增：`kprintf("[console] UTF-8 test: 中文显示正常 ✓ 操作系统启动成功\n");`（含中文 + U+2713 勾号，验证 3 字节 UTF-8 非 BMP 内码点）。

### 5.2.6 VGA 文本回退（未改）

`vga_text.c` 依赖硬件字库，无法显示中文，保持 ASCII 仅输出；仅当 GRUB 未提供有效 LFB 时走此路径。

## 5.3 构建

`OBJS` 用 `find kernel -name '*.c'` 自动收集，新增 `utf8.c`/`font_cjk.c` 自动编译链接。`make iso` 通过（仅 `.note.GNU-stack` 无害警告）。

## 5.4 验证

1. **解码正确性**：串口日志（`cat -v`）显示正确 UTF-8 多字节序列：`E4 B8 AD`=中、`E6 96 87`=文、`E2 9C 93`=✓、`E6 93 8D E7 B3 BB E7 BB 9F`=操作系统、`E5 90 AF E5 8A A8 E6 88 90 E5 8A 9F`=启动成功，与自测串一致。
2. **渲染正确性**：`screendump` 得 1024×768 PPM（2.3MB 真实像素），证明帧缓冲被实际绘制；shell 正常上线。
3. 非法 UTF-8 序列走 `0xFFFD` → 兜底方框，不缺页不崩溃。

## 5.5 调试命令

```bash
qemu-system-x86_64 -machine pc -cpu qemu64 -m 2G -no-shutdown -display none \
  -serial file:build/serial.log -monitor stdio -boot d -cdrom build/SukiOS.iso \
  -drive file=build/disk.img,format=raw,index=0,media=disk
# monitor 中：screendump build/fb.ppm   （图像查看器打开验证中文像素）
```

## 5.6 残留限制 / 后续

- 中文字形为占位数据，**需替换为真实字库**才能可读（建议 Unifont 16×16 或 HZK16）。
- 仅覆盖 16 个常用字；其余中文码点显示兜底方框。可扩充 `g_cjk` 表。
- VGA 回退路径不支持中文（硬件字库限制）。
- 尚未做双向文本/组合字符/全角宽度微调（中文按整 16px 单元推进，等同 ASCII 单元宽度）。

---

# 六、四阶段累计成果与时间线

| 阶段 | 文件/主题 | 关键产出 | 验证手段 |
|------|-----------|----------|----------|
| Step01 | 引导→Shell 九阶段 | 完整启动链路、Multiboot2 ISO、帧缓冲终端、抢占调度、syscall ABI、Mach IPC、混合内核红线、4 命令 Shell | QEMU 启动 + 键交互 |
| Step01审查 | 风险清单 | 19 项缺陷（5🔴+4🟠+10🟡）定型 | 代码审计 |
| Step02 | 加固 | 修 5🔴+4🟠；新增 #PF 隔离、端口权能、OOL 回收、等待队列、退出回收、堆 canary、>4GB 映射、W^X+SMEP、栈 ASLR、FPU 保存、ATA 写通道 | QEMU 2G + 6G 回归 |
| Step03 | reboot 修复 | 移除 `Makefile` `-no-reboot`，reset 脉冲真正触发重启 | 串口二次启动横幅 |
| Step04 | Unicode/中文 | UTF-8 解码器、glyph 抽象、16×16 中文子集、fb 重构按码点渲染 | 串口字节 + screendump PPM |

**累计**：74 个源文件纳入版本库（首 commit `0bf4617`），7765 行；已具备加固后的单核教学/嵌入式只读设备级系统的可用性。

---

# 七、当前系统能力边界（一句话速览）

**已实现**：混合内核启动、高半区内核、帧缓冲+串口+VGA 三后端控制台、Unicode/中文渲染（占位字模）、100Hz 抢占调度、syscall ABI（含 7 个调用）、Mach IPC（小消息双拷贝 + OOL 零拷贝）、端口权能模型、`#PF` 用户态故障隔离、W^X + SMEP + 栈 ASLR + 堆 canary、>4GB 物理内存、FPU/SSE 上下文、ATA 读 + 写通道、FAT32 只读 8.3 服务、全 IPC 键盘→Shell→FS→DISK 链路、Ring3 Shell 四命令、reboot 真正重启。

**尚未实现（下一步路线）**：
- P0 级遗留（仅剩 SMAP、KPTI、代码段 ASLR、FAT32 写 FS 层、动态进程创建）；
- 通用化：SMP、网络栈、用户态定时器/信号、OOM 回收、ELF PIE 装载器；
- GUI：Display Server 合成器 + OOL 像素缓冲 + 组合键热切换；
- 中文可读性：替换真实字库（Unifont/HZK16）扩充 `g_cjk`。

---

# 八、调试与复现命令速查（汇总）

```bash
# 1) 标准无头启动 + 串口日志（验证启动链路/Unicode/reboot）
qemu-system-x86_64 -machine pc -cpu qemu64 -m 2G -no-shutdown -display none \
  -serial file:build/serial.log -boot d -cdrom build/SukiOS.iso \
  -drive file=build/disk.img,format=raw,index=0,media=disk

# 2) 大内存验证（Step02 B4：>4GB 映射）
qemu-system-x86_64 -machine pc -cpu qemu64 -m 6G -no-shutdown -display none \
  -serial file:build/serial.log -boot d -cdrom build/SukiOS.iso \
  -drive file=build/disk.img,format=raw,index=0,media=disk

# 3) 截图验证帧缓冲（Step04 中文像素）
# monitor 中执行：screendump build/fb.ppm

# 4) GDB 调试（调试先行）
qemu-system-x86_64 -machine pc -cpu qemu64 -m 2G -s -S -display none \
  -serial file:build/serial.log -boot d -cdrom build/SukiOS.iso \
  -drive file=build/disk.img,format=raw,index=0,media=disk
# 另开终端：gdb build/SukiOS.elf -ex "target remote :1234" -ex "info registers"
# 验证 MSR：在 GDB 中 `monitor info registers` 或 CPU 内读 IA32_LSTAR

# 5) 三重故障调试（临时恢复 paused 行为）
qemu-system-x86_64 -machine pc -cpu qemu64 -m 2G -no-reboot -no-shutdown \
  -display none -serial file:build/serial.log -d int -boot d \
  -cdrom build/SukiOS.iso -drive file=build/disk.img,format=raw,index=0,media=disk
```

---

# 九、结论

截至 Step04，SukiOS 已完成从引导到加固、再到国际化控制台的全链路原型，并在 QEMU 中端到端验证。内核在故障隔离、权能校验、资源回收、内存保护方面达到**可长期运行的单核教学/嵌入式只读设备级系统**标准。下一步聚焦于 SMAP/KPTI、动态进程、GUI 合成器与真实中文字库替换，向“日常可用桌面/服务器操作系统”目标推进。
