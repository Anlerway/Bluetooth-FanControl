#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""构建侧打包器：把发布目录压成 raw LZMA2 归档流，追加到安装器 exe 尾部。

布局（追加在 exe 之后）：
    [payload0 压缩流][payload1 压缩流][64 字节 footer]

footer（小端）：
    char magic[8]   = "FCPAYLD1"
    u32  version    = 2
    u32  flags      bit0=p0 存在, bit1=p1 存在, bit8..15=dictProp0, bit16..23=dictProp1
    u64  p0Offset  p0CompSize  p0RawSize
    u64  p1Offset  p1CompSize  p1RawSize

归档流（解压后的字节，顺序即写入顺序）：
    char magic[8] = "FCARCH01"
    u32 fileCount
    u32 reserved
    entries: { u16 nameLen; u8 name[nameLen]; u64 size; u32 crc32 }
    data:    按 entries 同序拼接的文件内容

用法：
    pack.py --exe build/setup.exe --out FanControl-Setup-1.5.0.exe \
            --payload full=artifacts/release/selfcontained \
            --payload slim=artifacts/release/framework
    pack.py --inspect FanControl-Setup-1.5.0.exe
"""

import argparse
import lzma
import os
import struct
import sys
import time
import zlib

FOOTER_MAGIC = b"FCPAYLD1"
FOOTER_SIZE = 64
FOOTER_FMT = "<8sIIQQQQQQ"
ARCH_MAGIC = b"FCARCH01"
ARCH_VERSION = 2

DICT_SIZE = 1 << 23          # 8 MiB，与 preset 6 默认一致
PRESET = 6
CHUNK = 1 << 20


def dict_prop(dict_size):
    """字典大小 -> LZMA2 props 字节：dict = (2 | (p & 1)) << (p // 2 + 11)。"""
    p = 0
    while p < 40:
        if ((2 | (p & 1)) << (p // 2 + 11)) >= dict_size:
            return p
        p += 1
    return 40


def dict_size_of(prop):
    return (2 | (prop & 1)) << (prop // 2 + 11)


def collect(root):
    """按相对路径排序收集文件：(rel, abspath, size, crc32)。"""
    root = os.path.abspath(root)
    if not os.path.isdir(root):
        raise SystemExit("pack: 目录不存在: %s" % root)
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for fn in sorted(filenames):
            full = os.path.join(dirpath, fn)
            if not os.path.isfile(full):
                continue
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            out.append((rel, full))
    out.sort(key=lambda it: it[0])
    entries = []
    for rel, full in out:
        size = os.path.getsize(full)
        crc = 0
        with open(full, "rb") as f:
            while True:
                buf = f.read(CHUNK)
                if not buf:
                    break
                crc = zlib.crc32(buf, crc)
        entries.append((rel, full, size, crc & 0xFFFFFFFF))
    return entries


def build_archive_header(entries):
    hdr = bytearray()
    hdr += ARCH_MAGIC
    hdr += struct.pack("<II", len(entries), 0)
    for rel, _full, size, crc in entries:
        name = rel.encode("utf-8")
        if len(name) > 0xFFFF:
            raise SystemExit("pack: 路径过长: %s" % rel)
        hdr += struct.pack("<H", len(name))
        hdr += name
        hdr += struct.pack("<QI", size, crc)
    return bytes(hdr)


def pack_payload(root, fp):
    """把 root 压成一段 raw LZMA2 流写入 fp，返回 (compSize, rawSize, dictProp)。"""
    entries = collect(root)
    header = build_archive_header(entries)
    raw_size = len(header) + sum(e[2] for e in entries)

    comp = lzma.LZMACompressor(format=lzma.FORMAT_RAW, filters=[
        {"id": lzma.FILTER_LZMA2, "preset": PRESET, "dict_size": DICT_SIZE}])
    comp_size = 0

    def emit(data):
        nonlocal comp_size
        if data:
            fp.write(data)
            comp_size += len(data)

    started = time.time()
    emit(comp.compress(header))
    for rel, full, size, _crc in entries:
        with open(full, "rb") as f:
            while True:
                buf = f.read(CHUNK)
                if not buf:
                    break
                emit(comp.compress(buf))
    emit(comp.flush())
    elapsed = time.time() - started
    sys.stderr.write("pack: %-6s %4d files  raw %10d  comp %10d  %5.1f%%  %5.1fs\n" % (
        os.path.basename(root.rstrip("/\\")) or root, len(entries), raw_size,
        comp_size, 100.0 * comp_size / max(1, raw_size), elapsed))
    return comp_size, raw_size, dict_prop(DICT_SIZE), len(entries)


def strip_existing_footer(data):
    """如果 exe 尾部已有本工具的 footer，截断到原始 exe 长度。"""
    if len(data) < FOOTER_SIZE:
        return data, None
    footer = data[-FOOTER_SIZE:]
    if footer[:8] != FOOTER_MAGIC:
        return data, None
    fields = struct.unpack(FOOTER_FMT, footer)
    p0_offset = fields[3]
    if 0 < p0_offset <= len(data) - FOOTER_SIZE:
        sys.stderr.write("pack: 已有负载，截断到 %d 字节重新打包\n" % p0_offset)
        return data[:p0_offset], fields
    return data, fields


def make_footer(p0, p1, flags):
    f0 = p0 if p0 is not None else (0, 0, 0, 0)
    f1 = p1 if p1 is not None else (0, 0, 0, 0)
    return struct.pack(FOOTER_FMT, FOOTER_MAGIC, ARCH_VERSION, flags,
                       f0[0], f0[1], f0[2], f1[0], f1[1], f1[2])


def cmd_pack(args):
    with open(args.exe, "rb") as f:
        exe_bytes = f.read()
    exe_bytes, _ = strip_existing_footer(exe_bytes)

    payloads = []
    for spec in args.payload:
        if "=" not in spec:
            raise SystemExit("pack: --payload 需要 NAME=DIR 形式: %s" % spec)
        name, path = spec.split("=", 1)
        payloads.append((name.strip(), path.strip()))
    if not payloads:
        raise SystemExit("pack: 至少需要一个 --payload")

    tmp = args.out + ".tmp"
    with open(tmp, "wb") as fp:
        fp.write(exe_bytes)
        written = []
        for name, path in payloads:
            offset = fp.tell()
            comp_size, raw_size, prop, count = pack_payload(path, fp)
            written.append((name, offset, comp_size, raw_size, prop, count))

    flags = 0
    entries = []
    for i, (name, offset, comp_size, raw_size, prop, count) in enumerate(written):
        if i > 15:
            raise SystemExit("pack: 最多 16 段负载")
        flags |= (1 << i) | ((prop & 0xFF) << (8 + 8 * i))
        entries.append((offset, comp_size, raw_size))
    while len(entries) < 2:
        entries.append(None)

    footer = make_footer(entries[0], entries[1], flags)
    assert len(footer) == FOOTER_SIZE, len(footer)
    with open(tmp, "ab") as fp:
        fp.write(footer)

    os.replace(tmp, args.out)
    total = os.path.getsize(args.out)
    print("pack: -> %s  %.1f MB (exe %.1f MB + 负载 %.1f MB)" % (
        args.out, total / 1048576.0, len(exe_bytes) / 1048576.0,
        (total - len(exe_bytes) - FOOTER_SIZE) / 1048576.0))
    return 0


def cmd_inspect(args):
    with open(args.inspect, "rb") as f:
        data_len = os.path.getsize(args.inspect)
        f.seek(0)
        exe_head = f.read(2)
        if exe_head != b"MZ":
            raise SystemExit("pack: 不是 PE 文件: %s" % args.inspect)
        f.seek(data_len - FOOTER_SIZE)
        footer = f.read(FOOTER_SIZE)
        if footer[:8] != FOOTER_MAGIC:
            raise SystemExit("pack: 没有负载 footer")
        fields = struct.unpack(FOOTER_FMT, footer)
        _, version, flags = fields[0], fields[1], fields[2]
        segs = [(fields[3], fields[4], fields[5]), (fields[6], fields[7], fields[8])]
        print("footer: version=%d flags=0x%04X" % (version, flags))
        for i, (offset, comp_size, raw_size) in enumerate(segs):
            present = bool(flags & (1 << i))
            prop = (flags >> (8 + 8 * i)) & 0xFF
            print("  payload%d present=%s offset=%d comp=%d raw=%d dictProp=%d (dict %d)" % (
                i, present, offset, comp_size, raw_size, prop, dict_size_of(prop)))
            if not present:
                continue
            f.seek(offset)
            blob = f.read(comp_size)
            t0 = time.time()
            raw = lzma.decompress(blob, format=lzma.FORMAT_RAW, filters=[
                {"id": lzma.FILTER_LZMA2, "dict_size": dict_size_of(prop)}])
            dt = time.time() - t0
            if len(raw) != raw_size:
                print("    !! 解压长度不符 expected=%d got=%d" % (raw_size, len(raw)))
            if raw[:8] != ARCH_MAGIC:
                print("    !! 归档 magic 不符: %r" % raw[:8])
                continue
            count, _res = struct.unpack("<II", raw[8:16])
            pos = 16
            names = []
            ok = True
            files = []
            for _ in range(count):
                (nlen,) = struct.unpack("<H", raw[pos:pos + 2])
                pos += 2
                name = raw[pos:pos + nlen].decode("utf-8")
                pos += nlen
                size, crc = struct.unpack("<QI", raw[pos:pos + 12])
                pos += 12
                files.append((name, size, crc))
                if len(names) < 5:
                    names.append(name)
            for name, size, crc in files:
                got = zlib.crc32(raw[pos:pos + size]) & 0xFFFFFFFF
                if got != crc:
                    print("    !! CRC 不符 %s (%08x != %08x)" % (name, got, crc))
                    ok = False
                pos += size
            print("    files=%d crc=%s 解压 %.1fs  例: %s" % (
                count, "OK" if ok else "FAIL", dt, ", ".join(names)))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="FanControl 安装器负载打包器")
    ap.add_argument("--exe", help="未追加负载的安装器 exe")
    ap.add_argument("--out", help="输出文件名")
    ap.add_argument("--payload", action="append", default=[], metavar="NAME=DIR")
    ap.add_argument("--inspect", help="检查已有安装包的 footer 与负载")
    args = ap.parse_args(argv)
    if args.inspect:
        return cmd_inspect(args)
    if not args.exe or not args.out:
        ap.error("需要 --exe 与 --out")
    return cmd_pack(args)


if __name__ == "__main__":
    sys.exit(main())
