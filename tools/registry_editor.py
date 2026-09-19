#!/usr/bin/env python3
# tools/registry_editor.py
#
# SukiOS 安全配置存储（无签名版）——外部编辑工具 + 格式参考实现
#
# 作用：
#   1) 定义 SukiRegistry Hive v2 二进制格式的规范实现（Python 侧，C 内核侧将复用同一格式）。
#   2) 提供「层级键树」模型（类 Windows 注册表）：每个节点是「键」，键下可挂
#      「值」(values) 与「子键」(subkeys)。磁盘 body 就是这个键树的前序序列化。
#   3) 提供 JSON 互操作：
#        - export  : 把 .sre 二进制导出成人类可编辑的 .sre.json（键/值/子键结构）。
#        - compile : 把 .sre.json 编译回 .sre 二进制（导入 JSON 生成注册表）。
#   4) 提供 Windows 注册表风格的 tkinter GUI：左侧键树、右侧值列表。
#   5) 提供 gen-defaults 子命令，把 configs/default/ 下的默认配置生成到仓库
#      （构建系统随后把它们塞进磁盘镜像的 /sys/configs/）。
#
# 命名：按项目规范，新增函数使用 PascalCase（NameFunctionLikeThis）。
#
# 格式要点（与方案文档一致，并做工程化修正）：
#   - 路径统一用 Unix 风格 '/'；内核按路径 "System/Display/Width" 查值。
#   - CRC32 仅用于"意外损坏检测"，不宣称能防离线篡改（离线有原始磁盘访问者可重算）。
#   - generation 单调递增，用于在线（进程）场景防回滚/防重放；离线场景基线随重启归零。
#   - 磁盘布局：header(64B) + body（键树前序序列化）。
#       body 中每个键节点：
#         name_len: u32
#         name:     name_len 字节（UTF-8；根键名为 "System"/"User"/"Services"）
#         flags:    u32
#         value_count: u32
#         [值表] 每个值： vname_len:u32, vname, vtype:u32, vdata_len:u64, vdata
#         subkey_count: u32
#         [子键表] 每个子键： 内嵌一个键节点（前序递归）
#
# 依赖：tkinter / json / base64 仅在实际使用时导入；gen-defaults 在无显示环境也能跑。

import os
import sys
import struct

try:
    import json
except Exception:  # noqa
    json = None
try:
    import base64
except Exception:  # noqa
    base64 = None


# ---------------------------------------------------------------------------
# 常量
# ---------------------------------------------------------------------------
SUKREG_MAGIC = b"SUKREG\0\0"   # 8 字节
SUKREG_VERSION = 2             # v2：层级键树（v1 为已废弃的扁平 path 数组）
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
# 小端读取辅助
# ---------------------------------------------------------------------------
def _Le32(b: bytes, o: int) -> int:
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)


def _Le64(b: bytes, o: int) -> int:
    v = 0
    for i in range(8):
        v |= b[o + i] << (8 * i)
    return v


# ---------------------------------------------------------------------------
# 内存中的键树
# ---------------------------------------------------------------------------
class RegKey:
    """一个注册表键：持有 值表(values) 与 子键表(subkeys)。"""

    def __init__(self, name: str, flags: int = 0):
        self.name = name
        self.flags = flags
        # vname(str) -> (vtype:int, raw:bytes)
        self.values = {}
        # sname(str) -> RegKey
        self.subkeys = {}

    def GetValue(self, vname: str):
        e = self.values.get(vname)
        if not e:
            return None
        return (e[0], e[1])

    def SetValueRaw(self, vname: str, vtype: int, raw: bytes):
        self.values[vname] = (vtype, bytes(raw))

    def SetValue(self, vname: str, vtype: int, value, flags: int = 0):
        self.values[vname] = (vtype, EncodeValue(vtype, value))

    def DeleteValue(self, vname: str):
        if vname in self.values:
            del self.values[vname]

    def DeleteSubkey(self, sname: str):
        if sname in self.subkeys:
            del self.subkeys[sname]


# ---------------------------------------------------------------------------
# 值编解码
# ---------------------------------------------------------------------------
def EncodeValue(vtype: int, value) -> bytes:
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


def DecodeValue(vtype: int, raw: bytes):
    if vtype == TYPE_INT64:
        return struct.unpack("<q", raw[:8])[0]
    if vtype == TYPE_UINT64:
        return struct.unpack("<Q", raw[:8])[0]
    if vtype == TYPE_BOOL:
        return bool(raw[0]) if raw else False
    if vtype == TYPE_STRING:
        return raw.decode("utf-8", "replace")
    if vtype == TYPE_BINARY:
        return base64.b64encode(raw).decode("ascii")
    if vtype == TYPE_LINK:
        return raw.decode("utf-8", "replace")
    return ""


