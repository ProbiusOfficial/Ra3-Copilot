// injector.cpp - Ra3-Copilot 注入器 / 卸载器（x86）
//
// 用法:
//   injector.exe [dll_path]   注入（默认与自身同目录 ra3copilot.dll）；注入后复核模块表 + HTTP
//   injector.exe --unload     请求游戏内实例卸载自身，并等 ra3copilot 从进程消失
//   injector.exe --status     只查游戏进程在不在
//
// 流程（注入）: toolhelp 按名找 ra3_1.12.game -> 预检同名模块未加载 -> OpenProcess
//       -> 路径写目标进程 -> CreateRemoteThread(LoadLibraryW) -> GetExitCodeThread 拿 HMODULE
//       -> 复核：模块表出现 + 端口 17832 /ping 应答（防「注了但没活」）
// 流程（卸载）: POST 127.0.0.1:17832/unload（游戏内实例受理，下一帧真卸载）
//       -> 轮询模块表等 ra3copilot 消失（真卸载判据：消失 = 文件锁释放，可覆盖重编再注入）
// 提权: 游戏以管理员运行（继承 QuickLoader），OpenProcess 被拒时自动 UAC 提权重启自身
//       （ShellExecute runas，保留原参数）。
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <cwchar>

static const wchar_t* GAME_PROC = L"ra3_1.12.game";
static const wchar_t* DLL_NAME  = L"ra3copilot.dll";
static const int      HTTP_PORT = 17832;

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

// 以管理员身份重启自身，保留全部原参数（子命令 + DLL 路径都要带过去）
static bool RelaunchElevated(int argc, wchar_t** argv) {
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t params[1280] = L"";
    for (int i = 1; i < argc; ++i) {
        if (i > 1) wcscat_s(params, L" ");
        wcscat_s(params, L"\"");
        wcscat_s(params, argv[i]);
        wcscat_s(params, L"\"");
    }
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = params[0] ? params : nullptr;
    sei.nShow = SW_SHOW;
    if (!ShellExecuteExW(&sei))
        return false;
    if (sei.hProcess)
        CloseHandle(sei.hProcess);
    return true;
}

