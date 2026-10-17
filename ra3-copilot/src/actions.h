// actions.h - 对游戏对象/选中单位直接操作（地址与偏移基于 CE 修改器，见 sdk.h / symres）
#pragma once
#include <cstdint>

namespace ra3 {

// 游戏模块基址（进程内 GetModuleHandle(NULL)）
uintptr_t GameBase();

// 选中单位数量（是否处于"有选中"状态）
int SelectedCount();

// 单位操作（作用于"当前选中单位"），返回受影响单位数
int ActSpeed(int mode);         // 0=加速 1=减速 2=定住 3=恢复速度
int ActSpeedValue(float v);     // 按绝对速度值设置（0=定住，越大越快）；滑条用
float SelectedFirstSpeed();     // 首个选中单位当前的速度值（读不到返回 -1）；供 UI 显示
int ActHealth(int mode);   // 0=无限血量(写极大值) 1=残血 2=恢复血量
int LockSelectedHealth();  // 锁定血量：每帧把选中单位血量回写到上限（由 UI 的开关驱动）
int ActSetOwner();         // 收编敌方 / 复制到己方（归属改成本地玩家）
int ActKillSelected();     // 摧毁选中
int ActRank();             // 满级（调游戏 +0x35C200 单位升级）

// ---- 单位状态（表驱动；作用于选中单位）----
// 每个状态 =「若干 modifier」+「若干引擎 ObjectStatus」+ 可选的定身 / 易碎联动，
//   全部经引擎函数施加与解除；**不写 Thing+0x0CC/0x26C/0x274**（引擎槽位内部字段，每帧自查自写）。
enum {
    US_IRONCURTAIN = 0,   // 铁幕：无敌 + 铁幕外观
    US_INVULNERABLE,      // 无敌
    US_SPEEDUP,           // 加速
    US_SLOW,              // 减速
    US_BRITTLE,           // 易碎：护甲削弱 + 血量上限压制
    US_DAMAGEUP,          // 输出增强
    US_COUNT
};
// seconds <= 0 时取该状态的默认时长。返回受影响单位数。
int  ActUnitStatus(int kind, int on, int seconds);
// 重置：解除全部已知 modifier + 清全部 ObjectStatus + 行为解锁 + 速度还原 + 血量回满
int  ActStatusReset();
const char* UnitStatusName(int kind);   // UI 下拉用显示名
const char* UnitStatusTip(int kind);    // UI 悬浮说明

// 召唤：按对象类型哈希在落点生成单位（落点=首个选中单位处，否则鼠标光标处）
// owner：0=己方（本地玩家）
bool SummonByHash(uint32_t hash, int count, int owner, bool atMouse, const char* name);
// 带归属覆盖：teamOverride != 0 时用它当归属（可给敌方/任意玩家召唤），0 = 本地玩家
bool SummonByHashFor(uint32_t hash, int count, uintptr_t teamOverride, bool atMouse, const char* name);
// 最近一次召唤创建的对象（Thing 地址），返回个数（≤ max）—— 供"召唤后立刻对它操作"免去猜地址
int LastCreatedObjs(uintptr_t* out, int max);
// 指定召唤落点：下一次召唤生效一次后自动清除（跨线程安全；供 /summon?x=&y=&z= 用）
void SetSummonPosOverride(bool on, float x, float y, float z);

// ---- 落点来源（2026-10-16）----
// 一次性指定落点来源，下一单召唤生效后自动清除：0=视野中心 1=选中单位 2=光标
//   （供 /summon?place=view|selection|cursor；不给则按 UI 的「落点来源」设置）
void SetSummonPlace(int mode);
// 最近一次召唤**实际用到的**落点与来源（供 /summon 回执，调用方知道"投到哪了"）
void LastSpawnPos(float out[4], const char** src);
// 落点查询，写一行 JSON：kind 0=视野中心 1=选中单位 2=光标 3=按设置自动挑。
//   ⚠️ 会调用游戏换算函数 → 必须投递到渲染线程执行。返回写入长度。
int  SpawnPointJson(char* buf, int n, int kind);
bool SummonMCV();          // 召唤三阵营基地车各一辆
int  ActCopySelected();    // 复制选中单位（按"当前对象哈希"创建 1 个）
int  ActSummonSquad();     // 召唤预设单位 ×8（基于 CE 的 We Need Back）

// 供 tooltip / 诊断：模板库是否可用
bool TemplateStoreReady();

// 选中链表首个对象（Drawable；无选中返回 0）—— 供战场枚举反推 Thing vtable
uintptr_t FirstSelectedObject();

// ---- 创建后处理链（2026-10-15）----
// 正常建造路径在 CreateUnit 后还会调 PostA(0x84A0B0) / PostB(0x838AB0)（thiscall ecx=team）：
//   PostA：把 obj 从 team 的 pending 列表摘除 + 通知 [team+0x1278]
//   PostB：激活（内部 0x7191D0(obj,1)）+ [team+0x8c] 统计 + 收尾
// 缺这两步 → 单位没登记进玩家体系（小地图图标半透明/空心）。参数个数严格按 ret 8 / ret 0xC。
bool PostProcessCall(uintptr_t team, uintptr_t obj, bool runA, bool runB);
// 延迟登记：创建当帧调 PostA/PostB **无效**（实测），要隔一段时间再调。
//   SummonByHash 只入队；渲染线程每帧调 PostProcPump() 到期补跑。
void QueuePostProcess(uintptr_t team, uintptr_t thing);
void PostProcPump();

bool GetPostProcOn();                 // 是否让 SummonByHash 也走这条链（默认开）
void SetPostProcOn(bool on);
const char* PostProcLastError();

// ---- 召唤物隐形（2026-10-15 实机定案）----
//   默认 **关**：清掉隐形标志位（+0xC8 bit0x800 / +0x28 / +0x208 的 bit31）→
//     召唤出来就是实心、敌方会正常攻击。
//   开启：置上这三位 → 对敌方不可见（小地图空心、AI 不主动攻击）。
bool GetSummonStealth();
void SetSummonStealth(bool on);

// 选中单位按类型哈希聚合（纯内存读，跨线程安全）。返回种类数（≤ maxKinds）
int SelectedHashHistogram(uint32_t* outHash, int* outCount, int maxKinds);

// 崩溃面包屑：把一行追加写进 <DLL 目录>\ra3copilot_last.txt
// （overlay 的未处理异常处理器也用它，所以放在头里导出）
void CrashLogAppend(const char* line);

// ---- 调试：选中单位详情（供 UI「输出记录」面板显示）----
// 多行文本：对象/vtable/模板指针/模板哈希(反查名字)/body/血量/归属/坐标/状态位
int SelectedDiagText(char* buf, int n);
// 对象类型哈希 → 单位表里的显示名（找不到返回 nullptr）
const char* UnitNameByHash(uint32_t hash);

// 诊断：把召唤链路每一步的实际值写进日志（供定位失败原因）
void DiagReport();
// 诊断文本版：把同样内容拼进缓冲区（供 HTTP /diag 直接返回），返回写入长度
int  DiagText(char* buf, int n);
// 对象类型哈希 -> 模板指针（供 HTTP /resolve）
uintptr_t ResolveTemplate(uint32_t hash);
// 最近一次召唤失败的具体原因（供 UI 显示）
const char* SummonLastError();

// ---- 安全护栏 ----
bool SetDryRun(bool on);   // dry-run：只记录、不改内存（排查闪退用）
bool GetDryRun();
bool SetNoGameCall(bool on);  // 禁用"调用游戏函数"（召唤/满级）——最保险的一档
bool GetNoGameCall();
int  GuardSkips();         // 被护栏拦下的写次数
int  GuardWrites();        // 实际成功写入次数
void Ding();               // 功能触发反馈音（"叮"）

} // namespace ra3
