#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
exe_fingerprint.py - 游戏 exe 构建指纹（零依赖）

hook RVA 绑定的不是「版本号」而是**具体那一份二进制**：同为 v1.12.3444.25830
的不同发行版（Steam / 整合包 / 被启动器静态改过的版本）大小与代码布局都不同，
表现为 `precheck_aob.py` 全部失配。本脚本只读文件，打印可粘贴的指纹。

用法:
  python exe_fingerprint.py [--game <exe路径>]
"""
import argparse
import hashlib
import os
import struct
import sys
import time

DEFAULT_GAME = os.environ.get("RA3_GAME_EXE") or r"C:\GAME\Red Alert 3\Data\ra3_1.12.game"

# 受支持的构建（hook RVA / AOB 取自这份二进制，precheck_aob.py 应 30/30）
KNOWN = {
    "size": 9306120,
    "sha256": "518ef27297d490590f0315b6143cc7ab0010bf606d2f351ca64d447c3f7b0221",
    "label": "1.12 原版 game exe（未改）",
}


def parse_pe(data):
    if data[:2] != b"MZ":
        raise ValueError("不是 MZ")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise ValueError("不是 PE")
    coff = e_lfanew + 4
    machine, num_sections = struct.unpack_from("<HH", data, coff)
    timestamp = struct.unpack_from("<I", data, coff + 4)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    characteristics = struct.unpack_from("<H", data, coff + 18)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic == 0x10B:
        entry = struct.unpack_from("<I", data, opt + 16)[0]
        image_base = struct.unpack_from("<I", data, opt + 28)[0]
        size_of_image = struct.unpack_from("<I", data, opt + 56)[0]
        checksum = struct.unpack_from("<I", data, opt + 64)[0]
    else:
        entry = struct.unpack_from("<I", data, opt + 16)[0]
        image_base = struct.unpack_from("<Q", data, opt + 24)[0]
        size_of_image = struct.unpack_from("<I", data, opt + 56)[0]
        checksum = struct.unpack_from("<I", data, opt + 64)[0]
    sect = opt + opt_size
    secs = []
    for i in range(num_sections):
        o = sect + i * 40
        name = data[o:o + 8].rstrip(b"\0").decode("latin1")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, o + 8)
        secs.append((name, vaddr, vsize, rawptr, rawsize))
    return {
        "machine": machine, "timestamp": timestamp, "characteristics": characteristics,
        "entry": entry, "image_base": image_base, "size_of_image": size_of_image,
        "checksum": checksum, "sections": secs,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=DEFAULT_GAME)
    a = ap.parse_args()

    if not os.path.isfile(a.game):
        print(f"[!] 找不到文件: {a.game}")
        return 2
    data = open(a.game, "rb").read()
    try:
        pe = parse_pe(data)
    except ValueError as e:
        print(f"[!] {e}: {a.game}")
        return 2

    print(f"文件      {os.path.abspath(a.game)}")
    print(f"大小      {len(data)} ({len(data):#x})")
    print(f"MD5       {hashlib.md5(data).hexdigest()}")
    print(f"SHA1      {hashlib.sha1(data).hexdigest()}")
    print(f"SHA256    {hashlib.sha256(data).hexdigest()}")
    print(f"链接时间  {time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(pe['timestamp']))} UTC "
          f"({pe['timestamp']:#x})")
    print(f"机器      {pe['machine']:#06x}   特征 {pe['characteristics']:#06x}   "
          f"节数 {len(pe['sections'])}")
    print(f"入口      {pe['entry']:#x}   ImageBase {pe['image_base']:#x}   "
          f"SizeOfImage {pe['size_of_image']:#x}   PE校验和 {pe['checksum']:#x}")
    print()
    print(f"{'节':8s} {'VA':>10s} {'VSize':>10s} {'RawPtr':>10s} {'RawSize':>10s}  SHA256(前16)")
    for name, vaddr, vsize, rawptr, rawsize in pe["sections"]:
        body = data[rawptr:rawptr + rawsize]
        print(f"{name:8s} {vaddr:#010x} {vsize:#010x} {rawptr:#010x} {rawsize:#010x}  "
              f"{hashlib.sha256(body).hexdigest()[:16]}")

    print()
    if len(data) == KNOWN["size"]:
        print(f"[+] 大小与受支持构建一致（{KNOWN['label']}）")
        print("[+] SHA256 一致" if hashlib.sha256(data).hexdigest() == KNOWN["sha256"]
              else "[!] SHA256 不一致 —— 同尺寸但内容不同，同样不受支持")
    else:
        print(f"[-] 不是受支持构建。受支持: 大小 {KNOWN['size']} / "
              f"SHA256 {KNOWN['sha256']}")
        print("    把上面的输出贴到 issue，用于判定是哪个发行版。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
