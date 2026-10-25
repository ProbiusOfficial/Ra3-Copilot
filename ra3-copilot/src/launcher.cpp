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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <algorithm>
#include "injectcore.h"
#include "resource.h"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx9.h"
#include "ui_theme.h"
#include "units_corona.h"     // 预设增援的单位清单（与局内召唤面板同一张表）

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
const wchar_t* const kVersion = L"1.3.0";

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
std::wstring g_keysPath;
std::wstring g_presetPath;

// 呼出菜单的键（与局内修改器共用 ra3copilot_keys.ini 的 toggle= 行）。
// 75 配列等没有 INSERT 键的键盘靠这里改 —— 局内面板打不开时，这是唯一入口。
int  g_toggleVk = VK_INSERT;
bool g_toggleCtrl = false;
int  g_captureTarget = 0;       // 正在等哪个键：0=没有，-1=呼出键，>0=功能热键 id
ULONGLONG g_captureSince = 0;   // 捕获起始时刻：没有超时的话，点了按钮又不按，之后随便一个键就改掉呼出键
int  g_page = 0;                // 0=主界面，1=设置
bool g_keysTop = true;          // 进设置页时把键位列表滚回顶部（首帧同样从头看）

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

// ===== 快捷键 =====
// 与局内修改器共用 ra3copilot_keys.ini（格式见 overlay.cpp 的 LoadKeyBinds）：
//   enabled=1
//   toggle=<vk> <ctrl>
//   <id> <vk> <ctrl> <说明>
// 无节名的裸文本，按行读写。说明列由修改器写入，启动器只负责显示。

// 键码 → 显示名（与局内面板同一套叫法；两边独立编译，各维护一份）
std::wstring KeyNameW(int vk) {
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, static_cast<wchar_t>(vk));
    switch (vk) {
    case VK_OEM_MINUS:  return L"-";
    case VK_OEM_PLUS:   return L"+";
    case VK_OEM_4:      return L"[";
    case VK_OEM_6:      return L"]";
    case VK_OEM_5:      return L"\\";
    case VK_OEM_1:      return L";";
    case VK_OEM_7:      return L"'";
    case VK_OEM_COMMA:  return L",";
    case VK_OEM_PERIOD: return L".";
    case VK_OEM_2:      return L"/";
    case VK_PRIOR:      return L"PageUp";
    case VK_NEXT:       return L"PageDown";
    case VK_INSERT:     return L"Insert";
    case VK_DELETE:     return L"Delete";
    case VK_HOME:       return L"Home";
    case VK_END:        return L"End";
    case VK_SPACE:      return L"Space";
    case VK_TAB:        return L"Tab";
    case VK_BACK:       return L"Backspace";
    default: {
        wchar_t b[16] = { 0 };
        swprintf_s(b, L"VK%02X", vk);
        return b;
    }
    }
}

std::wstring KeyLabelW(int vk, bool ctrl) {
    std::wstring k = KeyNameW(vk);
    return ctrl ? (L"Ctrl+" + k) : k;
}

std::wstring ToggleLabelW() { return KeyLabelW(g_toggleVk, g_toggleCtrl); }

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return L"";
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

struct KeyRow {
    int id = 0;
    int vk = 0;
    bool ctrl = false;
    std::string desc;          // UTF-8
};

std::vector<KeyRow> g_keyRows;
bool g_hkEnabled = true;        // 功能热键总开关；界面上的「静默」是它的反面，默认不勾

