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

void ObjTableInit();                        // 幂等；解析内置表
int  ObjTableMapCount();
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

// 脚本 flag / timer 的键哈希：FNV-1（先乘 16777619 再异或字节）
uint32_t ObjNameHash(const char* name);
