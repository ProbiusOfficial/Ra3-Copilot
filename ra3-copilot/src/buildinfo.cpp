#include "buildinfo.h"
#include <cstdio>
#include <cstring>

namespace ra3 {
namespace {

// 0 = 未决议。POD 的常量初始化发生在任何静态初始化之前（全零内存），
// 所以这里的写读不受跨编译单元静态初始化顺序影响。
volatile LONG g_kind   = 0;
volatile LONG g_forced = 0;

// PE32 头解析。have = 已确保可读的字节数；越界一律返回 false，不猜。
bool ParsePe(const uint8_t* img, size_t have, BuildProbe* out) {
    if (!img || !out || have < 0x40) return false;
    if (img[0] != 'M' || img[1] != 'Z') return false;
    uint32_t lfanew = 0;
    std::memcpy(&lfanew, img + 0x3C, 4);
    if (lfanew + 24 > have) return false;
    if (img[lfanew] != 'P' || img[lfanew + 1] != 'E' ||
        img[lfanew + 2] != 0 || img[lfanew + 3] != 0) return false;
    const uint8_t* coff = img + lfanew + 4;
    uint16_t optSize = 0;
    std::memcpy(&optSize, coff + 16, 2);
    if (lfanew + 24 + optSize > have || optSize < 60) return false;
    const uint8_t* opt = coff + 20;
    uint16_t magic = 0;
    std::memcpy(&magic, opt, 2);
    if (magic != 0x10B) return false;   // 只处理 PE32；游戏是 32 位，PE32+ 不可能命中
    BuildProbe p;
    std::memcpy(&p.imageBase,   opt + 28, 4);
    std::memcpy(&p.sizeOfImage, opt + 56, 4);
    std::memcpy(&p.timeStamp,   coff + 4, 4);
    p.fileSize = out->fileSize;         // 由调用方填（内存镜像没有文件大小）
    *out = p;
    return true;
}

} // namespace

BuildKind ClassifyBuild(const BuildProbe& p) {
    if (p.sizeOfImage == 0x93B000) return BuildKind::Retail;
    if (p.sizeOfImage == 0x950000) return BuildKind::Steam;
    return BuildKind::Unknown;
}

bool ProbeImage(uintptr_t imageBase, BuildProbe* out) {
    if (!imageBase || !out) return false;
    BuildProbe p;
    bool ok = false;
    __try {
        ok = ParsePe(reinterpret_cast<const uint8_t*>(imageBase), 0x400, &p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (ok) *out = p;
    return ok;
}

bool ProbeImageProcess(HANDLE proc, uintptr_t imageBase, BuildProbe* out) {
    if (!proc || !imageBase || !out) return false;
    uint8_t head[0x400] = { 0 };
    SIZE_T got = 0;
    if (!ReadProcessMemory(proc, reinterpret_cast<const void*>(imageBase),
                           head, sizeof(head), &got) || got < 0x40)
        return false;
    BuildProbe p;
    if (!ParsePe(head, got, &p)) return false;
    *out = p;
    return true;
}

bool ProbeFile(const wchar_t* path, BuildProbe* out) {
    if (!path || !out) return false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return false;
    uint8_t head[0x400] = { 0 };
    const size_t got = fread(head, 1, sizeof(head), f);
    _fseeki64(f, 0, SEEK_END);
    const long long size = _ftelli64(f);
    fclose(f);
    if (got < 0x40) return false;
    BuildProbe p;
    if (!ParsePe(head, got, &p)) return false;
    if (size > 0) p.fileSize = static_cast<uint64_t>(size);
    *out = p;
    return true;
}

void SetBuild(BuildKind kind, bool forced) {
    InterlockedExchange(&g_kind, static_cast<LONG>(kind));
    InterlockedExchange(&g_forced, forced ? 1 : 0);
}

BuildKind GetBuild() {
    return static_cast<BuildKind>(InterlockedCompareExchange(&g_kind, 0, 0));
}

bool BuildOverridden() {
    return InterlockedCompareExchange(&g_forced, 0, 0) != 0;
}

const char* BuildName(BuildKind k) {
    switch (k) {
    case BuildKind::Retail: return "零售 1.12";
    case BuildKind::Steam:  return "Steam 1.12";
    default:                return "未知";
    }
}

BuildKind ResolveBuildForSelf(const wchar_t* iniPath, BuildProbe* probeOut) {
    BuildProbe p;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    const bool got = ProbeImage(base, &p);
    if (probeOut) *probeOut = p;
    const BuildKind detected = got ? ClassifyBuild(p) : BuildKind::Unknown;

    BuildKind force = BuildKind::Unknown;
    const bool forced = LoadBuildIni(iniPath, &force);
    SetBuild(forced ? force : detected, forced);
    return forced ? force : detected;
}

bool LoadBuildIni(const wchar_t* iniPath, BuildKind* force) {
    if (force) *force = BuildKind::Unknown;
    if (!iniPath || !force) return false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, iniPath, L"r") != 0 || !f) return false;
    bool found = false;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (_strnicmp(line, "mode=", 5) != 0) continue;
        const char* v = line + 5;
        if (_strnicmp(v, "steam", 5) == 0)       { *force = BuildKind::Steam;  found = true; }
        else if (_strnicmp(v, "retail", 6) == 0) { *force = BuildKind::Retail; found = true; }
        break;   // mode 只认第一行；auto / 拼错都当自动判定
    }
    fclose(f);
    return found;
}

void SaveBuildIni(const wchar_t* iniPath, const char* mode) {
    if (!iniPath) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, iniPath, L"w") != 0 || !f) return;
    fprintf(f, "# 目标构建。auto 按 PE 头 SizeOfImage 自动判定；retail / steam 强制指定。\n");
    fprintf(f, "# 强制值与实际二进制不符时，hook 逐条 AOB 校验会跳过不符的站点（不写错字节）。\n");
    fprintf(f, "mode=%s\n", mode && *mode ? mode : "auto");
    fclose(f);
}

} // namespace ra3
