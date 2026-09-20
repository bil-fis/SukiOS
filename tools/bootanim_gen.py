#!/usr/bin/env python3
# tools/bootanim_gen.py
#
# SukiOS 启动图资源打包器（构建期工具）。
#
# 作用：扫描 boot/anim/ 下的**全部** .bmp，转换成内核可直接 blit 的「自描述原始
# 位图」，并生成两件产物（都放在输出目录）：
#   1) blobs.S  ：把每张图（.raw）以 .incbin 链入内核 .rodata，导出符号
#                 bootanim_<安全标识>_start / _end；
#   2) table.c  ：符号表 g_bootanim_blobs[] = { "原始文件名(去扩展名)", start, end }，
#                 以 NULL 结尾。内核按注册表 Boot/BootLogoID 在该表里查找。
#
# 因此**新增一张启动图只需把 bmp 丢进 boot/anim/** 并重新 make：本脚本会自动
# 纳入、自动生成符号与表项，无需改动 Makefile 或内核代码。
#
# 转换后 .raw 布局（小端，与 include/kernel/bootanim.h 逐一对应）：
#   [0..4)   magic 'S''A''N''I'（0x494E4153）
#   [4..8)   u32 width
#   [8..12)  u32 height
#   [12..16) u32 stride（像素，恒 = width）
#   [16..)   像素 u32 XRGB = (r<<16)|(g<<8)|b（与 32bpp 帧缓冲小端写入一致）
#
# 支持的 BMP：BITMAPINFOHEADER(>=40) 或 V4/V5 头，biCompression=BI_RGB(0)，
# 位深 24 或 32，底部/顶部行序均支持。其它形态（调色板/压缩/RLE）会打印告警并
# 跳过该文件（不阻断构建）。
#
# 用法：tools/bootanim_gen.py <anim_dir> <out_dir>

import os
import re
import struct
import sys

MAGIC = b"SANI"
HEADER_LE = 16   # magic(4) + width(4) + height(4) + stride(4)


def SafeIdent(name: str) -> str:
    """把文件名转成合法 C/汇编标识符（与 table.c 引用保持一致）。"""
    s = re.sub(r"[^A-Za-z0-9_]", "_", name)
    if not s or s[0].isdigit():
        s = "_" + s
    return s


