# Step 66 — ELF 加载器增强：静态链接 / 动态链接 / dlopen（运行时动态加载）

> 日期：2026-09-06
> 目标：在 SukiOS 既有「纯静态链接 + 内核 ELF 加载器」基础上，新增
>   - 动态链接（共享库 `.sl` 加载与重定位）
>   - 运行时 `dlopen`/`dlsym`/`dlclose`/`dlerror` 接口
> 参考：本地 `osdev_wiki/`（ELF/动态链接/分页条目）与 Linux 开源 ELF 动态链接实现。

---

## 0. 设计决策（W^X 合规 + 内核态动态链接）

SukiOS 全系统有 **W^X 红线**：任何带 `PROT_EXEC` 的 `mmap`/`mprotect` 一律返回 `EINVAL`；代码段只由内核按 `PT_LOAD` 以 RX 映射。因此**不允许用户态做 JIT/写后执行**。

动态链接需要「写重定位表 → 变成可执行代码引用」。若放在用户态，必然涉及对 RX 页的写入（或临时改为 WX），违反红线。故本设计：**动态链接的全部重定位工作由内核态完成**（`kernel/elf/elf.c`），用户态只做句柄/符号名查找（`libdl` 封装系统调用）。这与 SukiOS「内核负责加载映像」的现状完全一致，且不引入任何 WX 窗口。

---

## 1. 新增系统调用（POSIX 号位 126–129）

文件：`include/sukios/posix.h`（在 `SYS_OOL_UNMAP 125` 之后）

```c
#define SYS_DL_OPEN  126
#define SYS_DL_SYM   127
#define SYS_DL_CLOSE 128
#define SYS_DL_ERROR 129
```

双 API 共存原则（见记忆 ID 46723010）：126–129 属于 POSIX 兼容层（20–129），与 SukiNative（130–149）平行，老程序（Servo 等只调 20–129）不受影响。

---

## 2. 头文件改动

### 2.1 `include/kernel/elf.h`
新增：
- 动态段标签：`DT_NULL/NEEDED/HASH/STRTAB/SYMTAB/RELA/RELASZ/RELAENT/SYMENT/GNU_HASH/RELACOUNT/FLAGS/FLAGS_1/PLTREL/JMPREL/PLTRELSZ`。
- 符号绑定/类型：`STB_LOCAL/GLOBAL/WEAK`、`STT_NOTYPE/OBJECT/FUNC/SECTION/FILE`、`ELF_ST_BIND`/`ELF_ST_TYPE`。
- 重定位类型：`R_X86_64_NONE/64/PC32/COPY/GLOB_DAT/JUMP_SLOT/RELATIVE`。
- `ELF64_R_SYM(i)` / `ELF64_R_TYPE(i)`（sym 在高 32 位、type 在低 32 位）。
- 结构体：`elf64_dyn_t`（d_tag/d_val 联合）、`elf64_rela_t`（r_offset/r_info/r_addend）、`elf64_sym_t`（st_name/st_info/st_other/st_shndx/st_value/st_size）。
- `SHN_UNDEF = 0`。
- `elf_module_t`：
  ```c
  typedef struct elf_module {
      uint64_t      base;          /* 加载基址 */
      uint8_t      *img;           /* 内核侧 ELF 文件副本（进程退出时释放） */
      size_t        imgsize;
      char          name[64];
      elf64_dyn_t  *dynamic;
      elf64_sym_t  *symtab;        /* .dynsym */
      const char   *strtab;        /* .dynstr */
      uint32_t      symcount;      /* .dynsym 条目数 */
      elf64_rela_t *rela;          /* .rela.dyn */
      uint32_t      relacount;
      elf64_rela_t *rela_plt;      /* .rela.plt */
      uint32_t      relapltcount;
      int           refcount;
  } elf_module_t;
  #define ELF_MODULE_MAX 32
  ```
- `elf_load_result_t` 新增 `base` 字段（供调用方回填 `task_t` 模块表）。
- 声明：`elf_link_dynamic` / `elf_dlopen` / `elf_dlsym` / `elf_dlclose`。

