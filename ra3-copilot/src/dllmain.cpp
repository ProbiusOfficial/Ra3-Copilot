// dllmain.cpp - DLL 入口：loader lock 里只起线程，干活放到工作线程
#include <windows.h>
#include "features.h"
#include "overlay.h"

static DWORD WINAPI WorkerThread(LPVOID) {
    // 一站式初始化：功能钩子 -> 等窗口 -> WndProc hook -> D3D9 vtable hook -> 输入线程
    ra3::OverlayInit();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        HANDLE t = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
