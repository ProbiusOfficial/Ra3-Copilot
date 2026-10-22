// features.cpp - 功能核实现（表驱动多 hook）
#include "features.h"
#include "hook.h"
#include "symres.h"
#include <cstring>
#include <cstdio>

namespace ra3 {

// ---- stub 引用的全局量定义 ----
extern "C" {
uint32_t g_localPlayerId  = 0;
uint32_t g_localPlayerObj = 0;
uint32_t g_moneyObj       = 0;
uint32_t g_selUnit        = 0;
uint32_t g_selHits        = 0;
uint32_t g_selTick        = 0;
uint32_t g_tickNow        = 0;
uint32_t g_okUnit         = 0;
uint32_t g_okBody         = 0;
int32_t  g_moneyOp        = 0;
int32_t  g_moneyValue     = 0;
uint8_t  g_flags[32]      = { 0 };
uint32_t g_zoomHits       = 0;      // 遥测：ZoomCaveStub 进入次数
uint32_t g_zoomCam34      = 0;      // 遥测：ZoomPostStub 采样的 [+0x34]
uint32_t g_zoomPitch      = 0;      // 遥测：ZoomPostStub 采样的 [+0x74]
uint32_t g_zoomObj        = 0;      // 遥测：最近一次 this 指针
uint32_t g_zoomVal        = 0;      // 遥测：ZoomCaveStub 进入时 [esi+0x44]
uint32_t g_zoomPost       = 0;      // 遥测：clamp 之后 [esi+0x44]
uint32_t g_zoomEntry      = 0;      // 遥测：缩放函数入口命中次数
uint32_t g_telemEnter     = 0;
uint32_t g_telemApplied   = 0;

uint32_t g_paCount = 0;
uint32_t g_paBuf[64 * 4] = { 0 };
uint32_t g_pcCount = 0;
uint32_t g_pcBuf[64 * 6] = { 0 };
uint32_t g_lpCount = 0;
uint32_t g_lpBuf[64 * 3] = { 0 };
uint32_t g_retLockProbe = 0;
uint32_t g_blCount = 0;
uint32_t g_blBuf[128 * 3] = { 0 };
uint32_t g_clanCount = 0;
uint32_t g_clanBuf[128 * 3] = { 0 };
uint32_t g_bpCount = 0;
uint32_t g_bpBuf[64] = { 0 };
uint32_t g_csCount = 0;
uint32_t g_csBuf[16] = { 0 };
uint32_t g_retBuildLock = 0;
uint32_t g_tipCount = 0;
uint32_t g_tipBuf[128 * 4] = { 0 };
uint32_t g_retBuildTip = 0;
uint32_t g_retScience = 0;
uint32_t g_retClanGate = 0;
uint32_t g_retBtnProbe = 0;
uint32_t g_retCsProbe = 0;
uint32_t g_retBtnEnable = 0;
uint32_t g_extFixCount = 0;   // 建造页补全扩展建筑：生效次数
uint32_t g_extLastCs   = 0;   // 最近一次被修正的 CommandSet
uint32_t g_extLastDst  = 0;   // 换到的 inner（= 扩展完整版）
uint32_t g_extScanMs   = 0;   // 全内存扫描耗时（ms）

uint32_t g_retMoney = 0, g_retPlayerId = 0, g_retPower = 0, g_retSCPoint = 0;
uint32_t g_scPointObj = 0, g_scPointVal = 0;
uint32_t g_retProtoAll = 0;
uint32_t g_retProtoClick = 0;
uint32_t g_retHaveAllSC = 0, g_retFastBuild1 = 0, g_retFastBuild2 = 0;
uint32_t g_retFastBuild3 = 0, g_retMap = 0, g_retUnitAmmo = 0;
uint32_t g_retDangerLevel = 0, g_retOreMine = 0, g_retGodMode = 0;
uint32_t g_retEnemyCantBuild = 0, g_retSuperPower = 0, g_retDisableAllSP = 0;
uint32_t g_retZoom = 0, g_retOneKill = 0, g_retOKData1 = 0, g_retOKData2 = 0;
uint32_t g_retZoomPost = 0;
uint32_t g_retZoomEntry = 0;
}

// ---- overlay 侧遥测 ----
uint32_t g_telemImguiReady = 0;
uint32_t g_telemRenderOnce = 0;
uint32_t g_telemWndFound   = 0;
uint32_t g_telemD3dHooked  = 0;

namespace {

const uint8_t AOB_MONEY[5]      = { 0x03, 0x78, 0x04, 0x8B, 0x11 };
const uint8_t AOB_PLAYERID[6]   = { 0x8B, 0x50, 0x28, 0x8B, 0x42, 0x20 };
const uint8_t AOB_POWER[9]      = { 0x8B, 0x40, 0x04, 0x8B, 0x8E, 0xB0, 0x03, 0x00, 0x00 };
const uint8_t AOB_SCPOINT[6]    = { 0x8B, 0x78, 0x34, 0x8B, 0x4E, 0x3C };
const uint8_t AOB_HAVEALLSC[5]  = { 0xF3, 0x0F, 0x10, 0x47, 0x2C };
const uint8_t AOB_FASTBUILD1[5] = { 0xF3, 0x0F, 0x2C, 0x46, 0x1C };
const uint8_t AOB_FASTBUILD2[6] = { 0xD9, 0x46, 0x1C, 0x89, 0x46, 0x5C };
const uint8_t AOB_FASTBUILD3[6] = { 0xD9, 0x86, 0xBC, 0x01, 0x00, 0x00 };
const uint8_t AOB_MAP[8]        = { 0xF3, 0x0F, 0x11, 0x85, 0x60, 0x02, 0x00, 0x00 };
const uint8_t AOB_UNITAMMO[5]   = { 0x8B, 0x0C, 0xBA, 0x85, 0xC9 };
const uint8_t AOB_DANGER[6]     = { 0x8B, 0x81, 0x78, 0x12, 0x00, 0x00 };
const uint8_t AOB_OREMINE[6]    = { 0x8B, 0x40, 0x08, 0x2B, 0x41, 0x14 };
const uint8_t AOB_GODMODE[5]    = { 0x8B, 0x50, 0x3C, 0x8B, 0xCE };
const uint8_t AOB_ENEMYBUILD[5] = { 0x03, 0x50, 0x04, 0x3B, 0xD7 };
const uint8_t AOB_SUPERPOWER[6] = { 0x8B, 0x98, 0x18, 0x04, 0x00, 0x00 };
const uint8_t AOB_DISABLEALLSP[6] = { 0x8B, 0x51, 0x50, 0x3B, 0x50, 0x0C };
// 0x43C3A0 前置条件检查：mov eax,[esp+4]/mov edx,[eax]/mov eax,[eax]/mov edx,[ecx+48h]/push esi/mov esi,[eax+4]
// ⚠ 以下两条语义**未经复核**：其依据是 0x69A990 起的反汇编，而该地址并非指令边界（2026-10-19 发现）。
//    字节本身能对上（AOB 命中），但"它在做什么"存疑（见内部《作废-协议格前置判定》）。
// 0x686920 协议格锁定判定（待复核，含 cond==NULL 与依赖递归）
// 0x69A9B3 写控件状态：mov [esi+0x18],al / mov edx,[ecx]（待复核）
const uint8_t AOB_PROTOALL[6] = { 0x88,0x46,0x18, 0x8B,0x11, 0x8B };
// 0x69AB03 协议格点击处理：mov eax,[esi+14h] / test eax,eax / je（+7 字节 AOB 仅定位，覆盖 5 字节）
const uint8_t AOB_PROTOCLICK[7] = { 0x8B,0x46,0x14, 0x85,0xC0, 0x74,0x39 };
// Zoom / 秒杀 3 hook（基于 CE）—— 唯一长签名
const uint8_t AOB_ZOOM[7]     = { 0xF3,0x0F,0x11,0x46,0x44,0x74,0x39 };
const uint8_t AOB_ZOOMPOST[11] = { 0x88,0x9E,0x54,0x26,0x00,0x00,0xE8,0xB8,0xEA,0xFF,0xFF };
const uint8_t AOB_ZOOMENTRY[14] = { 0xA1,0x50,0xB7,0xCD,0x00,0x53,0x33,0xDB,0x38,0x98,0xBC,0x00,0x00,0x00 };
const uint8_t AOB_ONEKILL[7]  = { 0xF3,0x0F,0x11,0x46,0x04,0x74,0x20 };
const uint8_t AOB_OKDATA1[8]  = { 0x8B,0x8E,0x3C,0x03,0x00,0x00,0x8B,0x95 };
const uint8_t AOB_OKDATA2[10] = { 0x8B,0x89,0x3C,0x03,0x00,0x00,0x85,0xC9,0x74,0x0C };
// 0xA7E9AB = UI「玩家技能按钮」画锁定时调用的 call 0xAD9EC0（5 字节整指令，exe 内唯一）
//   前文 A7E9A9: lea ecx,[esi+8]  ⇒ 进入时 ecx = 按钮体+8，esi = 按钮体
const uint8_t AOB_LOCKPROBE[5] = { 0xE8, 0x10, 0xB5, 0x05, 0x00 };
// 0x6B7440: push esi / push edi / mov esi,ecx / mov ecx,[0xCE2FB8]
// 建造/技能按钮「禁用态」绘制入口（0x6C6310 jmp 过来的唯一落点），AOB 10 字节三处同构。
const uint8_t AOB_BUILDLOCK[10] = { 0x56,0x57,0x8B,0xF1, 0x8B,0x0D, 0xB8,0x2F,0xCE,0x00 };
// 0x6C5D90: sub esp,8 / push esi / mov esi,ecx / mov ecx,[0xCDB754]
// 「禁用原因 tooltip」构造器（内部 push 0xC814EC = GUI:BuildabilityDisabled）
const uint8_t AOB_BUILDTIP[10] = { 0x83,0xEC,0x08, 0x56, 0x8B,0xF1, 0x8B,0x0D, 0x54,0xB7 };
// 0x4451F0: push ecx / push esi / mov esi,[esp+0xC] / test esi,esi / push edi / mov edi,ecx
// 建造前置科技判定 bool Science(Player*, Template*) —— 返回 false 即「科技锁定」的根因。
const uint8_t AOB_SCIENCE[11] = { 0x51,0x56, 0x8B,0x74,0x24,0x0C, 0x85,0xF6, 0x57, 0x8B,0xF9 };
// 0x43EA70: mov eax,[esp+4] / mov edx,[eax+0xC0] / shr edx,9 / test dl,1
// 「可用对象集合」判定 bool ClanGate(Player*, Template*) —— ret 4，返回 1 = 该模板不在
// 玩家 p+0x200 链表里（UI 显示为「科技锁定」）。三个 UI 调用点 0x6B7089 / 0x6BB228 / 0x6BB4E2
// 直接调它、绕过 0x4451F0，所以 Science 桩对它无效。
// 指令边界：+0 = 8B 44 24 04 (4B) / +4 = 8B 90 C0 00 00 00 (6B) → 边界在 +10
const uint8_t AOB_CLANGATE[10] = { 0x8B,0x44,0x24,0x04, 0x8B,0x90,0xC0,0x00,0x00,0x00 };
// 建造页按钮对象更新函数入口：sub esp,0Ch / push ebx / push esi / mov esi,ecx / mov ecx,[esi+10h]
const uint8_t AOB_BTNPROBE[10] = { 0x83,0xEC,0x0C, 0x53, 0x56, 0x8B,0xF1, 0x8B,0x4E,0x10 };
// 建造页列表构建器内：mov ebx,eax / test ebx,ebx / mov [esp+18h],ebx（ebx = CommandSet*）
const uint8_t AOB_CMDSET[8] = { 0x8B,0xD8, 0x85,0xDB, 0x89,0x5C,0x24,0x18 };
// 0xABB6F0: sub esp,8 / push esi / mov esi,ecx / call 0xABB360
// 建造页按钮「可用性刷新」函数（vtable 0x00C88518 的 slot13）。内部末尾才决定 enable/disable：
//   走 0xABB812 时 ecx=[esp+0x14](Player) / push ebp(Template) / call 0x845250(Buildability)
//   返回 0 → 调 0xAB7040(setDisabled) 画灰；返回 1 → 调 0xAB7000(setEnabled)。
// 桩在入口对扩展建筑直接 setEnabled(true) 并返回，绕开整条判定链。
// 重放 6 字节 = sub esp,8 / push esi / mov esi,ecx，边界在 +6（call 0xABB360）。
const uint8_t AOB_BTNENABLE[11] = { 0x83,0xEC,0x08, 0x56, 0x8B,0xF1, 0xE8,0x65,0xFC,0xFF,0xFF };

struct HookDef {
    const char*    name;
    uint32_t       rva;
    const uint8_t* aob;
    uint32_t       aobLen;
    void*          stub;
    uint32_t*      retVar;
    uint32_t       replayLen;   // 需重放的完整指令字节数（= 跳回偏移）
};

const HookDef kHooks[] = {
    { "PlayerID",   0x0FF95B, AOB_PLAYERID,  6, (void*)&PlayerIdCaveStub,   &g_retPlayerId,    6 },
    { "Money",      0x6CFDFE, AOB_MONEY,     5, (void*)&MoneyCaveStub,      &g_retMoney,       5 },
    { "Power",      0x6CFD0D, AOB_POWER,     9, (void*)&PowerCaveStub,      &g_retPower,       9 },
    { "SCPoint",    0x6CFE6C, AOB_SCPOINT,   6, (void*)&SCPointCaveStub,    &g_retSCPoint,     6 },
    { "HaveAllSC",  0x6CFEB5, AOB_HAVEALLSC, 5, (void*)&HaveAllSCCaveStub,  &g_retHaveAllSC,   5 },
    { "FastBuild1", 0x30F42E, AOB_FASTBUILD1,5, (void*)&FastBuild1CaveStub, &g_retFastBuild1,  5 },
    { "FastBuild2", 0x30F38C, AOB_FASTBUILD2,6, (void*)&FastBuild2CaveStub, &g_retFastBuild2,  6 },
    { "FastBuild3", 0x2F6CFF, AOB_FASTBUILD3,6, (void*)&FastBuild3CaveStub, &g_retFastBuild3,  6 },
    { "Map",        0x3C226C, AOB_MAP,       8, (void*)&MapCaveStub,        &g_retMap,         8 },
    { "UnitAmmo",   0x128735, AOB_UNITAMMO,  5, (void*)&UnitAmmoCaveStub,   &g_retUnitAmmo,    5 },
    { "DangerLevel",0x438ED0, AOB_DANGER,    6, (void*)&DangerLevelCaveStub,&g_retDangerLevel, 6 },
    { "OreMine",    0x2D4933, AOB_OREMINE,   6, (void*)&OreMineCaveStub,    &g_retOreMine,     6 },
    { "GodMode",    0x12EEDF, AOB_GODMODE,   5, (void*)&GodModeCaveStub,    &g_retGodMode,     5 },
    { "EnemyCantBuild",0x30F530,AOB_ENEMYBUILD,5,(void*)&EnemyCantBuildCaveStub,&g_retEnemyCantBuild,5 },
    { "SuperPower", 0x2EBB69, AOB_SUPERPOWER,6, (void*)&SuperPowerCaveStub,  &g_retSuperPower,  6 },
    { "DisableAllSP",0x43ECA6,AOB_DISABLEALLSP,6,(void*)&DisableAllSPCaveStub,&g_retDisableAllSP,6 },
    { "ProtoAll",   0x69A9B3,AOB_PROTOALL,    6,(void*)&ProtoAllCaveStub,   &g_retProtoAll,   5 },
    { "ProtoClick", 0x69AB03,AOB_PROTOCLICK,  7,(void*)&ProtoClickCaveStub, &g_retProtoClick, 5 },
    { "Zoom",       0x1EC7CD, AOB_ZOOM,    7, (void*)&ZoomCaveStub,       &g_retZoom,    5 },
    { "ZoomPost",   0x1EC82D, AOB_ZOOMPOST,11,(void*)&ZoomPostStub,       &g_retZoomPost,6 },
    { "ZoomEntry",  0x1EC730, AOB_ZOOMENTRY,14,(void*)&ZoomEntryStub,     &g_retZoomEntry,5 },
    { "OneKill",    0x3651AE, AOB_ONEKILL, 7, (void*)&OneKillCaveStub,    &g_retOneKill, 5 },
    { "OKData1",    0x3FE714, AOB_OKDATA1, 8, (void*)&OKData1CaveStub,    &g_retOKData1, 6 },
    { "OKData2",    0x2E24E3, AOB_OKDATA2, 10,(void*)&OKData2CaveStub,    &g_retOKData2, 6 },
    { "PowerLockProbe",0x67E9AB,AOB_LOCKPROBE, 5,(void*)&LockProbeCaveStub, &g_retLockProbe, 5 },   // VA 0xA7E9AB - 0x400000
    // ✗ 已撤（2026-10-22 10:42 崩溃根因）：replayLen=4 < 5 字节 patch，跳回 site+4 正好落在
    //   E9 的第 5 字节上 → 指令错位 → ecx 不是 [0xCE2FB8] → call 0x20C4A0 的 this 是垃圾
    //   → [esi+0x28] 野指针 → 0x20C4FB c0000005。且两局实测 n=0（黑锁不走这条），无诊断价值。
    //   铁律：replayLen 必须 ≥ 5 且必须是指令边界。
    // { "BuildLock",  0x6B7440, AOB_BUILDLOCK,  10,(void*)&BuildLockCaveStub, &g_retBuildLock,  4 },
    // ✗ 已撤：0x6C5D90 是通用 tooltip 构造器（不止禁用原因），抓到的是鼠标悬停的任意按钮
    //   （本局抓到 SovietRefinery，玩家已有 2 个），锁格并不走这里。
    // { "BuildTip",   0x6C5D90, AOB_BUILDTIP,   10,(void*)&BuildTipCaveStub,  &g_retBuildTip,   6 },
    { "Science",    0x4451F0, AOB_SCIENCE,    11,(void*)&ScienceCaveStub,   &g_retScience,    6 }, // 建造前置科技判定（可强开，默认关）
    { "ClanGate",   0x43EA70, AOB_CLANGATE,   10,(void*)&ClanGateCaveStub,  &g_retClanGate,  10 }, // 可用对象集合判定（0x1B，默认关）
    { "BuildBtn",   0x6B7070, AOB_BTNPROBE,   10,(void*)&BuildBtnProbeCaveStub, &g_retBtnProbe, 5 }, // 建造页按钮对象探针（0x1C，默认关；VA 0xAB7070）
    { "CmdSet",     0x6CB352, AOB_CMDSET,      8,(void*)&CommandSetProbeCaveStub, &g_retCsProbe, 8 }, // CommandSet 探针（0x1C，默认关；VA 0xACB352）
    { "BtnEnable",  0x6BB6F0, AOB_BTNENABLE,  11,(void*)&BtnEnableCaveStub, &g_retBtnEnable, 6 }, // 扩展建筑按钮强制可用（0x1E，默认关；VA 0xABB6F0）
};

// 已装站表（卸载回写用）：InstallHooks 记录，RemoveHooks 逆操作
int g_installedIdx[32] = { 0 };
int g_installedCount = 0;

} // namespace

HookStatus InstallHooks() {
    HookStatus st;
    st.moduleBase = MainModuleBase();
    st.moduleSize = MainModuleSize();
    st.total = static_cast<int>(sizeof(kHooks) / sizeof(kHooks[0]));
    if (!st.moduleBase || !st.moduleSize)
        return st;

    // 默认开启的「建造页扩展」两项（其余开关一律默认关，与历史行为一致）：
    //   0x1D 补全扩展建筑 —— 构建器 0xACB330 只在「进对局」那一刻跑一次，
    //        必须在那之前就是开的状态；让用户进局后再去勾，100% 会错过时机。
    //   0x1E 解除灰色     —— 与 0x1D 配套，否则补出来的两格是灰的、点不动。
    // 用户在 UI 里取消勾选仍然有效（只是重注入/重开游戏后会回到默认开）。
    g_flags[FID_BUILDEXT]  = 1;
    g_flags[FID_BTNENABLE] = 1;

    char* fp = st.failed;
    size_t remain = sizeof(st.failed);

    int idx = 0;
    for (const auto& h : kHooks) {
        const uintptr_t site = st.moduleBase + h.rva;
        if (!VerifyBytes(site, h.aob, h.aobLen)) {
            int n = _snprintf_s(fp, remain, _TRUNCATE, "%s ", h.name);
            if (n > 0) { fp += n; remain -= static_cast<size_t>(n); }
            ++idx;
            continue;
        }
        st.aobOk++;

        void* cave = AllocCave(64);
        if (!cave) { ++idx; continue; }

        uint8_t tramp[6] = { 0x68, 0, 0, 0, 0, 0xC3 };   // push <stub>; ret
        std::memcpy(tramp + 1, &h.stub, 4);
        if (!WriteRaw(reinterpret_cast<uintptr_t>(cave), tramp, sizeof(tramp))) { ++idx; continue; }

        *h.retVar = static_cast<uint32_t>(site + h.replayLen);
        if (PatchJmp(site, cave)) {
            st.installed++;
            if (g_installedCount < 32) g_installedIdx[g_installedCount++] = idx;
        }
        ++idx;
    }
    return st;
}

// 卸载：把「已装」的功能 hook 站回写原字节（只回写被 patch 的 5 字节 —— Zoom/OneKill 的
//   aobLen 为 7/10 长于 patch 长度，多出的部分是 stub 重放的指令，未被动过）。
// 逐站回读校验；返回成功回写数。code cave（AllocCave 的 64B/站）不回收：site 还原后
//   无人再跳入，泄漏量 ≈1.3KB/轮，可忽略。
int RemoveHooks() {
    int ok = 0;
    for (int k = 0; k < g_installedCount; ++k) {
        const HookDef& h = kHooks[g_installedIdx[k]];
        const uintptr_t site = MainModuleBase() + h.rva;
        if (WriteRaw(site, h.aob, 5) && VerifyBytes(site, h.aob, 5))
            ++ok;
    }
    return ok;
}

void SetFeature(int idx, bool on) {
    if (idx >= 0 && idx < FID_COUNT)
        g_flags[idx] = on ? 1 : 0;
}

bool GetFeature(int idx) {
    return (idx >= 0 && idx < FID_COUNT) ? (g_flags[idx] != 0) : false;
}

void TriggerOneShot(int idx) {
    if (idx >= 0 && idx < FID_COUNT)
        g_flags[idx] = 1;
}

void MoneyAdd(int32_t delta) {
    g_moneyValue = delta;
    g_moneyOp = 1;
}

void MoneySet(int32_t value) {
    g_moneyValue = value;
    g_moneyOp = 2;
}

int32_t LocalMoney() {
    if (!g_moneyObj) return 0;
    return *reinterpret_cast<int32_t*>(
        static_cast<uintptr_t>(g_moneyObj) + OFF_MONEY);
}

// ---- 免费建造：1 字节静态补丁（75 0C -> EB 0C），站点由符号解析给出 ----
bool SetFreeBuild(bool on) {
    const uintptr_t at = SymFreeBuildSite();
    if (!at) return false;
    const uint8_t jne = 0x75, jmp = 0xEB;
    uint8_t cur = *reinterpret_cast<uint8_t*>(at);
    if (cur != jne && cur != jmp) return false;       // 字节不符，别碰
    return WriteRaw(at, on ? &jmp : &jne, 1);
}

bool GetFreeBuild() {
    const uintptr_t at = SymFreeBuildSite();
    if (!at) return false;
    return *reinterpret_cast<uint8_t*>(at) == 0xEB;
}

// ---- 协议技能「需要该阵营基地」解锁 ----
// 0x83B740 = 玩家建筑列表谓词：遍历 Player+0x1260，逐个 Thing 调 0x7E43F0(tpl, filter)。
//   0x83B81B: 32 C0   xor al,al   ← 未命中分支（返回 false）
//   0x83B822: B0 01   mov al,1    ← 命中分支（返回 true）
// 把未命中分支改成 mov al,1 ⇒ 谓词恒真 ⇒ 跨阵营协议技能不再要求场上存在该阵营基地。
// 实测：改完后摧毁对方阵营建造场，技能按钮仍为可释放态。
static const uint8_t  kProtoUnlockOff[2] = { 0x32, 0xC0 };
static const uint8_t  kProtoUnlockOn[2]  = { 0xB0, 0x01 };
static const uint32_t kProtoUnlockRva    = 0x43B81B;   // VA 0x83B81B - ImageBase

bool GetProtoUnlock() {
    const uintptr_t at = MainModuleBase() + kProtoUnlockRva;
    return VerifyBytes(at, kProtoUnlockOn, 2);
}

bool SetProtoUnlock(bool on) {
    const uintptr_t at = MainModuleBase() + kProtoUnlockRva;
    if (!at) return false;
    const uint8_t* want   = on ? kProtoUnlockOn : kProtoUnlockOff;
    const uint8_t* expect = on ? kProtoUnlockOff : kProtoUnlockOn;
    if (VerifyBytes(at, want, 2)) return true;      // 已是目标状态（幂等）
    if (!VerifyBytes(at, expect, 2)) return false;  // 字节不符，别碰
    return WriteRaw(at, want, 2);
}

// ============================================================================
// 战役建造页补全扩展建筑（FID_BUILDEXT = 0x1D）
//
// 战役局里建造者（建造基地/起重车）用的 CommandSet 比扩展模式少 2~4 条。
// 三个可玩阵营结构完全一致（2026-10-22 实测，def 指针逐条共享）：
//   Soviet    0CCE8570(15) -> 0CCE8848(19)   胡杨防空 / 礁石岸防 / SovietCrane / 0D019050
//   Allied    0CCED830(15) -> 08E45CF0(18)   重型火炮防御基地 / 宙斯盾大型防御基地 / Upgrade_AlliedTech4
//   Celestial 08E42390(14) -> 08E42180(16)   能量加特林塔 / 能量轨道炮基地
//   Japan     0CCEB130(16) -> 08E3F6B0(20)   点防御控制塔 / 神风指挥塔 / 巨型要塞舰 / 欧米伽湮灭炸弹
// 引擎在内存里同时常驻着「扩展完整版」的同族 CommandSet（两者的 CommandButton
// 定义对象是同一批指针，逐条共享），所以只需把战役版 cs 的 inner 指针指向完整版
// 的 inner，count/base 一起跟过去 —— 不必扩数组、不必造对象、不必碰模板库。
//
// 时机：0xACB352（建造页列表构建器 0xACB330 内，刚查表拿到 cs、循环 new 按钮之前），
//       所以改完同一轮构建就按新 count 走，无需重建对局。
// 找法：扫全内存取 vtable == 0x00BE9178 的实例，按「含本阵营 PowerPlant/Barracks/
//       Refinery/WarFactory 四个签名」认出建造者类，再取「含扩展建筑且定义集 ⊇ 当前」的那份。
//       builder 签名必须按阵营分组 —— 苏联那四个哈希在盟军/天朝 cs 里一个都没有，
//       用单组判定会把它们全漏掉（这正是「只对苏联有效」的原因）。
// ============================================================================
namespace {

const uint32_t CS_VTABLE = 0x00BE9178;

// 各阵营「战役版缺、扩展版有」的额外建筑。Ra3BuildExtFix 用它判定某个 cs
// 是否已经是完整版；Ra3IsExtTpl 用它在按钮层强制可用（BtnEnableCaveStub）。
// 四阵营结构一致（2026-10-22 实测）：
//   苏联  0CCE8570(15) -> 0CCE8848(19)
//   盟军  0CCED830(15) -> 08E45CF0(18)
//   天朝  08E42390(14) -> 08E42180(16)
//   日本  0CCEB130(16) -> 08E3F6B0(20)   （日本建筑在日冕里全叫 *Egg）
const uint32_t EXT_BLD[] = {
    0x6094F50D,   // SovietHeavyAntiAirMissileTurret      胡杨防空导弹系统
    0x2D270F77,   // SovietDualPurposeCruiseMissileTurret 礁石岸防导弹要塞
    0x19681796,   // AlliedHeavyArtilleryDefenseBase      盟军重型火炮防御基地
    0x32535841,   // AlliedAegisLargeDefenseBase          盟军宙斯盾大型防御基地
    0xF84AA5E2,   // CelestialEnergyGatlingTower          天朝能量加特林塔
    0xCCA5F13A,   // CelestialEnergyRailgunBase           天朝能量轨道炮基地
    0xF6CAF260,   // JapanPointShieldControlTowerEgg      日本点防御控制塔
    0x7117987E,   // JapanKamikazeCommandTowerEgg         日本神风指挥塔
    0x5066384B,   // JapanGigaFortressShipEgg             日本巨型要塞舰
    0x078AB8D6,   // JapanOmegaOblivionBombEgg            日本欧米伽湮灭炸弹
};
const int EXT_BLD_N = static_cast<int>(sizeof(EXT_BLD) / sizeof(EXT_BLD[0]));

// 建造者识别签名：每阵营的「电厂 / 兵营 / 矿场 / 战车工厂」四栋。
// 四栋全命中才认作建造基地/起重车类 CommandSet（单看一个容易误配）。
const uint32_t SIG_BLD[][4] = {
    { 0xCF8D9F20, 0x80D4EA1E, 0xC45AAEAB, 0xD50C222E },  // Soviet
    { 0x89B86E3D, 0x509BD329, 0x8ECE261C, 0x8209C058 },  // Allied
    { 0xE7B1F1F5, 0x15B5C047, 0xCD6164E6, 0x41003FFD },  // Celestial
    { 0xC6B3F8AA, 0x57F07A3A, 0xFF4A8B60, 0xA02E31D3 },  // Japan（*Egg 后缀）
};
const int SIG_GRP_N = static_cast<int>(sizeof(SIG_BLD) / sizeof(SIG_BLD[0]));

const uint32_t MAX_CS_NODES = 768;
const uint32_t MAX_HASH     = 48;

struct CsNode {
    uint32_t cs, inner, count, base;
    uint32_t nhash;
    uint32_t hash[MAX_HASH];
    bool     builder;
    bool     hasExt;
};

CsNode   s_nodes[MAX_CS_NODES];
uint32_t s_nodeN       = 0;
bool     s_fullScanned = false;

bool ReadCsNode(uint32_t cs, CsNode& n) {
    __try {
        uint32_t inner = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(cs + 4));
        if (inner < 0x00010000) return false;
        uint32_t cnt  = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(inner + 4));
        uint32_t base = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(inner + 8));
        if (cnt == 0 || cnt > MAX_HASH || base < 0x00010000) return false;
        n.cs = cs; n.inner = inner; n.count = cnt; n.base = base;
        n.nhash = 0; n.builder = false; n.hasExt = false;
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t def = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(base + i * 4));
            if (def < 0x00010000) continue;
            uint32_t sub = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(def + 4));
            if (sub < 0x00010000) continue;
            uint32_t tpl = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(sub + 0x1C));
            if (tpl < 0x00010000) continue;
            uint32_t h = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(tpl + 8));
            if (!h) continue;
            if (n.nhash < MAX_HASH) n.hash[n.nhash++] = h;
            for (int e = 0; e < EXT_BLD_N; e++)
                if (h == EXT_BLD[e]) { n.hasExt = true; break; }
        }
        n.builder = false;
        for (int g = 0; g < SIG_GRP_N && !n.builder; g++) {
            bool f[4] = { false, false, false, false };
            for (uint32_t i = 0; i < n.nhash; i++)
                for (int k = 0; k < 4; k++) if (n.hash[i] == SIG_BLD[g][k]) f[k] = true;
            n.builder = f[0] && f[1] && f[2] && f[3];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int FindNode(uint32_t cs) {
    for (uint32_t i = 0; i < s_nodeN; i++)
        if (s_nodes[i].cs == cs) return static_cast<int>(i);
    return -1;
}

// 在 [lo, hi) 里搜 CommandSet 实例（vtable == 0x00BE9178），结果追加到 s_nodes。
// 注意：不要用「每命中一个 0x78 就 memchr 一次」的写法 —— 2GB 里 800 万次 memchr
// 调用要 4.6 秒（实测）。这里按 4 字节对齐步进比较，只走堆区。
void ScanRange(uintptr_t lo, uintptr_t hi) {
    if (hi > 0x7FFF0000) hi = 0x7FFF0000;
    if (lo < 0x10000) lo = 0x10000;
    uintptr_t addr = lo;
    while (addr < hi && s_nodeN < MAX_CS_NODES) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) break;
        uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        size_t size    = mbi.RegionSize;
        if (!size) break;
        uintptr_t next = base + size;
        DWORD prot = mbi.Protect;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && prot != PAGE_NOACCESS &&
            !(prot & PAGE_GUARD) && !(prot & PAGE_EXECUTE) &&
            (prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY))) {
            uintptr_t s = base > lo ? base : lo;
            uintptr_t e = next < hi ? next : hi;
            s = (s + 3) & ~(uintptr_t)3;
            for (uintptr_t a = s; a + 4 <= e && s_nodeN < MAX_CS_NODES; a += 4) {
                if (*reinterpret_cast<const uint32_t*>(a) != CS_VTABLE) continue;
                if (FindNode(static_cast<uint32_t>(a)) >= 0) continue;   // 已收录，去重
                CsNode nd;
                if (ReadCsNode(static_cast<uint32_t>(a), nd))
                    s_nodes[s_nodeN++] = nd;
            }
        }
        addr = next;
    }
}

