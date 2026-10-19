// protocol.cpp - 秘密协议：协议点 / 全协议 / 授予（实验）
#include "protocol.h"
#include "features.h"
#include "protocols_corona.h"
#include "aicmd.h"

#include <windows.h>
#include "MinHook.h"
#include <vector>
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

// 本地玩家对象：用 PlayerID hook 记下的那只（= 任意我方单位 Thing 的 owner）。
// 不能再用 g_moneyObj —— 那是 UI 侧的资金对象，+0xA44/+0x2C 对它是垃圾值。
// 真正的 Player 对象（实测链条，来自 0x4393E0 的取法）：
//     ctx    = [0xCE8C9C]
//     Player = [ctx + 0x28]        （该函数在特定条件下改用 [ctx+0x80]）
// Thing→owner 拿到的其实就是同一个对象，但 hook 没抓到时会为空，所以这里做兜底。
static uintptr_t RealPlayer() {
    uint32_t ctx = 0, pl = 0;
    if (!Rd32(0x00CE8C9C, ctx) || !ctx) return 0;
    if (!Rd32(ctx + 0x28, pl)) return 0;
    return pl;
}

uintptr_t PlayerObj() {
    const uintptr_t hooked = static_cast<uintptr_t>(g_localPlayerObj);
    return hooked ? hooked : RealPlayer();
}

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

// 把脚本引擎的「当前玩家」指向本地玩家，<This Player> / <This Object> 才解析得到。
//
// 依据（1.12 反汇编，非推测）：占位符解析函数 0x17FA80 开头就是
//     eax = [0xCDD190]                 ; ScriptEngine
//     if (engine + 0x17D88 == 0) return NULL     <-- 为 0 时 <This Player> 直接解析失败
//     edx = [engine + 0x17D88]         ; 当前 Player
//     ebx = [edx + 0x20]               ; 玩家序号（实测 =2，与 /snapshot 一致）
//     call 0x839170([0xCE8C9C], ebx, kind)       ; 按 序号+种类 取出参数对象
// 旧代码写的是 +0x17D68/+0x17D70（猜的，偏移错 0x20），所以此前一直解析成 NULL。
static constexpr uintptr_t kScriptEngine  = 0x00CDD190;
static constexpr uintptr_t kCurPlayerOff  = 0x17D88;

bool SetScriptContext() {
    const uintptr_t pl = PlayerObj();
    if (!pl) { g_lastErr = "本地玩家对象不可用（先进对局）"; return false; }
    uint32_t eng = 0;
    if (!Rd32(kScriptEngine, eng) || !eng) { g_lastErr = "脚本引擎不可用"; return false; }
    if (!Wr32(eng + kCurPlayerOff, static_cast<uint32_t>(pl))) {
        g_lastErr = "写当前玩家上下文失败";
        return false;
    }
    return true;
}

// 读回当前上下文，用于自检（0 = 没设上，<This Player> 必然解析失败）
uintptr_t CurrentScriptPlayer() {
    uint32_t eng = 0, pl = 0;
    if (!Rd32(kScriptEngine, eng) || !eng) return 0;
    if (!Rd32(eng + kCurPlayerOff, pl)) return 0;
    return pl;
}

// 引擎字符串（tag=4）：{?,?,len,char*} 四字结构，字符指针在 +0xC。
// ExecuteAction 取动作名用的是只认 tag=4 的取值器（0xA5D0），Lua 自己的压串
// （0xA940/0xA980）写的是 tag=3，两者表示不同 ⇒ 必须手工构槽。
struct FakeEngineStr { uint32_t a, b, len; const char* p; };
static FakeEngineStr g_fakeStr[16];
static int g_fakeStrIdx = 0;

void PushEngineStr(uintptr_t L, const char* text) {
    typedef int  (__cdecl* Top_t)(uintptr_t);
    typedef void (__cdecl* SetTop_t)(uintptr_t, int);
    Top_t gettop  = reinterpret_cast<Top_t>(GameBase() + 0xA3D0);
    SetTop_t settop = reinterpret_cast<SetTop_t>(GameBase() + 0xA3E0);
    FakeEngineStr* fs = &g_fakeStr[g_fakeStrIdx = (g_fakeStrIdx + 1) & 15];
    fs->a = 0; fs->b = 0; fs->len = static_cast<uint32_t>(strlen(text)); fs->p = text;
    const int n = gettop(L);
    settop(L, n + 1);                       // 先长栈（新槽被填成 nil）
    uint32_t* top  = *reinterpret_cast<uint32_t**>(L);
    uint32_t* slot = top - 4;               // 16 字节槽：{tag, pad, value, pad}
    slot[0] = 4;                            // tag = 引擎字符串
    slot[1] = 0;
    slot[2] = reinterpret_cast<uint32_t>(fs);
    slot[3] = 0;
}

// 统一入口：清栈 → 压最多 3 个名字串 → 调 fn(L) → 若引擎往栈上压了值，把栈顶的
// 字符串带回来（引擎的脚本接口多把「结果/原因」压成字符串返回，直接读它比自己猜强）。
int LuaPushAndCall(uintptr_t fnaddr, const char* const* strs, int nstr,
                   char* out, size_t outsz, bool engStr, bool paramsAsLua) {
    const uintptr_t L = LuaState();
    if (!L) {
        if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE, "lua_State 不可用（先进对局）");
        return -1;
    }
    const uintptr_t base = GameBase();
    SetTop_t settop = reinterpret_cast<SetTop_t>(base + 0xA3E0);
    PushStr_t push  = reinterpret_cast<PushStr_t>(base + 0xA980);
    Top_t gettop    = reinterpret_cast<Top_t>(base + 0xA3D0);
    typedef const char* (__cdecl* ToStr_t)(uintptr_t, int);
    ToStr_t tostr   = reinterpret_cast<ToStr_t>(base + 0xA5D0);
    Exec_t fn       = reinterpret_cast<Exec_t>(fnaddr);
    int r = -1;
    __try {
        if (gettop(L) != 0) {
            if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE, "Lua 栈非空，已跳过（防卡死）");
            return -1;
        }
        settop(L, 0);
        for (int i = 0; i < nstr; ++i) {
            if (!strs[i]) continue;
            // paramsAsLua：动作名仍按引擎串（tag4），其余按普通 Lua 串（tag3）。
            // 引擎的动作参数解析器可能走的是 lua_tostring 那一套，只认 tag3。
            if (engStr && !(paramsAsLua && i > 0)) PushEngineStr(L, strs[i]);
            else                                   push(L, strs[i]);
        }
        r = fn(L);
        const char* s = nullptr;
        if (gettop(L) > 0) {
            __try { s = tostr(L, -1); } __except (EXCEPTION_EXECUTE_HANDLER) { s = nullptr; }
        }
        if (out && outsz) {
            if (s) _snprintf_s(out, outsz, _TRUNCATE, "ret=%d str=\"%s\"", r, s);
            else   _snprintf_s(out, outsz, _TRUNCATE, "ret=%d", r);
        }
        settop(L, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        __try { settop(L, 0); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE, "调用异常");
        return -1;
    }
    return r;
}

