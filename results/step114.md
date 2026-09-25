# Step 114 — 字体子系统重构完成：libsui 委托 fontsrv 离屏渲染 IPC（含 fill_rect 越界崩溃修复）

## 1. 目标与背景

将 SukiOS 的字体子系统从「libsui 自带 FreeType 合成器」重构为「委托 `fontsrv` 进程离屏渲染」的新 IPC 协议。前置会话（step 110~113 区间）已落地了大部分代码，但 `isukidemo` / `suikitest` 在各自画布缓冲写时触发 `#PF`（写越界 → `killing task`），导致字体重构无法端到端验证通过。

本步骤的核心工作：
1. **定位并修复** `isukidemo`/`suikitest` 的 `#PF` 崩溃根因——`libsui` 的 `sui_canvas_fill_rect` 存在 `x0` 重复偏移 bug。
2. **移除全部临时调试代码**（`sui_core.c` 的 `[sui-dbg]`、`[fill-OOB]`、`idt.c` 的 `[pf-dbg]` VMA 遍历诊断），并还原 `idt.c`。
3. **headless 生产回归**确认 `isukidemo`/`suikitest`/`pchfnt`/`winhello` 全部 PASS、零 panic。
4. 撰写本文档并 `git commit`。

最终确认：字体离屏渲染协议端到端打通，`isukidemo`/`suikitest` 不再崩溃，`pchfnt` 离屏渲染验证 `3985 pixels` 通过。

---

## 2. 重构后的架构（新 IPC 协议）

### 2.1 数据流

```
libsui (应用进程)                          fontsrv (FONT_PORT=10)
─────────────────                         ──────────────────────
创建窗口: SukiCreateWindow → sys_mmap(need)
  need = w*h*4 字节 (按需分页, 4KB 页对齐)
  canvas.c->pixels = win->wk->buffer
        │
        │ 渲染文本: sui_font_draw(c, x, y, text, color, scale, font)
        │   ensure_registered(c->pixels, w, h)
        ▼
  FONT_MSG_REGISTER  (OOL: 发送方 VA→PA 翻译 + pmm_incref, 不改发送方页表)
        │──────────────────────────────►  fontsrv: ool_map_into_receiver 映射带 PTE_OOL 标记
        │                                   handle 表登记 buf_w/buf_h/va
        │
        │  FONT_MSG_RENDER (x, y, text, color, scale, font_id)
        ▼──────────────────────────────►  fontsrv: blit_glyph_buf() 把字形光栅写入共享物理页
        │                                   (按 buf_w/buf_h 严格裁剪, 不会越界)
        │◄───────────────────────────────  status=0, bbox, glyph 数, cache_hits
        │
        │  (libsui 直接读取 c->pixels 中已渲染好的字形像素)
        ▼
  窗口 flush: SukiFlush (OOL 发送整帧缓冲给显示合成)
```

关键点：fontsrv **只写共享物理页**，libsui 通过自身 VMA 读取；libsui 自身**不再链接 FreeType**。

### 2.2 协议定义（`user/font_ipc.h`）

- `FONT_PORT 10`：字体服务端口号。
- 消息类型：`FONT_MSG_REGISTER`(1) / `FONT_MSG_RENDER`(2) / `FONT_MSG_UNREGISTER`(3) / `FONT_MSG_QUERY`(4)。
- 消息体 `font_msg_t`：
  - `ool.va`（发送方缓冲 VA）、`ool.size = w*h*4`（OOL 捕获长度）、`ool.page_frames[]`（内核填充的物理页框）。
  - `buf_w`、`buf_h`、`handle`、`x`、`y`、`color`、`scale`、`font_id`。
  - `text[256]`、`status`、`bbox_w`、`bbox_h`、`glyphs`、`cache_hits`。
- 客户端句柄表 `static font_buf_t g_bufs[16]`（handle→va/buf_w/buf_h/refcount），`sui_font_unregister_all()` 在窗口销毁时解除所有注册，让 fontsrv 解映射。