// ini 缺失或还是老格式（没有说明列）时的出厂绑定。
// 取值与 overlay.cpp 的 kHKDefault 同源，只用于显示与冲突提示。
struct KeyDef { int id; int vk; bool ctrl; const char* desc; };
const KeyDef kKeyDefault[] = {
    { 1,  VK_F1,          true,  "加钱（每次金额）" },
    { 2,  VK_F2,          true,  "电力无限（开关）" },
    { 3,  VK_F3,          true,  "科技点无限（开关）" },
    { 4,  VK_F4,          true,  "全科技（开关）" },
    { 5,  VK_F5,          true,  "快速建造（开关）" },
    { 6,  VK_F6,          true,  "超武就绪（开关）" },
    { 7,  VK_F7,          true,  "禁用敌方超武（开关）" },
    { 29, VK_F8,          true,  "无限缩放（开关）" },
    { 8,  VK_F9,          true,  "全图 / 去迷雾（开关）" },
    { 9,  VK_F10,         true,  "敌人无法建造（开关）" },
    { 10, VK_F11,         true,  "玩家无敌（开关）" },
    { 12, 'P',            false, "升级单位（满级）" },
    { 30, 'O',            false, "接管单位（把选中单位改成你的）" },
    { 31, 'I',            false, "复制选中单位" },
    { 13, VK_OEM_MINUS,   false, "加速（选中单位）" },
    { 14, VK_OEM_PLUS,    false, "减速（选中单位）" },
    { 15, VK_PRIOR,       false, "定住单位" },
    { 16, VK_NEXT,        false, "恢复速度" },
    { 17, VK_OEM_4,       false, "无限血量" },
    { 18, VK_OEM_6,       false, "残血（血量设为 1）" },
    { 19, VK_OEM_5,       false, "恢复血量" },
    { 20, VK_OEM_1,       false, "选中单位弹药满（开关）" },
    { 21, VK_DELETE,      false, "摧毁选中" },
    { 22, VK_OEM_COMMA,   false, "危险等级 = 最高" },
    { 23, VK_OEM_PERIOD,  false, "危险等级 = 最低" },
    { 24, VK_OEM_2,       false, "危险等级 = 不干预" },
    { 25, VK_OEM_7,       false, "恢复矿场" },
    { 26, 'L',            false, "免费建造（开关）" },
    { 27, 'K',            false, "召唤基地车" },
    { 32, 'J',            false, "预设增援（召唤 ×8）" },
};

bool ReadKeysLines(std::vector<std::string>* out) {
    if (g_keysPath.empty()) return false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_keysPath.c_str(), L"r") != 0 || !f) return false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        out->push_back(s);
    }
    fclose(f);
    return true;
}

// ===== 预设增援（游戏内按 J）=====
// 与修改器共用 ra3copilot_preset.ini（DLL 同目录），格式见 actions.cpp 的 SquadLoadPreset：
//   hash=<对象类型哈希>   count=<1..64>   name=<对象模板名>
uint32_t    g_squadHash  = 0x6586A5A0u;
int         g_squadCount = 8;
std::string g_squadId    = "JapanCommandoTech1";

const ra3::SupportUnit* FindUnitByHash(uint32_t hash) {
    for (int i = 0; i < ra3::kSupportUnitCount; ++i)
        if (ra3::kSupportUnits[i].hash == hash) return &ra3::kSupportUnits[i];
    for (int i = 0; i < ra3::kCrateCount; ++i)
        if (ra3::kCrates[i].hash == hash) return &ra3::kCrates[i];
    return nullptr;
}

void LoadPreset() {
    g_squadHash = 0x6586A5A0u;
    g_squadCount = 8;
    g_squadId = "JapanCommandoTech1";
    if (g_presetPath.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_presetPath.c_str(), L"r") != 0 || !f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == ';') continue;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char* v = eq + 1;
        while (*v == ' ' || *v == '\t') ++v;
        for (char* q = v; *q; ++q) if (*q == '\r' || *q == '\n') { *q = '\0'; break; }
        if (_stricmp(line, "hash") == 0) {
            const unsigned long h = strtoul(v, nullptr, 0);
            if (h) g_squadHash = static_cast<uint32_t>(h);
        } else if (_stricmp(line, "count") == 0) {
            const int c = atoi(v);
            if (c >= 1 && c <= 64) g_squadCount = c;
        } else if (_stricmp(line, "name") == 0) {
            if (*v) g_squadId = v;
        }
    }
    fclose(f);
}

void SavePreset() {
    if (g_presetPath.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_presetPath.c_str(), L"w") != 0 || !f) {
        AppendLog(L"预设增援写入失败（运行目录不可写）", LOG_ERR);
        return;
    }
    fprintf(f, "# 预设增援：游戏内按 J 在鼠标位置召唤这些单位；改完存盘即生效，不用重注入\n");
    fprintf(f, "hash=0x%08X\n", g_squadHash);
    fprintf(f, "count=%d\n", g_squadCount);
    fprintf(f, "name=%s\n", g_squadId.c_str());
    fclose(f);
}

