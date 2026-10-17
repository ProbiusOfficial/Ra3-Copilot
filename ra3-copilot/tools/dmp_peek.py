#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""dmp_peek.py <minidump> --va 0xADDR --n N      读转储内存
                <minidump> --stack 0xESP --n N    看栈并标注
"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dmp_crash import parse, mem_regions, read_mem, u32

def annot(v):
    if 0x401000 <= v < 0x400000 + 0x93B000:
        return "GAME_CODE +%X" % (v - 0x400000)
    if 0xCAB000 <= v < 0xD31000:
        return "GAME_DATA +%X" % (v - 0x400000)
    if 0x00010000 <= v < 0x01000000:
        return "LOW/heap?"
    if 0x04000000 <= v < 0x20000000:
        return "HEAP?"
    if 0x001A0000 <= v < 0x001C0000:
        return "STACK?"
    if 0x70000000 <= v < 0x80000000:
        return "SYS/模块区?"
    return ""

def main():
    path = sys.argv[1]
    d, streams = parse(path)
    regs = mem_regions(d, streams)
    mode = sys.argv[2]
    va = int(sys.argv[3], 16)
    n = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x100
    if mode == "--va":
        b = read_mem(d, regs, va, n)
        for i in range(0, len(b) - 3, 4):
            v = struct.unpack_from("<I", b, i)[0]
            f = struct.unpack_from("<f", b, i)[0]
            print("%08X: %08X  %12.3f  %s" % (va + i, v, f if -1e12 < f < 1e12 else 0.0, annot(v)))
    elif mode == "--stack":
        b = read_mem(d, regs, va, n)
        for i in range(0, len(b) - 3, 4):
            v = struct.unpack_from("<I", b, i)[0]
            print("%08X: %08X  %s" % (va + i, v, annot(v)))

if __name__ == "__main__":
    main()
