#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
map_steam_rva.py - 为 Steam 版 ra3_1.12.game 重建 hook 地址表（分析用）

背景：Steam 版（TUC 1.13 更新包）自带的 Data\\ra3_1.12.game 是**重新编译**的二进制，
版本号同为 1.12.3444.25830，但代码布局整体错位 —— 30 个 hook 签名在原 RVA 上全部
不匹配（偏移差在 0x3BD60~0x3F060 之间浮动，不是整体平移）。

做法：
  1. 从 features.cpp 抓 kHooks（name / rva / AOB / replayLen）
  2. 以零售版 exe 的 rva 为锚，用 capstone 取一串指令的「助记符 + 内存操作数寄存器/偏移」
     指纹（丢掉立即数与绝对地址，避免布局差异干扰）
  3. 在 Steam 版里用 AOB 找出全部候选，逐个比对指纹，取匹配最长者
  4. 校验 replayLen 是否落在指令边界（跳转必须重放整条指令，错位 = 崩）

用法：
  python tools/map_steam_rva.py
  python tools/map_steam_rva.py --reta <exe> --steam <exe> --verbose
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from precheck_aob import parse_pe, parse_src, DEFAULT_SRC

try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
except ImportError:
    print("[x] 需要 capstone：pip install capstone")
    sys.exit(2)

RETA_DEFAULT = r"C:\GAME\Red Alert 3\Data\ra3_1.12.game"
STEAM_DEFAULT = (r"C:\Program Files (x86)\Steam\steamapps\common"
                 r"\Command and Conquer Red Alert 3\Data\ra3_1.12.game")
BASE = 0x400000
CTX_INSNS = 12          # 指纹长度（指令条数）
MIN_MATCH = 6           # 至少连续匹配这么多条才算同一处代码


def parse_hooks_full(src_path):
    """抓 name / rva / aob 名 / replayLen。kHooks 行尾最后一个数字是 replayLen。"""
    txt = open(src_path, "r", encoding="utf-8", errors="ignore").read()
    aobs = {}
    for m in re.finditer(r"const\s+uint8_t\s+(AOB_\w+)\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}", txt):
        vals = re.findall(r"0x[0-9A-Fa-f]{1,2}", m.group(2))
        aobs[m.group(1)] = bytes(int(v, 16) & 0xFF for v in vals)
    hooks = []
    for line in txt.splitlines():
        m = re.match(r'\s*\{\s*"(\w+)"\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*(0x[0-9A-Fa-f]+)\s*,'
                     r'\s*(AOB_\w+)\s*,\s*\d+\s*,.*?,\s*(\d+)\s*\}', line)
        if m:
            hooks.append((m.group(1), int(m.group(2), 16), m.group(4), int(m.group(5))))
    return aobs, hooks


def insn_sig(ins):
    """助记符 + 内存操作数(寄存器与固定偏移)，丢掉立即数与绝对地址。"""
    sig = ins.mnemonic
    mems = re.findall(r"\[([^\]]*)\]", ins.op_str)
    for mem in mems:
        parts = [p.strip() for p in mem.split("+")]
        keep = [p for p in parts if not re.fullmatch(r"0x[0-9a-f]+|\d+", p)]
        sig += " [" + "+".join(keep) + "]"
    return sig


def fingerprint(md, data, rva, n=CTX_INSNS):
    """从 rva 起取 n 条指令的 (rva, size, sig, raw)。"""
    out = []
    if rva < 0 or rva >= len(data):
        return out
    for ins in md.disasm(data[rva:rva + 128], BASE + rva):
        out.append((ins.address - BASE, ins.size, insn_sig(ins), bytes(ins.bytes)))
        if len(out) >= n:
            break
    return out


def boundary_ok(seq, want):
    """从起点累积指令长度，能否恰好等于 want。"""
    total = 0
    for _, sz, _, _ in seq:
        total += sz
        if total == want:
            return True
        if total > want:
            return False
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reta", default=RETA_DEFAULT)
    ap.add_argument("--steam", default=STEAM_DEFAULT)
    ap.add_argument("--src", default=os.path.normpath(DEFAULT_SRC))
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    for p in (args.reta, args.steam):
        if not os.path.exists(p):
            print("[x] 不存在: %s" % p)
            return 2

    A = open(args.reta, "rb").read()
    B = open(args.steam, "rb").read()
    aobs, hooks = parse_hooks_full(args.src)
    md = Cs(CS_ARCH_X86, CS_MODE_32)

    print("零售 : %s" % args.reta)
    print("Steam: %s" % args.steam)
    print("抓到 hook %d 个（含 replayLen）" % len(hooks))
    print()
    print("%-15s %-9s %-9s %-5s %-7s %s" % ("hook", "reta", "steam", "replay", "边界", "候选/匹配"))
    print("-" * 82)

    good, bad, miss = [], [], []
    for name, rva, aobname, replay in hooks:
        aob = aobs.get(aobname)
        if not aob:
            miss.append((name, "无 AOB 定义"))
            continue
        seq_a = fingerprint(md, A, rva)
        sig_a = [s[2] for s in seq_a]

        hits = []
        start = 0
        while True:
            i = B.find(aob, start)
            if i < 0:
                break
            hits.append(i)
            start = i + 1

        best, best_score = None, 0
        for h in hits:
            seq_b = fingerprint(md, B, h)
            sig_b = [s[2] for s in seq_b]
            score = 0
            for x, y in zip(sig_a, sig_b):
                if x != y:
                    break
                score += 1
            if score > best_score:
                best, best_score = h, score

        if best is None:
            print("%-15s 0x%06X  %-9s %-5d %-7s 0 处候选" % (name, rva, "-", replay, "-"))
            miss.append((name, "AOB 零命中（签名含绝对地址/rel32）"))
            continue

        seq_b = fingerprint(md, B, best)
        bok = boundary_ok(seq_b, replay)
        trust = "确认" if best_score >= MIN_MATCH else "存疑(%d条)" % best_score
        print("%-15s 0x%06X  0x%06X  %-5d %-7s %d 处候选 / %s" %
              (name, rva, best, replay, "OK" if bok else "错位", len(hits), trust))
        if args.verbose and len(hits) > 1:
            for h in hits:
                sq = fingerprint(md, B, h)
                sc = 0
                for x, y in zip(sig_a, [s[2] for s in sq]):
                    if x != y:
                        break
                    sc += 1
                print("      cand 0x%06X  match=%d  %s" %
                      (h, sc, " | ".join(s[2] for s in sq[:4])))

        if bok and best_score >= MIN_MATCH:
            good.append((name, best, replay))
        elif best_score >= MIN_MATCH:
            bad.append((name, best, replay, "边界错位"))
        else:
            bad.append((name, best, replay, "指纹存疑 %d 条" % best_score))

    print("-" * 82)
    print("确认 %d / 边界错位或存疑 %d / 未定位 %d" % (len(good), len(bad), len(miss)))
    if bad:
        print("\n需人工处理：")
        for name, rva, replay, why in bad:
            print("    %-15s 0x%06X replay=%d  %s" % (name, rva, replay, why))
    if miss:
        print("\n零命中（需先解出引用目标再构造签名）：")
        for name, why in miss:
            print("    %-15s %s" % (name, why))

    print("\n===== Steam 版 kHooks RVA（可直接抄） =====")
    for name, rva, replay in good:
        print('    { "%s", 0x%06X, AOB_%s, %d, ..., %d }' %
              (name, rva, name.upper(), 0, replay))
    return 0


if __name__ == "__main__":
    sys.exit(main())