// 全内存兜底（只在邻域没找到时走一次，慢但罕见）
void ScanAllMemory() {
    s_fullScanned = true;
    DWORD t0 = GetTickCount();
    ScanRange(0x10000, 0x7FFF0000);
    g_extScanMs = GetTickCount() - t0;
}

// 邻域扫描：同族 CommandSet 的实例指针是连着的（实测 0x0CCE8730 与 0x0CCE8848 只差 0x118），
// 所以只在 cs±0x40000 里找就够，耗时可以忽略。
void ScanNear(uint32_t cs) {
    if (cs < 0x80000) return;
    ScanRange(static_cast<uintptr_t>(cs) - 0x40000, static_cast<uintptr_t>(cs) + 0x40000);
}

}  // namespace

uint32_t BuildExtDump(uint32_t* out, uint32_t maxEntries) {
    if (!s_fullScanned) ScanAllMemory();
    uint32_t n = 0, bld = 0, ext = 0;
    for (uint32_t i = 0; i < s_nodeN; i++) {
        if (s_nodes[i].builder) bld++;
        if (s_nodes[i].builder && s_nodes[i].hasExt) ext++;
    }
    if (out && maxEntries >= 4) {
        out[n++] = s_nodeN;
        out[n++] = bld;
        out[n++] = ext;
        out[n++] = g_extScanMs;
    }
    for (uint32_t i = 0; i < s_nodeN && n + 3 <= maxEntries; i++) {
        if (!s_nodes[i].builder) continue;
        out[n++] = s_nodes[i].cs;
        out[n++] = s_nodes[i].inner;
        out[n++] = (s_nodes[i].count & 0xFFFF) | (s_nodes[i].hasExt ? 0x80000000u : 0);
    }
    return n;
}

