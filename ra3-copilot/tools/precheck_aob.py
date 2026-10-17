#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
precheck_aob.py - 交付前 AOB 预检（零依赖）

从 src/features.cpp 直接用正则抓出：
  - AOB_XXX[N] = { 0x.., ... };   常量数组
  - kHooks[] 表里的 { "Name", 0xRVA, AOB_XXX, N, ... }
然后逐个对着游戏 exe 的 PE 镜像（RVA -> 文件偏移）比对字节。

= 替游戏先跑一遍 fail-closed 校验（VerifyBytes 用的就是这套 AOB + RVA）。
改 hook / 改 AOB 后，先跑本脚本，再让游戏进局。

用法:
  python precheck_aob.py [--game <exe路径>] [--src <features.cpp路径>]
"""
import argparse
import os
import re
import struct
import sys

# 游戏 exe 路径：--game 参数 > RA3_GAME_EXE 环境变量 > 兜底默认值
DEFAULT_GAME = os.environ.get("RA3_GAME_EXE") or r"D:\GAME\Game_RTS\Red Alert 3\Data\ra3_1.12.game"
DEFAULT_SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "features.cpp")


def parse_pe(data):
    assert data[:2] == b"MZ", "不是 MZ"
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[e_lfanew:e_lfanew + 4] == b"PE\0\0", "不是 PE"
    coff = e_lfanew + 4
    num_sections = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    image_base = struct.unpack_from("<I", data, opt + 28)[0] if magic == 0x10B \
        else struct.unpack_from("<Q", data, opt + 24)[0]
    size_of_image = struct.unpack_from("<I", data, opt + 56)[0]
    sect = opt + opt_size
    secs = []
    for i in range(num_sections):
        o = sect + i * 40
        name = data[o:o + 8].rstrip(b"\0").decode("latin1")
        vsize = struct.unpack_from("<I", data, o + 8)[0]
        vaddr = struct.unpack_from("<I", data, o + 12)[0]
        rawsize = struct.unpack_from("<I", data, o + 16)[0]
        rawptr = struct.unpack_from("<I", data, o + 20)[0]
        secs.append((name, vaddr, vsize, rawptr, rawsize))
    return image_base, size_of_image, secs


def rva_to_off(rva, secs):
    for _name, va, vsize, rp, rs in secs:
        span = max(vsize, rs)
        if va <= rva < va + span:
            off = rp + (rva - va)
            if 0 <= (rva - va) < rs:
                return off
            return None  # 落在节末尾对齐区，无文件内容
    return None


def parse_src(src_path):
    txt = open(src_path, "r", encoding="utf-8", errors="ignore").read()
    aobs = {}
    for m in re.finditer(r"const\s+uint8_t\s+(AOB_\w+)\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}", txt):
        name = m.group(1)
        body = m.group(2)
        vals = re.findall(r"0x[0-9A-Fa-f]{1,2}", body)
        aobs[name] = bytes(int(v, 16) & 0xFF for v in vals)
    hooks = []
    for line in txt.splitlines():
        m = re.match(r'\s*\{\s*"(\w+)"\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*(AOB_\w+)\s*,', line)
        if m:
            hooks.append((m.group(1), int(m.group(2), 16), m.group(3)))
    return aobs, hooks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--src", default=os.path.normpath(DEFAULT_SRC))
    args = ap.parse_args()

    if not os.path.exists(args.game):
        print("[x] 游戏 exe 不存在: %s" % args.game)
        return 2
    if not os.path.exists(args.src):
        print("[x] features.cpp 不存在: %s" % args.src)
        return 2

    data = open(args.game, "rb").read()
    image_base, size_of_image, secs = parse_pe(data)
    aobs, hooks = parse_src(args.src)

    print("游戏: %s" % args.game)
    print("ImageBase=0x%08X SizeOfImage=0x%08X 节数=%d" % (image_base, size_of_image, len(secs)))
    print("源码: %s" % args.src)
    print("抓到 AOB 常量 %d 个 / kHooks 条目 %d 个" % (len(aobs), len(hooks)))
    print("-" * 68)

    ok = bad = 0
    for name, rva, aobname in hooks:
        aob = aobs.get(aobname)
        if aob is None:
            print("  %-16s [x] 找不到 AOB 定义 %s" % (name, aobname))
            bad += 1
            continue
        off = rva_to_off(rva, secs)
        if off is None:
            print("  %-16s [x] RVA 0x%06X 无法映射到文件" % (name, rva))
            bad += 1
            continue
        got = data[off:off + len(aob)]
        if got == aob:
            print("  %-16s [OK] RVA 0x%06X  %d 字节匹配" % (name, rva, len(aob)))
            ok += 1
        else:
            exp = " ".join("%02X" % b for b in aob)
            g = " ".join("%02X" % b for b in got)
            print("  %-16s [MISMATCH] RVA 0x%06X" % (name, rva))
            print("                   期望: %s" % exp)
            print("                   实际: %s" % g)
            bad += 1

    print("-" * 68)
    print("结果: %d/%d 通过" % (ok, ok + bad))
    if bad:
        print("[!] 有 %d 处不匹配 —— 游戏加载时这些 hook 会被 fail-closed 跳过" % bad)
    else:
        print("[+] 全部通过，可交付进局")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
