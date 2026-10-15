#!/usr/bin/env python3
"""find_device.py - 扫游戏进程内存，定位真实的 IDirect3DDevice9 对象（需提权）

判定：在可读写内存里找 DWORD P 满足
  · P（vtable 指针）落在 d3d9.dll 镜像范围内
  · 且 P[16](Reset) / P[17](Present) / P[42](EndScene) 也都落在 d3d9.dll 范围内
→ 那么持有该 P 的地址 X 就是一个 device 对象（[X] == P 指向的表）
"""
import ctypes, struct, sys
from ctypes import wintypes

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
TH32CS_SNAPPROCESS, TH32CS_SNAPMODULE, TH32CS_SNAPMODULE32 = 0x2, 0x8, 0x10
PROCESS_QUERY_INFORMATION, PROCESS_VM_READ = 0x0400, 0x0010
MEM_COMMIT, MEM_PRIVATE, MEM_IMAGE, MEM_MAPPED = 0x1000, 0x20000, 0x1000000, 0x40000


class PE32(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("c", wintypes.DWORD), ("pid", wintypes.DWORD),
                ("h", ctypes.c_void_p), ("m", wintypes.DWORD), ("n", wintypes.DWORD),
                ("pp", wintypes.DWORD), ("pri", ctypes.c_long), ("f", wintypes.DWORD),
                ("exe", ctypes.c_char * 260)]


class ME32(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("mid", wintypes.DWORD), ("pid", wintypes.DWORD),
                ("g", wintypes.DWORD), ("p", wintypes.DWORD), ("base", ctypes.c_void_p),
                ("size", wintypes.DWORD), ("hmod", wintypes.HMODULE),
                ("name", ctypes.c_char * 256), ("path", ctypes.c_char * 260)]


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wintypes.DWORD), ("_p1", wintypes.DWORD),
                ("RegionSize", ctypes.c_size_t), ("State", wintypes.DWORD),
                ("Protect", wintypes.DWORD), ("Type", wintypes.DWORD), ("_p2", wintypes.DWORD)]


k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
k32.OpenProcess.restype = wintypes.HANDLE
k32.VirtualQueryEx.restype = ctypes.c_size_t
k32.VirtualQueryEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]


def find_pid(name):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PE32(); pe.dwSize = ctypes.sizeof(pe); pid = 0
    if k32.Process32First(snap, ctypes.byref(pe)):
        while True:
            if pe.exe.decode('ascii', 'ignore').lower() == name.lower():
                pid = pe.pid; break
            if not k32.Process32Next(snap, ctypes.byref(pe)):
                break
    k32.CloseHandle(snap); return pid


def find_module(pid, sub):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    if snap == wintypes.HANDLE(-1).value:
        return None
    me = ME32(); me.dwSize = ctypes.sizeof(me); out = []
    if k32.Module32First(snap, ctypes.byref(me)):
        while True:
            if sub.lower() in me.name.decode('ascii', 'ignore').lower():
                out.append((me.name.decode('ascii', 'ignore'), me.base or 0, me.size))
            if not k32.Module32Next(snap, ctypes.byref(me)):
                break
    k32.CloseHandle(snap); return out


def main():
    pid = find_pid('ra3_1.12.game')
    if not pid:
        print('[x] game not running'); return 1
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        print(f'[x] OpenProcess denied err={ctypes.get_last_error()} (need elevation)'); return 1
    d3d = find_module(pid, 'd3d9.dll')
    if not d3d:
        print('[x] d3d9.dll not loaded'); return 1
    db, dsz = d3d[0][1], d3d[0][2]
    print(f'[*] pid={pid} d3d9.dll base=0x{db:08x} size={dsz}')

    def rd(a, n):
        buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t(0)
        if not k32.ReadProcessMemory(h, ctypes.c_void_p(a), buf, n, ctypes.byref(got)) or got.value != n:
            return None
        return buf.raw[:n]

    def u32(a):
        d = rd(a, 4)
        return struct.unpack('<I', d)[0] if d else None

    in_d3d = lambda v: v is not None and db <= v < db + dsz
    cands = []
    mbi = MBI(); addr = 0
    scanned = 0
    while addr < 0x7FFF0000:
        if not k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        base = mbi.BaseAddress or 0; size = int(mbi.RegionSize)
        if size <= 0:
            break
        if (mbi.State == MEM_COMMIT and mbi.Type in (MEM_PRIVATE, MEM_IMAGE, MEM_MAPPED)
                and size < 256 * 1024 * 1024):
            chunk = min(size, 4 * 1024 * 1024); off = 0
            while off < size:
                n = min(chunk, size - off)
                data = rd(base + off, n)
                if data:
                    scanned += n
                    for i in range(0, len(data) - 8, 4):
                        v = struct.unpack_from('<I', data, i)[0]
                        if db <= v < db + dsz:
                            # 候选 vtable 指针：验证 P[16]/P[17]/P[42]
                            if in_d3d(u32(v + 16 * 4)) and in_d3d(u32(v + 17 * 4)) and in_d3d(u32(v + 42 * 4)):
                                cands.append((base + off + i, v))
                off += n
        addr = base + size
    print(f'[*] scanned {scanned // 1048576} MB, candidates={len(cands)}')
    seen = set()
    for obj, vt in cands[:20]:
        if vt in seen:
            continue
        seen.add(vt)
        p17 = u32(vt + 17 * 4); p42 = u32(vt + 42 * 4)
        print(f'[+] device obj=0x{obj:08x} vtable=0x{vt:08x} Present=0x{p17:08x} EndScene=0x{p42:08x}')
    if cands:
        print(f'[*] distinct vtables: {len(seen)}')
    k32.CloseHandle(h)
    return 0


if __name__ == '__main__':
    import io, os
    _f = open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           'find_device_result.txt'),
              'w', encoding='utf-8', buffering=1)
    sys.stdout = _f
    sys.stderr = _f
    try:
        rc = main()
    except BaseException:
        import traceback
        traceback.print_exc()
        rc = 9
    finally:
        _f.flush()
    raise SystemExit(rc)
