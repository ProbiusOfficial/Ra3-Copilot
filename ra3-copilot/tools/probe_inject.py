#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""probe_inject.py - 分离诊断注入失败原因（权限 vs 参数）

只有 CreateRemoteThread(LoadLibrary) 受 WOW64 位数限制；
OpenProcess / VirtualAllocEx / WriteProcessMemory 跨位数都能跑。
"""
import ctypes
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)

# 找到游戏 pid
import subprocess
pid = 0
try:
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq ra3_1.12.game", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True, errors="ignore").stdout
    # CSV: "ra3_1.12.game","34368",...
    line = out.strip().splitlines()[0] if out.strip() else ""
    if line.startswith('"'):
        pid = int(line.split('","')[1])
except Exception as e:
    print("find pid failed:", e)

if not pid:
    print("[x] 未找到 ra3_1.12.game 进程")
    sys.exit(1)
print("[*] game pid =", pid)

PROC = 0x0002 | 0x0400 | 0x0008 | 0x0020   # CREATE_THREAD|QUERY_INFORMATION|VM_OPERATION|VM_WRITE
h = k32.OpenProcess(PROC, False, pid)
err = ctypes.get_last_error()
print("[1] OpenProcess(0x%04X) -> %s  err=%d %s" % (
    PROC, h, err, "" if h else "(ACCESS_DENIED=5 表示权限不足)"))

if not h:
    print("    => 需要管理员权限（游戏提权运行）")
    sys.exit(2)

k32.VirtualAllocEx.restype = ctypes.c_void_p
k32.VirtualAllocEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                               ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
MEM_COMMIT, MEM_RESERVE, PAGE_READWRITE = 0x1000, 0x2000, 0x04
SZ = 520

a = k32.VirtualAllocEx(h, None, SZ, MEM_COMMIT, PAGE_READWRITE)
print("[2] VirtualAllocEx(MEM_COMMIT) -> 0x%X  err=%d" % (a or 0, ctypes.get_last_error()))

b = k32.VirtualAllocEx(h, None, SZ, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
print("[3] VirtualAllocEx(MEM_COMMIT|RESERVE) -> 0x%X  err=%d" % (b or 0, ctypes.get_last_error()))

# 顺带验证写内存 + 读回
if b:
    k32.WriteProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                       ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p]
    buf = ctypes.create_string_buffer(b"RA3PROBE\x00")
    ok = k32.WriteProcessMemory(h, ctypes.c_void_p(b), buf, len(buf), None)
    print("[4] WriteProcessMemory ->", bool(ok), "err", ctypes.get_last_error())
    k32.VirtualFreeEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32]
    k32.VirtualFreeEx(h, ctypes.c_void_p(b), 0, 0x8000)

k32.CloseHandle(h)
print("[ok] 结论：看 [1] 是否失败 -> 权限；[2] 失败而 [3] 成功 -> injector 参数 bug（缺 MEM_RESERVE）")
