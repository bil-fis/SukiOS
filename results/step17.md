# Step 17：P0-3 SMP 多核支持（AP 启动 + per-CPU + 自旋锁 + IPI）

> 对照 `results/step10_todos.md` P0-3 里程碑。本阶段将 SukiOS 从单核内核升级为
> 真 SMP：BSP 经 INIT-SIPI-SIPI 唤醒全部 AP 进入 64 位长模式，建立 per-CPU
> 数据体系（GS_BASE）、ticket 自旋锁体系与 IPI（重调度/TLB shootdown/panic 停机）。
> QEMU `-smp 4` 实测 4/4 CPU 在线、IPI 自检 3/3 AP 应答、全服务栈正常启动。

---

## 1. 本阶段完成的功能

| 功能 | 状态 | 验证方式 |
|---|---|---|
| ticket 自旋锁（公平 FIFO，irqsave 变体） | 完成 | pmm/kheap/kprintf 全程持锁启动无死锁 |
| per-CPU 数据体系（GS_BASE MSR） | 完成 | 每 CPU 日志打印独立 slot 地址 |
| AP 实模式跳板（16→32→64 位） | 完成 | 3 个 AP 全部进入 ap_main 并握手 |
| INIT-SIPI-SIPI 启动协议 | 完成 | `[smp] 4/4 CPUs online` |
| per-CPU GDT/TSS（busy 位隔离） | 完成 | AP ltr 不 #GP |
| IPI：RESCHED/TLB_FLUSH/HALT | 完成 | `[smp] IPI selftest: 3/3 APs acked` |
| TLB shootdown 挂钩（内核映射收回时广播） | 完成 | vmm_unmap_page 高半区分支 |
| panic 跨核停机 + 输出锁强制重置 | 完成 | 代码审查（panic 路径不可自锁） |
| PMM/kheap/kprintf 单核假设销账（H8/M2） | 完成 | 加锁后 pmm_selftest PASS |
| 单核回退兼容 | 完成 | `QEMU_SMP=1` 打印 single CPU 并正常启动 |

---

## 2. 新增/修改文件清单

### 新增
| 文件 | 内容 |
|---|---|
| `include/kernel/spinlock.h` | ticket 自旋锁（头文件内联实现） |
| `include/kernel/percpu.h` | percpu_t 结构 + MAX_CPUS=8 + 接口声明 |
| `kernel/arch/x86_64/percpu.c` | GS_BASE 安装、cpu_index()/cpu_local() |
| `include/kernel/smp.h` | IPI 向量常量 + smp_init 等接口 |
| `kernel/arch/x86_64/smp.c` | AP 启动序列、IPI 处理器、ap_main、自检 |
| `kernel/arch/x86_64/ap_boot.S` | AP 实模式跳板（拷贝到物理 0x8000 运行） |

### 修改
| 文件 | 改动 |
|---|---|
| `include/kernel/apic.h` / `kernel/arch/x86_64/apic.c` | 新增 ICR 寄存器定义与 `lapic_send_init/send_startup/send_ipi/broadcast_ipi` |
| `kernel/arch/x86_64/gdt.c` / `include/kernel/gdt.h` | 新增 `gdt_init_ap(cpu)`：per-CPU GDT+TSS+IST 栈 |
| `kernel/arch/x86_64/idt.c` / `include/kernel/interrupts.h` | IPI 门 240/241/242 安装、`idt_load_ap()`、EOI 条件放宽到全部 `vec>=32 && vec!=0xFF` |
| `kernel/arch/x86_64/isr.S` | `ISR_NOERR 240/241/242` 存根 |
| `kernel/mm/pmm.c` | `g_pmm_lock` 串行化 alloc/free/incref/decref（销 H8/M2） |
| `kernel/mm/kmalloc.c` | `g_kheap_lock` 串行化 kmalloc/kfree 全路径 |
| `kernel/console.c` | kprintf 改用 `g_kp_lock` 跨核串行化；panic 先 `smp_halt_others()` 再强制重置锁；修复 `%` 后 `\0` 分支泄漏重入计数 |
| `kernel/mm/vmm.c` | `vmm_unmap_page` 收回内核高半区映射时 `smp_tlb_shootdown()` |
| `kernel/kmain.c` | clock_init 后接入 `percpu_install(0, bsp_lapic)` + `smp_init()` |
| `Makefile` | QEMU 增加 `-smp $(QEMU_SMP)`（默认 4，可 `QEMU_SMP=1` 回退） |

