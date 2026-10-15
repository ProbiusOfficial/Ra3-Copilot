// overlay.cpp - 真·游戏内绘制（进程内 hook d3d9，直接画在游戏渲染输出上）
//
// 渲染做法：进程内 d3d9 inline-hook（不注入独立窗口）：
//   自己造 dummy device → 读 IDirect3DDevice9 vtable[16/17/42] 的**函数地址**
//   → MinHook inline-hook 这些函数（位于 d3d9.dll，被游戏自己的 device 共用）
//   → Present detour 里用「游戏 device + 游戏窗口」渲染 ImGui → 再调原 Present。
//   （详见 docs/局内渲染解决方案.md）
//
// UI 布局：顶栏信息 + 分类页 + 底部「输出记录」日志区。
#include <windows.h>
#include <windowsx.h>
#include <intrin.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <vector>
#include <string>
#include "overlay.h"
#include "features.h"
#include "actions.h"
#include "symres.h"
#include "httpapi.h"
#include "battlefield.h"
#include "units_corona.h"
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "MinHook.h"
#include "hde/hde32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace ra3 {

OverlayConfig g_cfg;
OverlayStatus g_ov;

namespace {

volatile bool g_running = false;
uint32_t g_telemEndScene = 0;
uint32_t g_telemPresent = 0;
uint32_t g_telemWndHooked = 0;
uint32_t g_telemScanRounds = 0;
uint32_t g_telemVtablesHooked = 0;
HookStatus g_hookSt;

bool g_mhInited = false;

void* g_hookedTargets[16] = {};
int   g_hookedCount = 0;

typedef HRESULT (STDMETHODCALLTYPE* EndScene_t)(IDirect3DDevice9*);
typedef HRESULT (STDMETHODCALLTYPE* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*,
                                               HWND, const RGNDATA*);
typedef HRESULT (STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE* SwapPresent_t)(IDirect3DSwapChain9*, const RECT*, const RECT*,
                                                   HWND, const RGNDATA*, DWORD);
typedef HRESULT (STDMETHODCALLTYPE* SetTransform_t)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE,
                                                    const D3DMATRIX*);
EndScene_t    g_origEndScene    = nullptr;
Present_t     g_origPresent     = nullptr;
Reset_t       g_origReset       = nullptr;
SwapPresent_t g_origSwapPresent = nullptr;
SetTransform_t g_origSetTransform = nullptr;
WNDPROC       g_origWndProc     = nullptr;

// 菜单在游戏窗口客户区里的矩形（用于"吞掉落在菜单上的鼠标消息"）
// 可见面板的矩形（每帧由 BuildUI 填充，最多 12 个）——
// 多浮动面板后不能再只用一个矩形：鼠标落在【任一】面板上都要吞掉消息，否则点面板=点游戏世界
float g_panelRects[12][4] = { {0,0,0,0} };
volatile LONG g_panelRectN = 0;

// d3d9 视图矩阵（相机）——用于"视角中心"落点
float g_viewMat[16] = { 0 };
float g_projMat[16] = { 0 };
int   g_viewport[4] = { 0, 0, 0, 0 };   // x,y,w,h
volatile LONG g_hasView = 0;
volatile LONG g_hasProj = 0;
volatile LONG g_rejView = 0;    // 被"相机过滤"拒掉的 VIEW 矩阵次数（UI/2D 那些）
volatile LONG g_viewFromHook = 0;   // 1=VIEW 来自 SetTransform hook；0=来自 GetTransform 兜底（shader 游戏常返回单位矩阵，可疑）
volatile LONG g_viewRowMajor = 1;   // 1=视图矩阵已是行向量约定(v*M)；0=已从列主序转置归一
uint32_t g_vtInfo[8] = { 0 };   // vtable 诊断：[0]=16 [1]=17 [2]=42 [3]=44 [4]=45 [5]=device [6]=d3d9 base
// 原始 VIEW 矩阵诊断（不过滤、原样记录）：核对"过滤阈值是否搞反/行列约定是否不同"
float g_viewLast[16] = { 0 };   // 最后一次收到的 VIEW 矩阵
float g_viewLastFz = 0.f;       // 最后一次矩阵的 _33
float g_viewMinFz  = 999.f;     // 历史最小 _33
float g_viewMinMat[16] = { 0 };// 最小 _33 对应的矩阵
volatile LONG g_viewCount = 0;  // VIEW 调用总次数

// ===================== 输出记录（日志环） =====================
enum { LOG_INFO = 0, LOG_OK, LOG_WARN, LOG_ERR };
const ImVec4 kBtnOn(0.16f, 0.45f, 0.34f, 1.0f);   // 开关类按钮"已开启"的底色

// ---- 颜色栈护栏（2026-10-15 实机踩坑）----
// 症状：点「锁定当前金额」时弹出 ImGui 断言框
//   `Assertion failed! imgui.cpp: "Calling PopStyleColor() too many times!"`
// 根因：按钮回调把开关状态翻转了，而 Push / Pop 分别在"点击前 / 点击后"各判一次状态
//   → 点击那一帧要么"没 Push 却 Pop"（多弹 → 断言），要么"Push 了没 Pop"（泄漏）。
// 对策：① 所有 Push/Pop 一律走下面这对函数（带计数）；
//       ② 每帧结束 DrainColorStack() 把遗留的 push 清干净 —— 断言框从此不可能再出现。
int g_colStack = 0;
inline void PushBtnOn()  { ImGui::PushStyleColor(ImGuiCol_Button, kBtnOn); ++g_colStack; }
inline void PopBtnOn()   { if (g_colStack > 0) { ImGui::PopStyleColor(); --g_colStack; } }
inline void DrainColorStack() { while (g_colStack > 0) { ImGui::PopStyleColor(); --g_colStack; } }
struct LogLine { char text[176]; int kind; float t; };
const int kLogMax = 256;
LogLine g_log[kLogMax];
int  g_logCount = 0;
float g_logClock = 0.f;
bool g_logDirty = false;

// ---------- 详细日志落盘（崩溃定位） ----------
// 开关打开后：每一条「输出记录」都带时间戳 + 帧号追加写进 <DLL 目录>\ra3copilot_log.txt，
// 另有 5 秒一次的心跳（见 OverlayTick）。崩溃后看文件末尾 —— 最后一行就是死掉的那一步；
// 配合 ra3copilot_last.txt（记录最后一个危险动作及其参数）一起定位。
// 注：AddLog 会被渲染线程与 HTTP 线程同时调用，落盘必须加锁。
CRITICAL_SECTION g_logCs;
LONG g_logCsInit = 0;
char g_logPath[MAX_PATH] = { 0 };

void EnsureLogInit() {
    if (InterlockedCompareExchange(&g_logCsInit, 1, 0) != 0) return;
    InitializeCriticalSection(&g_logCs);
    char dir[MAX_PATH] = { 0 };
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&EnsureLogInit), &self);
    GetModuleFileNameA(self, dir, MAX_PATH);
    char* p = strrchr(dir, '\\');
    if (p) *(p + 1) = 0;
    _snprintf_s(g_logPath, sizeof(g_logPath), _TRUNCATE, "%sra3copilot_log.txt", dir);
}

// 追加一行到日志文件（开关判断放在调用点：崩溃 handler 要无视开关、强制写）
void WriteDiskLog(int kind, const char* msg) {
    EnsureLogInit();
    static const char* kn[4] = { "INFO", "OK  ", "WARN", "ERR " };
    EnterCriticalSection(&g_logCs);
    HANDLE h = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        SYSTEMTIME st; GetLocalTime(&st);
        char line[320];
        int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
                            "[%02d:%02d:%02d.%03d] t=%u %s %s\r\n",
                            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                            (unsigned)g_tickNow, kn[kind & 3], msg);
        DWORD wr = 0;
        if (n > 0) WriteFile(h, line, (DWORD)n, &wr, nullptr);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_logCs);
}

void AddLog(int kind, const char* fmt, ...) {
    char buf[176];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    const int idx = g_logCount % kLogMax;
    strncpy_s(g_log[idx].text, sizeof(g_log[idx].text), buf, _TRUNCATE);
    g_log[idx].kind = kind;
    g_log[idx].t = g_logClock;
    ++g_logCount;
    g_logDirty = true;
    if (g_cfg.diskLog) WriteDiskLog(kind, buf);
}

// ---------- 未处理异常捕获（真·崩溃现场） ----------
// 游戏未处理的异常触发时，把「异常码 / 出错地址 / 所属模块 / 寄存器」写进日志文件 ——
// 一眼就能看出崩在**我们 DLL / 游戏本体 / d3d9** 哪一边。无视日志开关，强制记录。
LONG WINAPI Ra3CrashFilter(EXCEPTION_POINTERS* ep) {
    __try {
        if (ep && ep->ExceptionRecord && ep->ContextRecord) {
            EXCEPTION_RECORD* er = ep->ExceptionRecord;
            CONTEXT* ctx = ep->ContextRecord;
            char modPath[MAX_PATH] = "?";
            char modName[MAX_PATH] = "?";
            HMODULE mod = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(ctx->Eip), &mod) && mod) {
                if (GetModuleFileNameA(mod, modPath, MAX_PATH)) {
                    const char* b = strrchr(modPath, '\\');
                    strncpy_s(modName, b ? b + 1 : modPath, _TRUNCATE);
                }
            }
            char b[512];
            _snprintf_s(b, sizeof(b), _TRUNCATE,
                        "[崩溃] code=%08X 出错地址=%08X 模块=%s | "
                        "EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X EBP=%08X ESP=%08X",
                        (unsigned)er->ExceptionCode, (unsigned)ctx->Eip, modName,
                        (unsigned)ctx->Eax, (unsigned)ctx->Ebx, (unsigned)ctx->Ecx,
                        (unsigned)ctx->Edx, (unsigned)ctx->Esi, (unsigned)ctx->Edi,
                        (unsigned)ctx->Ebp, (unsigned)ctx->Esp);
            WriteDiskLog(LOG_ERR, b);
            // 顺带把出错地址也写进面包屑文件（两张表一起看）
            char b2[192];
            _snprintf_s(b2, sizeof(b2), _TRUNCATE,
                        "[崩溃] code=%08X at %08X (%s)\r\n",
                        (unsigned)er->ExceptionCode, (unsigned)ctx->Eip, modName);
            CrashLogAppend(b2);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return EXCEPTION_CONTINUE_SEARCH;   // 交回系统，保留游戏原有的崩溃行为
}

void LogF(const char* fmt, ...) {
    char buf[176];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    AddLog(LOG_INFO, "%s", buf);
}

// ===================== 窗口 / 子类化 =====================
BOOL CALLBACK EnumGameWndProc(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) &&
        GetWindow(h, GW_OWNER) == nullptr) {
        *reinterpret_cast<HWND*>(lp) = h;
        return FALSE;
    }
    return TRUE;
}

