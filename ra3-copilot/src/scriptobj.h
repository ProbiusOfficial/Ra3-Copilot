#pragma once
#include <stdint.h>

// ---- 战役目标表 ----
// 数据来自 src/generated/objectives_data.h（由 research/tools/gen_objective_table.py 生成）。
// 每张图的目标来自脚本动作 SET_MISSION_OBJECTIVE_STATUS(目标名, 2)，其所在脚本的每个
// OrCondition 分组 = 一条完成路径；组内 FLAG / TIMER_EXPIRED 条件 = 可直接拨动的依赖。
//
// 依赖类型：
//   'F' = FLAG，置 1 生效（写节点 value 的 bit0）
//   'N' = 取反 FLAG，置 0 生效
//   'T' = TIMER，写 0xFFFFFFFF 表示"已过期"
// otherConds > 0 表示该路径还含无法用 flag 满足的条件（NAMED_DESTROYED 等），
// 拨了也未必完成，UI 需提示。

struct ObjDepInfo {
    char kind;            // 'F' / 'N' / 'T'
    const char* name;
};

struct ObjPathInfo {
    int objIndex;         // 所属目标在 map 内的下标
    int otherConds;
    int nDeps;
    ObjDepInfo deps[12];
};

struct ObjItemInfo {
    const char* name;
    int firstPath;
    int pathCount;
};

void ObjTableInit();                        // 幂等；解析当前来源（内置表或关卡脚本）
int  ObjTableMapCount();

// 用当前关卡的 .map 现场生成目标表（覆盖 mod 图 / 未导出的版本）。
// 读盘 + 解压 + 解析约几十毫秒，只能在 HTTP / 后台线程调；失败时保持原表。
bool ObjTableUseScript();
int  ObjTableSourceKind();                  // 0=内置表 1=关卡脚本
const char* ObjTableSourceNote();           // 来源摘要或失败原因
const char* ObjTableMapName(int m);
int  ObjTableMapObjCount(int m);
const ObjItemInfo* ObjTableMapObj(int m, int i);
const ObjPathInfo* ObjTablePath(int pathIdx);

// ---- 运行时节点扫描 ----
// 一次全内存扫描，收集"本表关心的所有依赖 hash"对应的脚本节点（flag/timer 表条目）。
// 返回命中个数。之后用 ScriptNodeValue 查询。
int  ScriptNodeScan();
bool ScriptNodeValue(uint32_t hash, uint32_t* outValue);
bool ScriptNodeAddr(uint32_t hash, uintptr_t* outNode);
// 同 ScriptNodeAddr，但会核对缓存地址里 +0x14 是否仍是该 hash。
// 缓存可能指向已被释放的旧节点（换关/脚本重载），写入前必须走这个。
bool ScriptNodeAddrLive(uint32_t hash, uintptr_t* outNode);

// 叫停正在进行的扫描（DLL 卸载前必须调，否则线程会跑在已卸载的代码上）。
void ScriptNodeScanAbort();
// 重新读一遍已收集节点的 value（写完 flag 后刷新显示用）。
void ScriptNodeRefresh();

// 自动识别当前关卡：返回 map 下标（无命中返回 -1），scoreOut 给出命中得分。
int  ObjDetectMap(int* scoreOut);

// 取第 pathIdx 条路径需要的写操作列表（hash + 期望值 + 模式）。
// mode: 0=直接写 value，1=置 bit0，2=清 bit0
// 返回条数（<= maxN）。
int  ObjPathWrites(int pathIdx, uint32_t* outHash, uint32_t* outValue,
                   int* outMode, int maxN);

// 某个依赖当前是否已满足：1=满足 0=未满足 -1=节点不存在（等于未满足）
int  ObjDepState(const char* name, char kind);

// ---- 诊断：列出某个 hash 在内存里的**全部**候选（不做"每 hash 只留一个"的去重） ----
// 用途：判断「节点不存在 / 写了不生效」是脚本没写过，还是节点识别挑错了对象。
struct ScriptNodeCand {
    uintptr_t node;
    uint32_t  L, P, R;      // 树指针
    uint32_t  scope;        // +0x10 作用域哈希（真节点只有少数几种取值）
    uint32_t  hash;         // +0x14 名字哈希
    uint32_t  value;        // +0x18 值
    uintptr_t root;         // 沿 P 上溯到的根（P 非法 / 成环时给 0）
    int       depth;        // 上溯步数
};

// 单次全内存扫描，收集 hashes[] 每个 hash 最多 keepPerHash 个候选。
// countOut[i] = 该 hash 命中的候选总数（可能大于实际存下的条数）。
// 返回实际存下的候选数。只读。
int ScriptNodeCandScan(const uint32_t* hashes, int n, int* countOut,
                       ScriptNodeCand* out, int keepPerHash, int maxTotal);

// 脚本 flag / timer 的键哈希：FNV-1（先乘 16777619 再异或字节）
uint32_t ObjNameHash(const char* name);

// 该哈希是否是目标表里的依赖名（节点校验用：真节点的 scope/value 不可能是 flag 名哈希）
bool ObjIsDepHash(uint32_t h);

// 线程栈范围快照（扫描前调一次）：哈希值本身就在栈上，"找 hash"必然命中参数变量自己。
void ScriptCollectStacks();
bool ScriptIsThreadStack(uintptr_t addr);
