// campaign_patch.cpp - 战役扩展与全科技模式：数据包等长原地补丁（实现）
//
// 只内嵌我们自己的 Lua 片段；密钥、索引、条目内容全部运行时从玩家本机数据包读取。
// 详见 campaign_patch.h 顶部说明。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "campaign_patch.h"
#include "resource.h"

namespace cup {
namespace {

const uint32_t kBLOCK = 512;
const char kEntry[]  = "data\\scripts\\scriptevents.xml";
const char kAnchor[] = "for i = 1, getn(PlayerRefList2), 1 do";
const char kTag[]    = "RA3COPILOT_CU_V4_OK";
// 版本无关前缀：用来认出「任意一代」我们打过的补丁。片段内容升级后，
// 旧版补丁仍能被识别、还原，不会卡在「找不到当前 snippet」。
const char kMark[]   = "RA3COPILOT_CU_";
// 日冕的 OnUICheckerCreated / OnUIChecker_Saved2Created 只在本地玩家带
// Upgrade_CelestialFaction 时写 "_cor"（日冕 24 格协议栏），其余阵营写
// "default"（原版 12 格）。这里做等长替换，让所有阵营战役都走日冕 UI。
// 两串必须严格等长，否则会破坏 BIG4 条目偏移。
const char kUiFrom[] = "toggleUI(\"default\")";
const char kUiTo[]   = "toggleUI(\"_cor\")   ";
static_assert(sizeof(kUiFrom) == sizeof(kUiTo), "UI 替换串必须严格等长");
const char kCacheMagic[] = "CUK1";
const char kBakMagic[]   = "CUB1";
const size_t kNPos = static_cast<size_t>(-1);

Info g_info;
std::wstring g_gameDir, g_runtimeDir;
std::vector<uint8_t> g_snippet;
std::wstring g_diag;
uint8_t g_key[kBLOCK] = {};
bool g_keyReady = false;

std::wstring Join(const std::wstring& a, const wchar_t* b) {
    std::wstring r = a;
    if (!r.empty() && r.back() != L'\\' && r.back() != L'/') r += L'\\';
    r += b;
    return r;
}

uint32_t BE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void Put64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint64_t Get64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
void Put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint32_t Get32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

uint32_t Crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFu;
}

HANDLE OpenRead(const std::wstring& path) {
    return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool ReadAt(HANDLE h, uint64_t off, void* buf, DWORD n) {
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(off);
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    return ReadFile(h, buf, n, &got, nullptr) && got == n;
}

bool WriteAt(HANDLE h, uint64_t off, const void* buf, DWORD n) {
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(off);
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    DWORD put = 0;
    return WriteFile(h, buf, n, &put, nullptr) && put == n;
}

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>* out) {
    HANDLE h = OpenRead(path);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li{};
    GetFileSizeEx(h, &li);
    if (li.QuadPart <= 0 || li.QuadPart > (64LL << 20)) { CloseHandle(h); return false; }
    out->resize(static_cast<size_t>(li.QuadPart));
    bool ok = ReadAt(h, 0, &(*out)[0], static_cast<DWORD>(out->size()));
    CloseHandle(h);
    return ok;
}

bool WriteWholeFile(const std::wstring& path, const uint8_t* p, size_t n) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool ok = WriteAt(h, 0, p, static_cast<DWORD>(n));
    CloseHandle(h);
    return ok;
}

// C[i] = P[i] + K[(absOff + i) % 512]。enc=true 加密，false 解密。
void Crypt(uint8_t* buf, size_t n, const uint8_t key[512], uint64_t absOff, bool enc) {
    const size_t phase = static_cast<size_t>(absOff % kBLOCK);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t k = key[(phase + i) % kBLOCK];
        buf[i] = enc ? static_cast<uint8_t>(buf[i] + k) : static_cast<uint8_t>(buf[i] - k);
    }
}

