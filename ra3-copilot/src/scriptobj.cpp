// 战役目标表：解析内置表 + 运行时脚本节点扫描。
//
// 表格式（见 generated/objectives_data.h）：
//   "M|<map 名>"
//   "O|<目标名>|<other 条件数>|<依赖1>|<依赖2>|..."   同一目标多行 = 多条完成路径
// 依赖前缀：F=FLAG(置1)  N=取反FLAG(置0)  T=TIMER(写 0=已过期)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include "scriptobj.h"
#include "scripttable.h"
#include "buildinfo.h"
#include "generated/objectives_data.h"
#include "maptbl.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ---- 解析结果 ----
#define OBJ_MAX_MAP   32
#define OBJ_MAX_PATH  256
#define OBJ_MAX_ITEM  256
#define OBJ_MAX_POOL  (48 * 1024)

static char   g_pool[OBJ_MAX_POOL];
static char*  g_poolCur = g_pool;

static const char* g_mapName[OBJ_MAX_MAP];
static int g_mapPathFirst[OBJ_MAX_MAP];
static int g_mapPathCount[OBJ_MAX_MAP];
static int g_mapObjFirst[OBJ_MAX_MAP];
static int g_mapObjCount[OBJ_MAX_MAP];
static int g_mapCount = 0;

static ObjPathInfo g_paths[OBJ_MAX_PATH];
static int g_pathCount = 0;

static ObjItemInfo g_objs[OBJ_MAX_ITEM];
static int g_objCount = 0;

static int g_inited = 0;

static uint32_t Fnv1(const char* s) {
    uint32_t h = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p)
        h = (h * 16777619u) ^ (uint32_t)*p;
    return h;
}

// 把串复制进池子（返回池内指针，便于原地切分而不动只读常量）
static char* PoolDup(const char* s, int len) {
    if (g_poolCur + len + 1 >= g_pool + OBJ_MAX_POOL) return nullptr;
    char* d = g_poolCur;
    memcpy(d, s, (size_t)len);
    d[len] = 0;
    g_poolCur += len + 1;
    return d;
}

// 原地按 '|' 切分，返回段数；第一段写回 s 首。
static int SplitPipe(char* s, char* seg[], int maxSeg) {
    int n = 0;
    seg[n++] = s;
    for (char* p = s; *p && n < maxSeg; ++p) {
        if (*p == '|') { *p = 0; seg[n++] = p + 1; }
    }
    return n;
}

// ---- 表来源 --------------------------------------------------------------
// 内置表只覆盖导出过的那批战役图；进关后用关卡 .map 现场解析，覆盖 mod 图与
// 其他版本。两者行格式相同，消费端不区分。
static const char* const* g_srcLines = kObjTableData;
static int  g_srcKind = 0;            // 0=内置 1=脚本解析
static char g_srcMap[128];            // 脚本表对应的地图名
static char g_srcNote[220];           // 来源说明 / 失败原因

static void ObjTableParse(const char* const* lines) {
    int curMap = -1;
    for (int i = 0; lines[i]; ++i) {
        const char* line = lines[i];
        const int len = (int)strlen(line);
        if (len < 2) continue;
        char* buf = PoolDup(line, len);
        if (!buf) break;
        char* seg[16];
        const int ns = SplitPipe(buf, seg, 16);
        if (seg[0][0] == 'M' && seg[0][1] == 0) {
            if (g_mapCount >= OBJ_MAX_MAP) break;
            curMap = g_mapCount++;
            g_mapName[curMap] = seg[1] ? seg[1] : "";
            g_mapPathFirst[curMap] = g_pathCount;
            g_mapPathCount[curMap] = 0;
            g_mapObjFirst[curMap] = g_objCount;
            g_mapObjCount[curMap] = 0;
            continue;
        }
        if (seg[0][0] != 'O' || seg[0][1] != 0) continue;
        if (curMap < 0 || g_pathCount >= OBJ_MAX_PATH) continue;
        const char* objName = seg[1] ? seg[1] : "";
        const int other = (seg[2] && seg[2][0]) ? atoi(seg[2]) : 0;

        // 同一目标的连续 O 行归到一个 ObjItemInfo
        int objIdx = g_mapObjCount[curMap] - 1;
        if (objIdx < 0 || strcmp(g_objs[g_mapObjFirst[curMap] + objIdx].name, objName) != 0) {
            if (g_objCount >= OBJ_MAX_ITEM) continue;
            objIdx = g_mapObjCount[curMap]++;
            if (g_mapObjFirst[curMap] + objIdx >= OBJ_MAX_ITEM) continue;
            ObjItemInfo& it = g_objs[g_mapObjFirst[curMap] + objIdx];
            it.name = objName;
            it.firstPath = g_pathCount;
            it.pathCount = 0;
            ++g_objCount;      // 必须累加：下一张图的 objFirst 从这里开始
        }
        ObjPathInfo& pi = g_paths[g_pathCount];
        pi.objIndex = objIdx;
        pi.otherConds = other;
        pi.nDeps = 0;
        for (int d = 3; d < ns && pi.nDeps < 12; ++d) {
            const char* dep = seg[d];
            if (!dep || !dep[0]) continue;
            if (dep[0] == 'X' && dep[1] == 0) break;   // X 之后是"其他条件"原文，不是依赖
            const char k = dep[0];
            if (k != 'F' && k != 'N' && k != 'T') continue;
            pi.deps[pi.nDeps].kind = k;
            pi.deps[pi.nDeps].name = dep + 1;
            ++pi.nDeps;
        }
        ++g_pathCount;
        ++g_mapPathCount[curMap];
        ++g_objs[g_mapObjFirst[curMap] + objIdx].pathCount;
    }
}

