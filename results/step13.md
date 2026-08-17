# Step 13：中风险审计修复批次（M1–M18）

本批次依据 `results/step10_todos.md` 的中风险 / 接口债务清单，一次性修复 M1–M18 共 18 项，
覆盖 12 个文件：`kmalloc.c`、`pmm.c`、`vmm.c`、`port.c`、`port.h`、`sched.c`、`syscall.c`、
`elf.c`、`elf.h`、`ata.c`、`hda.c`、`fs_server.c`、`suki.c`、`console.c`。

---

## M1 — kfree 双重释放防护

**文件**：`kernel/mm/kmalloc.c`

**问题**：`kfree()` 仅校验块头 canary（防堆头覆写），未检测块是否已处于 free 态。
重复 `kfree` 同一指针会让合并逻辑再次执行，破坏空闲链表 `next/prev` 指针。

**修复**：canary 校验之后、置位 `b->free` 之前增加显式检查：

```c
if (b->free) {
    kprintf("[kheap] DOUBLE FREE at %p, ignoring\n", (void *)b);
    return;
}
```

**语义**：double-free 从"静默破坏链表"降级为"打印告警并忽略"，堆结构不受影响。

---

## M2 — pmm 引用计数溢出防护

**文件**：`kernel/mm/pmm.c`

**问题**：`pmm_incref()` 对 `g_refcount[pg]`（uint32_t）无条件 `++`。OOL 共享页被无限
incref 时计数回绕为 0，释放逻辑误判"无引用"提前回收，他任务仍持有的共享页遭破坏。

**修复**：封顶 `0xFFFFFFFF`，达到上限即拒绝递增并打印
`[pmm] refcount overflow at page %lu`。位图非原子问题留待 P0-3 锁体系统一解决。

---

## M3 — vmm_map_page 重复映射感知

**文件**：`kernel/mm/vmm.c`

**问题**：`vmm_map_page()` 对已存在的 PTE 直接覆盖——旧物理页不释放（页泄漏），
且新 flags 可静默改变权限。ELF 多段页边界重叠等场景每次 exec 泄漏内核页。

**修复**：写 PTE 前检测 `pt[i1] & PTE_PRESENT`：
- 普通页（无 `PTE_OOL`，bit9 软件位）：`pmm_free_page(old)` 先释放旧页；
- OOL 共享页：保留引用计数、打印 `[vmm] remap over OOL page va=%p` 便于诊断。

---

## M4 — 地址空间销毁与 OOL 共享页引用统一（所有权重构）

**文件**：`kernel/mm/vmm.c` + `kernel/ipc/port.c`

**问题**：`vmm_destroy_address_space()` 对所有 PTE 一律 `pmm_free_page()` 无条件释放。
对 refcount>1 的 OOL 共享物理页，这会导致映射同一物理页的其它任务的页表指向已释放
内存。同时 `port_reap_ool()` 也对同一批页 `pmm_decref()`，与 vmm 路径重复计数。

**修复（所有权归一）**：
- `vmm_destroy_address_space()`：所有页（普通/大页/OOL/页表页）统一改用
  `pmm_decref()`。普通页 ref==1 时 decref 即释放；OOL 共享页 ref>1 时仅释放
  本地址空间的一份引用。
- `port_reap_ool()`：**不再**对物理页 decref，只回收 OOL VA 窗口区间到
  `g_ool_free` 空闲链表供复用。

**不变量**：每个地址空间对其映射的每个物理页恰好持有 1 份引用；引用的释放唯一
发生在 `vmm_destroy_address_space()`。

---

## M5 — 端口消息队列长度上限（背压）

**文件**：`kernel/ipc/port.c` + `include/ipc/port.h`

**问题**：`deliver()` 无界 `enqueue`，恶意/失控任务狂发消息可耗尽内核堆。

**修复**：
- `port.h` 新增 `PORT_QUEUE_MAX 64` 与错误码 `MACH_SEND_NO_BUFFER 0x10000005UL`；
- `deliver()` 投递前检查 `p->queue_len >= PORT_QUEUE_MAX`，超限 `kfree(m)` 并返回
  `MACH_SEND_NO_BUFFER`，形成显式背压。

---

## M6 — mach_msg 接收侧防御性校验

**文件**：`kernel/ipc/port.c`

**问题**：接收路径信任队列内消息的 `ool_page_count`（`ool_pages[]` 定容
`MACH_MSG_OOL_MAX_PAGES`，越界即数组越界读），且 `recv_limit` 小于消息头也放行。

