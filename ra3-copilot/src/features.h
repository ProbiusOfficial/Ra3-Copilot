// features.h - 功能核：多 hook（表驱动）+ 资金操作接口
#pragma once
#include <cstdint>

namespace ra3 {

// ---- stub 引用的全局量（stubs.asm EXTERNDEF）----
extern "C" {
extern uint32_t g_localPlayerId;   // 本地玩家 ID（整数）
extern uint32_t g_localPlayerObj;  // 本地"队伍/玩家"对象（PlayerID hook 写入，供召唤当 team）
extern uint32_t g_moneyObj;        // 加钱 hook 看到的"资金对象"（UI 读资金用，勿与上者混用）
extern uint32_t g_selUnit;         // 弹药 hook 遍历到的"当前选中单位"对象
extern uint32_t g_selHits;         // 选中单位命中计数
extern uint32_t g_selTick;         // 最近命中时标（帧号）
extern uint32_t g_tickNow;         // 帧计数（每帧 ++）
extern uint32_t g_okUnit;          // 秒杀：当前单位
extern uint32_t g_okBody;          // 秒杀：当前单位血量体
extern int32_t  g_moneyOp;         // 0=无 1=加钱 2=设置（一次性）
extern int32_t  g_moneyValue;      // 金额
extern uint8_t  g_flags[32];       // 功能开关数组（索引对齐 CE 的 FLAGS 偏移）
extern uint32_t g_zoomHits;        // 遥测：ZoomCaveStub 进入次数
extern float    g_zoomMax;         // 已废用：ZF 机制不写任何上限字段（保留占位，待清理）
extern float    g_zoomMaxOff;      // 已废用：同上
extern uint32_t g_zoomCam34;       // 遥测：ZoomPostStub 采样的 [+0x34] 相机系数
extern uint32_t g_zoomPitch;       // 遥测：ZoomPostStub 采样的 [+0x74] 俯仰
extern uint32_t g_zoomObj;         // 遥测：最近一次 this 指针（esi）
extern uint32_t g_zoomVal;         // 遥测：ZoomCaveStub 进入时 [esi+0x44] 的位模式
extern uint32_t g_zoomPost;        // 遥测：clamp 之后 [esi+0x44] 的位模式
extern uint8_t  g_zoomState;       // 已废用：旧护栏语义，现无写入方（保留占位，待清理）
extern uint32_t g_zoomEntry;       // 遥测：缩放函数入口命中次数
extern uint32_t g_telemEnter;      // money cave 执行次数
extern uint32_t g_telemApplied;    // 加钱生效次数

// 各 hook 返回地址（C++ 侧填充）
extern uint32_t g_retMoney, g_retPlayerId, g_retPower, g_retSCPoint, g_retHaveAllSC;
extern uint32_t g_retFastBuild1, g_retFastBuild2, g_retFastBuild3, g_retMap;
extern uint32_t g_retUnitAmmo, g_retDangerLevel, g_retOreMine, g_retGodMode;
extern uint32_t g_retEnemyCantBuild, g_retSuperPower, g_retDisableAllSP;
extern uint32_t g_retZoom, g_retOneKill, g_retOKData1, g_retOKData2;
extern uint32_t g_retZoomPost;
extern uint32_t g_retZoomEntry;
}

// stub 入口
extern "C" {
void PlayerIdCaveStub();
void MoneyCaveStub();
void PowerCaveStub();
void SCPointCaveStub();
void HaveAllSCCaveStub();
void FastBuild1CaveStub();
void FastBuild2CaveStub();
void FastBuild3CaveStub();
void MapCaveStub();
void UnitAmmoCaveStub();
void DangerLevelCaveStub();
void OreMineCaveStub();
void GodModeCaveStub();
void EnemyCantBuildCaveStub();
void SuperPowerCaveStub();
void DisableAllSPCaveStub();
void ZoomEntryStub();
void ZoomCaveStub();
void ZoomPostStub();
void OneKillCaveStub();
void OKData1CaveStub();
void OKData2CaveStub();
}

// ---- 功能开关索引（对齐 CE 的 FLAGS 偏移）----
enum FeatureIdx {
    FID_ZOOM_IN   = 0x0F,    // 视野放大（持续）
    FID_ZOOM_OUT  = 0x10,    // 视野缩小（持续）
    FID_POWER     = 8,
    FID_SCPOINT   = 9,
    FID_HAVEALLSC = 10,
    FID_FASTBUILD = 12,
    FID_SUPERPOWER = 0x0D,   // 己方超武就绪
    FID_DISABLESP  = 0x0E,   // 禁用（敌方）超武
    FID_MAP       = 0x11,
    FID_UNITAMMO  = 0x12,
    FID_DANGER    = 0x13,   // 1=最高 2=最低 0=不干预
    FID_OREMINE   = 0x14,   // 一次性
    FID_ENEMYBUILD = 0x15,  // 0=压5000 1=压1
    FID_GODMODE   = 0x16,
    FID_KILLMODE  = 0x17,   // 秒杀模式（敌方单位 HP 归零）
    FID_COUNT     = 32,
};

void SetFeature(int idx, bool on);
bool GetFeature(int idx);
void TriggerOneShot(int idx);   // 置 1（cave 内消费后自动清零）

// 免费建造：把 base+0x0E7565 的 `75 0C`(jne) 改成 `EB 0C`(jmp) → 建造检测恒真
// （基于 CE；比"快速建造"安全，不会破坏建造队列）
bool SetFreeBuild(bool on);
bool GetFreeBuild();

// ---- 遥测（overlay 侧）----
extern uint32_t g_telemImguiReady;
extern uint32_t g_telemRenderOnce;
extern uint32_t g_telemWndFound;
extern uint32_t g_telemD3dHooked;

// 安装结果
struct HookStatus {
    int  total = 0;
    int  installed = 0;
    int  aobOk = 0;
    uintptr_t moduleBase = 0;
    size_t moduleSize = 0;
    char failed[160] = { 0 };   // 未通过 AOB 校验的 hook 名（fail-closed 记录）
};

HookStatus InstallHooks();

// ---- 卸载（热重载）：把已装功能 hook 回写原字节并逐站回读校验 ----
// 返回成功回写数；调用方须与 InstallHooks 的 installed 相等才能继续卸载（不相等=现场异常，保命中止）
int RemoveHooks();

// ---- 资金接口 ----
void MoneyAdd(int32_t delta);
void MoneySet(int32_t value);
int32_t LocalMoney();
int  LocalPlayerId();
bool LocalPlayerReady();
uintptr_t LocalPlayerObj();

} // namespace ra3
