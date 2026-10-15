#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""fix_callable.py - 用实机探测结果产出「真正可召唤」的单位清单

背景：unit_codes_trainer.tsv 有 718 条，但实测（HTTP /resolve，走的就是召唤用的
      ThingFactory::Find）只有 502 条能找到模板。找不到的分两类：
        a) 本来就不是单位（武器挂点 / 效果体 / 投射物 / 技能触发体）→ 丢弃
        b) 是单位但那条定义是「未启用的旧名」→ 同族变体（如 _B / _Enhanced）才是
           游戏里实际注册的那个（例：AlliedGaintAirCraftCarrier 找不到，
           AlliedGaintAirCraftCarrier_B「奥林匹斯级航空母舰」可以）

做法：失败条目 → 在同前缀变体里挑一个「像单位 + 实测能 resolve」的替代；
      中立阵营整体排除（按需求）。

产出：unit-codes/data/callable_final.tsv
      faction / chinese_name / id / code_hex / primary / replaced_from
用法：python fix_callable.py          （需要游戏运行 + 修改器已注入）
"""
import csv
import io
import os
import sys
import urllib.request
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
WS = os.path.dirname(os.path.dirname(HERE))
D = os.path.join(WS, 'unit-codes', 'data')
TRAINER = os.path.join(D, 'unit_codes_trainer.tsv')
ALLT = os.path.join(D, 'unit_codes_all.tsv')
PROBE = os.path.join(D, 'callable_probe.tsv')
OUT = os.path.join(D, 'callable_final.tsv')
DROPPED = os.path.join(D, 'callable_dropped.tsv')
API = 'http://127.0.0.1:17832/resolve?hash=%s'

# 官方单位图鉴判据：kind_of 含 SELECTABLE 才算"召唤出来能用"
sys.path.insert(0, os.path.join(WS, 'unit-codes', 'tools'))
try:
    from official_db import is_usable, reason_unusable  # noqa: E402
except Exception:                                       # 官方库缺失时退化为不过滤
    def is_usable(_):
        return None

    def reason_unusable(_):
        return ''

# 明显不是"单位"的变体后缀（生成出来是垃圾/残骸/特效）
BAD_SUFFIX = ('Collapse', 'Projectile', 'Death', 'Rubble', 'Effect', 'Firework',
              'Off', 'Movie', '_Dropped', 'Weapon', 'Turret_Dummy')
# 优先选的后缀
GOOD_SUFFIX = ('_B', '_Enhanced', '_Upgrade', '')


def is_cjk(s):
    return any('\u4e00' <= c <= '\u9fff' for c in s)


def resolve(code):
    try:
        with urllib.request.urlopen(API % code, timeout=4) as f:
            t = f.read().decode('utf-8', 'replace')
        return 'template 00000000' not in t
    except Exception:
        return False


def pick_score(suffix, chinese):
    s = 0
    if suffix == '_B':
        s += 100
    elif suffix in ('_Enhanced', '_Upgrade'):
        s += 40
    if is_cjk(chinese):
        s += 60                 # 有中文名 → 基本可以确定是游戏里实际在用的那个
    s -= len(suffix)            # 越接近原型越好
    return s


def main():
    trainer = list(csv.DictReader(io.open(TRAINER, encoding='utf-8'), delimiter='\t'))
    allrows = list(csv.DictReader(io.open(ALLT, encoding='utf-8'), delimiter='\t'))
    probe = {}
    if os.path.exists(PROBE):
        for r in csv.DictReader(io.open(PROBE, encoding='utf-8'), delimiter='\t'):
            probe[r['id']] = (r['callable'] == '1')

    by_name = {}
    for a in allrows:
        by_name.setdefault(a['name'], a)

    final = []
    n_direct = n_replaced = n_dropped = 0
    fac_count = Counter()

    for r in trainer:
        fac = r['faction']
        if fac == '中立':                      # 按需求：中立不要
            continue
        name, code, chinese = r['name'], r['code_hex'], r['chinese_name']
        ok = probe.get(name)
        if ok is None:
            ok = resolve(code)
        if ok:
            final.append((fac, chinese, name, code, r['is_primary'], ''))
            n_direct += 1
            fac_count[fac] += 1
            continue

        # 找替代变体
        cands = []
        for a in allrows:
            n2 = a['name']
            if n2 == name or not n2.startswith(name):
                continue
            suf = n2[len(name):]
            if any(b in suf for b in BAD_SUFFIX):
                continue
            cands.append((pick_score(suf, a['chinese_name']), suf, a))
        cands.sort(key=lambda x: -x[0])

        pick = None
        for sc, suf, a in cands[:8]:
            if resolve(a['code_hex']):
                pick = a
                break
        if pick:
            disp = pick['chinese_name'] if is_cjk(pick['chinese_name']) else chinese
            final.append((fac, disp, pick['name'], pick['code_hex'], r['is_primary'], name))
            n_replaced += 1
            fac_count[fac] += 1
        else:
            n_dropped += 1

    # ---- 最后一道：能不能"用"（官方 kind_of 里的 SELECTABLE）----
    # 实测只能证明模板存在、能创建；证明不了创建出来能操作。
    # 反例：0x9356389F AlliedAntiNavyShipTech3 ✅ / 0xDB5A5941 AlliedChronoBeacon_Poseidon ❌
    usable, dropped2 = [], []
    for item in final:
        uid = item[2]
        v = is_usable(uid)
        if v is False:
            dropped2.append((item[0], item[1], uid, item[3], reason_unusable(uid)))
        else:
            usable.append(item)
    final = usable

    with io.open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write('faction\tchinese_name\tid\tcode_hex\tprimary\treplaced_from\n')
        for fac, cn, uid, code, prim, rep in final:
            f.write('%s\t%s\t%s\t%s\t%s\t%s\n' % (fac, cn, uid, code, prim, rep))

    with io.open(DROPPED, 'w', encoding='utf-8', newline='\n') as f:
        f.write('faction\tchinese_name\tid\tcode_hex\treason\n')
        for fac, cn, uid, code, why in dropped2:
            f.write('%s\t%s\t%s\t%s\t%s\n' % (fac, cn, uid, code, why))

    print('===== 结果 =====')
    print('直接可用 %d  变体补齐 %d  丢弃 %d  不可用剔除 %d  →  最终 %d 条'
          % (n_direct, n_replaced, n_dropped, len(dropped2), len(final)))
    print('\n按阵营：')
    for fac in sorted(fac_count):
        print('  %-6s %d' % (fac, fac_count[fac]))
    print('\n变体替换明细：')
    for fac, cn, uid, code, prim, rep in final:
        if rep:
            print('  %-30s → %-44s %s' % (rep[:30], uid[:44], code))
    print('\n不可用剔除明细（召唤出来不可选/不能用）：')
    for fac, cn, uid, code, why in dropped2:
        print('  %-6s %-18s %-42s %s' % (fac, cn[:18], uid[:42], why))
    print('\n[ok] 写入 %s' % OUT)
    print('[ok] 写入 %s' % DROPPED)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
