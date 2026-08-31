# Step 52 — 专属交叉工具链 x86_64-sukios-elf 构建与完整 libc 头落地

> 阶段目标：为 SukiOS 从源码构建一套**专属**交叉工具链（区别于系统的 `x86_64-elf-*`），
> 并补齐 GCC 在 freestanding 模式下编译 SukiOS 用户态程序所需的**完整标准 C 头文件**，
> 使内核用宿主 GCC、用户态用专属 GCC 的工具链分流正式成型。

---

## 一、交付内容总览

| 模块 | 状态 | 说明 |
|---|---|---|
| 交叉工具链 `x86_64-sukios-elf` | ✅ 已构建并安装 | Binutils 2.43 + GCC 14.2.0，安装至 `opt/bin/` |
| 国内/海外镜像自动探测 | ✅ 已落地 | 清华 TUNA 自动加速，海外回退官方源 |
| 下载完整性校验 | ✅ 已落地 | gzip/xz/bz2 校验防截断残包 |
| 解压守卫 | ✅ 已落地 | configure 存在则跳过，避免重复解压 |
| 内核/用户态工具链分流 | ✅ 已落地 | 内核 `CC=gcc`；用户态 `USER_CC=x86_64-sukios-elf-gcc` |
| 完整 libc 头（shims） | ✅ 已落地 | string/stdlib/stdio/errno/unistd/ctype/time/dirent |
| libgcc 支持 | ✅ 可构建 | `make -f cross/Makefile.gcc libgcc` |

---

## 二、交叉工具链构建（`cross/Makefile.gcc`）

### 2.1 目标三元组

```
TARGET := x86_64-sukios-elf
```

- 选用 `x86_64-sukios-elf`（而非 `x86_64-elf`）：`config.sub` 将 `sukios` 识别为
  vendor/OS 标识、`elf` 识别为 OS 类别，均被 binutils/gcc 接受。
- 工具名形如 `x86_64-sukios-elf-gcc` / `-ld` / `-as`，与系统已有的 `x86_64-elf-gcc`
  等**明确区分**，避免混淆。
- 安装前缀：`PREFIX := $(CURDIR)/opt`，故工具实际位于 `opt/bin/`。

### 2.2 目录布局（与主 Makefile 分离）

```
src/          源码包解压（gcc-14.2.0、binutils-2.43）
cross-build/  交叉工具链构建目录（gcc、binutils 子目录）
opt/          安装前缀（opt/bin/ 下放工具链）
```

> 关键：交叉工具链构建目录改为 `cross-build/`，**不再使用主 Makefile 的 `build/`**，
> 避免两者混用互相污染。

### 2.3 国内/海外自动识别 + 镜像加速

```make
# 自动探测：清华镜像 5s 内可达（HTTP 2xx）即视为国内
AUTO_CN := $(shell curl -fsI --connect-timeout 5 -m 8 \
            https://mirrors.tuna.tsinghua.edu.cn/gnu/binutils/ >/dev/null 2>&1 && echo 1 || echo 0)
IN_CHINA := $(AUTO_CN)
```

| 网络 | binutils | gcc |
|---|---|---|
| 国内 | 清华 TUNA `mirrors.tuna.tsinghua.edu.cn/gnu/binutils/...tar.gz` | 清华 TUNA `.../gnu/gcc/...` |
| 海外 | GNU FTP `ftp.gnu.org/gnu/binutils/...` | github `gcc-mirror/gcc` releases tag |
| 回退 | 反向官方源 | 反向官方源 + `gh.dl.ifiss.eu.org` 代理 |

可由 `IN_CHINA=1` / `IN_CHINA=0` 强制覆盖自动探测。运行时会打印 `[mirror] 国内/海外...` 标签。

### 2.4 下载完整性校验（防截断残包）

`fetch` 宏内部 `try_one` 函数：

```make
case "$(2)" in
    *.tar.gz|*.tgz) gzip -t "$(2)" 2>/dev/null  || return 1 ;;
    *.tar.xz)      xz -t "$(2)" 2>/dev/null     || return 1 ;;
    *.tar.bz2)     bzip2 -t "$(2)" 2>/dev/null || return 1 ;;
esac
```

每下载完一个包都做对应压缩格式校验；失败则删包、尝试下一个源（②回退源 → ③github 代理），
全部失败才报错退出。解决了早期"代理返回 200 但文件被截断"导致解压卡死的 bug。

### 2.5 解压守卫

```make
$(GCC_SRC)/configure: $(SRC)/gcc-$(GCC_VER).tar.gz
	@if [ -f "$@" ]; then echo "已解压，跳过"; \
	else tar -xzvf $< -C $(SRC) > $(SRC)/gcc.extract.log 2>&1; fi
	@touch $@
```

若 `configure` 已存在（之前解压过且完好），直接跳过解压，避免大包重复解压浪费时间；
解压过程用 `-v` 写详细日志到 `src/gcc.extract.log`，便于观察进度而非"卡住"。

### 2.6 GCC 配置（freestanding，--without-headers）

```make
$(GCC_SRC)/configure \
    --target=$(TARGET) --prefix=$(PREFIX) \
    --disable-nls --enable-languages=c,c++ \
    --without-headers --disable-libssp \
    --disable-libquadmath --disable-libsanitizer \
    --disable-libstdcxx --disable-shared --with-newlib
```

- `--without-headers` + `--with-newlib`：GCC 视目标为无宿主 C 库环境，
  匹配 SukiOS freestanding libc（`user/lib`）。
- GCC configure **仅依赖已安装的 binutils 工具**（`ld`/`as` 在 `opt/bin` 且 PATH 中），
  不再依赖 binutils 的 build 目录——避免在 `make clean` 清空 `cross-build/` 后被迫
  重建 binutils 而触发 `config.cache` 污染（`gas: cannot compute suffix of executables`）。

