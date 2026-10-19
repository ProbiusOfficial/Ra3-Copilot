"""修改器注入/卸载（ctypes 实现）。

!! 只能用于「目标进程位数 == Python 位数」的情况 !!
   本机 Python 是 64 位、游戏是 32 位 -> **不要用它注入游戏**，会跳飞把游戏打崩。
   32 位目标请用 injector.exe（Hostx86 编译）。脚本已内置位数闸门，跨位数直接拒绝。

抗 ASLR：先枚举目标模块取远端 kernel32 基址，再按偏移算 LoadLibraryW。

C++ 注入器在部分状态下 VirtualAllocEx 会失败，且错误信息被控制台编码截断；
这里直接用 EnumProcessModules 取目标进程里的 kernel32 基址，再按本进程
LoadLibraryW 的偏移算出远端地址，避免两进程基址不同导致的 0xC0000005。

用法：
    python inject.py                 # 注入
    python inject.py --unload        # 卸载
    python inject.py --pid 1234      # 指定进程
"""
import ctypes
import os
import sys
import time
from ctypes import wintypes as wt

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

# 关键：这些 API 的默认 restype 是有符号 int，而模块基址往往 > 0x80000000。
# 一旦和 unsigned 的远端基址混算，偏移就错到天上去（会把游戏打崩）。
k32.GetModuleHandleW.restype = ctypes.c_void_p
k32.GetModuleHandleW.argtypes = [ctypes.c_wchar_p]
k32.GetProcAddress.restype = ctypes.c_void_p
k32.GetProcAddress.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
k32.OpenProcess.restype = ctypes.c_void_p
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.CreateRemoteThread.restype = ctypes.c_void_p
k32.CreateRemoteThread.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                   ctypes.c_void_p, ctypes.c_void_p, wt.DWORD,
                                   ctypes.POINTER(wt.DWORD)]
k32.VirtualAllocEx.restype = ctypes.c_void_p
k32.VirtualAllocEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                               wt.DWORD, wt.DWORD]
k32.WriteProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                   ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.VirtualFreeEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD]
k32.WaitForSingleObject.argtypes = [ctypes.c_void_p, wt.DWORD]
k32.ReadProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]

PROCESS_ALL = 0x1F0FFF
TH32CS_SNAPPROCESS = 0x2
TH32CS_SNAPMODULE = 0x8
TH32CS_SNAPMODULE32 = 0x10
LIST_MODULES_ALL = 0x03
DLL_NAME = "ra3copilot.dll"
GAME = "ra3_1.12.game"


class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD),
                ("th32ProcessID", wt.DWORD), ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
                ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long),
                ("dwFlags", wt.DWORD), ("szExeFile", ctypes.c_char * 260)]


class MODULEENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD),
                ("th32ProcessID", wt.DWORD), ("GlblcntUsage", wt.DWORD),
                ("ProccntUsage", wt.DWORD), ("modBaseAddr", ctypes.POINTER(ctypes.c_byte)),
                ("modBaseSize", wt.DWORD), ("hModule", wt.HMODULE),
                ("szModule", ctypes.c_char * 256), ("szExePath", ctypes.c_char * 260)]


def is_wow64(handle):
    """目标是否为 32 位进程（在 WOW64 下运行）。"""
    val = wt.BOOL(0)
    if not psapi.IsWow64Process(handle, ctypes.byref(val)):
        return None
    return bool(val.value)


def check_bitness(handle):
    """返回 (ok, 说明)。跨位数注入必然跳飞，必须先挡掉。"""
    host64 = ctypes.sizeof(ctypes.c_void_p) == 8
    wow = is_wow64(handle)
    if wow is None:
        return False, "无法查询目标位数"
    target64 = not wow if host64 else False
    if host64 and wow:
        return False, "本脚本是 64 位、目标是 32 位：跨位数注入会打崩目标，请用 injector.exe"
    return True, "位数匹配"


def find_pid(name=GAME):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == wt.HANDLE(-1).value:
        return 0
    pe = PROCESSENTRY32()
    pe.dwSize = ctypes.sizeof(pe)
    pid = 0
    if k32.Process32First(snap, ctypes.byref(pe)):
        while True:
            if pe.szExeFile.decode("latin1").lower() == name.lower():
                pid = pe.th32ProcessID
                break
            if not k32.Process32Next(snap, ctypes.byref(pe)):
                break
    k32.CloseHandle(snap)
    return pid


