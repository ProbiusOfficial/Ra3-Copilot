// diag.cpp - Ra3-Copilot 远程诊断包生成器（x86 独立 exe，不依赖注入）
//
// 用法：
//   ra3diag.exe            生成诊断包，与自身同目录 ra3diag_yyyymmdd_hhmmss.txt
//   ra3diag.exe <目录>     指定输出目录
//
// 目的：只在别人机器上出问题时，把「现场」原样搬回来 —— 同样的哈希、同样的候选、
// 同样的原始字节，本地就能离线判断是脚本没写过（候选 0 个）还是节点识别挑错了对象
// （候选一堆、挑中的那个不在树里）。
//
// 采集内容：
//   1 环境（是否管理员 / OS / 时间）
//   2 游戏进程与 exe 指纹（大小 / 时间戳 / PE 时间戳 / SizeOfImage / 校验和）
//   3 加载模块（ra3copilot.dll 在不在、哪一份、多大；其它非系统模块 = Mod 线索）
//   4 修改器接口原文（/ping /state /objectives /flagdbg /enemy /log /scripttbl）
//   5 关卡脚本解析（读 .big 里的 .map 生成目标表，不依赖修改器是否注入）
//   5 独立内存扫描：当前地图每个脚本依赖的**全部**候选 + 命中点原始字节
//
// 只读：OpenProcess(PROCESS_VM_READ)，不写目标进程、不改文件。
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <ctime>
#include <cstdint>

#include "generated/objectives_data.h"
#include "maptbl.h"
#include "big4.h"

static const wchar_t* GAME_PROC = L"ra3_1.12.game";
static const wchar_t* DLL_NAME  = L"ra3copilot.dll";
static const int      HTTP_PORT = 17832;
static const char*    DIAG_VER  = "1";

// TheGameLogic 全局指针：VA 0x00CD8CE4（ImageBase 0x400000，无 ASLR），+0x98 是地图路径 C 串
static const DWORD    GL_VA     = 0x00CD8CE4u;
static const DWORD    IMAGE_BASE = 0x00400000u;

// ------------------------------------------------------------------ 通用

static DWORD FindGamePid() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, GAME_PROC) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static bool IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION el{};
    DWORD n = 0;
    GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &n);
    CloseHandle(tok);
    return el.TokenIsElevated != 0;
}

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
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) return false;
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return true;
}

static bool Rd(HANDLE h, uintptr_t addr, void* buf, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(h, (LPCVOID)addr, buf, n, &got) && got == n;
}

static uint32_t Fnv1(const char* s) {
    uint32_t h = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p)
        h = (h * 16777619u) ^ (uint32_t)*p;
    return h;
}

static void DumpHex(FILE* f, const BYTE* b, int n, uintptr_t base) {
    for (int i = 0; i < n; i += 16) {
        fprintf(f, "      %08X  ", (unsigned)(base + (uintptr_t)i));
        for (int j = 0; j < 16; ++j)
            fprintf(f, "%02X ", (i + j < n) ? b[i + j] : 0);
        fputc(' ', f);
        fputc('|', f);
        for (int j = 0; j < 16 && i + j < n; ++j) {
            const BYTE c = b[i + j];
            fputc((c >= 32 && c < 127) ? (int)c : '.', f);
        }
        fputs("|\n", f);
    }
}

// ------------------------------------------------------------------ HTTP

static char g_httpBuf[160 * 1024];

// 返回响应体长度；-1 = 连不上
static int HttpGet(const char* path, char* out, int n) {
    if (!out || n <= 0) return -1;
    out[0] = 0;
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return -1;
    int body = -1;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s != INVALID_SOCKET) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((u_short)HTTP_PORT);
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (connect(s, (sockaddr*)&a, sizeof(a)) == 0) {
            char req[512];
            _snprintf_s(req, sizeof(req), _TRUNCATE,
                        "GET %s HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", path);
            send(s, req, (int)strlen(req), 0);
            DWORD tmo = 30000;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
            int total = 0;
            for (;;) {
                if (total >= (int)sizeof(g_httpBuf) - 1) break;
                const int k = recv(s, g_httpBuf + total, (int)sizeof(g_httpBuf) - 1 - total, 0);
                if (k <= 0) break;
                total += k;
                g_httpBuf[total] = 0;
                // Content-Length 到齐就停（HTTP/1.0 一般靠对端关闭，这里只是提前收工）
                const char* cl = strstr(g_httpBuf, "Content-Length:");
                if (cl) {
                    const int want = atoi(cl + 15);
                    const char* hdrEnd = strstr(g_httpBuf, "\r\n\r\n");
                    if (hdrEnd && want > 0 && total >= (int)(hdrEnd + 4 - g_httpBuf) + want) break;
                }
            }
            if (total > 0) {
                const char* hdrEnd = strstr(g_httpBuf, "\r\n\r\n");
                if (hdrEnd) {
                    const char* b = hdrEnd + 4;
                    int len = total - (int)(b - g_httpBuf);
                    if (len > n - 1) len = n - 1;
                    memcpy(out, b, (size_t)len);
                    out[len] = 0;
                    body = len;
                }
            }
        }
        closesocket(s);
    }
    WSACleanup();
    return body;
}

