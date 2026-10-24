// 战役目标表：解析内置表 + 运行时脚本节点扫描。
//
// 表格式（见 generated/objectives_data.h）：
//   "M|<map 名>"
//   "O|<目标名>|<other 条件数>|<依赖1>|<依赖2>|..."   同一目标多行 = 多条完成路径
// 依赖前缀：F=FLAG(置1)  N=取反FLAG(置0)  T=TIMER(写 0xFFFFFFFF=已过期)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "scriptobj.h"
#include "generated/objectives_data.h"

#include <string.h>
#include <stdlib.h>

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

void ObjTableInit() {
    if (g_inited) return;
    g_inited = 1;
    int curMap = -1;
    for (int i = 0; kObjTableData[i]; ++i) {
        const char* line = kObjTableData[i];
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

// 节点头 +0x00 L / +0x04 P / +0x08 R 必须为 0 或堆地址；hash 在 +0x14，value 在 +0x18。
// 返回 0=不像 / 1=弱（P 为空：树根或游离） / 2=强（P 合法：真在树里，优先采信）
static int NodeGrade(uintptr_t node, uint32_t hash, uint32_t* outVal) {
    if (node < 0x01000000u) return 0;
    __try {
        const uint32_t* w = (const uint32_t*)node;
        if (w[5] != hash) return 0;
        const uint32_t L = w[0], P = w[1], R = w[2];
        if (L && (L < 0x01000000u || L >= 0x7F000000u)) return 0;
        if (R && (R < 0x01000000u || R >= 0x7F000000u)) return 0;
        if (P && (P < 0x01000000u || P >= 0x7F000000u)) return 0;
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

bool ScriptNodeValue(uint32_t hash, uint32_t* outValue) {
    for (int i = 0; i < g_hitCount; ++i)
        if (g_hits[i].hash == hash) { if (outValue) *outValue = g_hits[i].value; return true; }
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
    for (int i = 0; i < g_hitCount; ++i)
        if (g_hits[i].hash == hash) { if (outNode) *outNode = g_hits[i].node; return true; }
    return false;
}

// 当前地图路径：TheGameLogic（[0x00CD8CE4]）+0x98 是一个 C 串，
// 实测形如 data\maps\official\CAMP_S09_NewYork_Rao\CAMP_S09_NewYork_Rao.map。
// 按地图文件名认关卡是唯一可靠的方式 —— 靠脚本节点命中率会被跨关卡同名的
// TIMER - Objective Buffer 带偏（A01 曾经永远是「最像」的那张）。
static int MapFromGamePath() {
    uintptr_t gl = 0;
    __try { gl = *(const uintptr_t*)0x00CD8CE4; } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (!gl) return -1;
    const char* path = nullptr;
    __try { path = *(const char**)(gl + 0x98); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (!path || (uintptr_t)path < 0x01000000u) return -1;
    char buf[260];
    int n = 0;
    __try {
        for (; n < (int)sizeof(buf) - 1 && path[n]; ++n) buf[n] = path[n];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    buf[n] = 0;

    const char* base = buf;
    for (const char* s = buf; *s; ++s)
        if (*s == '\\' || *s == '/') base = s + 1;
    char name[128];
    int k = 0;
    for (; base[k] && base[k] != '.' && k < (int)sizeof(name) - 1; ++k) {
        char c = base[k];
        name[k] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    name[k] = 0;
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
                    const bool sat = (kind == 'T') ? (v == 0xFFFFFFFFu)
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
    return best;
}

int ObjPathWrites(int pathIdx, uint32_t* outHash, uint32_t* outValue,
                  int* outMode, int maxN) {
    const ObjPathInfo* p = ObjTablePath(pathIdx);
    if (!p) return 0;
    int n = 0;
    for (int d = 0; d < p->nDeps && n < maxN; ++d) {
        outHash[n] = Fnv1(p->deps[d].name);
        outMode[n] = (p->deps[d].kind == 'N') ? 2 : ((p->deps[d].kind == 'T') ? 0 : 1);
        outValue[n] = (p->deps[d].kind == 'T') ? 0xFFFFFFFFu : 0u;
        ++n;
    }
    return n;
}

int ObjDepState(const char* name, char kind) {
    if (!name) return -1;
    uint32_t v = 0;
    if (!ScriptNodeValue(Fnv1(name), &v)) return -1;
    if (kind == 'T') return (v == 0xFFFFFFFFu) ? 1 : 0;   // timer: 0xFFFFFFFF = 已过期
    if (kind == 'N') return (v & 1u) ? 0 : 1;             // 取反 flag
    return (v & 1u) ? 1 : 0;                              // flag: bit0
}

uint32_t ObjNameHash(const char* name) { return name ? Fnv1(name) : 0u; }
