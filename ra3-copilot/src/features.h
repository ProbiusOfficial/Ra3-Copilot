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
extern uint32_t g_zoomCam34;       // 遥测：ZoomPostStub 采样的 [+0x34] 相机系数
extern uint32_t g_zoomPitch;       // 遥测：ZoomPostStub 采样的 [+0x74] 俯仰
extern uint32_t g_zoomObj;         // 遥测：最近一次 this 指针（esi）
extern uint32_t g_zoomVal;         // 遥测：ZoomCaveStub 进入时 [esi+0x44] 的位模式
extern uint32_t g_zoomPost;        // 遥测：clamp 之后 [esi+0x44] 的位模式
extern uint32_t g_zoomEntry;       // 遥测：缩放函数入口命中次数
extern uint32_t g_telemEnter;      // money cave 执行次数
extern uint32_t g_telemApplied;    // 加钱生效次数
// 协议格遥测：0x69A9B3 每格刷新时记录（esi / 协议对象 / 原始标志 / 强制后标志）
extern uint32_t g_paCount;
extern uint32_t g_paBuf[64 * 4];
// 协议格点击遥测：0x69AB03 进入时记录（esi / 父面板 / 协议对象 / 已拥有 / 列表begin / 列表end）
extern uint32_t g_pcCount;
extern uint32_t g_pcBuf[64 * 6];
// 技能按钮「锁定」探针：0x0A7E9AB（call 0xAD9EC0 取锁定标志）进入时记录
// 每条 3 DWORD = { this(=body+8), esi(子对象), [this+4](标志所在的容器指针) }
extern uint32_t g_lpCount;
extern uint32_t g_lpBuf[64 * 3];
// 建造按钮「禁用态」探针：0x6B7440（画禁用 tooltip 的入口，push 0x207 = 禁用原因串）
// 进入时记录 { template([btn+0x18]), thing([btn+0x10]), hits } —— 只有被引擎判禁用的按钮才会来。
extern uint32_t g_blCount;
extern uint32_t g_blBuf[128 * 3];
// 禁用原因 tooltip 构造探针：0x6C5D90（内部硬编码 push 0xC814EC = GUI:BuildabilityDisabled）
// 只在「画禁用原因」时被调用。记录 { tooltipObj(ecx), arg0, arg1, hits }
extern uint32_t g_tipCount;
extern uint32_t g_tipBuf[128 * 4];
// ClanGate 询问记录（0x43EA70 每次被 UI 调用时记下 Template 指针）：
// 用途 = 「建造页当前询问过哪些模板」的快照，战役 / 扩展模式各跑一遍即可对比列表差异。
// 每条 3 DWORD = { template 指针, 模板哈希(C++ 侧补), 命中次数 }
extern uint32_t g_clanCount;
extern uint32_t g_clanBuf[128 * 3];
// 建造页按钮对象探针（0xAB7070 入口记录 ecx = 按钮对象自身指针）：
// 用途 = 拿到真实的按钮对象地址，再反查「持有它们的数组」→ 定位建造页列表本体（CommandSet）。
// 一条 4 字节，上限 64 个（一个页签最多十来个按钮）。默认关闭，开 g_flags[0x1C] 才记录。
extern uint32_t g_bpCount;
extern uint32_t g_bpBuf[64];
// CommandSet 探针（0xACB330 内 0xACB352 处 ebx = 查表得到的 CommandSet 对象）：
// 建造页列表的真正源头 —— 循环用 vt+0 取数量、vt+0xC 取第 i 个按钮定义。
extern uint32_t g_csCount;
extern uint32_t g_csBuf[16];