HWND FindGameWnd() {
    HWND found = nullptr;
    EnumWindows(EnumGameWndProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

LRESULT CALLBACK OverlayWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    // ★ 关键：鼠标落在菜单上时，把鼠标消息**吞掉**，不要传给游戏
    //   （否则点菜单等于点游戏世界 → 选择丢失/下错命令）
    if (g_ov.imguiReady && g_cfg.visible && g_cfg.blockInput) {
        bool havePt = true;
        POINT pt = { 0, 0 };
        switch (msg) {
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            pt.x = (short)LOWORD(l); pt.y = (short)HIWORD(l);
            ScreenToClient(h, &pt);
            break;
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP:
            pt.x = GET_X_LPARAM(l); pt.y = GET_Y_LPARAM(l);
            break;
        case WM_SETCURSOR:
            GetCursorPos(&pt); ScreenToClient(h, &pt);
            break;
        default:
            havePt = false;
            break;
        }
        if (havePt) {
            bool over = false;
            const LONG pn = g_panelRectN;
            for (LONG i = 0; i < pn && i < 12; ++i) {
                if (pt.x >= g_panelRects[i][0] && pt.x < g_panelRects[i][2] &&
                    pt.y >= g_panelRects[i][1] && pt.y < g_panelRects[i][3]) { over = true; break; }
            }
            if (over) {
                if (msg == WM_SETCURSOR) { SetCursor(LoadCursorA(nullptr, IDC_ARROW)); return TRUE; }
                if (g_ov.imguiReady) ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
                return 0;   // 吞掉，不进游戏
            }
        }
    }
    if (g_ov.imguiReady)
        ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
    return CallWindowProcW(g_origWndProc, h, msg, w, l);
}

HWND GetDeviceWindow(IDirect3DDevice9* dev) {
    __try {
        if (dev) {
            D3DDEVICE_CREATION_PARAMETERS cp = {};
            if (SUCCEEDED(dev->GetCreationParameters(&cp))) {
                if (cp.hFocusWindow) return cp.hFocusWindow;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    HWND h = FindWindowW(L"Command & Conquer(tm) Red Alert(tm) 3", nullptr);
    if (h) return h;
    h = FindGameWnd();
    if (h) return h;
    return GetForegroundWindow();
}

void EnsureImGui(IDirect3DDevice9* dev) {
    if (g_ov.imguiReady || !dev) return;
    HWND hwnd = GetDeviceWindow(dev);
    if (!hwnd || !IsWindow(hwnd)) return;

    if (g_ov.wnd != hwnd) {
        if (g_ov.wnd && g_origWndProc)
            SetWindowLongPtrW(g_ov.wnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
        g_ov.wnd = hwnd;
        g_origWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(OverlayWndProc)));
        g_telemWndHooked = g_origWndProc ? 1 : 0;
    }
    g_ov.device = dev;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(dev);

    // ★ 字形范围自己列全：ImGui 自带的 ChineseSimplifiedCommon **不含箭头等符号**，
    //   界面上会显示成 '?'（实测「复制单位 → 光标处」的箭头、"•" 之类的符号全丢了）。
    static const ImWchar kGlyphRanges[] = {
        0x0020, 0x00FF,   // 基本拉丁 + 拉丁补充（含 × · °）
        0x2000, 0x206F,   // 常用标点（— … ‘ ’）
        0x2190, 0x21FF,   // 箭头 ← ↑ → ↓
        0x2460, 0x24FF,   // 带圈数字 ①②
        0x25A0, 0x25FF,   // 几何图形 ● ■ ▲
        0x2600, 0x26FF,   // 杂项符号 ★ ☆
        0x3000, 0x30FF,   // CJK 标点（「」【】）+ 假名
        0x31F0, 0x31FF,
        0xFF00, 0xFFEF,   // 全角形式
        0x4E00, 0x9FAF,   // CJK 基本区（常用 + 次常用汉字）
        0,
    };
    ImFontConfig fc;
    fc.OversampleH = 1; fc.OversampleV = 1; fc.PixelSnapH = true;
    ImFont* f = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\msyh.ttc", 16.0f, &fc, kGlyphRanges);
    if (!f)
        f = io.Fonts->AddFontFromFileTTF(
            "C:\\Windows\\Fonts\\simhei.ttf", 16.0f, &fc, kGlyphRanges);
    if (!f) io.Fonts->AddFontDefault();

    // ---- 配色：深色 + 青色 ----
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 4.f; st.FrameRounding = 3.f; st.GrabRounding = 3.f;
    st.WindowBorderSize = 1.f; st.FrameBorderSize = 0.f;
    st.ItemSpacing = ImVec2(8, 6); st.FramePadding = ImVec2(8, 4);
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg]      = ImVec4(0.07f, 0.08f, 0.10f, 0.95f);
    c[ImGuiCol_TitleBg]       = ImVec4(0.10f, 0.13f, 0.18f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.18f, 0.26f, 1.00f);
    c[ImGuiCol_Tab]           = ImVec4(0.11f, 0.14f, 0.19f, 1.00f);
    c[ImGuiCol_TabHovered]    = ImVec4(0.20f, 0.42f, 0.55f, 1.00f);
    c[ImGuiCol_TabSelected]   = ImVec4(0.16f, 0.34f, 0.46f, 1.00f);
    c[ImGuiCol_Header]        = ImVec4(0.16f, 0.30f, 0.40f, 1.00f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.44f, 0.56f, 1.00f);
    c[ImGuiCol_Button]        = ImVec4(0.16f, 0.32f, 0.44f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.46f, 0.60f, 1.00f);
    c[ImGuiCol_ButtonActive]  = ImVec4(0.12f, 0.26f, 0.36f, 1.00f);
    c[ImGuiCol_CheckMark]     = ImVec4(0.35f, 0.85f, 0.95f, 1.00f);
    c[ImGuiCol_SliderGrab]    = ImVec4(0.35f, 0.75f, 0.90f, 1.00f);
    c[ImGuiCol_FrameBg]       = ImVec4(0.12f, 0.14f, 0.18f, 1.00f);

    g_ov.imguiReady = true;
    g_telemImguiReady = 1;
    AddLog(LOG_OK, "[渲染] ImGui 就绪  hwnd=%p device=%p", (void*)hwnd, (void*)dev);
}

void BuildUI();
// ★ 每帧逻辑（与菜单是否可见无关）：调试接口、帧计数、快捷键队列、自动空投
void OverlayTick();

void RenderOverlay(IDirect3DDevice9* dev) {
    ImGuiIO& io = ImGui::GetIO();
    POINT pt = {};
    if (GetCursorPos(&pt) && g_ov.wnd && IsWindow(g_ov.wnd)) ScreenToClient(g_ov.wnd, &pt);
    io.AddMousePosEvent(static_cast<float>(pt.x), static_cast<float>(pt.y));
    io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    BuildUI();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    g_telemRenderOnce++;
}

// ===================== 钩子 =====================
HRESULT STDMETHODCALLTYPE HookEndScene(IDirect3DDevice9* dev);
HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* dev, const RECT* a, const RECT* b,
                                      HWND h, const RGNDATA* c);
HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp);
HRESULT STDMETHODCALLTYPE HookSwapPresent(IDirect3DSwapChain9* sc, const RECT* a, const RECT* b,
                                          HWND h, const RGNDATA* c, DWORD fl);
// 4x4 转置（列主序 → 转成行向量约定，供反投影统一使用）
static void Transpose4(const D3DMATRIX* m, float out[16]) {
    const float* a = reinterpret_cast<const float*>(m);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out[i * 4 + j] = a[j * 4 + i];
}

// SetTransform（vtable[44]）：抓 VIEW 矩阵 → 用来算"视角中心"落点
HRESULT STDMETHODCALLTYPE HookSetTransform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE st,
                                           const D3DMATRIX* m) {
    if (m) {
        __try {
            if (st == D3DTS_VIEW) {
                // ★ 只接受"像 3D 场景相机"的视图矩阵（前向朝下，fz 明显为负），
                //   否则会抓到 UI/2D 那一遍的矩阵 → 视角中心算出垃圾
                const float fz = m->_33;      // 行向量约定：第 3 行 = 相机前向，_33 = 前向的 z 分量
                // 诊断：无条件记录（不过滤）—— 用来核对过滤阈值/行列约定是否搞反
                memcpy(g_viewLast, m, sizeof(float) * 16);
                g_viewLastFz = fz;
                InterlockedIncrement(&g_viewCount);
                if (fz < g_viewMinFz) { g_viewMinFz = fz; memcpy(g_viewMinMat, m, sizeof(float) * 16); }
                // ★ 判据改为「平移分量非零」：真·3D 相机视图矩阵含相机平移
                //   （行主序在 _41/_42/_43，列主序在 _14/_24/_34）；UI/2D 那遍是单位矩阵（平移=0）。
                //   旧判据 "_33 < -0.05" 实测把游戏 3D 相机的 VIEW 全部拒掉了（坐标约定不符）。
                const float tRow = (m->_41 < 0 ? -m->_41 : m->_41) + (m->_42 < 0 ? -m->_42 : m->_42) + (m->_43 < 0 ? -m->_43 : m->_43);
                const float tCol = (m->_14 < 0 ? -m->_14 : m->_14) + (m->_24 < 0 ? -m->_24 : m->_24) + (m->_34 < 0 ? -m->_34 : m->_34);
                const float tnorm = tRow > tCol ? tRow : tCol;
                if (tnorm > 1.0f) {
                    const bool rowMajor = (tRow >= tCol);
                    if (rowMajor) memcpy(g_viewMat, m, sizeof(float) * 16);
                    else Transpose4(m, g_viewMat);   // 列主序 → 归一为行向量约定
                    InterlockedExchange(&g_viewRowMajor, rowMajor ? 1 : 0);
                    InterlockedExchange(&g_hasView, 1);
                    InterlockedExchange(&g_viewFromHook, 1);   // 真·SetTransform 捕获
                } else {
                    InterlockedIncrement(&g_rejView);
                }
            } else if (st == D3DTS_PROJECTION) {
                if (g_viewRowMajor) memcpy(g_projMat, m, sizeof(float) * 16);
                else Transpose4(m, g_projMat);
                InterlockedExchange(&g_hasProj, 1);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (g_origSetTransform) return g_origSetTransform(dev, st, m);
    return D3D_OK;
}

bool HookFunction(void* target, void* detour, void** orig, const char* name) {
    if (!target || !orig) return false;
    for (int i = 0; i < g_hookedCount; ++i)
        if (g_hookedTargets[i] == target) return true;
    g_telemScanRounds++;
    MH_STATUS st = MH_CreateHook(target, detour, orig);
    if (st != MH_OK) { AddLog(LOG_ERR, "[钩子] %s 失败 MH=%d", name, st); return false; }
    if (g_hookedCount < 16) g_hookedTargets[g_hookedCount++] = target;
    g_telemVtablesHooked++;
    AddLog(LOG_OK, "[钩子] 已挂 %s @ %p", name, target);
    return true;
}

void HookDeviceVtable(IDirect3DDevice9* dev) {
    if (!dev) return;
    __try {
        void** vt = *reinterpret_cast<void***>(dev);
        if (!vt) return;
        HookFunction(vt[16], reinterpret_cast<void*>(&HookReset),
                     reinterpret_cast<void**>(&g_origReset), "Reset");
        HookFunction(vt[17], reinterpret_cast<void*>(&HookPresent),
                     reinterpret_cast<void**>(&g_origPresent), "Present");
        HookFunction(vt[42], reinterpret_cast<void*>(&HookEndScene),
                     reinterpret_cast<void**>(&g_origEndScene), "EndScene");
        IDirect3DSwapChain9* sc = nullptr;
        if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
            void** sv = *reinterpret_cast<void***>(sc);
            if (sv)
                HookFunction(sv[3], reinterpret_cast<void*>(&HookSwapPresent),
                             reinterpret_cast<void**>(&g_origSwapPresent), "SwapChain::Present");
            sc->Release();
        }
        // ★ IDirect3DDevice9 标准 vtable（0-based，按 d3d9.h 方法顺序）：
        //   16=Reset 17=Present 41=BeginScene 42=EndScene 43=Clear 44=SetTransform 45=GetTransform
        //   （曾误用 vt[24] —— 那是 CreateVolumeTexture，游戏永不调用 → VIEW 矩阵永远抓不到）
        HookFunction(vt[44], reinterpret_cast<void*>(&HookSetTransform),
                     reinterpret_cast<void**>(&g_origSetTransform), "SetTransform(VIEW)");
        // vtable 诊断：记下关键槽地址，便于一眼确认挂对了函数（应全部落在 d3d9.dll 内）
        g_vtInfo[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(vt[16]));
        g_vtInfo[1] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(vt[17]));
        g_vtInfo[2] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(vt[42]));
        g_vtInfo[3] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(vt[44]));
        g_vtInfo[4] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(vt[45]));
        g_vtInfo[5] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(dev));
        g_vtInfo[6] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(GetModuleHandleA("d3d9.dll")));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ===================== 学习钩子：定位游戏的"创建对象"函数 =====================