**修复**（在 `m->size > recv_limit` 检查之前）：
- `recv_limit < sizeof(mach_msg_header_t)` → `MACH_RCV_INVALID_NAME`；
- `m->has_ool` 且 `ool_page_count == 0 || > MACH_MSG_OOL_MAX_PAGES` →
  对可信范围内的页逐一 `pmm_decref` 后丢弃消息，返回 `MACH_RCV_NO_SPACE`。

---

## M7 — 任务数硬上限 + 内核栈溢出守卫

**文件**：`kernel/sched/sched.c`

**修复 1（任务上限）**：新增 `MAX_TASKS 256`；`task_create_kernel()` 入口检查
`g_task_count >= MAX_TASKS` 即拒绝创建（用户任务经同一路径创建，同受约束）。

**修复 2（栈守卫）**：
- 常量 `KSTACK_CANARY 0xCDC1FEEDDEADBEEFUL`；
- 任务创建时在内核栈最低 8 字节（`kstack_base`）写入哨兵（栈从 `kstack_top`
  向下增长）；
- `schedule()` 每次切换前校验当前任务哨兵，被破坏立即
  `panic("kernel stack overflow detected (task '%s' pid=%lu)")`，把静默内存损坏
  转为可诊断崩溃。idle 任务 `kstack_base==0` 跳过。

---

## M8 — exec_copy_args 失败路径内存回滚

**文件**：`kernel/syscall/syscall.c`

**问题**：`exec_copy_args()` 中途 `copy_from_user`/`kmalloc` 失败直接 `return false`，
已分配的 argv/envp 内核字符串全部泄漏（调用方以 argc=0 调 `exec_free_args` 不会释放）。

**修复**：5 处失败点统一改 `goto fail`；`fail:` 标签遍历已成功分配的
`argv_k[0..argc)` 与 `envp_k[0..envc)` 逐一 `kfree` 后返回 false。
成功路径的释放责任仍归调用方。

---

## M9 — ELF 头部整数回绕与 phnum 上限

**文件**：`kernel/elf/elf.c` + `include/kernel/elf.h`

**修复**：
- `elf.h` 新增 `ELF_PHDR_MAX 64`；
- `elf_validate()`：`e_phnum > ELF_PHDR_MAX` 拒绝；
  `phtab = e_phoff + e_phnum*e_phentsize` 计算后检查 `phtab < e_phoff`（加法回绕）
  与 `phtab > size`；
- `PT_LOAD` 逐段校验：`p_offset + p_filesz < p_offset`（回绕）、
  `p_vaddr + p_memsz < p_vaddr`（回绕）均拒绝，再做原有的 `size` /
  `USER_SPACE_TOP` 上限检查。

**效果**：恶意 ELF 无法通过字段回绕绕过边界检查，把段映射到内核区或越界读。

---

## M10 — ATA 读路径越界防护

**文件**：`kernel/drivers/ata.c`

**问题**：写路径已有 `lba + count > g_total_sectors` 保护，读路径缺失。

**修复**：`ata_read_sectors()` 入口增加
`(uint64_t)lba + count > g_total_sectors || (uint64_t)lba + count < lba`
（越界 + 无符号回绕）双检查。`g_total_sectors` 来自 IDENTIFY word 60/61
（当前 QEMU 盘 131072 扇区 = 64 MiB）。

---

## M11 — HDA verb 忙等让出 CPU

**文件**：`kernel/drivers/hda.c`

**问题**：`hda_verb_raw()` 等待 RIRB 响应的 100000 次 `io_wait` 忙等循环在 codec
无响应时长期占死 CPU（单核）。

**修复**：声明 `extern void task_yield(void);`，循环内每 8192 次迭代
（`(i & 0x1FFF) == 0`）调用一次 `task_yield()`。控制器响应与 CPU 调度无关，
让出不影响 verb 完成，但显著缩短占死窗口。

---

## M12 — HDA BDL 分配失败回滚

**文件**：`kernel/drivers/hda.c`

**问题**：预分配 BDL 页 + 32 个数据页（`HDA_BDL_ENTRIES`）过程中任一
`pmm_alloc_page()` 失败直接 return，已分配的页全部泄漏。

**修复**：失败时回滚——释放 `g_buf_phys[0..i)` 各数据页与 `g_bdl_phys` BDL 页，
`g_bdl_phys` 清零，打印 `[hda] BDL buffer alloc failed, rolled back`。

---

## M13 — FAT32 簇链环检测

**文件**：`user/fs_server.c`

**问题**：`read_dir`/`read_file`/`read_file_at` 跟随 FAT 簇链无步数上限，损坏文件
系统的环簇链导致 fs_server 死循环。

