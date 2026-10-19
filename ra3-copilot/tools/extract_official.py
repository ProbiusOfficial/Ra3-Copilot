#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从日冕（Corona）.lyi 解包数据里抽官方中文名/描述，刷新 tools/proto_zh.tsv。

数据来源（按优先级）：
  1. <data>/gamestrings.csf   —— 游戏真正的字符串表（10928 条）
  2. <data>/mod.relo          —— 模组自己的可读字符串表（UTF-8 明文，2516 条），CSF 的子集

⚠ CSF 的值不是明文：是 **UTF-16LE 逐 16 位取反（^ 0xFFFF）**。
   例：NAME:CelestialPowerSealOff 原始 ca8a 027f 4c81 de72 → '电能纳贡'
   验证过 DESC 与游戏内提示框逐字一致（2026-10-19）。

用法：
  python tools/extract_official.py                     # 用默认解包目录
  python tools/extract_official.py --data <dir>        # 指定目录
  python tools/extract_official.py --report            # 只报告覆盖率，不写文件
"""
import argparse
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TSV = os.path.join(HERE, "proto_zh.tsv")
DEFAULT_DATA = os.path.join(HERE, "..", "..", "research", "lyi", "out",
                            "corona_3.258", "data")

# 内部名和字符串表键对不上的，手工补别名（左：SpecialPower 去掉前缀后的短名）
ALIASES = {
    "CelestiallightningTroopUpgrade_Lv1": "CelestialLightningTroop_RANK1",
    "CelestiallightningTroopUpgrade_Lv2": "CelestialLightningTroop_RANK2",
    "CelestiallightningTroopUpgrade_Lv3": "CelestialLightningTroop_RANK3",
    "CelestialCenturionUpgrade":          "CelestialCenturion",
    "ChronoRiftSelectLvl1":               "PLAYERPOWERCHRONORIFTLVL1",
    "ChronoRiftSelectLvl2":               "PLAYERPOWERCHRONORIFTLVL2",
    "ChronoRiftSelectLvl3":               "PLAYERPOWERCHRONORIFTLVL3",
    "ChronosphereObjectSelect":           "PLAYERPOWERCHRONOSPHERE",
    "ChronoSwapSelect":                   "PLAYERPOWERCHRONOSWAP",
    "Pause01":                            "CelestialPlayerPowerPause",
    "SatelliteSweep":                     "PLAYERPOWERSATELLITE",
}


def parse_csf(path):
    """返回 {key: value}；key 形如 NAME:Foo / DESC:Foo。"""
    b = open(path, "rb").read()
    if b[:4] != b" FSC":
        raise ValueError("不是 CSF 文件：%s" % path)
    out, pos, n = {}, 20, len(b)
    while pos < n - 12:
        if b[pos:pos + 4] != b" LBL":
            pos += 1
            continue
        nlen = struct.unpack_from("<I", b, pos + 8)[0]
        name = b[pos + 12:pos + 12 + nlen].decode("latin1")
        q = pos + 12 + nlen
        if b[q:q + 4] != b" RTS":
            pos += 1
            continue
        vlen = struct.unpack_from("<I", b, q + 4)[0]
        raw = b[q + 8:q + 8 + vlen * 2]
        s = "".join(chr((~struct.unpack_from("<H", raw, i)[0]) & 0xFFFF)
                    for i in range(0, len(raw), 2))
        out[name] = s
        pos = q + 8 + vlen * 2
    return out


def parse_relo(path):
    t = open(path, "rb").read().decode("utf-8", "replace")
    pat = re.compile(r'^(NAME|DESC):([A-Za-z0-9_]+)\r?\n\t"(.*?)"\r?\nEnd',
                     re.M | re.S)
    return {m.group(1) + ":" + m.group(2): m.group(3) for m in pat.finditer(t)}


def norm(s):
    s = s.lower().replace("_", "").replace(" ", "")
    for p in ("playerpower", "specialpower", "purchase", "button"):
        if s.startswith(p):
            s = s[len(p):]
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=os.path.normpath(DEFAULT_DATA))
    ap.add_argument("--report", action="store_true")
    a = ap.parse_args()

    csf_p = os.path.join(a.data, "gamestrings.csf")
    relo_p = os.path.join(a.data, "mod.relo")
    if not os.path.isfile(csf_p):
        sys.exit("找不到 %s —— 先用 research/lyi/lyi.py 解包" % csf_p)

    table = parse_relo(relo_p) if os.path.isfile(relo_p) else {}
    table.update(parse_csf(csf_p))          # CSF 优先

    names = {k[5:]: v for k, v in table.items() if k.startswith("NAME:")}
    descs = {k[5:]: v for k, v in table.items() if k.startswith("DESC:")}

    idx = {}
    for k, v in names.items():
        idx.setdefault(norm(k), (k, v))

    rows = []
    for ln in open(TSV, encoding="utf-8-sig").read().splitlines():
        if not ln or ln.startswith("#"):
            continue
        parts = ln.split("\t")
        if len(parts) < 3:
            continue
        rows.append(parts[:3])

    out, hit, miss = [], 0, []
    for full, zh, fac in rows:
        short = full.replace("SpecialPower_", "").replace("SpecialPower", "")
        keys = [ALIASES.get(short, short), short, full]
        got = None
        for c in keys:
            got = idx.get(norm(c))
            if got:
                break
        if got:
            key, official = got
            # 描述可能挂在别名去掉等级后缀的键上（如 CelestialLightningTroop）
            desc = descs.get(key)
            if desc is None:
                base = re.sub(r"(_RANK\d|_Lvl\d|_Lv\d|L\d)$", "", key, flags=re.I)
                desc = descs.get(base, "")
            out.append((full, official, fac, desc))
            hit += 1
        else:
            out.append((full, zh, fac, ""))
            miss.append((full, zh))

    print("CSF 条目 %d（NAME %d / DESC %d），可读表 %d"
          % (len(table), len(names), len(descs), len(table)))
    print("官方名命中 %d / %d（%.0f%%）" % (hit, len(rows),
                                          100.0 * hit / max(1, len(rows))))
    if miss:
        print("未命中（保留手写名，游戏里没有对应条目）：")
        for f, z in miss:
            print("   %-50s %s" % (f, z))
    if a.report:
        return

    hdr = [
        "# 协议技能中文名 —— 由 tools/extract_official.py 从日冕解包数据自动生成",
        "# 列：内部名 <TAB> 官方中文名 <TAB> 阵营 <TAB> 官方描述",
        "# 来源：gamestrings.csf（UTF-16LE ^ 0xFFFF）+ mod.relo；未命中的条目保留手写译名",
        "# 只收「协议/玩家技能」——单位能力不进这张表（引擎也会拒绝授予）。",
        "# 手工改本文件会被下次生成覆盖；译名有误请改脚本里的 ALIASES 或数据源。",
    ]
    with open(TSV, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(hdr) + "\n")
        for full, name, fac, desc in out:
            desc = desc.replace("\r", " ").replace("\n", " ").replace("\t", " ")
            f.write("%s\t%s\t%s\t%s\n" % (full, name, fac, desc))
    print("已写 %s（%d 条，含描述 %d 条）"
          % (TSV, len(out), sum(1 for r in out if r[3])))


if __name__ == "__main__":
    main()
