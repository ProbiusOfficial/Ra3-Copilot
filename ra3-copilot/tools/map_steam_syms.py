#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
map_steam_syms.py - 把 symres.cpp 的符号签名搬到 Steam 版 exe 上验证

原理：symres 的 SIG 尾部常带绝对地址/rel32（如 SIG_KILL 末尾的 0xBE9508），
在 Steam 版里必然变。做法是逐级截短签名直到唯一命中，再用指令指纹确认
语义一致（同一函数），最后给出 Steam 版的 RVA。

无签名的两个（SetStatus / RemoveMod）改用"零售 RVA 处字节 + 指纹"定位。
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

RETA = r"C:\GAME\Red Alert 3\Data\ra3_1.12.game"
STEAM = (r"C:\Program Files (x86)\Steam\steamapps\common"
         r"\Command and Conquer Red Alert 3\Data\ra3_1.12.game")
BASE = 0x400000
SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "symres.cpp")

A = open(RETA, "rb").read()
B = open(STEAM, "rb").read()
md = Cs(CS_ARCH_X86, CS_MODE_32)

# ---- 从 symres.cpp 抽 SIG_*/MSK_* 与 R_* 常量 ----
txt = open(SRC, "r", encoding="utf-8", errors="ignore").read()
sigs = {}
for m in re.finditer(r"const uint8_t (\w+)\[\]\s*=\s*\{([^}]*)\}", txt):
    sigs[m.group(1)] = bytes(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(2)))
rva_consts = {m.group(1): int(m.group(2), 16)
              for m in re.finditer(r"constexpr uint32_t (R_\w+)\s*=\s*(0x[0-9A-Fa-f]+);", txt)}

# 符号 → (SIG 名, 零售 R_* 常量名)
PLAN = [
    ("CreateUnit",  "SIG_CREATE",     "R_CREATEUNIT"),
    ("LevelUp",     "SIG_LEVELUP",    "R_LEVELUP"),
    ("KillUnit",    "SIG_KILL",       "R_KILLUNIT"),
    ("TmplFind",    "SIG_FIND",       "R_TMPLFIND"),
    ("MouseMap",    "SIG_MOUSE",      "R_MOUSEMAP"),
    ("FreeBuild",   "SIG_FREE",       "R_FREEBUILD"),
    ("DrawSetMC",   "SIG_DRAWSETMC",  "R_DRAWSETMC"),
    ("DrawSetTint", "SIG_DRAWSETTINT", "R_DRAWSETTINT"),
    ("ApplyMod",    "SIG_APPLYMOD",   "R_APPLYMOD"),
    ("ModFind",     "SIG_MODFIND",    "R_MODFIND"),
    ("SetStatus",   None,             "R_SETSTATUS"),
    ("RemoveMod",   None,             "R_REMOVEMOD"),
]


def insn_sig(ins):
    sig = ins.mnemonic
    for mem in re.findall(r"\[([^\]]*)\]", ins.op_str):
        keep = [p.strip() for p in mem.split("+")
                if not re.fullmatch(r"0x[0-9a-f]+|\d+", p.strip())]
        sig += " [" + "+".join(keep) + "]"
    return sig


def fp(data, rva, n=14):
    out = []
    for ins in md.disasm(data[rva:rva + 256], BASE + rva):
        out.append(insn_sig(ins))
        if len(out) >= n:
            break
    return out


def match_len(x, y):
    k = 0
    for a, b in zip(x, y):
        if a != b:
            break
        k += 1
    return k


def find_shortest_unique(sig):
    """逐级截短签名，找第一个唯一命中的长度（优先长签名 = 更可信）。"""
    n = len(sig)
    for cut in range(n, 0, -1):
        sub = sig[:cut]
        hits = []
        i = 0
        while True:
            i = B.find(sub, i)
            if i < 0:
                break
            hits.append(i)
            i += 1
        if len(hits) == 1:
            return sub, hits, cut
        if len(hits) == 0:
            return None, [], cut
    return None, [], 0


print("%-13s %-9s %-9s %-6s %s" % ("symbol", "reta", "steam", "置信", "说明"))
print("-" * 76)
result = {}
for name, signame, rconst in PLAN:
    reta_rva = rva_consts.get(rconst, 0)
    fp_a = fp(A, reta_rva)

    if signame and signame in sigs:
        sig = sigs[signame]
        sub, hits, cut = find_shortest_unique(sig)
        if not hits:
            print("%-13s 0x%06X  %-9s %-6s 签名零命中（全长度试过）" % (name, reta_rva, "-", "-"))
            continue
        h = hits[0]
        score = match_len(fp_a, fp(B, h))
        note = "签名截到 %d/%d 字节唯一命中" % (cut, len(sig))
        good = score >= 8
        print("%-13s 0x%06X  0x%06X  %-6s %s / 指纹 %d/14" %
              (name, reta_rva, h, "OK" if good else "存疑", note, score))
        result[name] = (h, good)
    else:
        # 无签名：用零售 RVA 处 24 字节找，多命中用指纹挑
        probe = A[reta_rva:reta_rva + 24]
        hits = []
        i = 0
        while True:
            i = B.find(probe, i)
            if i < 0:
                break
            hits.append(i)
            i += 1
        best, bestsc = None, 0
        for h in hits:
            sc = match_len(fp_a, fp(B, h))
            if sc > bestsc:
                best, bestsc = h, sc
        if best is None:
            # 退一步：用 16 字节
            probe = A[reta_rva:reta_rva + 16]
            i = 0
            while True:
                i = B.find(probe, i)
                if i < 0:
                    break
                hits.append(i)
                i += 1
            for h in hits:
                sc = match_len(fp_a, fp(B, h))
                if sc > bestsc:
                    best, bestsc = h, sc
        if best is None:
            print("%-13s 0x%06X  %-9s %-6s 无签名且字节不中" % (name, reta_rva, "-", "-"))
        else:
            good = bestsc >= 8
            print("%-13s 0x%06X  0x%06X  %-6s 无签名/字节定位 %d 处 / 指纹 %d/14" %
                  (name, reta_rva, best, "OK" if good else "存疑", len(hits), bestsc))
            result[name] = (best, good)

print("-" * 76)
print("Steam 版符号 RVA（VA - 0x400000）：")
for name, (rva, good) in result.items():
    print("    %-13s RVA 0x%06X   VA 0x%08X  %s" %
          (name, rva, rva + BASE, "" if good else " ← 存疑，需人工复核"))
