# Step 05 — ELF 加载器 / execve 全链路调通（三个致命 syscall 入口 Bug 修复）

日期：2026-07-26
状态：**全部验证通过**（QEMU i440FX，`ls` / `cat` / `exec BIN/HELLO.ELF a b` 全链路回归）

---

## 一、本阶段成果总览

| # | 内容 | 结果 |
|---|------|------|
| 1 | 修复 syscall 入口 RFLAGS 计算破坏 `rax`（调用号）的 Bug | `unknown syscall 582` 根除 |
| 2 | 修复 syscall 返回路径 execve 帧改写破坏 `rax`（返回值）的 Bug | `mach_msg_send` 等返回值恢复正确 |
| 3 | 全局 syscall 返回暂存改为 **每任务** `scr_rip/scr_rsp`（`g_scratch` 指针机制） | 多任务并发 syscall 不再互相覆盖 RSP，用户态 #PF 崩溃根除 |
| 4 | 修复 `elf_build_stack` 中 argc 与 argv[] 之间的对齐空洞 | `argv[]` 野指针根除，System V ABI 初始栈完全合规 |
| 5 | 栈镜像 `kmalloc` 缓冲清零（防内核堆数据泄漏到用户栈） | 安全加固 |
| 6 | `sys_execve` 成功后把任务名替换为新映像 basename | 日志显示 `task 'HELLO.ELF' pid=5 exit(code=0)` |
| 7 | 移除全部临时调试打印（sched/ipc/fs_server/elf） | 日志干净 |

最终效果：**Shell 键入 `exec BIN/HELLO.ELF a b`，内核经 FS IPC 从 FAT32 读出 ELF64，
替换 shell 进程映像，新程序以 argc=3、argv 正确、栈 16 字节对齐的状态从 `_start` 运行并正常退出。**

---

## 二、Bug 详解与修复

### Bug 1：`unknown syscall 582` —— RFLAGS 值被当作系统调用号

- **文件**：`kernel/arch/x86_64/syscall_entry.S`
- **现象**：fs_server/input_server 启动即报 `[syscall] unknown syscall 582`（582 = 0x246 = 典型用户 RFLAGS）。
- **根因**：构造 iretq 帧时用
  `movq %r11, %rax; orq $0x200, %rax; pushq %rax` 计算帧 RFLAGS，
  **覆盖了 `rax` 中的系统调用号**；随后 `movq %rax, %rdi` 把 RFLAGS 当调用号传给 `syscall_dispatch`。
- **修复**：直接 `orq $0x200, %r11; pushq %r11`（r11 在 rcx 压栈后即为纯 scratch），`rax` 全程不动。
- **诊断手段**：objdump 反汇编用户 ELF，确认用户侧 `mov $imm,%eax; syscall` 正确 → 锁定内核侧。

### Bug 2：所有 syscall 返回值被破坏

- **文件**：`kernel/arch/x86_64/syscall_entry.S`
- **现象**：内核 `sys_mach_msg` 各失败分支均未触发（返回 0），但用户态 `mach_msg_send` 却报失败 → fs_server mount FAILED。
- **根因**：`syscall_dispatch` 返回后，execve 帧改写代码用
  `movq g_user_rip_scratch(%rip), %rax; movq %rax, 0(%rsp)` —— **`rax` 此刻是 syscall 返回值**，被 scratch 值覆盖，用户拿到的返回值是自己的 RSP/RIP 垃圾。
- **修复**：改用 `r11`/`rcx` 作暂存（iretq 前二者本就会被帧恢复覆盖，属于自由寄存器），`rax` 原样交还用户态。

### Bug 3：全局 scratch 被并发任务覆盖 → 用户态 #PF 崩溃

