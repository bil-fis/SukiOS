# Step 67：R_X86_64_COPY 拷贝重定位 + dlclose 物理页回收 + 内存回收机制完善

> 目标（用户原话）：实现 R_X86_64_COPY 与 dlclose 物理页回收，并完善内存回收机制。
> 可参考 osdev wiki 与 Linux 实现。

本步骤在 step66（内核态动态链接引擎 + dlopen/dlsym/dlclose 系统调用）的基础上，
补齐了**拷贝重定位**这一动态链接标准能力，并把**进程退出 / dlclose 的资源回收**做成
真正可工作、无泄漏、无内核堆损坏的实现。最终在 QEMU 生产场景回归中
`dltest PASS`、`POSIX test summary: PASS=192 FAIL=0`、零 panic、零 kheap 损坏。

---

## 1. 实现概览

| 能力 | 状态 | 关键文件 |
|------|------|----------|
| R_X86_64_COPY 拷贝重定位 | ✅ 已实现并验证（sl_const=42 拷入主程序 .bss 副本） | `kernel/elf/elf.c` `elf_apply_reloc` |
| dlclose 真正物理页回收（用户页解映射 + 物理页 refcount 释放 + 内核 ELF 副本 kfree） | ✅ 已实现并验证（reclaim 日志 + 槽位复用） | `kernel/elf/elf.c` `elf_unmap_module` / `elf_dlclose` |
| 进程退出清理内核 ELF 副本（修复内核堆泄漏） | ✅ 已实现（新增 `img_owned` 守卫，杜绝 kfree 静态 blob） | `kernel/elf/elf.c` `elf_free_modules`、`include/kernel/elf.h` |
| 修复动态链接器「写用户内存」的页内偏移双重计算 bug | ✅ 根因修复（影响所有用户态写） | `kernel/elf/elf.c` `elf_write_user` / `elf_copy_user` |

附带修复了一个**会损坏内核堆、并连带污染其它子系统的严重 bug**（见 §4），
修复后原先 flaky 的 `mprotect: read still works` 测试也转为通过。

---

## 2. R_X86_64_COPY（拷贝重定位）

### 2.1 背景（osdev / Linux 权威行为）

当主程序（ET_EXEC 或 PIE 主程序）以 `-Bdynamic -lxxx` 链接某个共享库，并引用库中
**全局变量**时，链接器无法在静态链接期为该符号确定地址（它在 .so 内），于是生成一条
`R_X86_64_COPY`（type=5）重定位：

- 链接器在主程序的 `.dynbss`（或 `.bss`）中分配一块「本地副本」槽位，并把该重定位项
  的 `r_offset` 指向这块副本；
- 运行时链接器需在**其它已加载模块**（共享库）中找到该符号的「定义」，把其初始值
  **复制**进主程序的本地副本；
- 此后主程序通过该本地副本访问变量（地址无关、避免跨模块数据竞争）。

关键点（踩坑点）：
- COPY 重定位**必须取自「定义方」符号的 `st_size`** 作为拷贝大小；主模块自身的 UNDEF
  引用符号 `st_size == 0`，若用它作大小会误判「大小未知」而丢弃该重定位。
- COPY 的源**必须来自其它模块**（共享库），**不能**用当前模块自身的同名符号——
  当前模块自身的同名符号只是「目标副本占位符」（`st_shndx != UNDEF`、`st_value` 指向
  `.bss` 副本），以它作源会复制未初始化的 0。

### 2.2 实现（`kernel/elf/elf.c :: elf_apply_reloc`）

```c
if (type == R_X86_64_COPY) {
    if (symidx == 0) return false;                 /* COPY 必须绑定符号 */
    elf64_sym_t *sym = &mod->symtab[symidx];
    const char *nm = mod->strtab + sym->st_name;
    uint64_t src = 0; size_t sz = 0;
    for (int k = 0; k < n; k++) {
        elf_module_t *dm = &mods[k];
        if (dm == mod) continue;                  /* 源必须来自其它模块（共享库）*/
        if (!dm->resident || !dm->symtab || !dm->strtab) continue;
        for (uint32_t s = 1; s < dm->symcount; s++) {
            elf64_sym_t *ds = &dm->symtab[s];
            if (ds->st_shndx == SHN_UNDEF) continue;  /* 只取真正定义 */
            if (ds->st_name && elf_sym_name_eq(dm, ds->st_name, nm)) {
                src = dm->base + ds->st_value;     /* 定义方运行时地址 */
                sz  = (size_t)ds->st_size;         /* 大小取自定义方 */
                break;
            }
        }
        if (src) break;
    }
    if (src == 0 || sz == 0) return false;
    elf_copy_user(as, addr, src, sz);             /* 跨模块拷贝（见 §4 修复）*/
    return true;
}
```

