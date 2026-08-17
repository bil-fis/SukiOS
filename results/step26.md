# step26 — P0-R8：ACPI S5 软关机（FADT + DSDT `_S5` 完整解析）与 R7 用户态 syscall clobber 修复

本步销账 `results/step22_p0_production_todos.md` 中的 **R8（电源管理：ACPI S5 关机）**，
并顺带修复审计项 **R7（Ring3 服务健壮性）** 中暴露出的一个真实内存/寄存器破坏 bug。

---

## 一、总览

| 项 | 内容 | 状态 |
| --- | --- | --- |
| R8-1 | FADT 电源管理寄存器解析（PM1a/PM1b Control Block） | ✅ 完成 |
| R8-2 | DSDT AML `_S5` 对象解析，提取真实 SLP_TYP | ✅ 完成 |
| R8-3 | `acpi_poweroff()` 触发平台断电 | ✅ 完成 |
| R8-4 | `SYS_REBOOT` 扩展 mode 参数（0=重启 / 1=关机） | ✅ 完成 |
| R8-5 | Shell `poweroff` / `shutdown` 命令 | ✅ 完成 |
| R7-a | `suki_syscall5` 内联汇编 clobber 缺失 r9/rbx 导致调用号被破坏 | ✅ 修复 |

端到端验证：QEMU 内输入 `poweroff` → 进程以 `rc=0` 自行退出（真实断电）；
输入 `reboot` → 系统完整重启一次（`shell online` 出现 2 次）。

---

## 二、R8：ACPI S5 软关机

### 2.1 背景与 ACPI 规范依据

ACPI 的软关机（Soft-Off，睡眠状态 S5）由 OSPM 主动写 PM1 控制寄存器完成：

```
PM1_CNT 写入值 = (SLP_TYP << 10) | SLP_EN(1 << 13)
```

其中两个信息缺一不可，且**分别来自两张不同的表**：

1. **PM1a_CNT_BLK / PM1b_CNT_BLK（I/O 端口地址）** —— 来自 **FADT**（签名 `FACP`）。
2. **SLP_TYP（S5 对应的睡眠类型编码）** —— **不在 FADT 内**，必须从
   **DSDT** 的 ACPI 命名空间对象 `_S5` 中解析（AML 字节码）。

这一点是本次实现踩坑最深的地方：早期实现凭"通用值"硬编码 `SLP_TYP=5`，
而 QEMU/SeaBIOS 的 DSDT 实际给出的是 **`SLP_TYP=0`**，导致写入值错误。

### 2.2 FADT 字段偏移（关键修正）

文件：`kernel/acpi/acpi.c` → `acpi_parse_fadt()`

按 ACPI 6.x 规范 Table 5-33（自 ACPI 1.0 起偏移固定）：

| 偏移 | 宽度 | 字段 | 用途 |
| --- | --- | --- | --- |
| `0x04` | 4 | Length | 判断是否含 ACPI 2.0+ 扩展字段 |
| `0x08` | 1 | Revision | 1 = ACPI 1.0，≥2 = 2.0+ |
| `0x28` | 4 | DSDT | 32 位 DSDT 物理地址 |
| `0x38` | 4 | PM1a_EVT_BLK | （未用） |
| `0x3C` | 4 | PM1b_EVT_BLK | （未用） |
| **`0x40`** | 4 | **PM1a_CNT_BLK** | **S5 写入目标端口** |
| **`0x44`** | 4 | **PM1b_CNT_BLK** | 第二个 PM1 控制块（多数平台为 0） |
| `0x48` | 4 | PM2_CNT_BLK | （未用） |
| `0x4C` | 4 | PM_TMR_BLK | （未用） |
| `0x8C` | 8 | X_DSDT | ACPI 2.0+ 64 位 DSDT 地址 |
| `0xAC` | 12 | X_PM1a_CNT_BLK | GAS：`id(1)/width(1)/off(1)/access(1)` + `address(8)` @ `0xB0` |
| `0xB8` | 12 | X_PM1b_CNT_BLK | GAS：`address` @ `0xBC` |

