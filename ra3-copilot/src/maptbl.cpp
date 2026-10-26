// 关卡 .map 脚本树解析 → 目标表 token 行。
//
// 格式来源：ref/ra3-map-bridge（第三方权威实现），与 research/tools/campaign_script_dump.py
// 一致。结构是严格的顺序读取，没有回溯，所以越界即视为"这份图不是这个格式"，
// 整体放弃、由调用方回落内置表 —— 不猜、不硬修。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "maptbl.h"
#include "big4.h"
#include "refpack.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define MT_MAX_HITS   1024
#define MT_MAX_NAME   96
#define MT_MAX_ARGS   4
#define MT_MAX_OBJ    8      // 一个脚本里最多认几个完成动作
#define MT_MAX_GRP    32     // 一个脚本里最多认几个 OrCondition
#define MT_MAX_NAMES  1536
#define MT_NAME_BUF   (128 * 1024)
#define MT_LINE_BUF   (512 * 1024)
#define MT_MAX_LINES  2048

// ---- 名字池 ---------------------------------------------------------------

static struct {
    int32_t     sid;
    const char* s;
    uint32_t    len;
} g_names[MT_MAX_NAMES];
static int      g_nameCount = 0;
static char     g_nameBuf[MT_NAME_BUF];
static uint32_t g_nameUsed = 0;

// 行池
static char        g_lineBuf[MT_LINE_BUF];
static uint32_t    g_lineUsed = 0;
static const char* g_lines[MT_MAX_LINES];
static int         g_lineCount = 0;

// 命中（一个目标 × 一个 OrCondition 组 = 一条路径）
static struct {
    char     obj[MT_MAX_NAME];
    uint32_t grpOff;
    uint32_t grpEnd;
} g_hits[MT_MAX_HITS];
static int g_hitCount = 0;

// ---- 读游标 ---------------------------------------------------------------
// 越界一律置 bad，之后所有读操作快速返回 0，调用方只在关键节点查一次 bad。

struct Cur {
    const uint8_t* d;
    uint32_t       n;
    bool           bad;
};

static uint8_t U8(Cur& c, uint32_t& p) {
    if (p + 1 > c.n) { c.bad = true; return 0; }
    return c.d[p++];
}

static uint16_t U16(Cur& c, uint32_t& p) {
    if (p + 2 > c.n) { c.bad = true; return 0; }
    const uint16_t v = (uint16_t)(c.d[p] | ((uint16_t)c.d[p + 1] << 8));
    p += 2;
    return v;
}

static uint32_t U24(Cur& c, uint32_t& p) {
    if (p + 3 > c.n) { c.bad = true; return 0; }
    const uint32_t v = (uint32_t)c.d[p] | ((uint32_t)c.d[p + 1] << 8) | ((uint32_t)c.d[p + 2] << 16);
    p += 3;
    return v;
}

static uint32_t U32(Cur& c, uint32_t& p) {
    if (p + 4 > c.n) { c.bad = true; return 0; }
    const uint32_t v = (uint32_t)c.d[p] | ((uint32_t)c.d[p + 1] << 8) |
                       ((uint32_t)c.d[p + 2] << 16) | ((uint32_t)c.d[p + 3] << 24);
    p += 4;
    return v;
}

static uint32_t Varint(Cur& c, uint32_t& p) {
    uint32_t r = 0, shift = 0;
    for (int i = 0; i < 5; ++i) {
        const uint8_t b = U8(c, p);
        if (c.bad) return 0;
        r |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return r;
}

// u16 长度前缀的串：返回指向 payload 内部的指针（不复制）
static bool DStr(Cur& c, uint32_t& p, const char** out, uint32_t* outLen) {
    const uint16_t len = U16(c, p);
    if (c.bad) return false;
    if (p + len > c.n) { c.bad = true; return false; }
    if (out) *out = (const char*)(c.d + p);
    if (outLen) *outLen = len;
    p += len;
    return true;
}

static void SkipDStr(Cur& c, uint32_t& p) {
    const uint16_t len = U16(c, p);
    if (c.bad) return;
    p += len;
    if (p > c.n) c.bad = true;
}

static void ChunkHdr(Cur& c, uint32_t& p, int32_t* cid, int32_t* size) {
    *cid = (int32_t)U32(c, p);
    U16(c, p);                    // version，解析用不上
    *size = (int32_t)U32(c, p);
}

// ---- 名字处理 -------------------------------------------------------------

static bool IsAscii(const char* s, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i)
        if ((uint8_t)s[i] >= 0x80) return false;
    return true;
}

