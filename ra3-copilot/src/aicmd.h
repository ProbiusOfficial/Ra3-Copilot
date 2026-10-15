// aicmd.h - P2 指挥层：AIUpdateInterface 直调（移动 / 攻击 / 待机）
//
// 语义链路：单位 Thing 地址 → 取其 AIUpdate（Thing+0x374） → 调 private* 虚函数：
//   · privateAttackObject(victim, maxShots, cmdSource)  运行时 VA 0x76E690（ret 0xC, na=3）
//   · privateMoveToPosition(Coord3D* pos, cmdSource)    运行时 VA 0x6EFF70（ret 8,   na=2）
//
// ★ 必须在渲染线程（游戏主线程）执行；地址为运行时 VA（ImageBase 0x400000，无 ASLR）。
// ★ na（栈参数量）必须与被调方 ret N/4 严格一致，否则调用器尾声 esp 失衡跳飞。
// ★ 联机禁用；本模块只对"本局存在的活体对象"下发命令。
#pragma once
#include <cstdint>

namespace ra3 {

// 取对象的 AIUpdate 指针（Thing+0x374）；失败返回 0（并写 AicmdLastError）
uintptr_t UnitAI(uintptr_t thing);

// 正统 thiscall 调用器（ecx=this；栈参由被调方清理，无失衡）
uintptr_t CallThisFn(uintptr_t fn, uintptr_t thiz,
                     uintptr_t a0, uintptr_t a1, uintptr_t a2, int na);
// 最近一次 CallThisFn 的异常地址/码（供诊断；null 表示 0）
void LastThisCallExc(uintptr_t* addr, uint32_t* code);

// ---- 命令（返回 true=已成功下发；失败原因见 AicmdLastError）----
bool OrderAttack(uintptr_t unitThing, uintptr_t victimThing);  // privateAttackObject @0x76E690
bool OrderMoveTo(uintptr_t unitThing, const float* pos3);      // privateMoveToPosition @0x6EFF70
bool OrderIdle(uintptr_t unitThing);                           // privateIdle（待验证）

// 最近一次失败原因（成功时为 "ok"）
const char* AicmdLastError();

} // namespace ra3
