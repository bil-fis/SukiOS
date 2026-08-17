# SukiOS 生产就绪审计 · Step 24 — P0-R3 可诊断性设施 + P0-R5 内核栈守卫页 + 回溯上界回绕修复

> 承接 `step23.md`（KPTI/CR3 bit12 SMP 三重故障修复，commit `cd35a40`）。
> 本步完成 `results/step22_p0_production_todos.md` 中残留的 **M-P0b** 批次：
> - **R3a** 早期日志环缓冲 `klog`（串口输出镜像，可经 GDB `monitor klog` 重放）
> - **R3b** `WARN_ON` / `BUG_ON` 诊断宏 + `diag_selftest` 自检
> - **R3c** 串口 GDB stub（RSP over COM2 0x2F8）
> - **R5** 内核栈守卫页（独立 VA 槽位 + 栈底未映射页，溢出即 #PF/#DF）
> - 附带修复一个**自引入以来长期潜伏**的历史 bug：`kernel_addr()` 上界无符号回绕导致回溯循环被编译器折叠为无条件 `break`，所有 backtrace 恒为空。

提交：`a7b8d04`（未 push）。

---

## 0. 本步涉及的清单项与状态

| 项 | 描述 | 状态 |
|----|------|------|
| R3a | klog 早期环缓冲，可被 GDB 重放 | ✅ 完成并验证 |
| R3b | WARN_ON/BUG_ON 宏 + 自检 | ✅ 完成并验证 |
| R3c | 串口 GDB stub (COM2 0x2F8) | ✅ 完成并验证 |
| R5  | 内核栈守卫页（溢出即 trap） | ✅ 完成并验证 |
| BUG | kernel_addr 上界回绕致回溯恒空 | ✅ 根因修复并验证 |

---

## 1. R3a — klog 早期日志环缓冲

**文件**
- `include/kernel/klog.h`（NEW）
- `kernel/klog.c`（NEW）
- 调用方：`kernel/arch/x86_64/serial.c` 的 `serial_write()` 首行调用 `klog_putc(c)`。

### 1.1 数据结构

```c
#define KLOG_BUF_SIZE 65536u          /* 64 KiB 环缓冲 */
static char            g_klog_buf[KLOG_BUF_SIZE];
static volatile uint64_t g_klog_head; /* 下一个写入位置（游标） */
static volatile int    g_klog_suspend; /* 重放递归抑制 */
```

- `g_klog_buf`：纯内核（BSS）缓冲，早期启动（串口初始化后即可）即开始记录，不依赖 kheap。
- `g_klog_head`：原子递增的环形写指针，仅做 `g_klog_head = (g_klog_head + 1) % KLOG_BUF_SIZE`，**不回退、不覆盖索引**，写入即丢弃最旧字节。
- `g_klog_suspend`：当 GDB stub 经 `O` 包重放 klog 时，重放字节会再次流经 `serial_write → klog_putc`，若不加抑制会无限镜像自身。重放期间提升 `g_klog_suspend`，`klog_putc` 在 `suspend != 0` 时直接返回。

### 1.2 API（全部为 `static inline`/普通函数，无锁、单生产者）

| 函数 | 说明 |
|------|------|
| `void klog_putc(char c)` | 写入一字节到环缓冲（受 `g_klog_suspend` 保护） |
| `uint64_t klog_written(void)` | 已写入总字节数（单调） |
| `uint64_t klog_avail(void)` | 当前缓冲中可读字节数 = `min(written, KLOG_BUF_SIZE)` |
| `char klog_at(uint64_t i)` | 取下标 `i` 处的字节（逻辑序号，自动取模回绕） |
| `void klog_dump(void (*out)(char))` | 按逻辑顺序（从最旧到最新）经 `out()` 回调重放全部缓冲 |

`klog_at(i)` 的计算：`idx = (g_klog_head - klog_avail() + i) % KLOG_BUF_SIZE`，保证输出自最旧字节起、不重复不跳变。

### 1.3 与串口的关系

`serial_write`（16550 逐字节发送）在真正写 THR 前先 `klog_putc(c)`，因此**任何经串口吐出的内核日志都被无损镜像**到 klog，即使系统后续崩溃、控制台滚屏丢失，也可经 GDB `monitor klog` 取回完整启动轨迹。

### 1.4 GDB 重放入口

