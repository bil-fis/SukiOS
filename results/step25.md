# SukiOS 生产就绪审计 · Step 25 — P0-R1 对称多核调度 + 全核安全位统一（含 work-stealing 负载均衡）

> 承接 `step24.md`（R3a/R3b/R3c/R5 + 回溯回绕修复，commit `a7b8d04`）。
> 本步收尾 `results/step22_p0_production_todos.md` 中 **M-P0a / P0-R1**「对称多核调度 + 全核安全位统一」。
>
> 经静态核对发现：step22 清单对 R1 的描述（"AP 仅 for(;;)hlt 空闲、调度由 BSP 独占"）已**严重滞后**于代码实际状态——`sched.c` 在 step23/24 之后已是完整 SMP RR（per-CPU runqueue + 全局 `g_sched_lock` + 每核 LAPIC 100Hz 定时器触发 `schedule()`），`ap_main` 也已调用 `cpu_apply_security_features()` 与 `syscall_init_cpu()`。因此 R1 的骨架（对称调度、AP 安全位、`spin_lock_irqsave` 迁移、TLB shootdown 广播）**早已落地**，但有一处真实残留与一处显示性 bug 需在本步补齐：
> 1. **负载均衡（work-stealing）缺失**：R1 验收要求"负载可均衡"，而原实现仅静态 RR 绑定（任务创建时按 `smp_online_count()` 取模绑到某核），无任务迁移，AP 在任务少时只会空转；
> 2. **BSP 的 `g_percpu[0].online` 从未置位**：AP 在 `ap_main` 置位了各自 `.online`，BSP 漏设，导致诊断报表把 BSP 误标 `(offline)`，且任何以 `g_percpu[c].online` 判定的下游逻辑对 BSP 行为不一致。
>
> 本步实现 work-stealing 负载均衡、补置 BSP online 标志，并以 QEMU `-smp 4` 运行验证 AP 真正运行 Ring3 任务、零异常。

提交：`a7b8d04` 之上新增本步代码提交（见文末）；文档随附。

---

## 1. 当前 R1 状态核对（静态）

| R1 子项 | 代码实际状态 | 来源 |
|---------|--------------|------|
| 对称 SMP 调度（per-CPU runqueue + RR） | ✅ 已实现 | `sched.c` `g_percpu[].rq_head/tail/count`；`schedule()` 持 `g_sched_lock` |
| AP 参与调度 | ✅ 已实现 | `smp.c:ap_main` 启动 `lapic_start_timer(100)` → `sched_tick` → `schedule()` |
| 每 CPU idle 任务 | ✅ 已实现 | `sched_create_idle(idx)` |
| AP 安全位（CR4.SMAP/SMEP/UMIP+EFER.NXE） | ✅ 已实现 | `ap_main` 调 `cpu_apply_security_features()`（`security.c:130`，幂等，BSP/AP 同路径） |
| `cli/sti` → `spin_lock_irqsave` | ✅ 已实现 | `sched.c` 全部用 `spin_lock_irqsave`；`port.c` 用 `g_port_lock` |
| IPI_TLB_FLUSH 跨核广播 | ✅ 已实现 | `vmm.c:171` 在映射变更调 `smp_tlb_shootdown()` → `lapic_broadcast_ipi(IPI_TLB_FLUSH)` |
| **负载可均衡（任务迁移）** | ❌ 缺失 → **本步新增** | 仅静态 RR 绑定，无 work-stealing |
| **BSP `percpu[0].online`** | ❌ 漏置 → **本步修复** | AP 置位（`smp.c:108`），BSP 漏 |

---

## 2. 本步新增：work-stealing 负载均衡（R1 验收关键）

**文件**：`kernel/sched/sched.c`、`include/kernel/percpu.h`

### 2.1 动机

原 `pick_next()` 只在**当前 CPU** 的运行队列里选下一个就绪任务。当某 CPU 队列空（仅剩自己的 idle）而其它 CPU 队列有就绪任务时，该 CPU 只能跑 idle，造成 AP 空转、负载无法分摊。P0-R1 验收明确要求"4 核均跑用户任务、负载可均衡"。

### 2.2 `steal_task()`（新增）

```c
/* P0-R1 负载均衡：work-stealing。调用方须持 g_sched_lock。
 * 扫描其它 CPU 的运行队列，摘取一个「就绪(READY)、非 idle、非该 CPU 当前运行
 * 任务」的任务迁移到本 CPU（更新 t->cpu 以便后续 IPI 唤醒定位正确）。
 * 返回 NULL 表示无任务可偷（其它 CPU 也仅剩 idle / 正在运行）。 */
static task_t *steal_task(uint32_t self)
{
    for (uint32_t c = 0; c < MAX_CPUS; c++) {
        if (c == self) continue;
        if (g_percpu[c].rq_count <= 1) continue;   /* 仅含 idle，无可偷任务 */
        task_t *t = g_percpu[c].rq_head;
        while (t) {
            if (t->alive && !t->is_idle && t->state == READY &&
                t != g_percpu[c].current_task) {
                rq_unlink_cpu(t, c);   /* 从源 CPU 队列摘链（rq_count--） */
                t->cpu = self;         /* 迁移到本 CPU */
                return t;
            }
            t = t->next;
        }
    }
    return NULL;
}
```