// 各 hook 返回地址（C++ 侧填充）
extern uint32_t g_retMoney, g_retPlayerId, g_retPower, g_retSCPoint, g_retHaveAllSC;
extern uint32_t g_retProtoAll;
extern uint32_t g_retProtoClick;
extern uint32_t g_retLockProbe;
extern uint32_t g_retBuildLock;
extern uint32_t g_retBuildTip;
extern uint32_t g_retScience;
extern uint32_t g_retClanGate;   // 可用对象集合判定桩返回地址（0x43EA70 + 10）
extern uint32_t g_retBtnProbe;   // 建造页按钮对象探针返回地址（0xAB7070 + 5）
extern uint32_t g_retCsProbe;    // CommandSet 探针返回地址（0xACB352 + 8）
extern uint32_t g_retBtnEnable;  // 建造按钮强制可用桩返回地址（0xABB6F0 + 6）
extern uint32_t g_btnForceCount; // 0x1E 命中扩展建筑模板的累计次数（演练也记）
extern uint32_t g_btnForceReal;  // 0 = 只计数（安全）；1 = 真调 setEnabled
extern uint32_t g_btnLastObj;    // 最近命中的按钮对象指针（只读取证）
extern uint32_t g_btnLastHash;   // 最近命中的模板哈希（只读取证）
extern uint32_t g_setDisRet;     // setDisabled 入口的调用者返回地址（取证：灰是谁下的令）
extern uint32_t g_setDisArg0;    // setDisabled 入口的第一个栈参数（取证）
extern uint32_t g_cbResult;      // 第二道门返回值（0 = 不置灰）
extern uint32_t g_cbTemplate;    // 第二道门看到的 Template
extern uint32_t g_cbEbx;         // 第二道门的第一个参数
extern uint32_t g_cbEsi;         // 第二道门看到的对象（含 +0x470/+0x471 标志字节）
extern uint32_t g_cbCount;       // 探针命中数
extern uint32_t g_cbTargetHash;  // 只记录该模板哈希
extern uint32_t g_cbLastArg1;    // 最近一次门的第二个参数（Template）
extern uint32_t g_cbRingN;       // 取证表已用条数（每格 4 数）
extern uint32_t g_cbRing[24][4]; // 取证表：{模板哈希, 门返回, arg0, 次数}
extern uint32_t g_canProbeFn;    // 门探针回调（stub 调它）
extern uint32_t g_cbProbeOn;     // 门探针开关（1=记录，0=完全旁路）
extern uint32_t g_builderLastCs; // 建造页构建器实际拿到的 CommandSet 句柄（只读取证）
extern uint32_t g_csKeyPtr;      // CsKeyCaveStub 的 &key（桩内部用）
extern uint32_t g_builderRingN;  // 构建器最近几次调用的计数
extern uint32_t g_csKeyCached;   // 把 Thing+0x464 写成扩展版 key 的成功次数
extern uint32_t g_csKeyCacheFail;// 写缓存失败的次数
extern uint32_t g_builderRing[8][2];  // 最近 8 次 {cs, 条目数}
void CanProbeRecord(uint32_t tpl, uint32_t ret, uint32_t arg0, uint32_t arg1);
uint32_t CanProbeDump(uint32_t* out, uint32_t max);
uint32_t CsDumpByKey(uint32_t key, uint32_t* out, uint32_t max);
extern uint32_t g_retBuildability; // Buildability hook 跳回点（站 + 8）
extern uint32_t g_retSetDis;       // setDisabled hook 跳回点（站 + 5）
extern uint32_t g_extCallCount;    // BuildExtCs hook 被调用次数
extern uint32_t g_extFixCount;   // 建造页补全扩展建筑：生效次数（真写）
extern uint32_t g_extDryCount;   // 演练：本会替换但未写的次数
extern bool     g_extApply;      // true = 真写内存（默认 false，先演练）
extern uint32_t g_extLastCs;     // 最近一次被修正的 CommandSet（0 = 从未）
extern uint32_t g_extLastDst;    // 换到的 inner（= 扩展完整版）
extern uint32_t g_extScanMs;     // 全内存扫描耗时（ms）
extern uint32_t g_retCsKey;      // cs key hook 跳回点（站 + 9）
extern uint32_t g_mapCsKeyFn;    // key 映射函数指针（stub 调它）
extern uint32_t g_csStoreSlot;   // CommandSet 注册表全局槽地址（两构建不同）
extern uint32_t g_csKeyCalls;    // key hook 进入次数
extern uint32_t g_csKeyHits;     // 真正换成 Enhanced key 的次数
extern uint32_t g_csKeyNoOwner;  // 收窄模式下读不到归属而放弃的次数
extern uint32_t g_csKeyLastKey;  // 最近一次的原 key
extern uint32_t g_csKeyLastEnh;  // 最近一次换成的 Enhanced key
extern uint32_t g_csKeyLastProd; // 最近一次的生产者对象
extern uint32_t g_csKeyLastOwner;// 最近一次读到的归属（Player 对象）
extern uint32_t g_scPointObj, g_scPointVal;
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
void ProtoAllCaveStub();
void ProtoClickCaveStub();
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
void LockProbeCaveStub();   // 技能按钮锁定探针（只记录）
void BuildLockCaveStub();   // 建造按钮禁用态探针（只记录）
void BuildTipCaveStub();    // 禁用原因 tooltip 构造探针（只记录）
void ScienceCaveStub();     // 建造前置科技判定桩（g_flags[0x1A] 打开时恒真）
void ClanGateCaveStub();    // 可用对象集合判定桩（g_flags[0x1B] 打开时对本机玩家恒「可用」）
void BuildBtnProbeCaveStub(); // 建造页按钮对象探针（g_flags[0x1C] 打开时记录 ecx）
void CommandSetProbeCaveStub(); // CommandSet 探针（g_flags[0x1C] 打开时记录 ebx）
void SetDisCaveStub();          // 0x1E：setDisabled 入口对扩展建筑改走 setEnabled
void BuildableCaveStub();       // 0x1E：Buildability 层对扩展建筑放行（不碰按钮对象）
void BtnEnableCaveStub();       // 0x1E 旧实现（已停用，站点=0）       // 建造按钮强制可用（g_flags[0x1E] 打开时对扩展建筑强制 setEnabled）
void BuildExtCsStub();          // 建造页构建器：把战役版 CommandSet 句柄换成同族完整版（不改对象，只换句柄）
void CsKeyCaveStub();           // 建造页构建器：查表前把 cs key 换成同名 Enhanced 版（0x1F）
void CanBuildProbeCaveStub();   // 建造按钮第二道门取证（只读，Steam 专用）
}