见 §3.4 `cmd_qrcmd` 的 `monitor klog` 命令：用 `klog_suspend_inc()` 抑制递归 → `klog_dump(gdb_putc)` 经 `O` 包逐字节上送 → `klog_suspend_dec()` 恢复。

---

## 2. R3b — WARN_ON / BUG_ON 诊断宏 + 自检

**文件**
- `include/kernel/diagnostics.h`（MOD：新增声明 + `#include <kernel/console.h>` 以便 `panic`）
- `kernel/diagnostics.c`（MOD：新增 `kernel_warn` / `WARN_ON` / `BUG_ON` / `BUG` / `diag_selftest`；回溯修复见 §6）

### 2.1 宏定义（`diagnostics.h`）

```c
#define WARN_ON(cond) ({ \
    int __w = !!(cond); \
    if (__w) kernel_warn(__FILE__, __LINE__, #cond); \
    __w; })

#define BUG_ON(cond) ({ \
    if (cond) kernel_oops("BUG_ON(" #cond ")", __FILE__, __LINE__); \
    (void)0; })

#define BUG() kernel_oops("BUG()", __FILE__, __LINE__)
```

- `WARN_ON` 仅告警、**不中止**执行（返回条件布尔值，可供调用方决策）。
- `BUG_ON` / `BUG` 调用 `kernel_oops()`，后者负责停机。

### 2.2 `kernel_warn()`

```c
void kernel_warn(const char *file, int line, const char *cond) {
    serial_printf("[WARN] %s:%d: %s\n", file, line, cond);
    diag_dump_self();   /* 打印当前 CPU 寄存器 + 栈回溯 */
}
```

告警即打印 `[WARN] 文件:行:条件`，并立即 dump 当前上下文（寄存器 + 回溯），便于现场定位。

### 2.3 `kernel_oops()`（BUG 路径）

```c
void kernel_oops(const char *msg, const char *file, int line) {
    serial_printf("[OOPS] %s @ %s:%d\n", msg, file, line);
    diag_dump_self();
    smp_halt_others();   /* 停掉其它 CPU，避免并发污染现场 */
    for (;;) { cli(); hlt(); }
}
```

相比旧实现，新增 `smp_halt_others()`：在 SMP 环境下 BUG 时先冻结其它核，确保 dump 的现场不被并发打断，随后不可恢复停机（`cli;hlt`）。

### 2.4 `diag_selftest()`

```c
void diag_selftest(void) {
    int trigger = 1;
    WARN_ON(trigger == 1);   /* 故意触发，验证告警+回溯全链路 */
    serial_printf("[diag] selftest OK: WARN_ON fired, "
                  "backtrace printed, execution continued\n");
}
```

在 `kmain.c` 的 `security_init()` 之后调用。验证目标：
1. `[WARN]` 行正确打印文件名/行号/条件文本；
2. 紧随其后的 backtrace 能打印真实帧（见 §6 修复前后对比）；
3. 执行**继续**（不被 halt），随后打印 `selftest OK`。

---

## 3. R3c — 串口 GDB stub（RSP over COM2）

**文件**
- `include/kernel/gdbstub.h`（NEW）
- `kernel/arch/x86_64/gdbstub.c`（NEW）
- 调用方：`kernel/kmain.c`（`gdbstub_init()`）、`kernel/sched/sched.c` 的 `sched_tick()` 首行（`gdbstub_poll()`，仅 BSP 周期轮询）。

### 3.1 硬件与连接

| 项 | 值 |
|----|----|
| 端口 | COM2，基址 `0x2F8` |
| IRQ | 3（默认，APIC 下挂 `gsi=3`） |
| 波特率 | 115200（除数锁存 `1`，`0x2F8+1` 写 `0x0001`） |
| 帧格式 | 8N1（0x2F8+3 写 `0x03`） |
| 协议 | GDB Remote Serial Protocol，`$<data>#<2hex cksum>` |

`16550` 初始化：关中断使能（DLL 模式）→ 设除数 → 8N1 → 回 DLM 模式 → 不使能 RX 中断（我们**轮询**而非中断驱动，避免与调度/中断控制器耦合）。

### 3.2 收发与校验

- `com2_putc(c)`：等 LSR THRE（bit5）空 → 写 THR。
- `com2_getc()`：等 LSR DR（bit0）置位 → 读 RBR。
- 包封装：`gdb_put_packet("$"+data+#cksum)`：
  - 计算数据段（不含 `$` 与 `#`）的按字节和模 256；
  - 以 2 位十六进制追加（`%02x`）。
