# Step 14：M2 回收路径审计收尾 + u_utoa_s 长度安全接口

本批次销账 step13 的两项遗留：
1. M2 计数封顶后的强制回收路径审计 + OOL 大规模共享压测；
2. `u_utoa` 系列改造为带 size 参数的 `u_utoa_s`。

---

## 一、M2 回收路径审计（kernel/mm/pmm.c + include/mm/pmm.h + kernel/kmain.c）

### 审计发现的两处真实缺陷

**缺陷 1 — 封顶后 decref 仍递减（use-after-free 窗口）**

step13 的 M2 修复在 `pmm_incref` 封顶 `0xFFFFFFFF` 后**拒绝递增**。这意味着封顶
时刻起存在"未计入的持有者"——真实持有者数 ≥ 计数值。而 `pmm_decref` 照常递减，
计数会先于真实持有者归零并释放页，其余持有者的映射立即指向已释放内存。

**缺陷 2 — pmm_free_page 无视引用计数**

`pmm_free_page` 直接 `bm_clear + refcount=0`。若任何路径对 refcount>1 的 OOL
共享页误调 `free_page`（而非 `decref`），其它持有者的页被连根释放。

### 修复：饱和粘滞（sticky saturation）+ 共享页守卫

新增常量：

```c
#define PMM_REF_SATURATED  0xFFFFFFFFu
```

**语义不变量**：计数一旦到达 `PMM_REF_SATURATED` 即"永久钉住"（pinned）——
永不递减、永不释放。宁可泄漏 1 页，绝不悬空引用。

- `pmm_incref`：`< SATURATED` 才递增；恰好到顶时打印
  `[pmm] refcount SATURATED at page %lu, pinned forever`。
- `pmm_decref`：入口检测 `== SATURATED` 直接返回 `SATURATED`（不递减）。
  终局释放（ref 1→0）改为直接 `bm_clear + g_used_pages--`，不再经
  `pmm_free_page`（避免与共享页守卫互扰）。
- `pmm_free_page`：`refcount > 1` 时打印
  `[pmm] free_page on SHARED page ... demoted to decref` 并降级为 decref 语义
  （仅减一份引用、页保留；饱和页粘滞不减）。仅 ref≤1 才真正走位图释放。

### 全内核 pmm_free_page 调用点审计结论（共 8 处，全部合法）

| 调用点 | 场景 | ref 状态 | 结论 |
|---|---|---|---|
| `kernel/drivers/hda.c:590,592` | M12 BDL 分配失败回滚 | 独占新页 ref=1 | 合法 |
| `kernel/elf/elf.c:332,351` | 段加载失败回滚 | 独占新页 ref=1 | 合法 |
| `kernel/kmain.c:81,82` | mm 自检 | 独占新页 ref=1 | 合法 |
| `kernel/mm/kmalloc.c:55` | 扩堆失败回滚 | 独占新页 ref=1 | 合法 |
| `kernel/mm/vmm.c:101` | M3 remap 释放旧非 OOL 页 | 独占 ref=1 | 合法 |

共享页（OOL）的释放已全部收敛到 `pmm_decref`（port.c 消费路径 + M4 的
vmm_destroy 统一路径）。新守卫可拦截未来任何误用并留下告警日志。

### 新增审计接口

```c
uint64_t pmm_refcount(void *phys_addr);   /* 读取当前引用计数（自检/审计用） */
```

### OOL 压测自检 pmm_selftest()

置于 `pmm.c` 内部（唯一可直接操纵 `g_refcount` 构造饱和态的位置——真实到达
饱和需 2^32 次 incref，QEMU 下不可行），由 `kmain.c` 的 `mm_selftest()` 挂接，
boot 时执行三组用例：

- **T1 OOL 大规模共享压测**：32 页 × 64 持有者 incref/decref × 8 轮
  （16384 次计数操作），模拟 64 个接收方共享 32 页 OOL 缓冲的极端场景；
  验证每页终态 ref==1 且空闲页守恒。
- **T2 饱和粘滞**：人工置 `SATURATED-1` 后 incref 到顶；验证 decref 不递减、
  `free_page` 不释放（位图仍置位）；最后人工恢复 ref=1 归还页。
- **T3 共享页守卫**：ref=2 的页误调 `pmm_free_page`，验证降级为 decref
  （页保留、ref→1），第二次 free 才真正释放。

失败打印 FAIL（不 panic，保留现场日志），全过则打印：

```
[pmm] refcount selftest (T1 stress/T2 saturate/T3 guard): PASS
```

---

## 二、u_utoa_s 长度安全接口（user/lib/suki.c + suki.h + 5 个调用方文件)

### 新接口

```c
char *u_utoa_s(uint64_t v, char *buf, size_t size);
```

约定：
- `size` 为 buf 总容量（含 NUL）；至多写 `size-1` 字符 + NUL；
- `size == 0` 时不写任何字节（调用方错误但绝不越界）；
- 容量不足时输出截断为高位在前的前缀；uint64 十进制最长 20 位，
  **size ≥ 21 即永不截断**；
- 内部临时缓冲 `tmp[21]` 定容，转换循环 `i < 20` 硬上界。

### 旧接口处置

`u_utoa(v, buf)` 保留为兼容包装 `u_utoa_s(v, buf, 24)`（现有调用方缓冲均为
`char[24]`），头文件注释标注"新代码勿用"。

### 调用点迁移（22 处，全部改为 sizeof(缓冲)）

| 文件 | 处数 | 缓冲 |
|---|---|---|
| `user/apps/hello.c` | 1 | `char buf[24]` |
| `user/apps/playaudio.c` | 13 | `char db[24]` / `char buf[24]` |
| `user/fs_server.c` | 6 | `char n[24]` / `char num[24]` |
| `user/shell.c` | 2 | `char db[24]` |

统一形如 `u_utoa_s(v, db, sizeof(db))`，编译期绑定容量，杜绝手写常数漂移。

---

## 验证

1. **编译**：`make iso disk` 通过（仅存的 `.note.GNU-stack` 链接提示为既有项，
   非本次引入）。
2. **QEMU 冒烟**（`make run-headless`，25 秒）关键输出：

```
[pmm] refcount SATURATED at page 851, pinned forever          <- T2 到顶告警
[pmm] free_page on SHARED page 851 (ref=4294967295), demoted  <- T2 守卫拦截钉住页
[pmm] free_page on SHARED page 851 (ref=2), demoted to decref <- T3 守卫降级
[pmm] refcount selftest (T1 stress/T2 saturate/T3 guard): PASS
[mm] selftest: free pages after = 523405                      <- 与修复前一致，无泄漏
```

   fs_server / shell 正常拉起，FAT32 挂载正常（`u_utoa_s` 迁移后数字输出全部正确：
   `spc=1 fat@32 data@2050 root_clus=2`）。
3. 自检前后空闲页 523406 → 523405，差 1 页为 `vmm_create_address_space` 的
   PML4（与历史基线一致），压测 16384 次计数操作零泄漏。

## 遗留事项

- pmm 计数/位图的原子性（SMP）仍随 P0-3 自旋锁体系销账。
- 饱和钉住页无回收手段（设计使然：宁泄漏不悬空）；若未来出现 SATURATED 告警，
  应视为 incref 泄漏 bug 的信号去修根因，而非解除钉住。
