#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""_verify_spawn.py — 落点功能自检（游戏需在跑、修改器已注入）。

只读部分：抓四个落点端点 + /diag 里的落点行，核对视野中心是否可用。
加 --summon 会真召唤一个单位（默认"磁爆坦克"）并核对回执里的实际落点。
"""
import json
import sys
import urllib.request

API = "http://127.0.0.1:17832"


def get(path, timeout=8.0):
    return urllib.request.urlopen(API + path, timeout=timeout).read().decode("utf-8", "ignore")


def post(path, timeout=10.0):
    req = urllib.request.Request(API + path, method="POST")
    return urllib.request.urlopen(req, timeout=timeout).read().decode("utf-8", "ignore")


def show(name, path):
    try:
        body = get(path)
    except Exception as e:
        print("  %-14s -> 请求失败: %s" % (name, e))
        return None
    try:
        d = json.loads(body)
    except Exception:
        print("  %-14s -> %s" % (name, body[:120]))
        return None
    if d.get("ok"):
        flag = "  [水面]" if d.get("water") else ""
        print("  %-14s -> (%8.1f, %8.1f, %6.1f)  via=%s%s"
              % (name, d["x"], d["y"], d["z"], d.get("src", "?"), flag))
    else:
        print("  %-14s -> 取不到（%s）" % (name, d.get("why", "")))
    return d


def main():
    print("=== 落点自检 ===")
    try:
        print("  ping -> %s" % get("/ping").strip())
    except Exception as e:
        print("  ping -> 失败：%s（游戏没跑 / 修改器没注入？）" % e)
        return 1

    print("\n-- 落点端点 --")
    view = show("视野中心 /view", "/view")
    show("选中单位 /selected_pos", "/selected_pos")
    show("光标 /cursor", "/cursor")
    show("自动挑 /spot", "/spot")

    print("\n-- /diag 摘要 --")
    for line in get("/diag").splitlines():
        if any(k in line for k in ("视野中心", "光标世界坐标", "选中单位坐标", "相机 抓到", "落点")):
            print("  " + line)

    if "--summon" in sys.argv:
        print("\n-- 真召唤一单（place=view）--")
        r = post("/summon?hash=0x911C7E98&count=1&place=view")
        print("  " + r)
        if view and view.get("ok") and "at=" in r:
            at = r.split("at=")[1].split(" via=")[0]
            print("  回执落点 at=%s  /view=(%.0f,%.0f,%.0f)" % (at, view["x"], view["y"], view["z"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
