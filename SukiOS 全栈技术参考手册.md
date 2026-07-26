# SukiOS 全栈技术参考手册 v1.0

**版本**：1.0
**架构**：x86_64 (LP64)
**内核哲学**：混合内核（Mach 消息传递 + BSD 抽象）

---

## 第1章：开发环境与工具链 (Build System)

### 1.1 必需工具
- **交叉编译器**：`x86_64-elf-gcc` (>= 12.0) + `x86_64-elf-binutils`
- **模拟器**：QEMU (>= 7.0) 启用 `qemu-system-x86_64`
- **磁盘工具**：`mtools` (操作FAT32镜像), `xorriso` (生成UEFI/BIOS双启ISO)
- **调试器**：GDB (需支持 `target remote :1234`)

### 1.2 顶层 Makefile 核心变量
```makefile
ARCH           = x86_64
CC             = x86_64-elf-gcc
AS             = x86_64-elf-as
LD             = x86_64-elf-ld
CFLAGS         = -ffreestanding -Wall -Wextra -O2 -mno-red-zone -mcmodel=kernel -mgeneral-regs-only -I include/
LDFLAGS        = -nostdlib -static -T boot/linker.ld
QEMU_FLAGS     = -machine q35 -cpu qemu64 -m 2G -serial stdio -device VGA -display gtk
```

### 1.3 镜像生成规范
- 使用 `grub-mkrescue` 生成 ISO，确保同时兼容 Legacy BIOS 和 UEFI。
- 磁盘镜像 (`hdd.img`) 必须包含 MBR 分区表 + 一个 FAT32 主分区（类型 `0x0C`），用于放置内核 ELF 和用户态程序。

---

## 第2章：内存布局 (Memory Layout) —— 绝对红线

### 2.1 虚拟地址空间划分 (x86_64 48-bit)
| 范围 | 用途 | 权限 |
| :--- | :--- | :--- |
| `0x0000000000000000` - `0x00007FFFFFFFFFFF` | **用户态空间** (每个进程独立) | U/S=1 |
| `0xFFFF800000000000` - `0xFFFFFFFFFFFFFFFF` | **内核空间** (全局共享) | U/S=0 |
| `0xFFFF800000000000` | **内核基址 (KERNEL_BASE)** | - |
| `0xFFFF800000100000` | 内核文本段映射 (Phys `0x100000`) | - |

### 2.2 物理内存映射 (早期BIOS/e820)
- `0x00000000 - 0x0009FFFF` : 低端内存 (保留)
- `0x000A0000 - 0x000FFFFF` : VGA显存/ROM (保留)
- `0x00100000 - 0x????????` : **可用内存 (内核加载于此)**
- 物理内存管理器 (PMM) 仅管理 `0x100000` 以上的可用区域，使用 **位图 (Bitmap)**，每bit代表4KB页。

### 2.3 内核栈布局
- **中断栈 (Interrupt Stack)**：每个CPU核心独立，大小 16KB，位于内核BSS段。
- **内核线程栈**：由 `kmalloc` 动态分配，在 `task_struct` 中记录 `kernel_rsp`。

---

## 第3章：启动协议 (Boot Protocol)

### 3.1 Multiboot2 强制要求
`boot/multiboot2_header.s` 必须包含以下请求标签：
1.  `MULTIBOOT_TAG_TYPE_FRAMEBUFFER`：请求 1024x768x32 像素格式。
2.  `MULTIBOOT_TAG_TYPE_EFI64`：强制UEFI启动模式（若硬件支持）。
3.  `MULTIBOOT_TAG_TYPE_ADDRESS`：告知 GRUB 我们不覆盖低端 `0x0-0x100000` 内存。

### 3.2 入口点规范
- 物理入口点：`0x100000`（汇编标签 `_start`）。
- **必须**在 5 条指令内完成：检查 CPUID -> 启用 PAE -> 加载临时三级分页表 -> 跳转至高地址 `kmain`。
- 临时页表必须同时包含 **恒等映射 (0x0)** 和 **高地址映射 (KERNEL_BASE)**，跳转完成后立即销毁恒等映射。

---

