# step23：KPTI/SMP 三重故障根因定位与修复（CR3 bit12 语义冲突）

## 摘要

上一阶段（step22 后）部署 KPTI（KAISER 双页表）后，系统在 SMP 启动期出现
**无任何串口异常输出的三重故障重启**：`cpu1 online -> created idle2 -> 机器复位`。
本阶段通过 QEMU `-d int` 异常链捕获 + KASLR slide 反解 + `objdump` 指令级比对，
定位到一个**设计级根因**并彻底修复，随后回退全部临时调试代码，KPTI 重新启用，
4 核 SMP + Ring3 服务全流程稳定。

## 一、故障现象

- QEMU `-machine pc -cpu qemu64 -smp 4`，启动到 `[smp] cpu1 online (lapic_id=1)`、
  `[sched] created idle2 for cpu2` 后瞬间整机复位（无 `-no-reboot` 时无限重启循环）。
- 串口完全没有 `[EXC]` / panic 输出——说明崩溃根本没有走到 `isr_dispatch`，
  属于硬件级三重故障。
- 关键排除实验：临时 `if (0)` 强制禁用 KPTI（`g_kpti_enabled=0`），**崩溃模式
  完全相同**——首次误导性结论是"与 KPTI 无关"；真相见下文（入口汇编不检查
  该标志，禁用只是没生效）。

## 二、定位过程（可复现方法论）

### 2.1 捕获异常链

```bash
qemu-system-x86_64 -machine pc -cpu qemu64 -smp 4 -m 2G -no-reboot \
    -display none -serial file:/tmp/suki.log -d int -D /tmp/qemu.int \
    -boot d -cdrom build/SukiOS.iso
```

`/tmp/qemu.int` 中三重故障前的完整异常链（`1:` 前缀 = cpu1）：

```
check_exception old: 0xffffffff new 0xe
     1: v=0e e=0018 i=0 cpl=0 IP=0008:ffff802cc011b26c pc=ffff802cc011b26c
        SP=0010:ffffc0000081bf48 CR2=ffff802cc011b26c
        CR0=e0000013 CR2=ffff802cc011b26c CR3=0000000000100000 CR4=00000220
check_exception old: 0xe new 0xe        ; #PF 处理自身再 #PF
check_exception old: 0x8 new 0xe        ; #DF(向量8) 又 #PF -> 三重故障
```

解读：
- `v=0e e=0018`：#PF，错误码 0x18 = bit3(**RSVD** 页表项含保留位) + bit4(**ID**
  取指访问)；
- `CR2 == RIP`：崩溃是**取指令本身缺页**；
- `CR3=0x100000`：**指向内核映像物理起点（0x100000 = 内核加载地址）**，这不是
  任何合法 PML4——页表行走读到的是内核第一页的指令字节，随机 bit 里必然有
  保留位 → RSVD #PF。

### 2.2 反解 KASLR 得到链接地址

崩溃 RIP `ffff802cc011b26c`；本次启动 KASLR slide = `0x2CC0000000`（1GB 粒度
槽位随机）。链接地址 = RIP − slide = `ffff80000011b26c`。

### 2.3 指令级比对（铁证）

```
$ nm build/kernel.elf | grep isr_common
ffff80000011b240 t isr_common

$ objdump -d build/kernel.elf --start-address=0xffff80000011b240 ...
ffff80000011b257: mov  %cr3,%rax
ffff80000011b25a: push %rax
ffff80000011b25b: test $0x1000,%rax
ffff80000011b261: je   ffff80000011b26c
ffff80000011b263: and  $0xffffffffffffefff,%rax   ; 清 bit12
ffff80000011b269: mov  %rax,%cr3                   ; <-- 写入垃圾 CR3
ffff80000011b26c: cld                              ; <-- 崩溃 RIP！
```

崩溃 RIP 正是 `isr_common` 中 KPTI 入口"清 bit12 写 CR3"之后的**下一条指令**：
写 CR3 全量刷 TLB → `cld` 的取指需重新页表行走 → 垃圾页表 → #PF(RSVD+ID)。

### 2.4 根因闭环

```
$ nm build/kernel.elf | grep p4_table
0000000000101000 d p4_table          ; boot 页表物理地址，bit12 = 1 ！
```

完整因果链：

1. `boot/boot.S` 的 `p4_table` 只按 4KB 对齐，链接后恰好落在物理 `0x101000`
   （bit12=1）；`vmm_init()` 直接沿用它作 `g_kernel_pml4`；
2. AP 经 `ap_boot.S` 装载同一 CR3=0x101000 进入 `ap_main`，开中断后收到首个
   中断（LAPIC 定时器/IPI）；
