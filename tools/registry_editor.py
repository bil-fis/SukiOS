#!/usr/bin/env python3
# tools/registry_editor.py
#
# SukiOS 安全配置存储（无签名版）——外部编辑工具 + 格式参考实现
#
# 作用：
#   1) 定义 SukiRegistry Hive 二进制格式的规范实现（Python 侧，C 内核侧将复用同一格式）。
#   2) 提供 tkinter GUI，用于在开发机/宿主机上离线查看、修改、新增配置键。
#   3) 提供 gen-defaults 子命令，把 configs/default/ 下的默认配置生成到仓库
#      （构建系统随后把它们塞进磁盘镜像的 /sys/configs/）。
#
# 命名：按项目规范，新增函数使用 PascalCase（NameFunctionLikeThis）。
#
# 格式要点（与方案文档一致，并做两处工程化修正）：
#   - 路径统一用 Unix 风格 '/'（方案文档里写的 '\System\...' 改为 '/System/...'）。
#   - CRC32 仅用于"意外损坏检测"，不宣称能防离线篡改（离线有原始磁盘访问者可重算）。
#   - generation 单调递增，用于在线（进程）场景防回滚/防重放；离线场景基线随重启归零，
#     属"移除签名"后的固有代价，如实记录、不夸大。
#   - 条目在磁盘上以扁平数组存储（type/flags/name_len/name/data_len/data），
#     加载时按路径构建层级树；比文档里的 parent/child/sibling 偏移更不易出错。
#
# 依赖：tkinter 仅在实际打开 GUI 时惰性导入，因此 gen-defaults 在无显示环境也能跑。

import os
import sys
import struct

# ---------------------------------------------------------------------------
# 常量
# ---------------------------------------------------------------------------
SUKREG_MAGIC = b"SUKREG\0\0"   # 8 字节
SUKREG_VERSION = 1
HEADER_SIZE = 64               # 头部固定 64 字节（含补齐）
HEADER_CRC_END = 48            # crc32 字段（offset 48）之前的字节数，校验不覆盖 crc 字段本身

# 值类型
TYPE_NONE = 0
TYPE_INT64 = 1
TYPE_UINT64 = 2
TYPE_BOOL = 3
TYPE_STRING = 4
TYPE_BINARY = 5
TYPE_LINK = 6

# 条目标志
FLAG_READONLY = 1 << 0
FLAG_SYSTEM = 1 << 1
FLAG_VOLATILE = 1 << 2
FLAG_HIDDEN = 1 << 3

TYPE_NAME = {
    TYPE_NONE: "none", TYPE_INT64: "int64", TYPE_UINT64: "uint64",
    TYPE_BOOL: "bool", TYPE_STRING: "string", TYPE_BINARY: "binary",
    TYPE_LINK: "link",
}
TYPE_NAME_LIST = [TYPE_NAME[t] for t in sorted(TYPE_NAME.keys())]


# ---------------------------------------------------------------------------
# CRC32（标准多项式 0xEDB88320，与方案文档一致；C 侧将用完全相同实现）
# ---------------------------------------------------------------------------
def Crc32(data: bytes, crc: int = 0xFFFFFFFF) -> int:
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1)) & 0xFFFFFFFF
    return crc ^ 0xFFFFFFFF