**两次历史错误（本步修复）**：

- 第一版误用 `0x48` / `0x4C` 取 PM1a/PM1b —— 那实际是 `PM2_CNT_BLK` / `PM_TMR_BLK`。
  实测结果 `PM1a_CNT=0x0 PM1b_CNT=0x608`（0x608 是 PM 定时器端口），
  于是向 **定时器端口** 写 S5，自然毫无反应。
- 第二版误写成 `0x3B` / `0x3F`（比正确值错位 1 字节），
  实测读出 `PM1a_CNT=0x0 PM1b_CNT=0x00060400` —— 从这个"0x604 被整体左移 8 位"
  的特征值反推出正确偏移必须是 `0x40`，从而定位问题。

ACPI 2.0+ 回落逻辑（32 位字段为 0 时用 GAS）：

```c
if (rev >= 2 && flen >= 0xB8 + 12) {
    if (pm1a == 0 && f[0xAC] == 1)   /* GAS address_space_id==1 → SystemIO */
        pm1a = (uint32_t)(*(const uint64_t *)(f + 0xB0) & 0xFFFFFFFFULL);
    if (pm1b == 0 && f[0xB8] == 1)
        pm1b = (uint32_t)(*(const uint64_t *)(f + 0xBC) & 0xFFFFFFFFULL);
}
```

同理 DSDT 也做 `X_DSDT` 回落。所有扩展字段访问前都先用 `Length` 字段做边界检查，
避免读越过表尾（ACPI 1.0 的 FADT 只有 116 字节，实测 QEMU 正是 `len=116`）。

### 2.3 DSDT `_S5` 的最小化 AML 解析器

文件：`kernel/acpi/acpi.c` → `acpi_parse_s5()` / `acpi_aml_pkg_int()`

`_S5` 在 AML 中有两种常见编码形态：

```
形态 A：Name (_S5, Package (4) { a, b, 0, 0 })
        NameOp(0x08) + NameSeg "_S5_" + PackageOp(0x12) ...

形态 B：Method (_S5, 0) { Return (Package (4){ a, b, 0, 0 }) }
        MethodOp(0x14) + PkgLength + NameSeg "_S5_" + ... + PackageOp(0x12) ...
```

解析流程：

1. 校验 DSDT 校验和（`acpi_checksum_ok`），失败则回落 `SLP_TYP=5`。
2. 跳过 36 字节 SDT 头，在 AML 体内线性扫描 NameSeg 前 3 字节 `5F 53 35`（即 `_S5`）。
3. 看前导字节 `aml[i-1]`：
   - `0x08` → 形态 A，包体起点 = `i + 4`（跳过 4 字节 NameSeg）。
   - `0x14` → 形态 B，先按 PkgLength 引导字节算出方法体范围，在其中找首个 `0x12`。
   - 其它 → 只是恰好出现的字节序列，跳过继续扫描。
4. 解析 `PackageOp`：`12 <PkgLength> <NumElements> <elements...>`。
   PkgLength 编码：引导字节高 2 位为 0 时占 1 字节；否则低 4 位 `nn` 表示后续还有
   `nn` 字节，总长 `1 + nn`。
5. 用 `acpi_aml_pkg_int()` 取包内前两个元素（分别是 PM1a、PM1b 的 SLP_TYP），
   支持 `ByteConst(0x0A)`、`WordConst(0x0B)`、`Zero(0x00)`、`One(0x01)` 四种常量形态。
6. 结果各取低 3 位（SLP_TYP 是 3 位字段）存入 `g_acpi.slp_typ_a/b`。
7. 全表未找到 `_S5` → 回落 `SLP_TYP=5`（多数物理 x86 平台的通用值）。

**QEMU 实测数据**（调试期 dump 出的原始 AML 字节）：

