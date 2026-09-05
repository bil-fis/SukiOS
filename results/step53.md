# Step 53 — Shell 命令行编辑器增强 + FreeType 字体服务与 pchfnt 接入

> 本阶段包含两项互相独立、但都面向「可用的人机界面」的工作：
>
> **A. Shell 编辑器增强（bash 风格）**：历史 100 条内存环形缓冲、方向键滚动、
> Tab 补全、行内光标编辑（←/→/Home/End/Delete）；并让 input_server 真正解析
> PS/2 `e0` 扩展扫描码，把方向键发成 ANSI 转义序列。
>
> **B. 接入 FreeType（lib/freetype-2.14.3）**：在 freestanding 用户态编译
> FreeType，编写常驻**字体服务 fontsrv**（FreeType 渲染入口程序，字体/字形内存缓存），
> 并编写外部程序 **pchfnt**（`--font` / `--text` / `--text-unicode`），经 Mach IPC
> 调用字体服务渲染文字。

---

## 一、Shell 编辑器增强

### 1.1 需求与实现对照

| 需求 | 实现 | 位置 |
|---|---|---|
| 历史 100 条，内存存储，不落盘，超出丢旧 | `HIST_MAX` 32 → **100**，环形 FIFO 覆盖最旧 | `user/shell.c` |
| 方向键上下滚动历史 | ↑=更旧、↓=更新，到底回到「新行」 | `user/shell.c` 转义状态机 |
| Tab 键补全 | 文件名补全：唯一匹配直接补全（目录补 `/`），多匹配补公共前缀 + 列出候选 | `edit_tab_complete()` |
| 行内光标编辑 | ←/→ 移动光标、Home/End 跳行首行尾、Backspace 删前、Delete 删后 | `edit_left/right/home/end`、`edit_backspace/delete` |
| 插入而非覆盖 | 光标处插入，右侧字符右移（`memmove`） | `edit_insert()` |

### 1.2 编辑状态（新增全局）

```c
static char g_line[LINE_MAX];   /* 当前编辑缓冲 */
static int  g_len;              /* 已输入字符数 */
static int  g_cur;              /* 光标下标 0..g_len（支持任意位置编辑） */
```

原实现只有「行尾追加 + `\b \b` 整行回退」，无法在行中间编辑。现改为**光标感知**：
插入/删除都以 `g_cur` 为锚点，重绘统一走 `redraw_from_cursor()`（清到行尾 →
重印光标后内容 → 按差值左移光标回位）。

### 1.3 历史环形缓冲

```c
#define HIST_MAX 100
static char g_hist[HIST_MAX][LINE_MAX];
static int  g_hist_count = 0;   /* 现有条数 */
static int  g_hist_idx = 0;     /* 浏览游标；= g_hist_count 表示「新行」 */
```

- 提交时：非空行入历史；`g_hist_idx` 复位到 `g_hist_count`（新行）。
- ↑：若 `g_hist_idx > 0` 则 `g_hist_idx--` 并 `hist_load(g_hist_idx)` + `redraw_full()`。
- ↓：若 `g_hist_idx < g_hist_count` 则 `g_hist_idx++`；到达末尾时 `hist_load_new()` 清空为新行。
- **不写入任何文件**：纯内存数组，超出 100 条由 `hist_push` 覆盖（环形 FIFO，丢最旧）。

### 1.4 转义序列状态机（扩展）

原状态机只有 `ESC_ESC`/`ESC_BRK` 且仅处理 `A`/`B`。现扩展为：

```c
static int g_esc = ESC_NONE;
static int g_esc_num = 0;   /* ESC[ 之后的数字前缀（如 Delete 的 3） */
```