3. `isr_common` 入口把「CR3 bit12=1」**误判为 KPTI 影子视图**，清 bit12 →
   CR3=0x100000（内核映像开头）；
4. 写 CR3 刷 TLB → 下一条 `cld` 取指走垃圾页表 → #PF e=0x18、CR2=RIP；
5. #PF 派发同样要经 IDT/取指 → 递归 #PF → #DF → #DF 处理又 #PF → 三重故障。

**为何"禁用 KPTI"不能止血**：`isr_common` 与 `syscall_entry` 的入口清位逻辑
是无条件的（不读 `g_kpti_enabled`），只要 CR3 bit12=1 就中招。

**权威依据（本地 osdev_wiki/wiki.osdev.org/CPU_Registers_x86-64，CR3 表）**：
- CR4.PCIDE=0 时 CR3 bit0-11 中仅 PWT(3)/PCD(5) 有义，其余保留；
- **bit12-63 = PML4 物理基址**（"Physical Base Address of the PML4 …… must be
  page aligned"）——bit12=1 是完全合法的 4KB 对齐地址。
- 因此「用 CR3 bit12 做内核/影子视图标签」成立的**前提**是：所有可作"内核
  视图"的 PML4 物理地址必须 8KB 对齐（bit12 恒 0）。任务地址空间的成对分配
  （`pmm_alloc_pages_aligned(2,2)`）已满足，唯独 boot 期 `p4_table` 漏网。

## 三、修复内容（3 处防线 + 1 处断言）

### 3.1 boot/boot.S —— p4_table 改 8KB 对齐（根修复）

```
.section .boot.bss, "aw", @progbits
.align 8192            ; 原为 .align 4096
p4_table:   .skip 4096
```

修复后 `nm` 验证：`p4_table = 0x102000`（bit12=0）。浪费 ≤4KB 引导期 BSS，
换取「内核 PML4 bit12 恒 0」的硬保证。

### 3.2 kernel/arch/x86_64/isr.S —— 入口清位增加 g_kpti_enabled 门控

```asm
    movq %cr3, %rax
    pushq %rax
    movabsq $g_kpti_enabled, %rcx
    cmpb $0, (%rcx)
    je 1f                    /* KPTI 未启用：CR3 原样使用 */
    testq $0x1000, %rax
    jz 1f
    andq $-4097, %rax
    movq %rax, %cr3
1:
```

理由：KPTI 未启用路径的任务 PML4 是**单页** `pmm_alloc_page()` 分配，物理地址
bit12 同样可能为 1；无门控时该任务收到中断就会复现同一崩溃。出口路径无需
门控——「保存-还原」语义在 saved_cr3 == 当前 CR3 时不写，天然安全。

### 3.3 kernel/arch/x86_64/syscall_entry.S —— syscall 入口同样门控

```asm
    pushq %rax
    movabsq $g_kpti_enabled, %rax
    cmpb  $0, (%rax)
    je    11f
    movq  %cr3, %rax
    testq $0x1000, %rax
    jz    11f
    andq  $-4097, %rax
    movq  %rax, %cr3
11: popq  %rax
```

（出口与 `enter_user_mode` 原本就有 `g_kpti_enabled` 检查，现在入口/出口对称。）

### 3.4 kernel/mm/vmm.c —— vmm_init 防回归断言

```c
g_kernel_pml4 = read_cr3() & PTE_ADDR_MASK;
if (g_kernel_pml4 & (1UL << 12)) {
    panic("vmm_init: kernel PML4 %p has bit12=1 (breaks KPTI CR3 tagging; "
          "p4_table must be 8KB-aligned)", (void *)g_kernel_pml4);
}
```

未来若更换引导页表来源（如 UEFI loader），违反不变量会在启动期立即 panic
并给出明确原因，而不是运行期随机三重故障。

## 四、KPTI CR3 bit12 标签的完整不变量（修复后）

| PML4 来源 | 分配方式 | bit12 | 说明 |
|---|---|---|---|
| boot `p4_table`（内核全局） | `.align 8192` 静态 | 恒 0 | 3.1 保证 |
| 任务地址空间（KPTI 开） | `pmm_alloc_pages_aligned(2,2)` 8KB 对齐对 | 偶页=0（内核视图），奇页=1（影子） | 原有设计 |
| 任务地址空间（KPTI 关） | `pmm_alloc_page()` 单页 | 可能为 1 | 3.2/3.3 门控保证不误清 |

即：**`g_kpti_enabled=1` 时「bit12=1 ⇔ 影子视图」全局成立；=0 时任何路径都
不触碰 bit12。**

## 五、回退的临时调试代码