// ---- 模块表查询：1=已加载 0=未加载 -1=查询失败（权限/进程已退出）----
static int QueryModule(DWORD pid, const wchar_t* name) {
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

// 等模块达到期望状态（want=true 等出现 / false 等消失）。返回 true=已达到
static bool WaitModule(DWORD pid, const wchar_t* name, bool want, int timeoutMs) {
    for (int waited = 0; waited < timeoutMs; waited += 100) {
        if (QueryModule(pid, name) == (want ? 1 : 0)) return true;
        Sleep(100);
    }
    return QueryModule(pid, name) == (want ? 1 : 0);
}

// ---- 最小 HTTP 客户端（127.0.0.1:17832）：发原始请求，读到至少 1 字节算通 ----
static bool HttpTalk(const char* req, char* resp, int rn) {
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return false;
    bool ok = false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s != INVALID_SOCKET) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((u_short)HTTP_PORT);
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (connect(s, (sockaddr*)&a, sizeof(a)) == 0) {
            send(s, req, (int)strlen(req), 0);
            DWORD tmo = 2000;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
            const int n = recv(s, resp, rn - 1, 0);
            if (n > 0) { resp[n] = 0; ok = true; }
        }
        closesocket(s);
    }
    WSACleanup();
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    const bool statusOnly = (argc > 1 && _wcsicmp(argv[1], L"--status") == 0);
    const bool unloadOnly = (argc > 1 && _wcsicmp(argv[1], L"--unload") == 0);

    const DWORD pid = FindGamePid();
    if (statusOnly) {
        if (!pid) { wprintf(L"[x] 未找到游戏进程 %ls\n", GAME_PROC); return 1; }
        wprintf(L"[+] 游戏进程 pid=%lu\n", pid);
        return 0;
    }

    // ==================== 卸载（热重载第一步）====================
    if (unloadOnly) {
        if (!pid) { wprintf(L"[+] 游戏未运行 —— 无需卸载\n"); return 0; }   // 幂等：已满足

        HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!h && GetLastError() == ERROR_ACCESS_DENIED && !IsElevated()) {
            wprintf(L"[*] 需要权限查模块表，正在申请 UAC 提权重启...\n");
            if (RelaunchElevated(argc, argv)) { wprintf(L"[*] 提权实例已接管，本实例退出\n"); return 0; }
        }

        const int before = QueryModule(pid, DLL_NAME);
        if (before == 0) {
            wprintf(L"[+] ra3copilot 未加载 —— 已是目标状态\n");
            if (h) CloseHandle(h);
            return 0;
        }
        if (before < 0)
            wprintf(L"[!] 模块表查询失败（权限不足？）—— 继续尝试经接口卸载\n");

        char resp[600] = { 0 };
        const bool talked = HttpTalk(
            "POST /unload HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n",
            resp, sizeof(resp));
        if (talked) {
            wprintf(L"[+] /unload 已受理：\n%hs\n", resp);
        } else if (before == 1) {
            wprintf(L"[x] 实例在跑但接口 17832 无应答 —— 无法协调卸载（查 ra3copilot_log.txt）\n");
            if (h) CloseHandle(h);
            return 1;
        } else {
            wprintf(L"[x] 接口无应答且模块表不可查 —— 无法确认卸载\n");
            if (h) CloseHandle(h);
            return 1;
        }

        wprintf(L"[*] 等待 ra3copilot 从进程消失（真卸载判据）...\n");
        const bool gone = WaitModule(pid, DLL_NAME, false, 15000);
        if (h) CloseHandle(h);
        if (gone) {
            wprintf(L"[+] 卸载完成 —— DLL 文件锁已释放，可重新编译并注入\n");
            return 0;
        }
        wprintf(L"[x] 15s 内模块仍在：卸载被中止（查 ra3copilot_log.txt 的 [卸载] 行）\n");
        return 1;
    }

    // ==================== 注入（默认）====================
    if (!pid) {
        wprintf(L"[x] 未找到游戏进程 %ls（先启动游戏并进入对局）\n", GAME_PROC);
        return 1;
    }
    wprintf(L"[+] 找到游戏进程 pid=%lu\n", pid);

    // DLL 路径：参数指定或默认与 injector 同目录
    wchar_t dllPath[MAX_PATH] = { 0 };
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

    // 防双实例预检：同名模块已在进程里 → 拒绝（重复注入会让新代码不生效 —— 两份映像各有独立
    //   全局量，AOB 校验与端口绑定都会失败/静默让位，最后「命令成功、跑的还是旧代码」）
    const wchar_t* base = wcsrchr(dllPath, L'\\');
    base = base ? base + 1 : dllPath;
    if (QueryModule(pid, base) == 1) {
        wprintf(L"[x] %ls 已在进程里 —— 先卸载再注入：\n", base);
        wprintf(L"    injector.exe --unload   ->   重新编译   ->   injector.exe\n");
        return 1;
    }

    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                               PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                           FALSE, pid);
    if (!h) {
        DWORD err = GetLastError();
        wprintf(L"[!] OpenProcess 失败 err=%lu\n", err);
        if (err == ERROR_ACCESS_DENIED && !IsElevated()) {
            wprintf(L"[*] 游戏是管理员权限，正在申请 UAC 提权重启注入器...\n");
            if (RelaunchElevated(argc, argv)) {
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

        // 复核：模块表出现 + 接口 /ping 应答
        const bool up = WaitModule(pid, base, true, 5000);
        char resp[300] = { 0 };
        bool alive = false;
        for (int i = 0; i < 20 && !alive; ++i) {
            if (HttpTalk("GET /ping HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n", resp, sizeof(resp)))
                alive = true;
            else
                Sleep(250);
        }
        wprintf(L"[+] 复核：模块表 %ls / HTTP 17832 %ls\n",
                up ? L"已出现" : L"未出现(?)",
                alive ? L"已应答" : L"未应答(?)");
        wprintf(L"[+] 游戏中按 INSERT 显示/隐藏菜单（其余热键见菜单「设置 -> 快捷键」）\n");
        return 0;
    }
    wprintf(L"[x] 注入失败（LoadLibraryW 返回 0，DLL 可能依赖缺失或崩溃）\n");
    return 1;
}
