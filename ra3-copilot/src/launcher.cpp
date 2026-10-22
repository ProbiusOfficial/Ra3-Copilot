// launcher.cpp - Ra3-Copilot 单文件启动器（x86，ImGui / D3D9）
//
// 一个 exe 完成：运行环境释放 → 识别游戏 → 注入 / 卸载 / 重载 → 状态与日志。
// 界面与局内 overlay 共用同一套深色 + 青色观感（ui_theme.h），但两者代码独立。
//
// 命令行：
//   Ra3Copilot.exe                 正常启动
//   Ra3Copilot.exe --auto-inject   发现游戏未注入时自动注入一次
//   Ra3Copilot.exe --extract-only  只释放运行文件并退出（自检 / 排障）
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <windowsx.h>
#include <d3d9.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <string>
#include <vector>
#include "injectcore.h"
#include "resource.h"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx9.h"
#include "ui_theme.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "ws2_32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace {

const wchar_t* const kWindowClass = L"Ra3CopilotLauncher";
const wchar_t* const kAppName = L"Ra3-Copilot";
const wchar_t* const kVersion = L"1.1.0";

enum { WM_APP_WORK_DONE = WM_APP + 1 };
enum { LOG_INFO = 0, LOG_OK, LOG_WARN, LOG_ERR };

struct WorkResult {
    bool ok = false;
    bool unload = false;
    std::wstring message;
};

struct LogEntry {
    std::wstring text;
    int kind = LOG_INFO;
    SYSTEMTIME st{};
};

HINSTANCE g_inst = nullptr;
HWND g_wnd = nullptr;
IDirect3D9* g_d3d = nullptr;
IDirect3DDevice9* g_dev = nullptr;
D3DPRESENT_PARAMETERS g_pp{};
bool g_running = true;
bool g_minimized = false;
bool g_deviceLost = false;
int g_dpi = 96;
float g_scale = 1.0f;

std::wstring g_exeDir;
std::wstring g_runtimeDir;
std::wstring g_dllPath;
std::wstring g_launcherPath;
std::wstring g_configPath;

DWORD g_pid = 0;
bool g_loaded = false;
bool g_http = false;
bool g_moduleUnknown = false;
bool g_busy = false;
bool g_autoInject = false;
bool g_minimizeAfter = true;
bool g_autoOnce = false;
bool g_chainReload = false;
bool g_relaunching = false;

std::vector<LogEntry> g_logs;
bool g_logDirty = false;
unsigned g_logSeq = 0;

int S(int v) { return static_cast<int>(v * g_scale + 0.5f); }

// ImGui 的 %ls 在 C locale 下会把中文变成 '?'；统一先转 UTF-8 再交给 Text。
std::string ToUtf8(const wchar_t* w) {
    if (!w || !w[0]) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::string ToUtf8(const std::wstring& w) { return ToUtf8(w.c_str()); }

std::wstring Join(const std::wstring& a, const std::wstring& b) {
    std::wstring r = a;
    if (!r.empty() && r.back() != L'\\') r += L'\\';
    r += b;
    return r;
}

std::wstring ExeDir() {
    wchar_t p[MAX_PATH * 2] = { 0 };
    GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
    std::wstring s(p);
    const size_t k = s.find_last_of(L"\\/");
    if (k != std::wstring::npos) s.resize(k);
    return s;
}

std::wstring LocalAppData() {
    wchar_t p[MAX_PATH] = { 0 };
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE,
                                   nullptr, SHGFP_TYPE_CURRENT, p)) && p[0])
        return p;
    return ExeDir();
}

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void EnsureDir(const std::wstring& p) { CreateDirectoryW(p.c_str(), nullptr); }

