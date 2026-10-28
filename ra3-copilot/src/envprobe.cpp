// envprobe.cpp - 环境/版本采集（GET /env）
//
// 为什么需要它：构建判定只认 PE 头的 SizeOfImage（零售 0x93B000 / Steam 0x950000），
// 但同一个「Steam 1.12」在不同机器上可以走完全不同的呈现路径 —— 游戏目录放了 d3d9.dll 代理、
// DXVK / dgVoodoo / ReShade 注入、Steam 叠加层改动 vtable、显卡驱动前缀不同，都会让
// Present / PresentEx / SwapChain::Present 的调用情况不一样。这些差异必须采出来才能定位。
//
// 全部只读、不碰引擎状态，可在 HTTP 线程直接调。
#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include "envprobe.h"

#pragma comment(lib, "version.lib")

namespace ra3 {
namespace {

struct Buf {
    char*  p;
    int    n;
    size_t w;
};

void Add(Buf* b, const char* fmt, ...) {
    if (!b || !b->p || b->w + 2 >= (size_t)b->n) return;
    va_list ap;
    va_start(ap, fmt);
    const int r = _vsnprintf_s(b->p + b->w, (size_t)b->n - b->w, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (r > 0) b->w += (size_t)r;
}

void ToUtf8(const wchar_t* src, char* out, int n) {
    out[0] = 0;
    if (src && n > 0) WideCharToMultiByte(CP_UTF8, 0, src, -1, out, n, nullptr, nullptr);
}

bool ContainsNoCase(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return false;
    const size_t nl = strlen(needle);
    for (const char* p = hay; *p; ++p)
        if (_strnicmp(p, needle, nl) == 0) return true;
    return false;
}

bool StartsWithNoCase(const char* s, const char* pre) {
    return s && pre && _strnicmp(s, pre, strlen(pre)) == 0;
}

// 文件版本四段（"10.0.19041.3636"）
void FileVersion(const wchar_t* path, char* out, int n) {
    out[0] = 0;
    DWORD dummy = 0;
    const DWORD sz = GetFileVersionInfoSizeW(path, &dummy);
    if (!sz) return;
    BYTE* buf = static_cast<BYTE*>(HeapAlloc(GetProcessHeap(), 0, sz));
    if (!buf) return;
    if (GetFileVersionInfoW(path, 0, sz, buf)) {
        VS_FIXEDFILEINFO* fi = nullptr;
        UINT len = 0;
        if (VerQueryValueW(buf, L"\\", reinterpret_cast<void**>(&fi), &len) && fi) {
            _snprintf_s(out, (size_t)n, _TRUNCATE, "%u.%u.%u.%u",
                        HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                        HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
        }
    }
    HeapFree(GetProcessHeap(), 0, buf);
}

// 关注关键词：呈现路径 / 图形 API 转换层 / 叠加层 / 驱动前缀
const char* kWatched[] = {
    "d3d9", "d3d8", "dxgi", "d3d11", "d3d12", "vulkan", "dxvk", "dgvoodoo", "reshade",
    "gameoverlay", "steam", "nvoglv", "atiumd", "atioglxx", "igdpix", "ig4icd", "nvapi",
    "amdxx", "opengl32",
};

void DumpExeAndPe(Buf* b) {
    wchar_t pathW[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, pathW, MAX_PATH);
    char path[MAX_PATH * 3] = { 0 };
    ToUtf8(pathW, path, sizeof(path));
    Add(b, "exe=%s\n", path);

    const BYTE* base = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
    __try {
        const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const IMAGE_NT_HEADERS32* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        char ts[64] = "?";
        const __time32_t t = static_cast<__time32_t>(nt->FileHeader.TimeDateStamp);
        struct tm tmv;
        if (_gmtime32_s(&tmv, &t) == 0) {
            _snprintf_s(ts, sizeof(ts), _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02dZ",
                        tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                        tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        }
        Add(b, "pe.sizeofimage=0x%X pe.timestamp=0x%08X (%s)\n",
            (unsigned)nt->OptionalHeader.SizeOfImage,
            (unsigned)nt->FileHeader.TimeDateStamp, ts);
        Add(b, "pe.entry=0x%X pe.checksum=0x%X pe.imagebase=0x%X sections=%u subsystem=%u\n",
            (unsigned)nt->OptionalHeader.AddressOfEntryPoint,
            (unsigned)nt->OptionalHeader.CheckSum,
            (unsigned)nt->OptionalHeader.ImageBase,
            (unsigned)nt->FileHeader.NumberOfSections,
            (unsigned)nt->OptionalHeader.Subsystem);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Add(b, "pe=unreadable\n");
    }

    char ver[48] = { 0 };
    FileVersion(pathW, ver, sizeof(ver));
    Add(b, "exe.version=%s\n", ver[0] ? ver : "?");

    // 游戏目录自带 d3d9.dll = 代理（dgVoodoo / DXVK / 旧 wrapper），呈现路径会被整体替换
    wchar_t dirW[MAX_PATH] = { 0 };
    wcscpy_s(dirW, pathW);
    wchar_t* slash = wcsrchr(dirW, L'\\');
    if (slash) *slash = 0;

    const wchar_t* probes[] = { L"d3d9.dll", L"d3d8.dll", L"dxgi.dll", L"d3d11.dll",
                                L"dgVoodoo.conf", L"dxvk.conf", L"ReShade.ini",
                                L"ReShade64.dll", L"d3d9.ini" };
    Add(b, "== 游戏目录自带的代理/叠加层 ==\n");
    int found = 0;
    for (int i = 0; i < static_cast<int>(sizeof(probes) / sizeof(probes[0])); ++i) {
        wchar_t p[MAX_PATH] = { 0 };
        swprintf_s(p, L"%s\\%s", dirW, probes[i]);
        const DWORD attr = GetFileAttributesW(p);
        if (attr == INVALID_FILE_ATTRIBUTES) continue;
        char p8[MAX_PATH * 3] = { 0 }, v8[48] = { 0 };
        ToUtf8(p, p8, sizeof(p8));
        FileVersion(p, v8, sizeof(v8));
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        const unsigned sz = GetFileAttributesExW(p, GetFileExInfoStandard, &fad)
                                ? (unsigned)fad.nFileSizeLow : 0u;
        Add(b, "%s ver=%s size=%u\n", p8, v8[0] ? v8 : "?", sz);
        ++found;
    }
    if (!found) Add(b, "(none)\n");
}

void DumpModules(Buf* b) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        Add(b, "modules=snapshot failed\n");
        return;
    }
    MODULEENTRY32W me = { sizeof(me) };

    Add(b, "== 关注模块（呈现/转换层/叠加层/驱动）==\n");
    int watched = 0;
    if (Module32FirstW(snap, &me)) {
        do {
            char name[MAX_PATH * 3] = { 0 }, path[MAX_PATH * 3] = { 0 };
            ToUtf8(me.szModule, name, sizeof(name));
            ToUtf8(me.szExePath, path, sizeof(path));
            const char* hit = nullptr;
            for (int i = 0; i < static_cast<int>(sizeof(kWatched) / sizeof(kWatched[0])); ++i) {
                if (ContainsNoCase(name, kWatched[i])) { hit = kWatched[i]; break; }
            }
            if (!hit) continue;
            char ver[48] = { 0 };
            FileVersion(me.szExePath, ver, sizeof(ver));
            Add(b, "%s base=%08X size=%X ver=%s path=%s\n", name,
                static_cast<unsigned>(reinterpret_cast<uintptr_t>(me.modBaseAddr)),
                (unsigned)me.modBaseSize, ver[0] ? ver : "?", path);
            ++watched;
        } while (Module32NextW(snap, &me));
    }
    if (!watched) Add(b, "(none)\n");

    Add(b, "== 非系统模块（最多 40，含我们自己的 DLL）==\n");
    int foreign = 0;
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            char path[MAX_PATH * 3] = { 0 }, name[MAX_PATH * 3] = { 0 };
            ToUtf8(me.szExePath, path, sizeof(path));
            if (StartsWithNoCase(path, "C:\\Windows\\")) continue;
            ToUtf8(me.szModule, name, sizeof(name));
            Add(b, "%s base=%08X size=%X path=%s\n", name,
                static_cast<unsigned>(reinterpret_cast<uintptr_t>(me.modBaseAddr)),
                (unsigned)me.modBaseSize, path);
            ++foreign;
            if (foreign >= 40) { Add(b, "...(truncated)\n"); break; }
        } while (Module32NextW(snap, &me));
    }
    if (!foreign) Add(b, "(none)\n");
    CloseHandle(snap);
}

void DumpSystem(Buf* b) {
    // 版本：GetVersionEx 受 manifest 影响，直接问 ntdll
    typedef LONG (WINAPI* RtlGetVersion_t)(PRTL_OSVERSIONINFOW);
    char os[96] = "?";
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersion_t rtl = nt ? reinterpret_cast<RtlGetVersion_t>(
                                   reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")))
                             : nullptr;
    if (rtl) {
        RTL_OSVERSIONINFOW vi = {};
        vi.dwOSVersionInfoSize = sizeof(vi);
        if (rtl(&vi) == 0) {
            _snprintf_s(os, sizeof(os), _TRUNCATE, "%u.%u build %u",
                        (unsigned)vi.dwMajorVersion, (unsigned)vi.dwMinorVersion,
                        (unsigned)vi.dwBuildNumber);
        }
    }
    Add(b, "== 系统 ==\n");
    Add(b, "os=%s\n", os);

    DEVMODEW dm = { sizeof(dm) };
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
        Add(b, "display=%ux%u %ubpp %uHz\n", (unsigned)dm.dmPelsWidth, (unsigned)dm.dmPelsHeight,
            (unsigned)dm.dmBitsPerPel, (unsigned)dm.dmDisplayFrequency);
    }
    wchar_t cwdW[MAX_PATH] = { 0 };
    char cwd[MAX_PATH * 3] = { 0 };
    GetCurrentDirectoryW(MAX_PATH, cwdW);
    ToUtf8(cwdW, cwd, sizeof(cwd));
    Add(b, "cwd=%s\n", cwd);
    Add(b, "cmdline=%s\n", GetCommandLineA());
}

} // namespace

void EnvProbeText(char* out, int n) {
    if (!out || n <= 0) return;
    Buf b = { out, n, 0 };
    out[0] = 0;
    DumpExeAndPe(&b);
    DumpModules(&b);
    DumpSystem(&b);
}

} // namespace ra3