| 序列 | 动作 |
|---|---|
| `ESC [ A` | 上：历史更旧 |
| `ESC [ B` | 下：历史更新（到末尾回到新行） |
| `ESC [ C` | 右移光标 |
| `ESC [ D` | 左移光标 |
| `ESC [ H` | Home（行首） |
| `ESC [ F` | End（行尾） |
| `ESC [ 3 ~` | Delete（删光标后字符） |
| `\t` | Tab 补全 |
| `\b` / `0x7F` | Backspace（删光标前字符） |

`ESC_BRK` 状态下若收到数字则累积到 `g_esc_num`，收到 `~` 再按数字分派（支持 `ESC[3~`）。

### 1.5 Tab 补全算法（`edit_tab_complete()`）

1. 从 `g_cur` 往回找词起点（跳过空格 / `>` / `|`），取出待补全前缀 `frag`。
2. 用 `strrchr(frag, '/')` 拆出**目录部分**与**基础名部分**；相对目录基于 `g_cwd` 拼接。
3. `opendir(dir)` 枚举，`readdir` 得到 `de->d_name` / `de->d_type`，
   按 `strncmp(name, base, strlen(base)) == 0` 收集匹配（跳过 `.` 开头，除非前缀以 `.` 起始）。
4. **唯一匹配**：直接 `edit_insert` 补齐余下字符；若 `d_type == DT_DIR` 则补 `/`。
5. **多匹配**：先算公共前缀并补齐，再换行列出全部候选，重印提示符与整行。

> 依赖补充：`user/lib/shims/dirent.h` 原先只定义了 `struct dirent`（含 `d_type`），
> 但**没有 `DT_*` 常量宏**。本次补齐 `DT_UNKNOWN/FIFO/CHR/DIR/BLK/REG/LNK/SOCK`
> 与 `DT_ISDIR(t)`，符合 POSIX `<dirent.h>`。

---

## 二、input_server：PS/2 `e0` 扩展键解析

原 `input_server.c` 把每个扫描码直接查表，方向键的 `e0 0x48` 等序列中 `t_norm[0x48]`
为 0，被 `if (!c) continue` 丢弃 —— **物理键盘方向键根本发不出去**。

新增 `e0` 前缀状态与扩展键映射：

```c
static const char *e0_seq(uint8_t c2)
{
    switch (c2) {
        case 0x48: return "\x1b[A";   /* Up    */
        case 0x50: return "\x1b[B";   /* Down  */
        case 0x4B: return "\x1b[D";   /* Left  */
        case 0x4D: return "\x1b[C";   /* Right */
        case 0x47: return "\x1b[H";   /* Home  */
        case 0x4F: return "\x1b[F";   /* End   */
        case 0x53: return "\x1b[3~";  /* Delete */
        default:   return NULL;
    }
}
```

主循环：收到 `0xE0` 置 `e0 = true` 并 `continue`；下一字节若为扩展键则
`send_escape(seq)` 逐字节发出（shell 端状态机重新拼装）；`e0` 扩展键的 break 帧忽略。
Tab 本身走普通路径（`t_norm[0x0F] = '\t'`），无需特殊处理。

---

## 三、FreeType 接入（freestanding 移植）

### 3.1 移植策略

FreeType 通过 `config/ftstdlib.h` 把全部 libc 调用抽象成 `ft_*` 宏。利用其
「若已定义 `FTSTDLIB_H_` 则整体跳过」的特性，提供**本仓专属适配头**替代系统 stdio：

```
-DFT_CONFIG_STANDARD_LIBRARY_H="\"user/lib/freetype_shim.h\""
```

`user/lib/freetype_shim.h` 提供：
- 整数极限常量（`FT_CHAR_BIT/FT_INT_MAX/FT_LONG_MAX/FT_ULLONG_MAX` 等）；
- `ft_mem*`/`ft_str*`（复用 shims/string.h）；
- `ft_smalloc/sfree/scalloc/srealloc`（复用 shims/stdlib.h）；
- `ft_qsort`、`ft_strtol`、`ft_getenv`、`ft_snprintf`；
- `ft_jmp_buf/ft_setjmp/ft_longjmp` → 本仓 `setjmp.h`；
- `FT_FILE` 占位类型 + `ft_fopen/fread/fseek/ftell/fclose` 桩
  （**永不调用**：本仓只用 `FT_New_Memory_Face`，不经过 stdio）；
