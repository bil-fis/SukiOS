# step94 —— 注册表改为 Windows 风格层级键树（`.sre`）并修复 ISO 大文件读取

## 0. 背景与目标

用户提供的 `configs/default/layers_example/*.sre.json` 采用「类 Windows 注册表」结构：
**每个节点是一个「键」(key)，键下可挂「值」(values) 与「子键」(subkeys)**。
本轮把 SukiOS 现有的扁平路径注册表（`configs/default/*.reg`，magic `SUKREG` v1）整体改造为
层级键树格式，并把文件后缀从 `.reg` 改为 `.sre`。

经用户审核确认的方案：
1. **`.sre` 仍为二进制**，保留 header 的 magic/version/CRC32/generation/flags，body 改为真正的键树；
   `.sre.json` 作为人类可编辑源，由 `tools/registry_editor.py` 编译成 `.sre`；内核 loader 改写为解析树。
2. **采纳示例 `.sre.json` 的新数据**作为规范默认（旧 `.reg` 的 `Boot.Timeout` 等条目不再保留）。
3. **转换后删除** `layers_example/*.sre.json`，同时给 Python 注册表管理器新增
   「生成/导出 JSON」与「导入 JSON 生成注册表」能力，并把 tkinter GUI 改成真正的 Windows 注册表样式。

---

## 1. `.sre` 二进制格式 v2（与 Python / C 双侧逐字段一致）

### 1.1 Header（固定 64 字节）

| 偏移 | 大小 | 字段 | 说明 |
|---|---|---|---|
| 0..8   | 8  | `magic`       | `"SUKREG\0\0"`（不变） |
| 8..12  | 4  | `version`     | **2**（v1 已废弃） |
| 12..16 | 4  | `flags`       | 整库标志（默认 `FLAG_SYSTEM=2`） |
| 16..24 | 8  | `root_offset` | 根键节点文件偏移，恒为 64 |
| 24..32 | 8  | `entry_count` | v2 未使用（置 0，保留兼容） |
| 32..40 | 8  | `generation`  | 代数（离线基线=1） |
| 40..48 | 8  | `timestamp`   | 时间戳（离线=0） |
| 48..52 | 4  | `crc32`       | 覆盖 `[0..48)` + body 的 CRC32 |
| 52..60 | 8  | `body_size`   | **v2 新增**：body 真实字节数 |
| 60..64 | 4  | `reserved`    | 补齐到 64 |

**`body_size` 新增原因（关键工程修正）**：CRC 与解析边界改为以 header 记录的**真实 body 长度**为准，
与「FS 按扇区读回时可能追加的尾部填充」解耦。若按「读回字节数 - 64」计算 CRC，一旦读取层多返回
了尾部填充（0 字节），CRC 必然失配。加入 `body_size` 后，无论读回多少尾部填充，CRC/解析都只覆盖真实 body。

### 1.2 Body（键树前序序列化，逐字节递归）

```
serialize_key(key):
  name_len:      u32
  name:          name_len 字节（UTF-8；根键名如 "System"/"User"/"Services"）
  flags:         u32
  value_count:   u32
  [值表] 每个值：
      vname_len: u32
      vname:     vname_len 字节（可为 0 → 该键的「默认值」）
      vtype:     u32
      vdata_len: u64
      vdata:     vdata_len 字节
  subkey_count:  u32
  [子键表] 每个子键：内嵌一个 serialize_key()（前序递归）
```

- 值排序：Python 侧按名字排序落盘（保证可复现）；C 解析不依赖顺序。
- 值类型枚举：`none=0, int64=1, uint64=2, bool=3, string=4, binary=5, link=6`。
- 键标志：`READONLY=1, SYSTEM=2, VOLATILE=4, HIDDEN=8`。
- **查值路径**：形如 `"System/Display/Width"` —— 首段=根键名，中间段=子键名，末段=值名。

---

## 2. Python 侧：`tools/registry_editor.py`（整体重写）

### 2.1 内存模型
- `class RegKey`：`name` / `flags` / `values: {vname: (vtype, raw_bytes)}` / `subkeys: {sname: RegKey}`。
- 提供 `SetValueRaw/GetValue/DeleteValue/DeleteSubkey` 等。

### 2.2 序列化 / 反序列化（与 C 完全对齐）
- `SerializeKey(key) -> bytes`、`ParseKeyNode(base, off) -> (RegKey, end_off)`（带越界校验，抛 `ValueError`）。
- `class RegistryHive`：`Save/ Load`，写/读 header（含 `body_size`）并做 magic/version/CRC32/root_offset 校验。

### 2.3 JSON 互操作（满足「可编辑源」需求）
- `JsonToKey(obj)`：`{根名: {values, subkeys}}` → `RegKey`。
  - **容错**：节点中除 `values`/`subkeys` 之外的额外键也当作子键处理（兼容示例里省略 `"subkeys"` 包裹层的手写写法）。