void ObjTableInit() {
    if (g_inited) return;
    g_inited = 1;
    ObjTableParse(g_srcLines);
}

int ObjTableMapCount() { ObjTableInit(); return g_mapCount; }

const char* ObjTableMapName(int m) {
    ObjTableInit();
    return (m >= 0 && m < g_mapCount) ? g_mapName[m] : "";
}

int ObjTableMapObjCount(int m) {
    ObjTableInit();
    return (m >= 0 && m < g_mapCount) ? g_mapObjCount[m] : 0;
}

const ObjItemInfo* ObjTableMapObj(int m, int i) {
    ObjTableInit();
    if (m < 0 || m >= g_mapCount) return nullptr;
    if (i < 0 || i >= g_mapObjCount[m]) return nullptr;
    return &g_objs[g_mapObjFirst[m] + i];
}

const ObjPathInfo* ObjTablePath(int pathIdx) {
    ObjTableInit();
    if (pathIdx < 0 || pathIdx >= g_pathCount) return nullptr;
    return &g_paths[pathIdx];
}

// ---- 运行时扫描 ----
#define NODE_MAX_HIT 2048

struct NodeHit { uint32_t hash; uintptr_t node; uint32_t value; int grade; };
static NodeHit g_hits[NODE_MAX_HIT];
static int g_hitCount = 0;

// 依赖 hash 集合（排序去重），扫描时只收这些
static uint32_t g_wantHash[OBJ_MAX_PATH * 12];
static int g_wantWeight[OBJ_MAX_PATH * 12];   // 稀有度权重：越独有的 flag 权重越高
static int g_wantCount = 0;

static int WantIndex(uint32_t h) {
    int lo = 0, hi = g_wantCount - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) >> 1;
        const uint32_t v = g_wantHash[mid];
        if (v == h) return mid;
        if (v < h) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

// 快速预筛位图：用 hash 低 20 位建 1M 位（128KB）。
// 有假阳性，但后面 LooksLikeNode 会精确核对 +0x14 的 hash，所以无害。
// 作用是把"每个 DWORD 一次二分"降到"一次位测试"，扫描从十几次秒到几秒。
static uint32_t g_wantBits[1u << 15];
static inline bool WantedFast(uint32_t h) {
    const uint32_t k = h & 0xFFFFFu;
    return (g_wantBits[k >> 5] >> (k & 31)) & 1u;
}

static void BuildWantSet() {
    g_wantCount = 0;
    ObjTableInit();
    for (int i = 0; i < g_pathCount; ++i) {
        for (int d = 0; d < g_paths[i].nDeps; ++d) {
            const uint32_t h = Fnv1(g_paths[i].deps[d].name);
            bool dup = false;
            for (int j = 0; j < g_wantCount; ++j) if (g_wantHash[j] == h) { dup = true; break; }
            if (!dup && g_wantCount < (int)(sizeof(g_wantHash) / sizeof(g_wantHash[0])))
                g_wantHash[g_wantCount++] = h;
        }
    }
    // 插入排序（百来条，够用）
    for (int i = 1; i < g_wantCount; ++i) {
        const uint32_t v = g_wantHash[i];
        int j = i - 1;
        while (j >= 0 && g_wantHash[j] > v) { g_wantHash[j + 1] = g_wantHash[j]; --j; }
        g_wantHash[j + 1] = v;
    }
    // 权重：按"该名字出现在多少张图里"取反比。
    // 共享名（TIMER - Objective Buffer 几乎每关都有）几乎不区分关卡；
    // 独有名字（FLAG - Intro Soviet 02 Patrol Destroyed）才是判据。
    for (int i = 0; i < g_wantCount; ++i) {
        const uint32_t h = g_wantHash[i];
        int df = 0;
        for (int m = 0; m < g_mapCount; ++m) {
            bool inMap = false;
            for (int p = g_mapPathFirst[m]; p < g_mapPathFirst[m] + g_mapPathCount[m] && !inMap; ++p)
                for (int d = 0; d < g_paths[p].nDeps; ++d)
                    if (Fnv1(g_paths[p].deps[d].name) == h) { inMap = true; break; }
            if (inMap) ++df;
        }
        g_wantWeight[i] = 1000 / (df > 0 ? df : 1);
    }
    // 位图预筛
    for (int i = 0; i < (1 << 15); ++i) g_wantBits[i] = 0;
    for (int i = 0; i < g_wantCount; ++i) {
        const uint32_t k = g_wantHash[i] & 0xFFFFFu;
        g_wantBits[k >> 5] |= (1u << (k & 31));
    }
}

