"""Scan the RA3 process for ASCII strings (script flag/timer/script names).

Read-only. Chunked ReadProcessMemory over committed readable regions, with a
size cap so a full 2GB walk stays fast.
"""
import ctypes
import ctypes.wintypes as wt
import re
import sys

k32 = ctypes.WinDLL('kernel32', use_last_error=True)


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_void_p), ('AllocationBase', ctypes.c_void_p),
                ('AllocationProtect', wt.DWORD), ('RegionSize', ctypes.c_size_t),
                ('State', wt.DWORD), ('Protect', wt.DWORD), ('Type', wt.DWORD)]


MEM_COMMIT = 0x1000
READABLE = (0x02, 0x04, 0x20, 0x40, 0x80)  # RO, RW, EXEC_R, EXEC_RW, EXEC_WC


def scan(pid, needles, max_region=64 << 20):
    h = k32.OpenProcess(0x10 | 0x400, False, pid)  # VM_READ | QUERY_INFORMATION
    if not h:
        raise SystemExit(f'OpenProcess failed err={ctypes.get_last_error()}')
    hits = {n: [] for n in needles}
    addr = 0
    total = 0
    while True:
        mbi = MBI()
        if not k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        size = mbi.RegionSize
        if size == 0:
            break
        base = mbi.BaseAddress or 0
        if mbi.State == MEM_COMMIT and mbi.Protect in READABLE and size <= max_region:
            buf = ctypes.create_string_buffer(size)
            got = ctypes.c_size_t(0)
            if k32.ReadProcessMemory(h, ctypes.c_void_p(base), buf, size, ctypes.byref(got)):
                data = buf.raw[:got.value]
                total += len(data)
                for n in needles:
                    start = 0
                    while True:
                        i = data.find(n, start)
                        if i < 0:
                            break
                        hits[n].append(base + i)
                        start = i + 1
                        if len(hits[n]) > 60:
                            break
        addr = base + size
        if addr >= 0x80000000:
            break
    k32.CloseHandle(h)
    return hits, total


def main():
    pid = int(sys.argv[1])
    needles = [s.encode() for s in sys.argv[2:]]
    hits, total = scan(pid, needles)
    print(f'[i] scanned {total / 1048576:.0f} MB')
    for n, addrs in hits.items():
        print(f'=== {n.decode()}  ({len(addrs)} hits)')
        for a in addrs[:12]:
            print(f'    {a:#010x}')


if __name__ == '__main__':
    main()