def modules(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    out = {}
    if snap == wt.HANDLE(-1).value:
        return out
    me = MODULEENTRY32()
    me.dwSize = ctypes.sizeof(me)
    if k32.Module32First(snap, ctypes.byref(me)):
        while True:
            base = ctypes.cast(me.modBaseAddr, ctypes.c_void_p).value or 0
            out[me.szModule.decode("latin1").lower()] = (base, me.modBaseSize)
            if not k32.Module32Next(snap, ctypes.byref(me)):
                break
    k32.CloseHandle(snap)
    return out


def remote_call(h, pid, fn_local, arg_bytes):
    """在目标进程里调用 fn_local（自动按 kernel32 基址偏移换算远端地址）。"""
    buf = k32.VirtualAllocEx(h, None, len(arg_bytes), 0x3000, 0x04)
    if not buf:
        raise OSError("VirtualAllocEx", ctypes.get_last_error())
    written = ctypes.c_size_t(0)
    if not k32.WriteProcessMemory(h, buf, arg_bytes, len(arg_bytes), ctypes.byref(written)):
        raise OSError("WriteProcessMemory", ctypes.get_last_error())

    local_k32 = int(k32.GetModuleHandleW("kernel32.dll") or 0)
    mods = modules(pid)
    if "kernel32.dll" not in mods:
        raise OSError("远端找不到 kernel32")
    remote_k32 = int(mods["kernel32.dll"][0])
    if not local_k32 or not remote_k32:
        raise OSError("基址无效 local=%08X remote=%08X" % (local_k32, remote_k32))
    offset = (int(fn_local) - local_k32) & 0xFFFFFFFF
    target = (remote_k32 + offset) & 0xFFFFFFFF
    # 安全性：目标必须落在远端 kernel32 的映像范围内，否则说明基址算错了
    base, size = mods["kernel32.dll"][0], mods["kernel32.dll"][1]
    if not (base <= target < base + max(size, 0x100000)):
        raise OSError("目标地址 %08X 不在远端 kernel32 [%08X,+%X) 内，已中止"
                      % (target, base, size))

    # 保险：比对目标地址处的机器码与本进程同一函数的字节
    def head_bytes(handle, addr, n=16):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t(0)
        if not k32.ReadProcessMemory(handle, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
            return None
        return buf.raw[:got.value]

    local_head = head_bytes(k32.GetCurrentProcess(), int(fn_local))
    remote_head = head_bytes(h, target)
    if local_head is None or remote_head is None or local_head[:16] != remote_head[:16]:
        raise OSError("目标地址机器码与本进程不一致，已中止（local=%s remote=%s）"
                      % (local_head.hex() if local_head else "?",
                         remote_head.hex() if remote_head else "?"))

    tid = wt.DWORD(0)
    th = k32.CreateRemoteThread(h, None, 0, ctypes.c_void_p(target), ctypes.c_void_p(buf), 0, ctypes.byref(tid))
    if not th:
        raise OSError("CreateRemoteThread", ctypes.get_last_error())
    k32.WaitForSingleObject(th, 10000)
    code = wt.DWORD(0)
    k32.GetExitCodeThread(th, ctypes.byref(code))
    k32.CloseHandle(th)
    k32.VirtualFreeEx(h, buf, 0, 0x8000)
    return code.value


def check_only(h, pid, fn_local):
    local_k32 = int(k32.GetModuleHandleW("kernel32.dll") or 0)
    mods = modules(pid)
    if "kernel32.dll" not in mods:
        return "远端无 kernel32"
    base, size = mods["kernel32.dll"][0], mods["kernel32.dll"][1]
    offset = (int(fn_local) - local_k32) & 0xFFFFFFFF
    target = (base + offset) & 0xFFFFFFFF
    ok = base <= target < base + max(size, 0x100000)
    return "local_k32=%08X remote_k32=%08X offset=%X target=%08X 在范围内=%s" % (
        local_k32, base, offset, target, ok)


def dll_override(argv):
    if "--dll" in argv:
        return argv[argv.index("--dll") + 1]
    return None


def main(argv):
    unload = "--unload" in argv
    pid = 0
    if "--pid" in argv:
        pid = int(argv[argv.index("--pid") + 1])
    if not pid:
        pid = find_pid()
    if not pid:
        print("游戏未运行" if unload else "未找到游戏进程")
        return 0 if unload else 1

    h = k32.OpenProcess(PROCESS_ALL, False, pid)
    if not h:
        print("OpenProcess 失败 err=%d（可能需管理员）" % ctypes.get_last_error())
        return 1
    try:
        ok_bits, why = check_bitness(h)
        if not ok_bits:
            print("[x] %s" % why)
            return 1
        if unload:
            mods = modules(pid)
            if DLL_NAME.lower() not in mods:
                print("未加载，无需卸载")
                return 0
            hp = k32.GetModuleHandleW("kernel32.dll")
            fn = k32.GetProcAddress(hp, b"FreeLibrary")
            rc = remote_call(h, pid, fn, ctypes.c_void_p(mods[DLL_NAME.lower()][0]).value.to_bytes(4, "little"))
            print("FreeLibrary 返回=%d（0 可能是仍在引用）" % rc)
            time.sleep(0.5)
            print("已卸载" if DLL_NAME.lower() not in modules(pid) else "仍加载中")
            return 0

        target_name = os.path.basename(dll_override(argv)).lower() if dll_override(argv) else DLL_NAME.lower()
        mods = modules(pid)
        if target_name in mods:
            print("已加载，跳过注入（如需换版本先 --unload）")
            return 0
        if "--dll" in argv:
            dll = os.path.abspath(argv[argv.index("--dll") + 1])
        else:
            dll = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", DLL_NAME))
        if not os.path.exists(dll):
            print("找不到 %s" % dll)
            return 1
        hp = k32.GetModuleHandleW("kernel32.dll")
        fn = k32.GetProcAddress(hp, b"LoadLibraryW")
        if "--dry-run" in argv:
            print("[dry-run]", check_only(h, pid, fn))
            print("[dry-run] 未注入")
            return 0
        rc = remote_call(h, pid, fn, (dll.encode("utf-16le") + b"\x00\x00"))
        print("LoadLibraryW 返回=0x%08X %s" % (rc, "（0 = 加载失败）" if rc == 0 else ""))
        for _ in range(20):
            if target_name in modules(pid):
                print("已注入 pid=%d" % pid)
                return 0
            time.sleep(0.25)
        print("注入后未见模块")
        return 1
    finally:
        k32.CloseHandle(h)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