// 复制进名字池。GBK 源串顺带转 UTF-8 —— 目标名可能是中文（mod 图），
// 而下游（UI / JSON）按 UTF-8 处理。
static const char* CopyName(const char* s, uint32_t len) {
    if (g_nameUsed + len * 2 + 2 >= (uint32_t)MT_NAME_BUF) return "";
    char* dst = g_nameBuf + g_nameUsed;

    if (IsAscii(s, len)) {
        memcpy(dst, s, len);
        dst[len] = 0;
        g_nameUsed += len + 1;
    } else {
        const int need = MultiByteToWideChar(936, 0, s, (int)len, nullptr, 0);
        if (need > 0) {
            wchar_t tmp[256];
            const int cap = (need < 256) ? need : 256;
            MultiByteToWideChar(936, 0, s, (int)len, tmp, cap);
            const int u8 = WideCharToMultiByte(CP_UTF8, 0, tmp, cap, nullptr, 0, nullptr, nullptr);
            if (u8 > 0 && g_nameUsed + (uint32_t)u8 + 1 < (uint32_t)MT_NAME_BUF) {
                WideCharToMultiByte(CP_UTF8, 0, tmp, cap, dst, u8, nullptr, nullptr);
                dst[u8] = 0;
                g_nameUsed += (uint32_t)u8 + 1;
            } else {
                memcpy(dst, s, len);
                dst[len] = 0;
                g_nameUsed += len + 1;
            }
        } else {
            memcpy(dst, s, len);
            dst[len] = 0;
            g_nameUsed += len + 1;
        }
    }
    return dst;
}

static const char* PoolName(int32_t sid) {
    int lo = 0, hi = g_nameCount - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (g_names[mid].sid == sid) return g_names[mid].s;
        if (g_names[mid].sid < sid) lo = mid + 1; else hi = mid - 1;
    }
    return nullptr;
}

static bool StrEq(const char* a, const char* b) {
    return a && b && strcmp(a, b) == 0;
}

// ---- 参数 -----------------------------------------------------------------

struct Arg {
    uint32_t    type;
    int32_t     ival;
    float       fval;
    const char* str;
    uint32_t    strLen;
};

static void ReadArgs(Cur& c, uint32_t& p, int32_t argc, Arg* args, int maxArgs, int* nRead) {
    *nRead = 0;
    if (argc < 0) argc = 0;
    if (argc > 64) argc = 64;              // 防御：畸形计数不能把这里拖死
    for (int32_t i = 0; i < argc && !c.bad; ++i) {
        Arg a;
        memset(&a, 0, sizeof(a));
        a.type = U32(c, p);
        if (c.bad) return;
        if (a.type == 16) {                // Position：3 个 float
            U32(c, p); U32(c, p); U32(c, p);
        } else {
            const uint32_t iv = U32(c, p);
            const uint32_t fv = U32(c, p);
            a.ival = (int32_t)iv;
            memcpy(&a.fval, &fv, 4);
            if (!DStr(c, p, &a.str, &a.strLen)) return;
        }
        if (*nRead < maxArgs) args[(*nRead)++] = a;
    }
}

// ---- 名字池解析 -----------------------------------------------------------

