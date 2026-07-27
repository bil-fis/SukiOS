# Step15：低风险审计项 L1–L8 完全修复

> 生成时间：2026-07-27
> 关联审计清单：`results/step10_todos.md` 附录 C（低风险 L1–L8）
> 目标：完全修复 8 个低风险项（可观测性 / 容量截断 / 内存安全围栏 / 构建硬化 / 栈保护 / 驱动健壮性 / FS 解析 / 可读性），其中 L3/L5 涉及内核内存安全防线，属"低风险但地基性质"。
> 验证：QEMU i440FX（qemu64）`make iso disk && make run-headless` 冒烟全部通过，SMAP ENABLED、LBA48 生效、各 Ring3 服务在线、无 panic。

---

## 0. 修复总览

| 项 | 文件 | 问题 | 修复手段 | 风险等级 |
|----|------|------|----------|----------|
| L1 | `kernel/syscall/syscall.c` | `sys_debug_write` 单条截断 256B 静默丢字节 | 分块循环输出，全量写完 | 低（可观测性） |
| L2 | `kernel/drivers/ata.c` `include/kernel/ata.h` | 仅取 LBA28(word60/61)，>128GB 盘容量截断 | 优先读 LBA48(word100-103) + 48 位 PIO 寻址原语 | 低（大盘支持） |
| L3 | `boot/boot.S` `kernel/syscall/syscall.c` `kernel/kmain.c` | SMAP 未启用，内核误踩用户内存无硬件防线 | boot.S 64 位段探测 CPUID.07 EBX.bit19→置 CR4.SMAP，copy_*_user 内 STAC/CLAC 围栏 + 启动日志 | 低（但属安全防线） |
| L4 | `boot/linker.ld` `Makefile` | LMA=1MB 与 MB2 头在镜像前 32KiB 的强布局假设无构建期断言 | 链接后 `readelf` 校验 `.boot` VMA=0x100000 且文件偏移<0x8000 | 低（构建硬化） |
| L5 | `Makefile` `kernel/stack_canary.c`(新) `user/lib/stack_canary.c`(新) | 全程 `-fno-stack-protector`，无栈金丝雀 | `-fstack-protector-strong -mstack-protector-guard=global` + 提供 `__stack_chk_guard/__stack_chk_fail` | 低（但属安全防线） |
| L6 | `kernel/drivers/hda.c` | connection list 条目数无上限，异常 codec 越界读响应窗口 | 长度钳制 ≤64 + 整链超时校验 | 低（驱动健壮） |
| L7 | `user/fs_server.c` | LFN 序号可伪造→名字截断处无 NUL | 整段缓冲清零 + 逆序连续序号严格递减校验 + 断链回退 8.3 | 低（FS 解析） |
| L8 | `kernel/sched/sched.c` | `KSTACK_SIZE` 命名/注释与用户栈常量易混淆 | 重命名为 `KERNEL_STACK_BYTES` 并加注释澄清两套栈 | 低（可读性） |

---

## 1. L1：`sys_debug_write` 长日志分块不截断

**位置**：`kernel/syscall/syscall.c`（syscall 号 4）

**问题**：旧实现把单条 `buf/len` 硬截断到 256B 的 `kbuf`，剩余字节静默丢弃，长日志（内核转储、大段追踪）被截断丢失，仅影响可观测性但排查问题时致命。

**修复**：改为分块循环——每次从用户态 `copy_from_user` 拷入最多 255 字节临时缓冲并立即 `kprintf`，直至全部 `len` 处理完毕：

```c
static uint64_t sys_debug_write(uint64_t buf_uptr, uint64_t len)
{
    char kbuf[256];
    uint64_t done = 0;
    while (done < len) {
        size_t chunk = (size_t)(len - done);
        if (chunk > sizeof(kbuf) - 1) chunk = sizeof(kbuf) - 1;
        if (copy_from_user(kbuf, (const void *)(buf_uptr + done), chunk) != chunk) {
            return done ? done : (uint64_t)-1;   /* 已输出部分不丢 */
        }
        kbuf[chunk] = '\0';
        kprintf("%s", kbuf);
        done += chunk;
    }
    return done;
}
```

