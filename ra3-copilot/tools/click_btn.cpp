// click_btn.cpp - 按窗口标题定位顶层窗口，再按控件文字定位子控件并点击（BM_CLICK）
//
// 用法：
//   click_btn.exe <窗口标题子串> <控件文字子串>            -> 点击该控件
//   click_btn.exe <窗口标题子串> --list                    -> 列出所有子控件（class + text）
//
// 用于无人值守地驱动 RA3 启动器的「进行游戏」按钮。
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

static HWND        g_top   = nullptr;
static const wchar_t* g_sub = nullptr;
static FILE*       g_dump  = nullptr;

BOOL CALLBACK EnumTopProc(HWND h, LPARAM) {
    wchar_t buf[512] = { 0 };
    int n = GetWindowTextW(h, buf, 512);
    if (g_dump) {
        wchar_t cls[128] = { 0 }; GetClassNameW(h, cls, 128);
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        fwprintf(g_dump, L"  top hwnd=%p pid=%lu vis=%d class=%s title=%s (len=%d)\n",
                 (void*)h, pid, (int)IsWindowVisible(h), cls, buf, n);
    }
    if (!IsWindowVisible(h)) return TRUE;
    if (buf[0] && wcsstr(buf, g_sub)) { g_top = h; return FALSE; }
    return TRUE;
}

#define MAXKIDS 64
struct FindCtx { const wchar_t* sub; HWND hit; int count; FILE* log; HWND kids[MAXKIDS]; int want; };
BOOL CALLBACK EnumChildProc(HWND h, LPARAM lp) {
    FindCtx* c = (FindCtx*)lp;
    wchar_t txt[512] = { 0 }, cls[128] = { 0 };
    GetWindowTextW(h, txt, 512);
    GetClassNameW(h, cls, 128);
    RECT r = { 0,0,0,0 }; GetWindowRect(h, &r);
    ++c->count;
    if (c->count <= MAXKIDS) c->kids[c->count - 1] = h;
    if (c->log) fwprintf(c->log, L"  [%d] class=%-24s hwnd=%p rect=(%ld,%ld)-(%ld,%ld) cx=%ld cy=%ld text=%s\n",
                         c->count, cls, (void*)h, r.left, r.top, r.right, r.bottom,
                         (r.right - r.left), (r.bottom - r.top), txt);
    if (c->want > 0) { if (c->count == c->want) c->hit = h; return TRUE; }
    if (!c->hit && txt[0] && wcsstr(txt, c->sub)) { c->hit = h; }
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    FILE* out = nullptr;
    wchar_t outPath[MAX_PATH];
    ExpandEnvironmentStringsW(L"%TEMP%\\click_btn_result.txt", outPath, MAX_PATH);
    _wfopen_s(&out, outPath, L"w, ccs=UTF-8");

    if (argc < 3) {
        if (out) fwprintf(out, L"usage: click_btn <title-sub> <btn-sub|--list>\n");
        if (out) fclose(out);
        return 2;
    }
    g_sub = argv[1];
    const wchar_t* btnSub = argv[2];
    const bool list = (wcscmp(btnSub, L"--list") == 0);
    const bool top  = (wcscmp(btnSub, L"--top") == 0);
    if (top) g_dump = out;

    EnumWindows(EnumTopProc, 0);
    g_dump = nullptr;

    if (top) {
        if (out) { fwprintf(out, L"[*] --top 模式，已列出全部顶层窗口，未点击\n"); fclose(out); }
        return 0;
    }
    if (!g_top) {
        // 找不到就顺便把全部顶层窗口 dump 出来（便于排错）
        if (out) {
            fwprintf(out, L"[x] 没找到标题含 \"%s\" 的可见顶层窗口\n", g_sub);
            g_dump = out; EnumWindows(EnumTopProc, 0); g_dump = nullptr;
            fclose(out);
        }
        return 3;
    }

    wchar_t tbuf[512] = { 0 };
    GetWindowTextW(g_top, tbuf, 512);
    if (out) fwprintf(out, L"[*] top hwnd=%p title=%s\n", (void*)g_top, tbuf);

    int wantIdx = 0;
    if (wcsncmp(btnSub, L"index:", 6) == 0) wantIdx = _wtoi(btnSub + 6);

    FindCtx c = { btnSub, nullptr, 0, list ? out : nullptr, { 0 }, wantIdx };
    EnumChildWindows(g_top, EnumChildProc, (LPARAM)&c);
    if (out) fwprintf(out, L"[*] 子控件共 %d 个\n", c.count);

    if (list || top) {
        if (out) { fwprintf(out, L"[*] %s 模式，未点击\n", top ? L"--top" : L"--list"); fclose(out); }
        return 0;
    }
    if (!c.hit) {
        if (out) { fwprintf(out, L"[x] 没有文字含 \"%s\" 的子控件\n", btnSub); }
        // 找不到就顺便把控件树 dump 出来（便于排错）
        FindCtx d = { L"\x01", nullptr, 0, out };
        EnumChildWindows(g_top, EnumChildProc, (LPARAM)&d);
        if (out) fclose(out);
        return 4;
    }

    if (out) fwprintf(out, L"[*] 命中控件 hwnd=%p，发送 BM_CLICK\n", (void*)c.hit);
    LRESULT r = SendMessageW(c.hit, BM_CLICK, 0, 0);
    if (out) { fwprintf(out, L"[*] BM_CLICK 返回 %ld, GetLastError=%lu\n", (long)r, GetLastError()); fclose(out); }
    return 0;
}
