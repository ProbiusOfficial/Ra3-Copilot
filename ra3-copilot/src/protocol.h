// protocol.h - 秘密协议（PlayerTech / Upgrade / SpecialPower）
#pragma once
#include <cstdint>
#include <cstddef>

namespace ra3 {

int  ProtocolCount();                 // = 可授予的 SpecialPower 数
const char* ProtocolId(int idx);
const char* ProtocolName(int idx);
const char* ProtocolFaction(int idx);

int  ProtocolTechCount();
const char* ProtocolTechId(int idx);
const char* ProtocolTechName(int idx);
const char* ProtocolTechFaction(int idx);

// ---- 协议技能选择表（人工中文名，来自 tools/proto_zh.tsv）----
// 与上面 439 条原始 SpecialPower 目录分开：这里只有「真正能授予的协议」。
int  ProtocolPickCount();
const char* ProtocolPickId(int idx);
const char* ProtocolPickZh(int idx);
const char* ProtocolPickFaction(int idx);
const char* ProtocolPickDesc(int idx);
// 授予门槛（实测得出，依据见 protocols_corona.h 的 kind 注释与 tools/proto_probe.py）：
//   0 = 可授予；1 = 本模组无玩家协议记录（引擎静默跳过）；2 = 与另一条共用同一协议记录。
int  ProtocolPickKind(int idx);
int  ProtoGrantPick(int idx, char* out, size_t outsz);

int  ProtocolPoints();
float ProtocolScience();
bool ProtocolSetPoints(int n);
bool ProtocolSetScience(float v);

// 开关级：0=全协议 1=快速冷却 2=禁用技能
bool ProtocolSetToggle(int which, bool on);
bool ProtocolGetToggle(int which);

// 授予：走引擎 ExecuteAction，参数用「内部名 + 真实玩家名」，失败原因写进 out
int  ProtoGrantPower(int idx, char* out, size_t outsz);
int  ProtoGrantTech(int idx, char* out, size_t outsz);
// 按内部名直接授予 Upgrade（mode=2）或 PlayerTech（mode=1），不走 Lua 脚本层。
// 返回 1=已派发、0=注册表里没这个名字、-1=不可用/异常；out 里带 hash 与模板地址，便于分辨失败原因。
int  ProtoGrantUpgrade(const char* name, int mode, char* out, size_t outsz);
// 一次性授予四个阵营的「许可等级」Upgrade（AlliedTech2..4 / CelestialTech_RANK1..3 /
// Japan{Barracks,WarFactory,NavalYard}Tech2..4）。只授当前对局，换关需重来。
// 返回成功授予的条数；out 里是「成功/命中」计数与失败明细。
int  ProtoGrantAllClearance(char* out, size_t outsz);
// 按 SpecialPower 内部名直接授予（不查字典）。who 用脚本玩家过滤器，缺省 "<This Player>"。
// 走引擎真入口 ExecuteAction(0x182C50) + PLAYER_GRANT_SPECIAL_POWER。
// 实测无阵营检查（跨阵营可授予）；who 认 <This Player> / <All Players> / 裸玩家名，可指向敌方。
// 只有「无授予对象」的技能（单位能力、被禁用的超武）会被静默跳过。
// 技能栏按钮状态：PLAYER_SPECIAL_POWER_AVAILABILITY(player, power, state)，
// state = Available / Disabled / Hidden。跨阵营注入的技能默认禁用，改 Available 即可点亮。
int  ProtoSetPowerState(const char* power, const char* state, const char* who,
                        char* out, size_t outsz);
int  ProtoGrantByName(const char* power, const char* who, char* out, size_t outsz);
// 诊断：在脚本动作表（[0xCDD190]+0x48，573 条 ×0x80，名字在 +0x0C）里按名字找下标。
// 下标就是执行器 0x5A2B40 里 switch 用的 action id —— 反汇编某个动作的实现时先查它。
int  ActionIndexByName(const char* name, char* out, size_t outsz);
const char* LocalPlayerName();

// 玩家字段探针：先 snap，让玩家在游戏里买一个协议，再 diff 找真正变化的字段
int   ProtocolUnlockIndex();
bool  ProtocolSetUnlockIndex(int n);
int   ProtocolSlotTotal();
// 已拥有协议 id 列表：SCPoint+0x18 起的 std::vector<uint32>（begin/end/cap = +0x18/+0x1C/+0x20）
int   ProtocolOwnedCount();
bool  ProtocolOwnedAt(int i, uint32_t* outId);
bool  ProtocolClaimId(uint32_t id);      // 直接塞进已购列表（面板该格立刻变「已拥有」）
bool  ProtocolClaimGrid(int grid);       // 按面板格序（0..14）取该格协议 id 并塞入
float ProtocolProgress();
bool  ProtocolSetProgress(float v);
bool PlayerFieldsSnap();
bool PlayerFieldsCalibrate();
bool PlayerDeepSnap();
int  PScanMark(uint32_t value, uint32_t* outCount);
int  PScanFilter(uint32_t want, uint32_t* outCount, char* out, size_t outsz);
void PlayerDeepDiff(char* out, size_t outsz);
// 全堆页级快照 / 差分（抓「展基地后哪个字段翻了」这类无结构变化）。三步：snap -> diff -> detail
bool HeapPageSnap(int* outPages);
void HeapPageDiff(char* out, size_t outsz, int mode);
void HeapPageDetail(char* out, size_t outsz);
void PlayerFieldsDiff(char* out, size_t outsz);
// 只读诊断：记录游戏动作执行器 0x5A2B40 的 action index / 参数
// 通用脚本动作：action 名 + 最多 3 个名字串参数，走引擎 ExecuteAction。
// 注意：只能传字符串参数（数字/表参数暂不支持）。返回 -1 表示投递失败，>=0 为引擎返回值。
int  LuaExecuteAction(const char* action, const char* a1, const char* a2, const char* a3,
                      char* out, size_t outsz, int mode = 0);
// 按 RVA 直调引擎 Lua C 函数（参数只支持名字串），用于 ObjectCreateAndFireTempWeapon 一类接口。
int  LuaCallRva(uint32_t rva, const char* a1, const char* a2, const char* a3,
                char* out, size_t outsz);
int  LuaSelfTest(const char* text, char* out, size_t outsz);
void ProtocolDiag(char* out, size_t outsz);
const char* ProtocolLastError();

} // namespace ra3
