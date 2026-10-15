// injector.cpp - Ra3-Copilot 注入器（x86）
//
// 用法:
//   injector.exe [dll_path]     默认注入同目录 ra3copilot.dll
//   injector.exe --status       只查游戏进程在不在
//
// 流程: toolhelp 按名找 ra3_1.12.game -> OpenProcess -> 路径写目标进程
//       -> CreateRemoteThread(LoadLibraryW) -> GetExitCodeThread 拿 HMODULE
// 提权: 游戏以管理员运行（继承 QuickLoader），OpenProcess 被拒时自动 UAC
//       提权重启自身（ShellExecute runas）。
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>

static const wchar_t* GAME_PROC = L"ra3_1.12.game";
static const wchar_t* DLL_NAME  = L"ra3copilot.dll";

static DWORD FindGamePid() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, GAME_PROC) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static bool IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        return false;
    TOKEN_ELEVATION el{};
    DWORD n = 0;
    GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &n);
    CloseHandle(tok);
    return el.TokenIsElevated != 0;
}

// 以管理员身份重启自身，保留原参数（DLL 路径透传）
static bool RelaunchElevated(const wchar_t* extraArg) {
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t params[MAX_PATH + 16]{};
    if (extraArg)
        _snwprintf_s(params, _countof(params), _TRUNCATE, L"\"%s\"", extraArg);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = extraArg ? params : nullptr;
    sei.nShow = SW_SHOW;
    if (!ShellExecuteExW(&sei))
        return false;
    if (sei.hProcess)
        CloseHandle(sei.hProcess);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    bool statusOnly = (argc > 1 && _wcsicmp(argv[1], L"--status") == 0);

    const DWORD pid = FindGamePid();
    if (!pid) {
        wprintf(L"[x] 未找到游戏进程 %ls（先启动游戏并进入对局）\n", GAME_PROC);
        return 1;
    }
    wprintf(L"[+] 找到游戏进程 pid=%lu\n", pid);
    if (statusOnly)
        return 0;

    // DLL 路径：参数指定或默认与 injector 同目录
    wchar_t dllPath[MAX_PATH]{};
    if (argc > 1) {
        wcsncpy_s(dllPath, argv[1], _TRUNCATE);
    } else {
        GetModuleFileNameW(nullptr, dllPath, MAX_PATH);
        wchar_t* slash = wcsrchr(dllPath, L'\\');
        if (slash)
            wcsncpy_s(slash + 1, MAX_PATH - (slash + 1 - dllPath), DLL_NAME, _TRUNCATE);
    }
    if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"[x] DLL 不存在: %ls\n", dllPath);
        return 1;
    }
    wprintf(L"[+] DLL: %ls\n", dllPath);

    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                               PROCESS_VM_OPERATION | PROCESS_VM_WRITE,
                           FALSE, pid);
    if (!h) {
        DWORD err = GetLastError();
        wprintf(L"[!] OpenProcess 失败 err=%lu\n", err);
        if (err == ERROR_ACCESS_DENIED && !IsElevated()) {
            wprintf(L"[*] 游戏是管理员权限，正在申请 UAC 提权重启注入器...\n");
            if (RelaunchElevated(argc > 1 ? argv[1] : nullptr)) {
                wprintf(L"[*] 提权实例已接管，本实例退出\n");
                return 0;
            }
        }
        wprintf(L"[x] 无法打开目标进程（请检查权限）\n");
        return 1;
    }

    // 路径写进目标进程
    const SIZE_T pathBytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(h, nullptr, pathBytes, MEM_COMMIT, PAGE_READWRITE);
    if (!remote) {
        wprintf(L"[x] VirtualAllocEx 失败 err=%lu\n", GetLastError());
        CloseHandle(h);
        return 1;
    }
    if (!WriteProcessMemory(h, remote, dllPath, pathBytes, nullptr)) {
        wprintf(L"[x] WriteProcessMemory 失败 err=%lu\n", GetLastError());
        VirtualFreeEx(h, remote, 0, MEM_RELEASE);
        CloseHandle(h);
        return 1;
    }

    // CreateRemoteThread(LoadLibraryW)
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    auto pfnLoad = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(k32, "LoadLibraryW"));
    HANDLE t = CreateRemoteThread(h, nullptr, 0, pfnLoad, remote, 0, nullptr);
    if (!t) {
        wprintf(L"[x] CreateRemoteThread 失败 err=%lu\n", GetLastError());
        VirtualFreeEx(h, remote, 0, MEM_RELEASE);
        CloseHandle(h);
        return 1;
    }
    WaitForSingleObject(t, 10000);
    DWORD hmod = 0;
    GetExitCodeThread(t, &hmod);
    CloseHandle(t);
    VirtualFreeEx(h, remote, 0, MEM_RELEASE);
    CloseHandle(h);

    if (hmod) {
        wprintf(L"[+] 注入成功! 模块句柄=%p\n", (void*)hmod);
        wprintf(L"[+] 游戏中按 INSERT 显示/隐藏菜单，F7 加钱\n");
        return 0;
    }
    wprintf(L"[x] 注入失败（LoadLibraryW 返回 0，DLL 可能依赖缺失或崩溃）\n");
    return 1;
}