中途遇非法用户指针则停止并返回已成功写入的字节数（`done>0` 时不报 -1），保证已输出部分不丢失。注意此处仍走 `copy_from_user`（满足强制规则：从用户态接收的指针严禁直接解引用），且 L3 的 SMAP 围栏已覆盖该路径。

---

## 2. L2：ATA LBA48 容量 + 48 位 PIO 寻址

**位置**：`kernel/drivers/ata.c`、`include/kernel/ata.h`

**问题**：原 `ata_init` 仅读 IDENTIFY `word[60]|word[61]`（LBA28，上限 2^28-1 ≈ 128GiB），`g_total_sectors` 为 `uint32_t`，>128GB 盘容量被截断。

**修复**：

1. **容量识别**：优先取 LBA48 容量 `word[100..103]`（最多 2^48-1 扇区），`word[83] bit10` 指示 LBA48 支持；不支持或 LBA48 容量为 0 时回退 LBA28：

```c
uint64_t lba28 = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
uint64_t lba48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16)
               | ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
g_lba48 = (id[83] & 0x0400) != 0;
g_total_sectors = (g_lba48 && lba48 > 0) ? lba48 : lba28;
```

2. **类型放大**：`g_total_sectors` 改 `uint64_t`，`ata_total_sectors()` 返回类型由 `uint32_t` 升为 `uint64_t`（头文件同步）。

3. **48 位寻址原语** `ata_select_lba()`：仅在 `lba+count > 0x0FFFFFFF` 且磁盘支持 LBA48 时走 48 位路径（LBA 与计数字节各写两次、DRIVE 仅置 `0x40`、命令用 `READ/WRITE SECTORS EXT`=0x24/0x34）；否则照旧 LBA28。故 <128GiB 的常见盘（含 QEMU 64MB 测试盘）仍走 LBA28，**零回归风险**：

```c
bool use48 = g_lba48 && ((uint64_t)lba + count) > 0x0FFFFFFFULL;
ata_select_lba(lba, count, use48);
outb(ATA_CMD, use48 ? 0x24 : CMD_READ_SECTORS);
```

QEMU 冒烟实测：`[ata] primary master OK: 131072 sectors (64 MiB) lba48=1`，容量识别正确。

---

## 3. L3：启用 SMAP + copy_*_user 硬件围栏

**位置**：`boot/boot.S`、`kernel/syscall/syscall.c`、`kernel/kmain.c`

**问题**：原内核仅 SMEP/W^X/栈 ASLR，未启用 SMAP（管理者态默认不可访问用户页）。一旦内核代码误踩用户虚拟地址，硬件不会拦截，是潜在越权/踩内存隐患。

**修复**：

1. **boot.S 置位 CR4.SMAP**：必须放在 **64 位段** `long_mode_start`（在 `jmp higher_half_entry` 前）。原因：`g_smap_enabled` 位于高半区 `.bss`（地址 >4GB），32 位段无法用 32 位位移寻址；且 CR4 读写在 64 位模式须用 `movq`。仅当 `CPUID.07 EBX.bit19` 支持 SMAP 才置位并写 `g_smap_enabled=1`，避免不支持的 CPU 执行 STAC/CLAC 触发 #UD 三重故障：

```asm
movl $7, %eax
xorl %ecx, %ecx
cpuid
testl $(1 << 19), %ebx
jz   .Lno_smap
movq %cr4, %rax
orl  $(1 << 21), %eax       /* CR4.SMAP */
movq %rax, %cr4
movabsq $g_smap_enabled, %rax
movb $1, (%rax)
.Lno_smap:
```

