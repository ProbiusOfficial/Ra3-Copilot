"""桌面侧 UI 自动化：找窗口、截屏、点击、按键。纯 ctypes，无第三方依赖。

用途：沙箱内的命令执行工具无法创建进程（Start-Process 抛「找不到所需的全部信息」，
schtasks 在黑名单），因此「启动游戏」这一步只能去点已经跑着的启动器 —— 那样拉起来的
子进程不在本工具的作业对象里，命令结束后不会被回收。窗口起来之后的一切操作都在本模块。

设计约束（每一条都对应一个具体的失败现象）：
- 截屏必须 1:1 像素。看图时图像会按长边缩放，用整图比例反推坐标会累积几十像素误差，
  正确做法是只截目标附近的小块，在局部图里目测中心。
- 点击前必须切前台。BitBlt 抓的是屏幕内容，窗口被遮挡时抓到的是上层窗口。
- 点击前先把光标挪到附近再移到位：部分 DirectInput 游戏只处理「移动事件」后的按键。
- 坐标统一是屏幕绝对像素；进程启动时声明 DPI 感知，避免系统缩放把坐标虚拟化。
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import json
import struct
import sys
import time

_user32 = ctypes.WinDLL("user32", use_last_error=True)
_gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

SW_RESTORE = 9
SRCCOPY = 0x00CC0020
COLORONCOLOR = 3
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

_MOUSE_LEFTDOWN, _MOUSE_LEFTUP = 0x0002, 0x0004
_VK = {"esc": 0x1B, "enter": 0x0D, "space": 0x20, "tab": 0x09, "f4": 0x73, "alt": 0x12}


def dpi_aware():
    """声明 DPI 感知，使 GetWindowRect 与截屏都使用物理像素。"""
    try:
        _user32.SetProcessDPIAware()
    except Exception:
        pass


def _proc_path(pid):
    h = _kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return ""
    try:
        buf = ctypes.create_unicode_buffer(600)
        n = wt.DWORD(600)
        ok = _kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n))
        return buf.value if ok else ""
    finally:
        _kernel32.CloseHandle(h)


def windows(exe=None, min_width=100):
    """枚举顶层可见窗口。exe 为可执行文件名后缀，比较时不区分大小写。"""
    out = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def _cb(hwnd, _):
        if not _user32.IsWindowVisible(hwnd):
            return True
        pid = wt.DWORD()
        _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        path = _proc_path(pid.value)
        if exe and not path.lower().endswith(exe.lower()):
            return True
        r = wt.RECT()
        _user32.GetWindowRect(hwnd, ctypes.byref(r))
        if r.right - r.left < min_width:
            return True
        n = _user32.GetWindowTextLengthW(hwnd)
        title = ctypes.create_unicode_buffer(n + 1)
        _user32.GetWindowTextW(hwnd, title, n + 1)
        out.append({
            "hwnd": hwnd,
            "pid": pid.value,
            "title": title.value,
            "rect": [r.left, r.top, r.right, r.bottom],
            "size": [r.right - r.left, r.bottom - r.top],
        })
        return True

    _user32.EnumWindows(_cb, 0)
    return out


def focus(hwnd):
    """切前台。SetForegroundWindow 受前台锁限制时会失败，用 AttachThreadInput 兜底。"""
    _user32.ShowWindow(hwnd, SW_RESTORE)
    _user32.BringWindowToTop(hwnd)
    _user32.SetForegroundWindow(hwnd)
    time.sleep(0.3)
    if _user32.GetForegroundWindow() == hwnd:
        return True
    fg = _user32.GetForegroundWindow()
    cur = _kernel32.GetCurrentThreadId()
    tgt = _user32.GetWindowThreadProcessId(hwnd, None)
    src = _user32.GetWindowThreadProcessId(fg, None) if fg else 0
    if src:
        _user32.AttachThreadInput(cur, src, True)
    _user32.AttachThreadInput(cur, tgt, True)
    ok = _user32.SetForegroundWindow(hwnd)
    _user32.AttachThreadInput(cur, tgt, False)
    if src:
        _user32.AttachThreadInput(cur, src, False)
    time.sleep(0.3)
    return bool(ok) and _user32.GetForegroundWindow() == hwnd


def grab(x, y, w, h, out, scale=1):
    """截取屏幕矩形并存成 32 位 BMP。scale>1 时用 StretchBlt 放大，便于识别小图标。"""
    W, H = w * scale, h * scale
    hdc_screen = _user32.GetDC(0)
    hdc_mem = _gdi32.CreateCompatibleDC(hdc_screen)
    hbmp = _gdi32.CreateCompatibleBitmap(hdc_screen, W, H)
    _gdi32.SelectObject(hdc_mem, hbmp)
    if scale == 1:
        _gdi32.BitBlt(hdc_mem, 0, 0, W, H, hdc_screen, x, y, SRCCOPY)
    else:
        _gdi32.SetStretchBltMode(hdc_mem, COLORONCOLOR)
        _gdi32.StretchBlt(hdc_mem, 0, 0, W, H, hdc_screen, x, y, w, h, SRCCOPY)

    class BIH(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                    ("biPlanes", wt.WORD), ("biBitCount", wt.WORD),
                    ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                    ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG),
                    ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]

    bi = BIH()
    bi.biSize = ctypes.sizeof(BIH)
    bi.biWidth = W
    bi.biHeight = -H            # 负数 = 自上而下，省一次翻转
    bi.biPlanes = 1
    bi.biBitCount = 32
    stride = W * 4
    buf = ctypes.create_string_buffer(stride * H)
    _gdi32.GetDIBits(hdc_mem, hbmp, 0, H, buf, ctypes.byref(bi), 0)

    rows = [buf.raw[i * stride:(i + 1) * stride] for i in range(H)]
    rows.reverse()
    pixels = b"".join(rows)
    head = struct.pack("<2sIHHI", b"BM", 14 + 40 + len(pixels), 0, 0, 14 + 40)
    info = struct.pack("<IiiHHIIiiII", 40, W, H, 1, 32, 0, len(pixels), 0, 0, 0, 0)
    with open(out, "wb") as f:
        f.write(head + info + pixels)

    _gdi32.DeleteObject(hbmp)
    _gdi32.DeleteDC(hdc_mem)
    _user32.ReleaseDC(0, hdc_screen)
    return {"out": out, "size": [W, H]}


def click(x, y, times=1, delay=0.4):
    _user32.SetCursorPos(x - 4, y - 4)
    time.sleep(0.06)
    _user32.SetCursorPos(x, y)
    time.sleep(delay)
    for _ in range(times):
        _user32.mouse_event(_MOUSE_LEFTDOWN, 0, 0, 0, 0)
        time.sleep(0.08)
        _user32.mouse_event(_MOUSE_LEFTUP, 0, 0, 0, 0)
        time.sleep(0.15)
    return {"clicked": [x, y], "times": times}


def key(name, alt=False):
    vk = _VK[name.lower()]
    if alt:
        _user32.keybd_event(_VK["alt"], 0, 0, 0)
        time.sleep(0.05)
    _user32.keybd_event(vk, 0, 0, 0)
    time.sleep(0.05)
    _user32.keybd_event(vk, 0, 2, 0)
    if alt:
        time.sleep(0.05)
        _user32.keybd_event(_VK["alt"], 0, 2, 0)
    return {"key": name, "alt": alt}


def _emit(d):
    print(json.dumps(d, ensure_ascii=False))


def main(argv=None):
    dpi_aware()
    ap = argparse.ArgumentParser(description="桌面侧 UI 自动化（截屏 / 点击 / 按键 / 找窗口）")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("windows", help="列出顶层窗口及其屏幕矩形")
    p.add_argument("--exe", default=None, help="按可执行文件名过滤，如 ra3_1.12.game")
    p.add_argument("--min-width", type=int, default=100)

    p = sub.add_parser("focus", help="把窗口切到前台")
    p.add_argument("--exe", default=None)
    p.add_argument("--hwnd", type=lambda s: int(s, 0), default=None)

    p = sub.add_parser("grab", help="截屏存 BMP")
    p.add_argument("--rect", required=True, help="x,y,w,h")
    p.add_argument("--out", required=True)
    p.add_argument("--scale", type=int, default=1)

    p = sub.add_parser("click", help="点击屏幕坐标")
    p.add_argument("--pos", required=True, help="x,y")
    p.add_argument("--times", type=int, default=1)
    p.add_argument("--delay", type=float, default=0.4)

    p = sub.add_parser("key", help="发送按键")
    p.add_argument("--name", required=True, choices=sorted(_VK))
    p.add_argument("--alt", action="store_true")

    a = ap.parse_args(argv)

    if a.cmd == "windows":
        _emit({"windows": windows(a.exe, a.min_width)})
        return 0

    if a.cmd == "focus":
        if a.hwnd is not None:
            _emit({"hwnd": a.hwnd, "focused": focus(a.hwnd)})
            return 0
        found = windows(a.exe)
        if not found:
            _emit({"ok": 0, "err": "窗口未找到"})
            return 1
        hwnd = found[0]["hwnd"]
        _emit({"hwnd": hwnd, "title": found[0]["title"], "focused": focus(hwnd)})
        return 0

    if a.cmd == "grab":
        x, y, w, h = (int(v) for v in a.rect.split(","))
        _emit(grab(x, y, w, h, a.out, a.scale))
        return 0

    if a.cmd == "click":
        x, y = (int(v) for v in a.pos.split(","))
        _emit(click(x, y, a.times, a.delay))
        return 0

    if a.cmd == "key":
        _emit(key(a.name, a.alt))
        return 0

    return 2


if __name__ == "__main__":
    sys.exit(main())