# ---------------------------------------------------------------------------
# Hive 实现
# ---------------------------------------------------------------------------
class RegistryHive:
    """SukiRegistry Hive：内存中为一棵路径树，磁盘上为 header + 扁平条目数组。"""

    def __init__(self):
        # 扁平存储：path(str) -> dict(type, flags, value:bytes)
        self.entries = {}
        self.generation = 0
        self.timestamp = 0
        self.flags = 0

    # ---- 序列化辅助 ----
    def _EncodeValue(self, vtype: int, value) -> bytes:
        if vtype == TYPE_INT64:
            return struct.pack("<q", int(value))
        if vtype == TYPE_UINT64:
            return struct.pack("<Q", int(value))
        if vtype == TYPE_BOOL:
            return struct.pack("<B", 1 if value else 0)
        if vtype == TYPE_STRING:
            return value.encode("utf-8") if isinstance(value, str) else bytes(value)
        if vtype in (TYPE_BINARY, TYPE_LINK):
            return bytes(value)
        return b""

    def _DecodeValue(self, vtype: int, raw: bytes):
        if vtype == TYPE_INT64:
            return struct.unpack("<q", raw[:8])[0]
        if vtype == TYPE_UINT64:
            return struct.unpack("<Q", raw[:8])[0]
        if vtype == TYPE_BOOL:
            return bool(raw[0]) if raw else False
        if vtype == TYPE_STRING:
            return raw.split(b"\0", 1)[0].decode("utf-8", "replace")
        return raw

    # ---- 读写磁盘 ----
    def Save(self, path: str):
        # 扁平条目数组
        blob = bytearray()
        for p in sorted(self.entries.keys()):
            e = self.entries[p]
            name = p.encode("utf-8")
            data = e["value"]
            blob += struct.pack("<II", e["type"], e["flags"])
            blob += struct.pack("<I", len(name)) + name
            blob += struct.pack("<Q", len(data)) + data
        body = bytes(blob)

        header = bytearray(HEADER_SIZE)
        header[0:8] = SUKREG_MAGIC
        struct.pack_into("<I", header, 8, SUKREG_VERSION)
        struct.pack_into("<I", header, 12, self.flags)
        struct.pack_into("<Q", header, 16, 0)          # root_offset（扁平布局未用）
        struct.pack_into("<Q", header, 24, len(self.entries))
        struct.pack_into("<Q", header, 32, self.generation)
        struct.pack_into("<Q", header, 40, self.timestamp)
        # crc32 覆盖 [0:HEADER_CRC_END] + body
        crc = Crc32(bytes(header[0:HEADER_CRC_END]) + body)
        struct.pack_into("<I", header, 48, crc)
        # 52..64 保留/补齐

        with open(path, "wb") as f:
            f.write(header)
            f.write(body)

    def Load(self, path: str):
        with open(path, "rb") as f:
            raw = f.read()
        if len(raw) < HEADER_SIZE:
            raise ValueError("文件过小，不是合法 Hive")
        header = raw[0:HEADER_SIZE]
        if header[0:8] != SUKREG_MAGIC:
            raise ValueError("魔数不匹配，不是 SukiRegistry Hive")
        version = struct.unpack_from("<I", header, 8)[0]
        if version != SUKREG_VERSION:
            raise ValueError("版本不兼容: %d" % version)
        body = raw[HEADER_SIZE:]
        calc = Crc32(header[0:HEADER_CRC_END] + body)
        stored = struct.unpack_from("<I", header, 48)[0]
        if calc != stored:
            raise ValueError("CRC32 校验失败（文件损坏或被篡改）")
        self.flags = struct.unpack_from("<I", header, 12)[0]
        self.generation = struct.unpack_from("<Q", header, 32)[0]
        self.timestamp = struct.unpack_from("<Q", header, 40)[0]
        entry_count = struct.unpack_from("<Q", header, 24)[0]

        self.entries = {}
        off = 0
        for _ in range(entry_count):
            vtype, flags = struct.unpack_from("<II", body, off); off += 8
            name_len = struct.unpack_from("<I", body, off)[0]; off += 4
            name = body[off:off + name_len].decode("utf-8"); off += name_len
            data_len = struct.unpack_from("<Q", body, off)[0]; off += 8
            data = body[off:off + data_len]; off += data_len
            self.entries[name] = {"type": vtype, "flags": flags, "value": data}
        return self

    # ---- 增删改查 ----
    def GetValue(self, path: str):
        e = self.entries.get(path)
        if not e:
            return None
        return (e["type"], self._DecodeValue(e["type"], e["value"]), e["flags"])

    def SetValue(self, path: str, vtype: int, value, flags: int = 0):
        # READONLY 保护（仅用户态编辑时；内核加载不受此限）
        e = self.entries.get(path)
        if e and (e["flags"] & FLAG_READONLY):
            raise PermissionError("键 %s 为只读" % path)
        self.entries[path] = {"type": vtype, "flags": flags,
                              "value": self._EncodeValue(vtype, value)}
        self.generation += 1

    def DeleteValue(self, path: str):
        if path in self.entries:
            del self.entries[path]
            self.generation += 1

    def EnumChildren(self, path: str):
        """返回 path 下一级子键名列表（path 以 '/' 结尾或为空表示根）。"""
        prefix = path.rstrip("/")
        seen = set()
        for p in self.entries:
            if prefix and not p.startswith(prefix + "/"):
                continue
            rest = p[len(prefix) + 1:] if prefix else p
            if "/" in rest:
                seen.add(rest.split("/", 1)[0])
            else:
                seen.add(rest)
        return sorted(seen)

    def Keys(self):
        return sorted(self.entries.keys())