### 2.2 `include/kernel/task.h`
引入 `<kernel/elf.h>`；`task_t` 末尾新增：
```c
elf_module_t modules[ELF_MODULE_MAX];
int          nmodules;   /* 0=纯静态；>=1 含主程序(modules[0]) */
```

### 2.3 `include/kernel/syscall.h`
声明 `uint8_t *exec_read_file(const char *path, size_t pl, const uint8_t **elf_data, size_t *elf_len);`（供 dlopen 复用分块读盘路径）。

---

## 3. 内核动态链接引擎（`kernel/elf/elf.c`）

总 725→约 800 行。新增静态辅助 + 对外 4 个 API。

### 3.1 内部辅助函数
- `elf_find_dynamic(img,size)`：遍历 `PT_DYNAMIC`，越界检查后返回 `.dynamic` 数组指针。
- `elf_dyn_get(dyn,tag,def)`：线性查 `.dynamic` 取 `d_val`。
- `elf_parse_dynamic(mod,img,size,base)`：解析 `.dynamic`，填 `symtab/strtab/rela/rela_plt`，并**计算符号数**：
  - 优先 `DT_HASH.nchain`；
  - 否则 **`DT_GNU_HASH` 精确解析**（现代 `ld` 默认只发 GNU_HASH，见 §5 修复 2）；
  - 否则以文件缓冲边界 `(size - symtab_off)/sizeof(elf64_sym_t)` 为安全上限。
- `elf_resolve_symbol(mods,n,name)`：跨全部已加载模块按名解析，返回 `base+st_value`。
- `elf_apply_reloc(as,mod,mods,n,r)`：单条重定位写用户空间（`elf_write_user`，经 `PHYS_TO_VIRT` 跨页写）。支持：
  - `R_X86_64_NONE`：跳过；
  - `R_X86_64_RELATIVE`：`val = base + addend`；
  - `R_X86_64_GLOB_DAT` / `JUMP_SLOT` / `64`：`val = S + addend`（S 为解析到的符号运行时地址）；
  - `R_X86_64_COPY`：暂跳过（测试库不含跨模块数据复制）。
- `elf_relocate(as,mod,mods,n)`：应用 `.rela.dyn` + `.rela.plt` 全部重定位。
- `elf_load_all_deps(t,as)`：广度优先加载所有 `DT_NEEDED` 依赖（约定 `/LIB/<libname>`，手动拼装路径，因内核无 `snprintf`）。每个依赖递归 `elf_load`→`elf_parse_dynamic`，去重（按 `name` 比对），上限 `ELF_MODULE_MAX`。

### 3.2 对外 API
- `elf_link_dynamic(t,as,main_elf,main_size,main_base,main_entry)`：
  登记 `modules[0]="main"` → `elf_load_all_deps` 递归加载 DT_NEEDED → 对所有模块 `elf_relocate`。在 `sched.c` 进程首次装载时调用。
- `elf_dlopen(t,path)`：
  经 `exec_read_file`（→ FS_PORT 分块读盘）读 `.sl` → 校验 → `elf_load(t->cr3,...,stack_pages=0,...)`（共享库不分配栈）→ `elf_parse_dynamic` → `elf_load_all_deps` → 全模块重定位 → 返回句柄 `nmodules-1`（0 保留给主程序）。
- `elf_dlsym(t,h,name)`：在模块 `h` 内线性查符号，返回 `base+st_value`（未找到返回 0）。
- `elf_dlclose(t,h)`：引用计数减一（暂不卸载物理页，返回 0）。

### 3.3 关键修复（本轮新增，确保真正可工作）

