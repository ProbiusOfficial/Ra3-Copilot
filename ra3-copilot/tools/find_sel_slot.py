#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""find_sel_slot.py - 在 ra3copilot.dll 内存里定位 symres 的 s_sel 槽（当前存着假地址 0xCE3EE8）"""
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
print("[*] ra3copilot.dll base=0x%08X size=0x%X" % (base, size))

# 整块读 DLL 映像
blob = b""
off = 0
while off < size:
    c = rd(base + off, min(0x20000, size - off))
    blob += c if c else b"\x00" * min(0x20000, size - off)
    off += 0x20000

# 1) 搜假地址 0xCE3EE8
needle = struct.pack("<I", 0x00CE3EE8)
print("--- 含 0xCE3EE8 的位置 ---")
i = blob.find(needle)
while i >= 0:
    # 槽结构: [addr(4)][how_ptr(4)]；how_ptr 应指向 "结构推导" 字符串
    how = struct.unpack("<I", blob[i + 4:i + 8])[0] if i + 8 <= len(blob) else 0
    tip = ""
    if base <= how < base + size:
        s = blob[how - base:how - base + 24]
        tip = s.split(b"\x00")[0].decode("utf-8", "ignore")
    print("  DLL+0x%X -> 0x%08X   how=0x%08X \"%s\"" % (i, base + i, how, tip))
    i = blob.find(needle, i + 1)

# 2) 搜真地址 0xCDB73C 作为对照
print("--- 含 0xCDB73C 的位置（不应出现在 s_sel 槽，除非已是真值） ---")
needle2 = struct.pack("<I", 0x00CDB73C)
i = blob.find(needle2); c = 0
while i >= 0 and c < 6:
    print("  DLL+0x%X" % i); i = blob.find(needle2, i + 1); c += 1
k32.CloseHandle(h)