---

## 3. 技术细节

### 3.1 ticket 自旋锁（include/kernel/spinlock.h）

数据结构：32 位 `tickets` 字段，`[31:16]=next`（下一张发放票号）、`[15:0]=owner`（当前放行票号）。

- `spin_lock`：`__atomic_fetch_add(&tickets, 1<<16)` 原子取票（生成 `lock xadd`），
  自旋等待 `owner == 我的票号`，`pause` 降功耗；FIFO 公平、无饥饿。
- `spin_unlock`：对低 16 位半字原子 `+1`（RELEASE 语义）。
- `spin_lock_irqsave`：`pushfq; popq; cli` 保存 RFLAGS 后拿锁，返回 flags；
  `spin_unlock_irqrestore` 按保存的 IF 位（bit9）决定是否 `sti`。
  单核下锁必然立即成功，退化为纯 cli/sti，零额外开销。
- 锁序（外层→内层）：`sched -> kmalloc -> pmm -> console`。console 最内层
  （任何路径的诊断打印都可能拿它），持 console 锁期间严禁再拿其它锁。

### 3.2 per-CPU 数据（percpu.h / percpu.c）

- `percpu_t` 布局（偏移固定，汇编依赖）：`cpu_index`@gs:0、`lapic_id`@gs:4、
  `kstack_top`@gs:8、`online`@gs:16、`ticks`。静态数组 `g_percpu[8]`。
- 每 CPU 把 **GS_BASE MSR (0xC0000101)** 指向自己的槽；`cpu_index()` 用
  `movl %gs:0, %reg` 一条指令取索引，无锁无查表。
- **不使用 swapgs**：SukiOS 用户态不会改写 GS 段（CR4.FSGSBASE 未开、
  enter_user_mode 不重载 gs 选择子），Ring0/Ring3 全程 GS_BASE 稳定，
  中断/异常路径无需 swapgs 配对——比 Linux 模型简单且此内核下安全。
- `g_percpu_ready` 守卫：BSP 安装前 `cpu_index()` 一律返回 0（否则 GS_BASE=0
  时读 `%gs:0` 会取到物理页 0/IVT 的垃圾）。
- `wrmsr` 封装在 `__attribute__((noinline)) wrmsr_isolated()`，约束
  `%ecx=MSR号 %eax=低32 %edx=高32`，Clobber memory。

### 3.3 AP 启动跳板（ap_boot.S，运行于物理 0x8000）

- SIPI 向量 0x08 → AP 上电 CS:IP=0800:0000=物理 0x8000（低 1MB 已被 PMM
  保留，无分配冲突）。
- 位置无关策略：代码链接在内核高半区 VMA，但运行在 0x8000——16/32 位段用
  `APADDR(sym) = sym - ap_tramp_start + 0x8000` 常量绝对寻址；64 位段用
  `%rip` 相对寻址（代码与邮箱同块拷贝，相对偏移不变）。
- 流程：实模式 cli → `lgdtl` 跳板内临时 GDT → CR0.PE → ljmp 32 位段 →
  CR4.PAE|OSFXSR → CR3=内核 PML4（邮箱）→ **EFER.LME|NXE** →
  CR0.PG|PE|MP → ljmp 64 位段 → 从邮箱取栈/入口 → `jmp ap_main`。
- 邮箱（跳板尾部 4×8 字节，BSP 在 SIPI 前填写）：`ap_mb_cr3`（内核 PML4
  物理地址）、`ap_mb_stack`（该 AP 独立 16KB 内核栈顶）、`ap_mb_entry`
  （ap_main 高半区地址）、`ap_mb_idx`（CPU 逻辑索引）。
