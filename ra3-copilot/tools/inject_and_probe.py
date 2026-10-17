#!/usr/bin/env python3
"""inject_and_probe.py - 一次提权完成：注入 ra3copilot.dll + 验证 + 读全部遥测

用法（提权）: python inject_and_probe.py [dll_path]
默认注入与本脚本同级目录的 ra3copilot.dll。
结果落盘 inject_probe_result.txt（提权场景 stdout 会丢）
"""
import ctypes, os, struct, sys, time
from ctypes import wintypes

k32 = ctypes.WinDLL('kernel32', use_last_error=True)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DLL_PATH = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'ra3copilot.dll')
MAP_FILE = os.path.join(ROOT, 'ra3copilot.map')

TH32CS_SNAPPROCESS = 0x2
TH32CS_SNAPMODULE = 0x8
TH32CS_SNAPMODULE32 = 0x10
PROCESS_ALL = 0x1F0FFF

RVA_MONEY = 0x6CFDFE
RVA_PLAYERID = 0x0FF95B
WANT_SYMS = [
    'g_localPlayerId', 'g_localPlayerObj', 'g_moneyOp', 'g_moneyValue',
    'g_retMoney', 'g_retPlayerId', 'g_telemEnter', 'g_telemApplied',
    'g_telemEndScene', 'g_telemPresent', 'g_telemWndHooked',
    'g_telemSwapPresent', 'g_telemCreateDevice', 'g_telemCreateDeviceEx',
    'g_telemDummyDevVtbl', 'g_telemDummyExDevVtbl',
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
k32.VirtualAllocEx.restype = ctypes.c_void_p
k32.VirtualAllocEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t,
                               wintypes.DWORD, wintypes.DWORD]
k32.WriteProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.LPCVOID,
                                   ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.LPVOID,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.CreateRemoteThread.restype = wintypes.HANDLE
k32.CreateRemoteThread.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t,
                                   ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD,
                                   ctypes.POINTER(wintypes.DWORD)]
k32.GetProcAddress.restype = ctypes.c_void_p
k32.GetProcAddress.argtypes = [wintypes.HMODULE, wintypes.LPCSTR]
k32.GetModuleHandleA.restype = wintypes.HMODULE
k32.GetModuleHandleA.argtypes = [wintypes.LPCSTR]


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


user32 = ctypes.WinDLL('user32', use_last_error=True)
WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user32.EnumWindows.argtypes = [WNDENUMPROC, wintypes.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]


def enum_windows_for_pid(pid):
    """列出该进程的所有顶级窗口：(hwnd, class, title, visible, owner)"""
    out = []

    def cb(hwnd, lp):
        p = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid:
            cls = ctypes.create_unicode_buffer(256)
            user32.GetClassNameW(hwnd, cls, 256)
            ttl = ctypes.create_unicode_buffer(256)
            user32.GetWindowTextW(hwnd, ttl, 256)
            vis = bool(user32.IsWindowVisible(hwnd))
            own = user32.GetWindow(hwnd, 4)   # GW_OWNER
            out.append((hwnd, cls.value, ttl.value, vis, own))
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return out


def load_map_rvas(path):
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
        if len(p) < 3:
            continue
        name = p[1]
        norm = None
        if name.startswith('_g_'):
            norm = name[1:]
        elif name.startswith('?g_'):
            # C++ 修饰名示例: ?g_telemEndScene@ra3@@3IA -> 取第一个 @ 之前
            norm = name[1:].split('@')[0]
        if not norm:
            continue
        try:
            syms[norm] = int(p[2], 16) - pref
        except Exception:
            pass
    return syms


