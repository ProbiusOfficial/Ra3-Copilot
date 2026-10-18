// protocol.cpp - 秘密协议：协议点 / 全协议 / 授予（实验）
#include "protocol.h"
#include "features.h"
#include "protocols_corona.h"
#include "aicmd.h"

#include <windows.h>
#include "MinHook.h"
#include <cstdio>
#include <cstring>

namespace ra3 {

namespace {

const char* g_lastErr = "";

uintptr_t GameBase() { return reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)); }

bool Rd32(uintptr_t at, uint32_t& v) {
    __try { v = *reinterpret_cast<uint32_t*>(at); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool Wr32(uintptr_t at, uint32_t v) {
    __try { *reinterpret_cast<uint32_t*>(at) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool WrF32(uintptr_t at, float v) {
    __try { *reinterpret_cast<float*>(at) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// 本地玩家经济对象：Money hook 看到的那只（+0x34 协议点 / +0x2C 科技点）
uintptr_t PlayerObj() { return static_cast<uintptr_t>(g_moneyObj); }

// LuaScriptEngine 对象 → +0x24 = lua_State*
uintptr_t LuaState() {
    uint32_t eng = 0;
    if (!Rd32(0x00CDD17C, eng) || !eng) return 0;
    uint32_t L = 0;
    if (!Rd32(eng + 0x24, L) || !L) return 0;
    return L;
}

// 引擎自带 Lua 的 cdecl 入口（RVA；对 1.12 日冕实机验证过可调用）
typedef void (__cdecl* SetTop_t)(uintptr_t, int);
typedef int  (__cdecl* PushStr_t)(uintptr_t, const char*);
typedef int  (__cdecl* Exec_t)(uintptr_t);
typedef int  (__cdecl* Top_t)(uintptr_t);

int LuaAction(const char* action, const char* a1, const char* a2, const char* a3) {
    const uintptr_t L = LuaState();
    if (!L) { g_lastErr = "lua_State 不可用（先进对局）"; return -1; }
    const uintptr_t base = GameBase();
    SetTop_t settop = reinterpret_cast<SetTop_t>(base + 0xA3E0);
    PushStr_t push  = reinterpret_cast<PushStr_t>(base + 0xA980);
    Exec_t exec     = reinterpret_cast<Exec_t>(base + 0x182C50);
    Top_t gettop    = reinterpret_cast<Top_t>(base + 0xA3D0);

    __try {
        // 只在 Lua 栈为空时操作；非空说明游戏正在用这个 state，清栈会把它弄挂。
        if (gettop(L) != 0) {
            g_lastErr = "Lua 栈非空，已跳过（防卡死）";
            return -1;
        }
        settop(L, 0);
        if (!push(L, action)) { settop(L, 0); g_lastErr = "push action 失败"; return -1; }
        if (a1) push(L, a1);
        if (a2) push(L, a2);
        if (a3) push(L, a3);
        // 动作执行前把脚本引擎的“当前对象”指向本地玩家；<This Object> 才会解析到它。
        const uintptr_t pl = PlayerObj();
        if (pl) {
            uint32_t eng = 0;
            if (Rd32(0x00CDD190, eng) && eng) {
                Wr32(eng + 0x17D68, static_cast<uint32_t>(pl));
                Wr32(eng + 0x17D70, static_cast<uint32_t>(pl));
            }
        }
        const int r = exec(L);
        settop(L, 0);
        return r;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_lastErr = "Lua 调用异常";
        __try { settop(L, 0); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return -1;
    }
}

} // namespace

int ProtocolCount() { return kProtocolCount; }

const char* ProtocolFaction(int idx) {
    return (idx >= 0 && idx < kProtocolCount) ? kProtocols[idx].faction : "";
}
const char* ProtocolName(int idx) {
    return (idx >= 0 && idx < kProtocolCount) ? kProtocols[idx].name : "";
}
const char* ProtocolTech(int idx) {
    return (idx >= 0 && idx < kProtocolCount) ? kProtocols[idx].tech : "";
}
const char* ProtocolUpgrade(int idx) {
    return (idx >= 0 && idx < kProtocolCount) ? kProtocols[idx].upgrade : "";
}
const char* ProtocolPower(int idx) {
    return (idx >= 0 && idx < kProtocolCount) ? kProtocols[idx].power : "";
}

// 协议点不在 +0x34（+0x34 是对象指针）；买协议时真实扣减的是 +0xA44
static constexpr uintptr_t kPlayerProtocolPoints = 0xA44;

int ProtocolPoints() {
    const uintptr_t p = PlayerObj();
    uint32_t v = 0;
    return (p && Rd32(p + kPlayerProtocolPoints, v)) ? static_cast<int>(v) : -1;
}

float ProtocolScience() {
    const uintptr_t p = PlayerObj();
    if (!p) return 0.f;
    __try { return *reinterpret_cast<float*>(p + 0x2C); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0.f; }
}

bool ProtocolSetPoints(int n) {
    const uintptr_t p = PlayerObj();
    if (!p) { g_lastErr = "玩家对象不可用（先进对局）"; return false; }
    return Wr32(p + kPlayerProtocolPoints, static_cast<uint32_t>(n));
}

bool ProtocolSetScience(float v) {
    const uintptr_t p = PlayerObj();
    if (!p) { g_lastErr = "玩家对象不可用（先进对局）"; return false; }
    return WrF32(p + 0x2C, v);
}

bool ProtocolSetToggle(int which, bool on) {
    switch (which) {
    case 0: SetFeature(FID_HAVEALLSC, on); return true;
    case 1: SetFeature(FID_SUPERPOWER, on); return true;
    case 2: SetFeature(FID_DISABLESP, on); return true;
    default: return false;
    }
}

bool ProtocolGetToggle(int which) {
    switch (which) {
    case 0: return GetFeature(FID_HAVEALLSC);
    case 1: return GetFeature(FID_SUPERPOWER);
    case 2: return GetFeature(FID_DISABLESP);
    default: return false;
    }
}

int ProtocolGrant(int idx, const char* player, char* out, size_t outsz) {
    if (idx < 0 || idx >= kProtocolCount) { g_lastErr = "协议序号越界"; return 0; }
    const char* pl = (player && player[0]) ? player : "<This Object>";
    const uintptr_t base = GameBase();
    const char* actLock  = reinterpret_cast<const char*>(base + 0x7F1D54);
    const char* actGrant = reinterpret_cast<const char*>(base + 0x7F09C8);

    int r1 = 1, r2 = 1;
    if (kProtocols[idx].tech[0])
        r1 = LuaAction(actLock, pl, kProtocols[idx].tech, "");
    if (kProtocols[idx].power[0])
        r2 = LuaAction(actGrant, kProtocols[idx].power, pl, nullptr);

    if (out && outsz) {
        _snprintf_s(out, outsz, _TRUNCATE,
                    "授予[%s] tech=%d power=%d player=%s", kProtocols[idx].name, r1, r2, pl);
    }
    return (r1 >= 0 && r2 >= 0) ? 1 : 0;
}

int ProtocolGrantLocal(int idx, char* out, size_t outsz) {
    uint32_t eng = 0;
    if (!Rd32(0x00CDD190, eng) || !eng) { g_lastErr = "脚本引擎不可用"; return 0; }
    const uintptr_t p = PlayerObj();
    if (!p) { g_lastErr = "本地玩家对象不可用"; return 0; }
    if (!Wr32(eng + 0x17D68, static_cast<uint32_t>(p)) ||
        !Wr32(eng + 0x17D70, static_cast<uint32_t>(p))) {
        g_lastErr = "写当前对象上下文失败";
        return 0;
    }
    return ProtocolGrant(idx, "<This Object>", out, outsz);
}

// ---- 直接按 PlayerTech hash 追加 TechNode（行为验证过的授予路径） ----
static bool ReadAt(uintptr_t addr, void* buf, size_t n) {
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr),
                             buf, n, nullptr) != FALSE;
}

static bool FindTechNode(uint32_t hash, uint32_t* outNode) {
    static const uint32_t kOffs[] = { 0x24, 0x28, 0x38, 0x48, 0x64, 0x6C };
    const uintptr_t lo = 0x0D000000, hi = 0x14000000;
    for (uintptr_t a = lo; a < hi; ) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi))) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t sz = mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_GUARD) == 0 &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0) {
            const size_t kChunk = 1u << 20;
            for (uintptr_t off = 0; off < sz; off += kChunk) {
                const size_t n = (sz - off < kChunk) ? static_cast<size_t>(sz - off) : kChunk;
                uint8_t* buf = static_cast<uint8_t*>(malloc(n));
                if (!buf) break;
                if (ReadAt(base + off, buf, n)) {
                    for (size_t i = 0; i + 4 <= n; i += 4) {
                        uint32_t v = 0;
                        memcpy(&v, buf + i, 4);
                        if (v != hash) continue;
                        const uintptr_t hit = base + off + i;
                        for (uint32_t o : kOffs) {
                            const uintptr_t node = hit - o;
                            if (node < 0x03000000 || node >= 0x20000000) continue;
                            uint8_t nd[0x80] = {};
                            if (!ReadAt(node, nd, sizeof(nd))) continue;
                            // 已知布局：
                            //   TimeBomb/Satellite: hash+0x38, self+0x3C
                            //   AirPower:           hash+0x48, self+0x4C
                            //   CryoSat:            hash+0x28, self+0x58（+0x6C 也有副本）
                            uint32_t after = 0;
                            memcpy(&after, nd + o + 4, 4);
                            uint32_t self58 = 0;
                            memcpy(&self58, nd + 0x58, 4);
                            const bool ok = (after == node) ||
                                            ((o == 0x28 || o == 0x6C) && self58 == node);
                            if (!ok) continue;
                            *outNode = static_cast<uint32_t>(node);
                            free(buf);
                            return true;
                        }
                    }
                }
                free(buf);
            }
        }
        a = base + sz;
    }
    return false;
}