// 单位显示名：优先中文名，表里没有就用模板名
std::wstring UnitLabel(uint32_t hash, const std::string& id) {
    const ra3::SupportUnit* u = FindUnitByHash(hash);
    return FromUtf8(u ? std::string(u->name) : id);
}

// 搜索命中：中文名 / 模板名 / 拼音都能搜，忽略大小写
bool UnitHit(const ra3::SupportUnit& u, const char* q) {
    if (!q[0]) return true;
    std::string hay = std::string(u.name) + " " + u.id + " " + u.py;
    std::string needle(q);
    auto lower = [](unsigned char c) { return static_cast<char>(tolower(c)); };
    std::transform(hay.begin(), hay.end(), hay.begin(), lower);
    std::transform(needle.begin(), needle.end(), needle.begin(), lower);
    return hay.find(needle) != std::string::npos;
}

void LoadKeyTable() {
    g_toggleVk = VK_INSERT;
    g_toggleCtrl = false;
    g_hkEnabled = true;
    g_keyRows.clear();

    std::vector<std::string> lines;
    if (ReadKeysLines(&lines)) {
        for (const std::string& ln : lines) {
            if (ln.empty() || ln[0] == '#') continue;
            if (ln.compare(0, 8, "enabled=") == 0) { g_hkEnabled = (ln[8] == '1'); continue; }
            if (ln.compare(0, 7, "toggle=") == 0) {
                int vk = 0, ctrl = 0;
                if (sscanf_s(ln.c_str() + 7, "%d %d", &vk, &ctrl) == 2 && vk > 0 && vk < 0x100) {
                    g_toggleVk = vk;
                    g_toggleCtrl = (ctrl != 0);
                }
                continue;
            }
            int id = 0, vk = 0, ctrl = 0;
            if (sscanf_s(ln.c_str(), "%d %d %d", &id, &vk, &ctrl) != 3) continue;
            if (vk <= 0 || vk >= 0x100) continue;
            // 第三个字段之后就是说明（老版本写的 ini 没有这一段）
            int fields = 0;
            const char* q = ln.c_str();
            while (*q && fields < 3) {
                while (*q == ' ') ++q;
                if (!*q) break;
                while (*q && *q != ' ') ++q;
                ++fields;
            }
            KeyRow r;
            r.id = id; r.vk = vk; r.ctrl = (ctrl != 0);
            if (*q) r.desc = std::string(q + 1);
            g_keyRows.push_back(r);
        }
    }
    if (g_keyRows.empty()) {
        for (const KeyDef& d : kKeyDefault) {
            KeyRow r; r.id = d.id; r.vk = d.vk; r.ctrl = d.ctrl; r.desc = d.desc;
            g_keyRows.push_back(r);
        }
    }
    for (KeyRow& r : g_keyRows) {
        if (!r.desc.empty()) continue;
        for (const KeyDef& d : kKeyDefault)
            if (d.id == r.id) { r.desc = d.desc; break; }
    }
    std::sort(g_keyRows.begin(), g_keyRows.end(),
              [](const KeyRow& a, const KeyRow& b) { return a.id < b.id; });
}

void SaveKeyTable() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_keysPath.c_str(), L"w") != 0 || !f) {
        AppendLog(L"快捷键写入失败（运行目录不可写）", LOG_ERR);
        return;
    }
    fprintf(f, "enabled=%d\n", g_hkEnabled ? 1 : 0);
    fprintf(f, "toggle=%d %d\n", g_toggleVk, g_toggleCtrl ? 1 : 0);
    for (const KeyRow& r : g_keyRows)
        fprintf(f, "%d %d %d %s\n", r.id, r.vk, r.ctrl ? 1 : 0, r.desc.c_str());
    fclose(f);
}

// 与已有绑定撞车时返回那条绑定的名字；没有冲突返回空串。
// target：-1 = 呼出键，>0 = 功能热键的 id
std::wstring KeyConflict(int target, int vk, bool ctrl) {
    if (target != -1 && g_toggleVk == vk && g_toggleCtrl == ctrl) return L"呼出菜单";
    for (const KeyRow& r : g_keyRows) {
        if (r.id == target) continue;
        if (r.vk == vk && r.ctrl == ctrl) return FromUtf8(r.desc);
    }
    return L"";
}

