// httpapi.cpp - 进程内调试接口实现（winsock，仅 127.0.0.1）
// winsock2.h 必须在 windows.h 之前（否则 windows.h 会先拉进旧 winsock.h，导致 ws2tcpip.h 报错）
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "httpapi.h"
#include "overlay.h"
#include "features.h"
#include "actions.h"
#include "battlefield.h"
#include "protocol.h"
#include "aicmd.h"
#include "battle_catalog.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>

namespace ra3 {
namespace {

SOCKET g_listen = INVALID_SOCKET;
volatile LONG g_stop = 0;
HANDLE g_httpThread = nullptr;   // 留句柄给 HttpApiJoin（热重载卸载时等线程真退出）

// ---- 脑机接口（MCP）遥测（2026-10-16 起取消「总闸」）----
// 监听只绑 127.0.0.1；把接口交给 AI 即视为授权，故不再拦截任何变更请求。
volatile LONG g_aiOpen = 1;      // 恒开放（保留字段，供 /ai/status 与面板显示兼容）
volatile LONG g_aiCalls = 0;     // 带 ai=1 的调用计数（纯遥测）
volatile LONG g_aiRejects = 0;   // 历史遗留，恒 0
const int kAiLogMax = 64;
struct AiLine { char t[16]; char text[152]; };
AiLine   g_aiLog[kAiLogMax];
volatile LONG g_aiLogHead = 0, g_aiLogCount = 0;
CRITICAL_SECTION g_aiCs;
volatile LONG g_aiCsInit = 0;

void AiCsEnter() { EnterCriticalSection(&g_aiCs); }
void AiCsLeave() { LeaveCriticalSection(&g_aiCs); }
void AiCsInit() {
    if (InterlockedCompareExchange(&g_aiCsInit, 1, 0) == 0) InitializeCriticalSection(&g_aiCs);
}

// ---- 命令槽：HTTP 线程 → 渲染线程 ----
// state: 0=空 1=待执行 2=已完成
struct CmdSlot {
    volatile LONG state;
    int  fn;
    int  a1;
    int  a2;
    int  a3;
    int  a4;
    int  a5;
    int  a6;
    char out[4096];
};
CmdSlot g_cmd;
const int kCmdWaitMs = 2500;

void CmdReply(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(g_cmd.out, sizeof(g_cmd.out), _TRUNCATE, fmt, ap);
    va_end(ap);
}

// ---- JSON 小工具 ----
int JsonEsc(char* dst, int n, const char* s) {
    int w = 0;
    auto put = [&](char c) { if (w < n - 1) dst[w++] = c; };
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s ? s : ""); *p; ++p) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') { put('\\'); put(static_cast<char>(c)); }
        else if (c < 0x20) { put(' '); }
        else put(static_cast<char>(c));
    }
    dst[w] = 0;
    return w;
}

// 把「单价目录名」按哈希取显示名
const char* NmOf(uint32_t h) {
    const BattleObj* o = BattleLookup(h);
    return o ? o->name : "?";
}

// ---- 战场快照 JSON（只读；在 HTTP 线程执行，不调游戏函数）----
int BuildSnapshot(char* buf, int n, bool enemyOnly) {
    BattleSummary s;
    ScanBattlefield(&s, false);          // 走缓存（TTL 内不重扫）
    int w = 0;
#define AP(...) w += _snprintf_s(buf + w, n - w, _TRUNCATE, __VA_ARGS__)
    AP("{\"ok\":1,\"tick\":%u,\"player\":%d,\"money\":%d,\"selection\":%d",
       (unsigned)s.tickNow, LocalPlayerId(), (int)LocalMoney(), SelectedCount());
    AP(",\"me\":{\"units\":%d,\"structs\":%d}", s.localUnits, s.localStructs);

    // 选中单位（按哈希聚合）
    AP(",\"selected\":[");
    {
        uint32_t hs[64]; int cs[64];
        int k = SelectedHashHistogram(hs, cs, 64);
        for (int i = 0; i < k; ++i) {
            char nm[96]; JsonEsc(nm, sizeof(nm), NmOf(hs[i]));
            AP("%s{\"name\":\"%s\",\"hash\":\"0x%08X\",\"n\":%d}",
               i ? "," : "", nm, (unsigned)hs[i], cs[i]);
        }
    }
    AP("]");

    // 我方构成 + 敌方明细
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 0 && enemyOnly) continue;     // /enemy 只出敌方
        AP(pass == 0 ? ",\"myUnits\":[" : ",\"enemies\":[");
        int first = 1;
        for (int i = 0; i < s.owners && i < 16; ++i) {
            const BattleOwner& o = s.list[i];
            if (pass == 0 && !o.isLocal) continue;
            if (pass == 1 && o.isLocal) continue;
            if (pass == 1 && o.playable == 0) continue;   // 敌方只列有战斗力的
            if (pass == 0) {
                for (int j = 0; j < o.topN; ++j) {
                    char nm[96]; JsonEsc(nm, sizeof(nm), o.top[j].name);
                    AP("%s{\"name\":\"%s\",\"n\":%d}", first ? "" : ",", nm, o.top[j].count);
                    first = 0;
                }
            } else {
                const char* side = (o.side >= 0 && o.side < B_SIDE_COUNT)
                                   ? kBattleSideNames[o.side] : "?";
                char sideEsc[32]; JsonEsc(sideEsc, sizeof(sideEsc), side);
                AP("%s{\"idx\":%d,\"units\":%d,\"structs\":%d,\"side\":\"%s\",\"top\":[",
                   first ? "" : ",", o.idx, o.units, o.structs, sideEsc);
                for (int j = 0; j < o.topN; ++j) {
                    char nm[96]; JsonEsc(nm, sizeof(nm), o.top[j].name);
                    AP("%s{\"name\":\"%s\",\"n\":%d}", j ? "," : "", nm, o.top[j].count);
                }
                AP("]}");
                first = 0;
            }
        }
        AP("]");
    }

    AP(",\"totals\":{\"playable\":%d,\"unknown\":%d}", s.totalPlayable, s.totalUnknown);
    AP(",\"scan\":{\"ms\":%u,\"mb\":%d,\"vt\":\"0x%08X\",\"cached\":%d}}",
       (unsigned)s.elapsedMs, s.scannedMb, (unsigned)BattleThingVtable(), s.fromCache ? 1 : 0);
#undef AP
    return w;
}