uint32_t Crc32(const void* data, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool FileCrc(const std::wstring& path, uint32_t* crc, uint64_t* size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li{};
    GetFileSizeEx(h, &li);
    if (size) *size = static_cast<uint64_t>(li.QuadPart);
    uint32_t c = 0xFFFFFFFFu;
    std::vector<uint8_t> buf(64 * 1024);
    DWORD got = 0;
    while (ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got) {
        for (DWORD i = 0; i < got; ++i) {
            const uint8_t b = buf[i];
            c ^= b;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
    }
    CloseHandle(h);
    if (crc) *crc = c ^ 0xFFFFFFFFu;
    return true;
}

bool ReadResource(int id, const void** data, DWORD* size) {
    HRSRC r = FindResourceW(g_inst, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return false;
    HGLOBAL g = LoadResource(g_inst, r);
    if (!g) return false;
    *data = LockResource(g);
    *size = SizeofResource(g_inst, r);
    return *data && *size;
}

// 只在内容不同时写入；游戏加载中的 DLL 不覆盖，留给用户先卸载。
bool WriteResourceIfChanged(int id, const std::wstring& path, std::wstring& err) {
    const void* data = nullptr;
    DWORD size = 0;
    if (!ReadResource(id, &data, &size)) {
        err = L"安装包损坏：找不到内嵌文件";
        return false;
    }
    uint32_t oldCrc = 0;
    uint64_t oldSize = 0;
    if (FileCrc(path, &oldCrc, &oldSize) && oldSize == size && oldCrc == Crc32(data, size))
        return true;

    const std::wstring tmp = path + L".new";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = L"无法写入运行目录：" + g_runtimeDir;
        return false;
    }
    DWORD wr = 0;
    const bool wrote = WriteFile(h, data, size, &wr, nullptr) && wr == size;
    CloseHandle(h);
    if (!wrote) {
        DeleteFileW(tmp.c_str());
        err = L"写入文件失败：" + path;
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD e = GetLastError();
        DeleteFileW(tmp.c_str());
        if (e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED)
            err = L"文件正在被游戏使用";
        else {
            wchar_t b[128];
            _snwprintf_s(b, _TRUNCATE, L"替换文件失败（错误 %lu）", e);
            err = b;
        }
        return false;
    }
    return true;
}

void WriteTextFile(const std::wstring& path, const std::wstring& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) {
        std::string u(static_cast<size_t>(n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, &u[0], n, nullptr, nullptr);
        DWORD wr = 0;
        WriteFile(h, u.data(), static_cast<DWORD>(u.size()), &wr, nullptr);
    }
    CloseHandle(h);
}

void AppendFileLog(const std::wstring& line) {
    if (g_runtimeDir.empty()) return;
    HANDLE h = CreateFileW(Join(g_runtimeDir, L"launcher.log").c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t stamp[64];
    _snwprintf_s(stamp, _TRUNCATE, L"[%04u-%02u-%02u %02u:%02u:%02u] ",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::wstring s = std::wstring(stamp) + line + L"\r\n";
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) {
        std::string u(static_cast<size_t>(n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &u[0], n, nullptr, nullptr);
        DWORD wr = 0;
        WriteFile(h, u.data(), static_cast<DWORD>(u.size()), &wr, nullptr);
    }
    CloseHandle(h);
}

void AppendLog(const std::wstring& text, int kind = LOG_INFO) {
    LogEntry e;
    e.text = text;
    e.kind = kind;
    GetLocalTime(&e.st);
    g_logs.push_back(std::move(e));
    if (g_logs.size() > 500) g_logs.erase(g_logs.begin(), g_logs.begin() + 100);
    g_logDirty = true;
    ++g_logSeq;
    AppendFileLog(text);
}

std::wstring ReadConfig(const wchar_t* key, const wchar_t* def) {
    wchar_t buf[1024] = { 0 };
    GetPrivateProfileStringW(L"launcher", key, def, buf, 1024, g_configPath.c_str());
    return buf;
}

void WriteConfig(const wchar_t* key, const std::wstring& val) {
    WritePrivateProfileStringW(L"launcher", key, val.c_str(), g_configPath.c_str());
}

void LoadConfig() {
    g_autoInject = ReadConfig(L"auto_inject", L"0") == L"1";
    g_minimizeAfter = ReadConfig(L"minimize_after", L"1") == L"1";
    g_launcherPath = ReadConfig(L"game_launcher", L"");
}

void SaveConfig() {
    WriteConfig(L"auto_inject", g_autoInject ? L"1" : L"0");
    WriteConfig(L"minimize_after", g_minimizeAfter ? L"1" : L"0");
    if (!g_launcherPath.empty()) WriteConfig(L"game_launcher", g_launcherPath);
}

bool EnsureRuntime() {
    if (g_runtimeDir.empty()) {
        g_runtimeDir = Join(LocalAppData(), L"Ra3Copilot");
        g_dllPath = Join(g_runtimeDir, L"ra3copilot.dll");
        g_configPath = Join(g_runtimeDir, L"launcher.ini");
    }
    EnsureDir(g_runtimeDir);
    EnsureDir(Join(g_runtimeDir, L"mcp"));
    if (!DirExists(g_runtimeDir)) return false;
    LoadConfig();

    const std::wstring oldKeys = Join(g_exeDir, L"ra3copilot_keys.ini");
    const std::wstring newKeys = Join(g_runtimeDir, L"ra3copilot_keys.ini");
    if (FileExists(oldKeys) && !FileExists(newKeys)) CopyFileW(oldKeys.c_str(), newKeys.c_str(), TRUE);

    std::wstring err;
    if (!WriteResourceIfChanged(IDR_PAYLOAD_DLL, g_dllPath, err) && !FileExists(g_dllPath)) {
        AppendLog(L"运行文件释放失败：" + err, LOG_ERR);
        return false;
    }
    // 游戏正在使用旧 DLL 时保留现有文件；卸载后「重载」会更新。
    std::wstring mcpErr;
    if (!WriteResourceIfChanged(IDR_PAYLOAD_MCP, Join(g_runtimeDir, L"mcp\\ra3_mcp.py"), mcpErr))
        AppendLog(L"MCP 脚本释放失败：" + mcpErr, LOG_WARN);
    std::wstring catErr;
    if (!WriteResourceIfChanged(IDR_PAYLOAD_CATALOG, Join(g_runtimeDir, L"mcp\\unit_catalog.json"), catErr))
        AppendLog(L"MCP 目录释放失败：" + catErr, LOG_WARN);
    return FileExists(g_dllPath);
}

std::wstring ParseDisplayIcon(const std::wstring& raw) {
    std::wstring s = raw;
    if (!s.empty() && s[0] == L'"') {
        const size_t e = s.find(L'"', 1);
        if (e != std::wstring::npos) s = s.substr(1, e - 1);
    } else {
        const size_t c = s.find(L',');
        if (c != std::wstring::npos) s = s.substr(0, c);
    }
    return s;
}

bool RegString(HKEY root, const wchar_t* sub, const wchar_t* name, std::wstring* out) {
    wchar_t buf[1024] = { 0 };
    DWORD cb = sizeof(buf);
    DWORD type = 0;
    if (RegGetValueW(root, sub, name, RRF_RT_REG_SZ, &type, buf, &cb) != ERROR_SUCCESS) return false;
    *out = buf;
    return true;
}

std::wstring FindLauncherPath() {
    if (!g_launcherPath.empty() && FileExists(g_launcherPath)) return g_launcherPath;
    const wchar_t* subs[] = {
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\CoronaLauncher",
        L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\CoronaLauncher",
    };
    const HKEY roots[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
    for (HKEY root : roots) {
        for (const wchar_t* sub : subs) {
            std::wstring v;
            if (RegString(root, sub, L"DisplayIcon", &v)) {
                const std::wstring p = ParseDisplayIcon(v);
                if (FileExists(p)) return p;
            }
            if (RegString(root, sub, L"InstallLocation", &v) && !v.empty()) {
                const std::wstring p = Join(v, L"CoronaLauncher.exe");
                if (FileExists(p)) return p;
            }
        }
    }
    const wchar_t* common[] = {
        L"C:\\GAME\\CoronaLauncher\\CoronaLauncher.exe",
        L"D:\\GAME\\Game_RTS\\CoronaLauncher\\CoronaLauncher.exe",
        L"C:\\Program Files\\CoronaLauncher\\CoronaLauncher.exe",
        L"C:\\Program Files (x86)\\CoronaLauncher\\CoronaLauncher.exe",
    };
    for (const wchar_t* p : common)
        if (FileExists(p)) return p;
    return L"";
}

bool BrowseLauncher(std::wstring* out) {
    wchar_t path[MAX_PATH * 2] = { 0 };
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_wnd;
    ofn.lpstrFilter = L"日冕启动器 (CoronaLauncher.exe)\0CoronaLauncher.exe\0可执行文件 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = L"选择日冕启动器（CoronaLauncher.exe）";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    *out = path;
    return true;
}

void ShowGameWindow() {
    if (!g_pid) return;
    struct Ctx { DWORD pid; HWND found; } ctx{ g_pid, nullptr };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == c->pid && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == nullptr) {
            c->found = h;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.found) {
        if (IsIconic(ctx.found)) ShowWindow(ctx.found, SW_RESTORE);
        SetForegroundWindow(ctx.found);
    }
}

void LaunchGame() {
    std::wstring p = FindLauncherPath();
    if (p.empty()) {
        if (!BrowseLauncher(&p)) return;
        g_launcherPath = p;
        SaveConfig();
    }
    const std::wstring dir = p.substr(0, p.find_last_of(L"\\/"));
    HINSTANCE r = ShellExecuteW(g_wnd, L"open", p.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        AppendLog(L"启动游戏失败，请手动启动日冕启动器", LOG_ERR);
        MessageBoxW(g_wnd, L"无法启动游戏。\n\n请手动启动日冕启动器（CoronaLauncher.exe），\n回到本窗口后点击「注入修改器」。",
                    kAppName, MB_ICONWARNING);
    } else {
        AppendLog(L"已启动游戏启动器：" + p, LOG_OK);
    }
}

void CopyTextToClipboard(const std::wstring& text) {
    if (!OpenClipboard(g_wnd)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (g) {
        void* p = GlobalLock(g);
        if (p) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(g);
            SetClipboardData(CF_UNICODETEXT, g);
        } else {
            GlobalFree(g);
        }
    }
    CloseClipboard();
}

std::wstring McpConfigJson() {
    std::wstring py = Join(g_runtimeDir, L"mcp\\ra3_mcp.py");
    std::wstring esc;
    for (wchar_t c : py) {
        if (c == L'\\') esc += L"\\\\";
        else esc += c;
    }
    return L"{\n  \"mcpServers\": {\n    \"ra3-copilot\": {\n"
           L"      \"command\": \"python\",\n"
           L"      \"args\": [\"" + esc + L"\"]\n"
           L"    }\n  }\n}";
}

void OnMcp() {
    CopyTextToClipboard(McpConfigJson());
    AppendLog(L"MCP 配置已复制到剪贴板（需要本机有 python）", LOG_OK);
    MessageBoxW(g_wnd,
                L"MCP 客户端配置已复制到剪贴板。\n\n"
                L"把它粘贴到支持 MCP 的客户端配置里即可。\n"
                L"要求：本机已安装 Python 3，并且游戏内修改器已注入。",
                kAppName, MB_ICONINFORMATION);
}

void OnFolder() {
    if (g_runtimeDir.empty()) return;
    ShellExecuteW(g_wnd, L"open", g_runtimeDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void RefreshStatus(bool pollHttp) {
    const DWORD pid = inj::FindGamePid();
    const bool changed = (pid != g_pid);
    const bool wasLoaded = (!changed && g_loaded);
    const bool wasHttp = (!changed && g_http);
    g_pid = pid;
    g_loaded = false;
    g_http = false;
    g_moduleUnknown = false;
    if (g_pid) {
        const int m = inj::QueryModule(g_pid, inj::kDllName);
        if (m == 1) {
            g_loaded = true;
        } else if (m < 0) {
            g_moduleUnknown = true;
            g_loaded = wasLoaded;
            g_http = wasHttp;
        }
        // 游戏提权时普通权限查不到模块表；接口应答同样能证明已注入。
        if (pollHttp) {
            char resp[256] = { 0 };
            g_http = inj::HttpGet("/ping", resp, sizeof(resp));
            if (g_http) {
                g_loaded = true;
                g_moduleUnknown = false;
            } else if (m == 0) {
                g_loaded = false;
            }
        }
    }

    static bool lastLoaded = false;
    static DWORD lastPid = 0;
    if (g_pid != lastPid || g_loaded != lastLoaded) {
        lastPid = g_pid;
        lastLoaded = g_loaded;
        if (g_loaded) AppendLog(g_http ? L"修改器已就绪（接口已应答）" : L"修改器已注入", LOG_OK);
        else if (g_pid) AppendLog(L"游戏已运行，修改器未注入", LOG_INFO);
    }

    if (changed) {
        if (g_pid) {
            const std::wstring p = inj::ProcessPath(g_pid);
            AppendLog(L"检测到游戏进程：" + (p.empty() ? std::wstring(L"ra3_1.12.game") : p), LOG_OK);
        } else if (!g_busy) {
            AppendLog(L"游戏已退出", LOG_INFO);
        }
    }
}

bool RelaunchElevated() {
    wchar_t self[MAX_PATH * 2] = { 0 };
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = L"--elevated --auto-inject";
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        AppendLog(L"已取消管理员授权；可右键选择「以管理员身份运行」", LOG_WARN);
        return false;
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);
    AppendLog(L"已请求管理员权限，正在重新启动…", LOG_INFO);
    g_relaunching = true;
    return true;
}

DWORD WINAPI InjectThread(LPVOID) {
    auto* r = new WorkResult{ false, false, L"" };
    r->ok = inj::Inject(g_pid, g_dllPath.c_str(), r->message);
    PostMessageW(g_wnd, WM_APP_WORK_DONE, 0, reinterpret_cast<LPARAM>(r));
    return 0;
}

DWORD WINAPI UnloadThread(LPVOID) {
    auto* r = new WorkResult{ false, true, L"" };
    r->ok = inj::Unload(g_pid, inj::kDllName, r->message);
    PostMessageW(g_wnd, WM_APP_WORK_DONE, 0, reinterpret_cast<LPARAM>(r));
    return 0;
}

void StartInject() {
    if (g_busy) return;
    if (!g_pid) { AppendLog(L"没有找到游戏进程", LOG_WARN); RefreshStatus(false); return; }
    if (!EnsureRuntime()) {
        AppendLog(L"运行文件释放失败，请检查杀毒软件或运行目录权限", LOG_ERR);
        MessageBoxW(g_wnd, L"运行文件释放失败。\n\n请检查杀毒软件是否拦截，或查看 launcher.log。",
                    kAppName, MB_ICONERROR);
        return;
    }
    if (inj::QueryModule(g_pid, inj::kDllName) == 1) {
        AppendLog(L"修改器已经在游戏里，无需重复注入", LOG_INFO);
        RefreshStatus(true);
        return;
    }
    g_busy = true;
    AppendLog(L"正在注入游戏…", LOG_INFO);
    CreateThread(nullptr, 0, InjectThread, nullptr, 0, nullptr);
}

void StartUnload() {
    if (g_busy) return;
    g_busy = true;
    AppendLog(L"正在请求卸载…", LOG_INFO);
    CreateThread(nullptr, 0, UnloadThread, nullptr, 0, nullptr);
}

void OnPrimary() {
    if (g_busy || !g_pid) return;
    if (g_loaded) StartUnload();
    else StartInject();
}

void OnReload() {
    if (g_busy || !g_pid) return;
    g_chainReload = true;
    AppendLog(L"重载：先卸载，再重新注入当前版本", LOG_INFO);
    if (g_loaded) StartUnload();
    else { g_chainReload = false; StartInject(); }
}

// ===================== D3D9 / ImGui =====================
bool CreateDeviceD3D(HWND hwnd) {
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) return false;
    ZeroMemory(&g_pp, sizeof(g_pp));
    g_pp.Windowed = TRUE;
    g_pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_pp.BackBufferFormat = D3DFMT_UNKNOWN;
    g_pp.EnableAutoDepthStencil = FALSE;
    g_pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    g_pp.hDeviceWindow = hwnd;

    HRESULT hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_pp, &g_dev);
    if (FAILED(hr))
        hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 D3DCREATE_SOFTWARE_VERTEXPROCESSING, &g_pp, &g_dev);
    if (FAILED(hr))
        hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, hwnd,
                                 D3DCREATE_SOFTWARE_VERTEXPROCESSING, &g_pp, &g_dev);
    return SUCCEEDED(hr);
}

void ResetDevice(int w, int h) {
    if (!g_dev || w <= 0 || h <= 0) return;
    ImGui_ImplDX9_InvalidateDeviceObjects();
    g_pp.BackBufferWidth = static_cast<UINT>(w);
    g_pp.BackBufferHeight = static_cast<UINT>(h);
    HRESULT hr = g_dev->Reset(&g_pp);
    if (hr == D3DERR_INVALIDCALL) {
        g_pp.BackBufferWidth = 0;
        g_pp.BackBufferHeight = 0;
        hr = g_dev->Reset(&g_pp);
    }
    if (SUCCEEDED(hr)) {
        g_deviceLost = false;
        ImGui_ImplDX9_CreateDeviceObjects();
    }
}

void DrawStatusCard() {
    const float s = g_scale;
    ImVec4 col(0.55f, 0.60f, 0.66f, 1.f);
    const wchar_t* status = L"检测中…";
    if (g_busy) status = L"正在处理…";
    else if (!g_pid) status = L"游戏未运行";
    else if (g_loaded) status = g_http ? L"修改器已就绪" : L"修改器已注入";
    else if (g_moduleUnknown) status = L"游戏已运行 · 需要管理员权限";
    else status = L"游戏已运行 · 未注入";

    if (g_busy) col = ImVec4(0.95f, 0.75f, 0.35f, 1.f);
    else if (g_loaded && g_http) col = ImVec4(0.45f, 0.90f, 0.65f, 1.f);
    else if (g_loaded) col = ImVec4(0.55f, 0.85f, 0.95f, 1.f);
    else if (g_pid) col = ImVec4(0.95f, 0.75f, 0.35f, 1.f);

    // 圆点和文字放在同一次 Text 调用里，避免 SameLine 错位。
    ImGui::TextColored(col, "● %s", ToUtf8(status).c_str());
    if (g_pid) {
        ImGui::SameLine(0, 16 * s);
        ImGui::TextDisabled("进程 %lu", g_pid);
    }
    if (!g_pid) {
        ImGui::TextDisabled("先启动日冕启动器并进入对局");
    } else if (g_busy) {
        const char* dots[] = { "", ".", "..", "..." };
        ImGui::TextDisabled("处理中%s 请稍候", dots[static_cast<int>(ImGui::GetTime() * 3) & 3]);
    } else if (!g_loaded) {
        ImGui::TextDisabled("点「注入」开始；需要时会请求一次管理员权限");
    }
}

void DrawLogPanel() {
    const float s = g_scale;
    ImGui::TextDisabled("运行记录");
    ImGui::SameLine(0, 8 * s);
    if (ImGui::SmallButton("清空")) { g_logs.clear(); ++g_logSeq; }

    ImGui::BeginChild("##log", ImVec2(0, -20 * s), true, ImGuiWindowFlags_HorizontalScrollbar);
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    for (const LogEntry& e : g_logs) {
        ImVec4 c(0.72f, 0.77f, 0.83f, 1.f);
        if (e.kind == LOG_OK) c = ImVec4(0.45f, 0.90f, 0.65f, 1.f);
        if (e.kind == LOG_WARN) c = ImVec4(0.95f, 0.85f, 0.40f, 1.f);
        if (e.kind == LOG_ERR) c = ImVec4(0.95f, 0.45f, 0.45f, 1.f);
        ImGui::TextColored(ImVec4(0.42f, 0.48f, 0.55f, 1.f), "%02u:%02u:%02u",
                           e.st.wHour, e.st.wMinute, e.st.wSecond);
        ImGui::SameLine(0, 8 * s);
        ImGui::TextColored(c, "%s", ToUtf8(e.text).c_str());
    }
    if (g_logDirty) {
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2 * lineH)
            ImGui::SetScrollHereY(1.0f);
        g_logDirty = false;
    }
    ImGui::EndChild();

    ImGui::TextDisabled("v1.12.3444.25830  ·  github.com/ProbiusOfficial/Ra3-Copilot");
}