size_t FindBytes(const std::vector<uint8_t>& h, const void* n, size_t nlen) {
    if (!nlen || nlen > h.size()) return kNPos;
    const uint8_t* p = static_cast<const uint8_t*>(n);
    for (size_t i = 0; i + nlen <= h.size(); ++i)
        if (memcmp(&h[i], p, nlen) == 0) return i;
    return kNPos;
}
size_t FindBytes(const std::vector<uint8_t>& h, const char* s) {
    return FindBytes(h, s, strlen(s));
}
size_t CountBytes(const std::vector<uint8_t>& h, const char* s) {
    const size_t n = strlen(s);
    size_t c = 0, i = 0;
    while (i + n <= h.size()) {
        if (memcmp(&h[i], s, n) == 0) { ++c; i += n; } else ++i;
    }
    return c;
}

bool IsSpace(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

// ===== Lua 片段：从资源读纯文本，按与生成脚本完全相同的规则包装成字节 =====
// 规则：丢掉空行与 -- 注释行；每行前加 4 个 tab，CRLF 连接，末尾补 CRLF；
//       外层包 do / end。字节必须与已应用的补丁逐字节相同，还原才能精确定位。
std::vector<uint8_t> BuildSnippet() {
    HRSRC r = FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDR_CAMPAIGN_LUA), RT_RCDATA);
    if (!r) return {};
    HGLOBAL g = LoadResource(GetModuleHandleW(nullptr), r);
    if (!g) return {};
    const void* p = LockResource(g);
    const DWORD n = SizeofResource(GetModuleHandleW(nullptr), r);
    if (!p || !n) return {};
    const char* s = static_cast<const char*>(p);

    std::string out = "\t\t\t\tdo\r\n";
    size_t i = 0;
    while (i < n) {
        size_t e = i;
        while (e < n && s[e] != '\n') ++e;
        size_t len = e - i;
        if (len && s[i + len - 1] == '\r') --len;
        const std::string line(s + i, len);
        i = (e < n) ? e + 1 : n;
        // strip() 非空
        size_t a = 0, b = line.size();
        while (a < b && (IsSpace(static_cast<uint8_t>(line[a])) || line[a] == '\n')) ++a;
        while (b > a && (IsSpace(static_cast<uint8_t>(line[b - 1])) || line[b - 1] == '\n')) --b;
        if (a == b) continue;
        // lstrip().startswith("--")
        if (b - a >= 2 && line[a] == '-' && line[a + 1] == '-') continue;
        out += "\t\t\t\t";
        out += line;
        out += "\r\n";
    }
    out += "\t\t\t\tend\r\n";
    return std::vector<uint8_t>(out.begin(), out.end());
}

// ===== 密钥恢复 =====
struct Entry { uint64_t off; uint32_t size; };
int ScanIndex(HANDLE h, uint64_t fsize, const uint8_t key[512], Entry* out, const char** why);

