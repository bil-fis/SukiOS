# Step40：libc 引入的堆基址 #PF 回归排查与内核 brk 收缩缺陷修复

## 1. 现象与排查路径（按用户要求"对照 git 上一版本 + 先读 osdev 文档"）

在实现 newlib 风格 libc（`user/lib/*.c`）并接入 `posixtest` 的 `test_libc()` 后，QEMU 启动出现：

```
[pf] user #PF: cr2=0x0000400000000000 write=1 pid=7 'posixtest' rip=0x0000000000402c29 -> killing task
[sched] task 'posixtest' pid=7 exited (code=139)
```

而 `user/posixtest.c` 的 `[libc] printf probe: ...` 已正确输出，`test_process()` 全部 PASS（含 `heap page writable after sbrk`）。崩在 `test_libc` 的字符串/内存段（`strcat` 之后进入 `strdup`→`malloc`）。

### 1.1 对照 git 上一版本（commit 7829b01，step39）

按用户指示，先把 `posixtest.c`、`Makefile` 还原为 `git HEAD` 版本（仅 `suki.c.o`，不含 `test_libc` 与我的 libc）重新构建运行，得到：

```
[PASS] brk query nonzero
[PASS] sbrk(4096) returns old brk
[PASS] brk advanced by 4096
[PASS] heap page writable after sbrk
[PASS] brk restored
=== POSIX test summary: PASS=106 FAIL=0 ===
ALL POSIX TESTS PASSED
```

**结论**：系统在"实现 libc 之前"确实完全正常（106/106，含对堆页的真实写访问 `*hp = 0x5A`）。因此崩溃确实由我新增的 libc 代码路径首次触发，但根因不一定在 libc 本身——需进一步定位。

### 1.2 阅读本地 osdev 文档

- `osdev_wiki/wiki.osdev.org/System_Calls`：明确 64-bit syscall 内核入口须 `swapgs` 保存用户 RSP，返回用户态前须保存/恢复**所有**被调用者/调用者寄存器（rdi/rsi/rdx/r8/r9/rcx/r10/r11/rbx 等），否则用户态状态被踩。
  → 据此核对 `kernel/arch/x86_64/syscall_entry.S::syscall_return_path`：它完整恢复了 `rbx/rbp/r12-r15/rdi/rsi/rdx/r10/r8/r9`，内核返回路径正确，**排除"内核破坏用户寄存器"假设**。
- `osdev_wiki/wiki.osdev.org/Calling_Conventions`：System V AMD64 调用约定下 rdi/rsi/rdx 是 scratch（调用者保存），leaf 函数可用 `rsp` 下方 128 字节 red zone；call 时须 16 字节栈对齐。
  → 据此核对 `user/lib/suki.h` 的 `suki_syscall5`/`__suki_syscall3` clobber 列表缺 `"rdi","rsi","rdx"`，但内核已正确还原这些寄存器，故非崩溃根因（仅潜在优化隐患，已在文档中标注，未改以免引入新风险）。

### 1.3 定位崩溃精确地址

`cr2=0x0000400000000000` 是**写访问**。该地址即 `POSIX_HEAP_BASE`（见下）。崩在 `test_libc` 中第一次真正通过 `malloc` 触碰堆基址的 `strdup` 调用。

## 2. 根因（两步，层次清晰）

### 2.1 内核 `sys_brk` 收缩/增长不对称（真正的缺陷）

`kernel/syscall/sys_posix.c::sys_brk`（约 368 行）：

```c
/* 首次调用：登记整块堆区 VMA */
if (t->brk_start == 0) {
    t->brk_start = POSIX_HEAP_BASE;          // = 0x0000400000000000
    t->brk       = POSIX_HEAP_BASE;
    vma_insert(t, POSIX_HEAP_BASE, POSIX_HEAP_BASE + POSIX_HEAP_MAX, ...);
}
...
if (addr > t->brk) {
    t->brk = addr;                          // 增长：只动断点，VMA 不动
} else if (addr < t->brk) {
    /* 收缩：vma_unmap_range 把 [页对齐(addr), 页对齐(brk)) 从 VMA 永久裁掉 */
    vma_unmap_range(t, new_end, old_end);
    t->brk = addr;
}
```