| 文件 | 回退内容 |
|---|---|
| `kernel/arch/x86_64/security.c` | `if (0)` 强制禁用 KPTI → 恢复 `if (!rdcl_no) g_kpti_enabled = 1;` |
| `kernel/arch/x86_64/ap_boot.S` | 删除 COM2 调试字符 '2'/'3'/'4'/'5' 与 `ap_com_putc` 例程 |
| `kernel/arch/x86_64/smp.c` | 删除 `ap_main` 的 'X' 内联输出与全部 `[ap] cpu%u ...` 分步标记、CR4 打印 |
| `kernel/arch/x86_64/idt.c` | 删除 `isr_dispatch` 的 `[EXC]` 入场 dump（常规诊断由既有 unhandled 路径 + `kernel_oops` 承担） |
| `kernel/kmain.c` | 删除 8 处 `[dbg]` 阶段标记 |

调试期沉淀的有效方法（记录备用，不留代码）：
- AP 早期排错用独立 COM2(0x2F8) 轮询输出 + QEMU 双 `-serial file:`，避免与
  BSP 的 COM1 kprintf 竞争产生乱码；
- `ap_boot.S` 内加 `call` 前必须先切好 64 位 RSP（32 位段遗留 RSP 高 32 位
  非零，`call` 压栈会写野地址）；
- 无 `[EXC]` 输出的重启 = 三重故障，直接上 `-d int -D file` 抓
  `check_exception` 链，配合 KASLR slide 反解 + `objdump` 指令级定位。

## 六、验证

### 6.1 构建

```bash
make iso     # L4 check 通过；nm 确认 p4_table=0x102000 (bit12=0)
make disk
```

### 6.2 无盘冒烟（40 秒，-d int 全程监控）

```bash
qemu-system-x86_64 -machine pc -cpu qemu64 -smp 4 -m 2G -display none \
    -serial file:/tmp/suki.log -d int -D /tmp/qemu.int -boot d -cdrom build/SukiOS.iso
```

结果：
- `check_exception` 计数 = **0**（全程零 CPU 异常，无重启）；
- `[vmm] kernel PML4 @ phys 0x102000`；
- `[security] Meltdown: unknown (no ARCH_CAPABILITIES) -> KPTI ACTIVE (KAISER
  dual page tables ...)`；
- `[security] KASLR: ENABLED slide=124928 MiB`；
- `[smp] 4/4 CPUs online`、`IPI selftest: 3/3 APs acked RESCHED`；
- Ring3 任务跨核运行：`input-server pid=5 cpu=1 cr3=0x39e000`、
  `shell pid=6 cpu=2 cr3=0x3f2000`（CR3 均为偶页 = 成对分配内核视图）；
- shell 横幅经 IPC 管线输出（高频 syscall/mach_msg 路径在 KPTI 下工作正常）。

### 6.3 带盘全流程（45 秒）

```bash
qemu-system-x86_64 ... -drive file=build/disk.img,format=raw,if=ide
```

结果：`disk-srv`（内核态 DISK_PORT）+ `fs-server`（Ring3 FAT32, cpu2,
cr3=0x3a2000）+ `shell` 全部在线，`Type 'help' for commands.` 提示符出现，
稳定无异常。

### 6.4 GDB 复核命令（备查）

```bash
qemu-system-x86_64 -machine pc -cpu qemu64 -smp 4 -m 2G -s -S \
    -display none -serial stdio -boot d -cdrom build/SukiOS.iso
# 另一终端：
gdb build/kernel.elf -ex 'target remote :1234' \
    -ex 'break isr_common' -ex 'continue' \
    -ex 'info registers cr3'    # 确认 bit12 语义与视图一致
```

## 七、涉及文件清单

| 文件 | 变更 |
|---|---|
| `boot/boot.S` | `p4_table` `.align 4096` → `.align 8192` + 设计注释 |
| `kernel/arch/x86_64/isr.S` | KPTI 入口清位增加 `g_kpti_enabled` 门控 |
| `kernel/arch/x86_64/syscall_entry.S` | 同上（syscall 入口） |
| `kernel/mm/vmm.c` | `vmm_init` 增加内核 PML4 bit12=0 断言 |
| `kernel/arch/x86_64/security.c` | 回退 KPTI 强制禁用调试补丁 |
| `kernel/arch/x86_64/ap_boot.S` | 回退 COM2 调试输出 |
| `kernel/arch/x86_64/smp.c` | 回退 `[ap]` 调试标记 |
| `kernel/arch/x86_64/idt.c` | 回退 `[EXC]` 入场 dump |
| `kernel/kmain.c` | 回退 `[dbg]` 阶段标记 |
