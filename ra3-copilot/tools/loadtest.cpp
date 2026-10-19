// loadtest.cpp - load a DLL in a harmless process to check load-time safety.
// usage: loadtest.exe <dll-path> [wait-ms]
// exit: 0 loaded and alive, 1 load failed, 3 crashed
#include <windows.h>
#include <cstdio>
#include <cstdlib>

static int OnLoadCrash(EXCEPTION_POINTERS* ep) {
    wprintf(L"[x] LoadLibrary raised code=%08X at %p\n",
            ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress);
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        wprintf(L"usage: loadtest.exe <dll> [wait-ms]\n");
        return 2;
    }
    DWORD wait_ms = 2000;
    if (argc >= 3) wait_ms = (DWORD)_wtoi(argv[2]);
    SetErrorMode(SEM_NOGPFAULTERRORBOX);

    if (wcscmp(argv[1], L"-") == 0) {          // sleeper mode for injection tests
        wprintf(L"[+] sleeper pid=%lu sleeping %lu ms\n", GetCurrentProcessId(), wait_ms);
        fflush(stdout);
        Sleep(wait_ms);
        return 0;
    }

    HMODULE h = nullptr;
    __try {
        h = LoadLibraryW(argv[1]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        EXCEPTION_POINTERS* ep = GetExceptionInformation();
        wprintf(L"[x] LoadLibrary raised code=%08X at %p
",
                ep->ExceptionRecord->ExceptionCode,
                ep->ExceptionRecord->ExceptionAddress);
        fflush(stdout);
        return 3;
    }
    if (h == nullptr) {
        wprintf(L"[x] LoadLibrary failed err=%lu
", GetLastError());
        return 1;
    }
    wprintf(L"[+] loaded at %p, waiting %lu ms\n", (void*)h, wait_ms);
    Sleep(wait_ms);
    wprintf(L"[+] still alive -> LoadLibrary OK\n");
    return 0;
}