static bool ParsePool(Cur& c, uint32_t* outEnd) {
    if (c.n < 8 || memcmp(c.d, "CkMp", 4) != 0) return false;
    uint32_t p = 4;
    const uint32_t count = U32(c, p);
    if (c.bad || count > 4096) return false;

    g_nameCount = 0;
    g_nameUsed = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t len = Varint(c, p);
        if (c.bad || len > 512 || p + len + 4 > c.n) return false;
        const char* raw = (const char*)(c.d + p);
        p += len;
        const int32_t sid = (int32_t)U32(c, p);
        if (c.bad) return false;
        if (g_nameCount < MT_MAX_NAMES) {
            g_names[g_nameCount].sid = sid;
            g_names[g_nameCount].s = CopyName(raw, len);
            g_names[g_nameCount].len = len;
            ++g_nameCount;
        }
    }

    // 按 sid 排序，供二分
    for (int i = 1; i < g_nameCount; ++i) {
        const auto key = g_names[i];
        int j = i - 1;
        while (j >= 0 && g_names[j].sid > key.sid) { g_names[j + 1] = g_names[j]; --j; }
        g_names[j + 1] = key;
    }

    *outEnd = p;
    return true;
}

// ---- 行拼接 ---------------------------------------------------------------

static char* LineNew() {
    if (g_lineCount >= MT_MAX_LINES - 1) return nullptr;
    if (g_lineUsed + 2048 >= (uint32_t)MT_LINE_BUF) return nullptr;
    g_lines[g_lineCount] = nullptr;       // 保证调用方按 nullptr 结尾遍历
    char* p = g_lineBuf + g_lineUsed;
    p[0] = 0;
    return p;
}

static void LineCommit(char* p) {
    if (!p) return;
    const uint32_t used = (uint32_t)strlen(p);
    g_lineUsed += used + 1;
    g_lines[g_lineCount++] = p;
}

// 把依赖名去掉玩家前缀（`Soviet_2/FLAG - x` -> `FLAG - x`）
static const char* StripScope(const char* s) {
    if (!s) return "";
    const char* slash = strrchr(s, '/');
    return slash ? slash + 1 : s;
}

// 生成一条 O 行。成功返回 true，dep 数为 0 也算成功（该路径只能靠实体条件）。
static bool EmitPath(Cur& c, const char* obj, uint32_t grpOff, uint32_t grpEnd,
                     int* outDeps, int* outOther) {
    char* line = LineNew();
    if (!line) return false;

    // 先把依赖与 other 数算出来，最后一次性拼整行
    char deps[1536];
    uint32_t depUsed = 0;
    int nDeps = 0, other = 0;

    uint32_t p = grpOff;
    while (p + 10 <= grpEnd && !c.bad) {
        int32_t cid = 0, size = 0;
        ChunkHdr(c, p, &cid, &size);
        if (c.bad) break;
        const uint32_t cb0 = p, cb1 = cb0 + (uint32_t)size;
        if (cb1 > grpEnd || cb1 > c.n) { c.bad = true; break; }

        uint32_t cp = cb0;
        U32(c, cp);                                    // contentType
        U8(c, cp);                                     // assetPropertyType
        const uint32_t nameId = U24(c, cp);
        const int32_t argc = (int32_t)U32(c, cp);
        Arg args[MT_MAX_ARGS];
        int na = 0;
        ReadArgs(c, cp, argc, args, MT_MAX_ARGS, &na);
        U32(c, cp);                       // content 尾部的 enable
        int32_t inverted = 0;
        if (cp + 4 <= c.n) inverted = (int32_t)U32(c, cp);
        if (c.bad) break;


        const char* nm = PoolName((int32_t)nameId);
        char dep[MT_MAX_NAME];
        dep[0] = 0;

        // payload 里的串没有 NUL 结尾保证（长度是单独给的），先复制再当 C 串用
        char rawName[MT_MAX_NAME * 2];
        if (na >= 1 && args[0].str) {
            uint32_t nl = args[0].strLen;
            if (nl > sizeof(rawName) - 1) nl = (uint32_t)sizeof(rawName) - 1;
            memcpy(rawName, args[0].str, nl);
            rawName[nl] = 0;
        } else {
            rawName[0] = 0;
        }

        if (StrEq(nm, "FLAG") && na >= 1 && rawName[0]) {
            // FLAG 的第二个参数是期望值：FLAG(x,0) 要求该 flag 为 0，
            // 语义上等于取反，但 isInverted 是 false —— 只看 isInverted 会写反。
            int want = 1;
            if (na >= 2) want = args[1].ival;
            const bool isZero = (want == 0) != (inverted != 0);
            _snprintf_s(dep, sizeof(dep), _TRUNCATE, "%c%s",
                        isZero ? 'N' : 'F', StripScope(rawName));
        } else if (StrEq(nm, "TIMER_EXPIRED") && na >= 1 && rawName[0]) {
            _snprintf_s(dep, sizeof(dep), _TRUNCATE, "T%s", StripScope(rawName));
        } else {
            ++other;
        }

        if (dep[0]) {
            const uint32_t dl = (uint32_t)strlen(dep);
            if (depUsed + dl + 2 < sizeof(deps)) {
                if (nDeps) deps[depUsed++] = '|';
                memcpy(deps + depUsed, dep, dl);
                depUsed += dl;
                deps[depUsed] = 0;
                ++nDeps;
            }
        }
        p = cb1;
    }

    deps[depUsed] = 0;
    const int written = _snprintf_s(line, 2048, _TRUNCATE, "O|%s|%d|%s", obj, other, deps);
    if (written < 0) return false;

    LineCommit(line);
    if (outDeps) *outDeps = nDeps;
    if (outOther) *outOther = other;
    return true;
}

