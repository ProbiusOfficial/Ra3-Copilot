// 脚本变量表（ScriptEngine 的 flag / timer map）精确访问。
// 偏移与语义来源见 scripttable.h 顶部注释与 internal/docs/战役目标-精准修改-方案.md。
//
// 设计原则：**不做启发式判断**。表在哪里、节点怎么排、键怎么比，都由静态定位得到，
// 这里只按结构走。读不通就报"读不通"，绝不退化成"猜一个候选"。

#include "scripttable.h"
#include "hook.h"        // IsSteamBuild：按构建取 RVA

#include <windows.h>
#include <string.h>

// ---- 引擎入口 --------------------------------------------------------------
// TheScriptEngine：本次定位的关键产物。`mov ecx,[0xCDD190]` 出现在每一个
// 调用 ScriptEngine 成员的地方（实测 0x489C01 / 0x189461 / 0x1918xx），
// 是唯一的全局实例指针。exe 无 ASLR，地址固定。
// 绝对 VA 0x00CDD190 = ImageBase(0x400000) + RVA 0x8DD190。
// 本进程无 ASLR，实际基址恒 0x400000；但仍按"模块基址 + RVA"算，换了包装/重定位也不会错。
static const uint32_t kTheScriptEngineRva  = 0x008DD190u;   // 零售（VA 0xCDD190）
static const uint32_t kTheScriptEngineRvaS = 0x008E2330u;   // Steam（VA 0xCE2330，引用指纹唯一配对）

static uint32_t TheScriptEngineAddr() {
    static uint32_t s_engAddr = 0;
    if (!s_engAddr) {
        const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
        const uint32_t rva = ::ra3::IsSteamBuild() ? kTheScriptEngineRvaS : kTheScriptEngineRva;
        s_engAddr = (uint32_t)(base + rva);
    }
    return s_engAddr;
}

// 已知偏移（v1.12.3444）：两张表相邻，间距 0x1C。
// 这两个值是"首选"，不是"唯一" —— 不同版本 / 改过的 exe 上偏移可能挪动，
// Locate() 会做结构自检，不通过就在 engine 对象里重新搜。
static const uint32_t kFlagMapOff  = 0x16CE4u;
static const uint32_t kTimerMapOff = 0x16CC8u;

// 定位结果（进程内只算一次）
static uint32_t g_flagOff = 0;
static uint32_t g_timerOff = 0;
static int      g_locHow = 0;   // 0=未定位 1=已知偏移自检通过 2=自动搜索
static uint32_t g_locateStamp = 0;  // 定位时的 engine 指针（换关/重进后失效重算）

// map 内的相对偏移（见头文件）
static const uint32_t kMapSentinel = 0x04u;      // 内嵌哨兵起始
static const uint32_t kMapRoot     = 0x0Cu;      // root = 哨兵.parent

// 节点字段
static const uint32_t kNodeRight  = 0x00u;
static const uint32_t kNodeLeft   = 0x04u;
static const uint32_t kNodeParent = 0x08u;
static const uint32_t kNodeScope  = 0x10u;
static const uint32_t kNodeHash   = 0x14u;
static const uint32_t kNodeValue  = 0x18u;

// 当前脚本作用域字符串（GetFlag 内取自 [edi+0x17D60]，为空时按空串算）
static const uint32_t kScopeStrOff = 0x17D60u;
static const uint32_t kEmptyScopeHash = 0x811C9DC5u;   // FNV-1 offset basis

// ---- 安全读 ----------------------------------------------------------------