**修复**：新增 `FAT_WALK_LIMIT 0x400000U`（约 400 万步，远超 64MiB 卷簇数）；
三处簇链循环各自加 `hops++ < FAT_WALK_LIMIT` 兜底条件。

---

## M14 — 簇号数据区范围校验

**文件**：`user/fs_server.c`

**问题**：越界簇号（损坏 FS）令 `clus_to_lba()` 算出非法 LBA，误读任意扇区。

**修复**：
- `fat32_mount()` 计算 `g_total_clusters = (tot_sec - g_data_begin) / g_sec_per_clus`
  （`tot_sec` 优先取 BPB offset 32 的 32 位总扇区数，为 0 则回退 offset 19 的 16 位值）；
- 4 处簇使用点（`read_dir` 循环、`read_file` 循环、`read_file_at` 起始簇与主循环）
  校验 `clus - 2 >= g_total_clusters` 即终止/返回 0。`g_total_clusters == 0`
  （异常 BPB）时跳过校验以保底可用。

---

## M15 — 路径解析截断与深度显式报错

**文件**：`user/fs_server.c`

**问题**：`resolve_path()` 组件超过 12 字符时静默截断、深度超过 8 级时静默丢弃
深层组件，两者都可能误匹配到错误文件。

**修复**：
- 组件拷贝上界 `i < 12`；拷满 12 字符仍未到分隔符 → 必然不是合法 8.3 名，
  返回空结果（未找到）；
- 8 级组件解析完后若 `*p != 0`（路径还有剩余）→ 深度超限，显式返回未找到。

---

## M16 — disk_read 应答扇区数校验

**文件**：`user/fs_server.c`

**问题**：`disk_read()` 组装 IPC 请求前不校验 `count`，异常值会使应答 memcpy
越出调用方缓冲。

**修复**：入口检查 `count == 0 || count > DISK_MAX_SECTORS`（=7，见
`include/ipc/disk_proto.h`）即返回 false。接收缓冲区本身定容
`DISK_MAX_SECTORS * SECTOR`。

---

## M17 — u_utoa 输出长度上界

**文件**：`user/lib/suki.c`

**问题**：`u_utoa()` 对输出缓冲无长度约束（接口未带 size 参数），调用方给小缓冲
时越界写。

**修复**：在不改接口的前提下加防御上界——`tmp[24]` 累积循环 `i >= 23` 即 break；
反序拷出循环 `j >= 23` 即补 NUL 提前返回。保证最多写 23 字符 + NUL（uint64 十进制
最长 20 位，正常值不受影响）。

---

## M18 — kprintf 重入深度保护

**文件**：`kernel/console.c`

**问题**：单核下 `irq_save` 已保证单条消息原子，但输出途中若触发 #PF 等异常再次
进入 kprintf（如 panic 路径）会无限递归耗尽内核栈。

**修复**：`static volatile int g_kp_depth`；进入时 `depth >= 4` 直接放弃本次输出
（restore IRQ 后 return），否则 `++`；退出前 `--`。普通嵌套（≤4 层）不受影响，
致命递归被截断。SMP 跨 CPU 串行化留待 P0-3 自旋锁。

---

## 验证

1. **全量编译**：`make iso disk` 通过，无 error/warning（内核 + 全部 Ring3 程序
   + FAT32 磁盘镜像）。
2. **QEMU 冒烟**（`make run-headless`，`-machine pc`，25 秒）：
   - PMM/VMM/kheap 自检通过（`kmalloc works`、`kzalloc zeroed=yes`）；
   - IPC 端口表就绪（`64 slots, well-known 1..8, 0=sentinel`）；
   - HDA codec 枚举正常（M11 让出逻辑不影响 verb 完成）；
   - ATA 识别 131072 扇区（M10 上限生效基准）；
   - fs-server / input-server / shell 三个 Ring3 任务全部拉起（M7 上限 256 内）；
   - FAT32 挂载 + 根目录枚举 6 项全部正确（M13/M14 簇校验未误伤正常卷）；
   - M4 所有权重构后 exec/销毁路径无 panic、无 refcount 告警。
3. 无任何 `error` / `panic` / `CORRUPTION` / `DOUBLE FREE` 输出。

## 遗留事项

- P0-3：SMP 前需引入自旋锁体系（pmm 位图原子性、kprintf 跨 CPU 串行化、端口表锁）。
- M2 计数封顶后未强制回收路径审计；OOL 大规模并发共享场景待压测。
- `u_utoa` 系列接口远期应改为带 size 参数的 `u_utoa_s`。