void BuildUi() {
    ImGuiIO& io = ImGui::GetIO();
    const float s = g_scale;
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(9 * s, 8 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // 顶栏只保留两行，和局内面板一样克制。
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.85f, 0.95f, 1.f));
    ImGui::TextUnformatted("Ra3-Copilot");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 8 * s);
    ImGui::TextDisabled("v1.1.0");
    ImGui::TextDisabled("红色警戒 3 · 日冕 Mod 局内修改器");

    ImGui::Separator();
    DrawStatusCard();
    ImGui::Separator();

    const bool canAct = g_pid && !g_busy;
    const char* primary = g_loaded ? "卸载" : "注入";
    ImGui::BeginDisabled(!g_pid || g_busy);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.42f, 0.52f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.55f, 0.66f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.34f, 0.42f, 1.f));
    if (ImGui::Button(primary, ImVec2(112 * s, 0))) OnPrimary();
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(g_loaded ? "从游戏进程里卸下修改器，文件锁随之释放"
                                   : "把修改器注入当前运行的游戏");

    ImGui::SameLine(0, 6 * s);
    ImGui::BeginDisabled(!canAct);
    if (ImGui::Button("重载", ImVec2(76 * s, 0))) OnReload();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("卸载后重新注入当前版本\n更新 Ra3Copilot.exe 后用这个");

    ImGui::SameLine(0, 6 * s);
    ImGui::BeginDisabled(g_busy);
    if (ImGui::Button(g_pid ? "游戏" : "启动", ImVec2(76 * s, 0))) {
        if (g_pid) ShowGameWindow();
        else LaunchGame();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(g_pid ? "把游戏窗口切到前台" : "启动日冕启动器；找不到时手动选择");

    ImGui::SameLine(0, 6 * s);
    if (ImGui::Button("MCP", ImVec2(68 * s, 0))) OnMcp();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("复制 MCP 客户端配置到剪贴板");
    ImGui::SameLine(0, 6 * s);
    if (ImGui::Button("目录", ImVec2(68 * s, 0))) OnFolder();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("打开运行目录（日志 / 配置 / MCP 脚本）");

    if (ImGui::Checkbox("自动注入", &g_autoInject)) SaveConfig();
    ImGui::SameLine(0, 18 * s);
    if (ImGui::Checkbox("注入后最小化", &g_minimizeAfter)) SaveConfig();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("注入完成后把启动器最小化，不挡游戏");

    ImGui::Separator();
    DrawLogPanel();

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void RenderFrame() {
    if (!g_dev) return;
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    BuildUi();
    ImGui::Render();

    g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(16, 19, 24), 1.0f, 0);
    if (SUCCEEDED(g_dev->BeginScene())) {
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        g_dev->EndScene();
    }
    const HRESULT hr = g_dev->Present(nullptr, nullptr, nullptr, nullptr);
    if (hr == D3DERR_DEVICELOST) {
        g_deviceLost = true;
        Sleep(20);
    } else if (hr == D3DERR_DEVICENOTRESET) {
        ResetDevice(static_cast<int>(ImGui::GetIO().DisplaySize.x),
                    static_cast<int>(ImGui::GetIO().DisplaySize.y));
    }
}

