// injectcore.cpp - 注入 / 卸载 / 状态探测（x86）
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include "injectcore.h"

namespace inj {

const wchar_t* const kGameProc = L"ra3_1.12.game";
const wchar_t* const kDllName  = L"ra3copilot.dll";

namespace {

DWORD FindPidByName(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

bool ModuleBase(DWORD pid, const wchar_t* name, uintptr_t* base) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    bool found = false;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, name) == 0) {
                if (base) *base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                found = true;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

// WSAStartup 只做一次：注入器可能连续调用（/ping 轮询）
void EnsureWinsock() {
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) == 0) {
        WSADATA w{};
        WSAStartup(MAKEWORD(2, 2), &w);
    }
}

} // namespace

DWORD FindGamePid() { return FindPidByName(kGameProc); }

bool IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION el{};
    DWORD n = 0;
    GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &n);
    CloseHandle(tok);
    return el.TokenIsElevated != 0;
}

int QueryModule(DWORD pid, const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    int found = 0;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, name) == 0) { found = 1; break; }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

bool WaitModule(DWORD pid, const wchar_t* name, bool want, int timeoutMs) {
    for (int waited = 0; waited < timeoutMs; waited += 100) {
        if (QueryModule(pid, name) == (want ? 1 : 0)) return true;
        Sleep(100);
    }
    return QueryModule(pid, name) == (want ? 1 : 0);
}

bool HttpTalk(const char* request, char* resp, int rn) {
    if (!request || !resp || rn <= 1) return false;
    resp[0] = 0;
    EnsureWinsock();
    bool ok = false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s != INVALID_SOCKET) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<u_short>(kHttpPort));
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        DWORD tmo = 2000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
        if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
            send(s, request, static_cast<int>(strlen(request)), 0);
            const int n = recv(s, resp, rn - 1, 0);
            if (n > 0) { resp[n] = 0; ok = true; }
        }
        closesocket(s);
    }
    return ok;
}