// 真正的 Lua 全局 `ExecuteAction`：RVA 0x182C50（= VA 0x582C50）。
// 2026-10-19 实机确认：用它执行 DEBUG_MESSAGE_BOX 真的弹了框（tick 冻结可证）。
// 旧常识里的 0x178670 实测是 **ObjectDescription**（把对象信息格式化成描述串），
// 这也是「调 0x178670 只得到字符串、无状态写入」的真正原因。详见
// research/protocol-skills/findings/引擎Lua脚本接口.md §八。
static constexpr uint32_t kRvaLuaExecuteAction = 0x182C50;

int LuaAction(const char* action, const char* a1, const char* a2, const char* a3) {
    const char* strs[4] = { action, a1, a2, a3 };
    // 参数按普通 Lua 串（tag=3）压栈 —— 见 LuaExecuteAction 上方关于 tag 的说明。
    return LuaPushAndCall(GameBase() + kRvaLuaExecuteAction, strs, 4, nullptr, 0, false, false);
}

// SAGE StringHasher（Paul Hsieh SuperFastHash 变体，init = 长度）。
// 与 unit-codes/tools/sage_hash.py 逐位一致；调用方负责先转小写。
static uint32_t SageHash(const char* s) {
    if (!s || !s[0]) return 0;
    const size_t n = strlen(s);
    const unsigned char* b = reinterpret_cast<const unsigned char*>(s);
    uint32_t h = static_cast<uint32_t>(n);
    size_t i = 0;
    for (size_t blocks = n / 4; blocks; --blocks, i += 4) {
        h += static_cast<uint32_t>((b[i + 1] << 8) | b[i]);
        const uint32_t tmp = static_cast<uint32_t>(((b[i + 3] << 8) | b[i + 2]) ^ (h << 5));
        h ^= (tmp << 11);
        h += (h >> 11);
    }
    switch (n % 4) {
    case 1:
        h += b[i];
        h = (h << 10) ^ h;
        h += (h >> 1);
        break;
    case 2:
        h += static_cast<uint32_t>((b[i + 1] << 8) | b[i]);
        h ^= (h << 11);
        h += (h >> 17);
        break;
    case 3:
        h += static_cast<uint32_t>((b[i + 1] << 8) | b[i]);
        h ^= ((h ^ static_cast<uint32_t>(b[i + 2] << 2)) << 16);
        h += (h >> 11);
        break;
    default:
        break;
    }
    h ^= (h << 3);  h += (h >> 5);
    h ^= (h << 2);  h += (h >> 15);
    h ^= (h << 10);
    return h;
}

} // namespace


// 通用 Lua C 函数调用：按 RVA 直调引擎导出的脚本接口，参数只支持名字串。
int LuaCallRva(uint32_t rva, const char* a1, const char* a2, const char* a3,
               char* out, size_t outsz) {
    // ra3-copilot/res 与 research 里的 lua_api_table.tsv 记的是**绝对地址**（0x40xxxx 以上），
    // 之前一律当 RVA 用，于是每个调用都落到错误函数上（静默无效）。
    // 这里统一兼容两种写法：>= ImageBase 的按 VA 处理。
    if (rva >= 0x400000u) rva -= 0x400000u;
    const char* strs[3] = { a1, a2, a3 };
    const int r = LuaPushAndCall(GameBase() + rva, strs, 3, out, outsz, false, false);
    if (out && outsz) {
        char tail[64];
        _snprintf_s(tail, sizeof(tail), _TRUNCATE, " [lua %05X]", rva);
        strncat_s(out, outsz, tail, _TRUNCATE);
    }
    return r;
}

// 通用脚本动作入口：action 走引擎 ExecuteAction，a1..a3 依次压栈。
// 只支持名字串参数（PLAYER_GRANT_SPECIAL_POWER / NAMED_* 多数动作正合此形）。
//
// 2026-10-19：`kRvaLuaExecuteAction` 是**Lua 全局函数**，它的所有实参都从 Lua 栈取，
// 所以动作名和各参数都必须压成普通 Lua 串（tag=3）。之前用 tag=4（引擎串）是给
// `ObjectDescription`（0x178670）那个旧误判服务的，对真入口只会取不到值。
// mode 保留兼容：mode=1 时只影响日志，不再改变压栈类型。
int LuaExecuteAction(const char* action, const char* a1, const char* a2, const char* a3,
                     char* out, size_t outsz, int mode) {
    (void)mode;
    if (!action || !action[0]) {
        if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE, "action 为空");
        return -1;
    }
    if (!SetScriptContext()) {
        if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE, "上下文未设置：%s", g_lastErr);
        return -1;
    }
    const char* strs[4] = { action, a1, a2, a3 };
    const int r = LuaPushAndCall(GameBase() + kRvaLuaExecuteAction, strs, 4, out, outsz,
                                 /*engStr=*/false, /*paramsAsLua=*/false);
    return r;
}

// 自检：到 Lua 栈上实际走一遍压栈/取串，报每一步的返回值。
// 用来区分「原语地址错」和「参数没进去」。
int LuaSelfTest(const char* text, char* out, size_t outsz) {
    const uintptr_t L = LuaState();
    if (!L) { if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE, "no lua_State"); return -1; }
    const uintptr_t base = GameBase();
    typedef void (__cdecl* SetTop_t)(uintptr_t, int);
    typedef int  (__cdecl* Top_t)(uintptr_t);
    typedef const char* (__cdecl* ToStr_t)(uintptr_t, int);
    typedef int  (__cdecl* Type_t)(uintptr_t, int);
    SetTop_t settop = reinterpret_cast<SetTop_t>(base + 0xA3E0);
    Top_t gettop    = reinterpret_cast<Top_t>(base + 0xA3D0);
    ToStr_t tostr   = reinterpret_cast<ToStr_t>(base + 0xA5D0);
    Type_t  ltype   = reinterpret_cast<Type_t>(base + 0xA520);
    if (!text || !text[0]) text = "SELFTEST";
    // 压 4 个引擎串，逐个取回：验证「第 2/3/4 个参数」在引擎取值器里是否可见。
    char buf[4][40];
    for (int i = 0; i < 4; ++i)
        _snprintf_s(buf[i], sizeof(buf[i]), _TRUNCATE, "%s_%d", text, i + 1);
    char res[4][48];
    int tops[4] = { 0 }, typs[4] = { 0 };
    const char* got[4] = { nullptr, nullptr, nullptr, nullptr };
    __try {
        if (gettop(L) != 0) settop(L, 0);
        for (int i = 0; i < 4; ++i) PushEngineStr(L, buf[i]);
        for (int i = 0; i < 4; ++i) {
            tops[i] = gettop(L);
            typs[i] = ltype(L, i + 1);
            got[i] = tostr(L, i + 1);
            _snprintf_s(res[i], sizeof(res[i]), _TRUNCATE, "%s", got[i] ? got[i] : "(null)");
        }
        settop(L, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE, "自检异常");
        return -1;
    }
    if (out && outsz)
        _snprintf_s(out, outsz, _TRUNCATE,
                    "L=%08X top=%d | a1=%s | a2=%s | a3=%s | a4=%s | type=%d,%d,%d,%d",
                    (unsigned)L, tops[3], res[0], res[1], res[2], res[3],
                    typs[0], typs[1], typs[2], typs[3]);
    return 0;
}