void Cleanup() {
    if (g_dev) {
        ImGui_ImplDX9_Shutdown();
        g_dev->Release();
        g_dev = nullptr;
    }
    if (g_d3d) { g_d3d->Release(); g_d3d = nullptr; }
    ImGui_ImplWin32_Shutdown();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
}

void InitDpiEarly() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI* SetCtx_t)(HANDLE);
    auto setCtx = reinterpret_cast<SetCtx_t>(GetProcAddress(u32, "SetProcessDpiAwarenessContext"));
    if (setCtx) setCtx(reinterpret_cast<HANDLE>(-4));
    else SetProcessDPIAware();

    typedef UINT (WINAPI* GetDpiForSystem_t)();
    auto getDpi = reinterpret_cast<GetDpiForSystem_t>(GetProcAddress(u32, "GetDpiForSystem"));
    int d = getDpi ? static_cast<int>(getDpi()) : 96;
    HDC dc = GetDC(nullptr);
    if (dc) {
        const int sysDpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
        if (sysDpi > d) d = sysDpi;
    }
    if (d < 96 || d > 480) d = 96;
    g_dpi = d;
    g_scale = static_cast<float>(g_dpi) / 96.0f;
}

void InitDpi(HWND hwnd) {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    typedef UINT (WINAPI* GetDpi_t)(HWND);
    auto getDpi = reinterpret_cast<GetDpi_t>(GetProcAddress(u32, "GetDpiForWindow"));
    int d = getDpi ? static_cast<int>(getDpi(hwnd)) : 96;
    HDC dc = GetDC(nullptr);
    if (dc) {
        const int sysDpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
        if (sysDpi > d) d = sysDpi;
    }
    if (d > g_dpi) g_dpi = d;
    if (g_dpi < 96 || g_dpi > 480) g_dpi = 96;
    g_scale = static_cast<float>(g_dpi) / 96.0f;
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, msg, w, l)) return 1;

    switch (msg) {
    case WM_SIZE:
        if (g_dev && w != SIZE_MINIMIZED) {
            g_minimized = false;
            ResetDevice(LOWORD(l), HIWORD(l));
        } else if (w == SIZE_MINIMIZED) {
            g_minimized = true;
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
        mmi->ptMinTrackSize.x = S(500);
        mmi->ptMinTrackSize.y = S(360);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((w & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_APP_WORK_DONE: {
        auto* r = reinterpret_cast<WorkResult*>(l);
        g_busy = false;
        bool handoff = false;
        if (r) {
            AppendLog(r->message, r->ok ? LOG_OK : LOG_ERR);
            if (r->ok) {
                if (!r->unload && g_minimizeAfter) ShowWindow(h, SW_MINIMIZE);
                if (r->unload && g_chainReload) {
                    g_chainReload = false;
                    StartInject();
                }
            } else if (!inj::IsElevated() && r->message.find(L"管理员权限") != std::wstring::npos) {
                handoff = RelaunchElevated();
            } else {
                g_chainReload = false;
                if (g_autoInject) {
                    g_autoInject = false;
                    SaveConfig();
                    AppendLog(L"自动注入已暂停；处理完上面的问题后可重新勾选", LOG_WARN);
                }
            }
            delete r;
        }
        if (handoff) {
            g_running = false;
            DestroyWindow(h);
            return 0;
        }
        RefreshStatus(true);
        return 0;
    }
    case WM_CLOSE:
        if (g_busy) {
            if (MessageBoxW(h, L"正在处理中，确定要退出吗？", kAppName,
                            MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
                return 0;
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_running = false;
        Cleanup();
        PostQuitMessage(0);
        return 0;
    case WM_PAINT:
        ValidateRect(h, nullptr);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(h, msg, w, l);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int show) {
    g_inst = inst;
    g_exeDir = ExeDir();
    g_runtimeDir = Join(LocalAppData(), L"Ra3Copilot");
    g_dllPath = Join(g_runtimeDir, L"ra3copilot.dll");
    g_configPath = Join(g_runtimeDir, L"launcher.ini");

    const bool extractOnly = cmdLine && wcsstr(cmdLine, L"--extract-only");
    const bool autoInject = cmdLine && wcsstr(cmdLine, L"--auto-inject");
    const bool elevated = cmdLine && wcsstr(cmdLine, L"--elevated");

    EnsureRuntime();
    if (autoInject) g_autoOnce = true;

    if (extractOnly) {
        std::wstring report;
        report += L"runtime=" + g_runtimeDir + L"\r\n";
        report += L"dll=" + g_dllPath + L"\r\n";
        report += L"dll_exists=" + std::wstring(FileExists(g_dllPath) ? L"1" : L"0") + L"\r\n";
        report += L"mcp_exists=" + std::wstring(FileExists(Join(g_runtimeDir, L"mcp\\ra3_mcp.py")) ? L"1" : L"0") + L"\r\n";
        report += L"catalog_exists=" + std::wstring(FileExists(Join(g_runtimeDir, L"mcp\\unit_catalog.json")) ? L"1" : L"0") + L"\r\n";
        report += L"elevated=" + std::wstring(inj::IsElevated() ? L"1" : L"0") + L"\r\n";
        WriteTextFile(Join(g_runtimeDir, L"extract_report.txt"), report);
        return 0;
    }

    HANDLE mutex = nullptr;
    if (!elevated) {
        mutex = CreateMutexW(nullptr, TRUE, L"Ra3Copilot.Launcher.SingleInstance");
        if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND old = FindWindowW(kWindowClass, nullptr);
            if (old) {
                if (IsIconic(old)) ShowWindow(old, SW_RESTORE);
                SetForegroundWindow(old);
            }
            CloseHandle(mutex);
            return 0;
        }
    }

    InitDpiEarly();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    RECT wr{ 0, 0, S(560), S(420) };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);
    HWND h = CreateWindowExW(0, kWindowClass, L"Ra3-Copilot · 修改器启动器",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             wr.right - wr.left, wr.bottom - wr.top,
                             nullptr, nullptr, inst, nullptr);
    if (!h) return 1;
    g_wnd = h;

    InitDpi(h);
    if (g_scale > 1.0f) {
        RECT sr{ 0, 0, S(560), S(420) };
        AdjustWindowRectEx(&sr, WS_OVERLAPPEDWINDOW, FALSE, 0);
        SetWindowPos(h, nullptr, 0, 0, sr.right - sr.left, sr.bottom - sr.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (!CreateDeviceD3D(h)) {
        MessageBoxW(h, L"无法创建 Direct3D 9 设备。\n\n请确认显卡驱动正常，或改用命令行注入器。",
                    kAppName, MB_ICONERROR);
        DestroyWindow(h);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui_ImplWin32_Init(h);
    ImGui_ImplDX9_Init(g_dev);
    ra3::ApplyUiTheme(1.0f);
    {
        ImGuiStyle& st = ImGui::GetStyle();
        st.ItemSpacing = ImVec2(7 * g_scale, 5 * g_scale);
        st.FramePadding = ImVec2(8 * g_scale, 4 * g_scale);
    }
    ra3::AddUiFont(io, 16.0f * g_scale);

    ShowWindow(h, show ? show : SW_SHOWNORMAL);
    UpdateWindow(h);

    RefreshStatus(true);
    AppendLog(std::wstring(L"Ra3-Copilot ") + kVersion + L" 已启动", LOG_OK);
    AppendLog(L"运行目录：" + g_runtimeDir, LOG_INFO);
    if (elevated) AppendLog(L"已获得管理员权限", LOG_INFO);
    if (g_autoOnce && g_pid && !g_loaded) StartInject();

    DWORD lastStatus = GetTickCount();
    DWORD lastHttp = 0;
    while (g_running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) { g_running = false; break; }
        }
        if (!g_running) break;

        const DWORD now = GetTickCount();
        if (now - lastStatus >= 800) {
            lastStatus = now;
            const bool pollHttp = (now - lastHttp) >= 3000;
            if (pollHttp) lastHttp = now;
            if (!g_relaunching && !g_busy) {
                RefreshStatus(pollHttp);
                if (g_autoOnce && g_pid && !g_loaded) { g_autoOnce = false; StartInject(); }
                else if (g_autoInject && g_pid && !g_loaded) StartInject();
            }
        }

        if (!g_minimized) RenderFrame();
        else Sleep(30);
    }

    if (ImGui::GetCurrentContext()) Cleanup();
    if (mutex) CloseHandle(mutex);
    return 0;
}