- `KeyToFileJson(key)`：`RegKey` → `{根名: {values, subkeys}}`（`binary/link` 以 base64 字符串表示）。
- 命令：
  - `export <in.sre> <out.json>` —— **生成 JSON 供人类修改**。
  - `compile <in.json> <out.sre>`（别名 `import`）—— **导入 JSON 生成注册表**。

### 2.4 Windows 注册表风格 GUI（`RunGui`）
- 左侧 `ttk.Treeview` 键树；右侧 `ttk.Treeview` 值列表（列：名称/类型/数据）。
- 菜单：文件（新建/打开/保存/另存为/**导出 JSON**/**导入 JSON**/退出）、编辑（新建键/新建值/删除键/删除值）、帮助。
- 双击值编辑；值编辑对话框含 值名/类型（下拉）/数据。打开/保存默认 `.sre`。
- 支持「默认值」（空值名）。懒加载 tkinter（不干扰 CLI）。

### 2.5 默认数据与 CLI
- 内嵌 `DEFAULT_SYSTEM_JSON` / `DEFAULT_USER_JSON` / `DEFAULT_SERVICES_JSON`（system/user 直接取自示例；
  services 无示例，沿用旧 `services.reg` 的 `Display/Fs/Input/Net.Autostart=true`）。
- `gen-defaults [dest]`：在 `dest`（默认 `configs/default`）生成 `system.sre`/`user.sre`/`services.sre`。
- 无参运行 → GUI；`edit [file]` → GUI 打开指定文件。

---

## 3. 内核侧改动

### 3.1 `include/kernel/registry.h`
- `SUKREG_VERSION` 1 → **2**；header 注释更新为 v2 层级键树布局；`sukreg_header_t` 的
  `uint32_t reserved[3]` → `uint64_t body_size; uint32_t reserved;`。

### 3.2 `kernel/registry/hive.c`（重写解析器）
新增（全部带越界防御）：
- `ParseKeyHeader(base,total,koff,key_node_t*)`：解析一个键节点头，返回 name/flags/值表偏移/子键表偏移。
- `SkipKey(...)`：递归求一个键节点结束偏移。
- `FindSubkeyByName(...)`：在父键子键表内按名查找。
- `FindValueInKey(...)`：在键值表内按名查找。
- `FindValueByPath(base,total,root_off,"System/Display/Width",...)`：按 `/` 拆段导航键树后取值。
- `RegistryParseSystem(...)`：校验 magic/version/CRC32(`[0..48)`+body)/root_offset（用 `body_size`），
  再按路径提取：
  - `System/Display/{Width,Height,Bpp}`（`uint64`，判定兼容 `INT64`/`UINT64`）
  - `System/Kernel/KdrEnabled`（`bool`）
  - `System/Boot/Verbose`（`bool`）

### 3.3 修复的 bug #1：`subkeys_off` 偏移错误
`ParseKeyHeader` 最初把 `subkeys_off` 记成 `subkey_count` 字段的偏移，而子键节点实际从其后 **+4** 字节开始，
导致导航/跳过从错误位置解析、查值失败。修正为「`subkey_count` 之后即第一个子键节点」。

### 3.4 数据与常量清理
- 删除 `configs/default/system.reg`、`user.reg`、`services.reg` 与整目录 `configs/default/layers_example/`。
- 生成 `configs/default/system.sre`(799B)、`user.sre`(252B)、`services.sre`(273B)。

---

## 4. 修复的 bug #2（潜在缺陷）：`IsoReadFile` 512 字节分块导致重复读同块

**现象**：内核启动报 `[boot] config: hive parse/CRC failed, using defaults`，注册表回退默认值。

**定位过程**（严格用 QEMU + serial 日志 + 独立 C 对照，未用 GDB）：
- 独立 host C 测试（与内核 `hive.c` 逐字一致）对 `configs/default/system.sre` 校验：CRC/版本/root_offset 全通过，
  值查找正确 → 说明文件与算法都正确。
- 内核加临时诊断（`kprintf`）打印：`magic=SUKR ver=2 body_size=735 crc_stored=c7e658a0`（均正确），
  但 `calc=fec50ca3`；进一步分段计算：`c_hdr_only == Python(0xc97882d6)`（算法与头部数据对），
  `c_body_only=0x9a7ef4e7 ≠ Python(0xa35da0e4)` → **body 数据不一致**。
- dump 缓冲区间：`data[0..512)` 与源文件逐字节一致，`data[512..799)` **全为 0** → 第二次读取被清零。

**根因**：`IsoReadFile` 原实现按 **512 字节分块**读（`uint8_t tmp[512]`）。799 字节文件需要两次 `iso_read`，
而两次都会 `IsoReadExtent` 覆盖 **同一批 LBA**（第二次 `off=512` 仍落在 0 号块）。ATAPI 对**同一 LBA 的重复读**
偶发返回全零扇区（`CdromReadBlocks` 的全零重试基于「整段非零」判据对此不敏感），于是文件后半段被清零。

