#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""patch_fz.py - 现场把 DLL 里的 -0.05f 判据常量改成 +1e30f（恒成立），救活相机抓取
用法: patch_fz.py [restore]
"""
import ctypes
import struct
import sys
import subprocess
from ctypes import wintypes

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
TH32CS_SNAPMODULE = 0x00000008
TH32CS_SNAPMODULE32 = 0x00000010
FZ_OFF = 0x8D608          # -0.05f 常量在 DLL 内的偏移
PAGE_READWRITE = 0x04


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

h = k32.OpenProcess(0x0020 | 0x0008 | 0x0010 | 0x0400, False, pid)  # VM_WRITE|VM_OP|VM_READ|QUERY
if not h:
    print("[x] OpenProcess 失败 err=%d（是否够权限？）" % ctypes.get_last_error()); sys.exit(1)

snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
me = MODULEENTRY32(); me.dwSize = ctypes.sizeof(me)
base = 0
if k32.Module32First(ctypes.c_void_p(snap), ctypes.byref(me)):
    while True:
        if "ra3copilot" in me.szModule.decode("latin1", "ignore").lower():
            base = ctypes.cast(me.modBaseAddr, ctypes.c_void_p).value; break
        if not k32.Module32Next(ctypes.c_void_p(snap), ctypes.byref(me)):
            break
k32.CloseHandle(ctypes.c_void_p(snap))
addr = base + FZ_OFF
print("[*] DLL base=0x%08X  常量地址=0x%08X" % (base, addr))

restore = len(sys.argv) > 1 and sys.argv[1] == "restore"
newval = struct.pack("<f", -0.05) if restore else struct.pack("<f", 1e30)
print("[*] 写入 %s (%s)" % (newval.hex(), "-0.05f(回滚)" if restore else "+1e30f(恒成立)"))

old = wintypes.DWORD(0)
if not k32.VirtualProtectEx(h, ctypes.c_void_p(addr), 4, PAGE_READWRITE, ctypes.byref(old)):
    print("[x] VirtualProtectEx 失败 err=%d" % ctypes.get_last_error()); sys.exit(1)
ok = k32.WriteProcessMemory(h, ctypes.c_void_p(addr), newval, 4, None)
k32.VirtualProtectEx(h, ctypes.c_void_p(addr), 4, old.value, ctypes.byref(old))
print("[+] WriteProcessMemory ->", bool(ok), "err=", ctypes.get_last_error())

buf = ctypes.create_string_buffer(4); got = ctypes.c_size_t(0)
k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, 4, ctypes.byref(got))
print("[+] 回读 =", buf.raw[:4].hex(), "=", struct.unpack("<f", buf.raw[:4])[0])
k32.CloseHandle(h)
