#pragma once
#include <stdint.h>

// EA RefPack 解压（RA3 变体）。头两字节 = 0x10 0xFB 或 0x80 0xFB，
// 之后 3 字节是解压后大小（0x10 用大端，0x80 用小端）。
//
// 返回解压缓冲，调用方用 RefpackFree 释放；失败返回 nullptr。
void* RefpackDecompress(const void* src, uint32_t srcSize, uint32_t* outSize);
void  RefpackFree(void* p);
