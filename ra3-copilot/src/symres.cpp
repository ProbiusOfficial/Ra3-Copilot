// symres.cpp - 符号解析实现（进程内扫描主模块内存）
//
// 解析顺序（每个符号）：
//   ① 特征码全模块扫描 → 唯一命中直接用；
//   ② 多命中 → 取"离 1.12 已知 RVA 最近"的那个（版本微调时地址不会跑远）；
//   ③ 0 命中 → 退回已知 RVA（但要求该处字节仍像函数入口，否则标记失败）
// 全局变量（ThingFactory / 选中管理器）用**结构推导**，不靠固定地址：
//   · ThingFactory：先解析出 TmplFind，再找 `8B 0D <imm32> E8 <rel32→TmplFind>` 的代码，
//     取出现次数最多的 imm32（本机 1.12 实测 110 处代码一致指向 0xCE6C58）。
//   · 选中管理器：扫 .data 找满足 [V+0x5c]=1..64 且 [V+0x50] 链表首对象(及其 body)的
//     **vtable 落在主模块内** 的候选，多候选取离已知 RVA 最近者（本机实测仅 2 个候选）。
#include "symres.h"
#include "overlay.h"
#include <windows.h>
#include <cstdio>
#include <cstring>

namespace ra3 {
namespace {

uint8_t* g_img = nullptr;
size_t   g_imgSize = 0;

// 1.12 已知值（仅作"就近选择"参考与兜底）
constexpr uint32_t R_CREATEUNIT = 0x205240;
constexpr uint32_t R_LEVELUP    = 0x35C200;
constexpr uint32_t R_KILLUNIT   = 0x39EA50;
constexpr uint32_t R_TMPLFIND   = 0x3E4230;
constexpr uint32_t R_MOUSEMAP   = 0x1ED4A0;
constexpr uint32_t R_THINGFACT  = 0x8E6C58;
constexpr uint32_t R_SELMGR     = 0x8DB73C;
constexpr uint32_t R_FREEBUILD  = 0x0E7565;

// ---- 特征码（取自 1.12 实测字节；'x'=精确, '?'=通配）----
const uint8_t SIG_CREATE[] = { 0x8B,0x4C,0x24,0x08,0x83,0xEC,0x3C,0x53,0x33,0xDB,0x33,0xC0,0x3B,0xCB,0x0F,0x84 };
const char    MSK_CREATE[] = "xxxxxxxxxxxxxxxx";
const uint8_t SIG_LEVELUP[] = { 0x55,0x8B,0x6C,0x24,0x08,0x85,0xED,0x57,0x8B,0xF9,0x7F,0x07,0x5F,0x32,0xC0,0x5D,0xC2,0x0C,0x00,0x53 };
const char    MSK_LEVELUP[] = "xxxxxxxxxxxxxxxxxxxx";
const uint8_t SIG_KILL[] = { 0x81,0xEC,0x9C,0x00,0x00,0x00,0x56,0x8B,0xF1,0x8D,0x4C,0x24,0x08,0xC7,0x44,0x24,0x04,0x08,0x95,0xBE };
const char    MSK_KILL[] = "xxxxxxxxxxxxxxxxxxxx";
const uint8_t SIG_FIND[] = { 0x83,0xEC,0x08,0x56,0x8D,0x71,0x28,0x8D,0x44,0x24,0x10,0x50,0x8D,0x4C,0x24,0x08,0x51,0x8B,0xCE,0xE8 };
const char    MSK_FIND[] = "xxxxxxxxxxxxxxxxxxxx";
const uint8_t SIG_MOUSE[] = { 0x81,0xEC,0xD0,0x00,0x00,0x00,0x83,0xBC,0x24,0xD4,0x00,0x00,0x00,0x00 };
const char    MSK_MOUSE[] = "xxxxxxxxxxxxxx";
const uint8_t SIG_FREE[] = { 0x75,0x0C,0xB8,0x02,0x00,0x00,0x00,0x5B,0x83,0xC4,0x0C,0xC2,0x10,0x00,0x33,0xC0,0x38,0x44,0x24,0x07 };
const char    MSK_FREE[] = "xxxxxxxxxxxxxxxxxxxx";

const int kMaxHits = 64;

int ScanAll(const uint8_t* sig, const char* mask, int len, uintptr_t* out, int maxOut) {
    if (!g_img || len <= 0) return 0;
    int n = 0;
    const size_t lim = g_imgSize - (size_t)len;
    for (size_t i = 0; i < lim; ++i) {
        bool ok = true;
        for (int k = 0; k < len; ++k)
            if (mask[k] == 'x' && g_img[i + k] != sig[k]) { ok = false; break; }
        if (ok) { if (n < maxOut) out[n] = reinterpret_cast<uintptr_t>(g_img + i); ++n; }
    }
    return n;
}

// 多命中 → 选离 fallback 最近的
uintptr_t PickNearest(uintptr_t* hits, int n, uint32_t rvaFallback) {
    if (n <= 0) return 0;
    const uintptr_t want = reinterpret_cast<uintptr_t>(g_img) + rvaFallback;
    uintptr_t best = hits[0];
    uintptr_t bestD = (hits[0] > want) ? hits[0] - want : want - hits[0];
    for (int i = 1; i < n; ++i) {
        uintptr_t d = (hits[i] > want) ? hits[i] - want : want - hits[i];
        if (d < bestD) { best = hits[i]; bestD = d; }
    }
    return best;
}

// 该地址处是否像函数入口
bool LooksLikeFuncEntry(uintptr_t a) {
    if (!a) return false;
    __try {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(a);
        const uint8_t b0 = p[0], b1 = p[1];
        if (b0 == 0x55) return true;
        if (b0 == 0x83 && b1 == 0xEC) return true;
        if (b0 == 0x81 && b1 == 0xEC) return true;
        if (b0 == 0x56 || b0 == 0x57 || b0 == 0x53 || b0 == 0x51) return true;
        if (b0 == 0x8B && b1 == 0xFF) return true;
        if (b0 == 0x6A && b1 == 0xFF) return true;
        if (b0 == 0xE9 || b0 == 0xFF) return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// 结构推导 ThingFactory：找 `8B 0D <imm32> E8 <rel32→find>`，取出现次数最多的 imm32
uintptr_t DeriveThingFactory(uintptr_t findAddr, int* outHits) {
    if (outHits) *outHits = 0;
    if (!g_img || !findAddr) return 0;
    uint32_t best = 0; int bestCnt = 0;
    struct { uint32_t g; int c; } cand[16] = {};
    int nCand = 0;
    const size_t lim = g_imgSize - 11;
    for (size_t i = 0; i < lim; ++i) {
        if (g_img[i] != 0x8B || g_img[i + 1] != 0x0D || g_img[i + 6] != 0xE8) continue;
        int32_t rel;
        std::memcpy(&rel, g_img + i + 7, 4);
        const uintptr_t tgt = reinterpret_cast<uintptr_t>(g_img + i + 11) + rel;
        if (tgt != findAddr) continue;
        uint32_t g;
        std::memcpy(&g, g_img + i + 2, 4);
        int k = 0;
        for (; k < nCand; ++k) if (cand[k].g == g) { ++cand[k].c; break; }
        if (k == nCand && nCand < 16) { cand[nCand].g = g; cand[nCand].c = 1; ++nCand; }
    }
    for (int i = 0; i < nCand; ++i) if (cand[i].c > bestCnt) { bestCnt = cand[i].c; best = cand[i].g; }
    if (outHits) *outHits = bestCnt;
    if (bestCnt < 3) return 0;           // 命中太少说明不是真 Find，不可信
    if (best < 0x400000 || best > 0x400000 + g_imgSize) return 0;
    return reinterpret_cast<uintptr_t>(g_img) + (best - 0x400000);   // 返回"全局变量的地址"
}

// 结构推导 选中管理器（扫 .data）
uintptr_t DeriveSelMgr() {
    if (!g_img) return 0;
    const uintptr_t modLo = reinterpret_cast<uintptr_t>(g_img);
    const uintptr_t modHi = modLo + g_imgSize;
    auto inMod = [&](uint32_t v) { return v >= (uint32_t)modLo && v < (uint32_t)modHi; };
    auto rd32 = [&](uintptr_t a, uint32_t& out) {
        __try { out = *reinterpret_cast<uint32_t*>(a); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    };
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_img);
    IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_img + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t cands[16] = {}; int nCand = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
        const uint32_t rva = sec[s].VirtualAddress;
        const uint32_t vsz = sec[s].Misc.VirtualSize;
        if (rva != 0x8AB000 && rva != 0x8A0000 && !(rva >= 0x8A0000 && rva < 0x940000)) continue;
        const size_t lim = vsz / 4;
        const uint32_t* p = reinterpret_cast<const uint32_t*>(g_img + rva);
        for (size_t i = 0; i + 1 < lim; ++i) {
            const uint32_t v = p[i];
            if (v < 0x01000000 || v >= 0x80000000 || inMod(v)) continue;
            uint32_t cnt = 0, node = 0, mgrVt = 0;
            if (!rd32(v + 0x5c, cnt) || cnt < 1 || cnt > 64) continue;
            // ★ 管理器自身 vtable 必须在主模块内 —— 排除堆上"形状相似"的假对象
            //   （2026-10-14 教训：缺这一条时误命中一个堆对象，导致所有选中单位操作失效）
            if (!rd32(v, mgrVt) || !inMod(mgrVt)) continue;
            if (!rd32(v + 0x50, node) || !node) continue;
            uint32_t obj = 0;
            if (!rd32(node + 8, obj) || !obj || inMod(obj)) continue;
            uint32_t vt = 0;
            if (!rd32(obj, vt) || !vt || !inMod(vt)) continue;
            uint32_t body = 0;
            if (!rd32(obj + 0x138, body) || !body || inMod(body)) continue;
            uint32_t bvt = 0;
            if (!rd32(body, bvt) || !bvt || !inMod(bvt)) continue;
            if (nCand < 16) cands[nCand++] = modLo + rva + (uintptr_t)i * 4;
        }
    }
    if (nCand == 0) return 0;
    // 多候选取离 1.12 已知 RVA 最近者
    return PickNearest(cands, nCand, R_SELMGR);
}

// ---- 缓存与来源 ----
struct Slot { uintptr_t addr; const char* how; };
Slot s_create = { 0, "" }, s_level = { 0, "" }, s_kill = { 0, "" }, s_find = { 0, "" };
Slot s_mouse = { 0, "" }, s_fact = { 0, "" }, s_sel = { 0, "" }, s_free = { 0, "" };
bool g_inited = false;

uintptr_t ResolveBySig(const uint8_t* sig, const char* mask, int len,
                       uint32_t rvaFallback, Slot& slot, const char* name) {
    uintptr_t hits[kMaxHits];
    const int n = ScanAll(sig, mask, len, hits, kMaxHits);
    if (n == 1) { slot.addr = hits[0]; slot.how = "AOB(唯一)"; return slot.addr; }
    if (n > 1)  { slot.addr = PickNearest(hits, n, rvaFallback); slot.how = "AOB(就近)"; return slot.addr; }
    // 兜底：已知 RVA + 入口形态校验
    const uintptr_t at = reinterpret_cast<uintptr_t>(g_img) + rvaFallback;
    if (LooksLikeFuncEntry(at)) { slot.addr = at; slot.how = "RVA兜底(已校验)"; }
    else { slot.addr = 0; slot.how = "解析失败"; OverlayLogLine(OVLOG_ERR, name); }
    return slot.addr;
}

} // namespace

void SymInit() {
    if (g_inited) return;
    HMODULE m = GetModuleHandleA(nullptr);
    if (!m) return;
    g_img = reinterpret_cast<uint8_t*>(m);
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_img);
    IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_img + dos->e_lfanew);
    g_imgSize = nt->OptionalHeader.SizeOfImage;
    g_inited = true;