### 2.7 libgcc（可选）

```
make -f cross/Makefile.gcc libgcc
```

执行 `all-target-libgcc` + `install-target-libgcc`，提供 `__divdi3` 等运行时符号。
依赖 `cross-build/gcc/Makefile` 已生成（即已跑过 `make gcc`）。

---

## 三、主 Makefile 工具链分流（`Makefile`）

关键修改：

```make
CROSS_OPT := $(CURDIR)/opt            # 修正自 cross/opt
SUKIOS_CC := $(CROSS_OPT)/bin/x86_64-sukios-elf-gcc
export PATH := $(CROSS_OPT)/bin:$(PATH)

CC := gcc                            # 内核固定用宿主 gcc
USER_CC := $(shell if [ -x $(SUKIOS_CC) ]; then echo $(SUKIOS_CC); else echo gcc; fi)

USER_CFLAGS += -I user/lib/shims     # 让专属 gcc 找到 freestanding 标准头
```

用户态规则（shell、fs_server、apps、fatfs、stack_canary）统一改用 `$(USER_CC)`，
链接追加 `-lgcc`（需要 libgcc 时）。内核始终用 `$(CC)=gcc`，不经过专属 gcc，
故内核编译不再受 freestanding 头缺失影响。

---

## 四、完整 libc 头（shims）

GCC freestanding 自带 `stdint/stddef/stdarg/stdbool/limits/float`，但**不自带**
`string/stdlib/stdio/errno/unistd/ctype/time/dirent`。本次补齐为完整标准头，
全部位于 `user/lib/shims/`，适配 SukiOS 系统调用（见 `suki.h`）。

| 头文件 | 关键内容 |
|---|---|
| `string.h` | 完整 `mem*/str*`，外加 `strerror`/`strsignal`（返回 `const char*`，覆盖 ENAMETOOLONG 等新错误码） |
| `stdlib.h` | `malloc/free/exit/getenv/atexit/strtol/strtoul/atoi` 等，含 `EXIT_*` 宏 |
| `stdio.h` | `printf/snprintf/puts` 等（依赖 libc.c 实现），`FILE` 占位 |
| `errno.h` | `extern int errno` + 全套 `E*` 宏（EEXIST/EINVAL/ENAMETOOLONG/ENOTDIR/EROFS/ENOSPC/ENXIO/ENOTTY/ESPIPE 等） |
| `unistd.h` | `open/read/write/close/lseek` 及 `ssize_t/pid_t/size_t` 等 POSIX 类型 |
| `ctype.h` | `isalpha/isdigit/isspace/tolower/toupper` 等 |
| `time.h` | `time_t/timeval/timespec`，`CLOCK_REALTIME/CLOCK_MONOTONIC` |
| `dirent.h` | `DIR/dirent` 结构（配合 `opendir/readdir/closedir` 由 libc 提供） |

`user/lib/libc.h` 重构：删除与标准头重复的裸声明，改为聚合包含上述 shims 头，
仅保留 SukiOS 特有的 `struct stat/utsname/tms/rlimit/timezone` 及 getopt 映射。

> 历史 bug 修复：原 `libc.h` 注释中提前闭合的 `*/`、以及重复定义的 `CLOCK_*` 宏已清理。

---

## 五、物理键盘方向键/Tab 转发说明（供 Step 53 联动）

SukiOS 输入路径：`input_server` 读取 PS/2 扫描码 → 经 `mach_msg`（PORT_INPUT，msgh_id=0x1002）
把按键字符发给 `shell`。当前 `input_server` **未解析 e0 前缀扫描码**（方向键 Up/Down/Left/Right
为 `e0 0x48/0x50/0x4B/0x4D`），这些键会被 `if (!c) continue` 丢弃。

要让 shell 的方向键历史滚动、Tab 补全在**物理键盘**下真正工作，Step 53 将同步改造
`input_server`：检测 e0 前缀，把方向键/Tab/Home/End/Delete 编码为 ANSI 转义序列
（`\x1b[A` 上、`\x1b[B` 下、`\x1b[C` 右、`\x1b[D` 左、`\x1b[H`、`\x1b[F`、`\x1b[3~`）
通过 `mach_msg` 字节流发给 shell；串口（headless）场景由 QEMU 串口直接注入相同转义序列，
shell 端统一解析。

---

## 六、验证方式

1. **工具链存在性**：
   ```bash
   ls opt/bin/x86_64-sukios-elf-gcc   # 应存在
   opt/bin/x86_64-sukios-elf-gcc --version   # 14.2.0
   ```
2. **用户态编译**（用专属 gcc）：
   ```bash
   export PATH="$(pwd)/opt/bin:$PATH"
   make clean && make run
   ```
   shell/fs_server/apps 用 `x86_64-sukios-elf-gcc` 编译，链接 `-lgcc`，无 `string.h` 缺失报错。
3. **内核编译**：`CC=gcc` 宿主编译，`#include <string.h>` 经 `-I user/lib/shims` 解析。
4. **QEMU 生产回归**：`-serial stdio` 看 shell 提示符 `SukiOS>`，命令/管道/历史可工作，零 panic。

---

## 七、文件清单（本次改动）

- `cross/Makefile.gcc`：镜像探测、完整性校验、解压守卫、目录分离、libgcc 目标
- `Makefile`：工具链分流（`USER_CC`、PATH、USER_CFLAGS）
- `user/lib/shims/*.h`：8 个完整标准头
- `user/lib/libc.h`：重构聚合标准头 + 保留 SukiOS 特有结构
- `user/lib/stdlib.c` / `string.c`：配套实现