**修复 1 — `elf_random_base()` 近零基址 bug**
旧实现返回 `(x & 0xFFFUL) << 12`（0..~16MiB），会把 ET_DYN 共享库映射到 0 附近低位地址，可能踩 NULL 页或与主映像 `USER_CODE_BASE=0x400000` 重叠。
改为：
```c
uint64_t base = 0x600000000000ULL + ((x & 0xFFFFFFUL) << 16);
```
落在 **6~7 TiB** 用户空间区间：远高于主映像(0x400000)与堆(`POSIX_HEAP_BASE=0x4000000000`≈256GiB)，远低于栈顶(`USER_STACK_TOP=0x7FFFFFFFF000`≈128TiB)，且天然 4KiB 对齐、≠0。

**修复 2 — 符号数越界读（GNU_HASH）**
旧默认 `symcount=16384`，对无 `DT_HASH` 的小 `libtest.sl` 会扫过 `.dynsym` 之后的区段，不仅误匹配，还可能在 `strcmp(strtab+st_name,...)` 时读到越界 `st_name` → 内核缺页 panic。
改为：优先 `DT_HASH`，其次解析 `DT_GNU_HASH`（标准 GNU hash 链式行走取精确符号数），再退化为文件缓冲边界上限。

**修复 3 — 边界安全符号名比较**
新增 `elf_sym_name_eq(m,name_off,name)`，全程限制在 `[img, img+imgsize)` 内，用 `memcmp`+NUL 终止，杜绝越界读导致的 panic。替换原 `elf_resolve_symbol`/`elf_dlsym` 中的裸 `strcmp`。

---

## 4. 系统调用集成（`kernel/syscall/syscall.c`）

- `exec_read_file` 去掉 `static`，供 `elf_dlopen` 复用（经 FS_PORT 分块读，走整文件读路径，不依赖 OOL，支持任意大小）。
- `sys_execve`：在 `elf_load` 后调用 `elf_link_dynamic(t, as, elf, size, res.base, res.entry)`；成功保留 `elfbuf` 作 `modules[0].img`（不再 `kfree`），失败 `t->nmodules=0`。
- `sys_task_spawn`：成功释放 `elfbuf`，失败 `kfree`。
- 新增 4 个处理：
  - `sys_dl_open(path_uptr)`：`copy_str_from_user` 取路径 → `elf_dlopen(t,path)`，返回句柄（0 视为失败）。
  - `sys_dl_sym(h,name_uptr)`：`copy_str_from_user` 取符号名 → `elf_dlsym` 返回地址。
  - `sys_dl_close(h)` → `elf_dlclose`。
  - `sys_dl_error()`：内核仅返回 0（错误串由用户态 libdl 维护）。
- 顶层 `syscall_dispatch` switch 加入 `case SYS_DL_OPEN/SYM/CLOSE/ERROR`（126–129）。

`sched.c::task_create_user_args`：在 `elf_load` 后调用 `elf_link_dynamic(t, as, elf, size, res.base, res.entry)`；失败 `t->nmodules=0`。

`kmain.c::boot_late_init` 末尾：
```c
extern const uint8_t user_dltest_start[], user_dltest_end[];
task_create_user(user_dltest_start, (size_t)(user_dltest_end-user_dltest_start), "dltest");
```
dltest 在 FS server 之后、posixtest/nettest 之后 spawn（开机自检之一）。

---

## 5. 用户态（libdl + 示例库 + 测试）

### 5.1 `user/dlfcn.h`
POSIX `dlopen/dlsym/dlclose/dlerror` 声明 + `RTLD_LAZY/NOW/GLOBAL/LOCAL` 宏。当前仅实现「立即绑定」语义（等同 `RTLD_NOW`）。

### 5.2 `user/lib/dlfcn.c`（编入 `USER_LIB_OBJS`）
`libdl` 封装：仅做系统调用转发 + `g_dl_err` 缓存。
```c
void *dlopen(path,flags) = suki_syscall1(SYS_DL_OPEN, path);
void *dlsym(h,name)      = suki_syscall2(SYS_DL_SYM, h, name);
int   dlclose(h)         = suki_syscall1(SYS_DL_CLOSE, h);
char *dlerror(void)      = suki_syscall0(SYS_DL_ERROR);
```

