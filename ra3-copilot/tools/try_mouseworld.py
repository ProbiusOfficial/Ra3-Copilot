#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""try_mouseworld.py - 基于 CE 的做法：调游戏 0x1ED4A0(src,dst,0,0) 求鼠标世界坐标"""
import ctypes
import struct
import subprocess
import urllib.request
from ctypes import wintypes

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq ra3_1.12.game", "/FO", "CSV", "/NH"],
                     capture_output=True, text=True, errors="ignore").stdout.strip()
pid = int(out.split('","')[1]) if out.startswith('"') else 0
if not pid:
    print("[x] 游戏未运行"); raise SystemExit(1)
h = k32.OpenProcess(0x20 | 0x8 | 0x10 | 0x400, False, pid)
GAME = 0x400000


def rd(a, n):
    b = ctypes.create_string_buffer(n); g = ctypes.c_size_t(0)
    ok = k32.ReadProcessMemory(h, ctypes.c_void_p(a), b, n, ctypes.byref(g))
    return b.raw[:g.value] if ok else None


def u32(a):
    b = rd(a, 4)
    return struct.unpack("<I", b)[0] if b else 0


cur = u32(GAME + 0x8DAEFC)
print("[*] [0x8DAEFC] 对象 = 0x%08X" % cur)
if not cur:
    print("[x] 光标对象为空"); raise SystemExit(1)

src = cur + 0x3C
sb = rd(src, 12)
print("[*] src(0x%08X) 3 个 dword = %s" % (src, sb.hex() if sb else "?"))
print("    作为 int  = %s" % (struct.unpack("<3i", sb) if sb else "?"))
print("    作为 float= %s" % (struct.unpack("<3f", sb) if sb else "?"))

k32.VirtualAllocEx.restype = ctypes.c_void_p
k32.VirtualAllocEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                               ctypes.c_uint32, ctypes.c_uint32]
dst = k32.VirtualAllocEx(h, None, 32, 0x1000 | 0x2000, 0x04)
print("[*] dst 缓冲 = 0x%08X" % (dst or 0))

url = "http://127.0.0.1:17832/call?rva=0x1ED4A0&cc=cdecl&a0=0x%X&a1=0x%X&a2=0" % (src, dst)
try:
    r = urllib.request.urlopen(url, timeout=12).read().decode()
    print("[*] /call -> %s" % r.strip())
except Exception as e:
    print("[x] /call 失败:", e)

db = rd(dst, 16)
if db:
    print("[+] dst 结果 = %s" % db.hex())
    print("    作为 float = %s" % (struct.unpack("<4f", db),))
k32.VirtualFreeEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32]
k32.VirtualFreeEx(h, ctypes.c_void_p(dst), 0, 0x8000)
k32.CloseHandle(h)