// ---- 目录：全部来自 tools/gen_protocols.py 生成的 protocols_corona.h ----
int ProtocolCount() { return kProtoPowerCount; }
const char* ProtocolId(int idx)       { return (idx >= 0 && idx < kProtoPowerCount) ? kProtoPowers[idx].id : ""; }
const char* ProtocolName(int idx)     { return (idx >= 0 && idx < kProtoPowerCount) ? kProtoPowers[idx].name : ""; }
const char* ProtocolFaction(int idx)  { return (idx >= 0 && idx < kProtoPowerCount) ? kProtoPowers[idx].faction : ""; }

int ProtocolTechCount() { return kProtoTechCount; }
const char* ProtocolTechId(int idx)      { return (idx >= 0 && idx < kProtoTechCount) ? kProtoTechs[idx].id : ""; }
const char* ProtocolTechName(int idx)    { return (idx >= 0 && idx < kProtoTechCount) ? kProtoTechs[idx].name : ""; }
const char* ProtocolTechFaction(int idx) { return (idx >= 0 && idx < kProtoTechCount) ? kProtoTechs[idx].faction : ""; }

// ---- 协议技能（人工中文名，tools/proto_zh.tsv）----
// 这批才是面板上「可勾选、可授予」的协议；其余 SpecialPower 多为单位能力（引擎会拒绝）。
int ProtocolPickCount() { return kProtoPickCount; }
const char* ProtocolPickId(int idx)      { return (idx >= 0 && idx < kProtoPickCount) ? kProtoPicks[idx].id : ""; }
const char* ProtocolPickZh(int idx)      { return (idx >= 0 && idx < kProtoPickCount) ? kProtoPicks[idx].zh : ""; }
const char* ProtocolPickFaction(int idx) { return (idx >= 0 && idx < kProtoPickCount) ? kProtoPicks[idx].faction : ""; }
// 官方描述串（来自日冕 gamestrings.csf 的 DESC:<名>，与游戏内提示框一致）；没有则为空串。
const char* ProtocolPickDesc(int idx)    { return (idx >= 0 && idx < kProtoPickCount) ? kProtoPicks[idx].desc : ""; }

int ProtoGrantPick(int idx, char* out, size_t outsz) {
    if (idx < 0 || idx >= kProtoPickCount) {
        g_lastErr = "协议序号越界";
        if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE, "%s", g_lastErr);
        return 0;
    }
    return ProtoGrantByName(kProtoPicks[idx].id, nullptr, out, outsz);
}

// 协议点（实测确认，2026-10-19）：
//     SCPoint 对象 = [Player + 0x1320]
//     点数         = [SCPoint + 0x34]      （整数；写 15 后面板即可全买）
// 依据：0x6CFE51 处的原指令链 mov ecx,[0xCE8C9C] / call 0x4393E0 /
//       mov eax,[eax+0x1320] / mov edi,[eax+0x34]，与 trainer 的 SCPoint 桩一致。
// 旧的 +0xA44（以及 +0x34 当对象指针的解释）全部作废。
static constexpr uintptr_t kPlayerSCPointStore = 0x1320;
static constexpr uintptr_t kSCPointOffset      = 0x34;

static uintptr_t SCPointObj() {
    const uintptr_t p = PlayerObj();
    uint32_t sub = 0;
    if (!p) return 0;
    if (!Rd32(p + kPlayerSCPointStore, sub) || !sub) return 0;
    return sub;
}

int ProtocolPoints() {
    const uintptr_t s = SCPointObj();
    uint32_t v = 0;
    return (s && Rd32(s + kSCPointOffset, v)) ? static_cast<int>(v) : -1;
}

float ProtocolScience() {
    const uintptr_t p = PlayerObj();
    if (!p) return 0.f;
    __try { return *reinterpret_cast<float*>(p + 0x2C); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0.f; }
}

bool ProtocolSetPoints(int n) {
    const uintptr_t s = SCPointObj();
    if (!s) { g_lastErr = "协议点对象不可用（先进对局）"; return false; }
    return Wr32(s + kSCPointOffset, static_cast<uint32_t>(n < 0 ? 0 : n));
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

// 本地玩家的**真实名字**（玩家对象 +0x18 指向 "Player_N"）。
// 实测：名字是 Player_1，而 +0x20 序号是 2 —— 两者不是一回事，别混用。
const char* LocalPlayerName() {
    static char buf[64];
    buf[0] = 0;
    const uintptr_t p = PlayerObj();
    if (!p) return buf;
    uint32_t sp = 0;
    if (!Rd32(p + 0x18, sp) || sp < 0x02000000) return buf;
    char tmp[64] = { 0 };
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(sp),
                           tmp, sizeof(tmp) - 1, &got) || got == 0)
        return buf;
    tmp[sizeof(tmp) - 1] = 0;
    strncpy_s(buf, sizeof(buf), tmp, _TRUNCATE);
    return buf;
}

// 授予一个 SpecialPower（技能）给本地玩家。
// 参数就是作者脚本里的写法：ExecuteAction("PLAYER_GRANT_SPECIAL_POWER", 技能名, 玩家名)
int ProtoGrantPower(int idx, char* out, size_t outsz) {
    if (idx < 0 || idx >= kProtoPowerCount) { g_lastErr = "技能序号越界"; return 0; }
    const char* who = LocalPlayerName();
    if (!who[0]) {
        g_lastErr = "读不到本地玩家名（先进对局）";
        if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE, "%s", g_lastErr);
        return 0;
    }
    const char* id = kProtoPowers[idx].id;
    char detail[192] = { 0 };
    const int r = LuaExecuteAction("PLAYER_GRANT_SPECIAL_POWER", id, who, nullptr,
                                   detail, sizeof(detail));
    if (out && outsz) {
        _snprintf_s(out, outsz, _TRUNCATE, "%s -> %s : r=%d %s", id, who, r, detail);
    }
    return (r >= 0) ? 1 : 0;
}

// 按内部名直接授予一个 SpecialPower（2026-10-19 实测可用，见引擎Lua脚本接口.md §九）。
// 实测**无阵营检查**：盟军/苏军技能都能给天朝玩家，`who` 也认裸玩家名（可指向敌方）。
// 唯一门槛：技能必须有「可授予对象」（`[[template+8]+0x134]` 非空）；单位能力/被禁用的超武会被静默跳过。
//
// 「授予是否真的生效」用差分判定，不用模板预判：
//   0x839C30(name) → 0x4A6BE0(容器 0x81D85EFA, nameKey, 0) 的查表按**名字键指针**做数值比较，
//   外部传 C 字面量永远查不到（实测 tpl 恒为 0）。所以改为比对 SCPoint+0x18 列表长度：
//   引擎的 grantPower（0x854300）对「已在列表」的指针去重、对「无授予对象」的模板整条跳过，
//   两种情况列表都不增长 —— 这就是「返回 1 但没反应」的唯一判据。
namespace {
int OwnedDelta(int before) {
    const int after = ProtocolOwnedCount();
    if (before < 0 || after < 0) return -1;
    return after - before;
}
} // namespace