## 第4章：核心数据结构定义 (ABI 稳定版)

### 4.1 任务控制块 (PCB) —— `include/kernel/task.h`
```c
typedef struct task {
    uint64_t id;                // 全局唯一PID
    enum task_state { READY, RUNNING, BLOCKED, WAITING } state;
    
    // 上下文切换 (保存于内核态栈顶)
    uint64_t rsp;               // 内核栈指针
    uint64_t rip;               // 下次调度执行的指令地址
    uint64_t cr3;               // 进程页表基址 (用户态使用)
    
    // 调度参数
    uint64_t ticks_remaining;   // 时间片剩余
    uint64_t priority;          // 0-255 (数值越小优先级越高)
    
    // IPC 关联
    struct kernel_port *reply_port; // 当前正在等待的回复端口
    
    // 文件系统上下文
    uint32_t cwd_cluster;       // 当前工作目录的FAT32簇号
    
    struct task *next;          // 就绪队列链表
} task_t;
```

### 4.2 物理内存管理器 (PMM) —— `include/mm/pmm.h`
```c
typedef struct page_frame {
    uint64_t base_addr;         // 物理基址 (按4K对齐)
    uint64_t ref_count;         // 引用计数 (用于共享内存零拷贝)
    struct page_frame *next;    // 空闲链表
} page_frame_t;

// 核心接口
void* pmm_alloc_page(void);
void  pmm_free_page(void* addr);
```

### 4.3 Mach 端口核心 —— `include/ipc/port.h`
```c
#define MACH_PORT_NULL 0

typedef struct kernel_port {
    uint32_t id;                // 端口名 (类似句柄)
    uint64_t owner_task;        // 拥有者PID
    uint64_t recv_task;         // 接收者PID (允许转移所有权)
    struct ipc_message *queue_head; // 消息队列头
    struct kernel_port *next;   // 哈希表链
} kernel_port_t;

// 消息结构 (对齐到 8 字节)
typedef struct ipc_message {
    struct ipc_message *next;
    uint64_t sender_pid;
    uint64_t size;              // 总大小 (包含头 + 数据)
    void    *ool_data;          // 若为带外(OOL)数据,指向物理页描述符
    char    data[0];            // 变长内联数据
} ipc_message_t;
```

---

## 第5章：系统调用与 IPC 协议 (Ring 0↔Ring 3)

### 5.1 Syscall 约定
- **指令**：`syscall` (使用 `IA32_LSTAR` MSR)
- **调用号**：`%rax`
- **参数**：`%rdi, %rsi, %rdx, %r10, %r8, %r9` (遵循 System V AMD64 ABI)
- **返回**：`%rax` (错误码) 或 `0` 成功
- **段选择子**：
  - 内核代码段 (CS) = `0x08`
  - 内核数据段 (DS) = `0x10`
  - 用户代码段 (CS) = `0x1B` (RPL=3)
  - 用户数据段 (DS) = `0x23` (RPL=3)

### 5.2 核心系统调用表 (仅3个起步)
| 编号 | 名称 | 功能描述 |
| :--- | :--- | :--- |
| `0` | `sys_mach_msg` | 发送/接收 IPC 消息（阻塞或非阻塞） |
| `1` | `sys_task_create` | 创建新进程（加载 ELF） |
| `2` | `sys_task_exit` | 终止当前进程 |
| `3` | `sys_yield` | 主动让出 CPU（协作式让步） |

### 5.3 IPC 消息标准布局 (二进制协议)
```c
// 发送给内核的原始数据 (用户态需按此结构填充)
typedef struct mach_msg_base {
    uint32_t msgh_remote_port;  // 目标端口ID
    uint32_t msgh_local_port;   // 回复端口ID (若需回复)
    uint32_t msgh_bits;         // bit 0: 是否包含OOL数据; bit 1: 是否期待回复
    uint32_t msgh_size;         // 消息总大小 (包含此头)
} mach_msg_base_t;

// 内核转发时会在头部附加 sender_pid 和 timestamp。
```

---

## 第6章：用户态服务接口契约 (Service APIs)