def DecodeBmp(raw: bytes):
    """解析 BMP -> (width, height, pixels_bytes)。
    pixels_bytes 为自上而下、每像素 u32 XRGB 的小端字节串。失败抛 ValueError。"""
    if len(raw) < 54 or raw[0:2] != b"BM":
        raise ValueError("不是 BMP（缺少 'BM' 签名）")
    # BMP 头字段偏移（BITMAPFILEHEADER 14B + BITMAPINFOHEADER 前 36B）：
    #   10 bfOffBits(u32) / 14 biSize(u32) / 18 biWidth(i32) / 22 biHeight(i32)
    #   26 biPlanes(u16) / 28 biBitCount(u16) / 30 biCompression(u32)
    data_off = struct.unpack_from("<I", raw, 10)[0]
    hdr_size = struct.unpack_from("<I", raw, 14)[0]
    w = struct.unpack_from("<i", raw, 18)[0]
    h = struct.unpack_from("<i", raw, 22)[0]
    planes = struct.unpack_from("<H", raw, 26)[0]
    bpp = struct.unpack_from("<H", raw, 28)[0]
    comp = struct.unpack_from("<I", raw, 30)[0]
    if hdr_size < 40:
        raise ValueError("不支持的 BMP 头（%d 字节，需 >= 40）" % hdr_size)
    if w <= 0 or h == 0:
        raise ValueError("BMP 尺寸非法 (%dx%d)" % (w, h))
    if planes != 1:
        raise ValueError("BMP 平面数非法 (%d)" % planes)
    if comp != 0:
        raise ValueError("仅支持未压缩 BMP（biCompression=%d）" % comp)
    if bpp not in (24, 32):
        raise ValueError("仅支持 24/32bpp（当前 %d）" % bpp)

    top_down = h < 0
    aw, ah = w, abs(h)
    row = ((aw * bpp + 31) // 32) * 4
    if data_off + row * ah > len(raw):
        raise ValueError("BMP 像素数据越界（文件被截断？）")

    bpp_bytes = bpp // 8
    out = bytearray(aw * ah * 4)
    for y in range(ah):
        sy = y if top_down else (ah - 1 - y)
        s = data_off + sy * row
        o = y * aw * 4
        for x in range(aw):
            p = s + x * bpp_bytes
            b, g, r = raw[p], raw[p + 1], raw[p + 2]
            struct.pack_into("<I", out, o + x * 4, (r << 16) | (g << 8) | b)
    return aw, ah, bytes(out)


def BuildRaw(raw: bytes) -> bytes:
    w, h, px = DecodeBmp(raw)
    head = MAGIC + struct.pack("<III", w, h, w)
    print("  anim: %dx%d, %d bytes -> %d bytes" % (w, h, len(raw), len(head) + len(px)))
    return head + px


def GenAssembler(out_dir: str, entries) -> str:
    """生成 blobs.S：每张图一个 .rodata 符号区间。"""
    lines = [
        "/* 自动生成：tools/bootanim_gen.py —— 启动图资源（请勿手改） */",
        "    .section .rodata",
    ]
    for name, ident, _ in entries:
        raw_path = os.path.abspath(os.path.join(out_dir, ident + ".raw"))
        lines += [
            "",
            "/* %s */" % name,
            "    .balign 8",
            "    .globl bootanim_%s_start" % ident,
            "    .globl bootanim_%s_end" % ident,
            "bootanim_%s_start:" % ident,
            '    .incbin "%s"' % raw_path,
            "bootanim_%s_end:" % ident,
        ]
    lines += ["", '    .section .note.GNU-stack,"",@progbits', ""]
    return "\n".join(lines)


def GenTable(entries) -> str:
    """生成 table.c：名称 -> 符号区间 的表（NULL 结尾）。"""
    lines = [
        "/* 自动生成：tools/bootanim_gen.py —— 启动图符号表（请勿手改） */",
        "#include <kernel/bootanim.h>",
        "",
    ]
    for _, ident, _ in entries:
        lines += [
            "extern const uint8_t bootanim_%s_start[];" % ident,
            "extern const uint8_t bootanim_%s_end[];" % ident,
        ]
    lines += ["", "const bootanim_blob_t g_bootanim_blobs[] = {"]
    if not entries:
        lines += ["    /* boot/anim 下暂无 .bmp：空表，内核跳过启动图标 */"]
    for name, ident, _ in entries:
        lines.append('    { "%s", bootanim_%s_start, bootanim_%s_end },'
                     % (name, ident, ident))
    lines += ["    { 0, 0, 0 },   /* 哨兵：name == NULL 结束 */", "};", ""]
    return "\n".join(lines)


def Main(argv):
    if len(argv) != 3:
        print("用法: bootanim_gen.py <anim_dir> <out_dir>")
        return 2
    anim_dir, out_dir = argv[1], argv[2]
    os.makedirs(out_dir, exist_ok=True)

    bmps = []
    if os.path.isdir(anim_dir):
        bmps = sorted(f for f in os.listdir(anim_dir)
                      if f.lower().endswith(".bmp"))
    print("==> 启动图：%d 个 bmp 来自 %s" % (len(bmps), anim_dir))

    entries = []
    used = set()
    for fn in bmps:
        name = os.path.splitext(fn)[0]
        ident = SafeIdent(name)
        # 防御：不同文件名可能映射到同一标识符，去重避免符号冲突
        base, k = ident, 2
        while ident in used:
            ident = "%s_%d" % (base, k)
            k += 1
        used.add(ident)

        path = os.path.join(anim_dir, fn)
        try:
            with open(path, "rb") as f:
                raw = f.read()
            blob = BuildRaw(raw)
        except Exception as ex:  # noqa
            print("  anim: 跳过 %s（%s）" % (fn, ex))
            continue
        with open(os.path.join(out_dir, ident + ".raw"), "wb") as f:
            f.write(blob)
        entries.append((name, ident, len(blob)))

    with open(os.path.join(out_dir, "blobs.S"), "w", encoding="utf-8") as f:
        f.write(GenAssembler(out_dir, entries))
    with open(os.path.join(out_dir, "table.c"), "w", encoding="utf-8") as f:
        f.write(GenTable(entries))

    print("==> 已生成 %s/blobs.S 与 %s/table.c（有效启动图 %d 张）"
          % (out_dir, out_dir, len(entries)))
    for name, ident, n in entries:
        print("    BootLogoID \"%s\"  %d bytes" % (name, n))
    return 0


if __name__ == "__main__":
    sys.exit(Main(sys.argv))
