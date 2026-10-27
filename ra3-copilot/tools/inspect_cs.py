# -*- coding: utf-8 -*-
"""只读取证：把 CommandSet 的 key / 按钮链 / 模板名全部 dump 出来。
用法: python tools/inspect_cs.py 09EDDE20 0DD7F920 ...
数据全部来自注入 DLL 的只读 HTTP 端点，不写任何内存。
"""
import sys, json, urllib.request, urllib.parse

BASE = "http://127.0.0.1:17832"


def hit(path, **kw):
    q = urllib.parse.urlencode(kw)
    url = BASE + path + ("?" + q if q else "")
    with urllib.request.urlopen(url, timeout=20) as r:
        return r.read().decode("utf-8", "replace")


def rd(addr, n):
    """读内存，返回 bytes。用 /read 的 hex 输出。"""
    txt = hit("/read", addr="0x%X" % addr, n=n)
    out = bytearray()
    for line in txt.splitlines():
        if ":" not in line:
            continue
        body = line.split(":", 1)[1]
        if "|" in body:
            body = body.split("|")[0]
        for tok in body.split():
            if len(tok) == 2:
                try:
                    out.append(int(tok, 16))
                except ValueError:
                    pass
    return bytes(out)


def u32(addr):
    b = rd(addr, 4)
    if len(b) < 4:
        return None
    return int.from_bytes(b[:4], "little")


def cstr(addr, maxlen=64):
    b = rd(addr, maxlen)
    z = b.find(b"\x00")
    if z >= 0:
        b = b[:z]
    try:
        return b.decode("utf-8")
    except UnicodeDecodeError:
        return b.decode("latin1")


def read_cs(cs):
    """返回 dict：vtable / inner / count / base / key / 按钮列表"""
    r = {"cs": cs}
    head = rd(cs, 8)
    if len(head) < 8:
        r["err"] = "unreadable"
        return r
    r["vt"] = int.from_bytes(head[0:4], "little")
    r["inner"] = int.from_bytes(head[4:8], "little")
    inner = r["inner"]
    if not inner:
        r["err"] = "inner=0"
        return r
    cnt = u32(inner + 4)
    arr = u32(inner + 8)
    r["count"] = cnt
    r["arr"] = arr
    desc = u32(inner)
    r["desc"] = desc
    if desc:
        r["desc4"] = u32(desc + 4)
        r["desc8"] = u32(desc + 8)
        r["descC"] = u32(desc + 0xC)
    # 按钮数组：优先 inner+8 指向的位置
    btns = []
    if arr and cnt and 0 < cnt < 200:
        raw = rd(arr, min(cnt, 40) * 4)
        for i in range(0, min(len(raw) // 4, cnt)):
            d = int.from_bytes(raw[i * 4:i * 4 + 4], "little")
            e = {"def": d}
            if d:
                sub = u32(d + 4)
                e["sub"] = sub
                if sub:
                    tpl = u32(sub + 0x1C)
                    e["tpl"] = tpl
                    if tpl and 0x10000 < tpl < 0x7FFFFFFF:
                        e["thash"] = u32(tpl + 8)
                        nmrec = u32(tpl + 0x1C)
                        if nmrec and 0x10000 < nmrec < 0x7FFFFFFF:
                            # 名字记录：常见布局 +0 = 长度, +4.. = 字符
                            ln = u32(nmrec)
                            if ln and 0 < ln < 64:
                                e["tname"] = cstr(nmrec + 4, ln + 2)
                            else:
                                e["tname"] = cstr(nmrec, 48)
            btns.append(e)
    r["buttons"] = btns
    return r


def main(argv):
    out = []
    for a in argv:
        cs = int(a, 16)
        d = read_cs(cs)
        out.append(d)
        print("=" * 78)
        print("cs=%08X inner=%08X count=%s arr=%08X desc=%08X k4=%s k8=%s kC=%s" % (
            cs, d.get("inner") or 0, d.get("count"), d.get("arr") or 0,
            d.get("desc") or 0,
            ("%08X" % d["desc4"]) if d.get("desc4") is not None else "-",
            ("%08X" % d["desc8"]) if d.get("desc8") is not None else "-",
            ("%08X" % d["descC"]) if d.get("descC") is not None else "-"))
        for i, b in enumerate(d.get("buttons", [])):
            print("  [%2d] def=%08X sub=%s tpl=%s hash=%s %s" % (
                i, b["def"],
                ("%08X" % b["sub"]) if b.get("sub") else "-",
                ("%08X" % b["tpl"]) if b.get("tpl") else "-",
                ("%08X" % b["thash"]) if b.get("thash") else "-",
                b.get("tname") or ""))
    with open("C:/Project/RA3/research/commandset/c_inspect.json", "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1)


if __name__ == "__main__":
    main(sys.argv[1:])
