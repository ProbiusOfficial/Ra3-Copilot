// battlefield.cpp - 全图战场枚举实现
//
// 只读扫描本进程（游戏进程）内存，按 Thing vtable 找所有对象：
//   Thing+0x04 -> 模板指针 ; 模板+0x08 -> 类型哈希
//   Thing+0x418 -> 归属(owner/Team 对象)
// 再用 battle_catalog 过滤出"可玩对象"（单位/建筑）并按 owner 分组。
//
// 线程：在 HTTP 线程执行（不调用任何游戏函数 → 跨线程安全）。
// 性能：只扫 MEM_PRIVATE 提交内存（本机 ~2.1GB，SSE2 加速，数百毫秒），结果带缓存。
#include "battlefield.h"
#include "battle_catalog.h"
#include "actions.h"
#include "features.h"
#include "overlay.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#ifdef _M_IX86_FP
#if _M_IX86_FP >= 2
#include <emmintrin.h>
#define BF_HAVE_SSE2 1
#endif
#endif
#ifndef BF_HAVE_SSE2
#define BF_HAVE_SSE2 0
#endif

namespace ra3 {
namespace {

const uint32_t kThingVtFallback = 0x00C4DB60;   // 1.12 日冕 实测（运行时优先从选中单位推导）
const int      kMaxOwners = 24;
const int      kMaxStacks = 48;                 // 每个 owner 记录的种类上限
const int      kMaxThings = 40000;              // 单次扫描收集的候选上限

// ---- 安全读（SEH）----
bool Rd32(uintptr_t at, uint32_t& v) {
    __try { v = *reinterpret_cast<const uint32_t*>(at); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- owner 累加器 ----
struct StackEnt { uint32_t hash; int count; };
struct OwnerAcc {
    uintptr_t addr;
    int  idx;
    int  units, structs, other, unknown;
    int  sideVotes[B_SIDE_COUNT];
    StackEnt st[kMaxStacks];
    int  nSt;
};

void AddStack(OwnerAcc& a, uint32_t hash, int count) {
    for (int i = 0; i < a.nSt; ++i)
        if (a.st[i].hash == hash) { a.st[i].count += count; return; }
    if (a.nSt < kMaxStacks) { a.st[a.nSt].hash = hash; a.st[a.nSt].count = count; ++a.nSt; }
}

OwnerAcc* FindOwner(OwnerAcc* arr, int& n, uintptr_t addr) {
    for (int i = 0; i < n; ++i) if (arr[i].addr == addr) return &arr[i];
    if (n >= kMaxOwners) return nullptr;
    OwnerAcc& a = arr[n++];
    memset(&a, 0, sizeof(a));
    a.addr = addr;
    a.idx = -1;
    return &a;
}

// ---- 扫描状态 ----
uintptr_t g_hits[kMaxThings];
int       g_nHits = 0;
uint32_t  g_thingVt = 0;
uint32_t  g_vtOverride = 0;      // /snapshot?vtab=0x.. 手动钉住（不需重编就能救）
char      g_note[160] = "";
int       g_scannedMb = 0;
uint32_t  g_elapsedMs = 0;
uint32_t  g_lastTick = 0;
BattleSummary g_cache;
bool      g_cacheValid = false;
DWORD     g_cacheTime = 0;
const DWORD kCacheTtlMs = 3000;

// 地址是否落在主模块映像内（vtable 判据）
bool InModule(uintptr_t p) {
    HMODULE m = GetModuleHandleA(nullptr);
    if (!m) return false;
    const uint8_t* img = reinterpret_cast<const uint8_t*>(m);
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(const_cast<uint8_t*>(img));
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(const_cast<uint8_t*>(img) + dos->e_lfanew);
    const uintptr_t base = reinterpret_cast<uintptr_t>(img);
    return p >= base && p < base + nt->OptionalHeader.SizeOfImage;
}

// 是否像一个 Team（owner）对象：指针合法且 vtable 在主模块内
bool LooksLikeTeam(uint32_t p) {
    if (!p || p < 0x00010000 || p >= 0x80000000) return false;
    uint32_t vt = 0;
    return Rd32(p, vt) && vt && InModule(vt);
}

// 运行时推导 Thing vtable：选中单位(Drawable) -> [d+0x138](Thing) -> vtable
// 必须校验"那真的是 Thing"：Thing+0x418 得是合法 Team 对象。
//   教训（2026-10-15 实机）：刚召唤出来的单位是"半初始化"的，它的 [d+0x138] 可能是垃圾
//   → 会推导出一个错误 vtable（实测 0x00C2E324）→ 全图扫描命中数暴跌 → 快照全空。
uint32_t DeriveThingVt() {
    if (g_vtOverride) return g_vtOverride;
    uintptr_t d = FirstSelectedObject();
    if (d) {
        uint32_t thing = 0, vt = 0, owner = 0;
        if (Rd32(d + 0x138, thing) && thing
            && Rd32(thing, vt) && vt && InModule(vt)
            && Rd32(thing + 0x418, owner) && LooksLikeTeam(owner))
            return vt;
        if (Rd32(d, vt) && vt && InModule(vt)          // 万一 d 本身就是 Thing
            && Rd32(d + 0x418, owner) && LooksLikeTeam(owner))
            return vt;
    }
    return kThingVtFallback;
}

// 找一个对齐 dword 序列：返回命中地址数（写入 g_hits）
#if BF_HAVE_SSE2
void ScanChunk(const uint32_t* p, const uint32_t* end, uint32_t vt) {
    const __m128i want = _mm_set1_epi32(static_cast<int>(vt));
    __try {
        while (p + 4 <= end) {
            __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
            int m = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(v, want)));
            while (m) {
                int b = 0;
                while (!(m & (1 << b))) ++b;
                if (g_nHits < kMaxThings) g_hits[g_nHits++] = reinterpret_cast<uintptr_t>(p + b);
                m &= m - 1;
            }
            p += 4;
        }
        while (p < end) {
            if (*p == vt && g_nHits < kMaxThings) g_hits[g_nHits++] = reinterpret_cast<uintptr_t>(p);
            ++p;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
#else
void ScanChunk(const uint32_t* p, const uint32_t* end, uint32_t vt) {
    __try {
        for (; p < end; ++p)
            if (*p == vt && g_nHits < kMaxThings) g_hits[g_nHits++] = reinterpret_cast<uintptr_t>(p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
#endif

// 全图扫描：收集所有 Thing 候选地址
void CollectThings(uint32_t vt) {
    g_nHits = 0;
    g_scannedMb = 0;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0x10000;
    while (addr < 0xFFF00000) {
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const size_t size = mbi.RegionSize;
        const DWORD prot = mbi.Protect;
        // 只扫堆（MEM_PRIVATE 的提交页）：对象都在堆里，跳过 IMAGE/MAPPED 省时间
        const bool ok = (mbi.State == MEM_COMMIT) && (mbi.Type == MEM_PRIVATE)
                        && !(prot & PAGE_GUARD) && !(prot & PAGE_NOACCESS)
                        && (prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
        if (ok) {
            g_scannedMb += static_cast<int>(size >> 20);
            // 64KB 分块（一块出错不拖累整段）
            const size_t kChunk = 0x10000;
            for (size_t off = 0; off < size; off += kChunk) {
                size_t n = (size - off < kChunk) ? (size - off) : kChunk;
                n &= ~static_cast<size_t>(3);
                if (!n) continue;
                ScanChunk(reinterpret_cast<const uint32_t*>(base + off),
                          reinterpret_cast<const uint32_t*>(base + off + n), vt);
            }
        }
        if (!size) break;
        addr = base + size;
    }
}

} // namespace

// ===================== 对外 =====================

bool ScanBattlefield(BattleSummary* out, bool fresh) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    const DWORD now = GetTickCount();
    if (!fresh && g_cacheValid && (now - g_cacheTime) < kCacheTtlMs) {
        *out = g_cache;
        out->fromCache = true;
        return out->owners > 0;
    }

    const DWORD t0 = GetTickCount();
    uint32_t vt = DeriveThingVt();
    CollectThings(vt);
    // 命中数兜底：候选 vtable 若是错的（典型：选中了刚召唤出来的半初始化单位），
    //   命中数会从数千暴跌到几十 → 用已知常量重扫，取命中更多的那个。
    if (vt != kThingVtFallback && g_nHits < 200) {
        const int a = g_nHits;
        CollectThings(kThingVtFallback);
        if (g_nHits > a) vt = kThingVtFallback;
    }
    g_thingVt = vt;

    // 分组累加
    static OwnerAcc owners[kMaxOwners];
    int nOwners = 0;
    const uintptr_t localOwner = LocalPlayerObj();
    int totalPlayable = 0, totalUnknown = 0;

    for (int i = 0; i < g_nHits; ++i) {
        const uintptr_t t = g_hits[i];
        uint32_t owner = 0;
        if (!Rd32(t + 0x418, owner) || owner < 0x10000) continue;

        uint32_t tmpl = 0, hash = 0;
        if (!Rd32(t + 0x04, tmpl) || tmpl < 0x10000) continue;
        if (!Rd32(tmpl + 0x08, hash)) continue;

        OwnerAcc* a = FindOwner(owners, nOwners, owner);
        if (!a) continue;
        if (a->idx < 0 && nOwners > 0) {
            uint32_t ix = 0;
            if (Rd32(owner + 0x20, ix) && ix < 32) a->idx = static_cast<int>(ix);
        }

        const BattleObj* o = BattleLookup(hash);
        if (!o) { ++a->unknown; ++totalUnknown; continue; }

        ++totalPlayable;
        if (o->cat == 0)      ++a->units;
        else if (o->cat == 1) ++a->structs;
        else                  ++a->other;
        if (o->side < B_SIDE_COUNT) ++a->sideVotes[o->side];
        AddStack(*a, o->hash, 1);
    }

    // 组装
    out->owners = nOwners;
    out->totalPlayable = totalPlayable;
    out->totalUnknown = totalUnknown;
    out->localUnits = out->localStructs = 0;
    out->enemyUnits = out->enemyStructs = 0;
    out->tickNow = g_tickNow;
    out->fromCache = false;

    // 排序：可玩对象多的在前
    int order[kMaxOwners];
    for (int i = 0; i < nOwners; ++i) order[i] = i;
    for (int i = 0; i < nOwners; ++i)
        for (int j = i + 1; j < nOwners; ++j) {
            const int pi = owners[order[i]].units + owners[order[i]].structs + owners[order[i]].other;
            const int pj = owners[order[j]].units + owners[order[j]].structs + owners[order[j]].other;
            if (pj > pi) { int tmp = order[i]; order[i] = order[j]; order[j] = tmp; }
        }

    int slot = 0;
    for (int k = 0; k < nOwners && slot < 16; ++k) {
        OwnerAcc& a = owners[order[k]];
        BattleOwner& b = out->list[slot++];
        b.addr = a.addr;
        b.idx = a.idx;
        b.isLocal = (a.addr == localOwner);
        b.units = a.units;
        b.structs = a.structs;
        b.playable = a.units + a.structs + a.other;
        b.unknown = a.unknown;
        // 多数派阵营（忽略中立/平民票）
        int best = -1, bestV = 0;
        for (int s = 0; s < B_SIDE_COUNT; ++s)
            if (a.sideVotes[s] > bestV) { bestV = a.sideVotes[s]; best = s; }
        b.side = best;
        b.topN = 0;
        // 取数量最多的前 10
        for (int i = 0; i < a.nSt && b.topN < 10; ++i) {
            int bi = i;
            for (int j = i + 1; j < a.nSt; ++j) if (a.st[j].count > a.st[bi].count) bi = j;
            StackEnt tmp = a.st[i]; a.st[i] = a.st[bi]; a.st[bi] = tmp;   // 就地选择排序（acc 是本地副本语义，可改）
            const BattleObj* o = BattleLookup(a.st[i].hash);
            b.top[b.topN].hash = a.st[i].hash;
            b.top[b.topN].name = o ? o->name : "?";
            b.top[b.topN].count = a.st[i].count;
            ++b.topN;
        }
        if (b.isLocal) { out->localUnits = b.units; out->localStructs = b.structs; }
        else { out->enemyUnits += b.units; out->enemyStructs += b.structs; }
    }

    g_elapsedMs = GetTickCount() - t0;
    out->elapsedMs = g_elapsedMs;
    out->scannedMb = g_scannedMb;
    snprintf(g_note, sizeof(g_note), "vt=%08X scanned=%dMB things=%d owners=%d %ums",
             (unsigned)g_thingVt, g_scannedMb, g_nHits, nOwners, (unsigned)g_elapsedMs);

    g_cache = *out;
    g_cacheValid = true;
    g_cacheTime = GetTickCount();
    g_lastTick = g_tickNow;
    return true;
}

// ===================== 活体对象详情（MCP 感知层自动化）=====================

namespace {
// 安全读 float（坏值过滤：NaN/Inf 归零，避免污染 JSON）
bool Rdf(uintptr_t at, float& v) {
    uint32_t raw = 0;
    if (!Rd32(at, raw)) return false;
    float f = *reinterpret_cast<const float*>(&raw);
    if (!(f == f) || f > 1e9f || f < -1e9f) { v = 0.f; return true; }
    v = f;
    return true;
}
} // namespace

bool ObjectDetail(uintptr_t t, BattleLiveObj* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!t || t < 0x10000 || t >= 0xFFF00000) return false;

    uint32_t vt = 0, owner = 0, tmpl = 0, hash = 0;
    if (!Rd32(t, vt) || !InModule(vt)) return false;          // 必须是主模块内的对象
    if (!Rd32(t + 0x418, owner) || owner < 0x10000) return false;
    if (!Rd32(t + 0x04, tmpl) || tmpl < 0x10000) return false;
    if (!Rd32(tmpl + 0x08, hash)) return false;

    out->thing = t;
    out->owner = owner;
    out->hash  = hash;
    uint32_t ix = 0;
    out->ownerIdx = (Rd32(owner + 0x20, ix) && ix < 64) ? static_cast<int>(ix) : -1;

    const BattleObj* o = BattleLookup(hash);
    out->name = o ? o->name : "?";
    out->side = o ? o->side : -1;
    out->cls  = o ? o->cls  : -1;
    out->cat  = o ? o->cat  : -1;

    uint32_t p = 0;
    if (Rd32(t + 0x374, p)) out->ai = p;                       // AIUpdate：命令入口 this
    if (Rd32(t + 0x33c, p)) {                                  // 血量体
        out->body = p;
        if (p) { Rdf(p + 0x04, out->hp); Rdf(p + 0x0c, out->hpMax); }
    }
    Rdf(t + 0x38, out->x);
    Rdf(t + 0x3c, out->y);
    Rdf(t + 0x40, out->z);

    out->isLocal = (owner == LocalPlayerObj());
    return true;
}

int ScanLiveObjects(BattleLiveObj* out, int max, const char* q,
                    int ownerIdx, int cls, int cat, bool fresh) {
    if (!out || max <= 0) return 0;
    if (fresh) g_cacheValid = false;
    BattleSummary s;
    ScanBattlefield(&s, false);       // 复用扫描（g_hits 已填；带 3s 缓存）
    if (g_nHits <= 0) return 0;

    const size_t qlen = (q && q[0]) ? strlen(q) : 0;
    int n = 0;
    for (int i = 0; i < g_nHits && n < max; ++i) {
        BattleLiveObj o;
        if (!ObjectDetail(g_hits[i], &o)) continue;
        if (ownerIdx >= 0 && o.ownerIdx != ownerIdx) continue;
        if (cls >= 0 && o.cls != cls) continue;
        if (cat >= 0 && o.cat != cat) continue;
        if (qlen) {
            char hex[16];
            snprintf(hex, sizeof(hex), "%08X", o.hash);
            const bool nameHit = o.name && strstr(o.name, q) != nullptr;
            const bool hashHit = (_strnicmp(hex, q, qlen) == 0);
            if (!nameHit && !hashHit) continue;
        }
        out[n++] = o;
    }
    return n;
}

bool LastBattleSummary(BattleSummary* out) {    if (!g_cacheValid || !out) return false;
    *out = g_cache;
    out->fromCache = true;
    return true;
}

int LocalUnitCount() {
    BattleSummary s;
    if (!ScanBattlefield(&s, false)) return -1;
    return s.localUnits + s.localStructs;
}

int EnemyUnitCount() {
    BattleSummary s;
    if (!ScanBattlefield(&s, false)) return -1;
    return s.enemyUnits + s.enemyStructs;
}

void BattlefieldInvalidate() { g_cacheValid = false; }

// 按玩家索引找队伍对象地址（用缓存的枚举结果；idx 见 /snapshot 的 owners[].idx）
uintptr_t FindTeamByIdx(int idx) {
    BattleSummary s;
    if (!ScanBattlefield(&s, false)) return 0;
    for (int i = 0; i < s.owners && i < 16; ++i)
        if (s.list[i].idx == idx) return s.list[i].addr;
    return 0;
}

// 手动钉住 Thing vtable（/snapshot?vtab=0x..）—— 免重编的热修
void SetThingVtOverride(uint32_t vt) {
    g_vtOverride = vt;
    g_cacheValid = false;
}
uint32_t ThingVtOverride() { return g_vtOverride; }

// ---- 后台刷新：面板请求 → HTTP 线程执行（不在渲染线程里扫内存，避免卡顿）----
static volatile LONG g_scanReq = 0;

void BattlefieldRequestScan() { InterlockedExchange(&g_scanReq, 1); }

void BattlefieldPump() {
    if (InterlockedExchange(&g_scanReq, 0) == 0) return;
    BattleSummary s;
    ScanBattlefield(&s, false);         // 走 TTL：请求再密也不会比 3s 更频繁地真扫
}

uint32_t BattleThingVtable() { return g_thingVt ? g_thingVt : kThingVtFallback; }
const char* BattleScanNote() { return g_note; }

} // namespace ra3