// 候选名字形式：扫描器同时产出 `SpecialPowerXxx` 与 `SpecialPower_Xxx` 两种写法，
// 但引擎容器里只登记了其中一种。授予前不预判，未生效时自动换另一种再试一次。
static void AltPowerName(const char* in, char* out, size_t outsz) {
    out[0] = 0;
    if (!in) return;
    if (strncmp(in, "SpecialPower_", 13) == 0) {
        _snprintf_s(out, outsz, _TRUNCATE, "SpecialPower%s", in + 13);
    } else if (strncmp(in, "SpecialPower", 12) == 0 && in[12]) {
        _snprintf_s(out, outsz, _TRUNCATE, "SpecialPower_%s", in + 12);
    }
}

// 试一次：返回列表增量（>0 表示真的授予了）
static int TryGrant(const char* name, const char* who, char* detail, size_t detsz) {
    const int before = ProtocolOwnedCount();
    if (detail && detsz) detail[0] = 0;
    const int r = LuaExecuteAction("PLAYER_GRANT_SPECIAL_POWER", name, who, nullptr, detail, detsz, 0);
    const int after = ProtocolOwnedCount();
    if (before < 0 || after < 0) return -1;
    (void)r;
    return after - before;
}

int ProtoGrantByName(const char* power, const char* who, char* out, size_t outsz) {
    if (!power || !power[0]) {
        g_lastErr = "power 为空";
        if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE, "%s", g_lastErr);
        return 0;
    }
    if (!who || !who[0]) who = "<This Player>";

    char detail[192] = { 0 };
    int delta = TryGrant(power, who, detail, sizeof(detail));
    if (delta > 0) {
        if (out && outsz)
            _snprintf_s(out, outsz, _TRUNCATE, "%s -> %s : 已授予（列表 +%d）%s",
                        power, who, delta, detail);
        return 1;
    }

    // 换另一种拼写再试：容器里只登记一种形式，另一种查不到模板会被整条跳过。
    char alt[128];
    AltPowerName(power, alt, sizeof(alt));
    if (alt[0]) {
        char d2[192] = { 0 };
        const int d2v = TryGrant(alt, who, d2, sizeof(d2));
        if (d2v > 0) {
            if (out && outsz)
                _snprintf_s(out, outsz, _TRUNCATE, "%s -> %s : 已授予（列表 +%d，实际名字 %s）%s",
                            power, who, d2v, alt, d2);
            return 1;
        }
    }

    if (out && outsz) {
        if (delta < 0)
            _snprintf_s(out, outsz, _TRUNCATE, "%s -> %s : %s（列表读不到，无法确认）", power, who, detail);
        else
            _snprintf_s(out, outsz, _TRUNCATE,
                        "%s -> 未生效：容器里查不到这个名字，或模板无可授予对象、已在列表中", power);
    }
    return 0;
}

// 技能栏按钮的可用性状态：脚本动作 PLAYER_SPECIAL_POWER_AVAILABILITY(player, power, state)，
// state 取字符串 "Available" / "Disabled" / "Hidden"（引擎里映射成 0/1/2 三个枚举，
// 分别对应玩家技能管理器里 +0xC4（可用）/ +0xD4（禁用）两个列表的增删）。
// 跨阵营注入的技能默认是禁用态 —— 用 Available 强制放进可用列表即可亮起来。
int ProtoSetPowerState(const char* power, const char* state, const char* who,
                       char* out, size_t outsz) {
    if (!power || !power[0]) {
        g_lastErr = "power 为空";
        if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE, "%s", g_lastErr);
        return 0;
    }
    if (!state || !state[0]) state = "Available";
    if (strcmp(state, "Available") != 0 && strcmp(state, "Disabled") != 0 &&
        strcmp(state, "Hidden") != 0) {
        g_lastErr = "state 只能是 Available / Disabled / Hidden";
        if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE, "%s", g_lastErr);
        return 0;
    }
    if (!who || !who[0]) who = "<This Player>";
    char detail[192] = { 0 };
    const int r = LuaExecuteAction("PLAYER_SPECIAL_POWER_AVAILABILITY", who, power, state,
                                   detail, sizeof(detail), 0);
    if (out && outsz)
        _snprintf_s(out, outsz, _TRUNCATE, "%s -> %s : %s r=%d %s",
                    power, who, state, r, detail);
    return (r >= 0) ? 1 : 0;
}

// 脚本动作表诊断：按名字定位 action id（= ExecuteAction 绑定里 strcmp 命中的下标，
// 也是执行器 0x5A2B40 里 `jmp [eax*4 + 0x5AD06C]` 的索引）。
// 表：engine=[0xCDD190]，基址 +0x48，stride 0x80，名字指针在 +0x0C，参数数在 +0x48。
int ActionIndexByName(const char* name, char* out, size_t outsz) {
    if (!name || !name[0]) {
        if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE, "name 为空");
        return -1;
    }
    uint32_t eng = 0;
    if (!Rd32(0x00CDD190, eng) || !eng) {
        if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE, "脚本引擎不可用");
        return -1;
    }
    for (int i = 0; i < 0x23D; ++i) {
        const uint32_t entry = eng + 0x48 + (uint32_t)i * 0x80;
        uint32_t namePtr = 0, nparams = 0;
        if (!Rd32(entry + 0x0C, namePtr) || !namePtr) continue;
        char buf[192] = { 0 };
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(namePtr),
                               buf, sizeof(buf) - 1, &got) || !got)
            continue;
        if (strcmp(buf, name) == 0) {
            Rd32(entry + 0x48, nparams);
            if (out && outsz)
                _snprintf_s(out, outsz, _TRUNCATE, "idx=%d entry=%08X nparams=%u", i, entry, nparams);
            return i;
        }
    }
    if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE, "未找到 %s", name);
    return -1;
}

// 解锁一个 PlayerTech（协议卡片的科技）。动作名取自引擎常量 PLAYER_LOCK_PLAYER_TECH。
int ProtoGrantTech(int idx, char* out, size_t outsz) {
    if (idx < 0 || idx >= kProtoTechCount) { g_lastErr = "科技序号越界"; return 0; }
    const char* who = LocalPlayerName();
    if (!who[0]) { g_lastErr = "读不到本地玩家名（先进对局）"; return 0; }
    const char* id = kProtoTechs[idx].id;
    char detail[192] = { 0 };
    const int r = LuaExecuteAction("PLAYER_LOCK_PLAYER_TECH", id, who, nullptr,
                                   detail, sizeof(detail));
    if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE, "%s -> %s : r=%d %s", id, who, r, detail);
    return (r >= 0) ? 1 : 0;
}