// ------------------------------------------------------------------ 依赖表

struct Dep { uint32_t hash; char kind; char name[96]; };
static Dep g_dep[512];
static int g_ndep = 0;

static void AddDep(char kind, const char* s, int len) {
    if (len <= 0 || len >= 96) return;
    char nm[96];
    memcpy(nm, s, (size_t)len);
    nm[len] = 0;
    const uint32_t h = Fnv1(nm);
    for (int i = 0; i < g_ndep; ++i) if (g_dep[i].hash == h) return;
    if (g_ndep >= 512) return;
    g_dep[g_ndep].hash = h;
    g_dep[g_ndep].kind = kind;
    memcpy(g_dep[g_ndep].name, nm, (size_t)len + 1);
    ++g_ndep;
}

// 内置目标表按 "O|目标名|other|依赖..." 存，依赖前缀 F/N/T；这里只取依赖名
static void LoadDeps() {
    for (int i = 0; kObjTableData[i]; ++i) {
        const char* line = kObjTableData[i];
        if (line[0] != 'O' || line[1] != '|') continue;
        int seg = 0;
        const char* segStart = line;
        for (const char* p = line;; ++p) {
            if (*p == '|' || *p == 0) {
                if (seg >= 3 && segStart[0] && (segStart[0] == 'F' || segStart[0] == 'N' || segStart[0] == 'T'))
                    AddDep(segStart[0], segStart + 1, (int)(p - segStart) - 1);
                ++seg;
                if (*p == 0) break;
                segStart = p + 1;
            }
        }
    }
}

// ------------------------------------------------------------------ 候选扫描

struct Cand {
    uintptr_t addr;
    uint32_t L, P, R, scope, hash, val;
    int dep;
    int density;      // 同 4KB 页里的候选数（真节点分散，假节点扎堆）
    DWORD type;       // MEM_PRIVATE / MEM_IMAGE / MEM_MAPPED
    uintptr_t regBase;
    char regName[32]; // 命中所在的已加载模块名（在模块映像里 = 铁定不是脚本节点）
};

struct ModInfo { uintptr_t base; uintptr_t end; char name[32]; };
static ModInfo g_mods[256];
static int     g_nmod = 0;

static void AddMod(uintptr_t base, uintptr_t size, const wchar_t* name) {
    if (g_nmod >= 256 || !base) return;
    g_mods[g_nmod].base = base;
    g_mods[g_nmod].end = base + size;
    WideCharToMultiByte(CP_UTF8, 0, name, -1, g_mods[g_nmod].name, 32, nullptr, nullptr);
    ++g_nmod;
}

static const char* ModAt(uintptr_t addr) {
    for (int i = 0; i < g_nmod; ++i)
        if (addr >= g_mods[i].base && addr < g_mods[i].end) return g_mods[i].name;
    return nullptr;
}

static Cand   g_cand[6000];
static int    g_ncand = 0;
static int    g_depCount[512];
static BYTE   g_chunk[8u << 20];

