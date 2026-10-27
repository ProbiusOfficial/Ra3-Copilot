# -*- coding: utf-8 -*-
"""通过 vtable 内容指纹定位 Steam 版的 Thing vtable。

思路：vtable 是一串函数指针。零售版每项指向某个函数，用该函数入口字节在 Steam 版
重定位，得到 Steam 侧的指针序列；再到 Steam 数据段里找「这串指针连续排列」的位置。
"""
import struct, sys

RET = r'C:\GAME\Red Alert 3\Data\ra3_1.12.game'
STM = r'C:\Program Files (x86)\Steam\steamapps\common\Command and Conquer Red Alert 3\Data\ra3_1.12.game'
BASE = 0x400000
A = open(RET, 'rb').read()
B = open(STM, 'rb').read()

N = 12


def func_prefix_hits(rva, maxlen=32):
    """用零售函数入口字节在 Steam 版定位；返回唯一命中或 None。"""
    for n in (maxlen, 24, 20, 16, 14, 12, 10, 8):
        if rva + n > len(A):
            continue
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
            return hits[0]
        if n == 8:
            return None
    return None


def read_vt(data, va, n):
    rva = va - BASE
    return [struct.unpack_from('<I', data, rva + 4 * i)[0] for i in range(n)]


def main():
    vt_retail = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x00C4DB60
    entries = read_vt(A, vt_retail, N)
    print("零售 vtable 0x%X 前 %d 项:" % (vt_retail, N))
    for i, e in enumerate(entries):
        print("   [%2d] 0x%08X" % (i, e))

    print("\n逐项重定位到 Steam：")
    mapped = []
    for i, e in enumerate(entries):
        if not (BASE <= e < BASE + 0x7C7000):
            print("   [%2d] 0x%08X  非 .text，跳过" % (i, e))
            mapped.append(None)
            continue
        r = func_prefix_hits(e - BASE)
        print("   [%2d] 0x%08X -> %s" % (i, e, ("0x%08X" % (BASE + r)) if r else "未定位"))
        mapped.append(BASE + r if r else None)

    # 用最长的一段连续可定位项做搜索模板
    good = [(i, v) for i, v in enumerate(mapped) if v]
    if len(good) < 4:
        print("\n可定位项太少，放弃")
        return

    # 取前 5 个可定位项（保持相对顺序）做成字节串
    picks = good[:5]
    print("\n用这 %d 项（下标 %s）做搜索模板" % (len(picks), [i for i, _ in picks]))
    if picks[-1][0] - picks[0][0] == len(picks) - 1:
        pat = b''.join(struct.pack('<I', v) for _, v in picks)
        print("   连续排列，模板 = %s" % pat.hex())
        hits = []
        i = 0
        while True:
            i = B.find(pat, i)
            if i < 0:
                break
            hits.append(i)
            i += 1
        print("   Steam 命中 %d 处:" % len(hits))
        for h in hits[:8]:
            cand = h - 4 * picks[0][0]
            print("     vtable RVA = 0x%06X (VA 0x%08X)" % (cand, BASE + cand))
    else:
        print("   不连续，改用逐项搜索（每项单独在数据段找唯一引用）")
        for i, v in picks:
            pat = struct.pack('<I', v)
            hits = []
            k = 0
            while True:
                k = B.find(pat, k)
                if k < 0:
                    break
                hits.append(k)
                k += 1
            print("     [%2d] 0x%08X 出现在 %d 处: %s" % (i, v, len(hits), [hex(x) for x in hits[:5]]))


if __name__ == '__main__':
    main()