### 6.1 文件系统服务 (FS_SERVER) —— 端口 `0x1001`
- **请求格式**：`[opcode: uint32][path: string][buffer: ool/内存]`
  - `opcode 1` = READ 文件 : 返回 `[error][data]`
  - `opcode 2` = LIST_DIR : 返回 `[dentry_count][entries...]`
- **约束**：路径字符串必须以 `\0` 结尾，最长 256 字符。

### 6.2 显示服务 (DISPLAY_SERVER) —— 端口 `0x1002`
- **请求格式**：`[opcode][x][y][width][height][pixel_buffer_ool]`
  - `opcode 1` = 绘制矩形 (像素数据从 OOL 内存取)
  - `opcode 2` = 绘制 BMP 图标
- **硬件交互**：显示服务通过 `sys_mach_msg` 向内核的 `PORT_DRIVER_FB` (0x0001) 请求设置分辨率或获取显存偏移。

### 6.3 输入服务 (INPUT_SERVER) —— 端口 `0x1003`
- 内核键盘中断 (`IRQ1`) 不进行扫描码解析，直接将原始 `[make/break][scancode]` 打包成消息发送至 `0x1003`。
- INPUT_SERVER 解析后，向所有注册了监听的应用广播 `[key_ascii][modifiers]`。

---

## 第7章：GUI 合成协议 (远期规范)

### 7.1 合成器 (Compositor) 零拷贝机制
- 应用调用 `sys_mach_msg` 发送 `UPDATE_WINDOW` 消息时，`msgh_bits` 必须设置 `OOL_BIT`。
- 消息体包含 `[window_id][offset_x][offset_y][dirty_rect]`。
- 内核收到此消息，**不拷贝** `dirty_rect` 对应的物理页，而是查 `page_frame` 引用计数+1，将物理页句柄转发给 DISPLAY_SERVER。
- DISPLAY_SERVER 处理完后，调用 `sys_mach_msg` 回复 `RELEASE_PAGE`，内核扣减引用计数。

---

## 第8章：调试与故障排查 (Debugging)

### 8.1 强制日志输出
- **必须**在 `kernel/arch/x86_64/serial.c` 实现 `serial_write(char)`，使用 COM1 (`0x3F8`)。
- 所有 `panic()` 函数必须同时输出到串口和帧缓冲（若可用）。
- QEMU 启动参数追加：`-serial file:debug.log` 以保存日志。

### 8.2 GDB 脚本约定
项目根目录放置 `.gdbinit`：
```gdb
set architecture i386:x86-64
target remote localhost:1234
add-symbol-file build/kernel.elf 0xFFFF800000100000
break kmain
continue
```

### 8.3 Triple Fault 救援方案
若物理机黑屏重启，请在 `boot/linker.ld` 中预留 `0x1000` 字节的 **救援堆栈**（物理地址固定），并在汇编入口处添加 `cli; hlt` 循环。若系统陷入该循环，说明分页崩溃，可用逻辑分析仪抓取 `RIP`。

---

## 第9章：编码风格与AI协作规范

### 9.1 命名规范
- **宏定义**：全大写 + 下划线 (`#define MAX_TASKS 128`)
- **类型定义**：小写 + `_t` 后缀 (`task_t`, `port_t`)
- **函数名**：`动词_名词` 格式 (`pmm_alloc_page`, `sched_add_task`)
- **全局变量**：`g_` 前缀 (`g_kernel_pml4`)

### 9.2 与AI协作铁律
1.  **单次生成限制**：每次只请求 AI 生成 **单个函数**（不超过 80 行）。AI 生成代码后，必须附带 **QEMU 验证指令**。
2.  **指针安全**：所有从用户态传入的指针（在 syscall 中）必须用 `copy_from_user` 拷贝到内核临时缓冲区，**严禁**直接解引用。
3.  **禁止内联汇编混编逻辑**：内联汇编必须单独封装在 `.c` 函数中，并添加 `__attribute__((noinline))`，防止编译器优化破坏指令顺序。

**文档签署**：本手册一旦定稿，所有后续开发均以本手册为准。若需修改内存布局或IPC协议，必须同时更新此文档的版本号。