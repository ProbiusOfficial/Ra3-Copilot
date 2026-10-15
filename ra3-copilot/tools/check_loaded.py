#!/usr/bin/env python3
"""check_loaded.py - 验证 ra3copilot.dll 是否已注入游戏进程（模块枚举）
用法: python check_loaded.py [进程名=ra3_1.12.game] [模块名=ra3copilot]
"""
import ctypes, sys
from ctypes import wintypes

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
TH32CS_SNAPPROCESS = 0x2
TH32CS_SNAPMODULE = 0x8
TH32CS_SNAPMODULE32 = 0x10


class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_void_p),
                ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", ctypes.c_long),
                ("dwFlags", wintypes.DWORD), ("szExeFile", ctypes.c_char * 260)]


class MODULEENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("th32ModuleID", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD), ("GlblcntUsage", wintypes.DWORD),
                ("ProccntUsage", wintypes.DWORD), ("modBaseAddr", ctypes.c_void_p),
                ("modBaseSize", wintypes.DWORD), ("hModule", wintypes.HMODULE),
                ("szModule", ctypes.c_char * 256), ("szExePath", ctypes.c_char * 260)]


def find_pid(name):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32(); pe.dwSize = ctypes.sizeof(pe)
    pid = 0
    if k32.Process32First(snap, ctypes.byref(pe)):
        while True:
            if pe.szExeFile.decode('ascii', 'ignore').lower() == name.lower():
                pid = pe.th32ProcessID
                break
            if not k32.Process32Next(snap, ctypes.byref(pe)):
                break
    k32.CloseHandle(snap)
    return pid


def list_modules(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    if snap == wintypes.HANDLE(-1).value:
        return None
    me = MODULEENTRY32(); me.dwSize = ctypes.sizeof(me)
    out = []
    if k32.Module32First(snap, ctypes.byref(me)):
        while True:
            out.append((me.szModule.decode('ascii', 'ignore'),
                        me.modBaseAddr or 0, me.modBaseSize))
            if not k32.Module32Next(snap, ctypes.byref(me)):
                break
    k32.CloseHandle(snap)
    return out


def main():
    proc = sys.argv[1] if len(sys.argv) > 1 else 'ra3_1.12.game'
    want = sys.argv[2] if len(sys.argv) > 2 else 'ra3copilot'
    lines = []

    def say(s):
        print(s)
        lines.append(s)

    k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    k32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    pid = find_pid(proc)
    if not pid:
        say(f'[x] {proc} not running')
        return 1
    mods = list_modules(pid)
    if mods is None:
        say(f'[x] module snapshot denied for pid={pid} (need elevation)')
        return 1
    say(f'[*] pid={pid} modules={len(mods)}')
    hits = [m for m in mods if want.lower() in m[0].lower()]
    for name, base, size in hits:
        say(f'[+] INJECTED: {name}  base=0x{base:08x} size={size}')
    if not hits:
        say(f'[x] {want} NOT loaded')
    # 结果落盘（提权场景下 stdout 可能丢失）
    try:
        import os
        out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'check_loaded_result.txt')
        open(out, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    except Exception:
        pass
    return 0 if hits else 2


if __name__ == '__main__':
    sys.exit(main())