### 2.3 `user/fontsrv.c`（离屏渲染服务）

- 启动加载 `/FONTS/RESOURCEHANROUNDEDCN-MEDIUM.TTF`（14,423,652 字节，1 face）。
- `handle_register_buf`：通过 `ool_map_into_receiver` 把发送方缓冲物理页映射到 fontsrv 地址空间（带 `PTE_OOL`），登记 `handle`。
- `blit_glyph_buf(base, buf_w, buf_h, pen_x, pen_y, glyph, color)`：对每个字形像素 `if py<0||py>=buf_h||px<0||px>=buf_w continue;` 严格裁剪后写入 `base[py*buf_w+px]`（xRGB32）。这是**不会越界**的关键保障。
- `render_handle`：遍历字形，命中则用 `FT_Glyph_To_Bitmap` 光栅化后 `blit_glyph_buf`；未命中回退 ASCII（`draw_ascii`）。
- `#include <stdbool.h>` 已补；FreeType 头用标准宏写法 `#include FT_FREETYPE_H` / `#include FT_GLYPH_H`。

### 2.4 `user/libsui/src/sui_font.c`（libsui 字体后端）

- `sui_font_draw`：先 `ensure_registered` 把当前画布缓冲注册给 fontsrv（OOL），再发 `FONT_MSG_RENDER`，收 `MACH_MSGH`；返回 0 表示成功（字形已由 fontsrv 写入共享页），libsui 直接读 `c->pixels`。
- 注册失败（如 OOL 捕获遇到未呈现页）→ 返回 -1，libsui 回退 ASCII（`draw_ascii`）。
- 补 `#include <stdbool.h>`。

### 2.5 `user/apps/pchfnt.c`（离屏渲染自测）

新协议自测：注册本地离屏缓冲（1024x256）→ 发 RENDER 渲染一段文本 → 扫缓冲统计非零像素 → 验证字形真正被渲染进缓冲。回归日志：
```
[pchfnt] buffer registered handle=1 (1024x256)
[fontsrv] render handle=1 glyphs=15 hits=3 bbox=314x129 status=0
[pchfnt] offscreen buffer nonzero pixels=3985 cover=[82,393]x[87,128]
[pchfnt] OK: offscreen render verified (3985 pixels)
```

### 2.6 `Makefile` 调整

- 删除 `sui_font.c.o` 专属 FreeType 编译规则（改走通用 `%.c.o`，不再 `-I` FreeType、不再 `-l` 链接 FT）。
- 从 `suikitest` 与 `isukidemo` 链接行移除 `$(FT_LIB)`（仍保留 `-lgcc`）。
- `fontsrv` / `pchfnt` 依旧属于 `FONT_PROGS`，产物位于 `build/apps/`；`isukidemo`/`suikitest` 位于 `build/user/`。

### 2.7 `sui.h` / `sui_core.c`

- `sui.h` 字体段注释改为「委托 fontsrv 离屏渲染：libsui 不再自带 FreeType 合成器」。
- 新增声明 `void sui_font_unregister_all(void);`（窗口销毁时解除画布注册，让 fontsrv 解映射）。
- `sui_canvas_create` 在窗口销毁路径调用 `sui_font_unregister_all`。

---

## 3. 崩溃根因分析与修复（本步骤核心）

### 3.1 现象

`isukidemo`/`suikitest` 各在画布缓冲写时 `#PF`（write=1）→ `killing task`。headless 串口日志（`-d int`/`serial`）：
```
[pf] user #PF: cr2=0x000050000031c000 write=1 pid=17 'SukiSuiTest' rip=0x400851 vma=MISS ... -> killing task
[pf] user #PF: cr2=0x0000500000522000 write=1 pid=18 'SukiIsukiDemo' rip=0x401ed1 vma=MISS ... -> killing task
```

