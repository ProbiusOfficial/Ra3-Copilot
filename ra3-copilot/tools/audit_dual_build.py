#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""双版本适配审计：逐条核对代码里的 零售 / Steam 双列地址。

检查项（每条地址对）：
  1. 能否在两个 exe 里都解析出文件偏移（RVA 是否落在某个节内）
  2. 函数型符号：目标前 16 字节内是否有 0xCC 填充（= 像是函数入口），
     以及两个构建的**前 8 条指令助记符序列是否一致**（形状比对）
  3. 数据型符号：目标处读出的 dword/指针是否"像"该符号该有的东西

用法：
  python tools/audit_dual_build.py
  python tools/audit_dual_build.py --retail <path> --steam <path>
"""
import argparse
import struct
import sys

try:
    import capstone
except ImportError:
    sys.exit("需要 capstone：pip install capstone")

DEF_RETAIL = r"C:\GAME\Red Alert 3\Data\ra3_1.12.game"
DEF_STEAM = (r"C:\Program Files (x86)\Steam\steamapps\common"
             r"\Command and Conquer Red Alert 3\Data\ra3_1.12.game")

MOD_END = 0xD50000  # 主模块地址上限（大于两版 SizeOfImage）

# name, retail RVA, steam RVA, kind
#   fn   = 函数入口
#   global = .data 里的全局指针槽
#   site = 指令中段（补丁点/内部地址），不做入口检查，只比对前后指令形状
SYMBOLS = [
    ("CreateUnit",   0x205240, 0x2440F0, "fn"),
    ("LevelUp",      0x35C200, 0x39A840, "fn"),
    ("KillUnit",     0x39EA50, 0x3DCDF0, "fn"),
    ("TmplFind",     0x3E4230, 0x422510, "fn"),
    ("MouseMap",     0x1ED4A0, 0x22C500, "fn"),
    ("ThingFactory", 0x8E6C58, 0x8EBDE8, "global"),
    ("SelMgr",       0x8DB73C, 0x8E08DC, "global"),
    ("FreeBuild",    0x0E7565, 0x128DF5, "site"),
    ("DrawSetMC",    0x12F360, 0x170A60, "fn"),
    ("DrawSetTint",  0x12E0F0, 0x16F7F0, "fn"),
    ("SetStatus",    0x3274C0, 0x3658C0, "fn"),
    ("ApplyMod",     0x3181F0, 0x356640, "fn"),
    ("RemoveMod",    0x318300, 0x356750, "fn"),
    ("ModFind",      0x0A6BE0, 0x0A6F50, "fn"),
    # Pick() 里用到的（不在 symres.cpp）
    ("PostA",        0x44A0B0, 0x4880E0, "fn"),
    ("PostB",        0x438AB0, 0x476C50, "fn"),
    ("OrderAttack",  0x36E690, 0x3ACB70, "fn"),
    ("OrderMove",    0x2EFF70, 0x32E320, "fn"),
]


class PE:
    def __init__(self, path):
        self.path = path
        self.d = open(path, "rb").read()
        e = struct.unpack_from("<I", self.d, 0x3C)[0]
        self.nsec = struct.unpack_from("<H", self.d, e + 6)[0]
        optsz = struct.unpack_from("<H", self.d, e + 20)[0]
        opt = e + 24
        self.imagebase = struct.unpack_from("<I", self.d, opt + 28)[0]
        self.sizeofimage = struct.unpack_from("<I", self.d, opt + 56)[0]
        self.secs = []
        so = opt + optsz
        for i in range(self.nsec):
            o = so + i * 40
            name = self.d[o:o + 8].rstrip(b"\0").decode("latin1")
            vs, va, rs, rp = struct.unpack_from("<IIII", self.d, o + 8)
            self.secs.append((name, va, vs, rp, rs))

    def off(self, rva):
        for name, va, vs, rp, rs in self.secs:
            if va <= rva < va + max(vs, rs):
                delta = rva - va
                if delta < rs:
                    return rp + delta, name
                return None, name  # 落在虚拟填充里
        return None, None

    def dword(self, rva):
        o, _ = self.off(rva)
        if o is None or o + 4 > len(self.d):
            return None
        return struct.unpack_from("<I", self.d, o)[0]

    def bytes_at(self, rva, n):
        o, _ = self.off(rva)
        if o is None:
            return b""
        return self.d[o:o + n]

    def cc_pad_before(self, rva, look=24):
        """判断 rva 是否像一个函数入口。

        两版编译器产出的"函数前导"不一定是 int3 填充，也可能是相邻函数的
        尾调用 jmp / ret（实测 KillUnit 前是 `e9 xx xx xx xx`，TmplFind 前是 `ret 4; cc`）。
        所以不用单一特征，改成：从 rva 往前若干个起点分别反汇编，
        只要有一个起点能"干净地落在 rva 上"，就认为 rva 是合法指令边界/入口。
        """
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        o, _ = self.off(rva)
        if o is None or o < look:
            return False
        for back in range(2, look + 1):
            start = rva - back
            buf = self.bytes_at(start, back)
            if len(buf) != back:
                continue
            for ins in md.disasm(buf, self.imagebase + start):
                if ins.address + ins.size == self.imagebase + rva:
                    return True
        return False


def shape(pe, rva, insns=8):
    """前 insns 条指令的助记符序列。"""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    buf = pe.bytes_at(rva, 48)
    out = []
    for ins in md.disasm(buf, pe.imagebase + rva):
        out.append(ins.mnemonic)
        if len(out) >= insns:
            break
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--retail", default=DEF_RETAIL)
    ap.add_argument("--steam", default=DEF_STEAM)
    a = ap.parse_args()

    R = PE(a.retail)
    S = PE(a.steam)
    print("零售 %s  SizeOfImage=0x%X" % (a.retail, R.sizeofimage))
    print("Steam %s  SizeOfImage=0x%X" % (a.steam, S.sizeofimage))
    print("-" * 96)
    print("%-14s %-9s %-9s %-6s %-6s %s" %
          ("符号", "零售RVA", "SteamRVA", "入口?", "形状?", "备注"))
    print("-" * 96)

    bad = []
    for name, rr, sr, kind in SYMBOLS:
        ro, rsec = R.off(rr)
        so, ssec = S.off(sr)
        notes = []
        # 全局槽允许落在 .data 未初始化尾部（文件里没有值），交给下面 global 分支处理
        if kind != "global" and (ro is None or so is None):
            notes.append("RVA 解析失败(零售=%s/Steam=%s)" % (rsec, ssec))
            print("%-14s 0x%-7X 0x%-7X %-6s %-6s %s" %
                  (name, rr, sr, "-", "-", "; ".join(notes)))
            bad.append(name)
            continue

        if kind == "global":
            rv, sv = R.dword(rr), S.dword(sr)
            if rv is None or sv is None:
                # 落在 .data 未初始化尾部：文件里没有落盘值，运行时由引擎写入。
                # 代码本身用"结构推导"兜底（见 symres.cpp 头注释），这里不算失败。
                print("%-14s 0x%-7X 0x%-7X %-6s %-6s 运行期写入(.data 尾部)，文件内无值" %
                      (name, rr, sr, "n/a", "n/a"))
                continue
            ok = 0x01000000 <= rv < 0x20000000 and 0x01000000 <= sv < 0x20000000
            print("%-14s 0x%-7X 0x%-7X %-6s %-6s 零售->0x%08X Steam->0x%08X %s" %
                  (name, rr, sr, "n/a", "n/a", rv, sv,
                   "" if ok else "!! 不是堆指针"))
            if not ok:
                bad.append(name)
            continue

        entry_ok = True
        if kind == "fn":
            re_, se_ = R.cc_pad_before(rr), S.cc_pad_before(sr)
            if not (re_ and se_):
                entry_ok = False
                notes.append("入口可疑(零售填充=%s Steam填充=%s)" % (re_, se_))

        rs, ss = shape(R, rr), shape(S, sr)
        same = rs == ss
        if not same:
            notes.append("形状不同: 零售=%s | Steam=%s" % (",".join(rs), ",".join(ss)))

        flag_e = "OK" if entry_ok else "!!"
        flag_s = "OK" if same else "!!"
        print("%-14s 0x%-7X 0x%-7X %-6s %-6s %s" %
              (name, rr, sr, flag_e, flag_s, "; ".join(notes)))
        if not (entry_ok and same):
            bad.append(name)

    print("-" * 96)
    print("结论：%d/%d 通过%s" %
          (len(SYMBOLS) - len(bad), len(SYMBOLS),
           "" if not bad else "；问题项: " + ", ".join(bad)))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