- **文件**：`kernel/arch/x86_64/syscall_entry.S`、`include/kernel/task.h`、`kernel/sched/sched.c`、`kernel/syscall/syscall.c`
- **现象**：fs_server mount 成功后，fs-server（write）与 input-server（read）先后 `user #PF: cr2=0` 被杀；`#PF` 打印增加 `rip=` 后确认崩溃点在正常代码，但 RSP 已是别的任务的。
- **根因**：`g_user_rip_scratch/g_user_rsp_scratch` 是**单例全局**。任务 A 阻塞在 `mach_msg_recv`（syscall 内睡眠）期间，任务 B 进入 syscall 会覆盖这两个全局；A 被唤醒返回时，iretq 帧的 RIP/RSP 槽用了 B 的值 → A 回到用户态时栈指针错乱 → NULL/野指针访问。
- **修复（架构级）**：
  1. `task_t` 新增相邻字段：
     ```
     uint64_t scr_rip;   /* offset +0（相对字段组） */
     uint64_t scr_rsp;   /* offset +8 */
     ```
  2. `sched.c` 新增全局指针 `uint64_t *g_scratch`，在 `sched_init`、`schedule()`、`task_exit_current()` 三处上下文切换点同步为 `&current->scr_rip`。
  3. `syscall_entry.S` 入口：
     ```asm
     movq  %rsp, g_utmp_rsp(%rip)        # 短命全局（IF=0 窗口内安全，单核）
     movq  g_syscall_kstack(%rip), %rsp  # 切内核栈
     ... 构帧（pushq %rcx 作 RIP、pushq g_utmp_rsp 作 RSP）...
     movq  g_scratch(%rip), %r11         # r11 = &current->scr_rip
     movq  %rcx, (%r11)                  # scr_rip
     movq  g_utmp_rsp(%rip), %rcx
     movq  %rcx, 8(%r11)                 # scr_rsp
     ```
  4. 返回路径同理经 `g_scratch` 解引用，覆盖 iretq 帧 RIP（帧+0）/RSP（帧+24）。
  5. `sys_execve` 改写 `t->scr_rip/t->scr_rsp`（不再有全局变量）。
- **踩坑记录**：第一版误写 `movq %rcx, (g_scratch(%rip))` —— 这是存到 *g_scratch 变量本身的地址*，
  并非解引用；且在帧构建前就用 r11 作暂存会破坏用户 RFLAGS。必须先 `movq g_scratch(%rip), %r11`
  取出指针再 `(%r11)` 索引，且暂存操作放在 iretq 帧构建**之后**（此时 rcx/r11 才自由）。

### Bug 4：`elf_build_stack` 的 argc/argv 空洞 → argv 野指针

- **文件**：`kernel/elf/elf.c`
- **现象**：`exec BIN/HELLO.ELF` 后新程序打印 `argc = 1` 正确，但读 `argv[0]` 立刻
  `user #PF: cr2=0x7ffffff19ff20000`（明显是错位读到的垃圾 8 字节）。
- **根因**：旧代码顺序为「写 auxv → envp[] → argv[] → `sp=(sp-8)&~15` → 写 argc」。
  最后的 16 字节对齐使 argc 与 argv[] 之间**可能插入 8 字节空洞**；而 crt0 按 ABI 取
  `argv = rsp+8`，读到的是空洞里的 kmalloc 未初始化垃圾。
- **修复**：先算出指针块总大小
  `block = na*16 + (envc+1)*8 + (argc+1)*8 + 8`，
  预对齐 `sp_final = (sp - block) & ~15`，再**自底向上连续写** argc → argv[]+NULL → envp[]+NULL → auxv。
  保证 `rsp % 16 == 0` 且四部分零空隙（System V AMD64 ABI 3.4.1 布局）。
- **附带加固**：`img` 缓冲 `memset 0`（栈页任何空隙不得残留内核堆数据）。

---

## 三、涉及文件清单

| 文件 | 变更 |
|------|------|
| `kernel/arch/x86_64/syscall_entry.S` | 入口/返回路径重构：RFLAGS 用 r11、返回值 rax 保全、每任务 scratch、新增 `g_utmp_rsp`(.bss)、删除旧全局 scratch 符号 |
| `include/kernel/task.h` | `task_t` 新增 `scr_rip`/`scr_rsp`（必须相邻，汇编按 +0/+8 索引） |
| `kernel/sched/sched.c` | 新增 `uint64_t *g_scratch`；3 处切换点同步；删调试打印 |
| `kernel/syscall/syscall.c` | `sys_execve` 用 `t->scr_rip/scr_rsp`；execve 后更新任务名为 basename；unknown syscall 打印用户 RIP |
| `kernel/elf/elf.c` | 初始栈布局重写（预对齐+连续写入）；栈镜像清零 |
| `kernel/arch/x86_64/idt.c` | 用户 #PF 打印增加故障 `rip=`（保留，排障常用） |
| `kernel/ipc/port.c` | 删除临时 `[ipc-debug]` 打印 |
| `user/fs_server.c` | 删除临时 `[fs-debug]` 打印 |