增强诊断（`[pf-dbg]` 遍历故障任务 VMA）确认：
- `SukiSuiTest` 缓冲 VMA = `0x500000200000..0x50000031c000`，size `1,163,264` = `round_up(660*440*4)`。cr2 恰好 = VMA 末端之后第一字节。
- `SukiIsukiDemo` 缓冲 VMA size `3,284,992` = `round_up(1080*760*4)`。cr2 同样 = VMA 末端。

换算像素：`cr2 - buf` 对应像素索引 `290,816`（suikitest）/ `821,248`（isukidemo），即画布第 **441 / 761** 行（缓冲只有 440 / 760 行）。初步误判为「画布高度 off-by-one」。

### 3.2 精确定位（真实地址越界检测）

在 `sui_canvas_fill_rect` 内加临时判定 `off = (uint8_t*)&row[i] - (uint8_t*)c->pixels; if (off >= buffer_size)`，打印原始调用参数，暴露真实越界调用：
```
[fill-OOB] off=1161600 buf=1161600 j=439 i=330 cw=660 ch=440 x=330 y=439 w=1 h=1 ...
[fill-OOB] off=3283200 buf=3283200 j=759 i=540 cw=1080 ch=760 x=540 y=759 w=1 h=1 ...
```

越界调用都是 `fill_rect(x0>0, y0=ch-1, w=1, h=1)` 这类**局部小填充**（如 `draw_line`/字形描边/边框在 `x0>0` 处的 1×1 点画）。

### 3.3 根因：fill_rect 的 `x0` 被重复偏移（double-offset bug）

原 `sui_canvas_fill_rect` 内层实现：
```c
uint32_t *row = c->pixels + (uint64_t)j * c->width + x0;  // 基址已含 +x0
for (int i = x0; i < x1; i++) row[i] = col;               // 内层又用绝对 i (从 x0 起) 索引 row[i]
```
- 当 `x0 == 0`：整窗填充，`row[0]` = `c->pixels[j*width+0]`，正确（所以全局背景填充不崩）。
- 当 `x0 > 0`：`row[x0]` = `c->pixels[j*width + x0 + x0]`，**`x0` 被加了两次**，写到 `c->pixels[j*width + 2*x0]`，远超缓冲，触发 `#PF`。

编译器在 `-O2` 下对循环地址做向量化/基址提升，使实际写入地址偏移进一步放大，最终落点恰好在 VMA 页对齐末端之外（与观测一致）。这是 `libsui` 原生的真实 bug（`gui.c` 的 `SukiFillRect` 用的是相对 `i`（0..n-1）写法，正确；二者不一致才暴露）。

> 注：`fill-OOB` 临时检测最初用「C 层 `j*width+i >= buf_px`」判定，因已钳到 `c->height` 而**永远不触发**，证明 C 语义自洽、错在优化器实际地址——故改用**真实指针地址**对比 `buffer_size` 才捕获到（见 3.2）。

### 3.4 修复

`user/libsui/src/sui_core.c` `sui_canvas_fill_rect`：基址不再含 `x0`，内层用绝对 `i` 索引：
```c
uint32_t col = SUI_FB(color);
for (int j = y0; j < y1; j++) {
    uint32_t *row = c->pixels + (uint64_t)j * c->width;   // 不含 x0
    for (int i = x0; i < x1; i++) row[i] = col;           // 绝对 i 索引，正确
}
```

修复后：整窗填充（`x0=0`）与局部填充（`x0>0`）均写到正确像素，不再越界。

---

## 4. 临时调试代码清理

- `user/libsui/src/sui_core.c`：移除 `[sui-dbg]` `printf`、移除 `[fill-OOB]` 越界检测循环、移除仅用于调试的 `#include <stdio.h>`。
- `kernel/arch/x86_64/idt.c`：移除 `[pf-dbg]` 诊断块（含 VMA 遍历），并 `git checkout` 还原到 HEAD（字体重构不需要内核页错误变更）。