// 文件里有明文全零的填充块，其密文恰好等于 512 字节循环密钥；
// 连续两个相同块即候选。用文件头 "BIG4" + 总长反推 K[0..7] 做廉价筛选，
// 避免对每个相同块都回跳读头；再用「能否解出合法索引表」做最终确认。
bool LoadKey(const std::wstring& path, uint64_t size, uint8_t key[512]) {
    HANDLE h = OpenRead(path);
    if (h == INVALID_HANDLE_VALUE) return false;
    uint8_t sig[8] = {};
    if (!ReadAt(h, 0, sig, 8)) { CloseHandle(h); return false; }

    uint8_t khead[8];
    const char* magic = "BIG4";
    for (int i = 0; i < 4; ++i) khead[i] = static_cast<uint8_t>(sig[i] - magic[i]);
    const uint32_t sz = static_cast<uint32_t>(size);
    const uint8_t szbe[4] = { static_cast<uint8_t>(sz >> 24), static_cast<uint8_t>(sz >> 16),
                              static_cast<uint8_t>(sz >> 8),  static_cast<uint8_t>(sz) };
    for (int i = 0; i < 4; ++i) khead[4 + i] = static_cast<uint8_t>(sig[4 + i] - szbe[i]);

    // 缓存：magic(4) + filesize(8) + 头 8 字节密文(8) + key(512)
    const std::wstring cp = Join(g_runtimeDir, L"campaign_pack.key");
    std::vector<uint8_t> cache;
    if (ReadWholeFile(cp, &cache) && cache.size() == 532 &&
        memcmp(&cache[0], kCacheMagic, 4) == 0 &&
        Get64(&cache[4]) == size && memcmp(&cache[12], sig, 8) == 0 &&
        memcmp(&cache[20], khead, 8) == 0) {
        memcpy(key, &cache[20], 512);
        const bool valid = ScanIndex(h, size, key, nullptr, nullptr) >= 50;
        CloseHandle(h);
        return valid;
    }

    const uint32_t kChunk = 4u << 20;
    std::vector<uint8_t> buf(kChunk);
    uint8_t prev[512] = {};
    bool hasPrev = false;
    uint64_t off = 0;
    bool found = false;
    while (off + kBLOCK <= size) {
        uint32_t want = static_cast<uint32_t>(std::min<uint64_t>(kChunk, size - off));
        want -= want % kBLOCK;
        if (want < kBLOCK) break;
        if (!ReadAt(h, off, &buf[0], want)) break;
        for (uint32_t i = 0; i + kBLOCK <= want; i += kBLOCK) {
            const uint8_t* blk = &buf[i];
            if (hasPrev && memcmp(blk, khead, 8) == 0 && memcmp(blk, prev, kBLOCK) == 0 &&
                ScanIndex(h, size, blk, nullptr, nullptr) >= 50) {
                memcpy(key, blk, kBLOCK);
                found = true;
                break;
            }
            memcpy(prev, blk, kBLOCK);
            hasPrev = true;
        }
        if (found) break;
        off += want;
    }
    CloseHandle(h);
    if (!found) return false;

    std::vector<uint8_t> c(532);
    memcpy(&c[0], kCacheMagic, 4);
    Put64(&c[4], size);
    memcpy(&c[12], sig, 8);
    memcpy(&c[20], key, 512);
    WriteWholeFile(cp, &c[0], c.size());
    return true;
}

// ===== 索引：BIG4 头 24 字节 + {name\0, u32 BE off, u32 BE size} 表 =====
// Entry 的前向声明在密钥恢复一节。
// 返回解析出的条目数（0 = 密钥不对 / 格式不对）。
// out 非空时顺带填目标条目；候选密钥的合法性也用它来验证：
// 只校验文件头 8 字节会放过假候选块，解出来的索引区全是垃圾。
int ScanIndex(HANDLE h, uint64_t fsize, const uint8_t key[512], Entry* out,
              const char** why) {
    if (why) *why = "ok";
    if (out) { out->off = 0; out->size = 0; }
    uint8_t hdr[24] = {};
    if (!ReadAt(h, 0, hdr, 24)) { if (why) *why = "readhdr"; return 0; }
    Crypt(hdr, 24, key, 0, false);
    if (memcmp(hdr, "BIG4", 4) != 0) { if (why) *why = "magic"; return 0; }
    if (BE32(hdr + 4) != static_cast<uint32_t>(fsize)) { if (why) *why = "total"; return 0; }
    const uint32_t idxEnd = BE32(hdr + 12);
    if (idxEnd < 24 || idxEnd > fsize || idxEnd > (16u << 20)) {
        if (why) *why = "idxend"; return 0;
    }
    std::vector<uint8_t> tbl(idxEnd - 24);
    if (!ReadAt(h, 24, &tbl[0], static_cast<DWORD>(tbl.size()))) {
        if (why) *why = "readtbl"; return 0;
    }
    Crypt(&tbl[0], tbl.size(), key, 24, false);
    const size_t nlen = strlen(kEntry);
    int n = 0;
    size_t p = 0;
    while (p + 9 <= tbl.size()) {
        size_t e = p;
        while (e < tbl.size() && tbl[e]) ++e;
        if (e + 9 > tbl.size()) break;
        ++n;
        if (out && e - p == nlen && memcmp(&tbl[p], kEntry, nlen) == 0) {
            out->off = BE32(&tbl[e + 1]);
            out->size = BE32(&tbl[e + 5]);
        }
        p = e + 9;
    }
    if (why && n == 0) *why = "noentry";
    return n;
}

struct Region {
    uint64_t base = 0;
    size_t head = 0;                 // off - base
    std::vector<uint8_t> cipher;    // [base, off+size)
    std::vector<uint8_t> plain;
    std::vector<uint8_t> text;      // 条目明文，长度 == size
};

