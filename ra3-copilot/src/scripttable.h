#pragma once
#include <stdint.h>

// ============================================================================
// ScriptEngine 的脚本变量表（flag / timer）—— 精确访问
//
// 取代原先"全内存搜 4 字节哈希 + 一堆启发式判据"的猜法。所有偏移与语义都是
// 静态定位 + live 交叉验证得出的（见 internal/docs/战役目标-精准修改-方案.md）：
//
//   TheScriptEngine = *(void**)0x00CDD190                （VA 固定，无 ASLR）
//   timer map = engine + 0x16CC8
//   flag  map = engine + 0x16CE4                         （两张表相邻，间距 0x1C）
//
// 每张表是一个标准 _Rb_tree（STLport 风格）：
//   map + 0x04 = 内嵌哨兵节点（R@+4 / L@+8 / P@+0xC），空表时自指
//   map + 0x0C = root（= 哨兵.P）
//   map + 0x10 = 哨兵 color
//
// 节点布局（28 字节）：
//   +0x00 right      +0x04 left        +0x08 parent      +0x0C color
//   +0x10 scope      +0x14 name hash   +0x18 value
//
// 键 = {scope, nameHash}，比较规则（由 0x171370 的查找循环实测）：
//   先比 scope（无符号），再比 hash；key > node 走 right，key < node 走 left。
//
// 值语义（由 0x489C10 `cmp byte ptr [eax], 0` 实测）：
//   **只有最低字节有效**（引擎按 bool 写），高 3 字节是分配器残留。
//   表里出现 0x08107F01 / 0x94000F00 这类"怪值"就是这么来的 —— 判断真假只看
//   `value & 0xFF`，千万不要拿整个 dword 当数值。
// ============================================================================

struct ScriptNodeInfo {
    uintptr_t addr;
    uint32_t  scope;
    uint32_t  hash;
    uint32_t  value;      // 只低字节有效，见上
    bool      isFlag;
};

// engine 指针可读、且至少一张表的 root 合法
bool     ScriptTblReady();
uint32_t ScriptTblEngine();
uint32_t ScriptTblRoot(bool flagTable);        // 0 = 空表 / 读不到
uint32_t ScriptTblSentinel(bool flagTable);
// 定位结果：0=未找到。LocateHow: 0=未定位 1=已知偏移自检通过 2=自动搜索命中
uint32_t ScriptTblFlagOff();
uint32_t ScriptTblTimerOff();
int      ScriptTblLocateHow();

// 全表遍历。fn 返回 false 提前停止。返回遍历到的节点数（-1 = 表不可用）。
typedef bool (*ScriptTblEnumFn)(const ScriptNodeInfo& n, void* ctx);
int  ScriptTblEnum(bool flagTable, ScriptTblEnumFn fn, void* ctx);
int  ScriptTblCount(bool flagTable);

// 按名字哈希全表找（同名可能存在于多个作用域，这里只取第一个；scopeOut 给出它的 scope）。
// 两张表都查，isFlagOut 指出命中在哪张。
bool ScriptTblFindByHash(uint32_t hash, bool* isFlagOut, uintptr_t* nodeOut,
                         uint32_t* scopeOut);

// 按 {scope, hash} 在指定表里二分查找。
uintptr_t ScriptTblFindExact(bool flagTable, uint32_t scope, uint32_t hash);

// 读某个 flag 的原始 value（低字节为真值）。找到返回 true。
bool ScriptTblFlagGet(const char* name, uint32_t* outValue);
// 写某个 flag。只改最低字节，保留高位（与引擎行为一致）。找到返回 true。
bool ScriptTblFlagSet(const char* name, bool on);
// 在已确认的节点地址上写值（写前核对 hash，防地址失效）。
bool ScriptTblWriteValue(uintptr_t node, uint32_t expectedHash, uint32_t value);

// 表是否健康（诊断用）：root 可读、节点数合理、键有序。
struct ScriptTblHealth {
    bool     ready;
    uint32_t engine;
    uint32_t flagRoot, timerRoot;
    int      flagCount, timerCount;
    int      badParent;      // 父子互指失败的边数
    int      unsorted;       // 中序键非递增的次数
    uint32_t flags[4];       // 表里出现的作用域种类数（最多记 4 个）
    int      nScopes;
};
void ScriptTblCheck(ScriptTblHealth* out);