// 战役建造页补全扩展建筑：由 CommandSetProbeCaveStub 在每次取到建造者 CommandSet 时调用。
// 若该 cs 缺扩展建筑（胡杨防空 / 礁石岸防），把它指向「扩展完整版」的 inner。
// 实现在 features.cpp；同一局内只扫一次内存，之后查缓存。返回新 inner（0 = 未改）。
extern "C" uint32_t Ra3BuildExtFix(uint32_t cs);
// 供 HTTP /buildext 查询用：导出扫描统计与建造者类 CommandSet 摘要。
uint32_t BuildExtDump(uint32_t* out, uint32_t maxEntries);
// cs key 映射（0x1F）：producer = 建造者对象，key = 原 CommandSet 名字哈希。
// 返回换过的 Enhanced key；0 = 不改（功能关 / 不在配对表 / 收窄模式下不属于本机玩家）。
uint32_t MapCsKey(uint32_t producer, uint32_t key);
// 供 HTTP /extkey 查询：导出 0x1F 的计数器与最近一次映射（顺序见实现）。
uint32_t CsKeyDump(uint32_t* out, uint32_t max);
uint32_t ReplaceBuildExtCs(uint32_t cs);   // 建造页构建期的句柄替换（只换句柄，不改对象）
// 诊断：对所有「建造者类但缺扩展建筑」的 CommandSet 跑一次修正，返回改了几个。
// 不依赖建造页构建器时机，只用于验证配对数据能否被选中（GET /buildext?fixall=1）。
uint32_t BuildExtFixAll();

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
    FID_PROTOALL  = 0x18,   // 协议格全解锁（跳过前置条件检查）
    FID_BUILDFORCE = 0x19,  // 建造按钮强制可用（把 vt+0x44 的 reason==0 改写成 1）
    FID_SCIALL    = 0x1A,   // 忽略建造前置科技（0x4451F0 恒真 → 解锁战役科技锁）
    FID_CLANGATE  = 0x1B,   // 可用对象集合判定（0x43EA70 对本机玩家返回「可用」→ 解锁建造页黑锁）
    FID_BTNPROBE  = 0x1C,   // 探针：记录建造页按钮对象地址（只在找列表结构时临时开）
    FID_BUILDEXT  = 0x1D,   // 战役建造页补全扩展建筑：构建时把战役版 CommandSet 换成扩展完整版
    FID_BTNENABLE = 0x1E,   // 扩展建筑按钮强制可用（0xABB6F0 直接走 setEnabled，绕过前置/范围判定）
    FID_EXTKEY    = 0x1F,   // 扩展模式列表：查表前把 cs key 换成 Enhanced key（覆盖 24 组，含生产建筑）
    FID_EXTKEYAI  = 0x20,   // 0x1F 的范围：0 = 只对本机玩家，1 = 全场（含 AI）
    FID_COUNT     = 32,
};

void SetFeature(int idx, bool on);
bool GetFeature(int idx);
void TriggerOneShot(int idx);   // 置 1（cave 内消费后自动清零）

// 免费建造：把 base+0x0E7565 的 `75 0C`(jne) 改成 `EB 0C`(jmp) → 建造检测恒真
// （基于 CE；比"快速建造"安全，不会破坏建造队列）
bool SetFreeBuild(bool on);
bool GetFreeBuild();

// 协议技能「需要该阵营基地」解锁：0x83B740（遍历 Player+0x1260 建筑列表的谓词）恒真。
// 2 字节补丁，幂等，字节不符时不改动。
bool SetProtoUnlock(bool on);
bool GetProtoUnlock();

// 血条常显：两处「该不该画血条」判定的 `jne`(75) 改成 `je`(74)，各 1 字节。
// 两处是同一判定的两份拷贝，必须同时改，缺一处会只剩部分单位显示。
bool SetHealthBarAlways(bool on);
bool GetHealthBarAlways();

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
