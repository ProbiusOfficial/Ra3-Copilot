// BIG4 索引读取：给"运行时解析关卡脚本"提供 .map 原始字节。
//
// 为什么自己解而不调引擎的文件系统：BIG4 是明文格式，解析成本fixed；而引擎的
// FileSystem 接口要按版本找函数、还要处理一套虚表，反而更脆。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "big4.h"

#include <string.h>
#include <stdio.h>

static const uint32_t kMaxIndexBytes = 48u << 20;   // 索引上限：超过就不像放着地图的包
static const int      kMaxEntryName  = (int)sizeof(((Big4Entry*)0)->name);

static char g_rootDir[MAX_PATH];      // 空 = 用宿主进程 exe 所在目录

static char g_cacheKey[420];
static Big4Entry g_cacheEntry;
static bool g_cacheOk = false;

// ---- 小工具 ---------------------------------------------------------------

static void LowerAscii(char* s) {
    for (; *s; ++s) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}

static void ToBackslash(char* s) {
    for (; *s; ++s) if (*s == '/') *s = '\\';
}

static bool EndsWith(const char* s, const char* suf) {
    const size_t a = strlen(s), b = strlen(suf);
    return a >= b && memcmp(s + a - b, suf, b) == 0;
}

// 二进制读：返回 VirtualAlloc 缓冲
static void* ReadFilePart(const char* path, uint32_t offset, uint32_t bytes, uint32_t* gotBytes) {
    if (gotBytes) *gotBytes = 0;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;

    void* buf = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) { CloseHandle(h); return nullptr; }

    LARGE_INTEGER li;
    li.QuadPart = offset;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
        VirtualFree(buf, 0, MEM_RELEASE);
        CloseHandle(h);
        return nullptr;
    }
    DWORD got = 0;
    const BOOL ok = ReadFile(h, buf, bytes, &got, nullptr);
    CloseHandle(h);
    if (!ok) {
        VirtualFree(buf, 0, MEM_RELEASE);
        return nullptr;
    }
    if (gotBytes) *gotBytes = got;
    return buf;
}

static void ExeDir(char* out, int cap) {
    out[0] = 0;
    if (g_rootDir[0]) { strncpy_s(out, (size_t)cap, g_rootDir, _TRUNCATE); return; }
    char p[MAX_PATH] = { 0 };
    if (!GetModuleFileNameA(nullptr, p, MAX_PATH)) return;
    char* last = nullptr;
    for (char* s = p; *s; ++s) if (*s == '\\' || *s == '/') last = s;
    if (last) *last = 0;
    strncpy_s(out, (size_t)cap, p, _TRUNCATE);
}

// ---- 单条目匹配 -----------------------------------------------------------

// 在 big 索引里找文件名以 "\<base>.map" 结尾的条目。
// 返回匹配质量（越大越好），0 = 不命中。
static int MatchInBig(const char* bigPath, HANDLE h, const char* relHint,
                      const char* mapBaseName, Big4Entry* best) {
    uint8_t head[16];
    DWORD got = 0;
    if (!ReadFile(h, head, 16, &got, nullptr) || got != 16) return 0;
    if (memcmp(head, "BIG4", 4) != 0) return 0;

    const uint32_t count     = ((uint32_t)head[8]  << 24) | ((uint32_t)head[9]  << 16) |
                               ((uint32_t)head[10] << 8)  |  (uint32_t)head[11];
    const uint32_t indexSize = ((uint32_t)head[12] << 24) | ((uint32_t)head[13] << 16) |
                               ((uint32_t)head[14] << 8)  |  (uint32_t)head[15];
    if (!count || indexSize < 8 || indexSize > kMaxIndexBytes) return 0;

    uint32_t idxGot = 0;
    uint8_t* idx = (uint8_t*)ReadFilePart(bigPath, 16, indexSize, &idxGot);
    if (!idx) return 0;

    char suffix[160];
    _snprintf_s(suffix, sizeof(suffix), _TRUNCATE, "\\%s.map", mapBaseName);
    LowerAscii(suffix);

    int bestScore = 0;
    uint32_t pos = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 9 > idxGot) break;
        const uint32_t off = ((uint32_t)idx[pos] << 24) | ((uint32_t)idx[pos + 1] << 16) |
                             ((uint32_t)idx[pos + 2] << 8) | (uint32_t)idx[pos + 3];
        const uint32_t sz  = ((uint32_t)idx[pos + 4] << 24) | ((uint32_t)idx[pos + 5] << 16) |
                             ((uint32_t)idx[pos + 6] << 8) | (uint32_t)idx[pos + 7];
        pos += 8;

        const uint32_t nameStart = pos;
        while (pos < idxGot && idx[pos]) ++pos;
        const uint32_t nameLen = pos - nameStart;
        if (pos < idxGot) ++pos;          // 跳过 NUL
        if (!nameLen || nameLen >= (uint32_t)kMaxEntryName) continue;

        char name[200];
        memcpy(name, idx + nameStart, nameLen);
        name[nameLen] = 0;
        ToBackslash(name);
        LowerAscii(name);

        if (!EndsWith(name, suffix)) continue;

        // 打分：与 relHint 的共同后缀越长越可信
        int score = 1;
        if (relHint && *relHint) {
            char hint[200];
            strncpy_s(hint, sizeof(hint), relHint, _TRUNCATE);
            ToBackslash(hint);
            LowerAscii(hint);
            const size_t a = strlen(name), b = strlen(hint);
            int common = 0;
            while (common < (int)a && common < (int)b &&
                   name[a - 1 - common] == hint[b - 1 - common]) ++common;
            score = 1 + common;
        }
        if (score > bestScore) {
            bestScore = score;
            strncpy_s(best->bigPath, sizeof(best->bigPath), bigPath, _TRUNCATE);
            strncpy_s(best->name, sizeof(best->name), name, _TRUNCATE);
            best->offset = off;
            best->size = sz;
            best->loose = false;
        }
    }
    VirtualFree(idx, 0, MEM_RELEASE);
    return bestScore;
}

