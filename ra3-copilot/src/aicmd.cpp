// aicmd.cpp - P2 指挥层实现（AIUpdateInterface 直调）
#include "aicmd.h"
#include "actions.h"      // GameBase()
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace ra3 {
namespace {

uintptr_t g_excAddr = 0;
uint32_t  g_excCode = 0;
char      g_err[160] = { 0 };

void SetErr(const char* fmt, ...) {
    if (!fmt) { g_err[0] = 0; return; }
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(g_err, sizeof(g_err), _TRUNCATE, fmt, ap);
    va_end(ap);
}

// 运行时 VA → RVA（ImageBase 0x400000，无 ASLR）。用 GameBase()+RVA 比硬编码 VA 稳。
const uintptr_t RVA_ATTACK = 0x76E690u - 0x400000u;   // privateAttackObject
const uintptr_t RVA_MOVE   = 0x6EFF70u - 0x400000u;   // privateMoveToPosition

// OrderMoveTo 用的持久 Coord3D（游戏会读这块内存，必须是静态存储、不能是调用者栈）
float g_moveScratch[3] = { 0, 0, 0 };

}  // namespace

uintptr_t UnitAI(uintptr_t thing) {
    if (!thing) { SetErr("null thing"); return 0; }
    uintptr_t ai = 0;
    __try {
        ai = *reinterpret_cast<uintptr_t*>(thing + 0x374);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        SetErr("read Thing+0x374 fault @%08X", (unsigned)thing);
        return 0;
    }
    if (!ai) SetErr("Thing+0x374(AI)=0 @%08X", (unsigned)thing);
    return ai;
}

uintptr_t CallThisFn(uintptr_t fn, uintptr_t thiz,
                     uintptr_t a0, uintptr_t a1, uintptr_t a2, int na) {
    g_excAddr = 0; g_excCode = 0;
    uintptr_t ret = 0;
    __try {
        if (na <= 0) {
            typedef uintptr_t(__thiscall* F0)(void*);
            ret = ((F0)fn)((void*)thiz);
        } else if (na == 1) {
            typedef uintptr_t(__thiscall* F1)(void*, uintptr_t);
            ret = ((F1)fn)((void*)thiz, a0);
        } else if (na == 2) {
            typedef uintptr_t(__thiscall* F2)(void*, uintptr_t, uintptr_t);
            ret = ((F2)fn)((void*)thiz, a0, a1);
        } else {
            typedef uintptr_t(__thiscall* F3)(void*, uintptr_t, uintptr_t, uintptr_t);
            ret = ((F3)fn)((void*)thiz, a0, a1, a2);
        }
    } __except (g_excAddr = (uintptr_t)GetExceptionInformation()->ExceptionRecord->ExceptionAddress,
                g_excCode = (uint32_t)GetExceptionInformation()->ExceptionRecord->ExceptionCode,
                EXCEPTION_EXECUTE_HANDLER) {
        ret = 0xDEADDEADu;
    }
    return ret;
}

void LastThisCallExc(uintptr_t* addr, uint32_t* code) {
    if (addr) *addr = g_excAddr;
    if (code) *code = g_excCode;
}

bool OrderAttack(uintptr_t unitThing, uintptr_t victimThing) {
    if (!victimThing) { SetErr("null victim"); return false; }
    uintptr_t ai = UnitAI(unitThing);
    if (!ai) return false;
    // privateAttackObject(victim, maxShots=0, cmdSource=0=PLAYER)
    uintptr_t r = CallThisFn(GameBase() + RVA_ATTACK, ai, victimThing, 0, 0, 3);
    if (r == 0xDEADDEADu) {
        SetErr("attack exception @%08X code=%08X", (unsigned)g_excAddr, g_excCode);
        return false;
    }
    g_err[0] = 0;   // ok
    return true;
}

bool OrderMoveTo(uintptr_t unitThing, const float* pos3) {
    if (!pos3) { SetErr("null pos"); return false; }
    uintptr_t ai = UnitAI(unitThing);
    if (!ai) return false;
    g_moveScratch[0] = pos3[0];
    g_moveScratch[1] = pos3[1];
    g_moveScratch[2] = pos3[2];
    // privateMoveToPosition(Coord3D* pos, cmdSource=0)
    uintptr_t r = CallThisFn(GameBase() + RVA_MOVE, ai, (uintptr_t)g_moveScratch, 0, 0, 2);
    if (r == 0xDEADDEADu) {
        SetErr("move exception @%08X code=%08X", (unsigned)g_excAddr, g_excCode);
        return false;
    }
    g_err[0] = 0;   // ok
    return true;
}

bool OrderIdle(uintptr_t unitThing) {
    (void)unitThing;
    SetErr("idle 未实现（privateIdle 槽位待实机确认）");
    return false;
}

const char* AicmdLastError() { return g_err[0] ? g_err : "ok"; }

} // namespace ra3