bool ApplyKey(int target, int vk, bool ctrl) {
    if (target == -1) {
        g_toggleVk = vk;
        g_toggleCtrl = ctrl;
    } else {
        bool found = false;
        for (KeyRow& r : g_keyRows)
            if (r.id == target) { r.vk = vk; r.ctrl = ctrl; found = true; break; }
        if (!found) return false;
    }
    SaveKeyTable();
    return true;
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
    g_keysPath = Join(g_runtimeDir, L"ra3copilot_keys.ini");
    g_presetPath = Join(g_runtimeDir, L"ra3copilot_preset.ini");
    EnsureDir(g_runtimeDir);
    EnsureDir(Join(g_runtimeDir, L"mcp"));
    if (!DirExists(g_runtimeDir)) return false;
    LoadConfig();

    const std::wstring oldKeys = Join(g_exeDir, L"ra3copilot_keys.ini");
    const std::wstring newKeys = Join(g_runtimeDir, L"ra3copilot_keys.ini");
    if (FileExists(oldKeys) && !FileExists(newKeys)) CopyFileW(oldKeys.c_str(), newKeys.c_str(), TRUE);
    LoadKeyTable();
    LoadPreset();
    if (!FileExists(g_presetPath)) SavePreset();   // 第一次运行落一份默认值，方便对着改

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
    AppendLog(L"MCP 配置已复制到剪贴板（粘贴到客户端的 mcpServers；需要本机 Python 3）", LOG_OK);
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
        if (g_loaded) AppendLog(g_http ? L"修改器已注入，接口已应答" : L"修改器已注入", LOG_OK);
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
    else if (g_loaded) status = L"修改器已注入";
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

const ImVec4 kAccent(0.35f, 0.85f, 0.95f, 1.f);
const ImVec4 kWarn(0.95f, 0.72f, 0.30f, 1.f);
const ImVec4 kDim(0.56f, 0.61f, 0.66f, 1.f);      // 弱化说明
const ImVec4 kDesc(0.82f, 0.86f, 0.90f, 1.f);     // 键位说明正文

// 主界面：状态 + 动作 + 运行记录
void DrawMainPage() {
    const float s = g_scale;
    DrawStatusCard();
    ImGui::Separator();

    const bool canAct = g_pid && !g_busy;
    const char* primary = g_loaded ? "卸载" : "注入";
    ImGui::BeginDisabled(!g_pid || g_busy);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.42f, 0.52f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.55f, 0.66f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.34f, 0.42f, 1.f));
    if (ImGui::Button(primary, ImVec2(104 * s, 0))) OnPrimary();
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(g_loaded ? "从游戏进程里卸下修改器，文件锁随之释放"
                                   : "把修改器注入当前运行的游戏");

    ImGui::SameLine(0, 6 * s);
    ImGui::BeginDisabled(!canAct);
    if (ImGui::Button("重载", ImVec2(68 * s, 0))) OnReload();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("卸载后重新注入当前版本\n更新 Ra3Copilot.exe 后用这个");

    ImGui::SameLine(0, 6 * s);
    ImGui::BeginDisabled(g_busy);
    if (ImGui::Button(g_pid ? "游戏" : "启动", ImVec2(68 * s, 0))) {
        if (g_pid) ShowGameWindow();
        else LaunchGame();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(g_pid ? "把游戏窗口切到前台" : "启动日冕启动器；找不到时手动选择");

    ImGui::SameLine(0, 6 * s);
    if (ImGui::Button("设置", ImVec2(68 * s, 0))) {
        g_page = 1;
        g_keysTop = true;
        g_captureTarget = 0;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("运行目录 / MCP 配置 / 快捷键");

    if (ImGui::Checkbox("自动注入", &g_autoInject)) SaveConfig();
    ImGui::SameLine(0, 18 * s);
    if (ImGui::Checkbox("注入后最小化", &g_minimizeAfter)) SaveConfig();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("注入完成后把启动器最小化，不挡游戏");

    ImGui::Separator();
    DrawLogPanel();
}

// 键名按钮：捕获中换成提示并高亮 —— 这一页唯一的交互就是它，得一眼认得出
bool DrawKeyCell(int target, int vk, bool ctrl, float w) {
    if (g_captureTarget == target) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.48f, 0.33f, 0.10f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.48f, 0.33f, 0.10f, 1.f));
        ImGui::Button("按新键…", ImVec2(w, 0));
        ImGui::PopStyleColor(2);
        return false;
    }
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.26f, 0.32f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.37f, 0.45f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.14f, 0.22f, 0.28f, 1.f));
    const bool hit = ImGui::Button(ToUtf8(KeyLabelW(vk, ctrl)).c_str(), ImVec2(w, 0));
    ImGui::PopStyleColor(3);
    return hit;
}