bool LoadRegion(const std::wstring& path, const Entry& en, const uint8_t key[512], Region* r) {
    r->base = en.off - en.off % kBLOCK;
    r->head = static_cast<size_t>(en.off - r->base);
    const uint64_t n = en.off + en.size - r->base;
    HANDLE h = OpenRead(path);
    if (h == INVALID_HANDLE_VALUE) return false;
    r->cipher.resize(static_cast<size_t>(n));
    const bool ok = ReadAt(h, r->base, &r->cipher[0], static_cast<DWORD>(n));
    CloseHandle(h);
    if (!ok) return false;
    r->plain = r->cipher;
    Crypt(&r->plain[0], r->plain.size(), key, r->base, false);
    if (r->head + en.size > r->plain.size()) return false;
    r->text.assign(r->plain.begin() + r->head, r->plain.begin() + r->head + en.size);
    return true;
}

// 写回必须在游戏没占用文件时做：share mode 0 会让 CreateFile 直接失败。
bool SaveRegion(const std::wstring& path, uint64_t base, const std::vector<uint8_t>& cipher) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool ok = WriteAt(h, base, &cipher[0], static_cast<DWORD>(cipher.size()));
    if (ok) FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

// ===== 等长改写 =====
// 把 [0,anchor) 内的「注释行 / 空行」按连续段收集。这些行的非换行字符
// 全部可以安全替换成空格，压缩掉多少字节就能插入多少字节的脚本。
struct Span { size_t a, z; };

bool IsBlankOrComment(const std::vector<uint8_t>& d, size_t i, size_t end, size_t* lineEnd) {
    size_t e = i;
    while (e < end && d[e] != '\n') ++e;
    const size_t le = (e < end) ? e + 1 : end;
    *lineEnd = le;
    size_t p = i;
    while (p < le && IsSpace(d[p]) && d[p] != '\n') ++p;
    const bool blank = (p >= le) || d[p] == '\n' || d[p] == '\r';
    const bool comment = (!blank && le - p >= 2 && d[p] == '-' && d[p + 1] == '-');
    return blank || comment;
}

std::vector<Span> CollectBlocks(const std::vector<uint8_t>& d, size_t end) {
    std::vector<Span> out;
    size_t i = 0;
    while (i < end) {
        size_t le = i;
        if (IsBlankOrComment(d, i, end, &le)) {
            const size_t a = i;
            size_t j = i;
            while (j < end) {
                size_t le2 = j;
                if (!IsBlankOrComment(d, j, end, &le2)) break;
                j = le2;
            }
            Span s; s.a = a; s.z = j;
            out.push_back(s);
            i = j;
        } else {
            i = le;
        }
    }
    return out;
}

// 严格等长的字节串替换（用于改 UI 分支调用，不引入任何新 Lua 语法）。
void ReplaceEqualLen(std::vector<uint8_t>* buf, const char* from, const char* to) {
    const size_t n = strlen(from);
    if (!buf || !n || strlen(to) != n || buf->size() < n) return;
    for (size_t p = 0; p + n <= buf->size();) {
        if (memcmp(&(*buf)[p], from, n) == 0) {
            memcpy(&(*buf)[p], to, n);
            p += n;
        } else {
            ++p;
        }
    }
}