// ---- 脚本树 -----------------------------------------------------------------

static void HandleScript(Cur& c, uint32_t b0, uint32_t b1, int* scriptCount);

static void WalkScripts(Cur& c, uint32_t start, uint32_t end, int* scriptCount, int depth) {
    if (depth > 8) return;
    uint32_t pos = start;
    while (pos + 10 <= end && !c.bad) {
        int32_t cid = 0, size = 0;
        ChunkHdr(c, pos, &cid, &size);
        if (c.bad) return;
        const uint32_t b0 = pos, b1 = b0 + (uint32_t)size;
        if (b1 > end || b1 > c.n) { c.bad = true; return; }

        const char* nm = PoolName(cid);
        if (StrEq(nm, "Script")) {
            HandleScript(c, b0, b1, scriptCount);
        } else if (StrEq(nm, "ScriptGroup")) {
            uint32_t p = b0;
            SkipDStr(c, p);          // 组名
            p += 2;                  // active / subroutine
            if (c.bad || p > b1) return;
            WalkScripts(c, p, b1, scriptCount, depth + 1);
        }
        pos = b1;
    }
}

static void HandleScript(Cur& c, uint32_t b0, uint32_t b1, int* scriptCount) {
    ++(*scriptCount);

    // meta：4 个串 + 6 字节标志 + i32 + 2 字节 + i32 + u8 + 2 个串
    uint32_t p = b0;
    for (int i = 0; i < 4; ++i) SkipDStr(c, p);
    p += 6;
    p += 4;
    p += 2;
    p += 4;
    p += 1;
    SkipDStr(c, p);
    SkipDStr(c, p);
    if (c.bad || p > b1) return;

    char objs[MT_MAX_OBJ][MT_MAX_NAME];
    int nObj = 0;
    struct { uint32_t off, end; } grps[MT_MAX_GRP];
    int nGrp = 0;

    uint32_t q = p;
    while (q + 10 <= b1 && !c.bad) {
        int32_t cid = 0, size = 0;
        ChunkHdr(c, q, &cid, &size);
        if (c.bad) return;
        const uint32_t cb0 = q, cb1 = cb0 + (uint32_t)size;
        if (cb1 > b1 || cb1 > c.n) { c.bad = true; return; }

        const char* nm = PoolName(cid);
        if (StrEq(nm, "OrCondition")) {
            if (nGrp < MT_MAX_GRP) {
                grps[nGrp].off = cb0;
                grps[nGrp].end = cb1;
                ++nGrp;
            }
        } else if (StrEq(nm, "ScriptAction") || StrEq(nm, "ScriptActionFalse")) {
            uint32_t cp = cb0;
            U32(c, cp);                                  // contentType
            U8(c, cp);                                   // propType
            const uint32_t nameId = U24(c, cp);
            const int32_t argc = (int32_t)U32(c, cp);
            Arg args[MT_MAX_ARGS];
            int na = 0;
            ReadArgs(c, cp, argc, args, MT_MAX_ARGS, &na);
            if (c.bad) return;

            // 两种"目标完成"写法都要认：
            //   SET_MISSION_OBJECTIVE_STATUS(目标, 2) —— 显式置状态
            //   MARK_MISSION_OBJECTIVE_COMPLETED(目标) —— 直接标记完成
            // 后者在苏军 J02/J03 里是唯一写法（那两张图只认前者会得到空表），
            // 在 A04/A05/A08/J07 里则补回了漏掉的目标。
            const char* anm = PoolName((int32_t)nameId);
            bool isDone = false;
            if (StrEq(anm, "SET_MISSION_OBJECTIVE_STATUS") && na >= 2 && args[1].ival == 2)
                isDone = true;
            else if (StrEq(anm, "MARK_MISSION_OBJECTIVE_COMPLETED") && na >= 1)
                isDone = true;

            if (isDone && args[0].str && nObj < MT_MAX_OBJ) {
                const char* raw = args[0].str;
                uint32_t len = args[0].strLen;
                while (len && raw[0] == '/') { ++raw; --len; }         // strip 前导
                while (len && raw[len - 1] == '/') --len;              // strip 尾随
                const char* nm2 = CopyName(raw, len);
                if (nm2 && nm2[0]) {
                    strncpy_s(objs[nObj], MT_MAX_NAME, nm2, _TRUNCATE);
                    ++nObj;
                }
            }
        }
        q = cb1;
    }

    for (int i = 0; i < nObj; ++i) {
        for (int g = 0; g < nGrp; ++g) {
            if (g_hitCount >= MT_MAX_HITS) return;
            strncpy_s(g_hits[g_hitCount].obj, MT_MAX_NAME, objs[i], _TRUNCATE);
            g_hits[g_hitCount].grpOff = grps[g].off;
            g_hits[g_hitCount].grpEnd = grps[g].end;
            ++g_hitCount;
        }
    }
}

