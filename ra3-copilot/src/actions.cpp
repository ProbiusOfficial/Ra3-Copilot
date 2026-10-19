// actions.cpp - 对游戏对象的直接操作
//
// 全部 RVA / 结构偏移：基于 CE 修改器的地址表，并对本机构建逐字节验证：
//   玩家/选中管理器 MOD+0x8DB73C  -> mgr;  mgr+0x50 = 选中链表头, mgr+0x5c = 选中数量
//   对象模板库     MOD+0x8E6C58  -> store
//   当前对象哈希   MOD+0x8E9838
//   模板查找       MOD+0x3E4230  this=store, arg=hash -> template*
//   创建单位       MOD+0x205240  cdecl(0, template, float* pos, team, team+0x10) -> object*
//   单位升级(满级) MOD+0x35C200  ecx=unit, cdecl(3, 1, 0)
//   击杀选中       MOD+0x39EA50  ecx=unit, cdecl(6, 19, 0)
//   鼠标地图坐标   MOD+0x1ED4A0
//   免费建造开关   MOD+0x0E7565  75 0C -> EB 0C
//   单位字段：+0x138 = body;  body+0x33c = 血量体(+4 当前/+0xc 上限/+0x10 备份)
//             body+0x374 -> +0x200 -> 速度子节点( +8 速度 / +0x40 备份 )
//             body+0x418 = 归属玩家对象
#include "actions.h"
#include "features.h"
#include "overlay.h"
#include "symres.h"
#include "units_corona.h"
#include "aicmd.h"        // OrderAttack / OrderMoveTo / OrderIdle（/order 指挥单位）
#include <windows.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdarg>