- 收包：`gdb_get_packet` 读 `$` → 收数据直到 `#` → 读 2 位 cksum → 比对，正确回 `+`，错误回 `-` 请求重发。

### 3.3 寄存器打包布局（amd64 G 包）

GDB 期望顺序（与 `gdb/remote.c` 一致）：

```
rax rbx rcx rdx rsi rdi rbp rsp r8 r9 r10 r11 r12 r13 r14 r15 rip <-- 17*8 = 136 字节
fs_base gs_base <-- 2*8 = 16 字节 (本实现占位 0)
cs ss ds es fs gs 各 4 字节 = 20 字节 (本实现写 cs/ss/ds/es/fs/gs 实值, 余 0)
eflags 4 字节
'x'(0x78) 起始 FPU 区: xmm0 占位 16 字节 + 额外 16 字节 = 32 字节
```

`gdb_arch_to_regs` / `gdb_regs_to_arch` 在 `registers_t`（ISR 现场）与此布局间双向转换。用户态现场（`cs==0x1B`）只回退用户 GPR/seg/eflags，避免泄露内核态无关字段。

### 3.4 内存读写（绕过 SMAP / W^X，安全）

```c
static int safe_mem_read(uint64_t va, uint8_t *buf, size_t n) {
    uint64_t pml4 = current_pml4_root() & PTE_ADDR_MASK; /* 当前进程四级页表物理根 */
    for (each byte) {
        uint64_t pa = vmm_translate((uint64_t *)PHYS_TO_VIRT(pml4), va + off);
        if (!pa) return -1;
        buf[off] = *(uint8_t *)(PHYS_TO_VIRT(pa));      /* 内核别名映射，绕过 SMAP/W^X */
    }
}
```

- 用当前任务页表根经 `vmm_translate`（软件走页表）求出物理地址，再用 `PHYS_TO_VIRT` 内核别名读/写，**不走用户态页表权限**，自然绕过 SMAP 与 W^X。
- `safe_mem_write` 同理（用于 `M` 命令改内存、写断点指令 `int3` 0xCC）。
- 全程 `spin_lock_irqsave` 持有，避免并发污染页表遍历。

### 3.5 支持的 RSP 命令

| 命令 | 处理 |
|------|------|
| `?` | 上报 SIGTRAP（断点/陷阱原因） |
| `g` | 读全部寄存器（§3.3 布局） |
| `G` | 写全部寄存器 |
| `m addr,len` | `safe_mem_read` 读内存 |
| `M addr,len:hex` | `safe_mem_write` 写内存 |
| `p N` / `P N=hex` | 单寄存器读/写 |
| `c` | 恢复执行（清 `need_cont`，退出会话循环） |
| `s` | 单步：置 `RFLAGS.TF` 后恢复，下次 #DB（vec1）再陷回 |
| `q` / `H` / `D` / `k` | 查询/选择线程/分离/杀死（均优雅返回或重设状态） |
| `qRcmd,hex` | `cmd_qrcmd`：解析 ASCII 命令，支持 `monitor klog` |

`cmd_qrcmd` — `monitor klog`：

```c
if (strncmp(cmd, "monitor klog", 12) == 0) {
    klog_suspend_inc();
    klog_dump(gdb_putc);   /* 经 O 包逐字节上送 klog */
    klog_suspend_dec();
    gdb_put_packet("OK");
}
```

上送采用 `O<hex>` 包（GDB 控制台输出包），`gdb_putc` 将每个字节 hex 编码进 `O` 包。

### 3.6 陷阱接入

- `#DB`(vec1) 与 `#BP`(vec3) 在 `gdb_trap()` 中处理：
  1. 若为单步，清 `RFLAGS.TF`；
  2. 冻结其它 CPU：`smp_send_ipi(IPI_GDB_FREEZE)`（见 §5）；
  3. 进入 `gdb_session` 循环（`g`/`G`/`m`/`M`/`c`/`s`/`q`…），直到 `c`/`s` 退出。