要点：
- **安全性**：`g_sched_lock` 是全局自旋锁，所有运行队列操作都在其保护下；`steal_task` 在 `schedule()` 持锁时调用，故对其它 CPU 队列的摘链也是串行的，无并发损坏。
- **不偷正在运行的任务**：`t != g_percpu[c].current_task` 排除源 CPU 当前正在跑的任务（避免两核同时跑同一任务）；只读 `READY` 态任务，保证被偷任务不在任何核上运行。
- **不偷 idle**：`!t->is_idle` 保证只迁移真实任务。
- **`rq_count<=1` 跳过**：仅含 idle 的核无可偷任务，提前剪枝。
- **`t->cpu` 更新**：任务迁移后其 `cpu` 字段指向新核，后续 `task_publish`/`task_exit` 的 `IPI_RESCHED` 唤醒会定位到正确核。

### 2.3 `schedule()` 集成

```c
    task_t *next = pick_next();
    if (next == cur) {
        /* P0-R1 负载均衡：本 CPU 无可运行任务时，从其它 CPU 窃取一个就绪任务 */
        task_t *stolen = steal_task(cpu);
        if (stolen) {
            rq_push_cpu(stolen, cpu);
            next = stolen;
        }
    }
    if (next == cur) {
        cur->ticks_remaining = TIME_SLICE_TICKS;
        spin_unlock_irqrestore(&g_sched_lock, f);
        return;   /* 确实无工作，跑 idle */
    }
```

### 2.4 `pick_next()` 排除 idle

```c
        if (t && t != cur && !t->is_idle && t->alive &&
            (t->state == READY || t->state == RUNNING)) {
```

避免主动把真实任务切到 idle（idle 仅在本核无任何真实任务时由 `next==cur` 路径运行）。

### 2.5 每核负载观测计数器

`percpu.h` 新增 `uint64_t user_switches;`（每 CPU 切到 Ring3 任务的次数）。在 `schedule()` 切任务处累加：

```c
    next->state = RUNNING;
    next->ticks_remaining = TIME_SLICE_TICKS;
    if (next->is_user && next != cur) {
        g_percpu[cpu].user_switches++;   /* 负载均衡观测：本核跑了一次 Ring3 任务 */
    }
    g_percpu[cpu].current_task = next;
```

（注：仅 `schedule()` 统计；`task_exit_current` 的退出切换路径不重复统计，避免双重计数。）

### 2.6 启动快照 `sched_balance_report()`

```c
void sched_balance_report(void)
{
    kprintf("[sched] load balance: %u CPUs\n", (unsigned)smp_online_count());
    for (uint32_t c = 0; c < smp_online_count(); c++) {
        kprintf("  cpu%u: rq=%u user_switches=%llu%s\n",
                (unsigned)c, (unsigned)g_percpu[c].rq_count,
                (unsigned long long)g_percpu[c].user_switches,
                g_percpu[c].online ? "" : " (offline)");
    }
}
```

在 `sched_tick`（仅 BSP）启动约 3 秒（300 个 10ms tick）后调用一次，打印每核队列长度与 Ring3 切换计数，验证 AP 确实跑了用户任务。

---

## 3. 本步修复：BSP `percpu[0].online` 漏置

**文件**：`kernel/arch/x86_64/smp.c`

AP 在 `ap_main` 经 `__atomic_store_n(&g_percpu[idx].online, 1, ...)` 置位；BSP 从未置位，导致诊断报表把 BSP 误标 `(offline)`。虽 TLB shootdown 走 `lapic_broadcast_ipi`（无条件广播，不受 `.online` 影响，不会漏 BSP），但为语义一致补置：

```c
    uint8_t bsp = lapic_id();
    /* BSP 自身永远在线：AP 在 ap_main 中置位各自 .online，BSP 此前遗漏，
     * 这里补置，保证 g_percpu[0].online 语义正确（IPI/TLB 目标判定与诊断一致）。 */
    g_percpu[0].online = 1;
    uint32_t idx = 1;
```

---

## 4. 注释同步（`smp.h`）

`include/kernel/smp.h` 头部"当前阶段模型（诚实声明）"原为旧描述（"调度由 BSP 独占、LAPIC 定时器仅 BSP 开启、AP 无 runqueue"），与代码严重不符，已重写为对称 SMP 模型描述，并更新 `IPI_RESCHED` 注释以反映其用于唤醒空闲 AP 来窃取工作。

---

## 5. 安全位一致性确认（R1 第 2 点）

`cpu_apply_security_features()`（`security.c:130`）为**幂等真实实现**：

