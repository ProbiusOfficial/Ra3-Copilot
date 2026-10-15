#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""find_fz.py - 在 ra3copilot.dll 里定位 -0.05f 常量及其引用指令（用于现场救活相机判据）"""
import ctypes
import struct
import sys
import subprocess
from ctypes import wintypes

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPMODULE = 0x00000008
TH32CS_SNAPMODULE32 = 0x00000010


class MODULEENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("th32ModuleID", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD), ("GlblcntUsage", wintypes.DWORD),
                ("ProccntUsage", wintypes.DWORD),
                ("modBaseAddr", ctypes.POINTER(ctypes.c_byte)),
                ("modBaseSize", wintypes.DWORD), ("hModule", wintypes.HMODULE),
                ("szModule", ctypes.c_char * 256), ("szExePath", ctypes.c_char * 260)]


out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq ra3_1.12.game", "/FO", "CSV", "/NH"],
                     capture_output=True, text=True, errors="ignore").stdout.strip()
pid = int(out.split('","')[1]) if out.startswith('"') else 0
if not pid:
    print("[x] 游戏未运行"); sys.exit(1)
h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
if not h:
    print("[x] OpenProcess 失败"); sys.exit(1)


def rd(addr, n):
    buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t(0)
    ok = k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got))
    return buf.raw[:got.value] if ok else None


snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
me = MODULEENTRY32(); me.dwSize = ctypes.sizeof(me)
base = size = 0
if k32.Module32First(ctypes.c_void_p(snap), ctypes.byref(me)):
    while True:
        if "ra3copilot" in me.szModule.decode("latin1", "ignore").lower():
            base = ctypes.cast(me.modBaseAddr, ctypes.c_void_p).value
            size = me.modBaseSize; break
        if not k32.Module32Next(ctypes.c_void_p(snap), ctypes.byref(me)):
            break
k32.CloseHandle(ctypes.c_void_p(snap))
print("[*] DLL base=0x%08X size=0x%X" % (base, size))

blob = b""
off = 0
while off < size:
    c = rd(base + off, min(0x40000, size - off))
    blob += c if c else b"\x00" * min(0x40000, size - off)
    off += 0x40000

# -0.05f = CD CC 4C BD
pat = struct.pack("<f", -0.05)
print("[*] -0.05f 字节 =", pat.hex())
addrs = []
i = blob.find(pat)
while i >= 0:
    addrs.append(base + i)
    print("  常量 @ 0x%08X (DLL+0x%X)" % (base + i, i))
    i = blob.find(pat, i + 1)

# 找引用这些常量的 comiss(xmm0, m32) = 0F 2F 05 <imm32>；也找 movss xmm, m32 = F3 0F 10 05
print("--- 引用指令 ---")
for a in addrs:
    ref = struct.pack("<I", a)
    for tag, pref in (("comiss xmm0,[c]", b"\x0f\x2f\x05"),
                      ("comiss xmm1,[c]", b"\x0f\x2f\x0d"),
                      ("movss xmm0,[c]", b"\xf3\x0f\x10\x05"),
                      ("movss xmm1,[c]", b"\xf3\x0f\x10\x0d")):
        p = pref + ref
        j = blob.find(p)
        while j >= 0:
            print("  %s @ 0x%08X (DLL+0x%X) 引用 0x%08X" % (tag, base + j, j, a))
            j = blob.find(p, j + 1)
k32.CloseHandle(h)