```
5f 53 35 5f 12 06 04 00 00 00 00
 _  S  5  _  ^PackageOp
              ^PkgLength=0x06
                 ^NumElements=4
                    ^^^^^^^^^^^ 元素: Zero, Zero, Zero, Zero
```

即 `Name(_S5, Package(4){0,0,0,0})` → **`SLP_TYP = 0`**，而非硬编码猜测的 5。

### 2.4 `acpi_poweroff()` 实现

```c
void acpi_poweroff(void)
{
    uint16_t val_a = (uint16_t)(((uint16_t)g_acpi.slp_typ_a << 10) | (1u << 13));
    uint16_t val_b = (uint16_t)(((uint16_t)g_acpi.slp_typ_b << 10) | (1u << 13));
    if (g_acpi.pm1a_cnt_blk) outw((uint16_t)g_acpi.pm1a_cnt_blk, val_a);
    if (g_acpi.pm1b_cnt_blk) outw((uint16_t)g_acpi.pm1b_cnt_blk, val_b);
    for (volatile uint64_t spin = 0; spin < 200000000ULL; spin++)
        __asm__ volatile("pause");
    kprintf("[acpi] poweroff: platform did not power off, halting\n");
    for (;;) __asm__ volatile("cli; hlt");
}
```

三个必须遵守的约束（均为调试期实测踩出）：

1. **必须用 16 位 `outw`**。QEMU 的 PIIX4 PM 把 `pm1_cnt` 注册为
   `.valid.min_access_size = 2, .max_access_size = 2` 的 MemoryRegion，
   用 32 位 `outl` 写入不会命中该寄存器（实测无效）。
2. **写入后不能立即 `HLT`**。QEMU 的关机请求走主循环 Bottom-Half 处理，
   需要 vCPU 线程保持可运行；立即 `cli; hlt` 会让 VM 停在 halted 态而进程不退出
   （调试期该现象一度被误判为"S5 类型不对"）。因此写后先忙等 `pause` 自旋。
3. **不写 `SCI_EN` 位**。纯 `SLP_TYP | SLP_EN` 即可；调试期附加 `SCI_EN(bit0)` 也能工作，
   但非必要，正式实现保持最小写入。

平台无 PM1 控制块（`pm1a_cnt_blk == 0`）时退化为永久停机，不破坏内核现场。

对应 QEMU 侧逻辑（`hw/acpi/core.c: acpi_pm1_cnt_write`）：检测到 `SLP_EN` 后取
`(val >> 10) & 7`，为软关机类型则调用 `qemu_system_shutdown_request()`。

### 2.5 数据结构变更

文件：`include/kernel/acpi.h`

```c
typedef struct {
    ...
    uint32_t pm1a_cnt_blk;   /* PM1a Control Block I/O 端口（FADT @0x40） */
    uint32_t pm1b_cnt_blk;   /* PM1b Control Block I/O 端口（FADT @0x44） */
    uint8_t  slp_typ_a;      /* S5 在 PM1a 的 SLP_TYP（来自 DSDT _S5 低 3 位） */
    uint8_t  slp_typ_b;      /* S5 在 PM1b 的 SLP_TYP */
    uint64_t dsdt_phys;      /* DSDT 物理地址（FADT @0x28 或 X_DSDT @0x8C） */
} acpi_info_t;

void acpi_poweroff(void);
```

`acpi_parse_fadt()` 在 `acpi_process_table()` 命中签名 `FACP` 时调用
（需在调用点前加 `static void acpi_parse_fadt(uint64_t);` 前向声明，
否则会因"static declaration follows non-static declaration"编译失败）。

### 2.6 syscall 与 Shell 接线

`kernel/syscall/syscall.c`：

```c
static uint64_t sys_reboot(uint64_t mode)
{
    if (mode != 0) {
        acpi_poweroff();      /* mode=1：ACPI S5 断电 */
    }
    /* mode=0：8042 键盘控制器 0xFE 脉冲复位 CPU */
    ...
}
/* dispatch: case SYS_REBOOT: return sys_reboot(a1); */
```