// 按名字授予 Upgrade（mode=2）或 PlayerTech（mode=1），不走 Lua。
//
// 为什么需要它：`NAMED_RECEIVE_UPGRADE` / `PLAYER_LOCK_PLAYER_TECH` 的对象与科技参数都要经过
// 脚本层的值解析，而 /action 只能压名字串，解析不到就静默返回 —— 无法判断「名字不存在」还是
// 「参数没解析」。这里直接按 hash 查注册表拿模板，再调 grant，两个结果分开报。
//
// 依据（1.12 实测，见 wiki/protocols/native-grant-path.md）：
//   registry = [0xCDBBF0]
//   lookup 0x5456F0(this=registry, ctx*)   ctx = { u32 0, u32 hash }（key 在 +4）
//   grant  0x84D7C0(this=team, template, mode, flag)   ret 0xC，mode 1=PlayerTech / 2=Upgrade
// grant 入口会读 [template+0xC] 再解引用，模板不完整会 AV —— 由 CallThisFn 的 SEH 兜住。
int ProtoGrantUpgrade(const char* name, int mode, char* out, size_t outsz) {
    if (!name || !name[0]) {
        g_lastErr = "name 为空";
        if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE, "%s", g_lastErr);
        return -1;
    }
    if (mode != 1) mode = 2;

    char lower[192];
    size_t i = 0;
    for (; name[i] && i + 1 < sizeof(lower); ++i) {
        const char ch = name[i];
        lower[i] = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
    }
    lower[i] = 0;
    const uint32_t hash = SageHash(lower);

    uint32_t reg = 0;
    if (!Rd32(0x00CDBBF0, reg) || !reg) {
        g_lastErr = "Upgrade 注册表不可用（先进对局）";
        if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE, "%s", g_lastErr);
        return -1;
    }

    struct LookupCtx { uint32_t reserved; uint32_t hash; } key = { 0u, hash };
    const uintptr_t tpl = CallThisFn(GameBase() + 0x1456F0, reg,
                                     reinterpret_cast<uintptr_t>(&key), 0, 0, 1);
    if (tpl == 0xDEADDEADu) {
        g_lastErr = "模板查找异常";
        if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE,
                                      "%s hash=%08X（lookup 0x5456F0 抛异常）", name, hash);
        return -1;
    }
    if (!tpl) {
        g_lastErr = "名字未注册";
        if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE,
                                      "%s hash=%08X -> 注册表里没有这个名字", name, hash);
        return 0;
    }

    const uintptr_t team = RealPlayer();
    if (!team) {
        g_lastErr = "本地玩家对象不可用（先进对局）";
        if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE, "%s tpl=%08X", g_lastErr, (unsigned)tpl);
        return -1;
    }

    const uintptr_t r = CallThisFn(GameBase() + 0x44D7C0, team, tpl,
                                   static_cast<uintptr_t>(mode), 1, 3);
    if (r == 0xDEADDEADu) {
        uintptr_t ea = 0; uint32_t ec = 0;
        LastThisCallExc(&ea, &ec);
        g_lastErr = "grant 异常（模板不完整）";
        if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE,
                                      "%s hash=%08X tpl=%08X mode=%d exc@%08X code=%08X",
                                      name, hash, (unsigned)tpl, mode, (unsigned)ea, ec);
        return -1;
    }
    if (out && outsz) _snprintf_s(out, outsz, _TRUNCATE,
                                  "%s hash=%08X tpl=%08X mode=%d ret=%08X",
                                  name, hash, (unsigned)tpl, mode, (unsigned)r);
    return 1;
}


// 依据（0x439500 / 0x43F570 反汇编）：
//     idx = [SCPoint+0x30]; total = [[[Player+0x10]]+0xA8]
//     需求 = [[[*]+0xAC][idx]] -> 虚表[+4]
// 0x439500 返回「idx+1 >= total」，用于判断是否到最后一格。
// ⚠ 两个字段别混：
//   +0x30 = 「当前格索引」——游戏自己维护，用来算需求/画进度条，写它不改可购买数
//   +0x40 = 「已解锁到第几格」——**UI 判可购买读的是这个**（实测能买 2 格时它=2）
static constexpr uintptr_t kSCUnlockCount = 0x40;

int ProtocolUnlockIndex() {
    const uintptr_t s = SCPointObj();
    uint32_t v = 0;
    return (s && Rd32(s + kSCUnlockCount, v)) ? static_cast<int>(v) : -1;
}

bool ProtocolSetUnlockIndex(int n) {
    const uintptr_t s = SCPointObj();
    if (!s) { g_lastErr = "协议点对象不可用（先进对局）"; return false; }
    if (n < 0) n = 0;
    const int total = ProtocolSlotTotal();
    if (total > 0 && n > total) n = total;       // 别超过本阵营协议总数，避免越界
    return Wr32(s + kSCUnlockCount, static_cast<uint32_t>(n));
}

// 已拥有协议 id 列表（实测 2026-10-19）：SCPoint+0x18/+0x1C/+0x20 = begin/end/cap，
// 元素是 uint32 id；id 的来源 = 协议对象 [[obj]+4]（obj->vtable[1]）。
// 面板「亮/灰」的直接判据就是 id 在不在这个列表里（0x43C320 查找），
// 所以往列表里塞 id 会让该格立刻变成「已拥有」。
static constexpr uintptr_t kSCOwnedBegin = 0x18;
static constexpr uintptr_t kSCOwnedEnd   = 0x1C;
static constexpr uintptr_t kSCOwnedCap   = 0x20;

int ProtocolOwnedCount() {
    const uintptr_t s = SCPointObj();
    if (!s) return -1;
    uint32_t b = 0, e = 0;
    if (!Rd32(s + kSCOwnedBegin, b) || !Rd32(s + kSCOwnedEnd, e)) return -1;
    if (!b || e < b || (e - b) > 4096) return -1;
    return static_cast<int>((e - b) / 4);
}

bool ProtocolOwnedAt(int i, uint32_t* outId) {
    const uintptr_t s = SCPointObj();
    if (!s || !outId || i < 0) return false;
    uint32_t b = 0;
    if (!Rd32(s + kSCOwnedBegin, b) || !b) return false;
    return Rd32(b + static_cast<uintptr_t>(i) * 4, *outId);
}

bool ProtocolClaimId(uint32_t id) {
    const uintptr_t s = SCPointObj();
    if (!s) { g_lastErr = "协议点对象不可用（先进对局）"; return false; }
    uint32_t b = 0, e = 0, cap = 0;
    if (!Rd32(s + kSCOwnedBegin, b) || !Rd32(s + kSCOwnedEnd, e) || !Rd32(s + kSCOwnedCap, cap))
        { g_lastErr = "已购列表指针读失败"; return false; }
    if (!b || e < b || cap < e) { g_lastErr = "已购列表指针异常"; return false; }
    for (uint32_t p = b; p < e; p += 4) {          // 已存在就不重复塞
        uint32_t v = 0;
        if (Rd32(p, v) && v == id) return true;
    }
    if (e + 4 > cap) { g_lastErr = "已购列表已满（需扩容，暂不支持）"; return false; }
    if (!Wr32(e, id)) { g_lastErr = "写 id 失败"; return false; }
    return Wr32(s + kSCOwnedEnd, e + 4);
}

// 本阵营协议格总数：[[Player+0x10]] + 0xA8（实测神州=10，日冕 UI 会自行扩展）
int ProtocolSlotTotal() {
    const uintptr_t p = PlayerObj();
    if (!p) return 0;
    uint32_t t = 0, inner = 0, n = 0;
    if (!Rd32(p + 0x10, t) || !t) return 0;
    if (!Rd32(t, inner) || !inner) return 0;
    return Rd32(inner + 0xA8, n) ? static_cast<int>(n) : 0;
}