static void ScanCandidates(HANDLE h, uintptr_t minAddr, uintptr_t maxAddr) {
    static uint32_t bits[1u << 15];
    memset(bits, 0, sizeof(bits));
    for (int i = 0; i < g_ndep; ++i) {
        const uint32_t k = g_dep[i].hash & 0xFFFFFu;
        bits[k >> 5] |= (1u << (k & 31));
    }
    const DWORD readMask = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t cur = minAddr;
    while (cur < maxAddr && VirtualQueryEx(h, (LPCVOID)cur, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_COMMIT && (uintptr_t)mbi.BaseAddress >= 0x00010000u &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (mbi.Protect & readMask)) {
            const uintptr_t base = (uintptr_t)mbi.BaseAddress;
            const size_t size = mbi.RegionSize;
            for (size_t off = 0; off < size; off += sizeof(g_chunk)) {
                const size_t n = (size - off) < sizeof(g_chunk) ? (size - off) : sizeof(g_chunk);
                if (!Rd(h, base + off, g_chunk, n)) continue;
                const uint32_t* w = (const uint32_t*)g_chunk;
                const size_t cnt = n / 4;
                for (size_t i = 0; i < cnt; ++i) {
                    const uint32_t hv = w[i];
                    if (!((bits[(hv & 0xFFFFFu) >> 5] >> (hv & 31)) & 1u)) continue;
                    int d = -1;
                    for (int j = 0; j < g_ndep; ++j) if (g_dep[j].hash == hv) { d = j; break; }
                    if (d < 0) continue;
                    const uintptr_t node = base + off + (uintptr_t)i * 4u - 0x14u;
                    if (node < 0x01000000u) continue;
                    if (g_depCount[d] >= 32) continue;          // 每 hash 最多留 32 个
                    if (g_ncand >= (int)(sizeof(g_cand) / sizeof(g_cand[0]))) continue;
                    Cand c{};
                    c.addr = node;
                    c.dep = d;
                    c.hash = hv;
                    uint32_t raw[8] = { 0 };
                    if (!Rd(h, node, raw, sizeof(raw))) continue;
                    if (raw[5] != hv) continue;                 // +0x14 必须是这个名字哈希
                    c.L = raw[0]; c.P = raw[1]; c.R = raw[2];
                    c.scope = raw[4];
                    c.val = raw[6];
                    MEMORY_BASIC_INFORMATION bm{};
                    if (VirtualQueryEx(h, (LPCVOID)node, &bm, sizeof(bm)) == sizeof(bm)) {
                        c.type = bm.Type;
                        c.regBase = (uintptr_t)bm.BaseAddress;
                    }
                    g_cand[g_ncand++] = c;
                    ++g_depCount[d];
                }
            }
        }
        const uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= cur) break;
        cur = next;
    }
    // 模块归属（命中在任何一个已加载模块的映像里 = 不可能是脚本节点）
    for (int i = 0; i < g_ncand; ++i) {
        const char* mn = ModAt(g_cand[i].addr);
        if (mn) {
            strncpy_s(g_cand[i].regName, mn, _TRUNCATE);
        }
    }
    // 页密度
    for (int i = 0; i < g_ncand; ++i) {
        const uintptr_t pg = g_cand[i].addr & ~(uintptr_t)0xFFF;
        int c = 0;
        for (int j = 0; j < g_ncand; ++j)
            if ((g_cand[j].addr & ~(uintptr_t)0xFFF) == pg) ++c;
        g_cand[i].density = c;
    }
}

// ------------------------------------------------------------------ 主流程