- 补 `SEEK_SET/CUR/END`（`ftsystem.c` 编译需要）。

`setjmp/longjmp` 为 FreeType 错误恢复所必需，本仓原先没有，新增：
- `user/lib/shims/setjmp.h`：`typedef uint64_t jmp_buf[8];`
- `user/lib/setjmp.S`：x86_64 SysV 纯汇编实现，保存
  `RBX/RBP/R12..R15/RSP/RIP`（`longjmp` 恢复并把 `val==0` 归一为 1）。

### 3.2 模块裁剪

默认 `ftmodule.h` 注册 Type1/CFF/PCF/BDF/SDF/SVG 等全部驱动，但本仓**只用 TrueType**。
故以

```
-DFT_CONFIG_MODULES_H="\"user/lib/ftmodule_min.h\""
```

覆盖（注意宏名是 `MODULES` 复数，FreeType 由 `ftinit.c` → `FT_Default_Drivers()` 消费），
仅注册：`autofit / tt / psaux / psnames / pshinter / sfnt / smooth / raster1`。
对应只编译 9 个源目录：`base sfnt truetype smooth raster autofit psaux pshinter psnames`。

其它编译要点：
- `-fcommon`：GCC10+ 默认 `-fno-common`，FreeType 跨单元的 `*_class` 会重复定义；
- `-Wl,--allow-multiple-definition`：`-r` 合并 `.o` 时忽略已初始化对象的重复符号；
- `-DFT_CONFIG_OPTION_USE_ZLIB=0`：关闭 gzip 路径，并补 `FT_Gzip_Uncompress` 桩满足链接。

### 3.3 产物

```
build/libfreetype.a   110 个对象，约 1.1 MB
```

`make build/libfreetype.a` 即可单独构建。

---

## 四、字体服务 fontsrv（常驻入口程序）

文件：`user/fontsrv.c`，作为独立磁盘程序 `::BIN/FONTSRV.SKA`（952 KB，含 FreeType），
由 **shell 启动时 `sys_task_spawn` 后台拉起**，认领 `FONT_PORT = 10`。

### 4.1 端口

- `include/ipc/port.h`：新增 `#define FONT_PORT 10`
- `kernel/ipc/port.c` `ipc_init()`：预留循环上界由 `APP_PORT` 改为 `FONT_PORT`，
  使 10 成为知名端口（`port_allocate` 从 9 起扫描时会跳过 10）
- `user/lib/suki.h`：同步新增 `FONT_PORT 10`

### 4.2 数据结构

**字体缓存（按路径，最多 8 个）**

```c
struct font_entry {
    char     path[256];
    uint8_t *data;      /* TTF 文件字节，常驻内存（OSDev 推荐：避免重复读盘/解析） */
    uint32_t size;
    FT_Face  face;
    int      used;      /* 访问计数，满时淘汰最少使用 */
};
```

加载：`open` → 循环 `read` 读满整个文件（动态倍增缓冲）→ `FT_New_Memory_Face(...)`。
若 FatFs 未启用长文件名导致长名打开失败，依次回退 8.3 短名
（`/FONTS/RESOUR~1.TTF`、`/FONTS/RESOURCE.TTF`、`/FONTS/FONT.TTF`）。

**字形缓存（应用层 LRU，2048 槽）**

```c
struct glyph_cache {
    FT_Face   face;  uint32_t glyph;  uint32_t size;   /* 键：face+字形索引+像素字号 */
    int32_t   w, h, top, left, advance;
    uint8_t  *bits;    /* 8-bit 灰度位图（光栅化结果拷贝） */
    int       used;    /* LRU 计数 */
};
```

