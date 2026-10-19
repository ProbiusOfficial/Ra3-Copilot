// probe.cpp - harmless DLL used to validate the injection path end to end.
#include <windows.h>

static void Marker() {
    HANDLE f = CreateFileW(L"C://Project//RA3//ra3-copilot//build_out//probe_hit.txt",
                           GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    const char msg[] = "probe dll loaded\n";
    WriteFile(f, msg, sizeof(msg) - 1, &w, nullptr);
    CloseHandle(f);
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        Marker();
    }
    return TRUE;
}