# ---------------------------------------------------------------------------
# 默认配置（构建系统复制到 /sys/configs/）
# ---------------------------------------------------------------------------
def BuildDefaults():
    sys_hive = RegistryHive()
    sys_hive.timestamp = 0
    sys_hive.SetValue("/System/Display/Width", TYPE_INT64, 1024, FLAG_SYSTEM)
    sys_hive.SetValue("/System/Display/Height", TYPE_INT64, 768, FLAG_SYSTEM)
    sys_hive.SetValue("/System/Display/Bpp", TYPE_INT64, 32, FLAG_SYSTEM)
    sys_hive.SetValue("/System/Network/DhcpEnabled", TYPE_BOOL, True, FLAG_SYSTEM)
    sys_hive.SetValue("/System/Network/Hostname", TYPE_STRING, "sukios", FLAG_SYSTEM)
    sys_hive.SetValue("/System/Kernel/PanicOnOops", TYPE_BOOL, True, FLAG_SYSTEM)
    sys_hive.SetValue("/System/Boot/Timeout", TYPE_INT64, 5, FLAG_SYSTEM)

    user_hive = RegistryHive()
    user_hive.SetValue("/User/Theme/Name", TYPE_STRING, "dark")
    user_hive.SetValue("/User/Locale", TYPE_STRING, "zh-CN")
    user_hive.SetValue("/User/Shell/HistorySize", TYPE_INT64, 100)

    svc_hive = RegistryHive()
    svc_hive.SetValue("/Services/Display/Autostart", TYPE_BOOL, True, FLAG_SYSTEM)
    svc_hive.SetValue("/Services/Fs/Autostart", TYPE_BOOL, True, FLAG_SYSTEM)
    svc_hive.SetValue("/Services/Input/Autostart", TYPE_BOOL, True, FLAG_SYSTEM)
    return {"system.reg": sys_hive, "user.reg": user_hive, "services.reg": svc_hive}


def GenDefaults(dest_dir: str):
    os.makedirs(dest_dir, exist_ok=True)
    for name, hive in BuildDefaults().items():
        p = os.path.join(dest_dir, name)
        hive.Save(p)
        print("生成默认配置: %s (%d 字节, generation=%d)" %
              (p, os.path.getsize(p), hive.generation))


