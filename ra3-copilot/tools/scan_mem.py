#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""scan_mem.py - 在游戏进程内存里按多种编码搜字符串（跨位数读，64 位 Python 读 32 位进程）

用途：找游戏**运行时**真正在用的文本。例：日冕的新建筑在 .big 里只有英文名，
      但游戏 UI 显示中文 —— 那份中文在压缩的字符串表（CSF）里，磁盘搜不到，只能扫内存。

用法：python scan_mem.py <进程名> <要搜的文本> [--max N]
例：  python scan_mem.py ra3_1.12.game 护盾发生器
"""
import ctypes
import sys
from ctypes import wintypes as wt

k32 = ctypes.WinDLL('kernel32', use_last_error=True)

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
MEM_COMMIT = 0x1000
PAGE_GUARD = 0x100
PAGE_NOACCESS = 0x01
READABLE = 0x02 | 0x04 | 0x20 | 0x40 | 0x80   # READONLY / READWRITE / EXECUTE_READ*


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_void_p), ('AllocationBase', ctypes.c_void_p),
                ('AllocationProtect', wt.DWORD), ('RegionSize', ctypes.c_size_t),
                ('State', wt.DWORD), ('Protect', wt.DWORD), ('Type', wt.DWORD)]


class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ProcessID', wt.DWORD),
                ('th32DefaultHeapID', ctypes.c_void_p), ('th32ModuleID', wt.DWORD),
                ('cntThreads', wt.DWORD), ('th32ParentProcessID', wt.DWORD),
                ('pcPriClassBase', ctypes.c_long), ('dwFlags', wt.DWORD),
                ('szExeFile', ctypes.c_char * 260)]


k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
k32.Process32First.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32)]
k32.Process32Next.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32)]
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]


def find_pid(exe):
    snap = k32.CreateToolhelp32Snapshot(0x2, 0)
    pe = PROCESSENTRY32(); pe.dwSize = ctypes.sizeof(pe)
    ok = k32.Process32First(snap, ctypes.byref(pe))
    while ok:
        if pe.szExeFile.decode('mbcs', 'replace').lower() == exe.lower():
            k32.CloseHandle(snap)
            return pe.th32ProcessID
        ok = k32.Process32Next(snap, ctypes.byref(pe))
    k32.CloseHandle(snap)
    return 0


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    exe, needle = sys.argv[1], sys.argv[2]
    max_hits = 12
    if '--max' in sys.argv:
        max_hits = int(sys.argv[sys.argv.index('--max') + 1])

    pid = find_pid(exe)
    if not pid:
        print('[x] %s 没在跑' % exe)
        return 1
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        print('[x] OpenProcess 失败 err=%d（试试管理员）' % ctypes.get_last_error())
        return 1

    pats = {
        'utf-8': needle.encode('utf-8'),
        'utf-16le': needle.encode('utf-16-le'),
        'utf-16le~': bytes(b ^ 0xFF for b in needle.encode('utf-16-le')),   # RA3 CSF 是按位取反的 UTF-16LE
        'utf-16be': needle.encode('utf-16-be'),
    }
    print('[*] pid=%d 搜 "%s"' % (pid, needle))

    buf = ctypes.create_string_buffer(1 << 20)
    addr, hits, scanned, regions = 0, 0, 0, 0
    while addr < 0xFFFFFFFF:
        mbi = MBI()
        if not k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        base = mbi.BaseAddress or 0
        size = mbi.RegionSize or 0x1000
        if mbi.State == MEM_COMMIT and (mbi.Protect & READABLE) and not (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)):
            regions += 1
            off = 0
            while off < size and hits < max_hits:
                chunk = min(1 << 20, size - off)
                got = ctypes.c_size_t(0)
                if k32.ReadProcessMemory(h, ctypes.c_void_p(base + off), buf, chunk, ctypes.byref(got)) and got.value:
                    data = buf.raw[:got.value]
                    scanned += got.value
                    for k, v in pats.items():
                        i = data.find(v)
                        while i >= 0 and hits < max_hits:
                            a = base + off + i
                            lo = max(0, i - 48); hi = min(len(data), i + len(v) + 48)
                            print('  HIT %-9s @ 0x%08X  %r' % (k, a, data[lo:hi]))
                            hits += 1
                            i = data.find(v, i + 1)
                off += chunk
        addr = base + size
        if size == 0:
            break
    print('[*] 扫过 %.0f MB / %d 个区域，命中 %d' % (scanned / 1048576, regions, hits))


if __name__ == '__main__':
    raise SystemExit(main())