    char buf[176];
    snprintf(buf, sizeof(buf), "[符号] 主模块 %p size=%X，开始解析…", g_img, (unsigned)g_imgSize);
    OverlayLogLine(OVLOG_INFO, buf);

    ResolveBySig(SIG_CREATE, MSK_CREATE, sizeof(SIG_CREATE), R_CREATEUNIT, s_create, "创建单位解析失败");
    ResolveBySig(SIG_LEVELUP, MSK_LEVELUP, sizeof(SIG_LEVELUP), R_LEVELUP, s_level, "单位升级解析失败");
    ResolveBySig(SIG_KILL, MSK_KILL, sizeof(SIG_KILL), R_KILLUNIT, s_kill, "击杀单位解析失败");
    ResolveBySig(SIG_MOUSE, MSK_MOUSE, sizeof(SIG_MOUSE), R_MOUSEMAP, s_mouse, "鼠标地图坐标解析失败");
    ResolveBySig(SIG_FREE, MSK_FREE, sizeof(SIG_FREE), R_FREEBUILD, s_free, "免费建造站点解析失败");

    // TmplFind 有两个同形函数 → 用"ThingFactory 推导命中数"来判别真身
    {
        uintptr_t hits[kMaxHits];
        const int n = ScanAll(SIG_FIND, MSK_FIND, sizeof(SIG_FIND), hits, kMaxHits);
        uintptr_t bestFind = 0, bestFact = 0; int bestCnt = 0;
        for (int i = 0; i < n; ++i) {
            int cnt = 0;
            uintptr_t f = DeriveThingFactory(hits[i], &cnt);
            if (cnt > bestCnt) { bestCnt = cnt; bestFind = hits[i]; bestFact = f; }
        }
        if (bestFind) { s_find.addr = bestFind; s_find.how = "AOB+结构"; s_fact.addr = bestFact; s_fact.how = "结构推导"; }
        else ResolveBySig(SIG_FIND, MSK_FIND, sizeof(SIG_FIND), R_TMPLFIND, s_find, "模板查找解析失败");
        snprintf(buf, sizeof(buf), "[符号] 模板查找=%08X(%s)  ThingFactory=%08X(%s,推导命中%d)",
                 (unsigned)s_find.addr, s_find.how, (unsigned)s_fact.addr, s_fact.how, bestCnt);
        OverlayLogLine(OVLOG_OK, buf);
    }
    if (!s_fact.addr) { s_fact.addr = reinterpret_cast<uintptr_t>(g_img) + R_THINGFACT; s_fact.how = "RVA兜底"; }