---

## 四、验证方式

### 构建与启动

```bash
make iso
qemu-system-x86_64 -machine pc -cpu qemu64 -m 2G -no-shutdown -display none \
  -serial file:build/serial.log \
  -monitor unix:/tmp/qmon.sock,server,nowait \
  -boot d -cdrom build/SukiOS.iso \
  -drive file=build/disk.img,format=raw,index=0,media=disk
```

### 交互测试（QEMU monitor sendkey，python3 直连 unix socket）

依次键入 `ls`、`cat HELLO.TXT`、`exec BIN/HELLO.ELF a b`。

### 回归结果（serial.log 摘录）

```
SukiOS> ls
README.TXT    94 bytes
HELLO.TXT     20 bytes
ROADMAP.TXT   80 bytes
SYS           <DIR>
BIN           <DIR>
SukiOS> cat HELLO.TXT
hello from FAT32 :)
SukiOS> exec BIN/HELLO.ELF a b
=== EXECVE OK: HELLO.ELF loaded as ELF64 and now running ===
argc = 3
  argv[0] = BIN/HELLO.ELF
  argv[1] = a
  argv[2] = b
hello from a freshly execve'd ELF process; exiting now.
[syscall] task 'HELLO.ELF' pid=5 exit(code=0)
[sched] task 'HELLO.ELF' pid=5 exited
```

检查点：
- 无 `unknown syscall`；
- 无 `user #PF`；
- fs_server 常驻服务循环（`[fs] entering service loop on FS_PORT`）；
- execve 全链路：shell → `sys_execve` → 内核作 IPC 客户端向 FS_PORT 发 `FS_MSG_READ_FILE` →
  fs_server（Ring3）经 DISK_PORT 读 FAT32 → OOL 回传 ELF 字节 → `elf_load` 建新地址空间（W^X）→
  销毁旧空间 → iretq 帧改写跳新入口。

### GDB 断点建议（如需复查）

```bash
qemu-system-x86_64 -s -S -machine pc -cpu qemu64 ...
gdb build/kernel.elf -ex 'target remote :1234' \
    -ex 'b syscall_entry' -ex 'b sys_execve' -ex c
# 验证：进入 syscall_entry 时 info registers rax rcx r11
#       （rax=调用号，rcx=用户RIP，r11=用户RFLAGS）
# 返回 iretq 前检查帧：x/5gx $rsp（RIP,CS=0x1B,RFLAGS|0x200,RSP,SS=0x23）
```

---

## 五、经验教训（syscall 入口寄存器纪律）

1. `syscall` 指令语义：`rcx=用户RIP`、`r11=用户RFLAGS`、`rax=调用号/返回值` —— 这三个寄存器
   在入口段每一条指令前后都要明确"现在谁持有什么"，任何临时计算都不许借用它们（帧建成前）。
2. iretq 帧建成后，`rcx/r11` 即为自由 scratch（帧会恢复用户值？不——syscall 约定 rcx/r11 本就
   由 CPU 破坏，用户侧编译器已把它们列入 clobber，因此内核可任意使用；**唯独 rax 是返回值必须保全**）。
3. 任何"每进程状态"绝不允许放单例全局——只要 syscall 可睡眠（IPC recv），就存在并发覆盖窗口。
4. AT&T 语法 `(sym(%rip))` 并不是解引用：`movq sym(%rip), %reg` 取的是变量值（此处为指针），
   字段访问必须两步：先取指针到寄存器，再 `off(%reg)`。
5. System V 初始栈四段（argc/argv/envp/auxv）必须零空隙连续，对齐只能通过**预先**下移
   `sp_final` 实现，绝不能在中途插 padding。