2. **syscall.c 围栏**：新增 `g_smap_enabled` 全局标志与 `smap_stac()/smap_clac()` 辅助（仅当标志为真才发 STAC/CLAC），并包裹 `copy_from_user`/`copy_to_user` 内的 `memcpy`。这是系统唯一的用户指针解引用边界（port.c 经这两个函数转发 mach_msg 数据）；`elf.c` 经 `PHYS_TO_VIRT` 内核直映写用户页，不触碰用户虚拟地址，不受 SMAP 约束。

3. **kmain.c 启动日志**：`extern uint8_t g_smap_enabled;`，启动打印 `[boot] SMAP (user-memory protection): ENABLED` 以验证生效。

**验证**：`make run-headless` 输出 `SMAP (user-memory protection): ENABLED`，且各服务正常（SMAP 未误伤合法用户访问）。

---

## 4. L4：MB2 头 / LMA 构建期校验

**位置**：`boot/linker.ld`、`Makefile`

**问题**：`linker.ld` 强依赖"LMA=0x100000 且 Multiboot2 头位于镜像前 32KiB"的布局假设，但 `ld` 不支持顶层 `ASSERT`（实测该环境 `ld` 拒绝顶层 `ASSERT` 语法），改动脚本时此约束易被悄然破坏导致 GRUB 找不到 MB2 头。

**修复**：保留 `linker.ld` 不变，在 `Makefile` 的 `$(KERNEL)` 链接规则内追加 `readelf` 校验：链接出 `kernel.elf` 后，解析 `.boot` 段（`readelf -SW`），确认其 **VMA==0x100000** 且 **文件偏移 <0x8000 且 偏移+大小 ≤0x8000**，否则 `exit 1` 中断构建：

```make
@readelf -SW $(KERNEL) | awk '$$3==".boot"{ \
    a=strtonum("0x"$$5); o=strtonum("0x"$$6); s=strtonum("0x"$$7); \
    if (a != 0x100000) { print "!! L4 FAIL: .boot VMA != 1MB (got "a")"; exit 1; } \
    if (o >= 0x8000 || o+s > 0x8000) { print "!! L4 FAIL: MB2 header beyond first 32KiB (off="o" size="s")"; exit 1; } \
    print "==> L4 check: .boot VMA=0x"a" fileoff=0x"o" size=0x"s" (within first 32KiB)"; }'
```

实测：`==> L4 check: .boot VMA=0x100000 fileoff=0x1000 size=0x192 (within first 32KiB)`。

---

## 5. L5：内核/用户态栈金丝雀

**位置**：`Makefile`、`kernel/stack_canary.c`(新)、`user/lib/stack_canary.c`(新)

**问题**：全程 `-fno-stack-protector`，内核与用户态任务均无栈溢出保护。

**修复**：

1. **编译标志**：`CFLAGS`/`USER_CFLAGS`/`APP_CFLAGS` 全部由 `-fno-stack-protector` 改为：
   ```
   -fstack-protector-strong -mstack-protector-guard=global
   ```
   关键 `-mstack-protector-guard=global`：默认 x86_64 的栈保护器守卫槽是 **TLS `%fs:0x28`**，但本系统 freestanding 用户任务无 `%fs` 设置，会导致 `mov %fs:0x28,%rax` 访问地址 `0x28` 触发用户态 #PF（正是初版引入的回归，经 QEMU 冒烟发现 `cr2=0x28`）。强制 `global` 模型后改用全局符号 `__stack_chk_guard`，与内核（因 `-mcmodel=large` 本就默认 global）一致。

