// sdk.h - RA3 (日冕 mod, v1.12.3444.25830) 内存常量总表
// 基于 CE 修改器的地址表，并对本机 ra3_1.12.game 逐字节验证（特征码校验通过才启用）
// 所有 RVA 相对 ra3_1.12.game 主模块基址（运行时 GetModuleHandle(NULL)）
#pragma once
#include <cstdint>

namespace ra3 {

// ---- 目标进程/版本（fail-closed 提示用）----
constexpr const char* GAME_PROC_NAME = "ra3_1.12.game";
constexpr const char* GAME_VER_STR   = "1.12.3444.25830";

// ---- 钩子点（基于 CE，字节级验证 MATCH）----
// 资金：add edi,[eax+4]; mov edx,[ecx]   -> eax = player 对象, [eax+4] = 现金
constexpr uint32_t RVA_MONEY    = 0x6CFDFE;
constexpr uint8_t  AOB_MONEY[5] = { 0x03, 0x78, 0x04, 0x8B, 0x11 };

// 本地玩家：mov edx,[eax+28]; mov eax,[edx+20] -> eax = 本地玩家对象
constexpr uint32_t RVA_PLAYERID    = 0x0FF95B;
constexpr uint8_t  AOB_PLAYERID[6] = { 0x8B, 0x50, 0x28, 0x8B, 0x42, 0x20 };

// ---- 结构偏移（基于 CE，多路内存对拍验证）----
constexpr uint32_t OFF_MONEY     = 0x04;   // [player+4]  = 现金 int32
constexpr uint32_t OFF_PLAYERID  = 0x0C;   // [player+0xC] = 玩家 ID
constexpr uint32_t OFF_LEVEL     = 0x418;  // [player+0x418] = 等级

// ---- 附录弹药：v0.2 后续功能候选（未实现，先登记）----
// 电力   RVA 0x6CFD0D  [eax+4]=100000, [eax+8]=0
// 科技点 RVA 0x6CFE6C  [eax+0x34]=15
// 全科技 RVA 0x6CFEB5  [edi+0x2c]=400.0f
// 快建   RVA 0x30F42E / 0x30F38C / 0x2F6CFF  [esi+0x1c]=100.0f
// 玩家无敌 RVA 0x12EEDF [unit+4]=9999999.0f
// 秒杀   RVA 0x3651AE  [esi+4]=0
// 全图   RVA 0x3C226C  [ebp+0x260]=99999.0f

} // namespace ra3
