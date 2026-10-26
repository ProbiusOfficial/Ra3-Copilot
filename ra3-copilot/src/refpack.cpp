// EA RefPack 解压。
//
// 输出大小在头里给全了（3 字节），所以一次性分配、不做动态增长 —— 地图最大也就
// 十几 MB，涨缩一次的代价比预分配高不到哪去，但预分配能让越界写变成明确的失败。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "refpack.h"

static const uint32_t kMaxOut = 64u << 20;

// 按给定 cap 解一次。成功返回缓冲并写 outSize；失败（越界/尺寸不符）返回 nullptr。
static void* TryDecompress(const uint8_t* d, uint32_t srcSize, uint32_t cap, uint32_t* outSize) {
    if (!cap || cap > kMaxOut) return nullptr;

    uint8_t* out = (uint8_t*)VirtualAlloc(nullptr, cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!out) return nullptr;

    uint32_t op = 0;
    uint32_t pos = 5;
    bool ok = true;

    for (;;) {
        if (pos >= srcSize) { ok = false; break; }
        const uint32_t first = d[pos++];
        uint32_t literal = 0, length = 0, offset = 0;

        if (first <= 0x7F) {                     // 短匹配
            if (pos + 1 > srcSize) { ok = false; break; }
            const uint32_t b2 = d[pos]; pos += 1;
            offset  = (((first & 0x60) << 3) | b2) + 1;
            length  = ((first & 0x1C) >> 2) + 3;
            literal = first & 0x03;
        } else if (first <= 0xBF) {               // 中匹配
            if (pos + 2 > srcSize) { ok = false; break; }
            const uint32_t b2 = d[pos], b3 = d[pos + 1]; pos += 2;
            offset  = (((b2 & 0x3F) << 8) | b3) + 1;
            length  = (first & 0x3F) + 4;
            literal = (b2 & 0xC0) >> 6;
        } else if (first <= 0xDF) {               // 长匹配
            if (pos + 3 > srcSize) { ok = false; break; }
            const uint32_t b2 = d[pos], b3 = d[pos + 1], b4 = d[pos + 2]; pos += 3;
            offset  = (((first & 0x10) << 12) | (b2 << 8) | b3) + 1;
            length  = (((first & 0x0C) << 6) | b4) + 5;
            literal = first & 0x03;
        } else if (first <= 0xFB) {               // 纯字面量
            literal = ((first & 0x1F) << 2) + 4;
        } else {                                  // 结束
            literal = first & 0x03;
            if (pos + literal > srcSize || op + literal > cap) { ok = false; break; }
            for (uint32_t i = 0; i < literal; ++i) out[op++] = d[pos++];
            break;
        }

        if (pos + literal > srcSize || op + literal > cap) { ok = false; break; }
        for (uint32_t i = 0; i < literal; ++i) out[op++] = d[pos++];

        if (length) {
            if (offset > op) { ok = false; break; }          // 引用还没写出来的位置
            if (op + length > cap) { ok = false; break; }
            // 逐字节复制：重叠区间靠这个语义才正确
            const uint32_t start = op - offset;
            for (uint32_t i = 0; i < length; ++i) out[op++] = out[start + i];
        }
    }

    if (!ok || op != cap) {
        VirtualFree(out, 0, MEM_RELEASE);
        return nullptr;
    }
    if (outSize) *outSize = op;
    return out;
}

void* RefpackDecompress(const void* src, uint32_t srcSize, uint32_t* outSize) {
    if (outSize) *outSize = 0;
    const uint8_t* d = (const uint8_t*)src;
    if (!d || srcSize < 8) return nullptr;
    if (d[1] != 0xFB || (d[0] != 0x10 && d[0] != 0x80)) return nullptr;

    const uint32_t sizeBe = ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 8) | d[4];
    const uint32_t sizeLe = (uint32_t)d[2] | ((uint32_t)d[3] << 8) | ((uint32_t)d[4] << 16);

    // 头魔数决定字节序；另一种再试一次 —— 极少数资产是混着来的，
    // 与其猜不如两种都跑，反正失败那次几乎立刻返回。
    const uint32_t firstCap  = (d[0] == 0x10) ? sizeBe : sizeLe;
    const uint32_t secondCap = (d[0] == 0x10) ? sizeLe : sizeBe;

    void* r = TryDecompress(d, srcSize, firstCap, outSize);
    if (r) return r;
    return TryDecompress(d, srcSize, secondCap, outSize);
}

void RefpackFree(void* p) {
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}