// 从最靠近锚点的注释块开始往前，累计压缩到恰好 snip.size() 字节，
// 再把脚本插在锚点前。压缩始终保留换行，只删空格，不改变任何代码行。
// 单块压缩不够时自动跨块，因此「还原后重新启用」也能稳定工作。
bool Rewrite(const std::vector<uint8_t>& text, const std::vector<uint8_t>& snip,
             std::vector<uint8_t>* out) {
    if (CountBytes(text, kAnchor) != 1) return false;
    const size_t i = FindBytes(text, kAnchor);
    if (i == kNPos || snip.empty()) return false;
    const size_t need = snip.size();
    const std::vector<Span> blocks = CollectBlocks(text, i);
    if (blocks.empty()) return false;
    size_t avail = 0;
    for (size_t b = 0; b < blocks.size(); ++b)
        for (size_t k = blocks[b].a; k < blocks[b].z; ++k)
            if (text[k] != '\r' && text[k] != '\n') ++avail;
    if (avail < need) return false;
    if (out == nullptr) return true;             // 只做可行性检查

    std::vector<uint8_t> prefix(text.begin(), text.begin() + i);
    size_t remaining = need;
    // 从后往前处理：删除都发生在当前块之后，前面块的偏移不受影响。
    for (size_t bi = blocks.size(); bi-- > 0 && remaining > 0;) {
        const size_t a = blocks[bi].a, z = blocks[bi].z;
        for (size_t k = a; k < z; ++k)
            if (prefix[k] != '\r' && prefix[k] != '\n') prefix[k] = ' ';
        std::vector<uint8_t> part;
        part.reserve(z - a);
        for (size_t k = z; k-- > a;) {
            if (prefix[k] == ' ' && remaining > 0) { --remaining; continue; }
            part.push_back(prefix[k]);
        }
        std::reverse(part.begin(), part.end());
        prefix.erase(prefix.begin() + a, prefix.begin() + z);
        prefix.insert(prefix.begin() + a, part.begin(), part.end());
    }
    if (remaining != 0) return false;

    out->clear();
    out->reserve(text.size());
    out->insert(out->end(), prefix.begin(), prefix.end());
    out->insert(out->end(), snip.begin(), snip.end());
    out->insert(out->end(), text.begin() + i, text.end());
    ReplaceEqualLen(out, kUiFrom, kUiTo);
    return out->size() == text.size();
}

// ===== 备份 =====
std::wstring BackupPath() { return Join(Join(g_runtimeDir, L"backup"), L"campaign_region.bak"); }
void EnsureBackupDir();
bool FileExistsW(const std::wstring& p);

bool SaveBackup(const Region& r, const Entry& en) {
    EnsureBackupDir();
    if (FileExistsW(BackupPath())) return true;   // 只保留第一次的原始区域
    std::vector<uint8_t> b(20 + r.cipher.size());
    memcpy(&b[0], kBakMagic, 4);
    Put64(&b[4], r.base);
    Put32(&b[12], static_cast<uint32_t>(r.cipher.size()));
    Put32(&b[16], Crc32(&r.cipher[0], r.cipher.size()));
    memcpy(&b[20], &r.cipher[0], r.cipher.size());
    (void)en;
    return WriteWholeFile(BackupPath(), &b[0], b.size());
}

void EnsureBackupDir() {
    const std::wstring d = Join(g_runtimeDir, L"backup");
    CreateDirectoryW(d.c_str(), nullptr);
}