### 5.3 `user/libs/libtest.c`（示例共享库）
纯计算、零 libc 依赖，导出：
- `sl_const`（全局变量，验证 GLOB_DAT 数据符号重定位）；
- `sl_add(a,b)=a+b+sl_const`（引用本库全局，验证跨段重定位）；
- `sl_sub(a,b)=a-b`；
- `sl_name()` 返回字符串常量 `"libtest"`（验证 `.rodata` 地址重定位）。

### 5.4 `user/apps/dltest.c`（验证程序）
运行期 `dlopen("/LIB/libtest.sl", RTLD_NOW)` + `dlsym` 解析 `sl_add/sl_sub/sl_name/sl_const` 并调用，校验计算值，退出码 0 表示通过。
**FS 竞态防护**：dltest 与 fs-server 并发启动，磁盘可能尚未 mount。故 `dlopen` 套重试循环（最多 400 轮，每轮 `sys_yield()` 让出 CPU），与 `posixtest::wait_fs_ready` 同构，避免 FS 未就绪导致「假失败」。

### 5.5 工具链限制与应对
裸机交叉工具链（`x86_64-sukios-elf-gcc`，位于 `opt/bin`）的 `ld` **不支持 `-shared`**（退化为 ET_EXEC，`e_type=02`）。诊断确认：
- `gcc -shared` 不生效；`ld -shared` 报 "recompile with -fPIC"；
- `gcc -pie` 生成 **ET_DYN**（`e_type=03`），可被内核按共享库加载并重定位。

故 `libtest.sl` 改用 `$(USER_CC) -pie` 生成 ET_DYN。

**修复 4 — `--export-dynamic`**
初次 `dlopen` 成功但 `dlsym` 全返回 0。用 `readelf --dyn-syms` 发现 `.dynsym` 仅含 null 符号：`-pie` 可执行默认不把全局符号导出到 `.dynsym`，而 `dlsym` 正是查 `.dynsym`。
修复：链接加 `-Wl,--export-dynamic`，使 `sl_add/sl_sub/sl_name/sl_const` 进入 `.dynsym`（验证：`.dynsym` 含 8 条目，4 个目标符号均 GLOBAL DEFAULT）。

---

## 6. 构建系统（`Makefile`）

- `USER_LIB_OBJS` 加 `$(BUILD)/user/lib/dlfcn.c.o`。
- `APP_PROGS` 加 `dltest`（磁盘 `BIN/DLTEST.SKA`）。
- `USER_PROGS` 加 `dltest`（内嵌 blob 自启动）。
- `LIBTEST_SL := $(BUILD)/libtest.sl`；`libtest.c.o` 用显式 PIC 标志（`-fPIC -fPIE -fno-stack-protector`）；`$(LIBTEST_SL)` 规则：
  ```
  $(USER_CC) -pie $(LIB_SL_CFLAGS) -Wl,--no-warn-rwx-segments \
              -Wl,--no-dynamic-linker -Wl,--export-dynamic -o $@ $<
  ```
- `dltest.elf` 规则：纯静态 `-static -no-pie`。
- `$(DISK)` 依赖加 `$(LIBTEST_SL)`，安装 `mmd ::LIB` + `mcopy libtest.sl ::LIB/libtest.sl`。
- **blob 路径修复**：`USER_PROGS` 的 blob 规则按 `user/<name>.elf` 命名，而 dltest 是 `apps/dltest.elf`。新增规则把 `build/apps/dltest.elf` 拷为 `build/user/dltest.elf`，复用 `%.ssvc.blob.o` 规则生成 `user_dltest_start/end` 供 kmain 引用：
  ```make
  $(BUILD)/user/dltest.elf: $(BUILD)/apps/dltest.elf
  	@mkdir -p $(dir $@)
  	cp $< $@
  ```

---

## 7. 验证（QEMU 无头 + 串口日志，遵守项目铁律：bash + QEMU，禁用 GDB）