// 供 BtnEnableCaveStub 用：该模板哈希是否是「扩展建筑」（三阵营任一）。
// asm 里 push hash / call Ra3IsExtTpl / add esp,4，返回 eax=1 表示命中。
extern "C" uint32_t Ra3IsExtTpl(uint32_t hash) {
    for (int i = 0; i < EXT_BLD_N; i++)
        if (hash == EXT_BLD[i]) return 1;
    return 0;
}

extern "C" uint32_t Ra3BuildExtFix(uint32_t cs) {
    if (!g_flags[FID_BUILDEXT]) return 0;
    if (!cs || cs < 0x00010000) return 0;
    // 先查缓存 → 再扫邻域（毫秒级）→ 最后才全内存兜底（几百 ms，罕见）
    int idx = FindNode(cs);
    if (idx < 0) { ScanNear(cs); idx = FindNode(cs); }
    if (idx < 0) {
        if (!s_fullScanned) ScanAllMemory();
        idx = FindNode(cs);
    }
    if (idx < 0) return 0;
    const CsNode* x = &s_nodes[idx];
    if (x->hasExt) return 0;          // 已经是完整版，不动
    if (!x->builder) return 0;        // 不是建造者类，不碰

    const CsNode* best = nullptr;
    for (uint32_t i = 0; i < s_nodeN; i++) {
        const CsNode& y = s_nodes[i];
        if (&y == x || !y.builder || !y.hasExt) continue;
        if (y.count < x->count) continue;
        bool all = true;
        for (uint32_t k = 0; k < x->nhash && all; k++) {
            bool found = false;
            for (uint32_t m = 0; m < y.nhash; m++)
                if (y.hash[m] == x->hash[k]) { found = true; break; }
            if (!found) all = false;
        }
        if (all && (!best || y.count > best->count)) best = &y;
    }
    if (!best) return 0;

    __try {
        uint32_t* p = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(cs + 4));
        if (*p == best->inner) return 0;
        *p = best->inner;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    g_extFixCount++;
    g_extLastCs  = cs;
    g_extLastDst = best->inner;
    return best->inner;
}

int LocalPlayerId() {
    if (!g_localPlayerId) return -1;
    return static_cast<int>(g_localPlayerId);
}

bool LocalPlayerReady() {
    return g_localPlayerId != 0;
}

uintptr_t LocalPlayerObj() {
    return static_cast<uintptr_t>(g_localPlayerObj);
}

} // namespace ra3