bool FileExistsW(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// 精确还原：备份是首次写入前的原始区域（自身带 CRC）。
// 严格通道：对备份重跑一次当前片段的改写 == 当前区域，说明版本一致。
// 宽松通道：Lua 片段升级过（如 V3 全技能 → V4 只给抽卡），严格比对必然失败；
//   此时只要「备份不含任何一代标记」且「当前区域带我们的标记」，
//   就能确认当前补丁出自这份备份，按字节写回同样是完整还原。
bool TryExactRestore(const Region& cur, std::vector<uint8_t>* cipher) {
    std::vector<uint8_t> b;
    if (!ReadWholeFile(BackupPath(), &b) || b.size() < 21) return false;
    if (memcmp(&b[0], kBakMagic, 4) != 0) return false;
    const uint64_t base = Get64(&b[4]);
    const uint32_t n = Get32(&b[12]);
    if (base != cur.base || n != cur.cipher.size() || b.size() != 20u + n) return false;
    if (Get32(&b[16]) != Crc32(&b[20], n)) return false;
    std::vector<uint8_t> plain(&b[20], &b[20] + n);
    Crypt(&plain[0], plain.size(), g_key, base, false);
    if (cur.head + cur.text.size() > plain.size()) return false;
    std::vector<uint8_t> oldText(plain.begin() + cur.head, plain.begin() + cur.head + cur.text.size());
    if (FindBytes(oldText, kMark) != kNPos) return false;   // 备份必须是干净的原始区域
    std::vector<uint8_t> expect;
    const bool strict = Rewrite(oldText, g_snippet, &expect) && expect == cur.text;
    if (!strict && FindBytes(cur.text, kMark) == kNPos) return false;  // 当前不是我们的补丁
    cipher->assign(b.begin() + 20, b.end());
    return true;
}

// 无备份时的兜底还原：片段总是紧贴锚点前、以 "\t\t\t\tdo\r\n" 开头且内含标记。
// 按结构定位而非按字节比对，因此跨版本也能还原；只把非换行字符空白化，长度不变。
bool WipeSnippet(std::vector<uint8_t>* text) {
    if (!text) return false;
    // 片段自带一行 kAnchor，打过补丁的文本里锚点会有两个：
    // 片段内的在前、原始脚本的在后，片段紧贴「后面那个」之前。取最后一个。
    const size_t nAnchor = strlen(kAnchor);
    size_t anchor = kNPos;
    for (size_t q = 0; q + nAnchor <= text->size(); ++q)
        if (memcmp(&(*text)[q], kAnchor, nAnchor) == 0) anchor = q;
    if (anchor == kNPos || anchor == 0) return false;
    const char kDo[] = "\t\t\t\tdo\r\n";
    const size_t nDo = sizeof(kDo) - 1;
    size_t p = kNPos;
    for (size_t q = 0; q + nDo <= anchor; ++q)
        if (memcmp(&(*text)[q], kDo, nDo) == 0) p = q;
    if (p == kNPos) return false;
    const size_t mark = FindBytes(*text, kMark);
    if (mark == kNPos || mark < p || mark >= anchor) return false;
    for (size_t k = p; k < anchor; ++k)
        if ((*text)[k] != '\r' && (*text)[k] != '\n') (*text)[k] = ' ';
    return true;
}

std::wstring LastPackPath() {
    std::vector<uint8_t> b;
    const std::wstring p = Join(g_runtimeDir, L"campaign_pack.path");
    if (!ReadWholeFile(p, &b) || b.empty() || b.size() > 4096) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(&b[0]),
                                      static_cast<int>(b.size()), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(&b[0]),
                        static_cast<int>(b.size()), &w[0], n);
    return w;
}

