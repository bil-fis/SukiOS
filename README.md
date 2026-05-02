# SukiOS

一个从零编写的 x86_64 单内核（Monolithic Kernel）操作系统，采用 Higher-Half 内核模型，通过 Multiboot2 协议由 GRUB2 引导启动。

## 目录

- [项目概览](#项目概览)
- [构建与运行](#构建与运行)
- [项目结构](#项目结构)
- [启动流程](#启动流程)
- [内存布局](#内存布局)
- [模块详解](#模块详解)
  - [引导程序 (boot.asm)](#引导程序-bootasm)
  - [链接器脚本 (linker.ld)](#链接器脚本-linkerld)
  - [全局描述符表 (GDT)](#全局描述符表-gdt)
  - [中断描述符表 (IDT)](#中断描述符表-idt)
  - [物理内存管理器 (PMM)](#物理内存管理器-pmm)
  - [虚拟内存管理器 (VMM)](#虚拟内存管理器-vmm)
  - [内核堆 (Heap)](#内核堆-heap)
  - [8259 PIC](#8259-pic)
  - [ACPI 解析](#acpi-解析)
  - [Local APIC](#local-apic)
  - [I/O APIC](#io-apic)
  - [LAPIC 定时器](#lapic-定时器)
  - [PS/2 键盘驱动](#ps2-键盘驱动)
  - [VGA 终端](#vga-终端)
  - [UART 串口](#uart-串口)
- [中断向量分配](#中断向量分配)
- [已支持硬件](#已支持硬件)
- [参考资料](#参考资料)

---

## 项目概览

| 属性 | 说明 |
|---|---|
| **架构** | x86_64 (64 位长模式) |
| **内核模型** | 单内核 (Monolithic Kernel)，Higher-Half |
| **引导协议** | Multiboot2 |
| **引导加载器** | GRUB2 |
| **C 标准** | GNU C11 (`-std=gnu11`) |
| **内存模型** | `-mcmodel=large`（绝对 64 位寻址） |
| **内核虚拟基址** | `0xFFFFFFFF80000000` |
| **物理加载地址** | `0x110000`（1MB + 64KB Bootstrap） |
| **代码语言** | C + NASM 汇编 |
| **目标平台** | QEMU (`qemu-system-x86_64`) |
| **开发环境** | WSL (Windows Subsystem for Linux) |

---

## 构建与运行

### 前置依赖

在 WSL (Ubuntu) 中安装：

```bash
sudo apt install nasm qemu-system-x86 grub-pc-bin grub-common xorriso gcc
```

### 构建目标

```bash
make              # 编译生成 build/kernel.elf
make verify       # 验证 Multiboot2 头部有效性
make iso          # 生成可启动 ISO (build/sukios.iso)
make run          # 生成 ISO 并在 QEMU 中运行 (128MB 内存)
make run-direct   # 通过 QEMU -kernel 直接加载运行
make debug        # ISO + GDB 远程调试 (-s -S, 端口 1234)
make clean        # 清理 build/ 目录
```

### 编译标志

| 标志 | 说明 |
|---|---|
| `-ffreestanding` | 无标准库，内核自包含 |
| `-mno-red-zone` | 禁用红区（中断安全） |
| `-mno-mmx -mno-sse -mno-sse2` | 禁用 SIMD（中断时不保存 SIMD 寄存器） |
| `-mcmodel=large` | 高半区内核需要绝对 64 位寻址 |
| `-fno-pic -fno-pie` | 无位置无关代码 |
| `-O2` | 优化等级 2 |
| `-z max-page-size=0x1000` | 页对齐 4KB（Multiboot2 要求） |

---

## 项目结构

```
SukiOS/
├── Makefile                    # 构建脚本
├── .gitignore                  # 忽略 build/ 和 *.log
├── boot/
│   ├── boot.asm                # Multiboot2 引导代码 (32 位 → 64 位)
│   ├── linker.ld               # 链接器脚本 (Higher-Half 内核)
│   └── grub.cfg                # GRUB2 配置
├── include/
│   ├── multiboot2.h            # Multiboot2 规范结构定义
│   └── kernel/
│       ├── kernel.h            # init_kernel() 声明
│       ├── io.h                # 端口 I/O (outb/inb) + MSR (rdmsr/wrmsr)
│       ├── tty.h               # VGA 文本模式终端接口
│       ├── pmm.h               # 物理内存管理器 (位图法)
│       ├── gdt.h               # GDT + TSS 结构定义
│       ├── idt.h               # IDT + 中断帧 + IRQ 注册
│       ├── pic.h               # 8259 PIC 接口
│       ├── uart.h              # UART COM1 串口接口
│       ├── acpi.h              # ACPI 表结构 (RSDP/MADT/XSDT/RSDT)
│       ├── apic.h              # Local APIC + I/O APIC 接口
│       ├── apic_timer.h        # LAPIC 定时器接口
│       ├── heap.h              # 内核堆 (kmalloc/kfree/krealloc/kcalloc)
│       ├── keyboard.h          # PS/2 键盘 (键码定义、事件结构、驱动接口)
│       └── vmm.h               # 虚拟内存管理器 (4 级分页)
└── kernel/
    ├── EarlyKernel/            # 启动阶段（无动态内存，直接操作物理内存）
    │   ├── main.c              # kernel_main 入口，解析 Multiboot2 信息
    │   ├── gdt.c               # 64 位 GDT + TSS + IST 初始化
    │   ├── idt.c               # IDT 初始化 + CPU 异常处理 + IRQ 分发
    │   ├── idt_stubs.asm       # ISR/IRQ 汇编存根 + 专用异常栈
    │   ├── pic.c               # 8259 PIC 重映射 + IMCR APIC 模式切换
    │   ├── pmm.c               # 物理内存位图管理器
    │   ├── tty.c               # VGA 80x25 文本模式终端
    │   └── uart.c              # UART COM1 串口驱动 (115200 8N1)
    ├── InitKernel/             # 高级初始化阶段（有 VMM 和堆）
    │   ├── main.c              # init_kernel 入口，APIC/定时器/键盘初始化
    │   ├── vmm.c               # 虚拟内存管理器 (4 级页表按需映射)
    │   ├── heap.c              # 内核堆 (隐式空闲链表, first-fit)
    │   ├── acpi.c              # ACPI 表解析 (RSDP/XSDT/MADT)
    │   └── apic.c              # APIC 子系统初始化 (LAPIC + IOAPIC)
    └── BasicDrivers/           # 硬件驱动
        ├── apic_timer.c        # LAPIC 定时器 (PIT 校准, 100Hz)
        └── keyboard.c          # PS/2 键盘驱动 (8042, Scancode Set 1)
```

---

## 启动流程

SukiOS 采用两阶段启动架构：

```
GRUB2 (Multiboot2)
  │ 32 位保护模式, EAX=magic, EBX=MBI 地址
  ▼
boot.asm _start
  │ 保存 Multiboot2 参数, 设置临时栈 (esp=0x78000)
  │ 初始化 7 张页表 (0x70000-0x76FFF)
  │   ├─ 身份映射: PML4[0] → 0-4MB (4KB 页)
  │   └─ 高半区映射: PML4[511] → KERNEL_VIRT → KERNEL_LMA (2MB)
  │ 启用 PAE → 加载 CR3 → 启用长模式 → 启用分页
  │ Far Jump 到 64 位代码 → lgdt → 切换内核栈
  │ 间接调用 kernel_main(RDI=magic, RSI=MBI)
  ▼
EarlyKernel kernel_main()
  ├─ tty_init()        VGA 终端 + UART 串口
  ├─ 解析 Multiboot2    命令行、引导加载器、内存信息、帧缓冲区
  ├─ gdt_init()        正式 GDT (含用户段) + TSS + IST
  ├─ pic_init()        PIC 重映射 + IMCR 切换到 APIC 模式
  ├─ idt_init()        CPU 异常门 (0-31) + IRQ 门 (32-47) + 伪中断 (255)
  ├─ pmm_init()        从 Multiboot2 MMAP 初始化物理内存位图
  └─ init_kernel() ─────────────────────────────────────┐
  │ (若 init_kernel 返回, 进入 sti;hlt 空闲循环)         │
  ▼                                                      │
InitKernel init_kernel()  ◄──────────────────────────────┘
  ├─ 打印 ASCII Art Logo + 版本信息
  ├─ vmm_init()        扩展身份映射到 0-4GB (4×1GB 大页)
  ├─ heap_init()       内核堆 (0xFFFFFFFFA0000000, 512MB 预留)
  ├─ acpi_init()       搜索 RSDP → 解析 MADT (LAPIC/IOAPIC 地址)
  ├─ apic_init()       LAPIC 启用 + IOAPIC 初始化
  ├─ apic_timer_init() PIT 校准 → 100Hz 周期定时器
  ├─ keyboard_init()   8042 自检 → 键盘初始化 → IOAPIC IRQ1 重定向
  ├─ 堆分配测试        kmalloc/kfree 验证
  └─ 主循环            keyboard_getchar_nb() + sti; hlt
```

### 两阶段设计理念

- **EarlyKernel**：在 VMM 和堆可用之前运行，直接操作物理内存，负责最基本的硬件初始化
- **InitKernel**：在 VMM 和堆就绪后运行，初始化高级功能（APIC、定时器、键盘驱动等）

---

## 内存布局

### 虚拟地址空间

| 区域 | 虚拟地址范围 | 大小 | 说明 |
|---|---|---|---|
| 身份映射 | `0x0000000000000000` - `0x00000000FFFFFFFF` | 4 GB | 访问物理内存、MMIO、ACPI 表（4×1GB 大页） |
| 内核代码/数据 | `0xFFFFFFFF80000000`+ | 2 MB | `.text` / `.rodata` / `.data` / `.bss`（boot.asm 映射，4KB 页） |
| 内核堆 | `0xFFFFFFFFA0000000`+ | 512 MB 预留 | kmalloc 区域（PML4[511] 按需 4KB 映射） |
| MMIO 映射 | `0xFFFFFFFFC0000000`+ | 512 MB 预留 | LAPIC、IOAPIC 等 MMIO（PML4[511] 按需 4KB 映射） |

### boot.asm 页表布局（7 张表，28KB）

| 表 | 物理地址 | 用途 |
|---|---|---|
| PML4 | `0x70000` | 顶层页表 |
| PDPTE_A | `0x71000` | 身份映射链 |
| PD_A | `0x72000` | 身份映射链 |
| PT_A | `0x73000` | 身份映射 0-4MB（1024×4KB） |
| PDPTE_B | `0x74000` | 高半区映射链 |
| PD_B | `0x75000` | 高半区映射链 |
| PT_B | `0x76000` | KERNEL_VIRT → KERNEL_LMA（2MB，512×4KB） |

### 链接器段布局

| 段 | VMA (虚拟地址) | LMA (物理地址) | 说明 |
|---|---|---|---|
| `.multiboot` | `0x100000` | `0x100000` | Multiboot2 头部（必须在前 32KB） |
| `.bootstrap` | `0x100000`+ | `0x100000`+ | Bootstrap 代码/数据（最大 64KB） |
| `.text` | `0xFFFFFFFF80000000`+ | `0x110000`+ | 内核代码 |
| `.rodata` | 高半区递增 | LMA 递增 | 只读数据 |
| `.data` | 高半区递增 | LMA 递增 | 已初始化数据 |
| `.bss` | 高半区递增 | LMA 递增 | 未初始化数据（含内核栈 16KB） |

---

## 模块详解

### 引导程序 (boot.asm)

**文件**: `boot/boot.asm` (253 行)

负责从 GRUB 接收控制权（32 位保护模式），完成 32 位到 64 位长模式的切换，并建立初始页表使内核能在高半区运行。

关键步骤：
1. 保存 Multiboot2 magic 和 MBI 地址
2. 清零 7 张页表（28KB，位于 `0x70000-0x76FFF`）
3. 建立**身份映射**：`PML4[0] → PDPTE_A → PD_A → PT_A`，映射前 4MB
4. 建立**高半区映射**：`PML4[511] → PDPTE_B[510] → PD_B[0] → PT_B`，映射 `KERNEL_VIRT` → `KERNEL_LMA`
5. 启用 PAE (CR4.PAE) → 加载 CR3 → 启用长模式 (EFER.LME) → 启用分页 (CR0.PG)
6. 加载 Bootstrap GDT，Far Jump 到 64 位代码
7. 切换到内核栈，调用 `kernel_main(RDI=magic, RSI=MBI)`

Bootstrap GDT 包含 3 个描述符：NULL、64 位代码段、32 位数据段。

### 链接器脚本 (linker.ld)

**文件**: `boot/linker.ld` (92 行)

将内核分为 Bootstrap 段（VMA=LMA，物理地址运行）和内核段（VMA=高半区，LMA=物理地址）。

关键导出符号：
- `__kernel_virt_end` — 内核虚拟地址结束（VMM 初始化使用）
- `__kernel_phys_end` — 内核物理地址结束（PMM 初始化使用）

### 全局描述符表 (GDT)

**文件**: `kernel/EarlyKernel/gdt.c`, `include/kernel/gdt.h`

64 位 GDT 包含 7 个条目：

| 选择子 | 说明 |
|---|---|
| `0x00` | 空描述符 |
| `0x08` | 内核 64 位代码段 (L=1, DPL=0) |
| `0x10` | 内核数据段 (G=1, DPL=0) |
| `0x18` | 用户 64 位代码段 (L=1, DPL=3) |
| `0x20` | 用户数据段 (G=1, DPL=3) |
| `0x28-0x30` | TSS 系统段 (16 字节描述符) |

TSS 配置：
- **RSP0**：Ring 3→0 中断切换栈（16KB）
- **IST1**：Double Fault 专用栈（16KB）
- **IST2**：NMI 专用栈（16KB）
- **IST3**：Machine Check 专用栈（16KB）
- **I/O 权限位图偏移** = TSS 大小（禁止 Ring 3 I/O）

使用 `lretq` 技巧重新加载 CS（Long Mode 不支持 Far Jump）。

### 中断描述符表 (IDT)

**文件**: `kernel/EarlyKernel/idt.c`, `kernel/EarlyKernel/idt_stubs.asm`, `include/kernel/idt.h`

256 个 IDT 条目，每个 16 字节（含 64 位偏移、IST 选择、类型属性）。

**汇编存根** (`idt_stubs.asm`)：
- **ISR 宏**：为 CPU 异常（向量 0-31）生成压栈/跳转代码，区分有/无错误码
- **IRQ 宏**：为 IRQ（向量 32-47）生成存根
- **`isr_common_stub`**：保存全部 15 个通用寄存器，调用 `exception_handler`，`iretq` 恢复
- **`irq_common_stub`**：类似，调用 `irq_handler`（含 LAPIC EOI）
- **专用栈**（BSS 段）：Double Fault / NMI / Machine Check 各 16KB

**异常处理**：打印异常名称、向量号、错误码、RIP、CS、RFLAGS、RSP，然后 `hlt` 停机。

**IRQ 分发**：`irq_handler()` 根据 `int_no - 0x20` 计算 IRQ 编号，调用注册的处理函数，发送 LAPIC EOI。

**IRQ 注册机制**：`irq_register_handler()` 维护一个 `irq_handler_fn[16]` 回调表，驱动通过该接口注册中断处理函数。

### 物理内存管理器 (PMM)

**文件**: `kernel/EarlyKernel/pmm.c`, `include/kernel/pmm.h`

基于位图的物理页分配器：
- 位图数组 `pmm_bitmap[128KB]`，支持最大 4GB 物理内存
- bit=0 空闲，bit=1 已使用
- 从 Multiboot2 MMAP 标签初始化：先全部标记已使用，再标记可用区域，最后标记内核占用页
- **First-fit** 分配策略，线性扫描
- 使用 `__kernel_phys_end`（链接器导出）确定内核结束物理地址

### 虚拟内存管理器 (VMM)

**文件**: `kernel/InitKernel/vmm.c`, `include/kernel/vmm.h`

4 级分页虚拟内存管理器，提供按需页表分配和映射：

- `vmm_init()`：从 boot.asm 的临时页表迁移，将身份映射扩展到 0-4GB（4 个 1GB 大页），保留内核高半区映射
- `vmm_map_page()`：4KB 页映射，按需分配 PML4/PDPTE/PD/PT 各级页表
- `vmm_alloc_map()`：分配物理页 + 清零 + 映射（用于堆扩展等）
- `vmm_map_mmio()`：将物理 MMIO 映射到专用虚拟区域 `0xFFFFFFFFC0000000+`（Uncacheable）
- `vmm_unmap_page()` / `vmm_get_phys()` / `vmm_change_flags()` / `vmm_is_mapped()`
- 页表遍历通过身份映射访问物理地址

权限组合：
| 宏 | 说明 |
|---|---|
| `VMM_KERN_RW` | 内核读写 (Present + RW) |
| `VMM_KERN_RO` | 内核只读 (Present) |
| `VMM_KERN_RW_CD` | MMIO Uncacheable (Present + RW + PCD + PWT) |
| `VMM_USER_RW` | 用户读写 (Present + RW + User) |
| `VMM_USER_RO` | 用户只读 (Present + User) |

### 内核堆 (Heap)

**文件**: `kernel/InitKernel/heap.c`, `include/kernel/heap.h`

基于隐式空闲链表的内核堆分配器（First-fit 策略）：

- **块头部**：16 字节（`size: uint64_t` + `next: uint64_t`）
- `size` 最高位（bit 63）标记分配状态：0=空闲，1=已分配
- 最小块：32 字节（16 头部 + 16 payload），16 字节对齐
- `kmalloc()`：First-fit 搜索，必要时通过 `vmm_alloc_map()` 扩展堆
- `kfree()`：标记空闲，合并相邻空闲块
- `krealloc()`：分配新块 + 复制 + 释放旧块
- `kcalloc()`：kmalloc + 清零
- 初始仅映射 4KB，随分配增长（最大预留 512MB）
- `heap_get_stats()`：返回 total/used/free 统计

### 8259 PIC

**文件**: `kernel/EarlyKernel/pic.c`, `include/kernel/pic.h`

PIC 重映射和禁用：
- ICW1-ICW4 标准初始化序列（级联模式，8086 模式）
- IRQ 重映射：主 PIC → 向量 0x20-0x27，从 PIC → 向量 0x28-0x2F
- 初始状态屏蔽所有 IRQ
- **IMCR 切换**：通过端口 0x22/0x23 将中断模式从 PIC 路由切换到 APIC（在有 PIC 和 IOAPIC 共存的系统中必需）

### ACPI 解析

**文件**: `kernel/InitKernel/acpi.c`, `include/kernel/acpi.h`

ACPI 表发现和解析：

- `acpi_find_rsdp()`：在 EBDA（`0x40E` 处）和 BIOS ROM 区域（`0xE0000-0xFFFFF`）搜索 RSDP 签名 `"RSD PTR "`
- `verify_checksum()`：校验和验证
- `acpi_find_table()`：优先使用 XSDT（64 位地址），回退 RSDT（32 位地址）
- `acpi_parse_madt()`：解析 MADT 表中的 I/O APIC（类型 1）和 Local APIC 地址覆盖（类型 5）
- `acpi_parse_isa_overrides()`：提取 ISA 中断源覆盖（类型 2），用于 IOAPIC 重定向配置
- `acpi_get_isa_override()`：查找指定 ISA IRQ 的 GSI 和 flags

支持的 MADT 条目类型：
| 类型 | 说明 |
|---|---|
| 1 | I/O APIC |
| 2 | ISA 中断源覆盖 (Interrupt Source Override) |
| 5 | Local APIC 地址覆盖 (64 位地址) |

### Local APIC

**文件**: `kernel/InitKernel/apic.c`, `include/kernel/apic.h`

Local APIC 初始化和管理：

- CPUID 检测 APIC 支持（EDX bit 9）
- 读取/设置 IA32_APIC_BASE MSR（启用全局 APIC）
- 通过 `vmm_map_mmio()` 永久映射 LAPIC MMIO 到 `0xFFFFFFFFC0000000`
- 设置 Spurious Interrupt Vector Register (SVR)：启用 APIC 软件 + 伪中断向量 0xFF
- `lapic_write()` / `lapic_read()`：直接 MMIO 读写（MMIO 基址寄存器对齐）

关键寄存器偏移：
| 寄存器 | 偏移 | 说明 |
|---|---|---|
| EOI | `0x0B0` | 中断结束确认 |
| SVR | `0x0F0` | Spurious Interrupt Vector |
| ICR | `0x300-0x310` | 中断命令寄存器 |
| LVT Timer | `0x320` | 本地定时器 |
| Init Count | `0x380` | 定时器初始计数值 |
| Cur Count | `0x390` | 定时器当前计数值 |
| Div Config | `0x3E0` | 定时器分频配置 |

### I/O APIC

**文件**: `kernel/InitKernel/apic.c`, `include/kernel/apic.h`

I/O APIC 初始化和中断重定向：

- 通过 `vmm_map_mmio()` 永久映射 IOAPIC MMIO 到 `0xFFFFFFFFC0001000`
- 读取 IOAPIC ID、版本号、最大重定向条目数
- 初始化时屏蔽所有重定向条目
- `ioapic_set_redirection()`：配置重定向条目（向量号、极性、触发模式、掩码），根据 MADT ISA Override flags 设置 Active Low / Level Triggered
- `ioapic_write()` / `ioapic_read()`：间接寄存器访问（REGSEL=0x00 + REGWIN=0x10）

重定向条目格式（64 位）：
- 低 32 位：向量 (bits 0-7), Delivery Mode (bits 8-10), Mask (bit 16), Trigger Mode (bit 15), Polarity (bit 13)
- 高 32 位：目标 APIC ID (bits 24-31)

### LAPIC 定时器

**文件**: `kernel/BasicDrivers/apic_timer.c`, `include/kernel/apic_timer.h`

基于处理器总线时钟的 LAPIC 定时器驱动：

**PIT 校准**（计算总线频率）：
1. 配置 PIT 通道 0 为模式 2（速率发生器），基准频率 1193182 Hz
2. LAPIC Timer 设置 Divide=16，使用最大初始计数值
3. 等待 PIT 减少约 11932 计数（≈10ms）
4. 读取 LAPIC Timer 减少的计数，计算总线频率：`ticks_elapsed * 100 * 16`

**运行配置**：
- 分频：16
- 模式：周期 (Periodic)
- 频率：100 Hz（10ms 间隔）
- `apic_timer_irq_handler()`：递增 tick 计数，调用用户回调
- `apic_timer_set_callback()`：注册回调（用于键盘按键重复）

### PS/2 键盘驱动

**文件**: `kernel/BasicDrivers/keyboard.c`, `include/kernel/keyboard.h`

完整的 PS/2 键盘驱动，支持 8042 控制器初始化、Scancode Set 1 解码、US QWERTY ASCII 映射和按键重复。

**8042 控制器初始化**（参考 OSDev 8042 "Initialising the PS/2 Controller"）：
1. 禁用端口 1 和端口 2
2. 刷新输入/输出缓冲区
3. 读取配置字节 → 修改（禁用端口 2 中断，使能端口 1 中断，启用翻译模式）
4. 写回配置字节
5. 控制器自检（0xAA → 期望 0x55）
6. 端口 1 自检（0xAB → 期望 0x00）
7. 使能端口 1

**键盘设备初始化**：
- 复位/自检 (0xFF)
- 设置 Typematic Rate (0xF3, 500ms 延迟, ~30Hz 重复率)
- 使能扫描 (0xF4)
- LED 更新 (0xED)
- 超时机制：100ms PS/2 超时，3 次重试

**Scancode Set 1 解码状态机**（6 种状态）：
| 状态 | 说明 |
|---|---|
| `STATE_NORMAL` | 普通键按下/释放 |
| `STATE_EXPECT_E0_DATA` | E0 扩展键数据 |
| `STATE_EXPECT_E0_RELEASE` | E0 序列中的释放码 |
| `STATE_EXPECT_E1_DATA_1` | Pause 键序列（第一个数据字节） |
| `STATE_EXPECT_E1_DATA_2` | Pause 键序列（第二个数据字节） |
| `STATE_EXPECT_E1_RELEASE_1` | Pause 键序列（第一个释放字节） |
| `STATE_EXPECT_E1_RELEASE_2` | Pause 键序列（第二个释放字节） |

**键码映射**：
- `set1_make_to_keycode[128]`：主键盘区映射
- `e0_make_to_keycode[128]`：E0 扩展键映射
- `keycode_to_ascii[128]` / `keycode_to_ascii_shift[128]`：US QWERTY ASCII 映射
- 字母键：CapsLock XOR Shift 决定大小写

**按键重复**：由 LAPIC 定时器中断驱动，500ms 首次延迟，~30Hz 重复率。

**环形缓冲区**：256 字节，head/tail 指针，溢出丢弃。

**LED 控制**：缓存机制，仅在 CapsLock/NumLock/ScrollLock 状态变化时发送 0xED 命令。

**IOAPIC 配置**：ISA IRQ1 → 向量 0x21，通过 `acpi_get_isa_override()` 支持 ACPI 中断源覆盖。

### VGA 终端

**文件**: `kernel/EarlyKernel/tty.c`, `include/kernel/tty.h`

VGA 80×25 文本模式终端驱动：

- 基地址 `0xB8000`，每字符 2 字节（字符 + 属性）
- 硬件光标支持（通过 0x3D4/0x3D5 端口）
- 屏幕上滚 (scroll)
- 16 色 VGA 颜色支持
- 双输出：每个字符同时输出到 VGA 和 COM1 串口
- `tty_print_hex64()`：64 位十六进制打印
- `tty_print_dec()`：十进制打印

### UART 串口

**文件**: `kernel/EarlyKernel/uart.c`, `include/kernel/uart.h`

UART COM1 串口驱动（调试输出）：

- 基地址 `0x3F8`
- 配置：115200 波特率，8N1（8 数据位，无校验，1 停止位）
- 波特率除数 = 1（1843200 / (16 × 1) = 115200）
- `uart_putchar()`：阻塞等待 LSR.TX_READY 后发送

---

## 中断向量分配

| 向量范围 | 数量 | 用途 |
|---|---|---|
| `0x00-0x1F` | 32 | CPU 异常（含 IST 专用栈：#DF→IST1, NMI→IST2, #MC→IST3） |
| `0x20` | 1 | IRQ0: LAPIC 定时器 (100Hz) |
| `0x21` | 1 | IRQ1: PS/2 键盘 |
| `0x22-0x2F` | 14 | IRQ2-15: 预留（串口、并口、RTC、ATA 等） |
| `0xFF` | 1 | APIC 伪中断 (Spurious Interrupt) |

---

## 已支持硬件

| 硬件 | 驱动位置 | 接口/状态 |
|---|---|---|
| VGA 文本模式 | `kernel/EarlyKernel/tty.c` | 80×25，硬件光标，滚动 |
| UART COM1 串口 | `kernel/EarlyKernel/uart.c` | 115200 8N1，调试输出 |
| 8259 PIC | `kernel/EarlyKernel/pic.c` | 重映射 + IMCR APIC 模式切换 |
| Local APIC | `kernel/InitKernel/apic.c` | MMIO 映射，SVR，定时器 LVT |
| I/O APIC | `kernel/InitKernel/apic.c` | MMIO 映射，重定向，ISA Override |
| LAPIC 定时器 | `kernel/BasicDrivers/apic_timer.c` | PIT 校准，100Hz 周期中断 |
| PS/2 键盘 (8042) | `kernel/BasicDrivers/keyboard.c` | Set 1 完整解码，US QWERTY，按键重复 |
| ACPI (RSDP/MADT) | `kernel/InitKernel/acpi.c` | XSDT/RSDT 表查找，MADT 解析 |

---

## 参考资料

- [OSDev Wiki](https://wiki.osdev.org/) — 操作系统开发知识库
- [Intel SDM (Software Developer's Manual)](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html) — x86/x86_64 架构手册
- [Multiboot2 Specification](https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html) — GNU GRUB 引导协议
- [ACPI Specification](https://uefi.org/specifications) — 高级配置与电源接口