int ProtocolGrantNode(uint32_t hash, char* out, size_t outsz) {
    const uintptr_t p = PlayerObj();
    if (!p) { g_lastErr = "玩家对象不可用（先进对局）"; return 0; }
    uint32_t beg = 0, end = 0, cap = 0;
    if (!Rd32(p + 0xA28, beg) || !Rd32(p + 0xA2C, end) || !Rd32(p + 0xA30, cap)) {
        g_lastErr = "读取协议 vector 失败";
        return 0;
    }
    if (beg < 0x03000000 || beg > end || cap < end || end + 4 > cap) {
        g_lastErr = "协议 vector 已满或指针异常（需先让游戏扩容）";
        return 0;
    }
    uint32_t node = 0;
    if (!FindTechNode(hash, &node)) {
        g_lastErr = "找不到该 hash 的 TechNode";
        return 0;
    }
    for (uint32_t a = beg; a < end; a += 4) {
        uint32_t v = 0;
        if (Rd32(a, v) && v == node) {
            g_lastErr = "该 TechNode 已在 vector 中";
            if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE, "node=%08X 已在 vector 中", node);
            return 1;
        }
    }
    if (!Wr32(end, node) || !Wr32(p + 0xA2C, end + 4)) {
        g_lastErr = "写协议 vector 失败";
        return 0;
    }
    if (out && outsz) {
        _snprintf_s(out, outsz, _TRUNCATE,
                    "已追加 node=%08X hash=%08X count=%u（打开/关闭协议面板刷新图标）",
                    node, hash, (end + 4 - beg) / 4);
    }
    return 1;
}

