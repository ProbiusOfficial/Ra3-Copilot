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
float    g_zoomMax        = 20000.0f; // 已废用：ZF 机制不写任何上限字段（保留占位，待清理）
float    g_zoomMaxOff     = 600.0f;  // 已废用：同上
uint32_t g_zoomCam34      = 0;      // 遥测：ZoomPostStub 采样的 [+0x34]
uint32_t g_zoomPitch      = 0;      // 遥测：ZoomPostStub 采样的 [+0x74]
uint32_t g_zoomObj        = 0;      // 遥测：最近一次 this 指针
uint32_t g_zoomVal        = 0;      // 遥测：ZoomCaveStub 进入时 [esi+0x44]
uint32_t g_zoomPost       = 0;      // 遥测：clamp 之后 [esi+0x44]
uint8_t  g_zoomState      = 0;      // 已废用：旧护栏语义，现无写入方（保留占位，待清理）
uint32_t g_zoomEntry      = 0;      // 遥测：缩放函数入口命中次数
uint32_t g_telemEnter     = 0;
uint32_t g_telemApplied   = 0;

uint32_t g_retMoney = 0, g_retPlayerId = 0, g_retPower = 0, g_retSCPoint = 0;
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
// Zoom / 秒杀 3 hook（基于 CE）—— 唯一长签名
const uint8_t AOB_ZOOM[7]     = { 0xF3,0x0F,0x11,0x46,0x44,0x74,0x39 };
const uint8_t AOB_ZOOMPOST[11] = { 0x88,0x9E,0x54,0x26,0x00,0x00,0xE8,0xB8,0xEA,0xFF,0xFF };
const uint8_t AOB_ZOOMENTRY[14] = { 0xA1,0x50,0xB7,0xCD,0x00,0x53,0x33,0xDB,0x38,0x98,0xBC,0x00,0x00,0x00 };
const uint8_t AOB_ONEKILL[7]  = { 0xF3,0x0F,0x11,0x46,0x04,0x74,0x20 };
const uint8_t AOB_OKDATA1[8]  = { 0x8B,0x8E,0x3C,0x03,0x00,0x00,0x8B,0x95 };
const uint8_t AOB_OKDATA2[10] = { 0x8B,0x89,0x3C,0x03,0x00,0x00,0x85,0xC9,0x74,0x0C };

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
    { "Zoom",       0x1EC7CD, AOB_ZOOM,    7, (void*)&ZoomCaveStub,       &g_retZoom,    5 },
    { "ZoomPost",   0x1EC82D, AOB_ZOOMPOST,11,(void*)&ZoomPostStub,       &g_retZoomPost,6 },
    { "ZoomEntry",  0x1EC730, AOB_ZOOMENTRY,14,(void*)&ZoomEntryStub,     &g_retZoomEntry,5 },
    { "OneKill",    0x3651AE, AOB_ONEKILL, 7, (void*)&OneKillCaveStub,    &g_retOneKill, 5 },
    { "OKData1",    0x3FE714, AOB_OKDATA1, 8, (void*)&OKData1CaveStub,    &g_retOKData1, 6 },
    { "OKData2",    0x2E24E3, AOB_OKDATA2, 10,(void*)&OKData2CaveStub,    &g_retOKData2, 6 },
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