哈希：`(uintptr_t)face ^ (glyph<<3) ^ (size<<17)`，开放寻址 + LRU 淘汰。
命中则**跳过 `FT_Load_Glyph`/`FT_Render_Glyph`**，直接复用位图 —— 长文本/重复字符显著省时。

### 4.3 渲染流程

1. `FT_Set_Pixel_Sizes(face, 0, req->pixel_size)`
2. 文本解析：`unicode_mode=0` 走 UTF-8 解码（`utf8_next`，支持 1~4 字节）；
   `=1` 走 `strtoul(p,&end,0)` 逐 token 解析（支持 `0x4E2D` / `72` 混写）
3. 逐字：`FT_Get_Char_Index` → 查字形缓存 → miss 则 `FT_Load_Glyph(FT_LOAD_RENDER)`
   并拷出 `slot->bitmap`（灰度 8-bit）→ `blit_glyph()` 合成
4. 合成：`blit_glyph` 按灰度 `a` 做 alpha 混合
   `dst = (c*a + d*(255-a))/255`，写 `0xFF<<24 | RGB`（xRGB32）
5. 笔位推进：`pen_x += slot->advance.x >> 6`（26.6 定点）

### 4.4 帧缓冲

fontsrv 与 display_server 一样经 `SYS_FRAMEBUFFER_MAP` 映射**同一块物理帧缓冲**，
字体直接叠加绘制（无需新增 display 合成消息）。
`enabled=0`（如 headless `-display none`）时降级为**仅渲染 + serial 报告统计**，不写屏、不 panic。

---

## 五、pchfnt（外部渲染程序）

文件：`user/apps/pchfnt.c` → `::BIN/PCHFNT.SKA`（28 KB）。

```
pchfnt --font <path> --text "中文 Hello"      [--size N] [--x N] [--y N] [--color 0xRRGGBB]
pchfnt --font <path> --text-unicode "0x4E2D 0x6587 72" ...
```

流程：`sys_port_claim(APP_PORT)` 作应答端口 → 构造 `font_render_req_t` 发往 `FONT_PORT`
→ 等待 `FONT_MSG_RENDER_DONE` 回执 → 打印统计并退出。

健壮性：**发送带重试**（最多 300 次 + `sys_yield`），因为 shell 刚 spawn fontsrv 时
它可能尚未完成端口认领（自检场景实测有效）。

### IPC 协议（`user/font_ipc.h`）

```c
#define FONT_MSG_RENDER      200
#define FONT_MSG_RENDER_DONE 201
#define FONT_TEXT_MAX        1024

typedef struct font_render_req {
    mach_msg_header_t h;
    uint32_t req_id, pixel_size;
    int32_t  x, y;
    uint32_t color;              /* 0xRRGGBB */
    uint32_t unicode_mode;       /* 0=UTF-8, 1=Unicode 码点串 */
    char     font_path[256];
    char     text[FONT_TEXT_MAX];
} font_render_req_t;

typedef struct font_render_done {
    mach_msg_header_t h;
    uint32_t req_id;
    int32_t  status, glyphs, cache_hits, bbox_w, bbox_h;
} font_render_done_t;
```

---

## 六、内核侧支持：装载大 ELF（关键修复）

**问题**：`fontsrv` 集成了 FreeType，ELF 达 **952 KB**，而
`EXEC_ELF_MAX = 16*4096 = 64 KiB`（与 OOL 单条 `MACH_MSG_OOL_MAX_PAGES=16` 一致），
`sys_task_spawn` 报 `fontsrv spawn failed`。

**修复**：不改动全局 OOL 结构（避免影响所有 IPC 使用者），改为
`exec_read_file()` 用 **`FS_MSG_READ_AT` 分块读**（内联响应，每块 ≤ `FS_READ_MAX=3584`）：

