# Step 18：P0-5 虚拟内存进阶——VMA + 按需分页 + COW + mmap/munmap

> 对应 `results/step10_todos.md` P0-5 里程碑。上一批（step17）完成 P0-3 SMP。
> 本批实现地址空间对象化（VMA）、demand zero-fill 按需分页、写时复制
> （COW）fork 地基、`sys_mmap`/`sys_munmap` 系统调用，并以内核自检 +
> Ring3 端到端两级验证收口。

---

## 1. 总览：实现了什么

| 子项 | 状态 | 说明 |
|---|---|---|
| VMA 链表（vm_area_t） | ✅ | 每任务按地址升序单链表，登记合法区间+权限+类型 |
| 按需分页（demand zero-fill） | ✅ | #PF 时查 VMA 命中即补零页，iretq 原地重试 |
| COW（写时复制） | ✅ | `vmm_fork_cow` 共享用户半区 + `vmm_cow_break` 写断开 |
| mmap/munmap syscall | ✅ | SYS_MMAP=14 / SYS_MUNMAP=15，匿名映射 W^X |
| 用户栈自动增长区 | ✅ | 已映射栈之下 128 页按需区（spawn 与 execve 两路径） |
| copy_*_user 与按需页协同 | ✅ | 预校验未映射页时代替用户补页 / COW 断开 |
| 内核自检 + Ring3 端到端验证 | ✅ | `vma_selftest` T1-T4 PASS；HELLO app mmap 测试 PASS |

文件清单：
- 新增 `include/mm/vma.h` — VMA 类型/接口/常量
- 新增 `kernel/mm/vma.c` — VMA 管理 + 按需填充 + 自检
- 修改 `include/mm/vmm.h` — `PTE_COW` 位、`vmm_fork_cow`/`vmm_cow_break` 声明
- 修改 `kernel/mm/vmm.c` — COW 实现（`pte_slot`/`vmm_fork_cow`/`vmm_cow_break`）
- 修改 `include/kernel/task.h` — `task_t.vma_list` 字段
- 修改 `include/kernel/syscall.h` — `SYS_MMAP`/`SYS_MUNMAP`，`SYSCALL_MAX=16`
- 修改 `kernel/syscall/syscall.c` — `sys_mmap`/`sys_munmap`、`user_access_ok`
  按需补页协同、execve 的 VMA 清理与新栈增长区登记
- 修改 `kernel/arch/x86_64/idt.c` — #PF 处理器先走 `vma_populate` 救援
- 修改 `kernel/sched/sched.c` — spawn 登记栈增长 VMA、exit 清理 VMA
- 修改 `kernel/kmain.c` — 挂接 `vma_selftest()`
- 修改 `user/lib/suki.h` — `sys_mmap`/`sys_munmap` 用户封装
- 修改 `user/apps/hello.c` — Ring3 端到端 mmap 按需分页测试

---

## 2. 数据结构与关键常量

### 2.1 vm_area_t（include/mm/vma.h）

```c
typedef struct vm_area {
    uint64_t start;   /* 页对齐，含 */
    uint64_t end;     /* 页对齐，不含 */
    uint64_t prot;    /* 补页附加 PTE 位：PTE_WRITE|PTE_NX 组合 */
    uint32_t type;    /* VMA_TYPE_ANON=1 / VMA_TYPE_STACK=2 */
    struct vm_area *next;   /* 按 start 升序 */
} vm_area_t;
```

- 挂在 `task_t.vma_list`（`void*`，避免 task.h 引入 mm 头依赖环）。
- 全局一把 `g_vma_lock`（ticket 自旋锁，irqsave），锁序位于 kmalloc 外层：
  `vma -> kmalloc -> pmm -> console`，无 ABBA 环。

### 2.2 地址常量

| 常量 | 值 | 说明 |
|---|---|---|
| `VMA_MMAP_BASE` | `0x0000500000000000` | mmap 匿名区起点（避开 OOL 窗口 0x600000000000） |
| `VMA_MMAP_TOP` | `0x0000580000000000` | mmap 区上限 |
| `VMA_MMAP_MAX` | 64 MB | 单次 mmap 上限（防恶意耗尽） |
| `VMA_STACK_GROW_PAGES` | 128（512KB） | 栈自动增长按需区 |
| `PTE_COW` | bit 10（软件位） | 写时复制标记（与 PTE_OOL=bit9 相邻） |

### 2.3 系统调用

| 调用号 | 名称 | 签名 | 语义 |
|---|---|---|---|
| 14 | SYS_MMAP | `mmap(len, prot)` | 仅登记 VMA 不给物理页；prot bit0=可写；恒 NX（W^X 红线）；返回基址或 0 |
| 15 | SYS_MUNMAP | `munmap(addr, len)` | 限 mmap 区间；解除映射+释放已填充页+VMA 拆分；0=成功 |

---

## 3. 算法与关键路径

### 3.1 缺页救援总入口 `vma_populate(t, addr, write)`

```
#PF (用户态) / user_access_ok 预校验失败
        │
        ▼
  PTE 已存在？
   ├─ 是：write 且 !PTE_WRITE 且 PTE_COW → vmm_cow_break()（写时复制断开）
   │       其余（NX 取指/真只读写/reserved）→ false → 杀任务
   └─ 否：vma_find(addr) 命中？
           ├─ 否 → false → 杀任务（维持原故障隔离语义）
           ├─ 是但 write 且区间只读 → false
           └─ 是 → pmm_alloc_page()（已清零）→ vmm_map_page(PRESENT|USER|prot)
                   → true → iretq 原地重试故障指令（用户完全无感）
```