`user/lib/suki.h`：新增 `suki_poweroff()` / `suki_reboot()` 包装。
`user/shell.c`：新增 `poweroff` / `shutdown` 命令并更新 `help` 文本。

调用链：

```
shell(Ring3) "poweroff"
  → suki_poweroff() → syscall SYS_REBOOT(rax=6), rdi=1
  → syscall_dispatch → sys_reboot(1)
  → acpi_poweroff()
  → outw(0x604, 0x2000)
  → QEMU acpi_pm1_cnt_write → qemu_system_shutdown_request() → exit(0)
```

---

## 三、R7：`suki_syscall5` 寄存器 clobber 缺失（真实 bug）

### 3.1 现象

串口日志中 INPUT_SERVER 持续刷屏：

```
unknown syscall 18446673704975827160 from pid=5
```

调用号是个巨大的垃圾值（明显是被当作 syscall 号的野指针/野数据）。

### 3.2 定位过程

反汇编 `build/user/input_server.elf`，在 `0x4000a8` 处看到：

```asm
mov %r9, %rax        ; 编译器把 syscall 号 n 缓存在 r9，每次调用前搬进 rax
```

而 `user/lib/suki.h` 中 `suki_syscall5` 的内联汇编 clobber 列表只有
`"rcx", "r11", "memory"`。但该函数用 `register uint64_t ... __asm__("r9")`
风格传参时，`sys_port_claim` 路径下的 `"r"` 约束会分配/破坏 `r9`。
编译器并不知道 `r9` 被破坏，于是在循环的后续迭代中继续复用那个已被污染的
`r9` 作为 syscall 号 → 产生天文数字调用号。

### 3.3 修复

```c
static inline uint64_t suki_syscall5(uint64_t n, uint64_t a1, uint64_t a2,
                                     uint64_t a3, uint64_t a4, uint64_t a5)
{
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = a4;
    register uint64_t r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "rcx", "r11", "r9", "rbx", "memory");
    return ret;
}
```

- 补入 `"r9"`、`"rbx"` 到 clobber list。
- `a4`/`a5` 必须用 `register ... __asm__("rXX")` 变量绑定，
  **不能**写成 `"r10"(a4)` 形式的约束 —— 那会报
  `matching constraint references invalid operand number`。

修复后反汇编确认编译器改为把 `n` 缓存在 `rbp`（callee-saved，跨调用保留），
运行期 `unknown syscall` 计数归零。

---

## 四、验证

### 4.1 构建

```bash
cd /mnt/d/Projects/SukiOS
make iso
```

### 4.2 S5 关机端到端验证

自动化脚本 `/tmp/probe_poweroff.py`：启动 QEMU（monitor 走 unix socket、
串口输出到文件），等待 `SukiOS>` 提示符后用 monitor `sendkey` 逐字符注入
`poweroff` + `ret`，然后**轮询 QEMU 进程是否自行退出**。

> **判定方式的关键修正**：早期脚本 grep QEMU 输出里的
> `"terminating on guest shutdown"` 来判断成功——**这是错的**。
> QEMU 因 guest 触发 S5 关机时是静默 `exit(0)`，不打印任何内容；
> 那句话只在 QEMU 收到宿主信号（如 SIGTERM）时才输出。
> 这个错误判据一度让所有正确尝试都被误判为失败，是本次调试耗时最久的坑。
> 正确判据：`Popen.poll() is not None`（进程已退出）。

手动等价命令：

```bash
qemu-system-x86_64 -m 512 -smp 4 -machine pc -cpu qemu64 \
  -drive file=build/SukiOS.iso,format=raw,if=ide,media=cdrom -boot d \
  -serial file:/tmp/sukios.log -display none -net none
# 在图形/串口界面输入：poweroff
```