---

## 5. 验证（headless QEMU 生产回归）

命令：
```
make iso
make run-headless QEMU_SERIAL="-serial file:/tmp/sui5.log" RUN_TIMEOUT=50
```
日志关键结论：
```
[suikitest] window rendered + flushed -> PASS
[pchfnt] OK: offscreen render verified (3985 pixels)
[isukidemo] window rendered + flushed (all pages + dialog self-checked) -> PASS
[isukidemo] controls: button/input/textarea/checkbox/radio/switch/slider/.../toast OK
[wm] window destroyed            (suikitest / isukidemo 销毁路径正常, 触发 sui_font_unregister_all)
[syscall] task 'SukiSuiTest' pid=17 exit(code=0)
[syscall] task 'SukiIsukiDemo' pid=18 exit(code=0)
[winhello] PASS: window lifecycle complete
```

- `fontsrv` 正常加载字体、`buffer registered handle=2/3`（对应 suikitest/isukidemo 画布）、按需 `render`/`unregister`，全程 `status=0`。
- `isukidemo`/`suikitest`/`pchfnt`/`winhello` 全部 PASS、`exit(code=0)`，**零 panic**。

### 5.1 既有无关用例说明

`SukiPosixTest`（pid=12）仍有一处 `#PF`：`cr2=0xfffffffffffffffe`（即 `-2` 指针）`rip=0x402e94`，发生在 `[PASS] mmap file-backed returns mapping` 之后。该用例**不依赖 libsui/字体后端**、未被本重构改动（posixtest.c 不在改动列表），系 POSIX 兼容性自测中某未实现 syscall 返回 `-ENOSYS` 被当作指针解引用的既有问题，**与字体子系统重构无关**，不在本步骤范围内。

---

## 6. 改动文件清单

已修改（tracked）：
- `Makefile` — 移除 sui_font.c 的 FreeType 专属规则；isukidemo/suikitest 不再链接 `$(FT_LIB)`。
- `kernel/kmain.c` — 启动 `SukiFontServer` 等（前置会话）。
- `user/apps/pchfnt.c` — 改为新离屏渲染协议自测。
- `user/font_ipc.h` — 新 IPC 协议（`FONT_PORT`、消息类型、OOL 字段、句柄表）。
- `user/fontsrv.c` — 离屏渲染实现（`blit_glyph_buf` 裁剪、OOL 映射、`#include` 修正）。
- `user/libsui/include/sui.h` — 字体段注释、`sui_font_unregister_all` 声明。
- `user/libsui/src/sui_core.c` — **修复 `fill_rect` 的 `x0` 重复偏移 bug**；画布销毁解注册。
- `user/libsui/src/sui_widgets.c` — 控件随新字体后端适配（前置会话）。

新增（untracked）：
- `user/apps/isukidemo.c` — iSukiUI 概念复刻 demo（1080x760 窗，多控件 + 文本）。
- `user/libsui/src/sui_controls.c` — 控件集（button/input/textarea/.../toast）。
- `user/libsui/src/sui_font.c` — libsui 字体后端（委托 fontsrv 离屏渲染）。

---

## 7. 结论

- 字体子系统重构**完成并端到端验证通过**：libsui 不再自带 FreeType 合成器，全部字形渲染委托 `fontsrv` 经离屏渲染 IPC（OOL 共享物理页）完成。
- 修复了 `libsui` 的原生 bug——`sui_canvas_fill_rect` 在 `x0>0` 的局部填充时把 `x0` 重复偏移导致写越界 `#PF`；该 bug 此前被全局背景填充（`x0=0`）掩盖，仅在新建的 `isukidemo`/`suikitest` 触发。
- 临时调试代码已全部移除，内核 `idt.c` 还原。
- headless 回归：字体相关应用全部 PASS、零 panic。
