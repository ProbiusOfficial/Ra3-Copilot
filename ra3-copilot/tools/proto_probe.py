"""只读探测：SpecialPowerTemplate 容器里有没有这个名字，以及它能不能被授予。

不看脚本返回值 —— PLAYER_GRANT_SPECIAL_POWER 对「查不到模板」「模板无授予对象」「已在列表」
三种情况都可能回 ret=1，返回值判不了成败。

判据链条（1.12 反汇编 + 本局实读）：
    0x585CB0 有两道门
        0x839C30(名字哈希) = 0x4A6BE0(容器 0x81D85EFA, 哈希, 0)
            查不到 → 抛 "Couldn't find specified special power template"
        查到 → eax = [[tpl+8]+0x134]，为 0 就 continue（静默跳过）
    容器读法：
        0x4A6050(this, &key) 里 this 取 0xCD7B5C+0x38*i 中 root 非空的那张
        head = node+0x58、root = [head+8]
        节点：left@0 right@4 parent@8 color@0xC key@0x10 value@0x14
        key = sage_hash(名字小写)、value = SpecialPowerTemplate*

用法（游戏进对局、DLL 已注入）：
    python ra3-copilot/tools/proto_probe.py
    python ra3-copilot/tools/proto_probe.py --dump      # 顺便列出容器全部 375 个名字
"""
import argparse
import json
import os
import re
import struct
import sys
import urllib.parse
import urllib.request

BASE = "http://127.0.0.1:17832"
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
TSV = os.path.join(HERE, "proto_zh.tsv")
OUT = os.path.join(ROOT, "research", "protocol-skills", "proto_probe.json")

sys.path.insert(0, os.path.join(ROOT, "unit-codes", "tools"))
from sage_hash import sage_hash  # noqa: E402

STORE_KEY = 0x81D85EFA
MAP_BASES = (0x00CD7B5C, 0x00CD7B94, 0x00CD7BCC)


def get(path):
    with urllib.request.urlopen(BASE + path, timeout=8) as r:
        return r.read().decode("utf-8", "replace")


def post(path):
    req = urllib.request.Request(BASE + path, method="POST")
    with urllib.request.urlopen(req, timeout=8) as r:
        return r.read().decode("utf-8", "replace")


def rd(addr, n=64):
    txt = get(f"/read?addr=0x{addr:08X}&n={n}")
    out = b""
    for line in txt.splitlines():
        m = re.match(r"^([0-9A-F]{8}): ((?:[0-9A-F]{2} )+)", line)
        if m:
            out += bytes.fromhex(m.group(2).replace(" ", ""))
    return out


def u32(addr):
    b = rd(addr, 16)
    return struct.unpack_from("<I", b, 0)[0] if len(b) >= 4 else 0


def find_store():
    """返回 (map_base, node, head, root)。"""
    s = get("/scratch?x=0&y=0&z=0&idx=0")
    a = int(re.search(r"@([0-9A-F]{8})", s).group(1), 16)
    post(f"/poke?addr=0x{a:08X}&type=u32&val=0x{STORE_KEY:08X}")
    for base in MAP_BASES:
        m = re.search(r"ret=([0-9A-F]{8})", get(
            f"/aicall?fn=0x4A6050&this=0x{base:08X}&a0=0x{a:08X}&na=1"))
        if not m:
            continue
        node = int(m.group(1), 16)
        if node < 0x1000:
            continue
        head = node + 0x58
        root = u32(head + 8)
        if root and root != head:
            return base, node, head, root
    raise RuntimeError("没找到 SpecialPowerTemplate 容器（容器键 0x81D85EFA）")


def walk(head, root):
    """返回 {hash: template_ptr}。"""
    d = {}
    stack = [root]
    while stack:
        n = stack.pop()
        if n == 0 or n == head:
            continue
        b = rd(n, 0x18)
        if len(b) < 0x18:
            continue
        left, right = struct.unpack_from("<II", b, 0)
        key, val = struct.unpack_from("<II", b, 0x10)
        d[key] = val
        if left and left != head:
            stack.append(left)
        if right and right != head:
            stack.append(right)
    return d


def variants(name):
    out = [name]
    if name.startswith("SpecialPower_"):
        out.append("SpecialPower" + name[13:])
    elif name.startswith("SpecialPower"):
        out.append("SpecialPower_" + name[12:])
    return out


def probe(name, table):
    for v in variants(name):
        tpl = table.get(sage_hash(v.lower()), 0)
        if tpl:
            data = u32(tpl + 8)
            grant = u32(data + 0x134) if data else 0
            return v, tpl, data, grant
    return None, 0, 0, 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", action="store_true", help="列出容器里全部条目")
    a = ap.parse_args()

    try:
        get("/ping")
    except Exception as e:
        print(f"接口不可达（{BASE}）：{e}")
        return 1

    base, node, head, root = find_store()
    table = walk(head, root)
    print(f"容器 map=0x{base:08X} node=0x{node:08X} 条目 {len(table)}\n")
    if a.dump:
        for k in sorted(table):
            print(f"  {k:08X} -> {table[k]:08X}")

    # 反查名字：容器按哈希存放，列出全部名字要扫 tsV + 其它已知名字表，这里只标已命中的
    rows = []
    for line in open(TSV, encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        p = line.rstrip("\n").split("\t")
        if len(p) >= 2:
            rows.append((p[0], p[1]))

    res = []
    for name, zh in rows:
        used, tpl, data, grant = probe(name, table)
        res.append({"name": name, "zh": zh, "used": used, "tpl": tpl,
                    "data": data, "grant": grant})

    notpl = [r for r in res if not r["used"]]
    nogrant = [r for r in res if r["used"] and not r["grant"]]
    ok = [r for r in res if r["used"] and r["grant"]]

    # 授予对象相同的分组
    by_grant = {}
    for r in ok:
        by_grant.setdefault(r["grant"], []).append(r["zh"])
    shared = {g: z for g, z in by_grant.items() if len(z) > 1}

    print(f"名字不在容器 {len(notpl)} / 无授予对象 {len(nogrant)} / 有对象 {len(ok)}\n")
    for r in notpl:
        print(f"  [无此模板] {r['name']:46s}{r['zh']}")
    for r in nogrant:
        print(f"  [无授予对象] {r['name']:46s}{r['zh']}  tpl={r['tpl']:08X}")
    for g, z in shared.items():
        print(f"  [共用对象 0x{g:08X}] " + " / ".join(z))

    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(res, f, ensure_ascii=False, indent=1)
    print(f"\n写入 {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