// 与按钮同行的文字：顶对齐会看起来贴在行上沿，用 AlignTextToFramePadding 压到中线
void TextOnRow(const char* text, const ImVec4& col) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

// ===== 设置页 =====
// 两页签：常规（运行目录 / 预设增援）、快捷键（呼出键 / 静默 / 全部键位）
bool g_pickOpen = false;              // 下一帧打开单位选择弹窗
char g_pickQuery[128] = { 0 };        // 选择弹窗的搜索词

void DrawUnitPicker() {
    const float s = g_scale;
    if (g_pickOpen) {
        ImGui::OpenPopup("##pickunit");
        g_pickOpen = false;
    }
    ImGui::SetNextWindowSize(ImVec2(560 * s, 470 * s), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("##pickunit", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;

    ImGui::TextColored(kAccent, "选择预设增援单位");
    ImGui::TextDisabled("中文名、模板名、拼音都能搜（例如 jdc = 基地车）；点一行即选中");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##q", "搜索…", g_pickQuery, sizeof(g_pickQuery));
    ImGui::Spacing();

    const float footer = ImGui::GetFrameHeightWithSpacing() + 6 * s;
    int shown = 0;
    ImGui::BeginChild("##unitlist", ImVec2(0, -footer), true);
    auto row = [&](const ra3::SupportUnit& u) {
        if (!UnitHit(u, g_pickQuery)) return;
        ++shown;
        ImGui::PushID(static_cast<int>(u.hash));
        if (ImGui::Selectable(u.name, u.hash == g_squadHash, 0, ImVec2(200 * s, 0))) {
            g_squadHash = u.hash;
            g_squadId = u.id;
            SavePreset();
            AppendLog(L"预设增援 → " + FromUtf8(std::string(u.name)) + L"（" +
                      FromUtf8(std::string(u.id)) + L"）×" + std::to_wstring(g_squadCount), LOG_OK);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n哈希 0x%08X · %s · %s", u.id, u.hash, u.type, u.faction);
        ImGui::SameLine(0, 10 * s);
        ImGui::TextDisabled("%s · %s", u.id, u.type);
        ImGui::PopID();
    };
    for (int i = 0; i < ra3::kSupportUnitCount; ++i) row(ra3::kSupportUnits[i]);
    for (int i = 0; i < ra3::kCrateCount; ++i) row(ra3::kCrates[i]);
    ImGui::EndChild();

    ImGui::TextDisabled("共 %d 个匹配", shown);
    ImGui::SameLine(0, 12 * s);
    if (ImGui::Button("取消", ImVec2(80 * s, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// 常规页：运行目录 + 预设增援
void DrawGeneralTab() {
    const float s = g_scale;

    ImGui::TextColored(kAccent, "运行目录");
    ImGui::SameLine(0, 10 * s);
    TextOnRow("配置、日志、MCP 脚本都在这里", kDim);
    ImGui::TextWrapped("%s", ToUtf8(g_runtimeDir).c_str());
    if (ImGui::Button("打开目录", ImVec2(104 * s, 0))) OnFolder();
    ImGui::SameLine(0, 6 * s);
    if (ImGui::Button("复制 MCP 配置", ImVec2(120 * s, 0))) OnMcp();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("把 MCP 客户端配置复制到剪贴板\n需要本机有 Python 3");

    ImGui::Separator();
    ImGui::TextColored(kAccent, "预设增援");
    ImGui::SameLine(0, 10 * s);
    TextOnRow("游戏内按 J，在鼠标位置召唤下面这个单位", kDim);
    ImGui::Spacing();

    const ra3::SupportUnit* u = FindUnitByHash(g_squadHash);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("单位");
    ImGui::SameLine(0, 16 * s);
    const std::wstring label = UnitLabel(g_squadHash, g_squadId) + L"  ...";
    if (ImGui::Button(ToUtf8(label).c_str(), ImVec2(190 * s, 0))) {
        g_pickOpen = true;
        g_pickQuery[0] = 0;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("点击选择要召唤的单位");
    ImGui::Indent(56 * s);
    if (u) ImGui::TextDisabled("%s · %s · %s · 0x%08X", u->id, u->faction, u->type, u->hash);
    else   ImGui::TextDisabled("%s · 0x%08X（不在单位表里）", g_squadId.c_str(), g_squadHash);
    ImGui::Unindent(56 * s);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("数量");
    ImGui::SameLine(0, 16 * s);
    int n = g_squadCount;
    ImGui::SetNextItemWidth(120 * s);
    if (ImGui::InputInt("##count", &n, 1, 8)) {
        if (n < 1) n = 1;
        if (n > 64) n = 64;
        if (n != g_squadCount) {
            g_squadCount = n;
            SavePreset();
        }
    }
    ImGui::SameLine(0, 12 * s);
    ImGui::TextDisabled("1 – 64；召唤出来归你自己");
}

// 快捷键页：呼出键 / 静默 / 全部键位（表格两列）
void DrawKeysTab() {
    const float s = g_scale;

    ImGui::TextColored(kAccent, "快捷键");
    ImGui::SameLine(0, 10 * s);
    TextOnRow("点键名改键（Esc 取消）；改动立即存盘，游戏中 1 秒内生效", kDim);
    ImGui::Spacing();

    // 呼出键：不受静默影响，单独一行放在列表上方
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("呼出菜单");
    ImGui::SameLine(0, 10 * s);
    if (DrawKeyCell(-1, g_toggleVk, g_toggleCtrl, 116 * s)) {
        g_captureTarget = -1;
        g_captureSince = GetTickCount64();
        AppendLog(L"请按下呼出菜单的新按键（Esc 取消）");
    }
    ImGui::SameLine(0, 12 * s);
    TextOnRow("不受静默开关影响", kDim);

    bool silent = !g_hkEnabled;              // 默认不勾选 = 功能键可用
    if (ImGui::Checkbox("静默功能快捷键", &silent)) {
        g_hkEnabled = !silent;
        SaveKeyTable();
        AppendLog(g_hkEnabled ? L"功能快捷键已启用" : L"功能快捷键已静默：只保留呼出菜单", LOG_OK);
    }
    ImGui::SameLine(0, 10 * s);
    TextOnRow("静默后按功能键不触发任何动作", kDim);

    // 状态行常驻，避免捕获时整块内容上下跳
    if (g_captureTarget != 0) ImGui::TextColored(kWarn, "正在等待按键…（Esc 取消）");
    else                      ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));

    // 键位列表：表格四列（键名·说明·键名·说明）。行高由按钮决定，说明与按钮垂直居中，
    // 两列共用一个滚动容器（分开滚会左右错位）。
    const int n = static_cast<int>(g_keyRows.size());
    const int half = (n + 1) / 2;
    const float rest = ImGui::GetContentRegionAvail().y;
    const float btnW = 100 * s;
    const float cellY = 4 * s;
    const float rowH = ImGui::GetFrameHeight() + cellY * 2;
    // 高度取整行，否则滚动区底部总是把最后一行切一半
    int rows = static_cast<int>((rest - 6 * s) / rowH);
    if (rows < 3) rows = 3;
    if (rows > half) rows = half;
    const float listH = rows * rowH + 6 * s;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(10 * s, cellY));
    if (g_keysTop) {                      // 每次进设置页从头看，别停在上一轮的滚动位置
        ImGui::SetNextWindowScroll(ImVec2(0.f, 0.f));
        g_keysTop = false;
    }
    ImGui::BeginChild("##keylist", ImVec2(0, listH), true);
    if (ImGui::BeginTable("##keys", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("##k1", ImGuiTableColumnFlags_WidthFixed, btnW);
        ImGui::TableSetupColumn("##d1", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("##k2", ImGuiTableColumnFlags_WidthFixed, btnW);
        ImGui::TableSetupColumn("##d2", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        for (int i = 0; i < half; ++i) {
            ImGui::TableNextRow();
            // 正在等待按键的那一行整行点亮，滚到哪都看得见
            const bool capL = (i < n) && (g_keyRows[i].id == g_captureTarget);
            const bool capR = (i + half < n) && (g_keyRows[i + half].id == g_captureTarget);
            if (g_captureTarget != 0 && (capL || capR))
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(110, 74, 22, 160));
            for (int col = 0; col < 2; ++col) {
                const int idx = i + col * half;
                if (idx >= n) continue;
                const KeyRow& r = g_keyRows[idx];
                ImGui::TableSetColumnIndex(col * 2);
                ImGui::PushID(r.id);
                if (DrawKeyCell(r.id, r.vk, r.ctrl, btnW)) {
                    g_captureTarget = r.id;
                    g_captureSince = GetTickCount64();
                    AppendLog(L"请按下「" + FromUtf8(r.desc) + L"」的新按键（Esc 取消）");
                }
                ImGui::PopID();
                ImGui::TableSetColumnIndex(col * 2 + 1);
                TextOnRow(r.desc.c_str(), kDesc);
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void DrawSettingsPage() {
    const float s = g_scale;
    if (ImGui::Button("< 返回", ImVec2(84 * s, 0))) {
        g_page = 0;
        g_captureTarget = 0;                 // 离开这一页就别留着捕获
    }
    ImGui::SameLine(0, 10 * s);
    ImGui::TextDisabled("设置");
    ImGui::Separator();

    if (ImGui::BeginTabBar("##settab")) {
        if (ImGui::BeginTabItem("常规")) {
            DrawGeneralTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("快捷键")) {
            DrawKeysTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    DrawUnitPicker();
}

void BuildUi() {
    ImGuiIO& io = ImGui::GetIO();
    const float s = g_scale;

    // 捕获超时兜底：点了按钮又不按，之后的第一个按键（可能完全无关）就会改掉绑定
    if (g_captureTarget != 0 && GetTickCount64() - g_captureSince > 15000) {
        g_captureTarget = 0;
        AppendLog(L"改键超时，已取消");
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(9 * s, 8 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // 顶栏两页共用
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::TextUnformatted("Ra3-Copilot");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 8 * s);
    ImGui::TextDisabled("v%s", ToUtf8(kVersion).c_str());
    ImGui::TextDisabled("红色警戒 3 · 日冕 Mod 局内修改器");
    ImGui::Separator();

    if (g_page == 1) DrawSettingsPage();
    else DrawMainPage();

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
    // 改键捕获要抢在 ImGui 前面：否则按键先被 ImGui 的输入处理吃掉
    if (g_captureTarget != 0) {
        // 切走就作废：否则回来随手按个键（哪怕按在别的窗口里）就把绑定改掉了
        if (msg == WM_KILLFOCUS) {
            g_captureTarget = 0;
            AppendLog(L"改键已取消（窗口失去焦点）");
            return 0;
        }
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
            const int vk = static_cast<int>(w);
            if (vk == VK_ESCAPE) {
                g_captureTarget = 0;
                AppendLog(L"已取消改键");
                return 0;
            }
            if (vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU) {   // 修饰键不当主键
                const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                const int target = g_captureTarget;
                const std::wstring dup = KeyConflict(target, vk, ctrl);
                if (!dup.empty()) {
                    AppendLog(L"该键已被「" + dup + L"」占用，请换一个（Esc 取消）", LOG_WARN);
                } else {
                    const std::wstring label = KeyLabelW(vk, ctrl);
                    const std::wstring name = (target == -1) ? std::wstring(L"呼出菜单") : FromUtf8(
                        [&] { for (const KeyRow& r : g_keyRows) if (r.id == target) return r.desc; return std::string(); }());
                    if (ApplyKey(target, vk, ctrl)) {
                        g_captureTarget = 0;
                        AppendLog(name + L" → " + label + L"；游戏中 1 秒内生效", LOG_OK);
                    } else {
                        g_captureTarget = 0;
                        AppendLog(L"改键失败：找不到对应的快捷键条目", LOG_ERR);
                    }
                }
            }
            return 0;
        }
        if (msg == WM_KEYUP || msg == WM_SYSKEYUP || msg == WM_CHAR || msg == WM_SYSCHAR) return 0;
    }

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

    RECT wr{ 0, 0, S(560), S(600) };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);
    HWND h = CreateWindowExW(0, kWindowClass, L"Ra3-Copilot · 修改器启动器",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             wr.right - wr.left, wr.bottom - wr.top,
                             nullptr, nullptr, inst, nullptr);
    if (!h) return 1;
    g_wnd = h;

    InitDpi(h);
    if (g_scale > 1.0f) {
        RECT sr{ 0, 0, S(560), S(600) };
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