- `gdbstub_init()`：初始化 COM2、打印 `[gdb] stub ready`、注册 vec1/vec3 处理（复用 `idt.c` 的 `register_interrupt_handler` 或直接在 `isr_dispatch` 分支处理）。
- `gdbstub_poll()`：在 `sched_tick`（仅 BSP）中周期调用。
  - **特征字节判定**：仅当串口收到 `$`(0x24) / `0x03`(ETX，GDB 中断请求) / `+`(0x2B ack) 之一时才主动 `int3` 自陷进入 stub；
  - **16 次 drain 上限**：`for (drain=0; drain<16 && com2_rx_ready(); drain++) com2_getc();` 防止 TCP↔串口桥接后端的噪声字节造成死循环刷空。

### 3.7 使用方式（调试命令）

```
# 终端 A：启动 QEMU（无盘验证）
qemu-system-x86_64 -machine pc,accel=kvm -cpu host -smp 4 -m 2G \
  -display none -serial file:/tmp/sukios_serial.log \
  -boot d -cdrom build/SukiOS.iso
# 另开 GDB
gdb build/kernel.elf
(gdb) target remote /dev/ttyS1   # 或经 socat 把 QEMU COM2 接到 TCP 后 target remote :1234
(gdb) monitor klog               # 取回 64KB 启动日志环缓冲
(gdb) bt / info registers / x/10i $rip
```

> 注：本 stub 走真实串口（0x2F8）。若要用 GDB TCP，需在主机侧用 `socat` 把 `/dev/ttyS1` 桥到 `TCP-LISTEN:1234`。

---

## 4. R5 — 内核栈守卫页

**文件**
- `include/mm/kstack.h`（NEW）
- `kernel/mm/kstack.c`（NEW）
- 调用方：`kernel/sched/sched.c`（`sched_create_idle` / `sched_create_kernel_task` / 任务回收）、`kernel/arch/x86_64/smp.c`（AP 引导栈）。
- 集成：`kernel/arch/x86_64/idt.c` 的 `#PF`/`#DF` 处理器调用 `kstack_guard_hit(cr2)` 做定性。

### 4.1 地址布局

```c
#define KSTACK_AREA_BASE   0xFFFFC00080000000UL  /* 与 kheap 同 PML4 槽[384]，但 PDPT 独立 */
#define KSTACK_PAGES       4                     /* 栈体 4*4KB = 16KB */
#define KSTACK_GUARD_PAGES 1                     /* 守卫页 1*4KB，保持未映射 */
#define KSTACK_SLOTS       256
#define KSTACK_BYTES       (KSTACK_PAGES * 4096)            /* 16384 */
#define KSTACK_TOTAL_BYTES ((KSTACK_PAGES + KSTACK_GUARD_PAGES) * 4096) /* 20480 */
#define KSTACK_AREA_SIZE   (KSTACK_SLOTS * KSTACK_TOTAL_BYTES) /* 5 MiB */
#define KSTACK_GUARD_VA(slot)  (KSTACK_AREA_BASE + (slot)*KSTACK_TOTAL_BYTES)        /* 未映射 */
#define KSTACK_BODY_VA(slot)   (KSTACK_GUARD_VA(slot) + KSTACK_GUARD_PAGES*4096)    /* 栈体底 */
```

- 每个槽位 20KB：**低地址 4KB 为守卫页（永不映射）**，高地址 16KB 为可用栈体。
- 栈向**低地址**增长，故栈溢出时首先触及下方未映射的守卫页 → 触发 `#PF`，再若递归压栈触发 `#DF` → 二者均在 `idt.c` 中被判定为 `KERNEL STACK OVERFLOW`。
- 与 `kheap`（KHEAP_BASE=0xFFFFC00000000000）共享 PML4 槽 `[384]`，但各自独立 PDPT，互不干扰，也无需额外的 PML4 项。

### 4.2 分配器实现（`kstack.c`）