// 回复进度：[SCPoint+0x2C]（float）。满一格所需值来自需求表（实测 360）。
float ProtocolProgress() {
    const uintptr_t s = SCPointObj();
    if (!s) return 0.f;
    __try { return *reinterpret_cast<float*>(s + 0x2C); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0.f; }
}

bool ProtocolSetProgress(float v) {
    const uintptr_t s = SCPointObj();
    if (!s) { g_lastErr = "协议点对象不可用"; return false; }
    return WrF32(s + 0x2C, v);
}

// ---- 玩家对象字段探针：用「买一个协议 → 前后差分」找协议点，而不是猜偏移 ----
// 关键改进：先取两次快照（间隔 1 秒、不做任何操作）算出「漂移字段」——
// 帧计数器这类自己会走的字段（+0x98/+0xC0 实测 15/s）必须先标出来排除，
// 否则真字段会被噪声淹没。
static constexpr int kPlayerSnapSz = 0x10000;   // 64KB：玩家对象可能远大于 0x4000
static uint8_t  g_pSnap[kPlayerSnapSz];
static uint8_t  g_drift[kPlayerSnapSz / 4];      // 每个 u32 一个标记
static uintptr_t g_pSnapAddr = 0;
static int      g_pSnapLen = 0;
static int      g_driftCount = 0;

static bool ReadPlayer(uintptr_t p, void* dst, int* got) {
    SIZE_T n = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(p), dst,
                           kPlayerSnapSz, &n) || n == 0)
        return false;
    *got = static_cast<int>(n);
    return true;
}

bool PlayerFieldsSnap() {
    const uintptr_t p = PlayerObj();
    if (!p) { g_lastErr = "玩家对象不可用"; return false; }
    int got = 0;
    if (!ReadPlayer(p, g_pSnap, &got)) { g_lastErr = "读玩家对象失败"; return false; }
    g_pSnapAddr = p;
    g_pSnapLen = got;
    memset(g_drift, 0, sizeof(g_drift));
    g_driftCount = 0;
    return true;
}

// 第二次快照：算出漂移字段集，并把参考帧推进到「现在」
bool PlayerFieldsCalibrate() {
    const uintptr_t p = PlayerObj();
    if (!p || p != g_pSnapAddr || g_pSnapLen <= 0) { g_lastErr = "先 snap 再校准"; return false; }
    uint8_t now[kPlayerSnapSz];
    int got = 0;
    if (!ReadPlayer(p, now, &got)) { g_lastErr = "读玩家对象失败"; return false; }
    const int n = (g_pSnapLen < got) ? g_pSnapLen : got;
    g_driftCount = 0;
    for (int off = 0; off + 4 <= n; off += 4) {
        if (memcmp(g_pSnap + off, now + off, 4) != 0) {
            g_drift[off / 4] = 1;
            ++g_driftCount;
        }
    }
    memcpy(g_pSnap, now, n);
    return true;
}

void PlayerFieldsDiff(char* out, size_t outsz) {
    const uintptr_t p = PlayerObj();
    if (!p || p != g_pSnapAddr || g_pSnapLen <= 0) {
        _snprintf_s(out, outsz, _TRUNCATE, "没有快照（对象已变，先重新 snap + 校准）");
        return;
    }
    uint8_t now[kPlayerSnapSz];
    int got = 0;
    if (!ReadPlayer(p, now, &got)) { _snprintf_s(out, outsz, _TRUNCATE, "读玩家对象失败"); return; }
    const int n = (g_pSnapLen < got) ? g_pSnapLen : got;
    int shown = 0, driftSkipped = 0, ptrSkipped = 0;
    size_t w = 0;
    for (int off = 0; off + 4 <= n; off += 4) {
        if (memcmp(g_pSnap + off, now + off, 4) != 0) {
            if (g_drift[off / 4]) { ++driftSkipped; continue; }   // 已知漂移字段
            uint32_t a2 = *reinterpret_cast<uint32_t*>(g_pSnap + off);
            uint32_t b2 = *reinterpret_cast<uint32_t*>(now + off);
            const bool pa = (a2 >= 0x02000000 && a2 < 0x14000000);
            const bool pb = (b2 >= 0x02000000 && b2 < 0x14000000);
            if (pa && pb) { ++ptrSkipped; continue; }             // 指针漂移
            if (w + 48 < outsz && shown < 16) {
                w += _snprintf_s(out + w, outsz - w, _TRUNCATE, "+%04X:%u->%u ", off, a2, b2);
                ++shown;
            }
        }
    }
    if (shown == 0)
        _snprintf_s(out, outsz, _TRUNCATE, "无结构字段变化（跳过漂移 %d 处 / 指针 %d 处，漂移表 %d 项）",
                    driftSkipped, ptrSkipped, g_driftCount);
    else if (w + 64 < outsz)
        _snprintf_s(out + w, outsz - w, _TRUNCATE, "| 跳过漂移 %d 处 / 指针 %d 处",
                    driftSkipped, ptrSkipped);
}

// ---- 全内存按值扫描：先把「值为 X 的 u32」全找出来，变了之后再过滤 ----
// 不依赖任何对象结构，专门用来定这种「知道值、不知道位置」的字段。
static std::vector<uint32_t> g_scanHits;

int PScanMark(uint32_t value, uint32_t* outCount) {
    g_scanHits.clear();
    const uintptr_t lo = 0x00010000, hi = 0x7FFF0000;
    uintptr_t a = lo;
    const size_t kChunk = 1u << 20;
    uint8_t* buf = static_cast<uint8_t*>(malloc(kChunk + 4));
    if (!buf) return -1;
    while (a < hi) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi))) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t sz = mbi.RegionSize;
        const DWORD prot = mbi.Protect;
        const bool readable = mbi.State == MEM_COMMIT && (prot & PAGE_GUARD) == 0 &&
                              (prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                       PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                       PAGE_EXECUTE_WRITECOPY)) != 0;
        if (readable) {
            for (uintptr_t off = 0; off < sz; off += kChunk) {
                const size_t n = static_cast<size_t>((sz - off < kChunk) ? (sz - off) : kChunk);
                SIZE_T got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(),
                                       reinterpret_cast<LPCVOID>(base + off), buf, n, &got) || got == 0)
                    continue;
                for (size_t i = 0; i + 4 <= got; i += 4) {
                    uint32_t v = 0;
                    memcpy(&v, buf + i, 4);
                    if (v == value && g_scanHits.size() < 400000)
                        g_scanHits.push_back(static_cast<uint32_t>(base + off + i));
                }
            }
        }
        const uintptr_t next = base + sz;
        if (next <= a) break;
        a = next;
    }
    free(buf);
    if (outCount) *outCount = static_cast<uint32_t>(g_scanHits.size());
    return 0;
}