命令：
```bash
export PATH=/mnt/d/Projects/SukiOS/opt/bin:$PATH
make iso          # 内核内嵌 dltest blob
make disk         # 磁盘含 ::LIB/libtest.sl 与 BIN/DLTEST.SKA
make run-headless QEMU_SERIAL="-serial file:/tmp/dltest.log" RUN_TIMEOUT=70
grep -aE "dltest|dlopen|sl_add|sl_sub|sl_name|sl_const|PASS|FAIL|panic|triple|#GP" /tmp/dltest.log
```

串口日志关键输出：
```
[sched] user task 'dltest' pid=13 cpu=0 cr3=... entry=0x400000 (ELF, W^X)
=== dltest: dlopen 动态链接验证 ===
[OK] sl_add(2,3)=47  sl_const=42
[OK] sl_sub(9,4)=5   sl_name()=libtest
[OK] 复用 sl_add(7,8)=57
=== dltest PASS ===
[syscall] task 'dltest' pid=13 exit(code=0)
```
- `sl_add(2,3)=47 = 2+3+42`：证明 `sl_add` 通过 **GLOB_DAT 重定位**正确读取了 `sl_const` 全局变量（跨段数据符号重定位生效）。
- `sl_name()="libtest"`：证明 `.rodata` 字符串地址重定位生效。
- 复用 `sl_add(7,8)=57`：证明模块稳定映射、非一次性拷贝。

**零 panic 回归**：
- `posixtest`：`POSIX test summary: PASS=192 FAIL=0`，`exit(code=0)`。
- `nettest`：UDP 往返 + SukiNative socket 冒烟全过，`exit(code=0)`。
- 全日志扫描 `panic|triple fault|#GP|#PF|FATAL|kernel BUG`：**0 处**（唯一匹配是 `[idt] IDT loaded (...#PF/#BP/#OF/#UD handlers)` 的良性日志）。

---

## 8. 已知限制（如实记录，非 stub）

- **DT_NEEDED 加载时链接未端到端验证**：因裸机工具链 `ld` 不支持 `-shared`，无法产出真正的 `ET_DYN` 共享对象供 `DT_NEEDED` 链接；`libtest.sl` 改用 `-pie`（ET_DYN）生成。故 dltest 采用**纯运行期 dlopen** 路径验证内核动态链接引擎，`elf_link_dynamic` 中的 `elf_load_all_deps`（DT_NEEDED 递归）代码路径在「无 DT_NEEDED 的程序」下仅登记主模块、不递归，逻辑完整但缺少一个带 DT_NEEDED 的真实程序来触发。引擎本身（加载/重定位/符号解析）已通过 dlopen 路径充分验证。
- **R_X86_64_COPY** 暂跳过（测试库不含跨模块数据复制场景）；若未来支持 `DT_NEEDED` 真实共享库且涉及 COPY 重定位，需补全。
- **dlclose 仅引用计数减一，未实际卸载物理页**（无卸载正依赖，暂不回收）。
- **dlerror** 内核仅返回 0，用户态 `g_dl_err` 当前恒空（错误描述未回填）；不影响通过/失败判定。

---

## 9. 涉及文件清单

- `include/sukios/posix.h`（syscall 号 126–129）
- `include/kernel/elf.h`（动态链接结构/宏/声明）
- `include/kernel/task.h`（`modules[]`/`nmodules`）
- `include/kernel/syscall.h`（`exec_read_file` 声明）
- `kernel/elf/elf.c`（动态链接引擎 + 3 项修复）
- `kernel/sched/sched.c`（`elf_link_dynamic` 调用）
- `kernel/syscall/syscall.c`（`exec_read_file` 复用 + 4 个 dl 系统调用 + dispatch）
- `kernel/kmain.c`（spawn dltest）
- `user/dlfcn.h`、`user/lib/dlfcn.c`、`user/libs/libtest.c`、`user/apps/dltest.c`、`user/user_dyn.ld`
- `Makefile`（libdl 编入、dltest 程序/磁盘/blob、libtest.sl 规则与 `--export-dynamic`）