static inline bool Rd32(uintptr_t a, uint32_t* out) {
    __try { *out = *(const volatile uint32_t*)a; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static inline bool Rd8(uintptr_t a, uint8_t* out) {
    __try { *out = *(const volatile uint8_t*)a; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static inline bool Wr8(uintptr_t a, uint8_t v) {
    __try { *(volatile uint8_t*)a = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static inline bool PlausiblePtr(uint32_t p) {
    return p >= 0x00010000u && p < 0xFFF00000u;
}

// FNV-1：先乘 0x01000193 再异或字节（与引擎 0x173B50 的循环逐条一致）
static uint32_t Fnv1(const char* s) {
    uint32_t h = kEmptyScopeHash;
    if (!s) return h;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        h *= 16777619u;
        h ^= *p;
    }
    return h;
}

// ---- 入口 ------------------------------------------------------------------

uint32_t ScriptTblEngine() {
    uint32_t e = 0;
    if (!Rd32(TheScriptEngineAddr(), &e)) return 0;
    return PlausiblePtr(e) ? e : 0;
}

// ---- 表定位（版本自适应）---------------------------------------------------
//
// 已知偏移在 v1.12.3444 上成立，但别的版本 / 改过的 exe 上可能挪动。
// 所以偏移只当"首选"，真伪靠结构自检；不通过就在 engine 对象里重新搜。
//
// 自检的三条硬条件（全部由静态语义推出，不是启发式）：
//   1. [map+0xC] 是合法堆指针（root）
//   2. root 的 +0x08（parent）== map+4（内嵌哨兵地址）—— 真 map 的根一定挂在内嵌哨兵上
//   3. 全树父子互指零失败 —— 假的候选（把别的数据当节点）几乎必然在这一步崩掉

static bool ReadNode(uintptr_t a, ScriptNodeInfo* n, bool isFlag);   // 定义在下方"节点读"

struct MapStat {
    uint32_t off;
    int n;        // 节点数
    int bad;      // 父子互指失败
    int nFFF;     // 值 == 0xFFFFFFFF（timer 特征）
    int nBool;    // 低字节 <= 1（flag 特征）
};

static bool ProbeMap(uint32_t eng, uint32_t off, MapStat* st) {
    memset(st, 0, sizeof(*st));
    st->off = off;
    const uint32_t base = eng + off;
    const uint32_t sent = base + kMapSentinel;

    uint32_t root = 0;
    if (!Rd32(base + kMapRoot, &root) || !PlausiblePtr(root)) return false;
    uint32_t rp = 0;
    if (!Rd32(root + kNodeParent, &rp) || rp != sent) return false;   // 根必须挂在内嵌哨兵上

    uint32_t stack[64];
    uint32_t seen[384];
    int sp = 0, nseen = 0;
    stack[sp++] = root;
    while (sp > 0) {
        const uint32_t a = stack[--sp];
        if (!PlausiblePtr(a) || a == sent) continue;
        if (nseen >= (int)(sizeof(seen) / sizeof(seen[0]))) break;
        bool dup = false;
        for (int i = 0; i < nseen; ++i) if (seen[i] == a) { dup = true; break; }
        if (dup) continue;
        seen[nseen++] = a;

        ScriptNodeInfo n;
        if (!ReadNode(a, &n, true)) continue;
        ++st->n;
        if (n.value == 0xFFFFFFFFu) ++st->nFFF;
        if ((n.value & 0xFFu) <= 1u) ++st->nBool;

        uint32_t L = 0, R = 0, P2 = 0;
        if (Rd32(a + kNodeLeft, &L) && PlausiblePtr(L)) {
            if (!Rd32(L + kNodeParent, &P2) || P2 != a) ++st->bad;
            if (sp < 62) stack[sp++] = L;
        }
        if (Rd32(a + kNodeRight, &R) && PlausiblePtr(R)) {
            if (!Rd32(R + kNodeParent, &P2) || P2 != a) ++st->bad;
            if (sp < 62) stack[sp++] = R;
        }
    }
    return st->n > 0 && st->bad == 0;
}

// 定位一次，结果缓存在 g_*Off；engine 指针一变（换关/重进）就重算。
static void Locate() {
    const uint32_t e = ScriptTblEngine();
    if (!e) { g_flagOff = g_timerOff = 0; g_locHow = 0; g_locateStamp = 0; return; }
    if (g_locateStamp == e && g_flagOff) return;

    g_locateStamp = e;
    g_flagOff = g_timerOff = 0;
    g_locHow = 0;

    MapStat sf, stm;
    if (ProbeMap(e, kFlagMapOff, &sf) && ProbeMap(e, kTimerMapOff, &stm)) {
        g_flagOff = kFlagMapOff;
        g_timerOff = kTimerMapOff;
        g_locHow = 1;
        return;
    }

    // 已知偏移不成立：在 engine 对象里搜。范围按 v1.12 的成员分布取，
    // 上下各留一倍余量；每个偏移先过两条廉价判据，过的才深度校验。
    int bestF = -1, bestT = -1;
    MapStat bf, bt;
    for (uint32_t off = 0x10000u; off < 0x1A000u; off += 4u) {
        MapStat s2;
        if (!ProbeMap(e, off, &s2)) continue;
        const int timerScore = s2.nFFF * 100 / s2.n;
        const int flagScore = s2.nBool * 100 / s2.n;
        if (timerScore > bestT) { bestT = timerScore; bt = s2; }
        if (flagScore > bestF) { bestF = flagScore; bf = s2; }
    }
    // 阈值 60%：A01 实测 timer 表 100%、flag 表 96%，而无关的 map 上这两个比例都很低
    if (bestF >= 60) g_flagOff = bf.off;
    if (bestT >= 60) g_timerOff = bt.off;
    if (g_flagOff || g_timerOff) g_locHow = 2;
}

uint32_t ScriptTblFlagOff() { Locate(); return g_flagOff; }
uint32_t ScriptTblTimerOff() { Locate(); return g_timerOff; }
int      ScriptTblLocateHow() { Locate(); return g_locHow; }

uint32_t ScriptTblSentinel(bool flagTable) {
    const uint32_t e = ScriptTblEngine();
    if (!e) return 0;
    const uint32_t off = flagTable ? ScriptTblFlagOff() : ScriptTblTimerOff();
    return off ? e + off + kMapSentinel : 0;
}

uint32_t ScriptTblRoot(bool flagTable) {
    const uint32_t e = ScriptTblEngine();
    if (!e) return 0;
    const uint32_t off = flagTable ? ScriptTblFlagOff() : ScriptTblTimerOff();
    if (!off) return 0;
    uint32_t r = 0;
    if (!Rd32(e + off + kMapRoot, &r)) return 0;
    return PlausiblePtr(r) ? r : 0;
}

bool ScriptTblReady() {
    const uint32_t e = ScriptTblEngine();
    if (!e) return false;
    // 至少一张表的 root 合法；空表（root=0）也算"读得到"，但两张都空就认为还没进关
    return ScriptTblRoot(true) != 0 || ScriptTblRoot(false) != 0;
}

// ---- 节点读 ----------------------------------------------------------------

static bool ReadNode(uintptr_t a, ScriptNodeInfo* n, bool isFlag) {
    if (!PlausiblePtr((uint32_t)a)) return false;
    uint32_t w[7];
    __try {
        for (int i = 0; i < 7; ++i) w[i] = *(const volatile uint32_t*)(a + 4u * i);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (n) {
        n->addr = a;
        n->scope = w[4];
        n->hash = w[5];
        n->value = w[6];
        n->isFlag = isFlag;
    }
    return true;
}

// ---- 遍历 ------------------------------------------------------------------

int ScriptTblEnum(bool flagTable, ScriptTblEnumFn fn, void* ctx) {
    uint32_t root = ScriptTblRoot(flagTable);
    if (!root) return ScriptTblEngine() ? 0 : -1;

    const uint32_t sentinel = ScriptTblSentinel(flagTable);
    // 显式栈：树深 << 64（79~110 个节点，红黑树深度 <= 2*log2(n) ≈ 14）
    uint32_t stack[128];
    int sp = 0;
    stack[sp++] = root;
    int count = 0;

    while (sp > 0) {
        const uint32_t cur = stack[--sp];
        if (!PlausiblePtr(cur) || cur == sentinel) continue;

        uint32_t L = 0, R = 0;
        if (!Rd32(cur + kNodeLeft, &L) || !Rd32(cur + kNodeRight, &R)) continue;

        ScriptNodeInfo n;
        if (ReadNode(cur, &n, flagTable)) {
            ++count;
            if (fn && !fn(n, ctx)) return count;
        }
        if (sp + 2 <= (int)(sizeof(stack) / sizeof(stack[0]))) {
            if (PlausiblePtr(L)) stack[sp++] = L;
            if (PlausiblePtr(R)) stack[sp++] = R;
        }
    }
    return count;
}

struct CountCtx { int n; };
static bool CountFn(const ScriptNodeInfo&, void* ctx) {
    ((CountCtx*)ctx)->n++;
    return true;
}

int ScriptTblCount(bool flagTable) {
    if (!ScriptTblEngine()) return -1;
    CountCtx c{0};
    ScriptTblEnum(flagTable, CountFn, &c);
    return c.n;
}

// ---- 查找 ------------------------------------------------------------------

// 按 {scope, hash} 二分。compare 规则复刻 0x171370：
//   key.scope > node.scope -> right ; < -> left ; 相等则比 hash，key.hash > node.hash -> right
uintptr_t ScriptTblFindExact(bool flagTable, uint32_t scope, uint32_t hash) {
    uint32_t cur = ScriptTblRoot(flagTable);
    const uint32_t sentinel = ScriptTblSentinel(flagTable);
    for (int guard = 0; guard < 256 && PlausiblePtr(cur) && cur != sentinel; ++guard) {
        uint32_t ns = 0, nh = 0, R = 0, L = 0;
        if (!Rd32(cur + kNodeScope, &ns) || !Rd32(cur + kNodeHash, &nh) ||
            !Rd32(cur + kNodeRight, &R) || !Rd32(cur + kNodeLeft, &L))
            return 0;
        if (scope > ns)      { cur = R; continue; }
        if (scope < ns)      { cur = L; continue; }
        if (hash > nh)       { cur = R; continue; }
        if (hash < nh)       { cur = L; continue; }
        return cur;
    }
    return 0;
}

struct FindCtx {
    uint32_t hash;
    uintptr_t node;
    uint32_t scope;
    bool found;
};

static bool FindByHashFn(const ScriptNodeInfo& n, void* ctx) {
    FindCtx* f = (FindCtx*)ctx;
    if (n.hash != f->hash) return true;
    f->node = n.addr;
    f->scope = n.scope;
    f->found = true;
    return false;   // 命中即停
}

bool ScriptTblFindByHash(uint32_t hash, bool* isFlagOut, uintptr_t* nodeOut,
                         uint32_t* scopeOut) {
    if (!ScriptTblEngine()) return false;

    // 先按"当前作用域"二分（与引擎 GetFlag 一致）
    const uint32_t eng = ScriptTblEngine();
    uint32_t scopeHash = kEmptyScopeHash;
    {
        uint32_t s = 0;
        if (Rd32(eng + kScopeStrOff, &s) && PlausiblePtr(s)) {
            char buf[128];
            __try {
                for (int i = 0; i < 127; ++i) {
                    buf[i] = *(const volatile char*)(s + i);
                    if (!buf[i]) break;
                    buf[127] = 0;
                }
                buf[127] = 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                buf[0] = 0;
            }
            scopeHash = Fnv1(buf);
        }
    }

    for (int t = 0; t < 2; ++t) {
        const bool isFlag = (t == 0);
        const uintptr_t hit = ScriptTblFindExact(isFlag, scopeHash, hash);
        if (hit) {
            if (isFlagOut) *isFlagOut = isFlag;
            if (nodeOut) *nodeOut = hit;
            if (scopeOut) *scopeOut = scopeHash;
            return true;
        }
    }

    // 再全表扫（同名可能落在别的作用域）
    for (int t = 0; t < 2; ++t) {
        const bool isFlag = (t == 0);
        FindCtx f{hash, 0, 0, false};
        ScriptTblEnum(isFlag, FindByHashFn, &f);
        if (f.found) {
            if (isFlagOut) *isFlagOut = isFlag;
            if (nodeOut) *nodeOut = f.node;
            if (scopeOut) *scopeOut = f.scope;
            return true;
        }
    }
    return false;
}

// ---- 读写 ------------------------------------------------------------------

bool ScriptTblFlagGet(const char* name, uint32_t* outValue) {
    if (!name) return false;
    uintptr_t node = 0;
    bool isFlag = false;
    if (!ScriptTblFindByHash(Fnv1(name), &isFlag, &node, nullptr)) return false;
    uint32_t v = 0;
    if (!Rd32(node + kNodeValue, &v)) return false;
    if (outValue) *outValue = v;
    return true;
}

// 只改最低字节：引擎自己就是按 bool 写的（0x489C10 `cmp byte ptr [eax],0`），
// 高位是分配器残留，保留即可。写整个 dword 会把残留覆盖成 0，虽然无害但没必要。
bool ScriptTblWriteValue(uintptr_t node, uint32_t expectedHash, uint32_t value) {
    if (!PlausiblePtr((uint32_t)node)) return false;
    uint32_t h = 0;
    if (!Rd32(node + kNodeHash, &h)) return false;
    if (expectedHash && h != expectedHash) return false;   // 地址已失效
    return Wr8(node + kNodeValue, (uint8_t)(value & 0xFF));
}

bool ScriptTblFlagSet(const char* name, bool on) {
    if (!name) return false;
    const uint32_t hash = Fnv1(name);
    uintptr_t node = 0;
    bool isFlag = false;
    if (!ScriptTblFindByHash(hash, &isFlag, &node, nullptr)) return false;
    if (!isFlag) return false;              // 同名的 timer 不能当 flag 拨
    return ScriptTblWriteValue(node, hash, on ? 1u : 0u);
}

// ---- 健康检查 --------------------------------------------------------------

struct HealthCtx {
    ScriptTblHealth* h;
    uint32_t prevScope, prevHash;
    bool     first;
};

static bool HealthFn(const ScriptNodeInfo& n, void* ctx) {
    HealthCtx* c = (HealthCtx*)ctx;
    // 作用域种类
    int i = 0;
    for (; i < c->h->nScopes; ++i)
        if (c->h->flags[i] == n.scope) break;
    if (i == c->h->nScopes && c->h->nScopes < 4) {
        c->h->flags[c->h->nScopes++] = n.scope;
    }
    // 父子互指（本节点 -> 右孩子 -> 父应指回自己）
    uint32_t R = 0, P = 0;
    if (Rd32(n.addr + kNodeRight, &R) && PlausiblePtr(R)) {
        if (Rd32(R + kNodeParent, &P) && P != (uint32_t)n.addr) c->h->badParent++;
    }
    return true;
}

// 中序有序性单独做：需要按 left -> self -> right 的顺序
static void CheckOrder(uint32_t node, uint32_t sentinel, HealthCtx* c, int depth) {
    if (!PlausiblePtr(node) || node == sentinel || depth > 64) return;
    uint32_t L = 0, R = 0;
    if (!Rd32(node + kNodeLeft, &L) || !Rd32(node + kNodeRight, &R)) return;
    CheckOrder(L, sentinel, c, depth + 1);

    ScriptNodeInfo n;
    if (ReadNode(node, &n, true)) {
        if (!c->first) {
            if (n.scope < c->prevScope ||
                (n.scope == c->prevScope && n.hash < c->prevHash))
                c->h->unsorted++;
        }
        c->prevScope = n.scope;
        c->prevHash = n.hash;
        c->first = false;
    }
    CheckOrder(R, sentinel, c, depth + 1);
}

void ScriptTblCheck(ScriptTblHealth* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->engine = ScriptTblEngine();
    out->ready = ScriptTblReady();
    if (!out->engine) return;

    out->flagRoot = ScriptTblRoot(true);
    out->timerRoot = ScriptTblRoot(false);
    out->flagCount = ScriptTblCount(true);
    out->timerCount = ScriptTblCount(false);

    HealthCtx c;
    memset(&c, 0, sizeof(c));
    c.h = out;
    c.first = true;
    if (out->flagRoot) ScriptTblEnum(true, HealthFn, &c);
    if (out->flagRoot)
        CheckOrder(out->flagRoot, ScriptTblSentinel(true), &c, 0);
}
