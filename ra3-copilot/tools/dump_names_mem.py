#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""dump_names_mem.py - 从游戏进程内存里导出「对象模板名 → 官方中文名」对照表

背景：日冕新对象的英文名存在 .big 里（未压缩，能直接读），但**中文名只存在于
      压缩的字符串表 (CSF) 里**，磁盘上搜不到。游戏运行时这张表已解密在内存中，
      形如：  KEY\0 [~16 字节元数据] UTF-16LE 中文值

做法：全内存扫描 → 找 UTF-16LE 的中文串 → 向前回溯最近的 ASCII 键 → 配对。
产出：unit-codes/data/cn_names_mem.json  { "AlliedAegisLargeDefenseBase": "埃癸斯护盾发生器", ... }
      以及仅含 NAME: 前缀（对象显示名）的 cn_names.json

用法：python dump_names_mem.py ra3_1.12.game
"""
import ctypes
import json
import os
import re
import sys
from ctypes import wintypes as wt

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
PROCESS_VM_READ, PROCESS_QUERY_INFORMATION = 0x0010, 0x0400
MEM_COMMIT, PAGE_GUARD, PAGE_NOACCESS = 0x1000, 0x100, 0x01
READABLE = 0x02 | 0x04 | 0x20 | 0x40 | 0x80

HERE = os.path.dirname(os.path.abspath(__file__))
WS = os.path.dirname(os.path.dirname(HERE))
OUT_ALL = os.path.join(WS, 'unit-codes', 'data', 'cn_names_mem.json')


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
k32.Process32First.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32)]
k32.Process32Next.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32)]
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]

KEY_RE = re.compile(rb'[A-Za-z_][A-Za-z0-9_:.\-]{2,63}')


def find_pid(exe):
    snap = k32.CreateToolhelp32Snapshot(0x2, 0)
    pe = PROCESSENTRY32(); pe.dwSize = ctypes.sizeof(pe)
    ok = k32.Process32First(snap, ctypes.byref(pe))
    while ok:
        if pe.szExeFile.decode('mbcs', 'replace').lower() == exe.lower():
            k32.CloseHandle(snap); return pe.th32ProcessID
        ok = k32.Process32Next(snap, ctypes.byref(pe))
    k32.CloseHandle(snap); return 0


def cjk_runs(data):
    """返回 [(start, end)] —— UTF-16LE 的中文串（>=2 个汉字）"""
    out, start, i, n = [], -1, 0, len(data) - 1
    while i < n:
        cp = data[i] | (data[i + 1] << 8)
        hit = (0x4E00 <= cp <= 0x9FFF) or (0x3000 <= cp <= 0x303F) or (0xFF01 <= cp <= 0xFF5E)
        if hit:
            if start < 0:
                start = i
        else:
            if start >= 0:
                if i - start >= 4:
                    out.append((start, i))
                start = -1
        i += 2
    if start >= 0 and n - start >= 4:
        out.append((start, n))
    return out


def key_before(data, pos, maxback=72):
    lo = max(0, pos - maxback)
    ms = list(KEY_RE.finditer(data[lo:pos]))
    if not ms:
        return None
    # 键与值之间还夹着元数据，取「离值最近、且后面紧跟 \x00 或元数据」的那个
    m = ms[-1]
    tail = data[lo + m.end():pos]
    if len(tail) > 40:          # 离太远，多半不是它的键
        return None
    return m.group().decode('ascii', 'replace')


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else 'ra3_1.12.game'
    pid = find_pid(exe)
    if not pid:
        print('[x] %s 没在跑' % exe); return 1
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        print('[x] OpenProcess 失败'); return 1
    rng = None
    if '--range' in sys.argv:
        i = sys.argv.index('--range')
        rng = (int(sys.argv[i + 1], 0), int(sys.argv[i + 2], 0))
    print('[*] pid=%d，开始扫内存… %s' % (pid, ('范围 %08X-%08X' % rng) if rng else '全量'))

    pairs = {}
    addr, scanned = (rng[0] if rng else 0), 0
    addr_hi = rng[1] if rng else 0xFFFFFFFF
    buf = ctypes.create_string_buffer(1 << 21)
    while addr < addr_hi:
        mbi = MBI()
        if not k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        base, size = mbi.BaseAddress or 0, mbi.RegionSize or 0x1000
        if mbi.State == MEM_COMMIT and (mbi.Protect & READABLE) and not (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)):
            off = 0
            while off < size:
                chunk = min(1 << 21, size - off)
                got = ctypes.c_size_t(0)
                if k32.ReadProcessMemory(h, ctypes.c_void_p(base + off), buf, chunk, ctypes.byref(got)) and got.value:
                    data = buf.raw[:got.value]
                    scanned += got.value
                    try:
                        data.decode('ascii')          # 纯 ASCII 块不可能含 UTF-16 中文 → 跳过（快）
                        off += chunk
                        continue
                    except UnicodeDecodeError:
                        pass
                    for s, e in cjk_runs(data):
                        k = key_before(data, s)
                        if not k:
                            continue
                        val = data[s:e].decode('utf-16-le', 'replace')
                        if 2 <= len(val) <= 40:
                            pairs.setdefault(k, val)
                off += chunk
        if size == 0:
            break
        addr = base + size

    print('[*] 扫过 %.0f MB，提取 %d 条' % (scanned / 1048576, len(pairs)))
    os.makedirs(os.path.dirname(OUT_ALL), exist_ok=True)
    with open(OUT_ALL, 'w', encoding='utf-8') as f:
        json.dump(pairs, f, ensure_ascii=False, indent=1, sort_keys=True)
    print('[ok] 写入 %s' % OUT_ALL)

    bare = {k: v for k, v in pairs.items() if not k.upper().startswith(('NAME:', 'DESC:', 'TYPE:'))}
    print('    其中「裸键」（对象模板名，无前缀）：%d 条' % len(bare))
    for k in list(bare)[:15]:
        print('      %-36s %s' % (k, bare[k]))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
