// d3d9_probe.cpp - x86 探针：dump IDirect3DDevice9 / SwapChain vtable 与关键函数字节
// 目的：为 inline hook（挂起线程 + patch 函数体）确定前几字节是否是完整指令边界
#include <windows.h>
#include <d3d9.h>
#include <cstdio>

int main() {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "d3d9probe";
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA("d3d9probe", "", WS_OVERLAPPEDWINDOW,
                             0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
    if (!wnd) { printf("CreateWindow failed\n"); return 1; }

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("Direct3DCreate9 failed\n"); return 1; }

    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = wnd;
    IDirect3DDevice9* dev = NULL;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    printf("CreateDevice hr=0x%08lx dev=%p\n", (unsigned long)hr, (void*)dev);
    if (!dev) { d3d->Release(); return 1; }

    HMODULE d3 = GetModuleHandleA("d3d9.dll");
    MODULEINFO mi = {0};
    GetModuleInformation(GetCurrentProcess(), d3, &mi, sizeof(mi));

    void** vt = *(void***)dev;
    void** vted = *(void***)d3d;
    printf("d3d9.dll base=%p size=0x%lx\n", (void*)mi.lpBaseOfDll, (unsigned long)mi.SizeOfImage);
    printf("device vtable @ %p\n", (void*)vt);
    printf("IDirect3D9 vtable @ %p\n", (void*)vted);

    struct { const char* name; int idx; } devm[] = {
        {"Reset", 16}, {"Present", 17}, {"EndScene", 42}, {"Clear", 43}
    };
    printf("--- IDirect3DDevice9 ---\n");
    for (int i = 0; i < 4; i++) {
        unsigned char* fn = (unsigned char*)vt[devm[i].idx];
        ULONG_PTR base = (ULONG_PTR)mi.lpBaseOfDll;
        int inside = (ULONG_PTR)fn >= base && (ULONG_PTR)fn < base + mi.SizeOfImage;
        printf("%-9s idx=%2d fn=%p rva=0x%06lx %s bytes=",
               devm[i].name, devm[i].idx, (void*)fn,
               (unsigned long)((ULONG_PTR)fn - base), inside ? "[in d3d9]" : "[OUTSIDE]");
        for (int k = 0; k < 16; k++) printf("%02X ", fn[k]);
        printf("\n");
    }
    printf("--- IDirect3D9 ---\n");
    for (int idx = 16; idx <= 17; idx++) {
        unsigned char* fn = (unsigned char*)vted[idx];
        printf("idx=%2d fn=%p bytes=", idx, (void*)fn);
        for (int k = 0; k < 16; k++) printf("%02X ", fn[k]);
        printf("\n");
    }

    IDirect3DSwapChain9* sc = NULL;
    if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
        void** svt = *(void***)sc;
        unsigned char* fn = (unsigned char*)svt[3];
        printf("--- IDirect3DSwapChain9 ---\n");
        printf("vtable @ %p  Present idx=3 fn=%p bytes=", (void*)svt, (void*)fn);
        for (int k = 0; k < 16; k++) printf("%02X ", fn[k]);
        printf("\n");
        sc->Release();
    }
    dev->Release();
    d3d->Release();
    DestroyWindow(wnd);
    return 0;
}