# ---------------------------------------------------------------------------
# tkinter GUI（惰性导入，无显示环境仅 gen-defaults 可用）
# ---------------------------------------------------------------------------
def RunGui(path: str):
    import tkinter as tk
    from tkinter import ttk, messagebox, filedialog

    root = tk.Tk()
    root.geometry("820x520")

    # 当前编辑会话状态：hive 为可变引用，path 记录当前文件（None 表示尚未关联文件）。
    state = {"hive": RegistryHive(), "path": path or None}

    # 尝试加载初始文件（若存在）
    if path and os.path.exists(path):
        try:
            state["hive"].Load(path)
        except Exception as ex:  # noqa
            messagebox.showerror("加载失败", str(ex))

    # ---- StringVar（必须在创建 Tk 根之后） ----
    type_var = tk.StringVar()
    val_var = tk.StringVar()
    path_var = tk.StringVar()

    def UpdateTitle():
        p = state["path"]
        if p:
            root.title("SukiOS Registry Editor — %s" % os.path.basename(p))
        else:
            root.title("SukiOS Registry Editor — (未命名)")

    # ---- 回调：树/表单 ----
    def RefreshTree():
        tree.delete(*tree.get_children())
        root_node = tree.insert("", "end", text="<root>", open=True)
        for k in state["hive"].Keys():
            parts = k.strip("/").split("/")
            parent = root_node
            for i in range(len(parts)):
                cur = "/".join(parts[:i + 1])
                existing = None
                for c in tree.get_children(parent):
                    if tree.item(c, "text") == parts[i]:
                        existing = c
                        break
                if existing is None:
                    existing = tree.insert(parent, "end", text=parts[i], values=(cur,))
                parent = existing

    def OnSelect(event):
        sel = tree.selection()
        if not sel:
            return
        item = tree.item(sel[0])
        cur = item.get("values")
        if not cur:
            return
        p = "/" + cur[0]
        r = state["hive"].GetValue(p)
        if r is None:
            return
        vtype, val, _ = r
        path_var.set(p)
        type_var.set(TYPE_NAME[vtype])
        val_var.set(str(val))

    def ClearForm():
        path_var.set("")
        type_var.set("")
        val_var.set("")

    # ---- 回调：文件操作 ----
    def LoadIntoState(p: str) -> bool:
        h = RegistryHive()
        try:
            h.Load(p)
        except Exception as ex:  # noqa
            messagebox.showerror("加载失败", str(ex))
            return False
        state["hive"] = h
        state["path"] = p
        return True

    def DoOpen():
        p = filedialog.askopenfilename(
            title="打开 SukiRegistry Hive",
            filetypes=[("SukiRegistry Hive", "*.reg"),
                       ("所有文件", "*.*")],
        )
        if not p:
            return
        if LoadIntoState(p):
            RefreshTree()
            ClearForm()
            UpdateTitle()

    def DoSaveFile():
        p = state["path"]
        if not p:
            DoSaveAs()
            return
        try:
            state["hive"].Save(p)
        except Exception as ex:  # noqa
            messagebox.showerror("保存失败", str(ex))
            return
        messagebox.showinfo("已保存", "写入 %s (generation=%d)" %
                            (p, state["hive"].generation))

    def DoSaveAs():
        initial = os.path.basename(state["path"]) if state["path"] else "system.reg"
        p = filedialog.asksaveasfilename(
            title="另存为 SukiRegistry Hive",
            defaultextension=".reg",
            initialfile=initial,
            filetypes=[("SukiRegistry Hive", "*.reg"),
                       ("所有文件", "*.*")],
        )
        if not p:
            return
        try:
            state["hive"].Save(p)
        except Exception as ex:  # noqa
            messagebox.showerror("保存失败", str(ex))
            return
        state["path"] = p
        UpdateTitle()
        messagebox.showinfo("已保存", "写入 %s (generation=%d)" %
                            (p, state["hive"].generation))

    def DoExit():
        root.destroy()

    def DoAbout():
        messagebox.showinfo(
            "关于",
            "SukiOS Registry Editor\n\n"
            "SukiRegistry Hive 二进制格式外部编辑器（无签名版）\n"
            "路径分隔符统一使用 Unix 风格 '/'",
        )

    # ---- 回调：键编辑 ----
    def DoSaveEntry():
        p = path_var.get().strip()
        if not p:
            return
        if not p.startswith("/"):
            p = "/" + p
        try:
            vt = [k for k, v in TYPE_NAME.items() if v == type_var.get()][0]
        except IndexError:
            messagebox.showerror("错误", "请选择合法的类型")
            return
        raw = val_var.get()
        try:
            if vt in (TYPE_INT64, TYPE_UINT64):
                state["hive"].SetValue(p, vt, int(raw))
            elif vt == TYPE_BOOL:
                state["hive"].SetValue(
                    p, vt, raw.strip().lower() in ("1", "true", "yes"))
            else:
                state["hive"].SetValue(p, vt, raw)
        except PermissionError as ex:
            messagebox.showerror("拒绝", str(ex))
            return
        except ValueError as ex:
            messagebox.showerror("错误", "值无法解析: %s" % ex)
            return
        if state["path"]:
            try:
                state["hive"].Save(state["path"])
            except Exception as ex:  # noqa
                messagebox.showerror("保存失败", str(ex))
        RefreshTree()
        messagebox.showinfo("已保存", "写入 %s (generation=%d)" %
                            (p, state["hive"].generation))

    def DoDeleteEntry():
        p = path_var.get().strip()
        if not p:
            return
        if not p.startswith("/"):
            p = "/" + p
        state["hive"].DeleteValue(p)
        if state["path"]:
            try:
                state["hive"].Save(state["path"])
            except Exception as ex:  # noqa
                messagebox.showerror("保存失败", str(ex))
        RefreshTree()
        ClearForm()

    # ---- 布局 ----
    pane = ttk.PanedWindow(root, orient="horizontal")
    pane.pack(fill="both", expand=True)

    tree = ttk.Treeview(pane)
    pane.add(tree, weight=1)

    right = ttk.Frame(pane)
    pane.add(right, weight=2)

    ttk.Label(right, text="键路径:").grid(row=0, column=0, sticky="w")
    path_entry = ttk.Entry(right, textvariable=path_var, width=48)
    path_entry.grid(row=0, column=1, sticky="we")
    ttk.Label(right, text="类型:").grid(row=1, column=0, sticky="w")
    type_combo = ttk.Combobox(right, textvariable=type_var,
                              values=TYPE_NAME_LIST,
                              state="readonly", width=12)
    type_combo.grid(row=1, column=1, sticky="w")
    ttk.Label(right, text="值:").grid(row=2, column=0, sticky="w")
    val_entry = ttk.Entry(right, textvariable=val_var, width=48)
    val_entry.grid(row=2, column=1, sticky="we")

    right.columnconfigure(1, weight=1)

    btn = ttk.Frame(right)
    btn.grid(row=3, column=0, columnspan=2, pady=8)
    ttk.Button(btn, text="保存键", command=DoSaveEntry).pack(side="left", padx=4)
    ttk.Button(btn, text="删除键", command=DoDeleteEntry).pack(side="left", padx=4)

    tree.bind("<<TreeviewSelect>>", OnSelect)

    # ---- 菜单栏 ----
    menubar = tk.Menu(root)

    file_menu = tk.Menu(menubar, tearoff=0)
    file_menu.add_command(label="打开...", accelerator="Ctrl+O", command=DoOpen)
    file_menu.add_separator()
    file_menu.add_command(label="保存", accelerator="Ctrl+S", command=DoSaveFile)
    file_menu.add_command(label="另存为...", accelerator="Ctrl+Shift+S",
                          command=DoSaveAs)
    file_menu.add_separator()
    file_menu.add_command(label="退出", accelerator="Ctrl+Q", command=DoExit)
    menubar.add_cascade(label="文件", menu=file_menu)

    help_menu = tk.Menu(menubar, tearoff=0)
    help_menu.add_command(label="关于", command=DoAbout)
    menubar.add_cascade(label="帮助", menu=help_menu)

    root.config(menu=menubar)

    # ---- 快捷键 ----
    root.bind_all("<Control-o>", lambda e: DoOpen())
    root.bind_all("<Control-s>", lambda e: DoSaveFile())
    root.bind_all("<Control-Shift-s>", lambda e: DoSaveAs())
    root.bind_all("<Control-Shift-S>", lambda e: DoSaveAs())
    root.bind_all("<Control-q>", lambda e: DoExit())

    UpdateTitle()
    RefreshTree()
    root.mainloop()


# ---------------------------------------------------------------------------
# 入口
# ---------------------------------------------------------------------------
def Main():
    if len(sys.argv) >= 2 and sys.argv[1] == "gen-defaults":
        dest = sys.argv[2] if len(sys.argv) >= 3 else "configs/default"
        GenDefaults(dest)
        return
    if len(sys.argv) >= 3 and sys.argv[1] == "edit":
        RunGui(sys.argv[2])
        return
    # 默认：编辑 configs/default/system.reg
    default = os.path.join("configs", "default", "system.reg")
    RunGui(default)


if __name__ == "__main__":
    Main()