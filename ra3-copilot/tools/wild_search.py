# -*- coding: utf-8 -*-
"""通配签名搜索：零售版把绝对地址挖成 ?? 当签名，到 Steam 版找同构点并读出实际地址。"""
import re, struct, sys

RET = r'C:\GAME\Red Alert 3\Data\ra3_1.12.game'
STM = r'C:\Program Files (x86)\Steam\steamapps\common\Command and Conquer Red Alert 3\Data\ra3_1.12.game'
A = open(RET, 'rb').read()
B = open(STM, 'rb').read()

WILD = None  # 单个未知字节


def parse(pat):
    """'A1 ?? ?? ?? ?? 85 C0' -> list[int|None]"""
    out = []
    for t in pat.split():
        out.append(None if t in ('??', '?', '**') else int(t, 16))
    return out


def search(data, pat):
    n = len(pat)
    hits = []
    for i in range(len(data) - n + 1):
        ok = True
        for k, b in enumerate(pat):
            if b is not None and data[i + k] != b:
                ok = False
                break
        if ok:
            hits.append(i)
    return hits


def probe(name, pat, addr_offsets):
    """addr_offsets: 相对匹配起点的偏移列表，读出其中的 u32 值"""
    p = parse(pat)
    hits = search(B, p)
    print("%-16s %s -> %d 处" % (name, pat, len(hits)))
    for h in hits[:6]:
        vals = [hex(struct.unpack_from('<I', B, h + o)[0]) for o in addr_offsets]
        print("    Steam 0x%06X  内含地址: %s" % (h, vals))
    return hits


print("=== R_CURTEMPLATE ===")
# 写点：mov eax,[esp+4]; mov [A],eax; mov [B],ecx; ret
probe('CT_w', '4C 24 04 A3 ?? ?? ?? ?? 89 0D ?? ?? ?? ?? C3', [5, 11])
# 读点：mov eax,[CT]; test eax,eax; jz; mov ecx,[ThingFactory]
probe('CT_r', 'A1 ?? ?? ?? ?? 85 C0 74 4D 8B 0D ?? ?? ?? ??', [1, 11])

print("\n=== R_CURSORPOS ===")
# mov edx,[0xCDAEF0]; mov ecx,[0xCDAEFC]; push edx; push ebx; push esi; push edi; call
probe('CP_r1', '8B 15 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 52 53 56 57 E8', [2, 8])
# mov ecx,[CP]; test ecx,ecx; jz +7; push 1; call
probe('CP_r2', '8B 0D ?? ?? ?? ?? 85 C9 74 07 6A 01 E8', [2])

print("\n=== R_POSTB ===")
# push esi; mov esi,ecx; mov ecx,[ScriptEngine]; push edi; call ...
probe('POSTB', '56 8B F1 8B 0D ?? ?? ?? ?? 57 E8', [5])
