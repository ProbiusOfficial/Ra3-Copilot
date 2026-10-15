#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""probe_cursor.py - 手动走"真选中管理器"拿选中单位世界坐标 + 定位落点缓存 g_lastPos"""
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


def find_pid():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq ra3_1.12.game", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True, errors="ignore").stdout.strip()
    if out.startswith('"'):
        return int(out.split('","')[1])
    return 0


pid = find_pid()
if not pid:
    print("[x] 游戏未运行")
    sys.exit(1)
print("[*] pid =", pid)

h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
if not h:
    print("[x] OpenProcess 失败")
    sys.exit(1)


def rd(addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t(0)
    ok = k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got))
    return buf.raw[:got.value] if ok else None


def u32(addr):
    b = rd(addr, 4)
    return struct.unpack("<I", b)[0] if b else 0


def f32(addr):
    b = rd(addr, 4)
    return struct.unpack("<f", b)[0] if b else 0.0


GAME_BASE = 0x400000
SELMGR_RVA = 0x8DB73C

print("=== 真选中管理器链 (RVA 0x8DB73C) ===")
mgr = u32(GAME_BASE + SELMGR_RVA)
print("  [0x%X] mgr = 0x%08X  vt = 0x%08X" % (GAME_BASE + SELMGR_RVA, mgr, u32(mgr)))
if mgr:
    cnt = u32(mgr + 0x5C)
    node = u32(mgr + 0x50)
    print("  +0x5c 选中数 = %d   +0x50 链表头 = 0x%08X" % (cnt, node))
    n = node
    for i in range(min(cnt, 8)):
        if not n:
            break
        obj = u32(n + 8)
        body = u32(obj + 0x138) if obj else 0
        vt = u32(obj) if obj else 0
        x = f32(body + 0x38) if body else 0.0
        y = f32(body + 0x3C) if body else 0.0
        z = f32(body + 0x40) if body else 0.0
        print("  [%d] node=0x%08X obj=0x%08X vt=0x%08X body=0x%08X  pos=(%.1f, %.1f, %.1f)"
              % (i, n, obj, vt, body, x, y, z))
        n = u32(n)   # next

print()
print("=== 定位 ra3copilot.dll + 落点缓存 g_lastPos ===")
snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
me = MODULEENTRY32()
me.dwSize = ctypes.sizeof(me)
base = size = 0
if k32.Module32First(ctypes.c_void_p(snap), ctypes.byref(me)):
    while True:
        name = me.szModule.decode("latin1", "ignore")
        if "ra3copilot" in name.lower():
            base = ctypes.cast(me.modBaseAddr, ctypes.c_void_p).value
            size = me.modBaseSize
            break
        if not k32.Module32Next(ctypes.c_void_p(snap), ctypes.byref(me)):
            break
k32.CloseHandle(ctypes.c_void_p(snap))
print("  ra3copilot.dll base=0x%08X size=0x%X" % (base, size))

if base:
    # g_lastPos 当前应约为 (10.0, 10.0, 210.0, 1.0) —— 在 DLL 数据区里找这个 float 序列
    pat = struct.pack("<4f", 10.0, 10.0, 210.0, 1.0)
    # 也找 (0,0,0,1) 初始值
    pat2 = struct.pack("<4f", 0.0, 0.0, 0.0, 1.0)
    found = []
    step = 0x10000
    off = 0
    data = b""
    while off < size:
        chunk = rd(base + off, min(step, size - off))
        if chunk:
            data += chunk
        else:
            data += b"\x00" * min(step, size - off)
        off += step
    for p, tag in ((pat, "10,10,210,1"), (pat2, "0,0,0,1")):
        i = data.find(p)
        cnt = 0
        while i >= 0 and cnt < 5:
            print("  [%s] @ 0x%08X (DLL+0x%X)" % (tag, base + i, i))
            i = data.find(p, i + 1)
            cnt += 1
k32.CloseHandle(h)