```c
for (;;) {
    ra->offset = offset; ra->length = want;   /* want ≤ 3584 */
    ipc_send_kernel(FS_PORT, req, ...);
    ipc_recv_kernel(rp, resp, sizeof(resp), &out, true);
    if (fr->length == 0) break;               /* EOF */
    memcpy(elfbuf + total, data, fr->length);
    total += fr->length; offset += fr->length;
}
```

同时 `EXEC_ELF_MAX` 提升为 **1 MiB**（分块读已不受 OOL 页数限制）。
该路径对任何大小（≤1 MiB）的映像都成立，也保留了对小映像的正确性。

### 顺带修复：Makefile 默认目标被抢占

FreeType/字体程序规则被插在 `all:` 之前，导致 `make` 把
`build/user/lib/freetype_shim.c.o` 当成默认目标、只编一个对象就退出（内核镜像不重建）。
已显式声明：

```make
all: $(KERNEL)
.DEFAULT_GOAL := all
```

---

## 七、构建系统集成

| 项 | 内容 |
|---|---|
| `FT_DIR` | `lib/freetype-2.14.3` |
| `FT_SRC_DIRS` | `base sfnt truetype smooth raster autofit psaux pshinter psnames` |
| `FT_CFLAGS` | `-ffreestanding -nostdlib -std=gnu11 -O2 -fcommon -DFT2_BUILD_LIBRARY -DFT_CONFIG_STANDARD_LIBRARY_H=... -DFT_CONFIG_MODULES_H=... -DFT_CONFIG_OPTION_USE_ZLIB=0` |
| `FONT_CFLAGS` | 字体程序编译（APP_CFLAGS + FT 的 `-D`/`-I`），含 `-I $(CURDIR)` |
| `$(FT_LIB)` | `build/libfreetype.a`（`-r` 合并 + `--allow-multiple-definition`） |
| `FONT_PROGS` | `fontsrv pchfnt`，链接 `$(USER_LIB_OBJS) $(FT_LIB) -lgcc` |
| 磁盘布局 | `resources/*.ttf` → `::FONTS/<大写名>`；`fontsrv/pchfnt` → `::BIN/*.SKA` |
| `USER_LIB_OBJS` | 新增 `$(BUILD)/user/lib/setjmp.S.o`（否则 FreeType 链接失败） |

libc 补齐：`stdlib.c` 新增 `qsort`（Lomuto 分区 + 显式栈迭代）、`string.c` 新增 `memchr`。

---

## 八、验证

### 8.1 构建

```bash
export PATH="$(pwd)/opt/bin:$PATH"
make            # 内核 + 内嵌服务（含 shell）
make disk       # FAT32 镜像：放入 FONTS/ 字体与 BIN/{FONTSRV,PCHFNT}.SKA
```

输出：`==> FreeType static lib build/libfreetype.a`、`==> font service ...`、
`==> valid Multiboot2 kernel`、`L4 check: .boot VMA=0x1048576`。

### 8.2 QEMU 生产回归（headless）

```bash
qemu-system-x86_64 -machine pc -m 256M -cpu qemu64 \
  -kernel build/kernel.ski -drive file=build/disk.img,format=raw,if=ide,media=disk \
  -serial file:/tmp/suki_font4.log -display none -no-reboot
```

关键日志（实测，零 panic）：

```
[shell] boot: launching font service + selftest
[sched] user task 'FONTSRV.SKA' pid=9 ... entry=0x400000 (ELF, W^X)
[fontsrv] framebuffer NOT available (headless?), rendering-only mode
[fontsrv] font service online (FreeType, FONT_PORT=10)
[pchfnt] request: font=/FONTS/RESOURCEHANROUNDEDCN-MEDIUM.TTF size=40 (utf8) text="SukiOS FreeType"
[fontsrv] loaded font /FONTS/RESOURCEHANROUNDEDCN-MEDIUM.TTF (14423652 bytes, 1 faces)
[fontsrv] rendered req=1 glyphs=15 hits=3 bbox=314x129 status=0
[pchfnt] done: status=0 glyphs=15 cache_hits=3 bbox=314x129
```