`addr = mod->base + r->r_offset`：
- 对 ET_EXEC 主程序 `base == 0`，`r_offset` 本身即绝对虚拟地址（如 `0x523d60`）；
- 对 ET_DYN 库 `base` 为随机加载基址，`r_offset` 为相对偏移；
两者统一成立（见 `elf_load` 中 `base = (ET_DYN)? elf_random_base() : 0`）。

### 2.3 验证证据（QEMU serial `/tmp/dltestK.log`）

```
[COPY] sl_const(主程序副本) = 42  (期望 42)
[OK] 加载期库: sl_add(2,3)=47  sl_const(库)=42  sl_sub(9,4)=5  sl_name()=libtest
```
`sl_const(库)=42` 来自 `dlsym(h1,"sl_const")` 读共享库定义；`sl_add(2,3)=47` = 2+3+42
证明 GLOB_DAT 跨段重定位也正确。

---

## 3. dlclose 物理页回收 + 内存回收机制

### 3.1 回收路径（`elf_dlclose`，引用计数归零后真正回收）

```c
int elf_dlclose(struct task *t, int h) {
    ...
    if (m->refcount > 0) { m->refcount--; }
    if (m->refcount > 0) return 0;     /* 仍被其它引用持有（DT_NEEDED 或重复 dlopen）*/
    kprintf("[elf] dlclose: reclaim module '%s' base=%p img=%p\n", ...);
    elf_unmap_module(t->cr3, m);       /* ① 解映射用户页 + pmm_decref 物理页 */
    if (m->img && m->img_owned) {      /* ② 释放内核侧 ELF 副本（kmalloc）*/
        kfree(m->img); m->img = NULL;
    }
    m->img_owned = 0;
    m->symtab = m->strtab = m->dynamic = m->rela = m->rela_plt = NULL;
    m->symcount = m->relacount = m->relapltcount = 0;
    m->resident = 0; m->name[0] = '\0';
    /* nmodules 不变：槽位留作空闲可复用，句柄 h 此后失效 */
    return 0;
}
```

**物理页释放采用 `pmm_decref` 而非 `pmm_free_page`**：正确应对共享页（RO 数据可能被多个
映射共享引用计数），与 `vmm_destroy_address_space` 一致，避免把仍被引用的页错误归还。

`elf_unmap_module` 从模块自身 ELF 副本（`mod->img`）重新解析 PT_LOAD，按页
`vmm_unmap_page` + `pmm_decref` 回收用户物理页。

### 3.2 槽位复用 / 去重（`elf_dlopen`）

- **同名去重**：`dlopen("/LIB/libtest.sl")` 若已加载则 `refcount++` 返回既有句柄；
- **空闲槽复用**：卸载后该槽 `resident=0`，再次 `dlopen` 优先复用该槽（`nmodules` 不变，
  句柄表不无限增长）；
- **失败回退**：加载/重定位失败清理 `resident` 与 `img`，`nmodules` 回退。

### 3.3 进程退出清理（`elf_free_modules`，修复内核堆泄漏）

原实现在 `task_exit_current` 中只 `vmm_destroy_address_space` 回收用户物理页，
但每个 `dlopen` 的 `.sl` 内核 ELF 副本（`exec_read_file` 的 kmalloc 缓冲）随进程
生命周期持续泄漏。`elf_free_modules` 在 `vmm_destroy_address_space` 之后、`t->cr3=0`
之前释放各模块 `img`：

```c
void elf_free_modules(struct task *t) {
    for (int i = 0; i < t->nmodules; i++) {
        elf_module_t *m = &t->modules[i];
        if (m->img && m->img_owned) kfree(m->img);  /* 仅释放 kmalloc 副本 */
        m->img = NULL; m->img_owned = 0; m->resident = 0;
    }
    t->nmodules = 0;
}
```

**关键修复（§4 同根因）**：主程序的 `img` 是内核内嵌的**静态 blob**（`user_dltest_start`
等，非 kmalloc），绝不可 `kfree`，否则损坏内核堆。新增 `img_owned` 字段：
- dlopen / DT_NEEDED 依赖经 `exec_read_file` 分配 → `img_owned = 1`（退出时释放）；
- 主程序内嵌 blob → `img_owned = 0`（永不释放）。

调用点（`kernel/sched/sched.c :: task_exit_current`）：
```c
vmm_destroy_address_space(t->cr3);   /* 回收用户物理页/页表 */
...
elf_free_modules(t);                 /* 释放各模块 ELF 副本缓冲（kmalloc）*/
t->cr3 = 0;
```

