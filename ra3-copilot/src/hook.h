// hook.h - 进程内补丁引擎（DLL 注入后在游戏进程内直接操作，无需跨进程）
#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>
#include "sdk.h"
#include "buildinfo.h"   // 构建决议与 Pick 双列取值：调用方经 hook.h 即可见

namespace ra3 {

// 主模块（exe）基址与大小：进程内 GetModuleHandle(NULL) + PE 头解析
uintptr_t MainModuleBase();
size_t    MainModuleSize();

// 当前构建是否为 Steam TUC 版。识别与决议见 buildinfo.h —— 这里只是便捷谓词，
// 读的是已决议的全局值（自动判定或玩家强制），不再自己算 SizeOfImage。
bool IsSteamBuild();

// AOB fail-closed：先读后比对，不符绝不碰
bool VerifyBytes(uintptr_t at, const uint8_t* expect, size_t n);

// RWX code cave：靠近主模块分配（E9 rel32 覆盖范围 +-2GB）
// 返回 cave 基址；失败返回 nullptr
void* AllocCave(size_t size);

// 5 字节 E9 detour（替换 5 字节原始指令，原指令须在 cave 内重放）
bool PatchJmp(uintptr_t at, void* target);

// 6 字节 push/ret detour（替换 6 字节原始指令，原指令须在 cave 内重放）
bool PatchPushRet(uintptr_t at, void* target);

// 原始字节写回（还原源 = SDK 的 AOB 常量，即原始指令字节）
bool WriteRaw(uintptr_t at, const void* data, size_t n);

// 把 rel32 跳转/调用字节写到指定位置（小工具）
inline void WriteRel32(uint8_t* p, uintptr_t from_next, uintptr_t to) {
    *(uint32_t*)p = (uint32_t)(to - from_next);
}

} // namespace ra3