namespace ra3 {

// actions 侧日志：格式化后送进界面「输出记录」
static void ActionLog(int kind, const char* fmt, ...) {
    char buf[176];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    OverlayLogLine(kind, buf);
}
#define R3_LOG ActionLog

uintptr_t GameBase() { return reinterpret_cast<uintptr_t>(::GetModuleHandleA(nullptr)); }

// 崩溃面包屑写入（定义在文件末尾、匿名空间之外 —— overlay 的崩溃处理器共用）
void CrashLogAppend(const char* line);
char* CrashLogPath();

// ---- 落点来源（GetSpawnPos 与 /summon?place= 共用）----
enum { PLACE_VIEW = 0, PLACE_SELECTED = 1, PLACE_CURSOR = 2, PLACE_NONE = -1 };

// 一次性落点来源覆盖：HTTP 线程写、渲染线程取用即清。
//   给 /summon?place=view|selection|cursor 用 —— 让调用方"点名要哪种落点"，
//   而不是自己编世界坐标（编出来的点落在水面，单位就生成在水面）。
static volatile LONG g_placeOv = PLACE_NONE;
void SetSummonPlace(int mode) {
    InterlockedExchange(&g_placeOv, (mode >= PLACE_VIEW && mode <= PLACE_CURSOR) ? mode : PLACE_NONE);
}

namespace {

constexpr uint32_t R_CURSORPOS   = 0x8DAEFC;
constexpr uint32_t R_CURTEMPLATE = 0x8E9838;

constexpr uint32_t OFF_BODY      = 0x138;
constexpr uint32_t OFF_HPBODY    = 0x33c;
constexpr uint32_t OFF_OWNER     = 0x418;
constexpr uint32_t OFF_LOCO      = 0x374;   // body -> locoMgr
constexpr uint32_t OFF_LOCO_NODE = 0x200;   // locoMgr -> node array

typedef void* (__fastcall* TemplateFind_t)(void* store, void* edx, uint32_t hash);
typedef void* (__cdecl*    CreateUnit_t)(int a0, void* tmpl, float* pos, void* team, void* teamMember);
typedef void  (__cdecl*    TerrainSnap_t)(float* src, float* dst, int a, int b);
// 屏幕像素 -> 世界坐标：src = 客户区像素 (x, y, 上下文) 三个 dword，dst = 命中地形的世界坐标
// （含高度）。实测第 3 个 dword 不参与运算，传 0 即可。
typedef void  (__cdecl*    ScreenToWorld_t)(void* src, void* dst, int a, int b);
typedef void  (__fastcall* LevelUp_t)(void* unit, void* edx, int a, int b, int c);
typedef void  (__fastcall* KillUnit_t)(void* body, void* edx, int a, int b, int c);   // 0x39EA50 thiscall(body,6,19,0)

// 安全读指针（SEH 包裹）
bool SafeRead32(uintptr_t at, uint32_t& out) {
    __try { out = *reinterpret_cast<uint32_t*>(at); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SafeReadBytes(uintptr_t at, void* dst, size_t n) {
    __try { std::memcpy(dst, reinterpret_cast<void*>(at), n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

char g_lastErr[160] = { 0 };
void SetErr(const char* s) { strncpy_s(g_lastErr, s, _TRUNCATE); }

// ===================== 安全护栏（降低闪退风险） =====================
// 原则：**任何写之前先证明这个地址是安全的**
//   ① VirtualQuery：已提交、可写、非 GUARD/NOACCESS，且整段在同一区域
//   ② 对象指针：[obj] 的 vtable 必须落在主模块内（说明是游戏对象，不是野指针/数值）
//   ③ 数值合理性：改 HP/速度前先看现值是否在合理范围（不合理 = 认错对象，跳过）
//   ④ dry-run：只记录不改内存
bool g_dryRun = false;
bool g_noGameCall = false;      // 禁止调用游戏函数（召唤/满级走游戏内部，护栏管不到内部状态）
int  g_guardSkip = 0;
int  g_guardDone = 0;

// 带坐标的取证（CreateUnit 用）
void CrashMarkPos(const char* what, float x, float y, float z, uintptr_t a1, uintptr_t a2) {
    __try {
        SYSTEMTIME st; GetLocalTime(&st);
        char buf[288];
        int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                            "[%02d:%02d:%02d.%03d] %-12s pos=%.1f,%.1f,%.1f a1=%08X a2=%08X\r\n",
                            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                            what, x, y, z, (unsigned)a1, (unsigned)a2);
        if (n > 0) CrashLogAppend(buf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ===================== 反馈音（"叮"）定义见文件下方 ra3 命名空间 =====================

bool IsMemOk(uintptr_t p, size_t n, bool writable) {
    // 上限必须是 4GB：WOW64 下 32 位进程有完整 4GB 用户空间，游戏对象实测在 0x9Exxxxxx！
    //   曾误用 0x7FFFFFFF → 把所有合法对象判成非法 → 复制/满级/收编全废。
    if (!p || p < 0x00010000u || p > 0xFFF00000u) return false;
    if (!n) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD prot = mbi.Protect;
    if (prot & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    if (writable && (prot & (PAGE_READONLY | PAGE_EXECUTE_READ))) return false;
    if (reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize < p + n) return false;
    return true;
}

// [obj] 的 vtable 落在主模块内 ⇒ 基本可确认是游戏对象
bool LooksLikeGameObject(uintptr_t obj) {
    uint32_t vt = 0;
    if (!IsMemOk(obj, 4, false)) return false;
    if (!SafeRead32(obj, vt)) return false;
    const uintptr_t base = GameBase();
    if (!base) return false;
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
    return vt >= base && vt < end;
}

// 数值合理性（防"认错对象就乱写"）
bool PlausibleFloat(float f, float lo, float hi) {
    if (!(f == f)) return false;      // NaN
    return f >= lo && f <= hi;
}

bool SafeWrite32(uintptr_t p, uint32_t v) {
    if (g_dryRun) { ++g_guardSkip; return false; }
    if (!IsMemOk(p, 4, true)) { ++g_guardSkip; return false; }
    __try { *reinterpret_cast<uint32_t*>(p) = v; ++g_guardDone; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { ++g_guardSkip; return false; }
}

bool SafeWriteFloat(uintptr_t p, float v) {
    if (g_dryRun) { ++g_guardSkip; return false; }
    if (!IsMemOk(p, 4, true)) { ++g_guardSkip; return false; }
    __try { *reinterpret_cast<float*>(p) = v; ++g_guardDone; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { ++g_guardSkip; return false; }
}

bool SafeReadFloat(uintptr_t p, float& v) {
    if (!IsMemOk(p, 4, false)) return false;
    __try { v = *reinterpret_cast<float*>(p); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ===================== 崩溃取证（面包屑落盘） =====================
// 每个"危险动作"（调游戏函数 / 写关键字段）之前，把标签追加写进 ra3copilot_last.txt。
// 游戏崩了之后看这个文件**末尾几行** = 最后做过哪几个动作 —— 比猜快得多。
void CrashMark(const char* what, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    __try {
        SYSTEMTIME st; GetLocalTime(&st);
        char buf[288];
        int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                            "[%02d:%02d:%02d.%03d] %-12s a1=%08X a2=%08X a3=%08X\r\n",
                            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                            what, (unsigned)a1, (unsigned)a2, (unsigned)a3);
        if (n > 0) CrashLogAppend(buf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// 目标地址是否像一个函数入口（防止地址与当前构建不符时盲调 → 崩）
bool LooksLikeFuncEntry(uintptr_t a) {
    uint8_t b[2] = { 0, 0 };
    if (!a || !SafeReadBytes(a, b, 2)) return false;
    if (b[0] == 0x55) return true;                              // push ebp
    if (b[0] == 0x83 && b[1] == 0xEC) return true;              // sub esp,imm8
    if (b[0] == 0x81 && b[1] == 0xEC) return true;              // sub esp,imm32
    if (b[0] == 0x56 || b[0] == 0x57 || b[0] == 0x53 || b[0] == 0x51) return true;
    if (b[0] == 0x8B && b[1] == 0xFF) return true;              // mov edi,edi
    if (b[0] == 0x8B && b[1] == 0xC0) return true;              // mov eax,eax
    if (b[0] == 0x6A && b[1] == 0xFF) return true;              // push -1 (SEH)
    if (b[0] == 0xE9) return true;                              // jmp (thunk)
    if (b[0] == 0xFF) return true;                              // jmp [..] thunk
    return false;
}

// 选中链表迭代
// 主来源：选中管理器（symres 解析，+0x50 链表头 / +0x5c 数量）
// 兜底：弹药 hook 遍历选中单位时抓到的 g_selUnit（解析失败时用它）
struct SelIter { int count; uintptr_t node; uintptr_t single; };

bool SelFresh() { return g_selUnit != 0 && (g_tickNow - g_selTick) < 600; }

bool SelBegin(SelIter& it) {
    it.count = 0; it.node = 0; it.single = 0;
    uintptr_t mgrGlob = SymSelMgr();          // 全局变量地址（其值才是管理器）
    uint32_t mgr = 0;
    if (mgrGlob && SafeRead32(mgrGlob, mgr) && mgr) {
        uint32_t count = 0, node = 0;
        if (SafeRead32(mgr + 0x5c, count) && SafeRead32(mgr + 0x50, node) && count > 0 && node) {
            it.count = static_cast<int>(count);
            it.node = node;
            return true;
        }
    }
    if (SelFresh()) { it.single = g_selUnit; it.count = 1; return true; }
    return false;
}

uintptr_t SelNext(SelIter& it) {
    if (it.single) { uintptr_t o = it.single; it.single = 0; return o; }
    while (it.count > 0 && it.node) {
        --it.count;
        uint32_t obj = 0;
        uintptr_t next = 0;
        SafeRead32(it.node + 8, obj);
        SafeRead32(it.node, next);
        it.node = next;
        if (obj) return obj;
    }
    return 0;
}

// 归一化到 Thing —— 所有单位操作都从这里拿"body"。
//   Drawable（原生单位）：[obj+0x138] 才是 Thing；
//   Thing（CreateUnit 返回的召唤物）：自身即 Thing，其 +0x138 是浮点值，
//   判据改用 Thing 专有的血量体指针 [+0x33C]。
//   只走前者时对召唤物恒定返回 0 → 定住/血量/收编/摧毁等全部静默跳过。
uintptr_t BodyOf(uintptr_t obj) {
    if (!LooksLikeGameObject(obj)) return 0;
    uint32_t b = 0;
    if (SafeRead32(obj + OFF_BODY, b) && IsMemOk(b, 0x44, false)) return b;   // 够读 +0x38/+0x3c/+0x40 即可
    uint32_t hp = 0;
    if (SafeRead32(obj + OFF_HPBODY, hp) && hp && IsMemOk(hp, 0x14, false)) return obj;
    return 0;
}

// 速度档位（引擎读取 +8 处的速度因子）
// 定住必须写 0 —— 0 即"速度为零"，单位完全静止；写非零小值（如 0.01）单位仍会缓慢移动。
//   与 CE SelectUnitFreeze 写 00000000(float_0) 一致。
constexpr float kSpeedFast   = 500.0f;
constexpr float kSpeedSlow   = 10.0f;
constexpr float kSpeedFreeze = 0.0f;

// 原速备份表：键 = 速度节点地址，值 = 我们动手前的速度。
// 为什么自己存、不用节点的 +0x40 槽：+0x40 那套备份靠"当前值是不是我们写的档位"反推
//   "这个节点有没有被改过"。滑条是连续值，第二次拖动时这个判据就没了 →
//   会把上一次设的值当成原速备份进去，此后"恢复原速"只会还原成上个滑条值。
// 条目在恢复时删除；表满覆盖最旧一条（节点地址可能被回收复用，但写回的是个合理速度值，无害）。
struct SpeedBak { uintptr_t node; float orig; };
constexpr int kSpeedBakMax = 256;
SpeedBak g_speedBak[kSpeedBakMax] = {};
int      g_speedBakNext = 0;

float* SpeedBakFind(uintptr_t n) {
    if (!n) return nullptr;
    for (int i = 0; i < kSpeedBakMax; ++i)
        if (g_speedBak[i].node == n) return &g_speedBak[i].orig;
    return nullptr;
}
void SpeedBakPut(uintptr_t n, float orig) {
    if (float* e = SpeedBakFind(n)) { *e = orig; return; }
    g_speedBak[g_speedBakNext].node = n;
    g_speedBak[g_speedBakNext].orig = orig;
    g_speedBakNext = (g_speedBakNext + 1) % kSpeedBakMax;
}
void SpeedBakDel(uintptr_t n) {
    for (int i = 0; i < kSpeedBakMax; ++i)
        if (g_speedBak[i].node == n) { g_speedBak[i].node = 0; g_speedBak[i].orig = 0.f; }
}

// 速度子节点处理：写入目标速度；首次改这个节点时把原值记进备份表
void SetSpeedNode(uintptr_t n, float v) {
    if (!n) return;
    float cur = 0;
    if (!IsMemOk(n, 0x44, true)) return;
    __try { cur = *reinterpret_cast<float*>(n + 8); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (cur == v) return;                                  // 已是目标值，不必写
    // 护栏：现值必须是合理的速度（0..1e5），否则认错对象 → 不写
    if (!PlausibleFloat(cur, 0.0f, 1.0e5f)) { ++g_guardSkip; return; }
    if (!SpeedBakFind(n)) SpeedBakPut(n, cur);             // 首次改动才记原值
    SafeWriteFloat(n + 8, v);
}

// 恢复我们改过的节点；没改过的不动
void RestoreSpeedNode(uintptr_t n) {
    if (!n) return;
    float* orig = SpeedBakFind(n);
    if (!orig) return;
    if (IsMemOk(n, 0x44, true)) SafeWriteFloat(n + 8, *orig);
    SpeedBakDel(n);
}

void SpeedNodes(uintptr_t body, uintptr_t& n1, uintptr_t& n2) {
    n1 = n2 = 0;
    uint32_t loco = 0, arr = 0;
    if (!SafeRead32(body + OFF_LOCO, loco) || !loco) return;
    if (!SafeRead32(loco + OFF_LOCO_NODE, arr) || !arr) return;
    uint32_t a = 0, b = 0;
    SafeRead32(arr, a);
    SafeRead32(arr + 4, b);
    if (a) { float f = 0; __try { f = *reinterpret_cast<float*>(a + 8); } __except (EXCEPTION_EXECUTE_HANDLER) { f = 0; } (void)f; n1 = a; }
    // 基于 CE：第二节点要求 [n2+0x18] == 1.0f（标志位）
    if (b) {
        float flag = 0;
        __try { flag = *reinterpret_cast<float*>(b + 0x18); } __except (EXCEPTION_EXECUTE_HANDLER) { flag = 0; }
        if (flag == 1.0f) n2 = b;
    }
}

// 落点候选 1：首个选中单位的位置
bool TrySelectedPos(float out[4]) {
    SelIter it;
    if (!SelBegin(it)) return false;
    uintptr_t obj = SelNext(it);
    if (!obj) return false;
    uintptr_t body = BodyOf(obj);
    if (!body) return false;
    uint32_t x = 0, y = 0, z = 0;
    if (!SafeRead32(body + 0x38, x) || !SafeRead32(body + 0x3c, y) || !SafeRead32(body + 0x40, z))
        return false;
    std::memcpy(&out[0], &x, 4);
    std::memcpy(&out[1], &y, 4);
    std::memcpy(&out[2], &z, 4);
    // 近原点(全 0/极小)判定为无效——实测某些对象类型这里不是坐标
    if (out[0] * out[0] + out[1] * out[1] + out[2] * out[2] < 1.0f) return false;
    return true;
}

// 落点候选 2：游戏光标所在的world坐标
bool TryCursorPos(float out[4]) {
    uint32_t base = static_cast<uint32_t>(GameBase());
    uint32_t cur = 0;
    if (!SafeRead32(base + R_CURSORPOS, cur) || !cur) return false;
    uint32_t x = 0, y = 0, z = 0;
    if (!SafeRead32(cur + 0x3c, x) || !SafeRead32(cur + 0x40, y) || !SafeRead32(cur + 0x44, z))
        return false;
    std::memcpy(&out[0], &x, 4);
    std::memcpy(&out[1], &y, 4);
    std::memcpy(&out[2], &z, 4);
    return true;
}

// 坐标是否可信（世界坐标量级，且不能接近原点 —— 实测"地图原点"就是全 0，是最典型的坏值）
bool PosLooksValid(const float p[4]) {
    for (int i = 0; i < 3; ++i) {
        if (!(p[i] == p[i])) return false;                 // NaN
        if (p[i] > 1.0e6f || p[i] < -1.0e6f) return false;
    }
    if (fabsf(p[0]) + fabsf(p[1]) + fabsf(p[2]) < 1.0f) return false;   // 近原点一律不可信
    return true;
}

// ---- 4x4 矩阵工具（行向量约定：v' = v * M）----
void Mat4Mul(const float a[16], const float b[16], float o[16]) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            o[i * 4 + j] = a[i * 4 + 0] * b[0 + j] + a[i * 4 + 1] * b[4 + j] +
                           a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
}

bool Mat4Inv(const float m[16], float inv[16]) {
    const float* a = m;
    float det =
        a[0] * (a[5] * (a[10] * a[15] - a[11] * a[14]) - a[9] * (a[6] * a[15] - a[7] * a[14]) + a[13] * (a[6] * a[11] - a[7] * a[10])) -
        a[4] * (a[1] * (a[10] * a[15] - a[11] * a[14]) - a[9] * (a[2] * a[15] - a[3] * a[14]) + a[13] * (a[2] * a[11] - a[3] * a[10])) +
        a[8] * (a[1] * (a[6] * a[15] - a[7] * a[14]) - a[5] * (a[2] * a[15] - a[3] * a[14]) + a[13] * (a[2] * a[7] - a[3] * a[6])) -
        a[12] * (a[1] * (a[6] * a[11] - a[7] * a[10]) - a[5] * (a[2] * a[11] - a[3] * a[10]) + a[9] * (a[2] * a[7] - a[3] * a[6]));
    if (det > -1e-12f && det < 1e-12f) return false;
    const float id = 1.f / det;
    float t[16];
    t[0]  =  (a[5] * (a[10]*a[15]-a[11]*a[14]) - a[9] * (a[6]*a[15]-a[7]*a[14]) + a[13] * (a[6]*a[11]-a[7]*a[10])) * id;
    t[1]  = -(a[1] * (a[10]*a[15]-a[11]*a[14]) - a[9] * (a[2]*a[15]-a[3]*a[14]) + a[13] * (a[2]*a[11]-a[3]*a[10])) * id;
    t[2]  =  (a[1] * (a[6]*a[15]-a[7]*a[14]) - a[5] * (a[2]*a[15]-a[3]*a[14]) + a[13] * (a[2]*a[7]-a[3]*a[6])) * id;
    t[3]  = -(a[1] * (a[6]*a[11]-a[7]*a[10]) - a[5] * (a[2]*a[11]-a[3]*a[10]) + a[9] * (a[2]*a[7]-a[3]*a[6])) * id;
    t[4]  = -(a[4] * (a[10]*a[15]-a[11]*a[14]) - a[8] * (a[6]*a[15]-a[7]*a[14]) + a[12] * (a[6]*a[11]-a[7]*a[10])) * id;
    t[5]  =  (a[0] * (a[10]*a[15]-a[11]*a[14]) - a[8] * (a[2]*a[15]-a[3]*a[14]) + a[12] * (a[2]*a[11]-a[3]*a[10])) * id;
    t[6]  = -(a[0] * (a[6]*a[15]-a[7]*a[14]) - a[4] * (a[2]*a[15]-a[3]*a[14]) + a[12] * (a[2]*a[7]-a[3]*a[6])) * id;
    t[7]  =  (a[0] * (a[6]*a[11]-a[7]*a[10]) - a[4] * (a[2]*a[11]-a[3]*a[10]) + a[8] * (a[2]*a[7]-a[3]*a[6])) * id;
    t[8]  =  (a[4] * (a[9]*a[15]-a[11]*a[13]) - a[8] * (a[5]*a[15]-a[7]*a[13]) + a[12] * (a[5]*a[11]-a[7]*a[9])) * id;
    t[9]  = -(a[0] * (a[9]*a[15]-a[11]*a[13]) - a[8] * (a[1]*a[15]-a[3]*a[13]) + a[12] * (a[1]*a[11]-a[3]*a[9])) * id;
    t[10] =  (a[0] * (a[5]*a[15]-a[7]*a[13]) - a[4] * (a[1]*a[15]-a[3]*a[13]) + a[12] * (a[1]*a[7]-a[3]*a[5])) * id;
    t[11] = -(a[0] * (a[5]*a[11]-a[7]*a[9]) - a[4] * (a[1]*a[11]-a[3]*a[9]) + a[8] * (a[1]*a[7]-a[3]*a[5])) * id;
    t[12] = -(a[4] * (a[9]*a[14]-a[10]*a[13]) - a[8] * (a[5]*a[14]-a[6]*a[13]) + a[12] * (a[5]*a[10]-a[6]*a[9])) * id;
    t[13] =  (a[0] * (a[9]*a[14]-a[10]*a[13]) - a[8] * (a[1]*a[14]-a[2]*a[13]) + a[12] * (a[1]*a[10]-a[2]*a[9])) * id;
    t[14] = -(a[0] * (a[5]*a[14]-a[6]*a[13]) - a[4] * (a[1]*a[14]-a[2]*a[13]) + a[12] * (a[1]*a[6]-a[2]*a[5])) * id;
    t[15] =  (a[0] * (a[5]*a[10]-a[6]*a[9]) - a[4] * (a[1]*a[10]-a[2]*a[9]) + a[8] * (a[1]*a[6]-a[2]*a[5])) * id;
    memcpy(inv, t, sizeof(t));
    return true;
}

// 用 v(屏幕NDC) * invM 得到世界点
bool Unproject(const float invM[16], float nx, float ny, float nz, float out[4]) {
    const float v[4] = { nx, ny, nz, 1.f };
    float r[4];
    for (int j = 0; j < 4; ++j)
        r[j] = v[0] * invM[0 * 4 + j] + v[1] * invM[1 * 4 + j] +
               v[2] * invM[2 * 4 + j] + v[3] * invM[3 * 4 + j];
    if (r[3] > -1e-9f && r[3] < 1e-9f) return false;
    const float iw = 1.f / r[3];
    out[0] = r[0] * iw; out[1] = r[1] * iw; out[2] = r[2] * iw; out[3] = 1.f;
    return true;
}

// 落点候选 0：**鼠标光标处**
// 读 [0x8DAEFC] 对象的 +0x3c/+0x40/+0x44（原样 3 个 dword），调用游戏自己的换算函数
//   0x1ED4A0(src, dst, 0, 0)，结果即"鼠标位置的地图世界坐标"。
// ⚠️ 该对象只在鼠标位于游戏窗口内时才刷新 —— 玩家在别的窗口（例如与 AI 对话）时它停在旧值，
//   走这条路会投到"上次鼠标停留处"。远程操作场景请优先用视野中心或选中单位。
bool TryMouseWorldPos(float out[4]) {
    const uintptr_t base = GameBase();
    const uintptr_t fn = SymMouseMap();          // = 0x1ED4A0
    if (!fn) return false;
    uint32_t cur = 0;
    if (!SafeRead32(base + R_CURSORPOS, cur) || !cur) return false;
    float src[4] = { 0, 0, 0, 0 }, dst[4] = { 0, 0, 0, 0 };
    // 原样拷贝 3 个 dword（格式由游戏函数决定，别当 float 解读）
    { uint32_t v = 0; if (SafeRead32(cur + 0x3c, v)) std::memcpy(&src[0], &v, 4); }
    { uint32_t v = 0; if (SafeRead32(cur + 0x40, v)) std::memcpy(&src[1], &v, 4); }
    { uint32_t v = 0; if (SafeRead32(cur + 0x44, v)) std::memcpy(&src[2], &v, 4); }
    __try {
        reinterpret_cast<ScreenToWorld_t>(fn)(src, dst, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    out[0] = dst[0]; out[1] = dst[1]; out[2] = dst[2]; out[3] = 1.f;
    return PosLooksValid(out);
}

// 视野中心：把游戏客户区**正中**的像素喂给同一个换算函数 0x1ED4A0。
//   不用抓 d3d9 视图矩阵做反投影 —— 实测游戏 SetTransform(D3DTS_VIEW) 传的全是单位矩阵
//   （34530 次调用、_33 恒为 1.0），3D 相机不走那条通道，反投影永远失败；
//   换算函数这条路实测可用，且输出天然带地形高度（水面就是水面高度）。
//   与光标无关：不依赖 [0x8DAEFC]，游戏窗口失焦也照样算得对。
bool TryViewCenterPos(float out[4]) {
    const uintptr_t fn = SymMouseMap();
    if (!fn) return false;
    HWND h = GameWindow();
    RECT r = { 0, 0, 0, 0 };
    if (!h || !IsWindow(h) || !GetClientRect(h, &r)) return false;
    const int cw = r.right - r.left, ch = r.bottom - r.top;
    if (cw < 64 || ch < 64) return false;            // 窗口未就绪/最小化
    uint32_t src[3] = { (uint32_t)(cw / 2), (uint32_t)(ch / 2), 0 };
    float dst[4] = { 0, 0, 0, 0 };
    __try {
        reinterpret_cast<ScreenToWorld_t>(fn)(src, dst, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    out[0] = dst[0]; out[1] = dst[1]; out[2] = dst[2]; out[3] = 1.f;
    return PosLooksValid(out);
}

// 落点：按设置选来源；都取不到时用"上次成功的位置"
float g_lastPos[4] = { 0, 0, 0, 1 };
bool  g_hasLastPos = false;

// 落点来源（诊断用）
const char* g_posSource = "?";
char g_posDiag[160] = { 0 };      // 逐来源尝试结果，如 "光标× 视野× 选中×"

// 落点来源编号见文件上方（PLACE_VIEW / PLACE_SELECTED / PLACE_CURSOR）

static void Note(const char* nm, bool ok) {
    char b[32];
    _snprintf_s(b, sizeof(b), _TRUNCATE, "%s%s ", nm, ok ? "√" : "×");
    strncat_s(g_posDiag, sizeof(g_posDiag), b, _TRUNCATE);
}
static void NoteBad(const char* nm) {
    char b[40];
    _snprintf_s(b, sizeof(b), _TRUNCATE, "%s(值非法) ", nm);
    strncat_s(g_posDiag, sizeof(g_posDiag), b, _TRUNCATE);
}

bool GetSpawnPos(float out[4], bool atMouse) {
    float tmp[4] = { 0, 0, 0, 1 };
    bool ok = false; const char* src = "none";
    g_posDiag[0] = 0;
    auto trySrc = [&](bool (*fn)(float*), const char* nm) -> bool {
        const bool got = fn(tmp);
        const bool r = got && PosLooksValid(tmp);
        if (got && !r) NoteBad(nm); else Note(nm, r);
        return r;
    };
    auto tryOne = [&](int m) -> bool {
        if (m == PLACE_VIEW) {
            if (trySrc(TryViewCenterPos, "视野")) { ok = true; src = "视野中心"; return true; }
        } else if (m == PLACE_SELECTED) {
            if (trySrc(TrySelectedPos, "选中"))   { ok = true; src = "选中单位"; return true; }
        } else if (trySrc(TryMouseWorldPos, "光标")) { ok = true; src = "光标"; return true; }
        return false;
    };
    // 显式指定（place=）优先；失败则回退到按设置的全序列
    const int ov = (int)InterlockedExchange(&g_placeOv, PLACE_NONE);
    if (ov != PLACE_NONE) tryOne(ov);
    if (!ok) {
        if (atMouse)                     { tryOne(PLACE_CURSOR) || tryOne(PLACE_VIEW) || tryOne(PLACE_SELECTED); }
        else if (g_cfg.posMode == 1)     { tryOne(PLACE_SELECTED) || tryOne(PLACE_VIEW) || tryOne(PLACE_CURSOR); }
        else if (g_cfg.posMode == 2)     { tryOne(PLACE_CURSOR) || tryOne(PLACE_VIEW) || tryOne(PLACE_SELECTED); }
        else                             { tryOne(PLACE_VIEW) || tryOne(PLACE_SELECTED) || tryOne(PLACE_CURSOR); }
    }
    if (ok && PosLooksValid(tmp)) {
        std::memcpy(out, tmp, sizeof(tmp));
        std::memcpy(g_lastPos, tmp, sizeof(tmp));
        g_hasLastPos = true; g_posSource = src;
        return true;
    }
    if (g_hasLastPos) { std::memcpy(out, g_lastPos, sizeof(g_lastPos)); g_posSource = "缓存"; return true; }
    return false;
}

void* FindTemplate(uint32_t hash) {
    uintptr_t g = SymThingFactory();
    uint32_t store = 0;
    if (!g || !SafeRead32(g, store) || !store) return nullptr;
    uintptr_t fnAddr = SymTmplFind();
    if (!fnAddr) return nullptr;
    void* tmpl = nullptr;
    __try {
        auto fn = reinterpret_cast<TemplateFind_t>(fnAddr);
        tmpl = fn(reinterpret_cast<void*>(store), nullptr, hash);
    } __except (EXCEPTION_EXECUTE_HANDLER) { tmpl = nullptr; }
    return tmpl;
}

constexpr uint32_t kSuperWeaponSkip[] = {
    0x95D6E965u, 0xFD87E82Au, 0x856C9DD6u, 0xD20552E1u, 0x1AFC9A6Eu, 0xD6D22475u
};

} // namespace

// ---- 危险动作流水账（崩溃取证；定义在匿名空间之外，供 overlay 的崩溃处理器共用）----
// 路径 = <DLL 目录>\ra3copilot_last.txt
// 追加写而不是覆盖：崩了之后**末尾几行就是"死前动作序列"**；超过 64 KB 自动清空重来。
char* CrashLogPath() {
    static char dir[MAX_PATH] = { 0 };
    if (!dir[0]) {
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&CrashLogPath), &self);
        GetModuleFileNameA(self, dir, MAX_PATH);
        char* p = strrchr(dir, '\\');
        if (p) *(p + 1) = 0;
        strncat_s(dir, MAX_PATH, "ra3copilot_last.txt", _TRUNCATE);
    }
    return dir;
}

void CrashLogAppend(const char* line) {
    char* path = CrashLogPath();
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &fad) &&
        (fad.nFileSizeHigh > 0 || fad.nFileSizeLow > 65536))
        DeleteFileA(path);
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(h, line, (DWORD)strlen(line), &wr, nullptr);
    CloseHandle(h);
}

bool TemplateStoreReady() {
    uintptr_t g = SymThingFactory();
    uint32_t store = 0;
    return g && SafeRead32(g, store) && store != 0;
}

int SelectedCount() {
    SelIter it;
    if (!SelBegin(it)) { SetErr("没有选中单位（先在游戏里选中）"); return 0; }
    return it.count;
}
int ActSpeed(int mode) {
    SelIter it;
    if (!SelBegin(it)) { SetErr("没有选中单位（先在游戏里选中）"); return 0; }
    int n = 0;
    uintptr_t obj;
    while ((obj = SelNext(it)) != 0) {
        uintptr_t body = BodyOf(obj);
        if (!body) continue;
        uintptr_t n1 = 0, n2 = 0;
        SpeedNodes(body, n1, n2);
        if (mode == 3) { RestoreSpeedNode(n1); RestoreSpeedNode(n2); }
        else {
            float v = (mode == 0) ? kSpeedFast : (mode == 1) ? kSpeedSlow : kSpeedFreeze;
            SetSpeedNode(n1, v);
            if (n2) SetSpeedNode(n2, v);
        }
        ++n;
    }
    return n;
}

// 按绝对速度值设置（滑条用）。0 = 定住，越大越快。
int ActSpeedValue(float v) {
    SelIter it;
    if (!SelBegin(it)) { SetErr("没有选中单位（先在游戏里选中）"); return 0; }
    int n = 0;
    uintptr_t obj;
    while ((obj = SelNext(it)) != 0) {
        uintptr_t body = BodyOf(obj);
        if (!body) continue;
        uintptr_t n1 = 0, n2 = 0;
        SpeedNodes(body, n1, n2);
        SetSpeedNode(n1, v);
        if (n2) SetSpeedNode(n2, v);
        ++n;
    }
    return n;
}

// 首个选中单位当前的速度值（供 UI 显示滑条锚点）
float SelectedFirstSpeed() {
    SelIter it;
    if (!SelBegin(it)) return -1.f;
    uintptr_t obj = SelNext(it);
    if (!obj) return -1.f;
    uintptr_t body = BodyOf(obj);
    if (!body) return -1.f;
    uintptr_t n1 = 0, n2 = 0;
    SpeedNodes(body, n1, n2);
    if (!n1) return -1.f;
    float cur = 0;
    if (!SafeReadFloat(n1 + 8, cur)) return -1.f;
    return cur;
}

int ActHealth(int mode) {
    SelIter it;
    if (!SelBegin(it)) { SetErr("没有选中单位（先在游戏里选中）"); return 0; }
    int n = 0;
    uintptr_t obj;
    while ((obj = SelNext(it)) != 0) {
        uintptr_t body = BodyOf(obj);
        if (!body) continue;
        uint32_t hp = 0;
        if (!SafeRead32(body + OFF_HPBODY, hp) || !hp) continue;
        if (!IsMemOk(hp, 0x14, true)) { ++g_guardSkip; continue; }
        float cur = 0;
        if (!SafeReadFloat(hp + 4, cur) || !PlausibleFloat(cur, 0.f, 1.0e7f)) { ++g_guardSkip; continue; }
        if (mode == 0) { SafeWriteFloat(hp + 4, 999999.f); SafeWriteFloat(hp + 0xC, 999999.f); }
        else if (mode == 1) { SafeWriteFloat(hp + 4, 1.0f); }
        else if (mode == 2) {
            float bk = 0;
            // ★ 下限为 1（不允许 0）：写出 HP=0 会留下"幽灵单位"——
            //   对象不被回收、模型仍在（基地车还能开走），但失去全部单位属性。
            if (SafeReadFloat(hp + 0x10, bk) && PlausibleFloat(bk, 1.f, 1.0e7f)) {
                SafeWriteFloat(hp + 4, bk); SafeWriteFloat(hp + 0xC, bk);
            } else { ++g_guardSkip; continue; }
        }
        ++n;
    }
    return n;
}

// 锁定血量（每帧调用，供 UI「锁定血量」开关）：
// 把选中单位的当前血量回写到它自己的上限值 → 血条不掉。
// 与「无限血量」（一次性把血量写成极大值）是两件事，UI 上是两个按钮。
int LockSelectedHealth() {
    SelIter it;
    if (!SelBegin(it)) return 0;
    int n = 0;
    uintptr_t obj;
    while ((obj = SelNext(it)) != 0) {
        uintptr_t body = BodyOf(obj);
        if (!body) continue;
        uint32_t hp = 0;
        if (!SafeRead32(body + OFF_HPBODY, hp) || !hp) continue;
        if (!IsMemOk(hp, 0x14, true)) continue;
        float maxv = 0, cur = 0;
        if (!SafeReadFloat(hp + 0xC, maxv) || !PlausibleFloat(maxv, 1.f, 1.0e7f)) continue;
        if (!SafeReadFloat(hp + 4, cur)) continue;
        if (cur < maxv) { SafeWriteFloat(hp + 4, maxv); ++n; }
    }
    return n;
}

int ActSetOwner() {
    SetErr("");
    uint32_t localPlayer = g_localPlayerObj;
    if (!localPlayer) { SetErr("本地队伍对象为 0（还没抓到玩家；进局后随便点一下资源再试）"); return 0; }
    uint32_t base = static_cast<uint32_t>(GameBase());
    uint32_t curTmpl = 0;
    SafeRead32(base + R_CURTEMPLATE, curTmpl);
    bool skip = false;
    for (uint32_t h : kSuperWeaponSkip) if (curTmpl == h) { skip = true; break; }
    if (skip) { SetErr("当前对象是超武/特殊类型，已跳过"); return 0; }

    SelIter it;
    if (!SelBegin(it)) { SetErr("没有选中单位（先在游戏里选中要收编的单位）"); return 0; }
    R3_LOG(0, "[动作] 接管单位 → 开始（选中 %d 个）", it.count);
    int n = 0;
    uintptr_t obj;
    while ((obj = SelNext(it)) != 0) {
        uintptr_t body = BodyOf(obj);
        if (!body) continue;
        uint32_t own = 0;
        SafeRead32(body + OFF_OWNER, own);
        // 护栏放宽：只要求"现值是 0 或可读指针"（原来要求像游戏对象，可能误拦）
        if (own && !IsMemOk(own, 4, false)) { ++g_guardSkip; continue; }
        CrashMark("SetOwner", body, localPlayer, 0);
        if (SafeWrite32(body + OFF_OWNER, localPlayer)) ++n;
    }
    if (!n) SetErr("选中项没有可改的归属字段（或护栏拦下）");
    return n;
}

int ActKillSelected() {
    SelIter it;
    if (!SelBegin(it)) { SetErr("没有选中单位（先在游戏里选中）"); return 0; }
    if (g_noGameCall) { SetErr("已开启「禁用游戏函数调用」，摧毁被跳过"); return -1; }
    R3_LOG(0, "[动作] 摧毁单位 → 开始（选中 %d 个）", it.count);
    uintptr_t fnAddr = SymKillUnit();          // = 0x39EA50
    if (!fnAddr) { SetErr("击杀函数未解析到"); return -1; }
    auto fn = reinterpret_cast<KillUnit_t>(fnAddr);
    // 先把选中单位全部收集到数组，再逐个击杀（2026-10-15 闪退修复）：
    //    击杀会让游戏把该单位从"选中链表"里摘掉 —— 边遍历边杀会拿到野指针 → 闪退。
    uintptr_t tgt[64];
    int nTgt = 0;
    uintptr_t obj;
    while ((obj = SelNext(it)) != 0) {
        if (nTgt >= 64) break;
        // 基于 CE：目标 = [obj+0x138]（body）
        uintptr_t b = BodyOf(obj);
        if (!b) { ++g_guardSkip; continue; }
        if (!LooksLikeGameObject(b)) { ++g_guardSkip; continue; }
        tgt[nTgt++] = b;
    }
    int n = 0;
    for (int i = 0; i < nTgt; ++i) {
        __try {
            CrashMark("KillUnit", tgt[i], 0, 0);
            fn(reinterpret_cast<void*>(tgt[i]), nullptr, 6, 19, 0);   // thiscall(body, 6, 19, 0)
            ++n;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (!n) SetErr("没有可摧毁的目标");
    return n;
}

typedef int (__thiscall* SetStatus_t)(void* thing, int status, int on);   // Object::SetStatus，ret 8

// ===================== modifier 通道（引擎 0x7181F0 施加 / 0x718300 解除 / 0x4A6BE0 查表）=====================
// 一切经引擎函数完成，**不读也不写引擎字段** —— 避免历史"手抄配方 / poke 越护栏 → 崩"的坑。
// ⚠️ 时长必须给帧数（15 帧/秒）：用 -1 约 12 秒就过期（2026-10-16 实测）。
constexpr uint32_t MOD_STORE         = 0xC5E07887u;  // 全局 modifier 库容器哈希
constexpr uint32_t MOD_ARMOR_UP      = 0x505EBD44u;  // MAP_ARMOR_Up：减弱护甲（实测 ×10.9 触底）
constexpr uint32_t MOD_HEALTH_DOWN   = 0xAC47C5FFu;  // MAP_HEALTH_MULT_Down：血量上限 ×0.9/层
constexpr uint32_t MOD_IRONCURTAIN   = 0x6516AA2Du;  // AttributeModifier_IronCurtain：无敌 + 铁幕外观
constexpr uint32_t MOD_INVULNERABLE  = 0x508D8641u;  // MAP_INVULNERABLE_Up：无敌
constexpr uint32_t MOD_SPEED_UP      = 0x5949C29Cu;  // MAP_SPEED_Up：加速
constexpr uint32_t MOD_SPEED_DOWN    = 0x46C6A82Bu;  // MAP_SPEED_Down：减速
constexpr uint32_t MOD_DAMAGE_UP     = 0x61E3FB6Eu;  // MAP_DAMAGE_MULT_Up：输出增强
constexpr int      kModFrames        = 1800;         // 120 秒（不要用 -1）
// 重置路径诊断计数（收尾打印并清零）
static int g_rmCalls = 0, g_rmNonZero = 0, g_rmExc = 0, g_rmSkipGuard = 0, g_rmSkipLookup = 0;

// 名字哈希 → modifier 对象
static uintptr_t ModifierLookup(uint32_t nameHash) {
    const uintptr_t fn = SymModifierFind();
    if (!fn) return 0;
    typedef uintptr_t(__cdecl* Find_t)(uint32_t, uint32_t, uintptr_t);
    uintptr_t obj = 0;
    __try { obj = reinterpret_cast<Find_t>(fn)(MOD_STORE, nameHash, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return obj;
}

// 施加 / 解除（thing = Thing，未归一化的原始 Thing）
static bool ModifierApply(uintptr_t thing, uint32_t nameHash, int dur) {
    const uintptr_t fn = SymApplyModifier();
    if (!fn || !thing || !LooksLikeGameObject(thing)) { ++g_guardSkip; return false; }
    const uintptr_t mod = ModifierLookup(nameHash);
    if (!mod) return false;
    typedef int(__thiscall* Apply_t)(void*, uintptr_t, int);
    int ret = 0;
    __try { ret = reinterpret_cast<Apply_t>(fn)(reinterpret_cast<void*>(thing), mod, dur); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return ret != 0;
}
static bool ModifierRemove(uintptr_t thing, uint32_t nameHash) {
    const uintptr_t fn = SymRemoveModifier();
    if (!fn || !thing || !LooksLikeGameObject(thing)) { ++g_guardSkip; ++g_rmSkipGuard; return false; }
    const uintptr_t mod = ModifierLookup(nameHash);
    if (!mod) { ++g_guardSkip; ++g_rmSkipLookup; return false; }
    typedef int(__thiscall* Remove_t)(void*, uintptr_t);
    int ret = 0;
    ++g_rmCalls;
    __try { ret = reinterpret_cast<Remove_t>(fn)(reinterpret_cast<void*>(thing), mod); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ++g_rmExc; return false; }
    if (ret) ++g_rmNonZero;
    return ret != 0;
}

// ===================== 单位状态（表驱动）=====================
// 依据（wiki/engine/crate-system.md §七 + research/crates/04）：
//   日冕箱子留给单位的只有两样 —— modifier + ObjectStatus（属性箱 = applyModifier(名, 时长)）。
// ⇒ 一个"状态" = 若干 modifier + 若干 ObjectStatus + 可选的定身 / 易碎联动。
// ⚠️ 一律走引擎函数（0x7181F0 施加 / 0x718300 解除 / Object::SetStatus）——
//   引擎拥有的内部槽位字段（如 Thing+0x0CC / +0x26C / +0x274）每帧自查自写，手写会被重置。
struct UnitStatusDef {
    const char* name;
    const char* tip;
    uint32_t    mods[3];     // modifier 名字哈希（0 结尾）
    int         stats[4];    // ObjectStatus 号（0 结尾）
    int         frames;      // 默认时长（帧；15 帧/秒）
    bool        haltMove;    // 定身：速度节点写 0
    bool        brittle;     // 易碎：当前血量压成 1
};

static const UnitStatusDef kUnitStatus[] = {
    { "铁幕状态", "无敌 + 铁幕外观（引擎自带 AttributeModifier_IronCurtain）",
      { MOD_IRONCURTAIN, 0 }, { 0 }, 1800, false, false },
    { "无敌状态", "不受伤害（MAP_INVULNERABLE_Up）",
      { MOD_INVULNERABLE, 0 }, { 0 }, 1800, false, false },
    { "加速状态", "移动速度提升（MAP_SPEED_Up）",
      { MOD_SPEED_UP, 0 }, { 0 }, 1800, false, false },
    { "减速状态", "移动速度降低（MAP_SPEED_Down）",
      { MOD_SPEED_DOWN, 0 }, { 0 }, 1800, false, false },
    { "易碎状态", "护甲削弱 + 血量上限压制（重复施加可叠得更脆）",
      { MOD_ARMOR_UP, MOD_HEALTH_DOWN, 0 }, { 0 }, 1800, false, false },
    { "输出增强", "自身输出伤害提升（MAP_DAMAGE_MULT_Up）",
      { MOD_DAMAGE_UP, 0 }, { 0 }, 1800, false, false },
};
static_assert(sizeof(kUnitStatus) / sizeof(kUnitStatus[0]) == US_COUNT,
              "kUnitStatus 与 US_* 枚举不同步");

const char* UnitStatusName(int kind) {
    if (kind < 0 || kind >= US_COUNT) return "?";
    return kUnitStatus[kind].name;
}
const char* UnitStatusTip(int kind) {
    if (kind < 0 || kind >= US_COUNT) return "";
    return kUnitStatus[kind].tip;
}

// Object::SetStatus —— 带"状态变更通知"（行为锁 / 音效 / UI 靠它）
static bool SetStatusOn(uintptr_t thing, int status, int on) {
    const uintptr_t fn = SymSetStatus();
    if (!fn || !status || !thing) return false;
    __try {
        CrashMark("UnitStatus", thing, static_cast<uintptr_t>(status), static_cast<uintptr_t>(on));
        reinterpret_cast<SetStatus_t>(fn)(reinterpret_cast<void*>(thing), status, on ? 1 : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

// 血量回满：上限从未被改过（易碎只压当前值 +4），读 +0x10 备份值写回
static bool RestoreHpFull(uintptr_t thing) {
    uint32_t hpObj = 0;
    if (!SafeRead32(thing + OFF_HPBODY, hpObj) || !hpObj || !IsMemOk(hpObj, 0x14, true)) return false;
    float bk = 0;
    if (!SafeReadFloat(hpObj + 0x10, bk) || !PlausibleFloat(bk, 1.f, 1.0e7f)) return false;
    SafeWriteFloat(hpObj + 4, bk);
    SafeWriteFloat(hpObj + 0xC, bk);
    return true;
}

// 易碎 = 把当前血量压成 1（一击必死）。与 ActHealth(1) 同理：
//   不叠 ARMOR_Up / HEALTH_MULT_Down 的原因 —— 那是逐层改倍率，撤回复杂；"一碰就碎"本质就是"只有 1 点血"。
static bool SquashHpTo1(uintptr_t thing) {
    uint32_t hpObj = 0;
    if (!SafeRead32(thing + OFF_HPBODY, hpObj) || !hpObj || !IsMemOk(hpObj, 0x14, true)) return false;
    SafeWriteFloat(hpObj + 4, 1.0f);
    return true;
}

static void DrainRmDiag(const char* tag) {
    if (!g_rmCalls) return;
    R3_LOG(0, "[状态] %s诊断: 解除调用=%d 非0=%d 异常=%d 护栏跳过=%d 查表空=%d",
           tag, g_rmCalls, g_rmNonZero, g_rmExc, g_rmSkipGuard, g_rmSkipLookup);
    g_rmCalls = g_rmNonZero = g_rmExc = g_rmSkipGuard = g_rmSkipLookup = 0;
}

// 施加 / 解除一个状态（作用于选中单位）—— 返回成功处理的单位数。
//   时限只是"到期自动撤"的保险，不是效果持续时间的承诺；提前撤用 on=0 或 ActStatusReset()。
//   注意：同一状态重复施加会叠层，0x718300 一次只撤一层 —— 叠了几层就要解除几次。
int ActUnitStatus(int kind, int on, int seconds) {
    if (kind < 0 || kind >= US_COUNT) { SetErr("未知单位状态"); return 0; }
    const UnitStatusDef& d = kUnitStatus[kind];
    SelIter it;
    if (!SelBegin(it)) { SetErr("没有选中单位（先在游戏里选中）"); return 0; }
    if (g_noGameCall) { SetErr("已开启「禁用游戏函数调用」，状态操作被跳过"); return -1; }
    if (!SymSetStatus() || !SymApplyModifier() || !SymRemoveModifier() || !SymModifierFind()) {
        SetErr("单位状态：符号未解析出（状态器 / 施加 / 解除 / 查表）");
        return 0;
    }
    if (seconds > 600) seconds = 600;                   // 上限 10 分钟（先 clamp 再乘，避免溢出）
    int frames = (seconds > 0) ? seconds * 15 : d.frames;
    if (frames <= 0) frames = d.frames;
    int n = 0;
    uintptr_t obj;
    while ((obj = SelNext(it)) != 0) {
        uintptr_t thing = BodyOf(obj);
        if (!thing || !LooksLikeGameObject(thing)) { ++g_guardSkip; continue; }
        for (int i = 0; i < 4 && d.stats[i]; ++i) SetStatusOn(thing, d.stats[i], on);
        for (int i = 0; i < 3 && d.mods[i]; ++i) {
            if (on) ModifierApply(thing, d.mods[i], frames);
            else    ModifierRemove(thing, d.mods[i]);
        }
        if (d.haltMove) {
            uintptr_t n1 = 0, n2 = 0;
            SpeedNodes(thing, n1, n2);
            if (on) { SetSpeedNode(n1, kSpeedFreeze); if (n2) SetSpeedNode(n2, kSpeedFreeze); }
            else    { RestoreSpeedNode(n1);           if (n2) RestoreSpeedNode(n2); }
        }
        if (d.brittle) { if (on) SquashHpTo1(thing); else RestoreHpFull(thing); }
        ++n;
    }
    if (!n) SetErr(on ? "没有可操作的目标" : "没有可解除的目标");
    else SetErr("");
    R3_LOG(n ? 0 : 1, "[状态] %s%s %d 个目标", on ? "施加" : "解除", d.name, n);
    if (!on) DrainRmDiag("解除");
    return n;
}

// 重置：把状态表上的全部已知效果撤干净 —— modifier 逐名解除 + 行为解锁（速度还原）+ 血量回满。
// 为什么不必先判断"挂了哪几只"：0x718300 的返回值不代表存在性（研究已证），
//   对未挂过的名字解除 = 一次空查询，与既有的逐名解除行为一致。
int ActStatusReset() {
    SelIter it;
    if (!SelBegin(it)) { SetErr("没有选中单位（先在游戏里选中）"); return 0; }
    if (g_noGameCall) { SetErr("已开启「禁用游戏函数调用」，重置被跳过"); return -1; }
    if (!SymRemoveModifier() || !SymModifierFind()) {
        SetErr("重置：符号未解析出（解除 / 查表）");
        return 0;
    }
    int n = 0;
    uintptr_t obj;
    while ((obj = SelNext(it)) != 0) {
        uintptr_t thing = BodyOf(obj);
        if (!thing || !LooksLikeGameObject(thing)) { ++g_guardSkip; continue; }
        for (int k = 0; k < US_COUNT; ++k)
            for (int i = 0; i < 3 && kUnitStatus[k].mods[i]; ++i)
                ModifierRemove(thing, kUnitStatus[k].mods[i]);
        uintptr_t n1 = 0, n2 = 0;
        SpeedNodes(thing, n1, n2);
        RestoreSpeedNode(n1); if (n2) RestoreSpeedNode(n2);
        RestoreHpFull(thing);
        ++n;
    }
    if (!n) SetErr("没有可重置的目标");
    else SetErr("");
    R3_LOG(n ? 0 : 1, "[状态] 重置 %d 个目标", n);
    DrainRmDiag("重置");
    return n;
}

int ActRank() {
    SelIter it;
    if (!SelBegin(it)) { SetErr("没有选中单位（先在游戏里选中）"); return 0; }
    if (g_noGameCall) { SetErr("已开启「禁用游戏函数调用」，满级被跳过"); return -1; }
    R3_LOG(0, "[动作] 升级单位 → 开始（选中 %d 个）", it.count);
    uintptr_t fnAddr = SymLevelUp();
    if (!fnAddr) return -1;
    auto fn = reinterpret_cast<LevelUp_t>(fnAddr);
    int n = 0;
    uintptr_t obj;
    while ((obj = SelNext(it)) != 0) {
        // 基于 CE 的 SelectUnitLevelUp：目标 = [body + 0x3CC]
        //   body 经 BodyOf 归一化（Drawable → [obj+0x138]；召唤物自身即 Thing）
        uint32_t tgt32 = 0;
        uintptr_t tgt = 0;
        uintptr_t body = BodyOf(obj);
        if (body && SafeRead32(body + 0x3CC, tgt32) && tgt32 && LooksLikeGameObject(tgt32))
            tgt = tgt32;
        if (!tgt) { ++g_guardSkip; continue; }   // 护栏：目标必须像游戏对象
        __try {
            CrashMark("LevelUp", tgt, obj, 0);
            fn(reinterpret_cast<void*>(tgt), nullptr, 3, 1, 0);   // 基于 CE: ecx=目标, (3,1,0)
            ++n;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (!n) SetErr("没有可升级的目标");
    return n;
}

// ===================== 创建后处理链（2026-10-15） =====================
// 背景（探宝实测）：召唤出来的单位**小地图图标是空心的**（= 没登记进玩家体系），
//   交战一会儿之后图标会自己变成实心（= 游戏在别的路径上把它补登记了）。
// 正常建造路径在 CreateUnit 之后还会调这两步（thiscall，ecx = team）：
//   PostA 0x84A0B0：把 obj 从 team 的 pending 列表摘除 + 通知 [team+0x1278]
//   PostB 0x838AB0：激活（内部 call 0x7191D0(obj,1)）+ [team+0x8c] 统计 + 收尾
// 参数（**权威来源 = 对运行进程线性反汇编**，见 research/memscan/dis_postfn.py）：
//   PostA：ret 8 → 2 个栈参 = (0, obj)        ← 函数体 mov edi,[esp+0x10] 取 arg2 当对象
//   PostB：ret 0xC → 3 个栈参 = (0, obj, 0)
//   ⚠️ ret 是"被调用方清栈"，参数个数错一个 → esp 失衡 → 必崩。故用 __fastcall 仿真 thiscall
//      且**参数个数严格写死**（fastcall 也是被调用方清栈，正好配 ret 8 / ret 0xC）。
namespace {
const uint32_t R_POSTA = 0x44A0B0;      // 0x84A0B0 - 0x400000
const uint32_t R_POSTB = 0x438AB0;      // 0x838AB0 - 0x400000
bool  g_postProcOn = false;             // ⚠️ 默认关。2026-10-15 实机结论：
                                        //   ① 它**不解决**"召唤物隐形/小地图半透明"（当帧调、延迟 1.2s 调都无效）；
                                        //   ② 它会调游戏内部函数（PostA 摘 pending 列表）→ 有"反复摘坏队伍链表"的嫌疑，
                                        //      与 08:14 那次"大量复制后闪退"无法排除关联。
                                        //   要试可临时 `/postproc?on=1`。
bool  g_summonStealth = false;          // 召唤物隐形开关：**默认关**（=清掉隐形位，召唤即实心）
char  g_postErr[128] = "";
typedef void(__fastcall* Thiscall2_t)(void* ecx, void* edx, uint32_t a1, uint32_t a2);
typedef void(__fastcall* Thiscall3_t)(void* ecx, void* edx, uint32_t a1, uint32_t a2, uint32_t a3);

// ---- 延迟后处理队列（时序是关键）----
// 实机结论（2026-10-15）：**创建当帧立刻调 PostA/PostB 无效**（小地图图标仍透明）；
//   **隔一段时间再调就有效**（图标立刻变实心）。所以改成入队 + 每帧泵，到期再调。
struct PendingPost { uintptr_t team; uintptr_t obj; DWORD due; };
const int     kMaxPendingPost = 64;
PendingPost   g_pendPost[kMaxPendingPost];
volatile LONG g_pendCount = 0;
const DWORD   kPostDelayMs   = 1200;    // 延迟（毫秒）—— 给游戏自己的更新循环先"看见"这个对象
const DWORD   kPostExpireMs  = 8000;    // 超过这个时长还没处理就丢弃（对象可能已失效）
}

// 入队（由 SummonByHash 调用；在渲染线程）
void QueuePostProcess(uintptr_t team, uintptr_t obj) {
    if (!g_postProcOn || !team || !obj) return;
    const LONG n = g_pendCount;
    if (n >= kMaxPendingPost) { R3_LOG(1, "[后处理] 队列已满，丢弃 %08X", (unsigned)obj); return; }
    g_pendPost[n].team = team;
    g_pendPost[n].obj  = obj;
    g_pendPost[n].due  = GetTickCount() + kPostDelayMs;
    InterlockedIncrement(&g_pendCount);
}

// 每帧由渲染线程调用：把到期的条目做掉
void PostProcPump() {
    const LONG n = g_pendCount;
    if (n <= 0) return;
    const DWORD now = GetTickCount();
    LONG w = 0;
    for (LONG i = 0; i < n; ++i) {
        PendingPost& p = g_pendPost[i];
        if (!p.obj) continue;
        if (now < p.due) { g_pendPost[w++] = p; continue; }        // 还没到期 → 留着
        // 到期：先校验对象此刻仍合法（可能已被摧毁/回收 → 指针已失效，绝不能再调）
        uint32_t vt = 0, nowOwner = 0;
        const uintptr_t base = GameBase();
        const bool vtOk = SafeRead32(p.obj, vt) && base && vt >= base && vt < base + 0x940000;
        const bool ownOk = SafeRead32(p.obj + OFF_OWNER, nowOwner) && nowOwner == (uint32_t)p.team;
        if (vtOk && ownOk) {
            PostProcessCall(p.team, p.obj, true, true);            // 内部自带 SEH + 入口字节校验
            R3_LOG(0, "[后处理] 延迟登记 obj=%08X（创建后 %ums）",
                   (unsigned)p.obj, (unsigned)(now - (p.due - kPostDelayMs)));
        } else {
            R3_LOG(1, "[后处理] 跳过 obj=%08X（对象已失效 vtOk=%d ownOk=%d）",
                   (unsigned)p.obj, vtOk ? 1 : 0, ownOk ? 1 : 0);
        }
    }
    InterlockedExchange(&g_pendCount, w);
}

bool PostProcessCall(uintptr_t team, uintptr_t obj, bool runA, bool runB) {
    g_postErr[0] = 0;
    const uintptr_t base = GameBase();
    if (!base) { _snprintf_s(g_postErr, sizeof(g_postErr), _TRUNCATE, "模块基址为 0"); return false; }
    if (!team || !obj) { _snprintf_s(g_postErr, sizeof(g_postErr), _TRUNCATE, "team/obj 为空"); return false; }
    if (!LooksLikeGameObject(obj)) { _snprintf_s(g_postErr, sizeof(g_postErr), _TRUNCATE, "obj 不像游戏对象"); return false; }
    if (!IsMemOk(team, 0x1280, false)) { _snprintf_s(g_postErr, sizeof(g_postErr), _TRUNCATE, "team 不像 Team 对象"); return false; }

    const uintptr_t a = base + R_POSTA, b = base + R_POSTB;
    // 入口字节校验（地址漂移/被 stub 时拒绝，绝不盲调）
    uint8_t ea[2] = { 0 }, eb[3] = { 0 };
    if (runA && (!SafeReadBytes(a, ea, 2) || ea[0] != 0x56 || ea[1] != 0x57)) {
        _snprintf_s(g_postErr, sizeof(g_postErr), _TRUNCATE, "PostA 入口字节不符(%02X %02X)", ea[0], ea[1]);
        return false;
    }
    if (runB && (!SafeReadBytes(b, eb, 3) || eb[0] != 0x56 || eb[1] != 0x8B || eb[2] != 0xF1)) {
        _snprintf_s(g_postErr, sizeof(g_postErr), _TRUNCATE, "PostB 入口字节不符(%02X %02X %02X)", eb[0], eb[1], eb[2]);
        return false;
    }
    __try {
        if (runA) {
            CrashMark("PostA", team, obj, 0);
            R3_LOG(0, "[后处理] PostA(team=%08X, obj=%08X)", (unsigned)team, (unsigned)obj);
            reinterpret_cast<Thiscall2_t>(a)(reinterpret_cast<void*>(team), nullptr, 0u,
                                             static_cast<uint32_t>(obj));
        }
        if (runB) {
            CrashMark("PostB", team, obj, 0);
            R3_LOG(0, "[后处理] PostB(team=%08X, obj=%08X)", (unsigned)team, (unsigned)obj);
            reinterpret_cast<Thiscall3_t>(b)(reinterpret_cast<void*>(team), nullptr, 0u,
                                             static_cast<uint32_t>(obj), 0u);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        _snprintf_s(g_postErr, sizeof(g_postErr), _TRUNCATE, "调用异常（参数/地址不符）");
        return false;
    }
    return true;
}

bool GetPostProcOn() { return g_postProcOn; }
void SetPostProcOn(bool on) { g_postProcOn = on; }
const char* PostProcLastError() { return g_postErr; }

bool GetSummonStealth() { return g_summonStealth; }
void SetSummonStealth(bool on) { g_summonStealth = on; }

// 最近一次召唤创建的对象（Thing）—— 供"召唤后立刻对它操作"用
static uintptr_t g_lastCreated[20] = { 0 };
static int g_lastCreatedN = 0;

// 指定召唤落点（跨线程：HTTP 线程写、渲染线程读；"用一次即清"）
static float g_summonOv[3] = { 0, 0, 0 };
static volatile LONG g_summonOvSet = 0;
void SetSummonPosOverride(bool on, float x, float y, float z) {
    g_summonOv[0] = x; g_summonOv[1] = y; g_summonOv[2] = z;
    InterlockedExchange(&g_summonOvSet, on ? 1 : 0);
}

// 最近一次召唤/复制**实际用到的**落点与来源。供 /summon 回执 ——
// 调用方（尤其 AI）能立刻看出"这一单投到哪了"，而不是拿到一个 ok 却不知道落点。
void LastSpawnPos(float out[4], const char** src) {
    std::memcpy(out, g_lastPos, sizeof(float) * 4);
    if (src) *src = g_posSource;
}

// 落点查询（只读）。kind: 0=视野中心 1=选中单位 2=光标 3=按设置自动挑。
// ⚠️ 视野中心/光标都会调用游戏换算函数 0x1ED4A0 —— 只能在渲染线程跑（见 httpapi 的投递）。
int SpawnPointJson(char* buf, int n, int kind) {
    if (!buf || n <= 0) return 0;
    float p[4] = { 0, 0, 0, 1 };
    const char* sk = "auto";
    bool ok = false;
    switch (kind) {
    case 0:  sk = "view";      ok = TryViewCenterPos(p); break;
    case 1:  sk = "selection"; ok = TrySelectedPos(p);   break;
    case 2:  sk = "cursor";    ok = TryMouseWorldPos(p);  break;
    default: sk = "auto";      ok = GetSpawnPos(p, false); break;
    }
    if (ok && !PosLooksValid(p)) ok = false;
    // 格式串一律 ASCII：MSVC 按代码页解析源码，中文紧邻 % 会把占位符吃掉
    if (!ok)
        return _snprintf_s(buf, n, _TRUNCATE,
                           "{\"ok\":0,\"kind\":\"%s\",\"why\":\"no-spawn-point [%s]\"}",
                           sk, g_posDiag);
    // z 贴 0 即该点在水面（地图水面高度为 0）；陆军/建筑落上去会漂在水面上
    return _snprintf_s(buf, n, _TRUNCATE,
        "{\"ok\":1,\"kind\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,\"water\":%d,\"src\":\"%s\"}",
        sk, p[0], p[1], p[2], (p[2] < 1.0f) ? 1 : 0, (kind == 3) ? g_posSource : sk);
}

bool SummonByHash(uint32_t hash, int count, int owner, bool atMouse, const char* name) {
    (void)owner;
    return SummonByHashFor(hash, count, 0, atMouse, name);
}

int LastCreatedObjs(uintptr_t* out, int max) {
    if (!out || max <= 0) return 0;
    const int n = (g_lastCreatedN < max) ? g_lastCreatedN : max;
    for (int i = 0; i < n; ++i) out[i] = g_lastCreated[i];
    return n;
}

// teamOverride != 0 → 用指定"队伍对象"当归属（可给敌方/任意玩家召唤）；0 → 本地玩家
bool SummonByHashFor(uint32_t hash, int count, uintptr_t teamOverride, bool atMouse, const char* name) {
    (void)name;
    SetErr("");
    if (g_noGameCall) { SetErr("已开启「禁用游戏函数调用」，召唤被跳过（更安全）"); return false; }
    if (count < 1) count = 1;
    if (count > 20) count = 20;
    uint32_t base = static_cast<uint32_t>(GameBase());
    if (!base) { SetErr("游戏模块基址为 0"); return false; }

    if (!TemplateStoreReady()) { SetErr("对象模板库(ThingFactory)为空/未解析到"); return false; }

    void* tmpl = FindTemplate(hash);
    if (!tmpl) { SetErr("模板未找到（该哈希不在模板库）"); return false; }
    // 护栏：模板必须像游戏对象，否则"拿垃圾指针去调 CreateUnit"必崩
    if (!LooksLikeGameObject(reinterpret_cast<uintptr_t>(tmpl))) {
        SetErr("模板对象不合法（护栏拦下，未调用 CreateUnit）"); return false;
    }

    // 归属：**固定本地玩家**（基于 CE 的 Copy For Me / We Need Back：team = 本地玩家对象）。
    //       之前优先取"选中单位的 owner"→ 复制敌方单位时副本会归敌方，已修正。
    uint32_t team = 0, teamMember = 0;
    team = teamOverride ? static_cast<uint32_t>(teamOverride) : g_localPlayerObj;
    if (!team) {                                  // 本地玩家还没抓到 → 才退回选中单位的归属
        SelIter it;
        if (SelBegin(it)) {
            uintptr_t o = SelNext(it);
            if (o) { uintptr_t b = BodyOf(o); if (b) SafeRead32(b + OFF_OWNER, team); }
        }
    }
    if (!team) { SetErr("没有可用的归属（本地队伍对象为 0）"); return false; }
    if (!SafeRead32(team + 0x10, teamMember) || !teamMember) {
        char eb[128];
        _snprintf_s(eb, sizeof(eb), _TRUNCATE, "归属对象 %08X 的 +0x10 为空", (unsigned)team);
        SetErr(eb); return false;
    }

    float pos[4];
    if (InterlockedCompareExchange(&g_summonOvSet, 0, 1) == 1) {  // 指定坐标（取用一次即清）
        pos[0] = g_summonOv[0]; pos[1] = g_summonOv[1]; pos[2] = g_summonOv[2]; pos[3] = 1.0f;
        std::memcpy(g_lastPos, pos, sizeof(pos)); g_hasLastPos = true;
        g_posSource = "指定坐标";
        _snprintf_s(g_posDiag, sizeof(g_posDiag), _TRUNCATE, "指定@%.0f,%.0f", pos[0], pos[1]);
    } else if (!GetSpawnPos(pos, atMouse)) {
        char eb[160];
        _snprintf_s(eb, sizeof(eb), _TRUNCATE, "落点取不到 [%s]", g_posDiag);
        SetErr(eb); return false;
    }

    const uintptr_t createAddr = SymCreateUnit();
    if (!createAddr) { SetErr("创建函数未解析到（符号解析失败）"); return false; }

    auto create = reinterpret_cast<CreateUnit_t>(createAddr);
    uintptr_t firstObj = 0;
    uintptr_t created[20] = { 0 };
    int made = 0;
    {
        uint32_t tvt = 0; SafeRead32((uintptr_t)tmpl, tvt);
        R3_LOG(0, "[召唤] 模板=%08X vtable=%08X (模块内:%s)", (unsigned)(uintptr_t)tmpl, tvt,
               (tvt >= base && tvt < base + 0x940000) ? "是" : "否!");
    }
    __try {
        for (int i = 0; i < count; ++i) {
            CrashMarkPos("CreateUnit", pos[0], pos[1], pos[2], (uintptr_t)tmpl, team);
            void* o = create(0, tmpl, pos, reinterpret_cast<void*>(team),
                             reinterpret_cast<void*>(teamMember));
            if (!o) continue;
            if (!firstObj) firstObj = reinterpret_cast<uintptr_t>(o);
            if (made < 20) created[made] = reinterpret_cast<uintptr_t>(o);
            ++made;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        SetErr("CreateUnit 调用异常（参数/地址不符）");
        return false;
    }
    R3_LOG(0, "[召唤] 落点=%.1f,%.1f,%.1f(来源:%s) 尝试[%s] 归属=%08X 成功%d/%d obj=%08X",
           pos[0], pos[1], pos[2], g_posSource, g_posDiag, team, made, count,
           (unsigned)firstObj);
    if (firstObj) {
        uint32_t vt = 0, body = 0, hp = 0, own = 0;
        uint32_t s390 = 0, s394 = 0, s3a0 = 0, s3ac = 0, bc = 0, c8 = 0;
        SafeRead32(firstObj, vt);
        SafeRead32(firstObj + 0x138, body);
        SafeRead32(firstObj + 0xBC, bc);
        SafeRead32(firstObj + 0xC8, c8);
        SafeRead32(firstObj + 0x390, s390);
        SafeRead32(firstObj + 0x394, s394);
        SafeRead32(firstObj + 0x3A0, s3a0);
        SafeRead32(firstObj + 0x3AC, s3ac);
        R3_LOG(0, "[召唤] obj vtable=%08X (%s) body=%08X [BC]=%08X [C8]=%08X",
               vt, (vt >= base && vt < base + 0x940000) ? "模块内 ✓" : "模块外 ✗(假对象)", body, bc, c8);
        R3_LOG(0, "[召唤] obj 状态位 390=%08X 394=%08X 3A0=%08X 3AC=%08X (%s)",
               s390, s394, s3a0, s3ac, (s390 | s394 | s3a0 | s3ac) ? "有状态 ✓" : "全 0 ✗");
        if (body) {
            SafeRead32(body + 0x33c, hp);
            SafeRead32(body + 0x418, own);
            R3_LOG(0, "[召唤] obj.body.hp=%08X owner=%08X", hp, own);
        }
    }
    if (made <= 0) { SetErr("CreateUnit 返回空对象"); return false; }
    g_lastCreatedN = (made < 20) ? made : 20;
    for (int i = 0; i < g_lastCreatedN; ++i) g_lastCreated[i] = created[i];
    Ding();
    // 2026-10-15 修复：「召唤/复制出来的单位技能灰、护盾/能力不回」
    //   根因：正常建造路径会在创建后把对象（Thing）的这 6 个状态字初始化成"可用态"；
    //         而 SummonByHash 走的是裸 CreateUnit，这 6 个字停在"半初始化"值 →
    //         单位的状态机接不上（技能不可用、护盾回复不触发）。
    //   实测（埃癸斯护盾发生器 0x32535841，逐字节对比 + 自注入 F 键验证）：
    //         写这 6 个字后，按 F 时它们会整组翻转并保持，技能可用、护盾开始回复。
    //   ⚠️ 偏移相对 **Thing**。CreateUnit 返回的就是 Thing（此时 [obj+0x138] 是浮点值、不是 body）；
    //      +0x90/+0x94 会被游戏逐帧重算，所以写一次只是"踢通状态机"，一次即可。
    struct StateWordFix { uint32_t off; uint32_t val; };
    static const StateWordFix kPostCreateStateWords[] = {
        { 0x080, 0x00000000u },   // 静息态 0x20000000 → 清
        { 0x084, 0x00000800u },   // 回盾激活位
        { 0x088, 0x00000000u },   // 静息态 0x00800000 → 清
        { 0x090, 0x00000002u },   // 技能/护盾"已启用"
        { 0x094, 0x00000020u },   //
        { 0x0D0, 0x00000000u },   // 静息态 0x40 → 清
    };

    // 判"CreateUnit 返回的到底是 Thing 还是 Drawable"：
    //   CreateUnit 实测返回 **Thing**；为兼容返回 Drawable 的版本，用 "Thing 才有的 HP 结构指针
    //   [obj+0x33c]" 做判据（HP 结构 = 一块可读的合法内存）。
    auto ResolveThing = [](uintptr_t obj) -> uintptr_t {
        uint32_t hp = 0;
        if (SafeRead32(obj + 0x33C, hp) && hp && IsMemOk(hp, 0x14, false)) return obj;
        uint32_t b = 0;
        if (SafeRead32(obj + OFF_BODY, b) && LooksLikeGameObject(b)) return b;
        return 0;
    };

    {
        int nb = 0, nfix = 0;
        for (int i = 0; i < made && i < 20; ++i) {
            if (!created[i]) continue;
            uintptr_t thing = ResolveThing(created[i]);
            if (!thing || !LooksLikeGameObject(thing)) { ++g_guardSkip; continue; }
            ++nb;
            for (int k = 0; k < (int)(sizeof(kPostCreateStateWords) / sizeof(kPostCreateStateWords[0])); ++k)
                if (SafeWrite32(thing + kPostCreateStateWords[k].off, kPostCreateStateWords[k].val)) ++nfix;
        }
        R3_LOG(0, "[召唤] 创建后状态字补写：%d 个单位 × 6 字（成功写入 %d 次）", nb, nfix);
    }
    // 创建后处理链（延迟版，`/postproc?on=1` 默认开）：把对象登记进玩家体系。
    //   ⚠️ 实测：**创建当帧立刻调无效**（图标仍透明）→ 这里只"入队"，
    //      由渲染线程每帧的 PostProcPump() 在 kPostDelayMs 之后再调。
    if (g_postProcOn) {
        int nQ = 0;
        for (int i = 0; i < made && i < 20; ++i) {
            if (!created[i]) continue;
            uintptr_t thing = ResolveThing(created[i]);
            if (!thing) { ++g_guardSkip; continue; }
            QueuePostProcess(team, thing);
            ++nQ;
        }
        R3_LOG(0, "[召唤] 已入队延迟登记 %d/%d 个（+%ums 后调 PostA/PostB）",
               nQ, made, (unsigned)kPostDelayMs);
    } else {
        R3_LOG(0, "[召唤] 后处理未启用（/postproc?on=1 可开；小地图图标会一直透明）");
    }

    // 基于 CE 的 SetUnitState：置状态位，让对象真正"进入世界"
    for (int i = 0; i < made && i < 20; ++i) {
        if (!created[i]) continue;
        if (!LooksLikeGameObject(created[i])) { ++g_guardSkip; continue; }   // 护栏：不是游戏对象就别写
        // 护栏修正（2026-10-15）：原来要求 [obj+0x138] 是对象指针，但 **CreateUnit 返回的是 Thing**，
        //    其 +0x138 是浮点（实测 4283D50B）→ 护栏恒定跳过 → SetUnitState 从来没生效过。
        //    改用 "Thing 专有的 HP 结构指针" 做判据（与上面的 ResolveThing 一致）。
        if (!ResolveThing(created[i])) { ++g_guardSkip; continue; }
        CrashMark("SetUnitState", created[i], 0, 0);
        uint32_t v = 0;
        if (SafeRead32(created[i] + 0xBC, v)) SafeWrite32(created[i] + 0xBC, v | 0x80000000u);
        // 2026-10-15 实机定案：下面这三位 = **"隐形/未入世"标志位**。
        //   证据（同型号同 vtable 逐字节对拍，`research/memscan/diff_states.py`）：
        //       实心(已被侦察单位照亮) = {+0xC8:0, +0x28:0, +0x208:0}
        //       隐形(刚召唤)           = {+0xC8:0x800, +0x28:0x80000000, +0x208:0x80000000}
        //   现场清这三位 → 图标立刻变实心（实机确认）。早期基于 CE 的那句 `[obj+0xC8] |= 0x800`
        //   （注释写"让它进入世界"）**其实正是把它变隐形的元凶**。
        //   → 默认**清掉**（召唤即实心、敌方正常攻击）；勾选「召唤单位隐形」则**置上**（当外挂用）。
        //   ⚠️ +0x28/+0x208 是游戏自己在创建时置上的，我们没写过，所以必须主动清。
        const bool stealth = g_summonStealth;
        const uint32_t c8Bit = stealth ? 0x00000800u : 0u;
        const uint32_t hiBit = stealth ? 0x80000000u : 0u;
        if (SafeRead32(created[i] + 0xC8, v))  SafeWrite32(created[i] + 0xC8,  (v & ~0x00000800u) | c8Bit);
        if (SafeRead32(created[i] + 0x28, v))  SafeWrite32(created[i] + 0x28,  (v & ~0x80000000u) | hiBit);
        if (SafeRead32(created[i] + 0x208, v)) SafeWrite32(created[i] + 0x208, (v & ~0x80000000u) | hiBit);
    }
    return true;
}

const char* SummonLastError() { return g_lastErr; }

// ---- 安全护栏对外接口（必须在匿名命名空间之外，才有外部链接）----
bool SetDryRun(bool on) { g_dryRun = on; return true; }
bool GetDryRun() { return g_dryRun; }
bool SetNoGameCall(bool on) { g_noGameCall = on; return true; }
bool GetNoGameCall() { return g_noGameCall; }
int  GuardSkips() { return g_guardSkip; }
int  GuardWrites() { return g_guardDone; }

// 功能触发反馈音（"叮"）—— 用系统提示音，异步，不卡帧
void Ding() { MessageBeep(MB_OK); }

// 诊断输出去向：nullptr=写「输出记录」，否则拼进 HTTP 缓冲
static char* g_diagBuf = nullptr;
static int   g_diagCap = 0;
static int   g_diagLen = 0;
static void DiagPrintf(const char* fmt, ...) {
    char line[224];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (g_diagBuf) {
        int room = g_diagCap - g_diagLen - 2;
        if (room <= 0) return;
        int n = (int)strlen(line);
        if (n > room) n = room;
        memcpy(g_diagBuf + g_diagLen, line, n);
        g_diagLen += n;
        g_diagBuf[g_diagLen++] = '\n';
        g_diagBuf[g_diagLen] = 0;
    } else {
        OverlayLogLine(0, line);
    }
}

static void DiagCore() {
    uint32_t base = static_cast<uint32_t>(GameBase());
    uintptr_t fg = SymThingFactory(), sg = SymSelMgr();
    uint32_t store = 0, pmgr = 0;
    SafeRead32(fg, store);
    SafeRead32(sg, pmgr);
    DiagPrintf("[诊断] base=%08X", base);
    DiagPrintf("[诊断] 符号: 创建=%08X(%s) 升级=%08X(%s) 查找=%08X(%s)",
           (unsigned)SymCreateUnit(), SymHow("create"),
           (unsigned)SymLevelUp(), SymHow("level"),
           (unsigned)SymTmplFind(), SymHow("find"));
    DiagPrintf("[诊断] 符号: ThingFactory=%08X(%s)->%08X  选中管理器=%08X(%s)->%08X",
           (unsigned)fg, SymHow("fact"), store, (unsigned)sg, SymHow("sel"), pmgr);
    DiagPrintf("[诊断] 免费建造站点=%08X(%s)", (unsigned)SymFreeBuildSite(), SymHow("free"));
    {   // 落点三来源逐个体检
        float view[16], proj[16]; int vp[4] = { 0,0,0,0 };
        const bool cam = GetCamData(view, proj, vp);
        float camPos[3] = { 0,0,0 }, fwd[3] = { 0,0,0 };
        if (cam) {
            camPos[0] = -(view[12]*view[0] + view[13]*view[4] + view[14]*view[8]);
            camPos[1] = -(view[12]*view[1] + view[13]*view[5] + view[14]*view[9]);
            camPos[2] = -(view[12]*view[2] + view[13]*view[6] + view[14]*view[10]);
            fwd[0] = view[8]; fwd[1] = view[9]; fwd[2] = view[10];
        }
        {
            uint32_t vti[8] = { 0 }; GetVtInfo(vti);
            DiagPrintf("[诊断] 相机 抓到=%s(来源:%s) vp=%d,%d,%d,%d 被拒视图矩阵=%d",
                   cam ? "是" : "否", vti[7] ? "SetTransform hook" : "GetTransform兜底(可疑)",
                   vp[0], vp[1], vp[2], vp[3], ViewRejects());
            DiagPrintf("[诊断] vtable: 16=%08X 17=%08X 42=%08X 44=%08X 45=%08X d3d9base=%08X",
                   vti[0], vti[1], vti[2], vti[3], vti[4], vti[6]);
        }
        DiagPrintf("[诊断] 相机 pos=%.1f,%.1f,%.1f 前向=%.2f,%.2f,%.2f",
               camPos[0], camPos[1], camPos[2], fwd[0], fwd[1], fwd[2]);
        {
            // 缩放 hook 遥测：入口/到点 + 距离 + 相机系数 + 俯仰
            union { uint32_t u; float f; } zv, zc, zp;
            zv.u = g_zoomVal; zc.u = g_zoomCam34; zp.u = g_zoomPitch;
            DiagPrintf("[诊断] 缩放 hook: 入口=%u 到点=%u obj=%08X 距离=%.1f 相机系数=%.4f 俯仰=%.4f 无限缩放=%s",
                       g_zoomEntry, g_zoomHits, g_zoomObj, zv.f, zc.f, zp.f,
                       GetFeature(FID_ZOOM_IN) ? "开" : "关");
        }
        {
            float lm[16], mm[16], lfz = 0, mfz = 0; int vc = 0;
            GetRawView(lm, &lfz, mm, &mfz, &vc);
            DiagPrintf("[诊断] 原始VIEW 次数=%d 末次_33=%.3f 最小_33=%.3f", vc, lfz, mfz);
            DiagPrintf("[诊断] VIEW末次 %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f",
                   lm[0], lm[1], lm[2], lm[3], lm[4], lm[5], lm[6], lm[7],
                   lm[8], lm[9], lm[10], lm[11], lm[12], lm[13], lm[14], lm[15]);
            DiagPrintf("[诊断] VIEW最小_33 %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f",
                   mm[0], mm[1], mm[2], mm[3], mm[4], mm[5], mm[6], mm[7],
                   mm[8], mm[9], mm[10], mm[11], mm[12], mm[13], mm[14], mm[15]);
        }
        float t[4];
        if (TryMouseWorldPos(t)) DiagPrintf("[诊断] 光标世界坐标 = %.1f %.1f %.1f", t[0], t[1], t[2]);
        else                     DiagPrintf("[诊断] 光标世界坐标: 失败");
        if (TryViewCenterPos(t)) DiagPrintf("[诊断] 视野中心 = %.1f %.1f %.1f", t[0], t[1], t[2]);
        else                     DiagPrintf("[诊断] 视野中心: 失败");
        if (TrySelectedPos(t))   DiagPrintf("[诊断] 选中单位坐标 = %.1f %.1f %.1f", t[0], t[1], t[2]);
        else                     DiagPrintf("[诊断] 选中单位坐标: 失败");
    }
    DiagPrintf("[诊断] 本地队伍 obj=%08X id=%u  资金对象=%08X",
           g_localPlayerObj, g_localPlayerId, g_moneyObj);
    if (store) {
        uint32_t vt = 0; SafeRead32(store, vt);
        DiagPrintf("[诊断] ThingFactory vtable=%08X", vt);
    }
    // 试用一个已知哈希（青龙战斗核心 0xC33C66E7）
    void* t = FindTemplate(0xC33C66E7u);
    DiagPrintf("[诊断] Find(0xC33C66E7) = %08X", (unsigned)(uintptr_t)t);
    if (g_localPlayerObj) {
        uint32_t tm = 0; SafeRead32(g_localPlayerObj + 0x10, tm);
        DiagPrintf("[诊断] 队伍+0x10 = %08X", tm);
    }
    {
        SelIter it; uint32_t cnt = 0, owner = 0, ownerM = 0;
        uintptr_t o0 = 0; uint32_t vt = 0, f138 = 0, f3cc = 0, f390 = 0, hp = 0;
        if (SelBegin(it)) {
            cnt = (uint32_t)it.count;
            uintptr_t o = SelNext(it);
            if (o) {
                o0 = o;
                SafeRead32(o, vt); SafeRead32(o + 0x138, f138);
                SafeRead32(o + 0x3CC, f3cc); SafeRead32(o + 0x390, f390);
                uintptr_t b = BodyOf(o);
                if (b) { SafeRead32(b + OFF_OWNER, owner); SafeRead32(b + OFF_HPBODY, hp);
                         if (owner) SafeRead32(owner + 0x10, ownerM); }
            }
        }
        DiagPrintf("[诊断] 选中 %u  首个obj=%08X vt=%08X [138]=%08X [3CC]=%08X [390]=%08X hp=%08X",
               cnt, (unsigned)o0, vt, f138, f3cc, f390, hp);
        DiagPrintf("[诊断] 该单位 owner=%08X owner+0x10=%08X", owner, ownerM);
    }
    DiagPrintf("[诊断] 选中数 = %d   hook 抓到单位 = %08X (tick %u, now %u)",
           SelectedCount(), g_selUnit, g_selTick, g_tickNow);
    uint8_t pb[8] = { 0 };
    uintptr_t cu = SymCreateUnit();
    if (cu) {
        SafeReadBytes(cu, pb, 8);
        DiagPrintf("[诊断] create@%08X 字节 = %02X %02X %02X %02X %02X %02X %02X %02X",
               (unsigned)cu, pb[0], pb[1], pb[2], pb[3], pb[4], pb[5], pb[6], pb[7]);
    } else {
        DiagPrintf("[诊断] 创建函数未解析到（符号解析失败）");
    }
    float pos[4];
    if (GetSpawnPos(pos, false)) DiagPrintf("[诊断] 落点 = %.1f %.1f %.1f", pos[0], pos[1], pos[2]);
    else     DiagPrintf("[诊断] 落点取不到");
}

// 日志版：写进界面「输出记录」
void DiagReport() { g_diagBuf = nullptr; DiagCore(); }

// 供 HTTP /resolve：对象类型哈希 -> 模板指针（只读）
uintptr_t ResolveTemplate(uint32_t hash) { return reinterpret_cast<uintptr_t>(FindTemplate(hash)); }

// ---- 建筑落点可建造性：直调 TheBuildAssistant::isLocationLegalToBuild ----
//
// 槽 RVA 0x8DBBCC -> BuildAssistant*（启动注册流程 `mov [槽], eax` 实证）。
// 判定函数 RVA 0x1535F0：thiscall，7 个栈参、`ret 0x1c`：
//   (const Coord3D* pos, const ThingTemplate* build, Real angle, UnsignedInt options,
//    const Object* builder, Player* player, void* outList)
// 返回 LegalBuildCode：0 = 可建；其余非 0 即不可建（具体码义见文档）。
// options 现有游戏侧调用点为 0x1A20；outList 传 0（函数体内已做空判）。
static float g_baPos[3] = { 0, 0, 0 };
static char  g_baErr[192] = { 0 };

const char* BuildableLastError() { return g_baErr[0] ? g_baErr : "ok"; }

int BuildableAt(float x, float y, float z, uint32_t tmplHash, float angle, uint32_t options) {
    g_baErr[0] = 0;
    constexpr uint32_t R_BA_SLOT   = 0x8DBBCC;   // TheBuildAssistant 全局槽
    constexpr uint32_t R_BA_LEGAL  = 0x1535F0;   // isLocationLegalToBuild
    // AOB 预检：sub esp,0x12c; push ebx; push ebp; push esi; push edi
    static const uint8_t kPro[] = { 0x81,0xEC,0x2C,0x01,0x00,0x00,0x53,0x55,0x56,0x57 };

    const uintptr_t base = GameBase();
    const uintptr_t fn = base + R_BA_LEGAL;
    if (std::memcmp(reinterpret_cast<const void*>(fn), kPro, sizeof(kPro)) != 0) {
        _snprintf_s(g_baErr, sizeof(g_baErr), _TRUNCATE, "AOB 不符 @%08X（版本不匹配）", (unsigned)fn);
        return -1;
    }
    uint32_t ba = 0;
    if (!SafeRead32(base + R_BA_SLOT, ba) || !ba) {
        _snprintf_s(g_baErr, sizeof(g_baErr), _TRUNCATE, "TheBuildAssistant 槽为空（对局未就绪？）");
        return -1;
    }
    const uintptr_t tmpl = ResolveTemplate(tmplHash);
    if (!tmpl) {
        _snprintf_s(g_baErr, sizeof(g_baErr), _TRUNCATE, "模板哈希 0x%08X 解析不到", (unsigned)tmplHash);
        return -1;
    }
    const uintptr_t player = LocalPlayerObj();
    if (!player) {
        _snprintf_s(g_baErr, sizeof(g_baErr), _TRUNCATE, "本地玩家对象未抓到（进局后点一下资源再试）");
        return -1;
    }
    g_baPos[0] = x; g_baPos[1] = y; g_baPos[2] = z;

    typedef int(__thiscall* F7)(void*, const float*, uintptr_t, float, uint32_t, uintptr_t, uintptr_t, uintptr_t);
    int r = -2;
    __try {
        r = ((F7)fn)(reinterpret_cast<void*>(static_cast<uintptr_t>(ba)), g_baPos, tmpl,
                     angle, options, 0, player, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        _snprintf_s(g_baErr, sizeof(g_baErr), _TRUNCATE, "判定函数抛异常（参数或状态不匹配）");
        r = -99;
    }
    return r;
}

// 文本版：把同样内容拼进缓冲区（供 HTTP /diag 直接返回），返回写入长度
int DiagText(char* buf, int n) {
    if (!buf || n <= 0) return 0;
    buf[0] = 0;
    g_diagBuf = buf; g_diagCap = n; g_diagLen = 0;
    DiagCore();
    g_diagBuf = nullptr;
    return g_diagLen;
}

bool SummonMCV() {
    // 盟军 MCV / 苏联 MCV / 升阳(帝国) MCV
    static const uint32_t kMcv[] = { 0x28DA574Eu, 0xAF4C0DA5u, 0x1C2EF767u };
    int ok = 0;
    for (uint32_t h : kMcv)
        if (SummonByHash(h, 1, 0, false, "base")) ++ok;
    return ok > 0;
}

// 基于 CE「Select Unit Copy For Me」(I)：用"当前对象哈希"在鼠标处创建 1 个
int ActCopySelected() {
    uint32_t base = static_cast<uint32_t>(GameBase());
    uint32_t hash = 0;
    if (!SafeRead32(base + R_CURTEMPLATE, hash) || !hash) { SetErr("当前对象哈希为空（先选中单位）"); return -1; }
    return SummonByHash(hash, 1, 0, true, "copy") ? 1 : -1;
}

// 基于 CE「We Need Back」(J)：在鼠标处创建 8 个预设单位
int ActSummonSquad() {
    return SummonByHash(0x6586A5A0u, 8, 0, true, "squad") ? 8 : -1;
}

// ===================== 调试：选中单位详情 =====================
// 给 UI 的「输出记录」面板用：把当前选中对象的内部结构摊开，便于现场排错
// （尤其是"召唤出来的单位未入世"那类问题 —— 一眼就能看出 [0x138] 是不是合法指针）。

// 对象类型哈希 → 单位表里的显示名
const char* UnitNameByHash(uint32_t hash) {
    if (!hash) return nullptr;
    for (int i = 0; i < kSupportUnitCount; ++i)
        if (kSupportUnits[i].hash == hash) return kSupportUnits[i].name;
    for (int i = 0; i < kCrateCount; ++i)
        if (kCrates[i].hash == hash) return kCrates[i].name;
    return nullptr;
}

// 往缓冲区追加（带格式），并推进写指针
static void DiagAdd(char* buf, size_t n, int& w, const char* fmt, ...) {
    if (w < 0 || static_cast<size_t>(w) >= n - 2) return;
    va_list ap; va_start(ap, fmt);
    int k = _vsnprintf_s(buf + w, n - static_cast<size_t>(w), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (k > 0) w += k;
}

int SelectedDiagText(char* buf, int n) {
    if (!buf || n < 64) return 0;
    int w = 0;
    buf[0] = 0;

    SelIter it;
    if (!SelBegin(it)) {
        DiagAdd(buf, static_cast<size_t>(n), w, "没有选中单位（先在游戏里选中一个）");
        return w;
    }
    const uint32_t base = static_cast<uint32_t>(GameBase());
    DiagAdd(buf, static_cast<size_t>(n), w, "选中 %d 个    本地玩家对象 %08X",
            it.count, (unsigned)g_localPlayerObj);

    uintptr_t obj;
    int idx = 0;
    while ((obj = SelNext(it)) != 0 && idx < 4) {
        ++idx;
        uint32_t vt = 0, tmpl = 0, hash = 0, body = 0;
        SafeRead32(obj, vt);                       // vtable
        SafeRead32(obj + 4, tmpl);                 // +0x04 = 对象模板指针
        if (tmpl) SafeRead32(tmpl + 8, hash);      // 模板 +0x08 = 类型哈希
        body = static_cast<uint32_t>(BodyOf(obj)); // 归一化到 Thing（原生走 +0x138，召唤物自身）

        const bool vtOk = (vt >= base && vt < base + 0x940000);
        DiagAdd(buf, static_cast<size_t>(n), w,
                "\n#%d  obj %08X   vtable %08X %s",
                idx, (unsigned)obj, (unsigned)vt, vtOk ? "(模块内)" : "(模块外!)");

        const char* nm = UnitNameByHash(hash);
        DiagAdd(buf, static_cast<size_t>(n), w,
                "\n     tmpl %08X   hash %08X   %s",
                (unsigned)tmpl, (unsigned)hash, nm ? nm : "(表里没有这个哈希)");

        const bool bodyOk = body && IsMemOk(body, 0x44, false);
        DiagAdd(buf, static_cast<size_t>(n), w, "\n     body %08X   %s",
                (unsigned)body,
                bodyOk ? "OK" : "✗ 取不到 body（既非 Drawable 也非 Thing）");

        if (bodyOk) {
            uint32_t hp = 0, own = 0;
            SafeRead32(body + OFF_HPBODY, hp);
            SafeRead32(body + OFF_OWNER, own);
            float px = 0, py = 0, pz = 0;
            SafeReadFloat(body + 0x38, px);
            SafeReadFloat(body + 0x3C, py);
            SafeReadFloat(body + 0x40, pz);
            DiagAdd(buf, static_cast<size_t>(n), w,
                    "\n     owner %08X   pos %.1f, %.1f, %.1f",
                    (unsigned)own, px, py, pz);
            if (hp && IsMemOk(hp, 0x14, false)) {
                float cur = 0, mx = 0, bk = 0;
                SafeReadFloat(hp + 4, cur);
                SafeReadFloat(hp + 0xC, mx);
                SafeReadFloat(hp + 0x10, bk);
                DiagAdd(buf, static_cast<size_t>(n), w,
                        "   hp %.0f/%.0f (备份 %.0f)", cur, mx, bk);
            }
        }
        uint32_t s390 = 0, s394 = 0, s3a0 = 0, s3ac = 0, bc = 0, c8 = 0;
        SafeRead32(obj + 0x390, s390);
        SafeRead32(obj + 0x394, s394);
        SafeRead32(obj + 0x3A0, s3a0);
        SafeRead32(obj + 0x3AC, s3ac);
        SafeRead32(obj + 0xBC, bc);
        SafeRead32(obj + 0xC8, c8);
        DiagAdd(buf, static_cast<size_t>(n), w,
                "\n     state 390=%08X 394=%08X 3A0=%08X 3AC=%08X  BC=%08X C8=%08X",
                s390, s394, s3a0, s3ac, bc, c8);
    }
    return w;
}

// 选中链表首个对象（Drawable；无选中返回 0）—— 供战场枚举反推 Thing vtable
uintptr_t FirstSelectedObject() {
    SelIter it;
    if (!SelBegin(it)) return 0;
    return SelNext(it);
}

// ===================== 箱子即爆（DLL 原生）=====================
// 存在的理由：MCP 层要 /summon → /obj → /aicall 三条请求，每条各占一帧，
//   箱子至少被画 2~3 帧（≈33~50 ms，肉眼就是"闪一下"）。
//   这里把「创建 + 校验 + 击杀」压进同一次命令回调 —— 命令泵在 Present hook 里跑，
//   创建时本帧的渲染列表早已收集完，击杀也在同帧完成 ⇒ 箱子一帧都不出现。
// 与 /aicall 的区别：不暴露"任意函数调用"，函数与栈参在编译期固定；函数地址走
//   SymKillUnit()（RVA 0x39EA50）而非硬编码 VA。
namespace {

const int kBurstQMax = 16;
struct BurstItem { uintptr_t thing; uint32_t hash; uint32_t dueTick; DWORD dueMs; };
BurstItem g_burstQ[kBurstQMax];
int       g_burstQN = 0;            // 只在渲染线程读写（ActBurstCrate / BurstPump 同线程）
uintptr_t g_burstLast = 0;
char      g_burstErr[160] = { 0 };

void BurstSetErr(const char* s) {
    if (s) strncpy_s(g_burstErr, s, _TRUNCATE); else g_burstErr[0] = 0;
}

// 校验 + 击杀。ecx 必须是 **Thing**（CreateUnit 的返回值），不是 body ——
//   实测传 body 不消耗箱子、死亡开火也不触发（靶无冻结指纹）；传 Thing 立刻生效。
// 地址复用防护：模板哈希必须等于本次创建时请求的哈希 ——
//   引擎会把已销毁对象的地址分给新对象（research/crates/06 §三），只认 vtable 会误杀。
bool BurstKillChecked(uintptr_t thing, uint32_t hash) {
    const uintptr_t fnAddr = SymKillUnit();
    if (!fnAddr) { BurstSetErr("击杀函数未解析到（符号解析失败）"); return false; }
    if (!LooksLikeGameObject(thing)) { BurstSetErr("对象不像游戏对象（护栏拦下）"); return false; }
    uint32_t tmpl = 0, h = 0;
    if (!SafeRead32(thing + 4, tmpl) || !tmpl) { BurstSetErr("读不到模板指针"); return false; }
    if (!SafeRead32(tmpl + 8, h) || h != hash) {
        _snprintf_s(g_burstErr, sizeof(g_burstErr), _TRUNCATE,
                    "模板哈希 %08X ≠ 请求 %08X（地址已被复用，拒绝开火）", h, hash);
        return false;
    }
    auto fn = reinterpret_cast<KillUnit_t>(fnAddr);
    __try { fn(reinterpret_cast<void*>(thing), nullptr, 6, 1, 0); }   // 拾取链原生参数
    __except (EXCEPTION_EXECUTE_HANDLER) {
        BurstSetErr("KillUnit 调用异常");
        return false;
    }
    return true;
}

} // namespace

// 创建 → 入队（至少隔一帧发力）。
//   为什么不能同帧杀：CreateUnit 当帧对象还没初始化完，Thing+4 的模板指针读回 0，
//   此时调 KillUnit 既不消耗箱子也不触发死亡开火（2026-10-18 实测）。
int ActBurstCrate(uint32_t hash, int delayMs) {
    g_burstLast = 0;
    BurstSetErr("ok");
    if (!hash) { SetErr("缺 hash"); BurstSetErr("缺 hash"); return 0; }
    if (!SummonByHashFor(hash, 1, 0, false, "burst")) {
        BurstSetErr(SummonLastError());
        return 0;
    }
    uintptr_t objs[2] = { 0, 0 };
    if (LastCreatedObjs(objs, 2) < 1 || !objs[0]) {
        BurstSetErr("创建返回成功但取不到对象地址");
        return 0;
    }
    const uintptr_t thing = objs[0];
    g_burstLast = thing;
    if (!LooksLikeGameObject(thing)) { BurstSetErr("创建出的对象不像游戏对象（护栏拦下）"); return 0; }
    if (g_burstQN >= kBurstQMax) { BurstSetErr("引爆队列已满"); return 0; }

    BurstItem& it = g_burstQ[g_burstQN++];
    it.thing = thing;
    it.hash = hash;
    it.dueTick = g_tickNow + 1;                        // 至少下一帧（对象初始化完成）
    it.dueMs = GetTickCount() + (DWORD)(delayMs > 0 ? delayMs : 0);
    return 1;
}

uintptr_t BurstLastObj() { return g_burstLast; }

const char* BurstLastError() { return g_burstErr[0] ? g_burstErr : "ok"; }

// 引爆队列推进（渲染线程每帧调用）。到期项若已被拾取/地址被复用 → BurstKillChecked 拒绝。
void BurstPump() {
    if (!g_burstQN) return;
    const DWORD now = GetTickCount();
    for (int i = 0; i < g_burstQN; ) {
        const BurstItem& it = g_burstQ[i];
        if (g_tickNow >= it.dueTick && (int)(now - it.dueMs) >= 0) {
            const uintptr_t obj = it.thing;
            if (BurstKillChecked(obj, it.hash)) R3_LOG(0, "[引爆] %08X 已引爆", (unsigned)obj);
            else                               R3_LOG(1, "[引爆] %08X 失败：%s", (unsigned)obj, BurstLastError());
            g_burstQ[i] = g_burstQ[--g_burstQN];
        } else {
            ++i;
        }
    }
}

// 选中单位按类型哈希聚合（纯内存读，跨线程安全）。返回种类数（≤ maxKinds）。
int SelectedHashHistogram(uint32_t* outHash, int* outCount, int maxKinds) {
    SelIter it;
    if (!SelBegin(it) || maxKinds <= 0) return 0;
    int n = 0;
    uintptr_t obj;
    while ((obj = SelNext(it)) != 0 && n < maxKinds) {
        uint32_t thing = static_cast<uint32_t>(BodyOf(obj)), tmpl = 0, hash = 0;
        if (!thing) continue;
        if (!SafeRead32(static_cast<uintptr_t>(thing) + 4, tmpl) || !tmpl) continue;
        if (!SafeRead32(static_cast<uintptr_t>(tmpl) + 8, hash) || !hash) continue;
        int i = 0;
        for (; i < n; ++i) if (outHash[i] == hash) break;
        if (i < n) ++outCount[i];
        else { outHash[n] = hash; outCount[n] = 1; ++n; }
    }
    return n;
}

} // namespace ra3