### 3.4 验证证据

```
[elf] dlclose: reclaim module 'libtest2.sl' base=0x000060a3e62c0000 img=0xffffc00000c6e030
[INFO] dlclose(libtest2) 已调用，内核应打印 reclaim 日志并回收物理页
[OK] 回收后复用: sl_add(1,2)=45          // 1+2+42=45，证明槽位已回收可复用
[elf] dlclose: reclaim module 'libtest2.sl' base=0x000060800a8a0000 img=0xffffc00000c6d060
=== dltest PASS ===
```
`kheap CORRUPTION` 计数从之前的非零降为 **0**。

---

## 4. 根因修复：动态链接器「写用户内存」的页内偏移双重计算

### 4.1 现象

COPY 重定位无论如何都写不进主程序 `.bss`（`sl_const` 始终为 0），但 `vmm_translate` 回读
目标帧内容却正常。经内核侧逐字节哨兵写 + 回读定位，发现**写目标落在了错误的物理地址**。

### 4.2 根因

`vmm_translate(as, va)` 的返回值**已经包含页内偏移**：
```c
return (pt[i1] & PTE_ADDR_MASK) + (virt & 0xFFF);   /* 页基址 + 页内偏移 */
```
但 `elf_write_user` / `elf_copy_user` 又额外加了一次 `(va & (PAGE_SIZE-1))`：
```c
uint64_t phys = vmm_translate(as, va);          /* 已是 页基址+offset，如 0x88ed60 */
uint8_t *kva = PHYS_TO_VIRT(phys);
memcpy(kva + (va & 0xfff), s, chunk);          /* 错！再 +offset → 0x88ed60+0x360 写到了 0x88e0c0 */
```
导致实际写入地址是 `页基址 + 2×页内偏移`，而程序读取的正确地址是 `页基址 + 1×页内偏移`
（由 MMU 经 PTE 翻译得到），二者错位 → 程序永远读到未写入的 0。

该 bug 影响**动态链接器所有对用户内存的写**：
- COPY 重定位（数据复制）——直接暴露；
- GLOB_DAT / JUMP_SLOT / RELATIVE 等若需写回用户 GOT/PLT，同样错位（之前因
  `dlsym` 直接 `m->base + st_value` 计算地址、不经 GOT 写回，故 `dlopen` 路径看似可用，
  掩盖了该 bug）。

### 4.3 修复

`vmm_translate` 返回已含偏移，`PHYS_TO_VIRT(phys)` 即定位到目标字节，**禁止再叠加偏移**：

```c
/* elf_write_user */
uint64_t phys = vmm_translate(as, va);          /* 已含页内偏移 */
uint8_t *kva = (uint8_t *)PHYS_TO_VIRT(phys);  /* 已定位到目标字节 */
size_t chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));
memcpy(kva, s, chunk);                          /* 不再 +page_off */

/* elf_copy_user */
uint64_t sphys = vmm_translate(as, src);
const uint8_t *sp = (const uint8_t *)PHYS_TO_VIRT(sphys);  /* 不再 +offset */
... memcpy(buf, sp, chunk); elf_write_user(as, dst, buf, chunk);
```

这是一处影响面极广但隐蔽的基础 bug，修复后所有用户态写路径才真正正确。

### 4.4 旁证：连带修复了 flaky 的 mprotect 测试

之前 `POSIX test summary: PASS=191 FAIL=1`（`[FAIL] mprotect: read still works`）其实是
§3.3 中「`kfree` 静态 blob」导致的**内核堆损坏**污染了相邻内存（页表/堆元数据）的继发表现。
修复 `img_owned` 后堆不再损坏，该测试恢复为 `PASS=192 FAIL=0`。

---

## 5. 工具链与链接脚本改动

### 5.1 工具链（`Makefile`）

- 新增 `USER_LD` 变量（`x86_64-sukios-elf-ld` / `x86_64-elf-ld` / `ld`）。
- `LIB_SL_CFLAGS` 去掉 `-fPIE`，改纯 `-fPIC`：否则共享对象内 PC32 重定位无法用 `-shared`
  解析（`relocation R_X86_64_PC32 ... recompile with -fPIC`）。
- `LIBTEST_SL` 规则改为 `$(USER_LD) -shared -export-dynamic --no-warn-rwx-segments
  --no-dynamic-linker -o $@ $<`：裸机 `gcc -shared` 会退化成 ET_EXEC，必须用 `ld -shared`
  直接产出真正的 ET_DYN 共享对象（dynsym 含 `sl_add/sl_sub/sl_const/sl_name`）。
