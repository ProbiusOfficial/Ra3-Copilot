// hook.cpp - 进程内补丁引擎实现
#include "hook.h"
#include "buildinfo.h"
#include <cstring>

namespace ra3 {

uintptr_t MainModuleBase() {
    return reinterpret_cast<uintptr_t>(::GetModuleHandleA(nullptr));
}

size_t MainModuleSize() {
    auto base = reinterpret_cast<const uint8_t*>(::GetModuleHandleA(nullptr));
    if (!base) return 0;
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->OptionalHeader.SizeOfImage;
}

bool IsSteamBuild() {
    return GetBuild() == BuildKind::Steam;
}

bool VerifyBytes(uintptr_t at, const uint8_t* expect, size_t n) {
    if (!at || !expect || !n) return false;
    return std::memcmp(reinterpret_cast<const void*>(at), expect, n) == 0;
}

void* AllocCave(size_t size) {
    // 主模块附近分配优先（游戏 exe 9MB，cave 放邻近地址 rel32 无压力）
    const uintptr_t base = MainModuleBase();
    for (uintptr_t hint : { base + 0x8000000, base + 0x10000000, uintptr_t(0) }) {
        void* p = ::VirtualAlloc(reinterpret_cast<void*>(hint), size,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!p) continue;
        // E9 rel32 覆盖范围 +-2GB
        intptr_t d = reinterpret_cast<uintptr_t>(p) - base;
        if (d > INT32_MAX || d < INT32_MIN) {
            ::VirtualFree(p, 0, MEM_RELEASE);
            continue;
        }
        return p;
    }
    return nullptr;
}

bool WriteRaw(uintptr_t at, const void* data, size_t n) {
    DWORD old = 0;
    if (!::VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_EXECUTE_READWRITE, &old))
        return false;
    std::memcpy(reinterpret_cast<void*>(at), data, n);
    ::VirtualProtect(reinterpret_cast<void*>(at), n, old, &old);
    return true;
}

bool PatchJmp(uintptr_t at, void* target) {
    if (!at || !target) return false;
    uint8_t patch[5] = { 0xE9, 0, 0, 0, 0 };
    WriteRel32(patch + 1, at + 5, reinterpret_cast<uintptr_t>(target));
    return WriteRaw(at, patch, sizeof(patch));
}

bool PatchPushRet(uintptr_t at, void* target) {
    if (!at || !target) return false;
    uint8_t patch[6] = { 0x68, 0, 0, 0, 0, 0xC3 };
    std::memcpy(patch + 1, &target, 4);
    return WriteRaw(at, patch, sizeof(patch));
}

} // namespace ra3