def do_inject(h, path_w, say):
    """CreateRemoteThread(LoadLibraryW) 注入；返回 hmod 或 0"""
    b = path_w.encode('utf-16-le') + b'\x00\x00'
    remote = k32.VirtualAllocEx(h, None, len(b), 0x1000 | 0x2000, 0x04)
    if not remote:
        say(f'[x] VirtualAllocEx failed err={ctypes.get_last_error()}')
        return 0
    written = ctypes.c_size_t(0)
    if not k32.WriteProcessMemory(h, remote, b, len(b), ctypes.byref(written)):
        say(f'[x] WriteProcessMemory failed err={ctypes.get_last_error()}')
        return 0
    pfn = k32.GetProcAddress(k32.GetModuleHandleA(b'kernel32.dll'), b'LoadLibraryW')
    t = k32.CreateRemoteThread(h, None, 0, pfn, remote, 0, None)
    if not t:
        say(f'[x] CreateRemoteThread failed err={ctypes.get_last_error()}')
        k32.VirtualFreeEx(h, remote, 0, 0x8000)
        return 0
    k32.WaitForSingleObject(t, 20000)
    code = wintypes.DWORD(0)
    k32.GetExitCodeThread(t, ctypes.byref(code))
    k32.CloseHandle(t)
    k32.VirtualFreeEx(h, remote, 0, 0x8000)
    return code.value