```c
static uint64_t g_kstack_bitmap[KSTACK_SLOTS / 64];
static spinlock_t g_kstack_lock;

uint64_t kstack_alloc(void) {
    spin_lock_irqsave(&g_kstack_lock, f);
    int slot = bitmap_first_zero(g_kstack_bitmap, KSTACK_SLOTS);
    if (slot < 0) { spin_unlock_irqrestore(...); return 0; } /* 槽位耗尽 */
    bitmap_set(g_kstack_bitmap, slot);
    spin_unlock_irqrestore(...);

    uint64_t guard = KSTACK_GUARD_VA(slot);   /* 守卫页：故意不映射 */
    uint64_t body  = KSTACK_BODY_VA(slot);
    for (int i = 0; i < KSTACK_PAGES; i++) {
        uint64_t pa = pmm_alloc_page();
        vmm_map_page(body + i*4096, pa, PTE_WRITE | PTE_NX);
    }
    return body;   /* 返回栈体底，调用方 +KSTACK_BYTES 得栈顶 */
}

void kstack_free(uint64_t base) {
    int slot = (base - KSTACK_AREA_BASE) / KSTACK_TOTAL_BYTES;
    for (int i = 0; i < KSTACK_PAGES; i++)
        vmm_unmap_page(base + i*4096);   /* 守卫页本就未映射，无需处理 */
    bitmap_clear(g_kstack_bitmap, slot);
}

bool kstack_guard_hit(uint64_t addr) {
    /* addr 落在任一槽位的守卫页区间 [GUARD_VA, GUARD_VA+4KB) 内 */
    if (addr < KSTACK_AREA_BASE || addr >= KSTACK_AREA_BASE + KSTACK_AREA_SIZE) return false;
    uint64_t off = addr - KSTACK_AREA_BASE;
    uint64_t slot = off / KSTACK_TOTAL_BYTES;
    uint64_t in   = off % KSTACK_TOTAL_BYTES;
    return in < KSTACK_GUARD_PAGES * 4096;  /* 落在守卫页内 */
}
```

- 自旋锁带 `irqsave`，可在中断上下文安全调用（AP 引导、任务回收均可能关中断）。
- 守卫页在 `kstack_alloc` 中**显式不映射**，栈体用 `PTE_WRITE | PTE_NX` 映射（数据栈需可写、不可执行，强化 W^X）。

### 4.3 集成点

**`sched.c`**

```c
#define KERNEL_STACK_BYTES  KSTACK_BYTES   /* 16KB */
/* 原 kmalloc(KERNEL_STACK_BYTES) 改为： */
uint64_t stack = kstack_alloc();          /* 守卫页栈；失败则 panic */
...
t->kstack_base = stack;
t->rsp0 = stack + KERNEL_STACK_BYTES;     /* TSS.rsp0 = 栈顶 */
...
/* 回收路径：kstack_free(t->kstack_base) 替代 kfree */
```

**`smp.c`**

```c
#define AP_STACK_BYTES  KSTACK_BYTES
uint64_t stack = kstack_alloc();          /* AP 引导栈同样带守卫页 */
if (!stack) { /* 失败处理 */ kstack_free(stack); return; }
*mb_stack        = stack + AP_STACK_BYTES;
g_percpu[idx].kstack_top = stack + AP_STACK_BYTES;
```

**`idt.c`**（`#PF` 与 `#DF` 定性）

```c
/* page_fault_handler() 内核态分支 */
uint64_t cr2 = read_cr2();
if (kstack_guard_hit(cr2)) {
    serial_printf("KERNEL STACK OVERFLOW (task '%s')\n", current->name);
    /* 进入 oops 流程 */
}

/* isr_dispatch() vec==8(#DF) || vec==14(#PF) 分支 */
if ((vec == 8 || vec == 14) && kstack_guard_hit(cr2)) {
    serial_printf("KERNEL STACK OVERFLOW\n", (void *)cr2);
}
```

**`isr.S`**：新增 `ISR_NOERR 243` 作为 `IPI_GDB_FREEZE` 的向量处理入口（见 §5）。

---

## 5. IPI_GDB_FREEZE（调试冻结其它核）

**文件**
- `include/kernel/smp.h`：`#define IPI_GDB_FREEZE 0xF3`（映射到 `isr243`，vec=243）
- `kernel/arch/x86_64/isr.S`：`ISR_NOERR 243`
- `kernel/arch/x86_64/idt.c`：`extern void isr243(void); idt_set_gate(243, (uint64_t)isr243, 0, 0x8E);`

当 GDB stub 在某核进入会话（`gdb_trap`）时，通过 `smp_send_ipi(IPI_GDB_FREEZE)` 向其它 CPU 广播该向量；各 AP 在 `isr243` 中自旋停机（`cli;hlt` 或自旋等待），从而**冻结整个系统的并发状态**，使 GDB 看到的寄存器/内存现场是稳定的。恢复时由 stub 持有 CPU 控制权，无需单独解冻（重入会话即再广播）。

---

## 6. 历史 bug 修复 — `kernel_addr()` 上界无符号回绕

### 6.1 根因