// 已确认 Find(RVA 0x3E4230, this=ThingFactory, hash->template) 在本构建有效；
// 游戏自己造单位时必然先 Find 再"创建"。挂住 Find：
//   ① 记下调用者返回地址；② 顺着调用者机器码往后解码，找紧跟的 call（= 创建函数候选）。
typedef void* (__fastcall* Find_t)(void* self, void* edx, uint32_t hash);
Find_t   g_origFind = nullptr;
uint32_t g_findCallers[48] = { 0 };
uint32_t g_createCand[48]  = { 0 };
volatile LONG g_findCallerCount = 0;

// 从 p 起最多解码 20 条指令，收集 call rel32 的目标
void ScanCallsAfter(uintptr_t p, uint32_t* out, int& n, int maxOut) {
    __try {
        uint8_t* q = reinterpret_cast<uint8_t*>(p);
        for (int k = 0; k < 20 && n < maxOut; ++k) {
            hde32s hs;
            unsigned len = hde32_disasm(q, &hs);
            if (!len) break;
            if (hs.opcode == 0xE8) {
                int32_t rel;
                memcpy(&rel, q + 1, 4);
                uint32_t tgt = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(q) + 5 + rel);
                if (tgt >= 0x400000 && tgt < 0xC00000) out[n++] = tgt;
            }
            q += len;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void* __fastcall HookFind(void* self, void* edx, uint32_t hash) {
    uint32_t r = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    const LONG n = g_findCallerCount;
    bool known = false;
    for (LONG i = 0; i < n && i < 48; ++i) if (g_findCallers[i] == r) { known = true; break; }
    if (!known && n < 48) {
        InterlockedIncrement(&g_findCallerCount);
        g_findCallers[n] = r;
        AddLog(LOG_WARN, "[学习] Find 调用者 #%d = %08X (hash %08X)", (int)n + 1, r, hash);
        int cn = 0;
        ScanCallsAfter(r, g_createCand, cn, 48);
        for (int i = 0; i < cn; ++i)
            AddLog(LOG_WARN, "[学习]   └ 其后 call -> %08X", g_createCand[i]);
    }
    if (g_origFind) return g_origFind(self, edx, hash);
    return nullptr;
}

void InstallLearnHooks() {
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (!base) return;
    void* tgt = reinterpret_cast<void*>(base + 0x3E4230);
    if (HookFunction(tgt, reinterpret_cast<void*>(&HookFind),
                     reinterpret_cast<void**>(&g_origFind), "ThingFactory::Find(学习)"))
        AddLog(LOG_INFO, "[学习] 已挂 Find；造一个单位就能记下创建路径");
}

void InstallRenderHooks() {
    if (g_mhInited) return;
    MH_STATUS mi = MH_Initialize();
    if (mi != MH_OK && mi != MH_ERROR_ALREADY_INITIALIZED) {
        AddLog(LOG_ERR, "[钩子] MH_Initialize 失败 %d", mi);
        return;
    }
    g_mhInited = true;

    HMODULE h9 = GetModuleHandleA("d3d9.dll");
    if (!h9) { AddLog(LOG_ERR, "d3d9.dll 未加载"); return; }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ra3copilot_overlay_dummy";
    RegisterClassExW(&wc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                               0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    AddLog(LOG_INFO, "d3d9.dll @ %p", (void*)h9);

    const DWORD flags[3] = {
        D3DCREATE_HARDWARE_VERTEXPROCESSING,
        D3DCREATE_MIXED_VERTEXPROCESSING,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING,
    };
    typedef IDirect3D9* (WINAPI* C9_t)(UINT);
    C9_t c9 = reinterpret_cast<C9_t>(GetProcAddress(h9, "Direct3DCreate9"));
    if (c9 && wnd) {
        IDirect3D9* d3d = c9(D3D_SDK_VERSION);
        if (d3d) {
            for (int i = 0; i < 3; ++i) {
                D3DPRESENT_PARAMETERS pp = {};
                pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = wnd;
                IDirect3DDevice9* dev = nullptr;
                if (SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                                                flags[i], &pp, &dev)) && dev) {
                    HookDeviceVtable(dev); dev->Release();
                }
            }
            d3d->Release();
        }
    }
    typedef HRESULT (WINAPI* CE_t)(UINT, IDirect3D9Ex**);
    CE_t ce = reinterpret_cast<CE_t>(GetProcAddress(h9, "Direct3DCreate9Ex"));
    if (ce && wnd) {
        IDirect3D9Ex* d3dx = nullptr;
        if (SUCCEEDED(ce(D3D_SDK_VERSION, &d3dx)) && d3dx) {
            D3DPRESENT_PARAMETERS pp = {};
            pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = wnd;
            IDirect3DDevice9Ex* devx = nullptr;
            if (SUCCEEDED(d3dx->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                    D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, nullptr, &devx)) && devx) {
                HookDeviceVtable(devx); devx->Release();
            }
            d3dx->Release();
        }
    }
    if (wnd) DestroyWindow(wnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    InstallLearnHooks();             // 学习钩子（挂 Find，用于定位创建函数）
    MH_EnableHook(MH_ALL_HOOKS);
    AddLog(LOG_OK, "[钩子] 已就绪：%d 个 d3d9 函数，等待首帧", g_hookedCount);
}

HRESULT STDMETHODCALLTYPE HookEndScene(IDirect3DDevice9* dev) {
    InterlockedIncrement(reinterpret_cast<LONG*>(&g_telemEndScene));
    if (g_origEndScene) return g_origEndScene(dev);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* dev, const RECT* a, const RECT* b,
                                      HWND h, const RGNDATA* c) {
    InterlockedIncrement(reinterpret_cast<LONG*>(&g_telemPresent));
    if (dev) {
        __try {
            if (!g_hasView || !g_hasProj) {   // 兜底：游戏可能用常量表而非 SetTransform
                D3DMATRIX m;
                if (!g_hasView && SUCCEEDED(dev->GetTransform(D3DTS_VIEW, &m))) {
                    // ★ 只采纳"像 3D 相机"的矩阵（行3=前向，_33=前向 z 应明显为负）。
                    //   shader 游戏不走固定管线时 GetTransform 会返回【单位矩阵】(._33=1) →
                    //   必须丢弃，否则污染 g_viewMat，反投影会算出近原点垃圾（"生成在左下角"的直接成因）。
                    if (m._33 < -0.05f) {
                        memcpy(g_viewMat, &m, sizeof(float) * 16);
                        InterlockedExchange(&g_hasView, 1);
                        InterlockedExchange(&g_viewFromHook, 0);
                    }
                }
                if (!g_hasProj && SUCCEEDED(dev->GetTransform(D3DTS_PROJECTION, &m))) {
                    memcpy(g_projMat, &m, sizeof(float) * 16); InterlockedExchange(&g_hasProj, 1);
                }
            }
            if (!g_viewport[2]) {
                D3DVIEWPORT9 vpw;
                if (SUCCEEDED(dev->GetViewport(&vpw)) && vpw.Width && vpw.Height) {
                    g_viewport[0] = (int)vpw.X; g_viewport[1] = (int)vpw.Y;
                    g_viewport[2] = (int)vpw.Width; g_viewport[3] = (int)vpw.Height;
                } else if (g_ov.wnd) {
                    // 兜底：GetViewport 在菜单/加载阶段可能返回 0 → 用窗口客户区尺寸
                    RECT rc = { 0, 0, 0, 0 };
                    GetClientRect(g_ov.wnd, &rc);
                    if (rc.right > 0 && rc.bottom > 0) {
                        g_viewport[0] = 0; g_viewport[1] = 0;
                        g_viewport[2] = rc.right; g_viewport[3] = rc.bottom;
                    }
                }
            }
            if (!g_ov.imguiReady) EnsureImGui(dev);
            if (g_ov.imguiReady && dev == g_ov.device) {
                OverlayTick();                     // ★ 无论菜单是否可见都要跑（否则 HTTP 接口会超时）
                if (g_cfg.visible) RenderOverlay(dev);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (g_origPresent) return g_origPresent(dev, a, b, h, c);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (g_ov.imguiReady) {
        __try { ImGui_ImplDX9_InvalidateDeviceObjects(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    HRESULT hr = g_origReset ? g_origReset(dev, pp) : D3D_OK;
    if (g_ov.imguiReady && SUCCEEDED(hr)) {
        __try { ImGui_ImplDX9_CreateDeviceObjects(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookSwapPresent(IDirect3DSwapChain9* sc, const RECT* a, const RECT* b,
                                          HWND h, const RGNDATA* c, DWORD fl) {
    if (sc) {
        __try {
            IDirect3DDevice9* dev = nullptr;
            if (SUCCEEDED(sc->GetDevice(&dev)) && dev) {
                if (!g_ov.imguiReady) EnsureImGui(dev);
                if (g_ov.imguiReady && dev == g_ov.device) {
                    OverlayTick();                 // ★ 兜底：若游戏只走 SwapChain::Present
                    if (g_cfg.visible) RenderOverlay(dev);
                }
                dev->Release();
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (g_origSwapPresent) return g_origSwapPresent(sc, a, b, h, c, fl);
    return D3D_OK;
}

// ===================== UI =====================
// ===================== 快捷键表（基于 CE 的布局） =====================
// 布局：Ctrl+F1..F12 + 单键 P/O/L/K/I/J、'-'/'+'、PgUp/PgDn、'['/']'/'\'、';'、Del、','/'.'/'/'、'''
enum {
    HK_MONEY = 1, HK_POWER, HK_SCPOINT, HK_HAVEALLSC, HK_FASTBUILD, HK_SUPERPOWER,
    HK_DISABLESP, HK_MAP, HK_ENEMYBUILD, HK_GODMODE, HK_KILLMODE,
    HK_LEVELUP, HK_SPEEDMAX, HK_SPEEDSLOW, HK_FREEZE, HK_SPEEDNORM,
    HK_HPMAX, HK_HPMIN, HK_HPNORM, HK_AMMO,
    HK_DESTROY, HK_DANGERMAX, HK_DANGERMIN, HK_DANGERNORM, HK_OREMINE,
    HK_FREEBUILD, HK_MCV, HK_CAPTURE, HK_ZOOM, HK_CHANGEID, HK_COPY, HK_WNB,
};
struct HKDef { int vk; bool ctrl; int id; const char* label; const char* desc; };
const HKDef kHK[] = {
    { VK_F1,  true,  HK_MONEY,       "Ctrl+F1",  "加钱（每次金额）" },
    { VK_F2,  true,  HK_POWER,       "Ctrl+F2",  "电力无限（开关）" },
    { VK_F3,  true,  HK_SCPOINT,     "Ctrl+F3",  "科技点无限（开关）" },
    { VK_F4,  true,  HK_HAVEALLSC,   "Ctrl+F4",  "全科技（开关）" },
    { VK_F5,  true,  HK_FASTBUILD,   "Ctrl+F5",  "快速建造（开关）" },
    { VK_F6,  true,  HK_SUPERPOWER,  "Ctrl+F6",  "超武就绪（开关）" },
    { VK_F7,  true,  HK_DISABLESP,   "Ctrl+F7",  "禁用敌方超武（开关）" },
    { VK_F9,  true,  HK_MAP,         "Ctrl+F9",  "全图 / 去迷雾（开关）" },
    { VK_F10, true,  HK_ENEMYBUILD,  "Ctrl+F10", "敌人无法建造（开关）" },
    { VK_F11, true,  HK_GODMODE,     "Ctrl+F11", "玩家无敌（开关）" },
    { 'P',    false, HK_LEVELUP,     "P",        "升级单位（满级）" },
    { 'O',    false, HK_CHANGEID,    "O",        "接管单位（把选中单位改成你的）" },
    { 'I',    false, HK_COPY,        "I",        "复制选中单位 → 光标处" },
    { VK_OEM_MINUS, false, HK_SPEEDMAX,  "-",       "加速（选中单位）" },
    { VK_OEM_PLUS,  false, HK_SPEEDSLOW, "+",       "减速（选中单位）" },
    { VK_PRIOR, false, HK_FREEZE,     "PageUp",   "冻结单位" },
    { VK_NEXT,  false, HK_SPEEDNORM,  "PageDown", "恢复速度" },
    { VK_OEM_4, false, HK_HPMAX,      "[",        "无限血量" },
    { VK_OEM_6, false, HK_HPMIN,      "]",        "残血（血量设为 1）" },
    { VK_OEM_5, false, HK_HPNORM,     "\\",       "恢复血量" },
    { VK_OEM_1, false, HK_AMMO,       ";",        "选中单位弹药满（开关）" },
    { VK_DELETE,false, HK_DESTROY,    "Delete",   "摧毁选中" },
    { VK_OEM_COMMA,  false, HK_DANGERMAX,  ",",   "危险等级 = 最高" },
    { VK_OEM_PERIOD, false, HK_DANGERMIN,  ".",   "危险等级 = 最低" },
    { VK_OEM_2, false, HK_DANGERNORM, "/",        "危险等级 = 不干预" },
    { VK_OEM_7, false, HK_OREMINE,    "'",        "恢复矿场" },
    { 'L',    false, HK_FREEBUILD,    "L",        "免费建造（开关）" },
    { 'K',    false, HK_MCV,          "K",        "召唤基地车" },
};

int  g_facIdx = 0;                 // 支援面板左侧阵营选中项（0=全部 … 末项=箱子）
bool g_primaryOnly = true;         // 支援面板：只看基准形态
char g_unitSearch[64] = "";

// 资源科技：金额锁定（点一下记下当前资金，之后每帧回写）
bool g_moneyLock = false;
int  g_moneyLockVal = 0;

// 单位操作：锁定血量（每帧把选中单位血量回写到上限）
bool g_hpLock = false;

// ★ 快捷键动作队列：输入线程只"投递"，真正执行放到渲染线程（= 游戏主线程）
//   原因：召唤/满级会调用游戏内部函数，从我们的输入线程调用 = 跨线程竞态 → 闪退（实测）
const int kHKQ = 32;
int g_hkQ[kHKQ] = { 0 };
volatile LONG g_hkHead = 0, g_hkTail = 0;

void PostHotkey(int id) {
    const LONG t = g_hkTail;
    if ((t + 1) % kHKQ == g_hkHead) return;          // 满则丢弃
    g_hkQ[t] = id;
    InterlockedExchange(&g_hkTail, (t + 1) % kHKQ);
}

int PopHotkey() {
    const LONG h = g_hkHead;
    if (h == g_hkTail) return 0;
    const int id = g_hkQ[h];
    InterlockedExchange(&g_hkHead, (h + 1) % kHKQ);
    return id;
}

void RunHotkey(int id);   // 定义在下方
std::vector<uint32_t> g_candHash;
std::vector<std::string> g_candName;
std::vector<int> g_candCount;
int  g_summonCount = 1;
int  g_owner = 0;
int  g_crateIdx = 0;
int  g_crateCount = 2;
bool g_crateAuto = false;
float g_cratePeriod = 20.f;
float g_crateTimer = 0.f;

void LogAct(const char* what, int n) {
    if (n > 0) { AddLog(LOG_OK, "[单位] %s：影响 %d 个单位", what, n); Ding(); return; }
    const char* e = SummonLastError();
    AddLog(LOG_WARN, "[单位] %s：%s", what, (e && e[0]) ? e : "没有可操作的单位（先在游戏里选中）");
}

void DrawLogPanel() {
    // ---- 选中单位调试详情（可选；默认关）----
    ImGui::Checkbox("显示选中单位调试详情", &g_cfg.showSelDiag);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("把当前选中对象的内部结构摊开：对象 / vtable / 模板指针 /\n"
                          "模板哈希（会反查成名字）/ body / 血量 / 归属 / 坐标 / 状态位。\n"
                          "排查「召唤出来的单位不能被操作」这类问题最有用 ——\n"
                          "看 body 那行是不是「不是合法指针 → 未入世」。");
    if (g_cfg.showSelDiag) {
        static char diagBuf[3072];
        static float lastUpd = -1.f;
        if (g_logClock - lastUpd >= 0.2f) {        // 每 0.2 秒刷新一次，不必每帧读内存
            lastUpd = g_logClock;
            if (SelectedDiagText(diagBuf, sizeof(diagBuf)) <= 0)
                strcpy_s(diagBuf, "(读取失败)");
        }
        ImGui::BeginChild("##seldiag", ImVec2(0, 170), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(diagBuf);
        ImGui::EndChild();
    }
    ImGui::Separator();
    ImGui::BeginChild("##log", ImVec2(0, 148), true, ImGuiWindowFlags_HorizontalScrollbar);
    const int n = g_logCount > kLogMax ? kLogMax : g_logCount;
    const int start = (g_logCount > kLogMax) ? (g_logCount - kLogMax) : 0;
    for (int i = 0; i < n; ++i) {
        const LogLine& L = g_log[(start + i) % kLogMax];
        ImVec4 col = ImVec4(0.75f, 0.80f, 0.85f, 1.f);
        if (L.kind == LOG_OK)   col = ImVec4(0.45f, 0.90f, 0.65f, 1.f);
        if (L.kind == LOG_WARN) col = ImVec4(0.95f, 0.85f, 0.40f, 1.f);
        if (L.kind == LOG_ERR)  col = ImVec4(0.95f, 0.45f, 0.45f, 1.f);
        ImGui::TextColored(col, "%6.1fs  %s", L.t, L.text);
    }
    if (g_logDirty) { ImGui::SetScrollHereY(1.f); g_logDirty = false; }
    ImGui::EndChild();
}

void TabResource() {
    if (LocalPlayerReady()) ImGui::Text("本地玩家 ID: %d", LocalPlayerId());
    else ImGui::TextDisabled("本地玩家：未抓到（观战 / 未进局）");
    if (LocalPlayerObj())
        ImGui::Text("玩家对象: 0x%08X   资金: %d", (unsigned)LocalPlayerObj(), LocalMoney());
    ImGui::Separator();

    ImGui::SetNextItemWidth(140);
    ImGui::InputInt("每次金额", &g_cfg.addAmount, 100, 1000);
    ImGui::SameLine();
    if (ImGui::Button("加钱 (F7)", ImVec2(120, 0))) MoneyAdd(g_cfg.addAmount);

    // 锁定金额：点一下把「当前资金」钉住（之后每帧回写），再点解除
    // ★ 状态必须取"点击前"的副本：按钮回调会翻转 g_moneyLock，若 Push/Pop 各判一次
    //   就会"没 Push 却 Pop" → ImGui 断言框（PopStyleColor too many times）。2026-10-15 实机踩到。
    const bool moneyLocked = g_moneyLock;
    if (moneyLocked) PushBtnOn();
    if (ImGui::Button(g_moneyLock ? "解除金额锁定" : "锁定当前金额", ImVec2(150, 0))) {
        if (!g_moneyLock) {
            if (!LocalPlayerReady())
                AddLog(LOG_WARN, "[金额] 还没抓到玩家（先进局），无法锁定");
            else {
                g_moneyLockVal = LocalMoney();
                g_moneyLock = true;
                AddLog(LOG_OK, "[金额] 已锁定 %d（每帧回写）", g_moneyLockVal);
            }
        } else {
            g_moneyLock = false;
            AddLog(LOG_INFO, "[金额] 解除锁定");
        }
    }
    if (moneyLocked) {
        PopBtnOn();
        ImGui::SameLine();
        ImGui::TextDisabled("锁定值 %d", g_moneyLockVal);
    }
    ImGui::Separator();
    bool b;
    b = GetFeature(FID_POWER);
    if (ImGui::Checkbox("电力无限", &b)) SetFeature(FID_POWER, b);
    ImGui::SameLine(220);
    b = GetFeature(FID_SCPOINT);
    if (ImGui::Checkbox("科技点无限", &b)) SetFeature(FID_SCPOINT, b);
    b = GetFeature(FID_HAVEALLSC);
    if (ImGui::Checkbox("全科技", &b)) SetFeature(FID_HAVEALLSC, b);
    ImGui::SameLine(220);
    b = GetFeature(FID_FASTBUILD);
    if (ImGui::Checkbox("快速建造", &b)) SetFeature(FID_FASTBUILD, b);
    ImGui::SameLine(360);
    {
        bool fb = GetFreeBuild();
        if (ImGui::Checkbox("免费建造", &fb)) {
            bool ok = SetFreeBuild(fb);
            AddLog(ok ? LOG_OK : LOG_ERR, "[建造] 免费建造 %s",
                   ok ? (fb ? "已开启" : "已关闭") : "失败（字节不符，未改动）");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("原理：把建造检测的 jne 改成 jmp（恒可真建造）\n"
                              "比「快速建造」安全，不会让建造卡死");
    }
    if (ImGui::Button("恢复矿场", ImVec2(120, 0))) TriggerOneShot(FID_OREMINE);
}

void TabBattle() {
    bool b;
    b = GetFeature(FID_GODMODE);
    if (ImGui::Checkbox("玩家无敌", &b)) SetFeature(FID_GODMODE, b);
    ImGui::SameLine(220);
    b = GetFeature(FID_MAP);
    if (ImGui::Checkbox("全图（去迷雾）", &b)) SetFeature(FID_MAP, b);
    b = GetFeature(FID_UNITAMMO);
    if (ImGui::Checkbox("选中单位弹药满", &b)) SetFeature(FID_UNITAMMO, b);
    ImGui::SameLine(220);
    b = GetFeature(FID_SUPERPOWER);
    if (ImGui::Checkbox("超武就绪", &b)) SetFeature(FID_SUPERPOWER, b);
    b = GetFeature(FID_DISABLESP);
    if (ImGui::Checkbox("禁用敌方超武", &b)) SetFeature(FID_DISABLESP, b);
    ImGui::SameLine(220);
    b = GetFeature(FID_ENEMYBUILD);
    if (ImGui::Checkbox("敌方无法建造", &b)) SetFeature(FID_ENEMYBUILD, b);

    ImGui::Separator();
    ImGui::Text("危险等级:");
    ImGui::SameLine();
    const int dg = g_flags[FID_DANGER];
    if (ImGui::RadioButton("不干预", dg == 0)) g_flags[FID_DANGER] = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("最高", dg == 1)) g_flags[FID_DANGER] = 1;
    ImGui::SameLine();
    if (ImGui::RadioButton("最低", dg == 2)) g_flags[FID_DANGER] = 2;

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.95f, 1.f), "组合 / 其它");
    {
        bool km = GetFeature(FID_KILLMODE);
        if (ImGui::Checkbox("秒杀模式（敌方单位即死）", &km)) {
            SetFeature(FID_KILLMODE, km);
            AddLog(km ? LOG_OK : LOG_INFO, "[秒杀] %s", km ? "开" : "关");
        }
        bool zi = GetFeature(FID_ZOOM_IN), zo = GetFeature(FID_ZOOM_OUT);
        if (ImGui::Checkbox("视野放大", &zi)) { SetFeature(FID_ZOOM_IN, zi); if (zi) SetFeature(FID_ZOOM_OUT, false); }
        ImGui::SameLine();
        if (ImGui::Checkbox("视野缩小", &zo)) { SetFeature(FID_ZOOM_OUT, zo); if (zo) SetFeature(FID_ZOOM_IN, false); }
        if (ImGui::Button("复制选中单位 → 光标处", ImVec2(180, 0))) {
            int r = ActCopySelected();
            AddLog(r > 0 ? LOG_OK : LOG_WARN, "[复制] 选中单位副本 %s", r > 0 ? "已生成" : "失败");
        }
        ImGui::SameLine();
        if (ImGui::Button("预设增援 ×8", ImVec2(140, 0))) {
            int r = ActSummonSquad();
            AddLog(r > 0 ? LOG_OK : LOG_WARN, "[预设增援] %s", r > 0 ? "已生成 ×8" : "失败");
        }
        ImGui::TextDisabled("「复制」= 把你选中的单位类型新造一个到光标处；"
                            "「收编」= 把选中（敌方）单位改成你的");
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.95f, 1.f), "选中单位操作");
    ImGui::TextDisabled("需要先在游戏里选中单位（受影响的单位数会记到输出记录）");
    if (ImGui::Button("超速", ImVec2(70, 0))) LogAct("超速", ActSpeed(0));
    ImGui::SameLine();
    if (ImGui::Button("慢速", ImVec2(70, 0))) LogAct("慢速", ActSpeed(1));
    ImGui::SameLine();
    if (ImGui::Button("冻结", ImVec2(70, 0))) LogAct("冻结", ActSpeed(2));
    ImGui::SameLine();
    if (ImGui::Button("恢复速度", ImVec2(86, 0))) LogAct("恢复速度", ActSpeed(3));
    ImGui::SameLine();
    if (ImGui::Button("满级升级", ImVec2(86, 0))) LogAct("满级升级", ActRank());

    if (ImGui::Button("无敌", ImVec2(70, 0))) LogAct("无敌", ActHealth(0));
    ImGui::SameLine();
    if (ImGui::Button("残血", ImVec2(70, 0))) LogAct("残血", ActHealth(1));
    ImGui::SameLine();
    if (ImGui::Button("恢复血量", ImVec2(86, 0))) LogAct("恢复血量", ActHealth(2));
    ImGui::SameLine();
    if (ImGui::Button("收编敌方(改归属)", ImVec2(120, 0))) LogAct("收编敌方", ActSetOwner());

    if (ImGui::Button("摧毁选中", ImVec2(86, 0))) LogAct("摧毁选中", ActKillSelected());
    ImGui::SameLine();
    if (ImGui::Button("召唤基地车", ImVec2(100, 0))) {
        bool ok = SummonMCV();
        AddLog(ok ? LOG_OK : LOG_WARN, "[召唤] 基地车 %s", ok ? "已生成" : "失败（看是否已进局/已选中）");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("选中 %d 个", SelectedCount());

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.95f, 1.f), "空投箱（拾取类奖励）");
    ImGui::TextDisabled("在落点刷出箱子，单位走过去随机出货");
    if (g_crateIdx >= kCrateCount) g_crateIdx = 0;
    static char crateItems[4096];
    if (!crateItems[0]) {
        size_t w = 0;
        for (int i = 0; i < kCrateCount && w < sizeof(crateItems) - 64; ++i) {
            int n = snprintf(crateItems + w, sizeof(crateItems) - w, "%s (%s)",
                             kCrates[i].name, kCrates[i].id);
            if (n <= 0) break;
            w += (size_t)n + 1;
            crateItems[w - 1] = '\0';
        }
        crateItems[sizeof(crateItems) - 1] = '\0';
    }
    ImGui::SetNextItemWidth(300);
    ImGui::Combo("箱子类型", &g_crateIdx, crateItems);
    ImGui::SetNextItemWidth(90);
    ImGui::DragInt("数量", &g_crateCount, 1, 1, 20);
    ImGui::SameLine();
    if (ImGui::Button("立即空投", ImVec2(100, 0))) DropCrate(g_owner, g_crateCount, true);
    ImGui::SameLine();
    ImGui::Checkbox("自动空投", &g_crateAuto);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::DragFloat("间隔(秒)", &g_cratePeriod, 1.f, 3.f, 300.f, "%.0f");
}

// ===== 面板：单位操作（2026-10-15 重排）=====
// 按钮顺序：升级单位 / 锁定血量 / 无限血量 / 加速 ‖ 恢复速度 / 减速 / 冻结单位 / 接管单位 / 摧毁单位
// 秒杀模式、We Need Back 已按需求移除；无限弹药与复制移到按钮区（秒杀原位置）。
void PanelUnitOps() {
    ImGui::Text("选中 %d 个单位", SelectedCount());
    ImGui::SameLine();
    ImGui::TextDisabled("（先在游戏里选中单位，再点下面的动作）");
    ImGui::Separator();

    // —— 特效 / 生成类 ——
    bool am = GetFeature(FID_UNITAMMO);
    if (am) PushBtnOn();
    if (ImGui::Button("无限弹药", ImVec2(100, 0))) {
        SetFeature(FID_UNITAMMO, !am);
        AddLog(!am ? LOG_OK : LOG_INFO, "[弹药] 无限弹药 %s", !am ? "开" : "关");
    }
    if (am) PopBtnOn();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("开关：选中单位的弹药保持满弹（亮色=已开启）");
    ImGui::SameLine();
    if (ImGui::Button("复制单位 → 光标处", ImVec2(-1, 0))) {
        int r = ActCopySelected();
        AddLog(r > 0 ? LOG_OK : LOG_WARN, "[复制] 选中单位副本 %s", r > 0 ? "已生成" : "失败");
    }

    ImGui::Separator();

    // —— 第 1 排：升级 / 血量 / 加速 ——
    if (ImGui::Button("升级单位", ImVec2(84, 0))) LogAct("升级单位", ActRank());
    ImGui::SameLine();
    const bool hpLocked = g_hpLock;                  // ★ 同上：取点击前的副本
    if (hpLocked) PushBtnOn();
    if (ImGui::Button("锁定血量", ImVec2(84, 0))) {
        g_hpLock = !g_hpLock;
        AddLog(g_hpLock ? LOG_OK : LOG_INFO, "[血量] 锁定血量 %s",
               g_hpLock ? "开（每帧回写满血）" : "关");
    }
    if (hpLocked) PopBtnOn();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("开关：每帧把选中单位的血量回写到其上限 —— 血条不会掉\n"
                          "（与「无限血量」的区别：这个是持续保持满血，不改变上限值）");
    ImGui::SameLine();
    if (ImGui::Button("无限血量", ImVec2(84, 0))) LogAct("无限血量", ActHealth(0));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("一次性把血量写成极大值（打不死）");
    ImGui::SameLine();
    if (ImGui::Button("加速", ImVec2(84, 0))) LogAct("加速", ActSpeed(0));

    // —— 第 2 排：速度 / 归属 / 摧毁 ——
    if (ImGui::Button("恢复速度", ImVec2(84, 0))) LogAct("恢复速度", ActSpeed(3));
    ImGui::SameLine();
    if (ImGui::Button("减速", ImVec2(84, 0))) LogAct("减速", ActSpeed(1));
    ImGui::SameLine();
    if (ImGui::Button("冻结单位", ImVec2(84, 0))) LogAct("冻结单位", ActSpeed(2));
    ImGui::SameLine();
    if (ImGui::Button("接管单位", ImVec2(84, 0))) LogAct("接管单位", ActSetOwner());
    ImGui::SameLine();
    if (ImGui::Button("摧毁单位", ImVec2(84, 0))) LogAct("摧毁单位", ActKillSelected());
}

// ===== 新面板：杂项操作（从 TabBattle 拆出，2026-10-15 UI 改版）=====
void PanelMisc() {
    bool b;
    b = GetFeature(FID_GODMODE);
    if (ImGui::Checkbox("玩家无敌", &b)) SetFeature(FID_GODMODE, b);
    b = GetFeature(FID_MAP);
    if (ImGui::Checkbox("清除地图迷雾", &b)) SetFeature(FID_MAP, b);
    b = GetFeature(FID_SUPERPOWER);
    if (ImGui::Checkbox("超武就绪", &b)) SetFeature(FID_SUPERPOWER, b);
    b = GetFeature(FID_DISABLESP);
    if (ImGui::Checkbox("禁用敌方超武", &b)) SetFeature(FID_DISABLESP, b);
    b = GetFeature(FID_ENEMYBUILD);
    if (ImGui::Checkbox("禁止电脑建造", &b)) SetFeature(FID_ENEMYBUILD, b);
    ImGui::Separator();
    ImGui::Text("危险等级:");
    ImGui::SameLine();
    const int dg = g_flags[FID_DANGER];
    if (ImGui::RadioButton("不干预", dg == 0)) g_flags[FID_DANGER] = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("最高", dg == 1)) g_flags[FID_DANGER] = 1;
    ImGui::SameLine();
    if (ImGui::RadioButton("最低", dg == 2)) g_flags[FID_DANGER] = 2;
}

// ===== 面板：脑机接口（MCP）=====
// AI（任意 MCP 客户端）通过本机 HTTP 接口读取局势、投放单位/箱子。
// 本面板 = 态势显示 + 客户端配置 + AI 调用记录。
// 2026-10-16：取消「总闸」——接口只监听 127.0.0.1，把 MCP 配置交给 AI 即视为已授权。
void PanelAi() {
    // ---- 接口状态（无总闸）----
    ImGui::TextColored(ImVec4(0.45f, 0.90f, 0.55f, 1.f), "● 接口已就绪 —— AI 可直接读写");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("仅监听 127.0.0.1（单机）。把 MCP 配置交给 AI 即视为已授权，\n"
                          "无需在面板里再开任何开关。");
    ImGui::TextDisabled("接口 %s   已服务 %d 次", HttpListenLine(), AiCallCount());
    ImGui::Separator();

    // ---- 实时战场态势 ----
    if (ImGui::CollapsingHeader("战场态势（我方 / 敌方）", ImGuiTreeNodeFlags_DefaultOpen)) {
        static float lastReq = -1.f;
        if (g_logClock - lastReq >= 1.5f) { lastReq = g_logClock; BattlefieldRequestScan(); }
        BattleSummary s;
        if (LastBattleSummary(&s)) {
            ImGui::Text("我方：部队 %d   建筑 %d", s.localUnits, s.localStructs);
            ImGui::SameLine();
            ImGui::TextDisabled("(共 %d 归属)", s.owners);
            for (int i = 0; i < s.owners && i < 16; ++i) {
                const BattleOwner& o = s.list[i];
                if (o.isLocal || o.playable == 0) continue;
                const char* side = (o.side >= 0 && o.side < B_SIDE_COUNT)
                                   ? kBattleSideNames[o.side] : "?";
                ImGui::BulletText("敌 %d [%s] 部队 %d / 建筑 %d", o.idx, side, o.units, o.structs);
                if (ImGui::IsItemHovered() && o.topN > 0) {
                    ImGui::BeginTooltip();
                    for (int j = 0; j < o.topN; ++j)
                        ImGui::Text("%s ×%d", o.top[j].name, o.top[j].count);
                    ImGui::EndTooltip();
                }
            }
            ImGui::TextDisabled("扫描 %dMB / %ums / vt=%08X %s",
                                s.scannedMb, (unsigned)s.elapsedMs,
                                (unsigned)BattleThingVtable(), s.fromCache ? "(缓存)" : "");
        } else {
            ImGui::TextDisabled("尚未采样（进局后自动刷新；悬停敌行看构成）");
        }
    }

    // ---- 客户端配置 ----
    if (ImGui::CollapsingHeader("MCP 客户端配置")) {
        ImGui::TextDisabled("把下面这段贴进支持 MCP 的客户端（Claude Desktop / 任意 agent）：");
        static char cfg[1024];
        _snprintf_s(cfg, sizeof(cfg), _TRUNCATE,
                    "{\n  \"mcpServers\": {\n    \"ra3-copilot\": {\n"
                    "      \"command\": \"python\",\n      \"args\": [\"%smcp\\\\ra3_mcp.py\"]\n"
                    "    }\n  }\n}",
                    DllDir());
        ImGui::PushItemWidth(-1);
        ImGui::InputTextMultiline("##mcpcfg", cfg, sizeof(cfg), ImVec2(-1, 108),
                                  ImGuiInputTextFlags_ReadOnly);
        ImGui::PopItemWidth();
        if (ImGui::SmallButton("复制")) ImGui::SetClipboardText(cfg);
        ImGui::SameLine();
        ImGui::TextDisabled("MCP 工具：get_battle_state / summon_unit / drop_crate …");
    }

    // ---- AI 调用记录 ----
    if (ImGui::CollapsingHeader("AI 调用记录（最近）", ImGuiTreeNodeFlags_DefaultOpen)) {
        static char lb[4096];
        static float lastDump = -1.f;
        if (g_logClock - lastDump >= 0.4f) {
            lastDump = g_logClock;
            int n = AiLogDump(lb, sizeof(lb), 40);
            if (n <= 0) strcpy_s(lb, "（暂无 AI 调用）");
        }
        ImGui::BeginChild("##ailog", ImVec2(0, 140), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(lb);
        ImGui::EndChild();
    }
}

// ---------- 支援面板：搜索匹配 ----------
char AsciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }
// 大小写不敏感子串匹配（UTF-8 中文字节 >127 原样比较，不影响中文）
bool SubStrNoCase(const char* hay, const char* needle) {
    if (!hay || !needle || !needle[0]) return false;
    const size_t n = strlen(needle);
    for (const char* p = hay; *p; ++p) {
        size_t i = 0;
        while (i < n && p[i] && AsciiLower(p[i]) == AsciiLower(needle[i])) ++i;
        if (i == n) return true;
    }
    return false;
}

// 中文名 / 对象模板名 / 拼音（首字母如 jdc + 全拼如 jidiche）/ 类型（搜"建筑"能筛出一类）
bool UnitMatches(const SupportUnit& u, const char* q) {
    return SubStrNoCase(u.name, q) || SubStrNoCase(u.id, q) ||
           (u.py && u.py[0] && SubStrNoCase(u.py, q)) || SubStrNoCase(u.type, q);
}

// ===== 面板：支援召唤（2026-10-15 改版：左侧竖排阵营 + 单位列表 + 候选）=====
void TabUnits() {
    ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.95f, 1.f), "支援单位召唤");
    ImGui::TextDisabled("先在游戏里选中一个单位当落点，再点「召唤」（落点来源见「设置」页）");
    // 召唤物隐形（2026-10-15）：默认关 = 召唤出来就是实心、敌方正常攻击。
    // 勾上 = 置"隐形位"，对敌方不可见（小地图空心、AI 不主动打）—— 当外挂用。
    {
        bool st = GetSummonStealth();
        if (ImGui::Checkbox("召唤单位隐形", &st)) {
            SetSummonStealth(st);
            AddLog(st ? LOG_OK : LOG_INFO, "[召唤] 隐形 %s（%s）", st ? "开" : "关",
                   st ? "敌方看不见、不主动攻击" : "召唤即实心、敌方正常攻击");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("关（默认）：召唤出来直接可见，敌方会正常攻击。\n"
                              "开：把对象置成「隐形」状态 —— 小地图图标空心、敌方不会主动攻击。\n"
                              "（实测三位标志位：+0xC8 bit0x800 / +0x28 / +0x208 的 bit31）");
    }

    // 阵营栏：0 = 全部；1..kSupportFactionCount = 数据里的阵营；kSupportFactionCount+1 = 箱子
    const int kCrateTab = kSupportFactionCount + 1;
    if (g_facIdx < 0 || g_facIdx > kCrateTab) g_facIdx = 0;
    const bool isCrate = (g_facIdx == kCrateTab);
    const SupportUnit* src = isCrate ? kCrates : kSupportUnits;
    const int srcN = isCrate ? kCrateCount : kSupportUnitCount;

    ImGui::SetNextItemWidth(190);
    ImGui::InputTextWithHint("##search", "名称 / 拼音(jdc) / 模板名…",
                             g_unitSearch, sizeof(g_unitSearch));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("三种都能搜：中文名（基地车）、拼音首字母或全拼（jdc / jidiche）、\n"
                          "对象模板名（AlliedMCV，大小写不限）\n"
                          "搜到内容时会自动跨阵营（列表项前面标注所属阵营）");
    ImGui::SameLine();
    ImGui::Checkbox("只看基准形态", &g_primaryOnly);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("只列会出现在建造/生产面板的基础形态；\n过滤 _Water / _EMC / _Dropped 等形态变体");

    const bool searching = (g_unitSearch[0] != 0);
    static std::vector<int> filt;
    filt.clear();
    for (int i = 0; i < srcN; ++i) {
        const SupportUnit& u = src[i];
        if (!isCrate) {
            // 搜索时忽略阵营页签（跨阵营找），平时按页签过滤
            if (!searching && g_facIdx > 0 &&
                strcmp(u.faction, kSupportFactions[g_facIdx - 1]) != 0) continue;
            if (g_primaryOnly && !u.primary) continue;
        }
        if (searching && !UnitMatches(u, g_unitSearch)) continue;
        filt.push_back(i);
    }

    // ---- 左：竖排阵营 ----
    ImGui::BeginChild("##fac", ImVec2(104, 268), true);
    if (ImGui::Selectable("全部", g_facIdx == 0)) g_facIdx = 0;
    for (int i = 0; i < kSupportFactionCount; ++i)
        if (ImGui::Selectable(kSupportFactions[i], g_facIdx == i + 1)) g_facIdx = i + 1;
    if (ImGui::Selectable("箱子", g_facIdx == kCrateTab)) g_facIdx = kCrateTab;
    ImGui::EndChild();

    // ---- 中：单位列表 ----
    ImGui::SameLine();
    ImGui::BeginChild("##list", ImVec2(340, 268), true);
    ImGui::TextDisabled("%d 项", (int)filt.size());
    for (int fi = 0; fi < (int)filt.size(); ++fi) {
        const SupportUnit& u = src[filt[fi]];
        ImGui::PushID((isCrate ? 5000 : 0) + filt[fi]);
        char label[288];
        if (isCrate)
            snprintf(label, sizeof(label), "%s##%s", u.name, u.id);   // 箱子页签内不必再标"箱子"
        else if (searching)      // 跨阵营搜索时把阵营也带出来
            snprintf(label, sizeof(label), "[%s·%s] %s##%s", u.type, u.faction, u.name, u.id);
        else
            snprintf(label, sizeof(label), "[%s] %s##%s", u.type, u.name, u.id);
        if (ImGui::Selectable(label, false)) {
            g_candHash.push_back(u.hash);
            g_candName.push_back(std::string(u.name));
            g_candCount.push_back(g_summonCount);
            AddLog(LOG_INFO, "[召唤] 加入候选: %s (0x%08X)", u.name, u.hash);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n%s\n0x%08X", u.name, u.id, u.hash);
        ImGui::PopID();
    }
    ImGui::EndChild();

    // ---- 右：候选 ----
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("候选 (%d)", (int)g_candHash.size());
    ImGui::BeginChild("##cand", ImVec2(238, 168), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (int i = 0; i < (int)g_candHash.size(); ++i) {
        ImGui::PushID(1000 + i);
        ImGui::SetNextItemWidth(46);
        ImGui::DragInt("##c", &g_candCount[i], 1, 1, 20);
        ImGui::SameLine();
        if (ImGui::SmallButton("移除")) {
            g_candHash.erase(g_candHash.begin() + i);
            g_candName.erase(g_candName.begin() + i);
            g_candCount.erase(g_candCount.begin() + i);
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(g_candName[i].c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SetNextItemWidth(100);
    ImGui::DragInt("默认数量", &g_summonCount, 1, 1, 20);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::Combo("归属", &g_owner, "己方\0玩家1\0玩家2\0玩家3\0玩家4\0玩家5\0玩家6\0玩家7\0", 9);
    if (ImGui::Button("召唤", ImVec2(120, 0))) {
        if (g_candHash.empty()) AddLog(LOG_WARN, "[召唤] 候选为空");
        for (int i = 0; i < (int)g_candHash.size(); ++i)
            SpawnSupportUnit(g_candHash[i], g_candCount[i], g_owner, true, g_candName[i].c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("清空", ImVec2(70, 0))) { g_candHash.clear(); g_candName.clear(); g_candCount.clear(); }

    // ---- 自定义代码召唤（十六进制，0x 前缀可有可无）----
    ImGui::Separator();
    static char hexBuf[24] = "";
    ImGui::SetNextItemWidth(150);
    ImGui::InputTextWithHint("##hex", "自定义代码 如 7918266E", hexBuf, sizeof(hexBuf));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("直接按单位代码召唤（十六进制）。\n"
                          "加不加 0x 前缀都行：7918266E / 0x7918266E / 0X7918266E。\n"
                          "代码可在上面列表里悬停查看，或看「输出记录」的调试详情。");
    ImGui::SameLine();
    if (ImGui::Button("召唤该代码", ImVec2(110, 0))) {
        unsigned v = 0;
        const char* p = hexBuf;
        while (*p == ' ' || *p == '\t') ++p;                       // 容忍前导空白
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;   // 容忍 0x / 0X 前缀
        if (*p && sscanf_s(p, "%x", &v) == 1 && v) {
            const char* kn = UnitNameByHash(v);
            SpawnSupportUnit(v, g_summonCount, g_owner, true, kn ? kn : "自定义代码");
        } else {
            AddLog(LOG_WARN, "[召唤] 代码无效：\"%s\"（示例：7918266E 或 0x7918266E）", hexBuf);
        }
    }
    ImGui::EndGroup();
}

void TabSettings() {
    ImGui::SliderFloat("背景透明度", &g_cfg.alpha, 0.2f, 1.0f, "%.2f");
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.95f, 1.f), "安全护栏");
    {
        bool dr = GetDryRun();
        if (ImGui::Checkbox("演练模式（只记录、不改内存）", &dr)) {
            SetDryRun(dr);
            AddLog(dr ? LOG_WARN : LOG_OK, "[护栏] 演练模式 %s", dr ? "已开启（不会真的改内存）" : "已关闭");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("怀疑某个功能导致闪退时打开它：只会往输出记录里写\n"
                              "「本应执行什么」，不会碰游戏内存 —— 用来二分定位是哪一项");
        bool ng = GetNoGameCall();
        if (ImGui::Checkbox("禁用游戏函数调用（召唤/满级 会被跳过）", &ng)) {
            SetNoGameCall(ng);
            AddLog(ng ? LOG_WARN : LOG_OK, "[护栏] 调用游戏函数 %s",
                   ng ? "已禁用（更安全，但召唤/满级不可用）" : "已允许");
        }
        ImGui::TextDisabled("护栏已拦下 %d 次可疑写入，实际写入 %d 次", GuardSkips(), GuardWrites());
    }
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.95f, 1.f), "崩溃日志");
    {
        bool dl = g_cfg.diskLog;
        if (ImGui::Checkbox("详细日志落盘（定位崩溃用）", &dl)) {
            g_cfg.diskLog = dl;
            AddLog(dl ? LOG_OK : LOG_INFO, "[日志] 详细日志落盘 %s", dl ? "开" : "关");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("开启后：每条输出记录 + 每个动作都带「时间 / 帧号 / 级别」写进日志文件，\n"
                              "另有每 5 秒一条的心跳（帧号 / 选中数 / 资金 / 钩子数）。\n"
                              "崩了直接看文件末尾 —— 最后一行就是死掉的那一步。");
        ImGui::SameLine();
        if (ImGui::SmallButton("清空日志")) {
            HANDLE h = CreateFileA(DiskLogPath(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            AddLog(LOG_INFO, "[日志] 已清空日志文件");
        }
        ImGui::TextDisabled("日志文件：%s", DiskLogPath());
        ImGui::TextDisabled("危险动作现场：ra3copilot_last.txt —— 动作名 + 参数 + 帧号");
    }
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.95f, 1.f), "鼠标 / 落点");
    ImGui::Checkbox("拦截游戏点击（鼠标在菜单上时不穿透到游戏）", &g_cfg.blockInput);
    ImGui::TextDisabled("开启后：点菜单不会同时点到游戏世界，选择/指向不会被清掉");
    ImGui::SetNextItemWidth(220);
    ImGui::Combo("落点来源", &g_cfg.posMode, "视角中心(相机)\0选中单位\0游戏光标\0");
    ImGui::TextDisabled("召唤 / 复制的落点。复制优先落在鼠标处；面板挡住位置时退化为视角中心");

    // ===== 快捷键（两列并排，各带滚动）=====
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.95f, 1.f), "快捷键");
    ImGui::TextDisabled("INSERT 显示或隐藏本菜单");
    {
        const int hkTotal = (int)(sizeof(kHK) / sizeof(kHK[0]));
        const int half = (hkTotal + 1) / 2;
        const float ch = ImGui::GetTextLineHeightWithSpacing() * (float)half + 10.0f;
        if (ImGui::BeginTable("##hktwo", 2, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            for (int col = 0; col < 2; ++col) {
                ImGui::TableSetColumnIndex(col);
                ImGui::BeginChild(col == 0 ? "##hkL" : "##hkR", ImVec2(0, ch), true);
                const int lo = col * half;
                const int hi = (col == 0) ? half : hkTotal;
                for (int i = lo; i < hi; ++i) {
                    ImGui::Text("%-9s", kHK[i].label);
                    ImGui::SameLine(84);
                    ImGui::TextDisabled("%s", kHK[i].desc);
                }
                ImGui::EndChild();
            }
            ImGui::EndTable();
        }
    }

    // ===== 关于（整段放最下面）=====
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.95f, 1.f), "关于");
    ImGui::TextWrapped("Ra3-Copilot 尝试兼容多版本红色警戒3命令与征服的进程内辅助程序");
    ImGui::TextDisabled("作者：探姬_Official");
    ImGui::TextDisabled("仓库：https://github.com/ProbiusOfficial/Ra3-Copilot");
}

void TabStatus() {
    ImGui::Text("功能钩子: %d/%d 安装（AOB 通过 %d）",
                g_hookSt.installed, g_hookSt.total, g_hookSt.aobOk);
    if (g_hookSt.failed[0]) ImGui::TextWrapped("AOB 不符未装: %s", g_hookSt.failed);
    ImGui::Separator();
    ImGui::Text("渲染: EndScene=%u  Present=%u  帧=%u",
                g_telemEndScene, g_telemPresent, g_telemRenderOnce);
    ImGui::Text("d3d9 函数已 hook: %u（尝试 %u）", g_telemVtablesHooked, g_telemScanRounds);
    ImGui::Text("游戏窗口: 0x%08X  子类化:%s  ImGui:%s",
                (unsigned)(uintptr_t)g_ov.wnd, g_telemWndHooked ? "OK" : "NO",
                g_ov.imguiReady ? "OK" : "NO");
    ImGui::Text("资金钩子: 进入 %u / 生效 %u", g_telemEnter, g_telemApplied);
    ImGui::Separator();
    if (ImGui::Button("诊断（写入输出记录）", ImVec2(200, 0))) DiagReport();
    ImGui::SameLine();
    ImGui::TextDisabled("点一下，把召唤链路每步的值打进日志");
}

// 每帧逻辑：与「菜单是否可见」无关。
// Present 与 SwapChain::Present 可能对同一帧各触发一次 → 用毫秒戳去重。
void OverlayTick() {
    static DWORD s_lastMs = 0;
    const DWORD now = GetTickCount();
    if (now == s_lastMs) return;
    const float dt = s_lastMs ? (now - s_lastMs) / 1000.0f : 0.0f;
    s_lastMs = now;

    HttpApiPump();                    // ★ 执行调试接口投递过来的命令（本线程=游戏主线程）
    PostProcPump();                   // ★ 到期补跑"创建后处理"（登记召唤物进玩家体系）
    ++g_tickNow;                      // 帧计数（选中单位新鲜度判断用）
    g_logClock += dt;
    // 详细日志：每 5 秒一条心跳（只落盘、不进 UI 日志环）。
    // 日志若断在某个心跳之后 = 崩溃/卡死；若在心跳前正常收尾 = 主动退出。
    static float lastBeat = -10.f;
    if (g_cfg.diskLog && g_logClock - lastBeat >= 5.0f) {
        lastBeat = g_logClock;
        char hb[176];
        _snprintf_s(hb, sizeof(hb), _TRUNCATE,
                    "[心跳] 帧=%u 选中=%d 资金=%d 钩子=%d/%d 护栏跳过=%d",
                    (unsigned)g_tickNow, SelectedCount(), (int)LocalMoney(),
                    g_hookSt.installed, g_hookSt.total, GuardSkips());
        WriteDiskLog(LOG_INFO, hb);
    }
    // ★ 先在渲染线程（= 游戏主线程）执行排队的快捷键动作
    for (int k = 0; k < 8; ++k) { const int id = PopHotkey(); if (!id) break; RunHotkey(id); }
    // 金额锁定：每帧把资金回写到锁定值（在游戏主线程上做，安全）
    if (g_moneyLock && LocalPlayerReady()) MoneySet(g_moneyLockVal);
    // 血量锁定：每帧把选中单位血量回写到其上限
    if (g_hpLock) LockSelectedHealth();
    // 自动空投
    if (g_crateAuto) {
        g_crateTimer += dt;
        if (g_crateTimer >= g_cratePeriod) { g_crateTimer = 0.f; DropCrate(g_owner, 2, true); }
    }
}

// ---- 各面板显隐（多浮动面板，2026-10-15 UI 改版）----
static bool g_pMain = true, g_pRes = true, g_pUnit = true, g_pSummon = true,
            g_pMisc = true, g_pSet = false, g_pLog = true, g_pAi = false;
static int  g_rectFill = 0;

// 记录当前窗口矩形（供 WndProc 判断"鼠标是否落在某个面板上"）
static void MarkPanelRect() {
    if (g_rectFill >= 12) return;
    const ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    g_panelRects[g_rectFill][0] = p.x;            g_panelRects[g_rectFill][1] = p.y;
    g_panelRects[g_rectFill][2] = p.x + s.x;      g_panelRects[g_rectFill][3] = p.y + s.y;
    ++g_rectFill;
}

void BuildUI() {
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = g_cfg.alpha;
    g_rectFill = 0;
    if (!g_cfg.visible) { DrainColorStack(); g_panelRectN = 0; return; }   // INSERT 总开关：关掉时一个面板都不画

    // 默认布局（仅首次显示生效；之后用户拖动的位置会被 ImGui 记住）
    static const ImVec2 kPos[8] = {
        ImVec2(40, 90),  ImVec2(40, 350),  ImVec2(300, 90), ImVec2(300, 400),
        ImVec2(660, 90), ImVec2(660, 350), ImVec2(40, 640), ImVec2(660, 560)
    };

    // ===== 主面板 =====
    ImGui::SetNextWindowPos(kPos[0], ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(210, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Ra3-Copilot##main", &g_pMain, ImGuiWindowFlags_AlwaysAutoResize)) {
        MarkPanelRect();
        ImGui::TextDisabled("面板显隐");
        ImGui::Checkbox("资源科技", &g_pRes);
        ImGui::Checkbox("单位操作", &g_pUnit);
        ImGui::Checkbox("支援召唤", &g_pSummon);
        ImGui::Checkbox("杂项操作", &g_pMisc);
        ImGui::Checkbox("脑机接口", &g_pAi);
        ImGui::Checkbox("设置", &g_pSet);
        ImGui::Checkbox("输出记录", &g_pLog);
        ImGui::Separator();
        if (ImGui::Button("全部收起")) { g_pRes = g_pUnit = g_pSummon = g_pMisc = g_pSet = g_pLog = g_pAi = false; }
        ImGui::SameLine();
        if (ImGui::Button("全部展开")) { g_pRes = g_pUnit = g_pSummon = g_pMisc = g_pSet = g_pLog = g_pAi = true; }
        ImGui::Separator();
        if (ImGui::Button("隐藏全部 (INSERT)")) g_cfg.visible = false;
    }
    ImGui::End();

    // ===== 资源科技 =====
    if (g_pRes) {
        ImGui::SetNextWindowPos(kPos[1], ImGuiCond_FirstUseEver);
        if (ImGui::Begin("资源科技##res", &g_pRes)) { MarkPanelRect(); TabResource(); }
        ImGui::End();
    }
    // ===== 单位操作 =====
    if (g_pUnit) {
        ImGui::SetNextWindowPos(kPos[2], ImGuiCond_FirstUseEver);
        if (ImGui::Begin("单位操作##unit", &g_pUnit)) { MarkPanelRect(); PanelUnitOps(); }
        ImGui::End();
    }
    // ===== 支援召唤 =====
    if (g_pSummon) {
        ImGui::SetNextWindowPos(kPos[3], ImGuiCond_FirstUseEver);
        if (ImGui::Begin("支援召唤##summon", &g_pSummon)) { MarkPanelRect(); TabUnits(); }
        ImGui::End();
    }
    // ===== 杂项操作 =====
    if (g_pMisc) {
        ImGui::SetNextWindowPos(kPos[4], ImGuiCond_FirstUseEver);
        if (ImGui::Begin("杂项操作##misc", &g_pMisc)) { MarkPanelRect(); PanelMisc(); }
        ImGui::End();
    }
    // ===== 脑机接口（MCP）=====
    if (g_pAi) {
        ImGui::SetNextWindowPos(kPos[7], ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(430, 460), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("脑机接口##ai", &g_pAi)) { MarkPanelRect(); PanelAi(); }
        ImGui::End();
    }
    // ===== 设置 =====
    if (g_pSet) {
        ImGui::SetNextWindowPos(kPos[5], ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(680, 620), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("设置##set", &g_pSet)) { MarkPanelRect(); TabSettings(); }
        ImGui::End();
    }
    // ===== 输出记录 =====
    if (g_pLog) {
        ImGui::SetNextWindowPos(kPos[6], ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(560, 200), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("输出记录##log", &g_pLog)) {
            MarkPanelRect();
            if (ImGui::SmallButton("清空")) g_logCount = 0;
            ImGui::SameLine();
            if (ImGui::SmallButton("关闭")) g_pLog = false;
            ImGui::Separator();
            DrawLogPanel();
        }
        ImGui::End();
    }

    DrainColorStack();        // ★ 护栏：把任何遗留的 PushStyleColor 清掉，杜绝 ImGui 断言框
    g_panelRectN = g_rectFill;
}

// ===================== 快捷键执行（表在上方 UI 区） =====================
void ToggleFeature(int idx, const char* name) {
    bool on = !GetFeature(idx);
    SetFeature(idx, on);
    Ding();
    AddLog(on ? LOG_OK : LOG_INFO, "[开关] %s → %s", name, on ? "开" : "关");
}

void RunHotkey(int id) {
    switch (id) {
    case HK_MONEY:        MoneyAdd(g_cfg.addAmount); Ding(); AddLog(LOG_OK, "[钱] +%d", g_cfg.addAmount); break;
    case HK_POWER:        ToggleFeature(FID_POWER, "电力无限"); break;
    case HK_SCPOINT:      ToggleFeature(FID_SCPOINT, "科技点无限"); break;
    case HK_HAVEALLSC:    ToggleFeature(FID_HAVEALLSC, "全科技"); break;
    case HK_FASTBUILD:    ToggleFeature(FID_FASTBUILD, "快速建造"); break;
    case HK_SUPERPOWER:   ToggleFeature(FID_SUPERPOWER, "超武就绪"); break;
    case HK_DISABLESP:    ToggleFeature(FID_DISABLESP, "禁用敌方超武"); break;
    case HK_MAP:          ToggleFeature(FID_MAP, "全图/去迷雾"); break;
    case HK_ENEMYBUILD:   ToggleFeature(FID_ENEMYBUILD, "敌人无法建造"); break;
    case HK_GODMODE:      ToggleFeature(FID_GODMODE, "玩家无敌"); break;
    case HK_AMMO:         ToggleFeature(FID_UNITAMMO, "选中单位弹药满"); break;
    case HK_CHANGEID:     LogAct("接管单位", ActSetOwner()); break;
    case HK_COPY: {
        int r = ActCopySelected();
        if (r > 0) { Ding(); AddLog(LOG_OK, "[复制] 选中单位副本已生成"); }
        else AddLog(LOG_WARN, "[复制] 失败：%s", SummonLastError());
        break;
    }
    case HK_DESTROY:      LogAct("摧毁单位", ActKillSelected()); break;
    case HK_LEVELUP:      LogAct("升级单位", ActRank()); break;
    case HK_SPEEDMAX:     LogAct("加速", ActSpeed(0)); break;
    case HK_SPEEDSLOW:    LogAct("减速", ActSpeed(1)); break;
    case HK_FREEZE:       LogAct("冻结单位", ActSpeed(2)); break;
    case HK_SPEEDNORM:    LogAct("恢复速度", ActSpeed(3)); break;
    case HK_HPMAX:        LogAct("无限血量", ActHealth(0)); break;
    case HK_HPMIN:        LogAct("残血", ActHealth(1)); break;
    case HK_HPNORM:       LogAct("恢复血量", ActHealth(2)); break;
    case HK_DANGERMAX:    g_flags[FID_DANGER] = 1; Ding(); AddLog(LOG_OK, "[危险] → 最高"); break;
    case HK_DANGERMIN:    g_flags[FID_DANGER] = 2; Ding(); AddLog(LOG_OK, "[危险] → 最低"); break;
    case HK_DANGERNORM:   g_flags[FID_DANGER] = 0; Ding(); AddLog(LOG_INFO, "[危险] → 不干预"); break;
    case HK_OREMINE:      TriggerOneShot(FID_OREMINE); Ding(); AddLog(LOG_OK, "[矿场] 已恢复"); break;
    case HK_FREEBUILD: {
        bool on = !GetFreeBuild();
        bool ok = SetFreeBuild(on);
        if (ok) Ding();
        AddLog(ok ? (on ? LOG_OK : LOG_INFO) : LOG_ERR, "[建造] 免费建造 → %s",
               ok ? (on ? "开" : "关") : "失败(字节不符)");
        break;
    }
    case HK_MCV: {
        bool ok = SummonMCV();
        if (ok) Ding();
        AddLog(ok ? LOG_OK : LOG_WARN, "[召唤] 基地车 %s", ok ? "已生成" : "失败");
        break;
    }
    }
}

// 游戏窗口是否在前台（失去焦点时热键一律不触发，避免切出去误触）
bool GameFocused() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

DWORD WINAPI InputThread(LPVOID) {
    const int N = (int)(sizeof(kHK) / sizeof(kHK[0]));
    bool down[sizeof(kHK) / sizeof(kHK[0])] = { false };
    bool lastIns = false;
    for (;;) {
        const bool focused = GameFocused();
        const bool ins = focused && (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if (ins && !lastIns) g_cfg.visible = !g_cfg.visible;
        lastIns = ins;

        const bool ctrl = focused && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        for (int i = 0; i < N; ++i) {
            const bool cur = focused && (GetAsyncKeyState(kHK[i].vk) & 0x8000) != 0;
            const bool edge = cur && !down[i];
            down[i] = cur;
            if (!edge) continue;
            if (kHK[i].ctrl != ctrl) continue;      // Ctrl 状态要匹配
            PostHotkey(kHK[i].id);                  // ← 只投递，不在本线程执行
        }
        Sleep(25);
    }
    return 0;
}

} // namespace

// ===================== 对外：召唤 / 空投 =====================
void OverlayLogLine(int kind, const char* text) { AddLog(kind, "%s", text); }

const char* DiskLogPath() { EnsureLogInit(); return g_logPath; }

// 本 DLL 所在目录（带结尾反斜杠）—— 脑机接口面板显示 MCP 配置路径用
const char* DllDir() {
    static char dir[MAX_PATH] = { 0 };
    if (!dir[0]) {
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&DllDir), &self);
        GetModuleFileNameA(self, dir, MAX_PATH);
        char* p = strrchr(dir, '\\');
        if (p) *(p + 1) = 0;
    }
    return dir;
}

int OverlayLogDump(char* buf, int n, int maxLines) {
    if (!buf || n <= 0) return 0;
    int w = 0;
    const int total = g_logCount > kLogMax ? kLogMax : g_logCount;
    const int start = (g_logCount > kLogMax) ? (g_logCount - kLogMax) : 0;
    int begin = total - (maxLines > 0 ? maxLines : total);
    if (begin < 0) begin = 0;
    for (int i = begin; i < total; ++i) {
        const LogLine& L = g_log[(start + i) % kLogMax];
        int k = _snprintf_s(buf + w, n - w, _TRUNCATE, "%6.1fs [k%d] %s\n", L.t, L.kind, L.text);
        if (k <= 0) break;
        w += k;
        if (w >= n - 2) break;
    }
    return w;
}

void OverlaySetCrate(int idx, int count) { g_crateIdx = idx; if (count > 0) g_crateCount = count; }

bool GetViewMatrix(float out[16]) {
    if (!g_hasView) return false;
    memcpy(out, g_viewMat, sizeof(float) * 16);
    return true;
}

int ViewRejects() { return (int)g_rejView; }

void GetVtInfo(uint32_t out[8]) {
    for (int i = 0; i < 7; ++i) out[i] = g_vtInfo[i];
    out[7] = static_cast<uint32_t>(g_viewFromHook);
}

// 原始 VIEW 矩阵诊断：lastMat/minMat 各 16 个 float；lastFz/minFz；count
void GetRawView(float lastMat[16], float* lastFz, float minMat[16], float* minFz, int* count) {
    memcpy(lastMat, g_viewLast, sizeof(float) * 16);
    memcpy(minMat, g_viewMinMat, sizeof(float) * 16);
    *lastFz = g_viewLastFz;
    *minFz = g_viewMinFz;
    *count = static_cast<int>(g_viewCount);
}

bool GetCamData(float view[16], float proj[16], int vp[4]) {
    if (!g_hasView || !g_hasProj || !g_viewport[2]) return false;
    memcpy(view, g_viewMat, sizeof(float) * 16);
    memcpy(proj, g_projMat, sizeof(float) * 16);
    memcpy(vp, g_viewport, sizeof(int) * 4);
    return true;
}

bool SpawnSupportUnit(uint32_t hash, int count, int ownerPlayer, bool atMouse, const char* name) {
    bool ok = SummonByHash(hash, count, ownerPlayer, atMouse, name);
    if (ok) AddLog(LOG_OK, "[召唤] %s (0x%08X) x%d 已生成", name ? name : "?", hash, count);
    else AddLog(LOG_WARN, "[召唤] %s (0x%08X) 失败：%s",
                name ? name : "?", hash, SummonLastError());
    return ok;
}

bool DropCrate(int ownerPlayer, int count, bool atMouse) {
    if (g_crateIdx < 0 || g_crateIdx >= kCrateCount) g_crateIdx = 0;
    const SupportUnit& c = kCrates[g_crateIdx];
    bool ok = SummonByHash(c.hash, count, ownerPlayer, atMouse, c.name);
    if (ok) AddLog(LOG_OK, "[空投] %s (%s) x%d 已生成", c.name, c.id, count);
    else AddLog(LOG_WARN, "[空投] %s 失败：%s", c.name, SummonLastError());
    return ok;
}

void OverlayInit() {
    SetUnhandledExceptionFilter(Ra3CrashFilter);   // ★ 先装崩溃捕获（崩溃现场落盘，无视日志开关）
    g_hookSt = InstallHooks();
    SymInit();                          // 符号解析（特征码+结构推导，先于任何动作）
    AddLog(LOG_OK, "功能钩子 %d/%d 已安装（AOB 通过 %d）",
           g_hookSt.installed, g_hookSt.total, g_hookSt.aobOk);

    HttpApiStart(17832);                // ★ 调试接口先起来（/ping 立刻可用；变更类命令等渲染线程）

    for (int i = 0; i < 1800 && !GetModuleHandleA("d3d9.dll"); ++i) Sleep(100);

    HANDLE t = CreateThread(nullptr, 0, InputThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    AddLog(LOG_INFO, "输入线程已启动（INSERT / F7）");

    InstallRenderHooks();
    g_running = true;
}

} // namespace ra3
