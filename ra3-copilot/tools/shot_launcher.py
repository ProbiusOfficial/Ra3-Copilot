#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""shot_launcher.py - 截取日冕启动器窗口（用于定位按钮坐标）

WPF 启动器没有 Win32 子控件（EnumChildWindows 返回 0），所以只能截屏 + 目视定位，
再用 win32api 模拟点击。本脚本只做「截屏」这一步。
"""
import ctypes
import os
import sys
import time

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)   # per-monitor DPI aware，保证坐标与截屏一致
except Exception:
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass

import win32gui
import win32con
from PIL import ImageGrab

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_launcher_shot.png")

hwnd = None
cands = []


def cb(h, _):
    if not win32gui.IsWindowVisible(h):
        return True
    cls = win32gui.GetClassName(h)
    t = win32gui.GetWindowText(h)
    # 启动器主窗 = WPF 类名 HwndWrapper[CoronaLauncher...]；排除资源管理器"属性"对话框
    if "CoronaLauncher" in cls and t and ("属性" not in t):
        l, tp, r, b = win32gui.GetWindowRect(h)
        cands.append((h, t, (r - l) * (b - tp)))
    return True


win32gui.EnumWindows(cb, None)
cands.sort(key=lambda x: -x[2])
if cands:
    hwnd = cands[0][0]
    print("candidates=%d" % len(cands))
    for h, t, area in cands[:3]:
        print("   hwnd=%s area=%d title=%s" % (h, area, t))
if not hwnd:
    print("launcher window not found")
    raise SystemExit(1)

title = win32gui.GetWindowText(hwnd)
print("hwnd=%s title=%s" % (hwnd, title))

win32gui.ShowWindow(hwnd, win32con.SW_RESTORE)
try:
    win32gui.SetForegroundWindow(hwnd)
except Exception as e:
    print("SetForegroundWindow failed: %s" % e)
time.sleep(0.6)

l, t, r, b = win32gui.GetWindowRect(hwnd)
print("rect=%d,%d,%d,%d size=%dx%d" % (l, t, r, b, r - l, b - t))
img = ImageGrab.grab(bbox=(l, t, r, b))
img.save(OUT)
print("saved %s %s" % (OUT, img.size))