- **两个关键正确性点**（其一为实测踩坑）：
  1. **EFER.NXE 必须置位**：BSP 的 syscall_init 已开 NXE，内核映射含
     PTE_NX(bit63) 页；AP 若 NXE=0，bit63 是保留位，翻译即 #PF。
  2. **数据段描述符必须平坦 4GB**（`0x00CF92000000FFFF`）：最初误用
     limit=0 的 `0x0000920000000000`，32 位保护模式段会检查 limit，
     `ap_pm32` 一读邮箱即 #GP → 三重故障 → 整机复位循环（实测现象：
     串口日志重复打印启动横幅）。64 位模式忽略 limit，故 BSP 的同款
     描述符无害，仅 32 位过渡段暴雷。

### 3.4 INIT-SIPI-SIPI（smp.c + apic.c）

- ICR 写法：先写 `ICR_HIGH(0x310)` 目标 LAPIC ID<<24，再写 `ICR_LOW(0x300)`
  触发；每次发送后轮询 bit12 (Delivery Status) 等待投递完成。
- 序列（Intel SDM Vol.3 8.4.4）：INIT assert (0xC500) → INIT deassert
  (0x8500) → 等 10ms → SIPI(0x4600|0x08) → 等 300µs → 未握手则补发第二次
  SIPI → 轮询 `g_percpu[idx].online`（ACQUIRE 语义），100ms 超时判失败。
- 串行逐个启动（全部 AP 共享一个 0x8000 邮箱），成功后 `g_online++`。
- 延时用 TSC 忙等（`clock_tsc_freq_hz()` 已由 P0-4 校准），故 `smp_init()`
  必须在 `clock_init()` 之后调用。

### 3.5 per-CPU GDT/TSS（gdt.c）

- TSS busy 位规则：`ltr` 会把 TSS 描述符标记 busy，已 busy 再 ltr 即 #GP，
  且 TSS 本身存 per-CPU 的 RSP0/IST——**GDT+TSS 必须每 CPU 独立**。
- `gdt_init_ap(cpu)`：静态 `g_gdt_ap[MAX_CPUS][7]`、`g_tss_ap[MAX_CPUS]`、
  `g_ist_ap[MAX_CPUS][4096]`，段布局与 BSP 完全一致
  （kcode=0x08 kdata=0x10 ucode=0x1B udata=0x23 tss=0x28）。
- IDT 全 CPU 共享一张（`idt_load_ap()` 仅执行 `lidt`）。

### 3.6 IPI 向量与处理（smp.h / smp.c / idt.c / isr.S）

| 向量 | 名称 | 语义 |
|---|---|---|
| 0xF0 (240) | IPI_RESCHED | 重调度请求（现阶段：AP `ticks++` 计数应答） |
| 0xF1 (241) | IPI_TLB_FLUSH | 整体重载 CR3 刷 TLB（协议最简且绝对正确；精确 invlpg+代际计数留待多核调度阶段） |
| 0xF2 (242) | IPI_HALT | panic 停机：`cli; hlt` 永久停车 |

- 广播用 ICR 目标简写 11b（all-excluding-self，`0x000C4000|vec`）。
- EOI：`isr_dispatch` 对 `vec>=32 && vec!=0xFF` 统一先发 LAPIC EOI 再调
  handler（IPI_HALT 不返回，但 EOI 已发出，不留 ISR 挂起位）。
- `vmm_unmap_page` 收回**内核高半区**映射时调用 `smp_tlb_shootdown()`
  广播 0xF1（内核映射所有 CPU 共享 TLB 缓存）；用户半区当前仅 BSP 调度，
  本地 invlpg 足够。

### 3.7 AP 生命周期（ap_main）

```
gdt_init_ap(idx) → idt_load_ap() → lapic_init()（软件使能+LVT全屏蔽）
→ percpu_install(idx, apic_id) → online=1（RELEASE 握手）
→ sti → for(;;) hlt   // idle：只响应 IPI；LAPIC 定时器不开
```
调度域本阶段仍由 BSP 独占（诚实声明）：per-CPU runqueue、AP 上跑任务
随后续里程碑推进；本阶段锁体系保护「BSP 任务流 vs AP 中断路径」并为
多核调度铺设正确性地基。

### 3.8 顺带修复的 3 个存量 bug

