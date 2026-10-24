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

// ---- 无限弹药（真弹药字段，2026-10-20 实测定案）----
// 结构（RA3 1.12.3444.25830，第三方 ra3-base 偏移 + 本机实测双向确认）：
//   Thing + 0x37C        -> 武器模块（模块对象：+0x00 vtable / +0x04 ModuleData / +0x08 宿主 Thing）
//   模块 + 0x24 / + 0x28 -> 武器槽指针数组 begin / end（每项 4 字节）
//   槽   + 0xA4 (byte)   -> 该槽是否激活
//   槽   + 0x94          -> 武器条目指针数组（固定 6 项）
//   条目 + 0x1C (int)    -> 当前弹药：实测 3→2→1→0 后换弹回 3
//   条目 + 0x08          -> 所属槽（回指校验用）
// 写 0x7FFFFFFF 后单位持续开火、永不换弹（实测每发 -1、无崩溃）。
// ⚠ 旧实现写 [Thing+0x1C]（那是 float 1.0），与弹药无关，且会写坏非武器对象。
// 每帧由 UI 在开关打开时调用；返回本次真正写入的条目数。
int AmmoFillSelected();
// ---- 「赋予」语义（2026-10-20 探宝要求）----
//   打开开关时把**当前选中**登记为常驻目标；之后即使取消选中，也一直给它补弹。
//   目标靠 (Thing, 模板哈希) 双重识别：地址被回收给别的单位时哈希对不上 → 自动剔除。
int  AmmoAddSelected();     // 把当前选中登记进常驻列表（幂等），返回新增个数
int  AmmoAddThing(uintptr_t thing);  // 直接登记单个 Thing（幂等），1=新增 0=已存在/不合格
int  AmmoTick();            // 维护常驻列表：补弹 + 剔除失效目标，返回仍存活的目标数
void AmmoClearTargets();    // 清空常驻列表（单位未淘汰时无需调用）
int  AmmoTargetCount();     // 常驻目标个数（UI 显示用）
// 单个 Thing（内部含全套指针/结构校验）；entryOut 可空，返回写入条目数，-1=对象不合格
int AmmoFill(uintptr_t thing, uintptr_t* entryOut, int maxOut);
// 只读：读出该 Thing 的弹药条目数与当前值（诊断/自测用），返回条目数
int AmmoQuery(uintptr_t thing, uintptr_t* entryOut, uint32_t* ammoOut, int maxOut);

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
// 海面折跃信标的兵种过滤（一次性，下一单生效后清除）：naval / air / amph；空或 "all" = 不过滤
void SetSeaBeaconKind(const char* kind);
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
// ---- 箱子即爆（DLL 原生）----
// 一次命令回调内完成「按哈希创建箱子 → 下一帧校验 + KillUnit(6,1,0)」。
//   击杀参数取拾取链原生形态，与游戏自己拾取箱子同一条链；
//   ⚠️ 不能同帧杀：CreateUnit 当帧对象未初始化完（Thing+4 读回 0），同帧 KillUnit 不消耗箱子、
//     也不触发死亡开火（2026-10-18 实测）——所以最少隔一帧发力。
//   delayMs > 0 时再额外等到该毫秒数，仅供对照实验。
// 返回 1=已创建并入队（引爆在下一帧）；0=创建/护栏失败（原因见 SummonLastError / BurstLastError）。
int ActBurstCrate(uint32_t hash, int delayMs);
uintptr_t BurstLastObj();          // 最近一次 /burst 创建的对象（Thing 地址），失败为 0
const char* BurstLastError();      // 最近一次失败原因（成功时为 "ok"）
void BurstPump();                  // 引爆队列推进（渲染线程每帧调用）

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

// ---- 建筑落点可建造性（直调游戏判定函数）----
// 返回 LegalBuildCode：0 = 可建；-1 = 前置检查失败（看 last error）；-99 = 调用抛异常
int BuildableAt(float x, float y, float z, uint32_t tmplHash, float angle, uint32_t options);
const char* BuildableLastError();

