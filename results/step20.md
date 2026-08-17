# Step 20：P0-8 内核安全地基（UMIP / NXE / 守卫页 IST 栈 / KASLR-lite / 金丝雀重播种 / Meltdown-KPTI 检测）

## 一、总览

P0-8 目标（step10_todos 原文）：「内核安全地基补全：SMAP(stac/clac)、KASLR（内核基址随机化）、KPTI、栈保护(-fstack-protector + IST 守卫页)」。

本批交付 6 项加固 + 1 项既有严重缺陷修复（BSP EFER.NXE 从未开启），QEMU 实测全部生效：

```
[security] SMEP=on SMAP=on UMIP=on NXE=on
[security] IST guard stacks: #DF installed @0xffffd00000005000, NMI installed @0xffffd0000000b000 (16KB+guard)
[security] stack canary reseeded (TSC entropy, NUL byte); heap base slide active
[security] Meltdown: immune (RDCL_NO=1) -> KPTI not needed
[kheap] heap @ 0xffffc00000e73000 (KASLR slide +14796 KiB), initial 64 KiB
```

## 二、关键缺陷修复：BSP EFER.NXE 从未开启

### 现象
`security_init` 的 NXE 校验首轮实测输出 `NXE=OFF`——`boot/boot.S` 只置了
`EFER.LME(bit8)`，从未置 `EFER.NXE(bit11)`。后果：

- 内核/用户页表里所有 `PTE_NX(bit63)` **完全无效**（用户栈、堆、mmap 匿名页
  的"不可执行"全是摆设，W^X 红线形同虚设）；
- 更糟：NXE=0 时 PTE bit63 是**保留位**，规范上置位会引发保留位 #PF——QEMU
  宽容未触发，真机上可能直接启动失败。

（讽刺的是 `kernel/arch/x86_64/ap_boot.S` 的 AP 路径一直是对的——AP 启动
trampoline 里写了 `LME|NXE`，只有 BSP 漏掉。）

### 修复（boot/boot.S，置 LME 处）
```asm
    /* 掩码存 esi（cpuid 会毁 eax/ebx/ecx/edx，不碰 esi） */
    movl $EFER_LME, %esi
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax         /* 最大扩展 leaf 探测 */
    jb   .Lno_nx
    movl $0x80000001, %eax
    cpuid
    testl $(1 << 20), %edx         /* CPUID.80000001H:EDX[20] = NX 支持 */
    jz   .Lno_nx
    orl  $(1 << 11), %esi          /* EFER.NXE */
.Lno_nx:
    movl $EFER_MSR, %ecx
    rdmsr
    orl  %esi, %eax
    wrmsr
```
要点：
- 不支持 NX 的 CPU 上置 NXE 会 #GP（三重故障），必须先 CPUID 探测；
- 掩码必须放 `esi`——第一版用了 `ebx`，但 `cpuid` 会 clobber ebx，自审后改正；
- GRUB 的 mbi 指针早已存入 `save_mbi` 内存，esi 在此段自由可用。

## 三、新增模块：kernel/arch/x86_64/security.c

### 3.1 UMIP 使能
- 探测：`CPUID.(EAX=7,ECX=0):ECX[2]`；使能：`CR4 |= (1<<11)`；写后读回确认。
- 作用：禁止 Ring3 执行 `sgdt/sidt/sldt/smsw/str`，堵住用户态读取 GDT/IDT
  线性地址（内核布局信息泄露 -> 辅助 KASLR 绕过）的经典通道。
- 实测：`UMIP=on`（qemu64 CPU 支持）。

### 3.2 NXE / SMEP / SMAP 启动期终审
- `rdmsr(IA32_EFER=0xC0000080)` 校验 bit11；读回 CR4 校验 bit20(SMEP)/bit21(SMAP)。
- 只验证不改写（boot.S 是唯一使能点），任何一项 OFF 都会在报告中大写示警。