### 3.2 COW fork（`vmm_fork_cow(src_pml4, dst_pml4)`）

遍历源 PML4 用户半区（表项 0..255）全部 4KB 叶页：
- **可写页**：源 PTE 改 `只读+PTE_COW` 并 `invlpg`；目标建同映射；`pmm_incref`。
- **只读页**：直接共享（+1 引用），无需 COW 位。
- **PTE_OOL 页**：跳过（OOL 生命周期归 IPC 引用计数，等价 XNU
  `VM_INHERIT_NONE`）。

### 3.3 COW 断开（`vmm_cow_break(pml4, virt)`）

- 引用计数 >1：`pmm_alloc_page` 新页 + `memcpy`（经 PHYS_TO_VIRT 内核直映）+
  改 PTE 为可写、清 COW、`invlpg`、旧页 `pmm_decref`。
- 引用计数 ==1：最后持有者零拷贝快路径——原页直接改回可写。

### 3.4 munmap 的 VMA 拆分（`vma_unmap_range`）

四种相交关系全支持：**完全覆盖**（整段移除）/**切头**/**切尾**/**中间挖洞**
（kmalloc 新节点拆成两段）。随后逐页 `vmm_unmap_page` + `pmm_decref`
（共享页引用计数守卫，不会误伤他空间）。

### 3.5 mmap 选址（`vma_find_free`）

升序链表单趟首次适配（first-fit）：候选点被某 VMA 相交则推进到其 `end`，
链表已升序故 O(n) 一趟即得，超 `VMA_MMAP_TOP` 返回 0。

### 3.6 copy_*_user 协同（syscall.c::user_access_ok）

逐页校验时：PTE 不存在 → 代用户 `vma_populate`（等价用户自己触发 #PF）；
只读 PTE 带 COW 位且需写 → 代用户 COW 断开。失败才判 EFAULT。
这保证「用户把尚未触碰的 mmap 缓冲区直接传给 syscall」也能工作。

### 3.7 栈自动增长

- `task_create_user_args`（spawn 路径）：已映射 `USER_STACK_PAGES=32` 页之下
  再登记 128 页 `VMA_TYPE_STACK` 按需区。
- `sys_execve`：同理（该路径立即映射 4 页），且切空间前 `vma_destroy_all`
  作废旧映像 VMA。
- `task_exit_current`：`vma_destroy_all` 回收链表元数据（物理页归
  `vmm_destroy_address_space` 统一回收）。

---

## 4. 验证方式与结果

### 4.1 内核自检 `vma_selftest()`（kmain::mm_selftest 挂接）

| 用例 | 内容 | 结果 |
|---|---|---|
| T1 | VMA 插入/查找/重叠拒绝/end 边界不含 | PASS |
| T2 | demand 填充：未映射页补零页可写；VMA 外不可救 | PASS |
| T3 | COW：fork 后双方只读+COW+同物理页+ref=2 → 子写断开内容独立、父页不变、ref 回归 → 父最后持有者零拷贝断开 | PASS |
| T4 | munmap 中间挖洞拆分 + 页回收 | PASS |
| 泄漏 | 全程 `pmm_free_pages()` 前后一致 | PASS |

串口输出：`[vma] selftest (T1 vma/T2 demand/T3 COW/T4 munmap): PASS`

### 4.2 Ring3 端到端（user/apps/hello.c）

`exec bin/hello`：`sys_mmap(64KB, WRITE)` → 逐页读（验证零页）→ 逐页写
（**真实 #PF** → `page_fault_handler` → `vma_populate` → iretq 重试）→
回读校验 → `sys_munmap`。

串口输出：`mmap demand-paging test: PASS`，进程正常 exit(0)。

### 4.3 回归

- `make iso disk` 零 error；QEMU `-smp 4`（KVM 与 TCG 双跑）完整启动；
- SMP：`[smp] 4/4 CPUs online` + `IPI selftest: 3/3 APs acked`（step17 不回归）；
- 全部既有自检（pmm/heap/IPC/FAT32/shell）不回归。

### 4.4 QEMU 调试命令

```bash
make iso disk && make run-headless          # 看 [vma] selftest PASS
# GDB 断点观察按需补页：
qemu-system-x86_64 -s -S -machine pc -cpu qemu64 -smp 4 -m 2G \
  -cdrom build/SukiOS.iso -drive file=build/disk.img,format=raw
gdb build/kernel.elf -ex 'target remote :1234' \
    -ex 'b vma_populate' -ex 'b vmm_cow_break' -ex c
```

---

## 5. 设计边界（诚实声明）

- **文件映射（VMA_TYPE_FILE）未实现**：接口位预留，等 P1-4 页缓存就绪接入。
- **完整 fork() syscall 未落地**：`vmm_fork_cow` 是地址空间层地基（自检全链
  路验证），复制内核栈/寄存器上下文的 fork 语义随 P1-5 交付。
- **ELF 段仍立即装载**：内容页必须拷贝，按需化收益低，维持现状。
- **mmap 恒 NX**：不提供 PROT_EXEC（W^X 红线），JIT 需求日后单独评估。
