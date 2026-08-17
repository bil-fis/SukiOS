# Step 11：审计 H1 修复 —— `elf_build_stack()` 参数数组边界加固

> 日期：2026-07-27
> 关联审计项：`results/step10_todos.md` 尾部审计清单 **H1**（标注"立即修"）
> 前置动作：本步开始前已完成 step05~step10 全部工作的 git 提交
> （commit `f1e2e7d`，并调整 `.gitignore` 排除 minimp3 内嵌仓库 / HDA 规格书 /
> 测试 mp3 等大体积非源码文件）。

---

## 1. 问题定义（审计 H1 原文）

> `kernel/elf/elf.c` `elf_build_stack()`：局部数组 `arg_va[64]/env_va[64]` 与
> `EXEC_ARG_MAX=64` 边界差一/容量硬编码，存在越界写内核栈风险。

复核代码后确认的**真实缺陷本质**（比审计原文更准确的表述）：

1. **无入口校验的隐式容量契约**：`elf_build_stack()` 内部用
   `uint64_t env_va[64], arg_va[64]` 定容，但对形参 `argc/envc`
   **没有任何校验**。它的安全性完全依赖"所有调用方都恰好把参数个数限制在
   64 以内"这一隐式约定：
   - `kernel/syscall/syscall.c` 用自己的 `#define EXEC_ARG_MAX 64` 限制；
   - `kernel/sched/sched.c::task_create_user` 传入的是内核固定 argv。

   两处 `64` 是**独立硬编码**，任何一方日后调大（或新增调用方忘记限制），
   `arg_va[i] = ...`（i ≥ 64）即直接越界写内核栈帧——破坏返回地址/相邻局部
   变量，属可利用的内核栈溢出。
2. **字符串区写入无下溢防护（与 H1 同源的边界缺陷，一并修复）**：
   ```c
   sp -= l;                     /* sp 为 uint64_t */
   memcpy(img + sp, envp[i], l);
   ```
   `sp` 初值为栈镜像容量 `cap = stack_pages * PAGE_SIZE`（execve 路径仅
   4 页 = 16 KiB）。argv/envp 字符串总长一旦超过 `cap`，`sp -= l` 发生
   **无符号回绕**成巨大偏移，`img + sp` 直接越界写 kmalloc 堆外内存 →
   内核堆损坏。64 个参数 × 每个最长 512 B（`EXEC_STR_MAX`）理论总量
   32 KiB > 16 KiB，**该路径实际可被用户态触发**。
3. 既有的 `block + 64 > sp` 检查只保护"指针块"阶段，发生在字符串写入
   **之后**，挡不住上述两条路径。

## 2. 修复内容

### 2.1 `include/kernel/elf.h` —— 新增共享常量

```c
#define ELF_ARG_MAX    64
```

配注释说明：`elf_build_stack()` 用它定容并强制校验；`syscall.c` 的
`EXEC_ARG_MAX` 引用它。**"拷贝上限"与"栈构造容量"从此永远一致**，
杜绝两处硬编码漂移。

### 2.2 `kernel/elf/elf.c::elf_build_stack()` —— 三重防护

1. **入口硬校验**（新增，位于任何分配之前）：
   ```c
   if (argc < 0 || argc > ELF_ARG_MAX || envc < 0 || envc > ELF_ARG_MAX) {
       return 0;
   }
   if ((argc > 0 && !argv) || (envc > 0 && !envp)) {
       return 0;
   }
   ```
   越限/负数/矛盾组合（argc>0 但 argv==NULL）一律拒绝，返回 0 由调用方
   走既有回滚路径（`vmm_destroy_address_space` 等）。
2. **局部数组容量改用共享常量**：
   ```c
   uint64_t env_va[ELF_ARG_MAX], arg_va[ELF_ARG_MAX];
   ```
3. **字符串/AT_RANDOM 写入下溢哨兵**（新增 `sp_floor = 64` 保护带）：
   每次 `sp -= l` 前检查 `sp < sp_floor + l`，不足即 `kfree(img); return 0`。
   哨兵值 64 与后续指针块检查 `block + 64 > sp` 的余量一致。
   覆盖三处写入点：envp 字符串循环、argv 字符串循环、AT_RANDOM 16 字节。

### 2.3 `kernel/syscall/syscall.c` —— 消除独立硬编码

```c
#define EXEC_ARG_MAX    ELF_ARG_MAX   /* 与 elf_build_stack 容量共享同一常量（H1） */
```

（该文件已 `#include <kernel/elf.h>`，无需新增头文件依赖。）

## 3. 行为语义变化

| 场景 | 修复前 | 修复后 |
|---|---|---|
| argc/envc ≤ 64、字符串总量正常 | 正常 | 正常（无任何变化） |
| 调用方传 argc/envc > 64 | 内核栈越界写（未定义行为） | `elf_load` 返回 false，exec/spawn 得 -1 |
| argv/envp 字符串总长超出栈镜像 | `sp` 回绕 → 内核堆越界写 | 同上，干净拒绝 |
| argc>0 但 argv==NULL | 解引用 NULL → #PF panic | 同上，干净拒绝 |

## 4. 验证

### 4.1 编译

```bash
make            # 全量编译 + 链接
# ==> Linked build/kernel.elf
# ==> valid Multiboot2 kernel
```

`kernel/elf/elf.c`、`kernel/syscall/syscall.c` 无任何警告
（`-Wall -Wextra`），lint 干净。

### 4.2 QEMU 冒烟回归（修改点在所有用户任务的装载路径上）

```bash
make iso disk
timeout 20 make run-headless QEMU_AUDIODRV=none
```

串口日志确认（`/tmp/h1_smoke.log`）：

- `fs-server`(pid=3)、`input-server`(pid=4)、`shell`(pid=5) 均以
  `(ELF, W^X)` 标记装载成功——三者都经过修改后的 `elf_build_stack()`；
- FAT32 挂载、根目录自检、`SukiOS>` 提示符全部正常；
- 无 #PF / panic / 异常输出。

### 4.3 建议的手工回归（交互路径）

```bash
make run
# shell 中：
exec BIN/hello                      # 无参 exec
exec BIN/playaudio MOONHALO.MP3     # 带参 exec（覆盖 argv 传递链）
```

## 5. 涉及文件

| 文件 | 变更 |
|---|---|
| `include/kernel/elf.h` | 新增 `ELF_ARG_MAX=64` 共享常量及注释 |
| `kernel/elf/elf.c` | `elf_build_stack()` 入口校验 + 数组定容改常量 + `sp_floor` 下溢哨兵×3 |
| `kernel/syscall/syscall.c` | `EXEC_ARG_MAX` 改为引用 `ELF_ARG_MAX` |

## 6. 遗留与关联项

- **H2**（`auxv[16]` 容量硬编码、`na` 无上界校验）：本步未动。当前 `na`
  恒为 8，无实际越界；待扩展 auxv 时按审计要求加 `na < AUXV_MAX` 断言。
- **D2**（exec 传参 argv 错乱，曾现 `file = 3`）：与 H1 不同源——H1 是
  容量边界问题，D2 疑在 shell 参数切分或 `exec_copy_args`/初始栈布局，
  仍待专项排查（建议下一步处理，可用 `exec BIN/playaudio MOONHALO.MP3 bench`
  作为复现用例）。
- H3（`port_allocate` 返回 0 语义混淆）、H4（`g_ool_bump` 无上界）仍标
  "立即修"，见 `results/step10_todos.md`。
