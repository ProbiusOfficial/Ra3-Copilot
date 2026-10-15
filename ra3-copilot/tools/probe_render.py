#!/usr/bin/env python3
"""probe_render.py - 渲染层专项诊断（需提权）

回答三个问题：
  1. d3d9 的 Present/EndScene 函数体是否被 inline hook（E9 / FF25 / 68..C3）
     -> 若被 hook，说明有别的插件（如日冕 EnhancerCorona）在接管渲染
  2. 我们改的那张 vtable 到底在哪、有几处引用
     -> 扫描内存找 HookEndScene/HookPresent 地址被引用的位置（vtable 槽）
  3. 进程里是否有 EnhancerCorona 等可疑 D3D 干预模块
"""
import ctypes, os, struct, sys
from ctypes import wintypes

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MAP_FILE = os.path.join(ROOT, 'ra3copilot.map')

TH32CS_SNAPPROCESS, TH32CS_SNAPMODULE, TH32CS_SNAPMODULE32 = 0x2, 0x8, 0x10
PROCESS_QUERY_INFORMATION, PROCESS_VM_READ = 0x0400, 0x0010
MEM_COMMIT, MEM_PRIVATE, MEM_IMAGE, MEM_MAPPED = 0x1000, 0x20000, 0x1000000, 0x40000
PAGE_NOACCESS, PAGE_GUARD = 0x01, 0x100


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


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wintypes.DWORD), ("_p1", wintypes.DWORD),
                ("RegionSize", ctypes.c_size_t), ("State", wintypes.DWORD),
                ("Protect", wintypes.DWORD), ("Type", wintypes.DWORD), ("_p2", wintypes.DWORD)]


k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
k32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
k32.OpenProcess.restype = wintypes.HANDLE
k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.LPVOID,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.VirtualQueryEx.restype = ctypes.c_size_t
k32.VirtualQueryEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]


def find_pid(name):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32(); pe.dwSize = ctypes.sizeof(pe)
    pid = 0
    if k32.Process32First(snap, ctypes.byref(pe)):
        while True:
            if pe.szExeFile.decode('ascii', 'ignore').lower() == name.lower():
                pid = pe.th32ProcessID; break
            if not k32.Process32Next(snap, ctypes.byref(pe)):
                break
    k32.CloseHandle(snap)
    return pid