// ---- 自动建筑摆放（A 路线：读样本 + 几何打分，引擎判定只做最终校验）----
struct AutoPlaceCand {
    float x, y, z;      // 世界坐标（z = 邻近建筑反距离加权插值）
    float score;        // 综合得分 0..1
    float dist;         // 到锚点（我方建筑质心）的平面距离
    float gap;          // 到最近已有建筑的地面间距
    float angleGap;     // 到最近同类建筑的方位角差（度）
    char  note[56];     // 依据摘要
};

// 生成候选点（按得分降序，彼此互不重叠）。纯几何 —— 只用 battlefield 缓存，不调游戏函数，
// HTTP 线程可安全调用。返回条数；0 = 无候选（原因写入 diag）。
int AutoPlaceCandidates(uint32_t tmplHash, AutoPlaceCand* out, int max, char* diag, int cap);

// 从候选里挑第一个通过引擎判定的点。⚠️ 内部调 BuildableAt → 只能渲染线程。
// 返回 1 = 引擎认可；0 = 全部未通过（仍写出几何最优，diag 说明）；-1 = 无候选。
int AutoPlacePick(const AutoPlaceCand* cands, int n, uint32_t tmplHash,
                  float angle, float out[3], char* diag, int cap);

// 建筑占地半径估算（米）。按模板名关键词查表，查不到用默认值。
float BuildingFootprint(uint32_t hash);

// 从单位表里按兵种随机取一个模板哈希。pool: naval|vehicle|infantry|air|structure|any
// 用途：`/summon?pool=naval` —— 等价于「地面折跃信标」的投下即出，但兵种可控。
// 返回 0 = 该兵种没有可召唤条目。
uint32_t RandomUnitHash(const char* pool);

// 最近一次召唤失败的具体原因（供 UI 显示）
const char* SummonLastError();

// ---- 战役脚本 flag ----
// 按名字设置 ScriptEngine 的脚本 flag / timer（如 "f_debug_skip_objective_01"）。
// 键 = 8 字节 {作用域哈希, FNV-1(名字)} —— 作用域有 4 种取值，**不恒为 0x811C9DC5**。
// 定位方式：搜 4 字节 FNV-1(名字)，候选节点 = 命中地址 - 0x14，再做指针字段校验；
// value 在节点 +0x18，只改 bit0。
//
// 分两段是为了线程安全：Resolve 只读扫描（慢，放 HTTP 线程，不卡渲染），
// Write 只改 4 字节（快，必须放渲染线程）。
uintptr_t ScriptFlagResolve(const char* name);   // 返回节点地址，0 = 没找到
int ScriptFlagWrite(uintptr_t node, int on);     // 1=已写 -1=异常
int ActScriptFlag(const char* name, int on);     // 便捷版（两段一起跑，仅供渲染线程内使用）
// 批量 Resolve：一次内存扫描同时定位多个 flag（逐个调会各扫一遍全内存）。
// nodes 与 names 对齐，0 = 没找到；返回命中的个数。同样只读，可放工作线程。
int ScriptFlagResolveBatch(const char* const* names, int n, uintptr_t* nodes);
const char* ScriptFlagLastError();

// 按"值"写脚本节点（flag 与 timer 通用）：
//   mode 0 = 直接写 value（timer 写 0xFFFFFFFF 表示已过期）
//   mode 1 = 置 bit0（flag 置真）
//   mode 2 = 清 bit0（flag 置假）
int ScriptNodeWrite(uintptr_t node, uint32_t value, int mode);

// ---- 安全护栏 ----
bool SetDryRun(bool on);   // dry-run：只记录、不改内存（排查闪退用）
bool GetDryRun();
bool SetNoGameCall(bool on);  // 禁用"调用游戏函数"（召唤/满级）——最保险的一档
bool GetNoGameCall();
int  GuardSkips();         // 被护栏拦下的写次数
int  GuardWrites();        // 实际成功写入次数
void Ding();               // 功能触发反馈音（"叮"）

} // namespace ra3