// 扫一个目录里的 .big。mapsOnly 为真时只看名字含 "map" 的包（快速路径）。
// 返回最高匹配质量。
static int ScanBigDir(const char* dir, const char* relHint, const char* mapBaseName,
                      Big4Entry* best, bool mapsOnly) {
    char pattern[MAX_PATH];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*.big", dir);

    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA(pattern, &fd);
    if (fh == INVALID_HANDLE_VALUE) return 0;

    int bestScore = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (mapsOnly) {
            char low[MAX_PATH];
            strncpy_s(low, sizeof(low), fd.cFileName, _TRUNCATE);
            LowerAscii(low);
            if (!strstr(low, "map")) continue;
        }
        char path[MAX_PATH];
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", dir, fd.cFileName);

        HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        const int sc = MatchInBig(path, h, relHint, mapBaseName, best);
        CloseHandle(h);
        if (sc > bestScore) bestScore = sc;
    } while (FindNextFileA(fh, &fd));
    FindClose(fh);
    return bestScore;
}

// 游戏根目录：exe 常放在 <root>\Data\ 下（RA3_1.12.game 就在那），
// 而 relHint（GameLogic+0x98）是相对 <root> 的，两者对齐才不会拼出 Data\Data。
static void RootDirOf(const char* exeDir, char* out, int cap) {
    strncpy_s(out, (size_t)cap, exeDir, _TRUNCATE);
    char* slash = strrchr(out, '\\');
    if (!slash) return;
    if (_stricmp(slash + 1, "Data") == 0) *slash = 0;
}

static bool HitLoose(const char* path, const char* name, Big4Entry* out) {
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return false;
    strncpy_s(out->bigPath, sizeof(out->bigPath), path, _TRUNCATE);
    strncpy_s(out->name, sizeof(out->name), name, _TRUNCATE);
    LowerAscii(out->name);
    out->offset = 0;
    out->size = 0;
    out->loose = true;
    return true;
}