def DisplayValue(vtype: int, raw: bytes) -> str:
    """GUI 右侧展示用：把值转成可读字符串。"""
    if vtype == TYPE_INT64:
        return str(struct.unpack("<q", raw[:8])[0])
    if vtype == TYPE_UINT64:
        return str(struct.unpack("<Q", raw[:8])[0])
    if vtype == TYPE_BOOL:
        return "true" if (raw and raw[0]) else "false"
    if vtype == TYPE_STRING:
        return raw.decode("utf-8", "replace")
    if vtype == TYPE_BINARY:
        s = base64.b64encode(raw).decode("ascii")
        return "(binary) " + (s if len(s) <= 40 else s[:40] + "...")
    if vtype == TYPE_LINK:
        return "(link) " + raw.decode("utf-8", "replace")
    return ""


# ---------------------------------------------------------------------------
# 二进制（键树）序列化 / 反序列化 —— 必须与 kernel/registry/hive.c 逐字段一致
# ---------------------------------------------------------------------------
def SerializeKey(key: RegKey) -> bytes:
    out = bytearray()
    name = key.name.encode("utf-8")
    out += struct.pack("<I", len(name)) + name
    out += struct.pack("<I", key.flags)
    items = sorted(key.values.items(), key=lambda kv: kv[0])
    out += struct.pack("<I", len(items))
    for vname, (vtype, raw) in items:
        vn = vname.encode("utf-8")
        out += struct.pack("<I", len(vn)) + vn
        out += struct.pack("<I", vtype)
        out += struct.pack("<Q", len(raw)) + raw
    subkeys = sorted(key.subkeys.items(), key=lambda kv: kv[0])
    out += struct.pack("<I", len(subkeys))
    for _sn, sk in subkeys:
        out += SerializeKey(sk)
    return bytes(out)


def ParseKeyNode(base: bytes, off: int):
    """解析一个键节点；返回 (RegKey, 节点结束偏移)。越界抛 ValueError。"""
    total = len(base)
    if off + 4 > total:
        raise ValueError("键节点截断(名称长度)")
    name_len = _Le32(base, off); off += 4
    if off + name_len > total:
        raise ValueError("键名越界")
    name = base[off:off + name_len].decode("utf-8"); off += name_len
    if off + 4 > total:
        raise ValueError("键标志越界")
    flags = _Le32(base, off); off += 4
    if off + 4 > total:
        raise ValueError("值计数越界")
    value_count = _Le32(base, off); off += 4
    key = RegKey(name, flags)
    for _ in range(value_count):
        if off + 4 > total:
            raise ValueError("值名长度越界")
        vn_len = _Le32(base, off); off += 4
        if off + vn_len > total:
            raise ValueError("值名越界")
        vn = base[off:off + vn_len].decode("utf-8"); off += vn_len
        if off + 4 > total:
            raise ValueError("值类型越界")
        vt = _Le32(base, off); off += 4
        if off + 8 > total:
            raise ValueError("值长度越界")
        vd_len = _Le64(base, off); off += 8
        if off + vd_len > total:
            raise ValueError("值数据越界")
        vd = base[off:off + vd_len]; off += vd_len
        key.values[vn] = (vt, bytes(vd))
    if off + 4 > total:
        raise ValueError("子键计数越界")
    subkey_count = _Le32(base, off); off += 4
    for _ in range(subkey_count):
        sub, off = ParseKeyNode(base, off)
        key.subkeys[sub.name] = sub
    return key, off


# ---------------------------------------------------------------------------
# JSON 互操作
# ---------------------------------------------------------------------------
def JsonTypeToEnum(name: str) -> int:
    for k, v in TYPE_NAME.items():
        if v == name:
            return k
    raise ValueError("未知类型: %s" % name)


def JsonEncodeValue(vtype: int, data) -> bytes:
    if vtype == TYPE_INT64:
        return struct.pack("<q", int(data))
    if vtype == TYPE_UINT64:
        return struct.pack("<Q", int(data))
    if vtype == TYPE_BOOL:
        return b"\x01" if data else b"\x00"
    if vtype == TYPE_STRING:
        return data.encode("utf-8")
    if vtype == TYPE_BINARY:
        return base64.b64decode(data)
    if vtype == TYPE_LINK:
        return data.encode("utf-8")
    return b""


