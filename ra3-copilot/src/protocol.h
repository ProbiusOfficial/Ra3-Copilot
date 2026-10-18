// protocol.h - 秘密协议（PlayerTech / Upgrade / SpecialPower）
#pragma once
#include <cstdint>
#include <cstddef>

namespace ra3 {

int  ProtocolCount();
const char* ProtocolFaction(int idx);
const char* ProtocolName(int idx);
const char* ProtocolTech(int idx);
const char* ProtocolUpgrade(int idx);
const char* ProtocolPower(int idx);

int  ProtocolPoints();
float ProtocolScience();
bool ProtocolSetPoints(int n);
bool ProtocolSetScience(float v);

// 开关级：0=全协议 1=快速冷却 2=禁用技能
bool ProtocolSetToggle(int which, bool on);
bool ProtocolGetToggle(int which);

// 按条授予：先解锁 PlayerTech，再授予 SpecialPower（实验通道，效果需实机确认）
// player 传脚本玩家名（如 "<This Player>" / "Player_1"）；返回 0 失败 / 1 已投递
int  ProtocolGrant(int idx, const char* player, char* out, size_t outsz);
// 把脚本引擎“当前对象”上下文指向本地玩家，再用 <This Object> 授予（实验）
int  ProtocolGrantLocal(int idx, char* out, size_t outsz);
// 直接按 PlayerTech hash 找 TechNode 并追加到玩家的协议 vector（行为验证路径）
int  ProtocolGrantNode(uint32_t hash, char* out, size_t outsz);
// 原生入口：hash -> 升级模板 -> grant(team, template, 2, 1)
int  ProtocolNativeGrant(uint32_t hash, char* out, size_t outsz);
// 只读诊断：记录游戏动作执行器 0x5A2B40 的 action index / 参数
int  ProtocolTraceExecInstall();
int  ProtocolTraceExecRead(char* out, size_t outsz);
const char* ProtocolLastError();

} // namespace ra3
