// features.cpp - 功能核实现（表驱动多 hook）
#include "features.h"
#include "hook.h"
#include "symres.h"
#include "cs_pairs.h"
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
// 0x1E 的两级开关：先「只计数」确认判定与结构正确，再放行真正的 setEnabled。
// 这样即使命中逻辑有偏差也不会改坏按钮对象 —— 前四次崩溃就是没分这两步。
uint32_t g_btnForceCount = 0;   // 命中扩展建筑模板的累计次数（演练也记）
uint32_t g_btnForceReal  = 0;   // 0 = 只计数（安全）；1 = 真调 setEnabled
uint32_t g_btnLastObj    = 0;   // 最近命中的按钮对象（只读取证：验 +0x18/+0x3D）
uint32_t g_btnLastHash   = 0;   // 最近命中的模板哈希（只读取证）
uint32_t g_retBuildability = 0; // Buildability hook 跳回点（站 + 8）
uint32_t g_retSetDis = 0;       // setDisabled hook 跳回点（站 + 5）
uint32_t g_extCallCount = 0;    // BuildExtCs hook 被调用次数（进局构建建造页时触发）
uint32_t g_extFixCount = 0;   // 建造页补全扩展建筑：生效次数（真写）
uint32_t g_extDryCount = 0;   // 演练：本会替换、但没写内存的次数
bool     g_extApply    = false;  // 默认演练（只报告）；显式 /buildext?apply=1 才真写
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
uint32_t g_retBuildExtCs = 0;            // BuildExtCsStub 的跳回点（site + 8）
uint32_t g_extReplaceFn  = 0;            // → ra3::ReplaceBuildExtCs（InstallHooks 里赋值）
// stub 里需要"绝对地址"的三处：两个构建不一致，运行期按 IsSteamBuild() 覆写
uint32_t g_buildVer      = 0;            // 0 = 零售 1.12，1 = Steam TUC 1.12
uint32_t g_zoomEntrySrc  = 0x00CDB750;   // ZoomEntry 原指令 mov eax,[abs] 的 abs
uint32_t g_lockProbeCall = 0x00AD9EC0;   // 技能锁定探针 call 的目标
uint32_t g_btnSetEnabled = 0;             // 建造按钮 setEnabled(true)，InstallHooks 里按构建填 VA（0=未就绪）
// 0x1F：查表前换 cs key
uint32_t g_retCsKey       = 0;            // CsKeyCaveStub 的跳回点（site + 9）
uint32_t g_mapCsKeyFn     = 0;            // → ra3::MapCsKey
uint32_t g_csStoreSlot    = 0x00CD8D00;   // CommandSet 注册表全局槽（Steam 0x00CDDEA0）
uint32_t g_csKeyCalls     = 0;
uint32_t g_csKeyHits      = 0;
uint32_t g_csKeyNoOwner   = 0;
uint32_t g_csKeyLastKey   = 0;
uint32_t g_csKeyLastEnh   = 0;
uint32_t g_csKeyLastProd  = 0;
uint32_t g_csKeyLastOwner = 0;
uint32_t g_csKeyLastLocal = 0;   // 收窄时现走玩家链拿到的本机对象（诊断归属误判用）
uint32_t g_setDisRet      = 0;   // setDisabled 入口的调用者返回地址（取证：灰是谁下的令）
uint32_t g_setDisArg0     = 0;   // setDisabled 入口的第一个栈参数（取证）
uint32_t g_cbResult       = 0;   // 第二道门的返回值（0 = 不置灰）
uint32_t g_cbTemplate     = 0;
uint32_t g_cbEbx          = 0;
uint32_t g_cbEsi          = 0;
uint32_t g_cbCount        = 0;
uint32_t g_cbTargetHash   = 0x6094F50Du;   // 默认盯胡杨防空
uint32_t g_retCanProbe    = 0;
// 建造按钮「第二道门」全量取证表：每格 {模板哈希, 门返回, arg0(玩家/阵营对象), 命中次数}
//   门返回 0 = 不置灰；非 0 = 走 setDisabled（1/6/7/8，见 Steam 0x18DD20）
uint32_t g_cbRing[24][4]  = {};
uint32_t g_cbRingN        = 0;
uint32_t g_cbLastArg1     = 0;   // 最近一次门的第二个参数（Template，= ebp）
uint32_t g_canProbeFn     = 0;   // → ra3::CanProbeRecord（InstallHooks 里赋值）
uint32_t g_cbProbeOn      = 1;   // 门探针开关（0 = 完全旁路，只剩原指令重放）
uint32_t g_builderLastCs  = 0;   // 建造页构建器实际拿到的 CommandSet 句柄（只读取证）
uint32_t g_csKeyPtr       = 0;   // CsKeyCaveStub 入口的 &key（写回用）
uint32_t g_builderRing[8][2] = {};  // 构建器最近 8 次 {cs, 条目数}
uint32_t g_builderRingN   = 0;
uint32_t g_csKeyCached    = 0;   // 成功把 Thing+0x464 写成扩展版 key 的次数
uint32_t g_csKeyCacheFail = 0;   // 写缓存失败（指针不可写）的次数
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
// Steam TUC 版同一函数：mov eax,[0xCE08F0]（零售是 [0xCDB750]，绝对地址随布局变）
const uint8_t AOB_ZOOMENTRY_STEAM[14] = { 0xA1,0xF0,0x08,0xCE,0x00,0x53,0x33,0xDB,0x38,0x98,0xBC,0x00,0x00,0x00 };
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
// Buildability(Player*,Template*) 入口 8 字节（到 mov edi,ecx 之前）：两版逐字节相同
// setDisabled 入口 10 字节（两版逐字节相同，全 exe 唯一；只 patch 前 5 字节）
const uint8_t AOB_SETDIS[10] = { 0x80,0x79,0x3D,0x00, 0x56, 0x8D,0x71,0x08, 0x74,0x0F };
const uint8_t AOB_BUILDABLE[8] = { 0x56, 0x8B,0x74,0x24,0x08, 0x85,0xF6, 0x57 };
const uint8_t AOB_BTNENABLE[11] = { 0x83,0xEC,0x08, 0x56, 0x8B,0xF1, 0xE8,0x65,0xFC,0xFF,0xFF };
// 建造页构建器里「拿到 CommandSet 句柄」那三条指令（两版字节相同，各 3 处命中，按 RVA 校验定位）：
//   mov ebx,eax / test ebx,ebx / mov [esp+18h],ebx
const uint8_t AOB_BUILDEXT_CS[8] = { 0x8B,0xD8, 0x85,0xDB, 0x89,0x5C,0x24,0x18 };
// 建造页构建器里「算完 key、尚未查表」那 9 字节：
//   mov ecx,[eax] / push ecx / mov ecx,[<注册表槽>]   —— 槽地址随构建变，故两套 AOB
const uint8_t AOB_CSKEY[9]        = { 0x8B,0x08, 0x51, 0x8B,0x0D, 0x00,0x8D,0xCD,0x00 };
const uint8_t AOB_CSKEY_STEAM[9]  = { 0x8B,0x08, 0x51, 0x8B,0x0D, 0xA0,0xDE,0xCD,0x00 };
// 建造按钮刷新的第二道门之后：mov edi,eax / test edi,edi / je +0x29（只读取证用）
const uint8_t AOB_CANPROBE[6]     = { 0x8B,0xF8, 0x85,0xFF, 0x74,0x29 };