def _JsonNodeToKey(name: str, node: dict) -> RegKey:
    key = RegKey(name, 0)
    for vn, v in (node.get("values") or {}).items():
        vt = JsonTypeToEnum(v["type"])
        key.values[vn] = (vt, JsonEncodeValue(vt, v["data"]))
    for sn, sub in (node.get("subkeys") or {}).items():
        key.subkeys[sn] = _JsonNodeToKey(sn, sub)
    # 容错：节点中除 values/subkeys 之外的额外键，也当作子键处理
    # （允许手写 JSON 省略 "subkeys" 包裹层，如 "Key": {"Sub": {...}}）
    for extra in node.keys():
        if extra in ("values", "subkeys"):
            continue
        if extra in key.subkeys:
            continue
        key.subkeys[extra] = _JsonNodeToKey(extra, node[extra])
    return key


def JsonToKey(obj: dict) -> RegKey:
    """顶层必须是 {根键名: {values, subkeys}}。"""
    if not isinstance(obj, dict) or len(obj) != 1:
        raise ValueError("JSON 顶层必须只有一个根键 {根名: {values,subkeys}}")
    rootname = next(iter(obj))
    return _JsonNodeToKey(rootname, obj[rootname])


def KeyToJsonNode(key: RegKey) -> dict:
    values = {}
    for vn, (vt, raw) in key.values.items():
        values[vn] = {"type": TYPE_NAME[vt], "data": DecodeValue(vt, raw)}
    subkeys = {sn: KeyToJsonNode(sk) for sn, sk in sorted(key.subkeys.items())}
    return {"values": values, "subkeys": subkeys}


def KeyToFileJson(key: RegKey) -> dict:
    return {key.name: KeyToJsonNode(key)}


# ---------------------------------------------------------------------------
# Hive（header + 根键）
# ---------------------------------------------------------------------------
class RegistryHive:
    def __init__(self):
        self.root = RegKey("System")
        self.generation = 0
        self.timestamp = 0
        self.flags = 0

    def Save(self, path: str):
        body = SerializeKey(self.root)
        header = bytearray(HEADER_SIZE)
        header[0:8] = SUKREG_MAGIC
        struct.pack_into("<I", header, 8, SUKREG_VERSION)
        struct.pack_into("<I", header, 12, self.flags)
        struct.pack_into("<Q", header, 16, HEADER_SIZE)   # root_offset = 64
        struct.pack_into("<Q", header, 24, 0)             # entry_count（v2 未用）
        struct.pack_into("<Q", header, 32, self.generation)
        struct.pack_into("<Q", header, 40, self.timestamp)
        crc = Crc32(bytes(header[0:HEADER_CRC_END]) + body)
        struct.pack_into("<I", header, 48, crc)
        struct.pack_into("<Q", header, 52, len(body))   # body_size
        with open(path, "wb") as f:
            f.write(header)
            f.write(body)

    def Load(self, path: str):
        with open(path, "rb") as f:
            raw = f.read()
        if len(raw) < HEADER_SIZE:
            raise ValueError("文件过小，不是合法 Hive")
        if raw[0:8] != SUKREG_MAGIC:
            raise ValueError("魔数不匹配，不是 SukiRegistry Hive")
        version = struct.unpack_from("<I", raw, 8)[0]
        if version != SUKREG_VERSION:
            raise ValueError("版本不兼容: %d（需要 %d）" % (version, SUKREG_VERSION))
        body_size = struct.unpack_from("<Q", raw, 52)[0]
        if body_size > len(raw) - HEADER_SIZE:
            raise ValueError("body_size 非法")
        body = raw[HEADER_SIZE:HEADER_SIZE + body_size]
        calc = Crc32(raw[0:HEADER_CRC_END] + body)
        stored = struct.unpack_from("<I", raw, 48)[0]
        if calc != stored:
            raise ValueError("CRC32 校验失败（文件损坏或被篡改）")
        self.flags = struct.unpack_from("<I", raw, 12)[0]
        self.generation = struct.unpack_from("<Q", raw, 32)[0]
        self.timestamp = struct.unpack_from("<Q", raw, 40)[0]
        root_off = struct.unpack_from("<Q", raw, 16)[0]
        if root_off < HEADER_SIZE or root_off > HEADER_SIZE + body_size:
            raise ValueError("root_offset 非法")
        self.root = ParseKeyNode(raw, root_off)[0]
        return self