1. **`pmm_decref` 锁泄漏（新引入后即修）**：饱和分支与正常路径 `return`
   前未 `spin_unlock_irqrestore`，首次 decref 后整个 PMM 永久死锁
   （实测：启动卡死在 `pmm_selftest` 的 T1 压测）。
2. **kprintf `%` 后遇 `\0` 分支泄漏重入计数**：早退路径未 `g_kp_depth--`，
   4 次该模式输出后 kprintf 永久静默。
3. **panic 自锁风险**：若 panic 恰发生在本 CPU 持有 `g_kp_lock` 的输出途中
  （如 kputc 里 #PF），不可重入的 ticket 锁令 panic 的 kprintf 永久自旋。
  现 panic 先 `smp_halt_others()` + `cli`，再强制 `spinlock_init` 重置锁
  （panic 不归路 + 他核已停 → 重置安全）。

---

## 4. 关键地址/常量

| 常量 | 值 | 说明 |
|---|---|---|
| AP 跳板物理地址 | `0x8000` | SIPI 向量 0x08；跳板 216 字节 |
| MSR_GS_BASE | `0xC0000101` | per-CPU 槽指针 |
| LAPIC ICR | `0x300/0x310` | LOW（触发）/HIGH（目标 ID<<24） |
| INIT assert/deassert | `0xC500/0x8500` | level 触发模式 |
| SIPI | `0x4600\|vec` | Start-Up delivery=110b |
| 广播（excl. self） | `0x000C4000\|vec` | 目标简写 11b |
| IPI 向量 | `0xF0/0xF1/0xF2` | 避开 0x20..0x2F IRQ 与 0xFF 伪中断 |
| AP 栈 | kmalloc 16KB/CPU | 栈顶写入邮箱 |
| MAX_CPUS | 8 | percpu/GDT/TSS/IST 静态数组上限 |
| AP 数据段描述符 | `0x00CF92000000FFFF` | 平坦 4GB（32 位段查 limit！） |

## 5. 验证记录（QEMU + KVM，`-machine pc -smp 4`）

```
[percpu] cpu0 installed (lapic_id=0, slot=0xffff800000132580)
[smp] trampoline copied to 0x0000000000008000 (216 bytes)
[percpu] cpu1 installed (lapic_id=1, slot=0xffff8000001325a0)
[smp] cpu1 online (lapic_id=1)
[percpu] cpu2 installed (lapic_id=2, slot=0xffff8000001325c0)
[smp] cpu2 online (lapic_id=2)
[percpu] cpu3 installed (lapic_id=3, slot=0xffff8000001325e0)
[smp] cpu3 online (lapic_id=3)
[smp] 4/4 CPUs online (BSP lapic_id=0)
[smp] IPI selftest: 3/3 APs acked RESCHED
[boot] all services spawned; idle task parked.
```

- `make run-headless`（默认 -smp 4）：4/4 在线，IPI 自检 3/3，全服务栈正常。
- `make run-headless QEMU_SMP=1`：打印 `single CPU system (MADT lapic_count=1)`
  直接跳过 AP 启动，系统行为与 SMP 前完全一致（回退兼容）。
- `pmm_selftest`（T1 压测 16384 次计数操作/T2 饱和/T3 守卫）在全锁化后 PASS。

调试命令：
```bash
make iso && make run-headless                 # 默认 4 核
make run-headless QEMU_SMP=1                  # 单核回退验证
make debug                                    # -s -S 配 GDB：
#   (gdb) info threads          # 查看 4 个 vCPU
#   (gdb) p/x $gs_base          # 验证各 CPU 的 percpu 槽指针
```

## 6. 遗留与展望

- AP 尚不参与任务调度（无 per-CPU runqueue；LAPIC 定时器仅 BSP 开启）。
- TLB shootdown 为整表重载 + 无应答等待（fire-and-forget）；精确
  invlpg + 代际计数应答随多核调度引入。
- `sched.c` 仍用 cli/sti 临界区（单 BSP 调度域下正确）；扩展调度域时
  须换 runqueue 自旋锁。
- syscall 入口的 `g_syscall_kstack` 等全局变量在多核调度域下须 percpu 化
  （swapgs 或 GS 偏移方案），本阶段 BSP 独占调度不受影响。
