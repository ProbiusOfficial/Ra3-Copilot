#!/usr/bin/env python3
"""probe_state.py - 诊断 ra3copilot.dll 在游戏进程里的运行状态（需提权）

优先用构建产出的 ra3copilot.map 符号表定位全局量：
  dll_base(进程模块) + map里的 RVA  ->  读值
map 缺失时退回"机器码特征提取"（从 cave->stub 的指令里抠绝对地址）。

诊断项：
  1. 两处钩子补丁字节（0x6CFDFE E9 / 0xFF95B 68..C3）
  2. 全部全局量：本地玩家 ID/对象、pending 操作、遥测计数
  3. 结论提示（钩子是否生效、D3D 渲染路径是否活着）
结果打印并落盘 probe_state_result.txt（提权场景 stdout 会丢）
"""
import ctypes, os, struct, sys
from ctypes import wintypes

k32 = ctypes.WinDLL('kernel32', use_last_error=True)

TH32CS_SNAPPROCESS = 0x2
TH32CS_SNAPMODULE = 0x8
TH32CS_SNAPMODULE32 = 0x10
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010

RVA_MONEY = 0x6CFDFE
RVA_PLAYERID = 0x0FF95B
HERE = os.path.dirname(os.path.abspath(__file__))
MAP_FILE = os.path.join(os.path.dirname(HERE), 'ra3copilot.map')

WANT_SYMS = [
    'g_localPlayerId', 'g_localPlayerObj', 'g_moneyOp', 'g_moneyValue',
    'g_retMoney', 'g_retPlayerId', 'g_telemEnter', 'g_telemApplied',
    'g_telemEndScene', 'g_telemPresent', 'g_telemWndHooked',
    'g_telemD3dHooked', 'g_telemImguiReady', 'g_telemWndFound',
    'g_telemRenderOnce',
]


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


k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
k32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
k32.OpenProcess.restype = wintypes.HANDLE
k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.LPVOID,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]


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


def find_module(pid, sub):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    if snap == wintypes.HANDLE(-1).value:
        return []
    me = MODULEENTRY32(); me.dwSize = ctypes.sizeof(me)
    out = []
    if k32.Module32First(snap, ctypes.byref(me)):
        while True:
            nm = me.szModule.decode('ascii', 'ignore')
            if sub.lower() in nm.lower():
                out.append((nm, me.modBaseAddr or 0, me.modBaseSize))
            if not k32.Module32Next(snap, ctypes.byref(me)):
                break
    k32.CloseHandle(snap)
    return out


def load_map_rvas(path):
    """解析 MSVC .map，返回 {符号名: RVA}"""
    syms = {}
    if not os.path.exists(path):
        return syms
    pref = 0x10000000
    for line in open(path, 'r', errors='ignore'):
        if 'Preferred load address' in line:
            try:
                pref = int(line.strip().split()[-1], 16)
            except Exception:
                pass
            continue
        p = line.split()
        if len(p) >= 3 and p[1].startswith('_g_'):
            try:
                syms[p[1][1:]] = int(p[2], 16) - pref   # 去掉 C 符号前缀下划线
            except Exception:
                pass
        elif len(p) >= 3 and p[1].startswith('?g_'):
            try:
                syms[p[1][1:].split('@')[0]] = int(p[2], 16) - pref   # C++ 修饰名
            except Exception:
                pass
    return syms