// ---- RE 小工具（安全访问，SEH 包裹）----
bool RdBytes(uintptr_t at, void* dst, size_t n) {
    __try { memcpy(dst, reinterpret_cast<const void*>(at), n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WrDword(uintptr_t at, uint32_t v) {
    __try { *reinterpret_cast<uint32_t*>(at) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
uintptr_t ModBase() { return reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)); }

// 按 RVA 调用游戏函数（渲染线程内，SEH 包裹）。cc: 0=cdecl 1=fastcall
uintptr_t CallRva(uint32_t rva, int cc, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    uintptr_t fn = ModBase() + rva;
    uintptr_t ret = 0;
    if (cc) {
        typedef uintptr_t(__fastcall* F)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
        __try { ret = ((F)fn)(a0, a1, a2, a3, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { ret = 0xDEADDEADu; }
    } else {
        typedef uintptr_t(__cdecl* F)(uint32_t, uint32_t, uint32_t, uint32_t);
        __try { ret = ((F)fn)(a0, a1, a2, a3); } __except (EXCEPTION_EXECUTE_HANDLER) { ret = 0xDEADDEADu; }
    }
    return ret;
}

// ---- P2 指挥层：正统 thiscall 调用器已抽到 aicmd.cpp（ra3::CallThisFn / OrderAttack / OrderMoveTo）----

// Coord3D 传参 scratch（DLL 自有内存，不经游戏堆；渲染线程/HTTP 线程写都安全）
static float g_coordScratch[8];   // 两组：[0..2] 主 [4..6] 备

// 脚本动作字符串参数：[0]=动作名 [1..3]=参数。命令槽只带 int，串走这里；
// HTTP 线程写 → 渲染线程读，PostAndWaitN 串行化，无并发。
static char g_scriptArgs[4][96];

// 在渲染线程里执行（由 HttpApiPump 调用）
void ExecCmd(CmdSlot& c) {
    switch (c.fn) {
    case 1: {   // 诊断文本（含调用游戏函数，必须在本线程）
        int n = DiagText(c.out, (int)sizeof(c.out));
        (void)n;
        break;
    }
    case 2: {   // 哈希 -> 模板（同样调用游戏函数）
        uintptr_t t = ResolveTemplate((uint32_t)c.a1);
        CmdReply("hash %08X -> template %08X", (unsigned)c.a1, (unsigned)t);
        break;
    }
    case 4: {   // copy
        int r = ActCopySelected();
        CmdReply(r > 0 ? "ok copy=1" : "fail copy: %s", r > 0 ? "" : SummonLastError());
        break;
    }
    case 5: {   // 预设增援 ×8（基于 CE 的 We Need Back）
        int r = ActSummonSquad();
        CmdReply(r > 0 ? "ok wnb=%d" : "fail wnb: %s", r, r > 0 ? "" : SummonLastError());
        break;
    }
    case 6:     // 基地车
        CmdReply(SummonMCV() ? "ok mcv" : "fail mcv: %s", SummonLastError());
        break;
    case 7:     // 收编
        CmdReply("capture=%d %s", ActSetOwner(), SummonLastError());
        break;
    case 8:     // 摧毁选中
        CmdReply("kill=%d", ActKillSelected());
        break;
    case 9:     // 满级
        CmdReply("rank=%d %s", ActRank(), SummonLastError());
        break;
    case 10:    // 速度：a1 >= 0 = 档位 mode；a1 < 0 = 按 a2 的绝对速度值（滑条）
        if (c.a1 < 0) CmdReply("speed=%.0f units=%d", (float)c.a2, ActSpeedValue((float)c.a2));
        else          CmdReply("speed=%d", ActSpeed(c.a1));
        break;
    case 11:    // 血量 mode
        CmdReply("health=%d", ActHealth(c.a1));
        break;
    case 12: {   // 召唤：a1=hash a2=count a3=teamOverride(0=本地玩家)
        const bool ok = SummonByHashFor((uint32_t)c.a1, c.a2 > 0 ? c.a2 : 1,
                                        (uintptr_t)(uint32_t)c.a3, false, "api");
        uintptr_t objs[20];
        const int nobj = LastCreatedObjs(objs, 20);
        char list[192] = { 0 };
        int w = 0;
        for (int i = 0; i < nobj && w < (int)sizeof(list) - 12; ++i)
            w += _snprintf_s(list + w, sizeof(list) - w, _TRUNCATE,
                             "%s%08X", i ? "," : "", (unsigned)objs[i]);
        float lp[4] = { 0, 0, 0, 1 }; const char* lsrc = "?";
        LastSpawnPos(lp, &lsrc);
        CmdReply("summon=%d objs=[%s] at=%.0f,%.0f,%.0f via=%s %s",
                 ok ? 1 : 0, list, lp[0], lp[1], lp[2], lsrc, SummonLastError());
        break;
    }
    case 13: {   // 空投 idx count
        OverlaySetCrate(c.a1, c.a2 > 0 ? c.a2 : 1);
        CmdReply("crate=%d %s", DropCrate(0, c.a2 > 0 ? c.a2 : 1, true) ? 1 : 0, SummonLastError());
        break;
    }
    case 14:    // 开关 f=索引 on=0/1
        if (c.a1 >= 0 && c.a1 < FID_COUNT) SetFeature(c.a1, c.a2 != 0);
        CmdReply("ok toggle %d=%d", c.a1, c.a2);
        break;
    case 15:    // 免费建造
        CmdReply("freebuild=%d", SetFreeBuild(c.a1 != 0) ? 1 : 0);
        break;
    case 16:    // 任意动作：按 RunActionById
        CmdReply("act=%d", 1);
        break;
    case 17: {  // poke: a1=addr a2=type(0=u32 1=f32 2=u8 3=u16) a3=val
        uintptr_t at = (uintptr_t)(uint32_t)c.a1;
        uint32_t v = (uint32_t)c.a3;
        bool ok = false;
        if (c.a2 == 2)      { __try { *(uint8_t*)at = (uint8_t)v; ok = true; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        else if (c.a2 == 3) { __try { *(uint16_t*)at = (uint16_t)v; ok = true; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        else                { ok = WrDword(at, v); }
        CmdReply("poke %08X <- %08X : %s", (unsigned)at, v, ok ? "ok" : "fail");
        break;
    }
    case 18: {  // call: a1=rva a2=cc a3,a4,a5=args（a0 由 a3 起）
        uintptr_t r = CallRva((uint32_t)c.a1, c.a2, (uint32_t)c.a3, (uint32_t)c.a4, (uint32_t)c.a5, 0);
        CmdReply("call rva=%X ret=%08X %s", (unsigned)c.a1, (unsigned)r,
                 r == 0xDEADDEADu ? "(EXCEPTION)" : "");
        break;
    }
    case 20: {  // aicall: a1=fnVA a2=this a3..a5=栈参a0,a1,a2 a6=栈参数量(0..3)
        uintptr_t r = CallThisFn((uintptr_t)c.a1, (uintptr_t)c.a2,
                                 (uintptr_t)c.a3, (uintptr_t)c.a4, (uintptr_t)c.a5, (int)c.a6);
        if (r == 0xDEADDEADu) {
            uintptr_t ea = 0; uint32_t ec = 0; LastThisCallExc(&ea, &ec);
            CmdReply("aicall fn=%08X this=%08X ret=DEADDEAD exc@%08X code=%08X",
                     (unsigned)c.a1, (unsigned)c.a2, (unsigned)ea, ec);
        } else
            CmdReply("aicall fn=%08X this=%08X ret=%08X", (unsigned)c.a1, (unsigned)c.a2, (unsigned)r);
        break;
    }
    case 22: {  // protocol: a1=act(0=points 1=science 2=授予技能 3=全协议 4=快速 5=禁用 20=快照 21=差分)
        const int act = (int)c.a1;
        if (act == 0) {
            const int n = (int)c.a2;
            const bool ok = ProtocolSetPoints(n);
            CmdReply("protocol points <- %d : %s", n, ok ? "ok" : "fail");
        } else if (act == 1) {
            const bool ok = ProtocolSetScience(399.0f);
            CmdReply("protocol science <- 399.0f : %s", ok ? "ok" : "fail");
        } else if (act == 2) {          // 授予技能（索引见 /protocol?act=read 的 count）
            char out[224] = { 0 };
            const int r = ProtoGrantPower((int)c.a3, out, sizeof(out));
            CmdReply("protocol grant %d %s", r, out);
        } else if (act == 3 + 0 || act == 3 || act == 4 || act == 5) {
            const int which = act - 3;
            const bool on = c.a4 != 0;
            const bool ok = ProtocolSetToggle(which, on);
            CmdReply("protocol toggle %d=%d : %s", which, on ? 1 : 0, ok ? "ok" : "fail");
        } else if (act == 8) {          // 解锁科技
            char out[224] = { 0 };
            const int r = ProtoGrantTech((int)c.a3, out, sizeof(out));
            CmdReply("protocol tech %d %s", r, out);
        } else if (act == 20) {
            const bool ok = PlayerFieldsSnap();
            CmdReply("protocol snap : %s", ok ? "ok（现在去游戏里买一个协议，再调 act=21）" : ProtocolLastError());
        } else if (act == 30) {          // unlock: 写协议格解锁索引（n=-1 视为「解锁到最后一格」）
            int n = (int)c.a2;
            const int total = ProtocolSlotTotal();
            if (n < 0 && total > 0) n = total - 1;
            const bool ok = ProtocolSetUnlockIndex(n);
            CmdReply("protocol unlock idx=%d/%d : %s", n, total, ok ? "ok" : ProtocolLastError());
        } else if (act == 31) {          // slot: 读解锁进度
            CmdReply("protocol unlock=%d/%d progress=%.1f points=%d",
                     ProtocolUnlockIndex(), ProtocolSlotTotal(), ProtocolProgress(), ProtocolPoints());
        } else if (act == 32) {          // ui: 协议格刷新遥测（按 esi 去重，取最新一轮）
            using namespace ra3;
            char out[3900] = { 0 };
            int w = 0;
            int k = _snprintf_s(out, sizeof(out), _TRUNCATE,
                                "pa=%u pc=%u\n", g_paCount, g_pcCount);
            if (k > 0) w = k;
            for (int i = (int)g_paCount - 1; i >= 0 && w < 3600; --i) {
                const uint32_t* r = &g_paBuf[i * 4];
                bool dup = false;
                for (int j = i + 1; j < (int)g_paCount; ++j)
                    if (g_paBuf[j * 4] == r[0]) { dup = true; break; }
                if (dup) continue;
                k = _snprintf_s(out + w, sizeof(out) - w, _TRUNCATE,
                                "slot esi=%08X obj=%08X raw=%u forced=%u\n",
                                r[0], r[1], r[2], r[3]);
                if (k > 0) w += k; else break;
            }
            CmdReply("%s", out);
        } else if (act == 33) {          // ui: 协议格点击遥测
            using namespace ra3;
            char out[3900] = { 0 };
            int w = 0;
            int k = _snprintf_s(out, sizeof(out), _TRUNCATE, "tap=%u\n", g_pcCount);
            if (k > 0) w = k;
            for (int i = 0; i < (int)g_pcCount && w < 3700; ++i) {
                const uint32_t* r = &g_pcBuf[i * 6];
                const unsigned begin = r[4], end = r[5];
                const int n = (begin && end && end >= begin) ? (int)((end - begin) / 4) : -1;
                k = _snprintf_s(out + w, sizeof(out) - w, _TRUNCATE,
                                "tap esi=%08X par=%08X obj=%08X own=%u list=%08X..%08X n=%d\n",
                                r[0], r[1], r[2], r[3], begin, end, n);
                if (k > 0) w += k; else break;
            }
            CmdReply("%s", out);
        } else if (act == 41) {          // owned: 读已购协议 id 列表
            const int n = ProtocolOwnedCount();
            char out[1200] = { 0 };
            int w = 0;
            int k = _snprintf_s(out, sizeof(out), _TRUNCATE, "owned=%d:", n);
            if (k > 0) w = k;
            for (int i = 0; i < n && w < 1100; ++i) {
                uint32_t id = 0;
                if (!ProtocolOwnedAt(i, &id)) break;
                k = _snprintf_s(out + w, sizeof(out) - w, _TRUNCATE, " %08X", id);
                if (k > 0) w += k; else break;
            }
            CmdReply("%s", out);
        } else if (act == 40) {          // claim: 直接把 protocol id 塞进已购列表
            const bool ok = ProtocolClaimId((uint32_t)c.a2);
            CmdReply("protocol claim id=%08X : %s", (unsigned)c.a2, ok ? "ok" : ProtocolLastError());
        } else if (act == 34) {          // ui: 清零遥测
            using namespace ra3;
            g_paCount = 0;
            g_pcCount = 0;
            CmdReply("protoui reset");
        } else if (act == 26) {          // pscan mark: 扫描「值==n」的 u32
            uint32_t cnt = 0;
            PScanMark((uint32_t)c.a2, &cnt);
            CmdReply("pscan mark val=%u 命中=%u（花掉它之后用 act=27&n=新值）", (unsigned)c.a2, cnt);
        } else if (act == 27) {          // pscan filter: 在命中里筛「现在==n」
            char o6[300] = { 0 };
            uint32_t cnt = 0;
            PScanFilter((uint32_t)c.a2, &cnt, o6, sizeof(o6));
            CmdReply("pscan filter val=%u 剩余=%u %s", (unsigned)c.a2, cnt, o6);
        } else if (act == 24) {
            const bool ok = PlayerDeepSnap();
            CmdReply("protocol deepsnap : %s", ok ? "ok（现在买一个协议，再调 act=25）" : ProtocolLastError());
        } else if (act == 25) {
            char out[900] = { 0 };
            PlayerDeepDiff(out, sizeof(out));
            CmdReply("protocol deepdiff %s", out);
        } else if (act == 22) {
            const bool ok = PlayerFieldsCalibrate();
            CmdReply("protocol calib : %s", ok ? "ok（已排除漂移字段，现在去买一个协议）" : ProtocolLastError());
        } else if (act == 21) {
            char out[512] = { 0 };
            PlayerFieldsDiff(out, sizeof(out));
            CmdReply("protocol diff %s", out);
        } else {
            char dg[160] = { 0 };
            ProtocolDiag(dg, sizeof(dg));
            CmdReply("protocol points=%d science=%.1f all=%d fast=%d disable=%d powers=%d | %s",
                     ProtocolPoints(), ProtocolScience(),
                     ProtocolGetToggle(0) ? 1 : 0,
                     ProtocolGetToggle(1) ? 1 : 0,
                     ProtocolGetToggle(2) ? 1 : 0,
                     ProtocolCount(), dg);
        }
        break;
    }

    case 21: {  // order: a1=unit(Thing) a2=act(0=attack 1=move 2=idle) a3=target(Thing) a4..a6=xyz(float位)
        const uintptr_t unit = (uintptr_t)(uint32_t)c.a1;
        const int act = c.a2;
        bool ok = false;
        if (act == 0)      ok = OrderAttack(unit, (uintptr_t)(uint32_t)c.a3);
        else if (act == 1) {
            float pos[3];
            pos[0] = *(float*)&c.a4; pos[1] = *(float*)&c.a5; pos[2] = *(float*)&c.a6;
            ok = OrderMoveTo(unit, pos);
        } else if (act == 2) ok = OrderIdle(unit);
        CmdReply("order act=%d unit=%08X target=%08X -> %s",
                 act, (unsigned)unit, (unsigned)(uint32_t)c.a3, ok ? "ok" : AicmdLastError());
        break;
    }
    case 19: {  // 创建后处理链：a1=team(0=本地玩家) a2=obj a3=runA a4=runB
        uintptr_t team = (uintptr_t)(uint32_t)c.a1;
        if (!team) team = LocalPlayerObj();
        const bool ok = PostProcessCall(team, (uintptr_t)(uint32_t)c.a2, c.a3 != 0, c.a4 != 0);
        CmdReply("postproc(P%s%s) team=%08X obj=%08X : %s",
                 c.a3 ? "A" : "", c.a4 ? "B" : "", (unsigned)team,
                 (unsigned)(uint32_t)c.a2, ok ? "ok" : PostProcLastError());
        break;
    }
    case 23: {  // 落点查询：a1=kind(0 视野中心 / 1 选中单位 / 2 光标 / 3 自动)
        char j[320] = { 0 };
        SpawnPointJson(j, sizeof(j), c.a1);
        CmdReply("%s", j);
        break;
    }
    case 24: {  // 单位状态：a1=kind(-1=重置全部) a2=on(1 施加 / 0 解除) a3=时长(秒)
        if (c.a1 == -1)
            CmdReply("status reset -> %d %s", ActStatusReset(), SummonLastError());
        else
            CmdReply("status %s(%d) %s -> %d %s",
                     UnitStatusName(c.a1), c.a1, c.a2 ? "apply" : "remove",
                     ActUnitStatus(c.a1, c.a2, c.a3), SummonLastError());
        break;
    }
    case 25: {  // burst: a1=hash a2=delayMs(0=同帧) —— 创建箱子并引爆
        const int r = ActBurstCrate((uint32_t)c.a1, (int)c.a2);
        CmdReply("burst=%d obj=%08X %s", r, (unsigned)BurstLastObj(), BurstLastError());
        break;
    }
    case 31: {  // aidx: 脚本动作表按名字查 id（g_scriptArgs[0]=动作名）
        char o7[256] = { 0 };
        const int ia = ActionIndexByName(g_scriptArgs[0], o7, sizeof(o7));
        CmdReply("aidx %d %s", ia, o7);
        break;
    }
    case 30: {  // grant: 按名字授予 SpecialPower（g_scriptArgs[0]=技能名 [1]=玩家过滤器）
        char o6[512] = { 0 };
        const int rg = ProtoGrantByName(g_scriptArgs[0],
                                        g_scriptArgs[1][0] ? g_scriptArgs[1] : "<This Player>",
                                        o6, sizeof(o6));
        CmdReply("grant r=%d %s", rg, o6);
        break;
    }
    case 27: {  // action: 引擎脚本动作，动作名与参数取自 g_scriptArgs；a4=mode
        char o2[512] = { 0 };
        const int r = LuaExecuteAction(g_scriptArgs[0],
                                       g_scriptArgs[1][0] ? g_scriptArgs[1] : nullptr,
                                       g_scriptArgs[2][0] ? g_scriptArgs[2] : nullptr,
                                       g_scriptArgs[3][0] ? g_scriptArgs[3] : nullptr,
                                       o2, sizeof(o2), (int)c.a4);
        CmdReply("%s", o2);
        (void)r;
        break;
    }
    case 26: {  // buildable: a1..a3=xyz(float) a4=模板哈希 a5=angle(float) a6=options
        const float bx = *(float*)&c.a1, by = *(float*)&c.a2, bz = *(float*)&c.a3;
        const int r = BuildableAt(bx, by, bz, (uint32_t)c.a4, *(float*)&c.a5, (uint32_t)c.a6);
        if (r < 0)
            CmdReply("buildable (%.1f,%.1f,%.1f) hash=%08X -> 失败 %s",
                     bx, by, bz, (unsigned)(uint32_t)c.a4, BuildableLastError());
        else
            CmdReply("buildable (%.1f,%.1f,%.1f) hash=%08X angle=%.1f opts=%04X -> code=%d%s",
                     bx, by, bz, (unsigned)(uint32_t)c.a4, *(float*)&c.a5,
                     (unsigned)(uint32_t)c.a6, r, r == 0 ? " 可建" : " 不可建");
        break;
    }
    case 29: {  // luatest: Lua 栈自检
        char o4[512] = { 0 };
        LuaSelfTest(g_scriptArgs[0], o4, sizeof(o4));
        CmdReply("%s", o4);
        break;
    }
    case 28: {  // lua: a1=RVA，参数取自 g_scriptArgs[0..2]（只支持名字串）
        char o3[512] = { 0 };
        LuaCallRva((uint32_t)c.a1,
                   g_scriptArgs[0][0] ? g_scriptArgs[0] : nullptr,
                   g_scriptArgs[1][0] ? g_scriptArgs[1] : nullptr,
                   g_scriptArgs[2][0] ? g_scriptArgs[2] : nullptr,
                   o3, sizeof(o3));
        CmdReply("%s", o3);
        break;
    }
    default:
        CmdReply("unknown fn=%d", c.fn);
        break;
    }
}

// ---- HTTP 小工具 ----
int StrToI(const char* s) { return s ? atoi(s) : 0; }

// 从 query 里取参数：?a=1&b=2
bool GetParam(const char* q, const char* key, char* out, int n) {
    if (!q || !key) return false;
    char pat[32];
    _snprintf_s(pat, sizeof(pat), _TRUNCATE, "%s=", key);
    const char* p = strstr(q, pat);
    if (!p) return false;
    p += strlen(pat);
    int i = 0;
    while (p[i] && p[i] != '&' && i < n - 1) { out[i] = p[i]; ++i; }
    out[i] = 0;
    return true;
}
int GetParamInt(const char* q, const char* key, int def) {
    char b[32];
    if (!GetParam(q, key, b, sizeof(b))) return def;
    return (int)strtoul(b, nullptr, 0);   // atoi 不认 0x 前缀（左下角惨案真根因）；base=0 同时支持十/十六进制
}

// %xx 与 + 解码（脚本动作参数里常带 "<This Player>" 这类尖括号）
void URLDecode(const char* in, char* out, int n) {
    int i = 0, o = 0;
    while (in && in[i] && o < n - 1) {
        if (in[i] == '%' && in[i + 1] && in[i + 2]) {
            char h[3] = { in[i + 1], in[i + 2], 0 };
            out[o++] = (char)strtoul(h, nullptr, 16);
            i += 3;
        } else if (in[i] == '+') {
            out[o++] = ' ';
            ++i;
        } else {
            out[o++] = in[i++];
        }
    }
    out[o] = 0;
}

// /status?kind= 名字 → US_* 枚举；不认识返回 -1（调用方回退按数字解析）
int StatusKindFromName(const char* s) {
    if (!s || !s[0]) return -1;
    if (!_stricmp(s, "ironcurtain") || !_stricmp(s, "iron")) return US_IRONCURTAIN;
    if (!_stricmp(s, "invulnerable") || !_stricmp(s, "invul")) return US_INVULNERABLE;
    if (!_stricmp(s, "speedup"))  return US_SPEEDUP;
    if (!_stricmp(s, "slow"))     return US_SLOW;
    if (!_stricmp(s, "brittle"))  return US_BRITTLE;
    if (!_stricmp(s, "damageup")) return US_DAMAGEUP;
    return -1;
}

// 把命令投递给渲染线程并等结果
bool PostAndWaitN(int fn, const int* args, int na, char* out, int n) {
    if (InterlockedCompareExchange(&g_cmd.state, 1, 0) != 0) {
        _snprintf_s(out, n, _TRUNCATE, "busy"); return false;
    }
    g_cmd.fn = fn;
    g_cmd.a1 = na > 0 ? args[0] : 0;
    g_cmd.a2 = na > 1 ? args[1] : 0;
    g_cmd.a3 = na > 2 ? args[2] : 0;
    g_cmd.a4 = na > 3 ? args[3] : 0;
    g_cmd.a5 = na > 4 ? args[4] : 0;
    g_cmd.a6 = na > 5 ? args[5] : 0;
    g_cmd.out[0] = 0;
    for (int i = 0; i < kCmdWaitMs / 10; ++i) {
        if (g_cmd.state == 2) {
            _snprintf_s(out, n, _TRUNCATE, "%s", g_cmd.out);
            InterlockedExchange(&g_cmd.state, 0);
            return true;
        }
        Sleep(10);
    }
    InterlockedExchange(&g_cmd.state, 0);
    _snprintf_s(out, n, _TRUNCATE, "timeout（渲染线程没在跑？）");
    return false;
}

void HttpSend(SOCKET c, const char* body, int len) {
    char hdr[160];
    int hn = _snprintf_s(hdr, sizeof(hdr), _TRUNCATE,
                         "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
                         "Content-Length: %d\r\nConnection: close\r\n\r\n", len);
    send(c, hdr, hn, 0);
    if (len > 0) send(c, body, len, 0);
}

// ---- 请求处理 ----
void Handle(SOCKET c, char* req) {
    // 取方法与路径
    char method[16] = { 0 }, path[512] = { 0 }, query[512] = { 0 };
    sscanf_s(req, "%15s %511s", method, (unsigned)sizeof(method), path, (unsigned)sizeof(path));
    char* q = strchr(path, '?');
    if (q) { _snprintf_s(query, sizeof(query), _TRUNCATE, "%s", q + 1); *q = 0; }

    static char out[4096];
    out[0] = 0;

    if (strcmp(path, "/ping") == 0) {
        HttpSend(c, "pong", 4);
        return;
    }
    // ---- 热重载：卸载自身（下一帧拆卸并自卸载；随后可重新编译 + 重新注入）----
    if (strcmp(path, "/unload") == 0) {
        const char* msg = "accepted: unload runs inside the next frame; this instance will disappear\n";
        HttpSend(c, msg, (int)strlen(msg));
        RequestUnload();
        return;
    }
    if (strcmp(path, "/state") == 0 || strcmp(path, "/diag") == 0) {
        // 诊断里会调用游戏函数（模板查找）→ 必须投到渲染线程执行
        int a[1] = { 0 };
        char r[4096] = { 0 };
        PostAndWaitN(1, a, 0, r, sizeof(r));
        HttpSend(c, r, (int)strlen(r));
        return;
    }
    // ---- 落点查询（只读；但要调游戏换算函数 0x1ED4A0 → 投渲染线程）----
    if (strcmp(path, "/view") == 0 || strcmp(path, "/cursor") == 0 ||
        strcmp(path, "/selected_pos") == 0 || strcmp(path, "/spot") == 0) {
        const int kind = (strcmp(path, "/view") == 0) ? 0
                       : (strcmp(path, "/selected_pos") == 0) ? 1
                       : (strcmp(path, "/cursor") == 0) ? 2 : 3;
        int a[1] = { kind };
        char r[320] = { 0 };
        PostAndWaitN(23, a, 1, r, sizeof(r));
        HttpSend(c, r, (int)strlen(r));
        return;
    }
    if (strcmp(path, "/log") == 0) {
        int n = GetParamInt(query, "n", 40);
        int len = OverlayLogDump(out, sizeof(out), n);
        HttpSend(c, out, len);
        return;
    }
    if (strcmp(path, "/help") == 0) {
        const char* h =
            "== 只读（HTTP 线程直接答）==\n"
            "GET  /ping\n"
            "GET  /state | /diag            状态+符号+相机+落点+选中单位结构\n"
            "GET  /log?n=40                 输出记录尾部\n"
            "GET  /read?addr=0x..&n=128     内存十六进制转储（RE 用）\n"
            "GET  /peek?addr=0x..&type=u32  单值读取 type=u32|f32|u8|u16|ptr\n"
            "GET  /resolve?hash=0x..        对象类型哈希 -> 模板指针\n"
            "== 脑机接口（MCP 用，只读）==\n"
            "GET  /snapshot[?fresh=1&vtab=0x..]  战场快照 JSON（资金/我方/敌方单位数+构成/选中/扫描统计）\n"
            "GET  /objects?q=&owner=&cls=&cat=&limit=40  活体对象列表（带 Thing/AI 地址/坐标/血量，MCP 感知层）\n"
            "GET  /obj?addr=0x..             单个对象详情（addr = Thing 地址，见 /objects）\n"
            "GET  /enemy                    仅敌方归属的明细\n"
            "GET  /units?q=&side=&cls=&limit=  单位目录检索（名字/模板名/哈希）\n"
            "GET  /ai/status                脑机接口状态 JSON\n"
            "POST /ai/set?on=0|1            兼容保留（已无闸门：开关不再拦截任何请求）\n"
            "== 落点查询（只读；要调游戏换算函数，投渲染线程）==\n"
            "GET  /view                     视野中心世界坐标（屏幕正中看出去的地面点，含地形高度）\n"
            "GET  /cursor                   鼠标光标处世界坐标（窗口失焦时停在旧值，远程操作别用）\n"
            "GET  /selected_pos             当前选中单位的世界坐标\n"
            "GET  /spot                     按「落点来源」设置自动挑一个可用落点（带 water 标记）\n"
            "== 变更（投递到渲染线程=游戏主线程）==\n"
            "POST /copy | /wnb | /mcv       复制选中 / 预设增援×8 / 基地车\n"
            "POST /capture | /kill | /rank  收编 / 摧毁选中 / 满级\n"
            "POST /speed?mode=0..3          超速/慢速/定住/恢复（或 value=0..1000 按值设置速度）\n"
            "POST /health?mode=0..2         无敌/残血/恢复\n"
            "POST /status?kind=&on=&sec=    单位状态：kind=ironcurtain(缺省)|invulnerable|speedup|slow|brittle|damageup\n"
            "                               （也认 0..5 数字；kind=reset 撤销全部状态+速度/血量还原）\n"
            "                               on=1 施加(默认) / 0 解除；sec=时长秒（默认 60）\n"
            "POST /summon?hash=0x..&count=N[&owner=索引][&place=view|selection|cursor][&x=&y=&z=]\n"
            "                                按哈希召唤（owner 见 /snapshot）。落点：place 指定来源，\n"
            "                                或给 x/y/z 指定坐标；都不给则按「落点来源」设置。回执带实际落点\n"
            "POST /crate?idx=N&count=N      空投箱\n"
            "POST /burst?hash=0x..[&x=&y=&z=|&place=view|selection|cursor][&delay=ms]\n"
            "                                箱子即爆：创建后下一帧自动击杀（效果=死亡开火）\n"
            "POST /toggle?f=N&on=0|1        功能开关（索引见 features.h）\n"
            "POST /action?name=&a1=&a2=&a3= 执行引擎脚本动作（ExecuteAction），参数只支持名字串\n"
            "                               例：/action?name=PLAYER_GRANT_SPECIAL_POWER&a1=SpecialPower_X&a2=%3CThis%20Player%3E\n"
            "POST /grant?power=SpecialPower_X[&who=<This Player>|Player_2|<All Players>]  按内部名授予协议技能（可跨阵营/指向敌方）\n"
            "POST /lua?rva=0x..&a1=&a2=&a3=  按 RVA 直调引擎 Lua C 函数（只支持名字串参数）\n"
            "POST /freebuild?on=0|1         免费建造\n"
            "POST /poke?addr=0x..&type=u32&val=0x..   写内存（RE 用）\n"
            "POST /call?rva=0x..&cc=fc|cdecl&a0=..&a1=..&a2=..  按 RVA 调函数\n"
            "POST /aicall?fn=0xVA&this=0x..&a0=&a1=&a2=&na=0..3  正统 thiscall 调用（na=栈参数量必须精确，P2 指挥实验）\n"
            "POST /order?unit=0x..&act=attack&target=0x..   指挥单位：攻击目标（Thing 地址）\n"
            "            |act=move&x=&y=&z=                  指挥单位：走向世界坐标\n"
            "GET  /scratch?x=&y=&z=[&idx=0|1]  Coord3D 传参缓冲，回地址\n"
            "GET  /buildable?x=&y=&z=&hash=0x..[&angle=&opts=0x..]  建筑落点可建性（直调游戏判定）\n"
            "GET  /postproc?on=0|1          创建后处理链开关（让召唤物登记进玩家体系）\n"
            "POST /postproc?obj=0x..[&team=0x..&a=1&b=1]  对单个对象手动跑后处理（验证用）\n"
            "  ↳ 变更类请求可加 &ai=1：仅作遥测计数；接口恒开放（无闸门）\n"
            "== 热重载 ==\n"
            "POST /unload                   卸载自身（下一帧拆卸并 FreeLibrary，端口随之释放）\n"
            "                               随后 injector.exe --unload 验证 + 重编译 + 重新注入\n";
        HttpSend(c, h, (int)strlen(h));
        return;
    }
    // ---- 只读 RE：/read ----
    if (strcmp(path, "/read") == 0 || strcmp(path, "/hex") == 0) {
        char ab[32] = { 0 };
        GetParam(query, "addr", ab, sizeof(ab));
        uintptr_t at = (uintptr_t)strtoul(ab, nullptr, 0);
        int n = GetParamInt(query, "n", 64);
        if (n < 16) n = 16; if (n > 256) n = 256;
        int w = 0;
        for (int off = 0; off < n; off += 16) {
            unsigned char b[16] = { 0 };
            bool ok = RdBytes(at + off, b, 16);
            w += _snprintf_s(out + w, sizeof(out) - w, _TRUNCATE, "%08X: ", (unsigned)(at + off));
            for (int i = 0; i < 16; ++i)
                w += _snprintf_s(out + w, sizeof(out) - w, _TRUNCATE, "%02X ", b[i]);
            w += _snprintf_s(out + w, sizeof(out) - w, _TRUNCATE, " |");
            for (int i = 0; i < 16; ++i) {
                char ch = (b[i] >= 32 && b[i] < 127) ? (char)b[i] : '.';
                w += _snprintf_s(out + w, sizeof(out) - w, _TRUNCATE, "%c", ch);
            }
            w += _snprintf_s(out + w, sizeof(out) - w, _TRUNCATE, "|%s\n", ok ? "" : " <unreadable>");
            if (w >= (int)sizeof(out) - 96) break;
        }
        HttpSend(c, out, w);
        return;
    }
    // ---- 只读 RE：/peek ----
    if (strcmp(path, "/peek") == 0) {
        char ab[32] = { 0 }, tb[16] = { 0 };
        GetParam(query, "addr", ab, sizeof(ab));
        GetParam(query, "type", tb, sizeof(tb));
        uintptr_t at = (uintptr_t)strtoul(ab, nullptr, 0);
        char line[160];
        if (tb[0] == 'f') {
            float fv = 0; bool ok = RdBytes(at, &fv, 4);
            _snprintf_s(line, sizeof(line), _TRUNCATE, "%08X f32 = %.6g %s", (unsigned)at, fv, ok ? "" : "(unreadable)");
        } else if (tb[0] == 'u' && tb[1] == '8') {
            unsigned char v = 0; RdBytes(at, &v, 1);
            _snprintf_s(line, sizeof(line), _TRUNCATE, "%08X u8 = %u (0x%02X)", (unsigned)at, v, v);
        } else if (tb[0] == 'u' && tb[1] == '1') {
            unsigned short v = 0; RdBytes(at, &v, 2);
            _snprintf_s(line, sizeof(line), _TRUNCATE, "%08X u16 = %u (0x%04X)", (unsigned)at, v, v);
        } else {
            uint32_t v = 0; bool ok = RdBytes(at, &v, 4);
            if (tb[0] == 'p') {
                uint32_t dv = 0; bool ok2 = ok && v && RdBytes(v, &dv, 4);
                _snprintf_s(line, sizeof(line), _TRUNCATE, "%08X ptr = %08X -> [%08X] = %08X %s",
                            (unsigned)at, v, v, dv, ok2 ? "" : "(bad)");
            } else {
                _snprintf_s(line, sizeof(line), _TRUNCATE, "%08X u32 = %u (0x%08X) %s", (unsigned)at, v, v, ok ? "" : "(unreadable)");
            }
        }
        HttpSend(c, line, (int)strlen(line));
        return;
    }
    // ---- 只读 RE：/resolve ----
    if (strcmp(path, "/resolve") == 0) {
        char hb[24] = { 0 };
        GetParam(query, "hash", hb, sizeof(hb));
        uint32_t hash = (uint32_t)strtoul(hb, nullptr, 0);
        int a[1] = { (int)hash };
        char line[128] = { 0 };
        PostAndWaitN(2, a, 1, line, sizeof(line));
        HttpSend(c, line, (int)strlen(line));
        return;
    }
    // ---- 战场快照（只读；MCP 的"眼睛"）----
    if (strcmp(path, "/snapshot") == 0 || strcmp(path, "/enemy") == 0) {
        static char big[24576];
        const bool enemyOnly = (strcmp(path, "/enemy") == 0);
        char vb[24] = { 0 };
        if (GetParam(query, "vtab", vb, sizeof(vb)))          // 手动钉 Thing vtable（热修）
            SetThingVtOverride((uint32_t)strtoul(vb, nullptr, 0));
        if (GetParamInt(query, "fresh", 0)) BattlefieldInvalidate();
        const int len = BuildSnapshot(big, sizeof(big), enemyOnly);
        if (GetParamInt(query, "ai", 0)) AiLogLine("GET %s -> %s", path, g_aiOpen ? "ok" : "ok(gate-off)");
        HttpSend(c, big, len);
        return;
    }
    // ---- 活体对象列表（带地址！MCP 自动感知：找谁、在哪、多少血、AI 指针）----
    // GET /objects?q=名字/哈希前缀&owner=索引&cls=&cat=&limit=40&fresh=1
    if (strcmp(path, "/objects") == 0) {
        static char ob[24576];
        char q[64] = { 0 };
        GetParam(query, "q", q, sizeof(q));
        const int ownerIdx = GetParamInt(query, "owner", -1);
        const int clsF     = GetParamInt(query, "cls", -1);
        const int catF     = GetParamInt(query, "cat", -1);
        int limit          = GetParamInt(query, "limit", 40);
        if (limit < 1) limit = 1;
        if (limit > 100) limit = 100;
        const bool fresh = GetParamInt(query, "fresh", 0) != 0;
        static ra3::BattleLiveObj objs[104];
        const int n = ra3::ScanLiveObjects(objs, limit, q, ownerIdx, clsF, catF, fresh);
        int w = _snprintf_s(ob, sizeof(ob), _TRUNCATE, "{\"count\":%d,\"objects\":[", n);
        for (int i = 0; i < n && w < (int)sizeof(ob) - 320; ++i) {
            const ra3::BattleLiveObj& o = objs[i];
            char nm[96];
            JsonEsc(nm, sizeof(nm), o.name ? o.name : "?");
            static const char* const kCat[] = { "unit", "struct", "other" };
            w += _snprintf_s(ob + w, sizeof(ob) - w, _TRUNCATE,
                "%s{\"thing\":\"0x%08X\",\"ai\":\"0x%08X\",\"hash\":\"0x%08X\",\"name\":\"%s\","
                "\"side\":\"%s\",\"cls\":\"%s\",\"cat\":\"%s\",\"owner\":%d,\"local\":%d,"
                "\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,\"hp\":%.0f,\"hpMax\":%.0f}",
                i ? "," : "", (unsigned)o.thing, (unsigned)o.ai, o.hash, nm,
                (o.side >= 0 && o.side < ra3::B_SIDE_COUNT) ? ra3::kBattleSideNames[o.side] : "?",
                (o.cls >= 0 && o.cls < ra3::B_CLS_COUNT) ? ra3::kBattleClassNames[o.cls] : "?",
                (o.cat >= 0 && o.cat < 3) ? kCat[o.cat] : "?",
                o.ownerIdx, o.isLocal ? 1 : 0, o.x, o.y, o.z, o.hp, o.hpMax);
        }
        w += _snprintf_s(ob + w, sizeof(ob) - w, _TRUNCATE, "]}");
        HttpSend(c, ob, w);
        return;
    }
    // ---- 单个对象详情：GET /obj?addr=0x..（addr = Thing，见 /objects 的 thing 字段）----
    if (strcmp(path, "/obj") == 0) {
        char ab[24] = { 0 };
        GetParam(query, "addr", ab, sizeof(ab));
        const uintptr_t a = (uintptr_t)strtoul(ab, nullptr, 0);
        ra3::BattleLiveObj o;
        char b[768];
        if (!a || !ra3::ObjectDetail(a, &o)) {
            const char* m = "404 不是合法对象地址（需 Thing 地址，见 /objects 的 thing 字段）";
            HttpSend(c, m, (int)strlen(m));
            return;
        }
        char nm[96];
        JsonEsc(nm, sizeof(nm), o.name ? o.name : "?");
        const int w = _snprintf_s(b, sizeof(b), _TRUNCATE,
            "{\"thing\":\"0x%08X\",\"ai\":\"0x%08X\",\"body\":\"0x%08X\",\"owner\":\"0x%08X\","
            "\"ownerIdx\":%d,\"hash\":\"0x%08X\",\"name\":\"%s\",\"side\":\"%s\",\"cls\":\"%s\","
            "\"local\":%d,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"hp\":%.1f,\"hpMax\":%.1f}",
            (unsigned)o.thing, (unsigned)o.ai, (unsigned)o.body, (unsigned)o.owner,
            o.ownerIdx, o.hash, nm,
            (o.side >= 0 && o.side < ra3::B_SIDE_COUNT) ? ra3::kBattleSideNames[o.side] : "?",
            (o.cls >= 0 && o.cls < ra3::B_CLS_COUNT) ? ra3::kBattleClassNames[o.cls] : "?",
            o.isLocal ? 1 : 0, o.x, o.y, o.z, o.hp, o.hpMax);
        HttpSend(c, b, w);
        return;
    }
    // ---- 单位目录检索：?q=名字/模板名/哈希&side=&cls=&limit= ----
    if (strcmp(path, "/units") == 0) {
        static char ob[16384];
        char q[64] = { 0 };
        GetParam(query, "q", q, sizeof(q));
        const int limit = GetParamInt(query, "limit", 40);
        const int sideF = GetParamInt(query, "side", -1);
        const int clsF = GetParamInt(query, "cls", -1);
        int w = 0;
        w += _snprintf_s(ob + w, sizeof(ob) - w, _TRUNCATE, "{\"units\":[");
        int hit = 0;
        for (int i = 0; i < kBattleObjectCount && hit < limit; ++i) {
            const BattleObj& o = kBattleObjects[i];
            if (sideF >= 0 && o.side != sideF) continue;
            if (clsF >= 0 && o.cls != clsF) continue;
            if (q[0]) {
                char hex[16];
                _snprintf_s(hex, sizeof(hex), _TRUNCATE, "%08X", (unsigned)o.hash);
                if (!strstr(o.name, q) && !strstr(o.id, q) && !_strnicmp(hex, q, strlen(q)) &&
                    !_strnicmp(o.id, q, strlen(q)))
                    continue;
            }
            char nm[96], id[96];
            JsonEsc(nm, sizeof(nm), o.name);
            JsonEsc(id, sizeof(id), o.id);
            w += _snprintf_s(ob + w, sizeof(ob) - w, _TRUNCATE,
                             "%s{\"name\":\"%s\",\"id\":\"%s\",\"hash\":\"0x%08X\","
                             "\"side\":\"%s\",\"cls\":\"%s\"}",
                             hit ? "," : "", nm, id, (unsigned)o.hash,
                             kBattleSideNames[o.side], kBattleClassNames[o.cls]);
            ++hit;
            if (w > (int)sizeof(ob) - 256) break;
        }
        w += _snprintf_s(ob + w, sizeof(ob) - w, _TRUNCATE, "],\"count\":%d}", hit);
        HttpSend(c, ob, w);
        return;
    }
    // ---- 创建后处理开关（不带 obj 时只切开关）----
    if (strcmp(path, "/postproc") == 0) {
        char ob[32] = { 0 };
        if (!GetParam(query, "obj", ob, sizeof(ob))) {
            const int on = GetParamInt(query, "on", -1);
            if (on >= 0) SetPostProcOn(on != 0);
            char b[192];
            int w = _snprintf_s(b, sizeof(b), _TRUNCATE,
                                "postproc=%s（召唤时自动登记） last=%s",
                                GetPostProcOn() ? "on" : "off", PostProcLastError());
            HttpSend(c, b, w);
            return;
        }
    }
    // ---- 脑机接口状态 / 开关 ----
    if (strcmp(path, "/ai/status") == 0) {
        char b[512];
        int w = _snprintf_s(b, sizeof(b), _TRUNCATE,
                            "{\"open\":%d,\"listen\":\"%s\",\"calls\":%d,\"rejects\":%d,"
                            "\"catalog\":%d}",
                            g_aiOpen ? 1 : 0,
                            g_listen != INVALID_SOCKET ? "127.0.0.1:17832" : "",
                            (int)g_aiCalls, (int)g_aiRejects, kBattleObjectCount);
        HttpSend(c, b, w);
        return;
    }
    if (strcmp(path, "/ai/set") == 0) {
        const bool on = GetParamInt(query, "on", 0) != 0;
        SetAiOpen(on);
        char b[128];
        int w = _snprintf_s(b, sizeof(b), _TRUNCATE, "ai=%s", on ? "open" : "closed");
        HttpSend(c, b, w);
        return;
    }

    // ---- 变更类：投递给渲染线程 ----
    // 已取消闸门（2026-10-16）：接口只监听 127.0.0.1，单机环境下「把 MCP 配置交给 AI」
    //   本身即视为授权。原「脑机接口开关」只拦带 ai=1 的请求（即 MCP 通道），却常被误当成
    //   能力开关，徒增"到底开没开"的困惑 → 不再拦截任何请求。
    //   ai=1 仅保留作遥测（计数 + 调用日志，见本函数末尾）。
    const bool isAi = GetParamInt(query, "ai", 0) != 0;
    int fn = 0, args[8] = { 0,0,0,0,0,0,0,0 }; int na = 0;   // 8 格余量（aicall 用到 args[5]）
    char pb[32] = { 0 }, tb[16] = { 0 }, vb[32] = { 0 };
    if (strcmp(path, "/copy") == 0)       { fn = 4; }
    else if (strcmp(path, "/wnb") == 0)   { fn = 5; }
    else if (strcmp(path, "/mcv") == 0)   { fn = 6; }
    else if (strcmp(path, "/capture") == 0) { fn = 7; }
    else if (strcmp(path, "/kill") == 0)  { fn = 8; }
    else if (strcmp(path, "/rank") == 0)  { fn = 9; }
    else if (strcmp(path, "/speed") == 0) {
        // value=NN 按绝对速度值设置（滑条值，0=定住）；只给 mode 时走原有档位
        char svb[24] = { 0 };
        if (GetParam(query, "value", svb, sizeof(svb))) {
            fn = 10; args[0] = -1; args[1] = (int)strtof(svb, nullptr); na = 2;
        } else {
            fn = 10; args[0] = GetParamInt(query, "mode", 0); na = 1;
        }
    }
    else if (strcmp(path, "/health") == 0){ fn = 11; args[0] = GetParamInt(query, "mode", 0); na = 1; }
    else if (strcmp(path, "/status") == 0) {
        // kind=名字或数字（见 StatusKindFromName，缺省 ironcurtain）；on=1 施加（默认）/ 0 解除；
        //   kind=reset 重置全部；sec=时长秒
        char kb[32] = { 0 };
        GetParam(query, "kind", kb, sizeof(kb));
        if (_stricmp(kb, "reset") == 0) { fn = 24; args[0] = -1; na = 3; }
        else {
            int kind = StatusKindFromName(kb);
            if (kind < 0) {
                const bool numeric = kb[0] >= '0' && kb[0] <= '9';
                if (kb[0] && !numeric) {   // 名字打错 → 明确拒绝（静默默认成铁幕会误操作）
                    const char* msg = "err: kind 不认识（ironcurtain|invulnerable|speedup|slow|"
                                      "brittle|damageup|reset，或 0..5 数字）";
                    HttpSend(c, msg, (int)strlen(msg));
                    return;
                }
                kind = kb[0] ? GetParamInt(query, "kind", 0) : US_IRONCURTAIN;
            }
            if (kind < 0 || kind >= US_COUNT) {
                const char* msg = "err: kind 越界（0..5）";
                HttpSend(c, msg, (int)strlen(msg));
                return;
            }
            fn = 24;
            args[0] = kind;
            args[1] = GetParamInt(query, "on", 1) ? 1 : 0;
            args[2] = GetParamInt(query, "sec", 60);
            na = 3;
        }
    }
    else if (strcmp(path, "/summon") == 0){
        fn = 12;
        char hb[24] = { 0 }, ob[16] = { 0 };
        GetParam(query, "hash", hb, sizeof(hb));
        args[0] = (int)strtoul(hb, nullptr, 0);
        args[1] = GetParamInt(query, "count", 1);
        // owner=<玩家索引> → 给该方召唤（缺省/0 = 本地玩家）。索引见 /snapshot 的 owners[].idx
        int teamOv = 0;
        if (GetParam(query, "owner", ob, sizeof(ob))) {
            const int idx = atoi(ob);
            if (idx > 0) {
                const uintptr_t t = FindTeamByIdx(idx);
                if (!t) {
                    const char* m = "404 找不到该玩家索引（先看 /snapshot 的 owners[].idx）";
                    HttpSend(c, m, (int)strlen(m)); return;
                }
                teamOv = (int)t;
            }
        }
        args[2] = teamOv;
        na = 3;
        // 落点来源：?place=view|selection|cursor（一次性，下一单生效后清除）。
        //   不给则按 UI 的「落点来源」设置（默认视野中心）。
        char plb[16] = { 0 };
        if (GetParam(query, "place", plb, sizeof(plb))) {
            const int m = (strcmp(plb, "view") == 0) ? 0
                        : (strcmp(plb, "selection") == 0 || strcmp(plb, "selected") == 0) ? 1
                        : (strcmp(plb, "cursor") == 0 || strcmp(plb, "mouse") == 0) ? 2 : -1;
            if (m >= 0) SetSummonPlace(m);
        }
        // 指定落点：?x=&y=&z=（给了 x 与 y 即以坐标为准）
        char xb[32] = { 0 }, yb[32] = { 0 }, zb[32] = { 0 };
        if (GetParam(query, "x", xb, sizeof(xb)) && GetParam(query, "y", yb, sizeof(yb))) {
            float px = strtof(xb, nullptr), py = strtof(yb, nullptr), pz = 0.0f;
            if (GetParam(query, "z", zb, sizeof(zb))) pz = strtof(zb, nullptr);
            SetSummonPosOverride(true, px, py, pz);
        }
    }
    else if (strcmp(path, "/burst") == 0) {
        // 箱子即爆：一次请求完成「按哈希创建箱子 → 下一帧校验 → 击杀」。
        //   hash  = 箱子模板哈希（必需）
        //   delay = 额外等待的毫秒数，缺省 0 = 创建后下一帧就引爆（对象初始化所需的最短间隔）
        //   落点与 /summon 共用：place=view|selection|cursor，或给 x/y/z
        fn = 25;
        char hb[24] = { 0 };
        GetParam(query, "hash", hb, sizeof(hb));
        args[0] = (int)strtoul(hb, nullptr, 0);
        args[1] = GetParamInt(query, "delay", 0);
        na = 2;
        char plb[16] = { 0 };
        if (GetParam(query, "place", plb, sizeof(plb))) {
            const int m = (strcmp(plb, "view") == 0) ? 0
                        : (strcmp(plb, "selection") == 0 || strcmp(plb, "selected") == 0) ? 1
                        : (strcmp(plb, "cursor") == 0 || strcmp(plb, "mouse") == 0) ? 2 : -1;
            if (m >= 0) SetSummonPlace(m);
        }
        char bx[32] = { 0 }, by[32] = { 0 }, bz[32] = { 0 };
        if (GetParam(query, "x", bx, sizeof(bx)) && GetParam(query, "y", by, sizeof(by))) {
            float px = strtof(bx, nullptr), py = strtof(by, nullptr), pz = 0.0f;
            if (GetParam(query, "z", bz, sizeof(bz))) pz = strtof(bz, nullptr);
            SetSummonPosOverride(true, px, py, pz);
        }
    }
    else if (strcmp(path, "/crate") == 0) {
        fn = 13; args[0] = GetParamInt(query, "idx", 0); args[1] = GetParamInt(query, "count", 1); na = 2;
    }
    else if (strcmp(path, "/toggle") == 0) {
        fn = 14; args[0] = GetParamInt(query, "f", -1); args[1] = GetParamInt(query, "on", 1); na = 2;
    }
    else if (strcmp(path, "/freebuild") == 0) {
        fn = 15; args[0] = GetParamInt(query, "on", 1); na = 1;
    }
    else if (strcmp(path, "/poke") == 0) {
        fn = 17;
        GetParam(query, "addr", pb, sizeof(pb));
        GetParam(query, "type", tb, sizeof(tb));
        GetParam(query, "val", vb, sizeof(vb));
        args[0] = (int)(uintptr_t)strtoul(pb, nullptr, 0);
        args[1] = (tb[0] == 'f') ? 1 : (tb[0] == 'u' && tb[1] == '8') ? 2 : (tb[0] == 'u' && tb[1] == '1') ? 3 : 0;
        if (args[1] == 1) { float f = (float)atof(vb); args[2] = (int)*(uint32_t*)&f; }
        else args[2] = (int)strtoul(vb, nullptr, 0);
        na = 3;
    }
    else if (strcmp(path, "/postproc") == 0) {   // 创建后处理链：?obj=0x..&team=0x..&a=1&b=1
        fn = 19;
        char ob[32] = { 0 }, tb2[32] = { 0 };
        GetParam(query, "obj", ob, sizeof(ob));
        GetParam(query, "team", tb2, sizeof(tb2));
        args[0] = (int)(uintptr_t)strtoul(tb2, nullptr, 0);
        args[1] = (int)(uintptr_t)strtoul(ob, nullptr, 0);
        args[2] = GetParamInt(query, "a", 1);
        args[3] = GetParamInt(query, "b", 1);
        na = 4;
    }
    else if (strcmp(path, "/call") == 0) {
        fn = 18;
        char rb[24] = { 0 }, cb[16] = { 0 };
        GetParam(query, "rva", rb, sizeof(rb));
        GetParam(query, "cc", cb, sizeof(cb));
        args[0] = (int)strtoul(rb, nullptr, 0);
        args[1] = (cb[0] == 'f') ? 1 : 0;
        args[2] = GetParamInt(query, "a0", 0);
        args[3] = GetParamInt(query, "a1", 0);
        args[4] = GetParamInt(query, "a2", 0);
        na = 5;
    }     else if (strcmp(path, "/aicall") == 0) {
        // P2 实验端点：正统 thiscall 调任意函数。fn 用 VA。变更类 → 渲染线程。
        // 注意：PostAndWaitN 把 args[0..] 映射到 c.a1..，故从 args[0] 开始填。
        fn = 20;
        char fb[24] = { 0 }, tb[24] = { 0 };
        GetParam(query, "fn", fb, sizeof(fb));
        GetParam(query, "this", tb, sizeof(tb));
        args[0] = (int)strtoul(fb, nullptr, 0);   // -> c.a1 = fnVA
        args[1] = (int)strtoul(tb, nullptr, 0);   // -> c.a2 = this
        args[2] = GetParamInt(query, "a0", 0);    // -> c.a3
        args[3] = GetParamInt(query, "a1", 0);    // -> c.a4
        args[4] = GetParamInt(query, "a2", 0);    // -> c.a5
        args[5] = GetParamInt(query, "na", 0);    // -> c.a6 栈参数量(0..3)，必须与被调方一致
        na = 6;
    } else if (strcmp(path, "/order") == 0) {
        // P2 指挥层：unit=单位 Thing 地址；act=attack|move|idle。
        //   attack: target=目标 Thing 地址；  move: x=&y=&z=世界坐标
        fn = 21;
        char ub[24] = { 0 }, ab[16] = { 0 }, gb2[24] = { 0 };
        GetParam(query, "unit", ub, sizeof(ub));
        GetParam(query, "act", ab, sizeof(ab));
        GetParam(query, "target", gb2, sizeof(gb2));
        args[0] = (int)strtoul(ub, nullptr, 0);                 // -> c.a1 unit
        args[1] = (strcmp(ab, "move") == 0) ? 1
                : (strcmp(ab, "idle") == 0) ? 2 : 0;            // -> c.a2 act
        args[2] = (int)strtoul(gb2, nullptr, 0);                // -> c.a3 target
        float ox = 0, oy = 0, oz = 0; char vb2[32];
        if (GetParam(query, "x", vb2, sizeof(vb2))) ox = strtof(vb2, nullptr);
        if (GetParam(query, "y", vb2, sizeof(vb2))) oy = strtof(vb2, nullptr);
        if (GetParam(query, "z", vb2, sizeof(vb2))) oz = strtof(vb2, nullptr);
        args[3] = (int)*(uint32_t*)&ox;                         // -> c.a4
        args[4] = (int)*(uint32_t*)&oy;                         // -> c.a5
        args[5] = (int)*(uint32_t*)&oz;                         // -> c.a6
        na = 6;
    } else if (strcmp(path, "/protocol") == 0) {
        fn = 22;
        char ab[24] = { 0 };
        GetParam(query, "act", ab, sizeof(ab));
        int act = 9;
        if (strcmp(ab, "points") == 0) act = 0;
        else if (strcmp(ab, "science") == 0) act = 1;
        else if (strcmp(ab, "grant") == 0) act = 2;
        else if (strcmp(ab, "all") == 0) act = 3;
        else if (strcmp(ab, "fast") == 0) act = 4;
        else if (strcmp(ab, "disable") == 0) act = 5;
        else if (strcmp(ab, "tech") == 0) act = 8;
        else if (strcmp(ab, "snap") == 0) act = 20;
        else if (strcmp(ab, "calib") == 0) act = 22;
        else if (strcmp(ab, "pscan") == 0) act = 26;
        else if (strcmp(ab, "pfilter") == 0) act = 27;
        else if (strcmp(ab, "unlock") == 0) act = 30;
        else if (strcmp(ab, "slot") == 0) act = 31;
        else if (strcmp(ab, "ui") == 0) act = 32;
        else if (strcmp(ab, "tap") == 0) act = 33;
        else if (strcmp(ab, "uireset") == 0) act = 34;
        else if (strcmp(ab, "claim") == 0) act = 40;
        else if (strcmp(ab, "owned") == 0) act = 41;
        else if (strcmp(ab, "deepsnap") == 0) act = 24;
        else if (strcmp(ab, "deepdiff") == 0) act = 25;
        else if (strcmp(ab, "diff") == 0) act = 21;
        args[0] = act;
        args[1] = GetParamInt(query, "n", 0);
        args[2] = GetParamInt(query, "idx", 0);
        args[3] = GetParamInt(query, "on", 1);
        na = 4;
    } else if (strcmp(path, "/action") == 0) {
        // 引擎脚本动作：name=动作名，a1..a3=名字串参数（URL 编码）
        // 例：/action?name=PLAYER_GRANT_SPECIAL_POWER&a1=SpecialPower_X&a2=%3CThis%20Player%3E
        fn = 27;
        char tmp[96];
        const char* keys[4] = { "name", "a1", "a2", "a3" };
        for (int i = 0; i < 4; ++i) {
            g_scriptArgs[i][0] = 0;
            if (GetParam(query, keys[i], tmp, sizeof(tmp)))
                URLDecode(tmp, g_scriptArgs[i], sizeof(g_scriptArgs[i]));
        }
        // mode=1：动作名按引擎串(tag4)，参数按普通 Lua 串(tag3)
        args[4] = GetParamInt(query, "mode", 0);
        if (!g_scriptArgs[0][0]) {
            const char* msg = "action: 缺 name";
            HttpSend(c, msg, (int)strlen(msg));
            return;
        }
        na = 0;
    } else if (strcmp(path, "/luatest") == 0) {
        fn = 29;
        na = 1;
    } else if (strcmp(path, "/lua") == 0) {
        // 直调引擎 Lua C 函数：rva=0x..&a1=&a2=&a3=（参数只支持名字串）
        fn = 28;
        char tmp2[96];
        const char* keys2[3] = { "a1", "a2", "a3" };
        for (int i = 0; i < 3; ++i) {
            g_scriptArgs[i][0] = 0;
            if (GetParam(query, keys2[i], tmp2, sizeof(tmp2)))
                URLDecode(tmp2, g_scriptArgs[i], sizeof(g_scriptArgs[i]));
        }
        args[0] = GetParamInt(query, "rva", 0);
        na = 1;
    } else if (strcmp(path, "/buildable") == 0) {
        // 建筑落点可建造性：x=&y=&z=&hash=&angle=&opts=
        fn = 26;
        char vb[32] = { 0 }, hb2[32] = { 0 };
        float x = 0, y = 0, z = 0, ang = 0;
        if (GetParam(query, "x", vb, sizeof(vb))) x = strtof(vb, nullptr);
        if (GetParam(query, "y", vb, sizeof(vb))) y = strtof(vb, nullptr);
        if (GetParam(query, "z", vb, sizeof(vb))) z = strtof(vb, nullptr);
        if (GetParam(query, "angle", vb, sizeof(vb))) ang = strtof(vb, nullptr);
        uint32_t hash = 0, opts = 0x1A20;
        GetParam(query, "hash", hb2, sizeof(hb2));
        hash = (uint32_t)strtoul(hb2, nullptr, 0);
        if (GetParam(query, "opts", vb, sizeof(vb))) opts = (uint32_t)strtoul(vb, nullptr, 0);
        args[0] = (int)*(uint32_t*)&x;
        args[1] = (int)*(uint32_t*)&y;
        args[2] = (int)*(uint32_t*)&z;
        args[3] = (int)hash;
        args[4] = (int)*(uint32_t*)&ang;
        args[5] = (int)opts;
        na = 6;
    } else if (strcmp(path, "/aidx") == 0) {
        // 脚本动作表按名字查 id：?name=PLAYER_GRANT_SPECIAL_POWER
        fn = 31;
        char tmp5[96];
        g_scriptArgs[0][0] = 0;
        if (GetParam(query, "name", tmp5, sizeof(tmp5)))
            URLDecode(tmp5, g_scriptArgs[0], sizeof(g_scriptArgs[0]));
        na = 1;
    } else if (strcmp(path, "/grant") == 0) {
        // 按 SpecialPower 内部名授予协议技能：power=SpecialPower_X[&who=<This Player>]
        fn = 30;
        char tmp4[128];
        g_scriptArgs[0][0] = 0;
        g_scriptArgs[1][0] = 0;
        if (GetParam(query, "power", tmp4, sizeof(tmp4)))
            URLDecode(tmp4, g_scriptArgs[0], sizeof(g_scriptArgs[0]));
        if (GetParam(query, "who", tmp4, sizeof(tmp4)))
            URLDecode(tmp4, g_scriptArgs[1], sizeof(g_scriptArgs[1]));
        na = 1;
    } else if (strcmp(path, "/scratch") == 0) {
        // Coord3D 传参缓冲：写 DLL 自有内存，回地址。仅 RE 实验用。
        char vb[32] = { 0 };
        float x = 0, y = 0, z = 0;
        if (GetParam(query, "x", vb, sizeof(vb))) x = strtof(vb, nullptr);
        if (GetParam(query, "y", vb, sizeof(vb))) y = strtof(vb, nullptr);
        if (GetParam(query, "z", vb, sizeof(vb))) z = strtof(vb, nullptr);
        int idx = GetParamInt(query, "idx", 0) ? 4 : 0;
        g_coordScratch[idx + 0] = x; g_coordScratch[idx + 1] = y; g_coordScratch[idx + 2] = z;
        _snprintf_s(out, sizeof(out), _TRUNCATE,
                    "scratch @%08X = (%.2f, %.2f, %.2f)",
                    (unsigned)(uintptr_t)&g_coordScratch[idx], x, y, z);
        HttpSend(c, out, (int)strlen(out));
        return;
    } else {
        HttpSend(c, "404 (see /help)", 14);
        return;
    }
    char r[1024] = { 0 };
    PostAndWaitN(fn, args, na, r, sizeof(r));
    _snprintf_s(out, sizeof(out), _TRUNCATE, "%s", r);
    if (isAi) { InterlockedIncrement(&g_aiCalls); AiLogLine("AI %s -> %s", path, r); }
    HttpSend(c, out, (int)strlen(out));
}

DWORD WINAPI HttpThread(LPVOID) {
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return 0;
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) { WSACleanup(); return 0; }

    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(17832);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    BOOL yes = TRUE;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    if (bind(g_listen, (sockaddr*)&a, sizeof(a)) != 0) {
        OverlayLogLine(OVLOG_ERR, "[接口] 端口 17832 绑定失败（可能已被占用）");
        closesocket(g_listen); g_listen = INVALID_SOCKET; WSACleanup(); return 0;
    }
    listen(g_listen, 4);
    OverlayLogLine(OVLOG_OK, "[接口] 已监听 http://127.0.0.1:17832  （/help 看用法）");

    while (!g_stop) {
        BattlefieldPump();                       // 面板请求的战场刷新（在本线程扫，不卡渲染）
        fd_set f; FD_ZERO(&f); FD_SET(g_listen, &f);
        timeval tv = { 0, 200000 };
        const int sel = select(0, &f, nullptr, nullptr, &tv);
        if (sel <= 0) continue;
        SOCKET c = accept(g_listen, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        DWORD tmo = 2000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
        char req[2048] = { 0 };
        recv(c, req, sizeof(req) - 1, 0);
        __try { Handle(c, req); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        closesocket(c);
    }
    if (g_listen != INVALID_SOCKET) closesocket(g_listen);
    WSACleanup();
    return 0;
}

} // namespace

void HttpApiStart(int port) {
    (void)port;
    AiCsInit();
    InterlockedExchange(&g_stop, 0);
    g_httpThread = CreateThread(nullptr, 0, HttpThread, nullptr, 0, nullptr);
}

void HttpApiStop() { InterlockedExchange(&g_stop, 1); }

bool HttpApiJoin() {
    if (!g_httpThread) return true;   // 未启动
    // 最坏情况：线程正卡在一次变更命令的回执等待（kCmdWaitMs=2500）→ 4s 上限足够
    if (WaitForSingleObject(g_httpThread, 4000) != WAIT_OBJECT_0) return false;
    CloseHandle(g_httpThread);
    g_httpThread = nullptr;
    return true;
}

void HttpApiPump() {
    if (g_cmd.state != 1) return;
    CmdSlot& c = g_cmd;
    c.out[0] = 0;
    __try { ExecCmd(c); } __except (EXCEPTION_EXECUTE_HANDLER) {
        _snprintf_s(c.out, sizeof(c.out), _TRUNCATE, "exception in cmd");
    }
    InterlockedExchange(&g_cmd.state, 2);
}

// ---- 脑机接口（MCP）对外 ----
void AiLogLine(const char* fmt, ...) {
    char tmp[160];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(tmp, sizeof(tmp), _TRUNCATE, fmt, ap);
    va_end(ap);

    AiCsEnter();
    SYSTEMTIME st; GetLocalTime(&st);
    int idx = static_cast<int>(g_aiLogHead) % kAiLogMax;
    if (idx < 0) idx = 0;
    _snprintf_s(g_aiLog[idx].t, sizeof(g_aiLog[idx].t), _TRUNCATE, "%02d:%02d:%02d",
                st.wHour, st.wMinute, st.wSecond);
    _snprintf_s(g_aiLog[idx].text, sizeof(g_aiLog[idx].text), _TRUNCATE, "%s", tmp);
    InterlockedExchange(&g_aiLogHead, (idx + 1) % kAiLogMax);
    if (g_aiLogCount < kAiLogMax) InterlockedIncrement(&g_aiLogCount);
    AiCsLeave();

    OverlayLogLine(OVLOG_INFO, tmp);
}

bool AiOpen() { return g_aiOpen != 0; }

void SetAiOpen(bool on) {
    // 兼容保留（2026-10-16 取消总闸）：此开关已不再拦截任何请求，仅翻标志 + 记一行日志。
    InterlockedExchange(&g_aiOpen, on ? 1 : 0);
    AiLogLine("[脑机] 开关 %s（提示：接口恒开放，此开关不再拦截请求）", on ? "开" : "关");
}

int AiCallCount() { return static_cast<int>(g_aiCalls); }
int AiRejectCount() { return static_cast<int>(g_aiRejects); }

const char* HttpListenLine() {
    return (g_listen != INVALID_SOCKET) ? "127.0.0.1:17832" : "未监听";
}

int AiLogDump(char* buf, int n, int maxLines) {
    if (!buf || n <= 0) return 0;
    AiCsEnter();
    const int count = static_cast<int>(g_aiLogCount);
    const int head = static_cast<int>(g_aiLogHead);
    const int total = count;
    int begin = total - (maxLines > 0 ? maxLines : total);
    if (begin < 0) begin = 0;
    const int startIdx = (count >= kAiLogMax) ? head : 0;
    int w = 0;
    for (int i = begin; i < total; ++i) {
        const AiLine& L = g_aiLog[(startIdx + i) % kAiLogMax];
        int k = _snprintf_s(buf + w, n - w, _TRUNCATE, "%s  %s\n", L.t, L.text);
        if (k <= 0) break;
        w += k;
        if (w >= n - 2) break;
    }
    AiCsLeave();
    return w;
}

} // namespace ra3