结论：14.4 MB 资源圆体 TTF 被 FreeType 成功解析（1 face），15 个字形光栅化成功
（bbox 314×129，符合 40 px 字号 × 15 字符的量级），字形缓存命中 3 次（重复字符），
IPC 请求/回执完整闭环，全程零 panic。

### 8.3 已知限制（如实记录）

1. **headless 下不写屏**：`-display none` 时 `SYS_FRAMEBUFFER_MAP` 返回 `enabled=0`，
   display-server 与 fontsrv 都降级；上面的验证证明的是「加载 + 光栅化 + 缓存 + IPC」
   链路，像素上屏需在带图形输出的 QEMU/物理机上肉眼确认。
2. **shell 交互键无法自动注入**：QEMU headless 下 `sendkey` 不注入键盘中断
   （QEMU 限制），方向键/Tab/光标编辑需在图形窗口**手动**测试。

### 8.4 需手动测试（请用户在图形窗口执行）

```bash
export PATH="$(pwd)/opt/bin:$PATH"
make run        # 或 make iso 后从 ISO 启动
```

| 步骤 | 操作 | 预期 |
|---|---|---|
| 1 | 依次输入几条命令（如 `help`、`ls`、`pwd`） | 正常执行 |
| 2 | 按 **↑** 若干次 | 逐条回滚历史命令，行被替换并重绘 |
| 3 | 按 **↓** 回到末尾 | 最终回到空的新行 |
| 4 | 输入 `cat /F` 后按 **Tab** | 补全为 `/FONTS/`（目录补 `/`） |
| 5 | 输入不唯一前缀后按 **Tab** | 补齐公共前缀并换行列出候选，提示符与整行重印 |
| 6 | 输入 `echo hello`，按 **←** 移动光标到中间，插入/删除字符 | 光标处插入、右侧字符右移；Backspace 删前、Delete 删后 |
| 7 | 按 **Home** / **End** | 跳到行首 / 行尾 |
| 8 | 执行 `exec pchfnt --font /FONTS/RESOURCEHANROUNDEDCN-MEDIUM.TTF --text "你好 SukiOS" --size 48 --x 100 --y 200` | 屏幕 (100,200) 处出现渲染出的文字 |
| 9 | 执行 `exec pchfnt --font /FONTS/RESOURCEHANROUNDEDCN-MEDIUM.TTF --text-unicode "0x4E2D 0x6587 72 0x69" --size 40` | 按 Unicode 码点渲染「中文hi」 |

---

## 九、文件清单

**新增**
- `user/lib/shims/setjmp.h`、`user/lib/setjmp.S`（setjmp/longjmp）
- `user/lib/freetype_shim.h`、`user/lib/freetype_shim.c`（FreeType 标准库适配）
- `user/lib/ftmodule_min.h`（精简模块注册表）
- `user/font_ipc.h`（字体 IPC 协议）
- `user/fontsrv.c`（字体服务）、`user/apps/pchfnt.c`（渲染工具）

**修改**
- `user/shell.c`（编辑器/历史/Tab/光标；启动时拉起 fontsrv + pchfnt 自检）
- `user/input_server.c`（PS/2 `e0` 扩展键 → ANSI 转义）
- `user/lib/shims/dirent.h`（补 `DT_*` 常量）
- `user/lib/stdlib.c`（新增 `qsort`）、`user/lib/string.c`（新增 `memchr`）
- `kernel/syscall/syscall.c`（`exec_read_file` 改分块读；`EXEC_ELF_MAX` 64 KiB→1 MiB）
- `include/ipc/port.h`、`kernel/ipc/port.c`、`user/lib/suki.h`（新增 `FONT_PORT=10`）
- `Makefile`（FreeType 库/字体程序规则、`.DEFAULT_GOAL := all`、磁盘字体与程序布局）
- `README.md`、`NOTICE`（FreeType 组件与许可声明）