# ---------------------------------------------------------------------------
# 默认配置（构建系统复制到 /sys/configs/）
# 直接采用 layers_example 的示例数据（Windows 注册表样式）。
# ---------------------------------------------------------------------------
DEFAULT_SYSTEM_JSON = {
    "System": {
        "values": {},
        "subkeys": {
            "Display": {
                "values": {
                    "Width": {"type": "uint64", "data": 1024},
                    "Height": {"type": "uint64", "data": 768},
                    "Bpp": {"type": "uint64", "data": 32},
                },
                "subkeys": {},
            },
            "Network": {
                "values": {
                    "DhcpEnabled": {"type": "bool", "data": True},
                    "Hostname": {"type": "string", "data": "sukios"},
                },
                "subkeys": {},
            },
            "Kernel": {
                "values": {
                    "PanicOnOops": {"type": "bool", "data": True},
                },
                "subkeys": {},
            },
            "Boot": {
                "values": {
                    "BootDeviceType": {"type": "string", "data": ""},
                    "ShowProgress": {"type": "bool", "data": True},
                    "ShowLogo": {"type": "bool", "data": True},
                },
                "subkeys": {
                    "CustomLogo": {
                        "values": {
                            "Path": {"type": "string", "data": ""},
                            "Enabled": {"type": "bool", "data": False},
                        },
                        "subkeys": {},
                    }
                },
            },
            "Services": {
                "values": {},
                "subkeys": {
                    "Display": {"values": {"Autostart": {"type": "bool", "data": True}}, "subkeys": {}},
                    "Fs": {"values": {"Autostart": {"type": "bool", "data": True}}, "subkeys": {}},
                    "Input": {"values": {"Autostart": {"type": "bool", "data": True}}, "subkeys": {}},
                },
            },
            "Drivers": {
                "values": {},
                "subkeys": {
                    "MANUFACTURER_NAME": {
                        "values": {},
                        "subkeys": {
                            "DRIVER_DISPLAY_NAME": {
                                "values": {
                                    "Path": {"type": "string", "data": ""},
                                    "Enabled": {"type": "bool", "data": True},
                                },
                                "subkeys": {},
                            }
                        },
                    }
                },
            },
        },
    }
}

DEFAULT_USER_JSON = {
    "User": {
        "values": {},
        "subkeys": {
            "Theme": {"values": {"Name": {"type": "string", "data": "dark"}}, "subkeys": {}},
            "Locale": {"values": {"": {"type": "string", "data": "zh-CN"}}, "subkeys": {}},
            "Shell": {"values": {"HistorySize": {"type": "uint64", "data": 100}}, "subkeys": {}},
            "Services": {"values": {}, "subkeys": {}},
        },
    }
}

# services 无示例 JSON，沿用旧 services.reg 的 Autostart 数据。
DEFAULT_SERVICES_JSON = {
    "Services": {
        "values": {},
        "subkeys": {
            "Display": {"values": {"Autostart": {"type": "bool", "data": True}}, "subkeys": {}},
            "Fs": {"values": {"Autostart": {"type": "bool", "data": True}}, "subkeys": {}},
            "Input": {"values": {"Autostart": {"type": "bool", "data": True}}, "subkeys": {}},
            "Net": {"values": {"Autostart": {"type": "bool", "data": True}}, "subkeys": {}},
        },
    }
}


def BuildDefaults():
    """返回 {文件名: RegKey 根键}。"""
    return {
        "system.sre": JsonToKey(DEFAULT_SYSTEM_JSON),
        "user.sre": JsonToKey(DEFAULT_USER_JSON),
        "services.sre": JsonToKey(DEFAULT_SERVICES_JSON),
    }


def GenDefaults(dest_dir: str):
    os.makedirs(dest_dir, exist_ok=True)
    for name, key in BuildDefaults().items():
        hive = RegistryHive()
        hive.root = key
        hive.flags = FLAG_SYSTEM
        hive.generation = 1
        hive.timestamp = 0
        p = os.path.join(dest_dir, name)
        hive.Save(p)
        print("生成默认配置: %s (%d 字节, generation=%d)" %
              (p, os.path.getsize(p), hive.generation))