int ProtocolNativeGrant(uint32_t hash, char* out, size_t outsz) {
    const uintptr_t base = GameBase();
    uint32_t ctxObj = 0, upgCenter = 0;
    if (!Rd32(0x00CE8C9C, ctxObj) || !ctxObj) { g_lastErr = "上下文全局不可用"; return 0; }
    if (!Rd32(0x00CDBBF0, upgCenter) || !upgCenter) { g_lastErr = "升级中心不可用"; return 0; }

    struct Ctx { uint32_t zero; uint32_t hash; } ctx = { 0, hash };

    // 0x5456F0: hash -> 升级模板（链表查找，key 在 ctx+4）
    uintptr_t tpl = CallThisFn(base + 0x1456F0, upgCenter,
                               reinterpret_cast<uintptr_t>(&ctx), 0, 0, 1);
    uint32_t mode = 2;
    if (!tpl || tpl == 0xDEADDEADu) {
        // 不是升级：按 hash 找 TechNode，取 node+0 作为 PlayerTech 模板
        uint32_t node = 0, t = 0;
        if (!FindTechNode(hash, &node) || !Rd32(node, t) || !t) {
            g_lastErr = "该 hash 既不在升级表，也找不到 TechNode";
            return 0;
        }
        tpl = t;
        mode = 1;
    }

    // 0x8393E0: 取本地队伍/管理器
    const uintptr_t team = CallThisFn(base + 0x4393E0, ctxObj, 0, 0, 0, 0);
    if (!team || team == 0xDEADDEADu) { g_lastErr = "取队伍/管理器失败"; return 0; }

    // 0x84D7C0: grant(manager, template, mode, 1)
    const uintptr_t ret = CallThisFn(base + 0x44D7C0, team, tpl, mode, 1, 3);
    if (ret == 0xDEADDEADu) { g_lastErr = "原生 grant 调用异常"; return 0; }

    if (out && outsz) {
        _snprintf_s(out, outsz, _TRUNCATE,
                    "hash=%08X mode=%u template=%08X team=%08X ret=%08X",
                    hash, mode, (unsigned)tpl, (unsigned)team, (unsigned)ret);
    }
    return 1;
}

