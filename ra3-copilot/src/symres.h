// symres.h - 版本无关的"符号解析"：特征码(AOB) + 结构推导，替代硬编码 RVA
//
// 背景：基于 CE 的地址表只对 1.12 有效；换版本 / 换 mod 之后即失效。
// 目标：换版本 / 换 mod 后，靠**特征码**与**结构关系**重新定位这些地址。
#pragma once
#include <cstdint>

namespace ra3 {

// 解析并缓存全部符号（OverlayInit 里调一次；结果写进输出记录）
void SymInit();

// ---- 函数地址 ----
uintptr_t SymCreateUnit();     // cdecl(0, tmpl, float* pos, team, team+0x10) -> object*
uintptr_t SymLevelUp();        // __fastcall(unit) + cdecl(3, 1, 0)
uintptr_t SymKillUnit();       // __fastcall(unit) + cdecl(6, 19, 0)
uintptr_t SymTmplFind();       // __fastcall(store, hash) -> template*
uintptr_t SymMouseMap();       // 鼠标地图坐标函数

// ---- 全局变量地址（其"值"才是对象指针）----
uintptr_t SymThingFactory();   // 对象模板库（TheThingFactory）
uintptr_t SymSelMgr();         // 选中管理器：+0x50 链表头 / +0x5c 数量

// ---- 可直接改字节的静态站点 ----
uintptr_t SymFreeBuildSite();  // `75 0C` -> `EB 0C`

// 每个符号是怎么来的（用于诊断显示）
const char* SymHow(const char* name);

} // namespace ra3