### 3.3 守卫页 IST 栈（#DF=IST1，NMI=IST2）
- 虚拟地址窗口：`0xFFFFD00000000000` 起（独立 PML4 槽，与
  KHEAP_BASE=0xFFFFC000... / KERNEL_BASE=0xFFFF8000... 物理直映区互不相交）。
- 每条栈窗口 6 页 VA：页0 = 守卫页（**刻意不映射**）、页1..4 = 16KB 映射栈
  （`PTE_PRESENT|PTE_WRITE|PTE_NX`，栈永不需要执行权限）、页5 = 窗口间隔离页。
- 栈顶：#DF = `0xFFFFD00000005000`，NMI = `0xFFFFD0000000B000`（实测日志一致）。
- 安全收益：异常栈自身溢出会撞上未映射守卫页 -> 立刻 #PF 定格现场，而
  gdt.c 的静态数组栈溢出会静默踩坏相邻内核数据。
- 安装：`tss_set_ist(n, top)` 直接改写 TSS 内存——CPU 每次取 IST 都实时读
  TSS，无需重新 ltr，下一次异常即用新栈。
- 失败兜底：任何页分配/映射失败则保留 gdt.c 启动期静态栈（不降级为无栈）。
- 范围：仅 BSP；AP 由 `gdt_init_ap` 配静态 IST 栈（AP 不跑 Ring3，异常面
  小，P1 再统一升级）。

### 3.4 Meltdown 免疫检测 -> KPTI 结论
- `CPUID.(7,0):EDX[29]` 确认 `IA32_ARCH_CAPABILITIES(0x10A)` 存在（读不存在
  的 MSR 会 #GP），再读其 bit0（RDCL_NO）。
- RDCL_NO=1：硬件免疫 Meltdown，KPTI 双页表是纯性能损耗 -> 不部署（实测路径）。
- RDCL_NO=0 或 MSR 不存在：如实报告 `KPTI pending (P1: dual page tables)`。
- 设计立场：诚实检测优于形式主义部署——KPTI 完整实现（trampoline、每任务
  双 PML4、PCID）列入 P1，仅在真实需要的硬件上启用。

### 3.5 特权指令隔离（手册 9.2 合规）
`cpuid_ex / rdmsr64 / read_cr4 / write_cr4` 均为独立 `__attribute__((noinline))`
函数，注释标明输入/输出约束与 Clobber；`write_cr4` 带 `memory` 屏障。

## 四、既有文件改动

### kernel/mm/kmalloc.c —— 堆基址 KASLR-lite
- 新增 `static uint64_t g_heap_base`，`kheap_init` 用 rdtsc 熵（双轮
  murmur3 fmix64）做 0..4095 页随机滑移：
  `g_heap_base = KHEAP_BASE + (x & 0xFFF) * PAGE_SIZE`（滑移熵 12 bit，
  0..~16 MiB）。实测本轮 slide=+14796 KiB。
- 内核代码段 KASLR 需 PIE 内核 + 引导链重定位，随 P0-6 UEFI 路径交付。

### kernel/stack_canary.c —— 金丝雀熵重播种
- 新增 `stack_canary_reseed()`：rdtsc -> fmix64 散列 -> 低字节强制 0x00
  （NUL 终结子，阻断 strcpy 类连续溢出保持 canary 合法的利用路径）。
- 调用时机安全论证：kmain 极早期（serial_init 后立即），开中断/启 AP/建任务
  之前——唯一「序言已读旧 guard」的在飞栈帧是 kmain 自身，而 kmain 永不返回，
  尾声比对永不发生；本文件以 -fno-stack-protector 编译，自身无插桩。

### kernel/arch/x86_64/gdt.c
- BSP 新增 `g_ist_stack_nmi[8192]` 静态 NMI 栈（启动期），`g_tss.ist[1]` 指向；
- AP 新增 `g_ist2_ap[MAX_CPUS][4096]`，`tss->ist[1]` 指向；
- 新增 `tss_set_ist(int n, uint64_t stack_top)`（n=1..7，写 `g_tss.ist[n-1]`）。