`POSIX_HEAP_BASE = 0x0000400000000000` 落在用户空间（`USER_SPACE_TOP = 0x00007FFFFFFFFFFF`，48-bit canonical 上限 128TB，4TB 合法），VMA 一次性登记 `[base, base+64MB)`。

问题：**收缩用 `vma_unmap_range` 把那段区间从 VMA 链表永久移除，但增长分支只改 `t->brk`、并不重建被裁掉的 VMA 子区间**。于是：

1. `test_process` 调 `SYS_SBRK(4096)` 增长 → 写 `b0+100` 成功（`heap page writable after sbrk` PASS）。
2. 随后 `SYS_SBRK(-4096)` 缩回到 base → `vma_unmap_range(t, base, base+PAGE)` 把**含堆基址 base 的那一页**移出 VMA。
3. 此后 `base` 这一页**已脱离 VMA**，但 `t->brk` 又回到 `base`（逻辑上"断点在 base"，物理上该页已被 unmap 且 VMA 缺口未补回）。
4. 任何访问 `base`（含写 `g_heap_base->total`）都触 `#PF`，内核 `vma_populate` 找不到覆盖 `base` 的 VMA → 判定非法 → 杀进程。

原本的 `test_process` 在 `sbrk` 缩回后**不再访问堆**，故缺陷一直潜伏未被触发；我的 `malloc` 是首个在"brk 已缩回 base"之后再次访问 `base` 的代码，从而暴露。

### 2.2 libc `heap_init` 假设堆基址页始终映射（触发点）

`user/lib/stdlib.c::heap_init`（修复前）：

```c
uintptr_t brk = (uintptr_t)suki_syscall1(SYS_BRK, 0);
g_heap_base = (block_t *)brk;
g_heap_base->total = BLK_HDR;   // 写 base 偏移 0 —— 此时 base 页已脱离 VMA
```

它直接写堆基址页，未先确保该页在 VMA 内可映射，因此踩中 2.1 的缺口。

## 3. 修复（两处，均符合"真正实现、不 stub、不破坏既有权衡"）

### 3.1 内核 `sys_brk` 收缩改为"只动断点、不裁剪 VMA"

`kernel/syscall/sys_posix.c`（sys_brk 收缩分支）改为：

```c
} else if (addr < t->brk) {
    /* 收缩：仅移动断点，不裁剪 VMA、不回收物理页。
     *
     * 原实现对 [页对齐上界(addr), 页对齐上界(brk)) 调 vma_unmap_range，
     * 把该区间从 VMA 链表永久裁掉；而增长分支只更新 t->brk、不重建被裁
     * 子区间，导致「先缩回、再增长到已裁区间」后该区间脱离 VMA，用户态
     * 访问即 #PF 杀进程。
     *
     * POSIX 仅要求 brk 断点可移动，是否立即回收物理页是实现细节；本系统
     * 选择「惰性保留」：VMA 首次 sys_brk(0) 时已一次性登记整块堆区，其后
     * 收缩/增长都只动断点，VMA 始终完整覆盖 [base, base+MAX)，用户态补页
     * （demand zero-fill）始终有效。物理页在进程退出时由 vma_destroy_all
     * 统一回收，无泄漏。 */
    t->brk = addr;
}
```

**为什么这是对的不破坏原本行为**：原本 `test_process` 的 brk 测试只检查断点移动与差值，`vma_unmap_range` 的"物理页回收"对该测试无可见影响；去掉裁剪后，brk 语义（断点位置、返回值、差值）完全不变，故而原本 5 项 brk 测试仍 PASS，且新增"缩回后再增长访问"也合法。

### 3.2 libc `heap_init` 先扩展一页确保基址页可映射

`user/lib/stdlib.c`：

- 新增 `#define HEAP_PAGE 4096UL`（freestanding 用户态无 `PAGE_SIZE`）。
- `heap_init` 改为先 `sys_brk(base + HEAP_PAGE)`，确保 `[base, base+HEAP_PAGE)` 处于 VMA 内并允许首次触碰补页，再写块头：