int PScanFilter(uint32_t want, uint32_t* outCount, char* out, size_t outsz) {
    size_t w = 0;
    uint32_t kept = 0;
    for (uint32_t addr : g_scanHits) {
        uint32_t v = 0;
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr),
                               &v, 4, &got) || got != 4)
            continue;
        if (v != want) continue;
        ++kept;
        if (kept <= 12 && w + 16 < outsz)
            w += _snprintf_s(out + w, outsz - w, _TRUNCATE, "%08X ", addr);
    }
    g_scanHits.clear();
    if (outCount) *outCount = kept;
    return 0;
}

// ---- 深层差分：玩家对象 + 它指向的一层子对象 ----
// 协议点不在玩家本体（实测买 1 点协议后 0x4000 内无变化），所以连子对象一起比。
// 子对象按「形似堆指针」收集，去重后最多 96 个，每个比 0x200 字节。
static constexpr int kDeepMax = 96;
static constexpr int kDeepSz  = 0x200;
static uint8_t  g_deepRoot[0x4000];
static int      g_deepRootLen = 0;
static uint32_t g_deepAddr[kDeepMax];
static uint8_t  g_deepSnap[kDeepMax][kDeepSz];
static int      g_deepN = 0;
static uintptr_t g_deepPlayer = 0;

static bool IsHeapPtr(uint32_t v) { return v >= 0x02000000 && v < 0x14000000; }

bool PlayerDeepSnap() {
    const uintptr_t p = PlayerObj();
    if (!p) { g_lastErr = "玩家对象不可用"; return false; }
    SIZE_T n = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(p),
                           g_deepRoot, sizeof(g_deepRoot), &n) || n == 0) {
        g_lastErr = "读玩家对象失败";
        return false;
    }
    g_deepRootLen = static_cast<int>(n);
    g_deepPlayer = p;
    g_deepN = 0;
    for (int off = 0; off + 4 <= g_deepRootLen && g_deepN < kDeepMax; off += 4) {
        const uint32_t v = *reinterpret_cast<uint32_t*>(g_deepRoot + off);
        if (!IsHeapPtr(v)) continue;
        bool dup = false;
        for (int k = 0; k < g_deepN; ++k)
            if (g_deepAddr[k] == v) { dup = true; break; }
        if (dup) continue;
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(v),
                               g_deepSnap[g_deepN], kDeepSz, &got) || got == 0)
            continue;
        g_deepAddr[g_deepN] = v;
        ++g_deepN;
    }
    return true;
}

void PlayerDeepDiff(char* out, size_t outsz) {
    const uintptr_t p = PlayerObj();
    if (!p || p != g_deepPlayer || g_deepRootLen <= 0) {
        _snprintf_s(out, outsz, _TRUNCATE, "没有深层快照（先 deepsnap）");
        return;
    }
    size_t w = 0;
    // 本体
    uint8_t now[0x4000];
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(p),
                           now, sizeof(now), &got) || got == 0) {
        _snprintf_s(out, outsz, _TRUNCATE, "读玩家对象失败");
        return;
    }
    const int n = (g_deepRootLen < static_cast<int>(got)) ? g_deepRootLen : static_cast<int>(got);
    int hits = 0;
    for (int off = 0; off + 4 <= n; off += 4) {
        if (memcmp(g_deepRoot + off, now + off, 4) == 0) continue;
        const uint32_t a = *reinterpret_cast<uint32_t*>(g_deepRoot + off);
        const uint32_t b = *reinterpret_cast<uint32_t*>(now + off);
        if (IsHeapPtr(a) && IsHeapPtr(b)) continue;
        if (w + 40 < outsz && hits < 10) {
            w += _snprintf_s(out + w, outsz - w, _TRUNCATE, "本体+%04X:%u->%u ", off, a, b);
            ++hits;
        }
    }
    // 子对象
    int subHits = 0;
    for (int k = 0; k < g_deepN && subHits < 12; ++k) {
        uint8_t cur[kDeepSz];
        SIZE_T g2 = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(g_deepAddr[k]),
                               cur, kDeepSz, &g2) || g2 == 0)
            continue;
        for (int off = 0; off + 4 <= static_cast<int>(g2); off += 4) {
            if (memcmp(g_deepSnap[k] + off, cur + off, 4) == 0) continue;
            const uint32_t a = *reinterpret_cast<uint32_t*>(g_deepSnap[k] + off);
            const uint32_t b = *reinterpret_cast<uint32_t*>(cur + off);
            if (IsHeapPtr(a) && IsHeapPtr(b)) continue;
            if (w + 56 < outsz && subHits < 12) {
                w += _snprintf_s(out + w, outsz - w, _TRUNCATE, "[%d]%08X+%03X:%u->%u ",
                                 k, g_deepAddr[k], off, a, b);
                ++subHits;
            }
        }
    }
    if (hits == 0 && subHits == 0)
        _snprintf_s(out, outsz, _TRUNCATE, "无变化（本体 0 处 / 子对象 %d 个）", g_deepN);
    else if (w + 40 < outsz)
        _snprintf_s(out + w, outsz - w, _TRUNCATE, "| 子对象 %d 个", g_deepN);
}

// ---- 全堆页级快照 / 差分 ----
// 用途：抓「展开某阵营基地后玩家身上哪个字段翻了」这种**没有结构线索**的状态变化。
// 只扫 MEM_PRIVATE 的已提交页（游戏堆），跳过本 DLL 镜像页与自己的缓冲页；每页一个 FNV-1a。
// 三步用法：
//   act=heapsnap                    建立基线
//   （场上做一次操作，例如展开/召唤某阵营基地）
//   act=heapdiff                    列出变化页，并把**变化后的字节**存下来
//   （把操作撤销，例如摧毁那个基地）
//   act=pagediff                    打印选中页里每个翻转的 u32（old->new）
static constexpr uint32_t kPageSz  = 0x1000;
static constexpr int      kPgKeep  = 256;
static constexpr size_t   kPageMax = 262144;

struct PageSig { uint32_t addr; uint32_t sum; };
static std::vector<PageSig>  g_pageSnap;
static std::vector<uint32_t> g_pgAddr;
static uint8_t               g_pgBytes[kPgKeep][0x1000];
static int                   g_pgN = 0;
static uint32_t              g_pgScanned = 0;

static uint32_t FnvPage(const uint8_t* p) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < kPageSz; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static void SelfModuleRange(uintptr_t* lo, uintptr_t* hi) {
    *lo = 0; *hi = 0;
    HMODULE hm = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&FnvPage, &hm) && hm) {
        *lo = reinterpret_cast<uintptr_t>(hm);
        *hi = *lo + 0x02000000;
    }
}

static uint8_t* PageBuf() {
    static uint8_t* p = nullptr;
    if (!p) p = static_cast<uint8_t*>(VirtualAlloc(nullptr, kPageSz, MEM_COMMIT, PAGE_READWRITE));
    return p;
}

