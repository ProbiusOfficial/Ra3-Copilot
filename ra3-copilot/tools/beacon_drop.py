#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""信标雨：在战场随机位置投下折跃信标，落地后自己出单位。

落点以场上已有对象为地形锚点再偏移 —— 引擎不校验落点，随便写 x/y/z 会把信标
塞进山体或悬空；锚点取对象的坐标与高度，偏移一小段，基本都在同一层地面上。
锚点 z < 165 判为海面，用海面信标（走海军/空军/两栖池），否则投地面信标。

要「几秒内砸几十个」必须并发：变更端点要投递到渲染线程排队执行，串行发一条
要等上一条落地，间隔压到 0 也快不起来。
"""

import argparse
import json
import math
import random
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor

API = "http://127.0.0.1:17832"
BEACON_SEA = "0x38C0BC4A"    # 海面折跃信标：投下即播折跃动画，2~5 s 落成随机海军/空军/两栖
BEACON_LAND = "0x18DEEDB5"   # 地面折跃信标：落地即随机出地面单位
WATER_Z = 165                # 本图水面 z 93~132、陆地 210+；165 以上按陆地处理


def call(path, method="GET", timeout=20):
    req = urllib.request.Request(API + path, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read().decode("utf-8", "replace")
    try:
        return json.loads(raw)
    except ValueError:
        return {"raw": raw}


def anchors(limit=400, view_radius=0):
    """只用有效地形上的对象当锚点。名字为 ? 或 z<=5 的多是地图边界/占位对象
    （实测投过去得到 z=-1，信标落在虚空里）。
    view_radius>0 时只取视野中心该半径内的锚点，让信标全落在看得见的地方。"""
    data = call("/objects?limit=%d" % limit)
    out = []
    for o in data.get("objects", []):
        if not (o.get("x") or o.get("y")):
            continue
        if o.get("name") in (None, "", "?"):
            continue
        if not (60 <= o.get("z", 0) <= 480):
            continue
        out.append(o)
    if view_radius > 0:
        view = call("/view")
        if isinstance(view, dict) and view.get("ok"):
            cx, cy = view["x"], view["y"]
            near = [o for o in out if (o["x"] - cx) ** 2 + (o["y"] - cy) ** 2 <= view_radius ** 2]
            if near:
                return near
            print("视野 %.0f 米内没有可作锚点的对象，退回全图" % view_radius)
    return out


def pick_points(pool, count, spread, sea_ratio):
    pts = []
    for _ in range(count):
        sea_anchors = [a for a in pool if a["z"] < WATER_Z]
        land_anchors = [a for a in pool if a["z"] >= WATER_Z]
        use_sea = sea_anchors and random.random() < sea_ratio
        anchor = random.choice(sea_anchors if use_sea else land_anchors or pool)
        ang = random.uniform(0, math.tau)
        r = random.uniform(spread * 0.25, spread)
        x = round(anchor["x"] + r * math.cos(ang), 1)
        y = round(anchor["y"] + r * math.sin(ang), 1)
        z = round(anchor["z"] + random.uniform(-1.5, 1.5), 1)
        h = BEACON_SEA if z < WATER_Z else BEACON_LAND
        pts.append((h, x, y, z, anchor.get("name") or "?"))
    return pts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=8)
    ap.add_argument("--spread", type=float, default=320, help="离锚点的最大偏移")
    ap.add_argument("--view-radius", type=float, default=0, help=">0 时只在视野中心该半径内撒")
    ap.add_argument("--delay", type=float, default=0, help="串行模式的间隔秒数")
    ap.add_argument("--threads", type=int, default=1, help="并发数；>1 时忽略 --delay")
    ap.add_argument("--sea-ratio", type=float, default=0.35)
    args = ap.parse_args()

    pool = anchors(view_radius=args.view_radius)
    if not pool:
        print("场上没有任何对象可作锚点")
        return

    pts = pick_points(pool, args.count, args.spread, args.sea_ratio)
    t0 = time.time()
    ok = 0

    def fire(job):
        h, x, y, z, _ = job
        try:
            res = call("/summon?hash=%s&x=%s&y=%s&z=%s" % (h, x, y, z), "POST", timeout=15)
        except Exception as exc:
            return False, {"err": str(exc)}
        # 变更端点回的是纯文本 "summon=1 hash=.. objs=[..]"，不是 JSON
        raw = res.get("raw", "") if isinstance(res, dict) else ""
        return (isinstance(res, dict) and res.get("summon") == 1) or raw.strip().startswith("summon=1"), res

    if args.threads > 1:
        with ThreadPoolExecutor(max_workers=args.threads) as ex:
            results = list(ex.map(fire, pts))
    else:
        results = []
        for job in pts:
            results.append(fire(job))
            time.sleep(args.delay)

    for (h, x, y, z, an), (good, res) in zip(pts, results):
        ok += 1 if good else 0
        if not good:
            print("失败 (%s,%s,%s) 锚点=%s %s" % (x, y, z, an, res), flush=True)

    print("投放 %d/%d 成功，耗时 %.2f 秒，视野半径 %.0f，落点区间 x[%.0f,%.0f] y[%.0f,%.0f]" % (
        ok, len(pts), time.time() - t0, args.view_radius,
        min(p[1] for p in pts), max(p[1] for p in pts),
        min(p[2] for p in pts), max(p[2] for p in pts)))


if __name__ == "__main__":
    main()
