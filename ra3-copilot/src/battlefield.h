// battlefield.h - 全图战场枚举（MCP「脑机接口」的感知层）
//
// 原理（2026-10-15 现场实测确定）：
//   · 游戏里每个对象 = 一个 Thing，其 vtable 统一（本构建 = 0x00C4DB60）。
//   · Thing+0x04 = 模板指针，模板+0x08 = 对象类型哈希。
//   · Thing+0x418 = 归属（owner，Team 对象；vtable 统一 0x00C5BCE0）。
//   → 扫内存找所有 Thing → 按 owner 分组 → 用 battle_catalog 过滤出"可玩对象"（单位/建筑）。
//
// 安全性：**只读**，且在 HTTP 线程执行（非游戏主线程）——不调用任何游戏函数。
//   代价：一次全扫约 2.4GB 提交内存（数百毫秒），故结果带缓存（默认 TTL 见实现）。
#pragma once
#include <cstdint>
#include "battle_catalog.h"   // B_SIDE_* / B_CLS_* / kBattleSideNames / BattleLookup

namespace ra3 {

// 单个单位种类（堆叠）
struct BattleStack {
    uint32_t    hash;
    const char* name;
    int         count;
};

// 一个归属（玩家/队伍）的战场概况
struct BattleOwner {
    uintptr_t   addr;          // owner(Team) 对象地址
    int         idx;           // owner+0x20 索引（读不到为 -1）
    bool        isLocal;       // 是否本地玩家
    int         units;         // 可玩对象中「单位」数
    int         structs;       // 可玩对象中「建筑」数
    int         playable;      // units + structs + other
    int         unknown;       // 哈希不在目录里的对象（场景物等）
    int         side;          // 多数派阵营（B_SIDE_*），无关单位则 -1
    int         topN;
    BattleStack top[10];       // 数量最多的前 10 个单位种类
};

struct BattleSummary {
    int  owners;               // 归属数
    int  localUnits, localStructs;
    int  enemyUnits, enemyStructs;
    int  totalPlayable, totalUnknown;
    int  scannedMb;            // 本次扫描的提交内存 MB
    uint32_t tickNow;          // 采样时帧号
    uint32_t elapsedMs;        // 本次扫描耗时
    bool fromCache;            // 结果是否来自缓存
    BattleOwner list[16];
};

// 单个活体对象（带地址的完整状态 —— 供 MCP「脑机接口」自动感知，无需人手点选）
struct BattleLiveObj {
    uintptr_t   thing;      // Thing（游戏对象）地址 —— 命令/攻击的 target 就用它
    uintptr_t   owner;      // 归属 Team 对象
    int         ownerIdx;   // 归属玩家索引（= owner+0x20；读不到为 -1）
    uintptr_t   ai;         // Thing+0x374 = AIUpdate 对象（命令入口 this 指针）
    uintptr_t   body;       // Thing+0x33c = 血量体对象
    uint32_t    hash;       // 类型哈希
    const char* name;       // 显示名（目录查不到为 "?"）
    int         side;       // B_SIDE_*（-1=未知）
    int         cls;        // B_CLS_*（-1=未知）
    int         cat;        // 0=单位 1=建筑 2=其他
    bool        isLocal;    // 是否本地玩家
    float       x, y, z;    // 世界坐标
    float       hp, hpMax;  // 当前/上限血量
};

// 枚举活体对象（带地址）。q = 名字子串或哈希十六进制前缀（空串=不过滤）；
// ownerIdx = -1 全部、否则按玩家索引；cls = -1 全部、否则按 B_CLS_*；cat = -1 全部。
// fresh = true 强制重扫。返回写入 out 的条数（≤ max）。
int ScanLiveObjects(BattleLiveObj* out, int max, const char* q,
                    int ownerIdx, int cls, int cat, bool fresh);

// 单个对象详情（按 Thing 地址）。地址非法/不是对象时返回 false。
bool ObjectDetail(uintptr_t thing, BattleLiveObj* out);

// 枚举全图（带缓存；fresh=true 强制重扫）。返回 false = 未进局/扫描失败（out 仍填基本信息）。
bool ScanBattlefield(BattleSummary* out, bool fresh);

// 取最近一次扫描的摘要（不触发扫描；没有则返回 false）
bool LastBattleSummary(BattleSummary* out);

// 便捷：我方 / 敌方 可玩对象数（不含缓存则触发一次扫描）
int LocalUnitCount();
int EnemyUnitCount();

// 按玩家索引（Team+0x20，见 /snapshot 的 owners[].idx）找**队伍对象地址**。
// 用缓存的全图枚举结果 —— 只读且不进渲染线程，供"给任意一方召唤"用。找不到返回 0。
uintptr_t FindTeamByIdx(int idx);

// 找一个**非本地、非中立**的归属（敌方 Team）—— 以敌方身份操作时用（如给敌方阵营召唤）。
// 找不到返回 0。
uintptr_t FindEnemyTeam();

// 清缓存（换局/切换玩家时用）
void BattlefieldInvalidate();

// 手动钉住 Thing vtable（`/snapshot?vtab=0x..`）—— 免重编热修；0 = 恢复自动推导
void SetThingVtOverride(uint32_t vt);
uint32_t ThingVtOverride();

// 后台刷新：面板请求 → HTTP 线程执行（避免在渲染线程里扫内存造成卡顿）
void BattlefieldRequestScan();
void BattlefieldPump();     // 由 HTTP 线程每轮调用（无请求则立刻返回）

// 诊断：枚举所用的 Thing vtable（运行时从选中单位推导，退化时为已知常量）
uint32_t BattleThingVtable();
const char* BattleScanNote();   // 最近一次扫描的一句话说明

} // namespace ra3