### kernel/arch/x86_64/idt.c
- IDT 门 IST 字段：`#DF(8)=IST1`、`NMI(2)=IST2`、其余=0。
- NMI 与 #DF 必须**不同** IST：NMI 可打断 #DF 处理，若共栈，硬件切栈会把
  新帧压在同一栈顶互相踩帧。

### kernel/kmain.c 接线
```
serial_init() -> stack_canary_reseed()          （极早期）
... pmm/vmm/kheap/mm_selftest ...
mm_selftest() -> security_init()                （SMP 启动前）
```

### include/kernel/security.h（新建）/ include/kernel/gdt.h
- 声明 `security_init()` / `stack_canary_reseed()` / `tss_set_ist()`。

## 五、P0-8 覆盖清单对照

| 项 | 状态 | 说明 |
|---|---|---|
| SMAP (stac/clac) | 既有+复核 | boot.S 使能，copy_*_user 已围栏；本批读回 CR4 终审 |
| SMEP | 既有+复核 | 同上 |
| UMIP | **本批新增** | CR4.UMIP，防 sgdt/sidt 泄露 |
| NX/W^X 实效化 | **本批修复** | BSP EFER.NXE 漏开（严重），已修 |
| KASLR | 部分交付 | 堆基址滑移 + 用户栈 ASLR（既有）；代码段 KASLR 随 P0-6 PIE |
| KPTI | 检测交付 | RDCL_NO 免疫检测；免疫则不部署，不免疫标记 P1 |
| 栈保护 | 强化 | -fstack-protector-strong（既有）+ TSC 熵重播种 + NUL 字节 |
| IST 守卫页 | **本批新增** | #DF/NMI 独立 16KB 栈 + 底部守卫页，BSP 生效 |

## 六、验证方式

```bash
make iso && make run-headless      # IDE PIO 路径
make run-ahci-headless             # AHCI DMA 路径
```
验收点（均实测通过）：
1. `[security] SMEP=on SMAP=on UMIP=on NXE=on`；
2. IST 栈顶地址 = 0xffffd00000005000 / 0xffffd0000000b000（守卫布局公式吻合）;
3. `[kheap] KASLR slide` 每次启动随机变化；
4. `[smp] 4/4 CPUs online` + IPI selftest 3/3（AP 路径无回归——AP 页表含
   NX 位，若 BSP/AP NXE 不一致会在共享内核映射上不对称 #PF）；
5. pmm/vma selftest PASS、shell 上线、AHCI wr-test PASS（全链路无回归）。

GDB 深检（可选）：
```bash
qemu-system-x86_64 -s -S ... ; gdb build/kernel.elf
(gdb) target remote :1234
# 断在 security_init 之后：
(gdb) p/x $cr4          # 期望 bit11(UMIP)|bit20(SMEP)|bit21(SMAP) 置位
# 监视 MSR：QEMU monitor 'info registers' 看 EFER=...NXE LME SCE
```

## 七、涉及文件

| 文件 | 动作 |
|---|---|
| kernel/arch/x86_64/security.c | 新建（本批核心） |
| include/kernel/security.h | 新建 |
| boot/boot.S | 修复：EFER.NXE 探测使能 |
| kernel/arch/x86_64/gdt.c | NMI 静态 IST2 栈 + tss_set_ist |
| include/kernel/gdt.h | 声明 tss_set_ist |
| kernel/arch/x86_64/idt.c | #DF=IST1 / NMI=IST2 |
| kernel/mm/kmalloc.c | 堆基址随机滑移 |
| kernel/stack_canary.c | stack_canary_reseed |
| kernel/kmain.c | 接线（极早期重播种 + security_init） |

## 八、P0 阶段收尾状态

- P0-1 ACPI / P0-2 APIC / P0-4 时钟：step13~15 完成
- P0-9 panic 诊断：step16 完成
- P0-3 SMP：step17 完成
- P0-5 按需分页+COW+mmap：step18 完成
- P0-7 AHCI：step19 完成
- **P0-8 安全地基：本批完成**
- P0-6 UEFI 启动（OVMF+GOP）：唯一剩余项
