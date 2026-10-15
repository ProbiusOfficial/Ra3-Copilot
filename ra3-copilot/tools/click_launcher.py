#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""click_launcher.py - 点击日冕启动器的「开始游戏」按钮

WPF 启动器没有 Win32 子控件，UI Automation 的 .NET 程序集又被本机策略拦，
所以走「窗口坐标 + 模拟鼠标」：先用 ALT 技巧抢前台（否则点击会落到别的窗口），
再 SetCursorPos + mouse_event。

用法：python click_launcher.py [dx] [dy]
      坐标为相对启动器窗口左上角的偏移；默认 = 窗口底边上方 ~95px 的水平中点
      （即截图中「开始游戏」按钮的位置）。
"""
import ctypes
import sys
import time

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    pass

import win32api
import win32con
import win32gui

DEFAULT_DY = -95          # 距窗口底边的像素（按钮在底部中央）


def find_launcher():
    best = None

    def cb(h, _):
        nonlocal best
        if not win32gui.IsWindowVisible(h):
            return True
        cls = win32gui.GetClassName(h)
        t = win32gui.GetWindowText(h)
        if "CoronaLauncher" in cls and t and ("属性" not in t):
            l, tp, r, b = win32gui.GetWindowRect(h)
            if best is None or (r - l) * (b - tp) > best[2]:
                best = (h, t, (r - l) * (b - tp), (l, tp, r, b))
        return True

    win32gui.EnumWindows(cb, None)
    return best


info = find_launcher()
if not info:
    print("launcher not found")
    raise SystemExit(1)
hwnd, title, _, _ = info

# 0) 窗口被最小化时 GetWindowRect 会返回 (-32000, -32000)，
#    照那个坐标点击会落到屏幕外（踩过这个坑）—— 必须先恢复再重新取 rect。
if win32gui.IsIconic(hwnd):
    print("window is minimized -> restore first")
    win32gui.ShowWindow(hwnd, win32con.SW_RESTORE)
    time.sleep(0.6)
l, t, r, b = win32gui.GetWindowRect(hwnd)
print("launcher hwnd=%s rect=%d,%d,%d,%d  %s" % (hwnd, l, t, r, b, title))
if l <= -30000 or r <= l:
    print("rect still off-screen, abort")
    raise SystemExit(2)

# 1) 抢前台：ALT 按下再抬起，可解除 SetForegroundWindow 的限制
win32api.keybd_event(win32con.VK_MENU, 0, 0, 0)
win32api.keybd_event(win32con.VK_MENU, 0, win32con.KEYEVENTF_KEYUP, 0)
try:
    win32gui.SetForegroundWindow(hwnd)
except Exception as e:
    print("SetForegroundWindow warn: %s" % e)
time.sleep(0.4)
fg = win32gui.GetForegroundWindow()
print("foreground now = %s (%s)  match=%s" % (fg, win32gui.GetWindowText(fg), fg == hwnd))

# 2) 计算目标点（默认：底部中央）
dx = int(sys.argv[1]) if len(sys.argv) > 1 else (r - l) // 2
dy = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_DY
if dy < 0:
    dy = (b - t) + dy
x, y = l + dx, t + dy
print("click at screen (%d, %d)  [window-relative %d, %d]" % (x, y, dx, dy))

win32api.SetCursorPos((x, y))
time.sleep(0.15)
win32api.mouse_event(win32con.MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
time.sleep(0.08)
win32api.mouse_event(win32con.MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
print("clicked")