def main():
    lines = []

    def say(s):
        print(s); lines.append(s)
        try:   # 立即落盘，任何中断都保留现场
            open(os.path.join(HERE, 'inject_probe_result.txt'), 'w', encoding='utf-8').write(
                '\n'.join(lines) + '\n')
        except Exception:
            pass

    pid = find_pid('ra3_1.12.game')
    if not pid:
        say('[x] ra3_1.12.game not running'); return 1
    say(f'[*] target pid={pid}')

    h = k32.OpenProcess(PROCESS_ALL, False, pid)
    if not h:
        say(f'[x] OpenProcess denied err={ctypes.get_last_error()} (need elevation)'); return 1

    # 目标进程映像路径（确认锁定的是正确进程）
    k32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                               wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    buf = ctypes.create_unicode_buffer(512)
    sz = wintypes.DWORD(512)
    if k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(sz)):
        say(f'[*] exe path = {buf.value}')

    # ---- 1. 注入（若未加载）；失败自动重试 ----
    no_inject = '--no-inject' in sys.argv
    already = find_module(pid, 'ra3copilot')
    if already:
        say(f'[i] already loaded: {already[0][0]} base=0x{already[0][1]:08x} '
            f'size={already[0][2]}')
    elif no_inject:
        say('[!] --no-inject given and DLL not loaded; run injector.exe (x86) first')
        k32.CloseHandle(h)
        return 1
    else:
        say(f'[*] dll = {DLL_PATH}')
        say(f'[*] dll exists={os.path.exists(DLL_PATH)} '
            f'size={os.path.getsize(DLL_PATH) if os.path.exists(DLL_PATH) else 0}')
        hmod = 0
        for attempt in range(1, 4):
            hmod = do_inject(h, DLL_PATH, say)
            say(f'[*] inject attempt {attempt}: LoadLibraryW -> '
                f'{("HMODULE 0x%08x" % hmod) if hmod else "0 (failed)"}')
            if hmod:
                break
            time.sleep(1.5)
        if not hmod:
            mods2 = find_module(pid, 'ra3copilot')
            say(f'[!] injection FAILED after 3 tries; partial load={bool(mods2)}')
            k32.CloseHandle(h)
            return 1
        say(f'[+] injected OK, HMODULE=0x{hmod:08x}')

    time.sleep(2.5)   # 等 DLL 工作线程完成初始化

    def rd(a, n):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t(0)
        if not k32.ReadProcessMemory(h, ctypes.c_void_p(a), buf, n, ctypes.byref(got)) or got.value != n:
            return None
        return buf.raw[:n]

    def u32(a):
        d = rd(a, 4)
        return struct.unpack('<I', d)[0] if d else None

    def i32(a):
        d = rd(a, 4)
        return struct.unpack('<i', d)[0] if d else None

    exe = find_module(pid, 'ra3_1.12.game') or find_module(pid, 'ra3_1')
    base = exe[0][1] if exe else 0
    say(f'[*] main base=0x{base:08x}')

    # ---- 2. hook 补丁字节 ----
    sm = base + RVA_MONEY
    bb = rd(sm, 5)
    say(f'[*] money site   0x{sm:08x}: {bb.hex() if bb else "FAIL"}'
        + ('  E9-OK' if bb and bb[0] == 0xE9 else '  NOT-PATCHED'))
    sp = base + RVA_PLAYERID
    b2 = rd(sp, 6)
    say(f'[*] playerid site 0x{sp:08x}: {b2.hex() if b2 else "FAIL"}'
        + ('  pushret-OK' if b2 and b2[0] == 0x68 and b2[5] == 0xC3 else '  NOT-PATCHED'))

    # ---- 2.5 窗口 / d3d9 模块（定位渲染层问题）----
    d3d9 = find_module(pid, 'd3d9')
    say(f'[*] d3d9.dll loaded={bool(d3d9)}'
        + (f' base=0x{d3d9[0][1]:08x}' if d3d9 else ''))
    wins = enum_windows_for_pid(pid)
    say(f'[*] game top-level windows={len(wins)}')
    for hwnd, cls, ttl, vis, own in wins:
        say(f'    hwnd=0x{hwnd:08x} vis={int(vis)} owner=0x{own or 0:08x} '
            f'class="{cls}" title="{ttl}"')

    # ---- 3. 符号 + 遥测 ----
    mods = find_module(pid, 'ra3copilot')
    if not mods:
        say('[x] ra3copilot.dll NOT loaded'); return 2
    db = mods[0][1]
    say(f'[*] dll base=0x{db:08x} size={mods[0][2]}')
    rvas = load_map_rvas(MAP_FILE)
    say(f'[+] map symbols={len(rvas)}')
    if rvas:
        for name in WANT_SYMS:
            if name not in rvas:
                continue
            a = db + rvas[name]
            v = i32(a)
            say(f'    {name:18} = {v} (0x{v & 0xFFFFFFFF:08x})' if v is not None
                else f'    {name:18} = READ-FAIL')
        lp = u32(db + rvas['g_localPlayerObj']) if 'g_localPlayerObj' in rvas else 0
        if lp and 0x10000 < lp < 0x7FFF0000:
            say(f'[+] LOCAL PLAYER obj=0x{lp:08x} id={i32(lp + 0xC)} money={i32(lp + 4)}')
        else:
            say('[!] g_localPlayerObj not captured yet')

        def gv(n):
            return i32(db + rvas[n]) if n in rvas else 0
        es, pr = gv('g_telemEndScene'), gv('g_telemPresent')
        sp, cd = gv('g_telemSwapPresent'), gv('g_telemCreateDevice')
        cdex = gv('g_telemCreateDeviceEx')
        ro = gv('g_telemRenderOnce')
        d3d, wnd, img = gv('g_telemD3dHooked'), gv('g_telemWndHooked'), gv('g_telemImguiReady')
        say(f'[=] d3dHooked={d3d} wndHooked={wnd} imguiReady={img}')
        say(f'[=] ES={es} Present={pr} SwapPresent={sp} CreateDevice={cd} CreateDeviceEx={cdex} '
            f'renderFrames={ro}')
        if not d3d:
            say('[!] HookD3D9 FAILED (dummy device creation refused?) -> no overlay UI')
        elif es == 0 and pr == 0 and sp == 0 and cdex == 0:
            say('[!] hooked vtables never called -> try Alt+Enter (rebuild device); '
                'if still 0, run probe_render.py to check for a D3D wrapper')
        elif not img:
            say('[!] render hooks fire but ImGui never init -> gameWnd problem?')
        else:
            say('[+] overlay pipeline ALIVE')

    k32.CloseHandle(h)
    try:
        open(os.path.join(HERE, 'inject_probe_result.txt'), 'w', encoding='utf-8').write(
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
            open(os.path.join(HERE, 'inject_probe_result.txt'), 'w', encoding='utf-8').write(
                'FATAL:\n' + traceback.format_exc() + '\n')
        except Exception:
            pass
        raise