    // 选中管理器：★ 优先「RVA + 结构验证」（CE 表逐字节验证过，对本构建可靠）；
    //   只有验证失败才退回"扫 .data 结构推导"（面向未知版本/其它 mod）。
    //   —— 2026-10-14 教训：扫 .data 曾误命中毒对象 → 所有"选中单位操作"（收编/血量/速度/满级/摧毁）全部失效。
    {
        const uintptr_t rvaSel = reinterpret_cast<uintptr_t>(g_img) + R_SELMGR;
        const uint32_t modLo = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_img));
        const uint32_t modHi = modLo + static_cast<uint32_t>(g_imgSize);
        uint32_t mgr = 0, vt = 0;
        __try { mgr = *reinterpret_cast<uint32_t*>(rvaSel); } __except (EXCEPTION_EXECUTE_HANDLER) { mgr = 0; }
        if (mgr) { __try { vt = *reinterpret_cast<uint32_t*>(mgr); } __except (EXCEPTION_EXECUTE_HANDLER) { vt = 0; } }
        const bool rvaOk = mgr && vt >= modLo && vt < modHi;
        if (rvaOk) { s_sel.addr = rvaSel; s_sel.how = "RVA(已验证)"; }
        else {
            s_sel.addr = DeriveSelMgr();
            if (s_sel.addr) s_sel.how = "结构推导(扫.data)";
            else { s_sel.addr = rvaSel; s_sel.how = "RVA兜底(未验证)"; }
        }
    }

    snprintf(buf, sizeof(buf), "[符号] 创建=%08X(%s) 升级=%08X(%s) 击杀=%08X(%s)",
             (unsigned)s_create.addr, s_create.how, (unsigned)s_level.addr, s_level.how,
             (unsigned)s_kill.addr, s_kill.how);
    OverlayLogLine(s_create.addr ? OVLOG_OK : OVLOG_ERR, buf);
    snprintf(buf, sizeof(buf), "[符号] 选中管理器=%08X(%s)  免费建造站点=%08X(%s)",
             (unsigned)s_sel.addr, s_sel.how, (unsigned)s_free.addr, s_free.how);
    OverlayLogLine(OVLOG_OK, buf);
}

uintptr_t SymCreateUnit() { return s_create.addr; }
uintptr_t SymLevelUp()    { return s_level.addr; }
uintptr_t SymKillUnit()   { return s_kill.addr; }
uintptr_t SymTmplFind()   { return s_find.addr; }
uintptr_t SymMouseMap()   { return s_mouse.addr; }
uintptr_t SymThingFactory() { return s_fact.addr; }
uintptr_t SymSelMgr()     { return s_sel.addr; }
uintptr_t SymFreeBuildSite() { return s_free.addr; }

const char* SymHow(const char* name) {
    if (!name) return "";
    if (strcmp(name, "create") == 0) return s_create.how;
    if (strcmp(name, "level") == 0)  return s_level.how;
    if (strcmp(name, "kill") == 0)   return s_kill.how;
    if (strcmp(name, "find") == 0)   return s_find.how;
    if (strcmp(name, "mouse") == 0)  return s_mouse.how;
    if (strcmp(name, "fact") == 0)   return s_fact.how;
    if (strcmp(name, "sel") == 0)    return s_sel.how;
    if (strcmp(name, "free") == 0)   return s_free.how;
    return "";
}

} // namespace ra3
