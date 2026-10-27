# -*- coding: utf-8 -*-
"""定位 Steam 版里 actions.cpp 剩余几个硬编码地址（全局变量用引用指纹，函数用入口字节）。"""
import struct, collections, sys

RET = r'C:\GAME\Red Alert 3\Data\ra3_1.12.game'
STM = r'C:\Program Files (x86)\Steam\steamapps\common\Command and Conquer Red Alert 3\Data\ra3_1.12.game'
BASE = 0x400000

A = open(RET, 'rb').read()
B = open(STM, 'rb').read()

# ---- 引用指纹：同一全局在不同构建里被引用次数一致 ----
OPS = [b'\x8b\x0d', b'\xa1', b'\xa3', b'\x8b\x15', b'\x89\x15', b'\x8b\x1d',
       b'\x89\x1d', b'\x8b\x05', b'\x89\x05', b'\x89\x0d']

def fp(d):
    tabs = [collections.Counter() for _ in OPS]
    for i in range(len(d) - 6):
        for k, op in enumerate(OPS):
            if d[i:i + len(op)] == op:
                v = struct.unpack_from('<I', d, i + len(op))[0]
                if BASE <= v < BASE + 0x1000000:
                    tabs[k][v] += 1
                break
    addrs = set()
    for t in tabs:
        addrs |= set(t)
    return {a: tuple(t[a] for t in tabs) for a in addrs}

fa, fb = fp(A), fp(B)
inv = collections.defaultdict(list)
for a, v in fb.items():
    inv[v].append(a)

TARGETS = {
    'R_CURSORPOS':   0x8DAEFC,
    'R_CURTEMPLATE': 0x8E9838,
}
print("=== 全局变量（引用指纹配对）===")
for name, rva in TARGETS.items():
    va = BASE + rva
    v = fa.get(va)
    if v is None:
        print("  %-14s 零售 0x%X  无引用记录" % (name, va))
        continue
    cands = inv.get(v, [])
    print("  %-14s 零售 0x%X  指纹=%s" % (name, va, v))
    print("        Steam 候选: %s" % ([hex(c - BASE) for c in cands] or "无"))

# ---- 函数入口：用零售前若干字节做长签名 ----
def find_func(name, rva, maxlen=64):
    for n in (maxlen, 48, 40, 32, 24, 20, 16, 14, 12, 10, 8):
        sub = A[rva:rva + n]
        hits = []
        i = 0
        while True:
            i = B.find(sub, i)
            if i < 0:
                break
            hits.append(i)
            i += 1
        if len(hits) == 1:
            print("  %-10s 零售 0x%06X -> Steam 0x%06X (%d 字节唯一)" % (name, rva, hits[0], n))
            return hits[0]
        if n == 8:
            print("  %-10s 零售 0x%06X -> 8 字节仍 %d 处: %s" % (name, rva, len(hits), [hex(h) for h in hits[:5]]))
    return None

print("\n=== 函数（入口字节唯一命中）===")
print("零售 R_POSTA 0x44A0B0 前 16: %s" % A[0x44A0B0:0x44A0C0].hex())
print("零售 R_POSTB 0x438AB0 前 16: %s" % A[0x438AB0:0x438AC0].hex())
find_func('R_POSTA', 0x44A0B0)
find_func('R_POSTB', 0x438AB0)