# ---------------------------------------------------------------------------
# tkinter GUI（Windows 注册表风格：左侧键树 / 右侧值列表；惰性导入）
# ---------------------------------------------------------------------------
def RunGui(path: str):
    import tkinter as tk
    from tkinter import ttk, messagebox, filedialog, simpledialog

    root = tk.Tk()
    root.geometry("900x600")

    state: dict = {
        "root": RegKey("System"),
        "path": path or None,
        "dirty": False,
        "flags": 0,
        "generation": 0,
    }
    key_map: dict = {}      # tree iid -> RegKey
    parent_map: dict = {}   # id(RegKey) -> 父 RegKey（根键为 None）
    val_map: dict = {}      # value-list iid -> vname

    # 尝试加载初始文件
    if path and os.path.exists(path):
        try:
            h = RegistryHive()
            h.Load(path)
            state["root"] = h.root
            state["flags"] = h.flags
            state["generation"] = h.generation
        except Exception as ex:  # noqa
            messagebox.showerror("加载失败", str(ex))

    # ---------- 标题 ----------
    def UpdateTitle():
        p = state["path"]
        mark = " *" if state["dirty"] else ""
        if p:
            root.title("SukiOS Registry Editor — %s%s" % (os.path.basename(p), mark))
        else:
            root.title("SukiOS Registry Editor — (未命名)%s" % mark)

    def MarkDirty():
        state["dirty"] = True
        UpdateTitle()

    # ---------- 键树 ----------
    def BuildTree():
        tree.delete(*tree.get_children())
        key_map.clear()
        parent_map.clear()
        root_iid = tree.insert("", "end", text=state["root"].name, open=True)
        key_map[root_iid] = state["root"]
        parent_map[id(state["root"])] = None
        _InsertChildren(root_iid, state["root"])

    def _InsertChildren(parent_iid, key):
        for sn in sorted(key.subkeys.keys()):
            sk = key.subkeys[sn]
            iid = tree.insert(parent_iid, "end", text=sn, open=False)
            key_map[iid] = sk
            parent_map[id(sk)] = key
            _InsertChildren(iid, sk)

    def GetSelectedKey():
        sel = tree.selection()
        if not sel:
            return None
        return key_map.get(sel[0])

    def GetParentOf(key):
        return parent_map.get(id(key))

    def SelectedKeyPath(key):
        parts = []
        cur = key
        while cur is not None and cur is not state["root"]:
            parts.append(cur.name)
            cur = GetParentOf(cur)
        parts.reverse()
        return ("/" + "/".join(parts)) if parts else "/"

    def OnSelectKey(event):
        key = GetSelectedKey()
        if key is None:
            return
        path_var.set(SelectedKeyPath(key))
        PopulateValues(key)

    def PopulateValues(key):
        val_list.delete(*val_list.get_children())
        val_map.clear()
        for vn in sorted(key.values.keys()):
            vt, raw = key.values[vn]
            disp = DisplayValue(vt, raw)
            label = vn if vn != "" else "(默认)"
            iid = val_list.insert("", "end", values=(label, TYPE_NAME[vt], disp))
            val_map[iid] = vn

    # ---------- 自动写盘 ----------
    def AutoSaveIfBound() -> bool:
        if not state["path"]:
            return False
        try:
            h = RegistryHive()
            h.root = state["root"]
            h.flags = state["flags"]
            h.generation = state["generation"] + 1
            h.timestamp = 0
            h.Save(state["path"])
            state["generation"] = h.generation
        except Exception as ex:  # noqa
            messagebox.showerror("保存失败", str(ex))
            return False
        return True

    # ---------- 值编辑对话框 ----------
    def ShowValueDialog(name: str, type_name: str, data_str: str, edit: bool):
        dlg = tk.Toplevel(root)
        dlg.title("编辑值" if edit else "添加值")
        dlg.transient(root)
        dlg.grab_set()
        dlg.resizable(False, False)

        d_name = tk.StringVar(value=name)
        d_type = tk.StringVar(value=type_name)
        d_data = tk.StringVar(value=data_str)

        frm = ttk.Frame(dlg, padding=10)
        frm.pack(fill="both", expand=True)
        ttk.Label(frm, text="值名:").grid(row=0, column=0, sticky="w", pady=4)
        ne = ttk.Entry(frm, textvariable=d_name, width=46)
        ne.grid(row=0, column=1, sticky="we", pady=4)
        ttk.Label(frm, text="类型:").grid(row=1, column=0, sticky="w", pady=4)
        cb = ttk.Combobox(frm, textvariable=d_type, values=list(TYPE_NAME.values()),
                          state="readonly", width=12)
        cb.grid(row=1, column=1, sticky="w", pady=4)
        ttk.Label(frm, text="值:").grid(row=2, column=0, sticky="w", pady=4)
        ttk.Entry(frm, textvariable=d_data, width=46).grid(row=2, column=1,
                                                            sticky="we", pady=4)
        frm.columnconfigure(1, weight=1)

        result = {"value": None}

        def OnOk():
            tname = d_type.get()
            try:
                vt = JsonTypeToEnum(tname)
            except Exception as ex:  # noqa
                messagebox.showerror("错误", str(ex))
                return
            raw_str = d_data.get()
            try:
                if vt in (TYPE_INT64, TYPE_UINT64):
                    raw = EncodeValue(vt, int(raw_str))
                elif vt == TYPE_BOOL:
                    raw = EncodeValue(vt, raw_str.strip().lower() in ("1", "true", "yes"))
                else:
                    raw = JsonEncodeValue(vt, raw_str)
            except Exception as ex:  # noqa
                messagebox.showerror("错误", "值无法解析: %s" % ex)
                return
            result["value"] = (d_name.get(), vt, raw)
            dlg.destroy()

        def OnCancel():
            dlg.destroy()

        bf = ttk.Frame(frm)
        bf.grid(row=3, column=0, columnspan=2, pady=(10, 0))
        ttk.Button(bf, text="确定", command=OnOk).pack(side="left", padx=4)
        ttk.Button(bf, text="取消", command=OnCancel).pack(side="left", padx=4)
        ne.focus_set()
        dlg.wait_window()
        return result["value"]

    # ---------- 回调：键 / 值 操作 ----------
    def AddKey():
        parent = GetSelectedKey() or state["root"]
        name = simpledialog.askstring("新建键",
                                      "在 %s 下新建子键名:" % parent.name, parent=root)
        if not name:
            return
        name = name.strip()
        if name == "" or "/" in name:
            messagebox.showerror("错误", "键名不能为空且不能含 '/'")
            return
        if name in parent.subkeys:
            messagebox.showerror("错误", "子键 %s 已存在" % name)
            return
        parent.subkeys[name] = RegKey(name)
        MarkDirty()
        BuildTree()
        AutoSaveIfBound()

    def AddValue():
        key = GetSelectedKey()
        if key is None:
            messagebox.showinfo("提示", "请先在左侧选择一个键")
            return
        res = ShowValueDialog("", "string", "", edit=False)
        if res is None:
            return
        vname, vt, raw = res
        if vname in key.values:
            messagebox.showerror("错误", "值 %r 已存在" % vname)
            return
        key.values[vname] = (vt, raw)
        MarkDirty()
        PopulateValues(key)
        AutoSaveIfBound()

    def EditValue():
        key = GetSelectedKey()
        if key is None:
            return
        sel = val_list.selection()
        if not sel:
            messagebox.showinfo("提示", "请选择要编辑的值")
            return
        vname = val_map[sel[0]]
        vt, raw = key.values[vname]
        res = ShowValueDialog(vname, TYPE_NAME[vt], DisplayValue(vt, raw), edit=True)
        if res is None:
            return
        new_name, new_vt, new_raw = res
        if new_name != vname and new_name in key.values:
            messagebox.showerror("错误", "值 %r 已存在" % new_name)
            return
        if new_name != vname:
            del key.values[vname]
        key.values[new_name] = (new_vt, new_raw)
        MarkDirty()
        PopulateValues(key)
        AutoSaveIfBound()

    def DeleteKey():
        key = GetSelectedKey()
        if key is None:
            return
        if key is state["root"]:
            messagebox.showerror("错误", "不能删除根键")
            return
        parent = GetParentOf(key)
        if not messagebox.askyesno("删除", "删除键 %s 及其下所有子键/值？" % key.name):
            return
        parent.DeleteSubkey(key.name)
        MarkDirty()
        BuildTree()
        AutoSaveIfBound()

    def DeleteValue():
        key = GetSelectedKey()
        if key is None:
            return
        sel = val_list.selection()
        if not sel:
            messagebox.showinfo("提示", "请选择要删除的值")
            return
        vname = val_map[sel[0]]
        if not messagebox.askyesno("删除", "删除值 %r？" % vname):
            return
        key.DeleteValue(vname)
        MarkDirty()
        PopulateValues(key)
        AutoSaveIfBound()

    # ---------- 回调：文件操作 ----------
    def ConfirmDiscard() -> bool:
        if not state["dirty"]:
            return True
        ans = messagebox.askyesnocancel("未保存的修改",
                                         "当前 Hive 有未保存的修改，是否先保存？")
        if ans is None:
            return False
        if ans:
            return DoSaveFile()
        return True

    def _SaveTo(p):
        h = RegistryHive()
        h.root = state["root"]
        h.flags = state["flags"]
        h.generation = state["generation"] + 1
        h.timestamp = 0
        h.Save(p)
        state["generation"] = h.generation

    def DoSaveFile() -> bool:
        p = state["path"]
        if not p:
            return DoSaveAs()
        try:
            _SaveTo(p)
        except Exception as ex:  # noqa
            messagebox.showerror("保存失败", str(ex))
            return False
        state["dirty"] = False
        UpdateTitle()
        messagebox.showinfo("已保存", "写入 %s (generation=%d)" %
                            (p, state["generation"]))
        return True

    def DoSaveAs() -> bool:
        p = filedialog.asksaveasfilename(
            title="另存为 SukiRegistry Hive",
            defaultextension=".sre",
            initialfile=os.path.basename(state["path"]) if state["path"] else "system.sre",
            filetypes=[("SukiRegistry Hive", "*.sre"), ("所有文件", "*.*")],
        )
        if not p:
            return False
        try:
            _SaveTo(p)
        except Exception as ex:  # noqa
            messagebox.showerror("保存失败", str(ex))
            return False
        state["path"] = p
        state["dirty"] = False
        UpdateTitle()
        messagebox.showinfo("已保存", "写入 %s (generation=%d)" %
                            (p, state["generation"]))
        return True

    def DoOpen():
        if not ConfirmDiscard():
            return
        p = filedialog.askopenfilename(
            title="打开 SukiRegistry Hive",
            filetypes=[("SukiRegistry Hive", "*.sre"), ("所有文件", "*.*")],
        )
        if not p:
            return
        try:
            h = RegistryHive()
            h.Load(p)
        except Exception as ex:  # noqa
            messagebox.showerror("加载失败", str(ex))
            return
        state["root"] = h.root
        state["flags"] = h.flags
        state["generation"] = h.generation
        state["path"] = p
        state["dirty"] = False
        BuildTree()
        PopulateValues(state["root"])
        UpdateTitle()

    def DoNew():
        if not ConfirmDiscard():
            return
        state["root"] = RegKey("NewHive")
        state["flags"] = 0
        state["generation"] = 0
        state["path"] = None
        state["dirty"] = False
        BuildTree()
        PopulateValues(state["root"])
        UpdateTitle()

    def DoExportJson():
        if not state["path"]:
            p = filedialog.asksaveasfilename(defaultextension=".json",
                                             filetypes=[("JSON", "*.json")])
        else:
            base = os.path.splitext(state["path"])[0] + ".json"
            p = filedialog.asksaveasfilename(
                initialfile=os.path.basename(base), defaultextension=".json",
                filetypes=[("JSON", "*.json")])
        if not p:
            return
        try:
            with open(p, "w", encoding="utf-8") as f:
                json.dump(KeyToFileJson(state["root"]), f, indent=2,
                          ensure_ascii=False)
        except Exception as ex:  # noqa
            messagebox.showerror("导出失败", str(ex))
            return
        messagebox.showinfo("已导出", "JSON 写入 %s" % p)

    def DoImportJson():
        p = filedialog.askopenfilename(
            title="导入 JSON 生成注册表",
            filetypes=[("JSON", "*.json"), ("所有文件", "*.*")])
        if not p:
            return
        try:
            with open(p, "r", encoding="utf-8") as f:
                obj = json.load(f)
            key = JsonToKey(obj)
        except Exception as ex:  # noqa
            messagebox.showerror("导入失败", str(ex))
            return
        state["root"] = key
        state["dirty"] = True
        UpdateTitle()
        BuildTree()
        PopulateValues(state["root"])
        AutoSaveIfBound()

    def DoExit():
        if not ConfirmDiscard():
            return
        root.destroy()

    def DoAbout():
        messagebox.showinfo(
            "关于",
            "SukiOS Registry Editor\n\n"
            "SukiRegistry Hive v2 二进制格式外部编辑器（无签名版）\n"
            "层级键树（类 Windows 注册表）：键 → 值 + 子键\n"
            "支持 .sre 二进制与 .sre.json 互转。",
        )

    # ---------- 布局 ----------
    menubar = tk.Menu(root)
    file_menu = tk.Menu(menubar, tearoff=0)
    file_menu.add_command(label="新建", command=DoNew)
    file_menu.add_command(label="打开...", command=DoOpen)
    file_menu.add_separator()
    file_menu.add_command(label="保存", command=DoSaveFile)
    file_menu.add_command(label="另存为...", command=DoSaveAs)
    file_menu.add_separator()
    file_menu.add_command(label="导出 JSON...", command=DoExportJson)
    file_menu.add_command(label="导入 JSON...", command=DoImportJson)
    file_menu.add_separator()
    file_menu.add_command(label="退出", command=DoExit)
    menubar.add_cascade(label="文件", menu=file_menu)
    edit_menu = tk.Menu(menubar, tearoff=0)
    edit_menu.add_command(label="新建键", command=AddKey)
    edit_menu.add_command(label="新建值", command=AddValue)
    edit_menu.add_command(label="删除键", command=DeleteKey)
    edit_menu.add_command(label="删除值", command=DeleteValue)
    menubar.add_cascade(label="编辑", menu=edit_menu)
    help_menu = tk.Menu(menubar, tearoff=0)
    help_menu.add_command(label="关于", command=DoAbout)
    menubar.add_cascade(label="帮助", menu=help_menu)
    root.config(menu=menubar)

    pane = ttk.PanedWindow(root, orient="horizontal")
    pane.pack(fill="both", expand=True)

    left = ttk.Frame(pane)
    pane.add(left, weight=1)
    ttk.Label(left, text="键").pack(anchor="w", padx=4, pady=(2, 0))
    tree = ttk.Treeview(left, show="tree")
    tree.pack(fill="both", expand=True, padx=4, pady=2)
    tree.bind("<<TreeviewSelect>>", OnSelectKey)

    right = ttk.Frame(pane)
    pane.add(right, weight=2)
    path_var = tk.StringVar()
    ttk.Label(right, text="键路径:").grid(row=0, column=0, sticky="w", padx=4)
    ttk.Entry(right, textvariable=path_var, width=48, state="readonly").grid(
        row=0, column=1, sticky="we", padx=4)
    ttk.Label(right, text="值").grid(row=1, column=0, columnspan=2, sticky="w", padx=4)
    val_list = ttk.Treeview(right, columns=("name", "type", "data"),
                            show="headings", height=18)
    val_list.heading("name", text="名称")
    val_list.heading("type", text="类型")
    val_list.heading("data", text="数据")
    val_list.column("name", width=160)
    val_list.column("type", width=80)
    val_list.column("data", width=300)
    val_list.grid(row=2, column=0, columnspan=2, sticky="nsew", padx=4, pady=2)
    val_list.bind("<Double-1>", lambda e: EditValue())
    right.rowconfigure(2, weight=1)
    right.columnconfigure(1, weight=1)

    btn = ttk.Frame(right)
    btn.grid(row=3, column=0, columnspan=2, pady=4)
    ttk.Button(btn, text="新建键", command=AddKey).pack(side="left", padx=3)
    ttk.Button(btn, text="新建值", command=AddValue).pack(side="left", padx=3)
    ttk.Button(btn, text="编辑值", command=EditValue).pack(side="left", padx=3)
    ttk.Button(btn, text="删除键", command=DeleteKey).pack(side="left", padx=3)
    ttk.Button(btn, text="删除值", command=DeleteValue).pack(side="left", padx=3)

    root.protocol("WM_DELETE_WINDOW", DoExit)
    UpdateTitle()
    BuildTree()
    PopulateValues(state["root"])
    root.mainloop()