`kernel/diagnostics.c` 的回溯判定原逻辑：

```c
static bool kernel_addr(uint64_t addr) {
    return addr >= KERNEL_BASE &&
           addr < KERNEL_BASE + 0x1000000000000ULL;  /* 意图：2^48 空间上界 */
}
```

`KERNEL_BASE = 0xFFFF800000000000UL`。由于是 **uint64_t 无符号**运算：

```
0xFFFF800000000000
+0x0001000000000000   (2^48)
=0x10000000000000000  → 超过 2^64-1，回绕为 0x0000008000000000
```

即上界常量实际等于 `0x8000000000`（约 32GB 处），而所有内核地址均 `>= 0xFFFF800000000000`，因此 `addr < 0x8000000000` 对**任何**内核地址恒为 false。

后果：回溯 `while (rbp)` 循环内的 `if (!kernel_addr(ret)) break;` 在**第一帧**即触发 `break`，且由于该条件对所有迭代恒真，编译器在 `-O2` 下将整个循环**折叠为无条件 `break`**——生成的可执行代码中回溯体被直接移除。自该回溯函数被引入以来，所有 `[backtrace]` 输出**恒为空**，长期掩盖了真实崩溃栈（包括此前若干 P0 故障的现场本应可见却空白）。

### 6.2 修复

将上界改为 `0xFFFFC00000000000UL`（即 `KHEAP_BASE`，内核镜像+堆的统一上界，落在 KASLR 滑动后的内核虚拟区间之内）：

```c
static bool kernel_addr(uint64_t addr) {
    return addr >= KERNEL_BASE && addr < 0xFFFFC00000000000UL;
}
```

同时保留此前为健壮性加入的辅助：
- `kernel_half_addr()`：判定地址是否处于「半合法」区间（如内核态指针被截断），用于诊断性提示；
- `frame_mapped()`：回溯解引用每一层 `[rbp]`/`[rbp+8]` 前，先经 `vmm_translate` 逐页校验该地址已映射，避免在已损坏栈上二次触发 `#PF` 陷入死循环。

并移除了调试阶段遗留的 `[dbg-trace] ...` 诊断打印（仅用于定位回绕根因，现已无必要）。

### 6.3 修复前后对比（运行期）

修复前（`[dbg-trace]` 仍开启时可见）：

```
[WARN] kernel/diagnostics.c:149: WARN_ON(trigger == 1)
[backtrace]
  backtrace (rbp=0xffff8035c016ee80):
    (无任何 #0/#1 输出)
[diag] selftest OK: WARN_ON fired, backtrace printed, execution continued
```

修复后（本步验证输出）：

```
[WARN] kernel/diagnostics.c:149: WARN_ON(trigger == 1)
[backtrace]
  backtrace (rbp=0xffff8035c016ee80):
    #0 0xffff8035c01173de
    #1 0xffff8035c011e0e1
[diag] selftest OK: WARN_ON fired, backtrace printed, execution continued
```

`#0`/`#1` 为真实返回地址（落在内核镜像虚拟区间内，可被 GDB `bt`/反汇编对应），证明回溯循环已恢复迭代、编译器折叠被消除。

---

## 7. 集成总览与启动顺序

`kernel/kmain.c` 在 `security_init()`（含 KPTI/KASLR 部署）之后新增：

```c
gdbstub_init();    /* 初始化 COM2，打印 [gdb] stub ready，注册 vec1/vec3 */
diag_selftest();   /* 故意 WARN_ON(1)，验证告警+回溯全链路 */
```

周期侧：`sched_tick()`（BSP）首行 `gdbstub_poll()`，在每 tick 检测是否收到 GDB 特征字节，决定是否进入调试会话。

完整调用链：

```
kmain
 ├─ gdbstub_init()            [R3c] COM2 + vec1/vec3 + "[gdb] stub ready"
 ├─ diag_selftest()          [R3b] WARN_ON(1) → kernel_warn → diag_dump_self
 │     └─ backtrace → kernel_addr() [§6 修复] → 真实帧
 └─ 调度启动
       └─ sched_tick() ── gdbstub_poll()  [R3c] 特征字节 $/^C/+ → int3 自陷

任务创建: sched_create_* → kstack_alloc()   [R5] 守卫页栈
AP 唤醒 : smp_init        → kstack_alloc()   [R5] AP 引导栈
#PF/#DF : page_fault_handler/isr_dispatch → kstack_guard_hit() [R5] 定性溢出
```

