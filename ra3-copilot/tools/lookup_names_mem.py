#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""lookup_names_mem.py - 用「对象模板名」去游戏内存里查它的官方中文名

原理：游戏的字符串表（CSF）解密后驻留在内存里，条目形如
        <模板名>\0 [12~20 字节元数据] <UTF-16LE 中文名>
      比"全表盲提取"可靠得多：直接用模板名精确定位，再校验值必须是纯中文。

产出：unit-codes/data/cn_names_mem.json  { "AlliedAegisLargeDefenseBase": "埃癸斯护盾发生器", ... }

用法：python lookup_names_mem.py ra3_1.12.game [--range 0x03300000 0x03600000]
"""
import csv
import ctypes
import io
import json
import os
import sys
from ctypes import wintypes as wt

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
PROCESS_VM_READ, PROCESS_QUERY_INFORMATION = 0x0010, 0x0400
MEM_COMMIT, PAGE_GUARD, PAGE_NOACCESS = 0x1000, 0x100, 0x01
READABLE = 0x02 | 0x04 | 0x20 | 0x40 | 0x80

HERE = os.path.dirname(os.path.abspath(__file__))
WS = os.path.dirname(os.path.dirname(HERE))
FINAL_TSV = os.path.join(WS, 'unit-codes', 'data', 'callable_final.tsv')
TRAINER_TSV = os.path.join(WS, 'unit-codes', 'data', 'unit_codes_trainer.tsv')
OUT = os.path.join(WS, 'unit-codes', 'data', 'cn_names_mem.json')


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

CJK_OK = set('·—…「」『』【】（）《》、，。！？：；')


def find_pid(exe):
    snap = k32.CreateToolhelp32Snapshot(0x2, 0)
    pe = PROCESSENTRY32(); pe.dwSize = ctypes.sizeof(pe)
    ok = k32.Process32First(snap, ctypes.byref(pe))
    while ok:
        if pe.szExeFile.decode('mbcs', 'replace').lower() == exe.lower():
            k32.CloseHandle(snap); return pe.th32ProcessID
        ok = k32.Process32Next(snap, ctypes.byref(pe))
    k32.CloseHandle(snap); return 0


def read_range(h, lo, hi):
    """把 [lo,hi) 里所有可读区域读出来 → [(base, bytes)]"""
    out, addr = [], lo
    while addr < hi:
        mbi = MBI()
        if not k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        base = mbi.BaseAddress or 0
        size = mbi.RegionSize or 0x1000
        lo2, hi2 = max(base, lo), min(base + size, hi)
        if hi2 > lo2 and mbi.State == MEM_COMMIT and (mbi.Protect & READABLE) \
           and not (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)):
            n = hi2 - lo2
            b = ctypes.create_string_buffer(n + 8)
            got = ctypes.c_size_t(0)
            if k32.ReadProcessMemory(h, ctypes.c_void_p(lo2), b, n, ctypes.byref(got)) and got.value:
                out.append((lo2, b.raw[:got.value]))
        addr = base + size
    return out


def utf16_cjk_at(data, pos, maxchars=40):
    """从 pos 开始解 UTF-16LE；返回纯中文串（不足 2 字返回 None）"""
    chars, i = [], pos
    while i + 1 < len(data) and len(chars) < maxchars:
        cp = data[i] | (data[i + 1] << 8)
        if 0x4E00 <= cp <= 0x9FFF or chr(cp) in CJK_OK:
            chars.append(chr(cp)); i += 2
        else:
            break
    if len(chars) < 2:
        return None
    return ''.join(chars)


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else 'ra3_1.12.game'
    lo, hi = 0x03000000, 0x04000000
    if '--range' in sys.argv:
        i = sys.argv.index('--range')
        lo, hi = int(sys.argv[i + 1], 0), int(sys.argv[i + 2], 0)
    pid = find_pid(exe)
    if not pid:
        print('[x] %s 没在跑' % exe); return 1
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        print('[x] OpenProcess 失败'); return 1

    chunks = read_range(h, lo, hi)
    total = sum(len(b) for _, b in chunks)
    print('[*] pid=%d 读到 %d 个区域 / %.1f MB' % (pid, len(chunks), total / 1048576))

    names = []
    for p in (FINAL_TSV, TRAINER_TSV):
        if os.path.exists(p):
            for r in csv.DictReader(io.open(p, encoding='utf-8'), delimiter='\t'):
                n = (r.get('name') or r.get('id') or '').strip()
                if n and n not in names:
                    names.append(n)
    print('[*] 待查模板名 %d 个' % len(names))

    def lookup(key):
        for base, data in chunks:
            idx = 0
            while True:
                i = data.find(key, idx)
                if i < 0:
                    break
                idx = i + 1
                # 必须是「完整键」：以 \0 结尾，且前面不能还是标识符字符
                #   （否则 CelestialAirfield 会命中 CelestialAirfieldUpgrade 里的一段）
                if i + len(key) >= len(data) or data[i + len(key)] != 0:
                    continue
                if i > 0 and data[i - 1] in b'0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_':
                    continue
                for skip in range(8, 26):
                    p = i + len(key) + skip
                    v = utf16_cjk_at(data, p)
                    if not v or not (2 <= len(v) <= 24):
                        continue
                    # 关键校验：实测条目里，值的字符数就写在它**前面 4 字节**的 u16 里
                    #   （如 "埃癸斯护盾发生器" 前是 08 00 0a 00）。对得上才算定位正确，
                    #   否则就是字节没对齐解出来的乱码（"刀馗""餀躟魰" 那类）。
                    if p < 4:
                        continue
                    L = data[p - 4] | (data[p - 3] << 8)
                    if L != len(v):
                        continue
                    return v
        return None

    found = {}
    for name in names:
        nb = name.encode('ascii', 'ignore')
        if not nb:
            continue
        for prefix in (b'NAME:', b''):     # ① 游戏的显示名键 ② 备用的裸键
            got = lookup(prefix + nb)
            if got:
                found[name] = got
                flag = '' if prefix else ' (裸键)'
                if flag:
                    found[name] = got
                break

    with io.open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(found, f, ensure_ascii=False, indent=1, sort_keys=True)
    print('[ok] 查到官方中文名 %d / %d，写入 %s' % (len(found), len(names), OUT))
    for k in list(found)[:20]:
        print('   %-38s %s' % (k, found[k]))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