# ---------------------------------------------------------------------------
# 入口
# ---------------------------------------------------------------------------
def Main():
    args = sys.argv[1:]
    if not args:
        RunGui(os.path.join("configs", "default", "system.sre"))
        return
    cmd = args[0]
    if cmd == "gen-defaults":
        dest = args[1] if len(args) >= 2 else "configs/default"
        GenDefaults(dest)
        return
    if cmd in ("compile", "import"):
        if len(args) < 3:
            print("用法: registry_editor.py compile <in.json> <out.sre>")
            sys.exit(1)
        with open(args[1], "r", encoding="utf-8") as f:
            obj = json.load(f)
        key = JsonToKey(obj)
        hive = RegistryHive()
        hive.root = key
        hive.flags = FLAG_SYSTEM
        hive.generation = 1
        hive.Save(args[2])
        print("已编译 %s -> %s" % (args[1], args[2]))
        return
    if cmd == "export":
        if len(args) < 3:
            print("用法: registry_editor.py export <in.sre> <out.json>")
            sys.exit(1)
        hive = RegistryHive()
        hive.Load(args[1])
        with open(args[2], "w", encoding="utf-8") as f:
            json.dump(KeyToFileJson(hive.root), f, indent=2, ensure_ascii=False)
        print("已导出 %s -> %s" % (args[1], args[2]))
        return
    if cmd == "edit":
        RunGui(args[1] if len(args) >= 2 else
               os.path.join("configs", "default", "system.sre"))
        return
    # 默认：把参数当作 .sre 路径打开
    RunGui(args[0] if os.path.exists(args[0]) else
           os.path.join("configs", "default", "system.sre"))


if __name__ == "__main__":
    Main()