int wmain(int argc, wchar_t** argv) {
    // 开关：--noelev 不自杀式提权（调试用）；--nopause 结束不等按键（脚本调用用）
    bool noElev = false, noPause = false;
    const wchar_t* outDir = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--noelev") == 0) noElev = true;
        else if (_wcsicmp(argv[i], L"--nopause") == 0) noPause = true;
        else outDir = argv[i];
    }
    if (!noElev && !IsElevated()) {
        if (RelaunchElevated(argc, argv)) return 0;
        printf("[!] not elevated - memory scan will likely fail (game runs as admin)\n");
    }

    // 输出目录
    wchar_t dir[MAX_PATH]{};
    if (outDir) {
        wcsncpy_s(dir, outDir, _TRUNCATE);
    } else {
        GetModuleFileNameW(nullptr, dir, MAX_PATH);
        wchar_t* slash = wcsrchr(dir, L'\\');
        if (slash) *(slash + 1) = 0;
    }
    if (dir[wcslen(dir) - 1] != L'\\') wcscat_s(dir, L"\\");

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t path[MAX_PATH]{};
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%sra3diag_%04d%02d%02d_%02d%02d%02d.txt",
                 dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) {
        printf("[x] cannot create report file\n");
        if (!noPause) system("pause");
        return 1;
    }

    LoadDeps();

    fprintf(f, "=== Ra3-Copilot 诊断包 v%s ===\n", DIAG_VER);
    fprintf(f, "生成 %04d-%02d-%02d %02d:%02d:%02d\n", st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond);
    fprintf(f, "管理员=%s  依赖总数=%d\n", IsElevated() ? "是" : "否", g_ndep);
    {
        OSVERSIONINFOW ov{};
        ov.dwOSVersionInfoSize = sizeof(ov);
#pragma warning(push)
#pragma warning(disable : 4996)
        GetVersionExW(&ov);
#pragma warning(pop)
        fprintf(f, "系统 Windows %lu.%lu build %lu\n", ov.dwMajorVersion, ov.dwMinorVersion,
                ov.dwBuildNumber);
    }

    // ---------- 进程与指纹 ----------
    const DWORD pid = FindGamePid();
    fprintf(f, "\n[1] 游戏进程\n");
    fprintf(f, "pid=%lu\n", pid);
    HANDLE h = nullptr;
    uintptr_t modBase = 0, modEnd = 0;
    if (!pid) {
        fprintf(f, "未找到 %ls —— 请先进对局再运行本工具\n", GAME_PROC);
    } else {
        h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!h) {
            fprintf(f, "OpenProcess 失败 err=%lu（权限不足？请用管理员运行）\n", GetLastError());
        }
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W me{};
            me.dwSize = sizeof(me);
            if (Module32FirstW(snap, &me)) {
                do {
                    if (_wcsicmp(me.szModule, GAME_PROC) == 0) {
                        modBase = (uintptr_t)me.modBaseAddr;
                        modEnd = modBase + me.modBaseSize;
                        fprintf(f, "path=%ls\nbase=%08X size=%08X\n", me.szExePath,
                                (unsigned)modBase, (unsigned)me.modBaseSize);
                        WIN32_FILE_ATTRIBUTE_DATA fa{};
                        if (GetFileAttributesExW(me.szExePath, GetFileExInfoStandard, &fa)) {
                            const ULONGLONG sz = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
                            FILETIME ftLocal{};
                            SYSTEMTIME s2{};
                            FileTimeToLocalFileTime(&fa.ftLastWriteTime, &ftLocal);
                            FileTimeToSystemTime(&ftLocal, &s2);
                            fprintf(f, "file=%llu bytes  mtime=%04d-%02d-%02d %02d:%02d:%02d\n", sz,
                                    s2.wYear, s2.wMonth, s2.wDay, s2.wHour, s2.wMinute, s2.wSecond);
                        }
                        break;
                    }
                } while (Module32NextW(snap, &me));
            }
            CloseHandle(snap);
        }
        // PE 头（模块基址处就是映像头）——比文件哈希更能判定构建
        if (h && modBase) {
            BYTE hdr[0x400] = { 0 };
            if (Rd(h, modBase, hdr, sizeof(hdr))) {
                const DWORD e_lfanew = *(DWORD*)(hdr + 0x3C);
                if (e_lfanew + 0x58 < sizeof(hdr)) {
                    const BYTE* nt = hdr + e_lfanew;
                    const DWORD sig = *(DWORD*)nt;
                    const WORD machine = *(WORD*)(nt + 4);
                    const DWORD tds = *(DWORD*)(nt + 8);
                    const DWORD soi = *(DWORD*)(nt + 0x50);
                    const DWORD csum = *(DWORD*)(nt + 0x58);
                    fprintf(f, "PE sig=%08X machine=%04X TimeDateStamp=%08X SizeOfImage=%08X checksum=%08X\n",
                            (unsigned)sig, machine, (unsigned)tds, (unsigned)soi, (unsigned)csum);
                }
            }
        }
    }

    // ---------- 模块 ----------
    fprintf(f, "\n[2] 加载模块（ra3copilot / 非系统）\n");
    if (pid) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE) {
            fprintf(f, "模块表不可读 err=%lu\n", GetLastError());
        } else {
            MODULEENTRY32W me{};
            me.dwSize = sizeof(me);
            int nCp = 0;
            if (Module32FirstW(snap, &me)) {
                do {
                    AddMod((uintptr_t)me.modBaseAddr, (uintptr_t)me.modBaseSize, me.szModule);
                    const bool isCp = (_wcsicmp(me.szModule, DLL_NAME) == 0);
                    if (isCp) {
                        WIN32_FILE_ATTRIBUTE_DATA fa{};
                        ULONGLONG sz = 0;
                        if (GetFileAttributesExW(me.szExePath, GetFileExInfoStandard, &fa))
                            sz = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
                        fprintf(f, "  %ls base=%08X size=%08X file=%llu path=%ls\n",
                                me.szModule, (unsigned)(uintptr_t)me.modBaseAddr, (unsigned)me.modBaseSize,
                                sz, me.szExePath);
                        ++nCp;
                    }
                } while (Module32NextW(snap, &me));
            }
            if (!nCp) fprintf(f, "  ra3copilot.dll 未加载（工具没注入，或注入失败）\n");
            // 非系统模块：Mod / 汉化 / 加速器 都会出现在这里，是「只有他那边不正常」的重要线索
            if (Module32FirstW(snap, &me)) {
                int printed = 0;
                do {
                    if (_wcsicmp(me.szModule, DLL_NAME) == 0) continue;
                    wchar_t low[MAX_PATH];
                    wcsncpy_s(low, me.szExePath, _TRUNCATE);
                    CharLowerBuffW(low, (DWORD)wcslen(low));
                    if (wcsstr(low, L"\\windows\\") || wcsstr(low, L"\\system32\\") ||
                        wcsstr(low, L"\\syswow64\\") || wcsstr(low, L"\\winsxs\\"))
                        continue;
                    if (printed < 40)
                        fprintf(f, "  [非系统] %ls base=%08X path=%ls\n", me.szModule,
                                (unsigned)(uintptr_t)me.modBaseAddr, me.szExePath);
                    ++printed;
                } while (Module32NextW(snap, &me));
                if (printed > 40) fprintf(f, "  ... 共 %d 个非系统模块\n", printed);
            }
            CloseHandle(snap);
        }
    }

    // ---------- 接口原文 ----------
    fprintf(f, "\n[3] 修改器接口（127.0.0.1:%d）\n", HTTP_PORT);
    static const char* kPaths[] = {
        "/ping", "/state", "/objectives", "/flagdbg", "/enemy", "/log?n=80",
        "/scripttbl",         // 表健康：engine / root / 节点数 / badParent / unsorted
        "/scripttbl?dump=1",  // flag 表全量（谁在表里、值多少）—— 判"到底写过没有"直接看这个
        "/scripttbl?t=timer&dump=1"
    };
    static char body[120 * 1024];
    for (int i = 0; i < (int)(sizeof(kPaths) / sizeof(kPaths[0])); ++i) {
        const int n = HttpGet(kPaths[i], body, sizeof(body));
        fprintf(f, "\n--- GET %s ---\n", kPaths[i]);
        if (n < 0) fprintf(f, "(无应答 —— 修改器未注入或接口未起)\n");
        else fprintf(f, "%s\n", body);
    }

    // ---------- 独立内存扫描 ----------
    fprintf(f, "\n[4] 独立内存扫描（不依赖 ra3copilot 的判定逻辑）\n");
    if (!h || !modBase) {
        fprintf(f, "跳过：拿不到游戏进程\n");
    } else {
        // 当前地图：TheGameLogic + 0x98
        char mapPath[260] = { 0 };
        uint32_t gl = 0;
        if (Rd(h, modBase + (GL_VA - IMAGE_BASE), &gl, 4) && gl >= 0x01000000u) {
            char* p = nullptr;
            if (Rd(h, (uintptr_t)gl + 0x98, &p, 4) && p && (uintptr_t)p >= 0x01000000u)
                Rd(h, (uintptr_t)p, mapPath, sizeof(mapPath) - 1);
        }
        fprintf(f, "当前地图: %s\n", mapPath[0] ? mapPath : "(未取到 —— 可能不在对局里)");

        ScanCandidates(h, (uintptr_t)0x00010000u, (uintptr_t)0xFFFE0000u);
        fprintf(f, "候选总数=%d（每个 hash 最多留 32 个）\n", g_ncand);

        uintptr_t lastDumpEnd = 0;
        for (int d = 0; d < g_ndep; ++d) {
            fprintf(f, "H=%08X %c[%s] c=%d\n", g_dep[d].hash, g_dep[d].kind, g_dep[d].name,
                    g_depCount[d]);
            for (int i = 0; i < g_ncand; ++i) {
                if (g_cand[i].dep != d) continue;
                const Cand& c = g_cand[i];
                const char* ty = (c.type == MEM_IMAGE) ? "IMAGE"
                               : (c.type == MEM_MAPPED) ? "MAPPED"
                               : (c.type == MEM_PRIVATE) ? "PRIVATE" : "?";
                fprintf(f, "    #%08X L=%08X P=%08X R=%08X scope=%08X hash=%08X val=%08X "
                           "page_density=%d type=%s region=%08X%s%s\n",
                        (unsigned)c.addr, c.L, c.P, c.R, c.scope, c.hash, c.val, c.density,
                        ty, (unsigned)c.regBase,
                        c.regName[0] ? " module=" : "", c.regName);
                // 命中点原始字节（node-0x10 起 0x60 字节）；相邻命中不重复 dump
                const uintptr_t from = c.addr - 0x10;
                if (from < lastDumpEnd) {
                    fprintf(f, "      (与上一条重叠，略)\n");
                    continue;
                }
                BYTE raw[0x60] = { 0 };
                if (Rd(h, from, raw, sizeof(raw))) {
                    DumpHex(f, raw, sizeof(raw), from);
                    lastDumpEnd = from + sizeof(raw);
                }
            }
        }
    }

    // ---------- 关卡脚本解析 ----------
    // 不经过修改器：自己读 .big 里的 .map 跑一遍解析。DLL 没注入、接口全无应答时，
    // 这一段仍然能回答"这关的目标表为什么出不来"。
    fprintf(f, "\n[5] 关卡脚本解析（不依赖 ra3copilot：直接读 .big 里的 .map）\n");
    {
        char gameDir[MAX_PATH] = { 0 };
        if (h) {
            char full[MAX_PATH] = { 0 };
            DWORD sz = (DWORD)sizeof(full);
            if (QueryFullProcessImageNameA(h, 0, full, &sz)) {
                char* slash = strrchr(full, '\\');
                if (slash) *slash = 0;
                strncpy_s(gameDir, sizeof(gameDir), full, _TRUNCATE);
            }
        }
        fprintf(f, "游戏目录: %s\n", gameDir[0] ? gameDir : "(未知)");

        char mapPath[260] = { 0 };
        if (h && modBase) {
            uint32_t gl = 0;
            if (Rd(h, modBase + (GL_VA - IMAGE_BASE), &gl, 4) && gl >= 0x01000000u) {
                char* p = nullptr;
                if (Rd(h, (uintptr_t)gl + 0x98, &p, 4) && p && (uintptr_t)p >= 0x01000000u)
                    Rd(h, (uintptr_t)p, mapPath, sizeof(mapPath) - 1);
            }
        }

        if (!mapPath[0]) {
            fprintf(f, "跳过：拿不到当前地图路径（不在对局里？）\n");
        } else {
            char base[128] = { 0 };
            const char* nm = mapPath;
            for (const char* q = mapPath; *q; ++q)
                if (*q == '\\' || *q == '/') nm = q + 1;
            int k = 0;
            for (; nm[k] && nm[k] != '.' && k < (int)sizeof(base) - 1; ++k) {
                const char c = nm[k];
                base[k] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
            }
            base[k] = 0;

            fprintf(f, "地图路径: %s\n", mapPath);
            if (gameDir[0]) Big4SetRootDir(gameDir);

            MapTblResult r;
            if (!MapTblBuild(mapPath, base, &r)) {
                fprintf(f, "解析失败: %s\n", r.err);
            } else {
                fprintf(f, "map=%s\n来源=%s\nscripts=%d objs=%d paths=%d lines=%d\n",
                        r.mapName, r.source, r.scriptCount, r.objCount, r.pathCount, r.lineCount);
                for (int i = 0; r.lines && r.lines[i]; ++i)
                    fprintf(f, "%s\n", r.lines[i]);
            }
        }
    }

    fprintf(f, "\n=== 完 ===\n");
    fprintf(f, "把这个文件原样发回即可（不含任何个人信息，只有内存里的脚本节点与接口回执）。\n");
    fclose(f);
    if (h) CloseHandle(h);

    wprintf(L"[ok] %ls\n", path);
    printf("[ok] report written, send this file back\n");
    if (!noPause) system("pause");
    return 0;
}