def all_modules(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    if snap == wintypes.HANDLE(-1).value:
        return []
    me = MODULEENTRY32(); me.dwSize = ctypes.sizeof(me)
    out = []
    if k32.Module32First(snap, ctypes.byref(me)):
        while True:
            out.append((me.szModule.decode('ascii', 'ignore'), me.modBaseAddr or 0, me.modBaseSize))
            if not k32.Module32Next(snap, ctypes.byref(me)):
                break
    k32.CloseHandle(snap)
    return out


def parse_map(path):
    syms = {}
    if not os.path.exists(path):
        return syms
    pref = 0x10000000
    for line in open(path, errors='ignore'):
        if 'Preferred load address' in line:
            try: pref = int(line.strip().split()[-1], 16)
            except Exception: pass
            continue
        p = line.split()
        if len(p) < 3:
            continue
        nm = p[1]
        short = None
        if nm.startswith('_'):
            short = nm[1:].split('@')[0]
        elif nm.startswith('?'):
            short = nm[1:].split('@')[0]
        if short:
            try: syms[short] = int(p[2], 16) - pref
            except Exception: pass
    return syms


def main():
    lines = []

    def say(s):
        print(s); lines.append(s)

    pid = find_pid('ra3_1.12.game')
    if not pid:
        say('[x] game not running'); return 1
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        say(f'[x] OpenProcess denied err={ctypes.get_last_error()}'); return 1

    def rd(a, n):
        buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t(0)
        if not k32.ReadProcessMemory(h, ctypes.c_void_p(a), buf, n, ctypes.byref(got)) or got.value != n:
            return None
        return buf.raw[:n]

    def u32(a):
        d = rd(a, 4)
        return struct.unpack('<I', d)[0] if d else None

    mods = all_modules(pid)
    dll = next((m for m in mods if 'ra3copilot' in m[0].lower()), None)
    d3d = next((m for m in mods if m[0].lower() == 'd3d9.dll'), None)
    if not dll:
        say('[x] ra3copilot.dll not loaded'); return 2
    db = dll[1]
    say(f'[*] dll base=0x{db:08x}')
    if d3d:
        say(f'[*] d3d9.dll base=0x{d3d[1]:08x} size={d3d[2]} '
            f'range=0x{d3d[1]:08x}-0x{d3d[1]+d3d[2]:08x}')

    # ---- 可疑模块（可能在做 D3D 干预）----
    sus = [m for m in mods if any(k in m[0].lower()
           for k in ('enhancer', 'corona', 'd3d', 'dxgi', 'hook', 'reshade', 'overlay',
                     'dgvoodoo', 'wine', 'wrapper', 'render'))]
    say('[*] candidate graphics/hook modules:')
    for nm, b, s in sus:
        say(f'    {nm} base=0x{b:08x} size={s}')

    rvas = parse_map(MAP_FILE)
    say(f'[+] map symbols={len(rvas)}')

    def sym(name):
        return db + rvas[name] if name in rvas else None

    # ---- 1. 原函数是否被 inline hook ----
    for key, label in (('g_origPresent', 'IDirect3DDevice9::Present'),
                       ('g_origEndScene', 'IDirect3DDevice9::EndScene'),
                       ('g_origSwapPresent', 'IDirect3DSwapChain9::Present')):
        a = sym(key)
        if not a:
            say(f'[!] {key} not in map'); continue
        fn = u32(a)
        if not fn:
            say(f'[*] {label}: orig ptr NULL (hook target not captured)'); continue
        body = rd(fn, 16)
        where = ''
        if d3d:
            where = ' [in d3d9]' if d3d[1] <= fn < d3d[1] + d3d[2] else ' [OUTSIDE d3d9!]'
        say(f'[*] {label} orig=0x{fn:08x}{where} bytes={body.hex() if body else "FAIL"}')
        if body:
            if body[0] == 0xE9:
                tgt = fn + 5 + struct.unpack('<i', body[1:5])[0]
                owner = next((m[0] for m in mods if m[1] <= tgt < m[1] + m[2]), '?')
                say(f'    -> INLINE HOOKED (E9 -> 0x{tgt:08x} in {owner})')
            elif body[0] == 0x68 and body[5] == 0xC3:
                tgt = struct.unpack('<I', body[1:5])[0]
                owner = next((m[0] for m in mods if m[1] <= tgt < m[1] + m[2]), '?')
                say(f'    -> PUSH/RET HOOKED (-> 0x{tgt:08x} in {owner})')
            elif body[0] == 0xFF and body[1] == 0x25:
                say('    -> FF25 HOOKED')
            else:
                say('    -> looks clean (no inline hook)')

    # ---- 2. 扫描内存：谁引用了我们的 Hook*（vtable 槽定位）----
    hooks = {k: sym(k) for k in ('HookEndScene', 'HookPresent', 'HookSwapPresent', 'HookCreateDevice')}
    say('[*] hook function addresses:')
    for k, v in hooks.items():
        say(f'    {k} = 0x{v:08x}' if v else f'    {k} = ?')
    targets = {v: k for k, v in hooks.items() if v}
    found = {k: [] for k in targets.values()}

    mbi = MBI(); addr = 0
    scanned = 0
    while addr < 0x7FFF0000:
        if not k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        base = mbi.BaseAddress or 0
        size = int(mbi.RegionSize)
        if size <= 0:
            break
        if (mbi.State == MEM_COMMIT and mbi.Type in (MEM_PRIVATE, MEM_IMAGE, MEM_MAPPED)
                and mbi.Protect != PAGE_NOACCESS and not (mbi.Protect & PAGE_GUARD)
                and size < 64 * 1024 * 1024):
            chunk = min(size, 4 * 1024 * 1024)
            off = 0
            while off < size:
                n = min(chunk, size - off)
                data = rd(base + off, n)
                if data:
                    scanned += n
                    for i in range(0, len(data) - 4, 4):
                        v = struct.unpack_from('<I', data, i)[0]
                        if v in targets:
                            found[targets[v]].append(base + off + i)
                off += n
        addr = base + size
    say(f'[*] scanned {scanned // 1048576} MB')
    for k, addrs in found.items():
        say(f'[=] references to {k}: {len(addrs)}')
        for a in addrs[:6]:
            say(f'      槽 @0x{a:08x}')
            # 该槽若属于某 vtable，打印表头信息
            tbl = a
            # 反推表基址：试 a-42*4 / a-17*4 等常见索引
            for idx, nm in ((42, 'EndScene'), (17, 'Present'), (16, 'Reset'), (3, 'SwapPresent')):
                cand = a - idx * 4
                first = u32(cand)
                if first and d3d and d3d[1] <= first < d3d[1] + d3d[2]:
                    say(f'       -> likely vtable@0x{cand:08x} (idx {idx} {nm}), [0]=0x{first:08x}')
                    break

    k32.CloseHandle(h)
    try:
        open(os.path.join(HERE, 'probe_render_result.txt'), 'w', encoding='utf-8').write(
            '\n'.join(lines) + '\n')
    except Exception:
        pass
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except SystemExit:
        raise
    except BaseException:
        import traceback
        try:
            open(os.path.join(HERE, 'probe_render_result.txt'), 'w', encoding='utf-8').write(
                'FATAL:\n' + traceback.format_exc() + '\n')
        except Exception:
            pass
        raise
