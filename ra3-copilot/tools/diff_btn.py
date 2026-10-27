# -*- coding: utf-8 -*-
"""只读：对比普通按钮 vs 扩展按钮的 def/sub 结构；读 CommandSet 描述符全貌。
用法: python tools/diff_btn.py
"""
import sys
sys.path.insert(0, "C:/Project/RA3/ra3-copilot/tools")
from inspect_cs import rd, u32, hit

def hexdump(addr, n, base=None, title=""):
    b = rd(addr, n)
    if title:
        print(title)
    for off in range(0, len(b), 16):
        chunk = b[off:off + 16]
        hexs = " ".join("%02X" % c for c in chunk)
        asc = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
        tags = []
        # 标注像指针的 dword
        for k in range(0, len(chunk) - 3, 4):
            v = int.from_bytes(chunk[k:k + 4], "little")
            if 0x00400000 <= v < 0x10000000:
                tags.append("+%02X->%08X" % (off + k, v))
        print("  %08X +%02X  %-47s |%s|  %s" % (addr + off, off, hexs, asc, " ".join(tags)))

# ---------- 1. 描述符全貌 ----------
print("########## 描述符对比（含扩展 vs 不含） ##########")
for cs, label in ((0x09EDDE20, "含扩展 grp0 count=17"), (0x0DD7F920, "不含 grp0 count=15")):
    inner = u32(cs + 4)
    desc = u32(inner)
    print("\n--- %s : cs=%08X inner=%08X desc=%08X" % (label, cs, inner, desc))
    hexdump(desc, 0x40, title="  描述符 0x40B:")
    hexdump(inner, 0x40, title="  inner 0x40B:")

# ---------- 2. 按钮 def/sub 对比 ----------
print("\n\n########## 按钮 def/sub 结构对比 ##########")
cases = [
    (0x0DD906D0, "普通 SovietWallPiece"),
    (0x0DD90600, "普通 SovietWarFactory"),
    (0x09EF6580, "扩展 SovietHeavyAntiAirMissileTurret"),
    (0x09EF6640, "扩展 SovietDualPurposeCruiseMissileTurret"),
]
for d, label in cases:
    sub = u32(d + 4)
    print("\n--- def=%08X (%s) sub=%08X" % (d, label, sub))
    hexdump(d, 0x40, title="  def 0x40B:")
    if sub:
        hexdump(sub, 0x40, title="  sub 0x40B:")