- 旧 `.reg` 文件均 < 512 字节（403/192/201 B），**只读一次** → 一直正常，缺陷长期潜伏；
- 新的层级 `.sre`（799 B）首次触发该缺陷。

**修复**（`kernel/fs/iso9660.c`）：`IsoReadFile` 改为**直接读入调用方缓冲**，分块取较大值（上限 64KiB），
保证每个 LBA 只读一次、无重叠：

```c
while (total < cap) {
    uint32_t chunk = (uint32_t)(cap - total);
    if (chunk > 65536u) chunk = 65536u;
    int rc = iso_read(h, buf + total, chunk, &nr);
    if (rc < 0) { iso_close(h); return rc; }
    if (nr == 0) break;
    total += nr;
}
```

---

## 5. 路径 / 调用点更新（`.reg` → `.sre`）

| 文件 | 改动 |
|---|---|
| `kernel/kmain.c` | `Stage3ReadConfig("/sys/configs/system.sre", ...)`；日志 `loaded system.sre`；注释同步 |
| `Makefile`（iso-single 896–898、disk 945–947、注释 943） | `configs/default/*.sre` → 镜像 `SYS/CONFIGS/*.sre` |
| `grub/grub.cfg` | 注释 `system.reg` → `system.sre` |
| `kernel/fs/fd.c` | 注释 `*.reg` → `*.sre` |
| `include/kernel/registry.h` | 版本/布局注释 |
| `configs/default/` | 删除 `*.reg` 与 `layers_example/`，新增 `*.sre` |

全仓检索确认无残留 `.reg` 引用。

---

## 6. 验证

### 6.1 格式自测（host，Python + 独立 C）
- `gen-defaults` 生成三份 `.sre`；`export`→`compile` 往返 `system.sre`/`user.sre`/`services.sre` 均 `cmp` 一致。
- 独立 C 程序（与内核解析逐字一致）在「**加 4KiB 尾部填充**」下校验：magic/version/CRC(root=`c7e658a0`)/
  `Display/Width=1024`/`Boot/CustomLogo/Path`/`Services/Input/Autostart` 全部正确 → 证明 `body_size` 抗填充。

### 6.2 QEMU 生产场景回归（ISO 启动，`-serial file` 落盘后 `grep`）
```
make iso-single
qemu-system-x86_64 -machine pc -cpu qemu64 -smp 1 -m 2G -no-shutdown \
    -display none -serial file:/tmp/boot.log -boot d -cdrom build/SukiOS-single.iso
```
日志关键行：
```
[boot] config: loaded system.sre (generation=1)
[boot] config: display 1024x768
[sched] user task 'SukiInputServer' ...
[sched] user task 'SukiDisplayServer' ...
[shell] SukiOS shell online (Ring3, bash-like)
```
- 异常扫描（panic / triple fault / #GP / #PF / Oops / BUG）**零命中**；
- 三个 Ring3 服务（SukiInputServer / SukiDisplayServer / SukiShell）正常上线。

### 6.3 FAT 磁盘路径
`make disk` 后 `mdir -i build/disk.img ::/SYS/CONFIGS`：
```
system   sre   799
user     sre   252
services sre   273
```
（旧 `.reg` 已不再出现。）

---

## 7. 关键文件与常量索引

- 格式定义/工具：`tools/registry_editor.py`（`SUKREG_MAGIC=b"SUKREG\0\0"`、`SUKREG_VERSION=2`、`HEADER_SIZE=64`、`HEADER_CRC_END=48`）
- 内核头：`include/kernel/registry.h`（`SUKREG_VERSION=2`、`sukreg_header_t`）
- 内核解析：`kernel/registry/hive.c`（`ParseKeyHeader/SkipKey/FindSubkeyByName/FindValueInKey/FindValueByPath/RegistryParseSystem`）
- 读取修复：`kernel/fs/iso9660.c`（`IsoReadFile`）
- 启动读取：`kernel/kmain.c`（`Stage3ReadConfig` / `Stage3LoadConfigAndKdr`，路径 `/sys/configs/system.sre`）
- 构建：`Makefile`（iso-single / disk 的 `SYS/CONFIGS` 复制）
- 数据：`configs/default/system.sre|user.sre|services.sre`

## 8. 遗留说明
- GUI 为 tkinter 桌面程序，无法在无显示的 CI/后台自动运行；已通过 Python 语法解析与 CLI（`gen-defaults`/`export`/`compile`）功能验证。Pylance 对 tkinter 桩的两条类型提示为类型检查器误报（行号指向空白行/`def`），不影响运行。
- `IsoReadFile` 的 512→64KiB 分块改动同时消除了「重复读同块」的隐患，对其它 >512 字节的内核文件读取（如未来更大的配置/资源）同样受益。