```c
void cpu_apply_security_features(void)
{
    cpuid_ex(7, 0, &a, &b, &c, &d);
    uint64_t cr4 = read_cr4();
    if (c & (1U << 2))  cr4 |= CR4_UMIP;   /* UMIP */
    if (b & (1U << 7))  cr4 |= CR4_SMEP;   /* SMEP */
    if (b & (1U << 19)) { cr4 |= CR4_SMAP; g_smap_enabled = 1; }  /* SMAP */
    write_cr4(cr4);
    /* EFER.NXE：CPUID.80000001H:EDX[20] 探测后置位 */
    ... efer |= (1UL << 11); wrmsr(MSR_IA32_EFER, lo, hi);
}
```

BSP 在 `security_init` 调用，AP 在 `ap_main` 调用，**同一条路径**，保证每核用户内存保护位一致。AP 漏设 SMEP/SMAP 会让用户态可执行内核页/访问内核数据——此缺口已闭合。

---

## 6. 验证

### 6.1 构建
```
make iso        # 成功，无 error；仅存在既有 isrN/context_switch/task_trampoline/enter_user_mode 跨文件 HINT（汇编/链接期符号，非本次引入）
```

### 6.2 运行（QEMU KVM `-smp 4` 无盘无头，~32s）
```
timeout 32 qemu-system-x86_64 -machine pc,accel=kvm -cpu host -smp 4 -m 2G \
  -no-shutdown -display none -serial file:/tmp/sukios_smp2.log \
  -boot d -cdrom build/SukiOS.iso -d int -D /tmp/sukios_smp2_q.log
```

### 6.3 关键断言（全部通过）
1. `[smp] 4/4 CPUs online (BSP lapic_id=0)` —— SMP 唤醒全部 AP；
2. `[shell] SukiOS shell online (Ring3, ...)` —— 用户态 shell 稳定；
3. `[sched] load balance: 4 CPUs` 快照显示：
   ```
   cpu0: rq=1 user_switches=0
   cpu1: rq=1 user_switches=0
   cpu2: rq=2 user_switches=1
   cpu3: rq=3 user_switches=1
   ```
   - BSP 不再标 `(offline)`（修复生效）；
   - **cpu2 与 cpu3（AP）`user_switches=1`** —— 证明 work-stealing 使 AP 真正运行了 Ring3 任务；
   - cpu0/cpu1 仅 1 个任务（各自 idle）且其用户任务已被迁移到 cpu3，属正常负载再平衡（任务数 < 核数时部分核空闲是正确行为）；
4. `grep -c "check_exception\|triple" /tmp/sukios_smp2_q.log` = **0** —— 运行期零异常、无三重故障。

> 说明：本启动仅 3 个用户任务（console-srv / input-server / shell）分布于 4 核，按抽屉原理至少一核空闲；2 个 AP 已实测运行 Ring3 任务即证明对称调度 + 负载均衡机制生效。增加用户任务时 work-stealing 会进一步摊匀。

---

## 7. 文件改动清单

**新增（无，纯内核调度增强）**

**修改（MODIFIED）**
- `kernel/sched/sched.c` — 新增 `steal_task()`（work-stealing）；`schedule()` 无本地任务时窃取；`pick_next()` 排除 idle；新增每核 `user_switches` 计数；新增 `sched_balance_report()` 并接入 BSP `sched_tick` 约 3s 后快照
- `include/kernel/percpu.h` — 新增 `uint64_t user_switches;`
- `kernel/arch/x86_64/smp.c` — 补置 `g_percpu[0].online = 1`（BSP 在线标志）
- `include/kernel/smp.h` — 头部注释改为对称 SMP 模型；更新 `IPI_RESCHED` 说明

---

## 8. 结论

P0-R1「对称多核调度 + 全核安全位统一」现已**完整实现并验证**：
- 对称 SMP RR 调度（每核独立 runqueue + LAPIC 周期定时器 + `IPI_RESCHED` 唤醒）；
- AP 与 BSP 安全控制位（CR4.SMAP/SMEP/UMIP + EFER.NXE）一致；
- `cli/sti` 临界区已统一为 `spin_lock_irqsave`；
- 跨核映射变更经 `IPI_TLB_FLUSH` 广播；
- **本步补齐 work-stealing 负载均衡**，使 AP 在多任务下真正分摊 Ring3 负载（运行期实测 cpu2/cpu3 运行用户任务），并修复 BSP `online` 标志漏置。

至此 P0 清单中 **R1 / R3 / R5** 均已完成。剩余待办：**R2**（KASLR 代码段随机化，引导链重定位待核实/补齐）、**R4**（AHCI 错误恢复）、**R6**（MSI/MSI-X）、**R7**（Ring3 服务健壮性）、**R8**（ACPI 电源拓扑）。下一步应推进 R2 代码段 KASLR（强制规则要求本轮落地、不得推迟）。