struct HookDef {
    const char*    name;
    uint32_t       rva;         // 零售 1.12（SizeOfImage 0x93B000）
    uint32_t       rvaSteam;    // Steam TUC 1.12（SizeOfImage 0x950000）；0 = 该构建不装
    const uint8_t* aob;
    uint32_t       aobLen;
    void*          stub;
    uint32_t*      retVar;
    uint32_t       replayLen;   // 需重放的完整指令字节数（= 跳回偏移）
};

// 当前构建下该 hook 的站点 RVA（0 = 不适用）
inline uint32_t SiteRva(const HookDef& h) {
    return IsSteamBuild() ? h.rvaSteam : h.rva;
}

// 校验/回写用的 AOB：默认两构建共用；个别站点的签名里自带绝对地址，需按构建换
inline const uint8_t* AobFor(const HookDef& h) {
    if (IsSteamBuild() && h.aob == AOB_ZOOMENTRY) return AOB_ZOOMENTRY_STEAM;
    if (IsSteamBuild() && h.aob == AOB_CSKEY)     return AOB_CSKEY_STEAM;
    return h.aob;
}

// 第三列 = Steam TUC 1.12 版 RVA（重新编译的同版本二进制，布局整体错位）。
// 定位方法见 tools/map_steam_rva.py（AOB 候选 + capstone 指令指纹比对），
// 每个站点均校验 replayLen 落在指令边界上；0 = 该构建无对应站点，跳过不装。
const HookDef kHooks[] = {
    { "PlayerID",   0x0FF95B, 0x14119B, AOB_PLAYERID,  6, (void*)&PlayerIdCaveStub,   &g_retPlayerId,    6 },
    { "Money",      0x6CFDFE, 0x664E9E, AOB_MONEY,     5, (void*)&MoneyCaveStub,      &g_retMoney,       5 },
    { "Power",      0x6CFD0D, 0x664DAD, AOB_POWER,     9, (void*)&PowerCaveStub,      &g_retPower,       9 },
    { "SCPoint",    0x6CFE6C, 0x664F0C, AOB_SCPOINT,   6, (void*)&SCPointCaveStub,    &g_retSCPoint,     6 },
    { "HaveAllSC",  0x6CFEB5, 0x664F55, AOB_HAVEALLSC, 5, (void*)&HaveAllSCCaveStub,  &g_retHaveAllSC,   5 },
    { "FastBuild1", 0x30F42E, 0x34D81E, AOB_FASTBUILD1,5, (void*)&FastBuild1CaveStub, &g_retFastBuild1,  5 },
    { "FastBuild2", 0x30F38C, 0x34D77C, AOB_FASTBUILD2,6, (void*)&FastBuild2CaveStub, &g_retFastBuild2,  6 },
    { "FastBuild3", 0x2F6CFF, 0x33511F, AOB_FASTBUILD3,6, (void*)&FastBuild3CaveStub, &g_retFastBuild3,  6 },
    { "Map",        0x3C226C, 0x4005BC, AOB_MAP,       8, (void*)&MapCaveStub,        &g_retMap,         8 },
    { "UnitAmmo",   0x128735, 0x169D85, AOB_UNITAMMO,  5, (void*)&UnitAmmoCaveStub,   &g_retUnitAmmo,    5 },
    { "DangerLevel",0x438ED0, 0x477070, AOB_DANGER,    6, (void*)&DangerLevelCaveStub,&g_retDangerLevel, 6 },
    { "OreMine",    0x2D4933, 0x312EB3, AOB_OREMINE,   6, (void*)&OreMineCaveStub,    &g_retOreMine,     6 },
    { "GodMode",    0x12EEDF, 0x1705DF, AOB_GODMODE,   5, (void*)&GodModeCaveStub,    &g_retGodMode,     5 },
    { "EnemyCantBuild",0x30F530,0x34D920,AOB_ENEMYBUILD,5,(void*)&EnemyCantBuildCaveStub,&g_retEnemyCantBuild,5 },
    { "SuperPower", 0x2EBB69, 0x329F19, AOB_SUPERPOWER,6, (void*)&SuperPowerCaveStub,  &g_retSuperPower,  6 },
    { "DisableAllSP",0x43ECA6,0x47CC16, AOB_DISABLEALLSP,6,(void*)&DisableAllSPCaveStub,&g_retDisableAllSP,6 },
    { "ProtoAll",   0x69A9B3, 0x62F473, AOB_PROTOALL,    6,(void*)&ProtoAllCaveStub,   &g_retProtoAll,   5 },
    { "ProtoClick", 0x69AB03, 0x62F5C3, AOB_PROTOCLICK,  7,(void*)&ProtoClickCaveStub, &g_retProtoClick, 5 },
    { "Zoom",       0x1EC7CD, 0x22B82D, AOB_ZOOM,    7, (void*)&ZoomCaveStub,       &g_retZoom,    5 },
    { "ZoomPost",   0x1EC82D, 0x22B88D, AOB_ZOOMPOST,11,(void*)&ZoomPostStub,       &g_retZoomPost,6 },
    { "ZoomEntry",  0x1EC730, 0x22B790, AOB_ZOOMENTRY,14,(void*)&ZoomEntryStub,     &g_retZoomEntry,5 },
    { "OneKill",    0x3651AE, 0x3A366E, AOB_ONEKILL, 7, (void*)&OneKillCaveStub,    &g_retOneKill, 5 },
    { "OKData1",    0x3FE714, 0x43C964, AOB_OKDATA1, 8, (void*)&OKData1CaveStub,    &g_retOKData1, 6 },
    { "OKData2",    0x2E24E3, 0x320963, AOB_OKDATA2, 10,(void*)&OKData2CaveStub,    &g_retOKData2, 6 },
    // Steam 版未定位：签名是纯 rel32（call 目标随布局变），需先解出目标函数新地址再构造签名。
    // 探针性质、默认关，跳过不影响任何功能。
    { "PowerLockProbe",0x67E9AB,0x000000,AOB_LOCKPROBE, 5,(void*)&LockProbeCaveStub, &g_retLockProbe, 5 },
    // ✗ 已撤（2026-10-22 10:42 崩溃根因）：replayLen=4 < 5 字节 patch，跳回 site+4 正好落在
    //   E9 的第 5 字节上 → 指令错位 → ecx 不是 [0xCE2FB8] → call 0x20C4A0 的 this 是垃圾
    //   → [esi+0x28] 野指针 → 0x20C4FB c0000005。且两局实测 n=0（黑锁不走这条），无诊断价值。
    //   铁律：replayLen 必须 ≥ 5 且必须是指令边界。
    // { "BuildLock",  0x6B7440, AOB_BUILDLOCK,  10,(void*)&BuildLockCaveStub, &g_retBuildLock,  4 },
    // ✗ 已撤：0x6C5D90 是通用 tooltip 构造器（不止禁用原因），抓到的是鼠标悬停的任意按钮
    //   （本局抓到 SovietRefinery，玩家已有 2 个），锁格并不走这里。
    // { "BuildTip",   0x6C5D90, AOB_BUILDTIP,   10,(void*)&BuildTipCaveStub,  &g_retBuildTip,   6 },
    { "Science",    0x4451F0, 0x483260, AOB_SCIENCE,    11,(void*)&ScienceCaveStub,   &g_retScience,    6 }, // 建造前置科技判定（可强开，默认关）
    { "ClanGate",   0x43EA70, 0x47C9E0, AOB_CLANGATE,   10,(void*)&ClanGateCaveStub,  &g_retClanGate,  10 }, // 可用对象集合判定（0x1B，默认关）
    { "BuildBtn",   0x6B7070, 0x64BCF0, AOB_BTNPROBE,   10,(void*)&BuildBtnProbeCaveStub, &g_retBtnProbe, 5 }, // 建造页按钮对象探针（0x1C，默认关）
    { "CmdSet",     0x6CB352, 0x000000, AOB_CMDSET,      8,(void*)&CommandSetProbeCaveStub, &g_retCsProbe, 8 }, // CommandSet 探针（0x1C，默认关）
    //   ⚠ Steam 版此站让给 BuildExtCs：两者同址（Steam 0x662422），抢同一 8 字节会互相覆盖。
    //     零售仍走这条（旧路径=改 cs+4，已被实机验证）；Steam 走「换句柄」。
    // ⛔ BtnEnable 站点两版一律不装（=0）：它靠「手动调 setEnabled」改按钮状态，
    //    Steam 上 setEnabled 内部还有两层虚调用，17:25 实测把栈弹坏（EIP 落到栈上、EBP=0x206）。
    //    0x1E 已改到 Buildability 层实现（见 Buildable 项），走游戏自己的 UI 流程。
    { "BtnEnable",  0x000000, 0x000000, AOB_BTNENABLE,  11,(void*)&BtnEnableCaveStub, &g_retBtnEnable, 6 },
    // ⛔ 0x1E 的两条站点（Buildable / SetDisable）2026-10-30 起**一律不装**（RVA 置 0）。
    //    它们是「在判定层替扩展条目开绿灯」，实机后果：按钮亮着但点下去被秒取消
    //    （10-29 RVA 0x288470 链表遍历 AV），换成门钩子版本又在 0x4227A1 崩。
    //    根因：显示列表与判定用的不是同一份 CommandSet（按按钮下标取 def 会越界）。
    //    正解已改为 0x1F：换 cs key **并**写 Thing+0x464，让两边查到同一份列表，
    //    灰/亮完全交回引擎自己的科技链。桩体保留在 stubs.asm 里只为不破坏链接，永不调用。
    { "Buildable",  0x000000, 0x000000, AOB_BUILDABLE,   8,(void*)&BuildableCaveStub, &g_retBuildability, 8 },
    { "SetDisable", 0x000000, 0x000000, AOB_SETDIS,     10,(void*)&SetDisCaveStub, &g_retSetDis, 5 },
    // 建造页构建器：eax = 查表得到的 CommandSet 句柄 → 换成同族扩展完整版句柄（0x1D）
    // 零售 0x6CB352 / Steam 0x662422，重放 8 字节（= AOB 长度，落在指令边界上）
    { "BuildExtCs", 0x000000, 0x662422, AOB_BUILDEXT_CS, 8, (void*)&BuildExtCsStub, &g_retBuildExtCs, 8 },
    //   Steam 走「换句柄」：hook 构建器查表返回值，把战役版 cs 句柄换成同族完整版 —— 不写任何对象。
    //   零售不装（=0）：那里已有 CmdSet+Ra3BuildExtFix 的旧路径（改 cs+4），实机验证过，不换。
    //   ⚠ 2026-10-26 16:45：先前「换句柄崩」的归因**作废**。那四次崩溃的真凶是 0x1E 的 stub
    //      命中扩展建筑时 `mov al,1; ret` 跳过按钮构造函数（见 BtnEnable 处注释），与本站点无关。
    //      0x1E 已默认关，本路径重验中。
    // 扩展模式列表（0x1F）：查表前换 cs key —— 一次覆盖 24 组配对（含 8 处生产建筑），
    //   不靠「四栋建筑签名」猜同族。重放 9 字节（到 call <store.find> 之前），跳回点 = 站 + 9。
    //   默认关：与 0x1D 二选一（0x1D 换句柄、0x1F 换 key），同时开会互相抵消。
    { "CsKey",      0x6CB344, 0x662414, AOB_CSKEY, 9, (void*)&CsKeyCaveStub, &g_retCsKey, 9 },
    // 第二道门取证（只读）：Steam 0x650470 零售未定位 → 零售站点 0，跳过不装。
    { "CanProbe",   0x000000, 0x650470, AOB_CANPROBE, 6, (void*)&CanBuildProbeCaveStub, &g_retCanProbe, 6 },
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

    // ── 诊断模式：exe 同目录存在 ra3copilot_diag.flag 时，**一个功能 hook 都不装** ──
    // 只保留 d3d9 overlay + HTTP 端点（/buildext、/read、/peek、/objects 全是只读）。
    // 用途：在不想让任何 patch 落地的情况下做内存取证（对局崩怕了的时候用）。
    {
        // 用 DLL 自身路径（不是 exe 路径）—— flag 放 ra3-copilot 目录里，绝不往游戏目录写东西
        HMODULE hSelf = nullptr;
        if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCSTR>(&InstallHooks), &hSelf) && hSelf) {
            char p[MAX_PATH] = { 0 };
            if (::GetModuleFileNameA(hSelf, p, MAX_PATH)) {
                char* slash = strrchr(p, '\\');
                if (slash) {
                    *(slash + 1) = 0;
                    strncat_s(p, sizeof(p) - strlen(p) - 1, "ra3copilot_diag.flag", _TRUNCATE);
                    if (::GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) {
                        strncpy_s(st.failed, "DIAG-MODE(no functional hooks)", _TRUNCATE);
                        return st;
                    }
                }
            }
        }
    }

    // 默认开启的「建造页扩展」两项（其余开关一律默认关，与历史行为一致）：
    //   0x1D 补全扩展建筑 —— 构建器 0xACB330 只在「进对局」那一刻跑一次，
    //        必须在那之前就是开的状态；让用户进局后再去勾，100% 会错过时机。
    //   0x1E 解除灰色     —— 与 0x1D 配套，否则补出来的两格是灰的、点不动。
    // 用户在 UI 里取消勾选仍然有效（只是重注入/重开游戏后会回到默认开）。
    // ✅ 2026-10-26 18:33 实测通过（Steam 战役苏联）：生效=2（构建期换句柄），
    //    0x1E 命中 4700+ 次无崩溃；同一按钮 A/B 对照 —— 关=启用位0（灰）/ 开=启用位1（亮）。
    // ⚠ 2026-10-27 21:30：日冕（神州）环境点建造栏必崩（栈见 internal/docs/待修-mod环境建造栏崩溃.md）。
    //    「mod 环境默认关掉这几项」的改法已试并**回滚** —— 未验证有效，且会让 mod 用户失去功能。
    //    在拿到定论前保持历史默认值（1/1），不做环境分支。
    // 扩展模式（建造页）默认走 0x1F：查表前把 cs key 换成 Enhanced key，覆盖 24 组配对
    // （含 8 处生产建筑），零内存写入、两版同一套代码。
    // 0x1D（换 cs 句柄）只在 Steam 有站点、要全内存扫描 2.8~5 秒，且与 0x1F 同时开会互相抵消
    // → 默认关，仍可用 /toggle?f=0x1D&on=1 单独启用。
    g_flags[FID_EXTKEY]    = 1;
    g_flags[FID_BUILDEXT]  = 0;
    // ⛔ 0x1E 两版一律默认关（2026-10-26 16:45 定因）。
    //    BtnEnableCaveStub 命中扩展建筑时走 be_force：调完 setEnabled 就 `mov al,1; ret`
    //    —— 它跳过的是**建造按钮的构造函数**本身（零售 0xABB6F0 / Steam 0x650320 的
    //    `sub esp,8; push esi; mov esi,ecx; call ...` 就是按钮对象初始化）。被跳过 ⇒
    //    调用方拿到一个半初始化按钮对象，之后访问其字段时 EAX=0 → `call [eax+x]` →
    //    跳到堆上执行 → c0000005，且栈已毁（WER 报 StackHash_*，崩点无法反查）。
    //    四次崩溃（15:59 / 16:08 / 16:26 / 16:32）寄存器形态完全一致：
    //      EAX=0 EBX=0 ECX=EBP=按钮对象 EDI=0x2B(43)，出错地址在堆、模块=?
    //    16:32 那次 0x1D 已关仍崩 —— 因为那是扩展模式苏联局，建造页里本来就有
    //    SovietHeavyAntiAirMissileTurret / DualPurposeCruiseMissileTurret 两个按钮，
    //    0x1E 一看到就跳过构造。⇒ 与 0x1D 的替换逻辑无关，是 0x1E 这个 stub 写错了。
    //    要做「强制可点」，hooks 必须挂在「按钮构造完成之后」，不能在构造入口劫持。
    // ⛔ 0x1E 默认关：它是在「判定层」强行给扩展条目开绿灯的做法（setDisabled 改道 /
    //    Buildability 恒真 / 门返回可建），实机证明会造成「亮着但点下去被取消」，
    //    门钩子版本还会在下游 AV（RVA 0x4227A1）。扩展条目的可用性改由 0x1F 写
    //    Thing+0x464 缓存让引擎自己算，不再需要这一路。
    g_flags[FID_BTNENABLE] = 0;

    // stub 内联的绝对地址按构建切换。ZoomEntry 的原指令是 `mov eax,[0xCDB750]`
    // （Steam 版为 [0xCE08F0]），读错地址会让后续 `cmp [eax+0xBC],bl` 直接空指针崩。
    //
    // ⛔ 2026-10-26 18:05 定因：这两个必须是**运行时绝对地址**（stub 里直接 call/jmp/mov）。
    //    历史 bug：Steam 侧 setEnabled 写成了 RVA 0x64BC80（零售那个 0xAB7000 是 VA），
    //    `jmp eax` 于是落到映像内 RVA 0x24BC80 那个不相干的函数上。
    //    崩溃现场：EIP=0x64BC81 / EAX=0x64BC80 / ECX=按钮 / EBP=炮塔模板 ——
    //    四次崩溃（15:59 / 16:08 / 16:26 / 16:32）和 17:25、18:05 全部由它导致。
    //    统一口径：moduleBase + RVA，两个构建同一套写法，不再混用。
    const bool steam = IsSteamBuild();
    const uint32_t mb = static_cast<uint32_t>(st.moduleBase);
    g_buildVer      = steam ? 1 : 0;
    g_zoomEntrySrc  = mb + (steam ? 0x008E08F0u : 0x008DB750u);
    g_btnSetEnabled = mb + (steam ? 0x0064BC80u : 0x006B7000u);
    g_extReplaceFn  = reinterpret_cast<uint32_t>(&ReplaceBuildExtCs);   // BuildExtCsStub 的替换入口
    g_mapCsKeyFn    = reinterpret_cast<uint32_t>(&MapCsKey);            // CsKeyCaveStub 的映射入口
    g_canProbeFn    = reinterpret_cast<uint32_t>(&CanProbeRecord);      // CanBuildProbeCaveStub 的取证入口
    g_csStoreSlot   = steam ? 0x00CDDEA0u : 0x00CD8D00u;                // CommandSet 注册表槽
    // g_lockProbeCall 保持零售值：该站点在 Steam 版未定位，不做安装

    char* fp = st.failed;
    size_t remain = sizeof(st.failed);

    int idx = 0;
    for (const auto& h : kHooks) {
        const uint32_t siteRva = SiteRva(h);
        if (!siteRva) { ++idx; continue; }   // 该构建无对应站点（如 Steam 版未定位的探针）
        const uintptr_t site = st.moduleBase + siteRva;
        if (!VerifyBytes(site, AobFor(h), h.aobLen)) {
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
        const uint32_t siteRva = SiteRva(h);
        if (!siteRva) continue;
        const uintptr_t site = MainModuleBase() + siteRva;
        if (WriteRaw(site, AobFor(h), 5) && VerifyBytes(site, AobFor(h), 5))
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
// 补丁点（零售 VA 0x83B81B / Steam VA 0x8798CB，Steam 值由其前 34 字节上下文唯一命中确认）
constexpr uint32_t kProtoUnlockRvaRetail = 0x43B81B;
constexpr uint32_t kProtoUnlockRvaSteam  = 0x4798CB;
static uintptr_t ProtoUnlockSite() {
    return MainModuleBase() + (IsSteamBuild() ? kProtoUnlockRvaSteam : kProtoUnlockRvaRetail);
}

bool GetProtoUnlock() {
    const uintptr_t at = ProtoUnlockSite();
    return VerifyBytes(at, kProtoUnlockOn, 2);
}

bool SetProtoUnlock(bool on) {
    const uintptr_t at = ProtoUnlockSite();
    if (!at) return false;
    const uint8_t* want   = on ? kProtoUnlockOn : kProtoUnlockOff;
    const uint8_t* expect = on ? kProtoUnlockOff : kProtoUnlockOn;
    if (VerifyBytes(at, want, 2)) return true;      // 已是目标状态（幂等）
    if (!VerifyBytes(at, expect, 2)) return false;  // 字节不符，别碰
    return WriteRaw(at, want, 2);
}

// ============================================================================
// 血条常显（全图单位头顶血条一直画）
//
// 引擎里「这个 Drawable 要不要画血条」由一个判定函数回答，函数在二进制里有两份拷贝
// （一份 this 直接放 ecx，一份先取 Drawable+0x138 拿到 Thing 再判），骨架一致：
//
//   cmp  byte [g_showBars], 0   ; 0xCB4A10（零售 0xCB0348）总开关，当前=1
//   je   → 返回 false
//   cmp  byte [g_uiGate], 0     ; 0xCE08C5（零售 0xCDB725）当前=0
//   jne  → 画血条分支           ← 只改这一个字节
//   test byte [obj+0x13D], 0x40 ; 对象级「强制显示」位
//   jne  → 画血条分支
//   mov  eax, [obj+0x16C] / cmp eax, [g_cmp] ; 之后再接一串对象级条件
//
// g_uiGate 平时是 0，`jne` 永不成立，判定只能落到对象级条件上 —— 所以默认只有选中 /
// 悬停 / 刚挨打的单位有血条。把 `jne` 改成 `je`，就把「g_uiGate == 0」变成「去画血条」，
// 判定恒真 ⇒ 全图所有 Drawable 常显血条。
//
// 只写 1 字节、不动寄存器与栈，关掉时原样写回 0x75；字节不符（构建认错）时一个字不碰。
// ============================================================================
namespace {

constexpr uint8_t kHpBarJne = 0x75;   // 原始：g_uiGate != 0 才画（实际永不成立）
constexpr uint8_t kHpBarJe  = 0x74;   // 改后：g_uiGate == 0 就画

// 两处站点（RVA）。零售列由同一条 AOB 在零售 exe 里唯一命中确认，
// Steam 列的 VA（0x570293 / 0x55BCA0）与战网客户端 HP 条补丁打的两个点逐字节一致。
constexpr uint32_t kHpBarRvaRetail[2] = { 0x12EB93, 0x11A4A0 };
constexpr uint32_t kHpBarRvaSteam[2]  = { 0x170293, 0x15BCA0 };

inline uintptr_t HpBarSite(int i) {
    return MainModuleBase() + Pick(kHpBarRvaRetail[i], kHpBarRvaSteam[i]);
}

} // namespace

bool GetHealthBarAlways() {
    for (int i = 0; i < 2; ++i) {
        const uintptr_t at = HpBarSite(i);
        if (!at || *reinterpret_cast<const uint8_t*>(at) != kHpBarJe) return false;
    }
    return true;
}

bool SetHealthBarAlways(bool on) {
    const uint8_t want   = on ? kHpBarJe : kHpBarJne;
    const uint8_t expect = on ? kHpBarJne : kHpBarJe;
    bool allOk = true;
    for (int i = 0; i < 2; ++i) {
        const uintptr_t at = HpBarSite(i);
        if (!at) { allOk = false; continue; }
        if (*reinterpret_cast<const uint8_t*>(at) == want) continue;    // 已是目标态，幂等
        if (!VerifyBytes(at, &expect, 1)) { allOk = false; continue; }  // 字节不符，别碰
        if (!WriteRaw(at, &want, 1)) allOk = false;
    }
    return allOk;
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

// CommandSet 的 vtable。Steam 版 0xBF0838 由 vtable 内容指纹定位（tools/map_thing_vtable.py）：
// 可重定位的 [0][1][2][3][7] 五项全部落在 0x7F0838 起始的连续位置（+0/+4/+8/+0xC/+0x1C），
// 且两版夹在中间的字符串/元数据字节（"paus" / "ed=" / "Paus" / "veEn"）完全一致 —— 双重印证。
// 用函数而非全局常量：构建决议发生在 OverlayInit，静态初始化期读会拿到"未决议"。
inline uint32_t CsVtable() { return ::ra3::Pick(0x00BE9178, 0x00BF0838); }

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

// 运行时扩展模板白名单：由 0x1F 首次命中时，从 store 取出每组 (普通, 扩展) CommandSet
// 求差集填入 —— 覆盖生产建筑新增的单位/建筑，静态 EXT_BLD 只作兜底。
// 只收「扩展版有、普通版没有」的条目，普通条目一律不进，否则会把它们的科技前置一起放开。
const int MAX_EXT_TPL = 512;
uint32_t s_extTpl[MAX_EXT_TPL];
int      s_extTplN      = 0;
bool     s_extTplFilled = false;
int      s_extTplPair   = 0;   // 实际成功取到并求过差集的配对数（诊断）

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
    uint32_t innerVt;      // inner 对象的 vtable（inner+0）—— 同族判定用
    int      grp;          // 命中的建造者签名组下标（-1 = 不是建造者）
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
        n.nhash = 0; n.builder = false; n.hasExt = false; n.grp = -1;
        n.innerVt = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(inner));
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
            if (f[0] && f[1] && f[2] && f[3]) { n.builder = true; n.grp = g; }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// —— 从 SAGE store 按「名字哈希」取 CommandSet 句柄 ——
//   签名与建造页构建器用的是同一个：`mov ecx,[store]; push key; call find`，
//   thiscall、单参数、返回 cs 句柄（找不到返回 0）。
//   两版差异只有 store 全局与 find 的地址：零售 [0x00CD8D00] / RVA 0x0DF970，
//   Steam [0x00CDDEA0] / RVA 0x121370（两者代码形状一致，均为 thiscall + `ret 4`）。
typedef uint32_t(__thiscall* StoreFindFn)(void* store, uint32_t key);

uint32_t StoreFindCs(uint32_t key) {
    if (!key) return 0;
    const uint32_t storeVa = ::ra3::Pick(0x00CD8D00, 0x00CDDEA0);
    const uint32_t findVa  = ::ra3::Pick(0x004DF970, 0x00521370);
    uint32_t cs = 0;
    __try {
        void* store = *reinterpret_cast<void**>(static_cast<uintptr_t>(storeVa));
        if (!store) return 0;
        const StoreFindFn fn = reinterpret_cast<StoreFindFn>(static_cast<uintptr_t>(findVa));
        cs = fn(store, key);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return cs;
}

bool CsHasHash(const CsNode& n, uint32_t h) {
    for (uint32_t i = 0; i < n.nhash; i++)
        if (n.hash[i] == h) return true;
    return false;
}

// 一次性把 24 组配对的「扩展独有条目」灌进白名单。
//   调用点是 MapCsKey（建造页构建器内部、游戏线程），此时 store 一定已经就绪；
//   放这儿而不是启动时：一局没进时 store 里根本没有这些 key。
void FillExtTplTable() {
    if (s_extTplFilled) return;
    s_extTplFilled = true;
    for (int i = 0; i < kCsPairCount && s_extTplN < MAX_EXT_TPL; i++) {
        const uint32_t enhCs = StoreFindCs(kCsPairs[i].enh);
        if (!enhCs) continue;
        CsNode en = {};
        if (!ReadCsNode(enhCs, en)) continue;
        CsNode ba = {};
        const uint32_t baseCs = StoreFindCs(kCsPairs[i].base);
        const bool haveBase = baseCs && ReadCsNode(baseCs, ba);
        s_extTplPair++;
        for (uint32_t k = 0; k < en.nhash && s_extTplN < MAX_EXT_TPL; k++) {
            const uint32_t h = en.hash[k];
            if (haveBase && CsHasHash(ba, h)) continue;   // 普通版也有 ⇒ 原有条目，不碰
            bool dup = false;
            for (int j = 0; j < s_extTplN; j++)
                if (s_extTpl[j] == h) { dup = true; break; }
            if (!dup) s_extTpl[s_extTplN++] = h;
        }
    }
    // 一组都没取到 ⇒ 这次调用的时机不对（store 还没装好），下次命中再试一次。
    if (s_extTplPair == 0) s_extTplFilled = false;
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
    const uint32_t csVt = CsVtable();   // 循环外取一次：内层是逐 4 字节比较
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
                if (*reinterpret_cast<const uint32_t*>(a) != csVt) continue;
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

// BuildExtCsStub 调用的替换函数：把「战役版建造者句柄」换成「同族扩展完整版句柄」。
// ★ 只返回另一个已存在的句柄，**不修改任何游戏对象** —— 这是与「直接改 cs+4」的关键区别：
//   cs 对象只有 0x10 字节（+0 外层vtable / +4 引用计数 / +8 内层vtable / +0xC inner），
//   构造时还会把句柄注册进 manager+0x40 的 map（key = *(inner+0)+4）并调整 inner 侧引用计数。
//   直接写 inner 会绕过这两条约束，点开建造列表时在容器路径上崩（StackHash）。
uint32_t ReplaceCsImpl(uint32_t cs) {
    g_extCallCount++;                              // 先记账：能区分「hook 没触发」与「匹配失败」
    g_builderLastCs = cs;                          // 构建器查表拿到的 cs（无论 0x1D 开不开都记）
    {
        CsNode bn = {};
        const uint32_t cnt = (cs && ReadCsNode(cs, bn)) ? bn.nhash : 0;
        const uint32_t slot = g_builderRingN % 8;
        g_builderRing[slot][0] = cs;
        g_builderRing[slot][1] = cnt;
        g_builderRingN++;
    }
    if (!g_flags[FID_BUILDEXT]) return cs;
    if (!cs || cs < 0x00010000) return cs;
    int idx = FindNode(cs);
    if (idx < 0) { ScanNear(cs); idx = FindNode(cs); }
    if (idx < 0) return cs;
    // ★ cs 与它的 inner 常分属两个堆簇（实测 2026-10-22 战役局：战役版 cs=0x0CCC71E0，
    //   inner=0x08E21280，而同族完整版 cs=0x08E21270 就贴在 inner 旁边）。
    //   只扫 cs 邻域会一路扫到本簇里的中间变体，永远看不到完整版 ⇒ best 恒为空、生效=0。
    //   所以必须再按 inner 邻域补扫一次（毫秒级；ScanRange 内部已去重）。
    if (!s_nodes[idx].hasExt) {
        ScanNear(s_nodes[idx].inner);
        idx = FindNode(cs);
        if (idx < 0) return cs;
    }
    const CsNode* x = &s_nodes[idx];
    if (!x->builder || x->hasExt) return cs;       // 不是建造者，或本来就是完整版
    const CsNode* best = nullptr;
    for (uint32_t i = 0; i < s_nodeN; i++) {
        const CsNode& y = s_nodes[i];
        if (&y == x || !y.builder || !y.hasExt) continue;
        if (y.grp != x->grp) continue;             // 必须同族（同阵营建造者签名组）
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
    if (!best) return cs;
    g_extFixCount++;
    g_extLastCs  = cs;
    g_extLastDst = best->cs;                        // 采纳的是完整版的「句柄」，不是 inner
    return best->cs;
}

}  // namespace

// 给 InstallHooks 取址用（namespace 内的实现不能跨作用域取址）
uint32_t ReplaceBuildExtCs(uint32_t cs) { return ReplaceCsImpl(cs); }

namespace {
// 归属判定用的安全读：只在构造建造页时调用（一局几次），不值得做缓存。
bool RdPtr(uintptr_t at, uint32_t& out) {
    if (at < 0x00010000 || at > 0x7FFFFFFF) return false;
    __try {
        out = *reinterpret_cast<const uint32_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 每次现走的玩家链：`[[ctx]+0x28]`，用 [player+0x20] 是合法玩家索引（0..7）、
//   [player+0x10] 是 team 指针两条做校验。
//   ★ 为什么要「现走」：g_localPlayerObj 由 PlayerID hook 写入，而那条 hook 要等 UI 读资金
//     才触发 —— 进局那一刻它往往还是**上一局的旧对象**。拿旧对象做收窄会把本机全判成
//     「不是本机」，0x1F 就静默失效（2026-10-27 20:11 实测：进入=3 / 已换=0，就是这个原因）。
uint32_t LocalPlayerFresh() {
    const uint32_t ctxAddr = IsSteamBuild() ? 0xCEDE2CU : 0x00CE8C9CU;
    uint32_t ctx = 0, pl = 0, ix = 0xFFFF, team = 0;
    if (RdPtr(ctxAddr, ctx) && ctx &&
        RdPtr(ctx + 0x28, pl) && pl &&
        RdPtr(pl + 0x20, ix) && ix < 8 &&
        RdPtr(pl + 0x10, team) && team) {
        g_localPlayerObj = pl;                  // 顺手刷新缓存
        if (!g_localPlayerId) g_localPlayerId = ix;
        return pl;
    }
    return g_localPlayerObj;                    // 链走不通 → 退回缓存（可能是旧对象）
}
}  // namespace

// 0x1F 的映射入口（CsKeyCaveStub 调用，__cdecl）。
// 只回答「这个 key 该换成哪个」，不改内存、不动句柄 —— 与 0x1D 的换句柄是两条独立路径。
uint32_t MapCsKey(uint32_t producer, uint32_t key) {
    g_csKeyCalls++;
    if (!g_flags[FID_EXTKEY] || !key) return 0;
    uint32_t enh = 0;
    for (int i = 0; i < kCsPairCount; i++) {
        if (kCsPairs[i].base == key) { enh = kCsPairs[i].enh; break; }
    }
    if (!enh) return 0;                       // 不在配对表里 → 原样放行
    g_csKeyLastKey  = key;
    g_csKeyLastProd = producer;
    // 归属：Thing+0x418 = Player*。读不到就不判断 —— 收窄模式下宁可不生效，
    // 也不要把 AI 的建造列表一起改掉（构建器同时服务 AI 的建造决策）。
    uint32_t owner = 0;
    const bool ownerOk = RdPtr(static_cast<uintptr_t>(producer) + 0x418, owner) && owner >= 0x00010000;
    g_csKeyLastOwner = ownerOk ? owner : 0;
    if (!g_flags[FID_EXTKEYAI]) {
        if (!ownerOk) { g_csKeyNoOwner++; return 0; }
        const uint32_t local = LocalPlayerFresh();
        g_csKeyLastLocal = local;
        if (!local) { g_csKeyNoOwner++; return 0; }
        if (owner != local) return 0;
    }
    g_csKeyHits++;
    g_csKeyLastEnh = enh;
    // 让引擎自己认这套列表：生产者对象的 cs key 缓存（Thing+0x464）也写成扩展版。
    //   可用性判定 0x33B0F0 就是读这个字段去 store.find 的（读到非 0 直接返回，不重算），
    //   写对它之后扩展条目天然「在列表内」，灰/亮与下命令都走引擎自己的逻辑。
    if (producer) {
        __try {
            *reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(producer) + 0x464) = enh;
            g_csKeyCached++;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_csKeyCacheFail++;
        }
    }
    FillExtTplTable();   // 首次命中时顺带把「扩展独有条目」灌进可点白名单（只做一次）
    return enh;
}

// 供 HTTP /extkey 查询（4+8 个数，顺序与文档一致）
uint32_t CsKeyDump(uint32_t* out, uint32_t max) {
    uint32_t n = 0;
    if (!out || max < 4) return 0;
    out[n++] = g_csKeyCalls;
    out[n++] = g_csKeyHits;
    out[n++] = g_csKeyNoOwner;
    out[n++] = static_cast<uint32_t>(kCsPairCount);
    if (max >= 8) {
        out[n++] = g_csKeyLastKey;
        out[n++] = g_csKeyLastEnh;
        out[n++] = g_csKeyLastProd;
        out[n++] = g_csKeyLastOwner;
    }
    if (max >= 9) out[n++] = g_csKeyLastLocal;
    if (max >= 11) {
        out[n++] = static_cast<uint32_t>(s_extTplN);    // 白名单条目数（扩展独有模板）
        out[n++] = static_cast<uint32_t>(s_extTplPair); // 实际求过差集的配对数
    }
    if (max >= 13) {
        out[n++] = g_csKeyCached;      // Thing+0x464 写成扩展版 key 的次数
        out[n++] = g_csKeyCacheFail;
    }
    return n;
}

// CanBuildProbeCaveStub 的回调：记录每个建造按钮经过「第二道门」的结果。
//   参数来自桩压栈：template(ebp) / 门返回(eax) / arg0(ebx，门的判定主体) / arg1
void CanProbeRecord(uint32_t tpl, uint32_t ret, uint32_t arg0, uint32_t arg1) {
    g_cbTemplate = tpl;
    g_cbResult   = ret;
    g_cbEbx      = arg0;
    g_cbLastArg1 = arg1;
    g_cbCount++;
    for (uint32_t i = 0; i < g_cbRingN; ++i) {
        if (g_cbRing[i][0] == tpl) {
            g_cbRing[i][1] = ret;
            g_cbRing[i][2] = arg0;
            g_cbRing[i][3]++;
            return;
        }
    }
    if (g_cbRingN < 24) {
        g_cbRing[g_cbRingN][0] = tpl;
        g_cbRing[g_cbRingN][1] = ret;
        g_cbRing[g_cbRingN][2] = arg0;
        g_cbRing[g_cbRingN][3] = 1;
        g_cbRingN++;
    }
}

// 供 HTTP /csdump?key=0x.. 查询：按名字哈希取 CommandSet，导出它的条目模板哈希。
//   out[0] = cs 句柄，out[1] = 条目数，其后为各条目模板哈希。
uint32_t CsDumpByKey(uint32_t key, uint32_t* out, uint32_t max) {
    if (!out || max < 2) return 0;
    out[0] = 0;
    out[1] = 0;
    const uint32_t cs = StoreFindCs(key);
    if (!cs) return 2;
    out[0] = cs;
    CsNode n = {};
    if (!ReadCsNode(cs, n)) return 2;
    out[1] = n.nhash;
    uint32_t k = 2;
    for (uint32_t i = 0; i < n.nhash && k < max; i++) out[k++] = n.hash[i];
    return k;
}

// 供 HTTP /canprobe 查询：每条 4 个数（哈希 / 门返回 / arg0 / 次数）
uint32_t CanProbeDump(uint32_t* out, uint32_t max) {
    uint32_t n = 0;
    if (!out) return 0;
    for (uint32_t i = 0; i < g_cbRingN && n + 4 <= max; ++i) {
        out[n++] = g_cbRing[i][0];
        out[n++] = g_cbRing[i][1];
        out[n++] = g_cbRing[i][2];
        out[n++] = g_cbRing[i][3];
    }
    return n;
}

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
    for (uint32_t i = 0; i < s_nodeN && n + 6 <= maxEntries; i++) {
        if (!s_nodes[i].builder) continue;
        out[n++] = s_nodes[i].cs;
        out[n++] = s_nodes[i].inner;
        out[n++] = (s_nodes[i].count & 0xFFFF) | (s_nodes[i].hasExt ? 0x80000000u : 0);
        out[n++] = static_cast<uint32_t>(s_nodes[i].grp);   // 建造者签名组（同族判定）
        out[n++] = s_nodes[i].innerVt;                      // inner 的 vtable（同类判定）
        out[n++] = s_nodes[i].base;                         // 按钮定义数组基址
    }
    return n;
}

// 供 Buildable/SetDisableCaveStub 用：该模板哈希是否是「扩展独有条目」。
// asm 里 push hash / call Ra3IsExtTpl / add esp,4，返回 eax=1 表示命中。
// 先查运行时白名单（24 组配对的差集，含生产建筑新增的单位），再退回静态 EXT_BLD。
extern "C" uint32_t Ra3IsExtTpl(uint32_t hash) {
    for (int i = 0; i < EXT_BLD_N; i++)
        if (hash == EXT_BLD[i]) return 1;
    for (int i = 0; i < s_extTplN; i++)
        if (hash == s_extTpl[i]) return 1;
    return 0;
}

// 诊断用：对内存里所有「建造者类但还没含扩展建筑」的 CommandSet 跑一次修正。
// 用途是验证「配对数据本身能否被选中」，不依赖建造页构建器是否跑过。
uint32_t BuildExtFixAll() {
    if (!s_fullScanned) ScanAllMemory();
    const uint32_t sn = s_nodeN;
    uint32_t changed = 0;
    for (uint32_t i = 0; i < sn; i++) {
        if (!s_nodes[i].builder || s_nodes[i].hasExt) continue;
        if (Ra3BuildExtFix(s_nodes[i].cs)) changed++;
    }
    return changed;
}

extern "C" uint32_t Ra3BuildExtFix(uint32_t cs) {
    if (!g_flags[FID_BUILDEXT]) return 0;
    // ⛔ Steam 版禁用（2026-10-26 两次实测崩：均 c0000005 + StackHash_2beb，栈已损坏）。
    //   崩点固定是「点开建造列表那一刻」，说明确实命中了 CommandSet，但把 cs+4 换成
    //   另一个对象的 inner 后，游戏按对方的 count/base 走列表时读到不兼容数据。
    //   结论：Steam 版的 CommandSet 除了 (vtable, inner) 之外还有别的耦合（缓存引用/
    //   类型标签），单纯换 inner 不成立。要做得先摸清 Steam 版 CommandSet 的完整结构，
    //   在没有安全对局可崩之前不再尝试写。
    //   扫描与演练报告（/buildext）不受影响 —— 那条路径只读不写。
    if (IsSteamBuild()) return 0;
    if (!cs || cs < 0x00010000) return 0;
    // 先查缓存 → 再扫邻域（毫秒级）→ 最后才全内存兜底（几百 ms，罕见）
    int idx = FindNode(cs);
    if (idx < 0) { ScanNear(cs); idx = FindNode(cs); }
    if (idx < 0) {
        if (!s_fullScanned) ScanAllMemory();
        idx = FindNode(cs);
    }
    if (idx < 0) return 0;

    // cs 与它的 inner 分属两个不同的堆簇，这很常见（实测 2026-10-22 战役局：
    //   战役版 cs=0x0CCC71E0，inner=0x08E21280；完整版 cs=0x08E21270 就在 inner 旁边）。
    // 只扫 cs 邻域会一路扫到 0CCC 簇里的变体，永远看不到完整版 ⇒ best 恒为空、修正失败。
    // 所以再按 inner 邻域补扫一次（毫秒级，且已收录的会去重）。
    ScanNear(s_nodes[idx].inner);
    idx = FindNode(cs);
    if (idx < 0) return 0;
    const CsNode* x = &s_nodes[idx];
    if (x->hasExt) return 0;          // 已经是完整版，不动
    if (!x->builder) return 0;        // 不是建造者类，不碰

    const CsNode* best = nullptr;
    for (uint32_t i = 0; i < s_nodeN; i++) {
        const CsNode& y = s_nodes[i];
        if (&y == x || !y.builder || !y.hasExt) continue;
        // ★ 必须同族：同一个建造者签名组（SIG_BLD 的四格特征）。
        //   ⚠ 不要用 inner+0 当"类型"判据 —— 实测它指向的是同簇的另一个对象（0x09Exxxxx 堆地址），
        //   每个对象都不同，加这条会把所有候选筛光（演练=0）。
        if (y.grp != x->grp) continue;
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

    // 演练模式：只记账，不写内存（默认走这里 —— 验证候选对不对之前不动游戏里的对象）
    if (!g_extApply) {
        g_extDryCount++;
        g_extLastCs  = cs;
        g_extLastDst = best->inner;
        return 0;
    }

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

// 单值安全读（SEH）。static 函数而不是 lambda：lambda 里放 __try 会触发 C2712。
static bool RdU32Safe(uint32_t at, uint32_t& out) {
    __try {
        out = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(at));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t LocalPlayerObj() {
    if (g_localPlayerObj) return static_cast<uintptr_t>(g_localPlayerObj);
    // hook 没抓到时用静态链兜底：ctx = [[0xCxxxx] + 0x28]，验证方式是 [player+0x20] 必须是
    // 合法玩家索引（0..7）且 [player+0x10] 是 team 指针。这条链在两局实测都指向同一个对象。
    const uint32_t ctxAddr = IsSteamBuild() ? 0xCEDE2CU : 0x00CE8C9CU;
    uint32_t ctx = 0, pl = 0, ix = 0xFFFF, team = 0;
    if (!RdU32Safe(ctxAddr, ctx) || !ctx) return 0;
    if (!RdU32Safe(ctx + 0x28, pl) || !pl) return 0;
    if (!RdU32Safe(pl + 0x20, ix) || ix >= 8) return 0;
    if (!RdU32Safe(pl + 0x10, team) || !team) return 0;
    g_localPlayerObj = pl;         // 记住，后续不必再走这条链
    if (!g_localPlayerId) g_localPlayerId = ix;
    return static_cast<uintptr_t>(pl);
}

} // namespace ra3
