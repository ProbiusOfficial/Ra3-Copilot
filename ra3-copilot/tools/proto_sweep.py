"""逐个授予协议技能，按引擎回执把「失败」分成三类。

判据（2026-10-19 反汇编核对）：
    PLAYER_GRANT_SPECIAL_POWER → 0x585CB0
        0x839C30(name) 查不到模板 → 构造异常串 "Couldn't find specified special power template: "
                                     并抛出 ⇒ 我们的 Lua 封装拿到 ret=-1 / detail="调用异常"
        查到模板但 [[tpl+8]+0x134] == 0 → 循环 continue，静默跳过 ⇒ ret 正常、detail="ret=1"
        已在 SCPoint+0x18 列表 → 0x854300 去重 ⇒ 同上，ret 正常
    所以「列表长度 +1」才是生效；失败时用 detail 区分「名字查不到」与「模板无可授予对象」。

用法（游戏进对局、DLL 已注入）：
    python ra3-copilot/tools/proto_sweep.py
    python ra3-copilot/tools/proto_sweep.py --only Celestial    # 只跑名字含该串的
    python ra3-copilot/tools/proto_sweep.py --resume            # 跳过结果文件里已判定的
"""
import argparse
import os
import sys
import time
import urllib.parse
import urllib.request

BASE = "http://127.0.0.1:17832"
HERE = os.path.dirname(os.path.abspath(__file__))
TSV = os.path.join(HERE, "proto_zh.tsv")
OUT = os.path.join(HERE, "..", "..", "research", "protocol-skills", "proto_sweep.tsv")

OK, NOTPL, NOGRANT, UNSURE = "已授予", "查不到模板", "无授予对象", "无法判定"


def get(path, timeout=5):
    with urllib.request.urlopen(BASE + path, timeout=timeout) as r:
        return r.read().decode("utf-8", "replace").strip()


def post(path, timeout=20):
    req = urllib.request.Request(BASE + path, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode("utf-8", "replace").strip()


def owned():
    """返回已授予列表长度；读不到返回 -1。"""
    try:
        txt = get("/protocol?act=owned")
    except Exception:
        return -1
    # 回执里出现的 8 位十六进制个数即为长度
    n = 0
    for tok in txt.replace(",", " ").split():
        t = tok.strip("[]()<>")
        if len(t) == 8 and all(c in "0123456789ABCDEFabcdef" for c in t):
            n += 1
    return n if txt else -1


def load_rows():
    rows = []
    with open(TSV, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            p = line.rstrip("\n").split("\t")
            if len(p) >= 2:
                rows.append((p[0], p[1], p[2] if len(p) > 2 else ""))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="")
    ap.add_argument("--who", default="<This Player>")
    ap.add_argument("--resume", action="store_true")
    a = ap.parse_args()

    try:
        get("/ping", timeout=3)
    except Exception as e:
        print(f"接口不可达（{BASE}）：{e}\n先在游戏里注入 DLL 并进入对局。")
        return 1

    done = set()
    if a.resume and os.path.exists(OUT):
        with open(OUT, encoding="utf-8") as f:
            for line in f:
                if line.startswith("#") or not line.strip():
                    continue
                p = line.split("\t")
                if len(p) > 2 and p[2] in (OK, NOTPL, NOGRANT):
                    done.add(p[0])

    rows = load_rows()
    if a.only:
        rows = [r for r in rows if a.only.lower() in r[0].lower() or a.only in r[1]]
    if a.resume:
        rows = [r for r in rows if r[0] not in done]
    if not rows:
        print("没有待测条目。")
        return 0

    base_owned = owned()
    if base_owned < 0:
        print("读不到 SCPoint+0x18 列表（/protocol?act=owned 无返回）—— 确认已进对局。")
        return 1

    print(f"起始已授予 {base_owned} 项，待测 {len(rows)} 项\n")
    print(f"{'内部名':48s}{'中文':12s}{'结果':10s}细节")
    print("-" * 110)

    results = []
    for name, zh, _fac in rows:
        before = owned()
        q = (f"/action?name=PLAYER_GRANT_SPECIAL_POWER&a1={urllib.parse.quote(name)}"
             f"&a2={urllib.parse.quote(a.who)}")
        try:
            detail = post(q)
        except Exception as e:
            detail = f"HTTP 失败：{e}"
        time.sleep(0.05)
        after = owned()

        d = detail.replace("\n", " ")[:80]
        if before >= 0 and after > before:
            verdict = OK
        elif "调用异常" in detail:
            verdict = NOTPL
        elif "ret=1" in detail or "ret=0" in detail:
            verdict = NOGRANT
        else:
            verdict = UNSURE
        results.append((name, zh, verdict, d))
        print(f"{name:48s}{zh:12s}{verdict:10s}{d}")

    print("-" * 110)
    tally = {}
    for r in results:
        tally[r[2]] = tally.get(r[2], 0) + 1
    print("汇总：" + "  ".join(f"{k}={v}" for k, v in tally.items()))

    head = not os.path.exists(OUT)
    with open(OUT, "a", encoding="utf-8") as f:
        if head:
            f.write("# proto_sweep 结果（内部名 / 中文 / 判定 / 引擎回执）\n")
        if head or not a.resume:
            f.write(f"# 本次运行 {time.strftime('%Y-%m-%d %H:%M')}，共 {len(results)} 条\n")
        for name, zh, verdict, d in results:
            f.write(f"{name}\t{zh}\t{verdict}\t{d}\n")
    print(f"\n写入 {os.path.normpath(OUT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