- `dltest.elf` 链接改为动态链接：
  `$(USER_CC) -nostdlib -no-pie -fno-pic -Wl,--build-id=none -Wl,--no-warn-rwx-segments
   -Wl,--no-dynamic-linker -Wl,-Bdynamic -Lbuild -l:libtest.sl -T user/user.ld ... -lgcc`
  生成 `DT_NEEDED=libtest.sl` + `R_X86_64_COPY`（对 `sl_const` 的引用）。
- disk 安装增加 `mcopy -i $@ $(LIBTEST_SL) ::LIB/libtest2.sl`（纯运行期 dlopen 回收测试用）。

### 5.2 用户链接脚本（`user/user.ld`）

新增 `.dynamic`、`.got`（含 `.got.plt`）、`.data.rel.ro` 段（位于 `.bss` 之后、`/DISCARD/`
之前），供动态链接可执行文件放置动态段与全局偏移表，使 `-Bdynamic` 链接的 ET_EXEC
能正确生成 COPY 重定位。

---

## 6. 验证方式（bash + QEMU，符合内存铁律）

> 严禁 GDB / 外部脚本编排。仅用 bash + QEMU 自带机制：`make run-headless` 后台启动，
> 用 `-serial file:/tmp/dltestK.log` 落盘，再 `grep` 判读。

```bash
cd /mnt/d/Projects/SukiOS
export PATH=/mnt/d/Projects/SukiOS/opt/bin:$PATH
make iso                                   # 内核内嵌 dltest blob
make disk                                  # 磁盘含 ::LIB/libtest.sl / ::LIB/libtest2.sl
make run-headless QEMU_SERIAL="-serial file:/tmp/dltestK.log" RUN_TIMEOUT=115
grep -aE "dltest|COPY|reclaim|sl_add|sl_sub|sl_name|sl_const|PASS ===" /tmp/dltestK.log
```

**判读标准**：
- `[COPY] sl_const(主程序副本) = 42` → COPY 重定位成功；
- `[elf] dlclose: reclaim module 'libtest2.sl'` → 物理页回收触发；
- `[OK] 回收后复用: sl_add(1,2)=45` → 槽位复用证明彻底回收；
- `=== dltest PASS ===` 且无 `panic` / `triple fault` / `kheap CORRUPTION` → 生产场景零 panic；
- `POSIX test summary: PASS=192 FAIL=0` → 整体回归全绿。

---

## 7. 改动文件清单

| 文件 | 改动要点 |
|------|----------|
| `kernel/elf/elf.c` | `elf_apply_reloc` 增加 R_X86_64_COPY（`dm==mod` 跳过、取定义方 `st_size`）；修复 `elf_write_user`/`elf_copy_user` 页内偏移双重计算；`elf_unmap_module` 真正回收用户物理页；`elf_free_modules` 加 `img_owned` 守卫；`elf_dlopen` 去重/槽位复用；`elf_dlclose` 引用归零后回收 |
| `include/kernel/elf.h` | `elf_module_t` 增加 `resident`、`img_owned`；更新 `elf_free_modules` 声明与注释 |
| `kernel/sched/sched.c` | `task_exit_current` 在 `vmm_destroy_address_space` 后调用 `elf_free_modules` 修复内核堆泄漏 |
| `user/apps/dltest.c` | 重写：引用 `extern int sl_const`（触发 COPY）、加载期库 dlsym、`/LIB/libtest2.sl` 运行期 dlopen + dlclose 回收 + 再次 dlopen 验证槽位复用 |
| `user/user.ld` | 新增 `.dynamic`/`.got`/`.data.rel.ro` 段 |
| `Makefile` | `USER_LD`、libtest 纯 `-fPIC`、`ld -shared` 产出 ET_DYN、dltest 动态链接、disk 安装 `libtest2.sl` |

---

## 8. 安全 / 防御性说明

- COPY 重定位强制 `src != 0 && sz != 0` 才执行，避免对未知/零大小符号越界写；
- 源解析跳过 `SHN_UNDEF` 与当前模块自身，只取真实定义，杜绝「副本占位符」自拷贝；
- `elf_unmap_module` 按 `mod->img` 重新解析 PT_LOAD，无需在 `elf_module_t` 缓存段表，
  边界由 `vmm_translate` 与 `pmm_decref` 保证；
- `pmm_decref`（非 `pmm_free_page`）正确应对共享/引用计数物理页，避免误回收仍被引用的页；
- `img_owned` 严格区分静态 blob 与 kmalloc 副本，杜绝 kfree 静态区导致的堆损坏。

全部实现为真正可工作代码，无 stub / TODO / 留待以后，已通过 QEMU 生产场景回归（零 panic、
零 kheap 损坏、`POSIX PASS=192 FAIL=0`）。
