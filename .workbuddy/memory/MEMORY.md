# SukiOS 项目长期记忆

## 项目概况

- **项目名**: SukiOS
- **类型**: 从零手写的 x86_64 单内核操作系统（Monolithic Kernel, Higher-Half）
- **引导协议**: Multiboot2，由 GRUB2 引导
- **代码量**: ~4300 行（C + NASM 汇编）
- **构建环境**: WSL (Ubuntu)，GCC + NASM，目标平台 QEMU

## 关键技术参数

- 内核虚拟基址: `0xFFFFFFFF80000000`
- 物理加载地址: `0x110000`（1MB + 64KB Bootstrap）
- 编译选项: `-mcmodel=large -ffreestanding -mno-red-zone -mno-sse/mmx -fno-pic -O2`
- C 标准: `gnu11`

## 目录结构

```
boot/        — boot.asm (引导汇编), linker.ld, grub.cfg
kernel/
  EarlyKernel/  — 无堆/VMM阶段：main, gdt, idt, idt_stubs, pic, pmm, tty, uart
  InitKernel/   — 有堆/VMM阶段：main, vmm, heap, acpi, apic
  BasicDrivers/ — apic_timer, keyboard
  Userland/     — userland.asm（Ring 3 用户态程序）
include/kernel/ — 所有模块头文件（15个）
```

## 两阶段启动架构

1. **EarlyKernel** (`kernel_main`): 无动态内存阶段，初始化 tty→gdt→pic→idt→pmm
2. **InitKernel** (`init_kernel`): 有堆/VMM后，初始化 vmm→heap→acpi→apic→timer→keyboard
3. 最终通过 `iretq` 切换到 Ring 3 用户态（`userland.asm`）

## 虚拟地址空间

| 区域 | 地址 |
|---|---|
| 身份映射 | 0x0 – 0xFFFFFFFF (4GB, 4×1GB大页) |
| 内核代码/数据 | 0xFFFFFFFF80000000+ |
| 内核堆 | 0xFFFFFFFFA0000000+ (512MB预留) |
| MMIO | 0xFFFFFFFFC0000000+ (LAPIC/IOAPIC等) |

## 已实现功能

- GDT+TSS+IST（专用异常栈：DF/NMI/MC）
- IDT 256门（CPU异常+IRQ+syscall/sysret）
- PMM 位图分配器（支持4GB）
- VMM 4级分页（按需映射、MMIO映射）
- 内核堆（隐式空闲链表，first-fit）
- ACPI 解析（RSDP→XSDT/RSDT→MADT）
- LAPIC + IOAPIC（MMIO映射，中断重定向，ISA Override支持）
- LAPIC 定时器（PIT校准，100Hz周期）
- PS/2 键盘（8042，Scancode Set 1，按键重复，环形缓冲区）
- VGA 终端（80×25，硬件光标，滚动）
- UART COM1（115200 8N1，调试输出）
- Ring 3 用户态（通过iretq切换，syscall write/read/exit）

## 最近调试记录

- 修复过 GDT `make_seg` 中 flags 和 limit_high 位置写反的 bug
- 修复过 `idt_stubs.asm` BSS 段问题（专用异常栈）
- 修复过 ACPI 内存映射 4MB-1GB 间隙导致的 Page Fault（扩展身份映射到4GB）
- 曾修复 boot.asm 第223行 NASM invalid operand type 错误
- 用户态 Ring 3 切换已实现（userland.asm 通过 iretq）

## 项目下一步方向（推测）

- 系统调用完善（目前只有 write/read/getpid/exit）
- 进程/线程管理
- 文件系统
- 网络协议栈