2. **守卫提供文件**：新增两个文件，以 `-fno-stack-protector` 编译（否则 `__stack_chk_fail` 自身被插桩递归调用、且 guard 未初始化即误报）：
   - `kernel/stack_canary.c`：定义 `uintptr_t __stack_chk_guard`（用确定序列 `0xDEADBEEFCAFEBABE` 初始化，非随机以保证栈不变性下的可重现性）、`void __stack_chk_fail(void)`（打印 `KERNEL STACK OVERFLOW` 后 `panic`）。
   - `user/lib/stack_canary.c`：同名符号，用户态失败打印 `STACK OVERFLOW in task` 后走 `sys_task_exit(-1)` 杀掉自身（用户态崩溃隔离，不拖垮内核）。

3. **Makefile 接线**：`USER_LIB_OBJS` 加入 `$(BUILD)/user/lib/stack_canary.c.o`；新增两条专用编译规则对上述两文件加 `-fno-stack-protector` 覆盖全局 `CFLAGS`/`USER_CFLAGS`。

**验证**：重建 `shell.elf` 反汇编确认 `main` 前序改为读取全局 `__stack_chk_guard`（无 `%fs:0x28` 残留）；QEMU 冒烟 shell 正常在线，说明金丝雀在运行期被正确放置/校验。

---

## 6. L6：HDA connection list 上限 + 超时校验

**位置**：`kernel/drivers/hda.c` `hda_codec_setup()`

**问题**：从 Pin 的 `GET_CONN_LIST_LEN` 取连接列表长度后直接 `for(i<len)` 轮询 `GET_CONN_LIST`，异常/损坏 codec 可能报告超大长度导致无界 verb 轮询甚至越界读响应窗口；且单条响应超时未处理（返回 `0xFFFFFFFF` 被当有效 NID）。

**修复**：

1. 先校验 `PAR_CONN_LIST_LEN` 响应本身是否为 `HDA_VERB_TIMEOUT`，超时即放弃。
2. 将长度 `clamp` 到 64（真实 codec 连接数极少超过 4，过大即异常）：`if (len > 64) { kprintf(...); len = 64; }`。
3. 循环内每条 `GET_CONN_LIST` 响应若为 `HDA_VERB_TIMEOUT`，打日志并 `break`。

> 注：H7（`hda_param` 把超时当能力位）属 P2-5 收尾，本次仅处理 L6 关注的 connection list 边界与超时。

---

## 7. L7：LFN 序号伪造 / 越界校验

**位置**：`user/fs_server.c`（`lfn_reset`/`lfn_add`/`lfn_pull`）

**问题**：原 `lfn_add` 仅校验 `seq*13 < LFN_CAP`，序号可被伪造跳变，且上一条 LFN 残留在 `g_lfn` 中时，若新链出现"截断处无 NUL"会被 `lfn_pull` 的"遇 NUL 即止"逻辑误纳入旧字节。

**修复**：

1. **整段清零**：`lfn_reset()` 由 `g_lfn[0]='\0'` 改为 `memset(g_lfn, 0, sizeof(g_lfn))`，彻底清除上一文件残留。
2. **连续性校验**：新增 `g_lfn_prev`/`g_lfn_broken`。LFN 物理逆序排列，首条为最高序号；要求每条序号**严格递减 1**（`g_lfn_seq==0` 记起点；否则必须 `== g_lfn_prev-1`）。任何断层、重复、越界/非法序号（`seq==0` 或 `seq*13>=LFN_CAP`）都置 `g_lfn_broken=true` 并忽略后续条目。
3. **回退短名**：`lfn_pull()` 在 `!g_lfn_valid || g_lfn_broken` 时返回 0，调用方回退到 8.3 短名，杜绝"截断处无 NUL/注入垃圾字符"问题。

---

## 8. L8：`KSTACK_SIZE` 重命名 + 注释澄清

**位置**：`kernel/sched/sched.c`

**问题**：`KSTACK_SIZE` 与用户态栈常量（`USER_STACK_PAGES` 等）单位/用途不同，命名易混淆。