// 依赖哈希集合查询（节点校验用）。首次调用会建表，之后是二分。
bool ObjIsDepHash(uint32_t h) {
    ObjTableInit();
    if (g_wantCount == 0) BuildWantSet();
    return WantIndex(h) >= 0;
}

// ---- 线程栈排除 ----
// 扫描时的散列值本身就在栈上：找 hash 必然命中"参数变量自己"，把它的 -0x14 当成节点，
// 周围全是栈帧里的局部变量与返回地址（看起来正好是像指针的字段）。写入就落在栈上 ——
// 日志报成功、游戏毫无反应。栈上永远不可能有脚本节点，整段排除。
struct StackRange { uintptr_t lo, hi; };
static StackRange g_stack[64];
static int g_nstack = 0;

typedef struct _THREAD_BASIC_INFO {
    LONG ExitStatus;
    PVOID TebBaseAddress;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR UniqueThreadId;
    ULONG_PTR AffinityMask;
    LONG Priority;
    LONG BasePriority;
} THREAD_BASIC_INFO_;

void ScriptCollectStacks() {
    g_nstack = 0;
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    if (!nt) return;
    typedef LONG(WINAPI * PFN_QIT)(HANDLE, int, PVOID, ULONG, PULONG);
    PFN_QIT qit = (PFN_QIT)GetProcAddress(nt, "NtQueryInformationThread");
    if (!qit) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    const DWORD me = GetCurrentProcessId();
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != me) continue;
            if (g_nstack >= (int)(sizeof(g_stack) / sizeof(g_stack[0]))) break;
            HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!th) continue;
            THREAD_BASIC_INFO_ tbi;
            memset(&tbi, 0, sizeof(tbi));
            ULONG got = 0;
            if (qit(th, 0, &tbi, sizeof(tbi), &got) == 0 && tbi.TebBaseAddress) {
                __try {
                    const uint32_t* teb = (const uint32_t*)tbi.TebBaseAddress;
                    const uintptr_t base = (uintptr_t)teb[1];    // StackBase
                    const uintptr_t lim = (uintptr_t)teb[2];     // StackLimit
                    if (base > lim && lim) {
                        g_stack[g_nstack].lo = lim;
                        g_stack[g_nstack].hi = base;
                        ++g_nstack;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

bool ScriptIsThreadStack(uintptr_t addr) {
    for (int i = 0; i < g_nstack; ++i)
        if (addr >= g_stack[i].lo && addr < g_stack[i].hi) return true;
    return false;
}

// 节点头 +0x00 L / +0x04 P / +0x08 R 必须为 0 或用户态堆地址；hash 在 +0x14，value 在 +0x18。
// 返回 0=不像 / 1=弱（P 为空：树根或游离） / 2=强（P 合法：真在树里，优先采信）
//
// 三个判据（缺一不可）：
//   1. 命中必须落在 MEM_PRIVATE 区域。脚本节点是引擎堆上的独立结构，永远不会在模块映像里；
//      而"全内存搜 4 字节哈希"必然命中所有**存着这些哈希的数组**——包括本模块自己的
//      依赖哈希表。实测一张图 450 个候选里 405 个落在 ra3copilot.dll 自己的映像中。
//   2. scope(+0x10) 与 value(+0x18) 都不能等于某个 flag 名哈希。错位读出来的"节点"
//      （把 {scope,hash,value} 数组读偏 4/8 字节）字段正好是相邻的哈希；真节点的 scope
//      是脚本作用域哈希、value 是标志值，都不会是名字哈希。
//   3. 指针上限放到 0xFFF00000：游戏是 LAA，堆能到 0xFFxxxxxx，旧上限 0x7F000000 会把
//      高地址的父/子节点整个判死，真节点反而被筛掉。
// value 还要排除"指向节点自己这块分配"的情况：那是链表/索引里的自引用字段
// （实测一整片私有堆里存着 {hash, 指针, hash, 指针...} 的记录，凭 scope/hash 完全
//  像节点，写进去游戏毫无反应 —— 用户那边的"写成功但不生效"就是这种）。
static bool NodeRegionPrivate(uintptr_t node, uint32_t val) {
    if (ScriptIsThreadStack(node) || ScriptIsThreadStack(node + 0x1C)) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((LPCVOID)node, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.Type != MEM_PRIVATE) return false;
    const uintptr_t b = (uintptr_t)mbi.BaseAddress;
    if ((uintptr_t)val >= b && (uintptr_t)val < b + (uintptr_t)mbi.RegionSize) return false;
    // 与之相邻（同一块分配的另一段）也算自引用：名字表就是
    // {hash, 名字指针} 紧邻排列，指针常常刚好越过分段边界。
    const uintptr_t v = (uintptr_t)val;
    const uintptr_t d = (v > node) ? (v - node) : (node - v);
    if (v && d < 0x10000u) return false;
    return true;
}

static int NodeGrade(uintptr_t node, uint32_t hash, uint32_t* outVal) {
    if (node < 0x01000000u) return 0;
    __try {
        const uint32_t* w = (const uint32_t*)node;
        if (w[5] != hash) return 0;
        const uint32_t L = w[0], P = w[1], R = w[2];
        if (L && (L < 0x00010000u || L >= 0xFFF00000u)) return 0;
        if (R && (R < 0x00010000u || R >= 0xFFF00000u)) return 0;
        if (P && (P < 0x00010000u || P >= 0xFFF00000u)) return 0;
        if (ObjIsDepHash(w[4]) || ObjIsDepHash(w[6])) return 0;
        if (w[4] == 0) return 0;          // 作用域恒非 0（无前缀是 0x811C9DC5），0 是错位读
        if (!NodeRegionPrivate(node, w[6])) return 0;
        const uint32_t v = w[6];
        if (v == 0xCCCCCCCCu || v == 0xCDCDCDCDu) return 0;   // 调试填充，不是真值
        if (outVal) *outVal = v;
        return (P >= 0x01000000u) ? 2 : 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static volatile LONG g_scanAbort = 0;

int ScriptNodeScan() {
    ObjTableInit();
    ScriptCollectStacks();
    if (g_wantCount == 0) BuildWantSet();
    g_hitCount = 0;
    InterlockedExchange(&g_scanAbort, 0);
    if (g_wantCount == 0) return 0;

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t cur = (uintptr_t)si.lpMinimumApplicationAddress;
    const uintptr_t end = (uintptr_t)si.lpMaximumApplicationAddress;
    const DWORD readMask = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
    const size_t step = 8u << 20;
    MEMORY_BASIC_INFORMATION mbi;

    while (cur < end && VirtualQuery((LPCVOID)cur, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (g_scanAbort) break;
        const uintptr_t base = (uintptr_t)mbi.BaseAddress;
        const size_t size = mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && base >= 0x01000000u &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (mbi.Protect & readMask)) {
            for (size_t off = 0; off < size; off += step) {
                const size_t n = (size - off) < step ? (size - off) : step;
                __try {
                    const uintptr_t start = (base + off + 3) & ~(uintptr_t)3;
                    const uint32_t* w = (const uint32_t*)start;
                    const size_t cnt = (n > (start - (base + off)))
                                           ? (n - (start - (base + off))) / 4 : 0;
                    for (size_t i = 0; i < cnt; ++i) {
                        const uint32_t hv = w[i];
                        if (!WantedFast(hv)) continue;      // 位图预筛（1/9000 假阳性）
                        if (WantIndex(hv) < 0) continue;    // 精确确认：必须真的在依赖集合里
                        const uintptr_t node = (uintptr_t)(w + i) - 0x14;
                        uint32_t val = 0;
                        const int g = NodeGrade(node, hv, &val);
                        if (!g) continue;
                        // 同 hash 只留一份：已有更强的（在树里的）就不再收
                        int dup = -1;
                        for (int k = 0; k < g_hitCount; ++k)
                            if (g_hits[k].hash == hv) { dup = k; break; }
                        if (dup >= 0) {
                            if (g_hits[dup].grade < g) {
                                g_hits[dup].node = node;
                                g_hits[dup].value = val;
                                g_hits[dup].grade = g;
                            }
                            continue;
                        }
                        if (g_hitCount >= NODE_MAX_HIT) continue;
                        g_hits[g_hitCount].hash = hv;
                        g_hits[g_hitCount].node = node;
                        g_hits[g_hitCount].value = val;
                        g_hits[g_hitCount].grade = g;
                        ++g_hitCount;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
        }
        const uintptr_t next = base + size;
        if (next <= cur) break;
        cur = next;
    }
    return g_hitCount;
}

// 统一的"按名字哈希找节点"入口：先查脚本变量表（结构确定），
// 表不可用（没进关 / 读不到 TheScriptEngine）时才退回旧的全内存扫描。
static bool TblFindNode(uint32_t hash, uintptr_t* outNode, uint32_t* outVal) {
    uintptr_t node = 0;
    bool isFlag = false;
    if (!ScriptTblFindByHash(hash, &isFlag, &node, nullptr) || !node) return false;
    uint32_t v = 0;
    __try {
        v = *(const uint32_t*)(node + 0x18);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (outNode) *outNode = node;
    if (outVal) *outVal = v;
    return true;
}

bool ScriptNodeValue(uint32_t hash, uint32_t* outValue) {
    uint32_t v = 0;
    if (TblFindNode(hash, nullptr, &v)) {
        if (outValue) *outValue = v;
        return true;
    }
    for (int i = 0; i < g_hitCount; ++i) {
        if (g_hits[i].hash != hash) continue;
        // 实时读 + 核对 hash：缓存地址可能在换关/脚本重载后被别的数据占用，
        // 直接返还扫描快照会把「上一局的 1」当成「这一局的 1」。
        __try {
            if (*(const uint32_t*)(g_hits[i].node + 0x14) != hash) return false;
            if (outValue) *outValue = *(const uint32_t*)(g_hits[i].node + 0x18);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        return true;
    }
    return false;
}

void ScriptNodeScanAbort() { InterlockedExchange(&g_scanAbort, 1); }

void ScriptNodeRefresh() {
    for (int i = 0; i < g_hitCount; ++i) {
        __try {
            g_hits[i].value = *(const uint32_t*)(g_hits[i].node + 0x18);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

bool ScriptNodeAddr(uint32_t hash, uintptr_t* outNode) {
    uintptr_t node = 0;
    if (TblFindNode(hash, &node, nullptr)) { if (outNode) *outNode = node; return true; }
    for (int i = 0; i < g_hitCount; ++i)
        if (g_hits[i].hash == hash) { if (outNode) *outNode = g_hits[i].node; return true; }
    return false;
}

// 缓存里的地址可能已经失效（换关 / 脚本重载后节点被释放，同址被别的数据占用）。
// 写入前用它核对：地址还读得通，且 +0x14 仍是同一个 hash。
// 失败时调用方应改用 ScriptFlagResolveHash() 重新扫一遍。
bool ScriptNodeAddrLive(uint32_t hash, uintptr_t* outNode) {
    uintptr_t t = 0;
    if (TblFindNode(hash, &t, nullptr) && t) { if (outNode) *outNode = t; return true; }
    uintptr_t n = 0;
    if (!ScriptNodeAddr(hash, &n) || !n) return false;
    __try {
        if (*(const uint32_t*)(n + 0x14) != hash) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (outNode) *outNode = n;
    return true;
}

// 当前地图路径：[TheGameLogic]+0x98 是一个 C 串，实测形如
// data\maps\official\CAMP_S08_EasterIsland_Stewart\CAMP_S08_EasterIsland_Stewart.map。
// 按地图文件名认关卡是唯一可靠的方式 —— 靠脚本节点命中率会被跨关卡同名的
// TIMER - Objective Buffer 带偏（A01 曾经永远是「最像」的那张）。
//
// GameLogic 全局 VA 按构建分两列：零售 0x00CD8CE4 / Steam 0x00CDDE84。两者差 0x51A0，
// 与 features.cpp 里 0x00CD8D00 / 0x00CDDEA0 那一对同差。取错列读到的是 .data 里的
// 未用槽位（值为 0）—— 表现不是崩溃，而是「读不到当前地图路径」，于是掉进启发式打分，
// 关卡认错。两列都读不出 *.map 时扫一遍 .data 兜底（别的构建 / 槽位被别的对象重用）。
static volatile LONG g_mapPathVa  = 0;   // 兜底探测到的全局 VA（0 = 还没有）
static volatile LONG g_mapPathNextMs = 0;   // 兜底探测的冷却截止（GetTickCount）

// 地址起的 n 字节是否落在已提交、可读的页里
static bool PageReadable(uintptr_t addr, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
    if (!(mbi.Protect & ok)) return false;
    return (addr + n) <= ((uintptr_t)mbi.BaseAddress + mbi.RegionSize);
}

static bool RdU32(uintptr_t addr, uintptr_t* out) {
    if (!PageReadable(addr, 4)) return false;
    __try { *out = *(const uintptr_t*)addr; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

// 读 C 串（同进程内，VA 即绝对地址；无 ASLR）
static bool ReadCStr(uintptr_t addr, char* out, int cap) {
    out[0] = 0;
    if (cap < 2 || !PageReadable(addr, 2)) return false;
    int n = 0;
    __try {
        const char* p = (const char*)addr;
        for (; n < cap - 1 && p[n]; ++n) out[n] = p[n];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
    out[n] = 0;
    return n > 0;
}

// 只认地图路径：必须以 .map 结尾。别把 GameLogic 里别的串当路径用。
static bool LooksLikeMapPath(const char* s) {
    const size_t n = strlen(s);
    if (n < 8 || n >= 260) return false;
    return _stricmp(s + n - 4, ".map") == 0;
}

// gva 指向 GameLogic：取 +0x98 的串。out 可为 nullptr（只校验）。
static bool PathFromGlobal(uintptr_t gva, char* out, int cap) {
    uintptr_t obj = 0, p = 0;
    if (!gva || !RdU32(gva, &obj) || !obj) return false;
    if (!RdU32(obj + 0x98, &p) || !p) return false;
    char tmp[260];
    if (!ReadCStr(p, tmp, (int)sizeof(tmp))) return false;
    if (!LooksLikeMapPath(tmp)) return false;
    if (out && cap > 0) strncpy_s(out, (size_t)cap, tmp, _TRUNCATE);
    return true;
}

// 路径里是否真有 \maps\ 这一层（战役/遭遇战图都在 maps 下；additionalmaps 不算）
static bool HasMapsDir(const char* s) {
    for (const char* p = s; *p; ++p)
        if (_strnicmp(p, "\\maps\\", 6) == 0) return true;
    return false;
}

// 兜底：扫主模块可写数据段，找「[全局] → 对象，且对象 +0x98 是 *.map 路径」的那个全局。
// 先只认 \maps\ 下的路径（战役与遭遇战图都在那儿，能避开 mapmetadata_*.manifest 这类
// 同结构的对象），一个都没有才放宽到任意 .map —— additionalmaps 里的 mod 图靠第二遍。
// .data 几十万字节、单次遍历几十毫秒，只可能在后台线程跑到。
static uintptr_t DetectMapPathGlobal() {
    const uintptr_t img = (uintptr_t)GetModuleHandleW(nullptr);
    if (!img) return 0;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)img;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(img + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    for (int strict = 1; strict >= 0; --strict) {
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
            const DWORD vsz = sec[s].Misc.VirtualSize;
            if (!(sec[s].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
            if (vsz < 0x1000 || vsz > 0x400000) continue;   // .data 量级；跳过 .tls 之类
            const uintptr_t lo = img + sec[s].VirtualAddress;
            const uintptr_t hi = lo + vsz - 4;
            for (uintptr_t a = lo; a < hi; a += 4) {
                uintptr_t obj = 0;
                if (!RdU32(a, &obj) || obj < 0x01000000u || obj >= 0x7F000000u) continue;
                char tmp[260];
                if (!PathFromGlobal(a, tmp, (int)sizeof(tmp))) continue;
                if (strict && !HasMapsDir(tmp)) continue;
                return a;
            }
        }
    }
    return 0;
}

// 当前地图路径的来源全局：已知两列 → 探测缓存 → 兜底探测（失败冷却 5 秒再试）
static uintptr_t MapPathGlobal() {
    uintptr_t cached = (uintptr_t)InterlockedCompareExchange(&g_mapPathVa, 0, 0);
    if (cached && PathFromGlobal(cached, nullptr, 0)) return cached;

    const uintptr_t picked = ::ra3::Pick(0x00CD8CE4u, 0x00CDDE84u);
    if (PathFromGlobal(picked, nullptr, 0)) {
        InterlockedExchange(&g_mapPathVa, (LONG)picked);
        return picked;
    }

    // 冷却而不是"最多试 N 次"：主菜单阶段扫不到是正常的，进局后必须还能自己找回来。
    const DWORD now = GetTickCount();
    if ((LONG)(now - (DWORD)g_mapPathNextMs) < 0) return 0;
    const uintptr_t g = DetectMapPathGlobal();
    if (g) InterlockedExchange(&g_mapPathVa, (LONG)g);
    else    InterlockedExchange(&g_mapPathNextMs, (LONG)(now + 5000));
    return g;
}

// 当前地图的相对路径，原样返回（'\\' 分隔）。读不到返回 false。
static bool GameMapRelPath(char* out, int cap) {
    out[0] = 0;
    return PathFromGlobal(MapPathGlobal(), out, cap);
}

// 从路径取小写、不含扩展名的文件名
static void BaseNameFromPath(const char* path, char* out, int cap) {
    out[0] = 0;
    const char* base = path;
    for (const char* s = path; *s; ++s)
        if (*s == '\\' || *s == '/') base = s + 1;
    int k = 0;
    for (; base[k] && base[k] != '.' && k < cap - 1; ++k) {
        const char c = base[k];
        out[k] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    out[k] = 0;
}

static int MapFromGamePath() {
    char rel[260];
    if (!GameMapRelPath(rel, (int)sizeof(rel))) return -1;
    char name[128];
    BaseNameFromPath(rel, name, (int)sizeof(name));
    if (!name[0]) return -1;
    for (int m = 0; m < g_mapCount; ++m)
        if (_stricmp(g_mapName[m], name) == 0) return m;
    return -1;
}

int ObjDetectMap(int* scoreOut) {
    ObjTableInit();
    const int byPath = MapFromGamePath();
    if (byPath >= 0) {
        if (scoreOut) *scoreOut = 9999;
        return byPath;
    }
    if (g_hitCount == 0) ScriptNodeScan();
    // 分两轮打分：
    //   第 1 轮只算「该图独有的依赖名」（稀有度权重满值 1000）。
    //     TIMER - Objective Buffer 这类名字几乎每张图都有，按总量打分时依赖项最多的
    //     A01 永远第一 —— 这就是「人在 S08 却识别成 A01」的原因。
    //   第 2 轮（第 1 轮全 0 时）退回全部依赖，避免通用名为主的图识别不出来。
    int best = -1, bestScore = 0;
    for (int pass = 0; pass < 2 && bestScore == 0; ++pass) {
        for (int m = 0; m < g_mapCount; ++m) {
            int hit = 0, total = 0;
            for (int p = g_mapPathFirst[m]; p < g_mapPathFirst[m] + g_mapPathCount[m]; ++p) {
                for (int d = 0; d < g_paths[p].nDeps; ++d) {
                    const uint32_t h = Fnv1(g_paths[p].deps[d].name);
                    const int wi = WantIndex(h);
                    if (wi < 0) continue;
                    if (pass == 0 && g_wantWeight[wi] < 1000) continue;
                    ++total;
                    uint32_t v = 0;
                    if (!ScriptNodeValue(h, &v)) continue;
                    const char kind = g_paths[p].deps[d].kind;
                    const bool sat = (kind == 'T') ? (v == 0u)
                                   : (kind == 'N') ? !(v & 1u) : (v & 1u) != 0;
                    if (sat) ++hit;
                }
            }
            if (total == 0) continue;
            // 用「命中率」而不是命中数：A01 独有名字最多（9 个目标），
            // 按绝对数打分它永远排第一，人却在别的关里。
            const int score = hit * 1000 / total;
            if (score > bestScore) { bestScore = score; best = m; }
        }
    }
    if (scoreOut) *scoreOut = bestScore;
    // 启发式只是最后一层（路径读得到时根本走不到这儿）。独有依赖命中率不到一半 = 目标表里
    // 没有像当前这关的图 —— 返回 -1 让人手动选关，比猜一张错的更安全：猜错会把别的关的
    // flag 拨一遍。
    if (bestScore < 500) return -1;
    return best;
}

// 用当前关卡的 .map 现场生成目标表，替换内置表。
//
// 读盘 + 两层解压 + 脚本树解析约 30-60ms，只能在 HTTP / 后台线程调用；
// 渲染线程只读已有结果。失败时保持原表不动，原因记在 g_srcNote 里。
bool ObjTableUseScript() {
    char rel[260];
    if (!GameMapRelPath(rel, (int)sizeof(rel))) {
        _snprintf_s(g_srcNote, sizeof(g_srcNote), _TRUNCATE,
                    "读不到当前地图路径（还在载入或未进对局）");
        return false;
    }
    char base[128];
    BaseNameFromPath(rel, base, (int)sizeof(base));
    if (!base[0]) {
        _snprintf_s(g_srcNote, sizeof(g_srcNote), _TRUNCATE, "地图路径里没有文件名");
        return false;
    }
    if (g_srcKind == 1 && _stricmp(base, g_srcMap) == 0) return true;   // 已经是这张图

    MapTblResult r;
    if (!MapTblBuild(rel, base, &r)) {
        _snprintf_s(g_srcNote, sizeof(g_srcNote), _TRUNCATE, "脚本表不可用：%s", r.err);
        return false;
    }

    g_srcLines = r.lines;
    g_srcKind = 1;
    strncpy_s(g_srcMap, sizeof(g_srcMap), base, _TRUNCATE);
    _snprintf_s(g_srcNote, sizeof(g_srcNote), _TRUNCATE,
                "%d 脚本 -> %d 目标 / %d 路径", r.scriptCount, r.objCount, r.pathCount);

    // 整表重建：行格式与内置表相同，重置池后按新来源解析
    g_poolCur = g_pool;
    g_mapCount = 0;
    g_pathCount = 0;
    g_objCount = 0;
    g_wantCount = 0;
    g_hitCount = 0;
    g_inited = 0;
    ObjTableInit();
    return true;
}

int         ObjTableSourceKind() { return g_srcKind; }
const char* ObjTableSourceNote() { return g_srcNote; }

int ObjPathWrites(int pathIdx, uint32_t* outHash, uint32_t* outValue,
                  int* outMode, int maxN) {
    const ObjPathInfo* p = ObjTablePath(pathIdx);
    if (!p) return 0;
    int n = 0;
    for (int d = 0; d < p->nDeps && n < maxN; ++d) {
        outHash[n] = Fnv1(p->deps[d].name);
        outMode[n] = (p->deps[d].kind == 'N') ? 2 : ((p->deps[d].kind == 'T') ? 0 : 1);
        outValue[n] = 0u;   // flag 写 0=假；timer 写 0=已过期
        ++n;
    }
    return n;
}

int ObjDepState(const char* name, char kind) {
    if (!name) return -1;
    uint32_t v = 0;
    if (!ScriptNodeValue(Fnv1(name), &v)) return -1;
    // timer：存的是"剩余帧数"，倒计时到 0 就是过期。0xFFFFFFFF = 建过但从未启动。
    // 反证：A02 开场 `TIMER - Objective 01/4 - Complete` 都是 0xFFFFFFFF，而这两个目标
    // 显然没完成；A01 全程 110 个 timer 也全是 0xFFFFFFFF。若 -1 算过期，目标会自己打勾。
    if (kind == 'T') return (v == 0u) ? 1 : 0;
    // flag 只有最低字节有效（引擎按 bool 写，0x489C10 `cmp byte ptr [eax],0`）。
    // 表里出现 0x08107F01 / 0x94000F00 这类怪值就是高位残留，不能整 dword 判真假。
    const bool on = (v & 0xFFu) != 0;
    if (kind == 'N') return on ? 0 : 1;                   // 取反 flag
    return on ? 1 : 0;
}

uint32_t ObjNameHash(const char* name) { return name ? Fnv1(name) : 0u; }

// ---- 诊断：全候选扫描 ----
// 与 ScriptNodeScan 同一条内存遍历，但不去重：同一个名字哈希在内存里可能撞到
// 好几个长得像节点的位置（任意 4 字节等于哈希、后面跟几个像指针的字段就过校验），
// 现行逻辑只留一个"最强"的，可能留错 —— 表现就是「日志写成功、游戏没反应」。
static uintptr_t NodeRootOf(uintptr_t node, int* depthOut) {
    uintptr_t cur = node;
    int d = 0;
    __try {
        for (; d < 64; ++d) {
            const uint32_t P = *(const uint32_t*)(cur + 0x04);
            if (!P || P < 0x01000000u || P >= 0x7F000000u) break;
            if ((uintptr_t)P == node) break;
            cur = (uintptr_t)P;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (depthOut) *depthOut = 0;
        return 0;
    }
    if (depthOut) *depthOut = d;
    return cur;
}

int ScriptNodeCandScan(const uint32_t* hashes, int n, int* countOut,
                       ScriptNodeCand* out, int keepPerHash, int maxTotal) {
    if (!hashes || n <= 0 || n > 64 || !countOut || !out || keepPerHash <= 0) return 0;
    ScriptCollectStacks();
    for (int i = 0; i < n; ++i) countOut[i] = 0;

    static uint32_t bits[1u << 15];        // 低 20 位预筛，假阳性后面会精确核对
    for (int i = 0; i < (1 << 15); ++i) bits[i] = 0;
    for (int i = 0; i < n; ++i) {
        const uint32_t k = hashes[i] & 0xFFFFFu;
        bits[k >> 5] |= (1u << (k & 31));
    }

    int stored = 0;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t cur = (uintptr_t)si.lpMinimumApplicationAddress;
    const uintptr_t end = (uintptr_t)si.lpMaximumApplicationAddress;
    const DWORD readMask = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
    const size_t step = 8u << 20;
    MEMORY_BASIC_INFORMATION mbi;

    while (cur < end && VirtualQuery((LPCVOID)cur, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (g_scanAbort) break;
        const uintptr_t base = (uintptr_t)mbi.BaseAddress;
        const size_t size = mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && base >= 0x01000000u &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (mbi.Protect & readMask)) {
            for (size_t off = 0; off < size; off += step) {
                const size_t nb = (size - off) < step ? (size - off) : step;
                __try {
                    const uintptr_t start = (base + off + 3) & ~(uintptr_t)3;
                    const uint32_t* w = (const uint32_t*)start;
                    const size_t cnt = (nb > (start - (base + off)))
                                           ? (nb - (start - (base + off))) / 4 : 0;
                    for (size_t i = 0; i < cnt; ++i) {
                        const uint32_t hv = w[i];
                        if (!bits[(hv & 0xFFFFFu) >> 5]) continue;
                        if (!((bits[(hv & 0xFFFFFu) >> 5] >> (hv & 31)) & 1u)) continue;
                        int k = -1;
                        for (int j = 0; j < n; ++j) if (hashes[j] == hv) { k = j; break; }
                        if (k < 0) continue;
                        const uintptr_t node = (uintptr_t)(w + i) - 0x14;
                        if (node < 0x01000000u) continue;
                        __try {
                            const uint32_t* q = (const uint32_t*)node;
                            if (q[5] != hv) continue;
                            const uint32_t L = q[0], P = q[1], R = q[2];
                            if (L && (L < 0x00010000u || L >= 0xFFF00000u)) continue;
                            if (R && (R < 0x00010000u || R >= 0xFFF00000u)) continue;
                            if (P && (P < 0x00010000u || P >= 0xFFF00000u)) continue;
                            if (ObjIsDepHash(q[4]) || ObjIsDepHash(q[6])) continue;
                            if (q[4] == 0) continue;      // 作用域恒非 0，0 是错位读
                            if (!NodeRegionPrivate(node, q[6])) continue;
                            const uint32_t val = q[6];
                            if (val == 0xCCCCCCCCu || val == 0xCDCDCDCDu) continue;
                            const int slot = countOut[k];
                            ++countOut[k];
                            if (slot >= keepPerHash || stored >= maxTotal) continue;
                            ScriptNodeCand& c = out[stored++];
                            c.node = node;
                            c.L = L; c.P = P; c.R = R;
                            c.scope = q[4];
                            c.hash = hv;
                            c.value = val;
                            c.depth = 0;
                            c.root = NodeRootOf(node, &c.depth);
                        } __except (EXCEPTION_EXECUTE_HANDLER) {
                        }
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
        }
        const uintptr_t next = base + size;
        if (next <= cur) break;
        cur = next;
    }
    return stored;
}