bool HeapPageSnap(int* outPages) {
    if (g_pageSnap.capacity() < kPageMax) g_pageSnap.reserve(kPageMax);
    g_pageSnap.clear();
    g_pgAddr.clear();
    g_pgN = 0;
    uint8_t* page = PageBuf();
    if (!page) { g_lastErr = "页缓冲分配失败"; return false; }
    uintptr_t slo = 0, shi = 0;
    SelfModuleRange(&slo, &shi);
    const uint32_t bufLo = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_pageSnap.data()));
    const uint32_t bufHi = bufLo + static_cast<uint32_t>(g_pageSnap.capacity() * sizeof(PageSig)) + kPageSz;

    // 只扫游戏堆：0x02000000..0x14000000（玩家/协议点/科技对象/兵种全在这段）。
    // 全地址空间要 2.9 s，游戏线程会被按住；缩到这段约 1/7。
    uintptr_t a = 0x02000000;
    const uintptr_t hiEnd = 0x14000000;
    while (a < hiEnd) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi))) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t sz = mbi.RegionSize;
        const DWORD pr = mbi.Protect;
        const bool ok = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
                        (pr & PAGE_GUARD) == 0 &&
                        (pr & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                               PAGE_EXECUTE_WRITECOPY)) != 0;
        if (ok) {
            for (uintptr_t off = 0; off + kPageSz <= sz; off += kPageSz) {
                const uintptr_t pa = base + off;
                if (pa >= slo && pa < shi) continue;
                const uint32_t pa32 = static_cast<uint32_t>(pa);
                if (pa32 >= bufLo && pa32 < bufHi) continue;
                SIZE_T got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(pa),
                                       page, kPageSz, &got) || got != kPageSz)
                    continue;
                PageSig sg; sg.addr = pa32; sg.sum = FnvPage(page);
                g_pageSnap.push_back(sg);
                if (g_pageSnap.size() >= kPageMax) break;
            }
        }
        const uintptr_t next = base + sz;
        if (next <= a) break;
        a = next;
    }
    g_pgScanned = static_cast<uint32_t>(g_pageSnap.size());
    if (outPages) *outPages = static_cast<int>(g_pageSnap.size());
    return true;
}

// mode 0 = 本次变化集；1 = 与上一轮取交集；2 = 从上一轮里减掉（用来剔掉每帧都在变的抖动页）
static std::vector<uint32_t> g_pgPrev;

static void KeepBytes(const std::vector<uint32_t>& v) {
    uint8_t* page = PageBuf();
    g_pgN = 0;
    if (!page) return;
    for (size_t i = 0; i < v.size() && g_pgN < kPgKeep; ++i) {
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(v[i])),
                               page, kPageSz, &got) || got != kPageSz)
            continue;
        memcpy(g_pgBytes[g_pgN], page, kPageSz);
        g_pgAddr.push_back(v[i]);
        ++g_pgN;
    }
}

void HeapPageDiff(char* out, size_t outsz, int mode) {
    if (g_pageSnap.empty()) { _snprintf_s(out, outsz, _TRUNCATE, "没有页快照（先 act=heapsnap）"); return; }
    uint8_t* page = PageBuf();
    if (!page) { _snprintf_s(out, outsz, _TRUNCATE, "页缓冲不可用"); return; }

    std::vector<uint32_t> cur;
    int changed = 0, gone = 0;
    for (size_t i = 0; i < g_pageSnap.size(); ++i) {
        const PageSig& sg = g_pageSnap[i];
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(sg.addr)),
                               page, kPageSz, &got) || got != kPageSz) { ++gone; continue; }
        if (FnvPage(page) == sg.sum) continue;
        ++changed;
        cur.push_back(sg.addr);
    }

    std::vector<uint32_t> prev; prev.swap(g_pgPrev);
    g_pgPrev = cur;                      // 本轮变化集，供下一轮交集/差集

    std::vector<uint32_t> res;
    if (mode == 1) {
        for (size_t i = 0; i < cur.size(); ++i)
            for (size_t j = 0; j < prev.size(); ++j)
                if (cur[i] == prev[j]) { res.push_back(cur[i]); break; }
    } else if (mode == 2) {              // cur - prev：剔掉上一轮也在变的抖动页
        for (size_t i = 0; i < cur.size(); ++i) {
            bool inPrev = false;
            for (size_t j = 0; j < prev.size(); ++j) if (cur[i] == prev[j]) { inPrev = true; break; }
            if (!inPrev) res.push_back(cur[i]);
        }
    } else {
        res = cur;
    }

    // 结果页的当前字节存下来，供 pagediff
    g_pgAddr.clear(); g_pgN = 0;
    KeepBytes(res);

    size_t w = 0;
    const int shown = (res.size() < 40) ? (int)res.size() : 40;
    for (int i = 0; i < shown && w + 12 < outsz; ++i)
        w += _snprintf_s(out + w, outsz - w, _TRUNCATE, "%08X ", res[i]);
    if (w < outsz)
        _snprintf_s(out + w, outsz - w, _TRUNCATE,
                    "| 本次变 %d 页 / 结果 %d 页（抓 %d 页字节）/ 共 %u 页 / 失联 %d",
                    changed, (int)res.size(), g_pgN, g_pgScanned, gone);
}

void HeapPageDetail(char* out, size_t outsz) {
    if (!g_pgN) { _snprintf_s(out, outsz, _TRUNCATE, "没有抓取页（先在变化后调 act=heapdiff）"); return; }
    uint8_t cur[0x1000];
    size_t w = 0; int hits = 0, pages = 0;
    for (int k = 0; k < g_pgN; ++k) {
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(g_pgAddr[k])),
                               cur, kPageSz, &got) || got != kPageSz)
            continue;
        int n = 0;
        for (uint32_t off = 0; off + 4 <= kPageSz; off += 4) {
            uint32_t x = 0, y = 0;
            memcpy(&x, g_pgBytes[k] + off, 4);
            memcpy(&y, cur + off, 4);
            if (x == y) continue;
            ++n;
            if (n <= 8 && w + 40 < outsz) {
                w += _snprintf_s(out + w, outsz - w, _TRUNCATE, "%08X+%03X:%u->%u ",
                                 g_pgAddr[k], off, x, y);
                ++hits;
            }
        }
        if (n) ++pages;
    }
    if (w + 48 < outsz)
        _snprintf_s(out + w, outsz - w, _TRUNCATE, "| %d 页有变化", pages);
    if (!hits) _snprintf_s(out, outsz, _TRUNCATE, "抓下的 %d 页里没有 u32 变化", g_pgN);
}

// 诊断一行：引擎 / 当前脚本玩家（0 = <This Player> 解析不了）/ 玩家对象 / 序号
void ProtocolDiag(char* out, size_t outsz) {
    uint32_t eng = 0, cur = 0;
    Rd32(kScriptEngine, eng);
    if (eng) Rd32(eng + kCurPlayerOff, cur);
    const uintptr_t pl = PlayerObj();
    const char* nm = LocalPlayerName();
    _snprintf_s(out, outsz, _TRUNCATE,
                "engine=%08X ctx=%08X player=%08X idx=%u name=%s money=%08X sc=%08X",
                eng, cur, (unsigned)pl, pl ? *(uint32_t*)(pl + 0x20) : 0u,
                nm[0] ? nm : "?", g_moneyObj, (unsigned)SCPointObj());
}

// ExecuteAction 调用记录已移除（2026-10-19）：挂 0x178670 会在实时对局里把游戏打崩，
// 收益不抵风险。要看参数，改用离线反汇编或单独开一局做实验。

const char* ProtocolLastError() { return g_lastErr; }

} // namespace ra3