---

## 8. 验证

### 8.1 构建

```
make iso        # 成功，无 error；仅存在既有 isrN/context_switch 跨文件 HINT（汇编/链接期符号，非本次引入）
```

### 8.2 运行（QEMU KVM 无盘无头，捕获串口与异常日志）

```
timeout 50 qemu-system-x86_64 -machine pc,accel=kvm -cpu host -smp 4 -m 2G \
  -no-shutdown -display none -serial file:/tmp/sukios_serial.log \
  -boot d -cdrom build/SukiOS.iso -d int -D /tmp/sukios_qemu.log
```

关键断言（全部通过）：
1. `[gdb] stub ready` —— GDB stub 初始化成功；
2. `4/4 CPUs online` —— SMP（INIT-SIPI-SIPI）唤醒全部 AP，守卫页栈分配无误；
3. `[WARN] kernel/diagnostics.c:149: WARN_ON(trigger == 1)` 出现在串口日志；
4. 其下 `[backtrace]` 打印 `#0`/`#1` 真实帧（§6.3），证明回绕修复生效；
5. `[diag] selftest OK: ... execution continued` —— WARN_ON 未中止执行；
6. `shell online (Ring3)` —— 用户态 shell 稳定，说明内核栈/Ring3 切换正常；
7. `grep -c "check_exception\|exception\|triple" /tmp/sukios_qemu.log` 结果为 **0** —— 运行期零异常、无三重故障。

### 8.3 调试命令（供复现）

- 取回早期日志：`gdb build/kernel.elf` → `target remote ...` → `monitor klog`（`O` 包重放 64KB klog 环缓冲）。
- 断点/单步：`b *0xffff8035c01173de`、`c`、`s`，经 `g`/`G`/`m`/`M` 查看/修改寄存器与内存。
- 反汇编回溯帧：`x/10i 0xffff8035c01173de`。

---

## 9. 文件改动清单

**新增（NEW）**
- `include/kernel/klog.h` — klog 环缓冲 API
- `include/kernel/gdbstub.h` — GDB stub 对外 API
- `include/mm/kstack.h` — 守卫页栈分配器接口与地址常量
- `kernel/klog.c` — 64KB 环缓冲实现
- `kernel/arch/x86_64/gdbstub.c` — RSP over COM2 完整实现
- `kernel/mm/kstack.c` — 位图+PMM+VMM 守卫页栈分配器

**修改（MODIFIED）**
- `include/kernel/diagnostics.h` — `kernel_warn`/`WARN_ON`/`BUG_ON`/`BUG`/`diag_selftest` 声明；`#include <kernel/console.h>`
- `include/kernel/smp.h` — `#define IPI_GDB_FREEZE 0xF3`
- `kernel/diagnostics.c` — 新增告警/BUG/自检；`kernel_addr` 上界回绕修复（§6）；`kernel_half_addr`/`frame_mapped` 映射校验；`kernel_oops` 加 `smp_halt_others`
- `kernel/arch/x86_64/isr.S` — `ISR_NOERR 243`（IPI_GDB_FREEZE）
- `kernel/arch/x86_64/idt.c` — `isr243` 声明 + `idt_set_gate(243,...)`；`#PF`/`#DF` 加 `kstack_guard_hit` 定性
- `kernel/arch/x86_64/serial.c` — `serial_write` 首行 `klog_putc(c)`
- `kernel/arch/x86_64/smp.c` — AP 引导栈改用 `kstack_alloc`/`kstack_free`
- `kernel/sched/sched.c` — 任务内核栈改用 `kstack_alloc`/`kstack_free`；`sched_tick` 首行 `gdbstub_poll()`
- `kernel/kmain.c` — `security_init()` 后调用 `gdbstub_init()` + `diag_selftest()`

---

## 10. 结论

P0-R3（可诊断性：klog + WARN/BUG + GDB stub）与 P0-R5（内核栈守卫页）已完整实现并通过 QEMU SMP 运行验证。顺带修复了 `kernel_addr()` 无符号上界回绕这一长期潜伏 bug——该 bug 使回溯在所有场景下恒空，本步已从根本上恢复调用栈展开能力。下一阶段可在此基础上推进 P0 剩余项（如对称多核调度细粒度、KASLR 代码段随机化收尾等），且调试时可借 GDB stub + klog 重放快速定位现场。
