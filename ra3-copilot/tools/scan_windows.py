#!/usr/bin/env python3
"""scan_windows.py - 扫游戏进程窗口结构（顶层 + 子窗口 + 样式/扩展样式）
用途：查看叠加面板应挂为"顶层窗口"还是"游戏窗口的子窗口(WS_CHILD)"
用法：先运行游戏，再跑本脚本
"""
import ctypes
from ctypes import wintypes

u32 = ctypes.WinDLL('user32', use_last_error=True)
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)


class PE32(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_void_p),
                ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", ctypes.c_long),
                ("dwFlags", wintypes.DWORD), ("szExeFile", ctypes.c_char * 260)]


GWL_STYLE, GWL_EXSTYLE = -16, -20
STYLE_FLAGS = [(0x40000000, 'WS_CHILD'), (0x80000000, 'WS_POPUP'), (0x10000000, 'WS_VISIBLE'),
               (0x00C00000, 'WS_CAPTION'), (0x00080000, 'WS_SYSMENU'), (0x00040000, 'WS_THICKFRAME')]
EX_FLAGS = [(0x00080000, 'WS_EX_LAYERED'), (0x08000000, 'WS_EX_NOACTIVATE'),
            (0x00000008, 'WS_EX_TOPMOST'), (0x00000080, 'WS_EX_TOOLWINDOW'),
            (0x00000020, 'WS_EX_TRANSPARENT'), (0x00000100, 'WS_EX_WINDOWEDGE')]

results = []


def find_pid(name):
    snap = k32.CreateToolhelp32Snapshot(2, 0)
    pe = PE32(); pe.dwSize = ctypes.sizeof(pe); pid = 0
    if k32.Process32First(snap, ctypes.byref(pe)):
        while True:
            if pe.szExeFile.decode('ascii', 'ignore').lower() == name.lower():
                pid = pe.th32ProcessID; break
            if not k32.Process32Next(snap, ctypes.byref(pe)):
                break
    k32.CloseHandle(snap)
    return pid


def info(h):
    p = wintypes.DWORD()
    u32.GetWindowThreadProcessId(h, ctypes.byref(p))
    cls = ctypes.create_unicode_buffer(256); u32.GetClassNameW(h, cls, 256)
    ttl = ctypes.create_unicode_buffer(256); u32.GetWindowTextW(h, ttl, 256)
    st = u32.GetWindowLongW(h, GWL_STYLE) & 0xFFFFFFFF
    ex = u32.GetWindowLongW(h, GWL_EXSTYLE) & 0xFFFFFFFF
    r = wintypes.RECT(); u32.GetWindowRect(h, ctypes.byref(r))
    owner = u32.GetWindow(h, 4) or 0
    parent = u32.GetParent(h) or 0
    sfl = [n for v, n in STYLE_FLAGS if st & v]
    efl = [n for v, n in EX_FLAGS if ex & v]
    return (h, p.value, cls.value, ttl.value, st, ex,
            (r.left, r.top, r.right - r.left, r.bottom - r.top),
            int(u32.IsWindowVisible(h)), owner, parent, sfl, efl)


def child_cb(h, lp):
    results.append(('  CHILD', info(h)))
    return True


CHILD_C = CB(child_cb)


def top_cb(h, lp):
    i = info(h)
    if i[1] == PID:
        results.append(('TOP', i))
        u32.EnumChildWindows(h, CHILD_C, 0)
    return True


TOP_C = CB(top_cb)
PID = 0

if __name__ == '__main__':
    PID = find_pid('ra3_1.12.game')
    if not PID:
        print('[x] 游戏未运行'); raise SystemExit(1)
    print(f'[*] game pid={PID}')
    u32.EnumWindows(TOP_C, 0)
    for kind, (h, p, cls, ttl, st, ex, rect, vis, owner, parent, sfl, efl) in results:
        print(f'{kind} hwnd=0x{h:08X} vis={vis} rect={rect}')
        print(f'      class="{cls}"  title="{ttl}"')
        print(f'      style=0x{st:08X} {sfl}')
        print(f'      exstyle=0x{ex:08X} {efl}   owner=0x{owner:08X} parent=0x{parent:08X}')
    if not results:
        print('[!] 该进程没有任何窗口')