**实测输出**：

```
[acpi] S5 from DSDT _S5: PM1a SLP_TYP=0 PM1b SLP_TYP=0
[acpi] FADT rev=1 PM1a_CNT=0x604 PM1b_CNT=0x0 DSDT=0x1ffe0040 S5 A=0 B=0
SukiOS> poweroff
powering off (ACPI S5)...
[acpi] poweroff: S5 -> PM1a=0x604 val=0x2000 PM1b=0x0 val=0x2000
→ SHUTDOWN_OK rc=0        （QEMU 进程自行退出）
```

交叉印证：调试期扫描 PCI bus0 得到
`00:01 vend=8086 dev=7113 cls=06 sub=80 pm0x40=00000601`
→ PIIX4 PM 设备 PMBA = `0x600`（bit0 为 IO 使能位），
故 PM1a_CNT = `0x600 + 4` = **`0x604`**，与 FADT 解析结果完全一致。

### 4.3 reboot 回归验证

```bash
python3 /tmp/probe_reboot.py     # 注入 "reboot"
# 结果：串口日志中 "shell online" 出现 2 次、"rebooting" 1 次，QEMU 保持运行
```

说明 mode=0 的 8042 `0xFE` 复位路径未受影响。

### 4.4 整机回归

16 秒无干预运行，`grep -ci "panic|check_exception|unknown syscall"` → **0**：

```
[security] KASLR: ENABLED slide=9216 MiB
[acpi] MADT: 4 enabled LAPIC(s), IOAPIC @ 0xfec00000
[smp] 4/4 CPUs online (BSP lapic_id=0)
[input] INPUT_SERVER online (Ring3 scancode parser)
[shell] SukiOS shell online (Ring3)
[sched] load balance: 4 CPUs
  cpu0: rq=1 user_switches=0
  cpu1: rq=1 user_switches=0
  cpu2: rq=2 user_switches=1
```

KASLR、SMP 对称调度 + work-stealing、Ring3 服务全部正常，零异常。

---

## 五、涉及文件清单

| 文件 | 变更 |
| --- | --- |
| `include/kernel/acpi.h` | `acpi_info_t` 新增 `pm1a_cnt_blk`/`pm1b_cnt_blk`/`slp_typ_a`/`slp_typ_b`/`dsdt_phys`；声明 `acpi_poweroff()` |
| `kernel/acpi/acpi.c` | 新增 `acpi_aml_pkg_int()`、`acpi_parse_s5()`、`acpi_parse_fadt()`、`acpi_poweroff()`；`acpi_process_table()` 命中 `FACP` 时调用解析 |
| `kernel/syscall/syscall.c` | `#include <kernel/acpi.h>`；`sys_reboot(mode)` 支持 mode=1 走 ACPI S5 |
| `user/lib/suki.h` | 新增 `suki_poweroff()`/`suki_reboot()`；**修复 `suki_syscall5` clobber 补 `r9`/`rbx`** |
| `user/shell.c` | 新增 `poweroff`/`shutdown` 命令，更新 `help` |

---

## 六、P0 审计残留项进度

| 项 | 内容 | 状态 |
| --- | --- | --- |
| R1 | 对称多核调度（per-CPU rq + work-stealing） | ✅ step25 |
| R2 | 代码段 KASLR | ✅ 早已落地（boot.S 重定位） |
| R3 | 栈回溯 / 异常诊断 | ✅ step24 |
| R4 | AHCI 错误恢复 | ⬜ 待做 |
| R5 | 回溯上界回绕修复 | ✅ step24 |
| R6 | MSI / MSI-X 中断 | ⬜ 待做 |
| R7 | Ring3 服务健壮性 | 🔶 本步修复 syscall clobber bug；H7/D1/M13 待做 |
| R8 | 电源管理 ACPI S5 关机 | ✅ 本步完成（多 IOAPIC / PCI `_PRT` 路由另列） |