// ---- 对外 -------------------------------------------------------------------

static void SetErr(MapTblResult* r, const char* fmt, ...) {
    if (!r) return;
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(r->err, sizeof(r->err), _TRUNCATE, fmt, ap);
    va_end(ap);
}

bool MapTblBuild(const char* relHint, const char* mapBaseName, MapTblResult* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    g_lineCount = 0;
    g_hitCount = 0;
    g_lineUsed = 0;
    g_nameCount = 0;
    g_nameUsed = 0;
    // 行指针数组必须整体清空：LineNew 只在写第 i 行前把 g_lines[i] 置空，
    // 最后一行的下一个槽没人写。上一张表的行数比这张多时（A01 9 目标 → A02 6 目标），
    // g_lines[lineCount] 会留着上一张表的旧指针，消费方按 nullptr 遍历就会串到别的关卡去。
    for (int i = 0; i < MT_MAX_LINES; ++i) g_lines[i] = nullptr;

    if (!mapBaseName || !*mapBaseName) { SetErr(out, "地图名为空"); return false; }
    strncpy_s(out->mapName, sizeof(out->mapName), mapBaseName, _TRUNCATE);

    Big4Entry ent;
    if (!Big4FindMap(relHint, mapBaseName, &ent)) {
        SetErr(out, "在 .big 与散文件里都没找到 %s.map", mapBaseName);
        return false;
    }
    _snprintf_s(out->source, sizeof(out->source), _TRUNCATE, "%s%s",
                ent.loose ? "file: " : "", ent.loose ? ent.bigPath : ent.name);

    uint32_t rawSize = 0;
    void* raw = Big4ReadEntry(ent, &rawSize);
    if (!raw) { SetErr(out, "读取 .map 条目失败"); return false; }

    // 两层：外层 refpack 解出 "EAR\0" + 8 字节头 + 内层 refpack
    uint32_t l1Size = 0;
    void* l1 = RefpackDecompress(raw, rawSize, &l1Size);
    Big4BufFree(raw);
    if (!l1) { SetErr(out, ".map 外层解压失败（%u 字节）", rawSize); return false; }
    if (l1Size < 12 || memcmp(l1, "EAR\0", 4) != 0) {
        RefpackFree(l1);
        SetErr(out, ".map 头不是 EAR/NWB 格式");
        return false;
    }

    uint32_t paySize = 0;
    void* payload = RefpackDecompress((const uint8_t*)l1 + 8, l1Size - 8, &paySize);
    RefpackFree(l1);
    if (!payload) { SetErr(out, ".map 内层解压失败"); return false; }

    Cur c;
    c.d = (const uint8_t*)payload;
    c.n = paySize;
    c.bad = false;

    uint32_t poolEnd = 0;
    if (!ParsePool(c, &poolEnd)) {
        RefpackFree(payload);
        SetErr(out, "名字池(CkMp)解析失败，%u 字节", paySize);
        return false;
    }

    // 顶层：找 PlayerScriptsList
    int scriptCount = 0;
    uint32_t pos = poolEnd;
    bool found = false;
    while (pos + 10 <= c.n && !c.bad) {
        int32_t cid = 0, size = 0;
        ChunkHdr(c, pos, &cid, &size);
        if (c.bad) break;
        const uint32_t b0 = pos, b1 = b0 + (uint32_t)size;
        if (b1 > c.n) { c.bad = true; break; }

        const char* nm = PoolName(cid);
        if (StrEq(nm, "PlayerScriptsList")) {
            found = true;
            uint32_t q = b0;
            while (q + 10 <= b1 && !c.bad) {
                int32_t scid = 0, ssz = 0;
                ChunkHdr(c, q, &scid, &ssz);
                if (c.bad) break;
                const uint32_t sb1 = q + (uint32_t)ssz;
                if (sb1 > b1 || sb1 > c.n) { c.bad = true; break; }
                if (StrEq(PoolName(scid), "ScriptList"))
                    WalkScripts(c, q, sb1, &scriptCount, 0);
                q = sb1;
            }
            break;
        }
        pos = b1;
    }

    out->scriptCount = scriptCount;

    if (!found) {
        const char* err = c.bad ? "脚本树越界（格式不符）" : "没有 PlayerScriptsList（不是地图？）";
        RefpackFree(payload);
        SetErr(out, "%s", err);
        return false;
    }
    if (c.bad) {
        RefpackFree(payload);
        SetErr(out, "脚本树解析越界（chunk %d 处）", scriptCount);
        return false;
    }

    // 按目标首次出现顺序分组输出
    if (g_hitCount == 0) {
        RefpackFree(payload);
        SetErr(out, "脚本里没有 SET_MISSION_OBJECTIVE_STATUS(...,2)，%d 个脚本", scriptCount);
        return false;
    }

    char* mline = LineNew();
    if (mline) {
        _snprintf_s(mline, 2048, _TRUNCATE, "M|%s", mapBaseName);
        LineCommit(mline);
        ++out->mapCount;
    }

    bool seen[MT_MAX_HITS];
    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < g_hitCount; ++i) {
        if (seen[i]) continue;
        const char* obj = g_hits[i].obj;
        bool first = true;
        for (int j = i; j < g_hitCount; ++j) {
            if (seen[j] || strcmp(g_hits[j].obj, obj) != 0) continue;
            seen[j] = true;
            int nd = 0, oth = 0;
            if (!EmitPath(c, obj, g_hits[j].grpOff, g_hits[j].grpEnd, &nd, &oth)) continue;
            if (first) { ++out->objCount; first = false; }
            ++out->pathCount;
        }
    }

    RefpackFree(payload);

    if (c.bad) {
        SetErr(out, "生成过程中越界，结果不完整");
        return false;
    }

    g_lines[g_lineCount] = nullptr;      // 显式收尾，消费方（ObjTableParse）按 nullptr 遍历
    out->lines = g_lines;
    out->lineCount = g_lineCount;
    return g_lineCount > 1;
}

void MapTblFree(MapTblResult* r) {
    if (!r) return;
    // 行与名字都在静态池里，换图时被下一次构建覆盖，这里只清描述符
    r->lines = nullptr;
    r->lineCount = 0;
    r->mapCount = r->objCount = r->pathCount = r->scriptCount = 0;
    r->mapName[0] = 0;
    r->source[0] = 0;
    r->err[0] = 0;
}