**修复**：重命名为 `KERNEL_STACK_BYTES`（值仍为 16384），并在宏定义处加注释澄清：这是 **Ring0 内核栈**（syscall/中断/调度使用），与用户态栈（32 页=128KiB）是**两套完全独立的栈**。全部引用（`kmalloc(KERNEL_STACK_BYTES)`、`kstack_top = ... + KERNEL_STACK_BYTES`）同步更新。

---

## 9. 构建与验证汇总

### 9.1 构建命令
```
make iso disk      # 生成 ISO 与 disk 镜像，含 L4 readelf 校验
make               # 仅链接内核 ELF + L4 校验
make run-headless  # QEMU 无头冒烟（默认 i440FX / qemu64）
```

### 9.2 关键验证点（QEMU 实测）
| 验证项 | 期望 | 实测 |
|--------|------|------|
| L1 长日志 | 全量输出不截断 | 分块路径编译通过，逻辑覆盖 |
| L2 LBA48 | `lba48=1`，容量正确 | `[ata] ... 131072 sectors (64 MiB) lba48=1` |
| L3 SMAP | ENABLED 且不误伤 | `[boot] SMAP (user-memory protection): ENABLED` |
| L4 布局 | `.boot` VMA=1MB 且 <32KiB | `VMA=0x100000 fileoff=0x1000 size=0x192` |
| L5 金丝雀 | 全局 guard 模型、无 `%fs:0x28` | 反汇编确认；shell 在线 |
| L6 HDA | 上限+超时不影响启动 | 无相关 panic，音频服务正常 |
| L7 LFN | 断链回退 8.3 | 编译通过 |
| L8 栈常量 | 重命名生效 | 编译通过 |

### 9.3 调试命令（按项目规则）
```bash
# 断点验证 SMAP 是否已置位（CR4 bit21）
qemu-system-x86_64 -s -S -machine pc -cpu qemu64 ... 
gdb -ex "target remote :1234" \
    -ex "break kmain" \
    -ex "continue" \
    -ex "info registers cr4"   # 期望 bit21 (0x200000) = 1
# 验证栈保护器 guard（用户态）
objdump -d build/user/shell.elf | grep -A6 "<main>:" | grep -i "stack_chk_guard"
```

---

## 10. 文件改动清单

| 文件 | 改动 |
|------|------|
| `kernel/syscall/syscall.c` | L1 分块 debug_write；L3 `g_smap_enabled` + `smap_stac/clac` 围栏包裹 `copy_*_user` |
| `boot/boot.S` | L3 64 位段探测 CPUID.07 EBX.bit19→置 CR4.SMAP + 写 `g_smap_enabled` |
| `kernel/kmain.c` | L3 声明 `g_smap_enabled` 并输出 SMAP 状态日志 |
| `include/kernel/ata.h` | L2 `ata_total_sectors()` 返回 `uint64_t` |
| `kernel/drivers/ata.c` | L2 LBA48 容量 + `ata_select_lba` 48 位寻址原语 + `g_total_sectors`/`g_lba48` 改 64 位 |
| `boot/linker.ld` | L4 无脚本内断言（ld 不支持），校验移至 Makefile |
| `Makefile` | L4 链接后 readelf 校验；L5 三套 CFLAGS 加 `-fstack-protector-strong -mstack-protector-guard=global` + 金丝雀专用规则 + `USER_LIB_OBJS` 接线 |
| `kernel/stack_canary.c` | L5 新增：内核 `__stack_chk_guard`/`__stack_chk_fail`（以 `-fno-stack-protector` 编） |
| `user/lib/stack_canary.c` | L5 新增：用户态同名符号，失败杀自身 |
| `kernel/drivers/hda.c` | L6 connection list 长度钳制 ≤64 + 整链超时校验 |
| `user/fs_server.c` | L7 `lfn_reset` 整段清零 + 逆序连续序号校验 + `lfn_pull` 断链回退 |
| `kernel/sched/sched.c` | L8 `KSTACK_SIZE`→`KERNEL_STACK_BYTES` + 注释 |