def main():
    lines = []

    def say(s):
        print(s); lines.append(s)

    pid = find_pid('ra3_1.12.game')
    if not pid:
        say('[x] ra3_1.12.game not running'); return 1

    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        say(f'[x] OpenProcess denied err={ctypes.get_last_error()} (need elevation)'); return 1

    def rd(addr, n):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t(0)
        if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)) or got.value != n:
            return None
        return buf.raw[:n]

    def u32(a):
        d = rd(a, 4)
        return struct.unpack('<I', d)[0] if d else None

    def i32(a):
        d = rd(a, 4)
        return struct.unpack('<i', d)[0] if d else None

    exe = find_module(pid, 'ra3_1.12.game') or find_module(pid, 'ra3_1')
    if not exe:
        say('[x] cannot resolve main module'); return 1
    base = exe[0][1]
    say(f'[*] pid={pid} main module base=0x{base:08x} size={exe[0][2]}')

    # ---- 1. 钩子补丁字节 ----
    sm = base + RVA_MONEY
    b = rd(sm, 5)
    say(f'[*] money site 0x{sm:08x}: {b.hex() if b else "READ-FAIL"}'
        + ('  -> E9 detour OK' if b and b[0] == 0xE9 else '  -> NOT patched'))
    sp = base + RVA_PLAYERID
    b2 = rd(sp, 6)
    say(f'[*] playerid site 0x{sp:08x}: {b2.hex() if b2 else "READ-FAIL"}'
        + ('  -> push/ret OK' if b2 and b2[0] == 0x68 and b2[5] == 0xC3 else '  -> NOT patched'))

    # ---- 2. dll 模块 + 符号 ----
    mods = find_module(pid, 'ra3copilot')
    if not mods:
        say('[x] ra3copilot.dll not loaded'); k32.CloseHandle(h); return 2
    db, ds = mods[0][1], mods[0][2]
    say(f'[*] ra3copilot.dll base=0x{db:08x} size={ds}')

    rvas = load_map_rvas(MAP_FILE)
    if rvas:
        say(f'[+] map symbols: {len(rvas)} (from ra3copilot.map)')
        for name in WANT_SYMS:
            if name not in rvas:
                continue
            addr = db + rvas[name]
            val = i32(addr)
            if val is None:
                say(f'[*] {name:18} @0x{addr:08x} = READ-FAIL')
            elif name in ('g_localPlayerObj', 'g_retMoney', 'g_retPlayerId', 'g_localPlayerId'):
                say(f'[*] {name:18} @0x{addr:08x} = 0x{val & 0xFFFFFFFF:08x} ({val})')
            else:
                say(f'[*] {name:18} @0x{addr:08x} = {val}')
        # 派生结论
        lp = u32(db + rvas['g_localPlayerObj']) if 'g_localPlayerObj' in rvas else 0
        if lp:
            say(f'[+] local player obj = 0x{lp:08x}  id={i32(lp + 0xC)}  money={i32(lp + 4)}')
        te = i32(db + rvas['g_telemEnter']) if 'g_telemEnter' in rvas else 0
        ta = i32(db + rvas['g_telemApplied']) if 'g_telemApplied' in rvas else 0
        es = i32(db + rvas['g_telemEndScene']) if 'g_telemEndScene' in rvas else 0
        pr = i32(db + rvas['g_telemPresent']) if 'g_telemPresent' in rvas else 0
        ro = i32(db + rvas['g_telemRenderOnce']) if 'g_telemRenderOnce' in rvas else 0
        say(f'[*] money cave entered={te} applied={ta}')
        say(f'[*] EndScene={es} Present={pr} renderFrames={ro}')
        if es == 0 and pr == 0:
            say('[!] neither EndScene nor Present hook fired -> d3d vtable hook ineffective')
        elif te == 0:
            say('[!] money hook never executed -> game path not hit yet (menu/pause?)')
    else:
        say('[!] ra3copilot.map not found - fallback to machine-code extraction')
        if b and b[0] == 0xE9:
            cave = sm + 5 + struct.unpack('<i', b[1:5])[0]
            cb = rd(cave, 6)
            if cb and cb[0] == 0x68:
                stub = struct.unpack('<I', cb[1:5])[0]
                say(f'[+] stub @0x{stub:08x}')
                for off, nm in ((4, 'g_localPlayerId'), (0x12, 'g_moneyOp'), (0x20, 'g_moneyValue')):
                    a = u32(stub + off)
                    if a:
                        say(f'[*] {nm:16} @0x{a:08x} = {i32(a)}')

    k32.CloseHandle(h)
    try:
        out = os.path.join(HERE, 'probe_state_result.txt')
        open(out, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    except Exception:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
