#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""战役战场导演：按节拍给我方投送增援、压减敌方兵力、随机投放折跃信标。

落点取我方部队质心附近，不用「视野中心」——镜头随战斗漂移，
视野中心可能离部队一两千米，援军就投到敌人堆里白给了。
"""

import argparse
import json
import math
import random
import time
import urllib.request

API = "http://127.0.0.1:17832"
KILLUNIT = 0x79EA50          # KillUnit(ecx=Thing, 栈参 6/19/0)；传 Body 会静默无效
STRUCTURE_CORE = ("核心", "建造基地", "基地车")

POOLS = ["infantry", "infantry", "infantry", "vehicle", "vehicle", "air"]


def call(path, method="GET", timeout=20):
    req = urllib.request.Request(API + path, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read().decode("utf-8", "replace")
    try:
        return json.loads(raw)
    except ValueError:
        return {"raw": raw}


def units_of(owner):
    data = call("/objects?owner=%d&cat=unit&limit=200" % owner)
    return data.get("objects", []) if isinstance(data, dict) else []


def player_side(owner=1):
    """玩家阵营只能由人指定：`/objects?cat=structure` 的类别过滤并不可靠
    （实测会返回一堆召唤出来的载具），而召唤物带的是「兵种原阵营」。
    自动猜测一旦猜成敌方，压减就会清自己的部队 —— 所以判定失败时宁可不压减。"""
    return None


def hostiles(allowed_sides):
    """返回（敌方 owner 列表, 敌方单位总数）。
    `allowed_sides` 是硬白名单：只压减这些阵营，同阵营 NPC 是友军不碰。
    宁可少杀，也不做「除了我都杀」——判错阵营会直接清掉盟友。"""
    snap = call("/snapshot")
    idx, total = [], 0
    for e in snap.get("enemies", []):
        if e.get("idx") == 1 or e.get("units", 0) <= 0:
            continue
        if allowed_sides and e.get("side") not in allowed_sides:
            continue
        idx.append(e["idx"])
        total += e["units"]
    return idx, total


def my_army():
    army = units_of(1)
    return [u for u in army if u.get("hp", 0) > 0]


def drop_point(army, spread=260):
    """锚点取离视野中心最近的我方单位：基地与前线可能相隔两三千米，
    随机挑单位会让援军落到玩家没在看的另一头。"""
    view = call("/view")
    if isinstance(view, dict) and view.get("ok"):
        vx, vy = view["x"], view["y"]
        base = min(army, key=lambda u: (u["x"] - vx) ** 2 + (u["y"] - vy) ** 2)
    else:
        base = army[0]
    ang = random.uniform(0, math.tau)
    r = random.uniform(spread * 0.5, spread)
    return round(base["x"] + r * math.cos(ang), 1), round(base["y"] + r * math.sin(ang), 1), base["z"]


def summon(pool, count, pt):
    x, y, z = pt
    q = "/summon?pool=%s&count=%d&x=%s&y=%s&z=%s" % (pool, count, x, y, z)
    return call(q, "POST")


def drain_enemy(hostile_idx, army, horizon=900):
    """抽掉一个远离我方部队的普通战斗单位；建筑/核心/高血量英雄不动。"""
    cx = sum(u["x"] for u in army) / len(army)
    cy = sum(u["y"] for u in army) / len(army)
    cands = []
    for owner in hostile_idx:
        for u in units_of(owner):
            name = u.get("name") or ""
            if u.get("hp", 0) <= 0 or u.get("hp", 0) > 1200:
                continue
            if any(k in name for k in STRUCTURE_CORE):
                continue
            dx, dy = u["x"] - cx, u["y"] - cy
            if (dx * dx + dy * dy) ** 0.5 < horizon:
                continue
            cands.append(u)
    if not cands:
        return None
    tgt = random.choice(cands)
    call("/aicall?fn=0x%X&this=%s&a0=6&a1=19&a2=0&na=3" % (KILLUNIT, tgt["thing"]), "POST")
    return tgt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=float, default=45)
    ap.add_argument("--interval", type=float, default=45, help="节拍秒数")
    ap.add_argument("--max-army", type=int, default=10, help="我方单位数达到此值后停止增援")
    ap.add_argument("--enemy-cap", type=int, default=26, help="敌方单位总数超过此值才压减")
    ap.add_argument("--enemy-sides", default="帝国,盟军", help="允许压减的阵营白名单（逗号分隔）")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    def say(msg):
        print("%s %s" % (time.strftime("%H:%M:%S"), msg), flush=True)

    deadline = time.time() + args.minutes * 60
    next_beacon = time.time() + random.uniform(120, 200)
    me = player_side() or "苏联"
    allowed = [s for s in args.enemy_sides.split(",") if s and s != me]
    wave = 0
    say("我方阵营=%s，压减白名单=%s" % (me, "/".join(allowed) or "（空，只投援军）"))

    while time.time() < deadline:
        try:
            army = my_army()
            if not army:
                say("[等] 我方暂无单位（还没开局？）")
                time.sleep(args.interval)
                continue

            n = len(army)
            if n < args.max_army:
                wave += 1
                size = 1 if n >= args.max_army - 3 else random.choice((1, 2, 2, 3))
                pool = random.choice(POOLS if n < args.max_army - 4 else ["infantry"])
                pt = drop_point(army)
                r = summon(pool, size, pt)
                say("[援军 #%d] %s x%d -> %s  回执=%s" % (wave, pool, size, (pt[0], pt[1]), r.get("raw") or r))

            host, total = hostiles(allowed)
            if total > args.enemy_cap and random.random() < 0.55:
                killed = drain_enemy(host, army)
                if killed:
                    say("[压减] %s @%.0f,%.0f 已清除（敌方 %d）" % (killed["name"], killed["x"], killed["y"], total - 1))

            if time.time() >= next_beacon:
                call("/crate?idx=55&count=1", "POST")
                say("[信标] 地面折跃信标已投下")
                next_beacon = time.time() + random.uniform(180, 300)

        except Exception as exc:                      # 对局切换/结算期接口会短暂失败，继续等
            say("[跳过] %s" % exc)

        time.sleep(args.interval + random.uniform(-8, 8))


if __name__ == "__main__":
    main()