// ---- 只读诊断：动作执行器 0x5A2B40 记录 ----
typedef int (__thiscall* ExecFn)(void*, void*);
static ExecFn g_origExec = nullptr;
static volatile LONG g_execCalls = 0, g_execIndex = 0, g_execCount = 0;
static volatile LONG g_execP1 = 0, g_execP2 = 0, g_execMatch = 0;

static __declspec(naked) void DetExec() {
    __asm {
        pushad
        mov  eax, dword ptr [esp + 0x24]   // arg（pushad 后 ret 在 +0x20）
        test eax, eax
        je   skip
        mov  ecx, dword ptr [eax + 4]      // action index
        mov  dword ptr [g_execIndex], ecx
        inc  dword ptr [g_execCalls]
        cmp  ecx, 20
        je   match
        cmp  ecx, 25
        je   match
        cmp  ecx, 26
        je   match
        cmp  ecx, 27
        je   match
        cmp  ecx, 28
        je   match
        cmp  ecx, 41
        je   match
        jmp  skip
    match:
        mov  ecx, dword ptr [eax + 8]      // param count
        mov  dword ptr [g_execCount], ecx
        mov  ecx, dword ptr [eax + 0xC]    // param1
        mov  dword ptr [g_execP1], ecx
        mov  ecx, dword ptr [eax + 0x10]   // param2
        mov  dword ptr [g_execP2], ecx
        inc  dword ptr [g_execMatch]
    skip:
        popad
        jmp  dword ptr [g_origExec]
    }
}

int ProtocolTraceExecInstall() {
    g_execCalls = 0; g_execIndex = 0; g_execCount = 0; g_execP1 = 0; g_execP2 = 0; g_execMatch = 0;
    void* target = reinterpret_cast<void*>(GameBase() + 0x1A2B40);
    MH_STATUS s = MH_CreateHook(target, reinterpret_cast<void*>(&DetExec),
                                reinterpret_cast<void**>(&g_origExec));
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) { g_lastErr = "hook 5A2B40 失败"; return 0; }
    MH_EnableHook(target);
    return 1;
}

int ProtocolTraceExecRead(char* out, size_t outsz) {
    if (!out || !outsz) return 0;
    _snprintf_s(out, outsz, _TRUNCATE,
                "execCalls=%ld index=%ld count=%ld p1=%08X p2=%08X match=%ld",
                g_execCalls, g_execIndex, g_execCount, g_execP1, g_execP2, g_execMatch);
    return 1;
}

const char* ProtocolLastError() { return g_lastErr; }

} // namespace ra3