void SaveLastPackPath(const std::wstring& p) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return;
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, &s[0], n, nullptr, nullptr);
    WriteWholeFile(Join(g_runtimeDir, L"campaign_pack.path"),
                   reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

std::vector<std::wstring> PackCandidates(const std::wstring& gameDir,
                                         const std::wstring& explicitPack) {
    if (!explicitPack.empty()) return std::vector<std::wstring>(1, explicitPack);
    std::vector<std::wstring> dirs;
    if (!gameDir.empty()) {
        dirs.push_back(Join(gameDir, L"CoronaData\\Corona"));
        dirs.push_back(gameDir);
    }
    dirs.push_back(L"C:\\GAME\\CoronaLauncher\\CoronaData\\Corona");
    dirs.push_back(L"D:\\GAME\\Game_RTS\\CoronaLauncher\\CoronaData\\Corona");
    dirs.push_back(L"C:\\Program Files\\CoronaLauncher\\CoronaData\\Corona");
    dirs.push_back(L"C:\\Program Files (x86)\\CoronaLauncher\\CoronaData\\Corona");

    std::vector<std::pair<uint64_t, std::wstring>> found;
    for (size_t d = 0; d < dirs.size(); ++d) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(Join(dirs[d], L"*.lyi").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const uint64_t sz = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            if (sz > (64u << 20)) found.push_back(std::make_pair(sz, Join(dirs[d], fd.cFileName)));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(found.begin(), found.end(),
              [](const std::pair<uint64_t, std::wstring>& a,
                 const std::pair<uint64_t, std::wstring>& b) { return a.first > b.first; });
    std::vector<std::wstring> out;
    std::vector<std::wstring> seen;
    for (size_t i = 0; i < found.size(); ++i) {
        bool dup = false;
        for (size_t j = 0; j < seen.size(); ++j)
            if (_wcsicmp(seen[j].c_str(), found[i].second.c_str()) == 0) { dup = true; break; }
        if (dup) continue;
        seen.push_back(found[i].second);
        out.push_back(found[i].second);
    }
    // 上次命中的包优先：常规启动只处理这一个，不必每次遍历十几个候选。
    const std::wstring last = LastPackPath();
    if (!last.empty()) {
        bool dup = false;
        for (size_t i = 0; i < out.size(); ++i)
            if (_wcsicmp(out[i].c_str(), last.c_str()) == 0) { dup = true; break; }
        if (!dup) out.insert(out.begin(), last);
    }
    return out;
}

} // namespace

const Info& Cached() { return g_info; }
const std::wstring& Diag() { return g_diag; }

Info Probe(const std::wstring& gameDir, const std::wstring& runtimeDir,
           const std::wstring& explicitPack) {
    g_gameDir = gameDir;
    g_runtimeDir = runtimeDir;
    g_keyReady = false;
    g_info = Info();
    g_diag.clear();
    g_diag += L"gameDir=[" + gameDir + L"] ";
    g_snippet = BuildSnippet();
    if (g_snippet.empty()) {
        g_info.state = kError;
        g_info.detail = L"安装包缺少战役扩展脚本资源";
        return g_info;
    }

    const std::vector<std::wstring> packs = PackCandidates(gameDir, explicitPack);
    g_diag += L"packs=" + std::to_wstring(packs.size()) + L"; ";
    for (size_t i = 0; i < packs.size(); ++i) {
        const size_t slash = packs[i].find_last_of(L"\\/");
        const std::wstring base = (slash == std::wstring::npos) ? packs[i] : packs[i].substr(slash + 1);
        HANDLE h = OpenRead(packs[i]);
        if (h == INVALID_HANDLE_VALUE) { g_diag += base + L":open_fail; "; continue; }
        LARGE_INTEGER li{};
        GetFileSizeEx(h, &li);
        const uint64_t fsize = static_cast<uint64_t>(li.QuadPart);
        Entry en{};
        uint8_t key[512] = {};
        const char* why = "nokey";
        int nentry = 0;
        const bool okKey = LoadKey(packs[i], fsize, key);
        if (okKey) nentry = ScanIndex(h, fsize, key, &en, &why);
        const bool okEnt = okKey && nentry > 0 && en.size > 0;
        g_diag += base + L":sz=" + std::to_wstring(fsize) + L",key=" +
                  (okKey ? L"1" : L"0") + L",ent=" + (okEnt ? L"1" : L"0") +
                  L",why=" + std::wstring(why, why + strlen(why)) +
                  L",n=" + std::to_wstring(nentry) +
                  L",off=" + std::to_wstring(en.off) + L"; ";
        CloseHandle(h);
        if (!okEnt || !en.size || en.off + en.size > fsize) continue;

        Region r;
        if (!LoadRegion(packs[i], en, key, &r)) continue;
        memcpy(g_key, key, 512);
        g_keyReady = true;
        g_info.path = packs[i];
        g_info.base = r.base;
        g_info.size = en.size;
        SaveLastPackPath(packs[i]);

        if (FindBytes(r.text, kTag) != kNPos) {
            g_info.state = kEnabled;
            g_info.detail = L"已启用 · 全科技底座 + 单抽/十连，其余技能局内自选";
        } else if (FindBytes(r.text, kMark) != kNPos) {
            // 旧版片段（V3 会开局授予全部技能）：不直接再插一份，
            // 交给 SetEnabled(true) 先还原再重打，避免两份脚本叠加。
            g_info.state = kOutdated;
            g_info.detail = L"旧版补丁 · 启动游戏时自动升级";
        } else if (Rewrite(r.text, g_snippet, nullptr)) {
            g_info.state = kOriginal;
            g_info.detail = L"原版 · 勾选后启用";
        } else {
            g_info.state = kMismatch;
            g_info.detail = L"数据包内容与本版本不匹配，已拒绝写入";
        }
        return g_info;
    }

    g_info.state = kNotFound;
    g_info.detail = L"未找到游戏数据包（可在本窗口手动指定）";
    return g_info;
}

bool SetEnabled(bool enable, std::wstring* msg) {
    if (!msg) return false;
    if (g_info.path.empty() || !g_keyReady || g_snippet.empty()) {
        *msg = L"未找到可用的游戏数据包，请先确认游戏安装位置";
        return false;
    }
    HANDLE h = OpenRead(g_info.path);
    if (h == INVALID_HANDLE_VALUE) { *msg = L"数据包打不开，请确认游戏安装位置"; return false; }
    LARGE_INTEGER li{};
    GetFileSizeEx(h, &li);
    const uint64_t fsize = static_cast<uint64_t>(li.QuadPart);
    Entry en{};
    const char* why = "?";
    if (ScanIndex(h, fsize, g_key, &en, &why) < 1 || !en.size) {
        CloseHandle(h);
        *msg = L"数据包索引已变化，请重新打开本窗口再试";
        return false;
    }
    CloseHandle(h);

    Region r;
    if (!LoadRegion(g_info.path, en, g_key, &r)) { *msg = L"读取脚本条目失败"; return false; }
    const bool hasTag  = FindBytes(r.text, kTag)  != kNPos;   // 当前版本
    const bool hasMark = FindBytes(r.text, kMark) != kNPos;   // 任意一代

    if (enable && hasTag) { *msg = L"战役扩展已处于启用状态"; return true; }
    if (!enable && !hasMark) { *msg = L"战役扩展已处于还原状态"; return true; }

    // 旧版片段升级：先回到干净文本，再用当前片段重新改写。
    // 不这么做会插入第二份脚本（旧版那份仍会开局授予全部技能）。
    std::vector<uint8_t> baseText = r.text;
    std::vector<uint8_t> basePlain = r.plain;
    if (enable && hasMark && !hasTag) {
        std::vector<uint8_t> restored;
        if (!TryExactRestore(r, &restored)) {
            *msg = L"旧版补丁无法按备份还原，已拒绝写入（请先取消勾选再重新勾选）";
            return false;
        }
        std::vector<uint8_t> plain = restored;
        Crypt(&plain[0], plain.size(), g_key, r.base, false);
        if (r.head + r.text.size() > plain.size()) {
            *msg = L"备份区域长度异常，已拒绝写入";
            return false;
        }
        basePlain = plain;
        baseText.assign(plain.begin() + r.head, plain.begin() + r.head + r.text.size());
    }

    std::vector<uint8_t> newCipher;
    if (enable) {
        std::vector<uint8_t> patched;
        if (!Rewrite(baseText, g_snippet, &patched)) {
            *msg = L"脚本结构与本版本不匹配，已拒绝写入（未改动任何文件）";
            return false;
        }
        SaveBackup(r, en);
        newCipher = basePlain;
        memcpy(&newCipher[r.head], &patched[0], patched.size());
        Crypt(&newCipher[0], newCipher.size(), g_key, r.base, true);
    } else {
        if (!TryExactRestore(r, &newCipher)) {
            // 没有可用备份时：先按精确字节找当前片段，找不到就按结构
            // 定位「紧贴锚点、含标记」的整段 do...end，空白化后等价原版。
            const size_t p = FindBytes(r.text, &g_snippet[0], g_snippet.size());
            if (p != kNPos) {
                for (size_t k = p; k < p + g_snippet.size(); ++k)
                    if (r.text[k] != '\r' && r.text[k] != '\n') r.text[k] = ' ';
            } else if (!WipeSnippet(&r.text)) {
                *msg = L"当前补丁不是本启动器写入的，且没有可用备份，无法还原";
                return false;
            }
            ReplaceEqualLen(&r.text, kUiTo, kUiFrom);
            newCipher = r.plain;
            memcpy(&newCipher[r.head], &r.text[0], r.text.size());
            Crypt(&newCipher[0], newCipher.size(), g_key, r.base, true);
        }
    }

    if (newCipher.size() != r.cipher.size()) { *msg = L"长度校验失败，未写入"; return false; }
    if (!SaveRegion(g_info.path, r.base, newCipher)) {
        *msg = L"写入失败：数据包正被游戏占用，请先退出游戏再操作";
        return false;
    }

    Region v;
    if (!LoadRegion(g_info.path, en, g_key, &v)) { *msg = L"写入后回读失败"; return false; }
    const bool nowTag = FindBytes(v.text, kTag) != kNPos;
    if (nowTag != enable) { *msg = L"回读校验不通过，请重新探测状态"; return false; }
    *msg = enable ? L"战役扩展已启用：全科技底座 + 单抽/十连，其余技能局内自选"
                  : L"战役扩展已还原，游戏数据包恢复原状";
    return true;
}

} // namespace cup