```c
static void heap_init(void)
{
    uintptr_t base = (uintptr_t)suki_syscall1(SYS_BRK, 0);
    if (!base) return;
    uintptr_t first_top = (base + HEAP_PAGE);
    uintptr_t got = (uintptr_t)suki_syscall1(SYS_BRK, first_top);
    if (got != first_top) return;          /* OOM：堆不可用 */
    g_heap_base = (block_t *)base;
    g_heap_base->total = (size_t)(first_top - base);   /* 首块覆盖整页 */
    g_heap_base->free  = 1;
    g_heap_base->next  = NULL;
    g_free_head = g_heap_base;
    g_heap_init = 1;
}
```

首个空闲块覆盖 `[base, base+HEAP_PAGE)`，后续 `malloc` 的 first-fit 切分、`free` 的线性相邻合并、`heap_extend` 基于 `sys_brk(0)` 推进断点的逻辑整体自洽（均按 `g_heap_base..brk` 区间扫描，块头 24 字节 = `sizeof(block_t)`，分配器内部一致）。

## 4. 验证（QEMU 生产场景，零 panic）

构建命令（遵守项目规则，不 source makeenv.sh，用 `make iso`/`make disk`）：

```bash
cd /mnt/d/Projects/SukiOS
make clean && make iso && make disk
qemu-system-x86_64 -machine pc -cpu qemu64 -smp 1 -m 512 \
  -drive file=build/SukiOS.iso,format=raw,if=ide,media=cdrom,index=1 -boot d \
  -drive file=build/disk.img,format=raw,index=0,media=disk \
  -display none -serial file:/tmp/suki.fix2.log -no-shutdown
```

实测结果（节选）：

```
[PASS] brk query nonzero
[PASS] sbrk(4096) returns old brk
[PASS] brk advanced by 4096
[PASS] heap page writable after sbrk
[PASS] brk restored
...
[libc] printf probe: 42 42 dead ok Z
[PASS] printf returns >0 byte count
[PASS] strlen("hello")=5
[PASS] strcmp equal / less
[PASS] strcpy / strcat / strdup / strchr / strstr
[PASS] malloc returns non-null
[PASS] malloc 2nd block / 3rd small block / small block writable
[PASS] malloc after free non-null
[PASS] calloc zeroed
[PASS] realloc grows
[PASS] libc open(/README.TXT) / read / close
[PASS] libc open create / write / write-read roundtrip
[PASS] libc getpid > 0
[PASS] libc waitpid reaps child
...
=== POSIX test summary: PASS=132 FAIL=0 ===
ALL POSIX TESTS PASSED
[syscall] task 'posixtest' pid=7 exit(code=0)
```

对照：

- 修复前（git HEAD 代码 + libc）：`test_process` 106 PASS，但 `test_libc` 触 `base` 写 #PF，进程 code=139。
- 修复后：整体 132 PASS（原 106 + 新增 libc 26 项断言），**零 #PF、零 panic、零 killing**。
- 服务进程：FS_SERVER 挂载 FAT32、shell、INPUT_SERVER 均正常上线，`[boot] all services spawned; system fully up`，无 triple fault。

附带验证 `git HEAD` 纯原本版本在修复内核后仍 106 PASS（内核改动向后兼容，未破坏既有权衡）。

## 5. 涉及文件

| 文件 | 改动 |
|------|------|
| `kernel/syscall/sys_posix.c` | `sys_brk` 收缩分支去掉 `vma_unmap_range`，改为只移动断点（惰性保留堆页，修复增长不可恢复 VMA 的缺陷） |
| `user/lib/stdlib.c` | 新增 `HEAP_PAGE`；`heap_init` 先 `sys_brk` 扩展一页确保基址页在 VMA 内，再初始化首空闲块 |
| `user/posixtest.c` | `test_libc()` 中清理排查期临时调试打印（`about-to-ck1`/`after-ck1`/`r1`/`r2` 等），保留有效断言与 `printf probe` 诊断 |

## 6. 后续注意（已记录，未改以免引入风险）

`user/lib/suki.h` 的 `suki_syscall5`/`__suki_syscall3` 的 clobber 列表缺少 `"rdi","rsi","rdx"`。内核返回已正确还原这些寄存器，故当前非崩溃根因；但属潜在优化隐患，后续应在确认不影响既有 106 项测试后再补全 clobber 列表。

## 7. 提交

修复完成后执行 `git commit`（按项目约定：简短英文前缀 + 中文描述，不 `git push`）。
