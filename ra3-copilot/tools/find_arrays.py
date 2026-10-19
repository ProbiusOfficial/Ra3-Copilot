"""在目标进程里找「连续 N 个指针、且各自指向的对象 vtable 相同」的数组。

用途：定位玩家的「已购协议」列表（15 个同类 SpecialPower 对象）。
只读，不写目标进程。
"""
import ctypes
import struct
import sys
from ctypes import wintypes as wt

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.OpenProcess.restype = ctypes.c_void_p
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.ReadProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.VirtualQueryEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                               ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t

MEM_COMMIT = 0x1000
PAGE_GUARD = 0x100
PAGE_NOACCESS = 0x01
RDATA_LO, RDATA_HI = 0x7C7000, 0x8AB000   # 本版游戏 .rdata（vtable 一般在这）
HEAP_LO, HEAP_HI = 0x02000000, 0x14000000


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_void_p), ('AllocationBase', ctypes.c_void_p),
                ('AllocationProtect', wt.DWORD), ('RegionSize', ctypes.c_size_t),
                ('State', wt.DWORD), ('Protect', wt.DWORD), ('Type', wt.DWORD)]


class Mem:
    def __init__(self, pid):
        self.h = k32.OpenProcess(0x0410, False, pid)
        if not self.h:
            raise OSError('OpenProcess failed %d' % ctypes.get_last_error())
        self.buf = ctypes.create_string_buffer(4 << 20)

    def read(self, addr, n):
        got = ctypes.c_size_t(0)
        ok = k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), self.buf, n,
                                   ctypes.byref(got))
        if not ok:
            return b''
        return self.buf.raw[:got.value]

    def u32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack('<I', b)[0] if len(b) == 4 else 0


def regions(mem):
    a = 0x10000
    mbi = MBI()
    while a < 0x7FFF0000:
        if not k32.VirtualQueryEx(mem.h, ctypes.c_void_p(a), ctypes.byref(mbi),
                                 ctypes.sizeof(mbi)):
            break
        base = int(mbi.BaseAddress or 0)
        size = int(mbi.RegionSize)
        prot = mbi.Protect
        if (mbi.State == MEM_COMMIT and prot != PAGE_NOACCESS
                and not (prot & PAGE_GUARD) and size):
            yield base, size
        nxt = base + size
        if nxt <= a:
            break
        a = nxt


def scan(mem, minrun=8, maxobj_read=4096):
    hits = []
    CH = 4 << 20
    for base, size in regions(mem):
        if size > 512 << 20:          # 跳过巨大区域（资源/映射）
            continue
        off = 0
        while off < size:
            n = min(CH, size - off)
            buf = mem.read(base + off, n)
            if len(buf) < 16:
                off += CH
                continue
            run = []
            for i in range(0, len(buf) - 3, 4):
                v = struct.unpack_from('<I', buf, i)[0]
                if HEAP_LO <= v < HEAP_HI:
                    run.append(v)
                else:
                    if len(run) >= minrun:
                        hits.append((base + off + i - len(run) * 4, list(run)))
                    run = []
            if len(run) >= minrun:
                hits.append((base + off + len(buf) - len(buf) % 4 - len(run) * 4, list(run)))
            off += CH
    # 过滤：同类 vtable
    out = []
    for addr, ptrs in hits:
        vt = mem.u32(ptrs[0])
        if not (RDATA_LO <= vt < RDATA_HI):
            continue
        same = 0
        for p in ptrs[:64]:
            if mem.u32(p) == vt:
                same += 1
            else:
                break
        if same >= minrun:
            out.append((addr, same, vt, ptrs[:same]))
    return out


def main():
    pid = int(open('_pid.txt').read().strip()) if len(sys.argv) < 2 else int(sys.argv[1])
    mem = Mem(pid)
    print('pid=%d 已连接' % pid)
    res = scan(mem)
    print('候选数组 %d 个' % len(res))
    res.sort(key=lambda r: -r[1])
    for addr, n, vt, ptrs in res[:40]:
        print('  @%08X  长度=%-4d vtable=%08X  %s' %
              (addr, n, vt, [hex(x) for x in ptrs[:6]]))
    with open('_cand_arrays.txt', 'w', encoding='utf-8') as f:
        for addr, n, vt, ptrs in res:
            f.write('%08X\t%d\t%08X\t%s\n' %
                    (addr, n, vt, ','.join('%08X' % x for x in ptrs)))


if __name__ == '__main__':
    main()