// 散文件：游戏会优先用磁盘上的同名文件（mod / 汉化常这么覆盖）
static bool TryLoose(const char* exeDir, const char* relHint, const char* mapBaseName,
                     Big4Entry* out) {
    char root[MAX_PATH];
    RootDirOf(exeDir, root, MAX_PATH);

    // 1) 按 relHint：先相对游戏根，再相对 exe 所在目录
    if (relHint && *relHint) {
        char hint[400];
        strncpy_s(hint, sizeof(hint), relHint, _TRUNCATE);
        ToBackslash(hint);
        const bool absolute = (hint[0] == '\\') || (hint[1] == ':');
        if (!absolute) {
            char cand[MAX_PATH * 2];
            _snprintf_s(cand, sizeof(cand), _TRUNCATE, "%s\\%s", root, hint);
            if (HitLoose(cand, hint, out)) return true;
            _snprintf_s(cand, sizeof(cand), _TRUNCATE, "%s\\%s", exeDir, hint);
            if (HitLoose(cand, hint, out)) return true;
        }
    }

    // 2) 常规战役路径（同样两个基准各试一次）
    char base[200];
    _snprintf_s(base, sizeof(base), _TRUNCATE, "%s.map", mapBaseName);
    const char* bases[2] = { root, exeDir };
    for (int i = 0; i < 2; ++i) {
        char cand[MAX_PATH * 2];
        _snprintf_s(cand, sizeof(cand), _TRUNCATE, "%s\\Data\\maps\\official\\%s\\%s",
                    bases[i], mapBaseName, base);
        if (HitLoose(cand, base, out)) return true;
        _snprintf_s(cand, sizeof(cand), _TRUNCATE, "%s\\maps\\official\\%s\\%s",
                    bases[i], mapBaseName, base);
        if (HitLoose(cand, base, out)) return true;
    }
    return false;
}

// ---- 对外 -----------------------------------------------------------------

void Big4SetRootDir(const char* dir) {
    g_rootDir[0] = 0;
    if (dir && *dir) strncpy_s(g_rootDir, sizeof(g_rootDir), dir, _TRUNCATE);
    Big4ResetCache();
}

void Big4ResetCache() {
    g_cacheKey[0] = 0;
    g_cacheOk = false;
}

bool Big4FindMap(const char* relHint, const char* mapBaseName, Big4Entry* out) {
    if (!mapBaseName || !*mapBaseName || !out) return false;

    char base[200];
    strncpy_s(base, sizeof(base), mapBaseName, _TRUNCATE);
    LowerAscii(base);

    char key[420];
    _snprintf_s(key, sizeof(key), _TRUNCATE, "%s|%s", base, relHint ? relHint : "");
    if (g_cacheOk && strcmp(key, g_cacheKey) == 0) {
        *out = g_cacheEntry;
        return true;
    }

    char exeDir[MAX_PATH];
    ExeDir(exeDir, MAX_PATH);
    if (!exeDir[0]) return false;

    Big4Entry e;
    memset(&e, 0, sizeof(e));

    if (!TryLoose(exeDir, relHint, base, &e)) {
        char sub[MAX_PATH];
        int best = ScanBigDir(exeDir, relHint, base, &e, true);          // 先只看名字含 map 的包
        if (best == 0) {
            best = ScanBigDir(exeDir, relHint, base, &e, false);         // 再全扫
        }
        if (best == 0) {
            _snprintf_s(sub, sizeof(sub), _TRUNCATE, "%s\\Data", exeDir);
            best = ScanBigDir(sub, relHint, base, &e, true);
            if (best == 0) best = ScanBigDir(sub, relHint, base, &e, false);
        }
        if (best == 0) return false;
    }

    strncpy_s(g_cacheKey, sizeof(g_cacheKey), key, _TRUNCATE);
    g_cacheEntry = e;
    g_cacheOk = true;

    *out = e;
    return true;
}

void* Big4ReadEntry(const Big4Entry& e, uint32_t* outSize) {
    if (outSize) *outSize = 0;

    if (e.loose) {
        HANDLE h = CreateFileA(e.bigPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return nullptr;
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (64 << 20)) {
            CloseHandle(h);
            return nullptr;
        }
        void* buf = VirtualAlloc(nullptr, (SIZE_T)sz.QuadPart, MEM_COMMIT | MEM_RESERVE,
                                 PAGE_READWRITE);
        if (!buf) { CloseHandle(h); return nullptr; }
        DWORD got = 0;
        const BOOL ok = ReadFile(h, buf, (DWORD)sz.QuadPart, &got, nullptr);
        CloseHandle(h);
        if (!ok || got != (DWORD)sz.QuadPart) {
            VirtualFree(buf, 0, MEM_RELEASE);
            return nullptr;
        }
        if (outSize) *outSize = got;
        return buf;
    }

    if (!e.size) return nullptr;
    uint32_t gotBytes = 0;
    void* buf = ReadFilePart(e.bigPath, e.offset, e.size, &gotBytes);
    if (!buf || gotBytes != e.size) {
        if (buf) VirtualFree(buf, 0, MEM_RELEASE);
        return nullptr;
    }
    if (outSize) *outSize = gotBytes;
    return buf;
}

void Big4BufFree(void* p) {
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}