bool HttpGet(const char* path, char* resp, int rn) {
    char req[512];
    _snprintf_s(req, sizeof(req), _TRUNCATE,
                "GET %s HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                path ? path : "/ping");
    return HttpTalk(req, resp, rn);
}

bool Inject(DWORD pid, const wchar_t* dllPath, std::wstring& message) {
    message.clear();
    if (!pid) { message = L"未找到游戏进程（先启动游戏并进入对局）"; return false; }
    if (!dllPath || !dllPath[0]) { message = L"DLL 路径为空"; return false; }
    if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
        message = L"DLL 不存在：";
        message += dllPath;
        return false;
    }

    const wchar_t* base = wcsrchr(dllPath, L'\\');
    base = base ? base + 1 : dllPath;
    if (QueryModule(pid, base) == 1) {
        message = L"修改器已在游戏里运行（如需更新，请先卸载）";
        return false;
    }

    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                               PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                           FALSE, pid);
    if (!h) {
        const DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            message = IsElevated()
                ? L"无法打开游戏进程（权限被拒绝，可能被安全软件拦截）"
                : L"需要管理员权限才能注入（游戏以更高权限运行）";
        } else {
            wchar_t b[128];
            _snwprintf_s(b, _TRUNCATE, L"无法打开游戏进程（错误 %lu）", err);
            message = b;
        }
        return false;
    }

    const SIZE_T pathBytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(h, nullptr, pathBytes, MEM_COMMIT, PAGE_READWRITE);
    if (!remote) {
        wchar_t b[128];
        _snwprintf_s(b, _TRUNCATE, L"目标进程内存分配失败（错误 %lu）", GetLastError());
        message = b;
        CloseHandle(h);
        return false;
    }
    if (!WriteProcessMemory(h, remote, dllPath, pathBytes, nullptr)) {
        wchar_t b[128];
        _snwprintf_s(b, _TRUNCATE, L"写入目标进程失败（错误 %lu）", GetLastError());
        message = b;
        VirtualFreeEx(h, remote, 0, MEM_RELEASE);
        CloseHandle(h);
        return false;
    }

    // LoadLibraryW 的远程地址：用本进程 kernel32 的偏移 + 目标进程 kernel32 基址，
    // 避免直接拿本进程函数指针（跨进程虽然有共享映射，但按模块基址算更稳）。
    HMODULE localK32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC localLoad = GetProcAddress(localK32, "LoadLibraryW");
    uintptr_t targetK32 = 0;
    uintptr_t remoteLoad = reinterpret_cast<uintptr_t>(localLoad);
    if (localK32 && localLoad && ModuleBase(pid, L"kernel32.dll", &targetK32)) {
        remoteLoad = targetK32 + (reinterpret_cast<uintptr_t>(localLoad) -
                                  reinterpret_cast<uintptr_t>(localK32));
    }

    HANDLE t = CreateRemoteThread(
        h, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteLoad),
        remote, 0, nullptr);
    if (!t) {
        wchar_t b[128];
        _snwprintf_s(b, _TRUNCATE, L"创建远程线程失败（错误 %lu）", GetLastError());
        message = b;
        VirtualFreeEx(h, remote, 0, MEM_RELEASE);
        CloseHandle(h);
        return false;
    }
    WaitForSingleObject(t, 10000);
    DWORD hmod = 0;
    GetExitCodeThread(t, &hmod);
    CloseHandle(t);
    VirtualFreeEx(h, remote, 0, MEM_RELEASE);

    if (!hmod) {
        CloseHandle(h);
        message = L"注入失败：DLL 加载返回 0（可能依赖缺失或被安全软件拦截）";
        return false;
    }

    const bool up = WaitModule(pid, base, true, 5000);
    bool alive = false;
    char resp[256] = { 0 };
    for (int i = 0; i < 20 && !alive; ++i) {
        if (HttpGet("/ping", resp, sizeof(resp))) alive = true;
        else Sleep(250);
    }
    CloseHandle(h);

    wchar_t b[320];
    _snwprintf_s(b, _TRUNCATE,
                 L"注入成功。模块表：%ls；接口：%ls。回游戏按 INSERT 打开菜单。",
                 up ? L"已出现" : L"未确认", alive ? L"已应答" : L"未应答");
    message = b;
    return true;
}

bool Unload(DWORD pid, const std::wstring& dllName, std::wstring& message) {
    message.clear();
    if (!pid) { message = L"游戏未运行，无需卸载"; return true; }

    const int before = QueryModule(pid, dllName.c_str());
    if (before == 0) { message = L"修改器未加载，无需卸载"; return true; }

    char resp[512] = { 0 };
    const bool talked = HttpTalk(
        "POST /unload HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n",
        resp, sizeof(resp));
    if (!talked) {
        if (before < 0) {
            message = L"无法查询游戏模块（权限不足），且接口无应答";
            return false;
        }
        message = L"修改器在运行，但接口无应答 —— 请查看日志或重启游戏";
        return false;
    }

    bool gone = WaitModule(pid, dllName.c_str(), false, 15000);
    if (!gone && before < 0) {
        // 查不到模块表（游戏提权）：接口从「应答」变成「拒绝连接」即证明已卸载
        bool alive = true;
        for (int i = 0; i < 40; ++i) {
            char ping[128] = { 0 };
            if (HttpGet("/ping", ping, sizeof(ping))) {
                Sleep(250);
            } else {
                alive = false;
                break;
            }
        }
        gone = !alive;
    }
    if (!gone) {
        message = L"卸载请求已发送，但 15 秒内模块仍在（可能正忙，请稍后重试）";
        return false;
    }
    message = L"已卸载。可以重新注入。";
    return true;
}

std::wstring ProcessPath(DWORD pid) {
    std::wstring out;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return out;
    wchar_t buf[MAX_PATH * 2] = { 0 };
    DWORD n = static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]));
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) out.assign(buf, n);
    CloseHandle(h);
    return out;
}

} // namespace inj
